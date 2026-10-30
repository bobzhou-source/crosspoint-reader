from __future__ import annotations

import base64
import hashlib
import hmac
import http.client
import json
import math
import os
import re
import secrets
import ssl
import struct
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit

from .cache import Cache
from .common import ReaderError, atomic_write, bounded_read, digest

DEFAULT_ORIGIN = "https://robertzhou.dev"
MAX_EPUB = 1024 * 1024
MAX_METADATA = 16 * 1024
MAX_ANNOTATION_BODY = 64 * 1024
MAX_RESPONSE = 12 * 1024 * 1024
MAX_SAFE_INTEGER = 2**53 - 1
UUID_PATTERN = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
HEX_PATTERN = re.compile(r"[0-9a-f]{64}")
ITEM_NAMESPACE = uuid.UUID("55f50f65-5f9b-54bc-bfa6-1a537d8207c8")


def json_bytes(value) -> bytes:
    try:
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True,
                          allow_nan=False).encode("utf-8")
    except (ValueError, TypeError, UnicodeError, RecursionError) as exc:
        raise ReaderError("Cloud data must be valid UTF-8 JSON with finite numbers") from exc


def parse_json(data: bytes):
    def invalid_constant(value):
        raise ValueError("non-finite number")
    def unique_fields(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON field")
            result[key] = value
        return result
    try:
        return json.loads(data.decode("utf-8"), parse_constant=invalid_constant, object_pairs_hook=unique_fields)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ReaderError("Invalid cloud JSON; expected UTF-8, unique fields and finite numbers") from exc


def identifier(value, field="id") -> str:
    if not isinstance(value, str) or not UUID_PATTERN.fullmatch(value):
        raise ReaderError(f"Cloud {field} must be a canonical lowercase UUID")
    return value


def integer(value, field, minimum=0, maximum=MAX_SAFE_INTEGER) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ReaderError(f"Cloud {field} must be an integer from {minimum} to {maximum}")
    return value


def text(value, field, maximum, *, empty=False, multiline=False) -> str:
    if not isinstance(value, str):
        raise ReaderError(f"Cloud {field} must be text")
    try:
        # Server text limits count JavaScript UTF-16 code units, not Python code points.
        length = len(value.encode("utf-16-le")) // 2
    except UnicodeError as exc:
        raise ReaderError(f"Cloud {field} contains invalid Unicode") from exc
    if (length > maximum or (not empty and not value.strip())
            or any((ord(c) < 32 and not (multiline and c in "\r\n\t")) or ord(c) == 127 for c in value)):
        raise ReaderError(f"Cloud {field} is empty, contains control characters, or exceeds {maximum} UTF-16 units; nothing is truncated")
    return value


def origin_url(value: str) -> str:
    try:
        if not isinstance(value, str) or any(ord(c) < 33 or ord(c) > 126 for c in value) or "\\" in value:
            raise ValueError
        parts = urlsplit(value)
        if (parts.scheme != "https" or not parts.hostname or parts.username is not None or parts.password is not None
                or parts.path not in ("", "/") or "?" in value or "#" in value
                or (parts.port is not None and not 1 <= parts.port <= 65535)):
            raise ValueError
        return "https://" + parts.netloc.lower()
    except ValueError as exc:
        raise ReaderError("Cloud --origin must be an explicit HTTPS origin (host and optional port only), without credentials, path, query or fragment") from exc


def validate_item(item: dict) -> dict:
    if not isinstance(item, dict):
        raise ReaderError("Cloud item metadata must be an object")
    identifier(item.get("id"))
    text(item.get("title"), "title", 512)
    integer(item.get("size"), "size", 58, MAX_EPUB)
    if not isinstance(item.get("sha256"), str) or not HEX_PATTERN.fullmatch(item["sha256"]):
        raise ReaderError("Cloud item SHA-256 must be 64 lowercase hex characters")
    if item.get("source_path") is not None:
        text(item["source_path"], "source_path", 1024)
    if item.get("original_url") is not None:
        url = text(item["original_url"], "original_url", 2048)
        try:
            parsed = urlsplit(url)
            if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username is not None or parsed.password is not None:
                raise ValueError
            parsed.port
        except ValueError as exc:
            raise ReaderError("Cloud original_url must be HTTP(S) without embedded credentials") from exc
    return item


def validate_epub(data: bytes) -> None:
    if len(data) > MAX_EPUB:
        raise ReaderError("Cloud EPUB exceeds 1 MiB; use LocalSend/USB instead. Bytes are never truncated or recompressed")
    if (len(data) < 58 or data[:4] != b"PK\x03\x04" or struct.unpack_from("<H", data, 6)[0] & 1
            or struct.unpack_from("<H", data, 8)[0] != 0 or struct.unpack_from("<HH", data, 26) != (8, 0)
            or data[30:58] != b"mimetypeapplication/epub+zip"):
        raise ReaderError("Cloud EPUB requires the first uncompressed, unencrypted, extra-field-free mimetype ZIP entry; convert again")


def prepare_push(cache: Cache, cache_id: str, source_path: str | None = None) -> tuple[dict, bytes]:
    record = cache.get(cache_id, verify=False)
    if not record:
        raise ReaderError("No cached article with this ID; convert/fetch locally before cloud push")
    path = Path(record["path"])
    if path.stat().st_size > MAX_EPUB:
        raise ReaderError("Cloud EPUB exceeds 1 MiB; use LocalSend/USB instead. Bytes are never truncated or recompressed")
    data = bounded_read(path, MAX_EPUB)
    validate_epub(data)
    checksum = digest(data)
    if checksum != record.get("sha256") or len(data) != record.get("size"):
        raise ReaderError("Cached EPUB integrity mismatch; convert again before cloud push")
    metadata = {"title": record.get("title"), "original_url": record.get("source_url") or None,
                "source_path": source_path if source_path is not None else record["filename"],
                "sha256": checksum, "size": len(data)}
    identity = json_bytes([record.get("source_id") or cache_id, metadata]).decode("utf-8")
    metadata = {"id": str(uuid.uuid5(ITEM_NAMESPACE, identity)), **metadata}
    validate_item(metadata)
    if len(json_bytes(metadata)) > MAX_METADATA:
        raise ReaderError("Cloud item metadata exceeds 16 KiB; use local delivery, not truncation")
    return metadata, data


@dataclass(frozen=True)
class Response:
    status: int
    headers: dict[str, str]
    data: bytes


def https_request(origin: str, method: str, route: str, body: bytes, headers: dict[str, str],
                  timeout: float, max_bytes: int) -> Response:
    parts = urlsplit(origin_url(origin))
    connection = http.client.HTTPSConnection(parts.hostname, parts.port or 443, timeout=timeout,
                                             context=ssl.create_default_context())
    try:
        # http.client has no redirect, cookie, proxy-environment, or authentication handlers.
        connection.request(method, route, body=body, headers=headers)
        response = connection.getresponse()
        response_headers = {key.lower(): value for key, value in response.getheaders()}
        if response.status >= 300:
            # Error bodies (including large WAF pages) are neither needed nor echoed.
            return Response(response.status, {}, b"")
        limit = max_bytes
        length = response_headers.get("content-length")
        if length is not None and (len(length) > 16 or not length.isascii() or not length.isdecimal() or int(length) > limit):
            raise ReaderError("Cloud response exceeds its size limit or has invalid Content-Length")
        data = response.read(limit + 1)
        if len(data) > limit:
            raise ReaderError("Cloud response exceeds its size limit")
        if length is not None and len(data) != int(length):
            raise ReaderError("Cloud response was incomplete; no file was installed")
        if response_headers.get("content-encoding", "identity").lower() != "identity":
            raise ReaderError("Cloud returned unsupported compressed HTTP content")
        return Response(response.status, response_headers, data)
    except (OSError, http.client.HTTPException) as exc:
        # Library errors can include private values; do not print their text.
        raise ReaderError("Cloud HTTPS request failed. Check DNS, connectivity and trusted TLS certificates. No automatic retry; a write may have succeeded, so retry the same IDs") from exc
    finally:
        connection.close()


class CloudClient:
    def __init__(self, origin=DEFAULT_ORIGIN, timeout=30, *, transport=None, clock=None, nonce=None):
        self.origin = origin_url(origin)
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= 300:
            raise ReaderError("Cloud timeout must be greater than 0 and at most 300 seconds")
        self.timeout = timeout
        key = os.environ.get("READER_SYNC_KEY", "")
        if not HEX_PATTERN.fullmatch(key):
            raise ReaderError("Set the dedicated READER_SYNC_KEY environment variable to 64 lowercase hex characters; no key files or CLI secret arguments are supported")
        self._key = key.encode("utf-8")
        self._transport = transport or https_request
        self._clock = clock or time.time
        self._nonce = nonce or (lambda: secrets.token_hex(16))

    def request(self, method, route, body=b"", content_type=None, *, expected=(200,), max_bytes=MAX_RESPONSE):
        if (method not in {"GET", "POST", "PUT", "DELETE"}
                or not re.fullmatch(r"/reader/(?:items(?:/[0-9a-f-]{36}(?:/(?:content|ack))?)?|annotations(?:/export)?)(?:\?after=(?:0|[1-9][0-9]*)&limit=[1-9][0-9]*)?", route)
                or type(body) is not bytes):
            raise ReaderError("Invalid cloud request method, route or byte body")
        timestamp = str(int(self._clock()))
        nonce = self._nonce()
        if not re.fullmatch(r"[0-9a-f]{32}", nonce):
            raise ReaderError("Cloud nonce generator did not return 16 random bytes as lowercase hex")
        canonical = "\n".join(("reader-sync-v1", method, route, digest(body), timestamp, nonce)).encode("utf-8")
        signature = base64.urlsafe_b64encode(hmac.new(self._key, canonical, hashlib.sha256).digest()).rstrip(b"=").decode("ascii")
        headers = {"Authorization": "Bearer " + self._key.decode("utf-8"), "X-Reader-Timestamp": timestamp,
                   "X-Reader-Nonce": nonce, "X-Reader-Signature": signature, "User-Agent": "ReaderSend/1",
                   "Accept": "application/json, application/epub+zip, application/x-ndjson", "Accept-Encoding": "identity",
                   "Content-Length": str(len(body))}
        if content_type:
            headers["Content-Type"] = content_type
        response = self._transport(self.origin, method, route, body, headers, self.timeout, max_bytes)
        if 300 <= response.status < 400:
            raise ReaderError("Cloud redirect refused; verify --origin. Credentials were not forwarded")
        if response.status not in expected:
            hints = {
                400: "Invalid metadata/annotation; check field types, limits and capture time",
                401: "Check READER_SYNC_KEY and system clock (must be within 300 seconds)",
                403: "Access/WAF denied; approved integration must allow the ReaderSend/1 client",
                404: "Item/route missing or upload unfinished; list the inbox and finish the original push",
                405: "API method not supported; check server/client versions",
                409: "Replay, immutable ID conflict or unfinished state; retry only identical content with the same IDs, or use a new ID for an edited annotation",
                413: "Cloud size limit exceeded; use LocalSend/USB, never truncate",
                415: "Unsupported content type/encoding; check server/client versions",
                422: "EPUB size, SHA-256 or mimetype header mismatch; regenerate the local EPUB",
                429: "Rate limit reached; wait at least 60 seconds (daily limit resets at UTC midnight)",
                503: "Reader sync disabled/unavailable; ask the deployment owner to check configuration",
                507: "Private storage full; delete unneeded EPUBs explicitly (annotations have no deletion API)",
            }
            hint = hints.get(response.status, "Unexpected API response; check server availability and compatibility")
            raise ReaderError(f"Cloud HTTP {response.status}: {hint}. No automatic retry")
        return response

    def json_request(self, method, route, body=b"", *, expected=(200,)):
        response = self.request(method, route, body, "application/json" if body else None, expected=expected)
        value = parse_json(response.data)
        if not isinstance(value, dict):
            raise ReaderError("Cloud response must be a JSON object")
        return value

    def items(self, *, ready_only=False) -> list[dict]:
        records = self.json_request("GET", "/reader/items").get("items")
        if not isinstance(records, list) or len(records) > 64:
            raise ReaderError("Invalid cloud inbox list (expected at most 64 items)")
        seen = set()
        for record in records:
            validate_item(record)
            if record.get("state") not in ("uploading", "ready", "consumed") or record["id"] in seen:
                raise ReaderError("Cloud inbox has invalid states or duplicate IDs")
            seen.add(record["id"])
        return [record for record in records if record["state"] == "ready"] if ready_only else records

    def push(self, metadata: dict, data: bytes) -> dict:
        validate_item(metadata)
        validate_epub(data)
        if digest(data) != metadata["sha256"] or len(data) != metadata["size"]:
            raise ReaderError("Cloud push bytes do not match metadata; nothing sent")
        raw = json_bytes(metadata)
        if len(raw) > MAX_METADATA:
            raise ReaderError("Cloud metadata exceeds 16 KiB")
        self._item_response(self.json_request("POST", "/reader/items", raw, expected=(200, 201)), metadata)
        response = self.request("PUT", f"/reader/items/{metadata['id']}/content", data, "application/epub+zip", expected=(200, 201))
        item = self._item_response(parse_json(response.data), metadata)
        if item.get("state") not in {"ready", "consumed"}:
            raise ReaderError("Cloud upload did not produce completed content; retry the same cached item")
        return item

    @staticmethod
    def _item_response(value, metadata):
        item = value.get("item") if isinstance(value, dict) else None
        validate_item(item)
        if any(item.get(key) != val for key, val in metadata.items()):
            raise ReaderError("Cloud returned different immutable item metadata")
        return item

    def pull(self, item_id: str, output_dir: Path, *, overwrite=False) -> dict:
        identifier(item_id)
        output_dir = output_dir.expanduser()
        destination = output_dir / (item_id + ".epub")
        sidecar = output_dir / (item_id + ".json")
        if not overwrite and any(path.exists() or path.is_symlink() for path in (destination, sidecar)):
            raise ReaderError("Cloud pull output exists; choose another directory or use --force")
        item = next((item for item in self.items() if item["id"] == item_id), None)
        if item is None or item["state"] == "uploading":
            raise ReaderError("Cloud EPUB is missing or not ready; complete its original push first")
        response = self.request("GET", f"/reader/items/{item_id}/content", max_bytes=MAX_EPUB)
        if (len(response.data) != item["size"] or digest(response.data) != item["sha256"]
                or response.headers.get("x-reader-sha256") != item["sha256"]
                or response.headers.get("content-length") != str(item["size"])):
            raise ReaderError("Cloud EPUB SHA-256/size verification failed; no file installed and no acknowledgement sent")
        validate_epub(response.data)
        atomic_write(destination, response.data, overwrite=overwrite)
        atomic_write(sidecar, json_bytes(item) + b"\n", overwrite=overwrite)
        return {"item": item, "output": str(destination), "metadata": str(sidecar), "acknowledged": False}

    def ack(self, item_id: str) -> dict:
        identifier(item_id)
        result = self.json_request("POST", f"/reader/items/{item_id}/ack")
        item = self._item_response(result, {"id": item_id})
        if item.get("state") != "consumed":
            raise ReaderError("Cloud acknowledgement did not mark the item consumed")
        return item

    def delete(self, item_id: str) -> dict:
        identifier(item_id)
        self.request("DELETE", f"/reader/items/{item_id}", expected=(204,))
        return {"deleted": item_id, "annotations_preserved": True}
