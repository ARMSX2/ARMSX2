"""Connecting a controller keeps the screens under the navigation modifier alive."""

import unittest

from ios_source import SWIFT, block, read, without_comments


CAN = SWIFT / "Models/ControllerAccessibilityNavigation.swift"


class OneChainInEveryState(unittest.TestCase):
    """navigationContent returned bare content without a controller and the modifier chain
    with one, so every connect or sleep rebuilt what it wrapped, the Settings
    NavigationStack included."""

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(CAN))

    def test_no_branch_returns_the_bare_content(self):
        content = block(self.source, "private func navigationContent(")
        self.assertNotIn("else { content }", content, "navigationContent branches on the controller again")
        self.assertNotIn("hasConnectedController", content, "navigationContent branches on the controller again")

    def test_inactive_styles_keep_the_outer_style(self):
        button = block(self.source, "private struct ControllerAccessibilityRegisteringButtonStyle")
        toggle = block(self.source, "private struct ControllerAccessibilityRegisteringToggleStyle")
        self.assertIn("} else { Button(configuration) }", button)
        self.assertIn("} else { Toggle(configuration) }", toggle)

    def test_a_scope_that_registers_nothing_suppresses_outer_styles(self):
        content = block(self.source, "private func navigationContent(")
        self.assertIn("\\.controllerAccessibilityAutomaticTargetSuppressed,\n                !registersAutomaticTargets",
                      content, "an inactive or explicit-only scope lets an outer style register its buttons")


if __name__ == "__main__":
    unittest.main()
