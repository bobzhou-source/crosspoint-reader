import hashlib
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from xml.etree import ElementTree

from reader_send import book_hooks
from reader_send.book_hooks import CARD_FILENAME, CARD_ITEM_ID, CARD_MARKER, CARD_TITLE
from reader_send.common import ReaderError

STRONG = (
    "Every man who believes that power is a thing to be held has already lost it; power is only "
    "the shadow of a claim other men accept. And yet nobody admits this, because the admission "
    "would destroy the very claim they live by.",
    "Nothing is more dangerous than an idea that cannot be questioned, for such an idea must "
    "always end in the persecution of those who doubt it. The truth is not a possession but a "
    "practice, and every orthodoxy is the tomb of the practice that founded it.",
    "A state that must forbid argument has already confessed that its arguments are weak. But no "
    "tyrant ever confesses; instead he calls his weakness order, and his fear virtue.",
    "Men do not desire freedom; they desire the envy of their neighbours, and they will trade "
    "every liberty for the pleasure of seeing that envy satisfied. This is the secret engine of "
    "every demagogue, and it is why no constitution can survive a people who prefer resentment.",
    "The past is never past: what we call history is only the present arguing with itself in "
    "costume. Whoever claims to have escaped tradition is merely obeying a newer and less honest "
    "one, and his contempt for the old is itself an inheritance he did not choose.",
    "Pain is the only teacher that never flatters, and yet every comfort we invent is a scheme to "
    "avoid its lesson. A civilisation is measured not by its pleasures but by the pains it is "
    "willing to endure for the sake of something it loves more than itself.",
    "To love a thing is to be willing to be wrong about it in public, and no one who has never "
    "risked that humiliation has loved anything at all. Tolerance without conviction is not "
    "virtue; it is merely the courtesy of people who have nothing left to defend.",
    "The most dangerous lie is the one that is almost true, because it borrows the authority of "
    "the truth while quietly reversing its direction. Every tyranny begins as a correction, and "
    "every correction that cannot be corrected becomes the tyranny it once opposed.",
)
WEAK = (
    "The early cartographers drew the coastline from memory and often misplaced the river mouths. "
    "Their maps were copied many times before anyone corrected the errors.",
)


def chapter(title, paragraphs, *, heading="h2", paragraph_id=None):
    body = []
    for index, text in enumerate(paragraphs):
        identifier = f' id="{paragraph_id}"' if paragraph_id and index == 0 else ""
        body.append(f"<p{identifier}>{text}</p>")
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>'
            f"{title}</title></head><body><{heading}>{title}</{heading}>"
            + "".join(body) + "</body></html>")


def make_epub(path: Path, chapters: dict, *, opf_dir="OEBPS", docs_dir=None, href_prefix="",
              extra: dict | None = None, document_suffix=".xhtml"):
    docs_dir = opf_dir if docs_dir is None else docs_dir
    docs = {name: chapter(name, paragraphs) for name, paragraphs in chapters.items()}
    if extra:
        docs.update(extra)
    opf_path = f"{opf_dir}/content.opf" if opf_dir else "content.opf"
    items = "".join(
        f'<item id="c{index}" href="{href_prefix}{name}{document_suffix}" '
        f'media-type="application/xhtml+xml"/>'
        for index, name in enumerate(chapters))
    spine = "".join(f'<itemref idref="c{index}"/>' for index in range(len(chapters)))
    opf = ('<?xml version="1.0" encoding="UTF-8"?>'
           '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:identifier id="id">urn:uuid:11111111-1111-1111-1111-111111111111</dc:identifier>'
           '<dc:title>A Test Book</dc:title><dc:language>en</dc:language></metadata>'
           f'<manifest>{items}</manifest><spine>{spine}</spine></package>')
    container = ('<?xml version="1.0"?>'
                 '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                 f'<rootfiles><rootfile full-path="{opf_path}" '
                 'media-type="application/oebps-package+xml"/></rootfiles></container>')
    entries = {
        "mimetype": b"application/epub+zip",
        "META-INF/container.xml": container.encode(),
        opf_path: opf.encode(),
    }
    for name, markup in docs.items():
        entry = f"{docs_dir}/{name}{document_suffix}" if docs_dir else f"{name}{document_suffix}"
        entries[entry] = markup.encode()
    write_epub(path, entries)
    return path


