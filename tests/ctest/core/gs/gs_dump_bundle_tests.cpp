// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What a GS dump leaves behind (GS/GSDumpBundle.h): one zip holding the dump, the driver report and
// the screenshot, with the loose files gone. The rule worth pinning is the failure side: when the
// zip cannot be written, every loose file is still there, because the dump is the evidence and the
// zip is only how it travels.

#include "GS/GSDumpBundle.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "zip.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace
{
	class GSDumpBundleTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
			m_dir = (std::filesystem::temp_directory_path() /
					 (std::string("gs_dump_bundle_tests_") + info->name() + "_" + std::to_string(getpid())))
						.string();
			std::filesystem::remove_all(m_dir);
			std::filesystem::create_directories(m_dir);
		}

		void TearDown() override { std::filesystem::remove_all(m_dir); }

		std::string PathOf(const std::string& name) const { return Path::Combine(m_dir, name); }

		// Bytes that do not compress, so a stored entry and a deflated one differ in size.
		static std::vector<u8> Noise(u32 seed, size_t size)
		{
			std::vector<u8> v(size);
			u32 x = seed * 2654435761u + 1;
			for (u8& b : v)
			{
				x = x * 1664525u + 1013904223u;
				b = static_cast<u8>(x >> 24);
			}
			return v;
		}

		std::string Write(const std::string& name, const std::vector<u8>& bytes) const
		{
			const std::string path = PathOf(name);
			EXPECT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes.data(), bytes.size()));
			return path;
		}

		static std::unique_ptr<zip_t, void (*)(zip_t*)> OpenZip(const std::string& path)
		{
			int err = 0;
			zip_t* zf = zip_open(path.c_str(), ZIP_RDONLY, &err);
			return std::unique_ptr<zip_t, void (*)(zip_t*)>(zf, [](zip_t* z) {
				if (z)
					zip_close(z);
			});
		}

		static zip_int64_t Locate(zip_t* zf, const std::string& name)
		{
			return zip_name_locate(zf, name.c_str(), ZIP_FL_ENC_UTF_8);
		}

		static std::vector<u8> Read(zip_t* zf, const std::string& name)
		{
			const zip_int64_t index = Locate(zf, name);
			zip_stat_t st;
			if (index < 0 || zip_stat_index(zf, index, 0, &st) != 0)
				return {};
			std::vector<u8> out(st.size);
			zip_file_t* file = zip_fopen_index(zf, index, 0);
			if (!file)
				return {};
			const zip_int64_t got = zip_fread(file, out.data(), out.size());
			zip_fclose(file);
			if (got < 0 || static_cast<size_t>(got) != out.size())
				return {};
			return out;
		}

		std::string m_dir;
	};
} // namespace

TEST(GSDumpBundleName, ZipPathKeepsTheDumpFamilyExtension)
{
	// ".gs.zip" sits with ".gs.zst" and ".gs.xz": a dump bundle is told from any other zip by name.
	EXPECT_EQ(GSDumpBundle::ZipPath("/snaps/Game_SLUS-20000_20260101000000"), "/snaps/Game_SLUS-20000_20260101000000.gs.zip");
}

TEST_F(GSDumpBundleTest, PacksTheThreeFilesIntoOneZipAndRemovesTheLooseOnes)
{
	const std::vector<u8> dump = Noise(1, 300000);
	const std::vector<u8> report = {'{', '"', 'a', '"', ':', '1', '}'};
	const std::vector<u8> shot = Noise(3, 40000);
	const std::string dump_path = Write("g.gs.zst", dump);
	const std::string report_path = Write("g.driver.json", report);
	const std::string shot_path = Write("g.png", shot);
	const std::string zip_path = PathOf("g.gs.zip");

	ASSERT_TRUE(GSDumpBundle::Pack(zip_path, {dump_path, report_path, shot_path}));

	EXPECT_FALSE(FileSystem::FileExists(dump_path.c_str()));
	EXPECT_FALSE(FileSystem::FileExists(report_path.c_str()));
	EXPECT_FALSE(FileSystem::FileExists(shot_path.c_str()));

	auto zf = OpenZip(zip_path);
	ASSERT_TRUE(zf);
	EXPECT_EQ(zip_get_num_entries(zf.get(), 0), 3);
	EXPECT_EQ(Read(zf.get(), "g.gs.zst"), dump);
	EXPECT_EQ(Read(zf.get(), "g.driver.json"), report);
	EXPECT_EQ(Read(zf.get(), "g.png"), shot);
}

// Nothing else is in the folder afterwards: "only a single zip file" is the point.
TEST_F(GSDumpBundleTest, LeavesNothingButTheZipInTheFolder)
{
	const std::string a = Write("g.gs.zst", Noise(1, 1000));
	const std::string b = Write("g.driver.json", Noise(2, 100));
	const std::string c = Write("g.png", Noise(3, 1000));

	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {a, b, c}));

	std::vector<std::string> left;
	for (const auto& entry : std::filesystem::directory_iterator(m_dir))
		left.push_back(entry.path().filename().string());
	EXPECT_EQ(left, std::vector<std::string>{"g.gs.zip"});
}

