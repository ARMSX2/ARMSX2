// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "catalog_test_support.h"
#include "tar_test_support.h"

#include "Catalog/TarGz.h"

#include "common/FileSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

using namespace Catalog;
using CatalogTest::BuildTar;
using CatalogTest::BuildTarGz;
using CatalogTest::Gzip;
using CatalogTest::ScopedTempDir;
using CatalogTest::TarGzEntry;
using CatalogTest::WriteBytes;

namespace
{
	struct Extraction
	{
		TarResult result = TarResult::Failed;
		std::string error;
		TarExtractInfo info;
	};

	Extraction Extract(const ScopedTempDir& dir, const std::string& gz_bytes, const TarLimits& limits = {}, const std::atomic_bool* cancel = nullptr)
	{
		Extraction out;
		const std::string archive = dir.File("archive.tar.gz");
		EXPECT_TRUE(WriteBytes(archive, gz_bytes));
		out.result = ExtractTarGz(archive, dir.File("out"), limits, &out.info, &out.error, cancel);
		return out;
	}

	std::string Read(const ScopedTempDir& dir, const std::string& relative)
	{
		return FileSystem::ReadFileToString(dir.File("out/" + relative).c_str()).value_or("<missing>");
	}

	bool Exists(const ScopedTempDir& dir, const std::string& relative)
	{
		std::error_code ec;
		return std::filesystem::exists(dir.File("out/" + relative), ec) || std::filesystem::is_symlink(dir.File("out/" + relative), ec);
	}

	TarGzEntry File(std::string name, std::string data, unsigned mode = 0644)
	{
		TarGzEntry e;
		e.name = std::move(name);
		e.data = std::move(data);
		e.mode = mode;
		return e;
	}

	TarGzEntry Dir(std::string name)
	{
		TarGzEntry e;
		e.name = std::move(name);
		e.type = '5';
		e.mode = 0755;
		return e;
	}
} // namespace

// ---- paths ---------------------------------------------------------------------------------

TEST(TarGzPath, NormalisesHarmlessForms)
{
	std::string out;
	ASSERT_TRUE(NormalizeArchivePath("a/b/c.txt", &out));
	EXPECT_EQ(out, "a/b/c.txt");
	ASSERT_TRUE(NormalizeArchivePath("./a//b/./c", &out));
	EXPECT_EQ(out, "a/b/c");
	ASSERT_TRUE(NormalizeArchivePath("dir/", &out));
	EXPECT_EQ(out, "dir");
	ASSERT_TRUE(NormalizeArchivePath("./", &out));
	EXPECT_EQ(out, "");
	ASSERT_TRUE(NormalizeArchivePath("", &out));
	EXPECT_EQ(out, "");
}

TEST(TarGzPath, RefusesUnsafeForms)
{
	std::string out, error;
	EXPECT_FALSE(NormalizeArchivePath("/etc/passwd", &out, &error));
	EXPECT_NE(error.find("absolute"), std::string::npos);
	EXPECT_FALSE(NormalizeArchivePath("../evil", &out));
	EXPECT_FALSE(NormalizeArchivePath("a/../../evil", &out));
	EXPECT_FALSE(NormalizeArchivePath("a/b/..", &out));
	EXPECT_FALSE(NormalizeArchivePath("a\\b", &out));
	EXPECT_FALSE(NormalizeArchivePath(std::string("a\0b", 3), &out));
	EXPECT_FALSE(NormalizeArchivePath(std::string(256, 'x'), &out)); // one component over 255 bytes
	EXPECT_FALSE(NormalizeArchivePath(std::string(4097, 'x'), &out));

	std::string deep;
	for (int i = 0; i < 33; i++)
		deep += "d/";
	deep += "f";
	EXPECT_FALSE(NormalizeArchivePath(deep, &out)); // 34 components
	EXPECT_FALSE(NormalizeArchivePath(deep.substr(2), &out)); // 33
	EXPECT_TRUE(NormalizeArchivePath(deep.substr(4), &out)); // 32 is the limit
}

// ---- extraction ----------------------------------------------------------------------------

