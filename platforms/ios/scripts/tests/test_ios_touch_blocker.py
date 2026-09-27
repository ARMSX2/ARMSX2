"""The menu's touch blocker never runs during a game."""

import re
import unittest

from ios_source import SWIFT, read, without_comments


class TouchBlockerNeedsTheMenu(unittest.TestCase):
    """Leaving the menu does not clear controller navigation, so after the menu was driven
    with a pad the window-level recognizer kept cancelling touches in game."""

    def test_blocking_requires_an_active_menu(self):
        root = without_comments(read(SWIFT / "Views/RootView.swift"))
        blocks = re.search(r"blocksUnderlyingTouches:(.*?),\n", root, re.S)
        self.assertIsNotNone(blocks)
        self.assertIn("menuControllerInput.isMenuActive", blocks.group(1),
                      "the touch blocker can run while a game is on screen")


if __name__ == "__main__":
    unittest.main()
