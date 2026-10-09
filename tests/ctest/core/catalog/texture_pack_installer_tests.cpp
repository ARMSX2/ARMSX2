// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Installing a catalog pack end to end against the fake fetcher: streaming and verifying the
// download (single file and split), the free-space rule, extraction into staging, the commit with
// its rollback, cancellation, the one-install-at-a-time rule, and listing and removing what is
// installed. The archives are built in the tests; nothing touches the network.

#include "FakeFetcher.h"
#include "texture_pack_test_support.h"

#include "Catalog/Json.h"
#include "Catalog/TextureArchivePath.h"
#include "Catalog/TexturePackInstaller.h"

#include "fmt/format.h"

#include <thread>

using namespace Catalog;
using namespace CatalogTest;
using Phase = TexturePackInstaller::Phase;
using Status = TexturePackInstaller::Status;
using Progress = TexturePackInstaller::Progress;

namespace
{
	using Files = std::vector<std::pair<std::string, std::string>>;

	struct Archive
	{
		std::string bytes; // what the server sends
		TexturePack pack;
	};

	// A tar+zstd pack of the given files, described exactly in a catalog entry.
	Archive BuildTarPack(std::string id, std::string serial, const Files& files)
	{
		TarBuilder b;
		b.Dir("./").Dir("replacements/");
		u32 textures = 0;
		for (const auto& [name, data] : files)
		{
			b.File("replacements/" + name, data);
			if (TextureArchivePath::IsTextureFile(name) && !TextureArchivePath::IsJunkEntry(name))
				textures++;
		}
		const std::string tar = b.Finish();
		Archive a;
		a.bytes = ZstdCompress(tar);
		a.pack = DescribeArchive(std::move(id), std::move(serial), a.bytes, TexturePackFormat::TarZstd, tar.size(), textures);
		return a;
	}

	Files NumberedTextures(int count, const std::string& tag = "")
	{
		Files files;
		for (int i = 0; i < count; i++)
			files.emplace_back(fmt::format("{:016x}-{:08x}.png", i + 1, 0x100 + i), "texture " + tag + std::to_string(i));
		return files;
	}

	std::string ReadAll(const std::string& path)
	{
		return FileSystem::ReadFileToString(path.c_str()).value_or("<missing>");
	}

	// The phases in the order they were first reported.
	std::vector<Phase> PhaseOrder(const std::vector<Progress>& seen)
	{
		std::vector<Phase> order;
		for (const Progress& p : seen)
		{
			if (order.empty() || order.back() != p.phase)
				order.push_back(p.phase);
		}
		return order;
	}

	// Folders under `dir` whose name starts with `prefix`.
	std::vector<std::string> FoldersStartingWith(const std::string& dir, const std::string& prefix)
	{
		std::vector<std::string> found;
		std::error_code ec;
		for (const auto& item : std::filesystem::directory_iterator(dir, ec))
		{
			const std::string name = item.path().filename().string();
			if (name.rfind(prefix, 0) == 0)
				found.push_back(name);
		}
		std::sort(found.begin(), found.end());
		return found;
	}

	// A symbolic link at `link` to the folder `to`; false when the platform will not make one.
	bool MakeFolderLink(const std::string& to, const std::string& link)
	{
		std::error_code ec;
		std::filesystem::create_directory_symlink(to, link, ec);
		return !ec;
	}
} // namespace

class TexturePackInstallerTest : public ::testing::Test
{
protected:
	TexturePackInstallerTest()
		: state(temp.File("texture-packs.json"))
		, installer(http, state, MakeConfig())
	{
	}

	TexturePackInstaller::Config MakeConfig()
	{
		TexturePackInstaller::Config config;
		config.textures_dir = temp.File("textures");
		config.size_cache_path = temp.File("cache/pack-sizes.json");
		config.query_free_space = [this](const std::string&) { return free_space; };
		return config;
	}

	void Serve(const Archive& a) { http.Ok(a.pack.download_url, a.bytes); }

	std::string TexturePath(std::string_view relative) const { return temp.File("textures/" + std::string(relative)); }

	TexturePackInstaller::InstallResult Install(const TexturePack& pack, std::string_view serial)
	{
		return installer.Install(pack, serial, [this](const Progress& p) { seen.push_back(p); });
	}

	ScopedTempDir temp;
	FakeFetcher http;
	TexturePackState state;
	std::optional<u64> free_space = u64(1) << 50;
	TexturePackInstaller installer;
	std::vector<Progress> seen;
};

// ------------------------------------------------------------------------------------------
// A good install
// ------------------------------------------------------------------------------------------

TEST_F(TexturePackInstallerTest, InstallsATarZstdPackWhereTheEngineLooks)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	Serve(a);

	const auto result = Install(a.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(result.textures, 3u);

	// <Textures>/<SERIAL>/replacements/<files>, which is where GSTextureReplacements scans.
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 3u);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/replacements/0000000000000001-00000100.png")), "texture 0");

	// The record says what was installed, and staging is gone.
	const auto record = state.Find("pack-a");
	ASSERT_TRUE(record.has_value());
	EXPECT_EQ(record->serial, "SLUS-21410");
	EXPECT_EQ(record->format, "tar+zstd");
	EXPECT_EQ(record->archive_revision, 1u);
	EXPECT_EQ(record->sha256, a.pack.sha256);
	EXPECT_EQ(result.record.pack_id, "pack-a");
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
	EXPECT_FALSE(TexturePackInstaller::IsInstalling());
	EXPECT_EQ(http.RequestCount(a.pack.download_url), 1u);
}

