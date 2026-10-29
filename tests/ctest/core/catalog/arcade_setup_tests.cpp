// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// ArcadeSetup: the Proverb boot files (download, store, read a boot.elf on demand), the arcade BIOS status, what
// a game still lacks, and getting a game ready (an .acgame, or a loose image with its dongle beside it). Nothing
// here touches the network: the downloads go to a FakeFetcher.

#include "catalog_test_support.h"

#include "FakeFetcher.h"

#include "Arcade/AcGameFile.h"
#include "Arcade/ArcadeFiles.h"
#include "Arcade/ArcadeSetup.h"
#include "CDVD/CDVDcommon.h"
#include "Config.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "common/StringUtil.h"

#include "fmt/format.h"
#include "zip.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace ArcadeSetup;
using CatalogTest::FakeFetcher;
using CatalogTest::FakeReply;
using CatalogTest::ScopedTempDir;

namespace
{
	constexpr size_t MB = 1024 * 1024;
	constexpr u64 PS2_CARD = 8650752; // A card with its spare bytes: 16384 pages of 528.
	constexpr u64 BIN_CARD = 8 * MB; // A card without.
	const std::string REDIRECT_TARGET = "https://release-assets.githubusercontent.com/signed/proverb.zip?sp=r";

	void WriteFile(const std::string& path, const std::string& content)
	{
		ASSERT_TRUE(FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(path)).c_str(), true)) << path;
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << content;
	}

	std::string ReadFile(const std::string& path)
	{
		return FileSystem::ReadFileToString(path.c_str()).value_or("<missing>");
	}

	bool Exists(const std::string& path)
	{
		return FileSystem::FileExists(path.c_str());
	}

	std::vector<std::string> NamesIn(const std::string& dir)
	{
		std::vector<std::string> names;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
			names.push_back(entry.path().filename().string());
		std::sort(names.begin(), names.end());
		return names;
	}

	std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& entries)
	{
		ScopedTempDir dir;
		const std::string path = dir.File("made.zip");
		int err = 0;
		zip_t* za = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
		EXPECT_NE(za, nullptr);
		for (const auto& [name, content] : entries)
		{
			zip_source_t* source = zip_source_buffer(za, content.data(), content.size(), 0);
			EXPECT_GE(zip_file_add(za, name.c_str(), source, ZIP_FL_OVERWRITE), 0);
		}
		EXPECT_EQ(zip_close(za), 0);
		return ReadFile(path);
	}

	std::string BootElfOf(const std::string& id)
	{
		return "ELF:" + id;
	}

	// Proverb's layout: bin/<id>/boot.elf and a title.txt beside it, with the folders as entries.
	std::string MakeProverbZip(const std::vector<std::string>& ids)
	{
		std::vector<std::pair<std::string, std::string>> entries;
		for (const std::string& id : ids)
		{
			entries.emplace_back("bin/" + id + "/", "");
			entries.emplace_back("bin/" + id + "/boot.elf", BootElfOf(id));
			entries.emplace_back("bin/" + id + "/title.txt", "GameID: " + id + "\n");
		}
		return MakeZip(entries);
	}

	std::string Gzip(const std::string& data)
	{
		z_stream zs = {};
		EXPECT_EQ(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY), Z_OK);
		std::string out(deflateBound(&zs, static_cast<uLong>(data.size())), '\0');
		zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
		zs.avail_in = static_cast<uInt>(data.size());
		zs.next_out = reinterpret_cast<Bytef*>(out.data());
		zs.avail_out = static_cast<uInt>(out.size());
		EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
		out.resize(zs.total_out);
		deflateEnd(&zs);
		return out;
	}

	std::string ReleaseJson(const std::string& name, size_t zip_size, const std::string& digest)
	{
		std::string assets = R"({"name":"proverb.elf","size":25060,"updated_at":"2026-07-25T21:32:55Z","digest":"sha256:00"},)";
		assets += fmt::format(R"({{"name":"proverb.zip","size":{},"updated_at":"2026-07-25T21:32:56Z")", zip_size);
		if (!digest.empty())
			assets += fmt::format(R"(,"digest":"sha256:{}")", digest);
		assets += "}";
		return fmt::format(R"({{"name":"{}","published_at":"2026-07-25T21:32:54Z","assets":[{}]}})", name, assets);
	}

	void ScriptDownload(FakeFetcher& http, const std::string& zip, const std::string& release_json)
	{
		http.Ok(BOOT_RELEASE_URL, release_json)
			.Redirect(BOOT_FILES_URL, REDIRECT_TARGET)
			.Ok(REDIRECT_TARGET, zip, "application/octet-stream");
	}

	// A ROMDIR that LoadBiosVersion reads, with the ROMVER and EXTINFO of a console and of two arcade boards
	// (as in arcade_bios_tests.cpp).
	std::string MakeBios(const char* romver, const char* extinfo, size_t size)
	{
		std::string image(size, '\0');
		const auto record = [&image](size_t index, const char* name, uint32_t file_size) {
			char* at = image.data() + index * 16;
			std::strncpy(at, name, 10);
			std::memcpy(at + 12, &file_size, 4);
		};
		record(0, "RESET", 0x10);
		record(1, "ROMDIR", 0x50);
		record(2, "EXTINFO", 0x20);
		record(3, "ROMVER", 0x10);
		std::memcpy(image.data() + 0x70, extinfo, 15);
		std::memcpy(image.data() + 0x80, romver, 14);
		return image;
	}

	std::string ConsoleBios() { return MakeBios("0200AC20040614", "20040614-000000", 4 * MB); }
	std::string System256Bios() { return MakeBios("0100TZ20000603", "20040519-145634", 2 * MB); }
	std::string System246Bios() { return MakeBios("0100TZ20000603", "20021119-163841", 2 * MB); }

	// A memory card file of [size] bytes that is all [tag], so that two cards are told apart by their content.
	std::string Card(char tag, u64 size)
	{
		return std::string(size, tag);
	}

	// A made-up Conquest card: Soul Calibur II's header, then [tag].
	std::string ConquestCard(char tag, u64 size = 64 * 1024)
	{
		std::string card(size, tag);
		const std::string header = "Memory Card for SoulCaliburII (C)1995";
		card.replace(0, header.size(), header);
		return card;
	}

	std::string Hex(const std::string& bytes)
	{
		static const char digits[] = "0123456789abcdef";
		std::string hex;
		for (const unsigned char c : bytes)
		{
			hex += digits[c >> 4];
			hex += digits[c & 15];
		}
		return hex;
	}

	std::string Sha(const std::string& data)
	{
		return SHA256Digest::HashHex(data);
	}

	class ArcadeSetupTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_old_bios = EmuFolders::Bios;
			EmuFolders::Bios = m_dir.File("bios");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(EmuFolders::Bios.c_str(), true));

			m_games = m_dir.File("games");
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_games.c_str(), true));

			m_folders.arcade = m_dir.File("data/arcade");
			m_folders.memory_cards = m_dir.File("data/memcards");
			m_folders.conquest_card_gz = m_dir.File("resources/NM00007.conquestcard.gzip");
			// A small made-up Conquest card instead of the 8 MB real one, so that the swap by hash can run here.
			m_cleared = ConquestCard('C');
			m_uncleared = ConquestCard('U');
			m_folders.conquest.bytes = m_cleared.size();
			m_folders.conquest.uncleared_sha256 = Sha(m_uncleared);
			m_folders.conquest.probe_offset = 100;
			m_folders.conquest.probe_hex = Hex(m_uncleared.substr(100, 16));
			WriteFile(m_folders.conquest_card_gz, Gzip(m_cleared));
		}

		void TearDown() override { EmuFolders::Bios = m_old_bios; }

		Service MakeService() { return Service(m_folders); }

		std::string Games(const std::string& relative) const { return Path::Combine(m_games, relative); }
		std::string Cards(const std::string& relative) const { return Path::Combine(m_folders.memory_cards, relative); }
		std::string Data(const std::string& relative) const { return Path::Combine(m_folders.arcade, relative); }

		void PutBios(const char* name, const std::string& image) { WriteFile(Path::Combine(EmuFolders::Bios, name), image); }

		void InstallBootFiles(const std::vector<std::string>& ids)
		{
			const std::string zip = m_dir.File("installing.zip");
			WriteFile(zip, MakeProverbZip(ids));
			Service service = MakeService();
			const InstallResult result = service.InstallBootFilesFromFile(zip);
			ASSERT_TRUE(result.ok) << result.error;
		}

		// An .acgame in PCSX2x6's layout, as the Tekken 4 test fixture has it: the folder NM00004 beside it.
		std::string PutAcgame(const char* id, const std::string& extra_data = "")
		{
			const std::string path = Games(std::string(id) + ".acgame");
			WriteFile(path, fmt::format("[game]\nname=Game {0}\ngameid={0}\n\n[data]\nsubdir={0}\n{1}", id, extra_data));
			return path;
		}

		static bool Has(const MissingReport& report, PartKind kind)
		{
			return std::any_of(report.parts.begin(), report.parts.end(), [kind](const MissingPart& p) { return p.kind == kind; });
		}

		static std::vector<PartKind> Kinds(const MissingReport& report)
		{
			std::vector<PartKind> kinds;
			for (const MissingPart& part : report.parts)
				kinds.push_back(part.kind);
			return kinds;
		}

		ScopedTempDir m_dir;
		Folders m_folders;
		std::string m_games, m_old_bios, m_cleared, m_uncleared;
	};
} // namespace

