import re
import unittest

from ios_source import CPP, ROOT, SWIFT, block


BRIDGE = CPP / "ARMSX2Bridge.mm"
GRAPHICS_VIEW = SWIFT / "Views/Settings/GraphicsSettingsView.swift"
STORE = SWIFT / "Models/SettingsStore.swift"
STORE_GRAPHICS = SWIFT / "Models/SettingsStore+Graphics.swift"
OVERRIDES = ROOT / "pcsx2/PerGameOverrides.cpp"

HACK_TABLE = re.compile(r'\{"([\w_]+)", GSUserHackOverride::\w+')
PINNED_KEYS = re.compile(r'\{"([\w_]+)", GSHWFixId::\w+, GSUserHackOverride::(?!MaxCount)\w+\}')


class HackClaimPlumbing(unittest.TestCase):
    def setUp(self):
        self.bridge = BRIDGE.read_text()
        self.view = GRAPHICS_VIEW.read_text()
        self.store = STORE.read_text()
        self.store_graphics = STORE_GRAPHICS.read_text()
        self.overrides = OVERRIDES.read_text()

    def table_keys(self):
        table = self.bridge.split("s_graphics_hacks = {{", 1)[1].split("}};", 1)[0]
        return HACK_TABLE.findall(table)

    def pinned_hack_keys(self):
        return PINNED_KEYS.findall(self.overrides)

    def bool_hack_keys(self):
        options = self.store_graphics.split(
            "gsBoolHackOptions: [GameFixOption] = [", 1)[1].split("]", 1)[0]
        return set(re.findall(r'key: "([\w_]+)"', options))

    def test_every_reported_hack_row_claims_on_write(self):
        """A reported hack pins when its global row changes, so the GameDB cannot overwrite it."""
        via_bool_funnel = self.bool_hack_keys()
        for key in self.table_keys():
            with self.subTest(key=key):
                claimed = f'claiming("{key}"' in self.view or key in via_bool_funnel
                self.assertTrue(claimed, f"{key} is reported but never pinned from the UI")

    def test_bool_hack_funnel_always_claims(self):
        """Every bool hack row writes through setGSBoolHack, so the claim is made there."""
        funnel = block(self.store, "func setGSBoolHack(")
        self.assertIn("setGraphicsHackPinned(key, pinned: true)", funnel)

    def test_every_reported_hack_is_a_pinned_key(self):
        """The writer derives per-game claims from s_gs_keys, so a reported hack
        missing there, or mapped to MaxCount, loses its per-game claim on save."""
        pinned = set(self.pinned_hack_keys())
        for key in self.table_keys():
            with self.subTest(key=key):
                self.assertIn(key, pinned)


if __name__ == "__main__":
    unittest.main()
