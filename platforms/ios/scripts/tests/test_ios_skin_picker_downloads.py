"""Opening a skin picker downloads nothing; applying a listed catalog skin downloads it."""

import unittest

from ios_source import SWIFT, block, read, without_comments


INSTALLER = SWIFT / "Models/SkinInstaller.swift"


class SkinPickersDownloadOnApply(unittest.TestCase):
    """Opening either picker downloaded and installed every compatible catalog skin, the
    universal ones included, before anything was picked and with Automatic Download
    Custom Skin off."""

    @classmethod
    def setUpClass(cls):
        cls.installer = without_comments(read(INSTALLER))

    def test_the_listing_installs_nothing(self):
        listing = block(self.installer, "func catalogProposals(")
        for install in ("installCatalogProposals(", "installCatalogSkin(", "installForAutomaticAssignment("):
            with self.subTest(install=install):
                self.assertNotIn(install, listing, "the picker listing installs skins again")
        self.assertNotIn("downloadAvailableProposals", self.installer)

    def test_applying_a_listed_skin_installs_it(self):
        self.assertIn("installCatalogSkin(", block(self.installer, "func installIfNeeded("))
        library = without_comments(read(SWIFT / "Views/GameListView.swift"))
        game = without_comments(read(SWIFT / "Views/GameScreenView.swift"))
        self.assertIn("== skinAtPick", block(library, "private func completeAutomaticCustomSkinLaunch("),
                      "a download must not land over a skin chosen after the pick")
        self.assertIn("installIfNeeded(", block(library, "private func completeAutomaticCustomSkinLaunch("),
                      "the library picker applies a listed skin without downloading it")
        self.assertIn("installIfNeeded(", block(game, "private func applyRuntimeControllerSkinSelection("),
                      "the in-game picker applies a listed skin without downloading it")


if __name__ == "__main__":
    unittest.main()
