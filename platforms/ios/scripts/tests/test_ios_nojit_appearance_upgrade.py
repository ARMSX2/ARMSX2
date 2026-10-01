"""The automatic no-JIT theme only touches an appearance the player never chose."""

import unittest

from ios_source import SWIFT, block, read, without_comments


APPEARANCE = SWIFT / "Models/SettingsStore+Appearance.swift"
DYNAMIC = SWIFT / "Views/Background/Dynamic/DynamicBackgroundPresentation.swift"


class NoJITThemeRespectsTheUpgrader(unittest.TestCase):
    """An upgrader who had turned Use Dynamic Background off counted as untouched, so the
    first boot without JIT turned it back on with the crimson theme."""

    def test_base_era_background_keys_count_as_a_choice(self):
        keys = block(without_comments(read(APPEARANCE)), "static let legacyAppearanceKeys")
        for key in ("ARMSX2iOSDynamicBackgroundsEnabled", "ARMSX2iOSBackgroundDim"):
            with self.subTest(key=key):
                self.assertIn(f'"{key}"', keys, f"{key} is not treated as the player's choice")

    def test_the_toggle_marks_ownership(self):
        dynamic = without_comments(read(DYNAMIC))
        self.assertNotIn("$settings.dynamicBackgroundsEnabled", dynamic,
                         "Use Dynamic Background binds straight to the setting again")
        toggle = dynamic[dynamic.index("Toggle(isOn: Binding("):]
        self.assertRegex(toggle[:400], r"beginCustomThemeEditing\(\)\s+settings\.dynamicBackgroundsEnabled",
                         "the toggle does not mark the appearance before it changes")


if __name__ == "__main__":
    unittest.main()
