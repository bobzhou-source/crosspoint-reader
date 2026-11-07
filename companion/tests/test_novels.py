import codecs
import contextlib
import io
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock
from xml.etree import ElementTree

from reader_send import card, cli, novels
from reader_send.cache import Cache
from reader_send.card import CardVolume, sync_card
from reader_send.common import ReaderError, digest
from reader_send.epub import pack_epub
from reader_send.novels import (CEILING_FILE_BYTES, CHAPTER, ECHO, VOLUME, NovelDecodeError, NovelPreservationError,
                                check_preservation, classify_heading, convert_bytes, convert_tree, decode_text,
                                detect_headings, normalize_text, output_name, paragraphs_of, parse_filename,
                                parse_numeral, strip_decoration)

UUID = "6A4B4C73-B747-33DA-BF3D-455A3C03D0CB"
XHTML = "{http://www.w3.org/1999/xhtml}"
NCX = "{http://www.daisy.org/z3986/2005/ncx/}"
OPF = {"o": "http://www.idpf.org/2007/opf"}


def read_epub(data: bytes) -> dict[str, bytes]:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        return {info.filename: archive.read(info) for info in archive.infolist()}


def spine_names(entries: dict[str, bytes]) -> list[str]:
    package = ElementTree.fromstring(entries["OEBPS/content.opf"])
    manifest = {item.get("id"): item.get("href") for item in package.iterfind("o:manifest/o:item", OPF)}
    return [f"OEBPS/{manifest[ref.get('idref')]}" for ref in package.iterfind("o:spine/o:itemref", OPF)]


def blocks(entries: dict[str, bytes]) -> list[tuple[str, str]]:
    """Every (tag, text) block element in spine order."""
    found = []
    for name in spine_names(entries):
        body = ElementTree.fromstring(entries[name]).find(f"{XHTML}body")
        found.extend((child.tag.removeprefix(XHTML), "".join(child.itertext())) for child in body)
    return found


def nav_tree(entries: dict[str, bytes]) -> list:
    def walk(ol):
        result = []
        for item in ol.findall(f"{XHTML}li"):
            anchor = item.find(f"{XHTML}a")
            child = item.find(f"{XHTML}ol")
            result.append((anchor.text, walk(child) if child is not None else []))
        return result
    nav = ElementTree.fromstring(entries["OEBPS/nav.xhtml"])
    return walk(nav.find(f".//{XHTML}nav/{XHTML}ol"))


def ncx_tree(entries: dict[str, bytes]) -> list:
    def walk(parent):
        return [(point.findtext(f"{NCX}navLabel/{NCX}text"), walk(point)) for point in parent.findall(f"{NCX}navPoint")]
    return walk(ElementTree.fromstring(entries["OEBPS/toc.ncx"]).find(f"{NCX}navMap"))


def flat_book(chapters=8, body=("这是正文。", "又一段。")) -> str:
    lines = []
    for number in range(1, chapters + 1):
        lines.append(f"第{number}章 标题{number}")
        lines.extend(body)
    return "\n".join(lines) + "\n"


def long_paragraph(index: int, size: int = 100) -> str:
    return f"段落{index:03d}" + "文字" * size + "。"


class HeadingTest(unittest.TestCase):
    def test_numbered_headings_in_every_numeral_style_and_unit(self):
        chapters = ["第一章 重生", "第12章 开始", "第１２章 全角", "第三百二十四回 大战", "第五节", "第3篇", "第 七 章",
                    "第十话", "第壹章"]
        for line in chapters:
            heading = classify_heading(line)
            self.assertIsNotNone(heading, line)
            self.assertEqual(heading.level, CHAPTER, line)
        for line in ["第二卷 风起", "第10部", "第一集", "第三部 终章"]:
            self.assertEqual(classify_heading(line).level, VOLUME, line)
        self.assertEqual(classify_heading("第一章 重生").label, "第一章 重生")

    def test_special_titles_and_english_chapters(self):
        for line in ["序章", "序章 初见", "楔子", "引子", "尾声", "番外", "番外 一", "番外篇", "后记", "完本感言",
                     "Prologue", "Epilogue", "Chapter 12", "1、Chapter 1", "CHAPTER ONE", "chapter 3: Title"]:
            heading = classify_heading(line)
            self.assertIsNotNone(heading, line)
            self.assertEqual(heading.level, CHAPTER, line)

    def test_wrapped_and_markdown_headings(self):
        self.assertEqual(classify_heading("###第1章，尸兄###").label, "第1章，尸兄")
        self.assertEqual(classify_heading("###《自知之人》###").label, "《自知之人》")
        self.assertIsNone(classify_heading("## 一 穿越"))
        self.assertEqual(classify_heading("## 一 穿越", hash_headings=True), novels.Heading(CHAPTER, "一 穿越"))
        self.assertEqual(classify_heading("# 正文卷", hash_headings=True), novels.Heading(VOLUME, "正文卷"))
        self.assertIsNone(classify_heading("#标签", hash_headings=True))

    def test_prose_that_merely_mentions_headings_is_not_a_heading(self):
        for line in ["他说第一章已经写完了。", "第一章节的内容很精彩，", "第一章 他走了。", "楔子里面的东西", "番外人员到了",
                     "序章之后他又来了。", "这是第三章", "第一章 " + "很长" * 40, "Chapters are long"]:
            self.assertIsNone(classify_heading(line), line)

    def test_numerals(self):
        for text, value in [("12", 12), ("十二", 12), ("一百零五", 105), ("三百二十四", 324), ("二〇二三", 2023),
                            ("１２", 12), ("一千二百", 1200), ("两千", 2000), ("十", 10), ("二十", 20), ("一万二千", 12000)]:
            self.assertEqual(parse_numeral(text), value, text)
        self.assertIsNone(parse_numeral("abc"))

    def test_bare_numbered_headings_need_a_long_unbroken_sequence(self):
        numerals = ["一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "十一", "十二"]
        paragraphs = [x for n in numerals for x in (f"{n} 标题", "正文一句。")]
        self.assertEqual(len(detect_headings(paragraphs)), 12)
        self.assertEqual(detect_headings([x for n in numerals[:6] for x in (f"{n} 标题", "正文一句。")]), {})
        broken = [x for n in ["一", "二", "四"] * 4 for x in (f"{n} 标题", "正文一句。")]
        self.assertEqual(detect_headings(broken), {})

    def test_bare_chain_survives_missing_numbers_and_stray_numbered_lines(self):
        numbers = [1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14]  # 7 is missing
        paragraphs = []
        for n in numbers:
            paragraphs += [f"{n}.标题{n}", "正文一句。"]
            if n == 3:
                paragraphs.append("6.5倍的力量")
        found = detect_headings(paragraphs)
        self.assertEqual(len(found), len(numbers))
        self.assertNotIn(paragraphs.index("6.5倍的力量"), found)
        self.assertEqual([found[i].label for i in sorted(found)][:2], ["1.标题1", "2.标题2"])

    def test_bare_chain_accepts_a_run_of_missing_numbers_and_headings_glued_to_their_number(self):
        numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 18, 19, 20]  # 13-17 are missing
        paragraphs = []
        for n in numbers:
            if n == 3:
                paragraphs.append("3个人走了进来")  # looks like a heading without a separator; the real one wins
            paragraphs += [f"{n}克苏鲁{n}" if n in (11, 12) else f"{n}.标题{n}", "正文一句。"]
        found = detect_headings(paragraphs)
        self.assertEqual(len(found), len(numbers))
        self.assertNotIn(paragraphs.index("3个人走了进来"), found)
        self.assertIn("11克苏鲁11", [found[i].label for i in found])

    def test_bare_chain_refuses_lists_and_prefers_chapters_over_a_contents_block(self):
        floors = [f"{n}.回复{n}" for n in range(1, 30)] + ["正文。"]
        self.assertEqual(detect_headings(floors), {})
        latin = [x for n in range(1, 30) for x in (f"{n}L：回复", "又一句。")]
        self.assertEqual(detect_headings(latin), {})
        contents = [f"{n}.标题{n}" for n in range(1, 13)]
        book = contents + [x for n in range(1, 13) for x in (f"{n}.标题{n}", "正文。")]
        found = detect_headings(book)
        self.assertEqual(len(found), 12)
        self.assertGreaterEqual(min(found), len(contents))

    def test_jinjiang_star_headings_with_and_without_chapter_numbers(self):
        lines = []
        for n in range(1, 7):
            lines += [f"☆、第{n}章 第{n}朵白莲花" if n % 2 else f"☆、第{n}朵白莲花~", "内容。"]
        found = detect_headings(lines)
        self.assertEqual([found[i].label for i in sorted(found)],
                         ["第1章 第1朵白莲花", "第2朵白莲花~", "第3章 第3朵白莲花", "第4朵白莲花~",
                          "第5章 第5朵白莲花", "第6朵白莲花~"])

    def test_hash_headings_only_count_in_files_that_use_them(self):
        marked = [x for i in range(4) for x in (f"## {i}真的", "====", "内容。")]
        self.assertEqual(len(detect_headings(marked)), 4)
        stray = ["# 标题", "内容。", "## 小标题", "内容。"]
        self.assertEqual(detect_headings(stray), {})

    def test_headings_without_a_body_stay_plain_text(self):
        contents = [f"第{i}章 甲" for i in range(1, 6)]
        found = detect_headings(contents + [x for i in range(1, 7) for x in (f"第{i}章 甲", "内容。")])
        self.assertEqual(sorted(found), list(range(5, 17, 2)))


