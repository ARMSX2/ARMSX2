// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the automatic cover download: image sniffing, the on-disk miss cache with its
// back-off, the download job (fallback, remembered 404s, rate limits, cancellation) and the
// thread runner. Replies come from FakeFetcher; nothing touches the network. The image bytes are
// the first 512 bytes of real covers.

#include "cover_test_support.h"

#include "FakeFetcher.h"

#include "Catalog/CoverDownloadJob.h"
#include "Catalog/CoverSources.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Catalog;
using CatalogTest::FakeFetcher;
using CatalogTest::ScopedTempDir;

namespace
{
	using CoverTest::ARCADE_ROOT;
	using CoverTest::ExcerptIndex;
	using CoverTest::Ps2Jpg;
	using CoverTest::Ps2Png;
	using CoverTest::PSX_REPO;

	std::string Jpeg() { return CatalogTest::ReadFixture("covers/head.jpg"); }
	std::string Png() { return CatalogTest::ReadFixture("covers/head.png"); }
	std::string NotFoundBody() { return CatalogTest::ReadFixture("covers/not-found.txt"); }

	std::string ReadFile(const std::string& path)
	{
		return FileSystem::ReadFileToString(path.c_str()).value_or("<missing>");
	}

	bool Exists(const std::string& path) { return FileSystem::FileExists(path.c_str()); }

	constexpr std::time_t T0 = 1'800'000'000;

	CoverWorkItem Item(const std::string& serial, CoverPlatform platform = CoverPlatform::PS2)
	{
		CoverWorkItem item;
		item.id = "/games/" + serial + ".iso";
		item.serial = serial;
		item.title = "Game " + serial;
		item.platform = platform;
		return item;
	}

	// A job's world: a fake server, a covers folder, a miss cache on disk and a clock we move.
	struct Rig
	{
		ScopedTempDir dir;
		FakeFetcher http;
		std::time_t now = T0;
		std::shared_ptr<CoverMissCache> misses;

		Rig()
			: misses(std::make_shared<CoverMissCache>(dir.File("cache/covers/misses.json")))
		{
		}

		std::string CoversDir() const { return dir.File("covers"); }
		std::string CoverFile(const std::string& name) const { return Path::Combine(CoversDir(), name); }

		CoverJobOptions Options()
		{
			CoverJobOptions options;
			options.covers_dir = CoversDir();
			options.miss_cache = misses;
			options.clock = [this]() { return now; };
			// What the game list does: a cover exists if <serial>.<ext> is in the folder.
			options.has_cover = [this](const CoverWorkItem& item) {
				for (const char* ext : {".jpg", ".jpeg", ".png", ".webp"})
				{
					if (Exists(CoverFile(item.serial + ext)))
						return true;
				}
				return false;
			};
			return options;
		}

		CoverJobSummary Run(std::vector<CoverWorkItem> items, CoverJobOptions options)
		{
			return RunCoverDownloads(http, std::move(items), std::move(options));
		}

		CoverJobSummary Run(std::vector<CoverWorkItem> items) { return Run(std::move(items), Options()); }

		// A new process: the miss cache is re-read from disk.
		void Relaunch() { misses = std::make_shared<CoverMissCache>(dir.File("cache/covers/misses.json")); }
	};
} // namespace

TEST(CoverImageTest, ExtensionComesFromTheBytesNotTheUrl)
{
	const std::string jpeg = Jpeg();
	const std::string png = Png();
	EXPECT_EQ(ImageExtensionForBytes(jpeg.data(), jpeg.size()), ".jpg");
	EXPECT_EQ(ImageExtensionForBytes(png.data(), png.size()), ".png");

	const std::string webp = std::string("RIFF\x24\x00\x00\x00WEBPVP8 ", 16);
	EXPECT_EQ(ImageExtensionForBytes(webp.data(), webp.size()), ".webp");

	for (const std::string& not_an_image : {std::string("<html>404</html>"), NotFoundBody(), std::string("RIFF\x24\x00\x00\x00WAVEfmt ", 16), std::string("\xFF\xD8"), std::string()})
		EXPECT_EQ(ImageExtensionForBytes(not_an_image.data(), not_an_image.size()), "") << not_an_image;
}

