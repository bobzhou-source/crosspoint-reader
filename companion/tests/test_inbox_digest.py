import io
import json
import os
import tempfile
import unittest
import zipfile
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from xml.etree import ElementTree

from reader_send import feed_digest as fd
from reader_send import inbox_card
from reader_send.cloud_annotations import map_annotation
from reader_send.cloud_sync import delivery_name
from reader_send.epub import CONTAINER_XML, pack_epub

PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 64


def single(handle, status, iso, text, *, name=None, image=False, quoted=False, posts=1):
    """A single X post EPUB in the companion generator's layout."""
    name = name or handle.title()
    url = f"https://x.com/{handle}/status/{status}"
    divs = []
    for number in range(1, posts + 1):
        extra = '<figure><img src="images/0.jpg" alt=""/></figure>' if image and number == 1 else ""
        if quoted and number == 1:
            extra += '<blockquote><p>Q · <a href="https://x.com/q/status/1">Quoted post</a></p><p>quoted</p></blockquote>'
        divs.append(f'<div><h2>Post {number}</h2><p>{iso} · <a href="{url}">Original post</a></p>'
                    f'<p>{text} part {number} https://t.co/AbC123</p>{extra}</div>')
    doc = ('<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title>'
           f'</head><body><h1>{text}</h1><p>{name} (@{handle})</p><p>{iso}</p><p>Source: <a href="{url}">{url}</a></p>'
           + "".join(divs) + "</body></html>")
    opf = ('<?xml version="1.0" encoding="UTF-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
           'unique-identifier="book-id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           f'<dc:identifier id="book-id">urn:x:{status}</dc:identifier><dc:title>{text} https://t.co/AbC123</dc:title>'
           f'<dc:creator>{name} (@{handle})</dc:creator><dc:source>{url}</dc:source><dc:date>{iso[:10]}</dc:date>'
           '</metadata><manifest><item id="article" href="article.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>'
           '<spine toc="ncx"><itemref idref="article"/></spine></package>')
    entries = {"mimetype": "application/epub+zip", "META-INF/container.xml": CONTAINER_XML,
               "OEBPS/content.opf": opf, "OEBPS/article.xhtml": doc, "OEBPS/toc.ncx": "<ncx/>"}
    if image:
        entries["OEBPS/images/0.jpg"] = PNG
    return pack_epub(entries)


def article(title="Long read"):
    opf = ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
           f'<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{title}</dc:title>'
           '<dc:creator>Writer</dc:creator><dc:source>https://writer.substack.com/p/x</dc:source></metadata>'
           '<manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/></manifest>'
           '<spine><itemref idref="a"/></spine></package>')
    return pack_epub({"mimetype": "application/epub+zip", "META-INF/container.xml": CONTAINER_XML,
                      "OEBPS/content.opf": opf, "OEBPS/a.xhtml": "<html/>"})


def post(handle, status, iso, text="hello", **kwargs):
    return fd.read_single(single(handle, status, iso, text, **kwargs), item_id=f"00000000-0000-5000-8000-{status:0>12}")


def tree(root: Path) -> dict:
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in sorted(root.rglob("*")) if path.is_file()}


