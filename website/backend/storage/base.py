"""Shared types for audio storage providers."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


PROVIDERS = ("github", "cloudinary", "s3")
LINK_PROVIDER = "link"


@dataclass
class StoredAudio:
    """Result of uploading (or registering) one audio object."""
    provider: str
    url: str
    key: str = ""          # provider object key / path (empty for paste-link)
    size: int = 0
    public: dict[str, Any] = field(default_factory=dict)

    def meta(self) -> dict[str, Any]:
        out = {"provider": self.provider, "url": self.url}
        if self.key:
            out["key"] = self.key
        if self.size:
            out["size"] = self.size
        return out


class StorageError(RuntimeError):
    pass
