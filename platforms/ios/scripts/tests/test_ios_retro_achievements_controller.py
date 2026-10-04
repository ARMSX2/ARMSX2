"""The pause menu's RetroAchievements panel works with a controller."""

import unittest

from ios_source import block, read, swift, without_comments


GAME_SCREEN = swift("GameScreenView.swift")


class RetroAchievementsController(unittest.TestCase):
    """The panel had no controller session, so neither the D-pad nor Circle did anything."""

    @classmethod
    def setUpClass(cls):
        cls.panel = block(without_comments(read(GAME_SCREEN)), "private struct RetroAchievementsGamePanel")

    def test_it_has_a_session_that_circle_closes(self):
        self.assertIn('scopeKey: "runtime.retro-achievements"', self.panel)
        self.assertIn("onBack: {\n                close()", self.panel)

    def test_each_achievement_is_a_target(self):
        self.assertIn('id: "runtime.retro-achievements.\\(entry.id)"', self.panel)


if __name__ == "__main__":
    unittest.main()
