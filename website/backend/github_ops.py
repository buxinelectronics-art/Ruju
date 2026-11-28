"""GitHub helpers: Contents API uploads + Git Data API atomic tree commits."""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import time
from typing import Any

import requests

GITHUB_TOKEN = os.getenv("GITHUB_TOKEN", "")
GITHUB_REPO = os.getenv("GITHUB_REPO", "buxinelectronics-art/Ruju")
GITHUB_BRANCH = os.getenv("GITHUB_BRANCH", "main")
# Repo paths (GitHub Pages publishes website/docs/ as the site root).
ALLOWED = (
    "website/docs/adhan/", "website/docs/huruf/", "website/docs/hadith/",
    "website/docs/qa/", "website/docs/songs/",
)
ALLOWED_FILES = ("website/docs/media.json",)
SONGS_PREFIX = "website/docs/songs/"
SONGS_PAGE_SIZE = 10
SONGS_PAGE_MAX_BYTES = 3000
DOCS_WARN_BYTES = 500 * 1024 * 1024
DOCS_BLOCK_BYTES = 800 * 1024 * 1024
SONG_MP3_MAX_BYTES = 10 * 1024 * 1024
SONG_MP3_MAX_KBPS = 128

HURUF_NAMES = [
    "Alif", "Ba", "Ta", "Tha", "Jim", "Ha", "Kha", "Dal", "Dhal", "Ra", "Zay", "Sin",
    "Shin", "Sad", "Dad", "Tah", "Zah", "Ayn", "Ghayn", "Fa", "Qaf", "Kaf", "Lam",
    "Mim", "Nun", "Ha", "Waw", "Ya",
]


def configured() -> bool:
    return bool(GITHUB_TOKEN and GITHUB_REPO and GITHUB_BRANCH)


def headers() -> dict:
    if not GITHUB_TOKEN:
        raise RuntimeError("GITHUB_TOKEN not set on server")
    return {
        "Authorization": "Bearer " + GITHUB_TOKEN,
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }


def assert_path(path: str) -> str:
    path = path.replace("\\", "/").lstrip("/")
    if ".." in path.split("/"):
        raise ValueError("path not allowed")
    if path in ALLOWED_FILES or path.startswith(ALLOWED):
        return path
    raise ValueError("path not allowed")


def _url(path: str) -> str:
    return f"https://api.github.com/repos/{GITHUB_REPO}/{path}"


def api(method: str, path: str, **kw) -> requests.Response:
    for attempt in range(5):
        r = requests.request(method, _url(path), headers=headers(), timeout=60, **kw)
        if r.status_code == 403 and "rate limit" in r.text.lower():
            time.sleep(2 + attempt * 2)
            continue
        if r.status_code in (502, 503, 504):
            time.sleep(1 + attempt)
            continue
        return r
    return r


def get_file(path: str) -> dict | None:
    path = assert_path(path)
    r = api("GET", f"contents/{path}", params={"ref": GITHUB_BRANCH})
    if r.status_code == 404:
        return None
    if not r.ok:
        raise RuntimeError(f"GitHub read failed ({r.status_code})")
    return r.json()


def get_file_bytes(path: str) -> bytes | None:
    info = get_file(path)
    if not info:
        return None
    if info.get("encoding") == "base64" and info.get("content"):
        return base64.b64decode(info["content"])
    if info.get("download_url"):
        r = requests.get(info["download_url"], timeout=60)
        if r.ok:
            return r.content
    return None


def put_file(path: str, data: bytes, message: str) -> dict:
    path = assert_path(path)
    if not data:
        raise ValueError("empty file")
    existing = get_file(path)
    body = {
        "message": message,
        "content": base64.b64encode(data).decode("ascii"),
        "branch": GITHUB_BRANCH,
    }
    if existing and existing.get("sha"):
        body["sha"] = existing["sha"]
    r = api("PUT", f"contents/{path}", json=body)
    if not r.ok:
        raise RuntimeError(f"GitHub write failed ({r.status_code}): {r.text[:240]}")
    return r.json()