// The dump and the screenshot are compressed formats already: they are stored, not deflated again.
// The bytes here are zeros on purpose: libzip stores incompressible data by itself, so noise would
// pass even if the parts were handed to deflate.
TEST_F(GSDumpBundleTest, StoresTheAlreadyCompressedParts)
{
	const std::string dump_path = Write("g.gs.zst", std::vector<u8>(50000, 0));
	const std::string shot_path = Write("g.png", std::vector<u8>(50000, 0));
	const std::string xz_path = Write("h.gs.xz", std::vector<u8>(50000, 0));
	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {dump_path, shot_path, xz_path}));

	auto zf = OpenZip(PathOf("g.gs.zip"));
	ASSERT_TRUE(zf);
	for (const char* name : {"g.gs.zst", "g.png", "h.gs.xz"})
	{
		zip_stat_t st;
		ASSERT_EQ(zip_stat(zf.get(), name, 0, &st), 0) << name;
		EXPECT_EQ(st.comp_method, ZIP_CM_STORE) << name;
		EXPECT_EQ(st.comp_size, st.size) << name;
	}
}

TEST_F(GSDumpBundleTest, ADumpWrittenUncompressedIsDeflated)
{
	// An uncompressed .gs is the one part that is not compressed already, and it is mostly zeros.
	const std::string dump_path = Write("g.gs", std::vector<u8>(200000, 0));
	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {dump_path}));

	auto zf = OpenZip(PathOf("g.gs.zip"));
	ASSERT_TRUE(zf);
	zip_stat_t st;
	ASSERT_EQ(zip_stat(zf.get(), "g.gs", 0, &st), 0);
	EXPECT_LT(st.comp_size, st.size / 10);
}

// The screenshot can fail on its own ("Failed to render/download screenshot."). The dump and the
// report are still worth handing over.
TEST_F(GSDumpBundleTest, APartThatWasNeverWrittenIsLeftOut)
{
	const std::vector<u8> dump = Noise(1, 5000);
	const std::string dump_path = Write("g.gs.zst", dump);
	const std::string report_path = Write("g.driver.json", Noise(2, 100));

	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {dump_path, report_path, PathOf("g.png")}));

	auto zf = OpenZip(PathOf("g.gs.zip"));
	ASSERT_TRUE(zf);
	EXPECT_EQ(zip_get_num_entries(zf.get(), 0), 2);
	EXPECT_EQ(Read(zf.get(), "g.gs.zst"), dump);
	EXPECT_LT(Locate(zf.get(), "g.png"), 0);
}

TEST_F(GSDumpBundleTest, NothingToPackWritesNoZip)
{
	EXPECT_FALSE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {PathOf("g.gs.zst"), PathOf("g.png")}));
	EXPECT_FALSE(FileSystem::FileExists(PathOf("g.gs.zip").c_str()));
}

// The rule the feature must not break: a zip that cannot be written costs the packaging, never the dump.
TEST_F(GSDumpBundleTest, AZipThatCannotBeWrittenKeepsEveryLooseFile)
{
	const std::vector<u8> dump = Noise(1, 5000);
	const std::vector<u8> report = Noise(2, 100);
	const std::string dump_path = Write("g.gs.zst", dump);
	const std::string report_path = Write("g.driver.json", report);

	EXPECT_FALSE(GSDumpBundle::Pack(PathOf("no_such_folder/g.gs.zip"), {dump_path, report_path}));

	EXPECT_TRUE(FileSystem::FileExists(dump_path.c_str()));
	EXPECT_TRUE(FileSystem::FileExists(report_path.c_str()));
	EXPECT_EQ(FileSystem::GetPathFileSize(dump_path.c_str()), static_cast<s64>(dump.size()));
	EXPECT_EQ(FileSystem::GetPathFileSize(report_path.c_str()), static_cast<s64>(report.size()));
	EXPECT_FALSE(FileSystem::FileExists(PathOf("no_such_folder/g.gs.zip").c_str()));
}

// Two dumps in the same second get the same base name apart from a counter, and a leftover zip from
// an earlier run must not survive underneath a new one.
TEST_F(GSDumpBundleTest, ReplacesAZipAlreadyAtThatPath)
{
	const std::string zip_path = PathOf("g.gs.zip");
	const std::vector<u8> stale = Noise(9, 777);
	Write("g.gs.zip", stale);

	const std::vector<u8> dump = Noise(1, 5000);
	ASSERT_TRUE(GSDumpBundle::Pack(zip_path, {Write("g.gs.zst", dump)}));

	auto zf = OpenZip(zip_path);
	ASSERT_TRUE(zf);
	EXPECT_EQ(zip_get_num_entries(zf.get(), 0), 1);
	EXPECT_EQ(Read(zf.get(), "g.gs.zst"), dump);
}

// Game titles are in the file name: "Ōkami", "Ratchet & Clank".
TEST_F(GSDumpBundleTest, NonAsciiNamesSurvive)
{
	const std::string name = "\xC5\x8Ckami_SLUS-21000_20260101000000.gs.zst";
	const std::vector<u8> dump = Noise(1, 5000);
	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("o.gs.zip"), {Write(name, dump)}));

	auto zf = OpenZip(PathOf("o.gs.zip"));
	ASSERT_TRUE(zf);
	EXPECT_EQ(Read(zf.get(), name), dump);
}

TEST_F(GSDumpBundleTest, PartsInOtherFoldersAreNamedByTheirFileNameAlone)
{
	std::filesystem::create_directories(PathOf("sub"));
	const std::vector<u8> dump = Noise(1, 5000);
	ASSERT_TRUE(GSDumpBundle::Pack(PathOf("g.gs.zip"), {Write("sub/g.gs.zst", dump)}));

	auto zf = OpenZip(PathOf("g.gs.zip"));
	ASSERT_TRUE(zf);
	EXPECT_EQ(Read(zf.get(), "g.gs.zst"), dump);
}
