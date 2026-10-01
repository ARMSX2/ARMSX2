"""The library's Cheats & Patches panel has no empty band and matches Game Info's height."""

import unittest

from ios_source import SWIFT, block, read, without_comments


CHEATS = SWIFT / "Views/CheatsPatchesManagerView.swift"
LIBRARY = SWIFT / "Views/GameListView.swift"


class CheatsPanelSize(unittest.TestCase):
    """Testers found the panel too large: the right-stick marker sat in the Form as a row of
    its own, which a Form can't shrink below a normal row, so an empty section sat above the
    game title."""

    def test_the_form_starts_with_the_game(self):
        source = without_comments(read(CHEATS))
        form = source[source.find("Form {"):]
        self.assertTrue(form[len("Form {"):].lstrip().startswith("gameSection"))
        self.assertIn("ControllerRightStickScrollTarget(", block(source, "private var gameSection"))

    def test_it_is_as_tall_as_game_info(self):
        source = without_comments(read(LIBRARY))
        cover = source[source.find(".fullScreenCover(item: $cheatsManagerTarget)"):][:400]
        self.assertIn("maximumHeight: 700", cover)


if __name__ == "__main__":
    unittest.main()