def list_prefix(prefix: str) -> list[str]:
    """List file paths under docs/... using git tree (recursive)."""
    prefix = assert_path(prefix if prefix.endswith("/") else prefix + "/")
    ref = api("GET", f"git/ref/heads/{GITHUB_BRANCH}")
    if not ref.ok:
        raise RuntimeError("Cannot read branch ref")
    commit_sha = ref.json()["object"]["sha"]
    commit = api("GET", f"git/commits/{commit_sha}")
    if not commit.ok:
        raise RuntimeError("Cannot read commit")
    tree_sha = commit.json()["tree"]["sha"]
    tree = api("GET", f"git/trees/{tree_sha}", params={"recursive": "1"})
    if not tree.ok:
        raise RuntimeError("Cannot read tree")
    out = []
    for item in tree.json().get("tree", []):
        if item.get("type") == "blob" and item.get("path", "").startswith(prefix):
            out.append(item["path"])
    return out


def create_blob(data: bytes) -> str:
    r = api("POST", "git/blobs", json={
        "content": base64.b64encode(data).decode("ascii"),
        "encoding": "base64",
    })
    if not r.ok:
        raise RuntimeError(f"blob failed ({r.status_code}): {r.text[:200]}")
    return r.json()["sha"]


def website_docs_stats() -> dict[str, int]:
    """Total blob size under website/docs/ (Git tree; not Render disk)."""
    ref = api("GET", f"git/ref/heads/{GITHUB_BRANCH}")
    if not ref.ok:
        raise RuntimeError("Cannot read branch ref")
    commit_sha = ref.json()["object"]["sha"]
    commit = api("GET", f"git/commits/{commit_sha}")
    if not commit.ok:
        raise RuntimeError("Cannot read commit")
    tree_sha = commit.json()["tree"]["sha"]
    tree = api("GET", f"git/trees/{tree_sha}", params={"recursive": "1"})
    if not tree.ok:
        raise RuntimeError("Cannot read tree")
    total = 0
    files = 0
    songs = 0
    for item in tree.json().get("tree", []):
        if item.get("type") != "blob":
            continue
        path = item.get("path", "")
        if not path.startswith("website/docs/"):
            continue
        size = int(item.get("size") or 0)
        total += size
        files += 1
        if path.startswith(SONGS_PREFIX) and path.endswith(".mp3"):
            songs += size
    return {"docs_bytes": total, "docs_files": files, "songs_bytes": songs}


def atomic_update(changes: dict[str, bytes | str | None], message: str) -> str:
    """One commit: path -> bytes (new), existing blob sha (str), or None (delete)."""
    if not changes:
        raise ValueError("no changes")
    for path in changes:
        assert_path(path)

    ref = api("GET", f"git/ref/heads/{GITHUB_BRANCH}")
    if not ref.ok:
        raise RuntimeError("Cannot read branch ref")
    old_commit = ref.json()["object"]["sha"]
    commit = api("GET", f"git/commits/{old_commit}")
    if not commit.ok:
        raise RuntimeError("Cannot read commit")
    base_tree = commit.json()["tree"]["sha"]

    tree_items = []
    for path, val in changes.items():
        if val is None:
            tree_items.append({"path": path, "mode": "100644", "type": "blob", "sha": None})
        elif isinstance(val, str):
            tree_items.append({"path": path, "mode": "100644", "type": "blob", "sha": val})
        else:
            sha = create_blob(val)
            tree_items.append({"path": path, "mode": "100644", "type": "blob", "sha": sha})
            time.sleep(0.05)

    tree = api("POST", "git/trees", json={"base_tree": base_tree, "tree": tree_items})
    if not tree.ok:
        raise RuntimeError(f"tree failed ({tree.status_code}): {tree.text[:240]}")
    new_commit = api("POST", "git/commits", json={
        "message": message,
        "tree": tree.json()["sha"],
        "parents": [old_commit],
    })
    if not new_commit.ok:
        raise RuntimeError(f"commit failed ({new_commit.status_code}): {new_commit.text[:240]}")
    sha = new_commit.json()["sha"]
    # Update uses plural "refs" (GET uses singular "ref").
    upd = api("PATCH", f"git/refs/heads/{GITHUB_BRANCH}", json={"sha": sha, "force": False})
    if not upd.ok:
        raise RuntimeError(f"ref update failed ({upd.status_code}): {upd.text[:240]}")
    return sha


