"""L2 and R2 change the menu theme only when the player turns it on."""

import unittest

from ios_source import SWIFT, block, read, without_comments


SETTINGS = SWIFT / "Models/UIFrameRateSettings.swift"
ROUTER = SWIFT / "Models/MenuControllerInputRouter.swift"
APPEARANCE = SWIFT / "Views/Settings/AppearanceSettingsView.swift"


class ThemeTriggerSetting(unittest.TestCase):
    """Testers switched themes by accident, for example with the pad lying on a table."""

    def test_it_is_off_unless_saved_on(self):
        source = without_comments(read(SETTINGS))
        self.assertIn("changesThemeWithTriggers = defaults.object(forKey: Keys.themeTriggers) as? Bool ?? false", source)

    def test_the_triggers_check_it(self):
        trigger = block(without_comments(read(ROUTER)), "private func updateThemePresetButton(")
        self.assertIn("UIFrameRateSettings.shared.changesThemeWithTriggers", trigger)

    def test_appearance_offers_it_to_the_pad(self):
        source = without_comments(read(APPEARANCE))
        self.assertIn("isOn: $frameRates.changesThemeWithTriggers", source)
        self.assertEqual(source.count('"settings.appearance.theme-triggers"'), 2)


if __name__ == "__main__":
    unittest.main()
