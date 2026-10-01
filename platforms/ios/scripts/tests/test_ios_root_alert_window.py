"""App prompts show in their own window, above the pause menu's sheets."""

import unittest

from ios_source import SWIFT, block, read, without_comments


ROOT_VIEW = SWIFT / "Views/RootView.swift"


class RootAlertWindow(unittest.TestCase):
    """"Use Controller Navigation?" opened behind Save States, Speed, RetroAchievements and
    Cheats, so the pad looked dead."""

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(ROOT_VIEW))

    def test_prompts_render_through_the_window(self):
        self.assertIn("RootAlertWindow(content: rootControllerAlert)", self.source)
        self.assertEqual(self.source.count("ControllerNavigationAlert("), 1)
        self.assertIn("ControllerNavigationAlert(", block(self.source, "private var rootControllerAlert: AnyView?"))

    def test_the_window_sits_above_and_never_takes_key(self):
        anchor = block(self.source, "private final class RootAlertWindowAnchor")
        self.assertIn("alertWindow.windowLevel = .alert", anchor)
        self.assertNotIn("makeKey", anchor)

    def test_it_reads_the_environment_of_the_old_layer(self):
        # A background placed after these modifiers is their sibling and gets none of them.
        window = self.source.find("RootAlertWindow(content: rootControllerAlert)")
        router = self.source.find(".environment(\\.menuControllerInputRouter, menuControllerInput)")
        self.assertLess(window, router)
        self.assertIn(".environment(\\.self, context.environment)", self.source)


if __name__ == "__main__":
    unittest.main()
