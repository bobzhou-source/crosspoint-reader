import importlib.util
import io
from pathlib import Path
import sys
import zipfile

SCRIPTS = Path(__file__).parents[1] / "scripts"


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(module)
    return module


english = load("build_english_dict")
verify = load("verify_stardict")

LICENSE_HEADER = "  1 This software and database is being provided under a test license.\n"
DATA = {
    "noun": ["00000001 05 n 01 child 0 000 | a young person; \"the children played\"",
             "00000002 05 n 01 doe 0 000 | mature female of mammals of which the male is called buck",
             "00000003 05 n 02 book 0 volume 0 000 | a written work or composition"],
    "verb": ["00000004 30 v 01 go 0 000 | change location; move",
             "00000005 30 v 01 do 0 000 | engage in",
             "00000006 30 v 01 study 0 000 | be a student of a certain subject"],
    "adj": ["00000007 00 a 01 good 0 000 | having desirable or positive qualities"],
    "adv": ["00000008 02 r 01 well 0 000 | in a good or proper manner"],
}
INDEX = {
    "noun": ["book n 1 0 1 0 00000003", "child n 1 0 1 0 00000001", "doe n 1 0 1 0 00000002",
             "volume n 1 0 1 0 00000003"],
    "verb": ["do v 1 0 1 0 00000005", "go v 1 0 1 0 00000004", "study v 1 0 1 0 00000006"],
    "adj": ["good a 1 0 1 0 00000007"],
    "adv": ["well r 1 0 1 0 00000008"],
}


def archive(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as output:
        for name, text in files.items():
            output.writestr(f"dict/{name}", text)
    return buffer.getvalue()


def build(tmp_path):
    oewn = archive({
        **{f"data.{name}": (LICENSE_HEADER if name == "noun" else "") + "\n".join(lines) + "\n"
           for name, lines in DATA.items()},
        **{f"index.{name}": "\n".join(lines) + "\n" for name, lines in INDEX.items()},
        "verb.exc": "went go\n", "noun.exc": "children child\n", "adj.exc": "better good\n",
        "adv.exc": "better well\n",
        # SemCor tag counts order competing lemmas: the frequent verb "do" before "doe".
        "cntlist.rev": "go%2:38:00:: 1 9\nchild%1:18:00:: 1 4\ndo%2:41:00:: 1 30\n",
    })
    wordnet30 = archive({"verb.exc": "gone go\n", "noun.exc": "", "adj.exc": "", "adv.exc": ""})
    cmudict = archive({"cmudict": ";;; test\nDOES  D AH1 Z\nSTUDIED  S T AH1 D IY0 D\nBOOKS  B UH1 K S\n"})
    manifest = english.build(oewn, wordnet30, cmudict, tmp_path)
    return manifest, verify.StarDict(tmp_path)


def test_irregular_and_regular_forms_reach_their_lemmas_like_the_firmware(tmp_path):
    manifest, dictionary = build(tmp_path)
    assert verify.structural_checks(dictionary)["problems"] == []
    assert dictionary.lookup("went")[0::2] == ("go", "syn")
    assert dictionary.lookup("gone")[0::2] == ("go", "syn")
    assert dictionary.lookup("children")[0::2] == ("child", "syn")
    assert dictionary.lookup("books")[0::2] == ("book", "stem:book")
    headword, definition, path = dictionary.lookup("“Better,”")
    assert (headword, path) == ("better", "exact")
    assert definition.splitlines()[0] == "Form of: good (adjective), well (adverb)"
    # A .syn row for one reading would hide the other, so both lemmas share a headword.
    headword, definition, path = dictionary.lookup("does")
    assert (headword, path) == ("does", "exact")
    assert definition.splitlines()[0] == "Form of: do (verb), doe (noun)"
    # "es"/"ed" stripping would land on "studi": the regular form needs its own .syn row.
    assert dictionary.lookup("studied")[0::2] == ("study", "syn")


def test_definitions_are_plain_pos_labelled_lines_and_the_license_ships(tmp_path):
    manifest, dictionary = build(tmp_path)
    headword, definition, _ = dictionary.lookup("book")
    assert definition == "(noun)\n1. a written work or composition [= volume]"
    assert "\n\n" not in dictionary.lookup("child")[1]
    assert dictionary.lookup("the")[1].startswith("(article)")
    assert dictionary.ifo["sametypesequence"] == "m"
    assert int(dictionary.ifo["wordcount"]) == manifest["indexed_headwords"]
    assert "test license" in (tmp_path / "LICENSE.txt").read_text()