TEST(CoverMissCacheTest, MissesSurviveARestartAndExpire)
{
	ScopedTempDir dir;
	const std::string path = dir.File("misses.json");
	{
		CoverMissCache cache(path, std::chrono::hours(24 * 5));
		cache.RecordMiss("https://x/a.jpg", T0);
		cache.RecordMiss("https://x/b.jpg", T0 + 3600);
		ASSERT_TRUE(cache.Save());
	}

	CoverMissCache cache(path, std::chrono::hours(24 * 5));
	ASSERT_TRUE(cache.Load(T0 + 7200));
	EXPECT_EQ(cache.MissCount(), 2u);
	EXPECT_TRUE(cache.IsMissed("https://x/a.jpg", T0 + 7200));
	EXPECT_FALSE(cache.IsMissed("https://x/other.jpg", T0 + 7200));

	// Five days on, the older one is due again.
	EXPECT_TRUE(cache.IsMissed("https://x/a.jpg", T0 + 5 * 86400 - 1));
	EXPECT_FALSE(cache.IsMissed("https://x/a.jpg", T0 + 5 * 86400));

	// Loading later drops the expired entry for good and the next Save writes it out.
	CoverMissCache later(path, std::chrono::hours(24 * 5));
	ASSERT_TRUE(later.Load(T0 + 5 * 86400 + 1800));
	EXPECT_EQ(later.MissCount(), 1u);
	ASSERT_TRUE(later.Save());
	CoverMissCache reread(path, std::chrono::hours(24 * 5));
	ASSERT_TRUE(reread.Load(T0 + 5 * 86400 + 1800));
	EXPECT_EQ(reread.MissCount(), 1u);
}

TEST(CoverMissCacheTest, AMissStampedInTheFutureCountsAsExpired)
{
	// A handheld without a battery-backed clock boots in 1970, then reads the real time. A
	// record from the "future" must not suppress downloads for years.
	CoverMissCache cache;
	cache.RecordMiss("https://x/a.jpg", T0);
	EXPECT_FALSE(cache.IsMissed("https://x/a.jpg", T0 - 86400));
}

TEST(CoverMissCacheTest, BackoffIsKeptOnDiskAndEnds)
{
	ScopedTempDir dir;
	const std::string path = dir.File("misses.json");
	{
		CoverMissCache cache(path);
		cache.StartBackoff(T0, std::chrono::minutes(60));
		ASSERT_TRUE(cache.Save());
	}
	CoverMissCache cache(path);
	ASSERT_TRUE(cache.Load(T0 + 60));
	EXPECT_TRUE(cache.InBackoff(T0 + 60));
	EXPECT_TRUE(cache.InBackoff(T0 + 3599));
	EXPECT_FALSE(cache.InBackoff(T0 + 3600));
	// A back-off absurdly far ahead is a clock that moved, not a real one.
	EXPECT_FALSE(cache.InBackoff(T0 - 10 * 86400));
}

TEST(CoverMissCacheTest, ADamagedFileIsTreatedAsEmptyAndANeverChangedCacheWritesNothing)
{
	ScopedTempDir dir;
	const std::string path = dir.File("misses.json");
	ASSERT_TRUE(FileSystem::WriteStringToFile(path.c_str(), "{ this is not json"));

	CoverMissCache cache(path);
	EXPECT_FALSE(cache.Load(T0));
	EXPECT_EQ(cache.MissCount(), 0u);
	EXPECT_TRUE(cache.Save()); // nothing changed, so the damaged file is not rewritten
	EXPECT_EQ(ReadFile(path), "{ this is not json");

	cache.RecordMiss("https://x/a.jpg", T0);
	EXPECT_TRUE(cache.Save());
	CoverMissCache fresh(path);
	EXPECT_TRUE(fresh.Load(T0));
	EXPECT_EQ(fresh.MissCount(), 1u);

	CoverMissCache nowhere(dir.File("none/misses.json"));
	EXPECT_FALSE(nowhere.Load(T0));
	EXPECT_TRUE(nowhere.Save());
	EXPECT_FALSE(Exists(dir.File("none/misses.json")));
}

TEST(CoverMissCacheTest, AMemoryOnlyCacheKeepsWhatItHolds)
{
	CoverMissCache cache;
	cache.RecordMiss("https://x/a.jpg", T0);
	EXPECT_FALSE(cache.Load(T0));
	EXPECT_TRUE(cache.IsMissed("https://x/a.jpg", T0));
	EXPECT_TRUE(cache.Save());
}

