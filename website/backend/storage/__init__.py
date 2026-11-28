"""Audio storage facade: GitHub | Cloudinary | S3 | pasted HTTPS link."""
from __future__ import annotations

import json
import re
from typing import Any

import github_ops as gh
from storage import cloudinary_store, github_store, s3_store
from storage.base import LINK_PROVIDER, PROVIDERS, StoredAudio, StorageError
from storage.link_fetch import fetch_direct_mp3, assert_safe_url

MEDIA_PATH = "website/docs/media.json"


def providers_status() -> list[dict]:
    return [github_store.status(), cloudinary_store.status(), s3_store.status()]


def default_provider(settings: dict | None) -> str:
    p = str((settings or {}).get("audio_provider") or "github").strip().lower()
    return p if p in PROVIDERS else "github"


def resolve_provider(requested: str | None, settings: dict | None) -> str:
    raw = str(requested or "").strip().lower()
    if raw in ("", "default"):
        return default_provider(settings)
    if raw not in PROVIDERS:
        raise StorageError("provider must be github, cloudinary, or s3")
    return raw


def test_provider(provider: str) -> dict:
    provider = resolve_provider(provider, {"audio_provider": provider})
    if provider == "github":
        return github_store.test_connection()
    if provider == "cloudinary":
        return cloudinary_store.test_connection()
    if provider == "s3":
        return s3_store.test_connection()
    raise StorageError("Unknown provider")


def _validate_bytes(data: bytes, *, max_bytes: int, max_kbps: int | None) -> bytes:
    if max_kbps is not None:
        cleaned, errors = gh.validate_song_mp3(data)
        if errors:
            raise StorageError(errors[0])
        return cleaned
    if len(data) < 500:
        raise StorageError("File is too small")
    if len(data) > max_bytes:
        raise StorageError(f"File is too large (max {max_bytes // (1024 * 1024)} MB)")
    cleaned = gh.strip_id3(data)
    if not gh.looks_like_mp3(cleaned if cleaned else data):
        raise StorageError("File does not look like an MP3")
    return cleaned if len(cleaned) >= 64 else data


def upload_bytes(
    data: bytes,
    *,
    provider: str,
    docs_rel: str,
    object_key: str,
    max_bytes: int = 5 * 1024 * 1024,
    max_kbps: int | None = None,
) -> StoredAudio:
    cleaned = _validate_bytes(data, max_bytes=max_bytes, max_kbps=max_kbps)
    if provider == "github":
        return github_store.upload(cleaned, docs_rel)
    if provider == "cloudinary":
        return cloudinary_store.upload(cleaned, object_key.replace(".", "_"))
    if provider == "s3":
        return s3_store.upload(cleaned, object_key)
    raise StorageError("Unknown provider")


def register_link(url: str, *, max_kbps: int | None = 128) -> StoredAudio:
    assert_safe_url(url)
    raw = fetch_direct_mp3(url)
    cleaned = _validate_bytes(raw, max_bytes=10 * 1024 * 1024, max_kbps=max_kbps)
    # Keep the pasted URL (not a re-hosted copy); size is verified payload length.
    return StoredAudio(provider=LINK_PROVIDER, url=url.strip(), key="", size=len(cleaned))


def delete_stored(meta: dict | None) -> None:
    """Delete provider-owned object. Pasted links are never deleted remotely."""
    if not meta or not isinstance(meta, dict):
        return
    provider = str(meta.get("provider") or "")
    key = str(meta.get("key") or "")
    if provider == LINK_PROVIDER or not key:
        return
    if provider == "github":
        github_store.delete(key)
    elif provider == "cloudinary":
        cloudinary_store.delete(key)
    elif provider == "s3":
        s3_store.delete(key)


def load_media() -> dict[str, Any]:
    raw = gh.get_file_bytes(MEDIA_PATH)
    if not raw:
        return {"version": 0, "adhan": {}, "huruf": {}}
    try:
        obj = json.loads(raw.decode("utf-8"))
    except Exception:
        return {"version": 0, "adhan": {}, "huruf": {}}
    if not isinstance(obj, dict):
        return {"version": 0, "adhan": {}, "huruf": {}}
    obj.setdefault("version", 0)
    obj.setdefault("adhan", {})
    obj.setdefault("huruf", {})
    return obj


def save_media(media: dict[str, Any], message: str) -> str:
    media = dict(media)
    media["version"] = int(media.get("version") or 0) + 1
    raw = json.dumps(media, ensure_ascii=True, separators=(",", ":")).encode()
    if len(raw) > 4000:
        raise StorageError("media.json would exceed 4000 bytes; use shorter URLs")
    return gh.atomic_update({MEDIA_PATH: raw}, message)


def entry_from_stored(stored: StoredAudio) -> dict[str, Any]:
    return stored.meta()


def public_play_url(entry: dict | None, fallback_rel: str, version: int = 0) -> str | None:
    if entry and entry.get("url"):
        return str(entry["url"])
    if fallback_rel:
        base = github_store.public_url_for_docs_path(fallback_rel)
        return f"{base}?v={version}" if version else base
    return None


_SAFE_KEY = re.compile(r"^[a-zA-Z0-9_./-]{1,120}$")


def object_key_for(kind: str, name: str) -> str:
    key = f"ruju/{kind}/{name}".replace("//", "/")
    if not _SAFE_KEY.match(key):
        raise StorageError("Bad storage object key")
    return key