TEST_F(TexturePackInstallerTest, ReportsTheStagesInOrder)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	Serve(a);
	ASSERT_TRUE(Install(a.pack, "SLUS-21410").IsOk());

	EXPECT_EQ(PhaseOrder(seen), (std::vector<Phase>{Phase::Starting, Phase::Downloading, Phase::Verifying, Phase::Extracting, Phase::Installing}));

	// Downloading ends at the whole archive; extracting at the whole pack.
	const Progress* last_download = nullptr;
	const Progress* last_extract = nullptr;
	for (const Progress& p : seen)
	{
		if (p.phase == Phase::Downloading)
			last_download = &p;
		if (p.phase == Phase::Extracting)
			last_extract = &p;
	}
	ASSERT_NE(last_download, nullptr);
	EXPECT_EQ(last_download->done, a.pack.size_bytes);
	EXPECT_EQ(last_download->total, a.pack.size_bytes);
	ASSERT_NE(last_extract, nullptr);
	EXPECT_EQ(last_extract->done, 3u);
	EXPECT_EQ(last_extract->total, 3u);
}

TEST_F(TexturePackInstallerTest, TheSerialMayBeSpelledAnyWayAndMustBeThePacks)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(1));
	a.pack.serials.push_back("SLES-52187");
	Serve(a);

	ASSERT_TRUE(Install(a.pack, "sles 52187").IsOk());
	EXPECT_TRUE(FileSystem::DirectoryExists(TexturePath("SLES-52187/replacements").c_str()));
	EXPECT_EQ(state.Find("pack-a")->serial, "SLES-52187");

	for (const char* wrong : {"SCUS-97328", "../SLUS-21410", "SLUS-21410/../..", "", "PERSONA 3 FES", "/etc"})
	{
		const auto result = Install(a.pack, wrong);
		EXPECT_EQ(result.status, Status::Failed) << wrong;
	}
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SCUS-97328").c_str()));
	EXPECT_EQ(http.RequestCount(a.pack.download_url), 1u); // only the first, good one went to the network
}

TEST_F(TexturePackInstallerTest, InstallsAPackSplitIntoParts)
{
	Archive a = BuildTarPack("split", "SLUS-21410", NumberedTextures(40));
	// Cut the archive in three and describe the pieces.
	const size_t third = a.bytes.size() / 3;
	const std::string pieces[3] = {a.bytes.substr(0, third), a.bytes.substr(third, third), a.bytes.substr(2 * third)};
	a.pack.download_url.clear();
	for (int i = 0; i < 3; i++)
	{
		TexturePackPart part;
		part.download_url = fmt::format("https://dl.example.com/split.part{}", i + 1);
		part.size_bytes = pieces[i].size();
		part.sha256 = SHA256Digest::HashHex(pieces[i]);
		a.pack.parts.push_back(part);

		// A small chunk size gives progress reports between the parts.
		FakeReply reply;
		reply.body = pieces[i];
		reply.chunk_size = 16;
		http.Reply(part.download_url, reply);
	}

	const auto result = Install(a.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 40u);

	// One bar across the parts: it never goes back, and it ends at the whole archive.
	u64 previous = 0;
	for (const Progress& p : seen)
	{
		if (p.phase != Phase::Downloading)
			continue;
		EXPECT_GE(p.done, previous);
		EXPECT_EQ(p.total, a.pack.size_bytes);
		previous = p.done;
	}
	EXPECT_EQ(previous, a.pack.size_bytes);
}

TEST_F(TexturePackInstallerTest, ADamagedPartIsNamedAndTheRestAreNotFetched)
{
	Archive a = BuildTarPack("split", "SLUS-21410", NumberedTextures(40));
	const size_t third = a.bytes.size() / 3;
	std::string pieces[3] = {a.bytes.substr(0, third), a.bytes.substr(third, third), a.bytes.substr(2 * third)};
	a.pack.download_url.clear();
	for (int i = 0; i < 3; i++)
	{
		TexturePackPart part;
		part.download_url = fmt::format("https://dl.example.com/split.part{}", i + 1);
		part.size_bytes = pieces[i].size();
		part.sha256 = SHA256Digest::HashHex(pieces[i]);
		a.pack.parts.push_back(part);
	}
	pieces[1][5] ^= 0x55; // same length, wrong bytes
	for (int i = 0; i < 3; i++)
		http.Ok(a.pack.parts[i].download_url, pieces[i]);

	const auto result = Install(a.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_NE(result.error.find("Checksum mismatch (part 2 of 3)"), std::string::npos) << result.error;
	EXPECT_EQ(http.RequestCount(a.pack.parts[2].download_url), 0u);
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str()));
	EXPECT_FALSE(state.Find("split").has_value());
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
}

TEST_F(TexturePackInstallerTest, APartOfTheWrongLengthIsRefused)
{
	for (const int delta : {-1, +1})
	{
		Archive a = BuildTarPack("split", "SLUS-21410", NumberedTextures(5));
		const size_t half = a.bytes.size() / 2;
		const std::string pieces[2] = {a.bytes.substr(0, half), a.bytes.substr(half)};
		a.pack.download_url.clear();
		for (int i = 0; i < 2; i++)
		{
			TexturePackPart part;
			part.download_url = fmt::format("https://dl.example.com/split{}.part{}", delta, i + 1);
			part.size_bytes = pieces[i].size() + (i == 0 ? delta : 0); // the catalog is off by one for part 1
			part.sha256 = SHA256Digest::HashHex(pieces[i]);
			a.pack.parts.push_back(part);
			http.Ok(part.download_url, pieces[i]);
		}
		a.pack.size_bytes = a.pack.parts[0].size_bytes + a.pack.parts[1].size_bytes;

		const auto result = Install(a.pack, "SLUS-21410");
		EXPECT_EQ(result.status, Status::Failed) << delta;
		EXPECT_NE(result.error.find("Size mismatch (part 1 of 2)"), std::string::npos) << delta << ": " << result.error;
	}
}