def atomic_replace(prefix: str, files: dict[str, bytes], message: str) -> str:
    """Replace all blobs under prefix with `files` (path -> bytes) in one commit."""
    prefix = assert_path(prefix if prefix.endswith("/") else prefix + "/")
    for path in files:
        assert_path(path)
        if not path.startswith(prefix):
            raise ValueError("file outside prefix: " + path)

    ref = api("GET", f"git/ref/heads/{GITHUB_BRANCH}")
    if not ref.ok:
        raise RuntimeError("Cannot read branch ref")
    old_commit = ref.json()["object"]["sha"]
    commit = api("GET", f"git/commits/{old_commit}")
    if not commit.ok:
        raise RuntimeError("Cannot read commit")
    base_tree = commit.json()["tree"]["sha"]

    old_paths = set(list_prefix(prefix))
    new_paths = set(files)
    tree_items = []

    for path, data in files.items():
        sha = create_blob(data)
        tree_items.append({"path": path, "mode": "100644", "type": "blob", "sha": sha})
        time.sleep(0.05)

    for path in sorted(old_paths - new_paths):
        tree_items.append({"path": path, "mode": "100644", "type": "blob", "sha": None})

    tree = api("POST", "git/trees", json={"base_tree": base_tree, "tree": tree_items})
    if not tree.ok:
        raise RuntimeError(f"tree failed ({tree.status_code}): {tree.text[:240]}")
    new_tree = tree.json()["sha"]

    new_commit = api("POST", "git/commits", json={
        "message": message,
        "tree": new_tree,
        "parents": [old_commit],
    })
    if not new_commit.ok:
        raise RuntimeError(f"commit failed ({new_commit.status_code}): {new_commit.text[:240]}")
    sha = new_commit.json()["sha"]

    upd = api("PATCH", f"git/refs/heads/{GITHUB_BRANCH}", json={"sha": sha, "force": False})
    if not upd.ok:
        raise RuntimeError(f"ref update failed ({upd.status_code}): {upd.text[:240]}")
    return sha


def looks_like_mp3(data: bytes) -> bool:
    if len(data) < 4:
        return False
    if data[:3] == b"ID3":
        return True
    return data[0] == 0xFF and (data[1] & 0xE0) == 0xE0


def ascii_english(text: str) -> bool:
    try:
        text.encode("ascii")
    except UnicodeEncodeError:
        return False
    return True


