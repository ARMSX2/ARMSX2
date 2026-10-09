// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "catalog_test_support.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"
#include "Catalog/ResponseCache.h"

#include "common/Error.h"
#include "common/FileSystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>

using namespace Catalog;
using CatalogTest::ScopedTempDir;

namespace
{
	// Sets a file's modification time, which is what ResponseCache calls "fetched at".
	void SetFileTime(const std::string& path, std::time_t t)
	{
		const auto system_now = std::chrono::system_clock::now();
		const auto file_now = std::filesystem::file_time_type::clock::now();
		const auto wanted = std::chrono::system_clock::from_time_t(t);
		std::filesystem::last_write_time(path, file_now + (wanted - system_now));
	}

	bool NoTempFilesLeft(const std::string& dir)
	{
		for (const auto& entry : std::filesystem::directory_iterator(dir))
		{
			if (entry.path().extension() == ".tmp")
				return false;
		}
		return true;
	}
} // namespace

// ---- WriteFileAtomic --------------------------------------------------------------------

TEST(CatalogFileUtil, WritesANewFileAndCreatesParentDirectories)
{
	ScopedTempDir dir;
	const std::string path = dir.File("a/b/c.txt");
	Error error;
	ASSERT_TRUE(WriteFileAtomic(path, "hello", &error)) << error.GetDescription();
	EXPECT_EQ(FileSystem::ReadFileToString(path.c_str()).value_or(""), "hello");
	EXPECT_TRUE(NoTempFilesLeft(dir.File("a/b")));
}

TEST(CatalogFileUtil, ReplacesAnExistingFileWholesale)
{
	ScopedTempDir dir;
	const std::string path = dir.File("x.txt");
	ASSERT_TRUE(WriteFileAtomic(path, std::string(10000, 'a')));
	ASSERT_TRUE(WriteFileAtomic(path, "short"));
	EXPECT_EQ(FileSystem::ReadFileToString(path.c_str()).value_or(""), "short");
	EXPECT_TRUE(NoTempFilesLeft(dir.Dir()));
}

TEST(CatalogFileUtil, WritesAnEmptyFile)
{
	ScopedTempDir dir;
	const std::string path = dir.File("empty");
	ASSERT_TRUE(WriteFileAtomic(path, std::string_view()));
	EXPECT_EQ(FileSystem::GetPathFileSize(path.c_str()), 0);
}

TEST(CatalogFileUtil, FailureLeavesTheOriginalAndNoDebris)
{
	ScopedTempDir dir;
	const std::string path = dir.File("target");

	// A directory in the way makes the final rename fail after the data was written.
	ASSERT_TRUE(FileSystem::CreateDirectoryPath(path.c_str(), false));
	ASSERT_TRUE(FileSystem::WriteStringToFile(Path::Combine(path, "inner").c_str(), "x"));

	Error error;
	EXPECT_FALSE(WriteFileAtomic(path, "data", &error));
	EXPECT_TRUE(error.IsValid());
	EXPECT_TRUE(NoTempFilesLeft(dir.Dir()));
	EXPECT_TRUE(FileSystem::DirectoryExists(path.c_str()));
}

// ---- ResponseCache ----------------------------------------------------------------------

TEST(CatalogResponseCache, StoreThenLoad)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.File("texture-catalog"));
	EXPECT_FALSE(cache.Load("textures-v2.json").has_value());

	const std::time_t before = std::time(nullptr);
	ASSERT_TRUE(cache.Store("textures-v2.json", R"({"schemaVersion":2})"));

	const std::optional<ResponseCache::Entry> entry = cache.Load("textures-v2.json");
	ASSERT_TRUE(entry.has_value());
	EXPECT_EQ(entry->body, R"({"schemaVersion":2})");
	EXPECT_GE(entry->fetched_at, before - 1);
	EXPECT_LE(entry->fetched_at, std::time(nullptr) + 1);

	// The file is the JSON itself, where the spec says it lives.
	EXPECT_EQ(FileSystem::ReadFileToString(cache.PathFor("textures-v2.json").c_str()).value_or(""), R"({"schemaVersion":2})");
	EXPECT_TRUE(NoTempFilesLeft(cache.Directory()));
}

TEST(CatalogResponseCache, FreshnessFollowsTheFetchTime)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.Dir());
	ASSERT_TRUE(cache.Store("c.json", "{}"));

	const std::time_t t0 = 1'700'000'000;
	SetFileTime(cache.PathFor("c.json"), t0);
	const std::chrono::seconds six_hours(6 * 3600);

	EXPECT_TRUE(cache.IsFresh("c.json", six_hours, t0));
	EXPECT_TRUE(cache.IsFresh("c.json", six_hours, t0 + 6 * 3600 - 1));
	EXPECT_FALSE(cache.IsFresh("c.json", six_hours, t0 + 6 * 3600));
	EXPECT_FALSE(cache.IsFresh("c.json", six_hours, t0 + 7 * 24 * 3600));

	// A stale entry is still there to serve as the offline fallback.
	const std::optional<ResponseCache::Entry> stale = cache.Load("c.json");
	ASSERT_TRUE(stale.has_value());
	EXPECT_EQ(stale->body, "{}");
	EXPECT_NEAR(static_cast<double>(stale->fetched_at), static_cast<double>(t0), 2.0);
}