TEST_F(TexturePackInstallerTest, TheWholeArchiveIsCheckedEvenWhenEveryPartPassed)
{
	Archive a = BuildTarPack("split", "SLUS-21410", NumberedTextures(5));
	const size_t half = a.bytes.size() / 2;
	const std::string pieces[2] = {a.bytes.substr(0, half), a.bytes.substr(half)};
	a.pack.download_url.clear();
	for (int i = 0; i < 2; i++)
	{
		TexturePackPart part;
		part.download_url = fmt::format("https://dl.example.com/whole.part{}", i + 1);
		part.size_bytes = pieces[i].size();
		part.sha256 = SHA256Digest::HashHex(pieces[i]);
		a.pack.parts.push_back(part);
		http.Ok(part.download_url, pieces[i]);
	}
	a.pack.sha256 = SHA256Digest::HashHex("some other archive"); // the pieces are right, the whole is not

	const auto result = Install(a.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_EQ(result.error, "Checksum mismatch: the download was corrupted");
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str()));
}

// ------------------------------------------------------------------------------------------
// A download that goes wrong
// ------------------------------------------------------------------------------------------

TEST_F(TexturePackInstallerTest, ACorruptSingleFileDownloadIsRefused)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	std::string corrupt = a.bytes;
	corrupt[corrupt.size() / 2] ^= 0xFF;
	http.Ok(a.pack.download_url, corrupt);

	const auto result = Install(a.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_EQ(result.error, "Checksum mismatch: the download was corrupted");
	EXPECT_TRUE(ListFiles(temp.File("textures")).empty());
}

TEST_F(TexturePackInstallerTest, ADownloadOfTheWrongLengthIsRefused)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));

	// Shorter than the catalog says (a server that cut it short but closed cleanly).
	{
		FakeFetcher short_http;
		FakeReply reply;
		reply.body = a.bytes.substr(0, a.bytes.size() - 10);
		reply.content_length = -1; // not announced, so the connection looks complete
		short_http.Reply(a.pack.download_url, reply);
		TexturePackInstaller short_installer(short_http, state, MakeConfig());
		const auto result = short_installer.Install(a.pack, "SLUS-21410");
		EXPECT_EQ(result.status, Status::Failed);
		EXPECT_NE(result.error.find("Size mismatch"), std::string::npos) << result.error;
	}
	// Longer: stopped at the declared size rather than filling the disk.
	{
		FakeFetcher long_http;
		FakeReply reply;
		reply.body = a.bytes + std::string(1000, 'x');
		reply.content_length = -1;
		reply.chunk_size = 64;
		long_http.Reply(a.pack.download_url, reply);
		TexturePackInstaller long_installer(long_http, state, MakeConfig());
		const auto result = long_installer.Install(a.pack, "SLUS-21410");
		EXPECT_EQ(result.status, Status::Failed);
		EXPECT_NE(result.error.find("Size mismatch"), std::string::npos) << result.error;
	}
	EXPECT_TRUE(ListFiles(temp.File("textures")).empty());
}

TEST_F(TexturePackInstallerTest, FailedRequestsAreReportedPlainly)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(1));
	const std::string& url = a.pack.download_url;

	http.Status(url, 404, "gone");
	EXPECT_EQ(Install(a.pack, "SLUS-21410").error, "Download failed");

	FakeFetcher http2;
	http2.Fail(url, "connection reset");
	EXPECT_EQ(TexturePackInstaller(http2, state, MakeConfig()).Install(a.pack, "SLUS-21410").error, "Download failed");

	FakeFetcher http3;
	http3.Redirect(url, "http://insecure.example/pack");
	EXPECT_EQ(TexturePackInstaller(http3, state, MakeConfig()).Install(a.pack, "SLUS-21410").error, "Download failed");

	FakeFetcher http4;
	http4.Truncated(url, a.bytes, 10);
	EXPECT_EQ(TexturePackInstaller(http4, state, MakeConfig()).Install(a.pack, "SLUS-21410").error, "Download failed");

	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
	EXPECT_TRUE(state.All().empty());
}

TEST_F(TexturePackInstallerTest, AnArchiveThatIsNotWhatTheCatalogSaidFailsAtExtraction)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	a.pack.file_count = 4; // the catalog claims a texture that the archive does not hold
	Serve(a);

	const auto result = Install(a.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_NE(result.error.find("texture count mismatch"), std::string::npos) << result.error;
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
}

// ------------------------------------------------------------------------------------------
// Free space
// ------------------------------------------------------------------------------------------

TEST(TexturePackInstallerSpace, TheRequirementDependsOnTheFormat)
{
	constexpr u64 slack = 512ull * 1024 * 1024;
	static_assert(TexturePackInstaller::FREE_SPACE_SLACK == slack);

	TexturePack tar;
	tar.format = TexturePackFormat::TarZstd;
	tar.size_bytes = 3'000'000'000;
	tar.decompressed_size_bytes = 4'100'000'000;
	EXPECT_EQ(TexturePackInstaller::RequiredFreeSpace(tar), 3'000'000'000 + 4'100'000'000 + slack);

	TexturePack zip;
	zip.format = TexturePackFormat::Zip;
	zip.size_bytes = 3'000'000'000;
	EXPECT_EQ(TexturePackInstaller::RequiredFreeSpace(zip), 2 * 3'000'000'000 + slack);
}

TEST_F(TexturePackInstallerTest, RefusesToStartWithoutRoomForTheWholeInstall)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	Serve(a);
	const u64 required = TexturePackInstaller::RequiredFreeSpace(a.pack);

	free_space = required - 1;
	auto result = Install(a.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_EQ(result.error, fmt::format("Needs ~{} MB free, {} MB available", required / 1024 / 1024, (required - 1) / 1024 / 1024));
	EXPECT_TRUE(http.Requests().empty()) << "nothing may be downloaded first";
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));

	// Exactly enough is enough.
	free_space = required;
	EXPECT_TRUE(Install(a.pack, "SLUS-21410").IsOk());
}

