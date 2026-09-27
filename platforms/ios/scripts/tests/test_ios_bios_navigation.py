"""The BIOS page runs on the shared controller navigation, not a copy of the library's."""

import re
import unittest

from ios_source import SWIFT, block, read, without_comments


BIOS = SWIFT / "Views/BIOSListView.swift"
ROOT = SWIFT / "Views/RootView.swift"


class BIOSOnTheSharedNavigation(unittest.TestCase):
    """PR.md claimed one focus engine while the BIOS page kept its own listener, focus
    state and toolbar zone next to the shared navigation session."""

    @classmethod
    def setUpClass(cls):
        cls.bios = without_comments(read(BIOS))
        cls.root = without_comments(read(ROOT))

    def test_the_page_registers_a_navigation_session(self):
        self.assertIn(".controllerAccessibilityNavigation(", self.bios)
        self.assertIn('private let biosControllerScope = "menu.bios"', self.bios)

    def test_the_old_engine_is_gone(self):
        for old in ("BIOSControllerFocusState", "BIOSControllerCommandListener",
                    "handleControllerToolbarCommand", "latestLibraryEntryRequest"):
            with self.subTest(old=old):
                self.assertNotIn(old, self.bios)
        listeners = re.findall(r"\.onChange\(of: controllerInput\.latestEvent\)", self.bios)
        self.assertEqual(len(listeners), 1, "only the prompt listener should read raw events")

    def test_rows_and_toolbar_are_targets(self):
        self.assertIn("id: bios.filePath", block(self.bios, "private func biosRow("))
        for target in ("BIOSControllerTarget.boot", "BIOSControllerTarget.importBIOS",
                       "BIOSControllerTarget.refresh"):
            with self.subTest(target=target):
                self.assertIn("id: %s" % target, self.bios)

    def test_entry_waits_for_the_rows(self):
        self.assertIn(
            ".environment(\\.controllerAccessibilityTargetsSuppressed, !library.hasLoaded)",
            self.bios,
        )

    def test_root_view_enters_through_the_session(self):
        self.assertNotIn("selectedTab == 0 || selectedTab == 1", self.root)
        self.assertNotIn("selectedMenuTab == 0 || selectedMenuTab == 1", self.root)
        self.assertNotIn("tab == 0 || tab == 1", self.root)
        self.assertGreaterEqual(self.root.count('"menu.bios"'), 3)


if __name__ == "__main__":
    unittest.main()
