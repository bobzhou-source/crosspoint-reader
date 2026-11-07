import contextlib
import io
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
from xml.etree import ElementTree

from reader_send import card as card_module
from reader_send import card_autosync
from reader_send import cli
from reader_send import crossfire
from reader_send.cache import Cache
from reader_send.card import BOOK_INBOX, CardVolume, sync_card
from reader_send.common import ReaderError

UUID = "6A4B4C73-B747-33DA-BF3D-455A3C03D0CB"

DESIRE = (
    "The modern man believes that desire is a problem to be solved by technique. This is false. "
    "Desire is not a technique but a condition of the soul, and no method can manufacture it."
)
VITALITY = (
    "The soul is not a technique. Every attempt to manufacture desire by method is a symptom of "
    "decay. A man who believes love is a procedure has already lost the war; the truth is that "
    "vitality cannot be taught."
)
CONTRA = (
    "Technique is the refuge of the man without desire. On the contrary, desire is a condition of "
    "the soul, and no procedure can produce it. The modern illusion is that method can replace "
    "love, and that illusion is the root of our misery."
)
NOISE = (
    "The quarterly report lists the new factory locations, the shipping schedule and the revised "
    "budget for the coming fiscal year. Managers met on Tuesday to review the numbers."
)


def write_markdown(root: Path, publication: str, name: str, title: str, body: str,
                   url: str = "https://example.com/p") -> Path:
    directory = root / publication
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_text(f"# {title}\n\n{body}\n\noriginal content: {url}\n", encoding="utf-8")
    return path


def write_jsonl(root: Path, name: str, records: list[dict]) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    path = root / name
    path.write_text("\n".join(json.dumps(record) for record in records), encoding="utf-8")
    return path


def make_corpus(root: Path) -> tuple[Path, Path]:
    corpus = root / "corpus"
    neighbors = root / "neighbors"
    write_markdown(corpus, "delicioustacos", "20210610_140018_desire.md", "On Desire", DESIRE,
                   "https://delicioustacos.substack.com/p/desire")
    write_markdown(corpus, "conundrumcluster", "20230822_164003_noise.md", "Quarterly Report", NOISE)
    write_jsonl(neighbors, "mix.jsonl", [
        {"host": "bronzeagepervert", "title": "On Vitality", "post_date": "2020-01-02T00:00:00Z",
         "authors": "['Bronze Age Pervert']", "url": "https://example.com/vitality", "text": VITALITY},
        {"host": "niccolo", "title": "Against Technique", "post_date": "2021-05-05T00:00:00Z",
         "authors": "['Niccolo']", "url": "https://example.com/contra", "text": CONTRA},
    ])
    return corpus, neighbors


ANNOTATION = {
    "id": 1,
    "sourcePath": "books/Books/grey-eminence.epub",
    "excerpt": "desire is a condition of the soul, not a technique",
    "note": "Can method manufacture love?",
    "kind": "Question",
    "createdAt": 1762000000,
}


class ParsingTest(unittest.TestCase):
    def test_markdown_parsing_strips_boilerplate_and_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = write_markdown(
                root, "niccolo", "20200426_235506_guerrilla.md", "Guerrilla Media Mindset",
                "![image](https://example.com/x.png)\n\nFirst paragraph with **bold** text.\n\n"
                "This post is for paid subscribers",
                "https://example.com/g")
            post = crossfire.parse_markdown(path, "niccolo")
            self.assertEqual(post.title, "Guerrilla Media Mindset")
            self.assertEqual(post.date, "2020-04-26")
            self.assertEqual(post.url, "https://example.com/g")
            self.assertEqual(post.author, "niccolo")
            self.assertNotIn("example.com/x.png", post.text)
            self.assertNotIn("paid subscribers", post.text)
            self.assertIn("First paragraph with bold text.", post.text)

    def test_non_matching_filenames_are_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "notes.md"
            path.write_text("# Title\n\n" + DESIRE, encoding="utf-8")
            self.assertIsNone(crossfire.parse_markdown(path, "pub"))

    def test_jsonl_parsing_handles_author_lists_and_host_fallback(self):
        post = crossfire.parse_jsonl_post(
            {"host": "agharta", "title": "T", "post_date": "2023-04-05T10:00:00.000Z",
             "authors": "['Someone Else']", "text": VITALITY, "url": "https://e/x"}, "agharta")
        self.assertEqual((post.author, post.title, post.date), ("Someone Else", "T", "2023-04-05"))
        self.assertEqual(post.kind, "substack")
        fallback = crossfire.parse_jsonl_post({"title": "T", "text": VITALITY}, "agharta")
        self.assertEqual(fallback.author, "agharta")
        self.assertIsNone(crossfire.parse_jsonl_post({"title": "T", "text": "short"}, "agharta"))

    def test_chunking_respects_bounds_and_drops_short_fragments(self):
        post = crossfire.Post("a", "t", "2020-01-01", "", "substack",
                              "tiny\n\n" + ("Sentence about power and desire. " * 90))
        chunks = crossfire.chunk_post(post)
        self.assertTrue(chunks)
        for chunk in chunks:
            self.assertGreaterEqual(len(chunk.text), crossfire.MIN_CHUNK)
            self.assertLessEqual(len(chunk.text), crossfire.MAX_CHUNK)

    def test_content_tokens_filters_stopwords_and_folds_plurals(self):
        tokens = crossfire.content_tokens("The Powers and stories of cats, absolutely 12")
        self.assertIn("power", tokens)
        self.assertIn("story", tokens)
        self.assertIn("cats", tokens)
        self.assertNotIn("the", tokens)
        self.assertNotIn("12", tokens)


