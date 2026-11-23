"""The per-game settings dictionary is typed on the Swift side only.

PerGameSettingsDraft.writeCurrentSettings builds it as [ARMSX2PerGameKey: Any],
and ARMSX2WriteGameSettingsForIdentity reads it back one constant at a time.
The ObjC compiler does not check which constant a read names, so a wrong one
still runs and the value falls back to that key's default. These tests
compare the key sets instead.
"""

import re
import unittest

from ios_source import BRIDGE_DIR, block, bridge_text, read, swift


SAVE = swift("PerGameSettingsDraft+Save.swift")

WRITER = "static void ARMSX2WriteGameSettingsForIdentity"
BUILDER = "static NSMutableDictionary<NSString*, id>* ARMSX2BuildGlobalGameSettingsResult"
OVERRIDES = "static void ARMSX2ApplyPerGameSettingsOverrides"
SWIFT_DICT = "let settingsDict: [ARMSX2PerGameKey: Any] = ["


def raw_values():
    """Each ARMSX2PerGameKey constant name mapped to its string."""
    return dict(re.findall(r'ARMSX2PerGameKey const (ARMSX2PerGameKey\w+) = @"([^"]+)";',
                           read(BRIDGE_DIR / "ARMSX2BridgeKeys.mm")))


def setter_keys():
    """Keys ARMSX2WriteGameSettingsForIdentity reads out of the dictionary."""
    values = raw_values()
    return {values[name] for name in re.findall(r's\[(ARMSX2PerGameKey\w+)\]', block(bridge_text(), WRITER))}


def getter_keys():
    """Keys gameSettingsForISO puts into the dictionary it returns."""
    source = bridge_text()
    body = block(source, BUILDER) + block(source, OVERRIDES)
    names = (re.findall(r'(ARMSX2PerGameKey\w+)\s*:', body)
             + re.findall(r'result\[(ARMSX2PerGameKey\w+)\]', body))
    values = raw_values()
    return {values[name] for name in names}


def swift_keys():
    """Keys PerGameSettingsDraft.writeCurrentSettings puts into the dictionary it sends."""
    source = read(SAVE)
    start = source.index(SWIFT_DICT)
    # each member's Swift name is its raw value
    return set(re.findall(r'\.([A-Za-z0-9]+)\s*:', source[start:source.index("\n        ]", start)]))


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