TEST_F(ArcadeSetupTest, WithoutADownloadNothingIsInstalled)
{
	Service service = MakeService();
	const BootFilesStatus status = service.GetBootFilesStatus();
	EXPECT_FALSE(status.installed);
	EXPECT_TRUE(status.game_ids.empty());
	EXPECT_EQ(status.path, Data("proverb.zip"));
	EXPECT_FALSE(service.HasBootProgram("NM00004"));
}

TEST_F(ArcadeSetupTest, ADownloadIsStoredAndNotesWhatItIs)
{
	const std::string zip = MakeProverbZip({"NM00010", "NM00004", "NM00007"});
	FakeFetcher http;
	ScriptDownload(http, zip, ReleaseJson("Nightly build of v1.0.4", zip.size(), Sha(zip)));

	u64 last_done = 0, last_total = 0;
	DownloadOptions options;
	options.on_progress = [&](u64 done, u64 total) {
		last_done = done;
		last_total = total;
	};

	Service service = MakeService();
	const InstallResult result = service.DownloadBootFiles(http, options);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_FALSE(result.cancelled);
	EXPECT_EQ(last_done, zip.size());
	EXPECT_EQ(last_total, zip.size());

	const BootFilesStatus& status = result.status;
	EXPECT_TRUE(status.installed);
	EXPECT_EQ(status.game_ids, (std::vector<std::string>{"NM00004", "NM00007", "NM00010"})); // Sorted.
	EXPECT_EQ(status.size_bytes, zip.size());
	EXPECT_EQ(status.sha256, Sha(zip));
	EXPECT_EQ(status.release_name, "Nightly build of v1.0.4");
	EXPECT_EQ(status.release_time, 1785015176); // 2026-07-25T21:32:56Z, the zip's, not the release's.
	EXPECT_GT(status.installed_at, 0);

	// The stored zip is the download, nothing is left beside it, and it is the same to a fresh service.
	EXPECT_EQ(ReadFile(Data("proverb.zip")), zip);
	EXPECT_EQ(NamesIn(m_folders.arcade), (std::vector<std::string>{"proverb.json", "proverb.zip"}));
	const BootFilesStatus again = MakeService().GetBootFilesStatus();
	EXPECT_EQ(again.sha256, status.sha256);
	EXPECT_EQ(again.release_name, status.release_name);
	EXPECT_EQ(again.release_time, status.release_time);

	// Both requests went to GitHub, the second through the redirect to its signed address.
	EXPECT_EQ(http.RequestCount(BOOT_RELEASE_URL), 1u);
	EXPECT_EQ(http.RequestCount(BOOT_FILES_URL), 1u);
	EXPECT_EQ(http.RequestCount(REDIRECT_TARGET), 1u);
}

TEST_F(ArcadeSetupTest, ADownloadWorksWhenGitHubWillNotSayWhatTheReleaseIs)
{
	const std::string zip = MakeProverbZip({"NM00004"});
	FakeFetcher http;
	http.Status(BOOT_RELEASE_URL, 403, R"({"message":"API rate limit exceeded"})")
		.Redirect(BOOT_FILES_URL, REDIRECT_TARGET)
		.Ok(REDIRECT_TARGET, zip, "application/octet-stream");

	Service service = MakeService();
	const InstallResult result = service.DownloadBootFiles(http);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(result.status.game_ids, (std::vector<std::string>{"NM00004"}));
	EXPECT_EQ(result.status.sha256, Sha(zip));
	EXPECT_EQ(result.status.release_name, "");
	EXPECT_EQ(result.status.release_time, 0);
}

TEST_F(ArcadeSetupTest, ADownloadThatIsNotTheFileGitHubPublishedADigestForIsRefused)
{
	// GitHub's digest is another file's: the download was damaged or swapped on its way, or the release was
	// republished between the two requests. Either way nothing is installed.
	const std::string zip = MakeProverbZip({"NM00004"});
	FakeFetcher http;
	ScriptDownload(http, zip, ReleaseJson("Nightly build of v9", zip.size(), std::string(64, 'a')));

	Service service = MakeService();
	const InstallResult result = service.DownloadBootFiles(http);
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(result.cancelled);
	EXPECT_NE(result.error.find("checksum"), std::string::npos) << result.error;
	EXPECT_FALSE(result.status.installed);
	EXPECT_FALSE(Exists(Data("proverb.zip")));
	EXPECT_FALSE(Exists(Data("proverb.zip.part")));
	EXPECT_FALSE(service.HasBootProgram("NM00004"));
}

TEST_F(ArcadeSetupTest, ARefusedDownloadLeavesTheBootFilesThatWereThere)
{
	InstallBootFiles({"NM00004"});
	const std::string before = Service(m_folders).GetBootFilesStatus().sha256;

	const std::string replacement = MakeProverbZip({"NM00004", "NM00007"});
	FakeFetcher http;
	ScriptDownload(http, replacement, ReleaseJson("Nightly build of v9", replacement.size(), Sha("something else")));
	EXPECT_FALSE(MakeService().DownloadBootFiles(http).ok);

	const BootFilesStatus after = MakeService().GetBootFilesStatus();
	EXPECT_EQ(after.sha256, before);
	EXPECT_EQ(after.game_ids, (std::vector<std::string>{"NM00004"}));
	EXPECT_FALSE(Exists(Data("proverb.zip.part")));
}