class EncodingTest(unittest.TestCase):
    TEXT = "你好，世界。\n第二行……"

    def test_utf8_with_and_without_bom(self):
        self.assertEqual(decode_text(self.TEXT.encode("utf-8")), (self.TEXT, "utf-8", []))
        self.assertEqual(decode_text(codecs.BOM_UTF8 + self.TEXT.encode("utf-8")), (self.TEXT, "utf-8-bom", []))

    def test_utf16_requires_a_bom_and_handles_both_orders(self):
        self.assertEqual(decode_text(codecs.BOM_UTF16_LE + self.TEXT.encode("utf-16-le"))[:2], (self.TEXT, "utf-16-le"))
        self.assertEqual(decode_text(codecs.BOM_UTF16_BE + self.TEXT.encode("utf-16-be"))[:2], (self.TEXT, "utf-16-be"))
        with self.assertRaises(NovelDecodeError):
            decode_text(self.TEXT.encode("utf-16-le"))

    def test_gb18030_fallback(self):
        self.assertEqual(decode_text(self.TEXT.encode("gb18030")), (self.TEXT, "gb18030", []))

    def test_gb18030_with_embedded_utf8_punctuation_is_recovered_and_flagged(self):
        for tail in ("再见", "ab"):  # "ab" is the case strict GB18030 silently misreads as private-use characters
            data = "你好，世界".encode("gb18030") + "•".encode("utf-8") + tail.encode("gb18030")
            text, encoding, warnings = decode_text(data)
            self.assertEqual(text, f"你好，世界•{tail}")
            self.assertEqual(encoding, "gb18030+utf-8")
            self.assertTrue(warnings and "Mixed encodings" in warnings[0])

    def test_undecodable_files_are_refused_without_replacement_characters(self):
        for data in (b"\x81\x20abc", b"abc\xff\xfe\xfdxyz", b"ab\x00cd"):
            with self.assertRaises(NovelDecodeError):
                decode_text(data)

    def test_convert_refuses_an_undecodable_book(self):
        with self.assertRaisesRegex(NovelDecodeError, "Undecodable"):
            convert_bytes(b"\x81\x20" * 50, "bad")


class NormalizationTest(unittest.TestCase):
    def test_newlines_are_unified(self):
        text, dropped = normalize_text("a\r\nb\rc\u2028d\u2029e\u0085f\ng")
        self.assertEqual(text, "a\nb\nc\nd\ne\nf\ng")
        self.assertEqual(dropped, 0)

    def test_xml_invalid_controls_are_dropped_or_spaced_and_counted(self):
        text, dropped = normalize_text("a\x00b\x0bc\x1fd\x08e\ufffff")
        self.assertEqual(text, "ab c def")
        self.assertEqual(dropped, 3)

    def test_paragraphs_are_stripped_of_every_kind_of_leading_whitespace(self):
        text = "　　甲\n\t乙\n \u3000 \t丙  \n\n　　\n   \n丁"
        self.assertEqual(paragraphs_of(text), ["甲", "乙", "丙", "丁"])


class FilenameTest(unittest.TestCase):
    def test_patterns(self):
        cases = {
            "[作者]书名": ("书名", "作者"),
            "【作者】书名": ("书名", "作者"),
            "[作者]《书名》": ("书名", "作者"),
            "《书名》作者：某人": ("书名", "某人"),
            "《书名》某人著": ("书名", "某人"),
            "书名_某人": ("书名", "某人"),
            "书名（作者：某人）": ("书名", "某人"),
            "纯书名": ("纯书名", ""),
            "《只有书名》": ("只有书名", ""),
        }
        for stem, expected in cases.items():
            self.assertEqual(parse_filename(stem), expected, stem)

    def test_output_names_are_fat_safe_and_short(self):
        self.assertEqual(output_name("甲:乙?丙"), "甲：乙？丙.epub")
        self.assertEqual(output_name("书名. "), "书名.epub")
        long_name = output_name("长" * 300)
        self.assertLessEqual(len(long_name.encode("utf-8")), novels.MAX_OUTPUT_NAME_BYTES)
        self.assertNotEqual(long_name, output_name("长" * 299 + "短"))
        self.assertEqual(output_name("甲"), output_name("甲"))


