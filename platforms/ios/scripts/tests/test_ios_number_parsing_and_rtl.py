# test_ios_number_parsing_and_rtl.py — unit tests for NumberRow parsing, English numberLocale, and RTL gestures
# SPDX-License-Identifier: GPL-3.0+

import re
import subprocess
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
NUMBER_ROW_PATH = ROOT_DIR / "app/src/main/swift/Views/Settings/NumberRow.swift"
APP_LANGUAGE_PATH = ROOT_DIR / "app/src/main/swift/Models/AppLanguage.swift"
DYNAMIC_COLOURS_PATH = ROOT_DIR / "app/src/main/swift/Views/Background/Dynamic/DynamicColours.swift"

class TestNumberParsingAndRTL(unittest.TestCase):
    def test_app_language_english_number_locale(self):
        self.assertTrue(APP_LANGUAGE_PATH.exists())
        content = APP_LANGUAGE_PATH.read_text(encoding="utf-8")
        self.assertIn('case .system, .english: return Locale(identifier: "en")', content)
        self.assertNotIn('en_US_POSIX', content)

    def test_dynamic_colours_rtl_swipe(self):
        self.assertTrue(DYNAMIC_COLOURS_PATH.exists())
        content = DYNAMIC_COLOURS_PATH.read_text(encoding="utf-8")
        self.assertIn("@Environment(\\.layoutDirection) private var layoutDirection", content)
        self.assertIn("layoutDirection == .rightToLeft", content)
        self.assertIn("let forward = isRTL ? horizontalDistance > 0 : horizontalDistance < 0", content)

    def test_number_row_source_integrity(self):
        self.assertTrue(NUMBER_ROW_PATH.exists())
        content = NUMBER_ROW_PATH.read_text(encoding="utf-8")
        # Ensure manual separator cleaning and grouping heuristics were removed
        self.assertNotIn("cleaned.replacingOccurrences(of: grouping, with: \"\")", content)
        self.assertNotIn("cleaned.replacingOccurrences(of: decimal, with: \".\")", content)
        self.assertNotIn("try? Double(cleaned, format: .number.locale(locale))", content)
        self.assertNotIn("truncatingRemainder", content)
        self.assertIn("cleaned.replacingOccurrences(of: \",\", with: \".\")", content)
        self.assertIn("guard let val = Double(normalized), val.isFinite else { return nil }", content)

    def test_swift_number_parsing_runtime(self):
        swift_code = """
import Foundation

struct NumberFormatTest {
    let scale: Double

    func parse(_ text: String, locale: Locale = .autoupdatingCurrent) -> Double? {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let normalized = cleaned.replacingOccurrences(of: ",", with: ".")
        guard let val = Double(normalized), val.isFinite else { return nil }
        return val / scale
    }
}

let nf1 = NumberFormatTest(scale: 1)
let nf100 = NumberFormatTest(scale: 100)
let de = Locale(identifier: "de")
let en = Locale(identifier: "en")

guard nf1.parse("1,5", locale: de) == 1.5 else { exit(1) }
guard nf1.parse("1.5", locale: de) == 1.5 else { exit(2) }
guard nf1.parse("1,00", locale: de) == 1.0 else { exit(3) }
guard nf1.parse("1.00", locale: de) == 1.0 else { exit(4) }
guard nf1.parse("1,0", locale: de) == 1.0 else { exit(5) }
guard nf1.parse("1.0", locale: de) == 1.0 else { exit(6) }
guard nf1.parse("1.5", locale: en) == 1.5 else { exit(7) }
guard nf1.parse("1,5", locale: en) == 1.5 else { exit(8) }
guard nf1.parse("12,0", locale: en) == 12.0 else { exit(9) }
guard nf1.parse("12.0", locale: en) == 12.0 else { exit(10) }
guard nf1.parse("1500", locale: de) == 1500.0 else { exit(11) }
guard nf100.parse("70", locale: en) == 0.7 else { exit(12) }
guard nf1.parse("-3.5", locale: en) == -3.5 else { exit(13) }
guard nf1.parse("-3,5", locale: de) == -3.5 else { exit(14) }
guard nf1.parse("", locale: en) == nil else { exit(15) }
guard nf1.parse("invalid", locale: en) == nil else { exit(16) }
guard nf1.parse("1abc", locale: en) == nil else { exit(17) }
guard nf1.parse("1.5abc", locale: en) == nil else { exit(18) }
guard nf1.parse("1.500,25", locale: de) == nil else { exit(19) }
exit(0)
"""
        res = subprocess.run(["swift", "-e", swift_code], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Swift parse test failed: {res.stderr}")

if __name__ == "__main__":
    unittest.main()
