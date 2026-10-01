"""Stop asks in one anchored confirmation that keeps controller input and suits iOS 17.4 to 27."""

import unittest

from ios_source import SWIFT, block, read, without_comments


MENU = SWIFT / "Views/ControllerGameContextMenu.swift"
LIBRARY = SWIFT / "Views/GameListView.swift"
QUICK_MENU = SWIFT / "Views/QuickMenuView.swift"


class StopConfirmation(unittest.TestCase):
    """Testers found the Stop prompt unreadable: a large panel over busy cover art whose
    buttons did not look like buttons. It is now one small panel under the stop button."""

    @classmethod
    def setUpClass(cls):
        cls.view = block(without_comments(read(MENU)), "struct StopGameConfirmation: View")

    def test_both_stop_prompts_use_it(self):
        for path in (LIBRARY, QUICK_MENU):
            with self.subTest(path=path.name):
                source = without_comments(read(path))
                self.assertIn("StopGameConfirmation(", source)
                self.assertIn(".onPreferenceChange(StopConfirmationAnchorKey.self)", source)

    def test_it_is_glass_on_ios_26_and_frosted_before(self):
        self.assertIn("if #available(iOS 26, *)", self.view)
        self.assertIn("glassSurface(clear: true, cornerRadius: 28)", self.view)
        self.assertIn("OverlayFrostBackground()", self.view)

    def test_controller_input_keeps_the_alert_contract(self):
        for target in ('"alert.action.stop"', '"alert.action.cancel"'):
            with self.subTest(target=target):
                self.assertIn(target, self.view)
        self.assertIn("selectedIndex == index", self.view)
        self.assertIn(".accessibilityAction(.escape)", self.view)


if __name__ == "__main__":
    unittest.main()
