import io
import json
import random
import re
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from reader_send import story_graph as sg
from reader_send.common import ReaderError
from reader_send.concept_index import fallback_encode, fnv1a64


def h(text: str) -> int:
    return fnv1a64(text.encode("utf-8"))


def make_epub(title: str, chapters: list[str]) -> bytes:
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml",
                         '<?xml version="1.0"?><container><rootfiles>'
                         '<rootfile full-path="OEBPS/content.opf"/></rootfiles></container>')
        items = "".join(f'<item id="c{index}" href="text/c{index}.xhtml"/>' for index in range(len(chapters)))
        refs = "".join(f'<itemref idref="c{index}"/>' for index in range(len(chapters)))
        archive.writestr("OEBPS/content.opf", f"<package><metadata><dc:title>{title}</dc:title></metadata>"
                                              f"<manifest>{items}</manifest><spine>{refs}</spine></package>")
        for index, body in enumerate(chapters):
            paragraphs = "".join(f"<p>{line}</p>\n" for line in body.split("\n"))
            archive.writestr(f"OEBPS/text/c{index}.xhtml",
                             f"<html><head><title>x</title><style>p {{}}</style></head><body>{paragraphs}</body></html>")
    return stream.getvalue()


def event(cutoff, page, gloss="", slot=sg.SLOT_ARC, vec=None):
    return {"cutoff": cutoff, "chapter_or_page": page, "slot_hint": slot, "salience_hint": 40, "gloss": gloss,
            "target": "Wu Tan City", "vec": vec or [0] * 128}


C1, C2, C3 = sg.epub_cutoff(0, 0), sg.epub_cutoff(1, 640), sg.epub_cutoff(2, 6400)
BOOK = "/books/Novels/doupo.epub"


def sample_graph() -> bytes:
    entities = [
        {"term": "萧炎", "book_path": BOOK, "kind": sg.KIND_CHARACTER, "name": "Xiao Yan (Dou Qi disciple)",
         "events": [event(C3, 30, "clashed with Nalan Yanran (Ch.30)"), event(C1, 1, "introduced at the test (Ch.1)",
                                                                              sg.SLOT_ORIGIN, [5] * 128)]},
        {"term": "乌坦城", "book_path": BOOK, "kind": sg.KIND_LOCATION, "name": "Wu Tan City",
         "events": [event(C1, 1, "home of the Xiao clan")]},
        {"term": "Safed", "kind": sg.KIND_LOCATION, "name": "Safed (Kabbalist center)", "first_cutoff": 0,
         "events": [event(0, 1, "Luria taught here", sg.SLOT_CROSS_BOOK)]},
        {"term": "纳兰嫣然", "book_path": BOOK, "kind": sg.KIND_CHARACTER, "name": "Nalan Yanran",
         "events": [event(C2, 10, "came to break the engagement"), event(C3, 30, "lost the duel")]},
    ]
    aliases = [{"alias": "炎帝", "canonical": "萧炎", "reveal_cutoff": C3},
               {"alias": "Tzfat", "canonical": "Safed", "reveal_cutoff": 0}]
    return sg.build_story_graph(entities, aliases)


