"""S3-compatible object storage (Cloudflare R2, Backblaze B2, AWS S3)."""
from __future__ import annotations

import os
from urllib.parse import quote

from storage.base import StoredAudio, StorageError

S3_ENDPOINT = os.getenv("S3_ENDPOINT", "").strip().rstrip("/")
S3_BUCKET = os.getenv("S3_BUCKET", "").strip()
S3_ACCESS_KEY = os.getenv("S3_ACCESS_KEY", "").strip()
S3_SECRET_KEY = os.getenv("S3_SECRET_KEY", "").strip()
S3_PUBLIC_URL = os.getenv("S3_PUBLIC_URL", "").strip().rstrip("/")
S3_REGION = os.getenv("S3_REGION", "auto").strip() or "auto"


def configured() -> bool:
    return bool(S3_ENDPOINT and S3_BUCKET and S3_ACCESS_KEY and S3_SECRET_KEY and S3_PUBLIC_URL)


def status() -> dict:
    return {
        "id": "s3",
        "label": "S3-compatible (R2 / B2 / S3)",
        "configured": configured(),
        "bucket": S3_BUCKET if configured() else "",
        "endpoint": S3_ENDPOINT if configured() else "",
        "public_base": S3_PUBLIC_URL if configured() else "",
    }


def _client():
    try:
        import boto3
        from botocore.config import Config
    except ImportError as ex:
        raise StorageError("boto3 is not installed on the server") from ex
    return boto3.client(
        "s3",
        endpoint_url=S3_ENDPOINT,
        aws_access_key_id=S3_ACCESS_KEY,
        aws_secret_access_key=S3_SECRET_KEY,
        region_name=S3_REGION,
        config=Config(signature_version="s3v4", s3={"addressing_style": "path"}),
    )


def test_connection() -> dict:
    if not configured():
        raise StorageError(
            "Set S3_ENDPOINT, S3_BUCKET, S3_ACCESS_KEY, S3_SECRET_KEY, S3_PUBLIC_URL"
        )
    client = _client()
    try:
        client.head_bucket(Bucket=S3_BUCKET)
    except Exception as ex:
        raise StorageError(f"S3 test failed: {ex}") from ex
    return {"ok": True, "detail": f"Bucket {S3_BUCKET} reachable"}


def public_url(key: str) -> str:
    return f"{S3_PUBLIC_URL}/{quote(key, safe='/')}"


def upload(data: bytes, key: str) -> StoredAudio:
    if not configured():
        raise StorageError("S3 storage is not configured")
    key = key.lstrip("/")
    client = _client()
    try:
        client.put_object(
            Bucket=S3_BUCKET,
            Key=key,
            Body=data,
            ContentType="audio/mpeg",
            CacheControl="public, max-age=3600",
        )
    except Exception as ex:
        raise StorageError(f"S3 upload failed: {ex}") from ex
    return StoredAudio(provider="s3", url=public_url(key), key=key, size=len(data))


def delete(key: str) -> None:
    if not configured() or not key:
        return
    client = _client()
    try:
        client.delete_object(Bucket=S3_BUCKET, Key=key.lstrip("/"))
    except Exception as ex:
        raise StorageError(f"S3 delete failed: {ex}") from ex
