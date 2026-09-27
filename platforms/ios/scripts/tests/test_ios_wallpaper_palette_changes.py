"""A palette-only change does not stack wallpaper renderers."""

import unittest

from ios_source import SWIFT, block, read, without_comments


PRESENTATION = SWIFT / "Views/Background/Dynamic/DynamicBackgroundPresentation.swift"


class PaletteChangesStayInPlace(unittest.TestCase):
    """Every palette change built a new wallpaper view and kept up to four alive, and with
    Favorite Glow on every focus move onto a favorite changes the palette. A cap of two
    instead removed a still visible layer and flashed the background."""

    @classmethod
    def setUpClass(cls):
        cls.update = block(without_comments(read(PRESENTATION)), "private func updatePresentation(")

    def test_blending_renderers_update_in_place(self):
        self.assertIn("|| (paletteChanges && !blendsPaletteInPlace && !crossfadeRunning)", self.update,
                      "a palette change always builds a new wallpaper view")
        self.assertIn("case .faceButtons:", self.update)
        self.assertIn("!current.theme.usesPlayStation4PaletteBackdrop", self.update,
                      "Mart updates in place while its PS4 backdrop cannot blend")
        self.assertIn("!next.theme.usesPlayStation4PaletteBackdrop", self.update,
                      "Mart updates in place while its PS4 backdrop cannot blend")

    def test_a_palette_change_mid_crossfade_retargets_the_incoming_layer(self):
        self.assertIn("let crossfadeRunning = layers.count > 1", self.update,
                      "palette changes during a crossfade stack wallpaper views again")


if __name__ == "__main__":
    unittest.main()
