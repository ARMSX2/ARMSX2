"""On a landscape phone the Settings list starts at the top, with no blank clearance row."""

import unittest

from ios_source import SWIFT, read, without_comments


SETTINGS = SWIFT / "Views/Settings/SettingsRootView.swift"


class SettingsLandscapeTop(unittest.TestCase):
    """Testers saw the Settings options start a third of the way down in landscape, and at the
    bottom when they came in from the tab bar: a 96 pt blank first row, there to give focus room
    below the title, took a quarter of a landscape phone's height."""

    def test_the_clearance_row_is_left_out_at_compact_height(self):
        source = without_comments(read(SETTINGS))
        row = source[source.find("List {"):]
        row = row[:row.find(".frame(height: Self.topNavigationFocusClearance)")]
        self.assertIn("if controllerInput != nil, verticalSizeClass != .compact {", row)


if __name__ == "__main__":
    unittest.main()
