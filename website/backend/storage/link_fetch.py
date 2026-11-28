"""Fetch and validate a pasted direct HTTPS MP3 link."""
from __future__ import annotations

import ipaddress
import socket
from urllib.parse import urlparse

import requests

from storage.base import StorageError

MAX_LINK_LEN = 250
MAX_REDIRECTS = 3
MAX_BYTES = 10 * 1024 * 1024
FETCH_TIMEOUT = 25

_BLOCKED_HOST_SUFFIXES = (
    "youtube.com", "youtu.be", "music.youtube.com",
    "vimeo.com", "facebook.com", "fb.watch",
    "instagram.com", "tiktok.com", "twitter.com", "x.com",
    "drive.google.com", "docs.google.com", "dropbox.com",
    "soundcloud.com", "spotify.com", "open.spotify.com",
)


def _host_blocked_name(host: str) -> bool:
    h = host.lower().rstrip(".")
    if h in ("localhost",) or h.endswith(".localhost") or h.endswith(".local"):
        return True
    for suf in _BLOCKED_HOST_SUFFIXES:
        if h == suf or h.endswith("." + suf):
            return True
    return False


def _ip_is_private(ip: str) -> bool:
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return True
    return bool(
        addr.is_private or addr.is_loopback or addr.is_link_local
        or addr.is_multicast or addr.is_reserved or addr.is_unspecified
    )


def assert_safe_url(url: str, *, allow_relative_path_check: bool = True) -> str:
    url = (url or "").strip()
    if not url:
        raise StorageError("Paste an HTTPS audio link")
    if len(url) > MAX_LINK_LEN:
        raise StorageError(f"Link too long (max {MAX_LINK_LEN} characters)")
    if any(c.isspace() for c in url):
        raise StorageError("Link must not contain spaces")
    parsed = urlparse(url)
    if parsed.scheme.lower() != "https":
        raise StorageError("Link must start with https://")
    if not parsed.hostname:
        raise StorageError("Link host is missing")
    if parsed.username or parsed.password:
        raise StorageError("Link must not include credentials")
    if parsed.port not in (None, 443):
        raise StorageError("Link must use HTTPS port 443")
    host = parsed.hostname
    if _host_blocked_name(host):
        raise StorageError(
            "That looks like a sharing page (YouTube etc.), not a direct MP3 file. "
            "Paste a direct https://…mp3 link."
        )
    # Resolve every address for the host and reject private/local.
    try:
        infos = socket.getaddrinfo(host, 443, type=socket.SOCK_STREAM)
    except socket.gaierror as ex:
        raise StorageError("Cannot resolve link host") from ex
    if not infos:
        raise StorageError("Cannot resolve link host")
    for info in infos:
        ip = info[4][0]
        if _ip_is_private(ip):
            raise StorageError("Link host resolves to a private or local address")
    path = (parsed.path or "").lower()
    if allow_relative_path_check:
        # Soft hint: path should look like audio; final bytes still verified.
        if path.endswith((".html", ".htm", ".php", ".asp", ".aspx")):
            raise StorageError("Link points to a web page, not a direct MP3 file")
    return url


def fetch_direct_mp3(url: str) -> bytes:
    """Follow up to 3 redirects; verify HTTPS and non-private hosts at every hop."""
    current = assert_safe_url(url)
    session = requests.Session()
    session.max_redirects = 0
    for hop in range(MAX_REDIRECTS + 1):
        try:
            r = session.get(
                current,
                timeout=FETCH_TIMEOUT,
                stream=True,
                allow_redirects=False,
                headers={"User-Agent": "RujuAudioCheck/1.0", "Accept": "*/*"},
            )
        except requests.RequestException as ex:
            raise StorageError(f"Could not download link: {ex}") from ex

        if r.status_code in (301, 302, 303, 307, 308):
            if hop >= MAX_REDIRECTS:
                raise StorageError("Too many redirects (max 3)")
            loc = r.headers.get("Location") or ""
            if not loc:
                raise StorageError("Redirect without Location header")
            if loc.startswith("/"):
                p = urlparse(current)
                loc = f"{p.scheme}://{p.netloc}{loc}"
            current = assert_safe_url(loc)
            continue

        if r.status_code != 200:
            raise StorageError(f"Link returned HTTP {r.status_code}")

        ctype = (r.headers.get("Content-Type") or "").lower()
        if "text/html" in ctype or "application/json" in ctype:
            raise StorageError(
                "Link returned a web page, not an MP3. Use a direct audio file URL."
            )
        clen = r.headers.get("Content-Length")
        if clen and clen.isdigit() and int(clen) > MAX_BYTES:
            raise StorageError("Linked file is too large (max 10 MB)")

        chunks = []
        total = 0
        for chunk in r.iter_content(64 * 1024):
            if not chunk:
                continue
            total += len(chunk)
            if total > MAX_BYTES:
                raise StorageError("Linked file is too large (max 10 MB)")
            chunks.append(chunk)
        data = b"".join(chunks)
        if total < 64:
            raise StorageError("Linked file is too small to be an MP3")
        return data

    raise StorageError("Too many redirects (max 3)")
