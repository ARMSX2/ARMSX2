"""Scan Selected Folder picks up new files without undoing later changes."""

import unittest

from ios_source import SWIFT, block, read, without_comments


BOOTSTRAP = SWIFT / "Models/InitialContentBootstrap.swift"


class RescanLeavesTheLogoAndSounds(unittest.TestCase):
    """Every rescan imported the folder's logo and audio pack again, which restored a
    removed logo, switched the menu sounds back and deleted every per-sound volume."""

    @classmethod
    def setUpClass(cls):
        cls.source = without_comments(read(BOOTSTRAP))

    def test_only_picking_the_folder_imports_them(self):
        self.assertIn("importContents(from: rootURL, importsLogoAndAudioPack: false)",
                      block(self.source, "func scanSelectedFolder("))
        self.assertIn("importContents(from: selectedURL, importsLogoAndAudioPack: true)",
                      block(self.source, "func selectARMSX2Folder("))

    def test_the_imports_follow_the_flag(self):
        contents = block(self.source, "private func importContents(")
        self.assertIn("importsLogoAndAudioPack ? preparation.logoImage : nil", contents)
        self.assertIn("importsLogoAndAudioPack ? preparation.audioPackArchive : nil", contents)


if __name__ == "__main__":
    unittest.main()