class LayoutTest(unittest.TestCase):
    def test_record_sizes_and_cutoffs(self):
        self.assertEqual((sg.HEADER.size, sg.ENTITY.size, sg.EVENT.size, sg.ALIAS.size), (32, 64, 256, 24))
        self.assertEqual(sg.epub_cutoff(0, 0), 0x10000)
        self.assertEqual(sg.epub_cutoff(3, 64 * 5 + 63), (4 << 16) | 5)
        self.assertEqual(sg.epub_cutoff(1, 1 << 30), (2 << 16) | 0xFFFF)
        self.assertEqual(sg.txt_cutoff(0), 1 << 16)
        self.assertEqual(sg.txt_cutoff(5 * 1024 + 1023), (1 << 16) | 5)
        self.assertEqual(sg.txt_cutoff(1 << 40), (1 << 16) | 0xFFFF)
        with self.assertRaises(ReaderError):
            sg.epub_cutoff(-1, 0)

    def test_sections_are_sector_aligned_with_exact_fields(self):
        data = sample_graph()
        self.assertEqual(sg.verify_story_graph(data), [])
        magic, version, flags, entities, events, aliases, e_off, v_off, a_off = struct.unpack_from("<4sHHIIIIII", data)
        self.assertEqual((magic, version, flags, entities, events, aliases), (b"SGRF", 1, 0, 4, 6, 2))
        # 4 * 64 = 256 -> events at 1024; 6 * 256 = 1536 -> aliases at 2560; 2 * 24 -> file ends at 3072.
        self.assertEqual((e_off, v_off, a_off, len(data)), (512, 1024, 2560, 3072))
        self.assertEqual(data[32:512], bytes(480))
        for offset in (e_off, v_off, a_off, len(data)):
            self.assertEqual(offset % 512, 0)

    def test_entity_event_and_alias_bytes(self):
        data = sample_graph()
        graph = sg.read_story_graph(data)
        index = next(i for i, entity in enumerate(graph["entities"]) if entity["term_hash"] == h("萧炎"))
        raw = data[512 + 64 * index:512 + 64 * (index + 1)]
        term, book, first, start, count, kind, flags = struct.unpack_from("<QQIIHBB", raw)
        self.assertEqual((term, book, first, count, kind, flags), (h("萧炎"), h(BOOK), C1, 2, 1, 0))
        self.assertEqual(raw[28:60], b"Xiao Yan (Dou Qi disciple)".ljust(32, b"\0"))
        self.assertEqual(raw[60:64], bytes(4))
        first_event = data[1024 + 256 * start:1024 + 256 * (start + 1)]
        self.assertEqual(struct.unpack_from("<IHBB", first_event), (C1, 1, sg.SLOT_ORIGIN, 40))  # sorted by cutoff
        self.assertEqual(first_event[8:72], b"introduced at the test (Ch.1)".ljust(64, b"\0"))
        self.assertEqual(first_event[72:128], b"Wu Tan City".ljust(56, b"\0"))
        self.assertEqual(first_event[128:256], bytes([5]) * 128)
        aliases = [struct.unpack_from("<QQII", data, 2560 + 24 * i) for i in range(2)]
        self.assertEqual(sorted(aliases), aliases)
        self.assertIn((h("炎帝"), h("萧炎"), C3, 0), aliases)

    def test_long_utf8_strings_are_cut_on_a_codepoint_boundary(self):
        data = sg.build_story_graph([{"term": "x", "name": "炎" * 20, "events": [event(1, 1, "帝" * 40)]}])
        entity = sg.read_story_graph(data)["entities"][0]
        self.assertEqual(entity["name"], "炎" * 10)  # 30 bytes; an 11th would need 33 > 31
        self.assertEqual(entity["events"][0]["gloss"], "帝" * 21)  # 63 bytes

    def test_read_then_build_is_byte_identical(self):
        data = sample_graph()
        graph = sg.read_story_graph(data)
        aliases = [{"alias_hash": a["alias_hash"], "canonical_hash": a["canonical_hash"],
                    "reveal_cutoff": a["reveal_cutoff"]} for a in graph["aliases"]]
        self.assertEqual(sg.build_story_graph(graph["entities"], aliases), data)

    def test_an_empty_graph_is_one_sector(self):
        data = sg.build_story_graph([], [])
        self.assertEqual(len(data), 512)
        self.assertEqual(struct.unpack_from("<IIIIII", data, 8), (0, 0, 0, 512, 512, 512))
        self.assertEqual(sg.verify_story_graph(data), [])

    def test_duplicate_entities_and_bad_vectors_are_rejected(self):
        with self.assertRaisesRegex(ReaderError, "duplicate"):
            sg.build_story_graph([{"term": "a"}, {"term": "a", "book_path": None}])
        with self.assertRaisesRegex(ReaderError, "int8"):
            sg.build_story_graph([{"term": "a", "events": [event(1, 1, vec=[200] * 128)]}])

    def test_the_verifier_catches_broken_order_and_gates(self):
        data = bytearray(sample_graph())
        first, second = bytes(data[512:576]), bytes(data[576:640])
        data[512:576], data[576:640] = second, first
        self.assertTrue(any("strictly sorted" in problem for problem in sg.verify_story_graph(bytes(data))))
        late_gate = sg.build_story_graph([{"term": "a", "first_cutoff": C3, "events": [event(C1, 1)]}])
        self.assertTrue(any("precedes the spoiler gate" in p for p in sg.verify_story_graph(late_gate)))
        orphan = sg.build_story_graph([{"term": "a"}], [{"alias": "b", "canonical": "zzz", "reveal_cutoff": 0}])
        self.assertTrue(any("canonical" in p for p in sg.verify_story_graph(orphan)))
        self.assertEqual(sg.verify_story_graph(b"SGRF"), ["story-graph.idx is shorter than its 512-byte header"])
        with self.assertRaisesRegex(ReaderError, "failed verification"):
            with tempfile.TemporaryDirectory() as directory:
                sg.build_story_graph_index([{"term": "a", "first_cutoff": C3, "events": [event(C1, 1)]}], [],
                                           Path(directory) / "x.idx")


