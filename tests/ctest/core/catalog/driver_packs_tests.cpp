// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "catalog_test_support.h"
#include "tar_test_support.h"

#include "FakeFetcher.h"

#include "Catalog/DriverPacks.h"
#include "Catalog/Json.h"

#include "Host.h"

#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/SHA256.h"

#include "fmt/format.h"
#include "rapidjson/document.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#define DRIVER_TESTS_LINUX 1
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace DriverPacks;
using CatalogTest::BuildTarGz;
using CatalogTest::FakeFetcher;
using CatalogTest::FakeReply;
using CatalogTest::ScopedTempDir;
using CatalogTest::TarGzEntry;

namespace fs = std::filesystem;

namespace
{
	const std::string RELEASES_URL = "https://api.github.com/repos/bmdhacks/armsx2-turnip/releases";
	const std::string TARBALL_URL = "https://github.com/bmdhacks/armsx2-turnip/releases/download/axfl2-002/turnip-axfl2-002-aarch64.tar.gz";
	const std::string SUM_URL = TARBALL_URL + ".sha256";
	const std::string PLATFORM = "linux-aarch64";

	// ---- fake machines ---------------------------------------------------------------------------

	// A sysfs with one DRM device bound to `driver`.
	void MakeFakeSysfs(const std::string& root, const std::string& driver)
	{
		const fs::path device = fs::path(root) / "class" / "drm" / "card0" / "device";
		fs::create_directories(device);
		fs::create_symlink("../../../../bus/platform/drivers/" + driver, device / "driver");
		// A connector entry looks like a card to a sloppy match; it must not count.
		fs::create_directories(fs::path(root) / "class" / "drm" / "card0-HDMI-A-1" / "device");
		fs::create_symlink("../../../../bus/platform/drivers/msm", fs::path(root) / "class" / "drm" / "card0-HDMI-A-1" / "device" / "driver");
	}

	// ---- fake releases ---------------------------------------------------------------------------

	std::string ReleasesJson(const std::string& tag, const std::string& asset, u64 size, const std::string& digest_hex, bool with_sum_asset)
	{
		std::string assets = fmt::format(R"({{"name":"{0}","size":{1},"digest":"{2}","browser_download_url":"https://github.com/bmdhacks/armsx2-turnip/releases/download/{3}/{0}"}})",
			asset, size, digest_hex.empty() ? "" : "sha256:" + digest_hex, tag);
		if (with_sum_asset)
			assets += fmt::format(R"(,{{"name":"{0}.sha256","size":98,"browser_download_url":"https://github.com/bmdhacks/armsx2-turnip/releases/download/{1}/{0}.sha256"}})", asset, tag);
		return fmt::format(R"([{{"tag_name":"{0}","name":"Release {0}","draft":false,"prerelease":false,"published_at":"2026-10-04T23:54:03Z","assets":[{1}]}}])", tag, assets);
	}

	// ---- a pack archive ----------------------------------------------------------------------------

	std::string IcdManifestJson()
	{
		return R"({"ICD":{"api_version":"1.4.363","library_arch":"64","library_path":"/storage/turnip/axfl2-002/libvulkan_freedreno.so"},"file_format_version":"1.0.1"})";
	}

	std::string PackManifestJson()
	{
		return R"json({"build_tag":"axfl2-002","driver_info_token":"(git-axfl2-002)","mesa_version":"26.3.0-devel","platforms":"wayland"})json";
	}

	TarGzEntry Entry(std::string name, std::string data, unsigned mode = 0644)
	{
		TarGzEntry e;
		e.name = std::move(name);
		e.data = std::move(data);
		e.mode = mode;
		return e;
	}

	// The layout of the real axfl2-002 tarball, with `library` as libvulkan_freedreno.so.
	std::string MakePackTarGz(const std::string& library, const std::string& prefix = "turnip/axfl2-002/")
	{
		return BuildTarGz({
			Entry(prefix + "freedreno_icd.aarch64.json", IcdManifestJson()),
			Entry(prefix + "MANIFEST.json", PackManifestJson()),
			Entry(prefix + "libvulkan_freedreno.so", library, 0755),
			Entry(prefix + "lib-fallback/libz.so.1", "fallback", 0755),
		});
	}

	std::string Sha(const std::string& bytes)
	{
		return SHA256Digest::HashHex(bytes);
	}

#ifdef CATALOG_TEST_LIB_DIR
	std::string TestLib(const char* name)
	{
		return std::string(CATALOG_TEST_LIB_DIR) + "/libcatalogtest" + name + ".so";
	}

	std::string TestLibBytes(const char* name)
	{
		return FileSystem::ReadFileToString(TestLib(name).c_str()).value_or(std::string());
	}
#endif

	// ---- a whole rig -------------------------------------------------------------------------------

	struct Rig
	{
		ScopedTempDir dir;
		FakeFetcher http;
		ManagerConfig config;
		std::unique_ptr<Manager> manager;
		std::vector<std::string> checked_libraries;
		int serves = 0;

		explicit Rig(bool adreno = true, bool fake_library_check = true)
		{
			config.drivers_dir = dir.File("drivers");
			config.cache_dir = dir.File("cache");
			config.platform = PLATFORM;
			config.sysfs_root = dir.File("sys");
			MakeFakeSysfs(config.sysfs_root, adreno ? "msm" : "asahi");
			if (fake_library_check)
			{
				config.library_check = [this](const std::string& path, std::string*) {
					checked_libraries.push_back(path);
					return true;
				};
			}
			Rebuild();
		}

		void Rebuild() { manager = std::make_unique<Manager>(http, config); }

		RemoteDriver Remote(const std::string& archive_bytes, bool with_sum_asset = true, bool with_digest = true) const
		{
			RemoteDriver driver;
			driver.id = "armsx2turnip-axfl2-002";
			driver.feed_label = "ARMSX2 Turnip";
			driver.tag = "axfl2-002";
			driver.release_name = "Custom Turnip driver for ARMSX2 axfl2-002";
			driver.asset_name = "turnip-axfl2-002-aarch64.tar.gz";
			driver.download_url = TARBALL_URL;
			driver.size = archive_bytes.size();
			if (with_sum_asset)
				driver.checksum_url = SUM_URL;
			if (with_digest)
				driver.api_digest = Sha(archive_bytes);
			return driver;
		}

		// Scripts the network for one good install and returns the driver describing it. Every
		// call gets its own URLs: a FakeFetcher serves one URL's replies in the order they were
		// scripted, so two installs of different archives need two URLs.
		RemoteDriver Serve(const std::string& archive_bytes)
		{
			RemoteDriver driver = Remote(archive_bytes);
			const std::string suffix = "?serve=" + std::to_string(++serves);
			driver.download_url += suffix;
			driver.checksum_url += suffix;
			http.Ok(driver.download_url, archive_bytes, "application/octet-stream");
			http.Ok(driver.checksum_url, Sha(archive_bytes) + "  turnip-axfl2-002-aarch64.tar.gz\n", "text/plain");
			return driver;
		}

		InstallResult Install(const RemoteDriver& driver, InstallProgressCallback progress = {}, const Catalog::CancelToken& cancel = {})
		{
			return manager->Install(driver, progress, cancel);
		}

		std::string InstalledPath(const std::string& id, const std::string& file = {}) const
		{
			return file.empty() ? Path::Combine(config.drivers_dir, id) : Path::Combine(Path::Combine(config.drivers_dir, id), file);
		}

		bool StagingLeftOver() const
		{
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(config.drivers_dir, ec))
			{
				if (entry.path().filename().string().rfind(".install-", 0) == 0)
					return true;
			}
			return false;
		}
	};

	rapidjson::Document ReadJson(const std::string& path)
	{
		rapidjson::Document doc;
		const std::string text = FileSystem::ReadFileToString(path.c_str()).value_or("null");
		Catalog::ParseJson(text, &doc);
		return doc;
	}

	std::string Str(const rapidjson::Value& object, const char* key)
	{
		if (!object.IsObject() || !object.HasMember(key) || !object[key].IsString())
			return "<missing>";
		return object[key].GetString();
	}

	// Saves and restores the environment variables the Vulkan loader reads.
	struct EnvGuard
	{
		static constexpr const char* NAMES[3] = {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"};
		std::string saved[3];
		bool was_set[3];

		EnvGuard()
		{
			for (int i = 0; i < 3; i++)
			{
				const char* value = std::getenv(NAMES[i]);
				was_set[i] = value != nullptr;
				if (value)
					saved[i] = value;
				unsetenv(NAMES[i]);
			}
		}

		~EnvGuard()
		{
			for (int i = 0; i < 3; i++)
			{
				if (was_set[i])
					setenv(NAMES[i], saved[i].c_str(), 1);
				else
					unsetenv(NAMES[i]);
			}
		}
	};

	std::string Env(const char* name)
	{
		const char* value = std::getenv(name);
		return value ? value : "<unset>";
	}

