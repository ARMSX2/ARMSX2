"""Settings remembers the controller's row for one launch only."""

import unittest

from ios_source import MODELS, SWIFT, block, read, without_comments


ROUTER = MODELS / "MenuControllerInputRouter.swift"
SETTINGS = SWIFT / "Views/Settings/SettingsRootView.swift"


class SettingsFocusPerLaunch(unittest.TestCase):
    """Testers opened Appearance after a relaunch and it scrolled straight down to the row they
    had used last time, because every Settings page saved its focused row in UserDefaults."""

    def test_the_router_keeps_focus_in_memory(self):
        router = without_comments(read(ROUTER))
        self.assertNotIn("ARMSX2iOSControllerFocus.", router)
        for name in ("func rememberNavigationFocusKey", "func rememberedNavigationFocusKey"):
            with self.subTest(function=name):
                self.assertNotIn("UserDefaults", block(router, name))

    def test_the_settings_list_keeps_its_row_in_memory(self):
        settings = without_comments(read(SETTINGS))
        self.assertNotIn("ARMSX2iOSSettingsRememberedRoot", settings)
        self.assertIn("@State private var rememberedRootPaneRawValue", settings)
        self.assertIn("@State private var rememberedRootScrollPositionID", settings)


if __name__ == "__main__":
    unittest.main()
