"""Plug-in delivery: when the configured SD card is mounted, deliver ready cloud books to it.

One run, started by the ``dev.reader-workbench.card-autosync`` LaunchAgent (``StartOnMount``) or by hand:

1. Resolve the exact configured volume UUID. Any other volume, or no card, does nothing at all:
   no key is read, no request is made, nothing is written.
2. Pull ready cloud EPUBs into the cache (SHA-256 checked by ``CloudClient.pull``), copy only
   those books into ``/books/Inbox`` with the ordinary ``card sync`` machinery, and import card
   annotations into the configured annotation folder. Every pass plans its requests (listing pages,
   downloads, acknowledgements) inside the server's 60-per-minute allowance: books already in the
   cache are acknowledged first, then new books are downloaded with the requests that remain.
3. Read every new card copy back (uncached) and compare its SHA-256 with the cloud item's checksum;
   a copy already proven on an earlier run is not re-read while its size and mtime are unchanged.
   Only items whose card copy matches are acknowledged, which is what frees the server bytes.
4. HTTP 429 stops the run; nothing unverified is acknowledged and the next mount retries.

It never ejects, never stages firmware, never touches ``/books/Novels`` or ``/books/Books``, and
deletes no book. The management key comes from ``READER_SYNC_KEY`` or, when that is absent, the
dedicated Keychain item; it is held only in this process and never logged.
"""

from __future__ import annotations

import fcntl
import hashlib
import os
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Callable

from . import card, inbox_card
from .cache import Cache
from .cloud import DEFAULT_ORIGIN, CloudClient
from .cloud_sync import pull_ready
from .collect import _management_key
from .common import ReaderError

# The reader API allows 60 authenticated requests per minute (listing pages, downloads and acks all count).
# Each pass plans its requests inside REQUEST_ALLOWANCE - REQUEST_MARGIN; passes run PASS_WAIT_SECONDS apart.
REQUEST_ALLOWANCE = 60
REQUEST_MARGIN = 4
# Upper bound of downloads per pass: one listing page plus a download and an ack per book.
MAX_BATCH = (REQUEST_ALLOWANCE - REQUEST_MARGIN - 1) // 2
DEFAULT_BATCH = MAX_BATCH
DEFAULT_PASSES = 3
PASS_WAIT_SECONDS = 60
LOG_LIMIT_BYTES = 512 * 1024
LOCK_NAME = ".card-autosync.lock"
NOTIFICATION_TITLE = "Reader card"


def default_log_path() -> Path:
    return Path.home() / "Library/Logs/reader-card-autosync.log"


def is_rate_limited(error: Exception) -> bool:
    return isinstance(error, ReaderError) and str(error).startswith("Cloud HTTP 429")


class RunLog:
    def __init__(self, path: Path | None, clock: Callable[[], datetime] = datetime.now):
        self.path = path
        self.clock = clock

    def __call__(self, message: str) -> None:
        if self.path is None:
            return
        line = f"{self.clock().astimezone().isoformat(timespec='seconds')} {message}\n"
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.is_file() and self.path.stat().st_size > LOG_LIMIT_BYTES:
                os.replace(self.path, self.path.with_name(self.path.name + ".1"))
            with self.path.open("a", encoding="utf-8") as stream:
                stream.write(line)
        except OSError:
            pass


def macos_notification(message: str, title: str = NOTIFICATION_TITLE) -> None:
    script = ["on run argv", "display notification (item 1 of argv) with title (item 2 of argv)", "end run"]
    command = ["/usr/bin/osascript"]
    for line in script:
        command += ["-e", line]
    try:
        subprocess.run(command + [message, title], capture_output=True, timeout=15, check=False)
    except (OSError, subprocess.SubprocessError):
        pass