class RankingTest(unittest.TestCase):
    def chunks(self):
        return [
            crossfire.Chunk(crossfire.Post("a", "Desire", "2020-01-01", "", "substack", DESIRE), DESIRE),
            crossfire.Chunk(crossfire.Post("b", "Vitality", "2020-01-02", "", "substack", VITALITY), VITALITY),
            crossfire.Chunk(crossfire.Post("c", "Report", "2020-01-03", "", "substack", NOISE), NOISE),
        ]

    def test_bm25_ranks_relevant_chunks_above_noise(self):
        index = crossfire.BM25Index(self.chunks())
        ranked = index.search(set(crossfire.content_tokens("desire technique soul")), limit=10)
        self.assertTrue(ranked)
        self.assertNotEqual(ranked[0][2].post.title, "Report")
        self.assertTrue(any(third.post.title == "Report" for _, _, third in ranked) is False)

    def test_best_excerpt_is_bounded_and_contains_a_query_term(self):
        excerpt = crossfire.best_excerpt(VITALITY, set(crossfire.content_tokens("desire technique")))
        self.assertLessEqual(len(excerpt), crossfire.MAX_EXCERPT)
        self.assertIn("technique", excerpt.lower())
        self.assertTrue(excerpt.endswith((".", "!", "?", ".”", '."')))

    def test_select_replies_prefers_distinct_authors_and_keeps_metadata(self):
        annotation = crossfire.normalize_annotation(ANNOTATION)
        same = [crossfire.Chunk(crossfire.Post("a", "One", "2020-01-01", "u1", "substack", DESIRE), DESIRE),
                crossfire.Chunk(crossfire.Post("a", "Two", "2020-01-02", "u2", "substack", CONTRA), CONTRA),
                crossfire.Chunk(crossfire.Post("b", "Three", "2020-01-03", "u3", "substack",
                                               VITALITY), VITALITY)]
        candidates = [(10.0, chunk) for chunk in same]
        replies = crossfire.select_replies(annotation, candidates, replies=2)
        self.assertEqual(len(replies), 2)
        self.assertEqual(len({reply.chunk.post.author for reply in replies}), 2)
        for reply in replies:
            self.assertTrue(reply.chunk.post.title)
            self.assertTrue(reply.chunk.post.date)
            self.assertTrue(reply.excerpt)

    def test_select_replies_skips_chunks_without_enough_overlap(self):
        annotation = crossfire.normalize_annotation(ANNOTATION)
        noise = crossfire.Chunk(crossfire.Post("a", "Report", "2020-01-03", "", "substack", NOISE), NOISE)
        self.assertEqual(crossfire.select_replies(annotation, [(50.0, noise)]), [])


