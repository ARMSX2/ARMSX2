"""Game Info and the Disc Path picker draw the same glass surface as the library's other panels."""

import unittest

from ios_source import SWIFT, block, read, without_comments


LIBRARY = SWIFT / "Views/GameListView.swift"


class GameInfoPanelSurface(unittest.TestCase):
    """Both used to be system sheets. Moved into the library's floating panel, their Forms kept
    the opaque system background, so testers saw a black screen with a large title."""

    def test_each_panel_hides_the_form_background_and_draws_glass(self):
        source = without_comments(read(LIBRARY))
        for name in ("private struct GameInfoPanel", "private struct DiscLinkPicker"):
            with self.subTest(panel=name):
                body = block(source, name)
                self.assertIn(".scrollContentBackground(.hidden)", body)
                self.assertIn(".navigationBarTitleDisplayMode(.inline)", body)
                self.assertIn(".glassSurface(clear: false, cornerRadius: 26)", body)


if __name__ == "__main__":
    unittest.main()
