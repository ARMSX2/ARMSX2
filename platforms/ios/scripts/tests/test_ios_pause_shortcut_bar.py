"""The controller shortcut bar shows under the pause menu and goes the moment the menu closes."""

import unittest

from ios_source import block, read, swift, without_comments


GAME = swift("GameScreenView.swift")
HOST = swift("GameplayOverlayHost.swift")
CONTAINER = swift("GameOverlayContainer.swift")


class PauseShortcutBar(unittest.TestCase):
    """A tester asked for the start-of-game shortcut bar in the Quick Menu too, gone at once
    when it closes, and only with a controller connected."""

    def test_the_bar_sits_above_the_pause_menu_without_a_fade(self):
        host = without_comments(read(HOST))
        menu = host.find(".overlay {\n                runtimeQuickMenuOverlay\n            }")
        bar = host.find("overPauseMenu: true")
        self.assertGreater(menu, -1)
        self.assertGreater(bar, menu)
        self.assertIn("if overlay.overlayRoute == .paused {", host[menu:bar])
        self.assertIn(".transition(.identity)", host[bar:bar + 300])

    def test_the_start_bar_steps_aside_while_paused(self):
        self.assertIn("if showsGameplayControllerShortcutHelp, overlay.overlayRoute == .hidden {",
                      without_comments(read(GAME)))

    def test_the_card_leaves_room_only_with_a_controller(self):
        pause = block(without_comments(read(HOST)), "private var pauseMenuOverlay")
        self.assertIn("? GameplayControllerShortcutHelpOverlay.pauseCardReserve : 0", pause)
        container = without_comments(read(CONTAINER))
        self.assertIn("bottomReserve + barBase - cardMargin", container)
        self.assertIn(".offset(y: -lift / 2)", container)


if __name__ == "__main__":
    unittest.main()