TEST(CoverJob, SavesEachCoverUnderItsSerialWithTheExtensionOfItsBytes)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-21007"), Jpeg(), "image/jpeg")
		.Ok(Ps2Jpg("SLUS-20093"), Png(), "image/jpeg"); // lying content type, PNG bytes

	std::vector<CoverSaved> saved;
	std::vector<CoverProgress> progress;
	CoverJobOptions options = rig.Options();
	options.on_cover_saved = [&](const CoverSaved& s) { saved.push_back(s); };
	options.on_progress = [&](const CoverProgress& p) { progress.push_back(p); };

	const CoverJobSummary summary = rig.Run({Item("SLUS-21007"), Item("SLUS-20093")}, options);

	EXPECT_EQ(summary.end, CoverJobEnd::Completed);
	EXPECT_EQ(summary.total, 2u);
	EXPECT_EQ(summary.downloaded, 2u);
	EXPECT_EQ(summary.requests, 2u);
	EXPECT_EQ(ReadFile(rig.CoverFile("SLUS-21007.jpg")), Jpeg());
	EXPECT_EQ(ReadFile(rig.CoverFile("SLUS-20093.png")), Png());
	EXPECT_FALSE(Exists(rig.CoverFile("SLUS-20093.jpg")));

	ASSERT_EQ(saved.size(), 2u);
	EXPECT_EQ(saved[0].id, "/games/SLUS-21007.iso");
	EXPECT_EQ(saved[0].serial, "SLUS-21007");
	EXPECT_EQ(saved[0].path, rig.CoverFile("SLUS-21007.jpg"));
	EXPECT_EQ(saved[0].url, Ps2Jpg("SLUS-21007"));
	EXPECT_EQ(saved[1].path, rig.CoverFile("SLUS-20093.png"));

	ASSERT_FALSE(progress.empty());
	EXPECT_EQ(progress.front().done, 0u);
	EXPECT_EQ(progress.front().current_title, "Game SLUS-21007");
	EXPECT_EQ(progress.back().done, 2u);
	EXPECT_EQ(progress.back().total, 2u);
	EXPECT_EQ(progress.back().current_title, "");

	// Downloads run one at a time, in the order given.
	const auto requests = rig.http.Requests();
	ASSERT_EQ(requests.size(), 2u);
	EXPECT_EQ(requests[0].url, Ps2Jpg("SLUS-21007"));
	EXPECT_EQ(requests[1].url, Ps2Jpg("SLUS-20093"));
}

TEST(CoverJob, ThreeDStyleAsksForThePngCases)
{
	Rig rig;
	rig.http.Ok(Ps2Png("SLUS-21007"), Png(), "image/png");
	CoverJobOptions options = rig.Options();
	options.preferences.style = CoverStyle::Case3D;
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007")}, options);
	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(ReadFile(rig.CoverFile("SLUS-21007.png")), Png());
}

TEST(CoverJob, Ps1AndArcadeGamesGoToTheirOwnSources)
{
	Rig rig;
	rig.http.Ok(std::string(PSX_REPO) + "/default/SCUS-94900.jpg", Jpeg(), "image/jpeg")
		.Ok(std::string(ARCADE_ROOT) + "/NM00007.jpg", Jpeg(), "image/jpeg");

	const CoverJobSummary summary = rig.Run({Item("SCUS-94900", CoverPlatform::PS1), Item("NM00007", CoverPlatform::Arcade)});
	EXPECT_EQ(summary.downloaded, 2u);
	EXPECT_TRUE(Exists(rig.CoverFile("SCUS-94900.jpg")));
	EXPECT_TRUE(Exists(rig.CoverFile("NM00007.jpg")));
}

TEST(CoverJob, ARegionCoverIsSavedUnderTheDiscsSerialSoTheGameListFindsIt)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SCAJ-20078"), Jpeg(), "image/jpeg");

	CoverJobOptions options = rig.Options();
	options.preferences.region = CoverRegion::JapanAsia;
	options.load_region_index = ExcerptIndex;
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007")}, options);

	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_TRUE(Exists(rig.CoverFile("SLUS-21007.jpg")));
	EXPECT_FALSE(Exists(rig.CoverFile("SCAJ-20078.jpg")));
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-21007")), 0u); // the disc's cover was never needed
}

