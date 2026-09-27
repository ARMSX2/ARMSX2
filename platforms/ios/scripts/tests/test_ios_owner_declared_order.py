"""A page sliding out can't replace the row order of the screen it uncovers."""

import unittest

from ios_source import SWIFT, block, read, without_comments


NAVIGATION = SWIFT / "Models/ControllerAccessibilityNavigation.swift"


class OwnerDeclaredOrder(unittest.TestCase):
    """In landscape, leaving Appearance with Circle left the Settings list with no focus:
    the outgoing page republished its order on the scope change, and the list kept
    seeking an Appearance row it doesn't have."""

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(NAVIGATION))

    def test_the_owner_marks_its_order(self):
        modifier = block(self.source, "private struct ControllerAccessibilityNavigationModifier")
        self.assertIn("session.ownerDeclaresOrder = declaredTargetOrder != nil", modifier)

    def test_pages_yield_to_it(self):
        page = block(self.source, "private struct ControllerAccessibilityTargetOrderModifier")
        self.assertEqual(page.count("session?.ownerDeclaresOrder != true"), 2)


if __name__ == "__main__":
    unittest.main()