class StructureTest(unittest.TestCase):
    def test_startup_members_precede_chapters_in_zip_directory(self):
        book = convert_bytes(flat_book(30).encode("utf-8"), "ordered")
        entries = read_epub(book.epub)
        names = list(entries)
        self.assertEqual(names[:6], ["mimetype", "META-INF/container.xml", "OEBPS/content.opf",
                                    "OEBPS/nav.xhtml", "OEBPS/toc.ncx", "OEBPS/style.css"])
        self.assertEqual(names[6:], spine_names(entries))
        self.assertEqual(len(spine_names(entries)), 30)
        self.assertEqual([label for label, _ in nav_tree(entries)],
                         [f"第{number}章 标题{number}" for number in range(1, 31)])

    def test_volumes_nest_chapters_and_front_matter_gets_its_own_file(self):
        lines = ["简介一行。", "作者：某人"]
        for volume in (1, 2):
            lines.append(f"第{volume}卷 卷名{volume}")
            for chapter in (1, 2, 3):
                lines += [f"第{(volume - 1) * 3 + chapter}章 章名", f"这是正文{volume}-{chapter}。", "又一段。"]
        book = convert_bytes("\n".join(lines).encode("utf-8"), "[某人]有卷的书")
        entries = read_epub(book.epub)
        self.assertEqual((book.info["volumes"], book.info["chapters"], book.info["files"]), (2, 6, 9))
        self.assertEqual(book.info["mode"], "headings")
        tree = [("卷首", []),
                ("第1卷 卷名1", [("第1章 章名", []), ("第2章 章名", []), ("第3章 章名", [])]),
                ("第2卷 卷名2", [("第4章 章名", []), ("第5章 章名", []), ("第6章 章名", [])])]
        self.assertEqual(nav_tree(entries), tree)
        self.assertEqual(ncx_tree(entries), tree)
        self.assertIn(b'name="dtb:depth" content="2"', entries["OEBPS/toc.ncx"])
        names = spine_names(entries)
        first = ElementTree.fromstring(entries[names[0]]).find(f"{XHTML}body")
        self.assertEqual(["".join(p.itertext()) for p in first], ["简介一行。", "作者：某人"])
        volume_page = ElementTree.fromstring(entries[names[1]]).find(f"{XHTML}body")
        self.assertEqual([(c.tag.removeprefix(XHTML), "".join(c.itertext())) for c in volume_page], [("h1", "第1卷 卷名1")])
        self.assertEqual([t for t, _ in blocks(entries) if t == "h2"], ["h2"] * 6)

    def test_flat_books_have_a_flat_toc_and_spine_order_matches_the_source(self):
        text = "\n".join(["前言之前的话。", flat_book(6)])
        entries = read_epub(convert_bytes(text.encode("utf-8"), "flat").epub)
        self.assertEqual([label for label, _ in nav_tree(entries)], ["卷首"] + [f"第{i}章 标题{i}" for i in range(1, 7)])
        self.assertTrue(all(not children for _, children in nav_tree(entries)))
        self.assertEqual([t for t, _ in blocks(entries)][:3], ["p", "h2", "p"])
        self.assertEqual(
            [text for _, text in blocks(entries)], paragraphs_of(text))

    def test_a_table_of_contents_at_the_top_is_not_split_into_empty_chapters(self):
        contents = "\n".join(f"第{i}章 甲" for i in range(1, 6))
        book = convert_bytes((contents + "\n" + flat_book(6)).encode("utf-8"), "toc")
        self.assertEqual(book.info["chapters"], 6)
        self.assertEqual(book.info["toc_entries"], 7)
        entries = read_epub(book.epub)
        front = ElementTree.fromstring(entries[spine_names(entries)[0]]).find(f"{XHTML}body")
        self.assertEqual([c.tag.removeprefix(XHTML) for c in front], ["p"] * 5)

    def test_epub_package_is_valid_and_labelled_zh_cn(self):
        book = convert_bytes(flat_book(6).encode("utf-8"), "[某人]书名")
        with zipfile.ZipFile(io.BytesIO(book.epub)) as archive:
            first = archive.infolist()[0]
            self.assertEqual((first.filename, first.compress_type), ("mimetype", zipfile.ZIP_STORED))
            self.assertEqual(archive.read("mimetype"), b"application/epub+zip")
            self.assertIsNone(archive.testzip())
        entries = read_epub(book.epub)
        package = ElementTree.fromstring(entries["OEBPS/content.opf"])
        dc = "{http://purl.org/dc/elements/1.1/}"
        self.assertEqual(package.findtext(f"o:metadata/{dc}language", namespaces=OPF), "zh-CN")
        self.assertEqual(package.findtext(f"o:metadata/{dc}title", namespaces=OPF), "书名")
        self.assertEqual(package.findtext(f"o:metadata/{dc}creator", namespaces=OPF), "某人")
        nav_item = package.find("o:manifest/o:item[@properties='nav']", OPF)
        self.assertEqual(nav_item.get("href"), "nav.xhtml")
        self.assertNotIn("OEBPS/nav.xhtml", spine_names(entries))
        self.assertEqual(package.find("o:spine", OPF).get("toc"), "ncx")
        self.assertIn(b'epub:type="toc"', entries["OEBPS/nav.xhtml"])
        self.assertIn(b"OEBPS/content.opf", entries["META-INF/container.xml"])

    def test_output_is_byte_for_byte_reproducible(self):
        data = flat_book(7).encode("utf-8")
        self.assertEqual(convert_bytes(data, "x").epub, convert_bytes(data, "x").epub)

    def test_author_falls_back_to_the_file_header(self):
        text = "题名：某书\n作者：某作者\n" + flat_book(6)
        self.assertEqual(convert_bytes(text.encode("utf-8"), "无作者书名").info["author"], "某作者")


class ParagraphStyleTest(unittest.TestCase):
    def test_paragraphs_are_trimmed_unindented_and_never_empty(self):
        text = ("　　序言一\n\t序言二\n\n" + "\n".join(
            f"第{i}章 标题\n　　　　第一段{i}。\n\t\t第二段{i}。\n   　 第三段{i}。\n　　\n \n\n" for i in range(1, 7)))
        book = convert_bytes(text.encode("utf-8"), "style")
        entries = read_epub(book.epub)
        parsed = blocks(entries)
        paragraphs = [text for tag, text in parsed if tag == "p"]
        self.assertEqual(len(paragraphs), 2 + 6 * 3)
        self.assertTrue(all(p and p == p.strip() and not p.startswith(("\u3000", " ", "\t")) for p in paragraphs))
        self.assertEqual([t for t, _ in parsed if t != "p"], ["h2"] * 6)
        for name in spine_names(entries):
            markup = entries[name].decode("utf-8")
            self.assertNotIn("<p></p>", markup)
            self.assertNotIn("<p>\u3000", markup)
            self.assertNotIn("<p> ", markup)
            self.assertNotIn("<p>\t", markup)
        css = entries["OEBPS/style.css"].decode("utf-8")
        self.assertIn("p{text-indent:2em;margin:0}", css)
        self.assertIn("h1,h2,h3{text-indent:0", css)
        self.assertEqual(book.info["mode"], "headings")

    def test_markup_characters_survive_escaping(self):
        text = flat_book(6, body=("a < b & c > d。", "“引号” & ‘单引号’ <tag>"))
        book = convert_bytes(text.encode("utf-8"), "escape")
        entries = read_epub(book.epub)
        self.assertIn("a < b & c > d。", [t for _, t in blocks(entries)])