TEST_F(ArcadeSetupTest, ABadDownloadChangesNothing)
{
	const std::string good = MakeProverbZip({"NM00004", "NM00007"});
	{
		FakeFetcher http;
		ScriptDownload(http, good, ReleaseJson("Nightly build of v1.0.4", good.size(), Sha(good)));
		Service service = MakeService();
		ASSERT_TRUE(service.DownloadBootFiles(http).ok);
	}

	const auto expect_untouched = [&](const InstallResult& result) {
		EXPECT_FALSE(result.ok);
		EXPECT_FALSE(result.cancelled);
		EXPECT_FALSE(result.error.empty());
		EXPECT_EQ(result.status.sha256, Sha(good));
		EXPECT_EQ(ReadFile(Data("proverb.zip")), good);
		EXPECT_EQ(NamesIn(m_folders.arcade), (std::vector<std::string>{"proverb.json", "proverb.zip"})); // No .part.
		EXPECT_EQ(MakeService().GetBootFilesStatus().game_ids, (std::vector<std::string>{"NM00004", "NM00007"}));
	};

	{
		FakeFetcher http;
		http.Status(BOOT_RELEASE_URL, 404).Status(BOOT_FILES_URL, 404, "Not Found");
		Service service = MakeService();
		const InstallResult result = service.DownloadBootFiles(http);
		expect_untouched(result);
		EXPECT_NE(result.error.find("404"), std::string::npos) << result.error;
	}
	// A zip with no boot programs in it.
	{
		FakeFetcher http;
		const std::string other = MakeZip({{"readme.txt", "hello"}, {"bin/NM00004/title.txt", "x"}});
		ScriptDownload(http, other, ReleaseJson("x", other.size(), Sha(other)));
		Service service = MakeService();
		expect_untouched(service.DownloadBootFiles(http));
	}
	// Something that is no zip at all (a web page in place of the file).
	{
		FakeFetcher http;
		ScriptDownload(http, "<html>Sign in</html>", "{}");
		Service service = MakeService();
		expect_untouched(service.DownloadBootFiles(http));
	}
	{
		FakeFetcher http;
		http.Status(BOOT_RELEASE_URL, 404).Redirect(BOOT_FILES_URL, REDIRECT_TARGET).Truncated(REDIRECT_TARGET, good, good.size() / 2);
		Service service = MakeService();
		expect_untouched(service.DownloadBootFiles(http));
	}
	{
		FakeFetcher http;
		http.Fail(BOOT_RELEASE_URL, "no route").Fail(BOOT_FILES_URL, "no route");
		Service service = MakeService();
		expect_untouched(service.DownloadBootFiles(http));
	}
}

TEST_F(ArcadeSetupTest, ADownloadThatIsTooBigIsRefused)
{
	// The cap is on what is written, whatever the server says it is sending.
	FakeFetcher http;
	http.Status(BOOT_RELEASE_URL, 404).Redirect(BOOT_FILES_URL, REDIRECT_TARGET);
	FakeReply reply;
	reply.body = std::string(1024, 'z');
	reply.content_length = static_cast<s64>(MAX_BOOT_FILES_BYTES + 1);
	reply.cut_after = 1024;
	http.Reply(REDIRECT_TARGET, reply);

	Service service = MakeService();
	const InstallResult result = service.DownloadBootFiles(http);
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(Exists(Data("proverb.zip")));
	EXPECT_FALSE(Exists(Data("proverb.zip.part")));
}

TEST_F(ArcadeSetupTest, ACancelledDownloadStopsAndKeepsWhatWasThere)
{
	InstallBootFiles({"NM00004"});
	const std::string before = Service(m_folders).GetBootFilesStatus().sha256;

	const std::string zip = MakeProverbZip({"NM00010"});
	FakeFetcher http;
	ScriptDownload(http, zip, ReleaseJson("x", zip.size(), Sha(zip)));
	DownloadOptions options;
	options.cancel.Cancel();

	Service service = MakeService();
	const InstallResult result = service.DownloadBootFiles(http, options);
	EXPECT_FALSE(result.ok);
	EXPECT_TRUE(result.cancelled);
	EXPECT_EQ(service.GetBootFilesStatus().sha256, before);
	EXPECT_EQ(service.GetBootFilesStatus().game_ids, (std::vector<std::string>{"NM00004"}));
}

TEST_F(ArcadeSetupTest, ABootProgramIsReadOutOfTheZipOnDemand)
{
	InstallBootFiles({"NM00004", "NM00007"});
	Service service = MakeService();
	EXPECT_TRUE(service.HasBootProgram("NM00007"));
	EXPECT_TRUE(service.HasBootProgram("nm00007")); // Any case.
	EXPECT_FALSE(service.HasBootProgram("NM00010"));

	const std::string out = m_dir.File("out/NM00007/boot.elf");
	std::string error;
	ASSERT_TRUE(service.ExtractBootProgram("NM00007", out, &error)) << error;
	EXPECT_EQ(ReadFile(out), BootElfOf("NM00007"));
	WriteFile(out, "stale");
	ASSERT_TRUE(service.ExtractBootProgram("nm00007", out, &error)) << error;
	EXPECT_EQ(ReadFile(out), BootElfOf("NM00007"));

	// A game it has none for: no file made, and a reason.
	const std::string none = m_dir.File("out/NM00010/boot.elf");
	EXPECT_FALSE(service.ExtractBootProgram("NM00010", none, &error));
	EXPECT_NE(error.find("NM00010"), std::string::npos) << error;
	EXPECT_FALSE(Exists(none));
}

