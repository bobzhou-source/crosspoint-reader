from __future__ import annotations

import hashlib
import os
import re
import tempfile
import unicodedata
from pathlib import Path
from urllib.parse import quote, urlsplit, urlunsplit

MAX_HTML_BYTES = 8 * 1024 * 1024
MAX_IMAGE_BYTES = 8 * 1024 * 1024
MAX_IMAGES = 32
USER_AGENT = "reader-send/0.1 (+local-first personal EPUB reader)"


class ReaderError(Exception):
    """An actionable input, network, or protocol error."""


def clean_text(value: str) -> str:
    return " ".join(xml_text(value).split())


def xml_text(value: str) -> str:
    return "".join(c for c in value if c in "\t\n\r" or 0x20 <= ord(c) <= 0xD7FF
                   or 0xE000 <= ord(c) <= 0xFFFD or 0x10000 <= ord(c) <= 0x10FFFF)


def http_url(value: str) -> str:
    try:
        parts = urlsplit(value)
        if parts.scheme.lower() not in {"http", "https"} or not parts.hostname:
            raise ValueError("expected an http:// or https:// URL")
        if parts.username is not None or parts.password is not None:
            raise ValueError("credentials in URLs are not supported; convert a locally saved HTML file")
        if any(ord(c) < 33 or ord(c) == 127 for c in value):
            raise ValueError("URL contains whitespace or control characters")
        host = parts.hostname.encode("idna").decode("ascii")
        if ":" in host:
            host = "[" + host + "]"
        if parts.port is not None:
            host += ":" + str(parts.port)
        path = quote(parts.path or "/", safe="/%:@!$&'()*+,;=-._~")
        query = quote(parts.query, safe="/?%:@!$&'()*+,;=-._~")
        return urlunsplit((parts.scheme.lower(), host, path, query, ""))
    except ValueError as exc:
        raise ReaderError(f"Invalid URL: {exc}") from exc


def digest(value: str | bytes) -> str:
    return hashlib.sha256(value.encode() if isinstance(value, str) else value).hexdigest()


def filename(title: str, identity: str) -> str:
    slug = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode().lower()
    slug = re.sub(r"[^a-z0-9]+", "-", slug).strip("-")[:72].rstrip("-") or "article"
    return f"{slug}-{digest(identity)[:12]}.epub"


def atomic_write(path: Path, data: bytes, *, overwrite: bool = True) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".reader-send-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if overwrite:
            os.replace(temporary, path)
        else:
            # link is atomic and refuses existing paths, including symlinks.
            os.link(temporary, path)
    except FileExistsError as exc:
        raise ReaderError(f"Output exists: {path}; choose another path or use --force") from exc
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def bounded_read(path: Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ReaderError(f"File exceeds the {limit // 1024} KiB limit: {path}")
    return data
