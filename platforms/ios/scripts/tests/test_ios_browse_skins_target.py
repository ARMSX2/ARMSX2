"""Browse Skins on the Virtual Pad page is a controller target like the rows around it."""

import unittest

from ios_source import SWIFT, read, without_comments


VIRTUAL_PAD = SWIFT / "Views/Settings/VirtualPadSettingsView.swift"


class BrowseSkinsIsATarget(unittest.TestCase):
    """The link only named a target for the automatic button style, which a NavigationLink
    in a Form never uses, so Down stopped at Import Skin and the rest of the page was
    out of reach."""

    @classmethod
    def setUpClass(cls):
        cls.page = without_comments(read(VIRTUAL_PAD))

    def test_the_link_opens_its_pane_as_an_action_target(self):
        self.assertIn(".controllerAccessibilityActionTarget(", self.page)
        self.assertIn("onOpenPane(.skinBrowser)", self.page)
        self.assertNotIn('.controllerAccessibilityTargetID(settings.localized("Browse Skins"))', self.page)


if __name__ == "__main__":
    unittest.main()
