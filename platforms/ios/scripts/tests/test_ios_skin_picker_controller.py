"""The in-game skin picker keeps its layout switch and moves Apply/Cancel sideways."""

import unittest

from ios_source import SWIFT, block, read, without_comments


GAME_SCREEN = SWIFT / "Views/GameScreenView.swift"


class SkinPickerController(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(GAME_SCREEN))

    def test_changing_skin_leaves_the_layout_switch_alone(self):
        select = block(self.source, "private func selectRuntimeControllerSkin(offset: Int)")
        self.assertNotIn("runtimeControllerSkinSetsLayout", select)

    def test_apply_and_cancel_move_with_left_and_right(self):
        links = block(self.source, "private static let runtimeControllerSkinActionLinks")
        self.assertIn('fromLabel: "alert.action.apply", direction: .right, toLabel: "alert.action.cancel"', links)
        self.assertIn('fromLabel: "alert.action.cancel", direction: .left, toLabel: "alert.action.apply"', links)
        self.assertIn("directionalLinks: Self.runtimeControllerSkinActionLinks", self.source)


if __name__ == "__main__":
    unittest.main()