def write_epub(path: Path, entries: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def sample_chapters(count=5):
    chapters = {}
    for number in range(1, count + 1):
        start = ((number - 1) * 3) % len(STRONG)
        chapters[f"chapter{number}"] = [STRONG[(start + offset) % len(STRONG)]
                                        for offset in range(3)]
    return chapters


class ScoringTest(unittest.TestCase):
    def test_provocative_claims_outrank_flat_narration(self):
        strong = book_hooks.score_text(STRONG[0])
        weak = book_hooks.score_text(WEAK[0])
        self.assertGreater(strong.total, weak.total + 2)
        self.assertGreaterEqual(strong.claims, 2)
        self.assertGreaterEqual(strong.contrasts, 1)

    def test_penalizes_apparatus_urls_citations_and_questions(self):
        self.assertIn("imprint", book_hooks.score_text(
            "All rights reserved. Printed in the United States. ISBN 9780000000000.").penalties)
        self.assertIn("url", book_hooks.score_text(
            "Every claim here is false; see http://example.com/must-read for the truth.").penalties)
        self.assertIn("question", book_hooks.score_text(
            "Is it not true that every man must serve something greater than himself?").penalties)
        self.assertIn("digits", book_hooks.score_text(
            "In 1618 the 3 friars walked 92 miles and 4 of them died before 1620.").penalties)

    def test_sentence_split_keeps_abbreviations_and_quotes(self):
        parts = book_hooks.sentences('Mr. Smith met Dr. Jones in the road. "Come now," he said. '
                                     "They walked on.")
        self.assertEqual(len(parts), 3)
        self.assertTrue(parts[0].startswith("Mr. Smith"))

    def test_clean_passage_strips_orphan_quotation_marks(self):
        self.assertEqual(book_hooks._clean_passage("’ and then he left."), "and then he left.")

    def test_words_and_mid_sentence_penalty(self):
        self.assertEqual(book_hooks.words("Power is a Shadow!"), ["power", "is", "a", "shadow"])
        self.assertIn("mid-sentence", book_hooks.score_text(
            "and then he said that power is never held but only lent.").penalties)


class ParsingTest(unittest.TestCase):
    def test_reads_spine_documents_and_skips_matter(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "book.epub"
            chapters = sample_chapters(4)
            chapters["copyright"] = ["Copyright 2020. All rights reserved. Printed in the United States."]
            chapters["index"] = [STRONG[0], STRONG[1]]
            chapters["toc"] = ["Chapter One 1 Chapter Two 45 Chapter Three 90."]
            make_epub(path, chapters)
            archive, opf, opf_text, documents = book_hooks.read_book(path)
            archive.close()
            self.assertEqual(opf, "OEBPS/content.opf")
            by_path = {document.path: document for document in documents}
            self.assertFalse(by_path["OEBPS/copyright.xhtml"].eligible)
            self.assertFalse(by_path["OEBPS/index.xhtml"].eligible)
            self.assertFalse(by_path["OEBPS/toc.xhtml"].eligible)
            self.assertTrue(by_path["OEBPS/chapter1.xhtml"].eligible)
            self.assertEqual(by_path["OEBPS/chapter1.xhtml"].title, "chapter1")

    def test_div_paragraph_fallback_and_nested_markup_spacing(self):
        markup = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>'
                  '<div><span>Every</span><span>man</span> who believes power can be held has '
                  'already lost it; power is only the shadow of a claim others accept.</div>'
                  '<div>' + STRONG[0] + "</div></body></html>")
        blocks = book_hooks._scan_blocks(markup, book_hooks.BLOCK_TAGS)
        self.assertEqual(blocks, [])
        blocks = book_hooks._scan_blocks(markup, ("div",))
        self.assertTrue(blocks)
        self.assertTrue(blocks[0].text.startswith("Every man who believes"))

    def test_zip_path_resolves_relative_hrefs(self):
        self.assertEqual(book_hooks._zip_path("content.opf", "OEBPS/ch1.xhtml"),
                         "OEBPS/ch1.xhtml")
        self.assertEqual(book_hooks._zip_path("OEBPS/content.opf", "text/ch1.xhtml"),
                         "OEBPS/text/ch1.xhtml")
        self.assertEqual(book_hooks._zip_path("OEBPS/content.opf", "../ch1.xhtml"), "ch1.xhtml")


class SelectionTest(unittest.TestCase):
    def test_selects_four_or_five_spread_across_chapters(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = make_epub(Path(temporary) / "book.epub", sample_chapters(6))
            passages = book_hooks.select_passages(path)
            self.assertIn(len(passages), (4, 5))
            self.assertGreaterEqual(len({passage.doc_path for passage in passages}), 3)
            for passage in passages:
                self.assertGreaterEqual(len(passage.text), book_hooks.MIN_PASSAGE)
                self.assertLessEqual(len(passage.text), book_hooks.MAX_PASSAGE)
                self.assertTrue(passage.chapter)
                self.assertTrue(passage.anchor)

    def test_never_selects_front_or_back_matter_even_when_strong(self):
        with tempfile.TemporaryDirectory() as temporary:
            chapters = sample_chapters(4)
            chapters["copyright"] = [STRONG[0]]
            chapters["index"] = list(STRONG)
            path = make_epub(Path(temporary) / "book.epub", chapters)
            passages = book_hooks.select_passages(path)
            self.assertFalse(any("copyright" in passage.doc_path or "index" in passage.doc_path
                                 for passage in passages))

    def test_existing_paragraph_id_is_reused_as_anchor(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "book.epub"
            base = root / "base.epub"
            make_epub(base, {f"chapter{n}": list(STRONG) for n in range(1, 5)})
            with zipfile.ZipFile(base) as archive:
                entries = {name: archive.read(name) for name in archive.namelist()}
            for number in range(1, 5):
                entries[f"OEBPS/chapter{number}.xhtml"] = chapter(
                    f"Chapter {number}", list(STRONG),
                    paragraph_id=f"passage-{number}").encode()
            write_epub(path, entries)
            result = book_hooks.apply_hooks(path)
            self.assertTrue(any(passage.anchor.startswith("passage-") for passage in result.passages))
            archive = zipfile.ZipFile(path)
            for passage in result.passages:
                if passage.anchor.startswith("passage-"):
                    self.assertIn(f'id="{passage.anchor}"'.encode(),
                                  archive.read(passage.doc_path))
            archive.close()

    def test_refuses_book_without_enough_self_contained_passages(self):
        with tempfile.TemporaryDirectory() as temporary:
            chapters = {f"chapter{n}": [WEAK[0]] for n in range(1, 4)}
            path = make_epub(Path(temporary) / "book.epub", chapters)
            with self.assertRaises(ReaderError):
                book_hooks.select_passages(path)

    def test_validates_count_and_minimum(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = make_epub(Path(temporary) / "book.epub", sample_chapters(4))
            for count, minimum in ((0, 0), (13, 1), (5, 6), (5, -1)):
                with self.assertRaises(ReaderError):
                    book_hooks.select_passages(path, count=count, minimum=minimum)

    def test_rejects_non_epub_and_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plain = root / "not.epub"
            plain.write_text("not a zip", encoding="utf-8")
            with self.assertRaises(ReaderError):
                book_hooks.select_passages(plain)
            with self.assertRaises(ReaderError):
                book_hooks.apply_hooks(root / "missing.epub")
            target = make_epub(root / "real.epub", sample_chapters(4))
            link = root / "link.epub"
            os.symlink(target, link)
            with self.assertRaises(ReaderError):
                book_hooks.apply_hooks(link)
            with self.assertRaises(ReaderError):
                book_hooks.apply_hooks(target, root / "out.txt")


class RewriteTest(unittest.TestCase):
    def hooked(self, root: Path, **kwargs) -> tuple[Path, book_hooks.HookResult]:
        path = make_epub(root / "book.epub", sample_chapters(5), **kwargs)
        return path, book_hooks.apply_hooks(path)

    def test_inserts_card_first_in_spine_with_resolving_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, result = self.hooked(Path(temporary))
            self.assertTrue(result.changed)
            self.assertIsNotNone(result.backup)
            self.assertTrue(result.backup.is_file())
            archive = zipfile.ZipFile(path)
            self.assertEqual(archive.namelist()[0], "mimetype")
            self.assertEqual(archive.getinfo("mimetype").compress_type, zipfile.ZIP_STORED)
            self.assertIsNone(archive.testzip())
            opf = archive.read("OEBPS/content.opf").decode()
            self.assertEqual(opf.count(f'id="{CARD_ITEM_ID}"'), 1)
            spine = opf[opf.index("<spine"):]
            self.assertTrue(spine.startswith(f'<spine><itemref idref="{CARD_ITEM_ID}"/>'))
            self.assertIn(CARD_MARKER, opf)
            card = archive.read(f"OEBPS/{CARD_FILENAME}")
            ElementTree.fromstring(card)
            text = card.decode()
            self.assertIn(CARD_TITLE, text)
            links = [part.split('"')[0] for part in text.split('href="')[1:]]
            self.assertEqual(len(links), len(result.passages))
            for link in links:
                target, _, anchor = link.partition("#")
                self.assertIn(f"OEBPS/{target}", archive.namelist())
                markup = archive.read(f"OEBPS/{target}").decode()
                self.assertEqual(markup.count(f'id="{anchor}"'), 1)
            archive.close()

    def test_rerun_is_idempotent_and_keeps_one_backup(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, first = self.hooked(Path(temporary))
            before = hashlib.sha256(path.read_bytes()).hexdigest()
            second = book_hooks.apply_hooks(path)
            after = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertFalse(second.changed)
            self.assertEqual(before, after)
            self.assertEqual(book_hooks.apply_hooks(path).changed, False)
            archive = zipfile.ZipFile(path)
            opf = archive.read("OEBPS/content.opf").decode()
            self.assertEqual(opf.count(f'id="{CARD_ITEM_ID}"'), 1)
            self.assertEqual(opf.count(f'idref="{CARD_ITEM_ID}"'), 1)
            self.assertEqual(archive.namelist().count(f"OEBPS/{CARD_FILENAME}"), 1)
            for passage in second.passages:
                markup = archive.read(passage.doc_path).decode()
                self.assertEqual(markup.count(f'id="{passage.anchor}"'), 1)
            archive.close()
            backups = list(Path(temporary).glob("*.pre-hooks.epub"))
            self.assertEqual(len(backups), 1)

    def test_output_copy_leaves_source_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = make_epub(root / "book.epub", sample_chapters(4))
            original = source.read_bytes()
            result = book_hooks.apply_hooks(source, root / "hooked.epub")
            self.assertEqual(source.read_bytes(), original)
            self.assertTrue(result.output.is_file())
            self.assertIn(f"OEBPS/{CARD_FILENAME}", zipfile.ZipFile(result.output).namelist())

    def test_opf_at_root_with_prefixed_hrefs(self):
        with tempfile.TemporaryDirectory() as temporary:
            chapters = sample_chapters(4)
            path = make_epub(Path(temporary) / "book.epub", chapters, opf_dir="",
                             docs_dir="OEBPS", href_prefix="OEBPS/", document_suffix=".htm")
            result = book_hooks.apply_hooks(path)
            archive = zipfile.ZipFile(path)
            card = archive.read(book_hooks.CARD_FILENAME).decode()
            for passage in result.passages:
                self.assertIn(passage.doc_path, archive.namelist())
                relative = passage.doc_path
                self.assertIn(f'href="{relative}#{passage.anchor}"', card)
            archive.close()

    def test_card_escapes_markup_in_passages(self):
        passage = book_hooks.Passage(
            text='Power is “the state” & <man>; and yet nobody admits it.',
            score=book_hooks.score_text("Power is not a thing but a relation, and yet nobody admits it."),
            doc_path="OEBPS/chapter1.xhtml", doc_href="chapter1.xhtml", chapter='A "Chapter" & More',
            block_index=0, start=0, anchor="rw-hook-1")
        card = book_hooks._card_xhtml([passage], "OEBPS/" + CARD_FILENAME)
        ElementTree.fromstring(card.encode())
        self.assertIn("&amp;", card)
        self.assertIn("&lt;man&gt;", card)
        self.assertIn("&quot;Chapter&quot;", card)
        self.assertIn('href="chapter1.xhtml#rw-hook-1"', card)


class RealBookTest(unittest.TestCase):
    """Opt-in: point RW_REAL_BOOKS at EPUB copies to exercise genuine publisher markup."""

    def test_real_epubs_produce_four_to_five_linked_passages(self):
        root = os.environ.get("RW_REAL_BOOKS")
        if not root or not Path(root).is_dir():
            self.skipTest("set RW_REAL_BOOKS to a folder of real EPUBs")
        books = sorted(Path(root).glob("*.epub"))
        self.assertTrue(books, "no EPUBs found in RW_REAL_BOOKS")
        for book in books:
            with self.subTest(book=book.name), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "hooked.epub"
                result = book_hooks.apply_hooks(book, output, backup=False)
                self.assertGreaterEqual(len(result.passages), 4, book.name)
                self.assertLessEqual(len(result.passages), 5, book.name)
                archive = zipfile.ZipFile(output)
                self.assertIsNone(archive.testzip())
                card = archive.read(result.card).decode()
                ElementTree.fromstring(card.encode())
                for passage in result.passages:
                    self.assertIn(passage.doc_path, archive.namelist())
                    self.assertIn(f"#{passage.anchor}", card)
                    self.assertIn(f'id="{passage.anchor}"',
                                  archive.read(passage.doc_path).decode())
                archive.close()
                second = book_hooks.apply_hooks(output)
                self.assertFalse(second.changed, book.name)


if __name__ == "__main__":
    unittest.main()
