"""Cloudinary audio uploads (signed server-side Upload API)."""
from __future__ import annotations

import hashlib
import os
import time

import requests

from storage.base import StoredAudio, StorageError

CLOUD_NAME = os.getenv("CLOUDINARY_CLOUD_NAME", "").strip()
API_KEY = os.getenv("CLOUDINARY_API_KEY", "").strip()
API_SECRET = os.getenv("CLOUDINARY_API_SECRET", "").strip()


def configured() -> bool:
    return bool(CLOUD_NAME and API_KEY and API_SECRET)


def status() -> dict:
    return {
        "id": "cloudinary",
        "label": "Cloudinary",
        "configured": configured(),
        "cloud": CLOUD_NAME if configured() else "",
    }


def _sign(params: dict[str, str]) -> str:
    # Official Cloudinary signature: sort params, join as key=value&, append api_secret, SHA-1.
    parts = [f"{k}={params[k]}" for k in sorted(params)]
    to_sign = "&".join(parts) + API_SECRET
    return hashlib.sha1(to_sign.encode("utf-8")).hexdigest()


def test_connection() -> dict:
    if not configured():
        raise StorageError("Set CLOUDINARY_CLOUD_NAME, CLOUDINARY_API_KEY, CLOUDINARY_API_SECRET")
    # Admin ping: signed upload of empty is wrong; use usage API with basic auth.
    r = requests.get(
        f"https://api.cloudinary.com/v1_1/{CLOUD_NAME}/usage",
        auth=(API_KEY, API_SECRET),
        timeout=20,
    )
    if r.status_code == 401:
        raise StorageError("Cloudinary credentials rejected")
    if not r.ok:
        raise StorageError(f"Cloudinary test failed ({r.status_code})")
    return {"ok": True, "detail": f"Cloud {CLOUD_NAME} reachable"}


def upload(data: bytes, public_id: str) -> StoredAudio:
    if not configured():
        raise StorageError("Cloudinary is not configured")
    if not public_id:
        raise StorageError("Missing Cloudinary public_id")
    ts = str(int(time.time()))
    params = {
        "public_id": public_id,
        "timestamp": ts,
        "resource_type": "video",  # audio files use video resource type on Cloudinary
    }
    # resource_type is NOT included in the signature string for upload API.
    sign_params = {"public_id": public_id, "timestamp": ts}
    signature = _sign(sign_params)
    url = f"https://api.cloudinary.com/v1_1/{CLOUD_NAME}/video/upload"
    files = {"file": ("audio.mp3", data, "audio/mpeg")}
    form = {
        "api_key": API_KEY,
        "timestamp": ts,
        "public_id": public_id,
        "signature": signature,
    }
    r = requests.post(url, data=form, files=files, timeout=120)
    if not r.ok:
        raise StorageError(f"Cloudinary upload failed ({r.status_code}): {r.text[:200]}")
    body = r.json()
    secure = body.get("secure_url") or body.get("url")
    if not secure:
        raise StorageError("Cloudinary upload returned no URL")
    # Prefer raw mp3 delivery URL when available
    if secure.endswith(".mp3") is False and "/upload/" in secure:
        # keep secure_url; Cloudinary serves audio from it
        pass
    return StoredAudio(
        provider="cloudinary",
        url=secure,
        key=body.get("public_id") or public_id,
        size=int(body.get("bytes") or len(data)),
        public={"format": body.get("format")},
    )


def delete(key: str) -> None:
    if not configured() or not key:
        return
    ts = str(int(time.time()))
    sign_params = {"public_id": key, "timestamp": ts}
    signature = _sign(sign_params)
    url = f"https://api.cloudinary.com/v1_1/{CLOUD_NAME}/video/destroy"
    r = requests.post(url, data={
        "api_key": API_KEY,
        "timestamp": ts,
        "public_id": key,
        "signature": signature,
    }, timeout=30)
    if not r.ok and r.status_code != 404:
        raise StorageError(f"Cloudinary delete failed ({r.status_code})")