TEST(CoverJob, AMissingRegionCoverFallsBackToTheDiscSerialOnce)
{
	Rig rig;
	rig.http.Status(Ps2Jpg("SCAJ-20078"), 404, NotFoundBody()).Ok(Ps2Jpg("SLUS-21007"), Jpeg(), "image/jpeg");

	CoverJobOptions options = rig.Options();
	options.preferences.region = CoverRegion::JapanAsia;
	options.load_region_index = ExcerptIndex;
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007")}, options);

	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(summary.missed, 0u);
	EXPECT_EQ(ReadFile(rig.CoverFile("SLUS-21007.jpg")), Jpeg());
	const auto requests = rig.http.Requests();
	ASSERT_EQ(requests.size(), 2u);
	EXPECT_EQ(requests[0].url, Ps2Jpg("SCAJ-20078"));
	EXPECT_EQ(requests[1].url, Ps2Jpg("SLUS-21007"));

	// The regional URL is remembered as missing; the next run goes straight to the disc cover.
	std::filesystem::remove(rig.CoverFile("SLUS-21007.jpg"));
	rig.Relaunch();
	const CoverJobSummary again = rig.Run({Item("SLUS-21007")}, [&] {
		CoverJobOptions o = rig.Options();
		o.preferences.region = CoverRegion::JapanAsia;
		o.load_region_index = ExcerptIndex;
		return o;
	}());
	EXPECT_EQ(again.downloaded, 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SCAJ-20078")), 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-21007")), 2u);
}

TEST(CoverJob, ThePinnedRegionOfOneGameDoesNotMoveTheOthers)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SCAJ-20078"), Jpeg(), "image/jpeg").Ok(Ps2Jpg("SLUS-20093"), Jpeg(), "image/jpeg");

	CoverJobOptions options = rig.Options();
	options.preferences.SetPin("SLUS-21007", CoverRegion::JapanAsia);
	options.load_region_index = ExcerptIndex;
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007"), Item("SLUS-20093")}, options);
	EXPECT_EQ(summary.downloaded, 2u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SCAJ-20078")), 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-20093")), 1u);
}

TEST(CoverJob, TheRegionIndexIsOnlyLoadedWhenAskedFor)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-21007"), Jpeg(), "image/jpeg");
	int loads = 0;
	CoverJobOptions options = rig.Options();
	options.load_region_index = [&]() {
		loads++;
		return ExcerptIndex();
	};
	rig.Run({Item("SLUS-21007")}, options);
	EXPECT_EQ(loads, 0);

	rig.http.Ok(Ps2Jpg("SLES-53411"), Jpeg(), "image/jpeg");
	options = rig.Options();
	options.preferences.region = CoverRegion::Europe;
	options.load_region_index = [&]() {
		loads++;
		return ExcerptIndex();
	};
	std::filesystem::remove(rig.CoverFile("SLUS-21007.jpg"));
	rig.Run({Item("SLUS-21007")}, options);
	EXPECT_EQ(loads, 1);
}

TEST(CoverJob, A404IsRememberedSoTheNextLaunchDoesNotAskAgain)
{
	Rig rig;
	rig.http.Status(Ps2Jpg("SLUS-99999"), 404, NotFoundBody());

	const CoverJobSummary first = rig.Run({Item("SLUS-99999")});
	EXPECT_EQ(first.missed, 1u);
	EXPECT_EQ(first.downloaded, 0u);
	EXPECT_EQ(first.requests, 1u);
	EXPECT_FALSE(Exists(rig.CoverFile("SLUS-99999.jpg")));
	EXPECT_TRUE(Exists(rig.dir.File("cache/covers/misses.json")));

	rig.Relaunch(); // a new process reads the miss file from disk
	rig.now += 3 * 86400;
	const CoverJobSummary second = rig.Run({Item("SLUS-99999")});
	EXPECT_EQ(second.skipped_known_miss, 1u);
	EXPECT_EQ(second.requests, 0u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-99999")), 1u);

	// After the expiry the cover may have been added to the repository: ask again.
	rig.Relaunch();
	rig.now += 3 * 86400;
	const CoverJobSummary third = rig.Run({Item("SLUS-99999")});
	EXPECT_EQ(third.missed, 1u);
	EXPECT_EQ(third.requests, 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-99999")), 2u);
}

TEST(CoverJob, ALibraryOfFiveHundredGamesWithoutCoversCostsTheNetworkOnlyOnce)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 500; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 30000 + i);
		rig.http.Status(Ps2Jpg(serial), 404, NotFoundBody());
		items.push_back(Item(serial));
	}

	const CoverJobSummary first = rig.Run(items);
	EXPECT_EQ(first.missed, 500u);
	EXPECT_EQ(first.requests, 500u);

	rig.Relaunch();
	const CoverJobSummary second = rig.Run(items);
	EXPECT_EQ(second.skipped_known_miss, 500u);
	EXPECT_EQ(second.requests, 0u);
	EXPECT_EQ(second.end, CoverJobEnd::Completed);
}

