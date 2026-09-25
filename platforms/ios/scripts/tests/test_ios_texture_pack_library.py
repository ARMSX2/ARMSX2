import subprocess
import tempfile
import unittest
from pathlib import Path

from ios_source import SWIFT


LIBRARY = SWIFT / "Models/TexturePackLibrary.swift"

DRIVER = """
import Foundation
let root = URL(fileURLWithPath: CommandLine.arguments[1])
var commands = CommandLine.arguments.dropFirst(2)
while let command = commands.popFirst() {
    switch command {
    case "sweep":
        TexturePackLibrary.sweepStaging(in: root)
    case "mark":
        let id = commands.popFirst()!, serial = commands.popFirst()!
        try TexturePackLibrary.markCatalogPack(id, in: root.appendingPathComponent("\\(serial)/replacements"))
    default:
        try TexturePackLibrary.remove(TexturePackLibrary.installed(in: root).first { $0.serial == command }!)
    }
}
print(TexturePackLibrary.installed(in: root).sorted { $0.serial < $1.serial }.map { pack in
    let excluded = (try? pack.folder.resourceValues(forKeys: [.isExcludedFromBackupKey]))?.isExcludedFromBackup
    return "\\(pack.serial) \\(pack.bytes > 0) \\(excluded == true)"
}.joined(separator: ","))
print(TexturePackLibrary.catalogIDs(in: root).sorted().joined(separator: ","))
"""


class TexturePackLibraryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        build = Path(cls.build.name)
        (build / "main.swift").write_text(DRIVER, encoding="utf-8")
        cls.exe = build / "driver"
        subprocess.run(["xcrun", "swiftc", str(LIBRARY), str(build / "main.swift"), "-o", str(cls.exe)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_driver(self, root, *remove):
        out = subprocess.run([str(self.exe), str(root), *remove], check=True, capture_output=True, text=True)
        return out.stdout.split("\n")[:2]

    def test_lists_packs_and_removes_only_the_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for serial in ("SLUS-21137", "SCUS-97209"):
                (root / serial / "replacements").mkdir(parents=True)
                (root / serial / "replacements" / "a.png").write_bytes(b"x" * 100)
            (root / "SLUS-21137" / "dumps").mkdir()
            (root / "SLES-50000" / "dumps").mkdir(parents=True)

            self.assertEqual(self.run_driver(root)[0], "SCUS-97209 true false,SLUS-21137 true false")
            self.assertEqual(self.run_driver(root, "SLUS-21137", "SCUS-97209")[0], "")
            self.assertTrue((root / "SLUS-21137" / "dumps").is_dir(), "a pack's dumps must survive its removal")
            self.assertFalse((root / "SCUS-97209").exists(), "an emptied serial folder should go too")
            self.assertTrue((root / "SLES-50000" / "dumps").is_dir())

    def test_catalog_marks_follow_the_pack_and_the_sweep_clears_leftovers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for serial in ("SLUS-20486", "SLES-50000"):
                (root / serial / "replacements").mkdir(parents=True)
                (root / serial / "replacements" / "a.png").write_bytes(b"x" * 100)
            (root / ".import-A" / "SLUS-20486").mkdir(parents=True)
            (root / ".import-B").mkdir()
            (root / ".import-B" / "pack.tar.zst").write_bytes(b"x")

            listed, ids = self.run_driver(root, "mark", "p1", "SLUS-20486", "mark", "p2", "SLUS-20486",
                                          "mark", "p1", "SLUS-20486", "mark", "p3", "SLES-50000", "sweep")
            self.assertEqual(listed, "SLES-50000 true true,SLUS-20486 true true")
            self.assertEqual(ids, "p1,p2,p3")
            self.assertEqual(sorted(p.name for p in root.iterdir()), ["SLES-50000", "SLUS-20486"])
            self.assertEqual(self.run_driver(root, "SLUS-20486"), ["SLES-50000 true true", "p3"])


if __name__ == "__main__":
    unittest.main()
