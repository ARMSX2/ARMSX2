#!/usr/bin/env python3
"""A saved shader parameter has to reach the core on a launch that opens no shader screen.

The overrides live in the core only because something pushed them: GSDevice keeps them in a
static that nothing writes to disk. Every push used to come from ShaderParams.load, which only
a visible shader screen calls, so a cold launch rendered the preset author's numbers and
opening Settings once silently corrected it. That is why it survived casual testing.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
MODELS = ROOT / "platforms/ios/app/src/main/swift/Models"
STORE = MODELS / "SettingsStore.swift"
PARAMS = MODELS / "ShaderParams.swift"

PUSH = "ShaderParams.pushStored("
BRIDGE = "ARMSX2Bridge.setShaderChainParameters("

DECLARATION = re.compile(r"^\s*(?:[\w@]+\s+)*(?:func\s+\w+|init)\s*\(")


def stripped(path):
    """The file with comments gone, so a comment can neither satisfy nor trip a check."""
    source = path.read_text(encoding="utf-8")
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def block_at(lines, first):
    """The brace-balanced block starting at line index `first`."""
    depth = 0
    for i in range(first, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i > first:
            return "\n".join(lines[first:i + 1])
    return "\n".join(lines[first:])


def function_named(source, name):
    """The body of the func declaring `name`, or None."""
    lines = source.split("\n")
    for i, line in enumerate(lines):
        if DECLARATION.match(line) and re.search(r"\b(?:func|init)\s+" + name + r"\s*\(", line):
            return block_at(lines, i)
    return None


def function_containing(source, needle):
    """The body of the func or init whose block holds `needle`, or None."""
    lines = source.split("\n")
    for i, line in enumerate(lines):
        if DECLARATION.match(line):
            body = block_at(lines, i)
            if needle in body:
                return body
    return None


class ShaderParamsColdPushTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.store = stripped(STORE)
        cls.params = stripped(PARAMS)
        # Empty rather than None, so a missing helper fails each check on its own terms.
        cls.push = function_named(cls.params, "pushStored") or ""

    def test_the_overrides_can_be_pushed_without_a_shader_screen(self):
        self.assertRegex(
            self.push, r"static\s+func\s+pushStored\s*\(",
            "ShaderParams has no static pushStored. Every other push is a method on an "
            "instance a shader screen owns, so without this one the stored overrides cannot "
            "reach the core until the user opens Settings.")
        self.assertNotRegex(
            self.push.split("\n")[0], r"\bprivate\b",
            "pushStored is private, so the launch path in SettingsStore cannot call it.")
        self.assertIn(
            BRIDGE, self.push,
            "pushStored does not call the bridge. GSDevice::SetShaderChainParams is the only "
            "route into the core's parameter store, and nothing persists that store.")

    def test_the_push_hands_core_a_resolved_path(self):
        self.assertRegex(
            self.push, re.escape(BRIDGE) + r"[^)]*forPreset:\s*url\.path",
            "pushStored pushes something other than the resolved absolute path. "
            "GSDevice::GetShaderChainParams compares the pushed preset against the one the "
            "chain loaded, so a token or a stale path is dropped and the values never land.")

    def test_a_token_that_resolves_to_nothing_pushes_nothing(self):
        resolved = self.push.find("ShaderPresetLibrary.resolve(")
        pushed = self.push.find(BRIDGE)
        self.assertNotEqual(resolved, -1, "pushStored does not resolve the token at all.")
        self.assertLess(
            resolved, pushed,
            "pushStored pushes before it resolves the token. A token naming a preset that is "
            "gone would push one preset's values under another preset's path.")
        self.assertRegex(
            self.push[resolved - 60:pushed], r"guard[^\n]*else\s*\{\s*return",
            "The resolve in pushStored is not guarded, so an unresolvable token carries on "
            "into the push instead of leaving the core alone.")

    def test_the_launch_path_pushes(self):
        """The reason this test exists: no shader screen opens on a cold launch."""
        migration = function_named(self.store, "migrateShaderChainSelectionV1")
        self.assertIsNotNone(
            migration, "cannot find migrateShaderChainSelectionV1; update this test")
        self.assertIn(
            PUSH, migration,
            "The launch path no longer pushes the saved overrides. This is the whole bug: the "
            "core comes up with an empty parameter store, the chain renders the preset "
            "author's defaults, and opening a shader screen is what corrects it.")
        init = function_containing(self.store, "suppressINIWrites = true")
        self.assertIsNotNone(init, "cannot find SettingsStore.init; update this test")
        self.assertIn(
            "migrateShaderChainSelectionV1()", init,
            "init no longer runs the function that carries the push, so the overrides reach "
            "the core no earlier than the first shader screen again.")

    def test_a_selection_change_pushes(self):
        selection = function_named(self.store, "applyShaderChainSelection")
        self.assertIsNotNone(
            selection, "cannot find applyShaderChainSelection; update this test")
        self.assertIn(
            PUSH, selection,
            "Changing the preset no longer pushes that preset's saved overrides, so a preset "
            "picked outside a shader screen renders on its own numbers until one opens.")

    def test_the_push_cannot_re_enter_the_settings_store(self):
        """It runs inside SettingsStore.init, where a .shared read is a swift_once deadlock."""
        self.assertNotIn(
            "SettingsStore", self.params,
            "ShaderParams names SettingsStore. pushStored is called from SettingsStore.init, "
            "so anything it reaches that touches SettingsStore.shared re-enters that init's "
            "in-flight swift_once: SIGTRAP 'BUG IN CLIENT OF LIBDISPATCH' on iOS 26.x, "
            "doesNotRecognizeSelector on iOS 27. Phase 4.2 was an emergency phase about "
            "exactly this.")


if __name__ == "__main__":
    unittest.main()
