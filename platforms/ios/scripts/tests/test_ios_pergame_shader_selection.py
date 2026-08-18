import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
SWIFT = ROOT / "platforms/ios/app/src/main/swift"
CPP = ROOT / "platforms/ios/app/src/main/cpp"

BRIDGE_H = CPP / "ARMSX2Bridge.h"
BRIDGE_MM = CPP / "ARMSX2Bridge.mm"
SELECTION = SWIFT / "Models/PerGameShaderSelection.swift"
APP_STATE = SWIFT / "Models/AppState.swift"
PANEL = SWIFT / "Views/PerGameSettingsPanel.swift"
SECTION = SWIFT / "Views/Settings/PerGame/PerGameShaderSection.swift"

STRING_METHODS = (
    "getPerGameINIString:",
    "setPerGameINIString:",
    "getPerGameINIStringForCurrentGame:",
    "setPerGameINIStringForCurrentGame:",
)
RESOLVER = "ShaderPresetLibrary.resolve"
TOKEN_KEY = "ShaderChainPresetRef"
ENABLED_KEY = "ShaderChainEnabled"
BOOT = "ARMSX2Bridge.bootISO("
REPAIR = "PerGameShaderSelection.repair"
FINGERPRINT = "func perGameFingerprint"
PANEL_STATES = ("perGameShaderChain", "perGameShaderPresetRef")


def without_comments(source):
    """What the compiler sees, so prose naming a symbol cannot satisfy a check."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def func_body(source, signature):
    """A Swift function from its signature to the brace that closes it."""
    if signature not in source:
        return None
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    return None


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
                    f"{method} is not declared, so Swift cannot see it. A preset selection is a "
                    "token and not a number, and the per-game family is Int, Bool and Float "
                    "without these: the per-game tier cannot store a shader choice at all.")
                self.assertIn(
                    method, self.impl,
                    f"{method} is declared and never implemented, which is a link error at best "
                    "and an unrecognized selector on device at worst.")

    def test_one_resolver_owns_the_token(self):
        self.assertIn(
            RESOLVER, self.selection,
            "PerGameShaderSelection no longer resolves through ShaderPresetLibrary, which is the "
            "only code that refuses .. components, re-checks containment under a root and confirms "
            "the file exists. A second resolver is a second set of those rules to get right.")
        for stray in (".appendingPathComponent(", "FileManager", "bundleRoot", "userRoot"):
            with self.subTest(stray=stray):
                self.assertNotIn(
                    stray, self.selection,
                    f"PerGameShaderSelection builds paths itself ({stray}), so a per-game token "
                    "takes a route to disk that plan 05's eleven mutations never tested.")
        self.assertIn(
            TOKEN_KEY, self.selection,
            f"the per-game file no longer carries {TOKEN_KEY}. An absolute path stops naming its "
            "preset the moment the container UUID changes, and this build is sideloaded, so that "
            "is every reinstall rather than a rare event.")

    def test_the_repair_runs_before_the_iso_reaches_core(self):
        sites = []
        for path in swift_sources():
            source = without_comments(path.read_text(encoding="utf-8"))
            sites += [path] * source.count(BOOT)
        self.assertEqual(
            len(sites), 1,
            "Swift reaches ARMSX2Bridge.bootISO from "
            + ", ".join(sorted({p.name for p in sites}))
            + ". The re-rooting hooks one call site, so any other one boots a game on the absolute "
            "path last install wrote. This counts Swift only: the native auto-boot path never "
            "calls bootISO and is knowingly left uncovered.")

        self.assertIn(
            REPAIR, self.app_state,
            "AppState never calls the repair, so a per-game token is never turned back into a "
            "path under this install's container and the chain fails to build.")
        self.assertIn(
            BOOT, self.app_state,
            "AppState no longer boots through ARMSX2Bridge.bootISO, so the ordering assertion "
            "below is measuring nothing. Re-anchor it on whatever replaced the call.")
        self.assertLess(
            self.app_state.index(REPAIR), self.app_state.index(BOOT),
            "the repair runs after the boot. UpdateGameSettingsLayer reads the per-game file "
            "during bootISO, so core already holds the stale path by the time the fix lands.")

    def test_an_unresolvable_token_turns_the_chain_off(self):
        body = func_body(self.selection, "static func repair(forISO")
        self.assertIsNotNone(
            body, "PerGameShaderSelection.repair is gone, so nothing re-roots a per-game token.")
        branch = else_block(body, RESOLVER)
        self.assertIsNotNone(
            branch, "the repair no longer guards the resolve, so a token naming nothing is treated "
            "as if it named something.")
        for key in ("presetRef", "presetPath"):
            with self.subTest(key=key):
                self.assertRegex(
                    branch, r"delete\w*\([^)]*" + key,
                    f"an unresolvable token leaves the {key} key in the per-game file. Both string "
                    "keys go, or the next boot re-reads the same dead selection and the panel "
                    "keeps reporting a preset that names nothing.")
        self.assertRegex(
            branch, r"enabled,\s*(value:\s*)?false",
            "an unresolvable token does not write the enabled key false, so the per-game file "
            "falls through to the global preset. That puts a shader on screen the player never "
            "chose for this game, and one frame cannot be told from another by looking at it. "
            "Off is visible and reads back as Off the next time the panel opens.")

    def test_save_notices_a_shader_only_change(self):
        self.assertIn(
            FINGERPRINT, self.panel,
            "the panel no longer fingerprints its state, so Save cannot tell a change from none.")
        body = func_body(self.panel, FINGERPRINT)
        self.assertIsNotNone(body, "perGameFingerprint has no readable body; update this test.")
        for state in PANEL_STATES:
            with self.subTest(state=state):
                self.assertIn(
                    state, self.panel,
                    f"{state} is not held by the panel, so nothing loads it and nothing saves it.")
                self.assertIn(
                    state, body,
                    f"{state} is missing from the fingerprint, so Save stays greyed out over the "
                    "only change the player made and it is discarded on dismiss. The token is the "
                    "half that matters most: picking a preset while the chain already reads On "
                    "moves nothing else the fingerprint can see.")

    def test_the_per_game_section_is_its_own_view(self):
        for pushed in ("NavigationLink(", "NavigationLink {"):
            with self.subTest(pushed=pushed):
                self.assertNotIn(
                    pushed, self.section,
                    "the preset row pushes onto a navigation stack. The panel builds one in "
                    "portrait only; landscape is a rail and a detail pane in a plain HStack, so on "
                    "a wide panel the row renders and does nothing at all.")
        self.assertNotIn(
            "ShaderChainSection(", self.section,
            "the global shader section is mounted under a per-game heading. Its only empty state "
            "is an empty presetRef, and the per-game tier needs three: use global, off, and a "
            "preset of its own. Its parameter rows also write the global override map.")


if __name__ == "__main__":
    unittest.main()