class StoreTest(unittest.TestCase):
    def test_refresh_is_incremental_and_removes_deleted_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            store = crossfire.CorpusStore(root / "index.sqlite3")
            try:
                first = store.refresh(corpus, neighbors)
                self.assertEqual(first["changed"], 3)
                self.assertGreater(first["posts"], 0)
                before = store.count()
                self.assertGreater(before, 0)
                self.assertEqual(store.refresh(corpus, neighbors)["changed"], 0)
                self.assertEqual(store.count(), before)

                post = corpus / "delicioustacos" / "20210610_140018_desire.md"
                post.write_text(post.read_text(encoding="utf-8") + "\n\n" + CONTRA, encoding="utf-8")
                self.assertEqual(store.refresh(corpus, neighbors)["changed"], 1)

                (corpus / "conundrumcluster" / "20230822_164003_noise.md").unlink()
                removed = store.refresh(corpus, neighbors)
                self.assertEqual(removed["removed"], 1)
                self.assertEqual(removed["changed"], 0)
                self.assertLess(store.count(), before)

                hits = store.search(set(crossfire.content_tokens("desire technique soul")), limit=10)
                self.assertTrue(hits)
                self.assertTrue(all(isinstance(score, float) for score, _ in hits))
            finally:
                store.close()

    def test_search_without_terms_is_empty(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            store = crossfire.CorpusStore(root / "index.sqlite3")
            try:
                store.refresh(corpus, neighbors)
                self.assertEqual(store.search(set()), [])
            finally:
                store.close()


class AnnotationTest(unittest.TestCase):
    def test_native_and_mapped_records_normalize_to_the_same_shape(self):
        native = crossfire.normalize_annotation(ANNOTATION)
        mapped = crossfire.normalize_annotation({
            "book_path": "books/Books/grey-eminence.epub",
            "selection": ANNOTATION["excerpt"], "note": ANNOTATION["note"],
            "kind": "question", "created_at": 1762000000000})
        self.assertEqual(native.selection, mapped.selection)
        self.assertEqual(native.book, mapped.book)
        self.assertEqual(native.kind, "question")
        self.assertEqual(native.created, mapped.created)
        self.assertEqual(native.key, mapped.key)

    def test_kind_mapping_and_optional_note(self):
        self.assertEqual(crossfire.normalize_annotation(
            {"sourcePath": "b", "excerpt": DESIRE, "kind": "ReviewLater"}).kind, "review_later")
        self.assertEqual(crossfire.normalize_annotation(
            {"sourcePath": "b", "excerpt": DESIRE, "kind": 1}).kind, "question")
        note_only = crossfire.normalize_annotation(
            {"sourcePath": "b", "note": "What about the will?", "kind": "Quote"})
        self.assertEqual(note_only.selection, "")

    def test_empty_and_duplicate_records_are_dropped(self):
        self.assertIsNone(crossfire.normalize_annotation({"sourcePath": "b"}))
        values = [ANNOTATION, dict(ANNOTATION)]
        self.assertEqual(len(crossfire.normalize_annotations(values)), 1)

    def test_load_annotation_file_reads_json_ndjson_and_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            single = root / "a.json"
            single.write_text(json.dumps(ANNOTATION), encoding="utf-8")
            self.assertEqual(len(crossfire.load_annotation_file(single)), 1)
            lines = root / "b.ndjson"
            lines.write_text(json.dumps(ANNOTATION) + "\n" + json.dumps(
                {**ANNOTATION, "excerpt": "another passage about power"}), encoding="utf-8")
            self.assertEqual(len(crossfire.load_annotation_file(lines)), 2)
            directory = root / "store"
            directory.mkdir()
            (directory / "one.json").write_text(json.dumps(ANNOTATION), encoding="utf-8")
            (directory / "._shadow.json").write_bytes(b"\x00\x05\x16\x07garbage")
            self.assertEqual(len(crossfire.load_annotation_file(directory)), 1)


class CardTest(unittest.TestCase):
    def test_build_card_is_a_valid_epub_and_escapes_markup(self):
        reply = crossfire.Reply(1.5, 3, 'A <passage> & "quote"',
                                crossfire.Chunk(crossfire.Post(
                                    "Author & Co", 'Title <em>', "2020-01-01", "https://e/x?a=1&b=2",
                                    "substack", DESIRE), DESIRE))
        annotation = crossfire.normalize_annotation({
            "sourcePath": "books/x.epub", "excerpt": 'Power & "the state"',
            "note": "Is <this> right?", "kind": "Quote", "createdAt": 1762000000})
        data = crossfire.build_card([(annotation, [reply])])
        path = Path(tempfile.mkdtemp()) / "card.epub"
        path.write_bytes(data)
        archive = zipfile.ZipFile(path)
        self.assertEqual(archive.namelist()[0], "mimetype")
        self.assertEqual(archive.getinfo("mimetype").compress_type, zipfile.ZIP_STORED)
        self.assertIsNone(archive.testzip())
        xhtml = archive.read("OEBPS/card.xhtml").decode()
        ElementTree.fromstring(xhtml.encode())
        self.assertIn("Cross-Fire: Marginalia &amp; Replies", xhtml)
        self.assertIn("&lt;passage&gt;", xhtml)
        self.assertIn("Author &amp; Co", xhtml)
        self.assertIn("Your note:", xhtml)
        self.assertIn("https://e/x?a=1&amp;b=2", xhtml)
        self.assertIn("books/x.epub", xhtml)
        archive.close()

    def test_write_card_is_deterministic_and_idempotent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            inbox = root / "Inbox"
            first = crossfire.write_card(inbox, [ANNOTATION], corpus=corpus, neighbors=neighbors,
                                         db=root / "index.sqlite3")
            self.assertTrue(first["written"])
            self.assertEqual(first["index"], "fts5")
            self.assertGreaterEqual(first["matches"], 2)
            path = Path(first["path"])
            self.assertTrue(path.is_file())
            self.assertEqual(path.name, first["filename"])
            second = crossfire.write_card(inbox, [ANNOTATION], corpus=corpus, neighbors=neighbors,
                                          db=root / "index.sqlite3")
            self.assertFalse(second["written"])
            self.assertEqual(second["filename"], first["filename"])
            self.assertEqual(len(list(inbox.iterdir())), 1)

    def test_dry_run_and_empty_annotations_write_nothing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            inbox = root / "Inbox"
            dry = crossfire.write_card(inbox, [ANNOTATION], corpus=corpus, neighbors=neighbors,
                                       db=root / "index.sqlite3", dry_run=True)
            self.assertEqual(dry["status"], "ok")
            self.assertFalse(inbox.exists() and any(inbox.iterdir()))
            empty = crossfire.write_card(inbox, [], corpus=corpus, neighbors=neighbors,
                                         db=root / "index.sqlite3")
            self.assertEqual(empty["status"], "empty")
            self.assertFalse(empty["written"])
            self.assertFalse(inbox.exists())

    def test_validates_reply_and_limit_bounds(self):
        with tempfile.TemporaryDirectory() as temporary:
            for replies, limit in ((0, 10), (7, 10), (2, 0), (2, 5001)):
                with self.assertRaises(ReaderError):
                    crossfire.write_card(Path(temporary), [ANNOTATION], replies=replies, limit=limit)

    def test_falls_back_to_bm25_when_no_sqlite_index_is_requested(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            result = crossfire.write_card(root / "Inbox", [ANNOTATION], corpus=corpus,
                                          neighbors=neighbors, db=None, dry_run=True)
            self.assertEqual(result["index"], "bm25")
            self.assertGreaterEqual(result["matches"], 2)


class SyncIntegrationTest(unittest.TestCase):
    def config(self, root):
        return {"version": 1, "volume_uuid": UUID,
                "annotations_dir": str(root / "annotations"),
                "firmware_path": str(root / "reviewed.bin"),
                "firmware_sha256": "0" * 64,
                "launchers_dir": str(root / "launchers")}

    def volume(self, root):
        card_root = root / "card"
        card_root.mkdir(parents=True, exist_ok=True)
        return CardVolume(card_root, "disk9s1", UUID)

    def store_annotation(self, volume):
        store = volume.root / ".crosspoint" / "annotations"
        store.mkdir(parents=True)
        (store / "1.json").write_text(json.dumps(ANNOTATION), encoding="utf-8")

    def test_sync_card_builds_crossfire_card_when_requested(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            volume = self.volume(root)
            self.store_annotation(volume)
            cache = Cache(root / "cache")
            result = sync_card(cache, self.config(root), volume=volume,
                               crossfire={"corpus": corpus, "neighbors": neighbors,
                                          "db": root / "index.sqlite3"})
            crossfire_result = result["annotations"]["crossfire"]
            self.assertEqual(crossfire_result["status"], "ok")
            self.assertTrue(crossfire_result["written"])
            cards = list((volume.root / BOOK_INBOX).glob("crossfire-*.epub"))
            self.assertEqual(len(cards), 1)
            self.assertEqual(cards[0].name, crossfire_result["filename"])

    def test_sync_card_unchanged_without_crossfire_flag(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            volume = self.volume(root)
            self.store_annotation(volume)
            result = sync_card(Cache(root / "cache"), self.config(root), volume=volume)
            self.assertNotIn("crossfire", result["annotations"])
            self.assertFalse((volume.root / BOOK_INBOX).exists())

    def test_autosync_threads_crossfire_through(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            volume = self.volume(root)
            cache = Cache(root / "cache")
            captured = {}

            def fake_sync(cache_arg, config, **kwargs):
                captured.update(kwargs)
                return {"annotations": {}, "card": {}, "books": {}}

            with patch.object(card_autosync.card, "sync_card", side_effect=fake_sync):
                result = card_autosync.autosync(
                    cache, self.config(root), resolve=lambda uuid: volume,
                    key_provider=lambda: ("", "no management key"),
                    notify=None, log=lambda message: None, crossfire=True)
            self.assertEqual(captured.get("crossfire"), True)
            self.assertEqual(result["status"], "error")


class CliTest(unittest.TestCase):
    def run_cli(self, argv):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            code = cli.main(argv)
        return code, stream.getvalue()

    def test_hooks_dry_run_prints_passages(self):
        import test_book_hooks

        with tempfile.TemporaryDirectory() as temporary:
            book = test_book_hooks.make_epub(Path(temporary) / "book.epub",
                                             test_book_hooks.sample_chapters(4))
            code, output = self.run_cli(["hooks", str(book), "--dry-run"])
            self.assertEqual(code, 0)
            payload = json.loads(output)
            self.assertGreaterEqual(len(payload["passages"]), 4)

    def test_hooks_writes_a_hooked_copy(self):
        import test_book_hooks

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            book = test_book_hooks.make_epub(root / "book.epub",
                                             test_book_hooks.sample_chapters(4))
            output_path = root / "hooked.epub"
            code, output = self.run_cli(["hooks", str(book), "-o", str(output_path)])
            self.assertEqual(code, 0)
            payload = json.loads(output)
            self.assertTrue(payload["changed"])
            self.assertTrue(output_path.is_file())

    def test_crossfire_command_writes_a_card(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            annotations = root / "annotations.json"
            annotations.write_text(json.dumps(ANNOTATION), encoding="utf-8")
            out = root / "card.epub"
            code, output = self.run_cli([
                "crossfire", "--annotations", str(annotations), "--out", str(out),
                "--corpus", str(corpus), "--neighbors", str(neighbors),
                "--db", str(root / "index.sqlite3")])
            self.assertEqual(code, 0)
            payload = json.loads(output)
            self.assertTrue(payload["written"])
            self.assertTrue(out.is_file())
            self.assertGreaterEqual(payload["matches"], 2)

    def test_crossfire_command_reports_missing_annotations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus, neighbors = make_corpus(root)
            empty = root / "empty.json"
            empty.write_text("[]", encoding="utf-8")
            code, _ = self.run_cli([
                "crossfire", "--annotations", str(empty), "--out", str(root / "x.epub"),
                "--corpus", str(corpus), "--neighbors", str(neighbors),
                "--db", str(root / "index.sqlite3")])
            self.assertEqual(code, 2)


class RealCorpusTest(unittest.TestCase):
    """Opt-in: RW_REAL_CORPUS/RW_REAL_NEIGHBORS (and optional RW_REAL_DB) exercise the real archive."""

    def test_real_archive_produces_attributed_replies(self):
        corpus = os.environ.get("RW_REAL_CORPUS")
        neighbors = os.environ.get("RW_REAL_NEIGHBORS")
        if not corpus or not Path(corpus).is_dir():
            self.skipTest("set RW_REAL_CORPUS to the Substack Markdown archive")
        database = os.environ.get("RW_REAL_DB")
        result = crossfire.write_card(
            None, [ANNOTATION], corpus=Path(corpus),
            neighbors=Path(neighbors) if neighbors else None,
            db=Path(database) if database else None, dry_run=True)
        self.assertEqual(result["status"], "ok")
        self.assertGreaterEqual(result["matches"], 1)
        reply = result["sections"][0]["replies"][0]
        self.assertTrue(reply["author"])
        self.assertTrue(reply["title"])
        self.assertTrue(reply["date"])
        self.assertGreaterEqual(len(reply["excerpt"]), 70)


if __name__ == "__main__":
    unittest.main()
