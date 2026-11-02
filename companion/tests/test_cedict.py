import gzip
import importlib.util
from pathlib import Path
import struct

spec = importlib.util.spec_from_file_location("build_cedict", Path(__file__).parents[1] / "scripts/build_cedict.py")
cedict = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cedict)


def read_entries(folder):
    index = (folder / "cedict.idx").read_bytes()
    data = (folder / "cedict.dict").read_bytes()
    result = {}
    pos = 0
    while pos < len(index):
        end = index.index(b"\0", pos)
        word = index[pos:end].decode()
        offset, size = struct.unpack_from(">II", index, end + 1)
        result[word] = data[offset:offset + size].decode()
        pos = end + 9
    return result


def test_dual_script_senses_order_and_license(tmp_path):
    source = gzip.compress((
        "# CC-CEDICT test\n"
        "中國 中国 [Zhong1 guo2] /China/\n"
        "中 中 [zhong1] /middle/center/\n"
        "中 中 [zhong4] /to hit/\n"
        "女 女 [nu:3] /female/\n"
        "ABC ABC [ABC] /letters/\n"
        "abc abc [abc] /another sense/\n"
        "𠀀 𠀀 [he1] /rare character/\n"
    ).encode())
    manifest = cedict.build(source, b"license fixture", tmp_path)
    entries = read_entries(tmp_path)
    assert entries["中国"] == entries["中國"] == "Zhōng guó\nChina"
    assert entries["中"] == "zhōng\nmiddle; center\n\nzhòng\nto hit"
    assert entries["女"] == "nǚ\nfemale"
    assert entries["ABC"] == "ABC\nletters\n\nabc\nanother sense"
    assert list(entries) == sorted(entries, key=cedict.sort_key)
    assert manifest["source_entries"] == 7
    assert manifest["indexed_headwords"] == 6
    assert (tmp_path / "LICENSE.txt").read_bytes() == b"license fixture"
    assert "CC BY-SA 4.0" in (tmp_path / "ATTRIBUTION.txt").read_text()
    assert (tmp_path / "cedict-source.txt.gz").read_bytes() == source


def test_tone_placement():
    assert cedict.tone_marks("liu2 gui4 ou3 shui3 lve4 nu:3 ma5") == "liú guì ǒu shuǐ lüè nǚ ma"
    assert cedict.tone_marks("Ai4 Lu:4 ng2 m4") == "Ài Lǜ ńg m̀"


def test_malformed_entry_rejected(tmp_path):
    import pytest
    with pytest.raises(ValueError, match="line 1"):
        cedict.build(gzip.compress(b"bad entry\n"), b"license", tmp_path)
