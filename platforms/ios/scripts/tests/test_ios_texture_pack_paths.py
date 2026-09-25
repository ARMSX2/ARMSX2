import subprocess
import tempfile
import unittest
from pathlib import Path

from ios_source import CPP


HEADER = CPP / "IOS/TexturePackPaths.h"

DRIVER = r"""
#include "TexturePackPaths.h"
#include <iostream>

int main(int argc, char** argv)
{
	const std::string mode = argv[1];
	for (int i = 2; i < argc; i++)
	{
		if (mode == "classify")
		{
			const TexturePackPaths::Entry entry = TexturePackPaths::Classify(argv[i]);
			const char* kind = entry.kind == TexturePackPaths::Kind::Texture ? "texture" :
				entry.kind == TexturePackPaths::Kind::Skip ? "skip" : "unsafe";
			std::cout << kind << " " << entry.path << "\n";
		}
		else
		{
			const auto serial = mode == "folder" ? TexturePackPaths::SerialFolderIn(argv[i]) :
				TexturePackPaths::FindSerial(argv[i]);
			std::cout << serial.value_or("none") << "\n";
		}
	}
	return 0;
}
"""


class TexturePackPathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        build = Path(cls.build.name)
        (build / "main.cpp").write_text(DRIVER, encoding="utf-8")
        cls.exe = build / "driver"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-I", str(HEADER.parent), str(build / "main.cpp"),
                        "-o", str(cls.exe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_driver(self, mode, *names):
        out = subprocess.run([str(self.exe), mode, *names], check=True, capture_output=True, text=True)
        return [line.strip() for line in out.stdout.splitlines()]

    def test_wrappers_are_stripped_the_way_android_does(self):
        self.assertEqual(self.run_driver(
            "classify",
            "replacements/a.png",
            "SLUS-21137/replacements/sub/a.dds",
            "SLUS-21137/a.ktx",
            "pack-0123456789abcdef0123456789abcdef01234567/a.astc",
            "Some Pack/a.png",
        ), ["texture a.png", "texture sub/a.dds", "texture a.ktx", "texture a.astc", "texture Some Pack/a.png"])

    def test_junk_and_non_textures_are_skipped(self):
        self.assertEqual(self.run_driver(
            "classify", "__MACOSX/replacements/._a.png", "replacements/.DS_Store", "replacements/readme.txt",
            "replacements/", "Thumbs.db",
        ), ["skip"] * 5)

    def test_escaping_paths_fail_the_whole_archive(self):
        self.assertEqual(self.run_driver(
            "classify", "../a.png", "replacements/../../a.png", "/a.png", "C:/a.png", "..\\a.png",
        ), ["unsafe"] * 5)

    def test_serials_come_from_folders_and_names(self):
        self.assertEqual(self.run_driver("folder", "SCUS-97209/replacements/a.png", "replacements/a.png"),
                         ["SCUS-97209", "none"])
        self.assertEqual(self.run_driver(
            "name", "ia-tom-clancy-s-splinter-cell-chaos-theory-slus-21137.zip", "SLUS_212.87", "pack.zip",
        ), ["SLUS-21137", "SLUS-21287", "none"])


if __name__ == "__main__":
    unittest.main()