def slug_topic(name: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", name.strip().lower()).strip("-")
    return (s or "topic")[:40]


def row_hash(*parts: str) -> str:
    blob = "\n".join(p.strip() for p in parts)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()[:16]


def parse_hadith_xlsx(raw: bytes) -> tuple[list[dict], list[str]]:
    from openpyxl import load_workbook
    from io import BytesIO

    errors: list[str] = []
    rows: list[dict] = []
    wb = load_workbook(BytesIO(raw), read_only=True, data_only=True)
    ws = wb.active
    header = [str(c.value or "").strip() for c in next(ws.iter_rows(min_row=1, max_row=1))]
    need = ["Number", "Title", "Text", "Source"]
    if [h.lower() for h in header[:4]] != [n.lower() for n in need]:
        return [], ["First row must be: Number, Title, Text, Source"]
    seen = set()
    for i, row in enumerate(ws.iter_rows(min_row=2, values_only=True), start=2):
        if not row or all(v is None or str(v).strip() == "" for v in row):
            continue
        number, title, text, source = (list(row) + [None] * 4)[:4]
        err_prefix = f"Row {i}: "
        try:
            num = int(number)
        except (TypeError, ValueError):
            errors.append(err_prefix + "Number must be an integer")
            continue
        title = str(title or "").strip()
        text = str(text or "").strip()
        source = str(source or "").strip()
        if num < 1 or num > 500:
            errors.append(err_prefix + "Number must be 1..500")
        if num in seen:
            errors.append(err_prefix + f"duplicate Number {num}")
        seen.add(num)
        if not title:
            errors.append(err_prefix + "Title required")
        if not text:
            errors.append(err_prefix + "Text required")
        if len(title) > 60:
            errors.append(err_prefix + "Title max 60 characters")
        if len(text) > 1500:
            errors.append(err_prefix + "Text max 1500 characters")
        if len(source) > 150:
            errors.append(err_prefix + "Source max 150 characters")
        for label, val in (("Title", title), ("Text", text), ("Source", source)):
            if val and not ascii_english(val):
                errors.append(err_prefix + f"{label} must be English ASCII")
        item = {"number": num, "title": title, "text": text, "source": source,
                "hash": row_hash(str(num), title, text, source)}
        raw_json = json.dumps(item, ensure_ascii=True).encode()
        if len(raw_json) > 3000:
            errors.append(err_prefix + "JSON would exceed 3000 bytes")
        rows.append(item)
    if len(rows) > 500:
        errors.append("Max 500 rows")
    rows.sort(key=lambda r: r["number"])
    return rows, errors


def parse_qa_xlsx(raw: bytes) -> tuple[list[dict], list[str]]:
    from openpyxl import load_workbook
    from io import BytesIO

    errors: list[str] = []
    rows: list[dict] = []
    wb = load_workbook(BytesIO(raw), read_only=True, data_only=True)
    ws = wb.active
    header = [str(c.value or "").strip() for c in next(ws.iter_rows(min_row=1, max_row=1))]
    need = ["Topic", "Question", "Answer", "Source"]
    if [h.lower() for h in header[:4]] != [n.lower() for n in need]:
        return [], ["First row must be: Topic, Question, Answer, Source"]
    topics: dict[str, int] = {}
    for i, row in enumerate(ws.iter_rows(min_row=2, values_only=True), start=2):
        if not row or all(v is None or str(v).strip() == "" for v in row):
            continue
        topic, question, answer, source = (list(row) + [None] * 4)[:4]
        err_prefix = f"Row {i}: "
        topic = str(topic or "").strip()
        question = str(question or "").strip()
        answer = str(answer or "").strip()
        source = str(source or "").strip()
        if not topic:
            errors.append(err_prefix + "Topic required")
        if not question:
            errors.append(err_prefix + "Question required")
        if not answer:
            errors.append(err_prefix + "Answer required")
        if len(topic) > 60:
            errors.append(err_prefix + "Topic max 60 characters")
        if len(question) > 300:
            errors.append(err_prefix + "Question max 300 characters")
        if len(answer) > 1500:
            errors.append(err_prefix + "Answer max 1500 characters")
        if len(source) > 150:
            errors.append(err_prefix + "Source max 150 characters")
        for label, val in (("Topic", topic), ("Question", question), ("Answer", answer), ("Source", source)):
            if val and not ascii_english(val):
                errors.append(err_prefix + f"{label} must be English ASCII")
        tid = slug_topic(topic)
        topics[tid] = topics.get(tid, 0) + 1
        item = {
            "topic": topic, "topic_id": tid, "question": question, "answer": answer,
            "source": source, "hash": row_hash(tid, question, answer, source),
        }
        if len(json.dumps(item, ensure_ascii=True).encode()) > 3000:
            errors.append(err_prefix + "JSON would exceed 3000 bytes")
        rows.append(item)
    if len(topics) > 20:
        errors.append("Max 20 Q&A topics")
    if len(rows) > 500:
        errors.append("Max 500 rows")
    return rows, errors


def load_hadith_index() -> dict[str, Any]:
    raw = get_file_bytes("website/docs/hadith/index.json")
    if not raw:
        return {"version": 0, "count": 0, "items": []}
    return json.loads(raw.decode("utf-8"))


def load_qa_index() -> dict[str, Any]:
    raw = get_file_bytes("website/docs/qa/index.json")
    if not raw:
        return {"version": 0, "topics": []}
    return json.loads(raw.decode("utf-8"))


def strip_id3(data: bytes) -> bytes:
    """Remove ID3v2 (start) and ID3v1 (end) so cover art / large tags are gone."""
    out = data
    if len(out) >= 10 and out[:3] == b"ID3":
        size = ((out[6] & 0x7F) << 21) | ((out[7] & 0x7F) << 14) | ((out[8] & 0x7F) << 7) | (out[9] & 0x7F)
        footer = 10 if (out[5] & 0x10) else 0
        skip = 10 + size + footer
        if skip < len(out):
            out = out[skip:]
    if len(out) >= 128 and out[-128:-125] == b"TAG":
        out = out[:-128]
    return out


_BITRATE_V1_L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 288, 320]
_BITRATE_V2_L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0]
_SAMPLERATE_V1 = [44100, 48000, 32000, 0]
_SAMPLERATE_V2 = [22050, 24000, 16000, 0]
_SAMPLERATE_V25 = [11025, 12000, 8000, 0]


