#!/usr/bin/env python3
"""Check a StarDict folder the way the reader firmware reads it.

Mirrors src/util/Dictionary.cpp: StringUtils::asciiCaseCmp ordering, the
.qidx/.sidx 256-entry sample sidecars, bisectSamples() + linear scan, .syn
ordinals resolved through locateByOrdinal(), cleanWord() and stemVariants().
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

SAMPLE_INTERVAL = 256
WORD_BUF_BYTES = 256
MAX_DEFINITION_BYTES = 64 * 1024


def fold(byte: int) -> int:
    return byte + 32 if 65 <= byte <= 90 else byte


def ascii_case_cmp(a: bytes, b: bytes) -> int:
    for x, y in zip(a, b):
        diff = fold(x) - fold(y)
        if diff:
            return diff
    if len(a) == len(b):
        return 0
    return fold(a[len(b)]) if len(a) > len(b) else -fold(b[len(a)])


def is_word_byte(c: int) -> bool:
    return c >= 0x80 or chr(c).isalnum()


def clean_word(word: str) -> bytes:
    b = word.encode("utf-8")
    start, end = 0, len(b)
    while start < end:
        if not is_word_byte(b[start]):
            start += 1
        elif end - start >= 3 and b[start] == 0xE2 and b[start + 1] in (0x80, 0x81):
            start += 3
        else:
            break
    while end > start:
        if not is_word_byte(b[end - 1]):
            end -= 1
        elif end - start >= 3 and b[end - 3] == 0xE2 and b[end - 2] in (0x80, 0x81):
            end -= 3
        else:
            break
    # rc10: internal U+2018/U+2019 apostrophes normalize to ASCII '.
    core = b[start:end].replace("\u2019".encode(), b"'").replace("\u2018".encode(), b"'")
    return bytes(c + 32 if 65 <= c <= 90 else c for c in core)


def stem_variants(word: bytes) -> list[bytes]:
    out: list[bytes] = []
    n = len(word)

    def add(value: bytes) -> None:
        if value not in out:
            out.append(value)

    def ends(suffix: bytes) -> bool:
        return n > len(suffix) and word.endswith(suffix)

    if ends(b"'s"):
        add(word[:-2])
    if ends("\u2019s".encode()):
        add(word[:-4])
    if ends(b"ies"):
        add(word[:-3] + b"y")
    if ends(b"es"):
        add(word[:-2])
    if ends(b"s"):
        add(word[:-1])
    if ends(b"ed"):
        add(word[:-2])
        add(word[:-1])
        if n >= 4 and word[n - 3] == word[n - 4]:
            add(word[:-3])
    if ends(b"ing"):
        add(word[:-3])
        add(word[:-3] + b"e")
        if n >= 5 and word[n - 4] == word[n - 5]:
            add(word[:-4])
    return out


class Source:
    """A sorted word-first StarDict file (.idx: 8 suffix bytes, .syn: 4)."""

    def __init__(self, data: bytes, suffix: int):
        self.data, self.suffix = data, suffix
        self.samples, self.entries = self._samples()

    def _samples(self) -> tuple[list[int], int]:
        # Dictionary::buildSidecar: entry 0, then every SAMPLE_INTERVAL-th entry start.
        samples, entries, pos = [0], 0, 0
        data, size = self.data, len(self.data)
        while pos < size:
            end = data.find(b"\0", pos)
            if end < 0 or end + 1 + self.suffix > size:
                break
            pos = end + 1 + self.suffix
            entries += 1
            if entries % SAMPLE_INTERVAL == 0 and pos < size:
                samples.append(pos)
        return samples, entries

    def read_word(self, pos: int) -> tuple[bytes, int] | None:
        # Dictionary::readWordInto: up to 255 bytes kept, the rest consumed.
        end = self.data.find(b"\0", pos)
        if end < 0:
            return None
        return self.data[pos:end][:WORD_BUF_BYTES - 1], end + 1

    def bisect(self, target: bytes) -> int:
        lo, hi = 0, len(self.samples) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            word, _ = self.read_word(self.samples[mid])
            if ascii_case_cmp(word, target) <= 0:
                lo = mid
            else:
                hi = mid - 1
        return self.samples[lo]

    def scan(self, target: bytes) -> tuple[bytes, bytes] | None:
        pos = self.bisect(target)
        while pos < len(self.data):
            got = self.read_word(pos)
            if got is None or got[1] + self.suffix > len(self.data):
                return None
            word, pos = got
            suffix = self.data[pos:pos + self.suffix]
            pos += self.suffix
            cmp = ascii_case_cmp(word, target)
            if cmp == 0:
                return word, suffix
            if cmp > 0:
                return None
        return None

    def by_ordinal(self, ordinal: int) -> tuple[bytes, bytes] | None:
        if ordinal >= self.entries:
            return None
        sample = min(ordinal // SAMPLE_INTERVAL, len(self.samples) - 1)
        pos, current = self.samples[sample], sample * SAMPLE_INTERVAL
        while current < ordinal:
            _, pos = self.read_word(pos)
            pos += self.suffix
            current += 1
        word, pos = self.read_word(pos)
        return word, self.data[pos:pos + self.suffix]


class StarDict:
    def __init__(self, folder: Path):
        idx = sorted(folder.glob("*.idx"))
        if len(idx) != 1:
            raise SystemExit(f"{folder}: expected exactly one .idx stem, found {len(idx)}")
        self.stem = idx[0].with_suffix("")
        self.idx = Source(idx[0].read_bytes(), 8)
        self.dict = self.stem.with_suffix(".dict").read_bytes()
        syn = self.stem.with_suffix(".syn")
        self.syn = Source(syn.read_bytes(), 4) if syn.exists() else None
        self.ifo = dict(line.split("=", 1) for line in self.stem.with_suffix(".ifo").read_text("utf-8").splitlines()
                        if "=" in line)

    def definition(self, suffix: bytes) -> str:
        offset, size = struct.unpack(">II", suffix)
        return self.dict[offset:offset + min(size, MAX_DEFINITION_BYTES)].decode("utf-8")

    def lookup(self, word: str) -> tuple[str, str, str] | None:
        """Dictionary::lookup: exact, then .syn, then stem variants. (headword, definition, path)."""
        cleaned = clean_word(word)
        if not cleaned:
            return None
        hit = self.idx.scan(cleaned)
        path = "exact"
        if hit is None and self.syn is not None:
            syn_hit = self.syn.scan(cleaned)
            if syn_hit is not None:
                hit = self.idx.by_ordinal(struct.unpack(">I", syn_hit[1])[0])
                path = "syn"
        if hit is None:
            for variant in stem_variants(cleaned):
                hit = self.idx.scan(variant)
                if hit is not None:
                    path = f"stem:{variant.decode('utf-8', 'replace')}"
                    break
        if hit is None:
            return None
        return hit[0].decode("utf-8"), self.definition(hit[1]), path


def structural_checks(d: StarDict) -> dict:
    problems: list[str] = []
    data, pos, previous, count = d.idx.data, 0, None, 0
    words: list[bytes] = []
    while pos < len(data):
        end = data.index(b"\0", pos)
        word = data[pos:end]
        offset, size = struct.unpack_from(">II", data, end + 1)
        pos = end + 9
        if len(word) >= WORD_BUF_BYTES:
            problems.append(f"headword over {WORD_BUF_BYTES - 1} bytes: {word[:40]!r}")
        if previous is not None and ascii_case_cmp(previous, word) >= 0:
            problems.append(f"idx out of order or duplicate: {previous!r} >= {word!r}")
        if size == 0 or size > MAX_DEFINITION_BYTES or offset + size > len(d.dict):
            problems.append(f"bad definition span for {word!r}: {offset}+{size}")
        else:
            blob = d.dict[offset:offset + size]
            if b"\0" in blob:
                problems.append(f"NUL inside definition of {word!r}")
            try:
                blob.decode("utf-8")
                word.decode("utf-8")
            except UnicodeDecodeError:
                problems.append(f"invalid UTF-8 at {word!r}")
        previous = word
        words.append(word)
        count += 1
    if pos != len(data):
        problems.append("trailing bytes in .idx")
    syn_count = 0
    if d.syn is not None:
        data, pos, previous = d.syn.data, 0, None
        while pos < len(data):
            end = data.index(b"\0", pos)
            word = data[pos:end]
            (ordinal,) = struct.unpack_from(">I", data, end + 1)
            pos = end + 5
            if ordinal >= count:
                problems.append(f"syn ordinal out of range: {word!r} -> {ordinal}")
            if previous is not None and ascii_case_cmp(previous, word) > 0:
                problems.append(f"syn out of order: {previous!r} > {word!r}")
            previous = word
            syn_count += 1
    if int(d.ifo.get("wordcount", -1)) != count:
        problems.append(f"ifo wordcount {d.ifo.get('wordcount')} != {count}")
    if int(d.ifo.get("idxfilesize", -1)) != len(d.idx.data):
        problems.append("ifo idxfilesize mismatch")
    if d.syn is not None and int(d.ifo.get("synwordcount", -1)) != syn_count:
        problems.append(f"ifo synwordcount {d.ifo.get('synwordcount')} != {syn_count}")
    if d.ifo.get("idxoffsetbits") == "64":
        problems.append("64-bit offsets are unsupported")

    # Every headword the reader could be handed resolves to itself; every
    # .syn row resolves to its target (or to an exact headword, which wins).
    unreachable = 0
    for ordinal, word in enumerate(words):
        text = word.decode("utf-8")
        if clean_word(text) != word.lower():
            unreachable += 1  # edge punctuation the reader strips before lookup
            continue
        hit = d.idx.scan(word.lower())
        if hit is None or hit[0] != word:
            problems.append(f"headword lookup failed: {word!r}")
    if d.syn is not None:
        first_target: dict[bytes, int] = {}
        data, pos = d.syn.data, 0
        while pos < len(data):
            end = data.index(b"\0", pos)
            key = data[pos:end].lower()
            first_target.setdefault(key, struct.unpack_from(">I", data, end + 1)[0])
            pos = end + 5
        for key, ordinal in first_target.items():
            exact = d.idx.scan(key)
            syn_hit = d.syn.scan(key)
            if exact is None and (syn_hit is None or struct.unpack(">I", syn_hit[1])[0] != ordinal
                                  or d.idx.by_ordinal(ordinal)[0] != words[ordinal]):
                problems.append(f"syn lookup failed: {key!r}")
    return {"headwords": count, "single_word_headwords": sum(b" " not in w for w in words),
            "synonyms": syn_count, "sidecar_samples": len(d.idx.samples),
            "headwords_needing_edge_punctuation": unreachable, "problems": problems}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--expect", action="append", default=[], metavar="WORD=LEMMA",
                        help="require WORD to resolve to LEMMA's entry, or to its own headword whose "
                             "definition opens with a 'Form of:' note naming LEMMA (repeatable)")
    parser.add_argument("words", nargs="*", help="words to look up and print")
    args = parser.parse_args()
    d = StarDict(args.folder)
    report = structural_checks(d)
    failures = list(report["problems"])
    lookups = {}
    expectations = dict(item.split("=", 1) for item in args.expect)
    for word in list(dict.fromkeys(args.words + list(expectations))):
        result = d.lookup(word)
        lookups[word] = None if result is None else {
            "headword": result[0], "path": result[2], "first_line": result[1].split("\n", 2)[:2]}
        want = expectations.get(word)
        resolved = result is not None and (
            result[0] == want or (result[1].startswith("Form of: ") and f" {want} (" in result[1].split("\n", 1)[0]
                                  and f"— {want} —" in result[1]))
        if want is not None and not resolved:
            failures.append(f"{word!r} resolved to {None if result is None else result[0]!r}, expected {want!r}")
        elif result is None:
            failures.append(f"{word!r} not found")
    report["lookups"] = lookups
    report["problems"] = failures
    print(json.dumps(report, indent=2, ensure_ascii=False))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
