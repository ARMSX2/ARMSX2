"""The OrbitKeys keyboard owns the controller while it is open, from every host."""

import unittest

from ios_source import SWIFT, block, read, without_comments


HOST = SWIFT / "Views/OrbitKeys/OrbitKeysKeyboardHost.swift"


class KeyboardCapturesTheRouter(unittest.TestCase):
    """Opened from Save Custom Theme or the Theme Gallery, the keyboard held no capture, so
    Circle also reached the Settings page underneath and popped it with the typed name."""

    @classmethod
    def setUpClass(cls):
        cls.host = without_comments(read(HOST))

    def test_opening_claims_the_router(self):
        self.assertIn("setNavigationCaptured(true)", block(self.host, "private func openKeyboard("))

    def test_closing_and_disappearing_release_it(self):
        for function in ("private func handleVisibilityChange(", "private func releaseResources("):
            with self.subTest(function=function):
                self.assertIn("setNavigationCaptured(false)", block(self.host, function),
                              f"{function} keeps the keyboard's capture")

    def test_the_capture_has_its_own_owner(self):
        helper = block(self.host, "private func setNavigationCaptured(")
        self.assertIn("MenuControllerNavigationCaptureOwner.orbitKeysKeyboard", helper)


if __name__ == "__main__":
    unittest.main()
