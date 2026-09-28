"""When the focused row leaves the declared order, focus moves to the row that replaced it."""

import unittest

from ios_source import MODELS, block, read, without_comments


NAVIGATION = MODELS / "ControllerAccessibilityNavigation.swift"


class FocusReplacement(unittest.TestCase):
    """After saving to an empty slot, Save turns into Load and Overwrite, but the ring stayed on
    the empty spot: explicit targets persist after unmounting, and nothing moved focus off one
    that had left the order for good."""

    def test_a_dropped_focus_is_replaced(self):
        source = without_comments(read(NAVIGATION))
        order = block(source, "func setDeclaredOrder(")
        self.assertIn("previousOrder.contains(focusedKey)", order)
        self.assertIn("refocusReplacement(of: focusedKey", order)
        refocus = block(source, "private func refocusReplacement(")
        self.assertIn("self.focusedKey == droppedKey", refocus)
        self.assertIn("mounted.filter(added.contains)", refocus)


if __name__ == "__main__":
    unittest.main()