TEST(TarGzExtract, WritesFilesAndDirectories)
{
	ScopedTempDir dir;
	const Extraction e = Extract(dir, BuildTarGz({
										  Dir("turnip/"),
										  Dir("turnip/axfl2-002/"),
										  File("turnip/axfl2-002/a.json", "{\"a\":1}"),
										  File("turnip/axfl2-002/lib.so", "ELF-ish", 0755),
										  Dir("turnip/axfl2-002/empty/"),
										  File("top.txt", ""),
									  }));
	ASSERT_EQ(e.result, TarResult::Ok) << e.error;

	EXPECT_EQ(Read(dir, "turnip/axfl2-002/a.json"), "{\"a\":1}");
	EXPECT_EQ(Read(dir, "turnip/axfl2-002/lib.so"), "ELF-ish");
	EXPECT_EQ(Read(dir, "top.txt"), "");
	EXPECT_TRUE(std::filesystem::is_directory(dir.File("out/turnip/axfl2-002/empty")));

	EXPECT_EQ(e.info.files.size(), 3u);
	EXPECT_NE(std::find(e.info.files.begin(), e.info.files.end(), "turnip/axfl2-002/lib.so"), e.info.files.end());
	EXPECT_EQ(e.info.bytes_written, 7u + 7u);

	const auto lib_perms = std::filesystem::status(dir.File("out/turnip/axfl2-002/lib.so")).permissions();
	const auto json_perms = std::filesystem::status(dir.File("out/turnip/axfl2-002/a.json")).permissions();
	EXPECT_NE(lib_perms & std::filesystem::perms::owner_exec, std::filesystem::perms::none);
	EXPECT_EQ(json_perms & std::filesystem::perms::owner_exec, std::filesystem::perms::none);
}

TEST(TarGzExtract, FileDataSpanningManyBlocksAndChunksSurvives)
{
	ScopedTempDir dir;
	std::string big(300000, '\0');
	for (size_t i = 0; i < big.size(); i++)
		big[i] = static_cast<char>((i * 31 + i / 509) & 0xFF);
	const Extraction e = Extract(dir, BuildTarGz({File("big.bin", big), File("after.txt", "still aligned")}));
	ASSERT_EQ(e.result, TarResult::Ok) << e.error;
	EXPECT_EQ(Read(dir, "big.bin"), big);
	EXPECT_EQ(Read(dir, "after.txt"), "still aligned");
}

TEST(TarGzExtract, SkipsLinksAndNeverCreatesThem)
{
	ScopedTempDir dir;
	TarGzEntry symlink;
	symlink.name = "sneaky";
	symlink.type = '2';
	symlink.link_target = "/etc";
	TarGzEntry hardlink;
	hardlink.name = "hard";
	hardlink.type = '1';
	hardlink.link_target = "real.txt";

	// A later entry written "through" the symlink must land inside the destination as a plain
	// file or fail, never in /etc.
	const Extraction e = Extract(dir, BuildTarGz({File("real.txt", "x"), symlink, hardlink, File("sneaky/passwd", "owned")}));
	ASSERT_EQ(e.result, TarResult::Ok) << e.error;
	EXPECT_EQ(e.info.skipped_entries, 2u);
	EXPECT_FALSE(std::filesystem::is_symlink(dir.File("out/sneaky")));
	EXPECT_FALSE(Exists(dir, "hard"));
	EXPECT_EQ(Read(dir, "sneaky/passwd"), "owned"); // an ordinary directory named sneaky
	EXPECT_EQ(Read(dir, "real.txt"), "x");
}

TEST(TarGzExtract, RefusesPathsThatEscape)
{
	for (const char* bad : {"../evil.txt", "a/../../evil.txt", "/tmp/evil.txt"})
	{
		ScopedTempDir dir;
		const Extraction e = Extract(dir, BuildTarGz({File("fine.txt", "ok"), File(bad, "evil")}));
		EXPECT_EQ(e.result, TarResult::Failed) << bad;
		EXPECT_NE(e.error.find("Unsafe path"), std::string::npos) << e.error;
		EXPECT_FALSE(std::filesystem::exists(dir.File("evil.txt"))) << bad;
	}
}

