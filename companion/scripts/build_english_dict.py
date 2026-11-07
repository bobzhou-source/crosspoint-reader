#!/usr/bin/env python3
"""Build an uncompressed English StarDict from Open English WordNet."""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import io
import json
from pathlib import Path
import re
import struct
from urllib.request import urlopen
import zipfile

NLTK_DATA = "https://raw.githubusercontent.com/nltk/nltk_data/gh-pages/packages/corpora/"
SOURCES = {
    # Open English WordNet 2025+ in WNDB format (CC BY 4.0): headwords, glosses, exception lists.
    "oewn": NLTK_DATA + "english_wordnet.zip",
    # Princeton WordNet 3.0: only its morphy exception lists, unioned with OEWN's.
    "wordnet30": NLTK_DATA + "wordnet.zip",
    # CMU Pronouncing Dictionary: only attests that a generated regular inflection is a real word.
    "cmudict": NLTK_DATA + "cmudict.zip",
}
FUNCTION_WORDS = Path(__file__).with_name("english_function_words.txt")
STEM = "english"
POS = (("noun", "n"), ("verb", "v"), ("adj", "a"), ("adv", "r"))
LABELS = {"n": "noun", "v": "verb", "a": "adjective", "r": "adverb"}
MAX_SENSES = 10
MAX_SYNONYMS = 3
# Firmware limits: Dictionary::wordBuf is 256 bytes (NUL included), definitions cap at 64 KiB.
MAX_HEADWORD_BYTES = 255
MAX_DEFINITION_BYTES = 64 * 1024


def sort_key(word: str) -> bytes:
    # StringUtils::asciiCaseCmp in the firmware's C locale: ASCII fold only.
    return word.encode("utf-8").lower()


@dataclass
class Entry:
    # spelling -> [(label, weight, senses)], senses already formatted as lines.
    groups: dict[str, list[tuple[str, float, list[str]]]] = field(default_factory=dict)
    preferred: str | None = None  # function-word spelling wins the headword display
    pos: set[str] = field(default_factory=set)

    def add(self, spelling: str, label: str, weight: float, sense: str) -> None:
        blocks = self.groups.setdefault(spelling, [])
        for existing_label, _, senses in blocks:
            if existing_label == label:
                if sense not in senses:
                    senses.append(sense)
                return
        blocks.append((label, weight, [sense]))

    def display(self) -> str:
        if self.preferred:
            return self.preferred
        lower = [s for s in self.groups if s == s.lower()]
        return lower[0] if lower else sorted(self.groups, key=lambda s: (-self.sense_count(s), s))[0]

    def sense_count(self, spelling: str) -> int:
        return sum(len(senses) for _, _, senses in self.groups[spelling])

    def render(self) -> list[str]:
        head = self.display()
        spellings = [head] + sorted((s for s in self.groups if s != head), key=lambda s: (-self.sense_count(s), s))
        lines: list[str] = []
        for spelling in spellings:
            # The headword line above already names the first spelling; label
            # only the other case variants ("Bush" under "bush").
            if spelling != head:
                lines.append(f"— {spelling} —")
            for label, _, senses in sorted(self.groups[spelling], key=lambda block: -block[1]):
                lines.append(f"({label})")
                lines.extend(f"{n}. {sense}" for n, sense in enumerate(senses[:MAX_SENSES], 1))
                if len(senses) > MAX_SENSES:
                    lines.append(f"(+{len(senses) - MAX_SENSES} more senses)")
        return lines


def fetch(name: str, path: Path | None) -> bytes:
    return path.read_bytes() if path else urlopen(SOURCES[name], timeout=120).read()


def member(archive: zipfile.ZipFile, basename: str) -> str | None:
    for info in archive.infolist():
        if info.filename.rsplit("/", 1)[-1] == basename and not info.is_dir():
            return archive.read(info).decode("utf-8")
    return None