class DigestTests(unittest.TestCase):
    def test_reads_single_and_strips_tco(self):
        value = post("alice", "11", "2026-11-02T14:05:00Z", image=True, quoted=True, name="Alice A")
        self.assertEqual((value.key, value.author, value.handle, value.url),
                         ("x:11", "Alice A", "alice", "https://x.com/alice/status/11"))
        self.assertEqual(value.title, "hello")
        self.assertEqual([src for src, _ in value.images], ["images/0.jpg"])

    def test_rejects_non_x_book(self):
        with self.assertRaises(Exception):
            fd.read_single(article())

    def test_groups_by_local_day_across_utc_midnight(self):
        # 03:30Z on 2 Nov is 22:30 on 1 Nov in New York (EST); 05:30Z is 00:30 on 2 Nov.
        posts = [post("a", "1", "2026-11-02T03:30:00Z"), post("b", "2", "2026-11-02T05:30:00Z")]
        volumes = fd.plan_volumes(posts)
        self.assertEqual([(v.day, [p.key for p in v.posts]) for v in volumes],
                         [(date(2026, 11, 1), ["x:1"]), (date(2026, 11, 2), ["x:2"])])
        utc = fd.plan_volumes(posts, "UTC")
        self.assertEqual([v.day for v in utc], [date(2026, 11, 2)])

    def test_volume_split_and_late_volumes_never_reuse_numbers(self):
        posts = [post("a", str(n), f"2026-11-02T1{n}:00:00Z") for n in range(5)]
        volumes = fd.plan_volumes(posts, max_posts=2)
        self.assertEqual([v.number for v in volumes], [1, 2, 3])
        self.assertEqual([v.title for v in volumes], ["X — Mon 2 Nov 2026", "X — 2 Nov (2)", "X — 2 Nov (3)"])
        self.assertEqual(volumes[0].filename, "X — Mon 2 Nov 2026.epub")
        late = fd.plan_volumes([post("a", "9", "2026-11-02T20:00:00Z")], existing={date(2026, 11, 2): 3})
        self.assertEqual([(v.number, v.title) for v in late], [(4, "X — 2 Nov (4)")])
        self.assertEqual(sum(len(v.posts) for v in volumes), 5)

    def test_duplicate_posts_appear_once(self):
        first = post("a", "7", "2026-11-02T12:00:00Z")
        volumes = fd.plan_volumes([first, first, post("a", "8", "2026-11-02T13:00:00Z")])
        self.assertEqual([p.key for v in volumes for p in v.posts], ["x:7", "x:8"])

    def test_manifest_contract_and_layout(self):
        posts = [post("zed", "3", "2026-11-02T15:00:00Z", name="Zed", image=True),
                 post("amy", "1", "2026-11-02T18:00:00Z", name="amy", posts=3),
                 post("amy", "2", "2026-11-02T13:00:00Z", name="amy")]
        volume = fd.plan_volumes(posts)[0]
        data = fd.build_digest(volume, digest_id="6ffac9ec-65ba-56cf-ba44-260ee69f2f6c")
        self.assertEqual(data, fd.build_digest(volume, digest_id="6ffac9ec-65ba-56cf-ba44-260ee69f2f6c"))
        feed = fd.read_manifest(data)
        self.assertEqual({k: feed[k] for k in ("format", "kind", "id", "source", "day", "volume", "timezone", "title")},
                         {"format": "reader-feed-v1", "kind": "digest", "id": "6ffac9ec-65ba-56cf-ba44-260ee69f2f6c",
                          "source": "x", "day": "2026-11-02", "volume": 1, "timezone": "America/New_York",
                          "title": "X — Mon 2 Nov 2026"})
        # Grouped by author (case-insensitive), chronological within an author.
        self.assertEqual([i["key"] for i in feed["items"]], ["x:2", "x:1", "x:3"])
        self.assertEqual(set(feed["items"][0]), {"id", "key", "source", "author", "handle", "published", "url",
                                                 "href", "title"})
        self.assertEqual(feed["items"][0]["id"], "00000000-0000-5000-8000-000000000002")
        self.assertEqual(feed["items"][0]["published"], "2026-11-02T13:00:00Z")
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertEqual(archive.namelist()[0], "mimetype")
            self.assertEqual(archive.getinfo("mimetype").compress_type, zipfile.ZIP_STORED)
            opf = ElementTree.fromstring(archive.read("OEBPS/content.opf"))
            ns = {"opf": "http://www.idpf.org/2007/opf"}
            hrefs = {i.get("id"): i.get("href") for i in opf.findall(".//opf:item", ns)}
            spine = [hrefs[r.get("idref")] for r in opf.findall(".//opf:itemref", ns)]
            self.assertEqual(spine, ["contents.xhtml"] + [i["href"] for i in feed["items"]])
            for item in feed["items"]:
                document = archive.read("OEBPS/" + item["href"]).decode()
                ElementTree.fromstring(document.encode())
                self.assertIn(f"@{item['handle']}", document)
                self.assertNotIn("t.co", document)
            thread = archive.read("OEBPS/posts/0002.xhtml").decode()
            self.assertIn('<p class="n">3/3</p>', thread)
            zed = archive.read("OEBPS/posts/0003.xhtml").decode()
            self.assertIn('src="../images/0003-0.jpg"', zed)
            self.assertEqual(archive.read("OEBPS/images/0003-0.jpg"), PNG)
            contents = archive.read("OEBPS/contents.xhtml").decode()
            self.assertLess(contents.index("amy"), contents.index("Zed"))
            self.assertIn("3 posts from 2 accounts", contents)

    def test_ordinary_book_has_no_manifest(self):
        self.assertIsNone(fd.read_manifest(article()))

    def test_delivery_name_follows_firmware_rules(self):
        self.assertEqual(delivery_name({"source_path": "X — Mon 2 Nov 2026.epub"}), "X — Mon 2 Nov 2026.epub")
        self.assertEqual(delivery_name({"source_path": "a/b/Writer — Title.epub"}), "Writer — Title.epub")
        for bad in ("a:b.epub", ".hidden.epub", "x.txt", None, ".epub", "a" * 240 + ".epub"):
            self.assertIsNone(delivery_name({"source_path": bad}))

    def test_cleanup_is_opt_in_in_card_config(self):
        from reader_send import card
        from reader_send.common import ReaderError
        base = {"version": 1, "volume_uuid": "12345678-1234-1234-1234-123456789ABC", "annotations_dir": "/a",
                "firmware_path": "/f.bin", "firmware_sha256": "a" * 64, "launchers_dir": "/l"}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "card.json"
            path.write_text(json.dumps(base))
            self.assertNotIn("inbox_cleanup", card.load_config(path))
            path.write_text(json.dumps({**base, "inbox_cleanup": True}))
            self.assertIs(card.load_config(path)["inbox_cleanup"], True)
            path.write_text(json.dumps({**base, "inbox_cleanup": "yes"}))
            with self.assertRaises(ReaderError):
                card.load_config(path)


class CardFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.inbox = self.root / "books/Inbox"
        self.inbox.mkdir(parents=True)
        (self.root / ".crosspoint").mkdir()
        self.now = datetime(2026, 12, 1, 12, tzinfo=timezone.utc)

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, name, data, *, age_days=None):
        path = self.inbox / name
        path.write_bytes(data)
        if age_days is not None:
            stamp = (self.now - timedelta(days=age_days)).timestamp()
            os.utime(path, (stamp, stamp))
        return "/books/Inbox/" + name

    def recents(self, *paths):
        (self.root / ".crosspoint/recent.json").write_text(
            json.dumps({"books": [{"path": p, "title": "", "author": "", "coverBmpPath": ""} for p in paths]}))

    def recent_paths(self):
        return [b["path"] for b in json.loads((self.root / ".crosspoint/recent.json").read_text())["books"]]


class RepackTests(CardFixture):
    def test_dry_run_plans_without_touching_the_card(self):
        a = self.put("a.epub", single("alice", "1", "2026-11-02T14:00:00Z", "one"))
        b = self.put("b.epub", single("bob", "2", "2026-11-02T15:00:00Z", "two"))
        dup = self.put("b-copy.epub", single("bob", "2", "2026-11-02T15:00:00Z", "two"))
        c = self.put("c.epub", single("carol", "3", "2026-11-03T15:00:00Z", "three"))
        self.put("Writer — Long read.epub", article())
        noted = self.put("noted.epub", single("dan", "4", "2026-11-02T16:00:00Z", "four"))
        folder = self.root / ".crosspoint/annotations" / inbox_card.annotation_key(noted)
        folder.mkdir(parents=True)
        (folder / "001.json").write_text(json.dumps({"sourcePath": noted, "id": 1, "createdAt": 1, "kind": 0}))
        before = tree(self.root)
        plan = inbox_card.plan_repack(self.root)
        summary = inbox_card.describe_repack(plan)
        self.assertEqual(tree(self.root), before)
        self.assertEqual(summary["singles"], 4)
        self.assertEqual([(d["file"], d["posts"], sorted(d["singles"])) for d in summary["digests"]],
                         [("/books/Inbox/X — Mon 2 Nov 2026.epub", 2, sorted([a, b, dup])),
                          ("/books/Inbox/X — Tue 3 Nov 2026.epub", 1, [c])])
        self.assertEqual(summary["kept"], [{"file": noted, "reason": "has annotations; left as a single"}])

    def test_apply_moves_singles_never_deletes_and_is_idempotent(self):
        originals = {name: single(h, s, "2026-11-02T14:00:00Z", h) for name, h, s in
                     (("a.epub", "alice", "1"), ("b.epub", "bob", "2"))}
        for name, data in originals.items():
            self.put(name, data)
        self.recents("/books/Inbox/a.epub", "/books/Other.epub")
        result = inbox_card.apply_repack(self.root, inbox_card.plan_repack(self.root))
        self.assertEqual(result, {"digests_written": 1, "singles_archived": 2, "recents_removed": 1})
        archive = self.inbox / ".singles-archive"
        self.assertEqual({p.name: p.read_bytes() for p in archive.iterdir()}, originals)
        digest_file = self.inbox / "X — Mon 2 Nov 2026.epub"
        self.assertEqual([i["key"] for i in fd.read_manifest(digest_file.read_bytes())["items"]], ["x:1", "x:2"])
        self.assertEqual(self.recent_paths(), ["/books/Other.epub"])
        self.assertTrue((self.root / ".crosspoint/library.dirty").exists())
        again = inbox_card.plan_repack(self.root)
        self.assertEqual((again["singles"], again["digests"]), (0, []))
        # A late single for the same day gets volume 2 and a post already in a digest is only archived.
        self.put("late.epub", single("carol", "3", "2026-11-02T20:00:00Z", "late"))
        self.put("again.epub", originals["a.epub"])
        late = inbox_card.plan_repack(self.root)
        self.assertEqual([d["file"] for d in late["digests"]], ["/books/Inbox/X — 2 Nov (2).epub"])
        self.assertEqual(late["already_in_digest"], ["/books/Inbox/again.epub"])