TEST(CatalogResponseCache, AnEntryFromTheFutureIsNotFresh)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.Dir());
	ASSERT_TRUE(cache.Store("c.json", "{}"));
	const std::time_t t0 = 1'700'000'000;
	SetFileTime(cache.PathFor("c.json"), t0);
	EXPECT_FALSE(cache.IsFresh("c.json", std::chrono::hours(24), t0 - 100));
}

TEST(CatalogResponseCache, DefaultNowIsTheRealClock)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.Dir());
	ASSERT_TRUE(cache.Store("c.json", "{}"));
	EXPECT_TRUE(cache.IsFresh("c.json", std::chrono::minutes(5)));
	SetFileTime(cache.PathFor("c.json"), std::time(nullptr) - 3600);
	EXPECT_FALSE(cache.IsFresh("c.json", std::chrono::minutes(5)));
}

TEST(CatalogResponseCache, MissingEntries)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.File("not-created-yet"));
	EXPECT_FALSE(cache.Load("x").has_value());
	EXPECT_FALSE(cache.FetchedAt("x").has_value());
	EXPECT_FALSE(cache.IsFresh("x", std::chrono::hours(1)));
	cache.Remove("x"); // harmless
}

TEST(CatalogResponseCache, StoreReplacesAndRemoveDeletes)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.Dir());
	ASSERT_TRUE(cache.Store("c.json", "one"));
	ASSERT_TRUE(cache.Store("c.json", "two"));
	EXPECT_EQ(cache.Load("c.json")->body, "two");
	cache.Remove("c.json");
	EXPECT_FALSE(cache.Load("c.json").has_value());
}

TEST(CatalogResponseCache, NamesMustBePlainFileNames)
{
	ScopedTempDir dir;
	const ResponseCache cache(dir.File("cache"));
	for (const char* bad : {"", ".", "..", "../escape", "sub/dir", "sub\\dir", "/abs"})
	{
		Error error;
		EXPECT_FALSE(cache.Store(bad, "x", &error)) << bad;
		EXPECT_TRUE(error.IsValid()) << bad;
		EXPECT_FALSE(cache.Load(bad).has_value()) << bad;
		EXPECT_FALSE(cache.FetchedAt(bad).has_value()) << bad;
	}
	EXPECT_FALSE(FileSystem::FileExists(dir.File("escape").c_str()));
}

// ---- JSON helpers -----------------------------------------------------------------------

TEST(CatalogJson, ParsesAndReportsErrorsWithAnOffset)
{
	rapidjson::Document doc;
	std::string error;
	EXPECT_TRUE(ParseJson(R"({"a":[1,2,3],"b":"x"})", &doc, &error));
	EXPECT_EQ(doc["a"].Size(), 3u);
	EXPECT_STREQ(doc["b"].GetString(), "x");

	rapidjson::Document bad;
	EXPECT_FALSE(ParseJson(R"({"a":)", &bad, &error));
	EXPECT_NE(error.find("offset"), std::string::npos) << error;

	rapidjson::Document empty;
	EXPECT_FALSE(ParseJson("", &empty, &error));
}

TEST(CatalogJson, AcceptsABomAndTextWithoutATerminator)
{
	const std::string with_bom = "\xEF\xBB\xBF{\"k\":1}garbage-after";
	rapidjson::Document doc;
	// The view stops before the garbage, as a body slice would.
	EXPECT_TRUE(ParseJson(std::string_view(with_bom).substr(0, 3 + 7), &doc));
	EXPECT_EQ(doc["k"].GetInt(), 1);
}

TEST(CatalogJson, RoundTripsThroughAFile)
{
	ScopedTempDir dir;
	rapidjson::Document doc;
	doc.SetObject();
	doc.AddMember("name", rapidjson::Value("pack", doc.GetAllocator()), doc.GetAllocator());
	doc.AddMember("size", 1536, doc.GetAllocator());

	const std::string path = dir.File("state/texture-packs.json");
	Error error;
	ASSERT_TRUE(WriteJsonFileAtomic(path, doc, &error)) << error.GetDescription();

	rapidjson::Document back;
	ASSERT_TRUE(ParseJson(FileSystem::ReadFileToString(path.c_str()).value_or(""), &back));
	EXPECT_STREQ(back["name"].GetString(), "pack");
	EXPECT_EQ(back["size"].GetInt(), 1536);
	EXPECT_EQ(JsonToString(doc), R"({"name":"pack","size":1536})");
}
