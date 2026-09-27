"""Library cards follow Larger Text, and the app's own prompts behave as modals."""

import unittest

from ios_source import SWIFT, block, read, without_comments


LIBRARY = SWIFT / "Views/GameListView.swift"
OVERLAYS = SWIFT / "Views/ControllerGameContextMenu.swift"


class CardTextFollowsLargerText(unittest.TestCase):
    """Card titles and details moved to fixed point sizes, so at the largest accessibility
    size a title .body drew 355 pt wide stayed at 163 pt."""

    def test_both_card_scales_include_the_text_size(self):
        library = without_comments(read(LIBRARY))
        self.assertIn("@ScaledMetric(relativeTo: .body) private var dynamicTypeTextScale", library)
        for scale in ("private var resolvedGameNameTextScale", "private var resolvedGameInfoTextScale"):
            with self.subTest(scale=scale):
                self.assertIn("* dynamicTypeTextScale", block(library, scale))

    def test_the_library_stops_growing_at_ax3(self):
        root = without_comments(read(SWIFT / "Views/RootView.swift"))
        self.assertEqual(root.count(".dynamicTypeSize(...DynamicTypeSize.accessibility3)"), 2,
                         "a GameListView is placed without the AX3 cap")


class PromptsAreModal(unittest.TestCase):
    """The prompts that used to be system alerts left VoiceOver free to move into the
    library behind them, and the scrub gesture did not close them."""

    def test_the_alert_and_the_context_menu(self):
        overlays = without_comments(read(OVERLAYS))
        for view in ("struct ControllerGameContextMenu: View", "struct ControllerNavigationAlert: View"):
            with self.subTest(view=view):
                body = block(overlays, view)
                self.assertIn(".accessibilityAddTraits(.isModal)", body)
                self.assertIn(".accessibilityAction(.escape)", body)


if __name__ == "__main__":
    unittest.main()
