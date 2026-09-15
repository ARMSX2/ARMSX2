import unittest

from ios_source import SWIFT, block, without_comments


SECTION = SWIFT / "Views/Settings/ShaderChainSection.swift"
PARAMS = SWIFT / "Models/ShaderParams.swift"


class ShaderParameterRowPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.section = without_comments(SECTION.read_text(encoding="utf-8"))
        cls.params = without_comments(PARAMS.read_text(encoding="utf-8"))

    def setUp(self):
        self.row = block(self.section, "private func parameterRow(")

    def test_a_parameter_is_adjusted_through_the_app_wide_numeric_row(self):
        self.assertIn("NumberRow(", self.row, "parameterRow() does not build a NumberRow")
        self.assertNotRegex(
            self.section, r"\bSlider\s*\(",
            "ShaderChainSection.swift builds a Slider instead of using NumberRow")
        self.assertNotIn(
            "TextField", self.section,
            "ShaderChainSection.swift builds a TextField instead of using NumberRow")

    def test_the_row_can_still_be_typed_into(self):
        self.assertNotIn(
            ".opaque(", self.section,
            "ShaderChainSection uses an .opaque format, and NumberRow's isTypeable is false "
            "for it")

    def test_the_value_that_reaches_the_store_is_the_presets_own_clamp(self):
        self.assertIn(
            "params.setValue", self.row,
            "parameterRow() does not write through params.setValue, where ShaderParam.clamped "
            "runs")

        clamp = block(self.params, "func clamped(")
        self.assertIn(
            "isNaN", clamp,
            "ShaderParam.clamped does not check isNaN")
        self.assertIn(
            "return initial", clamp,
            "ShaderParam.clamped does not fall back to the preset's initial value")

    def test_a_degenerate_range_is_refused_before_the_row_is_built(self):
        self.assertIn("isAdjustable", self.row, "parameterRow() does not check isAdjustable")
        self.assertLess(
            self.row.index("isAdjustable"), self.row.index("NumberRow("),
            "parameterRow() checks isAdjustable after it builds the NumberRow")

    def test_the_section_still_serves_two_hosts(self):
        self.assertIn(
            "let localized: @MainActor (String) -> String", self.section,
            "ShaderChainSection does not take a localized closure from its host")
        self.assertNotRegex(
            self.section, r"(?m)^\s*(let|var)\s+settings:\s*SettingsStore\s*$",
            "ShaderChainSection takes the SettingsStore as an initializer parameter")
        self.assertRegex(
            self.section, r"private var settings:\s*SettingsStore\s*\{\s*SettingsStore\.shared\s*\}",
            "ShaderChainSection has no private settings property returning SettingsStore.shared")

    def test_the_stops_are_derived_rather_than_enumerated(self):
        self.assertIn(
            "NumberRow.stops(", self.row,
            "parameterRow() does not derive its stops from NumberRow.stops(in:step:)")


if __name__ == "__main__":
    unittest.main()
