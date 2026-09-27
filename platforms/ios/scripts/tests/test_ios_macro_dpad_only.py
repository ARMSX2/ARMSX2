"""Controller macros read the D-pad, not the left stick."""

import unittest

from ios_source import CPP, MODELS, block, read, without_comments


ROUTER = MODELS / "MenuControllerInputRouter.swift"
GAMEPAD = CPP / "IOS/GamepadHaptics.mm"


class MacrosReadOnlyTheDPad(unittest.TestCase):
    """A left stick push past 0.58 counted as a D-pad press, so a Start + D-pad macro fired
    while the player held Start and steered, and a tester asked for the two to be separate."""

    def test_the_swift_trigger_reads_the_dpad(self):
        press = block(without_comments(read(ROUTER)),
                      "fileprivate func isPressed(on gamepad: GCExtendedGamepad) -> Bool {")
        self.assertIn("gamepad.dpad.right.isPressed", press)
        for axis in ("leftThumbstick.xAxis", "leftThumbstick.yAxis"):
            with self.subTest(axis=axis):
                self.assertNotIn(axis, press)

    def test_the_gameplay_gate_reads_the_dpad(self):
        source = read(GAMEPAD)
        self.assertIn("ARMSX2ResolveControllerMacroGameplayMask(u32 gamepad_index, u32 raw_mask)", source)
        self.assertNotIn("macro_direction_threshold", source)


if __name__ == "__main__":
    unittest.main()