	SettingsInterface& BaseLayer()
	{
		if (!Host::Internal::GetBaseSettingsLayer())
		{
			static MemorySettingsInterface memory;
			Host::Internal::SetBaseSettingsLayer(&memory);
		}
		return *Host::Internal::GetBaseSettingsLayer();
	}
} // namespace

// =================================================================================================
// Parsing a feed
// =================================================================================================

TEST(DriverPacksFeed, ParsesTheRealGithubResponse)
{
	std::vector<RemoteDriver> drivers;
	std::string error;
	ASSERT_TRUE(ParseReleases(DefaultFeeds()[0], CatalogTest::ReadFixture("drivers/releases.json"), &drivers, &error)) << error;

	// Two releases; the Android zip and its checksum are not Linux packs.
	ASSERT_EQ(drivers.size(), 2u);

	const RemoteDriver& newest = drivers[0];
	EXPECT_EQ(newest.id, "armsx2turnip-axfl2-002");
	EXPECT_EQ(newest.tag, "axfl2-002");
	EXPECT_EQ(newest.feed_label, "ARMSX2 Turnip");
	EXPECT_EQ(newest.release_name, "Custom Turnip driver for ARMSX2 (Adreno 6xx/7xx) axfl2-002");
	EXPECT_EQ(newest.asset_name, "turnip-axfl2-002-aarch64.tar.gz");
	EXPECT_EQ(newest.download_url, TARBALL_URL);
	EXPECT_EQ(newest.checksum_url, SUM_URL);
	EXPECT_EQ(newest.size, 4107024u);
	EXPECT_EQ(newest.api_digest, "687e72824c653efc5dbd5b650ae6223ddb0aa0c52d687f6f7fade9c7d95dc5bf");
	EXPECT_EQ(newest.published_at, "2026-10-04T23:54:03Z");
	EXPECT_FALSE(newest.prerelease);

	EXPECT_EQ(drivers[1].id, "armsx2turnip-axfl1-011");
	EXPECT_EQ(drivers[1].size, 4106858u);
}

TEST(DriverPacksFeed, SkipsDraftsMismatchesAndInsecureUrls)
{
	const std::string json = R"([
	  {"tag_name":"draft-1","draft":true,"assets":[{"name":"turnip-draft-aarch64.tar.gz","browser_download_url":"https://x.test/a"}]},
	  {"tag_name":"insecure","assets":[{"name":"turnip-insecure-aarch64.tar.gz","browser_download_url":"http://x.test/a"}]},
	  {"tag_name":"wrong-arch","assets":[{"name":"turnip-wrong-x86_64.tar.gz","browser_download_url":"https://x.test/a"}]},
	  {"tag_name":"no-url","assets":[{"name":"turnip-nourl-aarch64.tar.gz"}]},
	  {"tag_name":"pre","prerelease":true,"assets":[{"name":"turnip-pre-aarch64.tar.gz","size":5,"browser_download_url":"https://x.test/pre"}]},
	  {"name":"Only a title","assets":[{"name":"turnip-titled-aarch64.tar.gz","browser_download_url":"https://x.test/t"}]},
	  {"tag_name":"no-assets"}
	])";
	std::vector<RemoteDriver> drivers;
	ASSERT_TRUE(ParseReleases(DefaultFeeds()[0], json, &drivers, nullptr));
	ASSERT_EQ(drivers.size(), 2u);
	EXPECT_EQ(drivers[0].tag, "pre");
	EXPECT_TRUE(drivers[0].prerelease);
	EXPECT_EQ(drivers[0].checksum_url, "");
	EXPECT_EQ(drivers[0].api_digest, "");
	EXPECT_EQ(drivers[1].tag, "Only a title"); // the title stands in for a missing tag
	EXPECT_EQ(drivers[1].id, "armsx2turnip-only_a_title");
}

TEST(DriverPacksFeed, DuplicateIdsKeepTheFirst)
{
	const std::string json = R"([
	  {"tag_name":"A.1","assets":[{"name":"turnip-1-aarch64.tar.gz","browser_download_url":"https://x.test/first"}]},
	  {"tag_name":"a.1","assets":[{"name":"turnip-2-aarch64.tar.gz","browser_download_url":"https://x.test/second"}]}
	])";
	std::vector<RemoteDriver> drivers;
	ASSERT_TRUE(ParseReleases(DefaultFeeds()[0], json, &drivers, nullptr));
	ASSERT_EQ(drivers.size(), 1u);
	EXPECT_EQ(drivers[0].download_url, "https://x.test/first");
}

TEST(DriverPacksFeed, ADigestThatIsNotSha256HexIsIgnored)
{
	const std::string json = R"([{"tag_name":"t","assets":[{"name":"turnip-t-aarch64.tar.gz","digest":"sha1:abc","browser_download_url":"https://x.test/a"}]}])";
	std::vector<RemoteDriver> drivers;
	ASSERT_TRUE(ParseReleases(DefaultFeeds()[0], json, &drivers, nullptr));
	ASSERT_EQ(drivers.size(), 1u);
	EXPECT_EQ(drivers[0].api_digest, "");
}

TEST(DriverPacksFeed, RejectsWhatIsNotAReleaseList)
{
	std::vector<RemoteDriver> drivers;
	std::string error;
	EXPECT_FALSE(ParseReleases(DefaultFeeds()[0], "{\"message\":\"API rate limit exceeded\"}", &drivers, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(ParseReleases(DefaultFeeds()[0], "<html>", &drivers, &error));
	EXPECT_TRUE(ParseReleases(DefaultFeeds()[0], "[]", &drivers, &error));
	EXPECT_TRUE(drivers.empty());
}

TEST(DriverPacksFeed, TheDefaultFeedIsTheTurnipRepoForLinuxAarch64)
{
	const std::vector<Feed> feeds = DefaultFeeds();
	ASSERT_EQ(feeds.size(), 1u);
	EXPECT_EQ(feeds[0].releases_url, RELEASES_URL);
	EXPECT_EQ(feeds[0].platform, "linux-aarch64");
	EXPECT_EQ(feeds[0].asset_pattern, "turnip-*-aarch64.tar.gz");
}

TEST(DriverPacksIds, AreFolderSafe)
{
	EXPECT_EQ(MakeDriverId("armsx2turnip", "axfl2-002"), "armsx2turnip-axfl2-002");
	EXPECT_EQ(MakeDriverId("Local", "My Pack (v2)/../x"), "local-my_pack__v2__.._x"); // no separator survives
	EXPECT_EQ(MakeDriverId("", ".hidden"), "_hidden");
	EXPECT_EQ(MakeDriverId("p", std::string(300, 'a')).size(), 128u);

	EXPECT_TRUE(IsValidDriverId("armsx2turnip-axfl2-002"));
	EXPECT_TRUE(IsValidDriverId("a.b_c-d"));
	EXPECT_FALSE(IsValidDriverId(""));
	EXPECT_FALSE(IsValidDriverId(".hidden"));
	EXPECT_FALSE(IsValidDriverId("a/b"));
	EXPECT_FALSE(IsValidDriverId(".."));
	EXPECT_FALSE(IsValidDriverId("Upper"));
	EXPECT_FALSE(IsValidDriverId(std::string(129, 'a')));
}

// =================================================================================================
// Which machines get packs
// =================================================================================================

TEST(DriverPacksGpu, AdrenoIsADrmDeviceOnTheMsmDriver)
{
	ScopedTempDir dir;
	EXPECT_FALSE(IsAdrenoGpu(dir.File("sys"))); // no sysfs at all

	MakeFakeSysfs(dir.File("a"), "msm");
	EXPECT_TRUE(IsAdrenoGpu(dir.File("a")));

	MakeFakeSysfs(dir.File("b"), "asahi");
	EXPECT_FALSE(IsAdrenoGpu(dir.File("b"))); // the HDMI connector entry is bound to "msm" and must not count

	MakeFakeSysfs(dir.File("c"), "panfrost");
	EXPECT_FALSE(IsAdrenoGpu(dir.File("c")));
}

// Issue #887: on current mainline kernels (ROCKNIX 20260930 and Armada 20261010 on the SM8250, kernel
// 7.2) the DRM device belongs to the display controller's driver, msm_dpu. Older SoCs use msm_mdp.
TEST(DriverPacksGpu, TheMsmDisplayControllerDriversCount)
{
	ScopedTempDir dir;
	MakeFakeSysfs(dir.File("dpu"), "msm_dpu");
	EXPECT_TRUE(IsAdrenoGpu(dir.File("dpu")));

	MakeFakeSysfs(dir.File("mdp"), "msm_mdp");
	EXPECT_TRUE(IsAdrenoGpu(dir.File("mdp")));
}

// The GPU has its own platform driver, adreno, whatever the display side is called.
TEST(DriverPacksGpu, AGpuBoundToTheAdrenoDriverCounts)
{
	ScopedTempDir dir;
	const fs::path adreno = fs::path(dir.File("sys")) / "bus" / "platform" / "drivers" / "adreno";
	fs::create_directories(adreno);
	fs::create_symlink("../../../../module/msm", adreno / "module");
	EXPECT_FALSE(IsAdrenoGpu(dir.File("sys"))); // the driver is loaded but no GPU is bound to it

	fs::create_symlink("../../../../devices/platform/soc@0/3d00000.gpu", adreno / "3d00000.gpu");
	EXPECT_TRUE(IsAdrenoGpu(dir.File("sys")));
}

TEST(DriverPacksGpu, ADownstreamKgslDeviceCounts)
{
	ScopedTempDir dir;
	fs::create_directories(dir.File("sys/class/kgsl/kgsl-3d0"));
	EXPECT_TRUE(IsAdrenoGpu(dir.File("sys")));
}

TEST(DriverPacksGpu, AvailabilityExplainsItself)
{
	Rig adreno(true);
	EXPECT_TRUE(adreno.manager->GetAvailability().IsAvailable());
	EXPECT_EQ(adreno.manager->GetAvailability().message, "");

	Rig mali(false);
	const Availability no_gpu = mali.manager->GetAvailability();
	EXPECT_EQ(no_gpu.reason, Availability::Reason::NoMatchingGpu);
	EXPECT_EQ(no_gpu.message, "No driver packs for this GPU.");

	Rig other(true);
	other.config.platform.clear();
	other.Rebuild();
	EXPECT_EQ(other.manager->GetAvailability().reason, Availability::Reason::UnsupportedPlatform);
	EXPECT_EQ(other.manager->GetAvailability().message, "Driver packs are not available on this platform.");
}

// =================================================================================================
// First-run setup
// =================================================================================================

namespace
{
	// A machine the setup should offer a pack on: Adreno on Linux, nothing chosen, nothing installed.
	SetupContext OfferableContext()
	{
		SetupContext context;
		context.availability.reason = Availability::Reason::Available;
		return context;
	}
} // namespace

TEST(DriverPacksSetup, OffersAPackOnAnAdrenoThatHasNone)
{
	EXPECT_TRUE(ShouldOfferInSetup(OfferableContext()));
}

TEST(DriverPacksSetup, OffersNothingWhereThereAreNoPacks)
{
	SetupContext no_gpu = OfferableContext();
	no_gpu.availability.reason = Availability::Reason::NoMatchingGpu;
	EXPECT_FALSE(ShouldOfferInSetup(no_gpu));

	SetupContext other_os = OfferableContext();
	other_os.availability.reason = Availability::Reason::UnsupportedPlatform;
	EXPECT_FALSE(ShouldOfferInSetup(other_os));
}

TEST(DriverPacksSetup, OffersNothingToAPlayerWhoAlreadyDecided)
{
	SetupContext selected = OfferableContext();
	selected.selected_pack = "armsx2turnip-axfl2-002";
	EXPECT_FALSE(ShouldOfferInSetup(selected));

	// Installed but not selected: the player has been to the driver settings.
	SetupContext installed = OfferableContext();
	installed.installed_count = 1;
	EXPECT_FALSE(ShouldOfferInSetup(installed));
}

TEST(DriverPacksSetup, OffersNothingWhereALauncherSetsTheDriver)
{
	// ROCKNIX's launcher sets VK_DRIVER_FILES, so a selection would be ignored.
	SetupContext launcher = OfferableContext();
	launcher.environment_sets_driver = true;
	EXPECT_FALSE(ShouldOfferInSetup(launcher));
}

TEST(DriverPacksSetup, OffersNothingOnAnX11Session)
{
	SetupContext x11 = OfferableContext();
	x11.session_is_x11 = true;
	EXPECT_FALSE(ShouldOfferInSetup(x11));
}

TEST(DriverPacksSetup, AnEnvironmentThatNamesADriverCountsAsALauncherChoice)
{
	// ApplyEnvironment() says nothing about the environment while no pack is selected, so the setup reads it.
	EnvGuard guard;
	BaseLayer();
	SetSelectedPack({});
	Rig rig(true);
	EXPECT_FALSE(GetSetupContext(*rig.manager, false).environment_sets_driver);

	setenv("VK_DRIVER_FILES", "/storage/turnip/axfl2-002/freedreno_icd.aarch64.json", 1);
	EXPECT_TRUE(GetSetupContext(*rig.manager, false).environment_sets_driver);
	EXPECT_FALSE(ShouldOfferInSetup(GetSetupContext(*rig.manager, false)));

	unsetenv("VK_DRIVER_FILES");
	setenv("VK_ICD_FILENAMES", "/storage/turnip/axfl2-002/freedreno_icd.aarch64.json", 1);
	EXPECT_TRUE(GetSetupContext(*rig.manager, false).environment_sets_driver);
}

TEST(DriverPacksSetup, ReadsTheContextFromTheMachine)
{
	EnvGuard guard;
	BaseLayer();
	SetSelectedPack({});

	Rig rig(true);
	SetupContext context = GetSetupContext(*rig.manager, false);
	EXPECT_TRUE(context.availability.IsAvailable());
	EXPECT_EQ(context.selected_pack, "");
	EXPECT_EQ(context.installed_count, 0u);
	EXPECT_FALSE(context.session_is_x11);

	// Manager::Install exists only on desktop Linux, but listing the installed packs works on
	// every platform, so the pack is laid out by hand and this check runs everywhere.
	const std::string pack = Path::Combine(rig.config.drivers_dir, "armsx2turnip-axfl2-002");
	fs::create_directories(fs::path(pack));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(pack, "meta.json"), R"({"manifest":"freedreno_icd.aarch64.json","library":"libvulkan_freedreno.so"})"));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(pack, "freedreno_icd.aarch64.json"), "{}"));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(pack, "libvulkan_freedreno.so"), "lib"));
	context = GetSetupContext(*rig.manager, true);
	EXPECT_EQ(context.installed_count, 1u);
	EXPECT_TRUE(context.session_is_x11);

	SetSelectedPack("armsx2turnip-axfl2-002");
	EXPECT_EQ(GetSetupContext(*rig.manager, false).selected_pack, "armsx2turnip-axfl2-002");
	SetSelectedPack({});

	Rig mali(false);
	EXPECT_FALSE(GetSetupContext(*mali.manager, false).availability.IsAvailable());
}