TEST(CoverJob, AnErrorPageWithStatus200IsAMissNotACover)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-21007"), "<html>Not Found</html>", "text/html");
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007")});
	EXPECT_EQ(summary.missed, 1u);
	EXPECT_EQ(summary.downloaded, 0u);
	EXPECT_FALSE(Exists(rig.CoverFile("SLUS-21007.jpg")));
	EXPECT_TRUE(rig.misses->IsMissed(Ps2Jpg("SLUS-21007"), rig.now));
}

TEST(CoverJob, ABodyOverTheCapIsNotACover)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-21007"), Jpeg() + std::string(4000, 'x'), "image/jpeg");
	CoverJobOptions options = rig.Options();
	options.max_cover_bytes = 1000;
	const CoverJobSummary summary = rig.Run({Item("SLUS-21007")}, options);
	EXPECT_EQ(summary.missed, 1u);
	EXPECT_FALSE(Exists(rig.CoverFile("SLUS-21007.jpg")));
}

class CoverJobRateLimit : public ::testing::TestWithParam<int>
{
};

TEST_P(CoverJobRateLimit, StopsTheRunAndBacksOffAcrossLaunches)
{
	const int status = GetParam();
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg")
		.Status(Ps2Jpg("SLUS-10002"), status, "rate limited")
		.Ok(Ps2Jpg("SLUS-10002"), Png(), "image/png") // what it says after the back-off
		.Ok(Ps2Jpg("SLUS-10003"), Jpeg(), "image/jpeg");

	const std::vector<CoverWorkItem> items = {Item("SLUS-10001"), Item("SLUS-10002"), Item("SLUS-10003")};

	const CoverJobSummary first = rig.Run(items);
	EXPECT_EQ(first.end, CoverJobEnd::RateLimited);
	EXPECT_EQ(first.downloaded, 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-10003")), 0u); // never reached
	EXPECT_FALSE(rig.misses->IsMissed(Ps2Jpg("SLUS-10002"), rig.now)); // a rate limit is not a miss
	EXPECT_TRUE(rig.misses->InBackoff(rig.now));

	// Ten minutes later, in a new process: still backing off, no request is made.
	rig.Relaunch();
	rig.now += 600;
	const size_t before = rig.http.Requests().size();
	const CoverJobSummary during = rig.Run(items);
	EXPECT_EQ(during.end, CoverJobEnd::BackedOff);
	EXPECT_EQ(rig.http.Requests().size(), before);

	// After the hour the run resumes where it left off.
	rig.Relaunch();
	rig.now += 3600;
	const CoverJobSummary after = rig.Run(items);
	EXPECT_EQ(after.end, CoverJobEnd::Completed);
	EXPECT_EQ(after.already_had_cover, 1u);
	EXPECT_EQ(after.downloaded, 2u);
	EXPECT_TRUE(Exists(rig.CoverFile("SLUS-10002.png")));
	EXPECT_TRUE(Exists(rig.CoverFile("SLUS-10003.jpg")));
}

INSTANTIATE_TEST_SUITE_P(Statuses, CoverJobRateLimit, ::testing::Values(403, 429));

TEST(CoverJob, AGameThatAlreadyHasACoverIsNotRequested)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10002"), Jpeg(), "image/jpeg");
	CoverJobOptions options = rig.Options();
	options.has_cover = [](const CoverWorkItem& item) { return item.serial == "SLUS-10001"; };
	const CoverJobSummary summary = rig.Run({Item("SLUS-10001"), Item("SLUS-10002")}, options);
	EXPECT_EQ(summary.already_had_cover, 1u);
	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-10001")), 0u);
}

