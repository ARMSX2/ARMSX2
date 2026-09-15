"""Paths and text helpers for the iOS source tests."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SWIFT = ROOT / "platforms/ios/app/src/main/swift"
CPP = ROOT / "platforms/ios/app/src/main/cpp"


def read(path):
    return Path(path).read_text(encoding="utf-8")


def without_comments(text):
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def at(text, needle, what, start=0):
    found = text.find(needle, start)
    if found < 0:
        raise AssertionError("%s: could not find %r" % (what, needle))
    return found


def block(text, needle):
    start = text.find(needle)
    opening = text.find("{", start)
    if start >= 0 and opening >= 0:
        depth = 0
        for i in range(opening, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
    raise AssertionError("could not find a balanced block at %r" % needle)