TEST(DriverPacksSetup, TheRecommendedPackIsTheNewestThatIsNotAPrerelease)
{
	EXPECT_EQ(PickRecommended({}), nullptr);

	RemoteDriver beta;
	beta.id = "armsx2turnip-beta";
	beta.prerelease = true;
	RemoteDriver stable;
	stable.id = "armsx2turnip-stable";
	RemoteDriver older;
	older.id = "armsx2turnip-older";

	const std::vector<RemoteDriver> mixed = {beta, stable, older};
	ASSERT_NE(PickRecommended(mixed), nullptr);
	EXPECT_EQ(PickRecommended(mixed)->id, "armsx2turnip-stable");

	const std::vector<RemoteDriver> only_prereleases = {beta};
	ASSERT_NE(PickRecommended(only_prereleases), nullptr);
	EXPECT_EQ(PickRecommended(only_prereleases)->id, "armsx2turnip-beta");
}

// =================================================================================================
// Listing what a feed offers
// =================================================================================================

TEST(DriverPacksListing, ReadsTheFeedAndSendsAGithubAcceptHeader)
{
	Rig rig;
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));

	const RemoteListing listing = rig.manager->ListAvailable({});
	ASSERT_EQ(listing.drivers.size(), 2u);
	EXPECT_TRUE(listing.errors.empty());
	EXPECT_FALSE(listing.used_stale_cache);

	const auto requests = rig.http.Requests();
	ASSERT_EQ(requests.size(), 1u);
	const bool has_accept = std::any_of(requests[0].headers.begin(), requests[0].headers.end(), [](const auto& h) { return h.first == "Accept" && h.second.find("github") != std::string::npos; });
	EXPECT_TRUE(has_accept);
}

TEST(DriverPacksListing, ACachedListingSavesTheRequestUntilItIsOld)
{
	Rig rig;
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));

	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);
	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);
	EXPECT_EQ(rig.http.RequestCount(RELEASES_URL), 1u);

	// force_refresh goes to the network anyway.
	ASSERT_EQ(rig.manager->ListAvailable({}, true).drivers.size(), 2u);
	EXPECT_EQ(rig.http.RequestCount(RELEASES_URL), 2u);

	// A listing past its maximum age is fetched again.
	rig.config.listing_max_age = std::chrono::seconds(0);
	rig.Rebuild();
	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);
	EXPECT_EQ(rig.http.RequestCount(RELEASES_URL), 3u);
}

TEST(DriverPacksListing, ARateLimitFallsBackToTheCachedCopy)
{
	Rig rig;
	// Replies are served in the order scripted: the first request succeeds, the next gets 429.
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	rig.http.Status(RELEASES_URL, 429, R"({"message":"rate limit"})");
	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);

	const RemoteListing listing = rig.manager->ListAvailable({}, true);
	EXPECT_EQ(listing.drivers.size(), 2u);
	EXPECT_TRUE(listing.used_stale_cache);
	EXPECT_TRUE(listing.errors.empty());
}

TEST(DriverPacksListing, OfflineFallsBackToTheCachedCopy)
{
	Rig rig;
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	rig.http.Fail(RELEASES_URL, "could not resolve host");
	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);

	const RemoteListing listing = rig.manager->ListAvailable({}, true);
	EXPECT_EQ(listing.drivers.size(), 2u);
	EXPECT_TRUE(listing.used_stale_cache);
}

TEST(DriverPacksListing, WithNothingCachedAFailureIsReported)
{
	Rig rig;
	rig.http.Status(RELEASES_URL, 403, "{}");
	RemoteListing listing = rig.manager->ListAvailable({});
	EXPECT_TRUE(listing.drivers.empty());
	ASSERT_EQ(listing.errors.size(), 1u);
	EXPECT_EQ(listing.errors[0].feed_label, "ARMSX2 Turnip");
	EXPECT_NE(listing.errors[0].message.find("limiting"), std::string::npos) << listing.errors[0].message;

	Rig down;
	down.http.Fail(RELEASES_URL, "connection refused");
	listing = down.manager->ListAvailable({});
	ASSERT_EQ(listing.errors.size(), 1u);
	EXPECT_NE(listing.errors[0].message.find("connection refused"), std::string::npos);

	Rig garbled;
	garbled.http.Ok(RELEASES_URL, "<html>captive portal</html>");
	listing = garbled.manager->ListAvailable({});
	EXPECT_TRUE(listing.drivers.empty());
	EXPECT_EQ(listing.errors.size(), 1u);
}

TEST(DriverPacksListing, ABadResponseDoesNotOverwriteAGoodCache)
{
	Rig rig;
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	rig.http.Ok(RELEASES_URL, "<html>captive portal</html>");
	ASSERT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);

	const RemoteListing listing = rig.manager->ListAvailable({}, true);
	EXPECT_EQ(listing.drivers.size(), 2u); // still the good copy
	EXPECT_TRUE(listing.used_stale_cache);
}

TEST(DriverPacksListing, MakesNoRequestOnAMachineWithoutAnAdrenoGpu)
{
	Rig rig(false);
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	const RemoteListing listing = rig.manager->ListAvailable({});
	EXPECT_TRUE(listing.drivers.empty());
	EXPECT_TRUE(rig.http.Requests().empty());
}

TEST(DriverPacksListing, OnlyFeedsForThisPlatformAreRead)
{
	Rig rig;
	Feed other;
	other.id_prefix = "other";
	other.label = "Other";
	other.releases_url = "https://api.github.com/repos/example/other/releases";
	other.asset_pattern = "*.tar.gz";
	other.platform = "linux-x86_64";
	rig.config.feeds.push_back(other);
	rig.Rebuild();
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));

	EXPECT_EQ(rig.manager->ListAvailable({}).drivers.size(), 2u);
	EXPECT_EQ(rig.http.RequestCount(other.releases_url), 0u);
}

