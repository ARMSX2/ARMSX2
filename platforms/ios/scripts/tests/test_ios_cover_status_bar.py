"""Full-screen covers keep the status bar the menu chose."""

import unittest

from ios_source import SWIFT, read, without_comments


class CoverStatusBar(unittest.TestCase):
    """A full-screen cover gets its own controller, which showed the time and battery over
    Game Options even with Hide Status Bar in Menus on."""

    def test_every_cover_repeats_the_app_status_bar(self):
        for path in sorted(SWIFT.rglob("*.swift")):
            source = without_comments(read(path))
            start = source.find(".fullScreenCover(")
            while start >= 0:
                end = source.find(".fullScreenCover(", start + 1)
                cover = source[start:end if end >= 0 else len(source)][:8000]
                with self.subTest(path=path.name, at=source[:start].count("\n") + 1):
                    # The pad layout editor hides the bar itself.
                    self.assertTrue(
                        "appStatusBarHidden()" in cover or "PadLayoutEditView(" in cover
                    )
                start = end

    def test_the_layout_editor_hides_it(self):
        editor = without_comments(read(SWIFT / "Views/Settings/PadLayoutEditView.swift"))
        self.assertIn(".statusBarHidden()", editor)


if __name__ == "__main__":
    unittest.main()