def card_file_sha256(path: Path) -> str | None:
    """Hash a card file from the medium, bypassing the page cache; None when it is not a regular file."""
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError:
        return None
    try:
        if not os.path.isfile(path) or path.is_symlink():
            return None
        try:
            fcntl.fcntl(descriptor, fcntl.F_NOCACHE, 1)
        except (AttributeError, OSError):
            pass
        result = hashlib.sha256()
        while chunk := os.read(descriptor, 1024 * 1024):
            result.update(chunk)
        return result.hexdigest()
    except OSError:
        return None
    finally:
        os.close(descriptor)


def _acquire_lock(cache: Cache):
    cache.root.mkdir(parents=True, exist_ok=True)
    handle = open(cache.root / LOCK_NAME, "a")
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        return None
    return handle


def _verified_item(volume: card.CardVolume, entry: dict | None, item: dict, record: dict) -> tuple[bool, tuple[str, list[int]] | None]:
    """Is the card copy of this item byte-identical to the cloud checksum? Also a fresh proof to remember.

    A copy hashed on an earlier run is not read again while its size and mtime match that run's stamp;
    anything else (new copy, changed size or mtime, no stamp) is read back from the medium.
    """
    if entry is None or entry["state"] != "complete":
        return False, None
    path = volume.root / card.BOOK_INBOX / entry["filename"]
    stamp = card._file_stamp(path)
    if stamp is None:
        return False, None
    if stamp == card._trusted_stamp(entry, record, "card"):
        return True, None
    if card_file_sha256(path) == item["sha256"]:
        return True, (item["sha256"], stamp)
    return False, None


def autosync(
    cache: Cache,
    config: dict,
    *,
    resolve=None,
    client_factory: Callable[[], CloudClient] | None = None,
    key_provider: Callable[[], tuple[str, str]] = _management_key,
    notify: Callable[[str], None] | None = macos_notification,
    log: Callable[[str], None] | None = None,
    batch: int = DEFAULT_BATCH,
    passes: int = DEFAULT_PASSES,
    wait: Callable[[float], None] = time.sleep,
) -> dict:
    log = log or RunLog(None)
    if type(batch) is not int or not 1 <= batch <= MAX_BATCH:
        raise ReaderError(f"--batch must be an integer from 1 to {MAX_BATCH} (listing + downloads + acks must stay inside 60 requests/minute)")
    if type(passes) is not int or not 1 <= passes <= 10:
        raise ReaderError("--passes must be an integer from 1 to 10")
    expected = config["volume_uuid"]
    try:
        volume = (resolve or card.resolve_card)(expected)
    except ReaderError as exc:
        log(f"skip: configured reader card {expected} is not mounted ({exc})")
        return {"status": "skipped", "reason": "card not mounted"}
    if card._canonical_uuid(volume.volume_uuid) != card._canonical_uuid(expected):
        log(f"skip: mounted volume {volume.volume_uuid} is not the configured reader card")
        return {"status": "skipped", "reason": "volume UUID mismatch"}
    lock = _acquire_lock(cache)
    if lock is None:
        log("another autosync run holds the lock; exiting")
        return {"status": "busy"}
    try:
        return _run(cache, config, volume, client_factory, key_provider, notify, log, batch, passes, wait)
    finally:
        lock.close()


