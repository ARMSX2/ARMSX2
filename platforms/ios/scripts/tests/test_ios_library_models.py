"""Library cards draw from plain values, and the library and tab bar prefs live in two models."""

import re
import unittest

from ios_source import SWIFT, block, read, swift, without_comments

MODELS = ("GameLibraryAppearance.swift", "GameLibraryAppearance+ViewOptions.swift", "MenuTabBarAppearance.swift")
CARDS = {
    "GameLibraryRowCards.swift": ("struct GameLibraryTouchRow", "struct GameLibraryControllerRow"),
    "GameLibraryGridCards.swift": ("struct GameLibraryTouchGridCard", "struct GameLibraryControllerGridCard"),
    "GameLibraryCoverFlowCard.swift": ("struct GameLibraryCoverFlowCard",),
}


class CardsTakeValues(unittest.TestCase):
    """Card builders read the whole view, so one favorite redrew every card."""

    def test_no_card_reads_a_model(self):
        for name, structs in CARDS.items():
            source = without_comments(read(swift(name)))
            for needle in structs:
                with self.subTest(card=needle):
                    body = block(source, needle)
                    for model in ("GameLibraryModel", "SettingsStore", ".shared"):
                        self.assertNotIn(model, body)


class PrefsLiveInTheModels(unittest.TestCase):
    """An @AppStorage copy of a key misses writes that go through the model."""

    def test_no_view_keeps_a_copy(self):
        keys = set()
        for name in MODELS:
            keys |= set(re.findall(r'forKey: "(ARMSX2iOS\w+)"', read(swift(name))))
        self.assertEqual(len(keys), 63)
        for path in SWIFT.rglob("*.swift"):
            if path.name in MODELS:
                continue
            text = without_comments(read(path))
            for key in re.findall(r'@AppStorage\(\s*"(ARMSX2iOS\w+)"', text) + \
                    re.findall(r'forKey: "(ARMSX2iOS\w+)"', text):
                with self.subTest(file=path.name, key=key):
                    self.assertNotIn(key, keys)


if __name__ == "__main__":
    unittest.main()
