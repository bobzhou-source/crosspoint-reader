import json
import io
import math
import plistlib
import random
import struct
import subprocess
import tempfile
import unittest
import os
import zipfile
from pathlib import Path
from unittest.mock import patch

from reader_send import cache as cache_module
from reader_send import card as card_module
from reader_send import cli
from reader_send import concept_index
from reader_send import story_graph
from reader_send.cache import Cache
from reader_send.card import (BOOK_INBOX, FIRMWARE_CONFIG_KEYS, FIRMWARE_NAME, FIRMWARE_ORIGIN, CardVolume,
                              _validate_reader_sync_config, provision_reader_sync, resolve_card,
                              stage_firmware, sync_card)
from reader_send.assist import _unescape
from reader_send.cloud_annotations import annotation_batches, load_annotations
from reader_send.common import ReaderError, digest
from reader_send.epub import build_epub
from reader_send.extract import extract
from reader_send.network import PublicHTTP

UUID = "00000000-0000-0000-0000-000000000000"


def card_info(**overrides):
    info = {
        "DeviceIdentifier": "disk9s1",
        "Ejectable": True,
        "Internal": False,
        "MountPoint": "",
        "RemovableMedia": True,
        "VolumeUUID": UUID,
        "Writable": True,
        "WritableVolume": True,
    }
    info.update(overrides)
    return info


def completed(info, returncode=0):
    stdout = plistlib.dumps(info) if isinstance(info, dict) else b""
    return subprocess.CompletedProcess([], returncode, stdout=stdout, stderr=b"not found")