TEST(TarGzExtract, UnderstandsLongNameConventions)
{
	const std::string long_dir(60, 'd');
	const std::string long_path = long_dir + "/" + long_dir + "/" + std::string(40, 'f') + ".so";
	ASSERT_GT(long_path.size(), 100u);

	for (const auto naming : {TarGzEntry::Naming::Plain, TarGzEntry::Naming::GnuLongName, TarGzEntry::Naming::Pax})
	{
		ScopedTempDir dir;
		TarGzEntry entry = File(long_path, "payload");
		entry.naming = naming;
		const Extraction e = Extract(dir, BuildTarGz({entry, File("short.txt", "s")}));
		ASSERT_EQ(e.result, TarResult::Ok) << e.error << " naming=" << static_cast<int>(naming);
		EXPECT_EQ(Read(dir, long_path), "payload") << static_cast<int>(naming);
		EXPECT_EQ(Read(dir, "short.txt"), "s");
	}
}

TEST(TarGzExtract, ARealGnuTarArchiveWorks)
{
	// Made by GNU tar from the layout of the real axfl2-002 pack; checked in as a fixture.
	ScopedTempDir dir;
	const std::string archive = CatalogTest::FixturePath("drivers/turnip-synth-aarch64.tar.gz");
	TarExtractInfo info;
	std::string error;
	ASSERT_EQ(ExtractTarGz(archive, dir.File("out"), {}, &info, &error), TarResult::Ok) << error;
	EXPECT_EQ(info.files.size(), 6u);
	EXPECT_TRUE(Exists(dir, "turnip/axfl2-002/freedreno_icd.aarch64.json"));
	EXPECT_TRUE(Exists(dir, "turnip/axfl2-002/lib-fallback/libz.so.1"));
}

// ---- hostile or damaged archives -----------------------------------------------------------------

TEST(TarGzExtract, NotGzipIsRefused)
{
	ScopedTempDir dir;
	const Extraction e = Extract(dir, "this is not a gzip file at all");
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_FALSE(e.error.empty());
}

TEST(TarGzExtract, PlainTarWithoutGzipIsRefused)
{
	ScopedTempDir dir;
	const Extraction e = Extract(dir, BuildTar({File("a.txt", "a")}));
	EXPECT_EQ(e.result, TarResult::Failed);
}

TEST(TarGzExtract, TruncatedGzipIsRefused)
{
	ScopedTempDir dir;
	std::string gz = BuildTarGz({File("a.bin", std::string(20000, 'q'))});
	gz.resize(gz.size() / 2);
	const Extraction e = Extract(dir, gz);
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("truncated"), std::string::npos) << e.error;
}

TEST(TarGzExtract, CorruptedGzipTrailerIsCaught)
{
	// The last 8 bytes are CRC32 and length; flipping a CRC byte must not go unnoticed even though
	// every tar entry before it parsed fine.
	ScopedTempDir dir;
	std::string gz = BuildTarGz({File("a.txt", "hello")});
	gz[gz.size() - 8] ^= 0x55;
	const Extraction e = Extract(dir, gz);
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("corrupt"), std::string::npos) << e.error;
}

TEST(TarGzExtract, BadTarChecksumIsRefused)
{
	ScopedTempDir dir;
	TarGzEntry bad = File("a.txt", "a");
	bad.corrupt_checksum = true;
	const Extraction e = Extract(dir, BuildTarGz({bad}));
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("checksum"), std::string::npos) << e.error;
}

TEST(TarGzExtract, ArchiveCutMidFileIsRefused)
{
	ScopedTempDir dir;
	std::string tar = BuildTar({File("a.bin", std::string(5000, 'z'))});
	tar.resize(512 + 1000); // header and a fifth of the data
	const Extraction e = Extract(dir, Gzip(tar));
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("ends in the middle"), std::string::npos) << e.error;
}

