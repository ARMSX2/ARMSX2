"""Clear Liquid Glass on the Settings pages starts from the main toggle on upgrade."""

import re
import unittest

from ios_source import SWIFT, read, without_comments


class ClearGlassUpgrade(unittest.TestCase):
    """The sub-settings key is new, so every upgrade loaded it as true, and a player who
    had Clear Liquid Glass off got clear glass on every Settings page."""

    def test_the_sub_key_defaults_to_the_main_toggle(self):
        store = without_comments(read(SWIFT / "Models/SettingsStore.swift"))
        main = re.search(r'let (\w+) = UserDefaults\.standard\.object\(\s*'
                         r'forKey: "ARMSX2iOSClearLiquidGlassUI"\s*\) as\? Bool \?\? true', store)
        self.assertIsNotNone(main, "the stored main toggle is not read before the sub-settings key")
        sub = re.search(r'clearLiquidGlassUISubSettings = UserDefaults\.standard\.object\(\s*'
                        r'forKey: "ARMSX2iOSClearLiquidGlassUISubSettings"\s*\) as\? Bool \?\? '
                        + main.group(1) + r'\b', store)
        self.assertIsNotNone(sub, "a missing sub-settings key no longer defaults to the main toggle")
        self.assertLess(main.start(), sub.start())


if __name__ == "__main__":
    unittest.main()
