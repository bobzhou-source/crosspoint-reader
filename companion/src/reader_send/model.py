from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

from .common import digest, http_url


def source_id(url: str) -> str:
    parts = urlsplit(http_url(url))
    query = [(key, value) for key, value in parse_qsl(parts.query, keep_blank_values=True)
             if not key.lower().startswith("utm_") and key.lower() not in {"fbclid", "gclid"}]
    return "url:" + urlunsplit((parts.scheme, parts.netloc.lower(), parts.path, urlencode(query), ""))


@dataclass
class ReadingItem:
    title: str
    author: str
    source_url: str
    date: str
    language: str
    content: str
    images: list[str]
    warnings: list[str] = field(default_factory=list)
    source_type: str = "web"
    source_id: str = ""
    fetched_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    read_state: str = "unread"
    progress: float = 0.0
    provenance: dict[str, str] = field(default_factory=dict)

    def __post_init__(self):
        if not self.source_id:
            self.source_id = source_id(self.source_url) if self.source_url else "content:" + digest(self.content)

    def metadata(self) -> dict:
        return {key: getattr(self, key) for key in ("title", "author", "source_url", "date", "language", "warnings",
                "source_type", "source_id", "fetched_at", "read_state", "progress", "provenance")}
