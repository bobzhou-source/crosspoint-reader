from __future__ import annotations

import re
import time
from dataclasses import dataclass
from http.client import HTTPException
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
from urllib.robotparser import RobotFileParser

from .common import MAX_HTML_BYTES, USER_AGENT, ReaderError, http_url


@dataclass
class Response:
    data: bytes
    url: str
    content_type: str
    charset: str | None


class Redirects(HTTPRedirectHandler):
    def __init__(self, client: PublicHTTP):
        self.client = client

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        newurl = http_url(newurl)
        if getattr(req, "respect_robots", False):
            self.client.check_robots(newurl)
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected is not None:
            redirected.respect_robots = getattr(req, "respect_robots", False)
        return redirected


class PublicHTTP:
    """No cookies, login, credential storage, or JavaScript execution."""

    def __init__(self, timeout: float = 20, retries: int = 2):
        self.timeout = timeout
        self.retries = retries
        self.robots: dict[str, RobotFileParser] = {}
        self.opener = build_opener(Redirects(self))

    def check_robots(self, url: str) -> None:
        parts = urlsplit(url)
        origin = urlunsplit((parts.scheme, parts.netloc, "", "", ""))
        if origin not in self.robots:
            parser = RobotFileParser(origin + "/robots.txt")
            try:
                result = self._request(origin + "/robots.txt", 512 * 1024, False)
                parser.parse(result.data.decode("utf-8", "replace").splitlines())
            except HTTPError as exc:
                if exc.code in (404, 410):
                    parser.parse([])
                elif exc.code in (401, 403):
                    parser.parse(["User-agent: *", "Disallow: /"])
                else:
                    raise ReaderError(f"Cannot check robots.txt (HTTP {exc.code}); use saved HTML instead") from exc
            except (URLError, OSError, HTTPException) as exc:
                raise ReaderError(f"Cannot check robots.txt: {exc}; use saved HTML instead") from exc
            self.robots[origin] = parser
        if not self.robots[origin].can_fetch(USER_AGENT, url):
            raise ReaderError("robots.txt disallows this URL; use an authorized local HTML/export file instead")

    def _request(self, url: str, limit: int, robots: bool) -> Response:
        for attempt in range(self.retries + 1):
            try:
                request = Request(url, headers={"User-Agent": USER_AGENT,
                    "Accept": "text/html,application/xhtml+xml,image/*;q=0.9,*/*;q=0.5",
                    "Accept-Encoding": "identity"})
                request.respect_robots = robots
                deadline = time.monotonic() + max(60, self.timeout * 3)
                with self.opener.open(request, timeout=self.timeout) as response:
                    if response.headers.get("Content-Encoding", "identity").lower() not in ("", "identity"):
                        raise ReaderError("Server ignored identity encoding; save this page as HTML instead")
                    length = response.headers.get("Content-Length", "")
                    if length.isdigit() and int(length) > limit:
                        raise ReaderError(f"Download exceeds {limit // 1024} KiB limit")
                    chunks = bytearray()
                    while len(chunks) <= limit:
                        chunk = response.read1(min(65536, limit + 1 - len(chunks)))
                        if not chunk:
                            break
                        chunks.extend(chunk)
                        if time.monotonic() > deadline:
                            raise ReaderError("Download exceeded the total request deadline")
                    if len(chunks) > limit:
                        raise ReaderError(f"Download exceeds {limit // 1024} KiB limit")
                    return Response(bytes(chunks), http_url(response.url),
                                    response.headers.get_content_type(), response.headers.get_content_charset())
            except HTTPError as exc:
                if exc.code not in (429, 500, 502, 503, 504) or attempt == self.retries:
                    raise
                exc.close()
            except (URLError, OSError, HTTPException):
                if attempt == self.retries:
                    raise
            time.sleep(min(2 ** attempt, 4))
        raise AssertionError("unreachable")

    def get(self, url: str, limit: int = MAX_HTML_BYTES) -> Response:
        url = http_url(url)
        self.check_robots(url)
        try:
            return self._request(url, limit, True)
        except HTTPError as exc:
            hint = "; for authenticated posts, convert a locally saved HTML/export file" if exc.code in (401, 403) else ""
            raise ReaderError(f"HTTP {exc.code} fetching {url}{hint}") from exc
        except (URLError, OSError, HTTPException) as exc:
            raise ReaderError(f"Cannot fetch {url}: {exc}") from exc


def decode_html(data: bytes, charset: str | None = None) -> str:
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16", "replace")
    if not charset:
        match = re.search(br"charset\s*=\s*[\"']?\s*([a-zA-Z0-9_-]+)", data[:8192], re.I)
        charset = match.group(1).decode("ascii") if match else "utf-8-sig"
    try:
        return data.decode(charset, "replace")
    except LookupError as exc:
        raise ReaderError(f"Unknown HTML character encoding: {charset}") from exc