TEST(DriverPacksListing, ASecondFeedAddsToTheFirstAndAFailingOneDoesNotHideIt)
{
	Rig rig;
	Feed second;
	second.id_prefix = "second";
	second.label = "Second";
	second.releases_url = "https://api.github.com/repos/example/second/releases";
	second.asset_pattern = "*.tar.gz";
	second.platform = PLATFORM;
	rig.config.feeds.push_back(second);
	rig.Rebuild();
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	rig.http.Fail(second.releases_url, "boom");

	const RemoteListing listing = rig.manager->ListAvailable({});
	EXPECT_EQ(listing.drivers.size(), 2u);
	ASSERT_EQ(listing.errors.size(), 1u);
	EXPECT_EQ(listing.errors[0].feed_label, "Second");
}

TEST(DriverPacksListing, CancellationStopsBeforeTheRequest)
{
	Rig rig;
	rig.http.Ok(RELEASES_URL, CatalogTest::ReadFixture("drivers/releases.json"));
	Catalog::CancelToken cancel;
	cancel.Cancel();
	EXPECT_TRUE(rig.manager->ListAvailable(cancel).drivers.empty());
	EXPECT_TRUE(rig.http.Requests().empty());
}

// =================================================================================================
// Settings
// =================================================================================================

TEST(DriverPacksSettings, TheSelectionRoundTripsThroughTheBaseSettings)
{
	BaseLayer();
	SetSelectedPack({});
	EXPECT_EQ(GetSelectedPack(), "");

	SetSelectedPack("armsx2turnip-axfl2-002");
	EXPECT_EQ(GetSelectedPack(), "armsx2turnip-axfl2-002");
	EXPECT_EQ(BaseLayer().GetStringValue("EmuCore/GS", "LinuxDriverPack", ""), "armsx2turnip-axfl2-002");

	BaseLayer().SetStringValue("EmuCore/GS", "LinuxDriverPack", "  padded \n");
	EXPECT_EQ(GetSelectedPack(), "padded");

	SetSelectedPack({});
	EXPECT_EQ(GetSelectedPack(), "");
	EXPECT_FALSE(BaseLayer().ContainsValue("EmuCore/GS", "LinuxDriverPack"));
}

#ifdef DRIVER_TESTS_LINUX

// =================================================================================================
// Loading a library the way the Vulkan loader will
// =================================================================================================

#ifdef CATALOG_TEST_LIB_DIR

TEST(DriverPacksTestLoad, ALibraryThatLoadsPasses)
{
	std::string error;
	EXPECT_TRUE(TestLoadLibrary(TestLib("ok"), &error)) << error;
}

TEST(DriverPacksTestLoad, TheLoadingProcessKeepsNoDescriptorsButItsOwnPipe)
{
	// The emulator has files, sockets and state open when the check forks. The child must not carry them into
	// the vendor library's constructors; ctorfds aborts if it finds any descriptor above stderr besides the
	// one the child answers on.
	const int first = open("/dev/null", O_RDONLY); // no O_CLOEXEC: it would survive an exec too
	const int second = dup(first);
	const int third = fcntl(first, F_DUPFD, 200);
	ASSERT_GE(first, 3);
	ASSERT_GE(second, 3);
	ASSERT_GE(third, 200);

	std::string error;
	EXPECT_TRUE(TestLoadLibrary(TestLib("ctorfds"), &error)) << error;

	// Closing them in the child leaves the parent's alone.
	EXPECT_NE(fcntl(first, F_GETFD), -1);
	EXPECT_NE(fcntl(second, F_GETFD), -1);
	EXPECT_NE(fcntl(third, F_GETFD), -1);
	close(first);
	close(second);
	close(third);
}

TEST(DriverPacksTestLoad, ALibraryWithAMissingDependencyIsRefusedWithTheLibraryName)
{
	ScopedTempDir dir;
	fs::copy_file(TestLib("dep"), dir.File("libcatalogtestdep.so"));
	fs::copy_file(TestLib("needsdep"), dir.File("libcatalogtestneedsdep.so"));

	std::string error;
	ASSERT_TRUE(TestLoadLibrary(dir.File("libcatalogtestneedsdep.so"), &error)) << error; // the pair loads

	fs::remove(dir.File("libcatalogtestdep.so"));
	EXPECT_FALSE(TestLoadLibrary(dir.File("libcatalogtestneedsdep.so"), &error));
	EXPECT_NE(error.find("libcatalogtestdep.so"), std::string::npos) << error;
	EXPECT_NE(error.find("cannot open shared object file"), std::string::npos) << error;
	EXPECT_NE(error.find("not installed"), std::string::npos) << error;
}

TEST(DriverPacksTestLoad, AnUndefinedSymbolIsRefused)
{
	std::string error;
	EXPECT_FALSE(TestLoadLibrary(TestLib("undefinedsymbol"), &error));
	EXPECT_NE(error.find("catalog_test_no_such_function"), std::string::npos) << error;
}

TEST(DriverPacksTestLoad, AConstructorThatAbortsCannotTakeTheCallerDown)
{
	std::string error;
	EXPECT_FALSE(TestLoadLibrary(TestLib("ctorabort"), &error));
	EXPECT_NE(error.find("crashed"), std::string::npos) << error;
	EXPECT_NE(error.find("Abort"), std::string::npos) << error; // strsignal(SIGABRT)
}

TEST(DriverPacksTestLoad, AConstructorThatSegfaultsCannotTakeTheCallerDown)
{
	std::string error;
	EXPECT_FALSE(TestLoadLibrary(TestLib("ctorsegv"), &error));
	EXPECT_NE(error.find("crashed"), std::string::npos) << error;
	EXPECT_NE(error.find("Segmentation"), std::string::npos) << error;
}

TEST(DriverPacksTestLoad, ACrashHandlerInTheCallerDoesNotRunInTheChild)
{
	// The emulator installs SIGSEGV/SIGABRT handlers. If the child inherited one, a crashing
	// constructor would run it and exit normally instead of dying by the signal, so the parent
	// would not see a crash. Install one that exits 0 and check the crash is still reported.
	struct sigaction handler;
	std::memset(&handler, 0, sizeof(handler));
	handler.sa_handler = [](int) { _exit(0); };
	sigemptyset(&handler.sa_mask);
	struct sigaction old_segv, old_abrt;
	sigaction(SIGSEGV, &handler, &old_segv);
	sigaction(SIGABRT, &handler, &old_abrt);

	std::string error;
	const bool loaded = TestLoadLibrary(TestLib("ctorsegv"), &error);
	const std::string error_abort = [&] {
		std::string e;
		TestLoadLibrary(TestLib("ctorabort"), &e);
		return e;
	}();

	sigaction(SIGSEGV, &old_segv, nullptr);
	sigaction(SIGABRT, &old_abrt, nullptr);

	EXPECT_FALSE(loaded);
	EXPECT_NE(error.find("crashed"), std::string::npos) << error;
	EXPECT_NE(error_abort.find("crashed"), std::string::npos) << error_abort;
}

TEST(DriverPacksTestLoad, ADestructorThatAbortsOnUnloadIsRefused)
{
	std::string error;
	EXPECT_FALSE(TestLoadLibrary(TestLib("dtorabort"), &error));
	EXPECT_NE(error.find("crashed"), std::string::npos) << error;
}

