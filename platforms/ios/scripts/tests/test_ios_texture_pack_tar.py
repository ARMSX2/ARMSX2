import io
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from ios_source import CPP


HEADER = CPP / "IOS/TexturePackTar.h"

DRIVER = r"""
#include "TexturePackTar.h"
#include <cstdio>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
	FILE* fp = std::fopen(argv[1], "rb");
	const auto read = [fp](void* buffer, size_t n) { return n == 0 || std::fread(buffer, 1, n, fp) == n; };
	std::string error;
	const bool ok = TexturePackTar::Read(read, [&](const std::string& name, uint64_t size) {
		std::vector<char> body(size);
		if (!read(body.data(), body.size()))
			return false;
		std::cout << name << " " << size << "\n";
		return true;
	}, error);
	std::cout << (ok ? "ok" : "error: " + error) << "\n";
	return 0;
}
"""

LONG = "./replacements/" + "a" * 120 + ".ktx"


def pack(entries, fmt=tarfile.GNU_FORMAT):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w", format=fmt) as tar:
        for name, data in entries:
            info = tarfile.TarInfo(name)
            if data is None:
                info.type = tarfile.DIRTYPE
            elif isinstance(data, str):
                info.type = tarfile.SYMTYPE
                info.linkname = data
            else:
                info.size = len(data)
            tar.addfile(info, io.BytesIO(data) if isinstance(data, bytes) else None)
    return out.getvalue()


class TexturePackTarTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.build.name)
        (cls.dir / "main.cpp").write_text(DRIVER, encoding="utf-8")
        cls.exe = cls.dir / "driver"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-I", str(HEADER.parent), str(cls.dir / "main.cpp"),
                        "-o", str(cls.exe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def read(self, data):
        path = self.dir / "pack.tar"
        path.write_bytes(data)
        out = subprocess.run([str(self.exe), str(path)], check=True, capture_output=True, text=True)
        return out.stdout.strip().splitlines()

    def test_files_folders_and_long_names_come_through(self):
        data = pack([("./", None), ("./replacements", None), ("./replacements/a.ktx", b"x" * 1000),
                     (LONG, b"12345"), ("./replacements/empty.png", b"")])
        self.assertEqual(self.read(data), ["./replacements/a.ktx 1000", LONG + " 5", "./replacements/empty.png 0", "ok"])

    def test_links_fail_the_archive(self):
        self.assertTrue(self.read(pack([("./replacements/a.ktx", "../../etc/passwd")]))[-1]
                        .startswith("error: the archive holds an entry that is not a file or folder"))

    def test_a_damaged_header_fails_the_archive(self):
        data = bytearray(pack([("./replacements/a.ktx", b"x")]))
        data[10] ^= 0xFF
        self.assertEqual(self.read(bytes(data))[-1], "error: a tar header is damaged")

    def test_a_cut_off_file_fails_the_archive(self):
        data = pack([("./replacements/a.ktx", b"x" * 5000)])
        self.assertEqual(self.read(data[:512 + 2000])[-1], "error: the archive ends inside ./replacements/a.ktx")

    def test_pax_records_are_outside_the_profile(self):
        self.assertTrue(self.read(pack([(LONG, b"x")], fmt=tarfile.PAX_FORMAT))[-1].startswith("error: "))


if __name__ == "__main__":
    unittest.main()
