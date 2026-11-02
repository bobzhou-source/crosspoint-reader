from __future__ import annotations

import time
import uuid
from pathlib import Path

from .cloud import (MAX_ANNOTATION_BODY, MAX_RESPONSE, CloudClient, identifier, integer,
                    json_bytes, parse_json, text)
from .common import ReaderError, bounded_read

ANNOTATION_NAMESPACE = uuid.UUID("c5a4683a-f89f-54d3-9b39-4a6db1da194d")
KINDS = ("quote", "question", "review_later")
FIELDS = {"id", "item_id", "book_path", "source_anchor", "selection", "kind", "note", "created_at"}


def kind_name(value) -> str:
    if type(value) is int and 0 <= value <= 2:
        return KINDS[value]
    if isinstance(value, str):
        names = {"Quote": "quote", "Question": "question", "ReviewLater": "review_later"}
        value = names.get(value.removeprefix("AnnotationKind::"), value)
        if value in KINDS:
            return value
    raise ReaderError("Annotation kind must be quote/question/review_later or native Quote/0, Question/1, ReviewLater/2")


def map_annotation(value, *, device_id=None, item_id=None, now=None) -> dict:
    if not isinstance(value, dict):
        raise ReaderError("Each annotation must be a JSON object")
    if item_id is not None:
        identifier(item_id, "item_id")
    if "sourcePath" in value:
        if device_id is None:
            raise ReaderError("Native book-local IDs require --device-id: choose a stable, nonsecret name for this device and reuse it on every upload")
        text(device_id, "device_id", 128)
        integer(value.get("id"), "native annotation id", 1, 128)
        captured = integer(value.get("createdAt"), "native createdAt (UTC seconds)") * 1000
        native_kind = kind_name(value.get("kind", 0))
        # Preserve all native fields, including pageContext and absent-vs-zero vo. Text
        # already mapped to selection/note is not duplicated within the 2048-unit anchor.
        native = {key: val for key, val in value.items() if key not in {"excerpt", "note"}}
        anchor = json_bytes({"format": "reader-workbench-native-v1", "device_id": device_id, "native": native}).decode("utf-8")
        stable = json_bytes([device_id, item_id, value]).decode("utf-8")
        record = {"id": str(uuid.uuid5(ANNOTATION_NAMESPACE, stable)), "item_id": item_id,
                  "book_path": value["sourcePath"], "source_anchor": anchor,
                  "selection": value.get("excerpt", ""), "kind": native_kind,
                  "note": value.get("note", ""), "created_at": captured}
    else:
        if set(value) - FIELDS - {"location", "sequence", "received_at"}:
            raise ReaderError("Unknown annotation fields; preserve native/location data inside source_anchor instead")
        record = {key: val for key, val in value.items() if key in FIELDS}
        if "location" in value:
            if "source_anchor" in record:
                raise ReaderError("Supply source_anchor or location, not both; neither can be discarded")
            if not isinstance(value["location"], dict):
                raise ReaderError("Annotation location must be a complete native location object")
            record["source_anchor"] = json_bytes(value["location"]).decode("utf-8")
        elif isinstance(record.get("source_anchor"), dict):
            record["source_anchor"] = json_bytes(record["source_anchor"]).decode("utf-8")
        if item_id is not None and record.get("item_id") not in (None, item_id):
            raise ReaderError("Annotation item_id conflicts with --item-id; no records sent")
        record["item_id"] = item_id if item_id is not None else record.get("item_id")
        record["kind"] = kind_name(record.get("kind"))
        record.setdefault("selection", "")
        record.setdefault("note", "")
    identifier(record.get("id"))
    if record["item_id"] is not None:
        identifier(record["item_id"], "item_id")
    text(record.get("book_path"), "book_path", 1024)
    text(record.get("source_anchor"), "source_anchor", 2048)
    text(record["selection"], "selection", 8192, empty=True, multiline=True)
    text(record["note"], "note", 8192, empty=True, multiline=True)
    captured = integer(record.get("created_at"), "created_at (epoch milliseconds)")
    if captured > int((time.time() if now is None else now) * 1000) + 300_000:
        raise ReaderError("Annotation capture time is over five minutes in the future; correct its source clock/time before upload")
    return record


APPLEDOUBLE_MAGIC = b"\x00\x05\x16\x07"


def _is_appledouble_sidecar(file: Path) -> bool:
    """macOS leaves a `._name` AppleDouble shadow beside files it writes or copies on a FAT card.

    Only a regular, non-symlink `._*` file that really starts with the AppleDouble magic is skipped.
    A `._x.json` that is not AppleDouble, or any other malformed `.json`, is still parsed and rejected.
    """
    if not file.name.startswith("._") or file.is_symlink() or not file.is_file():
        return False
    try:
        with file.open("rb") as stream:
            return stream.read(len(APPLEDOUBLE_MAGIC)) == APPLEDOUBLE_MAGIC
    except OSError:
        return False


