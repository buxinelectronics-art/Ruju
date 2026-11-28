"""Store audio blobs under website/docs/ via the existing GitHub helpers."""
from __future__ import annotations

import os

import github_ops as gh
from storage.base import StoredAudio, StorageError

DEFAULT_CONTENT_URL = os.getenv(
    "DEFAULT_CONTENT_URL", "https://buxinelectronics-art.github.io/Ruju"
).rstrip("/")


def configured() -> bool:
    return gh.configured()


def status() -> dict:
    return {
        "id": "github",
        "label": "GitHub (Pages)",
        "configured": configured(),
        "repo": gh.GITHUB_REPO if configured() else "",
        "branch": gh.GITHUB_BRANCH if configured() else "",
    }


def test_connection() -> dict:
    if not configured():
        raise StorageError("Set GITHUB_TOKEN, GITHUB_REPO and GITHUB_BRANCH")
    ref = gh.api("GET", f"git/ref/heads/{gh.GITHUB_BRANCH}")
    if not ref.ok:
        raise StorageError(f"GitHub ref failed ({ref.status_code})")
    return {"ok": True, "detail": f"Branch {gh.GITHUB_BRANCH} reachable"}


def public_url_for_docs_path(docs_rel: str) -> str:
    """docs_rel like 'adhan/fajr.mp3' -> Pages URL."""
    rel = docs_rel.lstrip("/")
    return f"{DEFAULT_CONTENT_URL}/{rel}"


def upload(data: bytes, docs_rel: str, message: str | None = None) -> StoredAudio:
    """Upload MP3 to website/docs/<docs_rel> and return the public Pages URL."""
    if not configured():
        raise StorageError("GitHub storage is not configured")
    rel = docs_rel.replace("\\", "/").lstrip("/")
    path = "website/docs/" + rel
    try:
        path = gh.assert_path(path)
    except ValueError as ex:
        raise StorageError("GitHub path not allowed") from ex
    try:
        gh.put_file(path, data, message or f"Upload {path}")
    except Exception as ex:
        raise StorageError(str(ex)) from ex
    url = public_url_for_docs_path(rel)
    return StoredAudio(provider="github", url=url, key=path, size=len(data))


def delete(key: str) -> None:
    """Delete a docs file by full repo path (best-effort; Contents API)."""
    if not key or not configured():
        return
    try:
        path = gh.assert_path(key) if key.startswith("website/docs/") else key
    except Exception:
        return
    info = gh.get_file(path)
    if not info or not info.get("sha"):
        return
    r = gh.api("DELETE", f"contents/{path}", json={
        "message": f"Delete {path}",
        "sha": info["sha"],
        "branch": gh.GITHUB_BRANCH,
    })
    if not r.ok and r.status_code != 404:
        raise StorageError(f"GitHub delete failed ({r.status_code})")
