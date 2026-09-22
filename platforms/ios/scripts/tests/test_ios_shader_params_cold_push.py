#!/usr/bin/env python3
"""A saved shader parameter reaches the core on a launch that opens no shader screen."""

import re
import unittest

from ios_source import SWIFT, block, read, without_comments


MODELS = SWIFT / "Models"
STORE = MODELS / "SettingsStore.swift"
PARAMS = MODELS / "ShaderParams.swift"

PUSH = "ShaderParams.pushStored("
BRIDGE = "ARMSX2Bridge.setShaderChainParameters("


class ShaderParamsColdPushTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.store = without_comments(read(STORE))
        cls.params = without_comments(read(PARAMS))
        cls.push = block(cls.params, "func pushStored(")

    def test_the_overrides_can_be_pushed_without_a_shader_screen(self):
        self.assertRegex(
            self.params, r"static\s+func\s+pushStored\s*\(",
            "ShaderParams has no static pushStored, so only a shader screen pushes the saved "
            "overrides")
        self.assertNotRegex(
            self.params, r"private[^\n]*func pushStored\(",
            "pushStored is private, so the launch path in SettingsStore cannot call it.")
        self.assertIn(
            BRIDGE, self.push,
            "pushStored does not call ARMSX2Bridge.setShaderChainParameters")

    def test_the_push_hands_core_a_resolved_path(self):
        self.assertRegex(
            self.push, re.escape(BRIDGE) + r"[^)]*forPreset:\s*url\.path",
            "pushStored does not pass the resolved url.path as forPreset:")

    def test_a_token_that_resolves_to_nothing_pushes_nothing(self):
        resolved = self.push.find("ShaderPresetLibrary.resolve(")
        pushed = self.push.find(BRIDGE)
        self.assertNotEqual(resolved, -1, "pushStored does not call ShaderPresetLibrary.resolve")
        self.assertLess(
            resolved, pushed,
            "pushStored calls the bridge before it resolves the token")
        self.assertRegex(
            self.push,
            r"guard\s+let\s+url\s*=\s*ShaderPresetLibrary\.resolve\(token\)\s*else\s*\{\s*return",
            "pushStored does not return when the token does not resolve")

    def test_the_launch_path_pushes(self):
        """No shader screen opens on a cold launch, so the launch path pushes the overrides."""
        migration = block(self.store, "func migrateShaderChainSelectionV1(")
        self.assertIn(
            PUSH, migration,
            "migrateShaderChainSelectionV1() does not call ShaderParams.pushStored(")

    def test_a_selection_change_pushes(self):
        selection = block(self.store, "func applyShaderChainSelection(")
        self.assertIn(
            PUSH, selection,
            "applyShaderChainSelection() does not call ShaderParams.pushStored(")

    def test_the_push_cannot_re_enter_the_settings_store(self):
        """It runs inside SettingsStore.init, where a .shared read is a swift_once deadlock."""
        self.assertNotIn(
            "SettingsStore", self.params,
            "ShaderParams names SettingsStore, but pushStored runs inside SettingsStore.init")


if __name__ == "__main__":
    unittest.main()
