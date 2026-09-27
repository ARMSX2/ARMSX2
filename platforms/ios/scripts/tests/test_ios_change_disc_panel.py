"""Pause > Change Disc is built like the pause menu, and Back returns focus to its row."""

import unittest

from ios_source import CPP, SWIFT, block, read, without_comments


GAME = SWIFT / "Views/GameScreenView.swift"
QUICK = SWIFT / "Views/QuickMenuView.swift"
BRIDGE = CPP / "ARMSX2Bridge.mm"


class ChangeDiscPanel(unittest.TestCase):
    """Testers saw an opaque black sheet: the old panel put its List in a NavigationStack, which
    paints its own background even when the list's is hidden."""

    def test_the_route_builds_the_new_panel(self):
        route = without_comments(read(GAME))
        route = route[route.find("case .changeDisc:"):][:900]
        self.assertIn("ChangeDiscPanel(", route)
        self.assertIn("variant: metrics.variant", route)
        self.assertIn("driveDisc: ARMSX2Bridge.discInDriveName()", route)
        self.assertNotIn("RuntimeDiscSwapPanel", read(GAME))

    def test_the_panel_uses_the_pause_menu_parts(self):
        panel = block(without_comments(read(QUICK)), "struct ChangeDiscPanel")
        self.assertNotIn("NavigationStack", panel)
        for part in ("OverlayPanelScaffold", "LandscapeCommandBar(", "OverlayHeader(",
                     "QuickMenuFooter(", "Insert Disc (No Reboot)", "Restart With Disc",
                     "\"runtime.disc.close\"", "\"runtime.disc.eject\"",
                     "\"runtime.disc.insert.\\(index)\"", "\"runtime.disc.restart.\\(index)\"",
                     "priority: 320", "orbStyle: .liquidGlass", "navigationBoundary"):
            with self.subTest(part=part):
                self.assertIn(part, panel)

    def test_the_drive_is_read_without_the_ini_fallback(self):
        accessor = block(read(BRIDGE), "+ (nullable NSString *)discInDriveName")
        self.assertIn("VMManager::GetDiscPath()", accessor)
        self.assertNotIn("currentISOPath", accessor)

    def test_back_focuses_the_row_that_opened_the_child(self):
        game = without_comments(read(GAME))
        self.assertIn("pauseMenuChild = destination", block(game, "private func openPauseMenuChild"))
        self.assertIn("returningFrom: pauseMenuChild", game)
        quick = without_comments(read(QUICK))
        self.assertIn("returningFrom.map(controllerTargetID(for:))", quick)


if __name__ == "__main__":
    unittest.main()
