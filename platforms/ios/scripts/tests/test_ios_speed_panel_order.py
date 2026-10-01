"""The Speed panel moves in the order it is drawn."""

import unittest

from ios_source import SWIFT, block, read, without_comments


GAME_SCREEN = SWIFT / "Views/GameScreenView.swift"


class SpeedPanelOrder(unittest.TestCase):
    """Done sits in the toolbar above the rows but was last in the order, so Up from Done
    went down to Fast Forward Speed and Down from it jumped back up to Done."""

    def test_done_comes_first(self):
        panel = block(without_comments(read(GAME_SCREEN)), "private struct SpeedControlPanel")
        order = panel[panel.find("controllerTargetOrder = ["):]
        self.assertLess(order.find('"runtime.speed.done"'), order.find('"runtime.speed.fast-forward"'))
        self.assertNotIn("preferredTrailingFocusLabels", panel)


if __name__ == "__main__":
    unittest.main()