class SplitTest(unittest.TestCase):
    def sizes(self, book):
        entries = read_epub(book.epub)
        return [len(entries[name]) for name in spine_names(entries)]

    def test_books_without_headings_are_split_by_size_at_paragraph_boundaries(self):
        paragraphs = [long_paragraph(i) for i in range(400)]
        book = convert_bytes("\n".join(paragraphs).encode("utf-8"), "无章节")
        self.assertEqual(book.info["mode"], "size")
        sizes = self.sizes(book)
        self.assertGreater(len(sizes), 4)
        self.assertLessEqual(max(sizes), CEILING_FILE_BYTES)
        self.assertGreaterEqual(min(sizes[:-1]), 30_000)
        entries = read_epub(book.epub)
        self.assertEqual([t for _, t in blocks(entries)], paragraphs)
        self.assertEqual([label for label, _ in nav_tree(entries)][:2], ["第1部分", "第2部分"])
        self.assertEqual(len(nav_tree(entries)), len(sizes))

    def test_a_huge_chapter_is_cut_into_files_with_one_toc_entry(self):
        lines = []
        for number in range(1, 7):
            lines.append(f"第{number}章 标题{number}")
            lines.extend(long_paragraph(i) for i in range(300 if number == 3 else 3))
        book = convert_bytes("\n".join(lines).encode("utf-8"), "huge")
        self.assertEqual((book.info["chapters"], book.info["toc_entries"], book.info["split_chapters"]), (6, 6, 1))
        sizes = self.sizes(book)
        self.assertGreater(len(sizes), 8)
        self.assertLessEqual(max(sizes), CEILING_FILE_BYTES)
        entries = read_epub(book.epub)
        self.assertEqual([t for _, t in blocks(entries)], paragraphs_of("\n".join(lines)))

    def test_a_very_long_chapter_gets_a_toc_entry_per_file(self):
        lines = ["第1章 开头", long_paragraph(0)]
        lines.append("第2章 超长")
        lines.extend(long_paragraph(i) for i in range(700))
        lines.extend(f"第{n}章 结尾{n}" if i == 0 else "尾。" for n in range(3, 7) for i in range(2))
        book = convert_bytes("\n".join(lines).encode("utf-8"), "very-long")
        entries = read_epub(book.epub)
        labels = [label for label, _ in nav_tree(entries)]
        parts = [label for label in labels if label.startswith("第2章 超长")]
        self.assertEqual(parts[0], "第2章 超长")
        self.assertGreaterEqual(len(parts), 8)
        self.assertEqual(parts[1:], [f"第2章 超长（{n}）" for n in range(2, len(parts) + 1)])
        self.assertEqual(book.info["chapters"], 6)
        self.assertEqual([t for _, t in blocks(entries)], paragraphs_of("\n".join(lines)))

    def test_one_enormous_paragraph_is_cut_at_sentence_ends(self):
        sentence = "这是一个很长很长的句子" * 30 + "。"
        text = flat_book(6) + sentence * 600 + "\n" + flat_book(1, body=("尾。",)).replace("第1章", "第7章")
        book = convert_bytes(text.encode("utf-8"), "paragraph")
        self.assertLessEqual(max(self.sizes(book)), CEILING_FILE_BYTES)
        self.assertTrue(any("split at sentence ends" in w for w in book.info["warnings"]))
        entries = read_epub(book.epub)
        pieces = [t for tag, t in blocks(entries) if tag == "p" and t.startswith("这是一个")]
        self.assertGreater(len(pieces), 3)
        self.assertTrue(all(piece.endswith("。") for piece in pieces))
        self.assertEqual("".join(pieces), sentence * 600)

    def test_an_unpunctuated_run_is_hard_cut_without_losing_text(self):
        text = flat_book(6) + "字" * 150_000 + "\n"
        book = convert_bytes(text.encode("utf-8"), "unpunctuated")
        self.assertLessEqual(max(self.sizes(book)), CEILING_FILE_BYTES)
        entries = read_epub(book.epub)
        self.assertEqual("".join(t for tag, t in blocks(entries) if t.startswith("字")), "字" * 150_000)

    def test_small_chapters_are_never_merged_or_split(self):
        book = convert_bytes(flat_book(30).encode("utf-8"), "small")
        self.assertEqual((book.info["files"], book.info["split_chapters"]), (30, 0))


class DecorationTest(unittest.TestCase):
    def test_strip_decoration_trims_only_the_edges(self):
        for line, expected in (("## 第1章 标题 ##", "第1章 标题"), ("=== 第一卷 ===", "第一卷"),
                               ("★ ※ 终章 ※ ★", "终章"), ("☆、第55章 第二朵白莲花", "第55章 第二朵白莲花"),
                               ("-第一章-", "第一章"), ("第1章 A-B*C", "第1章 A-B*C"),
                               ("第1章 ——引子", "第1章 ——引子"), ("***", "***")):
            self.assertEqual(strip_decoration(line), expected, line)

    def test_only_edge_decoration_counts_as_removed(self):
        self.assertEqual(novels._edge_removal("## 第1章 ##", "第1章"), 4)
        self.assertEqual(novels._edge_removal("☆、第1章", "第1章"), 2)
        for original, shown in (("第#1章", "第1章"), ("第1章 X", "第1章"), ("第1章、", "第1章"), ("第1章", "第2章")):
            with self.assertRaises(NovelPreservationError, msg=original):
                novels._edge_removal(original, shown)

    def test_decoration_lines_are_dropped_and_heading_decoration_is_stripped_and_counted(self):
        text = "".join(f"## {i}真的 ##\n=======\n内容{i}。\n--\n" for i in range(1, 7))
        book = convert_bytes(text.encode("utf-8"), "markdown")
        entries = read_epub(book.epub)
        pairs = blocks(entries)
        self.assertEqual([t for tag, t in pairs if tag == "h2"], [f"{i}真的" for i in range(1, 7)])
        self.assertEqual([t for tag, t in pairs if tag == "p"], [f"内容{i}。" for i in range(1, 7)])
        self.assertEqual([label for label, _ in nav_tree(entries)], [f"{i}真的" for i in range(1, 7)])
        self.assertEqual(book.info["removed_decoration_chars"], 6 * (4 + 7 + 2))
        self.assertEqual((book.info["decoration_lines_dropped"], book.info["headings_cleaned"]), (12, 6))

    def test_a_star_marker_goes_with_its_comma(self):
        text = "".join(f"☆、第{i}章 白莲花\n内容{i}。\n" for i in range(1, 7))
        book = convert_bytes(text.encode("utf-8"), "jinjiang")
        entries = read_epub(book.epub)
        self.assertEqual([t for tag, t in blocks(entries) if tag == "h2"], [f"第{i}章 白莲花" for i in range(1, 7)])
        self.assertEqual(book.info["removed_decoration_chars"], 12)

    def test_decoration_inside_text_is_kept_and_never_exempt_from_the_check(self):
        text = "".join(f"## 第{i}章 A-B ##\n--- 他说 ---\n*注：这是备注。\n---\n" for i in range(1, 7))
        book = convert_bytes(text.encode("utf-8"), "inline")
        entries = read_epub(book.epub)
        pairs = blocks(entries)
        self.assertEqual([t for tag, t in pairs if tag == "h2"], [f"第{i}章 A-B" for i in range(1, 7)])
        self.assertEqual([t for tag, t in pairs if tag == "p"][:2], ["--- 他说 ---", "*注：这是备注。"])
        normalized = normalize_text(text)[0]
        check_preservation(book.epub, normalized)
        name = next(n for n in spine_names(entries) if "--- 他说 ---".encode() in entries[n])
        lost = dict(entries)
        lost[name] = entries[name].replace("--- 他说 ---".encode(), "他说".encode())
        with self.assertRaisesRegex(NovelPreservationError, "non-whitespace character"):
            check_preservation(pack_epub(lost), normalized)
        restored = dict(entries)
        restored[name] = entries[name].replace("第1章 A-B".encode(), "## 第1章 A-B ##".encode())
        with self.assertRaises(NovelPreservationError):
            check_preservation(pack_epub(restored), normalized)

    def test_without_headings_markers_stay_text_but_decoration_lines_still_go(self):
        text = "".join(f"# 小标题{i}\n====\n内容{i}。\n" for i in range(1, 9))
        book = convert_bytes(text.encode("utf-8"), "plain")
        self.assertEqual(book.info["mode"], "size")
        texts = [t for _, t in blocks(read_epub(book.epub))]
        self.assertIn("# 小标题1", texts)
        self.assertNotIn("====", texts)
        self.assertEqual((book.info["removed_decoration_chars"], book.info["headings_cleaned"]), (32, 0))