TEST_F(TexturePackInstallerTest, AFilesystemThatCannotSayHowMuchIsFreeIsTried)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(1));
	Serve(a);
	free_space = 0;
	EXPECT_TRUE(Install(a.pack, "SLUS-21410").IsOk());

	Archive b = BuildTarPack("pack-b", "SLES-52187", NumberedTextures(1));
	Serve(b);
	free_space = std::nullopt;
	EXPECT_TRUE(Install(b.pack, "SLES-52187").IsOk());
}

TEST_F(TexturePackInstallerTest, ZipsNeedTwiceTheArchive)
{
	ASSERT_TRUE(WriteZip(temp.File("src.zip"), {{"SLUS-21410/replacements/0000000000000001-00000100.png", "one"}}));
	Archive z;
	z.bytes = ReadAll(temp.File("src.zip"));
	z.pack = DescribeArchive("zip-pack", "SLUS-21410", z.bytes, TexturePackFormat::Zip);
	Serve(z);

	free_space = 2 * z.pack.size_bytes + TexturePackInstaller::FREE_SPACE_SLACK - 1;
	EXPECT_EQ(Install(z.pack, "SLUS-21410").status, Status::Failed);
	free_space = 2 * z.pack.size_bytes + TexturePackInstaller::FREE_SPACE_SLACK;
	EXPECT_TRUE(Install(z.pack, "SLUS-21410").IsOk());
}

// ------------------------------------------------------------------------------------------
// Zip packs
// ------------------------------------------------------------------------------------------

TEST_F(TexturePackInstallerTest, InstallsASchemaOneZipPack)
{
	ASSERT_TRUE(WriteZip(temp.File("src.zip"),
		{
			{"SLUS-21410/replacements/0000000000000001-00000100.png", "one"},
			{"SLUS-21410/replacements/sub/0000000000000002-00000200.dds", "two"},
			{"SLUS-21410/replacements/notes.txt", "skipped"},
		}));
	Archive z;
	z.bytes = ReadAll(temp.File("src.zip"));
	z.pack = DescribeArchive("zip-pack", "SLUS-21410", z.bytes, TexturePackFormat::Zip);
	Serve(z);

	const auto result = Install(z.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")),
		(std::set<std::string>{"0000000000000001-00000100.png", "sub/0000000000000002-00000200.dds"}));
	const auto record = state.Find("zip-pack");
	ASSERT_TRUE(record.has_value());
	EXPECT_EQ(record->format, "zip");
	EXPECT_EQ(record->schema, 1);
}

TEST_F(TexturePackInstallerTest, AZipWithNoTexturesIsAnError)
{
	ASSERT_TRUE(WriteZip(temp.File("src.zip"), {{"readme.txt", "no textures here"}}));
	Archive z;
	z.bytes = ReadAll(temp.File("src.zip"));
	z.pack = DescribeArchive("zip-pack", "SLUS-21410", z.bytes, TexturePackFormat::Zip);
	Serve(z);

	const auto result = Install(z.pack, "SLUS-21410");
	EXPECT_EQ(result.status, Status::Failed);
	EXPECT_EQ(result.error, "Archive contained no textures");
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str()));
}

// ------------------------------------------------------------------------------------------
// Replacing, rolling back, cancelling, the one-at-a-time rule
// ------------------------------------------------------------------------------------------

TEST_F(TexturePackInstallerTest, AnInstallReplacesThePreviousPackAndItsRecord)
{
	Archive first = BuildTarPack("first", "SLUS-21410", NumberedTextures(3, "first"));
	Archive second = BuildTarPack("second", "SLUS-21410", NumberedTextures(2, "second"));
	Serve(first);
	Serve(second);

	ASSERT_TRUE(Install(first.pack, "SLUS-21410").IsOk());
	ASSERT_TRUE(Install(second.pack, "SLUS-21410").IsOk());

	// The folder holds the second pack alone, not a blend of both.
	const auto files = ListFiles(TexturePath("SLUS-21410/replacements"));
	EXPECT_EQ(files.size(), 2u);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/replacements/0000000000000001-00000100.png")), "texture second0");
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410/replacements.old").c_str()));

	// And the record follows: the first pack is no longer "installed" for this game.
	EXPECT_FALSE(state.Find("first").has_value());
	ASSERT_TRUE(state.Find("second").has_value());
	EXPECT_EQ(state.All().size(), 1u);
}

TEST_F(TexturePackInstallerTest, AFailedInstallLeavesThePreviousOneAlone)
{
	Archive good = BuildTarPack("good", "SLUS-21410", NumberedTextures(3, "good"));
	Serve(good);
	ASSERT_TRUE(Install(good.pack, "SLUS-21410").IsOk());

	Archive bad = BuildTarPack("bad", "SLUS-21410", NumberedTextures(2, "bad"));
	bad.pack.file_count = 9;
	Serve(bad);
	EXPECT_EQ(Install(bad.pack, "SLUS-21410").status, Status::Failed);

	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 3u);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/replacements/0000000000000001-00000100.png")), "texture good0");
	EXPECT_TRUE(state.Find("good").has_value());
	EXPECT_FALSE(state.Find("bad").has_value());
}

TEST(TexturePackInstallerCommit, SwapsInTheNewFolderAndDropsTheOldOne)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("staged").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("staged/new.png").c_str(), "new"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("game/replacements").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("game/replacements/old.png").c_str(), "old"));
	// A leftover from an interrupted earlier commit.
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("game/replacements.old").c_str(), true));

	ASSERT_TRUE(TexturePackInstaller::CommitDirectory(temp.File("staged"), temp.File("game/replacements")));
	EXPECT_EQ(ListFiles(temp.File("game")), std::set<std::string>{"replacements/new.png"});
	EXPECT_FALSE(FileSystem::DirectoryExists(temp.File("game/replacements.old").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(temp.File("staged").c_str()));
}