def _mp3_frame_info(hdr: bytes) -> tuple[int, int, int] | None:
    if len(hdr) < 4 or hdr[0] != 0xFF or (hdr[1] & 0xE0) != 0xE0:
        return None
    ver_id = (hdr[1] >> 3) & 0x03
    layer = (hdr[1] >> 1) & 0x03
    bitrate_idx = (hdr[2] >> 4) & 0x0F
    sr_idx = (hdr[2] >> 2) & 0x03
    padding = (hdr[2] >> 1) & 0x01
    if ver_id == 1 or layer != 1 or bitrate_idx in (0, 15) or sr_idx == 3:
        return None
    if ver_id == 3:  # MPEG1
        br = _BITRATE_V1_L3[bitrate_idx]
        sr = _SAMPLERATE_V1[sr_idx]
        frame = 144 * br * 1000 // sr + padding if sr else 0
    elif ver_id == 2:  # MPEG2
        br = _BITRATE_V2_L3[bitrate_idx]
        sr = _SAMPLERATE_V2[sr_idx]
        frame = 72 * br * 1000 // sr + padding if sr else 0
    elif ver_id == 0:  # MPEG2.5
        br = _BITRATE_V2_L3[bitrate_idx]
        sr = _SAMPLERATE_V25[sr_idx]
        frame = 72 * br * 1000 // sr + padding if sr else 0
    else:
        return None
    if br <= 0 or sr <= 0 or frame < 24:
        return None
    return br, sr, frame


def validate_song_mp3(raw: bytes) -> tuple[bytes, list[str]]:
    """Return (stripped_mp3, errors). Accepts real MPEG audio, max 10MB / 128kbps."""
    errors: list[str] = []
    if not raw or len(raw) < 64:
        return b"", ["File is too small to be a real MP3"]
    if len(raw) > SONG_MP3_MAX_BYTES:
        return b"", [f"File is too large (max {SONG_MP3_MAX_BYTES // (1024 * 1024)} MB)"]
    if not looks_like_mp3(raw):
        return b"", ["File is not a real MP3 (missing MPEG frame or ID3 header)"]
    cleaned = strip_id3(raw)
    if len(cleaned) < 32 or not (cleaned[0] == 0xFF and (cleaned[1] & 0xE0) == 0xE0):
        # try scanning for a frame after leftover junk
        found = -1
        for i in range(0, min(len(cleaned) - 4, 8192)):
            if cleaned[i] == 0xFF and (cleaned[i + 1] & 0xE0) == 0xE0 and _mp3_frame_info(cleaned[i:i + 4]):
                found = i
                break
        if found < 0:
            return b"", ["File does not contain a valid MP3 audio frame"]
        cleaned = cleaned[found:]
    max_br = 0
    pos = 0
    frames = 0
    while pos + 4 <= len(cleaned) and frames < 40:
        info = _mp3_frame_info(cleaned[pos:pos + 4])
        if not info:
            pos += 1
            continue
        br, _sr, flen = info
        max_br = max(max_br, br)
        frames += 1
        pos += flen
    if frames < 1:
        return b"", ["Could not read MP3 frame headers"]
    if max_br > SONG_MP3_MAX_KBPS:
        return b"", [f"Bitrate too high ({max_br} kbps). Max allowed is {SONG_MP3_MAX_KBPS} kbps"]
    if len(cleaned) < 64:
        return b"", ["MP3 audio after stripping tags is too small"]
    return cleaned, errors


def song_title_ok(title: str) -> str | None:
    title = (title or "").strip()
    if not title:
        return "Title is required"
    if len(title) > 60:
        return "Title max 60 characters"
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 .'\-,!?()]*", title):
        return "Title must use English letters (ASCII), max 60"
    if not re.search(r"[A-Za-z]", title):
        return "Title must include English letters"
    return None


def song_artist_ok(artist: str) -> str | None:
    artist = (artist or "").strip()
    if not artist:
        return None
    if len(artist) > 40:
        return "Artist max 40 characters"
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 .'\-,!?()]*", artist):
        return "Artist must use English letters (ASCII), max 40"
    return None


