"""Custom library titles survive a reinstall."""

import re
import unittest

from ios_source import SWIFT, block, read, without_comments


class CustomTitleKey(unittest.TestCase):
    """Titles were keyed by the absolute path, which moves with the app container on a
    reinstall, so every custom title was lost. Favorites already use the boot name."""

    @classmethod
    def setUpClass(cls):
        cls.library = without_comments(read(SWIFT / "Views/GameListView.swift"))

    def test_rename_keys_by_the_boot_name(self):
        self.assertIn("for: game.bootName", block(self.library, "private func renameGame("),
                      "a rename is stored under a key that changes on reinstall")

    def test_the_lookup_uses_the_same_key(self):
        self.assertRegex(self.library,
                         r"GameDisplayNameStore\.shared\.displayName\(\s*"
                         r"for: record\.external \? record\.path : record\.name,",
                         "the library looks titles up under a different key than rename")


if __name__ == "__main__":
    unittest.main()
