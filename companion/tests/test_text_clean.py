"""A chapter title printed twice in a row (heading + identical echo) is shown once in newly built books.

西游：拦路人！ repeats "第N章 ，标题" as an <h2> and an identical <h3> in every chapter.
"""
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from xml.etree import ElementTree

from reader_send import novels
from reader_send.novels import convert_bytes, convert_tree

XHTML = "{http://www.w3.org/1999/xhtml}"
OPF = {"o": "http://www.idpf.org/2007/opf"}


def read_epub(data: bytes) -> dict[str, bytes]:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist()}


def blocks(entries: dict[str, bytes]) -> list[tuple[str, str]]:
    package = ElementTree.fromstring(entries["OEBPS/content.opf"])
    manifest = {item.get("id"): item.get("href") for item in package.iterfind("o:manifest/o:item", OPF)}
    found = []
    for ref in package.iterfind("o:spine/o:itemref", OPF):
        body = ElementTree.fromstring(entries[f"OEBPS/{manifest[ref.get('idref')]}"]).find(f"{XHTML}body")
        found.extend((child.tag.removeprefix(XHTML), "".join(child.itertext())) for child in body)
    return found


def chapters(template: str, count: int = 6) -> bytes:
    return "".join(template.format(n=n) for n in range(1, count + 1)).encode("utf-8")


class DuplicateEchoTest(unittest.TestCase):
    def test_an_echo_identical_to_its_heading_is_dropped_and_accounted_for(self):
        book = convert_bytes(chapters("第{n}章 ，标题{n}\n第{n}章 ，标题{n}\n正文{n}。\n"), "dup")
        shown = blocks(read_epub(book.epub))
        self.assertNotIn("h3", [tag for tag, _ in shown])
        self.assertEqual([tag for tag, _ in shown][:2], ["h2", "p"])
        self.assertEqual([t for tag, t in shown if tag == "h2"][0], "第1章 ，标题1")  # source spacing untouched
        self.assertEqual((book.info["duplicate_headings_dropped"], book.info["chapters"]), (6, 6))
        # 6 dropped lines of 7 non-whitespace characters; they are not decoration
        self.assertEqual((book.info["duplicate_heading_chars_removed"], book.info["removed_decoration_chars"]), (42, 0))

    def test_dropped_duplicate_characters_are_counted_apart_from_decoration(self):
        text = chapters("第{n}章 标题\n## 第{n}章 标题\n====\n正文{n}。\n")
        book = convert_bytes(text, "dup2")
        self.assertEqual(book.info["duplicate_headings_dropped"], 6)
        self.assertEqual((book.info["duplicate_heading_chars_removed"], book.info["removed_decoration_chars"],
                          book.info["decoration_lines_dropped"]), (42, 24, 6))
        self.assertNotIn("h3", [tag for tag, _ in blocks(read_epub(book.epub))])

    def test_a_different_echo_is_kept(self):
        book = convert_bytes(chapters("第{n}章 标题{n}\n第00{n}章\n正文{n}。\n"), "echo")
        self.assertEqual([tag for tag, _ in blocks(read_epub(book.epub))][:3], ["h2", "h3", "p"])
        self.assertEqual(book.info["duplicate_headings_dropped"], 0)

    def test_an_echo_differing_only_in_spacing_is_kept(self):
        book = convert_bytes(chapters("第{n}章 ，标题\n第{n}章，标题\n正文{n}。\n"), "spacing")
        self.assertEqual([tag for tag, _ in blocks(read_epub(book.epub))][:3], ["h2", "h3", "p"])
        self.assertEqual(book.info["duplicate_headings_dropped"], 0)

    def test_preservation_still_rejects_lost_text_after_a_dropped_echo(self):
        text = chapters("第{n}章 标题{n}\n第{n}章 标题{n}\n正文{n}。\n").decode("utf-8")
        book = convert_bytes(text.encode("utf-8"), "dup")
        novels.check_preservation(book.epub, text)
        entries = read_epub(book.epub)
        name = next(n for n in entries if n.endswith("s00001.xhtml"))
        entries[name] = entries[name].replace("正文1。".encode(), "正文X。".encode())
        with self.assertRaises(novels.NovelPreservationError):
            novels.check_preservation(novels.pack_epub(entries), text)

    def test_source_whitespace_and_zero_width_characters_are_not_rewritten(self):
        text = chapters("第{n}章 ，标题\xa0二\n　　正文{n}  第一句。\xa0\xa0第二\u200b句\u00ad。 ，\n")
        shown = blocks(read_epub(convert_bytes(text, "verbatim").epub))
        self.assertEqual([t for tag, t in shown if tag == "h2"][0], "第1章 ，标题\xa0二")
        self.assertEqual([t for tag, t in shown if tag == "p"][0], "正文1  第一句。\xa0\xa0第二\u200b句\u00ad。 ，")

    def test_legacy_entries_stay_skipped_until_a_forced_rebuild(self):
        with tempfile.TemporaryDirectory() as temporary:
            src, out = Path(temporary, "src"), Path(temporary, "out")
            src.mkdir()
            (src / "书.txt").write_bytes(chapters("第{n}章 标题\n第{n}章 标题\n正文{n}。\n"))
            convert_tree(src, out)
            manifest_path = out / novels.MANIFEST_NAME
            manifest = json.loads(manifest_path.read_text("utf-8"))
            entry = manifest["entries"]["书.txt"]
            self.assertEqual(entry["text_clean"], novels.TEXT_CLEAN_REVISION)  # provenance of a new build
            epub_path = out / entry["output"]
            before = epub_path.read_bytes()
            entry.pop("text_clean")  # a book converted before this revision existed
            entry.pop("duplicate_headings_dropped")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            again = convert_tree(src, out)
            self.assertEqual((again["summary"]["converted"], again["summary"]["skipped"]), (0, 1))
            self.assertEqual(epub_path.read_bytes(), before)
            self.assertNotIn("text_clean", json.loads(manifest_path.read_text("utf-8"))["entries"]["书.txt"])
            forced = convert_tree(src, out, force=True)
            self.assertEqual((forced["summary"]["converted"], forced["summary"]["skipped"]), (1, 0))
            entry = json.loads(manifest_path.read_text("utf-8"))["entries"]["书.txt"]
            self.assertEqual((entry["text_clean"], entry["duplicate_headings_dropped"], entry["duplicate_heading_chars_removed"],
                              entry["removed_decoration_chars"]), (novels.TEXT_CLEAN_REVISION, 6, 30, 0))
            self.assertEqual((forced["summary"]["duplicate_headings_dropped"], forced["summary"]["duplicate_heading_chars_removed"]), (6, 30))


if __name__ == "__main__":
    unittest.main()