class CardTest(unittest.TestCase):
    def config(self, root, annotations=None, firmware=None, firmware_sha=None):
        return {
            "version": 1,
            "volume_uuid": UUID,
            "annotations_dir": str(annotations or root / "annotations"),
            "firmware_path": str(firmware or root / "reviewed.bin"),
            "firmware_sha256": firmware_sha or "0" * 64,
            "launchers_dir": str(root / "launchers"),
        }

    def cached_epub(self, root):
        cache = Cache(root / "cache")
        item = extract("<article><h1>Card book</h1><p>Readable text.</p></article>", "https://example.com/book")
        article_id = cache.key("card-book", {})
        data = build_epub(item, article_id, PublicHTTP(), no_images=True)
        return cache.store(article_id, item, data)

    def test_resolve_card_requires_matching_external_mounted_volume(self):
        with tempfile.TemporaryDirectory() as temporary:
            volumes = Path(temporary) / "Volumes"
            mount = volumes / "READER"
            mount.mkdir(parents=True)
            runner = lambda *args, **kwargs: completed(card_info(MountPoint=str(mount)))
            resolved = resolve_card(UUID, runner=runner, volumes_root=volumes,
                                    mount_check=lambda path: path == mount.resolve())
            self.assertEqual(resolved.root, mount.resolve())
            self.assertEqual(resolved.device, "disk9s1")

            unmounted = card_info()
            unmounted.pop("MountPoint")
            for bad in (
                completed({}, 1),
                completed(card_info(MountPoint=str(mount), VolumeUUID="11111111-1111-1111-1111-111111111111")),
                completed(card_info(MountPoint=str(mount), Internal=True)),
                completed(card_info(MountPoint=str(mount), RemovableMedia=False, Ejectable=False)),
                completed(card_info(MountPoint=str(mount), Writable=False)),
                completed(card_info(MountPoint=str(Path(temporary) / "unsafe"))),
                completed(unmounted),
            ):
                with self.assertRaises(ReaderError):
                    resolve_card(UUID, runner=lambda *args, **kwargs: bad, volumes_root=volumes,
                                 mount_check=lambda path: path == mount)
            self.assertEqual([p.name for p in Path(temporary).iterdir()], ["Volumes"])
            self.assertEqual(list(mount.iterdir()), [])

    def test_sync_is_idempotent_and_refuses_wrong_card(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            card.mkdir()
            volume = CardVolume(card, "disk9s1", UUID)
            cache = Cache(root / "cache")
            record = self.cached_epub(root)
            config = self.config(root)

            first = sync_card(cache, config, volume=volume)
            self.assertEqual(first["books"]["copied"], 1)
            self.assertEqual(first["books"]["unchanged"], 0)
            self.assertTrue((card / BOOK_INBOX / record["filename"]).is_file())
            self.assertTrue((card / ".crosspoint/library.dirty").is_file())
            self.assertEqual(first["annotations"]["records"], 0)

            second = sync_card(cache, config, volume=volume)
            self.assertEqual(second["books"]["copied"], 0)
            self.assertEqual(second["books"]["unchanged"], 1)
            self.assertEqual(len(list((card / BOOK_INBOX).iterdir())), 1)

            wrong = CardVolume(card, "disk9s1", "11111111-1111-1111-1111-111111111111")
            with self.assertRaises(ReaderError):
                sync_card(cache, config, volume=wrong)
            self.assertEqual(len(list((card / BOOK_INBOX).iterdir())), 1)

            destination = card / BOOK_INBOX / record["filename"]
            destination.write_bytes(b"unrelated bytes")
            third = sync_card(cache, config, volume=volume)
            self.assertEqual(third["books"]["copied"], 1)
            self.assertEqual(destination.read_bytes(), b"unrelated bytes")
            alternate = card / BOOK_INBOX / f"{destination.stem}-{record['sha256'][:8]}.epub"
            self.assertTrue(alternate.is_file())
            self.assertEqual(alternate.read_bytes(), Path(record["path"]).read_bytes())
            fourth = sync_card(cache, config, volume=volume)
            self.assertEqual(fourth["books"]["copied"], 0)
            self.assertEqual(fourth["books"]["unchanged"], 1)

    def test_interrupted_delivery_pending_ledger_marks_dirty_before_completion(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            card.mkdir()
            volume = CardVolume(card, "disk9s1", UUID)
            cache = Cache(root / "cache")
            record = self.cached_epub(root)
            inbox = card / BOOK_INBOX
            inbox.mkdir(parents=True)
            (inbox / record["filename"]).write_bytes(Path(record["path"]).read_bytes())
            ledger_path = cache.root / f".card-ledger-{UUID}.json"
            ledger_path.write_text(json.dumps({
                "version": 1,
                "items": {record["id"]: {"filename": record["filename"], "state": "pending"}},
            }), encoding="utf-8")
            self.assertFalse((card / ".crosspoint/library.dirty").exists())

            result = sync_card(cache, self.config(root), volume=volume)
            self.assertEqual(result["books"]["copied"], 0)
            self.assertEqual(result["books"]["unchanged"], 1)
            self.assertTrue((card / ".crosspoint/library.dirty").is_file())
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
            self.assertEqual(ledger["items"][record["id"]]["state"], "complete")
            self.assertEqual(len(list(inbox.iterdir())), 1)

    def test_annotations_import_preserves_anchors_without_rewriting_unchanged_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            source = card / ".crosspoint/annotations/0000000000000001"
            source.mkdir(parents=True)
            native = {
                "version": 1,
                "id": 1,
                "kind": 0,
                "sourcePath": "/books/Example.epub",
                "sourceTitle": "Example",
                "chapterTitle": "One",
                "chapterHref": "one.xhtml",
                "xpath": "/body/p[1]",
                "percentage": 0.25,
                "si": 0,
                "pc": 0,
                "pp": 0,
                "excerpt": "Exact excerpt",
                "note": "A note",
                "pageContext": False,
                "createdAt": 1700000000,
            }
            (source / "001.json").write_text(json.dumps(native), encoding="utf-8")
            cache = Cache(root / "cache")
            self.cached_epub(root)
            config = self.config(root)
            volume = CardVolume(card, "disk9s1", UUID)

            first = sync_card(cache, config, volume=volume)
            output = Path(first["annotations"]["output"])
            self.assertTrue(first["annotations"]["updated"])
            self.assertEqual(first["annotations"]["records"], 1)
            text = output.read_text(encoding="utf-8")
            decoded = _unescape(text)
            self.assertIn("# Reader annotations (" + UUID + ")", decoded)
            self.assertIn("- Source: /books/Example.epub", decoded)
            self.assertIn("reader-workbench-native-v1", decoded)
            self.assertIn("Exact excerpt", decoded)
            self.assertIn("A note", decoded)
            self.assertIn("- Source anchor: {", decoded)

            native_path = Path(first["annotations"]["native_output"])
            self.assertTrue(native_path.is_file())
            native_text = native_path.read_text(encoding="utf-8")
            native_records = load_annotations(native_path)
            self.assertEqual(len(native_records), 1)
            batches = annotation_batches(native_records, device_id=UUID)
            self.assertEqual(len(batches), 1)
            record = json.loads(batches[0])["annotations"][0]
            self.assertEqual(record["selection"], "Exact excerpt")
            self.assertEqual(record["note"], "A note")
            anchor = json.loads(record["source_anchor"])
            self.assertEqual(anchor["format"], "reader-workbench-native-v1")
            self.assertEqual(anchor["device_id"], UUID)
            self.assertEqual(anchor["native"]["sourcePath"], "/books/Example.epub")
            self.assertEqual(anchor["native"]["chapterHref"], "one.xhtml")
            self.assertEqual(anchor["native"]["xpath"], "/body/p[1]")

            second = sync_card(cache, config, volume=volume)
            self.assertFalse(second["annotations"]["updated"])
            self.assertFalse(second["annotations"]["native_updated"])
            self.assertEqual(output.read_text(encoding="utf-8"), text)
            self.assertEqual(native_path.read_text(encoding="utf-8"), native_text)

    def test_annotation_import_ignores_appledouble_shadows_on_the_card(self):
        appledouble = b"\x00\x05\x16\x07\x00\x02\x00\x00" + bytes(18) + b"Mac OS X        \x00\x02binary"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            store = card / ".crosspoint/annotations"
            source = store / "0000000000000001"
            source.mkdir(parents=True)
            native = {"version": 1, "id": 1, "kind": 0, "sourcePath": "/books/Example.epub", "sourceTitle": "Example",
                      "chapterTitle": "One", "chapterHref": "one.xhtml", "xpath": "/body/p[1]", "percentage": 0.25,
                      "si": 0, "pc": 0, "pp": 0, "excerpt": "Exact excerpt", "note": "A note",
                      "pageContext": False, "createdAt": 1700000000}
            (source / "001.json").write_text(json.dumps(native), encoding="utf-8")
            (source / "._001.json").write_bytes(appledouble)
            (store / "._0000000000000001").write_bytes(appledouble)
            cache = Cache(root / "cache")
            self.cached_epub(root)
            config = self.config(root)
            volume = CardVolume(card, "disk9s1", UUID)

            result = sync_card(cache, config, volume=volume)
            self.assertEqual(result["annotations"]["records"], 1)
            self.assertEqual(len(load_annotations(Path(result["annotations"]["native_output"]))), 1)

            # Only the AppleDouble shadows are tolerated: a malformed real record still fails the import.
            (source / "002.json").write_bytes(b"{not json")
            with self.assertRaises(ReaderError):
                sync_card(cache, config, volume=volume)

    def test_unchanged_size_and_mtime_skip_rehashing_but_a_changed_file_is_hashed_again(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card_root = root / "card"
            card_root.mkdir()
            record = self.cached_epub(root)
            cache = Cache(root / "cache")
            config = self.config(root)
            volume = CardVolume(card_root, "disk9s1", UUID)
            cache_hashes, card_hashes = [], []
            real_digest, real_card_hash = cache_module.digest, card_module.file_sha256

            def counting_digest(data):
                if isinstance(data, bytes):  # EPUB content; reading-state keys are str
                    cache_hashes.append(1)
                return real_digest(data)

            def counting_card_hash(path, maximum=None):
                card_hashes.append(path.name)
                return real_card_hash(path, maximum)

            with patch.object(cache_module, "digest", counting_digest), patch.object(card_module, "file_sha256", counting_card_hash):
                first = sync_card(cache, config, volume=volume)
                self.assertEqual((first["books"]["copied"], len(cache_hashes), card_hashes), (1, 1, []))
                # Nothing proves the card copy yet (a fresh copy is only trusted after a read-back), so it is hashed once.
                sync_card(cache, config, volume=volume)
                self.assertEqual((len(cache_hashes), len(card_hashes)), (1, 1))
                card_module.record_card_proofs(cache, UUID, {record["id"]: (record["sha256"], card_module._file_stamp(card_root / BOOK_INBOX / record["filename"]))})
                third = sync_card(cache, config, volume=volume)
                self.assertEqual((third["books"]["copied"], third["books"]["unchanged"]), (0, 1))
                self.assertEqual((len(cache_hashes), len(card_hashes)), (1, 1))  # same size and mtime: no re-hash on either side

                # Same size, different bytes, new mtime on the card: hashed again, no longer trusted, never overwritten.
                on_card = card_root / BOOK_INBOX / record["filename"]
                original = on_card.read_bytes()
                status = on_card.stat()
                on_card.write_bytes(original[:-1] + bytes([original[-1] ^ 0xFF]))
                os.utime(on_card, ns=(status.st_atime_ns, status.st_mtime_ns + 10_000_000_000))
                fourth = sync_card(cache, config, volume=volume)
                self.assertEqual(len(card_hashes), 2)
                self.assertEqual(fourth["books"]["copied"], 1)
                self.assertNotEqual(on_card.read_bytes(), original)
                self.assertEqual(sum(1 for path in (card_root / BOOK_INBOX).iterdir() if path.read_bytes() == original), 1)

                # Same size, different bytes, new mtime in the cache: hashed again and refused.
                epub = Path(record["path"])
                status = epub.stat()
                epub.write_bytes(original[:-1] + bytes([original[-1] ^ 0xFF]))
                os.utime(epub, ns=(status.st_atime_ns, status.st_mtime_ns + 10_000_000_000))
                with self.assertRaises(ReaderError):
                    sync_card(cache, config, volume=volume)
                self.assertGreater(len(cache_hashes), 1)

    def test_ledger_proofs_are_validated_and_old_ledgers_still_load(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = Cache(root / "cache")
            path = cache.root / f".card-ledger-{UUID}.json"
            path.parent.mkdir(parents=True)
            old = {"version": 1, "items": {"a" * 24: {"filename": "x.epub", "state": "complete"}}}
            path.write_text(json.dumps(old), encoding="utf-8")
            self.assertEqual(card_module._load_ledger(cache, UUID)["a" * 24], {"filename": "x.epub", "state": "complete"})
            good = {"sha256": "0" * 64, "card": [10, 20], "cache": [10, 30]}
            path.write_text(json.dumps({"version": 1, "items": {"a" * 24: {"filename": "x.epub", "state": "complete", "verified": good}}}), encoding="utf-8")
            self.assertEqual(card_module._load_ledger(cache, UUID)["a" * 24]["verified"], good)
            for bad in ({"sha256": "0" * 64}, {"sha256": "z" * 64, "card": [1, 2]}, {"sha256": "0" * 64, "card": [1]},
                        {"sha256": "0" * 64, "card": [1, -2]}, {"sha256": "0" * 64, "card": [True, 2]},
                        {"sha256": "0" * 64, "other": [1, 2]}):
                path.write_text(json.dumps({"version": 1, "items": {"a" * 24: {"filename": "x.epub", "state": "complete", "verified": bad}}}), encoding="utf-8")
                with self.assertRaises(ReaderError):
                    card_module._load_ledger(cache, UUID)

    def test_firmware_hash_is_checked_before_card_write_and_staging_is_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            card.mkdir()
            volume = CardVolume(card, "disk9s1", UUID)
            image = root / "reviewed.bin"
            image.write_bytes(b"reviewed firmware bytes")
            good = digest(image.read_bytes())
            bad_config = self.config(root, firmware=image, firmware_sha="1" * 64)
            with self.assertRaises(ReaderError):
                stage_firmware(bad_config, volume=volume)
            self.assertEqual(list(card.iterdir()), [])

            config = self.config(root, firmware=image, firmware_sha=good)
            first = stage_firmware(config, volume=volume)
            self.assertTrue(first["staged"])
            self.assertEqual(first["sha256"], good)
            self.assertEqual(first["destination"], str(card / FIRMWARE_NAME))
            self.assertTrue((card / FIRMWARE_NAME).is_file())
            self.assertEqual(Path(first["destination"]).name, FIRMWARE_NAME)
            self.assertIn(FIRMWARE_NAME, first["next_action"])
            self.assertIn("Settings → System → SD card firmware update", first["next_action"])
            self.assertIn("Update firmware?", first["next_action"])

            second = stage_firmware(config, volume=volume)
            self.assertFalse(second["staged"])
            self.assertEqual(second["sha256"], good)
            self.assertEqual((card / FIRMWARE_NAME).read_bytes(), image.read_bytes())

            with self.assertRaises(ReaderError):
                stage_firmware(config, image=image, expected_sha256=None, volume=volume)

    def reader_sync_record(self, **extra):
        record = {"version": 1, "origin": FIRMWARE_ORIGIN, "device_key": "ab" * 32, "device_id": UUID,
                  "enabled": True, "interval_seconds": 900}
        record.update(extra)
        return record

    def test_reader_sync_config_accepts_the_legacy_six_fields_and_the_optional_schedule(self):
        legacy = self.reader_sync_record()
        self.assertEqual(set(legacy), FIRMWARE_CONFIG_KEYS)
        self.assertEqual(_validate_reader_sync_config(legacy, UUID), legacy)
        for schedule in ("events", "interval"):
            checked = _validate_reader_sync_config(self.reader_sync_record(schedule=schedule), UUID)
            self.assertEqual(checked["schedule"], schedule)
        for bad in ("hourly", "Events", "", None, 1, True, ["events"]):
            with self.assertRaises(ValueError, msg=repr(bad)):
                _validate_reader_sync_config(self.reader_sync_record(schedule=bad), UUID)
        with self.assertRaises(ValueError):
            _validate_reader_sync_config(self.reader_sync_record(sync_schedule="events"), UUID)
        missing = self.reader_sync_record()
        del missing["interval_seconds"]
        with self.assertRaises(ValueError):
            _validate_reader_sync_config(missing, UUID)

    def test_provision_keeps_six_fields_by_default_and_preserves_an_explicit_schedule(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            card = root / "card"
            card.mkdir()
            volume = CardVolume(card, "disk9s1", UUID)
            config = self.config(root)
            key = "cd" * 32
            path = card / ".crosspoint/reader-sync.json"

            fresh = provision_reader_sync(config, key_reader=lambda: key, volume=volume)
            written = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(set(written), FIRMWARE_CONFIG_KEYS, "older firmware must still accept a fresh card")
            self.assertEqual(written["interval_seconds"], 900)
            self.assertEqual(fresh["schedule"], "events")

            # A card the device switched to the fixed interval keeps that choice when re-provisioned.
            written["schedule"] = "interval"
            path.write_text(json.dumps(written), encoding="utf-8")
            again = provision_reader_sync(config, enabled=False, key_reader=lambda: self.fail("key is preserved"),
                                          volume=volume)
            on_disk = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(again["schedule"], "interval")
            self.assertEqual(on_disk["schedule"], "interval")
            self.assertEqual(on_disk["device_key"], key)
            self.assertFalse(on_disk["enabled"])


class CardBooksTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.card = self.root / "card"
        self.card.mkdir()
        self.shelf = self.root / "shelf"
        (self.shelf / "english").mkdir(parents=True)
        (self.shelf / "Book One.epub").write_bytes(b"one")
        (self.shelf / "english" / "Book Two.epub").write_bytes(b"two")
        (self.shelf / "english" / "another-name.epub").write_bytes(b"one")  # same bytes as Book One
        (self.shelf / "notes.txt").write_bytes(b"not a book")
        self.volume = CardVolume(self.card, "disk9s1", UUID)
        self.cache = Cache(self.root / "cache")
        self.config = CardTest.config(self, self.root)

    def sync(self):
        return sync_card(self.cache, self.config, volume=self.volume, books_dir=self.shelf)

    def test_copies_shelf_epubs_once_with_a_library_marker(self):
        result = self.sync()["books"]["shelf"]
        self.assertEqual((result["planned"], result["copied"], result["unchanged"], result["errors"]), (2, 2, 0, []))
        library = self.card / card_module.BOOKS_DIR
        self.assertEqual((library / "Book One.epub").read_bytes(), b"one")
        self.assertEqual((library / "english" / "Book Two.epub").read_bytes(), b"two")
        self.assertFalse((library / "english" / "another-name.epub").exists())
        self.assertFalse((library / "notes.txt").exists())
        self.assertTrue((self.card / card_module.LIBRARY_DIRTY).is_file())

    def test_rerun_leaves_the_card_alone_and_writes_no_new_marker(self):
        self.sync()
        (self.card / card_module.LIBRARY_DIRTY).unlink()
        before = sorted((str(path.relative_to(self.card)), path.read_bytes())
                        for path in self.card.rglob("*") if path.is_file())
        result = self.sync()["books"]["shelf"]
        self.assertEqual((result["copied"], result["unchanged"], result["errors"]), (0, 2, []))
        self.assertEqual(sorted((str(path.relative_to(self.card)), path.read_bytes())
                                for path in self.card.rglob("*") if path.is_file()), before)
        self.assertFalse((self.card / card_module.LIBRARY_DIRTY).exists())

    def test_pre_hooks_backups_are_never_delivered_as_duplicate_books(self):
        (self.shelf / "Book One.pre-hooks.epub").write_bytes(b"one, before the front card")
        (self.shelf / "english" / "Book Two.PRE-HOOKS.EPUB").write_bytes(b"two, before the front card")
        os.symlink(self.root / "missing.epub", self.shelf / "Book Three.pre-hooks.epub")
        items, errors = card_module.local_book_items(self.shelf)
        self.assertEqual(sorted(item["rel"] for item in items), ["Book One.epub", "english/Book Two.epub"])
        self.assertEqual(errors, [])
        result = self.sync()["books"]["shelf"]
        self.assertEqual((result["planned"], result["copied"], result["errors"]), (2, 2, []))
        delivered = sorted(path.relative_to(self.card / card_module.BOOKS_DIR).as_posix()
                           for path in (self.card / card_module.BOOKS_DIR).rglob("*.epub"))
        self.assertEqual(delivered, ["Book One.epub", "english/Book Two.epub"])

    def test_different_card_bytes_are_preserved_under_a_collision_name(self):
        library = self.card / card_module.BOOKS_DIR
        library.mkdir(parents=True)
        (library / "Book One.epub").write_bytes(b"robert's own copy")
        result = self.sync()["books"]["shelf"]
        self.assertEqual((result["copied"], result["errors"]), (2, []))
        self.assertEqual((library / "Book One.epub").read_bytes(), b"robert's own copy")
        self.assertEqual([path.read_bytes() for path in library.glob("Book One-*.epub")], [b"one"])

    def test_a_missing_shelf_writes_nothing(self):
        with self.assertRaisesRegex(ReaderError, "Local book shelf not found"):
            sync_card(self.cache, self.config, volume=self.volume, books_dir=self.root / "nope")
        self.assertEqual(list(self.card.iterdir()), [])
        self.assertFalse((self.root / "annotations").exists())

    def test_symlinked_shelf_books_are_reported_not_followed(self):
        outside = self.root / "outside.epub"
        outside.write_bytes(b"outside")
        os.symlink(outside, self.shelf / "linked.epub")
        result = self.sync()["books"]["shelf"]
        self.assertEqual(result["planned"], 2)
        self.assertEqual(len(result["errors"]), 1)
        self.assertIn("symlink", result["errors"][0])
        self.assertFalse((self.card / card_module.BOOKS_DIR / "linked.epub").exists())

    def test_the_default_shelves_only_include_existing_sources(self):
        with patch.object(card_module, "default_novels_dir", return_value=self.root / "novels"), \
             patch.object(card_module, "default_books_dir", return_value=self.root / "books"), \
             patch.object(card_module, "default_dictionaries_dir", return_value=self.root / "dictionaries"), \
             patch.object(card_module, "default_models_dir", return_value=self.root / "models"), \
             patch.object(card_module, "default_usage_dir", return_value=self.root / "usage"):
            self.assertEqual(card_module.default_shelves(), {})
            (self.root / "books").mkdir()
            self.assertEqual(card_module.default_shelves(), {"books": self.root / "books"})
            (self.root / "novels").mkdir()
            self.assertEqual(card_module.default_shelves(), {"books": self.root / "books"})
            (self.root / "novels" / ".novels-manifest.json").write_text('{"version": 1, "entries": {}}')
            self.assertEqual(card_module.default_shelves(),
                             {"novels": self.root / "novels", "books": self.root / "books"})
            (self.root / "dictionaries").mkdir()
            self.assertEqual(card_module.default_shelves()["dictionaries"], self.root / "dictionaries")
            (self.root / "models").mkdir()
            (self.root / "usage").mkdir()
            self.assertEqual((card_module.default_shelves()["models"], card_module.default_shelves()["usage"]),
                             (self.root / "models", self.root / "usage"))

    def test_cli_parses_the_books_option(self):
        args = cli.parser().parse_args(["card", "sync", "--books", str(self.shelf)])
        self.assertEqual(args.books, self.shelf)
        self.assertIsNone(cli.parser().parse_args(["card", "sync"]).books)

    def test_card_sync_uses_the_default_shelves_and_an_explicit_option_wins(self):
        calls = []

        def recorded(cache, config, **options):
            calls.append(options)
            return {"books": {}}

        with patch.object(card_module, "default_shelves",
                          lambda: {"novels": Path("/novels"), "books": Path("/books"), "dictionaries": Path("/dicts"),
                                   "models": Path("/models"), "usage": Path("/usage")}), \
                patch.object(card_module, "load_config", lambda path: self.config), \
                patch.object(card_module, "sync_card", recorded), \
                patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(cli.main(["--cache-dir", str(self.cache.root), "card", "sync"]), 0)
            self.assertEqual(cli.main(["--cache-dir", str(self.cache.root), "card", "sync",
                                       "--books", str(self.shelf), "--dictionaries", str(self.root),
                                       "--models", str(self.root / "m"), "--usage", str(self.root / "u")]), 0)
        self.assertEqual((calls[0]["novels_dir"], calls[0]["books_dir"], calls[0]["dictionaries_dir"],
                          calls[0]["models_dir"], calls[0]["usage_dir"]),
                         (Path("/novels"), Path("/books"), Path("/dicts"), Path("/models"), Path("/usage")))
        self.assertEqual((calls[1]["novels_dir"], calls[1]["books_dir"], calls[1]["dictionaries_dir"],
                          calls[1]["models_dir"], calls[1]["usage_dir"]),
                         (Path("/novels"), self.shelf, self.root, self.root / "m", self.root / "u"))


class CardDictionariesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.card = self.root / "card"
        self.card.mkdir()
        self.source = self.root / "dictionaries"
        english = self.source / "english"
        english.mkdir(parents=True)
        for name, data in (("english.idx", b"idx"), ("english.dict", b"dict"), ("english.syn", b"syn"),
                           ("english.ifo", b"ifo"), ("LICENSE.txt", b"license"), ("._english.idx", b"apple"),
                           (".DS_Store", b"finder")):
            (english / name).write_bytes(data)
        cedict = self.source / "cedict"
        cedict.mkdir()
        (cedict / "cedict.idx").write_bytes(b"cidx")
        (cedict / "cedict.dict.dz").write_bytes(b"cdz")
        (self.source / "broken").mkdir()
        (self.source / "broken" / "broken.idx").write_bytes(b"no dict")
        self.volume = CardVolume(self.card, "disk9s1", UUID)
        self.cache = Cache(self.root / "cache")
        self.config = CardTest.config(self, self.root)

    def sync(self):
        return sync_card(self.cache, self.config, volume=self.volume, dictionaries_dir=self.source)["dictionaries"]

    def files(self):
        return sorted(path.relative_to(self.card).as_posix() for path in self.card.rglob("*")
                      if path.is_file() and path.parts[len(self.card.parts)] == "dictionaries")

    def test_copies_missing_dictionaries_then_leaves_them_alone(self):
        result = self.sync()
        self.assertEqual((result["copied"], result["unchanged"]), (7, 0))
        self.assertEqual(len(result["errors"]), 1)
        self.assertIn("broken", result["errors"][0])
        self.assertEqual(self.files(), [
            "dictionaries/cedict/cedict.dict.dz", "dictionaries/cedict/cedict.idx",
            "dictionaries/english/LICENSE.txt", "dictionaries/english/english.dict",
            "dictionaries/english/english.idx", "dictionaries/english/english.ifo",
            "dictionaries/english/english.syn"])
        self.assertEqual((self.card / "dictionaries/english/english.idx").read_bytes(), b"idx")
        again = self.sync()
        self.assertEqual((again["copied"], again["unchanged"]), (0, 7))

    def test_a_changed_index_is_replaced_and_its_stale_sidecar_dropped(self):
        self.sync()
        card_english = self.card / "dictionaries/english"
        (card_english / "english.qidx").write_bytes(b"old samples")
        (card_english / "english.sidx").write_bytes(b"syn samples")
        (card_english / "notes.txt").write_bytes(b"robert's file")
        source_idx = self.source / "english/english.idx"
        source_idx.write_bytes(b"IDX")  # same size, new bytes and mtime
        os.utime(source_idx, (source_idx.stat().st_atime, source_idx.stat().st_mtime + 60))
        result = self.sync()
        self.assertEqual((result["copied"], result["copied_paths"]), (1, [str(card_english / "english.idx")]))
        self.assertEqual((card_english / "english.idx").read_bytes(), b"IDX")
        self.assertFalse((card_english / "english.qidx").exists())
        self.assertTrue((card_english / "english.sidx").exists())
        self.assertEqual((card_english / "notes.txt").read_bytes(), b"robert's file")

    def test_a_hidden_card_copy_is_updated_in_place(self):
        (self.card / ".dictionaries/cedict").mkdir(parents=True)
        self.sync()
        self.assertEqual((self.card / ".dictionaries/cedict/cedict.idx").read_bytes(), b"cidx")
        self.assertFalse((self.card / "dictionaries/cedict").exists())
        self.assertTrue((self.card / "dictionaries/english/english.idx").is_file())

    def test_a_missing_source_writes_nothing(self):
        with self.assertRaisesRegex(ReaderError, "Dictionary source folder not found"):
            sync_card(self.cache, self.config, volume=self.volume, dictionaries_dir=self.root / "nope")
        self.assertEqual(list(self.card.iterdir()), [])


def tiny_epub(title: str, body: str) -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml",
                         '<?xml version="1.0"?><container><rootfiles>'
                         '<rootfile full-path="OEBPS/content.opf"/></rootfiles></container>')
        archive.writestr("OEBPS/content.opf",
                         f'<package><metadata><dc:title>{title}</dc:title></metadata>'
                         '<manifest><item id="c1" href="text/one.xhtml"/><item id="c2" href="text/two.xhtml"/></manifest>'
                         '<spine><itemref idref="c1"/><itemref idref="c2"/></spine></package>')
        archive.writestr("OEBPS/text/one.xhtml",
                         f"<html><head><style>p {{}}</style></head><body><p>{body}</p></body></html>")
        archive.writestr("OEBPS/text/two.xhtml", "<html><body><p>Second &amp; last.</p></body></html>")
    return stream.getvalue()


def tiny_model(seed: int = 7) -> bytes:
    """A valid PLHN v1 file: random int8 weights, power-of-two scales (embed 2^-5, router 2^-6, proj 2^-6, core 2^-7)."""
    generator = random.Random(seed)
    header = concept_index.MODEL_HEADER.pack(concept_index.MODEL_MAGIC, 1, 64, 128, 2, 2, 32, 16,
                                             2 ** -5, 2 ** -6, 2 ** -6, 2 ** -7, 0)
    payload = bytes(generator.randrange(-40, 41) & 0xFF for _ in range(concept_index.MODEL_PAYLOAD_BYTES))
    return header + payload


def tiny_cspw(fill: int = 1) -> bytes:
    """A cspw.bin of the contract size (393,728 bytes): CSPW v2 header, constant int8 payload."""
    header = struct.pack("<4sHHIIIIII", b"CSPW", 2, 0, 192, 128, 4, 256, 512, 0).ljust(512, b"\0")
    return header + bytes([fill]) * (393_728 - 512)


def tiny_engram(slots: int = 64, seed: int = 3) -> bytes:
    generator = random.Random(seed)
    header = concept_index.ENGRAM_HEADER.pack(concept_index.ENGRAM_MAGIC, 1, slots, 64, 2 ** -5)
    sector = header + bytes(512 - len(header))
    body = b"".join(bytes(8) + bytes(generator.randrange(-9, 10) & 0xFF for _ in range(24)) + bytes(32)
                    for _ in range(slots))
    return sector + body


class ConceptIndexTest(unittest.TestCase):
    def test_fnv1a64_matches_the_reference_vectors(self):
        self.assertEqual(concept_index.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(concept_index.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(concept_index.fnv1a64(b"foobar"), 0x85944171F73967E8)

    def test_the_contract_sizes(self):
        self.assertEqual(concept_index.MODEL_HEADER.size, 32)
        self.assertEqual(concept_index.MODEL_PAYLOAD_BYTES, 94_976)
        self.assertEqual((concept_index.INDEX_HEADER.size, concept_index.INDEX_RECORD.size), (16, 192))

    def test_a_bigram_slot_applies_only_on_an_exact_span_hash_match(self):
        text = "萧炎和纳兰 met at dawn".encode("utf-8")
        engram = bytearray(tiny_engram())
        model = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(bytes(engram)))
        spans = model.chunks(text)
        number = next(n for n in range(1, len(spans)) if spans[n][1] - spans[n - 1][0] <= 18)
        span_hash = concept_index.fnv1a64(text[spans[number - 1][0]:spans[number][1]])
        offset = 512 + (span_hash % 64) * 64
        struct.pack_into("<24b", engram, offset + 8, *([100] * 24))
        without_hash = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(bytes(engram)))
        struct.pack_into("<Q", engram, offset, span_hash)
        with_hash = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(bytes(engram)))
        self.assertEqual(with_hash.engram.bigram_delta(text[spans[number - 1][0]:spans[number][1]]), [50] * 24)
        self.assertIsNone(without_hash.engram.bigram_delta(text[spans[number - 1][0]:spans[number][1]]))
        self.assertNotEqual(with_hash.encode(text), without_hash.encode(text))
        struct.pack_into("<Q", engram, offset, span_hash ^ 1)  # same slot, another span
        self.assertEqual(concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(bytes(engram))).encode(text),
                         without_hash.encode(text))

    def test_the_model_encodes_deterministically_to_a_unit_int8_vector(self):
        model = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(tiny_engram()))
        text = "Platonic engrams: 柏拉图 remembers spans. ".encode("utf-8") * 8  # past the 256-byte input cap
        vector = model.encode(text)
        self.assertEqual(vector, model.encode(text + b"ignored past 256 bytes" * 20))
        self.assertEqual(len(vector), 128)
        self.assertTrue(all(-127 <= value <= 127 for value in vector))
        self.assertAlmostEqual(math.sqrt(sum(value * value for value in vector)), 127, delta=6)
        self.assertNotEqual(vector, concept_index.PlatonicHNet(tiny_model()).encode(text))  # the engram counts
        self.assertEqual(model.encode(b""), [0] * 128)
        spans = model.chunks(text)
        self.assertEqual((spans[0][0], spans[-1][1]), (0, min(len(text), 256)))
        for start, end in spans:
            self.assertLessEqual(end - start, 12)
            self.assertNotEqual(text[start] & 0xC0, 0x80)  # never starts inside a UTF-8 character

    @unittest.skipIf(concept_index._np is None, "numpy is not installed")
    def test_the_numpy_path_matches_the_pure_python_reference(self):
        model = concept_index.PlatonicHNet(tiny_model(seed=11), concept_index.Engram(tiny_engram()))
        for text in ("Short.".encode(), "Mixed 中文 and ASCII text, 🙂 emoji. ".encode("utf-8") * 9, bytes(range(256))):
            fast = (model.chunks(text), model.encode(text))
            with patch.object(concept_index, "_np", None):
                self.assertEqual((model.chunks(text), model.encode(text)), fast)

    def test_a_malformed_model_is_rejected(self):
        with self.assertRaisesRegex(ReaderError, "truncated"):
            concept_index.PlatonicHNet(tiny_model()[:-1])
        with self.assertRaisesRegex(ReaderError, "PLHN"):
            concept_index.PlatonicHNet(b"XXXX" + tiny_model()[4:])

    def test_the_fallback_encoder_folds_ascii_case(self):
        vector = concept_index.fallback_encode(b"Hello World")
        self.assertEqual(vector, concept_index.fallback_encode(b"hello world"))
        self.assertAlmostEqual(math.sqrt(sum(value * value for value in vector)), 127, delta=6)
        self.assertEqual(concept_index.fallback_encode(b""), [0] * 128)
        self.assertEqual(len(concept_index.fallback_encode(b"ab")), 128)

    def test_epub_text_reads_the_title_then_the_spine(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "x.epub"
            path.write_bytes(tiny_epub("A Title", "First words"))
            self.assertEqual(concept_index.epub_text(path), ("A Title", b"A Title\nFirst words Second & last."))
            path.write_bytes(b"not a zip")
            self.assertEqual(concept_index.epub_text(path), ("x", b"x\n"))


class CardModelsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.card = self.root / "card"
        (self.card / "books/Inbox/.trash").mkdir(parents=True)
        (self.card / "books/Books/Shelf").mkdir(parents=True)
        (self.card / "books/Inbox/Fresh Article.epub").write_bytes(tiny_epub("Fresh Article", "Inbox body text"))
        (self.card / "books/Books/Shelf/Old Book.epub").write_bytes(tiny_epub("Old Book", "Shelf body text"))
        (self.card / "books/Inbox/.trash/Gone.epub").write_bytes(tiny_epub("Gone", "trashed"))
        self.models = self.root / "models"
        self.models.mkdir()
        (self.models / "platonic-hnet.bin").write_bytes(tiny_model())
        (self.models / "engram.bin").write_bytes(tiny_engram())
        (self.models / "pe-writer.bin").write_bytes(b"PEWR" + struct.pack("<HH", 1, 128) + bytes(24))
        (self.models / "writer-clauses.bin").write_bytes(b"WCLS" + struct.pack("<HH", 1, 64) + bytes(1_048_600))
        (self.models / "extra-artifact.idx").write_bytes(b"extra")
        (self.models / "notes.txt").write_bytes(b"not a model")
        (self.models / ".hidden.bin").write_bytes(b"hidden")
        self.volume = CardVolume(self.card, "disk9s1", UUID)
        self.cache = Cache(self.root / "cache")
        self.config = CardTest.config(self, self.root)

    def sync(self, **options):
        return sync_card(self.cache, self.config, volume=self.volume, models_dir=self.models, **options)

    def card_models(self):
        return sorted(path.name for path in (self.card / "models").iterdir())

    def expected_vector(self, title, body):
        model = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(tiny_engram()))
        return model.encode(f"{title}\n{body} Second & last.".encode("utf-8"))

    def test_models_are_mirrored_once_and_verified(self):
        result = self.sync()["models"]
        self.assertEqual(result["errors"], [])
        self.assertEqual((result["copied"], result["unchanged"]), (7, 0))
        self.assertEqual(self.card_models(), ["engram.bin", "extra-artifact.idx", "inbox-concepts.idx",
                                              "pe-writer.bin", "platonic-hnet.bin", "story-graph.idx",
                                              "writer-clauses.bin"])
        self.assertEqual(result["missing"], ["cspw.bin"])
        for name in self.card_models():
            self.assertEqual(card_module.file_sha256(self.card / "models" / name),
                             card_module.file_sha256(self.models / name))
        again = self.sync()["models"]
        self.assertEqual((again["copied"], again["unchanged"]), (0, 7))
        self.assertEqual((again["index"]["encoded"], again["index"]["updated"]), (0, False))

    def test_a_changed_model_is_replaced_and_the_index_rebuilt(self):
        self.sync()
        for index in (self.models / "inbox-concepts.idx", self.card / "models/inbox-concepts.idx"):
            os.utime(index, (index.stat().st_atime, index.stat().st_mtime - 60))  # an earlier sync
        model = self.models / "platonic-hnet.bin"
        model.write_bytes(tiny_model(seed=8))
        os.utime(model, (model.stat().st_atime, model.stat().st_mtime + 60))
        result = self.sync()["models"]
        self.assertEqual(result["index"]["encoded"], 2)
        self.assertEqual(sorted(Path(path).name for path in result["copied_paths"]),
                         ["inbox-concepts.idx", "platonic-hnet.bin"])
        self.assertEqual((self.card / "models/platonic-hnet.bin").read_bytes(), tiny_model(seed=8))

    def test_a_copy_that_does_not_read_back_is_removed_and_reported(self):
        original = card_module._copy_replace_atomic

        def corrupting(source, destination):
            original(source, destination)
            if destination.name == "engram.bin":
                data = bytearray(destination.read_bytes())
                data[-1] ^= 0xFF
                destination.write_bytes(bytes(data))

        with patch.object(card_module, "_copy_replace_atomic", corrupting):
            result = self.sync()["models"]
        self.assertEqual(len(result["errors"]), 1)
        self.assertIn("engram.bin: readback does not match the source SHA-256; the bad copy was removed",
                      result["errors"][0])
        self.assertNotIn("engram.bin", self.card_models())
        self.assertEqual(result["copied"], 6)
        self.assertEqual(self.sync()["models"]["copied"], 1)  # retried on the next sync

    def test_malformed_writer_models_are_not_copied_and_reported(self):
        (self.models / "pe-writer.bin").write_bytes(b"PEWX" + struct.pack("<HH", 1, 128) + bytes(24))
        (self.models / "writer-clauses.bin").write_bytes(b"WCLS" + struct.pack("<HH", 1, 64) + bytes(100))
        result = self.sync()["models"]
        self.assertEqual(len(result["errors"]), 2)
        self.assertIn("pe-writer.bin: magic b'PEWX' is not b'PEWR'; not copied", result["errors"])
        self.assertIn("writer-clauses.bin: size 108 is not the expected 1048608 bytes; not copied",
                      result["errors"])
        self.assertNotIn("pe-writer.bin", self.card_models())
        self.assertNotIn("writer-clauses.bin", self.card_models())
        self.assertEqual(result["missing"], ["cspw.bin"])

    def test_a_writer_model_with_the_wrong_version_or_too_short_is_rejected(self):
        (self.models / "pe-writer.bin").write_bytes(b"PEWR" + struct.pack("<HH", 2, 128) + bytes(24))
        self.assertIn("pe-writer.bin: version 2 is not 1; not copied", self.sync()["models"]["errors"])
        (self.models / "pe-writer.bin").write_bytes(b"PEWR" + struct.pack("<HH", 1, 128))
        self.assertIn("pe-writer.bin: size 8 is below the minimum 32 bytes; not copied",
                      self.sync()["models"]["errors"])

    def test_the_icpt_index_covers_inbox_and_books(self):
        result = self.sync()["models"]["index"]
        self.assertEqual((result["encoder"], result["records"], result["encoded"], result["updated"]),
                         ("platonic-hnet.bin", 2, 2, True))
        data = (self.card / "models/inbox-concepts.idx").read_bytes()
        self.assertEqual(data, (self.models / "inbox-concepts.idx").read_bytes())
        self.assertEqual(data[:16], struct.pack("<IIII", 0x49435054, 1, 2, 128))
        self.assertEqual(len(data), 16 + 2 * 192)
        records = {record["path_hash"]: record for record in concept_index.read_index(data)}
        inbox = records[concept_index.fnv1a64(b"/books/Inbox/Fresh Article.epub")]
        shelf = records[concept_index.fnv1a64(b"/books/Books/Shelf/Old Book.epub")]
        self.assertEqual((inbox["label"], inbox["page_or_chunk"]), ("Fresh Article", 0))
        self.assertEqual(inbox["timestamp_or_flags"],
                         int((self.card / "books/Inbox/Fresh Article.epub").stat().st_mtime))
        self.assertEqual(inbox["vec"], self.expected_vector("Fresh Article", "Inbox body text"))
        self.assertEqual(shelf["vec"], self.expected_vector("Old Book", "Shelf body text"))
        self.assertEqual(data[16 + 16:16 + 16 + 48], b"Old Book".ljust(48, b"\0"))  # records sorted by card path

    def test_only_a_new_or_changed_book_is_encoded_again(self):
        self.sync()
        (self.card / "books/Inbox/Another.epub").write_bytes(tiny_epub("Another", "more"))
        result = self.sync()["models"]["index"]
        self.assertEqual((result["records"], result["encoded"]), (3, 1))
        (self.card / "books/Inbox/Another.epub").unlink()
        result = self.sync()["models"]["index"]
        self.assertEqual((result["records"], result["encoded"], result["updated"]), (2, 0, True))

    def test_without_a_model_the_fallback_encoder_is_used(self):
        (self.models / "platonic-hnet.bin").unlink()
        result = self.sync()["models"]
        self.assertEqual(result["index"]["encoder"], concept_index.FALLBACK_ENCODER)
        records = concept_index.read_index((self.card / "models/inbox-concepts.idx").read_bytes())
        record = next(record for record in records if record["label"] == "Fresh Article")
        self.assertEqual(record["vec"], concept_index.fallback_encode(b"Fresh Article\nInbox body text Second & last."))
        (self.models / "platonic-hnet.bin").write_bytes(tiny_model())
        self.assertEqual(self.sync()["models"]["index"]["encoded"], 2)  # a new encoder re-encodes every book

    def test_records_another_exporter_wrote_are_kept(self):
        foreign = {"path_hash": concept_index.fnv1a64(b"/books/Books/Shelf/Old Book.epub"), "page_or_chunk": 3,
                   "timestamp_or_flags": 7, "label": "trainer passage", "vec": [1] * 128}
        novel = {**foreign, "path_hash": concept_index.fnv1a64(b"/books/Novels/x.epub"), "page_or_chunk": 0}
        (self.models / "inbox-concepts.idx").write_bytes(concept_index.pack_index([foreign, novel]))
        result = self.sync()["models"]["index"]
        self.assertEqual((result["kept"], result["books"], result["encoded"]), (2, 1, 1))
        records = concept_index.read_index((self.models / "inbox-concepts.idx").read_bytes())
        self.assertEqual([record["label"] for record in records], ["trainer passage", "trainer passage", "Fresh Article"])
        again = self.sync()["models"]["index"]
        self.assertEqual((again["kept"], again["books"], again["encoded"], again["updated"]), (2, 1, 0, False))

    def test_a_missing_models_folder_writes_nothing(self):
        with self.assertRaisesRegex(ReaderError, "Model source folder not found"):
            sync_card(self.cache, self.config, volume=self.volume, models_dir=self.root / "nope")
        self.assertFalse((self.card / "models").exists())

    def test_session_logs_and_the_liquid_refresh_table_are_backed_up(self):
        logs = self.card / "Benchmark/logs"
        logs.mkdir(parents=True)
        (logs / "session-aaaa-bbbb-00.jsonl").write_bytes(b'{"e":1}\n')
        (logs / "notes.txt").write_bytes(b"skip")
        (self.card / ".crosspoint").mkdir()
        (self.card / ".crosspoint/liquid-refresh.bin").write_bytes(b"\x01\x02")
        usage = self.root / "usage"
        result = sync_card(self.cache, self.config, volume=self.volume, usage_dir=usage)["usage"]
        self.assertEqual((result["copied"], result["errors"]), (2, []))
        self.assertEqual(sorted(path.name for path in usage.iterdir()),
                         ["liquid-refresh.bin", "session-aaaa-bbbb-00.jsonl"])
        self.assertEqual((usage / "liquid-refresh.bin").read_bytes(), b"\x01\x02")
        self.assertEqual(sync_card(self.cache, self.config, volume=self.volume, usage_dir=usage)["usage"]["unchanged"], 2)
        (logs / "session-aaaa-bbbb-00.jsonl").write_bytes(b'{"e":1}\n{"e":2}\n')
        result = sync_card(self.cache, self.config, volume=self.volume, usage_dir=usage)["usage"]
        self.assertEqual((result["copied"], result["unchanged"]), (1, 1))
        self.assertEqual((usage / "session-aaaa-bbbb-00.jsonl").read_bytes(), b'{"e":1}\n{"e":2}\n')

    def test_cli_parses_the_models_and_usage_options(self):
        args = cli.parser().parse_args(["card", "autosync", "--models", "/m", "--usage", "/u"])
        self.assertEqual((args.models, args.usage), (Path("/m"), Path("/u")))
        self.assertIsNone(cli.parser().parse_args(["card", "sync"]).models)

    def test_all_seven_model_files_are_synced_when_changed(self):
        self.assertEqual(card_module.MODEL_FILENAMES, ("platonic-hnet.bin", "engram.bin", "inbox-concepts.idx",
                                                       "cspw.bin", "story-graph.idx", "pe-writer.bin",
                                                       "writer-clauses.bin"))
        cspw = self.models / "cspw.bin"
        cspw.write_bytes(tiny_cspw())
        prebuilt = story_graph.build_story_graph(
            [{"term": "萧炎", "book_path": "/books/Novels/doupo.epub", "kind": 1, "name": "Xiao Yan",
              "events": [{"cutoff": story_graph.epub_cutoff(0, 0), "chapter_or_page": 1, "gloss": "introduced"}]}])
        (self.models / "story-graph.idx").write_bytes(prebuilt)
        result = card_module.sync_models_to_card(self.volume, self.models)
        self.assertEqual((result["errors"], result["missing"]), ([], []))
        self.assertTrue(result["story_graph"]["prebuilt"])
        for name in card_module.MODEL_FILENAMES:
            self.assertEqual(card_module.file_sha256(self.card / "models" / name),
                             card_module.file_sha256(self.models / name), name)
        self.assertEqual((self.card / "models/story-graph.idx").read_bytes(), prebuilt)
        self.assertEqual(len((self.card / "models/cspw.bin").read_bytes()), 393_728)
        unchanged = card_module.sync_models_to_card(self.volume, self.models)
        self.assertEqual((unchanged["copied"], unchanged["unchanged"]), (0, 8))
        cspw.write_bytes(tiny_cspw(fill=3))  # same size, new bytes and mtime
        os.utime(cspw, (cspw.stat().st_atime, cspw.stat().st_mtime + 60))
        bigger = story_graph.build_story_graph(story_graph.read_story_graph(prebuilt)["entities"],
                                               [{"alias": "炎帝", "canonical": "萧炎", "reveal_cutoff": 0x30000}])
        (self.models / "story-graph.idx").write_bytes(bigger)
        changed = card_module.sync_models_to_card(self.volume, self.models)
        self.assertEqual(sorted(Path(path).name for path in changed["copied_paths"]), ["cspw.bin", "story-graph.idx"])
        self.assertEqual((self.card / "models/cspw.bin").read_bytes(), tiny_cspw(fill=3))
        self.assertEqual((self.card / "models/story-graph.idx").read_bytes(), bigger)

    def test_a_missing_story_graph_is_built_from_the_card_books(self):
        (self.card / "books/Books/Shelf/Luria.epub").write_bytes(
            tiny_epub("Major Trends", "Then Isaac Luria taught in Safed. " * 5))
        result = self.sync()["models"]
        graph = result["story_graph"]
        self.assertEqual((graph["prebuilt"], graph["updated"], graph["books"], graph["extracted"]), (False, True, 3, 3))
        self.assertGreaterEqual(graph["entities"], 2)
        data = (self.card / "models/story-graph.idx").read_bytes()
        self.assertEqual(data, (self.models / "story-graph.idx").read_bytes())
        self.assertEqual(story_graph.verify_story_graph(data), [])
        found = story_graph.query_story_graph(data, "Isaac Luria", "/books/Books/Shelf/Luria.epub", 0)
        self.assertEqual((found["status"], found["entity"]["kind"]), ("found", story_graph.KIND_CHARACTER))
        origin = found["events"][0]
        self.assertEqual((origin["gloss"], origin["slot_hint"]), ("Then Isaac Luria taught in Safed.", 1))
        model = concept_index.PlatonicHNet(tiny_model(), concept_index.Engram(tiny_engram()))
        self.assertEqual(origin["vec"], model.encode(origin["gloss"].encode("utf-8")))  # the text up to the cutoff
        self.assertEqual(story_graph.query_story_graph(data, "Isaac Luria", "/books/Books/Shelf/Luria.epub",
                                                       story_graph.epub_cutoff(0, 0))["status"], "not_yet_introduced")
        self.assertEqual(self.sync()["models"]["story_graph"]["extracted"], 0)


if __name__ == "__main__":
    unittest.main()