TEST(TexturePackInstallerCommit, PutsTheOldFolderBackWhenTheSwapFails)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("game/replacements").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("game/replacements/old.png").c_str(), "old"));

	// The staged folder is missing, so the second rename fails after the first has happened.
	EXPECT_FALSE(TexturePackInstaller::CommitDirectory(temp.File("no-such-staging"), temp.File("game/replacements")));
	EXPECT_EQ(ListFiles(temp.File("game")), std::set<std::string>{"replacements/old.png"});
	EXPECT_FALSE(FileSystem::DirectoryExists(temp.File("game/replacements.old").c_str()));
}

TEST(TexturePackInstallerCommit, CreatesTheGamesFolderForAFirstInstall)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("staged").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("staged/new.png").c_str(), "new"));
	ASSERT_TRUE(TexturePackInstaller::CommitDirectory(temp.File("staged"), temp.File("textures/SLUS-21410/replacements")));
	EXPECT_EQ(ListFiles(temp.File("textures")), std::set<std::string>{"SLUS-21410/replacements/new.png"});
}

TEST_F(TexturePackInstallerTest, CancellingWhileDownloadingStopsAndCleansUp)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(60));
	FakeReply reply;
	reply.body = a.bytes;
	reply.chunk_size = 20;
	http.Reply(a.pack.download_url, reply);
	ASSERT_GT(a.bytes.size(), 100u);

	CancelToken cancel;
	const auto result = installer.Install(a.pack, "SLUS-21410", [&](const Progress& p) {
		if (p.phase == Phase::Downloading)
			cancel.Cancel();
	}, cancel);

	EXPECT_EQ(result.status, Status::Cancelled);
	EXPECT_TRUE(result.error.empty());
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
	EXPECT_TRUE(state.All().empty());
	EXPECT_FALSE(TexturePackInstaller::IsInstalling());
}

TEST_F(TexturePackInstallerTest, CancellingBeforeTheStartDoesNothing)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	Serve(a);
	CancelToken cancel;
	cancel.Cancel();
	EXPECT_EQ(installer.Install(a.pack, "SLUS-21410", {}, cancel).status, Status::Cancelled);
	EXPECT_TRUE(http.Requests().empty());
}

TEST_F(TexturePackInstallerTest, CancellingWhileExtractingKeepsThePreviousPack)
{
	Archive old_pack = BuildTarPack("old", "SLUS-21410", NumberedTextures(2, "old"));
	Serve(old_pack);
	ASSERT_TRUE(Install(old_pack.pack, "SLUS-21410").IsOk());

	Archive big = BuildTarPack("big", "SLUS-21410", NumberedTextures(70, "big"));
	Serve(big);
	CancelToken cancel;
	const auto result = installer.Install(big.pack, "SLUS-21410", [&](const Progress& p) {
		if (p.phase == Phase::Extracting)
			cancel.Cancel();
	}, cancel);

	EXPECT_EQ(result.status, Status::Cancelled);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/replacements/0000000000000001-00000100.png")), "texture old0");
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 2u);
	EXPECT_TRUE(state.Find("old").has_value());
	EXPECT_FALSE(state.Find("big").has_value());
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".texturepacks-tmp").c_str()));
}

TEST_F(TexturePackInstallerTest, OnlyOneInstallRunsAtATime)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(3));
	Archive b = BuildTarPack("pack-b", "SLES-52187", NumberedTextures(3));
	Serve(a);
	Serve(b);

	Status nested = Status::Ok;
	bool installing_inside = false;
	const auto result = installer.Install(a.pack, "SLUS-21410", [&](const Progress& p) {
		if (p.phase != Phase::Extracting)
			return;
		installing_inside = TexturePackInstaller::IsInstalling();
		// A second request while the first is mid-flight, from another thread and another
		// installer object.
		std::thread other_thread([&]() {
			TexturePackInstaller other(http, state, MakeConfig());
			nested = other.Install(b.pack, "SLES-52187").status;
		});
		other_thread.join();
	});

	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_TRUE(installing_inside);
	EXPECT_EQ(nested, Status::Busy);
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLES-52187").c_str()));
	EXPECT_EQ(http.RequestCount(b.pack.download_url), 0u);
	EXPECT_FALSE(TexturePackInstaller::IsInstalling());

	// Once the first is done the second goes through.
	EXPECT_TRUE(Install(b.pack, "SLES-52187").IsOk());
}

// ------------------------------------------------------------------------------------------
// Installed packs: listing, removing
// ------------------------------------------------------------------------------------------

class InstalledPacksTest : public TexturePackInstallerTest
{
protected:
	void MakeFolder(const std::string& serial, int textures, bool with_dumps = false)
	{
		for (int i = 0; i < textures; i++)
			ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(serial + "/replacements/sub").c_str(), true) &&
						FileSystem::WriteStringToFile(TexturePath(fmt::format("{}/replacements/sub/{:016x}-00000001.png", serial, i)).c_str(), std::string(10, 'x')));
		if (with_dumps)
		{
			ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(serial + "/dumps").c_str(), true));
			ASSERT_TRUE(FileSystem::WriteStringToFile(TexturePath(serial + "/dumps/dump.png").c_str(), "dump"));
		}
	}
};

