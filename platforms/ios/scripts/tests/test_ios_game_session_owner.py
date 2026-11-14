"""The game lifecycle lives in GameSession. AppState keeps the screen, chrome and hinge."""

import unittest

from ios_source import read, swift, without_comments


APP_STATE = swift("AppState.swift")
SESSION = swift("GameSession.swift")
LIFECYCLE = ("runningGameName", "func bootGame(", "func bootBIOSOnly(", "func stopGame(",
             "func shutdownAndBoot(", "func resetCurrentVM(", "func returnToMenu(",
             "func returnToGame(", "ARMSX2iOSVMDidShutdown")


class GameSessionOwnsTheLifecycle(unittest.TestCase):
    """AppState held the running game, boot, stop and restart beside the screen state."""

    @classmethod
    def setUpClass(cls):
        cls.state = without_comments(read(APP_STATE))
        cls.session = without_comments(read(SESSION))

    def test_app_state_holds_none_of_it(self):
        for needle in LIFECYCLE:
            with self.subTest(needle=needle):
                self.assertNotIn(needle, self.state)
                self.assertIn(needle, self.session)

    def test_app_state_never_reaches_the_session(self):
        """The session calls AppState to change the screen, never the other way."""
        self.assertNotIn("GameSession", self.state)


if __name__ == "__main__":
    unittest.main()