def clean(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip().rstrip(";").strip()
    # The definition screen runs plain text through htmlToPlainText(): keep a
    # literal "<tag" or "&name;" from being read as markup.
    text = re.sub(r"<(?=[A-Za-z/!?])", "< ", text)
    return re.sub(r"&(?=[#A-Za-z0-9]{1,15};)", "& ", text)


def split_gloss(gloss: str) -> tuple[str, str | None]:
    quote = re.search(r';\s*"', gloss)
    if not quote:
        return clean(gloss), None
    examples = re.findall(r'"([^"]+)"', gloss[quote.start():])
    return clean(gloss[:quote.start()]), clean(examples[0]) if examples else None


def parse_data(text: str) -> dict[str, tuple[list[str], str]]:
    synsets = {}
    for line in text.splitlines():
        if not line or line.startswith("  "):
            continue
        head, _, gloss = line.partition(" | ")
        fields = head.split()
        count = int(fields[3], 16)
        words = [re.sub(r"\([a-z]+\)$", "", fields[4 + 2 * i]).replace("_", " ") for i in range(count)]
        synsets[fields[0]] = (words, gloss)
    return synsets


def parse_index(text: str):
    for line in text.splitlines():
        if not line or line.startswith("  "):
            continue
        fields = line.split()
        synset_count, pointer_count = int(fields[2]), int(fields[3])
        tagged = int(fields[5 + pointer_count])
        yield fields[0].replace("_", " "), fields[6 + pointer_count:6 + pointer_count + synset_count], tagged


def parse_license(data_noun: str) -> str:
    header = [re.sub(r"^\s+\d+\s?", "", line).rstrip() for line in data_noun.splitlines() if line.startswith("  ")]
    return "\n".join(header).strip() + "\n\nCC BY 4.0: https://creativecommons.org/licenses/by/4.0/legalcode\n"


def load_function_words(path: Path) -> list[tuple[str, str, list[str]]]:
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("|", 2)
        if len(parts) != 3:
            raise ValueError(f"{path.name} line {number}: expected word|label|senses")
        rows.append((parts[0], parts[1], [clean(s) for s in parts[2].split(" || ")]))
    return rows


def parse_exceptions(archives: list[zipfile.ZipFile]) -> dict[str, list[tuple[str, str]]]:
    forms: dict[str, list[tuple[str, str]]] = {}
    for archive in archives:
        for name, pos in POS:
            text = member(archive, f"{name}.exc") or ""
            for line in text.splitlines():
                fields = line.split()
                if len(fields) < 2:
                    continue
                form = fields[0].replace("_", " ")
                for lemma in fields[1:]:
                    pair = (lemma.replace("_", " "), pos)
                    if pair[0] != form and pair not in forms.setdefault(form, []):
                        forms[form].append(pair)
    return forms


def regular_inflections(word: str, pos: str) -> list[str]:
    """Spelling-rule inflections; callers keep only those a word list attests."""
    if not re.fullmatch(r"[a-z]+", word) or len(word) < 2:
        return []
    vowels = "aeiou"
    one_syllable = len(re.findall(r"[aeiouy]+", word)) == 1
    cvc = len(word) >= 3 and word[-1] not in "aeiouwxy" and word[-2] in vowels and word[-3] not in vowels
    cons_y = len(word) >= 2 and word[-1] == "y" and word[-2] not in vowels

    def suffixed(suffix: str) -> list[str]:
        if cons_y and not suffix.startswith("i"):
            return [word[:-1] + "i" + suffix]
        if suffix[0] in "ei" and word.endswith("e") and not (suffix == "ing" and word.endswith(("ee", "oe", "ye"))):
            return [word + suffix[1:]] if suffix[0] == "e" else [word[:-1] + suffix]
        if cvc:
            return [word + word[-1] + suffix] + ([] if one_syllable else [word + suffix])
        if word.endswith("c") and suffix[0] in "ei":
            return [word + "k" + suffix, word + suffix]
        return [word + suffix]

    def plural() -> list[str]:
        if re.search(r"(s|x|z|ch|sh)$", word):
            return [word + "es"]
        if cons_y:
            return [word[:-1] + "ies"]
        return [word + "s", word + "es"] if word.endswith("o") else [word + "s"]

    if pos == "n":
        # morphy's "men" -> "man" rule (women, firemen); "humen" never attests.
        return plural() + ([word[:-3] + "men"] if word.endswith("man") else [])
    if pos == "v":
        gerund = [word[:-2] + "ying"] if word.endswith("ie") else (
            [word + "ing"] if cons_y else suffixed("ing"))
        return plural() + suffixed("ed") + gerund
    if pos == "a":
        return suffixed("er") + suffixed("est")
    return []


def firmware_stems(word: str) -> list[str]:
    """Dictionary::stemVariants, in order."""
    out: list[str] = []
    n = len(word)

    def add(value: str) -> None:
        if value not in out:
            out.append(value)

    def ends(suffix: str) -> bool:
        return n > len(suffix) and word.endswith(suffix)

    if ends("'s"):
        add(word[:-2])
    if ends("\u2019s"):
        add(word[:-2])
    if ends("ies"):
        add(word[:-3] + "y")
    if ends("es"):
        add(word[:-2])
    if ends("s"):
        add(word[:-1])
    if ends("ed"):
        add(word[:-2])
        add(word[:-1])
        if n >= 4 and word[-3] == word[-4]:
            add(word[:-3])
    if ends("ing"):
        add(word[:-3])
        add(word[:-3] + "e")
        if n >= 5 and word[-4] == word[-5]:
            add(word[:-4])
    return out


def build(oewn: bytes, wordnet30: bytes, cmudict: bytes, output: Path,
          function_words: Path = FUNCTION_WORDS) -> dict:
    oewn_zip = zipfile.ZipFile(io.BytesIO(oewn))
    wn30_zip = zipfile.ZipFile(io.BytesIO(wordnet30))
    cmu_zip = zipfile.ZipFile(io.BytesIO(cmudict))
    entries: dict[bytes, Entry] = {}

    for row, (word, label, senses) in enumerate(load_function_words(function_words)):
        entry = entries.setdefault(sort_key(word), Entry())
        entry.preferred = entry.preferred or word
        for sense in senses:
            entry.add(word, label, 1e9 - row, sense)

    # OEWN's index files zero tagsense_cnt; its cntlist.rev keeps the SemCor
    # tag counts that order parts of speech and competing lemmas by frequency.
    frequency: dict[tuple[str, str], int] = {}
    for line in (member(oewn_zip, "cntlist.rev") or "").splitlines():
        fields = line.split()
        if len(fields) == 3 and "%" in fields[0] and fields[2].isdigit():
            lemma, _, sense = fields[0].partition("%")
            if sense[:1] in "12345" and sense[:1]:
                pair = (lemma.replace("_", " "), "nvara"[int(sense[0]) - 1])
                frequency[pair] = frequency.get(pair, 0) + int(fields[2])

    lemma_pos: set[tuple[bytes, str]] = set()
    wordnet_lemmas = 0
    data_noun = ""
    for name, pos in POS:
        data_text = member(oewn_zip, f"data.{name}")
        index_text = member(oewn_zip, f"index.{name}")
        if not data_text or not index_text:
            raise ValueError(f"WordNet archive lacks data.{name} or index.{name}")
        data_noun = data_noun or data_text
        synsets = parse_data(data_text)
        for lemma, offsets, tagged in parse_index(index_text):
            if len(lemma.encode("utf-8")) > MAX_HEADWORD_BYTES:
                continue
            wordnet_lemmas += 1
            # Tag count, then sense count, then noun > verb > adjective > adverb.
            weight = (max(tagged, frequency.get((lemma, pos), 0)) + 0.001 * len(offsets)
                      + {"n": 0.0004, "v": 0.0003, "a": 0.0002, "r": 0.0001}[pos])
            for offset in offsets:
                words, gloss = synsets[offset]
                spelling = next((w for w in words if w.lower() == lemma), lemma)
                definition, example = split_gloss(gloss)
                synonyms = []
                for w in words:
                    if w.lower() != lemma and w not in synonyms:
                        synonyms.append(w)
                sense = definition + (f': "{example}"' if example else "")
                if synonyms:
                    sense += f" [= {', '.join(synonyms[:MAX_SYNONYMS])}]"
                key = sort_key(spelling)
                entry = entries.setdefault(key, Entry())
                entry.add(spelling, LABELS[pos], weight, sense)
                entry.pos.add(pos)
                lemma_pos.add((key, pos))

    # Morphy exception lists (irregular inflections), kept only where the
    # lemma really is a headword of that part of speech.
    exceptions: dict[bytes, list[tuple[bytes, str]]] = {}
    exception_spelling: dict[bytes, str] = {}
    for form, pairs in parse_exceptions([oewn_zip, wn30_zip]).items():
        if len(form.encode("utf-8")) > MAX_HEADWORD_BYTES:
            continue
        kept = [(sort_key(lemma), pos) for lemma, pos in pairs if (sort_key(lemma), pos) in lemma_pos]
        kept = [pair for i, pair in enumerate(kept) if pair not in kept[:i] and pair[0] != sort_key(form)]
        if kept:
            key = sort_key(form)
            exceptions.setdefault(key, [])
            exceptions[key] += [pair for pair in kept if pair not in exceptions[key]]
            exception_spelling.setdefault(key, form)

    # Regular inflections the firmware stemmer cannot resolve to their lemma
    # ("studied" -> "study", "hopes" -> "hope", "happier" -> "happy"), attested
    # by the CMU word list so the spelling rules never invent words.
    attested = set()
    for line in (member(cmu_zip, "cmudict") or "").splitlines():
        if line and not line.startswith(";;;"):
            attested.add(re.sub(r"\(\d+\)$", "", line.split(" ", 1)[0]).lower())
    if not attested:
        raise ValueError("CMU dictionary archive lacks cmudict")
    headwords = set(entries)
    candidates: dict[bytes, list[tuple[bytes, str]]] = {}
    for key, pos in sorted(lemma_pos):
        lemma = key.decode("utf-8")
        # Inflect only lowercase common words: abbreviations and names ("HA",
        # "WA") would otherwise claim forms like "has" and "was".
        spelled = entries[key].groups.get(lemma, [])
        if not any(label == LABELS[pos] for label, _, _ in spelled) or (len(lemma) < 3 and pos != "v"):
            continue
        for form in regular_inflections(lemma, pos):
            form_key = sort_key(form)
            if form in attested and form_key not in headwords and (key, pos) not in candidates.get(form_key, []):
                candidates.setdefault(form_key, []).append((key, pos))
    regular: dict[bytes, list[tuple[bytes, str]]] = {}
    for form_key, pairs in candidates.items():
        if form_key in exceptions:
            # A .syn row preempts the stemmer, so an exception-listed form
            # must also carry its regular readings.
            exceptions[form_key] += [pair for pair in pairs if pair not in exceptions[form_key]]
            continue
        resolved = next((s for s in firmware_stems(form_key.decode("utf-8")) if sort_key(s) in headwords), None)
        # Skip only when the stemmer already lands on every reading; otherwise
        # the .syn row needs them all ("does" -> do, doe), or "es" stripping
        # alone would show "doe".
        if resolved is None or any(lemma != sort_key(resolved) for lemma, _ in pairs):
            regular[form_key] = pairs

    # Forms that resolve to several lemmas ("axes" -> ax, axis) become their own
    # headwords, so a lookup shows every candidate instead of the first.
    inflections = {**regular, **exceptions}
    spelling = {key: exception_spelling.get(key, key.decode("utf-8")) for key in inflections}

    def lemma_note(pairs: list[tuple[bytes, str]]) -> str:
        grouped: dict[bytes, list[str]] = {}
        for key, pos in pairs:
            grouped.setdefault(key, []).append(LABELS[pos])
        return "Form of: " + ", ".join(f"{entries[key].display()} ({', '.join(labels)})"
                                       for key, labels in grouped.items())

    def weight(key: bytes) -> float:
        return max(w for blocks in entries[key].groups.values() for _, w, _ in blocks)

    rendered: dict[bytes, list[str]] = {key: entry.render() for key, entry in entries.items()}
    lemma_order: dict[bytes, list[bytes]] = {}
    combined = 0
    for key, pairs in inflections.items():
        lemmas = sorted(dict.fromkeys(k for k, _ in pairs), key=lambda k: -weight(k))
        lemma_order[key] = lemmas
        pairs.sort(key=lambda pair: lemmas.index(pair[0]))
        if key not in entries and len(lemmas) == 1:
            continue  # resolved through .syn to the lemma's own entry
        # A form that is a headword ("saw", "teeth", "left") keeps its own
        # senses, then shows each lemma it inflects, since the reader's exact
        # .idx match always wins over .syn.
        lines = [lemma_note(pairs)] + (rendered[key] if key in entries else [])
        for lemma in lemmas:
            lines.append(f"— {entries[lemma].display()} —")
            lines.extend(entries[lemma].render())
        rendered[key] = lines
        combined += key not in entries

    displays = {key: (entries[key].display() if key in entries else spelling[key]) for key in rendered}
    ordered = sorted(rendered)
    ordinal = {key: i for i, key in enumerate(ordered)}
    output.mkdir(parents=True, exist_ok=True)
    largest = 0
    with (output / f"{STEM}.dict").open("wb") as data, (output / f"{STEM}.idx").open("wb") as index:
        for key in ordered:
            encoded = "\n".join(rendered[key]).encode("utf-8")
            if len(encoded) > MAX_DEFINITION_BYTES:
                raise ValueError(f"definition for {displays[key]!r} exceeds {MAX_DEFINITION_BYTES} bytes")
            largest = max(largest, len(encoded))
            index.write(displays[key].encode("utf-8") + b"\0" + struct.pack(">II", data.tell(), len(encoded)))
            data.write(encoded)

    # .syn: every form that is not its own headword resolves through its
    # lemma's .idx ordinal. Headword forms keep their exception mappings too;
    # an exact .idx hit always wins over .syn in the firmware, so those rows
    # only document the mapping.
    synonyms = []
    for key, lemmas in lemma_order.items():
        if key not in rendered or key in entries:
            synonyms.extend((key, ordinal[lemma], spelling[key]) for lemma in lemmas)
    synonyms.sort(key=lambda row: row[0])  # stable: most frequent lemma first
    with (output / f"{STEM}.syn").open("wb") as syn:
        for _, target, form in synonyms:
            syn.write(form.encode("utf-8") + b"\0" + struct.pack(">I", target))

    (output / f"{STEM}.ifo").write_text(
        "StarDict's dict ifo file\nversion=2.4.2\n"
        f"wordcount={len(ordered)}\nsynwordcount={len(synonyms)}\n"
        f"idxfilesize={(output / f'{STEM}.idx').stat().st_size}\n"
        "bookname=English (Open English WordNet)\nsametypesequence=m\n"
        "author=Open English WordNet team; Princeton University\nwebsite=https://en-word.net/\n"
        "description=CC BY 4.0; WordNet glosses by part of speech, function words, "
        "irregular and regular inflections. See LICENSE.txt and ATTRIBUTION.txt.\n",
        encoding="utf-8",
    )
    (output / "LICENSE.txt").write_text(parse_license(data_noun), encoding="utf-8")
    (output / "ATTRIBUTION.txt").write_text(
        "Open English WordNet (https://en-word.net/), by the Open English WordNet team, "
        "derived from Princeton WordNet 3.1. License: CC BY 4.0.\n"
        f"Source: {SOURCES['oewn']}\n"
        f"Irregular forms also from Princeton WordNet 3.0 exception lists: {SOURCES['wordnet30']}\n"
        f"Regular inflections attested against the CMU Pronouncing Dictionary word list: {SOURCES['cmudict']}\n"
        "Changes: converted to StarDict; merged ASCII-case-equivalent headwords; capped each part of "
        f"speech at {MAX_SENSES} senses with one example and up to {MAX_SYNONYMS} synonyms; added "
        "hand-written function-word, pronoun and contraction entries; added inflection notes and a "
        ".syn index of irregular and stemmer-resistant regular inflections.\n",
        encoding="utf-8")
    sources = {"oewn": oewn, "wordnet30": wordnet30, "cmudict": cmudict}
    manifest = {
        "sources": {name: {"url": SOURCES[name], "sha256": hashlib.sha256(blob).hexdigest(), "bytes": len(blob)}
                    for name, blob in sources.items()},
        "wordnet_lemmas": wordnet_lemmas,
        "indexed_headwords": len(ordered),
        "single_word_headwords": sum(1 for key in ordered if b" " not in key),
        "combined_inflection_headwords": combined,
        "synonyms": len(synonyms),
        "irregular_forms": len(exceptions),
        "regular_forms": len(regular),
        "largest_definition_bytes": largest,
        "files": {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(), "bytes": p.stat().st_size}
                  for p in sorted(output.iterdir()) if p.name in (
                      f"{STEM}.ifo", f"{STEM}.idx", f"{STEM}.dict", f"{STEM}.syn", "LICENSE.txt", "ATTRIBUTION.txt")},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "SHA256SUMS").write_text("".join(f"{v['sha256']}  {k}\n" for k, v in manifest["files"].items()))
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--oewn", type=Path, help="Previously downloaded nltk_data english_wordnet.zip")
    parser.add_argument("--wordnet30", type=Path, help="Previously downloaded nltk_data wordnet.zip")
    parser.add_argument("--cmudict", type=Path, help="Previously downloaded nltk_data cmudict.zip")
    args = parser.parse_args()
    manifest = build(fetch("oewn", args.oewn), fetch("wordnet30", args.wordnet30),
                     fetch("cmudict", args.cmudict), args.output)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