TEST_F(InstalledPacksTest, ListsWhatIsInstalledWithSizesAndRecords)
{
	MakeFolder("SLUS-21410", 3);
	MakeFolder("SLES-52187", 1);
	MakeFolder("PERSONA 3 FES", 2); // a hand-copied folder for an ELF boot
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath("SCUS-97328/dumps").c_str(), true)); // dumps only: not a pack
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(".hidden/replacements").c_str(), true));
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("pack-a", "SLUS-21410", "x", TexturePackFormat::TarZstd, 1), "SLUS-21410")));

	const auto packs = installer.ListInstalled();
	ASSERT_EQ(packs.size(), 3u);
	EXPECT_EQ(packs[0].serial, "PERSONA 3 FES");
	EXPECT_EQ(packs[1].serial, "SLES-52187");
	EXPECT_EQ(packs[2].serial, "SLUS-21410");

	EXPECT_EQ(packs[2].file_count, 3u);
	EXPECT_EQ(packs[2].bytes, 30u);
	EXPECT_EQ(packs[2].directory, TexturePath("SLUS-21410/replacements"));
	ASSERT_TRUE(packs[2].record.has_value());
	EXPECT_EQ(packs[2].record->pack_id, "pack-a");
	EXPECT_FALSE(packs[0].record.has_value()); // nothing to say about a hand-copied pack
	EXPECT_EQ(packs[0].file_count, 2u);
}

TEST_F(InstalledPacksTest, ListingDropsRecordsOfPacksDeletedBehindOurBack)
{
	MakeFolder("SLUS-21410", 1);
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("kept", "SLUS-21410", "x", TexturePackFormat::TarZstd, 1), "SLUS-21410")));
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("gone", "SLES-52187", "x", TexturePackFormat::TarZstd, 1), "SLES-52187")));

	installer.ListInstalled();
	EXPECT_TRUE(state.Find("kept").has_value());
	EXPECT_FALSE(state.Find("gone").has_value());
}

TEST_F(InstalledPacksTest, ACancelledListingReturnsNothingAndKeepsEveryRecord)
{
	MakeFolder("SLUS-21410", 2);
	MakeFolder("SLES-52187", 1);
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("pack-a", "SLUS-21410", "x", TexturePackFormat::TarZstd, 1), "SLUS-21410")));
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("pack-b", "SLES-52187", "x", TexturePackFormat::TarZstd, 1), "SLES-52187")));

	CancelToken cancel;
	cancel.Cancel();
	EXPECT_TRUE(installer.ListInstalled(cancel).empty());

	// A partial list handed to Reconcile() would have dropped the record of every pack it did not reach.
	EXPECT_TRUE(state.Find("pack-a").has_value());
	EXPECT_TRUE(state.Find("pack-b").has_value());

	EXPECT_EQ(installer.ListInstalled().size(), 2u); // the same installer lists normally afterwards
}

TEST_F(InstalledPacksTest, SizesComeFromTheCacheWhileTheFolderIsUnchanged)
{
	MakeFolder("SLUS-21410", 3);
	ASSERT_EQ(installer.ListInstalled().at(0).file_count, 3u);

	// Rewrite the saved sizes: right modification time keeps the (wrong) number, a different one
	// makes the folder be measured again.
	const std::string replacements = TexturePath("SLUS-21410/replacements");
	const s64 mtime = static_cast<s64>(FileSystem::GetFileTimestamp(replacements.c_str()).value());
	const auto write_cache = [&](s64 stamp) {
		ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("cache/pack-sizes.json").c_str(),
			fmt::format(R"({{"SLUS-21410":{{"m":{},"c":999,"b":12345}}}})", stamp)));
	};

	write_cache(mtime);
	TexturePackInstaller trusting(http, state, MakeConfig());
	const auto trusted = trusting.ListInstalled();
	ASSERT_EQ(trusted.size(), 1u);
	EXPECT_EQ(trusted[0].file_count, 999u);
	EXPECT_EQ(trusted[0].bytes, 12345u);

	write_cache(mtime - 100);
	TexturePackInstaller measuring(http, state, MakeConfig());
	EXPECT_EQ(measuring.ListInstalled().at(0).file_count, 3u);

	// And the fresh measurement is saved for next time.
	rapidjson::Document doc;
	ASSERT_TRUE(Catalog::ParseJson(ReadAll(temp.File("cache/pack-sizes.json")), &doc));
	EXPECT_EQ(doc["SLUS-21410"]["c"].GetUint64(), 3u);
}

TEST_F(InstalledPacksTest, AnInstallDropsTheRememberedSize)
{
	Archive first = BuildTarPack("first", "SLUS-21410", NumberedTextures(3));
	Archive second = BuildTarPack("second", "SLUS-21410", NumberedTextures(5));
	Serve(first);
	Serve(second);

	ASSERT_TRUE(Install(first.pack, "SLUS-21410").IsOk());
	EXPECT_EQ(installer.ListInstalled().at(0).file_count, 3u);
	ASSERT_TRUE(Install(second.pack, "SLUS-21410").IsOk());
	EXPECT_EQ(installer.ListInstalled().at(0).file_count, 5u);
}

TEST_F(InstalledPacksTest, RemovingAPackTakesItOutOfUseAndForgetsIt)
{
	MakeFolder("SLUS-21410", 3);
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath("SLUS-21410/replacements.old/x").c_str(), true)); // an interrupted commit
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("pack-a", "SLUS-21410", "x", TexturePackFormat::TarZstd, 1), "SLUS-21410")));
	MakeFolder("SLES-52187", 1);

	EXPECT_TRUE(installer.Remove("SLUS-21410"));
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410").c_str())); // empty, so the folder goes too
	EXPECT_TRUE(FileSystem::DirectoryExists(TexturePath("SLES-52187/replacements").c_str()));
	EXPECT_FALSE(state.Find("pack-a").has_value());

	// Nothing is left behind under the temporary names.
	for (const auto& file : ListFiles(temp.File("textures")))
		EXPECT_EQ(file.find(".deleting-"), std::string::npos) << file;
	std::error_code ec;
	for (const auto& item : std::filesystem::directory_iterator(temp.File("textures"), ec))
		EXPECT_NE(item.path().filename().string().rfind(".deleting-", 0), 0u) << item.path();
}

