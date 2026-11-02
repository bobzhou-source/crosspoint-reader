"""Drain the private shared-link queue through the existing fetch/push flow.

The reader/Share shortcut only enqueues public HTTPS URLs. A scheduled Mac
collector converts each pending link with the normal companion article path
(or the authorized signed-in Chrome X adapter for X/Twitter post URLs), pushes
the verified cached EPUB through the existing cloud item flow, and only then
acknowledges the link. Conversion failures are acknowledged as explicit job
errors; cloud failures leave the link pending for the next pass. It never
extracts cookies, calls a private API, or invents an EPUB.
"""

from __future__ import annotations

import ipaddress
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlsplit

from .cache import Cache, ID_PATTERN
from .cloud import CloudClient, prepare_push
from .common import ReaderError

DEFAULT_LINK_LIMIT = 8
FETCH_TIMEOUT = 20 * 60


def _link_url(value: str) -> str:
    try:
        if not isinstance(value, str) or any(ord(c) < 33 or ord(c) > 126 for c in value):
            raise ValueError
        parts = urlsplit(value)
        if (parts.scheme != "https" or not parts.hostname or parts.username is not None
                or parts.password is not None):
            raise ValueError
        host = parts.hostname.rstrip(".").lower()
        if host in ("localhost", "localhost.localdomain") or host.endswith((".local", ".internal", ".home", ".lan")):
            raise ValueError
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            address = None
        if address is not None and not address.is_global:
            raise ValueError
        parts.port
        return value
    except ValueError as exc:
        raise ReaderError("Shared link must be a public HTTPS URL without embedded credentials") from exc


def _error_text(stderr: str, returncode: int | None) -> str:
    message = (stderr or "").strip()
    if not message:
        message = f"conversion exited {returncode}" if returncode is not None else "conversion failed"
    message = re.sub(r"[\x00-\x1f\x7f]+", " ", message)
    return message[:1024] or "conversion failed"


def _safe_error(value) -> str:
    message = re.sub(r"[\x00-\x1f\x7f]+", " ", str(value)).strip()
    return message[:1024] or "link processing failed"


def _conversion_args(url: str) -> list[str]:
    host = (urlsplit(url).hostname or "").lower()
    if host in {"x.com", "www.x.com", "mobile.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com"}:
        return ["x", url, "--browser"]
    return ["fetch", url]


def _fetch(cache: Cache, url: str, *, runner=subprocess.run) -> dict:
    command = [
        str(Path(sys.executable).absolute()),
        "-m",
        "reader_send",
        "--cache-dir",
        str(cache.root),
        *_conversion_args(url),
    ]
    env = os.environ.copy()
    env.pop("READER_SYNC_KEY", None)
    try:
        completed = runner(command, capture_output=True, text=True, timeout=FETCH_TIMEOUT,
                          check=False, env=env)
    except subprocess.TimeoutExpired as exc:
        raise ReaderError(f"link conversion timed out after {FETCH_TIMEOUT} seconds") from exc
    except OSError as exc:
        raise ReaderError(f"link conversion could not start: {exc}") from exc
    if completed.returncode != 0:
        raise ReaderError(_error_text(completed.stderr, completed.returncode))
    try:
        record = json.loads(completed.stdout)
    except (ValueError, TypeError) as exc:
        raise ReaderError("link conversion returned invalid JSON") from exc
    if not isinstance(record, dict) or not isinstance(record.get("id"), str) or not ID_PATTERN.fullmatch(record["id"]):
        raise ReaderError("link conversion did not return a full cache ID")
    return record


def drain(client: CloudClient, cache: Cache, *, limit: int = DEFAULT_LINK_LIMIT,
          runner=subprocess.run) -> dict:
    if type(limit) is not int or not 1 <= limit <= 64:
        raise ReaderError("Shared-link limit must be 1-64")
    links = client.links(limit=limit)
    remote = {} if not links else {item["id"]: item for item in client.items()}
    fetched = 0
    pushed = 0
    synced = 0
    acked = 0
    failed = 0
    deferred = 0
    errors = []
    results = []
    for link in links:
        if link["status"] != "pending":
            continue
        try:
            url = _link_url(link["url"])
        except ReaderError as exc:
            message = _safe_error(exc)
            try:
                client.ack_link(link["id"], error=message)
                failed += 1
                results.append({"id": link["id"], "status": "failed", "error": message})
            except ReaderError as ack_error:
                errors.append({"link": link["id"], "errors": [message, str(ack_error)]})
            continue
        try:
            record = _fetch(cache, url, runner=runner)
            fetched += 1
        except ReaderError as exc:
            message = _safe_error(exc)
            try:
                client.ack_link(link["id"], error=message)
                failed += 1
                results.append({"id": link["id"], "status": "failed", "error": message})
            except ReaderError as ack_error:
                errors.append({"link": link["id"], "errors": [message, str(ack_error)]})
            continue
        try:
            metadata, data = prepare_push(cache, record["id"])
            existing = remote.get(metadata["id"])
            if existing is not None and existing.get("state") != "uploading":
                if existing.get("sha256") != metadata["sha256"] or existing.get("size") != metadata["size"]:
                    raise ReaderError("Cloud item ID already holds different bytes")
                item = existing
                synced += 1
            else:
                item = client.push(metadata, data)
                pushed += 1
        except ReaderError as exc:
            deferred += 1
            errors.append({"link": link["id"], "cache_id": record["id"],
                           "stage": "push", "errors": [str(exc)]})
            continue
        try:
            client.ack_link(link["id"], item_id=item["id"])
            acked += 1
            results.append({"id": link["id"], "status": "succeeded",
                            "cache_id": record["id"], "item_id": item["id"]})
        except ReaderError as exc:
            deferred += 1
            errors.append({"link": link["id"], "cache_id": record["id"],
                           "stage": "ack", "errors": [str(exc)]})
    return {
        "ok": not errors,
        "listed": len(links),
        "fetched": fetched,
        "pushed": pushed,
        "already_synced": synced,
        "acked": acked,
        "failed": failed,
        "deferred": deferred,
        "results": results,
        "errors": errors,
    }
