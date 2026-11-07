"""The companion features working together on one card: shelf books with front cards, Cross-Fire and Inbox upkeep."""

import json
import tempfile
import unittest
import zipfile
from datetime import datetime, timezone
from pathlib import Path

from reader_send import book_hooks, card as card_module, card_autosync, inbox_card
from reader_send.cache import Cache
from reader_send.card import BOOK_INBOX, BOOKS_DIR, CardVolume, sync_card

import test_book_hooks
import test_crossfire

UUID = test_crossfire.UUID


class CompanionIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.card = self.root / "card"
        self.card.mkdir()
        self.volume = CardVolume(self.card, "disk9s1", UUID)
        self.cache = Cache(self.root / "cache")
        self.config = {"version": 1, "volume_uuid": UUID, "annotations_dir": str(self.root / "annotations"),
                       "firmware_path": str(self.root / "fw.bin"), "firmware_sha256": "0" * 64,
                       "launchers_dir": str(self.root / "launchers")}
        self.corpus, self.neighbors = test_crossfire.make_corpus(self.root)
        self.crossfire = {"corpus": self.corpus, "neighbors": self.neighbors, "db": self.root / "index.sqlite3"}
        store = self.card / ".crosspoint" / "annotations"
        store.mkdir(parents=True)
        (store / "1.json").write_text(json.dumps(test_crossfire.ANNOTATION), encoding="utf-8")
        self.shelf = self.root / "shelf"
        self.shelf.mkdir()
        self.book = test_book_hooks.make_epub(self.shelf / "Grey Eminence.epub", test_book_hooks.sample_chapters(5))
        self.hooked = book_hooks.apply_hooks(self.book)
        self.backup = self.shelf / "Grey Eminence.pre-hooks.epub"

    def test_the_fixture_book_is_enriched_and_keeps_its_backup_beside_it(self):
        self.assertTrue(self.hooked.changed)
        self.assertEqual(self.hooked.backup.resolve(), self.backup.resolve())
        self.assertTrue(self.backup.is_file())
        with zipfile.ZipFile(self.book) as archive:
            self.assertIn(self.hooked.card, archive.namelist())
        with zipfile.ZipFile(self.backup) as archive:
            self.assertNotIn(self.hooked.card, archive.namelist())

    def test_card_sync_delivers_the_enriched_book_not_its_backup_and_builds_the_crossfire_card(self):
        result = sync_card(self.cache, self.config, volume=self.volume, books_dir=self.shelf,
                           crossfire=self.crossfire)
        shelf = result["books"]["shelf"]
        self.assertEqual((shelf["planned"], shelf["copied"], shelf["errors"]), (1, 1, []))
        delivered = sorted(path.name for path in (self.card / BOOKS_DIR).rglob("*.epub"))
        self.assertEqual(delivered, ["Grey Eminence.epub"])
        self.assertEqual((self.card / BOOKS_DIR / "Grey Eminence.epub").read_bytes(), self.book.read_bytes())
        self.assertEqual(result["annotations"]["crossfire"]["status"], "ok")
        self.assertEqual(len(list((self.card / BOOK_INBOX).glob("crossfire-*.epub"))), 1)

    def test_autosync_runs_shelf_crossfire_and_inbox_cleanup_in_one_mount(self):
        result = card_autosync.autosync(
            self.cache, self.config, resolve=lambda uuid: self.volume,
            key_provider=lambda: ("", "No Keychain item"), notify=None,
            log=card_autosync.RunLog(self.root / "autosync.log"),
            books_dir=self.shelf, crossfire=self.crossfire)
        self.assertEqual(result["shelves"]["books"]["copied"], 1)
        self.assertIsNone(result["shelves"]["novels"])
        self.assertEqual(result["annotations"]["crossfire"]["status"], "ok")
        self.assertTrue(result["cleanup"]["dry_run"])
        self.assertEqual(result["cleanup"]["counts"]["trash"], 0)
        self.assertEqual(sorted(path.name for path in (self.card / BOOKS_DIR).glob("*.epub")), ["Grey Eminence.epub"])

    def test_inbox_cleanup_never_trashes_a_crossfire_card(self):
        sync_card(self.cache, self.config, volume=self.volume, crossfire=self.crossfire)
        later = datetime(2036, 1, 1, tzinfo=timezone.utc)
        plan = inbox_card.plan_cleanup(self.card, {"seen": {}, "finished": {}, "trashed": {}}, later)
        entries = [entry for entry in plan["files"] if "crossfire-" in entry["file"]]
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["action"], "keep")


if __name__ == "__main__":
    unittest.main()