def _run(cache, config, volume, client_factory, key_provider, notify, log, batch, passes, wait) -> dict:
    expected = config["volume_uuid"]
    log(f"reader card {expected} mounted at {volume.root}; starting delivery")

    result: dict = {"status": "ok", "delivered": 0, "acked": [], "unverified": [], "errors": [],
                    "annotations": None, "passes": 0}
    card_result = None
    client = None
    previous_key = os.environ.get("READER_SYNC_KEY")
    try:
        key, key_error = key_provider()
        if key_error:
            result["status"] = "error"
            result["errors"].append(key_error)
            log(f"cloud skipped: {key_error}")
        else:
            os.environ["READER_SYNC_KEY"] = key
            try:
                client = client_factory() if client_factory else CloudClient(DEFAULT_ORIGIN, 30)
            except ReaderError as exc:
                result["status"] = "error"
                result["errors"].append(str(exc))
                log(f"cloud skipped: {exc}")
            finally:
                key = ""
        for number in range(1, passes + 1):
            if client is None:
                break
            result["passes"] = number
            try:
                outcome = _pass(cache, config, volume, client, batch, log, result["acked"])
            except ReaderError as exc:
                if is_rate_limited(exc):
                    result["status"] = "rate_limited"
                    log("HTTP 429 from the reader server: stopping; unacknowledged items are retried on the next mount")
                else:
                    result["status"] = "error"
                    log(f"error: {exc}")
                result["errors"].append(str(exc))
                break
            except OSError as exc:
                result["status"] = "error"
                result["errors"].append(str(exc))
                log(f"error: {exc}")
                break
            card_result = outcome["card"] or card_result
            result["unverified"] += outcome["unverified"]
            if outcome["rate_limited"]:
                result["status"] = "rate_limited"
                result["errors"].append("Cloud HTTP 429 while acknowledging")
                log("HTTP 429 while acknowledging: stopping; remaining verified items are acknowledged on the next mount")
                break
            if not outcome["deferred"] or not outcome["acked_now"] or number == passes:
                break
            log(f"{outcome['deferred']} more ready item(s); waiting {PASS_WAIT_SECONDS}s for the next batch")
            wait(PASS_WAIT_SECONDS)
    finally:
        if previous_key is None:
            os.environ.pop("READER_SYNC_KEY", None)
        else:
            os.environ["READER_SYNC_KEY"] = previous_key

    if card_result is None:
        # Cloud unavailable or nothing to deliver: the card's annotations are still imported.
        try:
            card_result = card.sync_card(cache, config, volume=volume, only_ids=set())
        except (ReaderError, OSError) as exc:
            result["status"] = "error" if result["status"] == "ok" else result["status"]
            result["errors"].append(str(exc))
            log(f"annotation import failed: {exc}")
    if card_result is not None:
        result["annotations"] = card_result["annotations"]
        result["card"] = card_result["card"]
    result["cleanup"] = _cleanup(cache, config, volume, log)
    for item in result["unverified"]:
        log(f"not acknowledged (card copy did not verify): {item['id']}")
    for item_id in result["acked"]:
        log(f"delivered and acknowledged: {item_id}")
    result["delivered"] = len(result["acked"])
    annotations = (result["annotations"] or {}).get("records", 0)
    log(f"done: status={result['status']} delivered={result['delivered']} annotations={annotations}")
    if notify is not None:
        notify(_message(result, annotations))
    result["acked"] = list(result["acked"])
    return result


