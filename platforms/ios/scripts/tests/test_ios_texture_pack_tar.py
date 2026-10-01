import io
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from ios_source import CPP, ROOT

try:
    from compression import zstd
except ImportError:
    zstd = None


HEADER = CPP / "IOS/TexturePackTar.h"
ZSTD = ROOT / "platforms/android/app/src/main/cpp/3rdparty/zstd/lib"
ZSTD_SOURCES = ["common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c",
                "common/xxhash.c", "common/zstd_common.c", "decompress/huf_decompress.c", "decompress/zstd_ddict.c",
                "decompress/zstd_decompress.c", "decompress/zstd_decompress_block.c"]

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

ZSTD_DRIVER = r"""
#include "TexturePackTar.h"
#include "TexturePackZstd.h"
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
	TexturePackZstd zstd(std::fopen(argv[1], "rb"));
	const auto read = [&zstd](void* buffer, size_t n) { return zstd.Read(buffer, n); };
	std::string error;
	const bool ok = zstd.Valid() && TexturePackTar::Read(read, [&](const std::string& name, uint64_t size) {
		std::vector<unsigned char> body(size);
		if (!read(body.data(), body.size()))
			return false;
		uint32_t hash = 2166136261u;
		for (unsigned char c : body)
			hash = (hash ^ c) * 16777619u;
		std::cout << name << " " << size << " " << hash << "\n";
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


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


@unittest.skipUnless(zstd, "needs Python 3.14's compression.zstd")
class TexturePackZstdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.build.name)
        objects = []
        for source in ZSTD_SOURCES:
            obj = cls.dir / (Path(source).stem + ".o")
            subprocess.run(["xcrun", "clang", "-c", "-O1", "-DZSTD_DISABLE_ASM", "-I", str(ZSTD), str(ZSTD / source),
                            "-o", str(obj)], check=True, capture_output=True)
            objects.append(str(obj))
        (cls.dir / "main.cpp").write_text(ZSTD_DRIVER, encoding="utf-8")
        cls.exe = cls.dir / "driver"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-I", str(HEADER.parent), "-I", str(ZSTD),
                        str(cls.dir / "main.cpp"), *objects, "-o", str(cls.exe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def read(self, data):
        path = self.dir / "pack.tar.zst"
        path.write_bytes(data)
        out = subprocess.run([str(self.exe), str(path)], check=True, capture_output=True, text=True)
        return out.stdout.strip().splitlines()

    def test_files_come_through_the_decoder_intact(self):
        # Many decoder buffers' worth, with small entries straddling their edges.
        big = bytes(3 * 1024 * 1024) + bytes(range(256)) * 4096
        small = [("./replacements/%d.png" % i, bytes([i]) * (i * 37)) for i in range(50)]
        entries = [("./replacements/big.ktx", big)] + small + [(LONG, b"tail")]
        expected = ["%s %d %d" % (name, len(data), fnv(data)) for name, data in entries] + ["ok"]
        self.assertEqual(self.read(zstd.compress(pack(entries), level=19)), expected)

    def test_a_cut_off_stream_fails_the_archive(self):
        data = zstd.compress(pack([("./replacements/a.ktx", bytes(range(256)) * 2000)]))
        self.assertTrue(self.read(data[:len(data) // 2])[-1].startswith("error: "))

    def test_bytes_that_are_not_zstd_fail_the_archive(self):
        self.assertTrue(self.read(pack([("./replacements/a.ktx", b"x")]))[-1].startswith("error: "))


if __name__ == "__main__":
    unittest.main()