class EchoTest(unittest.TestCase):
    PAIRS = [("第一章", "第001章"), ("第2章", "第二章"), ("第三章", "第０３章"), ("第四章", "第4章"), ("第五章", "第005章"),
             ("第6章", "第六章")]

    def convert(self, template):
        text = "".join(template.format(a=a, b=b, n=n) for n, (a, b) in enumerate(self.PAIRS, 1))
        return convert_bytes(text.encode("utf-8"), "echo")

    def test_an_echo_of_the_chapter_number_joins_the_titled_chapter(self):
        book = self.convert("## {a} 标题{n}\n==========\n{b}\n正文{n}。\n")
        entries = read_epub(book.epub)
        shown = blocks(entries)
        titles = [f"{a} 标题{n}" for n, (a, _) in enumerate(self.PAIRS, 1)]
        self.assertEqual((book.info["chapters"], book.info["files"], book.info["toc_entries"]), (6, 6, 6))
        self.assertEqual([t for tag, t in shown if tag == "h2"], titles)
        self.assertEqual([t for tag, t in shown if tag == "h3"], [b for _, b in self.PAIRS])
        self.assertEqual([t for tag, t in shown if tag == "p"], [f"正文{n}。" for n in range(1, 7)])
        self.assertEqual([tag for tag, _ in shown][:3], ["h2", "h3", "p"])
        self.assertEqual([label for label, _ in nav_tree(entries)], titles)
        self.assertEqual((book.info["removed_decoration_chars"], book.info["decoration_lines_dropped"],
                          book.info["headings_cleaned"]), (6 * (2 + 10), 6, 6))

    def test_an_echo_directly_under_its_heading_joins_it_too(self):
        book = self.convert("{a} 标题{n}\n{b}\n正文{n}。\n")
        self.assertEqual((book.info["chapters"], book.info["files"]), (6, 6))
        self.assertEqual([tag for tag, _ in blocks(read_epub(book.epub))][:3], ["h2", "h3", "p"])

    def test_a_site_prefix_before_the_number_does_not_hide_the_echo(self):
        book = self.convert("## 正文_{a} 标题{n}\n{b} 标题{n}\n正文{n}。\n")
        shown = blocks(read_epub(book.epub))
        self.assertEqual((book.info["chapters"], book.info["files"]), (6, 6))
        # The prefix is part of the shown title, so the bare echo is the same title and is dropped.
        self.assertEqual([tag for tag, _ in shown][:2], ["h2", "p"])
        self.assertEqual(book.info["duplicate_headings_dropped"], 6)
        self.assertEqual(shown[0][1], "正文_第一章 标题1")

    def test_several_echoes_stay_with_the_chapter(self):
        text = "".join(f"第{n}章 标题\n第{n}章 标题（一更）\n----\n第0{n}章（二更）\n正文{n}。\n"
                       for n in range(1, 7))
        book = convert_bytes(text.encode("utf-8"), "chain")
        self.assertEqual(book.info["chapters"], 6)
        self.assertEqual([tag for tag, _ in blocks(read_epub(book.epub))][:4], ["h2", "h3", "h3", "p"])

    def test_other_numbers_other_units_and_a_body_in_between_are_not_echoes(self):
        volumes = "".join(f"第{n}卷 风起\n第{n}章 开始\n正文{n}。\n" for n in range(1, 7))
        book = convert_bytes(volumes.encode("utf-8"), "volumes")
        self.assertEqual((book.info["volumes"], book.info["chapters"]), (6, 6))
        self.assertNotIn("h3", [tag for tag, _ in blocks(read_epub(book.epub))])
        with_body = "".join(f"第{n}章 标题\n序言{n}。\n第00{n}章 重复\n正文{n}。\n" for n in range(1, 7))
        book = convert_bytes(with_body.encode("utf-8"), "body")
        self.assertEqual(book.info["chapters"], 12)
        self.assertNotIn("h3", [tag for tag, _ in blocks(read_epub(book.epub))])

    def test_a_heading_with_only_echoes_has_no_body_and_stays_plain_text(self):
        text = "".join(f"第{n}章 标题\n正文{n}。\n第{n}章 标题（求票）\n第00{n}章 标题，到网址\n" for n in range(1, 7))
        book = convert_bytes(text.encode("utf-8"), "footer")
        shown = blocks(read_epub(book.epub))
        self.assertEqual(book.info["chapters"], 6)
        self.assertNotIn("h3", [tag for tag, _ in shown])
        self.assertEqual([t for tag, t in shown if tag == "p"][:3], ["正文1。", "第1章 标题（求票）", "第001章 标题，到网址"])

    def test_detect_headings_marks_the_echo(self):
        found = detect_headings(["## 第一章 甲", "====", "第001章", "正文"])
        self.assertEqual({index: heading.level for index, heading in found.items()}, {0: CHAPTER, 2: ECHO})