TEST(CoverJob, ACoverThatAppearsDuringTheDownloadIsNotOverwritten)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	int asks = 0;
	CoverJobOptions options = rig.Options();
	// No cover before the request, the user's own cover by the time the bytes are in.
	options.has_cover = [&](const CoverWorkItem&) { return ++asks > 1; };
	bool announced = false;
	options.on_cover_saved = [&](const CoverSaved&) { announced = true; };

	const CoverJobSummary summary = rig.Run({Item("SLUS-10001")}, options);
	EXPECT_EQ(rig.http.RequestCount(Ps2Jpg("SLUS-10001")), 1u);
	EXPECT_EQ(summary.already_had_cover, 1u);
	EXPECT_EQ(summary.downloaded, 0u);
	EXPECT_FALSE(Exists(rig.CoverFile("SLUS-10001.jpg")));
	EXPECT_FALSE(announced);
}

TEST(CoverJob, GamesSharingASerialAreFetchedOnceAndUnusableSerialsAreSkipped)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	CoverWorkItem second_copy = Item("SLUS-10001");
	second_copy.id = "/games/other-dump.iso";
	const CoverJobSummary summary = rig.Run({Item("SLUS-10001"), second_copy, Item("../escape"), Item("")});
	EXPECT_EQ(summary.total, 1u);
	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(rig.http.Requests().size(), 1u);
}

TEST(CoverJob, CancellingBetweenGamesStopsTheRunCleanly)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 10; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 40000 + i);
		rig.http.Ok(Ps2Jpg(serial), Jpeg(), "image/jpeg");
		items.push_back(Item(serial));
	}

	CoverJobOptions options = rig.Options();
	int saved = 0;
	options.on_cover_saved = [&](const CoverSaved&) {
		if (++saved == 3)
			options.cancel.Cancel();
	};
	const CoverJobSummary summary = rig.Run(items, options);
	EXPECT_EQ(summary.end, CoverJobEnd::Cancelled);
	EXPECT_EQ(summary.downloaded, 3u);
	EXPECT_EQ(rig.http.Requests().size(), 3u);

	// Nothing half-written is left in the covers folder.
	size_t files = 0;
	for (const auto& entry : std::filesystem::directory_iterator(rig.CoversDir()))
	{
		EXPECT_NE(entry.path().extension(), ".tmp");
		files++;
	}
	EXPECT_EQ(files, 3u);
}

TEST(CoverJob, ACancelAlreadyRaisedMeansNoRequests)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	CoverJobOptions options = rig.Options();
	options.cancel.Cancel();
	const CoverJobSummary summary = rig.Run({Item("SLUS-10001")}, options);
	EXPECT_EQ(summary.end, CoverJobEnd::Cancelled);
	EXPECT_EQ(rig.http.Requests().size(), 0u);
}

TEST(CoverJob, SeveralFailuresInARowEndTheRunWithoutRecordingMisses)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 6; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 50000 + i);
		rig.http.Fail(Ps2Jpg(serial), "could not resolve host");
		items.push_back(Item(serial));
	}

	const CoverJobSummary summary = rig.Run(items);
	EXPECT_EQ(summary.end, CoverJobEnd::NetworkDown);
	EXPECT_EQ(summary.failed, 3u);
	EXPECT_EQ(rig.http.Requests().size(), 3u);
	EXPECT_EQ(rig.misses->MissCount(), 0u);
	EXPECT_FALSE(rig.misses->InBackoff(rig.now));
}

TEST(CoverJob, ASuccessBetweenFailuresStartsTheCountAgain)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	const std::vector<std::string> serials = {"SLUS-60001", "SLUS-60002", "SLUS-60003", "SLUS-60004", "SLUS-60005"};
	for (const std::string& serial : serials)
	{
		if (serial == "SLUS-60003")
			rig.http.Ok(Ps2Jpg(serial), Jpeg(), "image/jpeg");
		else
			rig.http.Status(Ps2Jpg(serial), 503);
		items.push_back(Item(serial));
	}
	const CoverJobSummary summary = rig.Run(items);
	EXPECT_EQ(summary.end, CoverJobEnd::Completed);
	EXPECT_EQ(summary.failed, 4u);
	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(rig.misses->MissCount(), 0u); // 503 is not "not found"
}