class BinarySearchTest(unittest.TestCase):
    def test_every_entity_and_alias_is_found_among_many(self):
        generator = random.Random(3)
        terms = [f"term-{generator.randrange(10 ** 9)}" for _ in range(300)]
        books = ["/books/Books/a.epub", "/books/Books/b.epub", None]
        entities = [{"term": term, "book_path": books[i % 3], "events": [event(C1 + i, 1)]} for i, term in enumerate(terms)]
        aliases = [{"alias": f"alias-{term}", "canonical": term, "reveal_cutoff": 0} for term in terms[:100]]
        data = sg.build_story_graph(entities, aliases)
        self.assertEqual(sg.verify_story_graph(data), [])
        header = sg._header(data)
        keys = [struct.unpack_from("<QQ", data, 512 + 64 * i) for i in range(len(terms))]
        self.assertEqual(keys, sorted(keys))
        for i, term in enumerate(terms):
            index = sg._find_entity(data, header, h(term), h(books[i % 3]) if books[i % 3] else 0)
            self.assertEqual(sg._entity_at(data, header, index)["term_hash"], h(term))
            self.assertIsNone(sg._find_entity(data, header, h(term) ^ 1, 0))
        for term in terms[:100]:
            self.assertEqual(sg._resolve_alias(data, header, h(f"alias-{term}"), C1), (h(term), False))
        self.assertEqual(sg._resolve_alias(data, header, h("nope"), C1), (None, False))

    def test_book_records_win_over_the_wildcard(self):
        data = sg.build_story_graph([
            {"term": "Luria", "book_path": "/books/Books/scholem.epub", "name": "Luria (Scholem)",
             "events": [event(C2, 2)]},
            {"term": "Luria", "name": "Luria (cross-book)", "first_cutoff": 0, "events": [event(0, 1)]}])
        self.assertEqual(sg.query_story_graph(data, "Luria", "/books/Books/scholem.epub", C1)["status"],
                         "not_yet_introduced")
        other = sg.query_story_graph(data, "Luria", "/books/Books/kaplan.epub", C1)
        self.assertEqual((other["status"], other["entity"]["name"]), ("found", "Luria (cross-book)"))
        self.assertEqual(sg.query_story_graph(data, "Luria", None, C1)["entity"]["name"], "Luria (cross-book)")


class SpoilerGateTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "story-graph.idx"
        result = sg.build_story_graph_index(sg.read_story_graph(sample_graph())["entities"],
                                            [{"alias": "炎帝", "canonical": "萧炎", "reveal_cutoff": C3},
                                             {"alias": "Tzfat", "canonical": "Safed", "reveal_cutoff": 0}], self.path)
        self.assertEqual((result["entities"], result["events"], result["aliases"], result["updated"]), (4, 6, 2, True))
        self.assertEqual(self.path.read_bytes(), sample_graph())

    def query(self, term, cutoff, book=BOOK):
        return sg.query_story_graph(self.path, term, book, cutoff)

    def test_an_entity_is_hidden_until_its_first_cutoff(self):
        self.assertEqual(self.query("纳兰嫣然", C1)["status"], "not_yet_introduced")
        self.assertEqual(self.query("纳兰嫣然", C1)["events"], [])
        self.assertEqual(self.query("纳兰嫣然", C2 - 1)["status"], "not_yet_introduced")
        found = self.query("纳兰嫣然", C2)
        self.assertEqual((found["status"], [e["cutoff"] for e in found["events"]]), ("found", [C2]))
        self.assertEqual(self.query("纳兰嫣然", 0)["status"], "found")  # 0 = no gate
        self.assertEqual(self.query("nobody", C3)["status"], "not_found")

    def test_events_are_filtered_to_the_reading_position(self):
        self.assertEqual([e["gloss"] for e in self.query("萧炎", C1)["events"]], ["introduced at the test (Ch.1)"])
        self.assertEqual([e["gloss"] for e in self.query("萧炎", C3 - 1)["events"]], ["introduced at the test (Ch.1)"])
        self.assertEqual([e["cutoff"] for e in self.query("萧炎", C3)["events"]], [C1, C3])
        self.assertEqual([e["cutoff"] for e in self.query("萧炎", 0)["events"]], [C1, C3])

    def test_an_alias_resolves_only_after_its_reveal(self):
        hidden = self.query("炎帝", C2)
        self.assertEqual((hidden["status"], hidden["alias"], hidden["lookup_hash"]), ("not_found", "not_revealed", h("炎帝")))
        revealed = self.query("炎帝", C3)
        self.assertEqual((revealed["status"], revealed["alias"]), ("found", "revealed"))
        self.assertEqual(revealed["entity"]["name"], "Xiao Yan (Dou Qi disciple)")
        self.assertEqual(self.query("炎帝", 0)["alias"], "revealed")
        always = self.query("Tzfat", C1, "/books/Books/other.epub")
        self.assertEqual((always["status"], always["entity"]["name"]), ("found", "Safed (Kabbalist center)"))

    def test_an_unchanged_rebuild_does_not_rewrite(self):
        before = self.path.stat().st_mtime_ns
        result = sg.build_story_graph_index(sg.read_story_graph(sample_graph())["entities"],
                                            [{"alias": "炎帝", "canonical": "萧炎", "reveal_cutoff": C3},
                                             {"alias": "Tzfat", "canonical": "Safed", "reveal_cutoff": 0}], self.path)
        self.assertFalse(result["updated"])
        self.assertEqual(self.path.stat().st_mtime_ns, before)


ENGLISH = [
    "Preface\nThis book is about mystical movements in early modern Europe.",
    "The movement began quietly. Later the teacher Isaac Luria arrived in Safed and gathered students.\n"
    "In Safed the students listened. Isaac Luria said the vessels had shattered.",
    "Isaac Luria taught at night in Safed. The doctrine of tzimtzum came from Isaac Luria said his students.\n"
    "Pilgrims came to Safed. Then Isaac Luria died young.",
]

CHINESE = [
    "萧炎站在乌坦城的广场上。" + "萧炎笑道：“走吧。”" * 3,
    "纳兰嫣然来到萧家。萧炎冷冷道：“三年之约。”纳兰嫣然道：“好。”" + "纳兰嫣然说：“我等你。”" * 3,
    "一个老者说他就是炎帝。萧炎点头道：“原来如此。”" + "萧炎道：“炎帝之名。”" * 2,
]


class ExtractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def book(self, name, title, chapters):
        path = self.root / name
        path.write_bytes(make_epub(title, chapters))
        return path

    def assert_spoiler_safe(self, path, book):
        _, texts = sg.epub_spine_texts(path)
        for entity in book["entities"]:
            self.assertEqual(entity["first_cutoff"], entity["events"][0]["cutoff"])
            for item in entity["events"]:
                spine, low = (item["cutoff"] >> 16) - 1, item["cutoff"] & 0xFFFF
                shown = re.sub(r"\s+", " ", texts[spine][:low << 6])
                self.assertIn(item["gloss"], shown)  # every gloss character is at or before the cutoff
                self.assertIn(entity["term"], shown)

    def test_visible_text_counts_body_character_data_like_the_firmware(self):
        markup = ('<?xml version="1.0"?><html><head><title>T</title><style>p{}</style></head>'
                  '<body><!-- c --><p>A&amp;B</p>\n<p>caf&#233; <b>x</b></p><script>no()</script></body></html>')
        self.assertEqual(sg.visible_text(markup), "A&B\ncafé x")
        path = self.book("t.epub", "T", ["one", "two"])
        self.assertEqual(sg.epub_spine_texts(path), ("T", ["one\n", "two\n"]))
        with self.assertRaisesRegex(ReaderError, "unreadable EPUB"):
            (self.root / "bad.epub").write_bytes(b"nope")
            sg.epub_spine_texts(self.root / "bad.epub")

    def test_english_names_are_found_and_gated_at_their_first_chapter(self):
        path = self.book("luria.epub", "Major Trends", ENGLISH)
        book = sg.extract_book_story_entities(path, "/books/Books/luria.epub", min_mentions=3)
        found = {entity["term"]: entity for entity in book["entities"]}
        self.assertEqual(book["title"], "Major Trends")
        self.assertIn("Isaac Luria", found)
        self.assertIn("Safed", found)
        self.assertNotIn("Isaac", found)  # folded into the full name
        luria = found["Isaac Luria"]
        self.assertEqual(luria["kind"], sg.KIND_CHARACTER)
        self.assertEqual(found["Safed"]["kind"], sg.KIND_LOCATION)
        self.assertEqual(luria["first_cutoff"] >> 16, 2)  # spine 1
        self.assertEqual([e["chapter_or_page"] for e in luria["events"]], [2, 3])
        self.assertEqual([e["slot_hint"] for e in luria["events"]], [sg.SLOT_ORIGIN, sg.SLOT_ROLE])
        self.assertIn("Isaac Luria", luria["events"][0]["gloss"])
        self.assertEqual(luria["events"][1]["gloss"], "Isaac Luria taught at night in Safed.")
        self.assertEqual(luria["events"][1]["target"], "Safed")  # shown in the gloss, so not ahead of the reader
        self.assertEqual(found["Safed"]["events"][0]["target"], "Isaac Luria")
        self.assert_spoiler_safe(path, book)
        data = sg.build_story_graph(book["entities"])
        self.assertEqual(sg.query_story_graph(data, "Isaac Luria", "/books/Books/luria.epub", sg.epub_cutoff(0, 999))
                         ["status"], "not_yet_introduced")
        self.assertEqual(sg.query_story_graph(data, "Isaac Luria", "/books/Books/luria.epub", sg.epub_cutoff(1, 9999))
                         ["status"], "found")

    def test_chinese_names_and_a_late_alias_reveal(self):
        path = self.book("doupo.epub", "斗破苍穹", CHINESE)
        book = sg.extract_book_story_entities(path, BOOK, min_mentions=3,
                                              aliases=[{"alias": "炎帝", "canonical": "萧炎"}])
        found = {entity["term"]: entity for entity in book["entities"]}
        self.assertEqual(set(found), {"萧炎", "纳兰嫣然"})
        self.assertEqual(found["萧炎"]["kind"], sg.KIND_CHARACTER)
        self.assertEqual(found["萧炎"]["first_cutoff"] >> 16, 1)
        self.assertEqual(found["纳兰嫣然"]["first_cutoff"] >> 16, 2)
        self.assertEqual(found["萧炎"]["events"][1]["target"], "纳兰嫣然")
        self.assert_spoiler_safe(path, book)
        self.assertEqual(len(book["aliases"]), 1)
        reveal = book["aliases"][0]["reveal_cutoff"]
        self.assertEqual(reveal >> 16, 3)
        data = sg.build_story_graph(book["entities"], book["aliases"])
        self.assertEqual(sg.verify_story_graph(data), [])
        self.assertEqual(sg.query_story_graph(data, "炎帝", BOOK, sg.epub_cutoff(1, 0))["status"], "not_found")
        self.assertEqual(sg.query_story_graph(data, "炎帝", BOOK, reveal)["entity"]["term_hash"], h("萧炎"))

    def test_given_terms_and_the_encoder_are_used(self):
        path = self.book("luria.epub", "Major Trends", ENGLISH)
        calls = []

        def encode(text):
            calls.append(text)
            return fallback_encode(text)

        book = sg.extract_book_story_entities(path, "/books/Books/luria.epub", encode=encode,
                                              terms=[{"term": "tzimtzum", "kind": sg.KIND_CONCEPT,
                                                      "name": "tzimtzum (contraction)"}])
        (entity,) = book["entities"]
        self.assertEqual((entity["name"], entity["kind"], len(entity["events"])), ("tzimtzum (contraction)", 4, 1))
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0].endswith(entity["events"][0]["gloss"].encode("utf-8")))
        self.assertEqual(entity["events"][0]["vec"], fallback_encode(calls[0]))

    def test_cross_book_terms_get_a_wildcard_entry(self):
        one = sg.extract_book_story_entities(self.book("a.epub", "Book A", ENGLISH), "/books/Books/a.epub",
                                             min_mentions=3)
        two = sg.extract_book_story_entities(self.book("b.epub", "Book B", ENGLISH[1:]), "/books/Books/b.epub",
                                             min_mentions=3)
        wildcard = {entity["term_hash"]: entity for entity in sg.cross_book_entities([one, two])}
        luria = wildcard[h("Isaac Luria")]
        self.assertEqual((luria["book_path_hash"], luria["first_cutoff"]), (0, 0))
        self.assertEqual([e["target"] for e in luria["events"]], ["Book A Ch.2", "Book B Ch.1"])
        data = sg.build_story_graph(one["entities"] + two["entities"] + list(wildcard.values()))
        elsewhere = sg.query_story_graph(data, "Isaac Luria", "/books/Books/c.epub", sg.epub_cutoff(0, 0))
        self.assertEqual((elsewhere["status"], len(elsewhere["events"])), ("found", 2))
        self.assertEqual({e["slot_hint"] for e in elsewhere["events"]}, {sg.SLOT_CROSS_BOOK})


class RefreshTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.card, self.models = root / "card", root / "models"
        (self.card / "books/Books").mkdir(parents=True)
        (self.card / "books/Novels").mkdir(parents=True)
        self.models.mkdir()
        # min_mentions is 4 during sync, so repeat the chapters.
        (self.card / "books/Books/luria.epub").write_bytes(make_epub("Major Trends", ENGLISH + ENGLISH[1:]))
        (self.card / "books/Novels/doupo.epub").write_bytes(make_epub("斗破苍穹", CHINESE + CHINESE))

    def test_builds_then_keeps_unchanged_books(self):
        result = sg.refresh_story_graph(self.models, self.card)
        self.assertEqual((result["prebuilt"], result["updated"], result["books"], result["extracted"], result["pending"]),
                         (False, True, 2, 2, 0))
        data = (self.models / sg.STORY_GRAPH_NAME).read_bytes()
        self.assertEqual(sg.verify_story_graph(data), [])
        self.assertEqual(sg.query_story_graph(data, "萧炎", "/books/Novels/doupo.epub", 0)["status"], "found")
        self.assertEqual(sg.query_story_graph(data, "Isaac Luria", "/books/Books/luria.epub", 0)["status"], "found")
        again = sg.refresh_story_graph(self.models, self.card)
        self.assertEqual((again["updated"], again["extracted"]), (False, 0))
        (self.card / "books/Novels/doupo.epub").unlink()
        gone = sg.refresh_story_graph(self.models, self.card)
        self.assertEqual((gone["updated"], gone["books"], gone["extracted"]), (True, 1, 0))
        self.assertEqual(sg.query_story_graph(self.models / sg.STORY_GRAPH_NAME, "萧炎", None, 0)["status"],
                         "not_found")

    def test_a_prebuilt_index_is_never_touched(self):
        prebuilt = sample_graph()
        (self.models / sg.STORY_GRAPH_NAME).write_bytes(prebuilt)
        result = sg.refresh_story_graph(self.models, self.card)
        self.assertEqual((result["prebuilt"], result["updated"], result["entities"]), (True, False, 4))
        self.assertEqual((self.models / sg.STORY_GRAPH_NAME).read_bytes(), prebuilt)
        self.assertFalse((self.models / sg.STATE_NAME).exists())

    def test_the_time_budget_leaves_books_for_the_next_sync(self):
        ticks = iter([0.0, 0.5, 2.0, 3.0])  # the deadline is 1.0: the first book fits, the second waits
        result = sg.refresh_story_graph(self.models, self.card, budget_seconds=1, clock=lambda: next(ticks))
        self.assertEqual((result["extracted"], result["pending"]), (1, 1))
        result = sg.refresh_story_graph(self.models, self.card)
        self.assertEqual((result["extracted"], result["pending"], result["books"]), (1, 0, 2))