TEST_F(ArcadeSetupTest, ABootProgramCannotBeReadBeforeTheDownload)
{
	Service service = MakeService();
	std::string error;
	EXPECT_FALSE(service.ExtractBootProgram("NM00004", m_dir.File("x.elf"), &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(Exists(m_dir.File("x.elf")));
}

TEST_F(ArcadeSetupTest, OnlyBinFolderBootElfEntriesAreBootPrograms)
{
	// Proverb's own layout is bin/<id>/boot.elf; the same under another folder or in another case is the same
	// thing, the Android app's (?:^|/)bin/(NM\d{5})/boot\.elf$. Nothing else is.
	const std::string zip = MakeZip({
		{"proverb/BIN/nm00001/BOOT.ELF", "a"},
		{"bin/NM00002/boot.elf", "b"},
		{"xbin/NM00003/boot.elf", "no: not a bin folder"},
		{"bin/NM00004/boot.elf.bak", "no"},
		{"bin/NM0005/boot.elf", "no: four digits"},
		{"bin/NM00006/sub/boot.elf", "no: one folder too deep"},
		{"bin/NM00007/title.txt", "no"},
	});
	WriteFile(m_dir.File("odd.zip"), zip);
	Service service = MakeService();
	const InstallResult result = service.InstallBootFilesFromFile(m_dir.File("odd.zip"));
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(result.status.game_ids, (std::vector<std::string>{"NM00001", "NM00002"}));

	ASSERT_TRUE(service.ExtractBootProgram("NM00001", m_dir.File("o1"), nullptr));
	EXPECT_EQ(ReadFile(m_dir.File("o1")), "a");
}

TEST_F(ArcadeSetupTest, ARealProverbZipIsUnderstood)
{
	// A trimmed copy of the real Proverb zip as GitHub serves it: three games, 25028-byte boot.elf each.
	Service service = MakeService();
	const InstallResult result = service.InstallBootFilesFromFile(CatalogTest::FixturePath("arcade/proverb-trimmed.zip"));
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(result.status.game_ids, (std::vector<std::string>{"NM00004", "NM00007", "NM00010"}));

	const std::string out = m_dir.File("out/boot.elf");
	ASSERT_TRUE(service.ExtractBootProgram("NM00007", out));
	const std::string elf = ReadFile(out);
	EXPECT_EQ(elf.size(), 25028u);
	EXPECT_EQ(elf.substr(0, 4), "\x7f"
								"ELF");
}

TEST_F(ArcadeSetupTest, AZipThePlayerHasCanBeInstalledWithoutTheNetwork)
{
	WriteFile(m_dir.File("mine.zip"), MakeProverbZip({"NM00004", "NM00005"}));
	Service service = MakeService();
	const InstallResult result = service.InstallBootFilesFromFile(m_dir.File("mine.zip"));
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(result.status.game_ids.size(), 2u);
	EXPECT_EQ(result.status.release_name, ""); // Nothing is known of where it came from.
	EXPECT_EQ(result.status.sha256, Sha(ReadFile(m_dir.File("mine.zip"))));
	EXPECT_TRUE(Exists(m_dir.File("mine.zip"))); // Copied, not moved.

	// What is not boot files is turned away, and the installed ones stay.
	WriteFile(m_dir.File("not-a-zip.zip"), "plain text");
	WriteFile(m_dir.File("other.zip"), MakeZip({{"readme.txt", "x"}}));
	for (const char* name : {"not-a-zip.zip", "other.zip", "missing.zip"})
	{
		const InstallResult bad = service.InstallBootFilesFromFile(m_dir.File(name));
		EXPECT_FALSE(bad.ok) << name;
		EXPECT_FALSE(bad.error.empty()) << name;
		EXPECT_EQ(bad.status.game_ids.size(), 2u) << name;
	}
	EXPECT_EQ(service.GetBootFilesStatus().sha256, result.status.sha256);
}

TEST_F(ArcadeSetupTest, AnUpdateIsFoundByTheDigestGitHubGives)
{
	const std::string zip = MakeProverbZip({"NM00004"});
	{
		FakeFetcher http;
		ScriptDownload(http, zip, ReleaseJson("Nightly build of v1.0.4", zip.size(), Sha(zip)));
		Service service = MakeService();
		ASSERT_TRUE(service.DownloadBootFiles(http).ok);
	}
	Service service = MakeService();

	FakeFetcher same;
	same.Ok(BOOT_RELEASE_URL, ReleaseJson("Nightly build of v1.0.4", zip.size(), Sha(zip)));
	UpdateCheck check = service.CheckForBootFilesUpdate(same);
	EXPECT_EQ(check.state, UpdateCheck::State::UpToDate) << check.error;
	EXPECT_EQ(check.release_name, "Nightly build of v1.0.4");

	// The live release JSON describes another file than this one.
	FakeFetcher live;
	live.Ok(BOOT_RELEASE_URL, CatalogTest::ReadFixture("arcade/proverb-nightly-release.json"));
	check = service.CheckForBootFilesUpdate(live);
	EXPECT_EQ(check.state, UpdateCheck::State::UpdateAvailable) << check.error;
	EXPECT_EQ(check.size_bytes, 1195760u);
	EXPECT_EQ(check.release_time, 1785015176);

	// Without a digest: a different size says so, the same size does not.
	FakeFetcher other_size;
	other_size.Ok(BOOT_RELEASE_URL, ReleaseJson("x", zip.size() + 1, ""));
	EXPECT_EQ(service.CheckForBootFilesUpdate(other_size).state, UpdateCheck::State::UpdateAvailable);
	FakeFetcher same_size;
	same_size.Ok(BOOT_RELEASE_URL, ReleaseJson("x", zip.size(), ""));
	EXPECT_EQ(service.CheckForBootFilesUpdate(same_size).state, UpdateCheck::State::UpToDate);
}

TEST_F(ArcadeSetupTest, AnUpdateCheckSaysWhatAReleaseWouldGiveWhenNothingIsInstalled)
{
	FakeFetcher http;
	http.Ok(BOOT_RELEASE_URL, CatalogTest::ReadFixture("arcade/proverb-nightly-release.json"));
	Service service = MakeService();
	const UpdateCheck check = service.CheckForBootFilesUpdate(http);
	EXPECT_EQ(check.state, UpdateCheck::State::NotInstalled) << check.error;
	EXPECT_EQ(check.release_name, "Nightly build of v1.0.4");
	EXPECT_EQ(check.size_bytes, 1195760u);
	EXPECT_EQ(check.release_time, 1785015176);
}

TEST_F(ArcadeSetupTest, AnUpdateCheckThatCannotGetAnAnswerSaysSo)
{
	Service service = MakeService();
	{
		FakeFetcher http;
		http.Status(BOOT_RELEASE_URL, 429, R"({"message":"rate limit"})");
		const UpdateCheck check = service.CheckForBootFilesUpdate(http);
		EXPECT_EQ(check.state, UpdateCheck::State::Failed);
		EXPECT_NE(check.error.find("429"), std::string::npos) << check.error;
	}
	{
		FakeFetcher http;
		http.Fail(BOOT_RELEASE_URL, "no route");
		EXPECT_EQ(service.CheckForBootFilesUpdate(http).state, UpdateCheck::State::Failed);
	}
	{
		FakeFetcher http;
		http.Ok(BOOT_RELEASE_URL, "<html>not json</html>");
		EXPECT_EQ(service.CheckForBootFilesUpdate(http).state, UpdateCheck::State::Failed);
	}
	{
		FakeFetcher http;
		http.Ok(BOOT_RELEASE_URL, R"({"name":"x","assets":[{"name":"proverb.elf","size":1}]})");
		EXPECT_EQ(service.CheckForBootFilesUpdate(http).state, UpdateCheck::State::Failed);
	}
}

TEST_F(ArcadeSetupTest, TheArcadeBiosIsReadFromTheBiosFolder)
{
	Service service = MakeService();
	EXPECT_FALSE(service.GetBiosStatus().IsPresent());
	EXPECT_EQ(service.GetBiosStatus().Summary(), "");

	// A console BIOS is not one.
	PutBios("console.bin", ConsoleBios());
	EXPECT_FALSE(service.GetBiosStatus().IsPresent());

	PutBios("system256.bin", System256Bios());
	PutBios("system246.bin", System246Bios());
	PutBios("junk.bin", std::string(2 * MB, 'x'));
	const BiosStatus status = service.GetBiosStatus();
	ASSERT_EQ(status.dumps.size(), 2u);
	EXPECT_EQ(status.dumps[0].file_name, "system246.bin");
	EXPECT_EQ(status.dumps[0].board, "System 246 Rack C");
	EXPECT_EQ(status.dumps[1].file_name, "system256.bin");
	EXPECT_EQ(status.dumps[1].board, "System 256");
	EXPECT_EQ(status.Summary(), "System 246 Rack C, System 256");
}

TEST_F(ArcadeSetupTest, AGameRunsOnTheBiosTheCoreWouldPickForIt)
{
	std::string needs;
	EXPECT_EQ(FindBiosFor("NM00004", &needs), "");
	EXPECT_EQ(needs, "");

	PutBios("system256.bin", System256Bios());
	EXPECT_EQ(FindBiosFor("NM00004", &needs), "system256.bin");
	// Battle Gear 3 refuses the System 256 BIOS.
	EXPECT_EQ(FindBiosFor("NM00010", &needs), "");
	EXPECT_EQ(needs, "System 246");

	PutBios("system246.bin", System246Bios());
	EXPECT_EQ(FindBiosFor("NM00010", &needs), "system246.bin");
}

TEST_F(ArcadeSetupTest, AnAcgameLacksWhatIsNotThere)
{
	const std::string acgame = PutAcgame("NM00004");
	Service service = MakeService();

	MissingReport report = service.Missing(acgame);
	EXPECT_EQ(report.game_id, "NM00004");
	EXPECT_EQ(report.title, "Game NM00004"); // The .acgame's own name.
	EXPECT_FALSE(report.IsReady());
	EXPECT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::ArcadeBios, PartKind::BootProgram, PartKind::Image, PartKind::Dongle}));
	EXPECT_EQ(report.parts[1].detail, "boot.elf");
	EXPECT_EQ(report.parts[2].detail, "NM00004.chd");
	EXPECT_EQ(report.parts[3].detail, "NM00004.ps2");
	EXPECT_EQ(report.Summary(), "arcade BIOS, boot file, game image, dongle");

	PutBios("system256.bin", System256Bios());
	EXPECT_EQ(Kinds(service.Missing(acgame)), (std::vector<PartKind>{PartKind::BootProgram, PartKind::Image, PartKind::Dongle}));

	// The boot program comes from the downloaded boot files...
	InstallBootFiles({"NM00004"});
	EXPECT_EQ(Kinds(service.Missing(acgame)), (std::vector<PartKind>{PartKind::Image, PartKind::Dongle}));

	// ...the image is in the game's folder, found whatever its case...
	WriteFile(Games("NM00004/nm00004.CHD"), "image");
	EXPECT_EQ(Kinds(service.Missing(acgame)), (std::vector<PartKind>{PartKind::Dongle}));

	// ...and the dongle beside the .acgame, or in the memory cards folder.
	WriteFile(Games("NM00004.ps2"), "dongle");
	report = service.Missing(acgame);
	EXPECT_TRUE(report.IsReady()) << report.Summary();
	EXPECT_EQ(report.Summary(), "");

	std::filesystem::remove(Games("NM00004.ps2"));
	EXPECT_TRUE(Has(service.Missing(acgame), PartKind::Dongle));
	WriteFile(Cards("NM00004.ps2"), "dongle");
	EXPECT_TRUE(service.Missing(acgame).IsReady());
}

