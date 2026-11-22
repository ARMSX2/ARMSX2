"""Paths and text helpers for the iOS source tests."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def _dir(path):
    if not path.is_dir():
        raise AssertionError("%s is missing, so these tests would read nothing" % path)
    return path


IOS_ROOT = _dir(ROOT / "platforms/ios")
BRIDGE_DIR = _dir(IOS_ROOT / "Bridge")
RUNTIME_DIR = _dir(IOS_ROOT / "Runtime")
SWIFT = _dir(IOS_ROOT / "Swift")
ASSETS = _dir(IOS_ROOT / "assets")


def read(path):
    return Path(path).read_text(encoding="utf-8")


def swift(name):
    """Finds a Swift file by name under SWIFT, so tests survive files moving into feature folders."""
    found = sorted(SWIFT.rglob(name))
    if len(found) != 1:
        raise AssertionError("expected one %s under %s, found %d" % (name, SWIFT, len(found)))
    return found[0]


def store_text():
    """SettingsStore and the sibling files its extensions live in."""
    files = sorted(SWIFT.rglob("SettingsStore*.swift"))
    if not files:
        raise AssertionError("no SettingsStore*.swift under %s" % SWIFT)
    return "\n".join(read(p) for p in files)


# Each store's main file, with the text that opens its class and its init.
STORE_INITS = {
    "SettingsStore.swift": ("final class SettingsStore {", "private init()"),
    "SettingsStoreAppearance.swift": ("final class Appearance {", "        init() {"),
    "SettingsStoreController.swift": ("final class Controller {", "        init() {"),
    "SettingsStoreNetwork.swift": ("final class Network {", "        init() {"),
}


def store_inits():
    """(file name, text, class body, init body) for every store in STORE_INITS."""
    found = []
    for name, (class_needle, init_needle) in STORE_INITS.items():
        text = read(swift(name))
        for needle in (class_needle, init_needle):
            count = text.count(needle)
            if count != 1:
                raise AssertionError("%s: expected %r once, found %d" % (name, needle, count))
        found.append((name, text, block(text, class_needle), block(text, init_needle)))
    return found


def bridge_text():
    """Every ARMSX2Bridge*.mm, so a split bridge still reads as one."""
    files = sorted(BRIDGE_DIR.glob("ARMSX2Bridge*.mm"))
    if not files:
        raise AssertionError("no ARMSX2Bridge*.mm under %s" % BRIDGE_DIR)
    return "\n".join(read(p) for p in files)


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