TEST_F(InstalledPacksTest, RemovingAPackKeepsTextureDumps)
{
	MakeFolder("SLUS-21410", 2, true);
	EXPECT_TRUE(installer.Remove("SLUS-21410"));
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410/replacements").c_str()));
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/dumps/dump.png")), "dump");
}

TEST_F(InstalledPacksTest, RemoveTakesAFolderNameNotAPath)
{
	MakeFolder("SLUS-21410", 1);
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("outside/replacements").c_str(), true));

	for (const char* bad : {"", ".", "..", "../outside", "SLUS-21410/..", "a/b", "a\\b", ".texturepacks-tmp", ".hidden"})
		EXPECT_FALSE(installer.Remove(bad)) << bad;
	EXPECT_TRUE(FileSystem::DirectoryExists(temp.File("outside/replacements").c_str()));
	EXPECT_TRUE(FileSystem::DirectoryExists(TexturePath("SLUS-21410/replacements").c_str()));
}

TEST_F(InstalledPacksTest, SweepFinishesDeletesThatWereInterrupted)
{
	MakeFolder("SLUS-21410", 1);
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(".deleting-SLES-52187-replacements-123/sub").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(TexturePath(".deleting-SLES-52187-replacements-123/sub/a.png").c_str(), "x"));

	installer.SweepRemoved();
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".deleting-SLES-52187-replacements-123").c_str()));
	EXPECT_TRUE(FileSystem::DirectoryExists(TexturePath("SLUS-21410/replacements").c_str()));

	// Listing sweeps too.
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(".deleting-X-replacements-1").c_str(), true));
	installer.ListInstalled();
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath(".deleting-X-replacements-1").c_str()));
}

TEST_F(InstalledPacksTest, ListingAMissingTexturesFolderIsEmpty)
{
	EXPECT_TRUE(installer.ListInstalled().empty());
	EXPECT_TRUE(installer.Remove("SLUS-21410")); // no pack is left there, which is what was asked for
}

// ------------------------------------------------------------------------------------------
// A replacements folder the installer did not make
// ------------------------------------------------------------------------------------------

class UserFolderTest : public InstalledPacksTest
{
protected:
	// A folder of textures the player made or copied, with no install record.
	void MakeUserFolder(const std::string& serial = "SLUS-21410")
	{
		ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath(serial + "/replacements").c_str(), true));
		ASSERT_TRUE(FileSystem::WriteStringToFile(TexturePath(serial + "/replacements/mine.png").c_str(), "hand made"));
	}

	std::vector<std::string> KeptFolders(const std::string& serial = "SLUS-21410")
	{
		return FoldersStartingWith(TexturePath(serial), "replacements.user-");
	}
};

TEST_F(UserFolderTest, AnInstallKeepsAFolderTheInstallerDidNotMake)
{
	MakeUserFolder();
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2, "pack"));
	Serve(a);

	const auto result = Install(a.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;

	// The pack is where the engine reads it, and the player's textures are beside it under another name.
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 2u);
	EXPECT_FALSE(FileSystem::FileExists(TexturePath("SLUS-21410/replacements/mine.png").c_str()));
	const auto kept = KeptFolders();
	ASSERT_EQ(kept.size(), 1u);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/" + kept[0] + "/mine.png")), "hand made");
	EXPECT_FALSE(FileSystem::DirectoryExists(TexturePath("SLUS-21410/replacements.old").c_str()));
}

TEST_F(UserFolderTest, AUiCanAskBeforeAnInstallWouldSetAFolderAside)
{
	EXPECT_FALSE(installer.HasUnrecordedReplacements("SLUS-21410")); // nothing there

	MakeUserFolder();
	EXPECT_TRUE(installer.HasUnrecordedReplacements("SLUS-21410"));
	EXPECT_TRUE(installer.HasUnrecordedReplacements("slus 21410")); // spelled as Install() accepts it
	EXPECT_FALSE(installer.HasUnrecordedReplacements("SLES-52187")); // another game's folder
	EXPECT_FALSE(installer.HasUnrecordedReplacements("../SLUS-21410")); // not a serial
	EXPECT_FALSE(installer.HasUnrecordedReplacements(""));

	// Once the install has kept it, what is at replacements is a pack with a record: nothing to ask.
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2));
	Serve(a);
	const auto result = Install(a.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_FALSE(installer.HasUnrecordedReplacements("SLUS-21410"));

	// The result names the folder the player's textures are in now.
	ASSERT_EQ(KeptFolders().size(), 1u);
	EXPECT_EQ(result.kept_previous, KeptFolders()[0]);
	EXPECT_EQ(result.kept_previous.rfind(TexturePackInstaller::KEPT_FOLDER_PREFIX, 0), 0u);
}

TEST_F(UserFolderTest, AFirstInstallHasNothingToKeep)
{
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2));
	Serve(a);
	const auto result = Install(a.pack, "SLUS-21410");
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_TRUE(result.kept_previous.empty());
	EXPECT_TRUE(KeptFolders().empty());
}

TEST_F(UserFolderTest, AnInstallOverAPackItMadeKeepsNothing)
{
	Archive first = BuildTarPack("first", "SLUS-21410", NumberedTextures(3, "first"));
	Archive second = BuildTarPack("second", "SLUS-21410", NumberedTextures(2, "second"));
	Serve(first);
	Serve(second);
	ASSERT_TRUE(Install(first.pack, "SLUS-21410").IsOk());
	ASSERT_TRUE(Install(second.pack, "SLUS-21410").IsOk());
	EXPECT_TRUE(KeptFolders().empty());
}