TEST_F(ArcadeSetupTest, AnAcgamesOwnBootProgramDoesNotNeedTheBootFiles)
{
	const std::string acgame = PutAcgame("NM00004");
	PutBios("system256.bin", System256Bios());
	WriteFile(Games("NM00004/boot.elf"), "own");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Cards("NM00004.ps2"), "dongle");
	EXPECT_TRUE(MakeService().Missing(acgame).IsReady());
}

TEST_F(ArcadeSetupTest, MissingChangesNothing)
{
	const std::string acgame = PutAcgame("NM00004");
	WriteFile(Games("NM00004/boot.elf"), "own");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Games("NM00004.ps2"), "dongle");
	PutBios("system256.bin", System256Bios());

	MakeService().Missing(acgame);
	EXPECT_FALSE(Exists(Cards("NM00004.ps2")));
	EXPECT_FALSE(FileSystem::DirectoryExists(m_folders.arcade.c_str()));
}

TEST_F(ArcadeSetupTest, AnAcgameWithACardNeedsItToo)
{
	const std::string acgame = PutAcgame("NM00004", "card=second.ps2\n");
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004"});
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Cards("NM00004.ps2"), "dongle");

	Service service = MakeService();
	const MissingReport report = service.Missing(acgame);
	ASSERT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::Card}));
	EXPECT_EQ(report.parts[0].detail, "second.ps2");

	WriteFile(Games("second.ps2"), "card");
	EXPECT_TRUE(service.Missing(acgame).IsReady());
}

TEST_F(ArcadeSetupTest, AGameThatRefusesTheBiosYouHaveNamesTheBoardItNeeds)
{
	const std::string acgame = PutAcgame("NM00010");
	PutBios("system256.bin", System256Bios());
	const MissingReport report = MakeService().Missing(acgame);
	ASSERT_TRUE(Has(report, PartKind::BoardBios));
	EXPECT_FALSE(Has(report, PartKind::ArcadeBios));
	EXPECT_EQ(report.parts[0].detail, "System 246");
	EXPECT_NE(report.Summary().find("System 246 BIOS"), std::string::npos) << report.Summary();

	PutBios("system246.bin", System246Bios());
	EXPECT_FALSE(Has(MakeService().Missing(acgame), PartKind::BoardBios));
}

TEST_F(ArcadeSetupTest, SomeGamesOnlyReachTheirAttractDemo)
{
	for (const char* id : {"NM00014", "NM00020", "NM00022"})
	{
		EXPECT_TRUE(IsAttractOnly(id)) << id;
		EXPECT_TRUE(MakeService().Missing(PutAcgame(id)).attract_only) << id;
	}
	EXPECT_FALSE(IsAttractOnly("NM00004"));
	EXPECT_FALSE(MakeService().Missing(PutAcgame("NM00004")).attract_only);
}

TEST_F(ArcadeSetupTest, AFileThatIsNoGameIsUnreadable)
{
	Service service = MakeService();
	WriteFile(Games("bad.acgame"), "[game]\nname=No ID\n");
	WriteFile(Games("no-id.chd"), "image");
	WriteFile(Games("NM00010.txt"), "text");
	for (const char* name : {"bad.acgame", "no-id.chd", "NM00010.txt", "does-not-exist.acgame", "NM00099.chd"})
	{
		const MissingReport report = service.Missing(Games(name));
		EXPECT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::Unreadable})) << name;
		EXPECT_EQ(report.game_id, "") << name;

		const PrepareResult prepared = service.Prepare(Games(name));
		EXPECT_FALSE(prepared.ok) << name;
		EXPECT_FALSE(prepared.error.empty()) << name;
	}
}

TEST_F(ArcadeSetupTest, ALooseImageLacksWhatIsNotThere)
{
	const std::string image = Games("loose/Battle Gear 3 (NM00010).chd");
	WriteFile(image, "image");
	Service service = MakeService();

	MissingReport report = service.Missing(image);
	EXPECT_EQ(report.game_id, "NM00010");
	EXPECT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::ArcadeBios, PartKind::BootProgram, PartKind::Dongle}));

	PutBios("system256.bin", System256Bios()); // Battle Gear 3 refuses it.
	EXPECT_EQ(Kinds(service.Missing(image)), (std::vector<PartKind>{PartKind::BoardBios, PartKind::BootProgram, PartKind::Dongle}));
	PutBios("system246.bin", System246Bios());
	InstallBootFiles({"NM00010"});
	EXPECT_EQ(Kinds(service.Missing(image)), (std::vector<PartKind>{PartKind::Dongle}));

	// A small .bin beside the image is a board ROM, not a dongle; a card file is.
	WriteFile(Games("loose/NM00010.bin"), std::string(1000, 'r'));
	EXPECT_TRUE(Has(service.Missing(image), PartKind::Dongle));
	WriteFile(Games("loose/NM00010.ps2"), Card('D', PS2_CARD));
	report = service.Missing(image);
	EXPECT_TRUE(report.IsReady()) << report.Summary();
}

TEST_F(ArcadeSetupTest, ALooseImageTakesItsGameFromItsFolderToo)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004"});
	WriteFile(Games("NM00004/disc.iso"), "image");
	WriteFile(Games("NM00004/dongle.bin"), Card('D', BIN_CARD));
	const MissingReport report = MakeService().Missing(Games("NM00004/disc.iso"));
	EXPECT_EQ(report.game_id, "NM00004");
	EXPECT_TRUE(report.IsReady()) << report.Summary();
}

TEST_F(ArcadeSetupTest, APackedImageCannotBePlayed)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004"});
	WriteFile(Games("NM00004.iso.gz"), std::string(100, 'z'));
	WriteFile(Games("NM00004.ps2"), Card('D', PS2_CARD));

	Service service = MakeService();
	const MissingReport report = service.Missing(Games("NM00004.iso.gz"));
	EXPECT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::PackedImage}));
	EXPECT_EQ(report.parts[0].detail, "NM00004.iso.gz");
	EXPECT_FALSE(service.Prepare(Games("NM00004.iso.gz")).ok);
	EXPECT_FALSE(Exists(Cards("NM00004.ps2")));
}

TEST_F(ArcadeSetupTest, EveryPartHasWordsForAList)
{
	for (const PartKind kind : {PartKind::ArcadeBios, PartKind::BoardBios, PartKind::BootProgram, PartKind::Image, PartKind::PackedImage,
			 PartKind::Dongle, PartKind::Card, PartKind::Unreadable})
	{
		const MissingPart part{kind, "THE-DETAIL"};
		EXPECT_FALSE(part.Short().empty());
		EXPECT_GT(part.Explain().size(), 20u);
		EXPECT_EQ(part.Short().find("{}"), std::string::npos) << part.Short();
		EXPECT_EQ(part.Explain().find("{}"), std::string::npos) << part.Explain();
	}
	// The ones that name a file or board say which.
	for (const PartKind kind : {PartKind::BoardBios, PartKind::BootProgram, PartKind::Image, PartKind::PackedImage, PartKind::Dongle, PartKind::Card})
		EXPECT_NE((MissingPart{kind, "THE-DETAIL"}).Explain().find("THE-DETAIL"), std::string::npos);
}

