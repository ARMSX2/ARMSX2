import subprocess
import tempfile
import unittest
from pathlib import Path

from ios_source import SWIFT


LIBRARY = SWIFT / "Models/TexturePackLibrary.swift"

DRIVER = """
import Foundation
let root = URL(fileURLWithPath: CommandLine.arguments[1])
for serial in CommandLine.arguments.dropFirst(2) {
    try TexturePackLibrary.remove(TexturePackLibrary.installed(in: root).first { $0.serial == serial }!)
}
print(TexturePackLibrary.installed(in: root).sorted { $0.serial < $1.serial }.map { "\\($0.serial) \\($0.bytes > 0)" }.joined(separator: ","))
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
        return out.stdout.strip()

    def test_lists_packs_and_removes_only_the_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for serial in ("SLUS-21137", "SCUS-97209"):
                (root / serial / "replacements").mkdir(parents=True)
                (root / serial / "replacements" / "a.png").write_bytes(b"x" * 100)
            (root / "SLUS-21137" / "dumps").mkdir()
            (root / "SLES-50000" / "dumps").mkdir(parents=True)

            self.assertEqual(self.run_driver(root), "SCUS-97209 true,SLUS-21137 true")
            self.assertEqual(self.run_driver(root, "SLUS-21137", "SCUS-97209"), "")
            self.assertTrue((root / "SLUS-21137" / "dumps").is_dir(), "a pack's dumps must survive its removal")
            self.assertFalse((root / "SCUS-97209").exists(), "an emptied serial folder should go too")
            self.assertTrue((root / "SLES-50000" / "dumps").is_dir())


if __name__ == "__main__":
    unittest.main()