TEST(DriverPacksTestLoad, AConstructorThatNeverReturnsIsStopped)
{
	std::string error;
	const auto start = std::chrono::steady_clock::now();
	EXPECT_FALSE(TestLoadLibrary(TestLib("ctorhang"), &error, std::chrono::milliseconds(700)));
	const auto elapsed = std::chrono::steady_clock::now() - start;
	EXPECT_NE(error.find("did not finish"), std::string::npos) << error;
	EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST(DriverPacksTestLoad, FilesThatAreNotLibrariesAreRefused)
{
	ScopedTempDir dir;
	std::string error;
	EXPECT_FALSE(TestLoadLibrary(dir.File("missing.so"), &error));
	EXPECT_NE(error.find("cannot be read"), std::string::npos) << error;

	ASSERT_TRUE(CatalogTest::WriteBytes(dir.File("text.so"), "not an elf file\n"));
	EXPECT_FALSE(TestLoadLibrary(dir.File("text.so"), &error));
	EXPECT_NE(error.find("could not be loaded"), std::string::npos) << error;
}

#endif // CATALOG_TEST_LIB_DIR

// =================================================================================================
// Installing
// =================================================================================================

TEST(DriverPacksInstall, TheRealLayoutInstallsAndTheManifestPointsAtTheInstalledLibrary)
{
	// The fixture is a GNU tar of the real pack's layout (files under turnip/axfl2-002/, the ICD
	// manifest's library_path an absolute ROCKNIX path).
	Rig rig;
	const std::string archive = FileSystem::ReadFileToString(CatalogTest::FixturePath("drivers/turnip-synth-aarch64.tar.gz").c_str()).value_or("");
	ASSERT_FALSE(archive.empty());
	const RemoteDriver driver = rig.Serve(archive);

	const InstallResult result = rig.Install(driver);
	ASSERT_TRUE(result.Ok()) << result.message;
	EXPECT_EQ(result.id, "armsx2turnip-axfl2-002");

	const std::string dir = rig.InstalledPath(result.id);
	// Wrapper folders are gone; the pack's files are at the top.
	EXPECT_TRUE(fs::exists(Path::Combine(dir, "libvulkan_freedreno.so")));
	EXPECT_TRUE(fs::exists(Path::Combine(dir, "freedreno_icd.aarch64.json")));
	EXPECT_TRUE(fs::exists(Path::Combine(dir, "MANIFEST.json")));
	EXPECT_TRUE(fs::exists(Path::Combine(dir, "lib-fallback/libz.so.1")));
	EXPECT_FALSE(fs::exists(Path::Combine(dir, "turnip")));
	EXPECT_FALSE(rig.StagingLeftOver());

	// The shipped manifest named /storage/turnip/axfl2-002/...; ours names the installed file.
	const rapidjson::Document manifest = ReadJson(Path::Combine(dir, "freedreno_icd.aarch64.json"));
	ASSERT_TRUE(manifest.IsObject());
	EXPECT_EQ(Str(manifest["ICD"], "library_path"), Path::Combine(dir, "libvulkan_freedreno.so"));
	EXPECT_TRUE(Path::IsAbsolute(Str(manifest["ICD"], "library_path")));
	EXPECT_EQ(Str(manifest["ICD"], "api_version"), "1.4.363"); // the rest of the shipped manifest is kept
	EXPECT_EQ(Str(manifest["ICD"], "library_arch"), "64");
	EXPECT_EQ(Str(manifest, "file_format_version"), "1.0.1");

	// The library check ran on the library, before the pack was committed.
	ASSERT_EQ(rig.checked_libraries.size(), 1u);
	EXPECT_EQ(Path::GetFileName(rig.checked_libraries[0]), "libvulkan_freedreno.so");

	const rapidjson::Document meta = ReadJson(Path::Combine(dir, "meta.json"));
	EXPECT_EQ(Str(meta, "id"), "armsx2turnip-axfl2-002");
	EXPECT_EQ(Str(meta, "version"), "axfl2-002");
	EXPECT_EQ(Str(meta, "manifest"), "freedreno_icd.aarch64.json");
	EXPECT_EQ(Str(meta, "library"), "libvulkan_freedreno.so");
	EXPECT_EQ(Str(meta, "buildTag"), "axfl2-002");
	EXPECT_EQ(Str(meta, "driverInfoToken"), "(git-axfl2-002)");
	EXPECT_EQ(Str(meta, "mesaVersion"), "26.3.0-devel");
	EXPECT_EQ(Str(meta, "platforms"), "wayland");
	EXPECT_EQ(Str(meta, "archiveSha256"), Sha(archive));

	const std::vector<InstalledDriver> installed = rig.manager->ListInstalled();
	ASSERT_EQ(installed.size(), 1u);
	EXPECT_EQ(installed[0].id, "armsx2turnip-axfl2-002");
	EXPECT_EQ(installed[0].name, "Custom Turnip driver for ARMSX2 axfl2-002");
	EXPECT_EQ(installed[0].driver_info_token, "(git-axfl2-002)");
	EXPECT_EQ(installed[0].library_path, Path::Combine(dir, "libvulkan_freedreno.so"));
	EXPECT_EQ(installed[0].manifest_path, Path::Combine(dir, "freedreno_icd.aarch64.json"));
	EXPECT_GT(installed[0].installed_at, 0);
	EXPECT_TRUE(rig.manager->FindInstalled("armsx2turnip-axfl2-002").has_value());
	EXPECT_FALSE(rig.manager->FindInstalled("nope").has_value());
}

TEST(DriverPacksInstall, ProgressWalksThroughTheStagesInOrder)
{
	Rig rig;
	const RemoteDriver driver = rig.Serve(MakePackTarGz("lib"));
	std::vector<InstallStage> stages;
	u64 last_download_total = 0;
	const InstallResult result = rig.Install(driver, [&](const InstallProgress& p) {
		if (stages.empty() || stages.back() != p.stage)
			stages.push_back(p.stage);
		if (p.stage == InstallStage::Downloading)
			last_download_total = p.total;
	});
	ASSERT_TRUE(result.Ok()) << result.message;
	EXPECT_EQ(stages, (std::vector<InstallStage>{InstallStage::Preparing, InstallStage::Downloading, InstallStage::Verifying, InstallStage::Extracting, InstallStage::Checking, InstallStage::Finishing}));
	EXPECT_EQ(last_download_total, driver.size);
}

TEST(DriverPacksInstall, ARealLibraryPassesTheRealTestLoad)
{
#ifdef CATALOG_TEST_LIB_DIR
	Rig rig(true, false); // no injected check: the library really is dlopened
	const std::string good = TestLibBytes("ok");
	ASSERT_FALSE(good.empty());
	const RemoteDriver driver = rig.Serve(MakePackTarGz(good));

	const InstallResult result = rig.Install(driver);
	ASSERT_TRUE(result.Ok()) << result.message;

	// And the installed library, at its final path, loads.
	const auto installed = rig.manager->FindInstalled(result.id);
	ASSERT_TRUE(installed.has_value());
	std::string error;
	EXPECT_TRUE(TestLoadLibrary(installed->library_path, &error)) << error;
#else
	GTEST_SKIP() << "test libraries not built";
#endif
}

TEST(DriverPacksInstall, ALibraryThatDoesNotLoadIsRefusedAndNothingIsInstalled)
{
#ifdef CATALOG_TEST_LIB_DIR
	Rig rig(true, false);
	const RemoteDriver driver = rig.Serve(MakePackTarGz("this is not a shared library"));

	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::LoadFailed);
	EXPECT_NE(result.message.find("could not be loaded"), std::string::npos) << result.message;
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
	EXPECT_FALSE(fs::exists(rig.InstalledPath("armsx2turnip-axfl2-002")));
	EXPECT_FALSE(rig.StagingLeftOver());

	// A constructor that crashes is also just a refusal.
	Rig crashy(true, false);
	const RemoteDriver bad = crashy.Serve(MakePackTarGz(TestLibBytes("ctorsegv")));
	const InstallResult crashed = crashy.Install(bad);
	EXPECT_EQ(crashed.status, InstallStatus::LoadFailed);
	EXPECT_NE(crashed.message.find("crashed"), std::string::npos) << crashed.message;
	EXPECT_TRUE(crashy.manager->ListInstalled().empty());
#else
	GTEST_SKIP() << "test libraries not built";
#endif
}

TEST(DriverPacksInstall, ALibraryCheckFailureKeepsThePreviousInstall)
{
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("first"))).Ok());
	const std::string before = FileSystem::ReadFileToString(rig.InstalledPath("armsx2turnip-axfl2-002", "libvulkan_freedreno.so").c_str()).value_or("");
	EXPECT_EQ(before, "first");

	rig.config.library_check = [](const std::string&, std::string* error) {
		*error = "libwayland-client.so.0: cannot open shared object file";
		return false;
	};
	rig.Rebuild();
	const RemoteDriver second = rig.Serve(MakePackTarGz("second"));
	const InstallResult result = rig.Install(second);
	EXPECT_EQ(result.status, InstallStatus::LoadFailed);
	EXPECT_EQ(result.message, "libwayland-client.so.0: cannot open shared object file");
	EXPECT_EQ(FileSystem::ReadFileToString(rig.InstalledPath("armsx2turnip-axfl2-002", "libvulkan_freedreno.so").c_str()).value_or(""), "first");
}

TEST(DriverPacksInstall, ReinstallingReplacesThePack)
{
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("first"))).Ok());
	// Stray file in the old install: gone after the replacement.
	ASSERT_TRUE(CatalogTest::WriteBytes(rig.InstalledPath("armsx2turnip-axfl2-002", "stray.txt"), "x"));

	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("second"))).Ok());
	EXPECT_EQ(FileSystem::ReadFileToString(rig.InstalledPath("armsx2turnip-axfl2-002", "libvulkan_freedreno.so").c_str()).value_or(""), "second");
	EXPECT_FALSE(fs::exists(rig.InstalledPath("armsx2turnip-axfl2-002", "stray.txt")));
	EXPECT_EQ(rig.manager->ListInstalled().size(), 1u);
	EXPECT_FALSE(rig.StagingLeftOver());
}

TEST(DriverPacksInstall, ThePackCanSitAtTheArchiveRootOrDeepInsideIt)
{
	for (const char* prefix : {"", "a/b/c/d/"})
	{
		Rig rig;
		const RemoteDriver driver = rig.Serve(MakePackTarGz("lib", prefix));
		const InstallResult result = rig.Install(driver);
		ASSERT_TRUE(result.Ok()) << prefix << ": " << result.message;
		EXPECT_TRUE(fs::exists(rig.InstalledPath(result.id, "libvulkan_freedreno.so"))) << prefix;
		EXPECT_FALSE(fs::exists(rig.InstalledPath(result.id, "a"))) << prefix;
	}
}

TEST(DriverPacksInstall, ARelativeLibraryPathInTheManifestIsHonoured)
{
	Rig rig;
	const std::string manifest = R"({"ICD":{"api_version":"1.3.0","library_path":"./lib/libother.so"},"file_format_version":"1.0.0"})";
	const std::string archive = BuildTarGz({
		Entry("pack/vk_icd.json", manifest),
		Entry("pack/lib/libother.so", "relative", 0755),
		Entry("pack/libother.so", "decoy", 0755),
	});
	const InstallResult result = rig.Install(rig.Serve(archive));
	ASSERT_TRUE(result.Ok()) << result.message;
	const auto installed = rig.manager->FindInstalled(result.id);
	ASSERT_TRUE(installed.has_value());
	EXPECT_EQ(installed->library_path, rig.InstalledPath(result.id, "lib/libother.so"));
	EXPECT_EQ(Str(ReadJson(installed->manifest_path)["ICD"], "library_path"), installed->library_path);
}

TEST(DriverPacksInstall, ArchivesThatAreNotPacksAreRefused)
{
	struct Case
	{
		const char* what;
		std::string archive;
		const char* message_part;
	};
	const std::vector<Case> cases = {
		{"no manifest", BuildTarGz({Entry("p/libvulkan_freedreno.so", "x")}), "no Vulkan driver manifest"},
		{"manifest without ICD", BuildTarGz({Entry("p/icd.json", "{\"x\":1}"), Entry("p/libvulkan_freedreno.so", "x")}), "no Vulkan driver manifest"},
		{"library missing", BuildTarGz({Entry("p/freedreno_icd.aarch64.json", IcdManifestJson())}), "does not contain it"},
		{"empty library", BuildTarGz({Entry("p/freedreno_icd.aarch64.json", IcdManifestJson()), Entry("p/libvulkan_freedreno.so", "")}), "does not contain it"},
		{"path traversal", BuildTarGz({Entry("p/freedreno_icd.aarch64.json", IcdManifestJson()), Entry("../evil.so", "x")}), "Unsafe path"},
		{"not gzip", "just some bytes", "corrupt"},
	};
	for (const Case& c : cases)
	{
		Rig rig;
		const InstallResult result = rig.Install(rig.Serve(c.archive));
		EXPECT_EQ(result.status, InstallStatus::BadArchive) << c.what << ": " << result.message;
		EXPECT_NE(result.message.find(c.message_part), std::string::npos) << c.what << ": " << result.message;
		EXPECT_TRUE(rig.manager->ListInstalled().empty()) << c.what;
		EXPECT_FALSE(rig.StagingLeftOver()) << c.what;
		EXPECT_FALSE(fs::exists(Path::Combine(rig.dir.Dir(), "evil.so"))) << c.what;
	}
}

