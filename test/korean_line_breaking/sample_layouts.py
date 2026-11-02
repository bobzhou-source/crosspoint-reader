"""Read-only sample EPUB paragraph probe for LayoutDump's deterministic host metrics.

Usage: python3 sample_layouts.py LAYOUT_DUMP SAMPLE_DIRECTORY OUTPUT_JSON
Does not edit EPUBs. Reports raw paragraph text, archive/member provenance, and
layout words/positions/gaps at two widths, with forced justification/zero indent.
These are real-text stress fixtures, NOT per-book on-device defect counts.
Inline styling/HTML parsing is tested separately by ChapterHtmlSlimParserTest.
"""

import json
from html.parser import HTMLParser
from pathlib import Path
import subprocess
import sys
import zipfile


class Paragraphs(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.current = None
        self.paragraphs = []

    def handle_starttag(self, tag, attrs):
        if tag in ("p", "h1", "h2", "h3"):
            self.finish()
            self.current = []
        elif tag == "br":
            self.finish()
            self.current = []

    def handle_endtag(self, tag):
        if tag in ("p", "h1", "h2", "h3"):
            self.finish()

    def handle_data(self, data):
        if self.current is not None:
            self.current.append(data)

    def finish(self):
        if self.current:
            self.paragraphs.append("".join(self.current))
        self.current = None


def main():
    executable, directory, output = sys.argv[1:]
    records = []
    for archive in sorted(Path(directory).glob("*.epub")):
        chosen = {}
        with zipfile.ZipFile(archive) as book:
            for member in book.namelist():
                if not member.lower().endswith((".html", ".xhtml", ".htm")):
                    continue
                parser = Paragraphs()
                parser.feed(book.read(member).decode("utf-8-sig"))
                parser.finish()
                for paragraph in parser.paragraphs:
                    categories = []
                    if paragraph.strip():
                        chosen.setdefault("first", (member, paragraph))
                    if len(paragraph.strip()) >= 100:
                        categories.append("prose")
                    if "第79章" in paragraph:
                        categories.append("chapter79")
                    if "  " in paragraph and paragraph.strip():
                        categories.append("repeated_ascii_space")
                    if "\xa0" in paragraph:
                        categories.append("nbsp")
                    if "\xad" in paragraph:
                        categories.append("soft_hyphen")
                    for category in categories:
                        chosen.setdefault(category, (member, paragraph))
        for category, (member, paragraph) in chosen.items():
            record = dict(archive=archive.name, member=member, category=category, paragraph=paragraph, layouts=[])
            for width in (160, 360):
                result = subprocess.run([executable, str(width), "2"], input=paragraph, text=True,
                                        capture_output=True, check=True)
                record["layouts"].append(json.loads(result.stdout))
            records.append(record)
    Path(output).write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n")
    print(f"{len(records)} real paragraphs from {len(set(r['archive'] for r in records))} EPUBs -> {output}")
    for record in records:
        for layout in record["layouts"]:
            gaps = [gap for line in layout["lines"] for gap in line["gaps"]]
            print(record["archive"], record["category"], layout["width"],
                  f"lines={len(layout['lines'])} max_gap={max(gaps, default=0)}")


if __name__ == "__main__":
    main()
