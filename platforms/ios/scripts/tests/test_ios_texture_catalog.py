import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from ios_source import SWIFT


CATALOG = SWIFT / "Models/TextureCatalog.swift"

DRIVER = """
import Foundation
let data = try! Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
guard let packs = TextureCatalog.parse(data) else { print("rejected"); exit(0) }
let owned = Set(CommandLine.arguments.dropFirst(2))
for pack in packs {
    print(pack.id, pack.serials.joined(separator: ","), TextureCatalog.serial(for: pack, owned: owned), pack.fileName)
}
"""

SHA = "AB" * 32


def entry(**changes):
    base = {"id": "p1", "name": "Pack", "gameTitle": "Game", "serials": ["slus 21287", "SLES_525.41"],
            "authors": ["someone"], "sourceUrl": "https://example.org/p", "downloadUrl": "https://example.org/p.tar.zst",
            "sizeBytes": 1000, "sha256": SHA, "format": "tar+zstd", "archiveRevision": 1, "decompressedSizeBytes": 2000}
    base.update(changes)
    return {k: v for k, v in base.items() if v is not None}


class TextureCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.build.name)
        (cls.dir / "main.swift").write_text(DRIVER, encoding="utf-8")
        cls.exe = cls.dir / "driver"
        subprocess.run(["xcrun", "swiftc", str(CATALOG), str(cls.dir / "main.swift"), "-o", str(cls.exe)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_driver(self, catalog, *owned):
        path = self.dir / "catalog.json"
        path.write_text(json.dumps(catalog), encoding="utf-8")
        out = subprocess.run([str(self.exe), str(path), *owned], check=True, capture_output=True, text=True)
        return out.stdout.strip().splitlines()

    def test_bad_entries_drop_themselves_and_good_ones_survive(self):
        catalog = {"schemaVersion": 2, "entries": [
            entry(),
            entry(id="plain-http", downloadUrl="http://example.org/p.tar.zst"),
            entry(id="short-hash", sha256="abc"),
            entry(id="no-size", decompressedSizeBytes=None),
            entry(id="zip-in-schema-2", format="zip"),
            entry(id="split", parts=[{"downloadUrl": "https://example.org/1", "sizeBytes": 1, "sha256": SHA}]),
            entry(id="no-serial", serials=["not a serial"]),
            entry(id="wrong-type", sizeBytes="big"),
            entry(id="../escape"),
            entry(id="two\nlines"),
            entry(name="duplicate id"),
        ]}
        self.assertEqual(self.run_driver(catalog), ["p1 SLUS-21287,SLES-52541 SLUS-21287 p1.tar.zst"])

    def test_the_owned_region_is_installed_rather_than_the_first(self):
        catalog = {"schemaVersion": 2, "entries": [entry()]}
        self.assertEqual(self.run_driver(catalog, "SLES-52541")[0].split()[2], "SLES-52541")

    def test_schema_one_is_zip_only(self):
        catalog = {"schemaVersion": 1, "entries": [entry(format=None, decompressedSizeBytes=None, id="z"),
                                                   entry(id="tar")]}
        self.assertEqual([line.split()[0] for line in self.run_driver(catalog)], ["z"])

    def test_an_unknown_schema_or_nothing_usable_rejects_the_source(self):
        self.assertEqual(self.run_driver({"schemaVersion": 3, "entries": [entry()]}), ["rejected"])
        self.assertEqual(self.run_driver({"schemaVersion": 2, "entries": [entry(sha256="x")]}), ["rejected"])


if __name__ == "__main__":
    unittest.main()
