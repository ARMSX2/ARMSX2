import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
SWIFT = ROOT / "platforms/ios/app/src/main/swift"
QUICK_MENU = SWIFT / "Views/QuickMenuView.swift"
GAME_SCREEN = SWIFT / "Views/GameScreenView.swift"

SECTION = "ShaderChainSection("
PANEL = "ShaderControlPanel"
ROUTER = "openPauseMenuChild"
APPLY = "applyGraphicsSettingsNow"
BRIDGE = "ARMSX2Bridge"


def without_comments(source):
    """What the compiler sees, so prose naming a symbol cannot satisfy a check."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def type_body(source, name):
    match = re.search(r"(?ms)^private struct " + name + r": View \{.*?\n\}\n", source)
    return match.group(0) if match else None


def func_body(source, name):
    match = re.search(r"(?ms)^    private func " + name + r"\(.*?\n    \}\n", source)
    return match.group(0) if match else None


class QuickMenuShaderRoutePolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.quick_menu = without_comments(QUICK_MENU.read_text(encoding="utf-8"))
        cls.game_screen = without_comments(GAME_SCREEN.read_text(encoding="utf-8"))

    def test_the_section_is_not_mounted_inside_the_pause_card(self):
        self.assertNotIn(
            SECTION, self.quick_menu,
            "the shader section is being constructed inside the pause card. Its body is two "
            "Sections and it embeds a NavigationLink to the preset browser, while the Quick Menu "
            "is OverlaySectionCard rows with no navigation stack anywhere in it: the rows lay out "
            "wrong and the Preset row becomes a link that renders but goes nowhere.")

    def test_the_hosted_panel_gives_the_section_a_stack_and_a_form(self):
        panel = type_body(self.game_screen, PANEL)
        self.assertIsNotNone(
            panel, f"{PANEL} is gone, so the Shaders route has nothing to present and the pause "
            "menu row opens an empty sheet.")

        for token in ("NavigationStack {", "Form {", SECTION):
            with self.subTest(token=token):
                self.assertIn(
                    token, panel,
                    f"{PANEL} no longer contains {token!r}. The embedded preset browser is a "
                    "NavigationLink and needs the stack; the section only lays out inside a Form "
                    "or a List. Losing either leaves a sheet that still draws.")

        self.assertLess(
            panel.index("NavigationStack {"), panel.index("Form {"),
            f"{PANEL} nests its Form outside the NavigationStack, so the Preset row has no stack "
            "to push the browser onto.")
        self.assertLess(
            panel.index("Form {"), panel.index(SECTION),
            f"{PANEL} mounts the section outside its Form, where Section-shaped rows do not lay "
            "out.")

    def test_the_route_table_stays_exhaustive(self):
        router = func_body(self.game_screen, ROUTER)
        self.assertIsNotNone(router, f"{ROUTER} is gone; update this test")
        self.assertNotRegex(
            router, r"(?m)^\s*default:",
            f"{ROUTER} grew a default arm. GameScreenView.swift:1008-1010 records that its "
            "exhaustiveness is the only thing stopping a new destination routing to "
            ".pausedPresenting with no view presenting it, which strands the overlay.")
        self.assertIn(
            ".shaders", router,
            "the Shaders destination is no longer routed, so the pause menu row is inert.")

    def test_neither_file_applies_graphics_by_hand(self):
        for path, source in ((QUICK_MENU, self.quick_menu), (GAME_SCREEN, self.game_screen)):
            with self.subTest(path=path.name):
                self.assertNotIn(
                    APPLY, source,
                    f"{path.name} calls {APPLY} directly. Both shader settings live in EmuCore/GS, "
                    "so Setting.appliesGraphics is true and SettingsStore.commit already coalesces "
                    "an apply over 120 ms: a hand-written one is a second, uncoalesced reload on "
                    "every write.")

    def test_the_pause_card_takes_its_capabilities_from_the_host(self):
        self.assertNotIn(
            BRIDGE, self.quick_menu,
            f"QuickMenuView reaches for {BRIDGE}. It is a presentation view that takes every "
            "capability as a let from GameScreenView, so a bridge call inside it cannot be driven "
            "from a preview and cannot be substituted by the host.")


if __name__ == "__main__":
    unittest.main()
