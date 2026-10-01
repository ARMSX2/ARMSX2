"""Saved and imported appearance themes: what a theme file can reach, and what survives it."""

import re
import unittest

from ios_source import SWIFT, block, read, without_comments


GALLERY = SWIFT / "Models/ThemeGalleryStore.swift"


class SavedThemesSurviveABadEntry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gallery = without_comments(read(GALLERY))

    def test_entries_decode_one_at_a_time(self):
        """One entry that fails as an array element used to empty the whole gallery."""
        self.assertNotIn("decode([SavedAppearanceTheme].self", self.gallery,
                         "the saved themes decode as one array again")
        self.assertIn("decode(SavedAppearanceTheme.self", self.gallery,
                      "the saved themes are not decoded entry by entry")

    def test_every_write_keeps_the_undecodable_entries(self):
        """Save, remove and import rewrite the key, so each goes through the helper that
        appends what this build could not read."""
        writes = re.findall(r"defaults\.set\([^\n]*Self\.themesKey", self.gallery)
        self.assertEqual(len(writes), 1, "the saved themes key is written in more than one place")
        self.assertIn("Self.themesKey", block(self.gallery, "private func persistThemes("),
                      "the one write of the saved themes key is not in persistThemes")
        self.assertIn("undecodableThemes", block(self.gallery, "private func persistThemes("),
                      "persistThemes drops the entries this build could not decode")


class ImportedBackgroundNames(unittest.TestCase):
    """BackgroundStorage joins the stored name to its folder, then replaces or removes that
    path, so a shared theme with "../" in the name reached the memory cards and the BIOS."""

    def test_only_a_plain_file_name_decodes(self):
        decoder = block(without_comments(read(SWIFT / "Models/BackgroundAsset.swift")),
                        "init(from decoder: Decoder)")
        self.assertIn("SkinAssetPath.isSafeRelative(filename)", decoder,
                      "BackgroundAsset decodes a filename without the path check")
        self.assertIn('!filename.contains("/")', decoder,
                      "BackgroundAsset decodes a filename that has a folder in it")


class ImportedNumbers(unittest.TestCase):
    """An imported particle amount of 1e300 trapped in Int() on every launch."""

    def test_the_import_checks_every_number_first(self):
        importer = block(without_comments(read(GALLERY)), "func importTheme(")
        self.assertIn("let object = try? JSONSerialization.jsonObject(with: data)", importer,
                      "a file the number check cannot read must be refused")
        self.assertLess(importer.index("numbersAreInRange("), importer.index("JSONDecoder().decode("),
                        "importTheme decodes the document before checking its numbers")

    def test_wallpaper_counts_are_clamped_before_int(self):
        rates = without_comments(read(SWIFT / "Models/UIFrameRateSettings.swift"))
        for helper in ("scaledDynamicEffectCount(", "scaledDynamicFaceButtonCount(",
                       "scaledDynamicGeometryCount("):
            with self.subTest(helper=helper):
                self.assertIn("DynamicBackgroundMath.clamp(", block(rates, f"func {helper}"),
                              f"{helper} converts to Int without a ceiling")


if __name__ == "__main__":
    unittest.main()