TEST_F(UserFolderTest, TwoKeptFoldersInTheSameSecondDoNotCollide)
{
	// The first install keeps the player's folder. Forgetting the record then makes the installed pack look like
	// one the player made, so the second install keeps that too, under a name the first did not take.
	MakeUserFolder();
	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2));
	Serve(a);
	ASSERT_TRUE(Install(a.pack, "SLUS-21410").IsOk());
	ASSERT_TRUE(state.ForgetSerial("SLUS-21410"));

	ASSERT_TRUE(Install(a.pack, "SLUS-21410").IsOk());
	EXPECT_EQ(KeptFolders().size(), 2u);
}

TEST_F(UserFolderTest, ASymlinkedReplacementsFolderIsNeverEmptied)
{
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("elsewhere").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("elsewhere/precious.png").c_str(), "precious"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath("SLUS-21410").c_str(), true));
	if (!MakeFolderLink(temp.File("elsewhere"), TexturePath("SLUS-21410/replacements")))
		GTEST_SKIP() << "cannot make a symbolic link here";

	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2));
	Serve(a);
	ASSERT_TRUE(Install(a.pack, "SLUS-21410").IsOk());

	EXPECT_EQ(ReadAll(temp.File("elsewhere/precious.png")), "precious");
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 2u);
	// The link itself is what was kept.
	const auto kept = KeptFolders();
	ASSERT_EQ(kept.size(), 1u);
	EXPECT_EQ(ReadAll(TexturePath("SLUS-21410/" + kept[0] + "/precious.png")), "precious");
}

TEST_F(UserFolderTest, ReplacingARecordedPackThatIsALinkRemovesTheLinkNotItsTarget)
{
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("elsewhere").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("elsewhere/precious.png").c_str(), "precious"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath("SLUS-21410").c_str(), true));
	if (!MakeFolderLink(temp.File("elsewhere"), TexturePath("SLUS-21410/replacements")))
		GTEST_SKIP() << "cannot make a symbolic link here";
	ASSERT_TRUE(state.Record(TexturePackState::RecordFor(DescribeArchive("old", "SLUS-21410", "x", TexturePackFormat::TarZstd, 1), "SLUS-21410")));

	Archive a = BuildTarPack("pack-a", "SLUS-21410", NumberedTextures(2));
	Serve(a);
	ASSERT_TRUE(Install(a.pack, "SLUS-21410").IsOk());

	EXPECT_EQ(ReadAll(temp.File("elsewhere/precious.png")), "precious");
	EXPECT_FALSE(std::filesystem::is_symlink(TexturePath("SLUS-21410/replacements")));
	EXPECT_EQ(ListFiles(TexturePath("SLUS-21410/replacements")).size(), 2u);
}

TEST_F(UserFolderTest, RemovingASymlinkedFolderRemovesTheLinkNotItsTarget)
{
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("elsewhere").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("elsewhere/precious.png").c_str(), "precious"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(TexturePath("SLUS-21410").c_str(), true));
	if (!MakeFolderLink(temp.File("elsewhere"), TexturePath("SLUS-21410/replacements")))
		GTEST_SKIP() << "cannot make a symbolic link here";
	// replacements.old can be a link too.
	if (!MakeFolderLink(temp.File("elsewhere"), TexturePath("SLUS-21410/replacements.old")))
		GTEST_SKIP() << "cannot make a symbolic link here";

	EXPECT_TRUE(installer.Remove("SLUS-21410"));
	EXPECT_EQ(ReadAll(temp.File("elsewhere/precious.png")), "precious");
	EXPECT_FALSE(std::filesystem::exists(TexturePath("SLUS-21410/replacements")));
}

TEST(TexturePackInstallerCommit, ALinkAtTheTargetIsReplacedWithoutEmptyingWhatItPointsAt)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("staged").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("staged/new.png").c_str(), "new"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("elsewhere").c_str(), true));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("elsewhere/precious.png").c_str(), "precious"));
	ASSERT_TRUE(FileSystem::EnsureDirectoryExists(temp.File("game").c_str(), true));
	if (!MakeFolderLink(temp.File("elsewhere"), temp.File("game/replacements")))
		GTEST_SKIP() << "cannot make a symbolic link here";

	ASSERT_TRUE(TexturePackInstaller::CommitDirectory(temp.File("staged"), temp.File("game/replacements")));
	EXPECT_EQ(ReadAll(temp.File("elsewhere/precious.png")), "precious");
	EXPECT_EQ(ReadAll(temp.File("game/replacements/new.png")), "new");
}

// ------------------------------------------------------------------------------------------
// Presentation helpers
// ------------------------------------------------------------------------------------------

TEST(TexturePackInstallerText, SizesAreShownTheWayTheCatalogAlwaysDid)
{
	EXPECT_EQ(FormatPackSize(0), "0 MB");
	EXPECT_EQ(FormatPackSize(241ull * 1024 * 1024 + 5), "241 MB");
	EXPECT_EQ(FormatPackSize(1024ull * 1024 * 1024 - 1), "1023 MB");
	EXPECT_EQ(FormatPackSize(1024ull * 1024 * 1024), "1.0 GB");
	EXPECT_EQ(FormatPackSize(1288490189), "1.2 GB");

	using P = TexturePackInstaller::Progress;
	EXPECT_EQ(DescribeProgress(P{Phase::Starting, 0, 0}), "Starting...");
	EXPECT_EQ(DescribeProgress(P{Phase::Downloading, 241ull * 1024 * 1024, 1288490189}), "Downloading 241 MB of 1.2 GB");
	EXPECT_EQ(DescribeProgress(P{Phase::Verifying, 0, 0}), "Verifying download...");
	EXPECT_EQ(DescribeProgress(P{Phase::Extracting, 32, 4277}), "Extracting 32 / 4277 files");
	EXPECT_EQ(DescribeProgress(P{Phase::Installing, 0, 0}), "Installing...");
}