// ---- trust: the checksum -----------------------------------------------------------------------------

TEST(DriverPacksInstall, AWrongChecksumInstallsNothing)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream");
	rig.http.Ok(SUM_URL, std::string(64, 'a') + "  turnip-axfl2-002-aarch64.tar.gz\n", "text/plain");
	const RemoteDriver driver = rig.Remote(archive, true, false);

	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::ChecksumMismatch);
	EXPECT_NE(result.message.find(Sha(archive)), std::string::npos) << result.message;
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
	EXPECT_TRUE(rig.checked_libraries.empty()); // never got as far as loading the library
	EXPECT_FALSE(rig.StagingLeftOver());
}

TEST(DriverPacksInstall, ACorruptedDownloadIsCaughtByTheChecksum)
{
	Rig rig;
	std::string archive = MakePackTarGz("lib");
	const RemoteDriver driver = rig.Remote(archive, false, true); // the digest of the good archive
	archive[archive.size() / 2] ^= 0x01;
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream"); // same size, one bit off

	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::ChecksumMismatch);
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
}

TEST(DriverPacksInstall, TheApiDigestStandsInWhenThereIsNoChecksumAsset)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream");
	const RemoteDriver driver = rig.Remote(archive, /*with_sum_asset=*/false, /*with_digest=*/true);
	ASSERT_TRUE(rig.Install(driver).Ok());
	EXPECT_EQ(rig.http.RequestCount(SUM_URL), 0u);
}

TEST(DriverPacksInstall, NoPublishedChecksumMeansNoInstallAndNoDownload)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream");
	const RemoteDriver driver = rig.Remote(archive, false, false);

	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::ChecksumMissing);
	EXPECT_EQ(rig.http.RequestCount(TARBALL_URL), 0u);
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
}

TEST(DriverPacksInstall, DisagreeingChecksumSourcesInstallNothing)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	RemoteDriver driver = rig.Serve(archive);
	driver.api_digest = std::string(64, 'b');

	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::ChecksumMismatch);
	EXPECT_NE(result.message.find("disagree"), std::string::npos) << result.message;
	EXPECT_EQ(rig.http.RequestCount(driver.download_url), 0u);
}

TEST(DriverPacksInstall, AnUnreadableChecksumFileIsRefused)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream");
	rig.http.Ok(SUM_URL, "<html>not a checksum</html>", "text/plain");
	RemoteDriver driver = rig.Remote(archive, true, false);
	EXPECT_EQ(rig.Install(driver).status, InstallStatus::ChecksumMissing);
}

TEST(DriverPacksInstall, AShaSumStyleFileWithUppercaseHexIsAccepted)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	rig.http.Ok(TARBALL_URL, archive, "application/octet-stream");
	std::string upper = Sha(archive);
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	rig.http.Ok(SUM_URL, upper + " *turnip-axfl2-002-aarch64.tar.gz\r\n", "text/plain");
	EXPECT_TRUE(rig.Install(rig.Remote(archive, true, false)).Ok());
}

// ---- trust: the transfer --------------------------------------------------------------------------------

TEST(DriverPacksInstall, DownloadFailuresAreReported)
{
	{
		Rig rig;
		const std::string archive = MakePackTarGz("lib");
		const RemoteDriver driver = rig.Remote(archive);
		rig.http.Ok(SUM_URL, Sha(archive) + "  x\n", "text/plain");
		rig.http.Status(TARBALL_URL, 404);
		const InstallResult result = rig.Install(driver);
		EXPECT_EQ(result.status, InstallStatus::DownloadFailed);
		EXPECT_NE(result.message.find("404"), std::string::npos) << result.message;
		EXPECT_FALSE(rig.StagingLeftOver());
	}
	{
		Rig rig;
		const std::string archive = MakePackTarGz("lib");
		const RemoteDriver driver = rig.Remote(archive);
		rig.http.Ok(SUM_URL, Sha(archive) + "  x\n", "text/plain");
		rig.http.Truncated(TARBALL_URL, archive, archive.size() / 2);
		const InstallResult result = rig.Install(driver);
		EXPECT_EQ(result.status, InstallStatus::DownloadFailed);
		EXPECT_FALSE(rig.StagingLeftOver());
	}
	{
		Rig rig;
		const std::string archive = MakePackTarGz("lib");
		const RemoteDriver driver = rig.Remote(archive);
		rig.http.Status(SUM_URL, 404);
		EXPECT_EQ(rig.Install(driver).status, InstallStatus::DownloadFailed);
	}
	{
		// The release said N bytes; the server sent a different, well-formed amount.
		Rig rig;
		const std::string archive = MakePackTarGz("lib");
		RemoteDriver driver = rig.Serve(archive);
		driver.size += 10;
		const InstallResult result = rig.Install(driver);
		EXPECT_EQ(result.status, InstallStatus::DownloadFailed);
		EXPECT_NE(result.message.find("incomplete"), std::string::npos) << result.message;
	}
}

TEST(DriverPacksInstall, OnlyHttpsDownloadsAreAccepted)
{
	Rig rig;
	RemoteDriver driver = rig.Serve(MakePackTarGz("lib"));
	driver.download_url = "http://example.com/pack.tar.gz";
	EXPECT_EQ(rig.Install(driver).status, InstallStatus::DownloadFailed);
	EXPECT_TRUE(rig.http.Requests().empty());
}

TEST(DriverPacksInstall, CancellingMidDownloadStopsAndCleansUp)
{
	Rig rig;
	const std::string archive = MakePackTarGz(std::string(200000, 'L'));
	RemoteDriver driver = rig.Remote(archive);
	rig.http.Ok(SUM_URL, Sha(archive) + "  x\n", "text/plain");
	FakeReply reply;
	reply.body = archive;
	reply.chunk_size = 4096;
	rig.http.Reply(TARBALL_URL, reply);

	Catalog::CancelToken cancel;
	const InstallResult result = rig.Install(driver, [&](const InstallProgress& p) {
		if (p.stage == InstallStage::Downloading && p.done > 0)
			cancel.Cancel(); }, cancel);
	EXPECT_EQ(result.status, InstallStatus::Cancelled);
	EXPECT_TRUE(result.message.empty());
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
	EXPECT_FALSE(rig.StagingLeftOver());
}

TEST(DriverPacksInstall, CancellingBeforeAnythingHappensIsClean)
{
	Rig rig;
	const RemoteDriver driver = rig.Serve(MakePackTarGz("lib"));
	Catalog::CancelToken cancel;
	cancel.Cancel();
	const InstallResult result = rig.Install(driver, {}, cancel);
	EXPECT_EQ(result.status, InstallStatus::Cancelled);
	EXPECT_FALSE(rig.StagingLeftOver());
}

TEST(DriverPacksInstall, NotEnoughFreeSpaceIsReportedBeforeDownloading)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	RemoteDriver driver = rig.Serve(archive);
	driver.size = 1ull << 55; // a petabyte
	const InstallResult result = rig.Install(driver);
	EXPECT_EQ(result.status, InstallStatus::NoSpace);
	EXPECT_NE(result.message.find("Not enough free space"), std::string::npos) << result.message;
	EXPECT_EQ(rig.http.RequestCount(driver.download_url), 0u);
}

TEST(DriverPacksInstall, OnlyOneInstallRunsAtATime)
{
	Rig rig;
	const std::string archive = MakePackTarGz("lib");
	const RemoteDriver driver = rig.Serve(archive);

	std::promise<void> inside_check, release_check;
	std::shared_future<void> release = release_check.get_future().share();
	rig.config.library_check = [&](const std::string&, std::string*) {
		inside_check.set_value();
		release.wait();
		return true;
	};
	rig.Rebuild();

	InstallResult first;
	std::thread worker([&] { first = rig.Install(driver); });
	inside_check.get_future().wait(); // the first install is now holding the lock

	RemoteDriver other = driver;
	other.id = "armsx2turnip-other";
	const InstallResult second = rig.Install(other);
	EXPECT_EQ(second.status, InstallStatus::Busy);

	std::string error;
	EXPECT_FALSE(rig.manager->Delete("armsx2turnip-axfl2-002", &error)); // deleting waits for no one
	EXPECT_NE(error.find("being installed"), std::string::npos) << error;

	release_check.set_value();
	worker.join();
	EXPECT_TRUE(first.Ok()) << first.message;
}

TEST(DriverPacksInstall, ABadIdIsRefusedBeforeTouchingDisk)
{
	Rig rig;
	RemoteDriver driver = rig.Serve(MakePackTarGz("lib"));
	driver.id = "../../escape";
	EXPECT_FALSE(rig.Install(driver).Ok());
	EXPECT_TRUE(rig.http.Requests().empty());
	EXPECT_FALSE(fs::exists(rig.config.drivers_dir));
}

// ---- importing, listing, deleting -----------------------------------------------------------------------------

