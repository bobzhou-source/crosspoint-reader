from __future__ import annotations

import hashlib
import json
import os
import plistlib
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import uuid
from dataclasses import dataclass
from pathlib import Path

from .cache import Cache
from .cloud_annotations import load_annotations, map_annotation
from .common import ReaderError, atomic_write, bounded_read
from . import concept_index, novels, story_graph
from .book_hooks import BACKUP_SUFFIX

CONFIG_VERSION = 1
CONFIG_KEYS = {
    "version",
    "volume_uuid",
    "annotations_dir",
    "firmware_path",
    "firmware_sha256",
    "launchers_dir",
}
OPTIONAL_CONFIG_KEYS = {"inbox_cleanup"}
FIRMWARE_NAME = "reader-workbench-rollback-reviewed.bin"
FIRMWARE_CONFIG = Path(".crosspoint/reader-sync.json")
FIRMWARE_CONFIG_VERSION = 1
FIRMWARE_ORIGIN = "https://robertzhou.dev"
FIRMWARE_INTERVAL = 900
FIRMWARE_CONFIG_KEYS = {"version", "origin", "device_key", "device_id", "enabled", "interval_seconds"}
# Optional since the event-driven sync: written by the device's Schedule row. A missing key means "events".
FIRMWARE_SCHEDULE_KEY = "schedule"
FIRMWARE_SCHEDULES = ("events", "interval")
DEVICE_KEY_SERVICE = "reader-workbench:device-key"
DEVICE_KEY_ACCOUNT = "b"
LIBRARY_DIRTY = Path(".crosspoint/library.dirty")
BOOK_INBOX = Path("books/Inbox")
NOVELS_DIR = Path("books/Novels")
BOOKS_DIR = Path("books/Books")
ANNOTATION_STORE = Path(".crosspoint/annotations")
ANNOTATION_NATIVE_FORMAT = "reader-workbench-native-v1"
CLOUD_ANNOTATIONS_NAME = "cloud-annotations.ndjson"
CARD_LEDGER_TEMPLATE = ".card-ledger-{volume_uuid}.json"
CARD_LEDGER_VERSION = 1
SHARE_SHORTCUT_NAME = "Send to Reader"
MAX_FIRMWARE_BYTES = 64 * 1024 * 1024
MAX_EPUB_BYTES = 256 * 1024 * 1024
MAX_BOOK_CARD_PATH_BYTES = 255
# Offline StarDict folders live in /dictionaries/<name>/ (DictionaryRegistry);
# the reader keeps a hidden /.dictionaries root too.
DICTIONARIES_DIR = Path("dictionaries")
HIDDEN_DICTIONARIES_DIR = Path(".dictionaries")
# Lookup sidecars the reader builds next to <stem>.idx / <stem>.syn.
DICTIONARY_SIDECARS = {".idx": ".qidx", ".syn": ".sidx"}
# Model artifacts the ConceptEngine and CS-PW kernel load from /models; every other .bin/.idx in the
# Mac folder is mirrored too, and a sync result names the ones the folder lacks.
MODELS_DIR = Path("models")
MODEL_FILENAMES = ("platonic-hnet.bin", "engram.bin", "inbox-concepts.idx", "cspw.bin", "story-graph.idx",
                   "pe-writer.bin", "writer-clauses.bin")
# PE-Writer artifacts are header-checked before they reach the card: (magic, u32 LE version at
# offset 4, minimum size, exact size or None). A file that fails is not copied and is reported.
MODEL_HEADERS = {
    "pe-writer.bin": (b"PEWR", 1, 32, None),
    "writer-clauses.bin": (b"WCLS", 1, 1_048_608, 1_048_608),
}
MODEL_SUFFIXES = (".bin", ".idx")
# Reader telemetry backed up to the Mac on every sync.
BENCHMARK_LOGS = Path("Benchmark/logs")
LIQUID_REFRESH = Path(".crosspoint/liquid-refresh.bin")


@dataclass(frozen=True)
class CardVolume:
    root: Path
    device: str
    volume_uuid: str


def default_config_path() -> Path:
    return Path.home() / "Library/Application Support/reader-send/card.json"


def default_novels_dir() -> Path:
    """Where `novels convert` puts the Drive collection on this Mac."""
    return Path.home() / "papyrix" / "novels-epub"


def default_books_dir() -> Path:
    """The Mac shelf whose EPUBs are delivered to /books/Books."""
    return Path.home() / "papyrix" / "books"


def default_dictionaries_dir() -> Path:
    """The Mac StarDict folders (cedict, english) mirrored to /dictionaries."""
    return Path.home() / "papyrix" / "dictionaries"



def default_models_dir() -> Path:
    """The Mac folder of ConceptEngine model artifacts mirrored to /models."""
    return Path.home() / "papyrix" / "models"


def default_usage_dir() -> Path:
    """Where the reader's session logs and liquid-refresh table are backed up for calibration."""
    return Path.home() / "papyrix" / "verification" / "refresh-calib" / "usage"

def default_shelves() -> dict[str, Path]:
    """Existing Mac shelf sources for the automatic card copy, keyed by card shelf.

    A converted-novel shelf is only usable once it has a manifest, and a local
    book shelf or dictionary folder only once it exists, so a Mac without either
    syncs just the cloud Inbox instead of failing.
    """
    shelves: dict[str, Path] = {}
    novels_dir = default_novels_dir()
    if (novels_dir / novels.MANIFEST_NAME).is_file():
        shelves["novels"] = novels_dir
    books_dir = default_books_dir()
    if books_dir.is_dir():
        shelves["books"] = books_dir
    dictionaries_dir = default_dictionaries_dir()
    if dictionaries_dir.is_dir():
        shelves["dictionaries"] = dictionaries_dir
    models_dir = default_models_dir()
    if models_dir.is_dir():
        shelves["models"] = models_dir
    usage_dir = default_usage_dir()
    if usage_dir.is_dir():
        shelves["usage"] = usage_dir
    return shelves


def _canonical_uuid(value: str) -> str:
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError) as exc:
        raise ReaderError("Card volume UUID must be a complete UUID shown by diskutil info") from exc
    return str(parsed).upper()


