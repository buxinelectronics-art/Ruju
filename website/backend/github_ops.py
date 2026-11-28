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
ALLOWED = ("website/docs/adhan/", "website/docs/huruf/", "website/docs/hadith/", "website/docs/qa/")

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
    if ".." in path.split("/") or not path.startswith(ALLOWED):
        raise ValueError("path not allowed")
    return path


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

    upd = api("PATCH", f"git/ref/heads/{GITHUB_BRANCH}", json={"sha": sha, "force": False})
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