class GroupedChaptersTest(unittest.TestCase):
    def test_threshold_keeps_v9_epub_bytes_at_and_below_3000(self):
        # Golden SHA-256 values from converter v9, including its package identity.
        for count, expected in [
            (2999, "882965f3e1754172163a9343f856e0b48651065b3fd6499ec1a539ba067950ad"),
            (3000, "72cb2aa84e01017b6c71c617918f492e0bfca1419e6db718eb80114ca0cedd51"),
        ]:
            with self.subTest(chapters=count):
                book = convert_bytes(flat_book(count).encode(), "threshold")
                self.assertEqual(digest(book.epub), expected)
                self.assertEqual(book.info["files"], count)

    def test_grouping_is_bounded_and_every_chapter_has_a_unique_reachable_fragment(self):
        count = 3001
        paragraph = "这是完整正文。" * 200
        book = convert_bytes(flat_book(count, body=(paragraph,)).encode(), "grouped")
        entries = read_epub(book.epub)
        names = spine_names(entries)
        self.assertLess(len(names), 1500)
        self.assertEqual(book.info["split_chapters"], 0)
        self.assertLessEqual(max(len(entries[name]) for name in names), CEILING_FILE_BYTES)
        targets = {}
        ids = []
        for name in names:
            body = ElementTree.fromstring(entries[name]).find(f"{XHTML}body")
            self.assertEqual(len(body) % 2, 0)
            for heading, prose in zip(list(body)[::2], list(body)[1::2]):
                self.assertEqual(heading.tag, f"{XHTML}h2")
                self.assertEqual(prose.text, paragraph)  # no new chapter splits
                anchor = heading.get("id")
                self.assertTrue(anchor)
                ids.append(anchor)
                targets[f"{name.removeprefix('OEBPS/')}#{anchor}"] = heading.text
        self.assertEqual(len(ids), count)
        self.assertEqual(len(set(ids)), count)
        nav = ElementTree.fromstring(entries["OEBPS/nav.xhtml"])
        links = list(nav.iter(f"{XHTML}a"))
        self.assertEqual(len(links), count)
        self.assertEqual([targets[a.get("href")] for a in links], [f"第{i}章 标题{i}" for i in range(1, count + 1)])
        ncx = ElementTree.fromstring(entries["OEBPS/toc.ncx"])
        self.assertEqual([c.get("src") for c in ncx.iter(f"{NCX}content")], [a.get("href") for a in links])

    def test_existing_oversize_chapter_split_and_volume_hierarchy_are_preserved(self):
        text = "卷首正文。\n第一卷 风起\n卷内正文。\n" + flat_book(6)
        text += "第7章 长章\n" + "\n".join(long_paragraph(i, 400) for i in range(200))
        with mock.patch.object(novels, "GROUP_CHAPTER_THRESHOLD", 5):
            book = convert_bytes(text.encode(), "long")
        self.assertEqual(book.info["split_chapters"], 1)
        self.assertLessEqual(book.info["max_file_bytes"], CEILING_FILE_BYTES)
        entries = read_epub(book.epub)
        self.assertEqual(nav_tree(entries), ncx_tree(entries))
        self.assertEqual(nav_tree(entries)[1][0], "第一卷 风起")
        self.assertEqual(len(nav_tree(entries)[1][1]), 7 + book.info["files"] - 1)
        check_preservation(book.epub, normalize_text(text)[0])

    def test_missing_duplicate_and_mismatched_fragments_fail_preservation(self):
        text = flat_book(6)
        with mock.patch.object(novels, "GROUP_CHAPTER_THRESHOLD", 5):
            entries = read_epub(convert_bytes(text.encode(), "fragments").epub)
        for mutate, expected in [
            (lambda e: e.__setitem__("OEBPS/s00001.xhtml", e["OEBPS/s00001.xhtml"].replace(b'id="c00002"', b'id="missing"')),
             "dangling"),
            (lambda e: e.__setitem__("OEBPS/s00001.xhtml", e["OEBPS/s00001.xhtml"].replace(b'id="c00002"', b'id="c00001"')),
             "duplicate"),
            (lambda e: e.__setitem__("OEBPS/toc.ncx", e["OEBPS/toc.ncx"].replace(b"#c00002", b"#c00001")),
             "inconsistent"),
        ]:
            with self.subTest(expected=expected):
                changed = dict(entries)
                mutate(changed)
                with self.assertRaisesRegex(novels.NovelError, expected):
                    check_preservation(pack_epub(changed), text)


class PreservationTest(unittest.TestCase):
    def setUp(self):
        self.text = flat_book(8)
        self.book = convert_bytes(self.text.encode("utf-8"), "preserve")
        self.entries = read_epub(self.book.epub)
        self.normalized = normalize_text(self.text)[0]

    def tamper(self, name, old, new):
        entries = dict(self.entries)
        entries[name] = entries[name].replace(old, new)
        self.assertNotEqual(entries[name], self.entries[name])
        return pack_epub(entries)

    def test_the_converted_book_passes_an_independent_check(self):
        stats = check_preservation(self.book.epub, self.normalized)
        self.assertEqual(stats["files"], 8)

    def test_whitespace_differences_are_not_content(self):
        stretched = self.tamper("OEBPS/s00002.xhtml", "<p>这是".encode(), "<p>  　这是\n".encode())
        check_preservation(stretched, self.normalized)

    def test_a_dropped_character_fails_the_book(self):
        broken = self.tamper("OEBPS/s00003.xhtml", "又一段".encode(), "又一".encode())
        with self.assertRaisesRegex(NovelPreservationError, "non-whitespace character"):
            check_preservation(broken, self.normalized)

    def test_reordered_spine_fails_the_book(self):
        opf = self.entries["OEBPS/content.opf"].decode("utf-8")
        swapped = opf.replace('idref="s00001"', 'idref="TMP"').replace('idref="s00002"', 'idref="s00001"').replace('idref="TMP"', 'idref="s00002"')
        entries = dict(self.entries, **{"OEBPS/content.opf": swapped.encode("utf-8")})
        with self.assertRaises(NovelPreservationError):
            check_preservation(pack_epub(entries), self.normalized)

    def test_extra_text_and_malformed_xhtml_fail_the_book(self):
        extra = self.tamper("OEBPS/s00001.xhtml", b"</body>", "<p>多出来的字</p></body>".encode())
        with self.assertRaises(NovelPreservationError):
            check_preservation(extra, self.normalized)
        malformed = self.tamper("OEBPS/s00001.xhtml", b"</body>", b"<p></body>")
        with self.assertRaisesRegex(novels.NovelError, "malformed"):
            check_preservation(malformed, self.normalized)

    def test_oversized_spine_files_and_dangling_toc_fail_the_book(self):
        entries = dict(self.entries)
        entries["OEBPS/s00001.xhtml"] = entries["OEBPS/s00001.xhtml"].replace(
            b"</body>", b"<!--" + b"x" * CEILING_FILE_BYTES + b"--></body>")
        with self.assertRaisesRegex(novels.NovelError, "ceiling"):
            check_preservation(pack_epub(entries), self.normalized)
        dangling = self.tamper("OEBPS/toc.ncx", b'src="s00002.xhtml"', b'src="s99999.xhtml"')
        with self.assertRaisesRegex(novels.NovelError, "nav/NCX"):
            check_preservation(dangling, self.normalized)

    def test_a_lossy_builder_is_caught_before_anything_is_returned(self):
        real = novels.plan_pages

        def lossy(segments):
            plan = real(segments)
            plan.pages[2].blocks.pop()
            return plan

        with mock.patch.object(novels, "plan_pages", lossy):
            with self.assertRaises(NovelPreservationError):
                convert_bytes(self.text.encode("utf-8"), "lossy")

    def test_control_characters_are_counted_and_the_rest_preserved(self):
        text = flat_book(6).replace("这是正文。", "这是\x08正文\x0b。")
        book = convert_bytes(text.encode("utf-8"), "controls")
        self.assertEqual(book.info["removed_control_chars"], 6)
        self.assertTrue(any("control characters" in w for w in book.info["warnings"]))

    def test_isolated_nul_bytes_are_removed_and_counted_but_nul_heavy_files_are_refused(self):
        text = flat_book(6, body=("这是正文。",) * 100).replace("这是正文。", "这是正文。\x00", 1)
        book = convert_bytes(text.encode("utf-8"), "nul")
        self.assertEqual(book.info["removed_control_chars"], 1)
        with self.assertRaisesRegex(NovelDecodeError, "NUL"):
            convert_bytes(("a\x00" * 1000).encode("utf-8"), "binary")


def write_library(src: Path) -> None:
    (src / "sub").mkdir(parents=True)
    (src / ".qidian-backup").mkdir()
    (src / "[作者]第一本.txt").write_bytes(flat_book(8).encode("utf-8"))
    (src / "sub" / "第二本.txt").write_bytes(flat_book(8).encode("gb18030"))
    (src / "没有章节.txt").write_text("\n".join(long_paragraph(i) for i in range(120)), encoding="utf-8")
    (src / "bad.txt").write_bytes(b"\x81\x20" * 200)
    (src / ".hidden.txt").write_bytes(flat_book(8).encode("utf-8"))
    (src / ".qidian-backup" / "backup.txt").write_bytes(flat_book(8).encode("utf-8"))
    (src / "notes.md").write_text("not a novel")
    (src / "Existing.epub").write_bytes(b"already an epub")


