"""OrbitKeys haptics follow Haptic Feedback and UI Rumble Strength."""

import unittest

from ios_source import SWIFT, block, read, without_comments


MODEL = SWIFT / "Views/OrbitKeys/OrbitKeysModel.swift"


class KeyboardHapticsGoThroughTheRouter(unittest.TestCase):
    """The keyboard ran its own generators and controller engines, so every key buzzed
    the phone and the pad with Haptic Feedback off or UI Rumble at 0%."""

    def test_no_generators_or_engines_of_its_own(self):
        model = without_comments(read(MODEL))
        for own in ("FeedbackGenerator(", "CHHapticEngine", "createEngine("):
            with self.subTest(own=own):
                self.assertNotIn(own, model, "the keyboard plays haptics outside the settings again")

    def test_feedback_goes_through_play_touch_haptics(self):
        haptics = block(without_comments(read(MODEL)), "struct OrbitKeysHaptics")
        self.assertIn("router?.playTouchHaptics(", haptics)


if __name__ == "__main__":
    unittest.main()
