#!/usr/bin/env python3
"""Build an uncompressed StarDict from the official MDBG CC-CEDICT release."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import unicodedata
from urllib.request import urlopen

SOURCE = "https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.txt.gz"
LICENSE = "https://creativecommons.org/licenses/by-sa/4.0/legalcode.txt"
ENTRY = re.compile(r"^(\S+) (\S+) \[(.*?)\] /(.*)/$")
SYLLABLE = re.compile(r"([A-Za-züÜêÊ:]+)([0-5])")
MARKS = "\u0304\u0301\u030c\u0300"


def tone_marks(pinyin: str) -> str:
    def mark(match: re.Match[str]) -> str:
        syllable = match[1].replace("u:", "ü").replace("U:", "Ü").replace("v", "ü").replace("V", "Ü")
        tone = int(match[2])
        if tone in (0, 5):
            return syllable
        lower = syllable.lower()
        if "a" in lower:
            index = lower.index("a")
        elif "e" in lower:
            index = lower.index("e")
        elif "ou" in lower:
            index = lower.index("o")
        else:
            vowels = [i for i, c in enumerate(lower) if c in "iouüê"]
            # Syllabic m/n (e.g. 嗯 [ng2]) can carry a tone too.
            index = vowels[-1] if vowels else next((i for i, c in enumerate(lower) if c in "mn"), -1)
        if index < 0:
            return match[0]
        return unicodedata.normalize("NFC", syllable[:index + 1] + MARKS[tone - 1] + syllable[index + 1:])
    return SYLLABLE.sub(mark, pinyin)


def sort_key(word: str) -> bytes:
    # StringUtils::asciiCaseCmp in the firmware's C locale: ASCII fold only.
    return word.encode("utf-8").lower()


def build(source: bytes, license_text: bytes, output: Path) -> dict:
    text = gzip.decompress(source).decode("utf-8-sig")
    entries: dict[bytes, tuple[str, list[str]]] = {}
    count = 0
    comments = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            if line.startswith("#"):
                comments.append(line)
            continue
        match = ENTRY.fullmatch(line)
        if not match:
            raise ValueError(f"Malformed CC-CEDICT entry at line {number}")
        traditional, simplified, pinyin, glosses = match.groups()
        definition = tone_marks(pinyin) + "\n" + "; ".join(glosses.split("/"))
        count += 1
        for word in dict.fromkeys((simplified, traditional)):
            if "\x00" in word or len(word.encode()) >= 256:
                raise ValueError(f"Unsupported headword at line {number}")
            key = sort_key(word)
            if key not in entries:
                entries[key] = (word, [])
            definitions = entries[key][1]
            if definition not in definitions:
                definitions.append(definition)
    if not entries:
        raise ValueError("No CC-CEDICT entries")
    output.mkdir(parents=True, exist_ok=True)
    with (output / "cedict.dict").open("wb") as data, (output / "cedict.idx").open("wb") as index:
        for key in sorted(entries):
            word, definitions = entries[key]
            encoded = "\n\n".join(definitions).encode("utf-8")
            index.write(word.encode("utf-8") + b"\0" + struct.pack(">II", data.tell(), len(encoded)))
            data.write(encoded)
    (output / "cedict.ifo").write_text(
        "StarDict's dict ifo file\nversion=2.4.2\n"
        f"wordcount={len(entries)}\nidxfilesize={(output / 'cedict.idx').stat().st_size}\n"
        "bookname=CC-CEDICT Chinese-English\nsametypesequence=m\n"
        "author=CC-CEDICT contributors\nwebsite=https://www.mdbg.net/chinese/dictionary?page=cc-cedict\n"
        "description=CC BY-SA 4.0; simplified and traditional; tone-mark pinyin. See LICENSE.txt and ATTRIBUTION.txt.\n",
        encoding="utf-8",
    )
    (output / "LICENSE.txt").write_bytes(license_text)
    (output / "ATTRIBUTION.txt").write_text(
        "CC-CEDICT, by the CC-CEDICT contributors. Started by Paul Denisowski in 1997.\n"
        f"Source: {SOURCE}\nLicense: https://creativecommons.org/licenses/by-sa/4.0/\n"
        "Changes: converted to StarDict; indexed both scripts; merged duplicate headword senses; "
        "converted numbered pinyin to tone marks. This converted database is CC BY-SA 4.0.\n\n"
        + "\n".join(comments) + "\n", encoding="utf-8")
    (output / "cedict-source.txt.gz").write_bytes(source)
    manifest = {"source_url": SOURCE, "source_entries": count, "indexed_headwords": len(entries),
                "files": {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(), "bytes": p.stat().st_size}
                          for p in sorted(output.iterdir()) if p.name in (
                              "cedict.ifo", "cedict.idx", "cedict.dict", "LICENSE.txt", "ATTRIBUTION.txt", "cedict-source.txt.gz")}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "SHA256SUMS").write_text("".join(f"{v['sha256']}  {k}\n" for k, v in manifest["files"].items()))
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, help="Previously downloaded official .txt.gz")
    parser.add_argument("--license", type=Path, help="Previously downloaded CC BY-SA 4.0 legalcode.txt")
    args = parser.parse_args()
    source = args.source.read_bytes() if args.source else urlopen(SOURCE, timeout=60).read()
    license_text = args.license.read_bytes() if args.license else urlopen(LICENSE, timeout=60).read()
    print(json.dumps(build(source, license_text, args.output), indent=2))


if __name__ == "__main__":
    main()
