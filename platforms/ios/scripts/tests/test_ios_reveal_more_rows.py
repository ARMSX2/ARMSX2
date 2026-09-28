"""Up and Down scroll on past rows the pad can't focus."""

import unittest

from ios_source import SWIFT, block, read, without_comments


NAVIGATION = SWIFT / "Models/ControllerAccessibilityNavigation.swift"


class RevealMoreRows(unittest.TestCase):
    """In landscape, Graphics stopped at Texture Offset Y: the Skipdraw rows below it are
    disabled, and the next control had not mounted yet, so Down had nowhere to go."""

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(NAVIGATION))

    def test_the_edge_scrolls_before_it_gives_up(self):
        self.assertIn("return revealMoreRows(direction) || wrapToOtherEnd(direction)", self.source)

    def test_it_only_runs_where_nothing_else_can_seek(self):
        reveal = block(self.source, "private func revealMoreRows(")
        self.assertIn("declaredOrder.isEmpty", reveal)
        self.assertIn("isRightStickScrolling != true", reveal)
        self.assertIn("if isRevealingRows {", reveal)


if __name__ == "__main__":
    unittest.main()