def _pass(cache: Cache, config: dict, volume: card.CardVolume, client: CloudClient, batch: int, log,
          acked: list) -> dict:
    requests = client.requests
    items = client.items(ready_only=True)
    # Everything this pass sends must fit the per-minute allowance, so plan it first: books already in the
    # cache need only an ack (acknowledged first, they free server bytes soonest); a new book needs a
    # download and an ack. Whatever does not fit carries to the next pass or the next mount.
    budget = max(0, REQUEST_ALLOWANCE - REQUEST_MARGIN - (client.requests - requests))
    records = cache.entries()
    cached = {record["sha256"] for record in records if record.get("sha256")}
    waiting = sum(1 for item in items if item["state"] == "ready" and item["sha256"] in cached)
    pulls = min(batch, (budget - min(waiting, budget)) // 2)
    pulled = pull_ready(client, cache, items, limit=pulls, known=cached)
    # Items another client released between the listing and the download have no bytes to deliver.
    released = set(pulled["released"])
    wanted = [item for item in items if item["state"] == "ready" and item["id"] not in released]
    wanted_shas = {item["sha256"] for item in wanted}
    by_sha: dict[str, dict] = {}
    for record in records:
        if record.get("sha256") in wanted_shas and record["sha256"] not in by_sha:
            by_sha[record["sha256"]] = record
    for entry in pulled["pulled"]:
        record = cache.get(entry["cache_id"], verify=False)
        if record is not None and record["sha256"] in wanted_shas and record["sha256"] not in by_sha:
            by_sha[record["sha256"]] = record
    wanted = [item for item in wanted if item["sha256"] in by_sha]
    wanted.sort(key=lambda item: item["sha256"] not in cached)  # stable: cached books first, server order kept
    log(f"{len(items)} ready on server; pulled {len(pulled['pulled'])}, {len(pulled['already_cached'])} already cached, "
        f"{pulled['deferred']} deferred")
    card_result = card.sync_card(cache, config, volume=volume, only_ids={record["id"] for record in by_sha.values()})
    ack_budget = budget - len(pulled["pulled"]) - len(pulled["released"])
    ledger = card._load_ledger(cache, volume.volume_uuid)
    unverified, fresh_proofs = [], {}
    rate_limited = False
    carried = 0
    before = len(acked)
    for index, item in enumerate(wanted):
        if len(acked) - before >= ack_budget:
            carried = len(wanted) - index
            break
        record = by_sha[item["sha256"]]
        verified, proof = _verified_item(volume, ledger.get(record["id"]), item, record)
        if not verified:
            unverified.append(item)
            continue
        if proof is not None:
            fresh_proofs[record["id"]] = proof
        try:
            client.ack(item["id"])
        except ReaderError as exc:
            if is_rate_limited(exc):
                rate_limited = True
                break
            raise
        acked.append(item["id"])
    card.record_card_proofs(cache, volume.volume_uuid, fresh_proofs)
    if carried:
        log(f"request budget reached ({REQUEST_ALLOWANCE - REQUEST_MARGIN}/minute planned): "
            f"{carried} verified-or-unchecked item(s) are acknowledged on a later pass or mount")
    return {"card": card_result, "unverified": [{"id": item["id"]} for item in unverified],
            "deferred": pulled["deferred"] + carried, "rate_limited": rate_limited, "acked_now": len(acked) - before}


def _message(result: dict, annotations: int) -> str:
    count = result["delivered"]
    text = f"{count} book{'s' if count != 1 else ''} delivered to the reader card"
    if annotations:
        text += f"; {annotations} annotation{'s' if annotations != 1 else ''} imported"
    if result["status"] == "rate_limited":
        text += "; server rate limit reached, the rest will follow on the next plug-in"
    elif result["status"] == "error" or result["errors"]:
        text += "; problems logged in ~/Library/Logs/reader-card-autosync.log"
    if result["unverified"]:
        text += f"; {len(result['unverified'])} held back (copy not verified)"
    return text


def _cleanup(cache: Cache, config: dict, volume: card.CardVolume, log) -> dict:
    """Plan the Inbox auto-delete pass every mount; apply it only with ``inbox_cleanup: true`` in card.json."""
    apply = config.get("inbox_cleanup", False) is True
    try:
        ledger = card._load_ledger(cache, volume.volume_uuid)
        delivered = {f"/{card.BOOK_INBOX.as_posix()}/{entry['filename']}" for entry in ledger.values()}
        outcome = inbox_card.run_cleanup(volume.root, inbox_card.state_path(cache.root, volume.volume_uuid),
                                         apply=apply, delivered=delivered)
    except (ReaderError, OSError) as exc:
        log(f"inbox cleanup skipped: {exc}")
        return {"error": str(exc)}
    counts = outcome["counts"]
    mode = "applied" if apply else "dry run (set \"inbox_cleanup\": true in card.json to apply)"
    log(f"inbox cleanup {mode}: {counts['trash']} to trash, {counts['hold']} held for unconfirmed annotations, "
        f"{counts['purge']} trash entries past {inbox_card.TRASH_DAYS} days")
    for entry in outcome["files"]:
        if entry["action"] != "keep":
            log(f"  {entry['action']}: {entry['file']} ({entry['reason']})")
    return {"dry_run": outcome["dry_run"], "counts": counts, "applied": outcome["applied"]}
