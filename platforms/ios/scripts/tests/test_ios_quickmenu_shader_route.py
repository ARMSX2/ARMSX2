import unittest

from ios_source import SWIFT, at, block, without_comments


QUICK_MENU = SWIFT / "Views/QuickMenuView.swift"
GAME_SCREEN = SWIFT / "Views/GameScreenView.swift"
WORKSPACE = SWIFT / "Views/Settings/ShaderWorkspaceView.swift"

SECTION = "ShaderChainSection("
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

    def test_the_hosted_workspace_mounts_the_section_in_a_form(self):
        """In game, .shaders opens the per-game panel on its shader workspace. The section
        pushes nothing (test_ios_shader_catalog), so a Form is all it needs."""
        self.assertIn("initiallySelectsShaders:", self.game_screen,
                      "the in-game shader route no longer opens the shader workspace")
        workspace = block(without_comments(WORKSPACE.read_text(encoding="utf-8")),
                          "private var managementContent")
        form = at(workspace, "Form {", "the workspace Form")
        for mount in ("PerGameShaderSection(", SECTION):
            with self.subTest(mount=mount):
                self.assertLess(form, at(workspace, mount, mount),
                                f"the workspace mounts {mount} outside its Form")

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
