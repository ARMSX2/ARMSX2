import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
BRIDGE = ROOT / "platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm"
GRAPHICS_VIEW = ROOT / "platforms/ios/app/src/main/swift/Views/Settings/GraphicsSettingsView.swift"
STORE = ROOT / "platforms/ios/app/src/main/swift/Models/SettingsStore.swift"
STORE_GRAPHICS = ROOT / "platforms/ios/app/src/main/swift/Models/SettingsStore+Graphics.swift"

HACK_TABLE = re.compile(r'\{"([\w_]+)", GSUserHackOverride::\w+')
PINNED_KEYS = re.compile(r'\{"([\w_]+)", GSUserHackOverride::\w+\},')


class HackClaimPlumbing(unittest.TestCase):
    def setUp(self):
        self.bridge = BRIDGE.read_text()
        self.view = GRAPHICS_VIEW.read_text()
        self.store = STORE.read_text()
        self.store_graphics = STORE_GRAPHICS.read_text()

    def table_keys(self):
        block = self.bridge.split("s_graphics_hacks = {{", 1)[1].split("}};", 1)[0]
        return HACK_TABLE.findall(block)

    def pinned_hack_keys(self):
        block = self.bridge.split("s_pinned_hack_keys[] = {", 1)[1].split("};", 1)[0]
        return PINNED_KEYS.findall(block)

    def bool_hack_keys(self):
        block = self.store_graphics.split(
            "gsBoolHackOptions: [GameFixOption] = [", 1)[1].split("]", 1)[0]
        return set(re.findall(r'key: "([\w_]+)"', block))

    def test_every_reported_hack_row_claims_on_write(self):
        """A hack the bridge reports must pin when its global row changes, or the
        GameDB silently overwrites it and the row lies."""
        via_bool_funnel = self.bool_hack_keys()
        for key in self.table_keys():
            with self.subTest(key=key):
                claimed = f'claiming("{key}"' in self.view or key in via_bool_funnel
                self.assertTrue(claimed, f"{key} is reported but never pinned from the UI")

    def test_bool_hack_funnel_always_claims(self):
        """Every bool hack row shares one write funnel, so the claim belongs there
        rather than in a second list somebody has to remember to extend."""
        funnel = self.store.split("func setGSBoolHack(", 1)[1].split("\n    }", 1)[0]
        self.assertIn("setGraphicsHackPinned(key, pinned: true)", funnel)

    def test_every_reported_hack_is_a_pinned_key(self):
        """The writer derives per-game claims from s_pinned_hack_keys, so a reported
        hack missing there loses its per-game claim on save."""
        pinned = set(self.pinned_hack_keys())
        for key in self.table_keys():
            with self.subTest(key=key):
                self.assertIn(key, pinned)

    def test_writer_never_merges_global_claims(self):
        """The core folds base claims across the layers now; freezing them into a
        game file is the leak that suppressed database fixes forever."""
        self.assertNotIn("globalClaims", self.bridge)


if __name__ == "__main__":
    unittest.main()
