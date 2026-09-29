"""Save States scrolls a focused row out from under its header, and its list ends stop."""

import unittest

from ios_source import MODELS, SWIFT, block, read, without_comments


NAVIGATION = MODELS / "ControllerAccessibilityNavigation.swift"
PANEL = SWIFT / "Views/SaveStatesPanel.swift"


class SaveStatesScroll(unittest.TestCase):
    """After scrolling down and back up, Slot 1 stayed behind the title bar with the focus ring
    drawn over it: the visibility check used the scroll view's bounds, which run underneath the
    navigation bar and the pinned description."""

    def test_visibility_leaves_out_the_content_insets(self):
        check = block(without_comments(read(NAVIGATION)), "private func isFullyVisible")
        self.assertIn("scrollViewportFrame(in: scrollView)", check)
        self.assertNotIn("origin: scrollView.contentOffset", check)

    def test_the_first_and_last_slots_wrap_only_on_a_new_press(self):
        graph = block(without_comments(read(PANEL)), "init(rows: [[Target]], wraps: Bool = true)")
        self.assertIn("requiresFreshPress: true", graph)
        self.assertIn("nearest(in: above, to: target.column)", graph)
        self.assertIn("confinesHorizontalFocusMovement: true", read(PANEL))


if __name__ == "__main__":
    unittest.main()