TEST(DriverPacksInstall, AnArchiveFromDiskInstallsWithoutTheNetwork)
{
	Rig rig;
	const std::string archive = MakePackTarGz("local lib");
	const std::string path = rig.dir.File("My Turnip (test).tar.gz");
	ASSERT_TRUE(CatalogTest::WriteBytes(path, archive));

	const InstallResult result = rig.manager->InstallFromFile(path, {}, {});
	ASSERT_TRUE(result.Ok()) << result.message;
	EXPECT_EQ(result.id, "local-my_turnip__test_");
	EXPECT_TRUE(rig.http.Requests().empty());

	const auto installed = rig.manager->FindInstalled(result.id);
	ASSERT_TRUE(installed.has_value());
	EXPECT_EQ(installed->name, "My Turnip (test).tar.gz");
	EXPECT_EQ(installed->version, "imported");
	EXPECT_EQ(installed->archive_sha256, Sha(archive));
	EXPECT_EQ(FileSystem::ReadFileToString(installed->library_path.c_str()).value_or(""), "local lib");
	EXPECT_EQ(Str(ReadJson(installed->manifest_path)["ICD"], "library_path"), installed->library_path);

	EXPECT_EQ(rig.manager->InstallFromFile(rig.dir.File("missing.tar.gz"), {}, {}).status, InstallStatus::IoError);
}

TEST(DriverPacksInstall, ListingIgnoresIncompleteFoldersAndSortsByName)
{
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());

	RemoteDriver other = rig.Serve(MakePackTarGz("other"));
	other.id = "armsx2turnip-aaa";
	other.release_name = "AAA first by name";
	other.tag = "aaa";
	ASSERT_TRUE(rig.Install(other).Ok());

	// Things that are not packs.
	fs::create_directories(Path::Combine(rig.config.drivers_dir, "no-meta"));
	fs::create_directories(Path::Combine(rig.config.drivers_dir, ".install-leftover"));
	fs::create_directories(Path::Combine(rig.config.drivers_dir, "no-library"));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(rig.config.drivers_dir, "no-library/meta.json"), R"({"manifest":"m.json","library":"l.so"})"));
	fs::create_directories(Path::Combine(rig.config.drivers_dir, "escapes"));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(rig.config.drivers_dir, "escapes/meta.json"), R"({"manifest":"../../x.json","library":"../../y.so"})"));
	ASSERT_TRUE(CatalogTest::WriteBytes(Path::Combine(rig.config.drivers_dir, "a-file"), "x"));

	const std::vector<InstalledDriver> installed = rig.manager->ListInstalled();
	ASSERT_EQ(installed.size(), 2u);
	EXPECT_EQ(installed[0].id, "armsx2turnip-aaa");
	EXPECT_EQ(installed[1].id, "armsx2turnip-axfl2-002");
}

TEST(DriverPacksInstall, DeleteRemovesThePackAndOnlyThatPack)
{
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());
	fs::create_directories(Path::Combine(rig.config.drivers_dir, "keep-me"));

	std::string error;
	EXPECT_TRUE(rig.manager->Delete("armsx2turnip-axfl2-002", &error)) << error;
	EXPECT_FALSE(fs::exists(rig.InstalledPath("armsx2turnip-axfl2-002")));
	EXPECT_TRUE(rig.manager->ListInstalled().empty());
	EXPECT_TRUE(fs::exists(Path::Combine(rig.config.drivers_dir, "keep-me")));

	EXPECT_TRUE(rig.manager->Delete("armsx2turnip-axfl2-002", &error)); // already gone is fine
	EXPECT_FALSE(rig.manager->Delete("../keep-me", &error));
	EXPECT_FALSE(rig.manager->Delete("", &error));
	EXPECT_FALSE(rig.manager->Delete(".", &error));
	EXPECT_TRUE(fs::exists(Path::Combine(rig.config.drivers_dir, "keep-me")));
}

// =================================================================================================
// Pointing the process at a pack
// =================================================================================================

TEST(DriverPacksApply, SetsBothVariablesToTheInstalledManifest)
{
	EnvGuard env;
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());
	setenv("VK_ADD_DRIVER_FILES", "/somewhere/else.json", 1);

	const ApplyResult result = ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-axfl2-002", rig.config.sysfs_root);
	EXPECT_EQ(result.outcome, ApplyOutcome::Applied);

	const std::string manifest = rig.InstalledPath("armsx2turnip-axfl2-002", "freedreno_icd.aarch64.json");
	EXPECT_EQ(Env("VK_DRIVER_FILES"), manifest);
	EXPECT_EQ(Env("VK_ICD_FILENAMES"), manifest);
	EXPECT_EQ(Env("VK_ADD_DRIVER_FILES"), "<unset>"); // as the ROCKNIX launcher does
	EXPECT_EQ(result.manifest_path, manifest);
	EXPECT_NE(result.message.find("(git-axfl2-002)"), std::string::npos) << result.message; // what to look for in the GS log
	EXPECT_EQ(GetLastApplyResult().outcome, ApplyOutcome::Applied);
	EXPECT_EQ(GetLastApplyResult().pack_id, "armsx2turnip-axfl2-002");
}

TEST(DriverPacksApply, NothingSelectedLeavesTheEnvironmentAlone)
{
	EnvGuard env;
	setenv("VK_ADD_DRIVER_FILES", "/keep.json", 1);
	const ApplyResult result = ApplyEnvironment("/nonexistent", "", "/nonexistent");
	EXPECT_EQ(result.outcome, ApplyOutcome::SystemDriver);
	EXPECT_EQ(Env("VK_DRIVER_FILES"), "<unset>");
	EXPECT_EQ(Env("VK_ICD_FILENAMES"), "<unset>");
	EXPECT_EQ(Env("VK_ADD_DRIVER_FILES"), "/keep.json");
}

TEST(DriverPacksApply, AUserOrLauncherSettingWins)
{
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());

	for (const char* name : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES"})
	{
		EnvGuard env;
		setenv(name, "/storage/turnip/axfl1-005/freedreno_icd.aarch64.json", 1);
		const ApplyResult result = ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-axfl2-002", rig.config.sysfs_root);
		EXPECT_EQ(result.outcome, ApplyOutcome::EnvironmentAlreadySet) << name;
		EXPECT_EQ(Env(name), "/storage/turnip/axfl1-005/freedreno_icd.aarch64.json") << name;
		// The one that was not set stays unset: the user's choice is not completed for them.
		EXPECT_EQ(Env(std::string(name) == "VK_DRIVER_FILES" ? "VK_ICD_FILENAMES" : "VK_DRIVER_FILES"), "<unset>") << name;
	}
}

TEST(DriverPacksApply, AMissingPackFallsBackToTheSystemDriverAndClearsNothing)
{
	EnvGuard env;
	Rig rig;
	setenv("VK_ADD_DRIVER_FILES", "/keep.json", 1);
	const ApplyResult result = ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-gone", rig.config.sysfs_root);
	EXPECT_EQ(result.outcome, ApplyOutcome::PackMissing);
	EXPECT_NE(result.message.find("armsx2turnip-gone"), std::string::npos);
	EXPECT_EQ(Env("VK_DRIVER_FILES"), "<unset>");
	EXPECT_EQ(Env("VK_ICD_FILENAMES"), "<unset>");
	EXPECT_EQ(Env("VK_ADD_DRIVER_FILES"), "/keep.json");

	// Not a usable name: the same.
	EXPECT_EQ(ApplyEnvironment(rig.config.drivers_dir, "../../etc", rig.config.sysfs_root).outcome, ApplyOutcome::PackMissing);
}

TEST(DriverPacksApply, ADamagedPackCountsAsMissing)
{
	EnvGuard env;
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());
	fs::remove(rig.InstalledPath("armsx2turnip-axfl2-002", "libvulkan_freedreno.so"));
	EXPECT_EQ(ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-axfl2-002", rig.config.sysfs_root).outcome, ApplyOutcome::PackMissing);
	EXPECT_EQ(Env("VK_DRIVER_FILES"), "<unset>");
}

TEST(DriverPacksApply, ASelectedPackIsIgnoredOnAMachineWithoutAnAdrenoGpu)
{
	// VK_DRIVER_FILES replaces the loader's search; a Turnip ICD on a Mali machine would leave
	// no Vulkan device at all. Better to use the system driver.
	EnvGuard env;
	Rig rig(true);
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());
	Rig elsewhere(false);
	const ApplyResult result = ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-axfl2-002", elsewhere.config.sysfs_root);
	EXPECT_EQ(result.outcome, ApplyOutcome::GpuNotSupported);
	EXPECT_EQ(Env("VK_DRIVER_FILES"), "<unset>");
}

TEST(DriverPacksApply, TheZeroArgumentFormReadsTheSettingsAndDataRoot)
{
	EnvGuard env;
	BaseLayer();
	SetSelectedPack({});
	EXPECT_EQ(ApplyEnvironment().outcome, ApplyOutcome::SystemDriver);

	SetSelectedPack("armsx2turnip-not-installed");
	const ApplyResult result = ApplyEnvironment();
	// Not an Adreno machine in the test environment, or installed nowhere: either way no change.
	EXPECT_TRUE(result.outcome == ApplyOutcome::PackMissing || result.outcome == ApplyOutcome::GpuNotSupported) << static_cast<int>(result.outcome);
	EXPECT_EQ(Env("VK_DRIVER_FILES"), "<unset>");
	SetSelectedPack({});
}

// =================================================================================================
// Restarting
// =================================================================================================