TEST(TarGzExtract, OversizedFileIsRefusedBeforeItIsWritten)
{
	ScopedTempDir dir;
	TarLimits limits;
	limits.max_file_bytes = 1000;
	const Extraction e = Extract(dir, BuildTarGz({File("small.txt", "ok"), File("big.bin", std::string(1001, 'b'))}), limits);
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("big.bin"), std::string::npos) << e.error;
	EXPECT_FALSE(Exists(dir, "big.bin"));
}

TEST(TarGzExtract, TooManyEntriesIsRefused)
{
	ScopedTempDir dir;
	TarLimits limits;
	limits.max_entries = 3;
	const Extraction e = Extract(dir, BuildTarGz({File("1", "a"), File("2", "a"), File("3", "a"), File("4", "a")}), limits);
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("more than 3 entries"), std::string::npos) << e.error;
}

TEST(TarGzExtract, DecompressionBombIsStopped)
{
	// 8 MiB of zeros gzips to a few KB. With a 1 MiB expansion cap it must stop early.
	ScopedTempDir dir;
	TarLimits limits;
	limits.max_total_bytes = 1u << 20;
	limits.max_file_bytes = 64u << 20;
	const Extraction e = Extract(dir, BuildTarGz({File("zeros.bin", std::string(8u << 20, '\0'))}), limits);
	EXPECT_EQ(e.result, TarResult::Failed);
	EXPECT_NE(e.error.find("expands to more than"), std::string::npos) << e.error;
}

TEST(TarGzExtract, CancellationStopsTheExtraction)
{
	ScopedTempDir dir;
	std::atomic_bool cancel{true};
	const Extraction e = Extract(dir, BuildTarGz({File("a.txt", "a")}), {}, &cancel);
	EXPECT_EQ(e.result, TarResult::Cancelled);
}

TEST(TarGzExtract, MissingArchiveFailsCleanly)
{
	ScopedTempDir dir;
	std::string error;
	EXPECT_EQ(ExtractTarGz(dir.File("nope.tar.gz"), dir.File("out"), {}, nullptr, &error), TarResult::Failed);
	EXPECT_FALSE(error.empty());
}

// ---- damage anywhere ---------------------------------------------------------------------------------

TEST(TarGzExtract, EveryTruncationOfARealArchiveFailsCleanly)
{
	const std::string gz = FileSystem::ReadFileToString(CatalogTest::FixturePath("drivers/turnip-synth-aarch64.tar.gz").c_str()).value_or("");
	ASSERT_GT(gz.size(), 100u);

	// Cutting the file anywhere short of its end must be refused (the gzip trailer is missing), not
	// extracted as if complete and not crash.
	for (size_t keep = 0; keep < gz.size(); keep += 7)
	{
		ScopedTempDir dir;
		const Extraction e = Extract(dir, gz.substr(0, keep));
		EXPECT_NE(e.result, TarResult::Ok) << "kept " << keep << " of " << gz.size() << " bytes";
	}
}

TEST(TarGzExtract, BitFlipsNeverEscapeTheDestinationOrCrash)
{
	const std::string gz = FileSystem::ReadFileToString(CatalogTest::FixturePath("drivers/turnip-synth-aarch64.tar.gz").c_str()).value_or("");
	ASSERT_GT(gz.size(), 100u);

	u32 state = 12345;
	const auto next = [&state]() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};

	ScopedTempDir dir;
	for (int i = 0; i < 400; i++)
	{
		std::string damaged = gz;
		damaged[next() % damaged.size()] ^= static_cast<char>(1u << (next() % 8));
		std::filesystem::remove_all(dir.File("out"));
		const Extraction e = Extract(dir, damaged);
		if (e.result == TarResult::Ok)
		{
			// A flip the format cannot detect (inside file data, or a name character that is still
			// safe) may extract; whatever came out must still be inside `out`.
			for (const std::string& file : e.info.files)
			{
				EXPECT_EQ(file.find(".."), std::string::npos) << file;
				EXPECT_NE(file.front(), '/') << file;
			}
		}
		EXPECT_FALSE(std::filesystem::exists(dir.File("passwd")));
	}
}
