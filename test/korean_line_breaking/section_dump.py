"""Read actual firmware section caches without touching images/devices.

Usage: python3 section_dump.py OUTPUT.json SECTION.bin [SECTION.bin ...]
Accepts finalized/partial v47-v50 caches. Outputs exact cached word origins,
styles, ruby, tracking and page geometry. origin_steps are NOT gap widths:
font glyph advances/kerning are not stored in a section cache. Do not subtract
LayoutDump's fixed host advances from these actual firmware positions.
Pass --font-probe FontAdvanceDump --font FONT.ttf --point-size N to calculate
glyph advances/gaps with the actual TtfEpdFont backend at the specified size.
Use the exact captured font and size; this does not infer font identity from IDs.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


class Reader:
    def __init__(self, data, offset=0, end=None):
        self.data = data
        self.offset = offset
        self.end = len(data) if end is None else end

    def take(self, count):
        if count < 0 or self.offset + count > self.end:
            raise ValueError(f"truncated record at {self.offset}: need {count} bytes before {self.end}")
        result = self.data[self.offset:self.offset + count]
        self.offset += count
        return result

    def unpack(self, fmt):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def scalar(self, fmt):
        return self.unpack(fmt)[0]

    def string(self):
        return self.take(self.scalar("I")).decode("utf-8")


def text_block(reader):
    count, focus, size = reader.unpack("HBH")
    if count > 10000 or focus not in (0, 1) or size < count or (count == 0 and size != 0):
        raise ValueError("invalid TextBlock scalar fields")
    offsets = reader.unpack(f"{count}H")
    origins = reader.unpack(f"{count}h")
    suffix = reader.unpack(f"{count}H") if focus else ()
    styles = reader.unpack(f"{count}B")
    boundaries = reader.unpack(f"{count}B") if focus else ()
    raw = reader.take(size)
    words = []
    for i, offset in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < count else size
        if (i == 0 and offset != 0) or not offset < end <= size or raw[end - 1] != 0:
            raise ValueError("invalid TextBlock string offsets")
        words.append(raw[offset:end - 1].decode("utf-8"))
    # TextBlock::serialize writes one length-prefixed ruby string per word.
    ruby = [reader.string() for _ in range(count)]
    alignment, explicit = reader.unpack("BB")
    geometry = reader.unpack("9h")
    indent_defined, rtl, direction_defined, tracking = reader.unpack("BBBb")
    return dict(words=words, x=origins, styles=styles, ruby=ruby,
                focus_boundaries=boundaries, focus_suffix_x=suffix,
                origin_steps=[b - a for a, b in zip(origins, origins[1:])],
                alignment=alignment, alignment_explicit=bool(explicit),
                margins=geometry[:4], padding=geometry[4:8], indent=geometry[8],
                indent_defined=bool(indent_defined), rtl=bool(rtl),
                direction_defined=bool(direction_defined), tracking=tracking)


def page(reader):
    elements = []
    for _ in range(reader.scalar("H")):
        tag = reader.scalar("B")
        x, y = reader.unpack("hh")
        if tag == 1:
            elements.append(dict(type="text", line_x=x, y=y, **text_block(reader)))
        elif tag == 2:
            image_path, source_path = reader.string(), reader.string()
            width, height = reader.unpack("hh")
            elements.append(dict(type="image", x=x, y=y, path=image_path, source=source_path,
                                 width=width, height=height))
        elif tag == 3:
            width, thickness = reader.unpack("HB")
            elements.append(dict(type="rule", x=x, y=y, width=width, thickness=thickness))
        else:
            raise ValueError(f"unknown page element tag {tag}")
    footnotes = []
    for _ in range(reader.scalar("H")):
        number = reader.take(32).split(b"\0", 1)[0].decode("utf-8")
        href = reader.take(256).split(b"\0", 1)[0].decode("utf-8")
        footnotes.append(dict(number=number, href=href))
    links = []
    for _ in range(reader.scalar("H")):
        href = reader.take(256).split(b"\0", 1)[0].decode("utf-8")
        x, y, width, height = reader.unpack("hhhh")
        links.append(dict(href=href, x=x, y=y, width=width, height=height))
    if reader.offset != reader.end:
        raise ValueError(f"page ended at {reader.offset}, expected {reader.end}")
    return dict(elements=elements, footnotes=footnotes, links=links)


def decode(data):
    reader = Reader(data)
    version = reader.scalar("B")
    format_version = version if version < 128 else 28 + 254 - version
    if format_version not in (47, 48, 49, 50):
        raise ValueError(f"unsupported section version {version} (format {format_version})")
    font_id, compression, paragraph_spacing, alignment, width, height = reader.unpack("ifBBHH")
    hyphenation, embedded, images, focus, tracking, word_spacing = reader.unpack("BBBBbB")
    count, lut, anchors, paragraphs, list_items, visible = reader.unpack("HIIIII")
    header_end = reader.offset
    if not header_end <= lut <= len(data) - count * 4:
        raise ValueError("invalid page LUT offset")
    offsets = Reader(data, lut).unpack(f"{count}I")
    pages = []
    for i, offset in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < count else lut
        if not header_end <= offset < end <= lut:
            raise ValueError("invalid page range")
        pages.append(page(Reader(data, offset, end)))
    return dict(version=version, format_version=format_version, partial=version >= 128,
                font_id=font_id, compression=compression, paragraph_spacing=bool(paragraph_spacing),
                alignment=alignment, width=width, height=height, hyphenation=bool(hyphenation),
                embedded_style=bool(embedded), images=images, focus=bool(focus), tracking=tracking,
                word_spacing=word_spacing, anchors_offset=anchors, paragraphs_offset=paragraphs,
                list_items_offset=list_items, visible_offset=visible, pages=pages)


def add_font_metrics(record, executable, font_path, point_size):
    lines = [element for page in record["pages"] for element in page["elements"] if element["type"] == "text"]
    requests = []
    for line in lines:
        for index, word in enumerate(line["words"]):
            if "\n" in word or "\r" in word or "\0" in word:
                raise ValueError("control character in cached word")
            boundary = line["focus_boundaries"][index] if line["focus_boundaries"] else 0
            suffix = line["focus_suffix_x"][index] if line["focus_suffix_x"] else 0
            quoted = '"' + word.replace("\\", "\\\\").replace('"', '\\"') + '"'
            requests.append(f'{line["styles"][index]} {line["tracking"]} {record["word_spacing"]} '
                            f'{boundary} {suffix} {quoted}\n')
    result = subprocess.run([executable, str(font_path), str(point_size)], input="".join(requests),
                            text=True, capture_output=True, check=True)
    advances = [int(value) for value in result.stdout.splitlines()]
    if len(advances) != len(requests):
        raise ValueError(f"font probe returned {len(advances)} advances for {len(requests)} words")
    index = 0
    for line in lines:
        count = len(line["words"])
        widths = advances[index:index + count]
        index += count
        line["word_advances"] = widths
        origins = line["x"]
        line["gaps"] = [origins[i] - origins[i - 1] - widths[i - 1] if origins[i] >= origins[i - 1]
                        else origins[i - 1] - origins[i] - widths[i] for i in range(1, count)]
    record["measurement"] = "actual cached origins minus real TtfEpdFont rendered advances; not inferred defects"
    record["font_metrics"] = dict(path=str(font_path), sha256=hashlib.sha256(font_path.read_bytes()).hexdigest(),
                                  point_size=point_size, backend="TtfEpdFont/FreeInkFont/FreeType")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--font-probe")
    parser.add_argument("--font", type=Path)
    parser.add_argument("--point-size", type=int)
    args = parser.parse_args()
    if any((args.font_probe, args.font, args.point_size)) and not all((args.font_probe, args.font, args.point_size)):
        parser.error("--font-probe, --font and --point-size must be supplied together")
    result = []
    for name in args.inputs:
        record = decode(name.read_bytes())
        record["path"] = str(name)
        record["measurement"] = "actual cached word origins; origin_steps are not glyph gaps"
        if args.font_probe:
            add_font_metrics(record, args.font_probe, args.font, args.point_size)
        result.append(record)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(f"Decoded {len(result)} sections / {sum(len(r['pages']) for r in result)} pages -> {args.output}")


if __name__ == "__main__":
    main()
