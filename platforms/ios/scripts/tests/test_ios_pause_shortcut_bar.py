"""The controller shortcut bar shows under the pause menu and goes the moment the menu closes."""

import unittest

from ios_source import SWIFT, block, read, without_comments


GAME = SWIFT / "Views/GameScreenView.swift"
CONTAINER = SWIFT / "Views/GameOverlayContainer.swift"


class PauseShortcutBar(unittest.TestCase):
    """A tester asked for the start-of-game shortcut bar in the Quick Menu too, gone at once
    when it closes, and only with a controller connected."""

    def test_the_bar_sits_above_the_pause_menu_without_a_fade(self):
        game = without_comments(read(GAME))
        menu = game.find(".overlay {\n            runtimeQuickMenuOverlay\n        }")
        bar = game.find("if overlayRoute == .paused {\n                GameplayControllerShortcutHelpOverlay(")
        self.assertGreater(bar, menu)
        self.assertIn(".transition(.identity)", game[bar:bar + 300])

    def test_the_start_bar_steps_aside_while_paused(self):
        self.assertIn("if showsGameplayControllerShortcutHelp, overlayRoute == .hidden {",
                      without_comments(read(GAME)))

    def test_the_card_leaves_room_only_with_a_controller(self):
        pause = block(without_comments(read(GAME)), "private var pauseMenuOverlay")
        self.assertIn("? GameplayControllerShortcutHelpOverlay.pauseCardReserve : 0", pause)
        container = without_comments(read(CONTAINER))
        self.assertIn("bottomReserve + barBase - cardMargin", container)
        self.assertIn(".offset(y: -lift / 2)", container)


if __name__ == "__main__":
    unittest.main()