TEST_F(ArcadeSetupTest, AnAcgameIsReadyWhenItsDongleIsCopiedAndItsSramSeeded)
{
	const std::string acgame = PutAcgame("NM00004");
	WriteFile(Games("NM00004/boot.elf"), "own-elf");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Games("NM00004/sram.bin"), "seed");
	WriteFile(Games("NM00004.ps2"), "dongle-bytes"); // Beside the .acgame.
	PutBios("system256.bin", System256Bios());

	Service service = MakeService();
	PrepareResult result = service.Prepare(acgame);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_TRUE(result.missing.IsReady());
	EXPECT_EQ(result.game_id, "NM00004");
	EXPECT_EQ(result.title, "Game NM00004");
	EXPECT_EQ(result.acgame, acgame);
	EXPECT_EQ(result.elf, Games("NM00004/boot.elf"));
	EXPECT_EQ(result.media, Games("NM00004/NM00004.chd"));
	EXPECT_EQ(result.sram, Data("NM00004/sram.bin"));

	// The dongle is where the core mounts it from, and the SRAM, seeded from the one beside the game, is in the
	// data folder.
	EXPECT_EQ(ReadFile(Cards("NM00004.ps2")), "dongle-bytes");
	EXPECT_EQ(ReadFile(result.sram), "seed");

	// What the core writes to them is kept at the next launch.
	WriteFile(Cards("NM00004.ps2"), "written by the game");
	WriteFile(result.sram, "test menu settings");
	result = service.Prepare(acgame);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(ReadFile(Cards("NM00004.ps2")), "written by the game");
	EXPECT_EQ(ReadFile(result.sram), "test menu settings");
}

TEST_F(ArcadeSetupTest, AnAcgameWhoseDongleLeavesTheMemoryCardsFolderIsUnreadableAndCopiesNothing)
{
	// Everything else the game needs is there, and the file the dongle names exists beside the .acgame, so the
	// only thing standing in the way is the name.
	WriteFile(Games("NM00004/boot.elf"), "own-elf");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Games("NM00004/x.desktop"), "[Desktop Entry]");
	WriteFile(Games("x.desktop"), "[Desktop Entry]");
	PutBios("system256.bin", System256Bios());

	for (const char* key : {"dongle", "card"})
	{
		const std::string acgame = PutAcgame("NM00004", fmt::format("{}=../../x.desktop\n", key));
		Service service = MakeService();

		const MissingReport report = service.Missing(acgame);
		EXPECT_EQ(Kinds(report), (std::vector<PartKind>{PartKind::Unreadable})) << key;
		ASSERT_FALSE(report.parts.empty());
		EXPECT_NE(report.parts[0].Explain().find("x.desktop"), std::string::npos) << report.parts[0].Explain();

		const PrepareResult prepared = service.Prepare(acgame);
		EXPECT_FALSE(prepared.ok) << key;
		EXPECT_NE(prepared.error.find("x.desktop"), std::string::npos) << prepared.error;

		// m_folders.memory_cards is <dir>/data/memcards, so "../../x.desktop" would land in <dir>.
		EXPECT_FALSE(Exists(m_dir.File("x.desktop"))) << key;
		EXPECT_TRUE(NamesIn(m_dir.File("data/memcards")).empty()) << key;
	}
}

TEST_F(ArcadeSetupTest, WhatPrepareFoundGoesIntoTheBootParameters)
{
	const std::string acgame = PutAcgame("NM00004");
	WriteFile(Games("NM00004/boot.elf"), "own-elf");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Cards("NM00004.ps2"), "dongle");
	PutBios("system256.bin", System256Bios());

	const PrepareResult result = MakeService().Prepare(acgame);
	ASSERT_TRUE(result.ok) << result.error;

	// A caller that had a disc in hand: the disc source must go, or the arcade branch of Initialize() is skipped.
	VMBootParameters params;
	params.filename = "/games/some.iso";
	params.source_type = CDVD_SourceType::Iso;
	params.elf_override = "/games/other.elf";
	result.ApplyTo(&params);
	EXPECT_EQ(params.filename, acgame);
	EXPECT_FALSE(params.source_type.has_value());
	EXPECT_TRUE(params.elf_override.empty());
	EXPECT_EQ(params.arcade_elf, Games("NM00004/boot.elf"));
	EXPECT_EQ(params.arcade_media, Games("NM00004/NM00004.chd"));
	EXPECT_EQ(params.arcade_sram, Data("NM00004/sram.bin"));
	EXPECT_TRUE(VMManager::IsArcadeGameFileName(params.filename));
}

TEST_F(ArcadeSetupTest, AnAcgameWithoutABootProgramGetsOneFromTheBootFiles)
{
	const std::string acgame = PutAcgame("NM00004");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Cards("NM00004.ps2"), "dongle");
	PutBios("system256.bin", System256Bios());

	Service service = MakeService();
	PrepareResult result = service.Prepare(acgame);
	EXPECT_FALSE(result.ok);
	ASSERT_TRUE(Has(result.missing, PartKind::BootProgram));
	EXPECT_NE(result.error.find("boot.elf"), std::string::npos) << result.error;

	InstallBootFiles({"NM00004"});
	result = service.Prepare(acgame);
	ASSERT_TRUE(result.ok) << result.error;
	// Where the BIOS reads it through host:, in the data folder; the library folder is not written to.
	EXPECT_EQ(result.elf, Data("NM00004/boot.elf"));
	EXPECT_EQ(ReadFile(result.elf), BootElfOf("NM00004"));
	EXPECT_FALSE(Exists(Games("NM00004/boot.elf")));
}

TEST_F(ArcadeSetupTest, APathTheAcgameGotWrongInCaseStillBoots)
{
	// Names in an .acgame are typed by hand, and Linux cares about case.
	const std::string acgame = PutAcgame("NM00004", "elf=BOOT.ELF\nmediasrc=NM00004.CHD\nsram=SRAM.BIN\n");
	WriteFile(Games("NM00004/boot.elf"), "own-elf");
	WriteFile(Games("NM00004/nm00004.chd"), "image");
	WriteFile(Cards("NM00004.ps2"), "dongle");
	PutBios("system256.bin", System256Bios());

	const PrepareResult result = MakeService().Prepare(acgame);
	ASSERT_TRUE(result.ok) << result.error;

	// Where the filesystem ignores case (macOS, Windows) the name as typed already opens the file,
	// so it is kept as typed. Where case matters, only the name on disk will do.
	if (Exists(Games("NM00004/BOOT.ELF")))
	{
		EXPECT_EQ(StringUtil::Strcasecmp(result.elf.c_str(), Games("NM00004/boot.elf").c_str()), 0) << result.elf;
		EXPECT_EQ(StringUtil::Strcasecmp(result.media.c_str(), Games("NM00004/nm00004.chd").c_str()), 0) << result.media;
	}
	else
	{
		EXPECT_EQ(result.elf, Games("NM00004/boot.elf"));
		EXPECT_EQ(result.media, Games("NM00004/nm00004.chd"));
	}
}

TEST_F(ArcadeSetupTest, AnAcgameThatLacksSomethingIsLeftAlone)
{
	const std::string acgame = PutAcgame("NM00004");
	WriteFile(Games("NM00004/boot.elf"), "own-elf");
	WriteFile(Games("NM00004/NM00004.chd"), "image");
	WriteFile(Games("NM00004/sram.bin"), "seed");
	PutBios("system256.bin", System256Bios());

	const PrepareResult result = MakeService().Prepare(acgame);
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(Kinds(result.missing), (std::vector<PartKind>{PartKind::Dongle}));
	EXPECT_NE(result.error.find("NM00004.ps2"), std::string::npos) << result.error;
	EXPECT_FALSE(Exists(Data("NM00004/sram.bin")));
	EXPECT_FALSE(Exists(Cards("NM00004.ps2")));
}