namespace
{
	// Runs `body` in a forked child and returns its wait status. The child must _exit or exec.
	template <typename F>
	int InForkedChild(F body)
	{
		std::fflush(nullptr);
		const pid_t pid = fork();
		if (pid == 0)
		{
			body();
			_exit(99);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		return status;
	}

	// Writes env, an argument and whether a leaked descriptor survived into `marker`.
	const char* MARKER_SCRIPT = "exec > \"$1\"; echo \"env=${VK_DRIVER_FILES-unset}\"; echo \"keep=${KEEP_ME-unset}\"; echo \"arg=$3\"; "
								"if [ -e /proc/$$/fd/$2 ]; then echo leaked; else echo closed; fi";

	// /proc/self/cmdline for the given arguments: each one NUL terminated.
	std::string Cmdline(const std::vector<std::string>& args)
	{
		std::string blob;
		for (const std::string& arg : args)
		{
			blob += arg;
			blob.push_back('\0');
		}
		return blob;
	}

	std::string ReadProc(const char* path)
	{
		std::string data;
		if (std::FILE* fp = std::fopen(path, "rb"))
		{
			char buffer[4096];
			size_t n;
			while ((n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0)
				data.append(buffer, n);
			std::fclose(fp);
		}
		return data;
	}

	RelaunchPlan MarkerPlan(const std::string& marker, int leaked_fd)
	{
		RelaunchPlan plan;
		plan.executable = "/bin/sh";
		plan.argv = {"sh", "-c", MARKER_SCRIPT, "sh", marker, std::to_string(leaked_fd), "two words"};
		plan.unset_env = {"VK_DRIVER_FILES"};
		return plan;
	}
} // namespace

TEST(DriverPacksRelaunch, APlanKeepsTheOriginalArguments)
{
	RelaunchPlan plan;
	std::string error;
	const std::string cmdline = Cmdline({"/opt/armsx2/armsx2-qt", "-bigpicture", "", "/games/My Game.iso"});
	ASSERT_TRUE(MakeRelaunchPlan(&plan, cmdline, "/opt/armsx2/armsx2-qt", "", &error)) << error;
	EXPECT_EQ(plan.executable, "/opt/armsx2/armsx2-qt");
	ASSERT_EQ(plan.argv.size(), 4u);
	EXPECT_EQ(plan.argv[0], "/opt/armsx2/armsx2-qt");
	EXPECT_EQ(plan.argv[1], "-bigpicture");
	EXPECT_EQ(plan.argv[2], ""); // an empty argument survives
	EXPECT_EQ(plan.argv[3], "/games/My Game.iso");
}

TEST(DriverPacksRelaunch, AnAppImageIsRestartedThroughItsImageFile)
{
	ScopedTempDir dir;
	const std::string image = dir.File("ARMSX2-nightly-aarch64.AppImage");
	ASSERT_TRUE(CatalogTest::WriteBytes(image, "image"));

	RelaunchPlan plan;
	std::string error;
	// Inside an AppImage /proc/self/exe is on the temporary mount, which disappears with us.
	const std::string cmdline = Cmdline({"/tmp/.mount_ARMSX2abc/usr/bin/armsx2-qt", "-fullscreen"});
	ASSERT_TRUE(MakeRelaunchPlan(&plan, cmdline, "/tmp/.mount_ARMSX2abc/usr/bin/armsx2-qt", image, &error)) << error;
	EXPECT_EQ(plan.executable, image);
	ASSERT_EQ(plan.argv.size(), 2u);
	EXPECT_EQ(plan.argv[0], image);
	EXPECT_EQ(plan.argv[1], "-fullscreen");

	// An APPIMAGE variable that points at nothing is not trusted.
	ASSERT_TRUE(MakeRelaunchPlan(&plan, cmdline, "/tmp/.mount_ARMSX2abc/usr/bin/armsx2-qt", dir.File("gone.AppImage"), &error)) << error;
	EXPECT_EQ(plan.executable, "/tmp/.mount_ARMSX2abc/usr/bin/armsx2-qt");
}

TEST(DriverPacksRelaunch, ABinaryReplacedByAnUpdateIsStartedFromItsPath)
{
	ScopedTempDir dir;
	const std::string exe = dir.File("armsx2-qt");
	ASSERT_TRUE(CatalogTest::WriteBytes(exe, "new version"));

	RelaunchPlan plan;
	std::string error;
	ASSERT_TRUE(MakeRelaunchPlan(&plan, Cmdline({"armsx2-qt"}), exe + " (deleted)", "", &error)) << error;
	EXPECT_EQ(plan.executable, exe);

	// Deleted and not replaced: the kernel can still exec the old image through /proc.
	ASSERT_TRUE(MakeRelaunchPlan(&plan, Cmdline({"armsx2-qt"}), dir.File("other") + " (deleted)", "", &error)) << error;
	EXPECT_EQ(plan.executable, "/proc/self/exe");
}

TEST(DriverPacksRelaunch, NothingToRestartIsAnError)
{
	RelaunchPlan plan;
	std::string error;
	EXPECT_FALSE(MakeRelaunchPlan(&plan, "", "/x", "", &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(MakeRelaunchPlan(&plan, Cmdline({"a"}), "", "", &error));
}

TEST(DriverPacksRelaunch, ThePlanForThisProcessMatchesItsOwnCommandLine)
{
	RelaunchPlan plan;
	std::string error;
	ASSERT_TRUE(BuildRelaunchPlan(&plan, &error)) << error;

	const std::string cmdline = ReadProc("/proc/self/cmdline");
	ASSERT_FALSE(cmdline.empty());
	EXPECT_EQ(plan.argv[0], cmdline.substr(0, cmdline.find('\0')));
	EXPECT_FALSE(plan.executable.empty());
	std::error_code ec;
	EXPECT_TRUE(fs::exists(plan.executable, ec)) << plan.executable;
	EXPECT_TRUE(plan.unset_env.empty()); // nothing was applied by this process
}

TEST(DriverPacksRelaunch, TheRestartedProcessDoesNotInheritWhatWeApplied)
{
	EnvGuard env;
	Rig rig;
	ASSERT_TRUE(rig.Install(rig.Serve(MakePackTarGz("lib"))).Ok());
	ASSERT_EQ(ApplyEnvironment(rig.config.drivers_dir, "armsx2turnip-axfl2-002", rig.config.sysfs_root).outcome, ApplyOutcome::Applied);

	RelaunchPlan plan;
	std::string error;
	ASSERT_TRUE(BuildRelaunchPlan(&plan, &error)) << error;
	EXPECT_NE(std::find(plan.unset_env.begin(), plan.unset_env.end(), "VK_DRIVER_FILES"), plan.unset_env.end());
	EXPECT_NE(std::find(plan.unset_env.begin(), plan.unset_env.end(), "VK_ICD_FILENAMES"), plan.unset_env.end());

	// If something changed one since, it is no longer ours to remove.
	setenv("VK_ICD_FILENAMES", "/user/set/this.json", 1);
	ASSERT_TRUE(BuildRelaunchPlan(&plan, &error)) << error;
	EXPECT_NE(std::find(plan.unset_env.begin(), plan.unset_env.end(), "VK_DRIVER_FILES"), plan.unset_env.end());
	EXPECT_EQ(std::find(plan.unset_env.begin(), plan.unset_env.end(), "VK_ICD_FILENAMES"), plan.unset_env.end());

	// And a launcher's own variables are never ours.
	ApplyEnvironment(rig.config.drivers_dir, "", rig.config.sysfs_root);
	ASSERT_TRUE(BuildRelaunchPlan(&plan, &error)) << error;
	EXPECT_TRUE(plan.unset_env.empty());
}

TEST(DriverPacksRelaunch, ExecReplacesTheProcessDropsOurVariablesAndClosesLeakedDescriptors)
{
	ScopedTempDir dir;
	const std::string marker = dir.File("marker.txt");
	const int leaked = open("/dev/null", O_RDONLY); // no O_CLOEXEC on purpose
	ASSERT_GE(leaked, 3);

	const RelaunchPlan plan = MarkerPlan(marker, leaked);
	const int status = InForkedChild([&] {
		setenv("VK_DRIVER_FILES", "/pack/icd.json", 1);
		setenv("KEEP_ME", "yes", 1);
		std::string error;
		ExecRelaunchPlan(plan, &error); // returns only on failure
	});
	close(leaked);

	ASSERT_TRUE(WIFEXITED(status)) << status;
	ASSERT_EQ(WEXITSTATUS(status), 0) << "the exec'd shell should have exited 0";
	const std::string text = FileSystem::ReadFileToString(marker.c_str()).value_or("<no marker>");
	EXPECT_EQ(text, "env=unset\nkeep=yes\narg=two words\nclosed\n");
}

TEST(DriverPacksRelaunch, AFailedExecReturnsAndLeavesDescriptorsOpen)
{
	ScopedTempDir dir;
	const int fd = open("/dev/null", O_RDONLY);
	RelaunchPlan plan;
	plan.executable = dir.File("no-such-program");
	plan.argv = {"no-such-program"};
	std::string error;
	EXPECT_FALSE(ExecRelaunchPlan(plan, &error));
	EXPECT_NE(error.find("no-such-program"), std::string::npos) << error;
	// Close-on-exec was set, not close(): the descriptor still works in this process.
	char c;
	EXPECT_EQ(read(fd, &c, 1), 0);
	close(fd);
}

TEST(DriverPacksRelaunch, RestartAtExitRunsWhenTheProcessQuits)
{
	ScopedTempDir dir;
	const std::string marker = dir.File("marker.txt");
	const int leaked = open("/dev/null", O_RDONLY);
	const RelaunchPlan plan = MarkerPlan(marker, leaked);

	const int status = InForkedChild([&] {
		setenv("VK_DRIVER_FILES", "/pack/icd.json", 1);
		std::string error;
		if (!RelaunchAtExit(plan, &error))
			_exit(98);
		std::exit(7); // the app's normal quit; the exit handler replaces the process
	});
	close(leaked);

	ASSERT_TRUE(WIFEXITED(status)) << status;
	EXPECT_EQ(WEXITSTATUS(status), 0); // the new process's status, not 7
	EXPECT_EQ(FileSystem::ReadFileToString(marker.c_str()).value_or("<no marker>"), "env=unset\nkeep=unset\narg=two words\nclosed\n");
}

TEST(DriverPacksRelaunch, WithoutARequestTheExitHandlerDoesNothing)
{
	// RelaunchAtExit is never called here: a process that registered nothing exits normally.
	const int status = InForkedChild([] { std::exit(5); });
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 5);
}

#else // not desktop Linux

TEST(DriverPacksPlatform, ReportsNotAvailable)
{
	EXPECT_EQ(CurrentPlatform(), "");
	EXPECT_EQ(GetAvailability().reason, Availability::Reason::UnsupportedPlatform);
	std::string error;
	EXPECT_FALSE(TestLoadLibrary("/nonexistent", &error));
	EXPECT_EQ(ApplyEnvironment("/x", "pack").outcome, ApplyOutcome::Unsupported);
}

#endif
