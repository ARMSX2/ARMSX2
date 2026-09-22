import unittest

from ios_source import SWIFT, block, without_comments


QUICK_MENU = SWIFT / "Views/QuickMenuView.swift"
GAME_SCREEN = SWIFT / "Views/GameScreenView.swift"

SECTION = "ShaderChainSection("
PANEL = "ShaderControlPanel"
ROUTER = "openPauseMenuChild"
APPLY = "applyGraphicsSettingsNow"
BRIDGE = "ARMSX2Bridge"


class QuickMenuShaderRoutePolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.quick_menu = without_comments(QUICK_MENU.read_text(encoding="utf-8"))
        cls.game_screen = without_comments(GAME_SCREEN.read_text(encoding="utf-8"))

    def test_the_section_is_not_mounted_inside_the_pause_card(self):
        self.assertNotIn(
            SECTION, self.quick_menu,
            "QuickMenuView builds ShaderChainSection, which needs a Form and a NavigationStack "
            "the pause card does not have")

    def test_the_hosted_panel_gives_the_section_a_stack_and_a_form(self):
        panel = block(self.game_screen, f"private struct {PANEL}: View")

        for token in ("NavigationStack {", "Form {", SECTION):
            with self.subTest(token=token):
                self.assertIn(
                    token, panel,
                    f"{PANEL} does not contain {token!r}, which ShaderChainSection needs")

        self.assertLess(
            panel.index("NavigationStack {"), panel.index("Form {"),
            f"{PANEL} opens its Form outside the NavigationStack")
        self.assertLess(
            panel.index("Form {"), panel.index(SECTION),
            f"{PANEL} mounts ShaderChainSection outside its Form")

    def test_the_route_table_stays_exhaustive(self):
        router = block(self.game_screen, f"private func {ROUTER}(")
        self.assertNotRegex(
            router, r"(?m)^\s*default:",
            f"{ROUTER} has a default: arm, so a new destination can reach .pausedPresenting "
            "with no view for it")
        self.assertIn(
            ".shaders", router,
            "openPauseMenuChild does not route .shaders")

    def test_neither_file_applies_graphics_by_hand(self):
        for path, source in ((QUICK_MENU, self.quick_menu), (GAME_SCREEN, self.game_screen)):
            with self.subTest(path=path.name):
                self.assertNotIn(
                    APPLY, source,
                    f"{path.name} calls {APPLY}, but SettingsStore.commit already applies "
                    "EmuCore/GS settings")

    def test_the_pause_card_takes_its_capabilities_from_the_host(self):
        self.assertNotIn(
            BRIDGE, self.quick_menu,
            f"QuickMenuView calls {BRIDGE} instead of taking the capability from GameScreenView")


if __name__ == "__main__":
    unittest.main()