def load_songs_index() -> dict[str, Any]:
    raw = get_file_bytes(SONGS_PREFIX + "index.json")
    if not raw:
        return {"version": 0, "count": 0, "pageSize": SONGS_PAGE_SIZE}
    obj = json.loads(raw.decode("utf-8"))
    obj.setdefault("pageSize", SONGS_PAGE_SIZE)
    obj.setdefault("count", 0)
    obj.setdefault("version", 0)
    return obj


def load_songs_list() -> tuple[dict[str, Any], list[dict[str, str]]]:
    idx = load_songs_index()
    count = int(idx.get("count") or 0)
    page_size = int(idx.get("pageSize") or SONGS_PAGE_SIZE)
    songs: list[dict[str, str]] = []
    pages = (count + page_size - 1) // page_size if count else 0
    for p in range(1, pages + 1):
        raw = get_file_bytes(f"{SONGS_PREFIX}page{p:03d}.json")
        if not raw:
            continue
        obj = json.loads(raw.decode("utf-8"))
        for s in obj.get("songs") or []:
            if isinstance(s, dict) and s.get("id"):
                item = {
                    "id": str(s["id"]),
                    "title": str(s.get("title") or "")[:60],
                    "artist": str(s.get("artist") or "")[:40],
                }
                if s.get("url"):
                    item["url"] = str(s["url"])[:250]
                if s.get("provider"):
                    item["provider"] = str(s["provider"])[:20]
                if s.get("key"):
                    item["key"] = str(s["key"])[:160]
                songs.append(item)
    # Merge store meta from index.items if present (provider/key for admin delete).
    by_id = {it.get("id"): it for it in (idx.get("items") or []) if isinstance(it, dict)}
    for s in songs:
        meta = by_id.get(s["id"]) or {}
        for k in ("provider", "key", "size"):
            if meta.get(k) and not s.get(k):
                s[k] = meta[k]
    return idx, songs


def next_song_id(songs: list[dict[str, str]]) -> int:
    best = 0
    for s in songs:
        m = re.fullmatch(r"s(\d{4})", s.get("id") or "")
        if m:
            best = max(best, int(m.group(1)))
    return best + 1


def build_song_pages(songs: list[dict[str, str]]) -> dict[str, bytes]:
    """Build pageNNN.json blobs; each page <= 3000 bytes, 10 songs."""
    files: dict[str, bytes] = {}
    page_size = SONGS_PAGE_SIZE
    if not songs:
        return files
    pages = (len(songs) + page_size - 1) // page_size
    for p in range(1, pages + 1):
        chunk = songs[(p - 1) * page_size:p * page_size]

        def _page_bytes(items):
            rows = []
            for s in items:
                row = {"id": s["id"], "title": s["title"], "artist": s.get("artist") or ""}
                if s.get("url"):
                    row["url"] = str(s["url"])[:250]
                rows.append(row)
            return json.dumps({"songs": rows}, ensure_ascii=True, separators=(",", ":")).encode()

        raw = _page_bytes(chunk)
        if len(raw) > SONGS_PAGE_MAX_BYTES:
            for s in chunk:
                if len(raw) <= SONGS_PAGE_MAX_BYTES:
                    break
                s["artist"] = (s.get("artist") or "")[:20]
                raw = _page_bytes(chunk)
            if len(raw) > SONGS_PAGE_MAX_BYTES:
                raise ValueError(
                    f"page{p:03d}.json would exceed {SONGS_PAGE_MAX_BYTES} bytes "
                    "(shorten titles or audio URLs)"
                )
        files[f"{SONGS_PREFIX}page{p:03d}.json"] = raw
    return files


def songs_page_deletes(old_count: int, new_count: int) -> dict[str, None]:
    """Delete page files that are no longer needed after a shrink."""
    old_pages = (old_count + SONGS_PAGE_SIZE - 1) // SONGS_PAGE_SIZE if old_count else 0
    new_pages = (new_count + SONGS_PAGE_SIZE - 1) // SONGS_PAGE_SIZE if new_count else 0
    out: dict[str, None] = {}
    for p in range(new_pages + 1, old_pages + 1):
        out[f"{SONGS_PREFIX}page{p:03d}.json"] = None
    return out