TEST_F(ArcadeSetupTest, ALooseImageGetsAnAcgameAndItsDongleIsCopied)
{
	const std::string image = Games("Battle Gear 3 (NM00010).chd");
	const std::string dongle = Card('D', PS2_CARD);
	WriteFile(image, "image");
	WriteFile(Games("Battle Gear 3 (NM00010).ps2"), dongle);
	PutBios("system246.bin", System246Bios());
	InstallBootFiles({"NM00010"});

	Service service = MakeService();
	const PrepareResult result = service.Prepare(image);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(result.game_id, "NM00010");

	// The game stays where it is. What the core boots from is made in the data folder.
	EXPECT_EQ(result.acgame, Data("NM00010/NM00010.acgame"));
	EXPECT_EQ(result.media, image);
	EXPECT_EQ(result.elf, Data("NM00010/boot.elf"));
	EXPECT_EQ(ReadFile(result.elf), BootElfOf("NM00010"));
	EXPECT_EQ(result.sram, Data("NM00010/sram.bin"));
	EXPECT_TRUE(Exists(image));

	const auto info = Arcade::ReadAcGameInfo(result.acgame);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->gameid, "NM00010");
	EXPECT_EQ(info->name, result.title);
	EXPECT_EQ(info->dongle, "NM00010.ps2");
	EXPECT_EQ(info->card, "");
	EXPECT_EQ(info->base_dir, info->dir); // No game folder: [data] subdir is empty.
	EXPECT_EQ(info->elf_path, Data("NM00010/boot.elf"));

	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), dongle);
	EXPECT_EQ(NamesIn(m_folders.memory_cards), (std::vector<std::string>{"NM00010.ps2"})); // No temporary file left.

	VMBootParameters params;
	result.ApplyTo(&params);
	EXPECT_EQ(params.filename, result.acgame);
	EXPECT_EQ(params.arcade_media, image);
	EXPECT_EQ(params.arcade_elf, result.elf);
}

TEST_F(ArcadeSetupTest, ALooseImageNeedsTheBootFiles)
{
	const std::string image = Games("NM00004.chd");
	WriteFile(image, "image");
	WriteFile(Games("NM00004.ps2"), Card('D', PS2_CARD));
	PutBios("system256.bin", System256Bios());

	Service service = MakeService();
	const PrepareResult result = service.Prepare(image);
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(Kinds(result.missing), (std::vector<PartKind>{PartKind::BootProgram}));
	EXPECT_FALSE(Exists(Cards("NM00004.ps2")));
	EXPECT_FALSE(Exists(Data("NM00004/NM00004.acgame")));
}

TEST_F(ArcadeSetupTest, ADongleIsNamedByTheSizeItHas)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004", "NM00005", "NM00006", "NM00007"});
	Service service = MakeService();

	// A card with its spare bytes is a .ps2, one without a .bin, whatever the file beside the image is called.
	WriteFile(Games("a/NM00004.chd"), "image");
	WriteFile(Games("a/NM00004.bin"), Card('A', BIN_CARD));
	ASSERT_TRUE(service.Prepare(Games("a/NM00004.chd")).ok);
	EXPECT_EQ(ReadFile(Cards("NM00004.bin")), Card('A', BIN_CARD));

	WriteFile(Games("b/NM00005.chd"), "image");
	WriteFile(Games("b/NM00005.ps2"), Card('B', BIN_CARD)); // Named .ps2, holds a whole number of megabytes.
	ASSERT_TRUE(service.Prepare(Games("b/NM00005.chd")).ok);
	EXPECT_EQ(ReadFile(Cards("NM00005.bin")), Card('B', BIN_CARD));
	EXPECT_FALSE(Exists(Cards("NM00005.ps2")));

	// A packed dongle is unpacked on the way.
	WriteFile(Games("c/NM00006.chd"), "image");
	WriteFile(Games("c/NM00006.bin.gz"), Gzip(Card('C', PS2_CARD)));
	ASSERT_TRUE(service.Prepare(Games("c/NM00006.chd")).ok);
	EXPECT_EQ(ReadFile(Cards("NM00006.ps2")), Card('C', PS2_CARD));

	// A file too small to be a card is not a dongle, though its name says it is.
	WriteFile(Games("d/NM00007.chd"), "image");
	WriteFile(Games("d/NM00007.ps2"), Card('D', 1 * MB));
	const PrepareResult small = service.Prepare(Games("d/NM00007.chd"));
	EXPECT_FALSE(small.ok);
	EXPECT_TRUE(Has(small.missing, PartKind::Dongle));
	EXPECT_FALSE(Exists(Cards("NM00007.ps2")));
	EXPECT_FALSE(Exists(Cards("NM00007.bin")));
	for (const std::string& name : NamesIn(m_folders.memory_cards))
		EXPECT_NE(name[0], '.') << name; // No temporary file left.
}

TEST_F(ArcadeSetupTest, AMacsResourceForkBesideTheDongleDoesNotGetInItsWay)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004"});
	WriteFile(Games("NM00004.chd"), "image");
	WriteFile(Games("._NM00004.ps2"), "mac junk");
	WriteFile(Games("NM00004.ps2"), Card('D', PS2_CARD));
	ASSERT_TRUE(MakeService().Prepare(Games("NM00004.chd")).ok);
	EXPECT_EQ(ReadFile(Cards("NM00004.ps2")), Card('D', PS2_CARD));
}

TEST_F(ArcadeSetupTest, ADongleSwappedBesideTheImageReplacesTheCopy)
{
	PutBios("system246.bin", System246Bios());
	InstallBootFiles({"NM00010"});
	const std::string image = Games("g/NM00010.chd");
	WriteFile(image, "image");
	const std::string source_a = Games("g/NM00010.ps2");
	const std::string card_a = Card('A', PS2_CARD);
	const std::string from = Data("NM00010/dongle.from");
	Service service = MakeService();

	const auto lines = [&] {
		std::vector<std::string> out;
		std::string text = ReadFile(from), line;
		for (const char c : text)
		{
			if (c == '\n')
			{
				out.push_back(line);
				line.clear();
			}
			else
				line += c;
		}
		return out;
	};

	// The first launch copies it, and notes which file it came from: its name, its size, what it holds.
	WriteFile(source_a, card_a);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), card_a);
	EXPECT_EQ(lines(), (std::vector<std::string>{"NM00010.ps2", std::to_string(PS2_CARD), Sha(card_a)}));

	// The game writes to the copy; the next launch keeps what it wrote.
	std::string written = card_a;
	written[0] = 'W';
	WriteFile(Cards("NM00010.ps2"), written);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), written);

	// The same key under another name keeps the copy, and the note follows the file.
	std::filesystem::rename(source_a, Games("g/Battle Gear 3 (NM00010) japan.ps2"));
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), written);
	EXPECT_EQ(lines(), (std::vector<std::string>{"Battle Gear 3 (NM00010) japan.ps2", std::to_string(PS2_CARD), Sha(card_a)}));

	// A different key in its place replaces the copy.
	std::filesystem::remove(Games("g/Battle Gear 3 (NM00010) japan.ps2"));
	const std::string card_b = Card('B', PS2_CARD);
	WriteFile(source_a, card_b);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), card_b);
	EXPECT_EQ(lines(), (std::vector<std::string>{"NM00010.ps2", std::to_string(PS2_CARD), Sha(card_b)}));

	// One of the other kind (no spare bytes, so a .bin) in its place: the old copy goes, or the core would find
	// the wrong one first.
	std::filesystem::remove(source_a);
	const std::string card_c = Card('C', BIN_CARD);
	WriteFile(Games("g/NM00010.bin"), card_c);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.bin")), card_c);
	EXPECT_FALSE(Exists(Cards("NM00010.ps2")));
	EXPECT_EQ(lines(), (std::vector<std::string>{"NM00010.bin", std::to_string(BIN_CARD), Sha(card_c)}));

	// The key taken away with none in its place: the copy is kept.
	std::filesystem::remove(Games("g/NM00010.bin"));
	const PrepareResult kept = service.Prepare(image);
	ASSERT_TRUE(kept.ok) << kept.error;
	EXPECT_EQ(ReadFile(Cards("NM00010.bin")), card_c);
}

