"""The per-game settings dictionary is matched by string key, not by type.

PerGameSettingsPanel.save() builds it and ARMSX2WriteGameSettingsForIdentity
reads it back one key at a time. A misspelled key still compiles and still
runs, and the value falls back to whatever that key's default is, so nothing
reports it. These tests compare the key sets instead.
"""

import re
import unittest

from ios_source import CPP, SWIFT, block, read


BRIDGE_MM = CPP / "ARMSX2Bridge.mm"
PANEL = SWIFT / "Views/PerGameSettingsPanel.swift"

WRITER = "static void ARMSX2WriteGameSettingsForIdentity"
BUILDER = "static NSMutableDictionary<NSString*, id>* ARMSX2BuildGlobalGameSettingsResult"
OVERRIDES = "static void ARMSX2ApplyPerGameSettingsOverrides"
SWIFT_DICT = "let settingsDict: [String: Any] = ["


def setter_keys():
    """Keys ARMSX2WriteGameSettingsForIdentity reads out of the dictionary."""
    return set(re.findall(r's\[@"([A-Za-z]+)"\]', block(read(BRIDGE_MM), WRITER)))


def getter_keys():
    """Keys gameSettingsForISO puts into the dictionary it returns."""
    source = read(BRIDGE_MM)
    body = block(source, BUILDER) + block(source, OVERRIDES)
    return (set(re.findall(r'@"([A-Za-z]+)"\s*:', body))
            | set(re.findall(r'result\[@"([A-Za-z]+)"\]', body)))


def swift_keys():
    """Keys PerGameSettingsPanel.save() puts into the dictionary it sends."""
    source = read(PANEL)
    start = source.index(SWIFT_DICT)
    return set(re.findall(r'"([A-Za-z]+)"\s*:', source[start:source.index("\n        ]", start)]))


class PerGameSettingsKeys(unittest.TestCase):
    def test_the_keys_were_actually_found(self):
        # The checks below all pass on an empty set, so count the keys first.
        self.assertGreater(len(setter_keys()), 30)
        self.assertGreater(len(swift_keys()), 30)
        self.assertGreater(len(getter_keys()), 30)

    def test_swift_writes_exactly_what_the_bridge_reads(self):
        sent, read_back = swift_keys(), setter_keys()
        self.assertEqual(sent - read_back, set(), "Swift sends keys the bridge ignores")
        self.assertEqual(read_back - sent, set(), "the bridge reads keys Swift never sends")

    def test_the_getter_can_be_fed_back_to_the_setter(self):
        # setGameSettings(gameSettings(forISO:), forISO:) is an easy call to write.
        # A key the setter reads but the getter leaves out arrives as nil, and for
        # an override flag nil means delete.
        missing = setter_keys() - getter_keys()
        self.assertEqual(missing, set(), "getter does not emit: %s" % sorted(missing))


if __name__ == "__main__":
    unittest.main()
