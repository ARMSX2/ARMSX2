"""A preferred focus label may be a remembered session key, as Settings passes back on Back."""

import unittest

from ios_source import MODELS, block, read, without_comments


NAVIGATION = MODELS / "ControllerAccessibilityNavigation.swift"


class PreferredFocusKey(unittest.TestCase):
    """Back from a Settings page landed on Language. Settings handed back the router's remembered
    key (controller.focus.id.settings.root.appIcon), and matching wrapped it in the key prefix
    again, so nothing matched and focus fell to the first row."""

    def test_the_preferred_label_is_turned_back_into_an_id(self):
        source = without_comments(read(NAVIGATION))
        entry = block(source, "func focusContent(preferLast: Bool)")
        self.assertIn(".map(Self.navigationID)", entry)
        helper = block(source, "private static func navigationID(")
        self.assertIn("explicitKey(\"\")", helper)
        self.assertIn("dropFirst(prefix.count)", helper)


if __name__ == "__main__":
    unittest.main()
