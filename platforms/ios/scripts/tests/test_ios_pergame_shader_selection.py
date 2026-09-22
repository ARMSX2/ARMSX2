import unittest

from ios_source import CPP, SWIFT, block, without_comments


BRIDGE_H = CPP / "ARMSX2Bridge.h"
BRIDGE_MM = CPP / "ARMSX2Bridge.mm"
SELECTION = SWIFT / "Models/PerGameShaderSelection.swift"
APP_STATE = SWIFT / "Models/AppState.swift"
PANEL = SWIFT / "Views/PerGameSettingsPanel.swift"
SECTION = SWIFT / "Views/Settings/PerGame/PerGameShaderSection.swift"

STRING_METHODS = (
    "getPerGameINIString:",
    "setPerGameINIString:",
)
RESOLVER = "ShaderPresetLibrary.resolve"
TOKEN_KEY = "ShaderChainPresetRef"
ENABLED_KEY = "ShaderChainEnabled"
BOOT = "ARMSX2Bridge.bootISO("
REPAIR = "PerGameShaderSelection.repair"
FINGERPRINT = "func perGameFingerprint"
PANEL_STATES = ("perGameShaderChain", "perGameShaderPresetRef")


def else_block(body, anchor):
    """The block the `guard <anchor> ... else {` opens, brace-matched."""
    if anchor not in body:
        return None
    start = body.index(anchor)
    if "else" not in body[start:]:
        return None
    opening = body.index("{", body.index("else", start))
    depth = 0
    for i in range(opening, len(body)):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
            if depth == 0:
                return body[opening:i + 1]
    return None


def swift_sources():
    return sorted(SWIFT.rglob("*.swift"))


class PerGameShaderSelectionPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = BRIDGE_H.read_text(encoding="utf-8")
        cls.impl = BRIDGE_MM.read_text(encoding="utf-8")
        cls.selection = without_comments(SELECTION.read_text(encoding="utf-8"))
        cls.app_state = without_comments(APP_STATE.read_text(encoding="utf-8"))
        cls.panel = without_comments(PANEL.read_text(encoding="utf-8"))
        cls.section = without_comments(SECTION.read_text(encoding="utf-8"))

    def test_the_per_game_bridge_can_carry_a_string(self):
        for method in STRING_METHODS:
            with self.subTest(method=method):
                self.assertIn(
                    method, self.header,
                    f"{method} is not declared in ARMSX2Bridge.h, so Swift cannot store a "
                    "per-game preset token")
                self.assertIn(
                    method, self.impl,
                    f"{method} is declared but not implemented in ARMSX2Bridge.mm")

    def test_one_resolver_owns_the_token(self):
        self.assertIn(
            RESOLVER, self.selection,
            "PerGameShaderSelection does not resolve through ShaderPresetLibrary.resolve")
        for stray in (".appendingPathComponent(", "FileManager", "bundleRoot", "userRoot"):
            with self.subTest(stray=stray):
                self.assertNotIn(
                    stray, self.selection,
                    f"PerGameShaderSelection builds paths itself ({stray}) instead of going "
                    "through ShaderPresetLibrary")
        self.assertIn(
            TOKEN_KEY, self.selection,
            f"PerGameShaderSelection does not use the {TOKEN_KEY} key")

    def test_the_repair_runs_before_the_iso_reaches_core(self):
        sites = []
        for path in swift_sources():
            source = without_comments(path.read_text(encoding="utf-8"))
            sites += [path] * source.count(BOOT)
        self.assertEqual(
            len(sites), 1,
            "Swift calls ARMSX2Bridge.bootISO from "
            + ", ".join(sorted({p.name for p in sites}))
            + ", expected exactly one call site")

        self.assertIn(
            REPAIR, self.app_state,
            "AppState does not call PerGameShaderSelection.repair")
        self.assertIn(
            BOOT, self.app_state,
            "AppState does not call ARMSX2Bridge.bootISO(")
        self.assertLess(
            self.app_state.index(REPAIR), self.app_state.index(BOOT),
            "AppState calls PerGameShaderSelection.repair after ARMSX2Bridge.bootISO, which "
            "reads the per-game file")

    def test_an_unresolvable_token_turns_the_chain_off(self):
        body = block(self.selection, "static func repair(forISO")
        branch = else_block(body, RESOLVER)
        self.assertIsNotNone(
            branch, "repair(forISO:) has no guard with an else branch around the resolve")
        for key in ("presetRef", "presetPath"):
            with self.subTest(key=key):
                self.assertRegex(
                    branch, r"delete\w*\([^)]*" + key,
                    f"repair(forISO:) does not delete the {key} key when the token does not "
                    "resolve")
        self.assertRegex(
            branch, r"enabled,\s*(value:\s*)?false",
            "repair(forISO:) does not write the enabled key false when the token does not "
            "resolve, so the game falls back to the global preset")

    def test_save_notices_a_shader_only_change(self):
        body = block(self.panel, FINGERPRINT)
        for state in PANEL_STATES:
            with self.subTest(state=state):
                self.assertIn(
                    state, body,
                    f"{state} is missing from perGameFingerprint, so a shader-only change "
                    "leaves Save disabled")

    def test_the_per_game_section_is_its_own_view(self):
        for pushed in ("NavigationLink(", "NavigationLink {"):
            with self.subTest(pushed=pushed):
                self.assertNotIn(
                    pushed, self.section,
                    "PerGameShaderSection uses a NavigationLink, but the wide panel layout has "
                    "no NavigationStack")
        self.assertNotIn(
            "ShaderChainSection(", self.section,
            "PerGameShaderSection mounts ShaderChainSection, whose rows write the global "
            "shader settings")


if __name__ == "__main__":
    unittest.main()
