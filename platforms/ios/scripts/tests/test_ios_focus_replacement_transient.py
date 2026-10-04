"""A focused row that leaves the order only for a moment keeps its focus."""

import unittest

from ios_source import block, read, swift, without_comments


NAVIGATION = swift("ControllerAccessibilityNavigation.swift")


class TransientOrderChange(unittest.TestCase):
    """Storage takes its Clear buttons out of the order while it cleans. The replacement rule,
    meant for Save turning into Load and Overwrite, moved focus to Add External Game Folder."""

    def test_a_row_still_on_screen_is_left_alone(self):
        refocus = block(without_comments(read(NAVIGATION)), "private func refocusReplacement(")
        self.assertIn("if self.targets[droppedKey]?.view.value?.window != nil { return }", refocus)


if __name__ == "__main__":
    unittest.main()
