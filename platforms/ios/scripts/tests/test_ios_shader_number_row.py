import re
import unittest
from pathlib import Path


# Repo root, same as the other tests here.
ROOT = Path(__file__).resolve().parents[4]
SECTION = ROOT / "platforms/ios/app/src/main/swift/Views/Settings/ShaderChainSection.swift"
PARAMS = ROOT / "platforms/ios/app/src/main/swift/Models/ShaderParams.swift"

IDENTITY = ("SC-8 asks for the control every other numeric setting uses, not for a control that "
            "happens to accept a number, so a hand-built one leaves the criterion open however "
            "well it works.")


def without_comments(source):
    """What the compiler sees, so prose naming a symbol cannot satisfy a check."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", source, flags=re.S))


def parameter_row(code):
    match = re.search(r"(?ms)^    private func parameterRow\(.*?\n    \}\n", code)
    return match.group(0) if match else None


def clamp_body(code):
    match = re.search(r"(?ms)^    func clamped\(.*?\n    \}", code)
    return match.group(0) if match else None


class ShaderParameterRowPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.section = without_comments(SECTION.read_text(encoding="utf-8"))
        cls.params = without_comments(PARAMS.read_text(encoding="utf-8"))

    def setUp(self):
        self.row = parameter_row(self.section)
        self.assertIsNotNone(
            self.row,
            "ShaderChainSection has no parameterRow. Every claim below is about that one "
            "function, so a rename here turns this whole file into a test of nothing.")

    def test_a_parameter_is_adjusted_through_the_app_wide_numeric_row(self):
        self.assertIn("NumberRow(", self.row, IDENTITY)
        self.assertNotRegex(
            self.section, r"\bSlider\s*\(",
            "a bare slider is back in ShaderChainSection.swift. " + IDENTITY)
        self.assertNotIn(
            "TextField", self.section,
            "a typing field has been bolted next to the slider. " + IDENTITY)

    def test_the_row_can_still_be_typed_into(self):
        self.assertNotIn(
            ".opaque(", self.section,
            "an opaque format is a readout somebody else built, and NumberRow.swift:70-71 reads "
            "that as isTypeable == false. It is the one change that removes typed entry while "
            "the row still compiles, still slides and still looks right.")

    def test_the_value_that_reaches_the_store_is_the_presets_own_clamp(self):
        self.assertIn(
            "params.setValue", self.row,
            "the binding no longer writes through ShaderParams.setValue. That is where "
            "ShaderParam.clamped runs, and it is the only clamp in the pair that sends NaN to "
            "the author's initial instead of to the bottom of the range.")

        clamp = clamp_body(self.params)
        self.assertIsNotNone(clamp, "ShaderParam.clamped is gone; nothing bounds the author now")
        self.assertIn(
            "isNaN", clamp,
            "ShaderParam.clamped stopped testing for NaN. Preset values are decoded from a file "
            "a stranger wrote, and NumberRow.swift:688-691 would answer with range.lowerBound.")
        self.assertIn(
            "return initial", clamp,
            "ShaderParam.clamped no longer falls back to the preset's initial, so an unusable "
            "value now lands on a bound the author never chose.")

    def test_a_degenerate_range_is_refused_before_the_row_is_built(self):
        self.assertIn("isAdjustable", self.row, "the degenerate-range gate is gone")
        self.assertLess(
            self.row.index("isAdjustable"), self.row.index("NumberRow("),
            "isAdjustable no longer gates the row. NumberRow.swift:527-533 draws no track at all "
            "when the bounds do not increase, so a parameter that should read as a caption "
            "becomes a blank row instead.")

    def test_the_section_still_serves_two_hosts(self):
        self.assertIn(
            "let localized: @MainActor (String) -> String", self.section,
            "ShaderChainSection stopped taking a localizer from its caller. Both the settings "
            "page and the pause surface mount this section on that contract.")
        self.assertNotRegex(
            self.section, r"(?m)^\s*(let|var)\s+settings:\s*SettingsStore\s*$",
            "the store became an initializer parameter. NumberRow needs one, but every host has "
            "to absorb that change; VirtualPadSettingsView.swift:1279 reaches for it internally "
            "instead.")
        self.assertRegex(
            self.section, r"private var settings:\s*SettingsStore\s*\{\s*SettingsStore\.shared\s*\}",
            "the internal route to SettingsStore.shared is gone, so nothing can supply the store "
            "NumberRow requires without widening the initializer.")

    def test_the_stops_are_derived_rather_than_enumerated(self):
        self.assertIn(
            "NumberRow.stops(", self.row,
            "the detent list is no longer derived. NumberRow.swift:715-732 answers with an empty "
            "list when the author's increment has no round multiple, which is the continuous "
            "track such a parameter wants.")
        self.assertNotIn(
            "stepCount", self.row,
            "stepCount is back in the row. It clamps at 10,000 (ShaderParams.swift), and a "
            "detent list that long is a stop for every pixel of track.")


if __name__ == "__main__":
    unittest.main()
