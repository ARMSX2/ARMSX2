"""A glass row keeps its scroll view after the List reuses its cell."""

import unittest

from ios_source import MODELS, block, read, without_comments


NAVIGATION = MODELS / "ControllerAccessibilityNavigation.swift"


class ScrollOwnerMemory(unittest.TestCase):
    """Moving up through Settings in landscape put the focus ring behind the title: an iOS 26
    glass row has no UIScrollView ancestor, its scroll marker forgot it once the List reused the
    cell, and a row under the title is outside the marker, so reveal found no scroll view."""

    def test_the_owner_is_remembered_by_key(self):
        source = without_comments(read(NAVIGATION))
        lookup = block(source, "private func enclosingScrollView(for view: UIView)")
        self.assertIn("scrollOwners[key] = WeakView(owner)", lookup)
        self.assertIn("scrollOwners[key]?.value as? UIScrollView", lookup)
        self.assertIn("owner.window === window", lookup)

    def test_a_scope_change_forgets_the_owners(self):
        source = without_comments(read(NAVIGATION))
        self.assertIn("targets.removeAll(keepingCapacity: true)\n        scrollOwners.removeAll()", source)


if __name__ == "__main__":
    unittest.main()