TEST(CoverJob, ANotFoundCountsAsTheNetworkBeingUp)
{
	Rig rig;
	rig.http.Fail(Ps2Jpg("SLUS-70001"), "timeout")
		.Fail(Ps2Jpg("SLUS-70002"), "timeout")
		.Status(Ps2Jpg("SLUS-70003"), 404)
		.Fail(Ps2Jpg("SLUS-70004"), "timeout")
		.Fail(Ps2Jpg("SLUS-70005"), "timeout");
	const CoverJobSummary summary = rig.Run({Item("SLUS-70001"), Item("SLUS-70002"), Item("SLUS-70003"), Item("SLUS-70004"), Item("SLUS-70005")});
	EXPECT_EQ(summary.end, CoverJobEnd::Completed);
	EXPECT_EQ(summary.missed, 1u);
	EXPECT_EQ(summary.failed, 4u);
}

TEST(CoverJob, AFolderThatCannotBeWrittenIsAFailureNotACrash)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	// A regular file where the folder should be.
	ASSERT_TRUE(FileSystem::WriteStringToFile(rig.dir.File("covers").c_str(), "x"));
	const CoverJobSummary summary = rig.Run({Item("SLUS-10001")});
	EXPECT_EQ(summary.downloaded, 0u);
	EXPECT_EQ(summary.failed, 1u);
}

TEST(CoverJob, ForcedRedownloadReplacesTheOldCoverAndIgnoresTheMissCache)
{
	Rig rig;
	std::filesystem::create_directories(rig.CoversDir());
	const std::string old_cover = rig.CoverFile("Kuon.png");
	ASSERT_TRUE(FileSystem::WriteStringToFile(old_cover.c_str(), "old"));
	rig.misses->RecordMiss(Ps2Jpg("SCAJ-20078"), rig.now);
	ASSERT_TRUE(rig.misses->Save()); // a run starts by reading the file, so the miss has to be on disk
	rig.http.Ok(Ps2Jpg("SCAJ-20078"), Jpeg(), "image/jpeg");

	CoverWorkItem item = Item("SLUS-21007");
	item.force = true;
	item.replace_path = old_cover;

	CoverJobOptions options = rig.Options();
	options.preferences.region = CoverRegion::JapanAsia;
	options.load_region_index = ExcerptIndex;
	options.has_cover = [](const CoverWorkItem&) { return true; }; // would skip a normal run
	const CoverJobSummary summary = rig.Run({item}, options);

	EXPECT_EQ(summary.downloaded, 1u);
	EXPECT_EQ(ReadFile(rig.CoverFile("SLUS-21007.jpg")), Jpeg());
	EXPECT_FALSE(Exists(old_cover));
	EXPECT_FALSE(rig.misses->IsMissed(Ps2Jpg("SCAJ-20078"), rig.now)); // a hit clears the old miss
}

TEST(CoverJob, ForcedRedownloadKeepsTheOldCoverWhenTheServerHasNothing)
{
	Rig rig;
	std::filesystem::create_directories(rig.CoversDir());
	const std::string old_cover = rig.CoverFile("SLUS-21007.jpg");
	ASSERT_TRUE(FileSystem::WriteStringToFile(old_cover.c_str(), "old"));
	rig.http.Status(Ps2Png("SLUS-21007"), 404);

	CoverWorkItem item = Item("SLUS-21007");
	item.force = true;
	item.replace_path = old_cover;
	CoverJobOptions options = rig.Options();
	options.preferences.style = CoverStyle::Case3D;
	const CoverJobSummary summary = rig.Run({item}, options);
	EXPECT_EQ(summary.missed, 1u);
	EXPECT_EQ(ReadFile(old_cover), "old");
}