def snapshot(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): (p.read_bytes() if p.is_file() else None, p.stat().st_mtime_ns)
            for p in sorted(root.rglob("*"))}


class TreeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.src = self.root / "src"
        self.out = self.root / "out"
        write_library(self.src)

    def books(self, report):
        return {b["source"]: b for b in report["books"]}

    def test_first_run_converts_good_books_and_reports_failures(self):
        before = snapshot(self.src)
        report = convert_tree(self.src, self.out)
        summary = report["summary"]
        self.assertEqual((summary["total"], summary["converted"], summary["skipped"], summary["failed"]), (4, 3, 0, 1))
        self.assertEqual((summary["headings_mode"], summary["size_mode"]), (2, 1))
        self.assertLessEqual(summary["xhtml_max_bytes"], CEILING_FILE_BYTES)
        books = self.books(report)
        self.assertEqual(sorted(books), sorted(["bad.txt", "sub/第二本.txt", "[作者]第一本.txt", "没有章节.txt"]))
        self.assertIn("Undecodable", books["bad.txt"]["error"])
        self.assertEqual(books["sub/第二本.txt"]["encoding"], "gb18030")
        self.assertEqual(books["[作者]第一本.txt"]["chapters"], 8)
        self.assertTrue((self.out / "[作者]第一本.epub").is_file())
        self.assertTrue((self.out / "sub" / "第二本.epub").is_file())
        self.assertFalse((self.out / "bad.epub").exists())
        self.assertFalse(any(".hidden" in p.name or "backup" in p.name for p in self.out.rglob("*")))
        self.assertEqual(snapshot(self.src), before)
        manifest = json.loads((self.out / novels.MANIFEST_NAME).read_text("utf-8"))
        self.assertEqual(manifest["entries"]["bad.txt"]["status"], "failed")
        self.assertEqual(manifest["entries"]["sub/第二本.txt"]["status"], "ok")
        self.assertEqual(manifest["entries"]["[作者]第一本.txt"]["source_sha256"],
                         digest((self.src / "[作者]第一本.txt").read_bytes()))
        self.assertTrue((self.out / novels.REPORT_NAME).is_file())

    def test_second_run_is_idempotent_and_retries_only_failures(self):
        convert_tree(self.src, self.out)
        first = snapshot(self.out)
        report = convert_tree(self.src, self.out)
        self.assertEqual((report["summary"]["converted"], report["summary"]["skipped"], report["summary"]["failed"]), (0, 3, 1))
        second = snapshot(self.out)
        for name in ("[作者]第一本.epub", "sub/第二本.epub", "没有章节.epub"):
            self.assertEqual(first[name], second[name])

    def test_v9_migration_reconverts_only_books_above_the_chapter_threshold(self):
        (self.src / "threshold.txt").write_text(flat_book(3000), encoding="utf-8")
        (self.src / "grouped.txt").write_text(flat_book(3001), encoding="utf-8")
        with mock.patch.object(novels, "CONVERTER_VERSION", 9), mock.patch.object(novels, "GROUP_CHAPTER_THRESHOLD", 10000):
            convert_tree(self.src, self.out)
        before = snapshot(self.out)
        report = convert_tree(self.src, self.out)
        books = self.books(report)
        self.assertEqual(report["summary"]["converted"], 1)
        self.assertEqual(books["grouped.txt"]["status"], "converted")
        self.assertEqual(books["grouped.txt"]["converter_version"], 10)
        self.assertLess(books["grouped.txt"]["files"], 1500)
        self.assertEqual(books["threshold.txt"]["status"], "skipped")
        self.assertEqual(books["threshold.txt"]["converter_version"], 9)
        after = snapshot(self.out)
        for name in ("threshold.epub", "[作者]第一本.epub", "sub/第二本.epub", "没有章节.epub"):
            self.assertEqual(after[name], before[name])

    def test_the_manifest_and_report_record_removed_decoration(self):
        text = "".join(f"## {i}真的 ##\n=======\n内容{i}。\n" for i in range(1, 7))
        (self.src / "装饰.txt").write_text(text, encoding="utf-8")
        report = convert_tree(self.src, self.out)
        book = self.books(report)["装饰.txt"]
        self.assertEqual((book["removed_decoration_chars"], book["decoration_lines_dropped"], book["headings_cleaned"]),
                         (66, 6, 6))
        self.assertEqual((report["summary"]["removed_decoration_chars"], report["summary"]["decoration_lines_dropped"],
                          report["summary"]["headings_cleaned"]), (66, 6, 6))
        manifest = json.loads((self.out / novels.MANIFEST_NAME).read_text("utf-8"))
        self.assertEqual(manifest["entries"]["装饰.txt"]["removed_decoration_chars"], 66)
        again = convert_tree(self.src, self.out)
        self.assertEqual(again["summary"]["skipped"], 4)
        self.assertEqual(again["summary"]["removed_decoration_chars"], 66)

    def test_changed_sources_are_reconverted_and_force_rebuilds_everything(self):
        first = convert_tree(self.src, self.out)
        old_hash = {b["source"]: b.get("output_sha256") for b in first["books"]}
        (self.src / "sub" / "第二本.txt").write_bytes(flat_book(9).encode("utf-8"))
        report = convert_tree(self.src, self.out)
        self.assertEqual(report["summary"]["converted"], 1)
        self.assertEqual(self.books(report)["sub/第二本.txt"]["chapters"], 9)
        forced = convert_tree(self.src, self.out, force=True)
        self.assertEqual((forced["summary"]["converted"], forced["summary"]["skipped"]), (3, 0))
        self.assertEqual(self.books(forced)["[作者]第一本.txt"]["output_sha256"], old_hash["[作者]第一本.txt"])

    def test_a_missing_output_is_rebuilt_and_an_orphan_is_reported_not_deleted(self):
        convert_tree(self.src, self.out)
        (self.out / "没有章节.epub").unlink()
        (self.src / "sub" / "第二本.txt").unlink()
        report = convert_tree(self.src, self.out)
        self.assertEqual((report["summary"]["converted"], report["summary"]["skipped"]), (1, 1))
        self.assertTrue((self.out / "没有章节.epub").is_file())
        self.assertEqual(report["summary"]["orphaned"], ["sub/第二本.txt"])
        self.assertTrue((self.out / "sub" / "第二本.epub").is_file())

    def test_parallel_conversion_matches_inline_conversion(self):
        inline = convert_tree(self.src, self.out, jobs=1)
        parallel = convert_tree(self.src, self.root / "out2", jobs=2)
        self.assertEqual({k: b.get("output_sha256") for k, b in self.books(inline).items()},
                         {k: b.get("output_sha256") for k, b in self.books(parallel).items()})
        self.assertEqual(parallel["summary"]["converted"], 3)

    def test_the_output_directory_may_not_be_the_source_or_inside_it(self):
        before = snapshot(self.src)
        for target in (self.src, self.src / "converted", self.src / "sub" / "deeper"):
            with self.assertRaisesRegex(ReaderError, "outside the source"):
                convert_tree(self.src, target)
        self.assertEqual(snapshot(self.src), before)
        with self.assertRaisesRegex(ReaderError, "not found"):
            convert_tree(self.root / "missing", self.out)

    def test_an_invalid_manifest_stops_the_run_unless_forced(self):
        self.out.mkdir()
        (self.out / novels.MANIFEST_NAME).write_text("{not json")
        with self.assertRaisesRegex(ReaderError, "manifest is invalid"):
            convert_tree(self.src, self.out)
        self.assertEqual(convert_tree(self.src, self.out, force=True)["summary"]["converted"], 3)

    def test_colliding_output_names_fail_the_second_book_only(self):
        (self.src / "同名?.txt").write_bytes(flat_book(6).encode("utf-8"))
        (self.src / "同名？.txt").write_bytes(flat_book(7).encode("utf-8"))
        report = convert_tree(self.src, self.out)
        collided = [b for b in report["books"] if "collides" in b.get("error", "")]
        self.assertEqual(len(collided), 1)
        self.assertEqual(report["summary"]["converted"], 4)

    def test_cli_reports_summary_and_failures_and_never_writes_into_the_source(self):
        before = snapshot(self.src)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = cli.main(["novels", "convert", str(self.src), str(self.out), "--jobs", "1"])
        self.assertEqual(code, 2)
        result = json.loads(stdout.getvalue())
        self.assertEqual(result["summary"]["converted"], 3)
        self.assertEqual([f["source"] for f in result["failures"]], ["bad.txt"])
        self.assertIn("failed: bad.txt", stderr.getvalue())
        self.assertIn("converted: sub/第二本.txt", stderr.getvalue())
        self.assertEqual(snapshot(self.src), before)
        (self.src / "bad.txt").unlink()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cli.main(["novels", "convert", str(self.src), str(self.out), "--jobs", "1"]), 0)


class CardNovelsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.src = self.root / "src"
        self.out = self.root / "out"
        self.card = self.root / "card"
        self.card.mkdir()
        write_library(self.src)
        (self.src / "bad.txt").unlink()
        convert_tree(self.src, self.out)
        self.volume = CardVolume(self.card, "disk9s1", UUID)
        self.cache = Cache(self.root / "cache")
        self.config = {"version": 1, "volume_uuid": UUID, "annotations_dir": str(self.root / "annotations"),
                       "firmware_path": str(self.root / "reviewed.bin"), "firmware_sha256": "0" * 64,
                       "launchers_dir": str(self.root / "launchers")}

    def sync(self):
        return sync_card(self.cache, self.config, volume=self.volume, novels_dir=self.out)

    def test_copies_converted_books_with_a_library_marker_and_verifies_them(self):
        result = self.sync()["novels"]
        self.assertEqual((result["planned"], result["copied"], result["unchanged"], result["errors"]), (3, 3, 0, []))
        destination = self.card / card.NOVELS_DIR
        for name in ("[作者]第一本.epub", "sub/第二本.epub", "没有章节.epub"):
            self.assertEqual((destination / name).read_bytes(), (self.out / name).read_bytes())
        self.assertTrue((self.card / card.LIBRARY_DIRTY).is_file())
        self.assertEqual(result["library_dirty_marker"], str(self.card / card.LIBRARY_DIRTY))
        self.assertFalse(any(p.name.startswith(".reader-send-") for p in self.card.rglob("*")))
        self.assertFalse(any(p.suffix == ".txt" for p in self.card.rglob("*")))

    def test_rerun_leaves_existing_files_alone(self):
        self.sync()
        (self.card / card.LIBRARY_DIRTY).unlink()
        before = snapshot(self.card)
        result = self.sync()["novels"]
        self.assertEqual((result["planned"], result["copied"], result["unchanged"]), (0, 0, 3))
        self.assertEqual(snapshot(self.card), before)
        self.assertFalse((self.card / card.LIBRARY_DIRTY).exists())

    def test_different_bytes_on_the_card_are_never_overwritten_or_deleted(self):
        library = self.card / card.NOVELS_DIR
        library.mkdir(parents=True)
        (library / "没有章节.epub").write_bytes(b"robert's own file")
        (library / "unrelated.epub").write_bytes(b"keep me")
        inbox = self.card / card.BOOK_INBOX
        inbox.mkdir(parents=True)
        (inbox / "article.epub").write_bytes(b"inbox book")
        result = self.sync()["novels"]
        self.assertEqual((result["copied"], result["errors"]), (3, []))
        self.assertEqual((library / "没有章节.epub").read_bytes(), b"robert's own file")
        alternate = [p for p in library.glob("没有章节-*.epub")]
        self.assertEqual(len(alternate), 1)
        self.assertEqual(alternate[0].read_bytes(), (self.out / "没有章节.epub").read_bytes())
        self.assertEqual((library / "unrelated.epub").read_bytes(), b"keep me")
        self.assertEqual((inbox / "article.epub").read_bytes(), b"inbox book")
        again = self.sync()["novels"]
        self.assertEqual((again["copied"], again["unchanged"]), (0, 3))

    def test_nothing_is_written_when_the_converted_folder_has_no_manifest(self):
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaisesRegex(ReaderError, "No novel manifest"):
            sync_card(self.cache, self.config, volume=self.volume, novels_dir=empty)
        self.assertEqual(list(self.card.iterdir()), [])
        self.assertFalse((self.root / "annotations").exists())

    def test_outputs_that_no_longer_match_the_manifest_are_excluded_and_reported(self):
        target = self.out / "sub" / "第二本.epub"
        target.write_bytes(target.read_bytes() + b"tampered")
        result = self.sync()["novels"]
        self.assertEqual(result["copied"], 2)
        self.assertEqual(len(result["errors"]), 1)
        self.assertIn("no longer matches", result["errors"][0])
        self.assertFalse((self.card / card.NOVELS_DIR / "sub" / "第二本.epub").exists())

    def test_a_readback_mismatch_removes_the_bad_copy_and_reports_it(self):
        real = card._copy_atomic

        def corrupting(source, destination):
            real(source, destination)
            if destination.name == "没有章节.epub":
                destination.write_bytes(b"corrupted on the card")

        with mock.patch.object(card, "_copy_atomic", corrupting):
            result = self.sync()
        novels_result = result["novels"]
        self.assertEqual(novels_result["copied"], 2)
        self.assertEqual(len(novels_result["errors"]), 1)
        self.assertIn("readback", novels_result["errors"][0])
        self.assertFalse((self.card / card.NOVELS_DIR / "没有章节.epub").exists())
        self.assertTrue((self.card / card.LIBRARY_DIRTY).is_file())
        healed = self.sync()["novels"]
        self.assertEqual((healed["copied"], healed["unchanged"], healed["errors"]), (1, 2, []))

    def test_a_symlinked_novels_folder_is_refused_before_any_write(self):
        (self.card / "books").mkdir()
        elsewhere = self.root / "elsewhere"
        elsewhere.mkdir()
        os.symlink(elsewhere, self.card / card.NOVELS_DIR)
        with self.assertRaisesRegex(ReaderError, "not a normal directory"):
            self.sync()
        self.assertEqual(list(elsewhere.iterdir()), [])
        self.assertFalse((self.card / card.LIBRARY_DIRTY).exists())

    def test_sync_without_novels_has_no_novels_section(self):
        self.assertNotIn("novels", sync_card(self.cache, self.config, volume=self.volume))

    def test_cli_parses_the_novels_option(self):
        args = cli.parser().parse_args(["card", "sync", "--novels", str(self.out)])
        self.assertEqual(args.novels, self.out)
        self.assertIsNone(cli.parser().parse_args(["card", "sync"]).novels)


if __name__ == "__main__":
    unittest.main()
