"""Pause menu panels take the pad back after a touch."""

import unittest

from ios_source import SWIFT, block, read, without_comments


NAVIGATION = SWIFT / "Models/ControllerAccessibilityNavigation.swift"
CHEATS = SWIFT / "Views/CheatsPatchesManagerView.swift"


class PadAfterTouch(unittest.TestCase):
    """A touch retires every session in the router. Main menu pages get a nil router in
    touch mode and register again when it returns, but the pause menu panels keep theirs,
    so Shaders, Save States and the skin picker stayed dead after one touch."""

    def test_sessions_register_again_when_the_pad_returns(self):
        modifier = block(
            without_comments(read(NAVIGATION)),
            "private struct ControllerAccessibilityNavigationModifier",
        )
        change = modifier.find(".onChange(of: controllerInput?.isControllerNavigationEnabled)")
        self.assertGreaterEqual(change, 0)
        self.assertIn("updateRegistration()", modifier[change:change + 200])

    def test_cheats_takes_its_capture_back(self):
        source = without_comments(read(CHEATS))
        change = source.find(".onChange(of: controllerInput?.isControllerNavigationEnabled)")
        self.assertGreaterEqual(change, 0)
        self.assertIn("setNavigationCaptured(\n                    true", source[change:change + 300])


if __name__ == "__main__":
    unittest.main()