TEST_F(ArcadeSetupTest, ACopyMadeBeforeAnythingWasNotedIsTakenToBeFromTheFirstDongleThere)
{
	PutBios("system246.bin", System246Bios());
	InstallBootFiles({"NM00010"});
	const std::string image = Games("g/NM00010.chd");
	WriteFile(image, "image");
	const std::string card_a = Card('A', PS2_CARD);
	WriteFile(Games("g/NM00010.ps2"), card_a);
	// Put in the memory cards folder by hand, as a player does.
	WriteFile(Cards("NM00010.ps2"), "hand made copy");

	Service service = MakeService();
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), "hand made copy"); // Kept.
	EXPECT_EQ(ReadFile(Data("NM00010/dongle.from")), fmt::format("NM00010.ps2\n{}\n{}\n", PS2_CARD, Sha(card_a)));

	// Now the note is there, a swap is noticed.
	const std::string card_b = Card('B', PS2_CARD);
	std::filesystem::remove(Games("g/NM00010.ps2"));
	WriteFile(Games("g/other (NM00010).ps2"), card_b);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00010.ps2")), card_b);
}

TEST_F(ArcadeSetupTest, SoulCaliburIIGetsTheClearedConquestCard)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	WriteFile(Games("sc2/NM00007.ps2"), Card('D', PS2_CARD));

	Service service = MakeService();
	const PrepareResult result = service.Prepare(image);
	ASSERT_TRUE(result.ok) << result.error;

	// The core mounts a card of this name in slot 2; the .acgame names it too.
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), m_cleared);
	EXPECT_EQ(ReadFile(Cards("NM00007.ps2")), Card('D', PS2_CARD));
	const auto info = Arcade::ReadAcGameInfo(result.acgame);
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->card, "NM00007.conquestcard");
	EXPECT_EQ(info->dongle, "NM00007.ps2");
}

TEST_F(ArcadeSetupTest, OnlySoulCaliburIIGetsAConquestCard)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00004"});
	WriteFile(Games("NM00004.chd"), "image");
	WriteFile(Games("NM00004.ps2"), Card('D', PS2_CARD));
	const PrepareResult result = MakeService().Prepare(Games("NM00004.chd"));
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(NamesIn(m_folders.memory_cards), (std::vector<std::string>{"NM00004.ps2"}));
	EXPECT_EQ(Arcade::ReadAcGameInfo(result.acgame)->card, "");
}

TEST_F(ArcadeSetupTest, TheUnclearedConquestCardIsSwappedForTheClearedOneByItsHash)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	WriteFile(Games("sc2/NM00007.ps2"), Card('D', PS2_CARD));
	Service service = MakeService();

	// Exactly the card the 2.8.2 app shipped, which the game never wrote to: swapped.
	WriteFile(Cards("NM00007.conquestcard"), m_uncleared);
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), m_cleared);
	EXPECT_EQ(NamesIn(m_folders.memory_cards), (std::vector<std::string>{"NM00007.conquestcard", "NM00007.ps2"}));

	// A card the game wrote to is the player's: left alone, with every near miss of the old one.
	std::string written = m_uncleared;
	written[written.size() - 1] = 'W'; // Past the probe: only the hash tells.
	std::string probe_differs = m_uncleared;
	probe_differs[105] = 'W'; // Inside the probe.
	std::string other_size = m_uncleared + "x";
	for (const std::string& card : {written, probe_differs, other_size, m_cleared, ConquestCard('P')})
	{
		WriteFile(Cards("NM00007.conquestcard"), card);
		ASSERT_TRUE(service.Prepare(image).ok);
		EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), card);
	}
}

TEST_F(ArcadeSetupTest, AClearedCardThatIsTheWrongSizeIsNotSwappedIn)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	WriteFile(Games("sc2/NM00007.ps2"), Card('D', PS2_CARD));
	WriteFile(m_folders.conquest_card_gz, Gzip(m_cleared.substr(0, 1000))); // A damaged resource.

	// Over an uncleared card: it stays.
	WriteFile(Cards("NM00007.conquestcard"), m_uncleared);
	Service service = MakeService();
	ASSERT_TRUE(service.Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), m_uncleared);

	// With none: the game starts without one.
	std::filesystem::remove(Cards("NM00007.conquestcard"));
	const PrepareResult result = service.Prepare(image);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_FALSE(Exists(Cards("NM00007.conquestcard")));
	EXPECT_EQ(Arcade::ReadAcGameInfo(result.acgame)->card, "");
	for (const std::string& name : NamesIn(m_folders.memory_cards))
		EXPECT_NE(name[0], '.') << name;
}

TEST_F(ArcadeSetupTest, TheGameStartsWithoutAConquestCardWhenThereIsNoClearedOne)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	WriteFile(Games("NM00007.chd"), "image");
	WriteFile(Games("NM00007.ps2"), Card('D', PS2_CARD));
	std::filesystem::remove(m_folders.conquest_card_gz);

	const PrepareResult result = MakeService().Prepare(Games("NM00007.chd"));
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(NamesIn(m_folders.memory_cards), (std::vector<std::string>{"NM00007.ps2"}));
}

TEST_F(ArcadeSetupTest, TheConquestCardBesideTheImageComesBeforeTheClearedOne)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	// The player's own card (a size and header the game reads), and the dongle: told apart by what they hold.
	const std::string own = ConquestCard('P');
	WriteFile(Games("sc2/Soul Calibur II (NM00007).ps2"), own);
	WriteFile(Games("sc2/NM00007.bin"), Card('D', BIN_CARD));

	Service service = MakeService();
	const PrepareResult result = service.Prepare(image);
	ASSERT_TRUE(result.ok) << result.error;
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), own);
	EXPECT_EQ(ReadFile(Cards("NM00007.bin")), Card('D', BIN_CARD)); // The dongle is the other file.
	EXPECT_FALSE(Exists(Cards("NM00007.ps2")));
}

TEST_F(ArcadeSetupTest, ACardBesideTheImageThatLacksItsSpareBytesIsNotUsed)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	WriteFile(Games("sc2/NM00007.ps2"), Card('D', PS2_CARD));
	WriteFile(Games("sc2/NM00007.conquestcard"), ConquestCard('P', 32 * 1024)); // Dumped without its checksums.

	ASSERT_TRUE(MakeService().Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), m_cleared);
}

TEST_F(ArcadeSetupTest, AConquestCardNamedAsTheDongleIsMovedOutOfTheDongleCopysWay)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	const std::string image = Games("sc2/NM00007.chd");
	WriteFile(image, "image");
	WriteFile(Games("sc2/NM00007.ps2"), Card('D', PS2_CARD));
	// A Conquest card that sits in the memory cards folder as NM00007.ps2 would be written over by the dongle.
	const std::string own = ConquestCard('S');
	WriteFile(Cards("NM00007.ps2"), own);

	ASSERT_TRUE(MakeService().Prepare(image).ok);
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), own);
	EXPECT_EQ(ReadFile(Cards("NM00007.ps2")), Card('D', PS2_CARD));
}

TEST_F(ArcadeSetupTest, SoulCaliburIIAsAnAcgameGetsItsConquestCardUnlessTheAcgameNamesOne)
{
	PutBios("system256.bin", System256Bios());
	InstallBootFiles({"NM00007"});
	WriteFile(Games("NM00007/NM00007.chd"), "image");
	WriteFile(Cards("NM00007.ps2"), Card('D', PS2_CARD));
	Service service = MakeService();

	ASSERT_TRUE(service.Prepare(PutAcgame("NM00007")).ok);
	EXPECT_EQ(ReadFile(Cards("NM00007.conquestcard")), m_cleared);

	// An .acgame that names a card has chosen its own: none is added.
	std::filesystem::remove(Cards("NM00007.conquestcard"));
	WriteFile(Cards("mine.ps2"), "my card");
	ASSERT_TRUE(service.Prepare(PutAcgame("NM00007", "card=mine.ps2\n")).ok);
	EXPECT_FALSE(Exists(Cards("NM00007.conquestcard")));
}
