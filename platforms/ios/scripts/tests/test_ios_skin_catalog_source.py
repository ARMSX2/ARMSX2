"""Every install downloads and installs what this catalog lists, so its source is pinned
and a move is a visible change to this file."""

import unittest

from ios_source import SWIFT, read


class SkinCatalogSource(unittest.TestCase):
    def test_the_catalog_comes_from_the_agreed_repository(self):
        self.assertIn('static let repo = "bagasromadon/ARMSX2-CustomControllerSkins"',
                      read(SWIFT / "Models/SkinCatalog.swift"),
                      "the skin catalog moved to another repository")


if __name__ == "__main__":
    unittest.main()
