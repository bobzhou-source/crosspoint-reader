import json
import plistlib
import subprocess
import tempfile
import unittest
import os
from pathlib import Path
from unittest.mock import patch

from reader_send import cache as cache_module
from reader_send import card as card_module
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

UUID = "6A4B4C73-B747-33DA-BF3D-455A3C03D0CB"


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


if __name__ == "__main__":
    unittest.main()
