"""A per-game Virtual Pad reset also clears the catalog pick made for the serial."""

import unittest

from ios_source import SWIFT, block, read, without_comments


STORE = SWIFT / "Models/PadLayoutPresetStore.swift"
INSTALLER = SWIFT / "Models/SkinInstaller.swift"


class PadResetsClearTheCatalogPick(unittest.TestCase):
    """The effective skin and layout fall back to the serial entry, and a pick accepted
    before the game boots is applied again at the boot, so all three resets came back."""

    @classmethod
    def setUpClass(cls):
        cls.store = without_comments(read(STORE))

    def test_each_reset_clears_the_serial_entry(self):
        for reset in ("func clearSkin(for", "func clearVPadOverrides(for", "func setPreset("):
            with self.subTest(reset=reset):
                self.assertIn("clearAutomaticAssignment(", block(self.store, reset),
                              f"{reset} leaves the serial entry in place")

    def test_the_pending_pick_goes_too(self):
        helper = block(self.store, "private func clearAutomaticAssignment(")
        self.assertIn("automaticAssignmentKey(forSerial:", helper)
        self.assertIn("forgetAcceptedAssignment(forSerial:", helper,
                      "a pick accepted before the boot is applied again after the reset")
        forget = block(without_comments(read(INSTALLER)), "func forgetAcceptedAssignment(")
        self.assertIn("acceptedAssignments.removeValue(forKey:", forget)


if __name__ == "__main__":
    unittest.main()
