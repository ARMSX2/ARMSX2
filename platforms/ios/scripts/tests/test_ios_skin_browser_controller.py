"""The Skins browser moves in two columns and its preview takes Back."""

import unittest

from ios_source import MODELS, SWIFT, block, read, without_comments


BROWSER = SWIFT / "Views/Settings/SkinBrowserView.swift"
NAVIGATION = MODELS / "ControllerAccessibilityNavigation.swift"


class SkinBrowserController(unittest.TestCase):
    """Down stepped through thumbnails and Get buttons in turn and stopped at the rows on
    screen, and Circle in the preview popped the Skins page from under it."""

    @classmethod
    def setUpClass(cls):
        cls.browser = without_comments(read(BROWSER))
        cls.navigation = without_comments(read(NAVIGATION))

    def test_the_page_declares_its_columns_and_their_seam(self):
        self.assertIn("links: controllerColumns.links", self.browser)
        columns = block(self.browser, "private var controllerColumns:")
        self.assertIn("return (header + previews + actions, links)", columns)
        for target in ('"skin.preview.', '"skin.get.', '"skin.detail.'):
            with self.subTest(target=target):
                self.assertIn("id: " + target, self.browser)

    def test_search_and_filter_are_reachable(self):
        self.assertIn('.controllerAccessibilityOptionsPickerTarget(\n                    id: "skin.filter"',
                      self.browser)
        self.assertIn("OrbitKeysKeyboardView(", self.browser)
        self.assertIn('id: "skin.search"', self.browser)

    def test_page_links_are_followed(self):
        self.assertIn("(directionalLinks + pageDirectionalLinks).first(",
                      block(self.navigation, "private func linkedDestination("))

    def test_the_preview_closes_on_back(self):
        sheet = block(self.browser, "private struct SkinPreviewSheet")
        self.assertIn('scopeKey: "skin-browser.preview"', sheet)
        self.assertIn("dismiss()\n                return true", sheet)


if __name__ == "__main__":
    unittest.main()