TEST(CoverRunner, RunsOnAWorkerThreadAndReportsWhenFinished)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	CoverJobRunner runner;

	std::promise<CoverJobSummary> finished;
	std::thread::id worker;
	CoverJobOptions options = rig.Options();
	options.on_cover_saved = [&](const CoverSaved&) { worker = std::this_thread::get_id(); };

	ASSERT_TRUE(runner.Start(rig.http, {Item("SLUS-10001")}, options, [&](const CoverJobSummary& s) { finished.set_value(s); }));
	auto future = finished.get_future();
	ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
	EXPECT_EQ(future.get().downloaded, 1u);
	EXPECT_NE(worker, std::this_thread::get_id());

	// Idle again once the callback returned; a second run can start.
	for (int i = 0; i < 1000 && runner.IsRunning(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	EXPECT_FALSE(runner.IsRunning());
	EXPECT_TRUE(runner.Start(rig.http, {Item("SLUS-10001")}, rig.Options()));
	runner.Shutdown();
}

TEST(CoverRunner, RefusesASecondRunWhileOneIsGoing)
{
	Rig rig;
	rig.http.Ok(Ps2Jpg("SLUS-10001"), Jpeg(), "image/jpeg");
	CoverJobRunner runner;

	std::mutex mutex;
	std::condition_variable cv;
	bool in_job = false;
	bool release = false;

	CoverJobOptions options = rig.Options();
	options.on_progress = [&](const CoverProgress& p) {
		if (p.done != 0)
			return;
		std::unique_lock lock(mutex);
		in_job = true;
		cv.notify_all();
		cv.wait(lock, [&] { return release; });
	};
	ASSERT_TRUE(runner.Start(rig.http, {Item("SLUS-10001")}, options));
	{
		std::unique_lock lock(mutex);
		ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return in_job; }));
	}
	EXPECT_TRUE(runner.IsRunning());
	EXPECT_FALSE(runner.Start(rig.http, {Item("SLUS-10002")}, rig.Options()));
	{
		std::lock_guard lock(mutex);
		release = true;
	}
	cv.notify_all();
	runner.Shutdown();
	EXPECT_FALSE(runner.IsRunning());
}

TEST(CoverRunner, ShutdownCancelsARunInProgressAndWaitsForIt)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 200; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 80000 + i);
		rig.http.Ok(Ps2Jpg(serial), Jpeg(), "image/jpeg");
		items.push_back(Item(serial));
	}

	CoverJobRunner runner;
	std::atomic<int> saved{0};
	std::promise<CoverJobSummary> finished;
	CoverJobOptions options = rig.Options();
	options.on_cover_saved = [&](const CoverSaved&) {
		saved++;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	};
	ASSERT_TRUE(runner.Start(rig.http, items, options, [&](const CoverJobSummary& s) { finished.set_value(s); }));
	while (saved.load() < 3)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	runner.Shutdown(); // returns only when the thread is gone
	EXPECT_FALSE(runner.IsRunning());

	auto future = finished.get_future();
	ASSERT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
	const CoverJobSummary summary = future.get();
	EXPECT_EQ(summary.end, CoverJobEnd::Cancelled);
	EXPECT_LT(summary.downloaded, 200u);
	runner.Shutdown(); // twice is fine
}

TEST(CoverRunner, TheDestructorCancelsAndJoins)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 200; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 90000 + i);
		rig.http.Ok(Ps2Jpg(serial), Jpeg(), "image/jpeg");
		items.push_back(Item(serial));
	}
	std::atomic<int> saved{0};
	{
		CoverJobRunner runner;
		CoverJobOptions options = rig.Options();
		options.on_cover_saved = [&](const CoverSaved&) {
			saved++;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		};
		ASSERT_TRUE(runner.Start(rig.http, items, options));
		while (saved.load() < 2)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} // must not hang, crash, or leave the thread running over the Rig
	EXPECT_LT(saved.load(), 200);
}

TEST(CoverRunner, CancelFromACallbackDoesNotDeadlockShutdown)
{
	Rig rig;
	std::vector<CoverWorkItem> items;
	for (int i = 0; i < 50; i++)
	{
		const std::string serial = fmt::format("SLUS-{:05d}", 11000 + i);
		rig.http.Ok(Ps2Jpg(serial), Jpeg(), "image/jpeg");
		items.push_back(Item(serial));
	}
	CoverJobRunner runner;
	CoverJobOptions options = rig.Options();
	options.on_cover_saved = [&](const CoverSaved&) {
		runner.Cancel();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	};
	ASSERT_TRUE(runner.Start(rig.http, items, options));
	runner.Shutdown();
	EXPECT_FALSE(runner.IsRunning());
}