def _expected_sha(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise ReaderError("Expected SHA-256 must be exactly 64 hexadecimal characters")
    return value.lower()


def _regular_file(path: Path, label: str) -> Path:
    original = path.expanduser()
    if original.is_symlink():
        raise ReaderError(f"{label} must be an existing regular file, not a symlink: {original}")
    path = original.resolve()
    if not path.is_file():
        raise ReaderError(f"{label} must be an existing regular file: {path}")
    return path


def _within(path: Path, parent: Path) -> bool:
    return path == parent or parent in path.parents


def file_sha256(path: Path, maximum: int | None = None) -> str:
    total = 0
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            total += len(chunk)
            if maximum is not None and total > maximum:
                raise ReaderError(f"File exceeds the {maximum // (1024 * 1024)} MiB safety limit: {path}")
            result.update(chunk)
    return result.hexdigest()


def resolve_card(
    volume_uuid: str,
    *,
    runner=subprocess.run,
    volumes_root: Path = Path("/Volumes"),
    mount_check=os.path.ismount,
) -> CardVolume:
    """Resolve one configured removable UUID without trusting a disk number or volume name."""
    if sys.platform != "darwin":
        raise ReaderError("SD-card discovery is supported only on macOS")
    expected = _canonical_uuid(volume_uuid)
    try:
        completed = runner(
            ["/usr/sbin/diskutil", "info", "-plist", expected],
            capture_output=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ReaderError(f"Could not query the configured reader card {expected}: {exc}") from exc
    if completed.returncode != 0:
        raise ReaderError(f"Reader card {expected} is not mounted. Insert that exact SD card and try again")
    try:
        info = plistlib.loads(completed.stdout)
    except (plistlib.InvalidFileException, ValueError, TypeError) as exc:
        raise ReaderError("diskutil returned invalid volume information; no card files were changed") from exc
    if not isinstance(info, dict):
        raise ReaderError("diskutil returned invalid volume information; no card files were changed")
    actual = info.get("VolumeUUID")
    if not isinstance(actual, str) or _canonical_uuid(actual) != expected:
        raise ReaderError("The mounted volume does not match the configured card UUID; no card files were changed")
    if info.get("Internal") is not False:
        raise ReaderError("The configured card UUID resolved to an internal volume; refusing card writes")
    if not (info.get("RemovableMedia") or info.get("Ejectable")):
        raise ReaderError("The configured card UUID is not a removable/ejectable volume; refusing card writes")
    if info.get("Writable") is False or info.get("WritableVolume") is False:
        raise ReaderError("The configured card is not writable; no card files were changed")
    mount_value = info.get("MountPoint")
    if not isinstance(mount_value, str) or not mount_value:
        raise ReaderError("The configured card is not mounted; insert that exact SD card and try again")
    mount = Path(mount_value).expanduser()
    if not mount.is_absolute() or mount.resolve() == Path("/"):
        raise ReaderError("The configured card has an unsafe mount point; refusing card writes")
    volumes = volumes_root.expanduser().resolve()
    mount = mount.resolve()
    if mount == volumes or not _within(mount, volumes):
        raise ReaderError("The configured card is outside /Volumes; refusing card writes")
    if not mount_check(mount) or not mount.is_dir():
        raise ReaderError("The configured card is not a mounted directory; no card files were changed")
    device = info.get("DeviceIdentifier")
    if not isinstance(device, str) or not device:
        raise ReaderError("diskutil did not report a device identifier; refusing card writes")
    return CardVolume(mount, device, expected)


def load_config(path: Path | None = None) -> dict:
    path = (path or default_config_path()).expanduser()
    if path.is_symlink():
        raise ReaderError(f"Card configuration path must not be a symlink: {path}")
    path = path.resolve()
    if not path.is_file():
        raise ReaderError(f"Card configuration not found: {path}; run card setup first")
    try:
        value = json.loads(bounded_read(path, 64 * 1024))
    except (OSError, ValueError, TypeError, UnicodeError) as exc:
        raise ReaderError(f"Card configuration is invalid: {path}: {exc}") from None
    if (not isinstance(value, dict) or set(value) - OPTIONAL_CONFIG_KEYS != CONFIG_KEYS
            or value.get("version") != CONFIG_VERSION):
        raise ReaderError(f"Card configuration has an unsupported schema: {path}")
    if type(value.get("inbox_cleanup", False)) is not bool:
        raise ReaderError(f"Card configuration inbox_cleanup must be true or false: {path}")
    return {
        "version": CONFIG_VERSION,
        "volume_uuid": _canonical_uuid(value["volume_uuid"]),
        "annotations_dir": str(value["annotations_dir"]),
        "firmware_path": str(value["firmware_path"]),
        "firmware_sha256": _expected_sha(value["firmware_sha256"]),
        "launchers_dir": str(value["launchers_dir"]),
        # Optional opt-in: card autosync applies the Inbox auto-delete plan only when true.
        **({"inbox_cleanup": value["inbox_cleanup"]} if "inbox_cleanup" in value else {}),
    }


def _copy_atomic(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or destination.exists():
        raise ReaderError(f"Destination appeared while copying; not overwritten: {destination}")
    fd, temporary = tempfile.mkstemp(prefix=".reader-send-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as output, source.open("rb") as input_stream:
            while chunk := input_stream.read(1024 * 1024):
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        if destination.exists() or destination.is_symlink():
            raise ReaderError(f"Destination appeared while copying; not overwritten: {destination}")
        os.replace(temporary, destination)
        try:
            directory_fd = os.open(destination.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError:
            pass
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def _verified_cache_entries(cache: Cache, only_ids: set[str] | None = None,
                            ledger: dict[str, dict] | None = None) -> tuple[list[dict], dict[str, list[int]]]:
    """Cache entries whose EPUB bytes match their recorded SHA-256, plus each file's [size, mtime_ns].

    An entry is re-hashed unless the ledger proves this exact content was hashed before and the cache
    file's size and mtime have not changed since; a changed file is always re-hashed.
    """
    ledger = ledger or {}
    records, stamps = [], {}
    if only_ids is None:
        candidates = cache.entries()
    else:  # a subset (autosync) reads just those entries instead of scanning the whole cache
        candidates = [record for record in (cache.get(item_id, verify=False) for item_id in sorted(only_ids)) if record]
    for record in candidates:
        stamp = _file_stamp(Path(record["path"]))
        if stamp is None or stamp != _trusted_stamp(ledger.get(record["id"]), record, "cache"):
            record = cache.get(record["id"], verify=True)
            if record is None:
                continue
        records.append(record)
        if stamp is not None:
            stamps[record["id"]] = stamp
    return records, stamps


def _fsync_dir(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def _ledger_path(cache: Cache, volume_uuid: str) -> Path:
    return cache.root / CARD_LEDGER_TEMPLATE.format(volume_uuid=volume_uuid)


def _load_ledger(cache: Cache, volume_uuid: str) -> dict[str, dict]:
    path = _ledger_path(cache, volume_uuid)
    if not path.exists():
        return {}
    if path.is_symlink() or not path.is_file():
        raise ReaderError(f"Card delivery ledger is not a regular file: {path}")
    try:
        value = json.loads(bounded_read(path, 1024 * 1024))
    except (OSError, ValueError, TypeError, UnicodeError) as exc:
        raise ReaderError(f"Card delivery ledger is invalid: {path}: {exc}") from None
    if (not isinstance(value, dict) or value.get("version") != CARD_LEDGER_VERSION
            or not isinstance(value.get("items"), dict)):
        raise ReaderError(f"Card delivery ledger has an unsupported schema: {path}")
    items = {}
    for item_id, entry in value["items"].items():
        if (not isinstance(item_id, str) or not item_id
                or not isinstance(entry, dict) or not {"filename", "state"} <= set(entry) <= {"filename", "state", "verified"}
                or entry.get("state") not in {"pending", "complete"}
                or not isinstance(entry.get("filename"), str)
                or Path(entry["filename"]).name != entry["filename"]
                or ("verified" in entry and not _valid_proof(entry["verified"]))):
            raise ReaderError(f"Card delivery ledger has an invalid entry: {path}")
        items[item_id] = {key: entry[key] for key in ("filename", "state", "verified") if key in entry}
    return items


def _save_ledger(cache: Cache, volume_uuid: str, items: dict[str, dict]) -> None:
    path = _ledger_path(cache, volume_uuid)
    payload = json.dumps({"version": CARD_LEDGER_VERSION, "items": items},
                         sort_keys=True, indent=2) + "\n"
    atomic_write(path, payload.encode("utf-8"), overwrite=True)
    _fsync_dir(cache.root)


def _file_stamp(path: Path) -> list[int] | None:
    """[size, mtime_ns] of a regular, non-symlink file; None for anything else or when it cannot be read."""
    try:
        status = os.lstat(path)
    except OSError:
        return None
    return [status.st_size, status.st_mtime_ns] if stat.S_ISREG(status.st_mode) else None


def _valid_stamp(value) -> bool:
    return isinstance(value, list) and len(value) == 2 and all(type(part) is int and part >= 0 for part in value)


def _valid_proof(value) -> bool:
    return (isinstance(value, dict) and 1 < len(value) and {"sha256"} <= set(value) <= {"sha256", "card", "cache"}
            and isinstance(value["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", value["sha256"]) is not None
            and all(_valid_stamp(value[key]) for key in ("card", "cache") if key in value))


def _trusted_stamp(entry: dict | None, record: dict, kind: str) -> list[int] | None:
    """The [size, mtime_ns] recorded when this exact content ("card" copy or "cache" file) was last hashed.

    None without a proof for the record's SHA-256, so the caller hashes. A matching stamp is only a
    shortcut for unchanged files: a different size or mtime never matches it.
    """
    proof = entry.get("verified") if entry is not None and entry["state"] == "complete" else None
    if proof is None or proof["sha256"] != record.get("sha256") or kind not in proof:
        return None
    return proof[kind] if proof[kind][0] == record.get("size") else None


def _proof(record: dict, card_stamp: list[int] | None, cache_stamp: list[int] | None) -> dict | None:
    if card_stamp is None and cache_stamp is None:
        return None
    return {"sha256": record["sha256"], **({"card": card_stamp} if card_stamp else {}),
            **({"cache": cache_stamp} if cache_stamp else {})}


def _entry(filename: str, state: str, proof: dict | None) -> dict:
    return {"filename": filename, "state": state, **({"verified": proof} if proof else {})}


def record_card_proofs(cache: Cache, volume_uuid: str, proofs: dict[str, tuple[str, list[int]]]) -> None:
    """Remember {item id: (sha256, [size, mtime_ns])} of card copies that were just read back and matched.

    Later runs skip re-hashing a copy while its size and mtime are unchanged. Only completed ledger
    entries are updated; a copy that was never read back never gets a stamp.
    """
    ledger = _load_ledger(cache, volume_uuid)
    changed = False
    for item_id, (sha256, stamp) in proofs.items():
        entry = ledger.get(item_id)
        if entry is None or entry["state"] != "complete":
            continue
        previous = entry.get("verified")
        kept = previous if previous is not None and previous["sha256"] == sha256 else {}
        proof = {**kept, "sha256": sha256, "card": stamp}
        if proof != previous:
            ledger[item_id] = {**entry, "verified": proof}
            changed = True
    if changed:
        _save_ledger(cache, volume_uuid, ledger)


def _candidate_names(record: dict) -> list[str]:
    name = record["filename"]
    stem = Path(name).stem
    return [name, f"{stem}-{record['sha256'][:8]}.epub", f"{record['id']}.epub"]


def _deliver_record(record: dict, inbox: Path, preferred: str | None = None,
                    trusted: list[int] | None = None) -> tuple[Path, bool, list[int] | None]:
    """Place one cache entry in the inbox; returns (path, copied, stamp).

    ``stamp`` is the [size, mtime_ns] of an existing card copy whose bytes are proven identical: hashed
    now, or ``trusted`` (recorded after an earlier hash of the ``preferred`` file) still matches. A fresh
    copy has no stamp; only a later read-back proves it.
    """
    source = Path(record["path"])
    names = _candidate_names(record)
    if preferred is not None:
        if preferred not in names:
            names.insert(0, preferred)
        else:
            names.remove(preferred)
            names.insert(0, preferred)
    candidates = [inbox / name for name in names]
    # Existing identical bytes anywhere in the ladder are already delivered.
    for candidate in candidates:
        if candidate.is_symlink():
            continue
        if candidate.is_file():
            stamp = _file_stamp(candidate)
            if trusted is not None and candidate.name == preferred and stamp == trusted:
                return candidate, False, stamp
            if file_sha256(candidate, MAX_EPUB_BYTES) == record["sha256"]:
                return candidate, False, stamp
    for candidate in candidates:
        if candidate.is_symlink() or candidate.exists():
            continue
        _copy_atomic(source, candidate)
        return candidate, True, None
    raise ReaderError(f"All card filenames for {record['filename']} already hold different bytes; nothing overwritten")


def _markdown_escape(value) -> str:
    text = value if isinstance(value, str) else str(value)
    return (text.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n")
            .replace("\t", "\\t"))


def _annotation_markdown(native_records: list[dict], volume_uuid: str) -> tuple[bytes, int]:
    lines = [f"# Reader annotations ({volume_uuid})", ""]
    mapped = []
    for native in native_records:
        mapped.append(map_annotation(native, device_id=volume_uuid))
    for record, native in zip(mapped, native_records):
        title = native.get("sourceTitle") or native.get("sourcePath") or "Untitled"
        chapter = native.get("chapterTitle") or ""
        heading = title + (f" — {chapter}" if chapter and chapter != title else "")
        lines.extend([
            f"## {_markdown_escape(heading)}",
            "",
            f"- Source: {_markdown_escape(record['book_path'])}",
            f"- Kind: {record['kind']}",
            f"- Created: {record['created_at']}",
            f"- Selection: {_markdown_escape(record['selection'])}",
            f"- Note: {_markdown_escape(record['note'])}",
            f"- Source anchor: {record['source_anchor']}",
            "",
        ])
    return ("\n".join(lines) + "\n").encode("utf-8"), len(mapped)


def _run_crossfire(volume: CardVolume, native_records: list[dict], options) -> dict:
    """Content enrichment must never break book delivery, so failures become a status value."""
    from . import crossfire as crossfire_module

    settings = dict(options) if isinstance(options, dict) else {}
    try:
        return crossfire_module.write_card(volume.root / BOOK_INBOX, native_records,
                                           corpus=settings.get("corpus", crossfire_module.DEFAULT_CORPUS),
                                           neighbors=settings.get("neighbors", crossfire_module.DEFAULT_NEIGHBORS),
                                           replies=settings.get("replies", crossfire_module.DEFAULT_REPLIES),
                                           db=settings.get("db", crossfire_module.DEFAULT_DB),
                                           limit=settings.get("limit", crossfire_module.DEFAULT_CANDIDATES),
                                           dry_run=bool(settings.get("dry_run", False)))
    except (ReaderError, OSError, ValueError) as exc:
        return {"status": "error", "error": str(exc), "written": False}


def _annotation_import(volume: CardVolume, config: dict, crossfire=None) -> tuple[bytes | None, int, Path | None, bool, bytes | None, Path | None, bool, dict | None]:
    # The cloud NDJSON export is owned by `cloud sync --annotations-out`; never
    # rewrite it from card sync, which could replace unrelated downloaded data.
    source = volume.root / ANNOTATION_STORE
    if not source.is_dir() or not any(source.rglob("*.json")):
        return None, 0, None, False, None, None, False, None
    native_records = load_annotations(source)
    if not native_records:
        return None, 0, None, False, None, None, False, None
    markdown, count = _annotation_markdown(native_records, config["volume_uuid"])
    annotation_uuid = config["volume_uuid"].lower()
    destination = Path(config["annotations_dir"]).expanduser() / f"reading-inbox-{annotation_uuid}.md"
    native_destination = Path(config["annotations_dir"]).expanduser() / f"reading-inbox-{annotation_uuid}.json"
    native_bytes = (json.dumps({
        "format": ANNOTATION_NATIVE_FORMAT,
        "device_id": config["volume_uuid"],
        "annotations": native_records,
    }, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    for path in (destination, native_destination):
        if path.is_symlink():
            raise ReaderError(f"Annotation output path must not be a symlink: {path}")
    current_markdown = destination.read_bytes() if destination.exists() and destination.is_file() else None
    current_native = native_destination.read_bytes() if native_destination.exists() and native_destination.is_file() else None
    expected_heading = f"# Reader annotations ({config['volume_uuid']})".encode("utf-8")
    if current_markdown is not None and not current_markdown.startswith(expected_heading):
        raise ReaderError(f"Refusing to replace a non-managed annotation Markdown file: {destination}")
    if current_native is not None:
        try:
            existing_native = json.loads(current_native)
        except (UnicodeError, ValueError, TypeError):
            existing_native = None
        if not (isinstance(existing_native, dict)
                and existing_native.get("format") == ANNOTATION_NATIVE_FORMAT
                and existing_native.get("device_id") == config["volume_uuid"]
                and isinstance(existing_native.get("annotations"), list)):
            raise ReaderError(f"Refusing to replace a non-managed annotation JSON file: {native_destination}")
    if current_markdown != markdown:
        atomic_write(destination, markdown, overwrite=True)
    if current_native != native_bytes:
        atomic_write(native_destination, native_bytes, overwrite=True)
    crossfire_result = _run_crossfire(volume, native_records, crossfire) if crossfire else None
    return (markdown, count, destination, current_markdown != markdown,
            native_bytes, native_destination, current_native != native_bytes, crossfire_result)


def card_status(config: dict) -> tuple[dict, bool]:
    try:
        volume = resolve_card(config["volume_uuid"])
    except ReaderError as exc:
        return {"mounted": False, "volume_uuid": config["volume_uuid"], "reason": str(exc)}, False
    return {
        "mounted": True,
        "volume_uuid": volume.volume_uuid,
        "mount": str(volume.root),
        "device": volume.device,
        "annotations_dir": str(Path(config["annotations_dir"]).expanduser()),
        "reviewed_firmware": config["firmware_path"],
        "firmware_sha256": config["firmware_sha256"],
    }, True


def sync_card(cache: Cache, config: dict, *, volume: CardVolume | None = None, novels_dir: Path | None = None,
              books_dir: Path | None = None, dictionaries_dir: Path | None = None,
              models_dir: Path | None = None, usage_dir: Path | None = None,
              only_ids: set[str] | None = None, crossfire=None) -> dict:
    volume = volume or resolve_card(config["volume_uuid"])
    if volume.volume_uuid != _canonical_uuid(config["volume_uuid"]):
        raise ReaderError("Resolved card UUID does not match the configured card; no card files were changed")
    if volume.root.is_symlink() or not volume.root.is_dir():
        raise ReaderError("Configured card mount is not a normal directory; no card files were changed")
    novel_plan = _plan_novels(volume, Path(novels_dir)) if novels_dir is not None else None
    book_plan = _plan_books(volume, Path(books_dir)) if books_dir is not None else None
    dictionary_plan = _plan_dictionaries(Path(dictionaries_dir)) if dictionaries_dir is not None else None
    models_source = _plan_models(Path(models_dir)) if models_dir is not None else None
    annotations_dir = Path(config["annotations_dir"]).expanduser()
    if annotations_dir.exists() and (annotations_dir.is_symlink() or not annotations_dir.is_dir()):
        raise ReaderError(f"Annotation output must be a normal directory: {annotations_dir}")
    annotations_dir.mkdir(parents=True, exist_ok=True)
    ledger = _load_ledger(cache, volume.volume_uuid)
    records, cache_stamps = _verified_cache_entries(cache, only_ids, ledger)
    inbox = volume.root / BOOK_INBOX
    pending: dict[str, dict] = {
        item_id: {"filename": entry["filename"], "state": "pending"}
        for item_id, entry in ledger.items() if entry["state"] == "pending"
    }
    needs_dirty = bool(pending)
    copied: list[str] = []
    unchanged: list[str] = []
    proofs: dict[str, dict | None] = {}
    for record in records:
        entry = ledger.get(record["id"])
        preferred = entry["filename"] if entry is not None else None
        path, was_copied, card_stamp = _deliver_record(record, inbox, preferred, _trusted_stamp(entry, record, "card"))
        proofs[record["id"]] = _proof(record, card_stamp, cache_stamps.get(record["id"]))
        if was_copied:
            copied.append(str(path))
        else:
            unchanged.append(str(path))
        if (was_copied or entry is None or entry["state"] != "complete"
                or entry["filename"] != path.name):
            needs_dirty = True
            pending[record["id"]] = {"filename": path.name, "state": "pending"}
    marker = volume.root / LIBRARY_DIRTY
    if pending:
        # The pending ledger is deliberately written first: if the card is
        # removed before the marker becomes durable, the retry still knows this
        # item was new and must mark the library dirty even though the bytes match.
        _save_ledger(cache, volume.volume_uuid, {**ledger, **pending})
        if needs_dirty:
            atomic_write(marker, b"")
            _fsync_dir(marker.parent)
        for item_id in pending:
            ledger[item_id] = _entry(pending[item_id]["filename"], "complete", proofs.get(item_id))
    refreshed = False
    for item_id, proof in proofs.items():
        entry = ledger[item_id]
        if proof != entry.get("verified"):
            ledger[item_id] = _entry(entry["filename"], entry["state"], proof)
            refreshed = True
    if pending or refreshed:
        _save_ledger(cache, volume.volume_uuid, ledger)
    novel_result = _deliver_shelf(volume, novel_plan) if novel_plan is not None else None
    book_result = _deliver_shelf(volume, book_plan) if book_plan is not None else None
    dictionary_result = (_deliver_dictionaries(volume, dictionary_plan)
                         if dictionary_plan is not None else None)
    models_result = sync_models_to_card(volume, models_source) if models_source is not None else None
    usage_result = _collect_usage(volume, Path(usage_dir)) if usage_dir is not None else None
    (_, annotation_count, annotation_path, annotation_updated,
     _, native_path, native_updated, crossfire_result) = _annotation_import(volume, config, crossfire)
    result = {
        "card": {"volume_uuid": volume.volume_uuid, "mount": str(volume.root), "device": volume.device},
        "books": {
            "inbox": str(inbox),
            "verified_cache_entries": len(records),
            "copied": len(copied),
            "unchanged": len(unchanged),
            "copied_paths": copied,
            "unchanged_paths": unchanged,
            "library_dirty_marker": str(marker) if marker.exists() else None,
        },
        "annotations": {
            "records": annotation_count,
            "output": str(annotation_path) if annotation_path else None,
            "updated": annotation_updated,
            "native_output": str(native_path) if native_path else None,
            "native_updated": native_updated,
        },
    }
    if crossfire_result is not None:
        result["annotations"]["crossfire"] = crossfire_result
    if novel_result is not None:
        result["novels"] = novel_result
    if book_result is not None:
        result["books"]["shelf"] = book_result
    if dictionary_result is not None:
        result["dictionaries"] = dictionary_result
    if models_result is not None:
        result["models"] = models_result
    if usage_result is not None:
        result["usage"] = usage_result
    return result


def _plan_dictionaries(source: Path) -> dict:
    """List the Mac StarDict folders the reader would accept; reads only.

    A folder qualifies like DictionaryRegistry::findStem does on the device:
    exactly one `<stem>.idx` beside `<stem>.dict` or `<stem>.dict.dz`. Hidden
    files (`._*` AppleDouble, `.DS_Store`) and reader-built sidecars stay home.
    """
    root = Path(source).expanduser()
    if root.is_symlink() or not root.is_dir():
        raise ReaderError(f"Dictionary source folder not found: {root}")
    root = root.resolve()
    folders: list[tuple[str, list[Path]]] = []
    errors: list[str] = []
    for folder in sorted(root.iterdir()):
        if folder.name.startswith(".") or not folder.is_dir():
            continue
        if folder.is_symlink():
            errors.append(f"{folder.name}: dictionary folder is a symlink; not followed")
            continue
        files = sorted(path for path in folder.iterdir()
                       if not path.name.startswith(".") and path.suffix not in DICTIONARY_SIDECARS.values()
                       and not path.is_symlink() and path.is_file())
        stems = {path.name[:-len(".idx")] for path in files if path.name.endswith(".idx")}
        names = {path.name for path in files}
        if len(stems) != 1:
            errors.append(f"{folder.name}: expected one .idx file, found {len(stems)}")
            continue
        stem = next(iter(stems))
        if f"{stem}.dict" not in names and f"{stem}.dict.dz" not in names:
            errors.append(f"{folder.name}: {stem}.idx has no {stem}.dict or {stem}.dict.dz")
            continue
        folders.append((folder.name, files))
    return {"source": str(root), "folders": folders, "errors": errors}


def _same_copy(source: Path, destination: Path) -> bool:
    try:
        local, card_copy = source.stat(), destination.lstat()
    except FileNotFoundError:
        return False
    # Copies carry the source mtime; FAT stores it at 2-second resolution.
    return (stat.S_ISREG(card_copy.st_mode) and local.st_size == card_copy.st_size
            and abs(local.st_mtime - card_copy.st_mtime) <= 2)


def _copy_verified(source: Path, destination: Path) -> None:
    """Replace `destination` with `source`, read it back against the source SHA-256, and carry the mtime.

    A copy that does not read back identically is removed and reported as a ReaderError.
    """
    expected = file_sha256(source)
    _copy_replace_atomic(source, destination)
    if file_sha256(destination) != expected:
        try:
            destination.unlink()
            removed = "; the bad copy was removed"
        except OSError as exc:
            removed = f"; could not remove the bad copy: {exc}"
        raise ReaderError(f"readback does not match the source SHA-256{removed}")
    local = source.stat()
    os.utime(destination, (local.st_atime, local.st_mtime))


def _deliver_dictionaries(volume: CardVolume, plan: dict) -> dict:
    """Copy each dictionary file that is missing or changed on the card, verified by read-back.

    Card files the Mac folder does not have are left alone. A changed `.idx` or
    `.syn` drops its lookup sidecar, which the reader judges stale by size
    alone, so the next lookup rebuilds it against the new file.
    """
    copied: list[str] = []
    unchanged: list[str] = []
    errors: list[str] = list(plan["errors"])
    for name, files in plan["folders"]:
        target = volume.root / DICTIONARIES_DIR / name
        hidden = volume.root / HIDDEN_DICTIONARIES_DIR / name
        if not target.exists() and hidden.is_dir():
            target = hidden  # keep the card's hidden copy current instead of listing the dictionary twice
        if _path_has_symlink(volume.root, target) or (target.exists() and not target.is_dir()):
            errors.append(f"{name}: card folder is not a normal directory: {target}")
            continue
        for path in files:
            destination = target / path.name
            if _same_copy(path, destination):
                unchanged.append(str(destination))
                continue
            try:
                _copy_verified(path, destination)
            except (ReaderError, OSError) as exc:
                errors.append(f"{name}/{path.name}: {exc}")
                continue
            sidecar = DICTIONARY_SIDECARS.get(path.suffix)
            if sidecar is not None:
                try:
                    destination.with_suffix(sidecar).unlink(missing_ok=True)
                except OSError as exc:
                    errors.append(f"{name}/{path.name}: copied, but {exc}")
            copied.append(str(destination))
    return {"source": plan["source"], "copied": len(copied), "unchanged": len(unchanged),
            "copied_paths": copied, "errors": errors}


def _plan_models(source: Path) -> Path:
    root = Path(source).expanduser()
    if root.is_symlink() or not root.is_dir():
        raise ReaderError(f"Model source folder not found: {root}")
    return root.resolve()


def _mirror_files(files: list[Path], target: Path, card_root: Path | None) -> dict:
    """Copy each file into `target` when missing or changed (size, mtime), verified by SHA-256 read-back."""
    copied: list[str] = []
    unchanged: list[str] = []
    errors: list[str] = []
    if (_path_has_symlink(card_root, target) if card_root is not None else target.is_symlink()) or (
            target.exists() and not target.is_dir()):
        return {"copied": 0, "unchanged": 0, "copied_paths": [],
                "errors": [f"destination is not a normal directory: {target}"]}
    for path in files:
        destination = target / path.name
        if _same_copy(path, destination):
            unchanged.append(str(destination))
            continue
        try:
            _copy_verified(path, destination)
        except (ReaderError, OSError) as exc:
            errors.append(f"{path.name}: {exc}")
            continue
        copied.append(str(destination))
    return {"copied": len(copied), "unchanged": len(unchanged), "copied_paths": copied, "errors": errors}


def sync_models_to_card(volume: CardVolume, source: Path) -> dict:
    """Refresh the card-derived indexes in `source`, then mirror every model artifact to /models.

    `inbox-concepts.idx` covers the EPUBs now in /books/Inbox and /books/Books and `story-graph.idx`
    (unless a prebuilt one is there) those plus /books/Novels, so this runs after every shelf was
    delivered. A file is copied when its size or mtime differs from the card copy and verified by
    SHA-256 read-back. Card files the Mac folder lacks are left alone.
    """
    errors: list[str] = []
    try:
        index = concept_index.refresh_index(source, volume.root)
    except (ReaderError, OSError) as exc:
        index = None
        errors.append(f"{concept_index.INDEX_NAME}: not refreshed: {exc}")
    try:
        graph = story_graph.refresh_story_graph(source, volume.root)
    except (ReaderError, OSError) as exc:
        graph = None
        errors.append(f"{story_graph.STORY_GRAPH_NAME}: not refreshed: {exc}")
    files = sorted(path for path in source.iterdir()
                   if not path.name.startswith(".") and path.suffix in MODEL_SUFFIXES
                   and not path.is_symlink() and path.is_file())
    present = {path.name for path in files}
    invalid = [problem for problem in map(_model_header_problem, files) if problem]
    files = [path for path in files if not _model_header_problem(path)]
    result = _mirror_files(files, volume.root / MODELS_DIR, volume.root)
    return {"source": str(source), "index": index, "story_graph": graph,
            "missing": [name for name in MODEL_FILENAMES if name not in present],
            **result, "errors": errors + invalid + result["errors"]}


def _model_header_problem(path: Path) -> str | None:
    """Describe why a header-checked model file is malformed, or None when it is fine (or unchecked)."""
    spec = MODEL_HEADERS.get(path.name)
    if spec is None:
        return None
    magic, version, min_size, exact_size = spec
    size = path.stat().st_size
    if exact_size is not None and size != exact_size:
        return f"{path.name}: size {size} is not the expected {exact_size} bytes; not copied"
    if size < min_size:
        return f"{path.name}: size {size} is below the minimum {min_size} bytes; not copied"
    with path.open("rb") as handle:
        head = handle.read(8)
    if head[:4] != magic:
        return f"{path.name}: magic {head[:4]!r} is not {magic!r}; not copied"
    found = int.from_bytes(head[4:8], "little")
    if found != version:
        return f"{path.name}: version {found} is not {version}; not copied"
    return None


def _collect_usage(volume: CardVolume, usage_dir: Path) -> dict:
    """Back up the reader's benchmark session logs and learned liquid-refresh table to the Mac."""
    target = Path(usage_dir).expanduser()
    logs = volume.root / BENCHMARK_LOGS
    files: list[Path] = []
    if not _path_has_symlink(volume.root, logs) and logs.is_dir():
        files += sorted(path for path in logs.glob("session-*.jsonl") if not path.is_symlink() and path.is_file())
    table = volume.root / LIQUID_REFRESH
    if not _path_has_symlink(volume.root, table) and table.is_file():
        files.append(table)
    if not files:
        return {"destination": str(target), "copied": 0, "unchanged": 0, "copied_paths": [], "errors": []}
    target.mkdir(parents=True, exist_ok=True)
    return {"destination": str(target), **_mirror_files(files, target, None)}


def _path_has_symlink(root: Path, path: Path) -> bool:
    current = root
    for part in path.relative_to(root).parts:
        current = current / part
        if current.is_symlink():
            return True
    return False


def _plan_shelf(volume: CardVolume, items: list[dict], shelf: Path, errors: list[str]) -> dict:
    """Validate hashed shelf items against the card; reads only, writes nothing.

    Shared by `/books/Novels` and `/books/Books`: an existing identical copy is
    already delivered and left alone, a name that holds different bytes falls
    back to `<stem>-<sha256[:8]>.epub`, and nothing is ever overwritten.
    """
    root = volume.root / shelf
    if _path_has_symlink(volume.root, root) or (root.exists() and not root.is_dir()):
        raise ReaderError(f"Card {shelf} folder is not a normal directory: {root}")
    planned: list[tuple[dict, Path]] = []
    unchanged: list[str] = []
    claimed: set[Path] = set()
    for item in items:
        relative = Path(item["rel"])
        if relative.is_absolute() or ".." in relative.parts or not relative.name:
            errors.append(f"{item['rel']}: unsafe path")
            continue
        if item["size"] > MAX_EPUB_BYTES:
            errors.append(f"{item['rel']}: EPUB exceeds the {MAX_EPUB_BYTES // (1024 * 1024)} MiB safety limit")
            continue
        if len((shelf / relative).as_posix().encode("utf-8")) > MAX_BOOK_CARD_PATH_BYTES:
            errors.append(f"{item['rel']}: card path is longer than the reader's {MAX_BOOK_CARD_PATH_BYTES}-byte limit")
            continue
        candidates = [root / relative, root / relative.with_name(f"{relative.stem}-{item['sha256'][:8]}.epub")]
        if _path_has_symlink(volume.root, candidates[0].parent):
            errors.append(f"{item['rel']}: a symlinked folder blocks the card path; nothing written")
            continue
        existing = next((c for c in candidates
                         if not c.is_symlink() and c.is_file() and c.stat().st_size == item["size"]
                         and file_sha256(c, MAX_EPUB_BYTES) == item["sha256"]), None)
        if existing is not None:
            unchanged.append(str(existing))
            continue
        free = next((c for c in candidates if not c.is_symlink() and not c.exists() and c not in claimed), None)
        if free is None:
            errors.append(f"{item['rel']}: every card filename already holds different bytes; nothing overwritten")
            continue
        claimed.add(free)
        planned.append((item, free))
    return {"directory": root, "planned": planned, "unchanged": unchanged, "errors": errors}


def _plan_novels(volume: CardVolume, novels_dir: Path) -> dict:
    """Validate converted novels against their manifest and the card; reads only, writes nothing."""
    items, errors = novels.deliverable_outputs(novels_dir)
    return {"source": str(novels_dir), **_plan_shelf(volume, items, NOVELS_DIR, errors)}


def local_book_items(books_dir: Path) -> tuple[list[dict], list[str]]:
    """Every EPUB below the Mac book shelf, hashed, plus problems for the rest.

    Symlinks are reported, not followed, and one copy of identical bytes is
    enough: a shelf that keeps two names for the same book does not put the same
    book on the card twice. `<book>.pre-hooks.epub` files are the originals that
    `reader-send hooks` keeps beside a book it enriched; they are never
    delivered, or the card would hold the book twice.
    """
    root = Path(books_dir).expanduser().resolve()
    if not root.is_dir():
        raise ReaderError(f"Local book shelf not found: {root}")
    items: list[dict] = []
    errors: list[str] = []
    seen: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.name.lower().endswith(BACKUP_SUFFIX):
            continue
        if path.is_symlink():
            errors.append(f"{rel}: symlink is not copied")
            continue
        if not path.is_file() or path.suffix.lower() != ".epub":
            continue
        try:
            size = path.stat().st_size
            if size > MAX_EPUB_BYTES:
                errors.append(f"{rel}: EPUB exceeds the {MAX_EPUB_BYTES // (1024 * 1024)} MiB safety limit")
                continue
            sha = file_sha256(path, MAX_EPUB_BYTES)
        except (OSError, ReaderError) as exc:
            errors.append(f"{rel}: cannot read the book: {exc}")
            continue
        if sha in seen:
            continue
        seen[sha] = rel
        items.append({"rel": rel, "path": path, "sha256": sha, "size": size})
    return items, sorted(errors)


def _plan_books(volume: CardVolume, books_dir: Path) -> dict:
    """Validate the local shelf against the card; reads only, writes nothing."""
    items, errors = local_book_items(books_dir)
    return {"source": str(Path(books_dir).expanduser().resolve()), **_plan_shelf(volume, items, BOOKS_DIR, errors)}


def _deliver_shelf(volume: CardVolume, plan: dict) -> dict:
    """Copy planned shelf books without overwriting, reading every copy back against its hash."""
    marker = volume.root / LIBRARY_DIRTY
    copied: list[str] = []
    errors: list[str] = list(plan["errors"])
    if plan["planned"]:
        # Marker first: if the card is pulled mid-copy the library is still rescanned next boot.
        atomic_write(marker, b"")
        _fsync_dir(marker.parent)
    for item, destination in plan["planned"]:
        try:
            _copy_atomic(item["path"], destination)
        except (ReaderError, OSError) as exc:
            errors.append(f"{item['rel']}: {exc}")
            continue
        try:
            readback = file_sha256(destination, MAX_EPUB_BYTES)
        except (ReaderError, OSError) as exc:
            readback = f"unreadable ({exc})"
        if readback != item["sha256"]:
            try:
                destination.unlink()
                removed = "; the bad copy was removed"
            except OSError as exc:
                removed = f"; could not remove the bad copy: {exc}"
            errors.append(f"{item['rel']}: card readback does not match the shelf EPUB{removed}")
            continue
        copied.append(str(destination))
    return {
        "source": plan["source"], "directory": str(plan["directory"]), "planned": len(plan["planned"]),
        "copied": len(copied), "unchanged": len(plan["unchanged"]), "copied_paths": copied,
        "unchanged_paths": plan["unchanged"], "errors": errors,
        "library_dirty_marker": str(marker) if copied and marker.exists() else None,
    }


def _strict_json_object(data: bytes) -> dict:
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=no_duplicates)
    except (UnicodeError, ValueError, TypeError) as exc:
        raise ValueError(f"invalid JSON: {exc}") from None
    if not isinstance(value, dict):
        raise ValueError("config must be a JSON object")
    return value


def _validate_reader_sync_config(value: dict, volume_uuid: str) -> dict:
    keys = set(value)
    if not FIRMWARE_CONFIG_KEYS <= keys <= FIRMWARE_CONFIG_KEYS | {FIRMWARE_SCHEDULE_KEY}:
        raise ValueError("config must contain exactly the frozen schema fields (plus an optional schedule)")
    if value.get("version") != FIRMWARE_CONFIG_VERSION:
        raise ValueError("unsupported config version")
    if value.get("origin") != FIRMWARE_ORIGIN:
        raise ValueError("config origin is not the dedicated reader origin")
    key = value.get("device_key")
    if not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{64}", key):
        raise ValueError("device_key must be 64 lowercase hex characters")
    try:
        device_id = _canonical_uuid(value.get("device_id"))
    except (ReaderError, TypeError, ValueError):
        raise ValueError("device_id must be a complete UUID") from None
    if device_id != _canonical_uuid(volume_uuid):
        raise ValueError("device_id does not match this configured card")
    if type(value.get("enabled")) is not bool:
        raise ValueError("enabled must be a JSON boolean")
    if type(value.get("interval_seconds")) is not int or not 60 <= value["interval_seconds"] <= 86400:
        raise ValueError("interval_seconds must be an integer from 60 to 86400")
    if FIRMWARE_SCHEDULE_KEY in value and (
            type(value[FIRMWARE_SCHEDULE_KEY]) is not str or value[FIRMWARE_SCHEDULE_KEY] not in FIRMWARE_SCHEDULES):
        raise ValueError('schedule must be "events" or "interval"')
    return {**value, "device_id": device_id}


def _keychain_device_key(*, runner=subprocess.run) -> str:
    """Read the dedicated device key without accepting it as a CLI value or printing it."""
    try:
        completed = runner(
            ["/usr/bin/security", "find-generic-password", "-s", DEVICE_KEY_SERVICE,
             "-a", DEVICE_KEY_ACCOUNT, "-w"],
            capture_output=True, text=True, timeout=10, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ReaderError(f"Could not read the dedicated reader device key from Keychain: {exc}") from None
    key = (completed.stdout or "").strip()
    if completed.returncode != 0 or not re.fullmatch(r"[0-9a-f]{64}", key):
        raise ReaderError(
            f"Dedicated reader device key is missing or invalid in Keychain service {DEVICE_KEY_SERVICE}, "
            f"account {DEVICE_KEY_ACCOUNT}; store 64 lowercase hex characters there first"
        )
    return key


def provision_reader_sync(
    config: dict,
    *,
    force: bool = False,
    rotate_key: bool = False,
    enabled: bool | None = None,
    key_reader=_keychain_device_key,
    volume: CardVolume | None = None,
) -> dict:
    """Provision the exact frozen firmware config on the configured mounted SD card.

    The device key is never accepted as an argument or printed. An existing valid key is
    preserved unless ``rotate_key`` is explicit; a replacement is read from the dedicated
    Keychain entry. No firmware state or book/annotation file is touched.
    """
    volume_uuid = _canonical_uuid(config["volume_uuid"])
    volume = volume or resolve_card(volume_uuid)
    if volume.volume_uuid != volume_uuid:
        raise ReaderError("Resolved card UUID does not match the configured card; nothing written")
    crosspoint = volume.root / ".crosspoint"
    path = volume.root / FIRMWARE_CONFIG
    if crosspoint.is_symlink() or (crosspoint.exists() and not crosspoint.is_dir()):
        raise ReaderError(f"Reader card .crosspoint path is not a normal directory: {crosspoint}")
    if path.is_symlink():
        raise ReaderError(f"Reader sync config must not be a symlink: {path}")
    if path.exists() and not path.is_file():
        raise ReaderError(f"Reader sync config path is not a regular file: {path}")
    previous = None
    invalid = False
    if path.exists():
        try:
            previous = _validate_reader_sync_config(
                _strict_json_object(bounded_read(path, 8192)), volume_uuid)
        except (ReaderError, OSError, ValueError, TypeError) as exc:
            if not force:
                raise ReaderError(
                    f"Existing reader sync config is invalid ({exc}); nothing written. Use --force to replace it"
                ) from None
            invalid = True
    if previous is not None:
        key = previous["device_key"]
        key_source = "preserved"
        if rotate_key:
            key = key_reader()
            key_source = "keychain"
        state_enabled = previous["enabled"] if enabled is None else enabled
    else:
        key = key_reader()
        key_source = "keychain"
        state_enabled = True if enabled is None else enabled
    record = {
        "version": FIRMWARE_CONFIG_VERSION,
        "origin": FIRMWARE_ORIGIN,
        "device_key": key,
        "device_id": volume_uuid,
        "enabled": bool(state_enabled),
        "interval_seconds": FIRMWARE_INTERVAL,
    }
    if previous is not None and FIRMWARE_SCHEDULE_KEY in previous:
        # The user's explicit choice (made on the device) survives re-provisioning; a fresh card
        # keeps the six legacy fields so older firmware still reads it.
        record[FIRMWARE_SCHEDULE_KEY] = previous[FIRMWARE_SCHEDULE_KEY]
    crosspoint.mkdir(mode=0o700, parents=False, exist_ok=True)
    atomic_write(path, (json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8"))
    readback = _validate_reader_sync_config(_strict_json_object(bounded_read(path, 8192)), volume_uuid)
    if readback != record:
        raise ReaderError("Reader sync config readback did not match; inspect the card before use")
    return {
        "card": {"volume_uuid": volume.volume_uuid, "mount": str(volume.root), "device": volume.device},
        "config": str(path),
        "origin": record["origin"],
        "device_id": record["device_id"],
        "enabled": record["enabled"],
        "interval_seconds": record["interval_seconds"],
        "schedule": record.get(FIRMWARE_SCHEDULE_KEY, FIRMWARE_SCHEDULES[0]),
        "device_key": "present",
        "device_key_fingerprint": hashlib.sha256(key.encode("utf-8")).hexdigest()[:8],
        "key_source": key_source,
        "replaced_invalid": invalid,
    }


def _copy_replace_atomic(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".reader-send-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as output, source.open("rb") as input_stream:
            while chunk := input_stream.read(1024 * 1024):
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, destination)
        _fsync_dir(destination.parent)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def stage_firmware(
    config: dict,
    *,
    image: Path | None = None,
    expected_sha256: str | None = None,
    volume: CardVolume | None = None,
) -> dict:
    if image is not None:
        if expected_sha256 is None:
            raise ReaderError("An explicit firmware image requires --expected-sha256; nothing was written")
        source = _regular_file(image, "Firmware image")
        expected = _expected_sha(expected_sha256)
    else:
        source = _regular_file(Path(config["firmware_path"]), "Reviewed firmware image")
        expected = _expected_sha(config["firmware_sha256"])
        if expected_sha256 is not None:
            expected = _expected_sha(expected_sha256)
    actual = file_sha256(source, MAX_FIRMWARE_BYTES)
    if actual != expected:
        raise ReaderError(f"Firmware SHA-256 mismatch: expected {expected}, got {actual}; no card files were written")
    volume = volume or resolve_card(config["volume_uuid"])
    if volume.volume_uuid != _canonical_uuid(config["volume_uuid"]):
        raise ReaderError("Resolved card UUID does not match the configured card; no card files were written")
    if _within(source, volume.root):
        raise ReaderError("Firmware image must remain on the Mac, not on the reader card; nothing written")
    destination = volume.root / FIRMWARE_NAME
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        raise ReaderError(f"Firmware destination is not a regular file: {destination}")
    staged = False
    if destination.exists() and file_sha256(destination, MAX_FIRMWARE_BYTES) == expected:
        pass
    else:
        _copy_replace_atomic(source, destination)
        staged = True
    readback = file_sha256(destination, MAX_FIRMWARE_BYTES)
    if readback != expected:
        raise ReaderError("Firmware readback SHA-256 mismatch; do not update from this card")
    next_action = (
        "Safely eject the card, insert it in the reader, then open Settings → System → SD card firmware update; "
        f"choose Select firmware file (.bin), explicitly pick {FIRMWARE_NAME} at the card root (the picker lists "
        "every visible .bin, so do not assume the first entry), and confirm Update firmware?. Do not power off while "
        "it validates or writes. This replaces the inactive slot; stock may be replaced, and the fallback becomes "
        "the previous custom version. It is not a perpetual stock fallback. No flash, reboot, or eject was performed."
    )
    return {
        "card": {"volume_uuid": volume.volume_uuid, "mount": str(volume.root), "device": volume.device},
        "image": str(source),
        "destination": str(destination),
        "sha256": readback,
        "staged": staged,
        "next_action": next_action,
    }


def _launcher_prelude(
    cache: Cache,
    config_path: Path,
    launchers_dir: Path | None,
    annotations_dir: Path | None,
) -> str:
    python = shlex.quote(str(Path(sys.executable).absolute()))
    cache_dir = shlex.quote(str(cache.root))
    config = shlex.quote(str(config_path.expanduser().resolve()))
    launchers = shlex.quote(str(launchers_dir if launchers_dir is not None else cache.root))
    annotations = shlex.quote(str(annotations_dir)) if annotations_dir is not None else ""
    annotations_out = shlex.quote(str(
        (Path(annotations_dir).expanduser() if annotations_dir is not None
         else cache.root / "annotations") / CLOUD_ANNOTATIONS_NAME
    ))
    shortcut = shlex.quote(SHARE_SHORTCUT_NAME)
    return f"""#!/bin/zsh
set -u
PYTHON={python}
CACHE={cache_dir}
CONFIG={config}
LAUNCHERS={launchers}
ANNOTATIONS={annotations}
ANNOTATIONS_OUT={annotations_out}
SHORTCUT={shortcut}
run_cli() {{
  "$PYTHON" -m reader_send --cache-dir "$CACHE" "$@"
}}
shortcut_available() {{
  /usr/bin/shortcuts list 2>/dev/null | /usr/bin/grep -Fxq "$SHORTCUT"
}}
pause() {{
  if [[ -t 0 ]]; then
    printf '\\nPress Return to continue...'
    IFS= read -r _
  fi
}}
"""


def launcher_scripts(
    cache: Cache,
    config_path: Path,
    *,
    launchers_dir: Path | None = None,
    annotations_dir: Path | None = None,
) -> dict[str, str]:
    prelude = _launcher_prelude(cache, config_path, launchers_dir, annotations_dir)
    menu_script = prelude + """
print_menu() {
  cat <<'EOF'
Reader Workbench
 1) Sync now (collect)
 2) Send a link
 3) Update firmware (verified card update)
 4) Status
 5) Open files
 q) Quit
EOF
}

send_link() {
  if ! shortcut_available; then
    printf 'The "%s" Shortcut is not installed; no link was sent.\\n' "$SHORTCUT" >&2
    return 1
  fi
  printf 'Link URL: '
  IFS= read -r url || return 2
  if [[ -z "$url" ]]; then
    printf 'No URL supplied; no link was sent.\\n' >&2
    return 2
  fi
  local tmpdir input output rc
  tmpdir=$(/usr/bin/mktemp -d -t reader-workbench-link) || {
    printf 'Could not create a temporary input file; no link was sent.\\n' >&2
    return 1
  }
  input="$tmpdir/link.txt"
  output="$tmpdir/result.txt"
  printf '%s\\n' "$url" > "$input"
  rc=0
  if /usr/bin/shortcuts run "$SHORTCUT" --input-path "$input" --output-path "$output"; then
    if [[ -s "$output" ]]; then
      printf '\\nShortcut result:\\n'
      /bin/cat "$output"
    fi
  else
    printf '\\nThe Shortcut reported an error; delivery could not be confirmed.\\n' >&2
    rc=1
  fi
  /bin/rm -f "$input" "$output" 2>/dev/null || true
  /bin/rmdir "$tmpdir" 2>/dev/null || true
  return "$rc"
}

show_status() {
  run_cli card status --config "$CONFIG" || true
  if shortcut_available; then
    printf '\\nSend to Reader shortcut: installed\\n'
  else
    printf '\\nSend to Reader shortcut: not installed\\n'
  fi
  printf 'Cache: %s\\n' "$CACHE"
  printf 'Launchers: %s\\n' "$LAUNCHERS"
  if [[ -n "$ANNOTATIONS" ]]; then
    printf 'Annotations: %s\\n' "$ANNOTATIONS"
  fi
}

open_files() {
  /usr/bin/open "$LAUNCHERS" 2>/dev/null || true
  /usr/bin/open "$CACHE" 2>/dev/null || true
  if [[ -n "$ANNOTATIONS" && -d "$ANNOTATIONS" ]]; then
    /usr/bin/open "$ANNOTATIONS" 2>/dev/null || true
  fi
}

while true; do
  print_menu
  printf 'Choice: '
  if ! IFS= read -r choice; then
    exit 0
  fi
  case "$choice" in
    1)
      if run_cli collect --annotations-out "$ANNOTATIONS_OUT"; then
        printf '\\nSync finished.\\n'
      else
        printf '\\nSync did not finish; see the error above.\\n' >&2
      fi
      ;;
    2)
      send_link || true
      ;;
    3)
      if run_cli card update --config "$CONFIG"; then
        printf '\\nFirmware staging finished.\\n'
      else
        printf '\\nFirmware staging failed; see the error above.\\n' >&2
      fi
      ;;
    4)
      show_status
      ;;
    5)
      open_files
      ;;
    q|Q)
      exit 0
      ;;
    *)
      printf 'Unknown choice: %s\\n' "$choice" >&2
      ;;
  esac
  pause
done
"""
    return {"Reader Workbench.command": menu_script}


def write_launchers(
    cache: Cache,
    config_path: Path,
    destination: Path,
    *,
    annotations_dir: Path | None = None,
) -> list[str]:
    destination = destination.expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, script in launcher_scripts(
        cache, config_path, launchers_dir=destination, annotations_dir=annotations_dir
    ).items():
        path = destination / name
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise ReaderError(f"Launcher path is not a regular file: {path}")
        if not path.exists() or path.read_bytes() != script.encode("utf-8"):
            atomic_write(path, script.encode("utf-8"))
        path.chmod(0o700)
        paths.append(str(path))
    return paths


def setup(
    cache: Cache,
    config_path: Path,
    *,
    volume_uuid: str,
    annotations_dir: Path,
    firmware_path: Path,
    firmware_sha256: str,
    launchers_dir: Path,
) -> dict:
    volume_uuid = _canonical_uuid(volume_uuid)
    volume = resolve_card(volume_uuid)
    firmware_path = _regular_file(firmware_path, "Reviewed firmware image")
    firmware_sha256 = _expected_sha(firmware_sha256)
    actual = file_sha256(firmware_path, MAX_FIRMWARE_BYTES)
    if actual != firmware_sha256:
        raise ReaderError(f"Reviewed firmware SHA-256 mismatch: expected {firmware_sha256}, got {actual}; setup was not changed")
    if _within(firmware_path, volume.root):
        raise ReaderError("Reviewed firmware must remain on the Mac, not on the reader card")
    annotations_dir = annotations_dir.expanduser().resolve()
    launchers_dir = launchers_dir.expanduser().resolve()
    config_path = config_path.expanduser()
    if config_path.is_symlink():
        raise ReaderError(f"Card configuration path must not be a symlink: {config_path}")
    config_path = config_path.resolve()
    for label, path in (("Annotation output", annotations_dir),
                        ("Launcher destination", launchers_dir),
                        ("Card configuration", config_path)):
        if _within(path, volume.root):
            raise ReaderError(f"{label} must be on the Mac, not on the reader card: {path}")
    if annotations_dir.exists() and (annotations_dir.is_symlink() or not annotations_dir.is_dir()):
        raise ReaderError(f"Annotation output must be a normal directory: {annotations_dir}")
    if launchers_dir.exists() and (launchers_dir.is_symlink() or not launchers_dir.is_dir()):
        raise ReaderError(f"Launcher destination must be a normal directory: {launchers_dir}")
    annotations_dir.mkdir(parents=True, exist_ok=True)
    launchers_dir.mkdir(parents=True, exist_ok=True)
    record = {
        "version": CONFIG_VERSION,
        "volume_uuid": volume_uuid,
        "annotations_dir": str(annotations_dir),
        "firmware_path": str(firmware_path),
        "firmware_sha256": firmware_sha256,
        "launchers_dir": str(launchers_dir),
    }
    atomic_write(config_path, (json.dumps(record, indent=2) + "\n").encode("utf-8"))
    launchers = write_launchers(cache, config_path, launchers_dir, annotations_dir=annotations_dir)
    return {
        "config": str(config_path),
        "volume_uuid": volume_uuid,
        "card_mount": str(volume.root),
        "annotations_dir": str(annotations_dir),
        "reviewed_firmware": str(firmware_path),
        "firmware_sha256": firmware_sha256,
        "launchers": launchers,
    }
