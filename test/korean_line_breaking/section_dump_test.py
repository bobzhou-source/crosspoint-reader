import struct
import unittest

from section_dump import Reader, decode, text_block


def pack(fmt, *values):
    return struct.pack("<" + fmt, *values)


def string(value):
    encoded = value.encode("utf-8")
    return pack("I", len(encoded)) + encoded


def block():
    # Mirrors TextBlock::serialize, including ruby strings between arena and style.
    raw = "甲\0word\0".encode("utf-8")
    return (pack("HBH", 2, 1, len(raw)) + pack("2H", 0, 4) + pack("2h", 3, 27)
            + pack("2H", 0, 9) + pack("2B", 0, 2) + pack("2B", 0, 1) + raw
            + string("こう") + string("") + pack("BB9hBBBb", 0, 1, *range(9), 1, 0, 1, 2))


def section(version=50):
    text = pack("Bhh", 1, 7, 12) + block()
    image = pack("Bhh", 2, 0, 30) + string("image.bmp") + string("image.jpg") + pack("hh", 40, 50)
    rule = pack("BhhHB", 3, 0, 90, 200, 2)
    footnote = b"1".ljust(32, b"\0") + b"#note".ljust(256, b"\0")
    link = b"#target".ljust(256, b"\0") + pack("hhhh", 3, 12, 24, 16)
    page = pack("H", 3) + text + image + rule + pack("H", 1) + footnote + pack("H", 1) + link
    fmt = "BifBBHHBBBBbBHIIIII"
    start = struct.calcsize("<" + fmt)
    lut = start + len(page)
    header = pack(fmt, version, 123, 1.0, 1, 0, 360, 700, 0, 1, 2, 1, 2, 100, 1, lut, 0, 0, 0, 0)
    return header + page + pack("I", start)


class SectionDumpTest(unittest.TestCase):
    def test_exact_textblock_arena_ruby_focus_and_tracking(self):
        parsed = text_block(Reader(block()))
        self.assertEqual(parsed["words"], ["甲", "word"])
        self.assertEqual(parsed["x"], (3, 27))
        self.assertEqual(parsed["origin_steps"], [24])
        self.assertEqual(parsed["ruby"], ["こう", ""])
        self.assertEqual(parsed["focus_boundaries"], (0, 1))
        self.assertEqual(parsed["focus_suffix_x"], (0, 9))
        self.assertEqual(parsed["tracking"], 2)

    def test_page_preserves_origin_and_parses_image_rule_links(self):
        parsed = decode(section())
        self.assertEqual(parsed["font_id"], 123)
        self.assertEqual(parsed["word_spacing"], 100)
        page = parsed["pages"][0]
        self.assertEqual(page["elements"][0]["line_x"], 7)
        self.assertEqual(page["elements"][0]["x"], (3, 27))
        self.assertEqual(page["elements"][1]["source"], "image.jpg")
        self.assertEqual(page["elements"][2]["width"], 200)
        self.assertEqual(page["footnotes"], [dict(number="1", href="#note")])
        self.assertEqual(page["links"][0]["width"], 24)

    def test_supported_partial_and_final_versions(self):
        for version in range(47, 53):
            self.assertFalse(decode(section(version))["partial"])
            partial = decode(section(254 - (version - 28)))
            self.assertTrue(partial["partial"])
            self.assertEqual(partial["format_version"], version)

    def test_rejects_truncated_and_unknown_records(self):
        for data in (b"", section()[:-1], section(1), section(53)):
            with self.assertRaises(ValueError):
                decode(data)
        raw = bytearray(block())
        raw[5:7] = pack("H", 1)  # First word must start at offset zero.
        with self.assertRaises(ValueError):
            text_block(Reader(raw))


if __name__ == "__main__":
    unittest.main()