class CleanupTests(CardFixture):
    def progress(self, book, spine, page=0, total=0):
        folder = inbox_card.cache_dir(self.root, book)
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "progress.bin").write_bytes(spine.to_bytes(2, "little") + page.to_bytes(2, "little")
                                              + total.to_bytes(2, "little"))
        (folder / "section.bin").write_bytes(b"x")

    def ledger(self, book, item_id):
        folder = self.root / ".crosspoint/reader-sync/items"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f"{item_id}.json").write_text(json.dumps({"id": item_id, "path": book}))

    def digest_bytes(self, day="2026-11-02"):
        return fd.build_digest(fd.plan_volumes([post("a", "1", f"{day}T14:00:00Z")])[0])

    def run_at(self, state, days):
        return inbox_card.plan_cleanup(self.root, state, self.now + timedelta(days=days))

    def actions(self, plan):
        return {entry["file"]: entry["action"] for entry in plan["files"]}

    def test_cache_hash_and_annotation_key_match_firmware(self):
        self.assertEqual(inbox_card.std_hash("/.crosspoint/nav-home-accept/cover.epub"), 2794015026)
        self.assertEqual(inbox_card.annotation_key(""), "cbf29ce484222325")

    def test_finished_versus_unread_policies(self):
        state = inbox_card.load_state(self.root / "none.json")
        x_done = self.put("X — Mon 2 Nov 2026.epub", self.digest_bytes(), age_days=5)
        self.progress(x_done, 2)  # spine sentinel: contents + one post = 2 items
        x_old = self.put("X — Tue 3 Nov 2026.epub", self.digest_bytes("2026-11-03"), age_days=31)
        x_new = self.put("X — Wed 4 Nov 2026.epub", self.digest_bytes("2026-11-04"), age_days=10)
        long_unread = self.put("Writer — Unread.epub", article(), age_days=200)
        long_done = self.put("Writer — Done.epub", article())
        self.progress(long_done, 0, page=9, total=10)  # last page of the only spine item
        mine = self.put("My own book.epub", article())
        self.progress(mine, 1)
        for book, ident in ((long_unread, "u1"), (long_done, "u2")):
            self.ledger(book, ident)
        first = self.actions(self.run_at(state, 0))
        self.assertEqual(first[x_old], "trash")
        self.assertEqual(first[x_done], "keep")  # finished just now (first observed)
        self.assertEqual(first[x_new], "keep")
        self.assertEqual(first[long_unread], "keep")
        later = self.actions(self.run_at(state, 4))
        self.assertEqual(later[x_done], "trash")
        self.assertEqual(later[long_done], "keep")
        self.assertEqual(self.actions(self.run_at(state, 14))[long_done], "trash")
        final = self.actions(self.run_at(state, 400))
        self.assertEqual(final[long_unread], "keep")  # never auto-deleted while unread
        self.assertEqual(final[mine], "keep")  # not a synced feed item

    def test_annotation_hold_until_receipt(self):
        (self.root / ".crosspoint/reader-sync.json").write_text(json.dumps({"device_id": "x4", "device_key": "k" * 64}))
        state = inbox_card.load_state(self.root / "none.json")
        book = self.put("Writer — Noted.epub", article())
        self.ledger(book, "n1")
        self.progress(book, 1)
        record = {"sourcePath": book, "id": 1, "createdAt": 1700000000, "kind": 0, "excerpt": "hi"}
        folder = self.root / ".crosspoint/annotations" / inbox_card.annotation_key(book)
        folder.mkdir(parents=True)
        (folder / "001.json").write_text(json.dumps(record))
        self.run_at(state, 0)
        held = self.run_at(state, 15)
        self.assertEqual(self.actions(held)[book], "hold")
        self.assertEqual(held["counts"]["hold"], 1)
        receipts = self.root / ".crosspoint/reader-sync/notes"
        receipts.mkdir(parents=True)
        (receipts / f"{map_annotation(record, device_id='x4')['id']}.receipt").write_text("a" * 64)
        released = self.run_at(state, 15)
        self.assertEqual(self.actions(released)[book], "trash")
        inbox_card.apply_cleanup(self.root, released, state, self.now + timedelta(days=15))
        self.assertTrue((folder / "001.json").exists())  # native annotations stay on the card

    def test_apply_moves_to_trash_clears_cache_and_recents_then_purges(self):
        state_file = self.root / "state.json"
        book = self.put("X — Tue 3 Nov 2026.epub", self.digest_bytes("2026-11-03"), age_days=31)
        self.progress(book, 1)
        cache = inbox_card.cache_dir(self.root, book)
        self.recents(book, "/books/Keep.epub")
        dry = inbox_card.run_cleanup(self.root, state_file, now=self.now)
        self.assertTrue(dry["dry_run"])
        self.assertTrue((self.root / book.lstrip("/")).exists())
        applied = inbox_card.run_cleanup(self.root, state_file, apply=True, now=self.now)
        self.assertEqual(applied["applied"], {"trashed": 1, "caches_cleared": 1, "recents_removed": 1, "purged": 0})
        trashed = self.inbox / ".trash" / "X — Tue 3 Nov 2026.epub"
        self.assertTrue(trashed.exists())
        self.assertFalse(cache.exists())
        self.assertEqual(self.recent_paths(), ["/books/Keep.epub"])
        repeat = inbox_card.run_cleanup(self.root, state_file, apply=True, now=self.now + timedelta(days=6))
        self.assertEqual(repeat["applied"], {"trashed": 0, "caches_cleared": 0, "recents_removed": 0, "purged": 0})
        self.assertTrue(trashed.exists())
        purge = inbox_card.run_cleanup(self.root, state_file, apply=True, now=self.now + timedelta(days=7))
        self.assertEqual(purge["applied"]["purged"], 1)
        self.assertFalse(trashed.exists())
        self.assertEqual(inbox_card.run_cleanup(self.root, state_file, apply=True,
                                                now=self.now + timedelta(days=8))["applied"]["purged"], 0)


if __name__ == "__main__":
    unittest.main()