class TeacherTimelinesTest(unittest.TestCase):
    def test_teacher_rows_map_to_a_verified_index(self):
        rows = [
            {"term": "萧炎", "book_path": BOOK, "entity_kind": 1, "en_name_or_role": "Xiao Yan", "first_cutoff": C1,
             "events": [{"cutoff": C3, "chapter_or_page": 30, "slot_hint": 2, "salience_hint": 200,
                         "event_gloss": "won the duel", "target_or_loc": "Yunlan Sect", "scene_text": "萧炎胜了"},
                        {"cutoff": C1, "chapter_or_page": 1, "slot_hint": 1, "salience_hint": 90,
                         "event_gloss": "failed the test", "target_or_loc": "Wu Tan City", "scene_text": ""}],
             "aliases": [{"alias": "萧三少", "reveal_cutoff": C2}]},
            {"term": "Luria", "source": "substack", "book_path": "/substack/beta.txt", "entity_kind": 1,
             "en_name_or_role": "Isaac Luria", "first_cutoff": 0x10005,
             "events": [{"cutoff": 0x10009, "chapter_or_page": 4, "event_gloss": "beta on Luria",
                         "scene_text": "Luria and the vessels"}]},
            {"term": "Luria", "book_path": "/substack/alpha.txt", "entity_kind": 1,
             "events": [{"cutoff": 0x10001, "chapter_or_page": 9, "event_gloss": "alpha on Luria"}],
             "aliases": ["Ari"]},
        ]
        alias_rows = [{"alias": "炎帝", "canonical": "萧炎", "reveal_cutoff": C3, "book_path": BOOK},
                      {"alias": "x", "canonical": "missing", "reveal_cutoff": 0}]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "t.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows),
                                          encoding="utf-8")
            (root / "a.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in alias_rows),
                                          encoding="utf-8")
            entities, aliases = sg.load_teacher_timelines(root / "t.jsonl", root / "a.jsonl")
            result = sg.build_story_graph_index(entities, aliases, root / "story-graph.idx")
            data = (root / "story-graph.idx").read_bytes()
        self.assertEqual((result["entities"], result["events"], result["aliases"]), (2, 4, 3))
        xiao = sg.query_story_graph(data, "萧炎", BOOK, C2)
        self.assertEqual([(e["gloss"], e["target"]) for e in xiao["events"]], [("failed the test", "Wu Tan City")])
        self.assertEqual(xiao["events"][0]["vec"], [0] * 128)  # no scene text
        self.assertEqual(sg.query_story_graph(data, "萧炎", BOOK, C3)["events"][1]["vec"],
                         fallback_encode("萧炎胜了".encode("utf-8")))
        self.assertEqual(sg.query_story_graph(data, "萧三少", BOOK, C1)["status"], "not_found")
        self.assertEqual(sg.query_story_graph(data, "萧三少", BOOK, C2)["entity"]["name"], "Xiao Yan")
        self.assertEqual(sg.query_story_graph(data, "炎帝", BOOK, C3)["alias"], "revealed")
        # Substack rows are not card books: one merged wildcard record of always-visible cross-book echoes.
        echo = sg.query_story_graph(data, "Ari", "/books/Books/any.epub", C1)
        self.assertEqual((echo["status"], echo["entity"]["book_path_hash"], echo["entity"]["first_cutoff"]),
                         ("found", 0, 0))
        self.assertEqual([(e["gloss"], e["cutoff"], e["slot_hint"], e["chapter_or_page"]) for e in echo["events"]],
                         [("beta on Luria", 0, 3, 4), ("alpha on Luria", 0, 3, 9)])


if __name__ == "__main__":
    unittest.main()
