# test_ios_localization_parity.py — verifies Localizable.xcstrings integrity and parity
# SPDX-License-Identifier: GPL-3.0+

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parents[2]
RESOURCES_DIR = ROOT_DIR / "app/src/main/assets/resources"
XSTRINGS_PATH = RESOURCES_DIR / "Localizable.xcstrings"
INFO_PLIST_PATH = ROOT_DIR / "app/src/main/cpp/Info.plist.in"
APP_LANGUAGE_PATH = ROOT_DIR / "app/src/main/swift/Models/AppLanguage.swift"

EXPECTED_LANGUAGES = ["en", "zh-Hans", "ar", "es", "fr", "de", "it", "pt", "ja", "ko"]

class TestLocalizationParity(unittest.TestCase):
    def test_xcstrings_exists_and_valid_json(self):
        self.assertTrue(XSTRINGS_PATH.exists(), f"Missing {XSTRINGS_PATH}")
        data = json.loads(XSTRINGS_PATH.read_text(encoding="utf-8"))
        self.assertEqual(data.get("version"), "1.0")
        self.assertEqual(data.get("sourceLanguage"), "en")
        self.assertIn("strings", data)
        self.assertEqual(len(data["strings"]), 597)

    def test_info_plist_cfbundlelocalizations(self):
        self.assertTrue(INFO_PLIST_PATH.exists(), f"Missing {INFO_PLIST_PATH}")
        content = INFO_PLIST_PATH.read_text(encoding="utf-8")
        self.assertIn("<key>CFBundleLocalizations</key>", content)
        for lang in EXPECTED_LANGUAGES:
            self.assertIn(f"<string>{lang}</string>", content)

    def test_app_language_swift_shim(self):
        self.assertTrue(APP_LANGUAGE_PATH.exists(), f"Missing {APP_LANGUAGE_PATH}")
        content = APP_LANGUAGE_PATH.read_text(encoding="utf-8")
        self.assertNotIn("Self.translations", content)
        self.assertNotIn("Self.commonTranslations", content)
        self.assertNotIn("Self.uiSupplementTranslations", content)
        self.assertIn("bundle.localizedString(forKey: key, value: key, table: nil)", content)
        self.assertIn("bcp47Code", content)
        self.assertIn("bundleCache", content)

    def test_xcstringstool_compile(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            res = subprocess.run(
                ["xcrun", "xcstringstool", "compile", str(XSTRINGS_PATH), "--output-directory", tmpdir],
                capture_output=True,
                text=True
            )
            self.assertEqual(res.returncode, 0, f"xcstringstool failed: {res.stderr}")
            tmp_path = Path(tmpdir)
            for lang in EXPECTED_LANGUAGES:
                lproj = tmp_path / f"{lang}.lproj"
                self.assertTrue(lproj.is_dir(), f"Missing {lproj}")
                strings_file = lproj / "Localizable.strings"
                self.assertTrue(strings_file.is_file(), f"Missing {strings_file}")

    def test_sample_translation_parity(self):
        data = json.loads(XSTRINGS_PATH.read_text(encoding="utf-8"))
        strings = data["strings"]
        
        # Check known keys across languages
        self.assertEqual(
            strings["Games"]["localizations"]["zh-Hans"]["stringUnit"]["value"],
            "游戏"
        )
        self.assertEqual(
            strings["Settings"]["localizations"]["es"]["stringUnit"]["value"],
            "Ajustes"
        )
        self.assertEqual(
            strings["Resume"]["localizations"]["de"]["stringUnit"]["value"],
            "Fortsetzen"
        )
        self.assertEqual(
            strings["Resume"]["localizations"]["ar"]["stringUnit"]["value"],
            "استئناف"
        )
        self.assertEqual(
            strings["Enable RetroAchievements"]["localizations"]["it"]["stringUnit"]["value"],
            "Abilita RetroAchievements"
        )

    def test_slice3_parameterized_keys(self):
        data = json.loads(XSTRINGS_PATH.read_text(encoding="utf-8"))
        strings = data["strings"]
        
        slice3_keys = [
            ("Removed about %@ of generated data.", ["%@"]),
            ("Some files could not be removed:", []),
            ("VM is currently running.\nShut down and start %@?", ["%@"]),
            ("The game database is setting this for %@.", ["%@"]),
            ("The game database is setting this for this game.", []),
            ("Saved for %1$@. %2$@", ["%1$@", "%2$@"]),
            ("Default restores global settings; other presets change only their listed settings.", []),
            ("%@ s", ["%@"]),
            ("Last updated %@", ["%@"]),
        ]
        
        for key, specifiers in slice3_keys:
            self.assertIn(key, strings, f"Missing key: {key}")
            locs = strings[key].get("localizations", {})
            for lang in EXPECTED_LANGUAGES[1:]:  # all non-English languages
                self.assertIn(lang, locs, f"Missing {lang} translation for {key}")
                val = locs[lang]["stringUnit"]["value"]
                self.assertTrue(len(val) > 0, f"Empty value for {lang} in {key}")
                for spec in specifiers:
                    self.assertIn(spec, val, f"Missing format specifier {spec} in {lang} translation for {key}")

    def test_slice4_accessibility_and_screens(self):
        data = json.loads(XSTRINGS_PATH.read_text(encoding="utf-8"))
        strings = data["strings"]

        slice4_sample_keys = [
            ("D-pad", []),
            ("Face buttons", []),
            ("Left stick", []),
            ("Dynamic left thumbstick area", []),
            ("Camera swipe area", []),
            ("Game display", []),
            ("Undo", []),
            ("Redo", []),
            ("Show controls", []),
            ("Cheats & Patches", []),
            ("Patch sources", []),
            ("Cheat sources", []),
            ("Ready", []),
            ("Skins", []),
            ("Search skins", []),
            ("Remove %@", ["%@"]),
            ("Preview %@", ["%@"]),
            ("%@ is installed. Reinstall or remove it.", ["%@"]),
            ("This removes the complete patch file and %@ in it. This cannot be undone.", ["%@"]),
            ("1 entry", []),
            ("%d entries", ["%d"]),
            ("Downloading…", []),
            ("Reinstall Patches", []),
            ("Download Patches", []),
            ("Import", []),
        ]

        for key, specifiers in slice4_sample_keys:
            self.assertIn(key, strings, f"Missing key: {key}")
            locs = strings[key].get("localizations", {})
            for lang in EXPECTED_LANGUAGES[1:]:
                self.assertIn(lang, locs, f"Missing {lang} translation for {key}")
                val = locs[lang]["stringUnit"]["value"]
                self.assertTrue(len(val) > 0, f"Empty value for {lang} in {key}")
                for spec in specifiers:
                    self.assertIn(spec, val, f"Missing format specifier {spec} in {lang} translation for {key}")

if __name__ == "__main__":
    unittest.main()