def load_annotations(path: Path) -> list[dict]:
    path = path.expanduser()
    if path.is_dir():
        files = sorted(file for file in path.rglob("*.json") if not _is_appledouble_sidecar(file))
        if not files or len(files) > 4096:
            raise ReaderError("Native annotation directory must contain 1–4096 JSON records; use the annotations store, not Markdown exports")
    else:
        files = [path]
    records = []
    total = 0
    for file in files:
        if file.is_symlink():
            raise ReaderError("Annotation input must not be a symlink")
        raw = bounded_read(file, MAX_RESPONSE - total)
        total += len(raw)
        if file.suffix.lower() in {".ndjson", ".jsonl"}:
            values = [parse_json(line) for line in raw.splitlines() if line.strip()]
        else:
            value = parse_json(raw)
            if isinstance(value, dict) and "annotations" in value:
                values = value["annotations"]
            else:
                values = value if isinstance(value, list) else [value]
        if not isinstance(values, list) or any(not isinstance(value, dict) for value in values):
            raise ReaderError("Annotation input must be a JSON record/array, an annotations array, or NDJSON")
        records.extend(values)
        if len(records) > 4096:
            raise ReaderError("Annotation input exceeds 4096 records; no records sent")
    if not records:
        raise ReaderError("No annotation records found; no records sent")
    return records


def annotation_batches(values: list[dict], *, device_id=None, item_id=None, now=None) -> list[bytes]:
    if not values or len(values) > 4096:
        raise ReaderError("Supply 1–4096 annotations")
    seen = {}
    batches = []
    current = []
    size = len(b'{"annotations":[]}')
    for value in values:
        record = map_annotation(value, device_id=device_id, item_id=item_id, now=now)
        encoded = json_bytes(record)
        prior = seen.get(record["id"])
        if prior is not None:
            if prior != encoded:
                raise ReaderError("Conflicting annotation IDs in input; no records sent")
            continue
        seen[record["id"]] = encoded
        if len(encoded) + len(b'{"annotations":[]}') > MAX_ANNOTATION_BODY:
            raise ReaderError("A single annotation exceeds 64 KiB; use local export, never truncate")
        addition = len(encoded) + bool(current)
        if current and (len(current) == 32 or size + addition > MAX_ANNOTATION_BODY):
            batches.append(b'{"annotations":[' + b",".join(current) + b"]}")
            current = []
            size = len(b'{"annotations":[]}')
        size += len(encoded) + bool(current)
        current.append(encoded)
    if current:
        batches.append(b'{"annotations":[' + b",".join(current) + b"]}")
    return batches


def upload_annotations(client: CloudClient, batches: list[bytes]) -> dict:
    accepted = inserted = 0
    for index, body in enumerate(batches):
        try:
            result = client.json_request("POST", "/reader/annotations", body, expected=(200, 201))
            expected = len(parse_json(body)["annotations"])
            integer(result.get("accepted"), "accepted", expected, expected)
            integer(result.get("inserted"), "inserted", 0, expected)
            accepted += result["accepted"]
            inserted += result["inserted"]
        except ReaderError as exc:
            raise ReaderError(f"Annotation batch {index + 1}/{len(batches)} failed after {accepted} confirmed records. Earlier batches remain stored; rerun unchanged input for idempotency. {exc}") from exc
    return {"accepted": accepted, "inserted": inserted, "batches": len(batches)}


def download_annotations(client: CloudClient, *, after=0, limit=100, export=False) -> dict:
    integer(after, "after")
    integer(limit, "limit", 1, 100)
    records = []
    pages = []
    seen = set()
    total_bytes = 0
    cursor = after
    while True:
        route = "/reader/annotations" + ("/export" if export else "") + f"?after={cursor}&limit={limit}"
        response = client.request("GET", route)
        total_bytes += len(response.data)
        if total_bytes > MAX_RESPONSE:
            raise ReaderError("Annotation download exceeds the bounded export size; no output written")
        if export:
            page = [parse_json(line) for line in response.data.splitlines() if line.strip()]
            header = response.headers.get("x-reader-next-after")
            if header is not None and (len(header) > 16 or not header.isascii() or not header.isdecimal()):
                raise ReaderError("Invalid annotation export pagination header")
            next_after = int(header) if header is not None else None
            if response.data and not response.data.endswith(b"\n"):
                raise ReaderError("Incomplete annotation NDJSON export; no output written")
            pages.append(response.data)
        else:
            value = parse_json(response.data)
            if not isinstance(value, dict) or "next_after" not in value:
                raise ReaderError("Invalid annotation page response")
            page, next_after = value.get("annotations"), value["next_after"]
        if not isinstance(page, list) or len(page) > limit:
            raise ReaderError("Invalid annotation page size")
        for record in page:
            if not isinstance(record, dict) or set(record) != FIELDS | {"sequence", "received_at"}:
                raise ReaderError("Invalid annotation response fields")
            # Validate without modifying the returned record or its opaque anchor.
            text(record.get("source_anchor"), "source_anchor", 2048)
            map_annotation(record)
            if record["kind"] not in KINDS:
                raise ReaderError("Invalid annotation response category")
            sequence = integer(record.get("sequence"), "sequence", cursor + 1)
            integer(record.get("received_at"), "received_at")
            if record["id"] in seen:
                raise ReaderError("Duplicate annotation ID across pages")
            seen.add(record["id"])
            cursor = sequence
        records.extend(page)
        if len(records) > 4096:
            raise ReaderError("Annotation response exceeds the 4096-record storage contract")
        if next_after is None:
            break
        integer(next_after, "next_after", 1)
        if not page or next_after != cursor:
            raise ReaderError("Annotation pagination did not advance to the last receive sequence")
    return {"annotations": records, "after": cursor, "next_after": None,
            **({"ndjson": b"".join(pages)} if export else {})}
