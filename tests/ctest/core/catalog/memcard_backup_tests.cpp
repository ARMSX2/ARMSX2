// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Memory-card backups (pcsx2/SIO/Memcard/MemcardBackup.h) on synthetic card images in temp
// folders: card verification, snapshots and the dedupe by hash, the retention rule across dates,
// restore with its rollback, and the boot-time flow around the restore prompt. No real saves.

#include "catalog_test_support.h"

#include "SIO/Memcard/MemcardBackup.h"

#include "Catalog/Json.h"
#include "Config.h"

#include "common/Error.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"
#include "common/SHA256.h"
#include "common/ZipHelpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace MemcardBackup;
using CatalogTest::ScopedTempDir;

namespace
{
	namespace fs = std::filesystem;

	constexpr size_t CARD_SIZE = 256 * 1024; // above the 128 KiB floor, small enough to be quick

	void WriteFile(const fs::path& path, const std::string& content)
	{
		fs::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
	}

	std::string ReadFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	// A formatted PS2 card image; `seed` makes the contents differ.
	std::string CardImage(char seed, size_t size = CARD_SIZE)
	{
		std::string data(size, '\xff');
		static const char header[] = "Sony PS2 Memory Card Format 1.2.0.0";
		std::memcpy(data.data(), header, sizeof(header) - 1);
		for (size_t i = 512; i < size; i += 1031)
			data[i] = static_cast<char>(seed + static_cast<char>(i % 13));
		return data;
	}

	std::string Ps1CardImage()
	{
		std::string data(128 * 1024, '\0');
		data[0] = 'M';
		data[1] = 'C';
		return data;
	}

	// The files of a folder card; `seed` makes the contents differ.
	void MakeFolderCard(const fs::path& dir, char seed)
	{
		WriteFile(dir / "_pcsx2_superblock", std::string(64, seed));
		WriteFile(dir / "_pcsx2_index", "index");
		WriteFile(dir / "BASLUS-12345" / "icon.sys", std::string(100, seed));
		WriteFile(dir / "BASLUS-12345" / "data" / "save.bin", std::string(1000, static_cast<char>(seed + 1)));
	}

	// Every file under a folder: relative path -> content. For comparing whole folders.
	std::map<std::string, std::string> Tree(const fs::path& root)
	{
		std::map<std::string, std::string> files;
		if (!fs::exists(root))
			return files;
		for (const auto& entry : fs::recursive_directory_iterator(root))
		{
			if (entry.is_regular_file())
				files[fs::relative(entry.path(), root).generic_string()] = ReadFile(entry.path());
		}
		return files;
	}

	s64 LocalMs(int year, int month, int day, int hour, int minute = 0)
	{
		std::tm tm = {};
		tm.tm_year = year - 1900;
		tm.tm_mon = month - 1;
		tm.tm_mday = day;
		tm.tm_hour = hour;
		tm.tm_min = minute;
		tm.tm_isdst = -1;
		return static_cast<s64>(std::mktime(&tm)) * 1000;
	}

	struct ZipEntrySpec
	{
		std::string name;
		std::string data;
	};

	// Writes a zip of the given entries. Raw libzip so a test can build exactly the shape it wants,
	// including ones the service would never write.
	bool WriteZip(const std::string& path, const std::vector<ZipEntrySpec>& entries)
	{
		int err = 0;
		zip_t* za = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
		if (!za)
			return false;
		for (const ZipEntrySpec& e : entries)
		{
			zip_source_t* src = zip_source_buffer(za, e.data.data(), e.data.size(), 0);
			if (!src || zip_file_add(za, e.name.c_str(), src, ZIP_FL_ENC_UTF_8) < 0)
			{
				zip_discard(za);
				return false;
			}
		}
		const bool ok = (zip_close(za) == 0);
		if (!ok)
			zip_discard(za);
		return ok;
	}

	std::vector<std::string> ZipNames(const std::string& path)
	{
		std::vector<std::string> names;
		zip_error_t ze = {};
		auto za = zip_open_managed(path.c_str(), ZIP_RDONLY, &ze);
		if (!za)
			return names;
		const zip_int64_t n = zip_get_num_entries(za.get(), 0);
		for (zip_int64_t i = 0; i < n; i++)
			names.push_back(zip_get_name(za.get(), static_cast<zip_uint64_t>(i), 0));
		return names;
	}

	std::string ZipText(const std::string& path, const char* name)
	{
		zip_error_t ze = {};
		auto za = zip_open_managed(path.c_str(), ZIP_RDONLY, &ze);
		if (!za)
			return {};
		return ReadFileInZipToString(za.get(), name).value_or(std::string());
	}

	// The manifest JSON the Android app writes, as org.json prints it.
	std::string AndroidManifest(const std::string& card, s64 taken_at, const char* reason, u64 size, const std::string& hash,
		bool healthy, bool folder, const std::string& game)
	{
		std::ostringstream out;
		out << "{\"schemaVersion\":1,\"card\":\"" << card << "\",\"takenAt\":" << taken_at << ",\"reason\":\"" << reason
			<< "\",\"sizeBytes\":" << size << ",\"hash\":\"" << hash << "\",\"healthy\":" << (healthy ? "true" : "false")
			<< ",\"folder\":" << (folder ? "true" : "false") << ",\"game\":\"" << game << "\",\"appVersion\":\"1.0\"}";
		return out.str();
	}

	class Fixture
	{
	public:
		Fixture()
			: m_store(m_root.File("memcards"), m_root.File("memcard-backups"))
		{
			fs::create_directories(m_root.File("memcards"));
			m_store.SetClock([this]() { return m_now += 3'600'000; });
		}

		Store& store() { return m_store; }
		std::string CardPath(const std::string& name) const { return m_root.File("memcards/" + name); }
		std::string BackupDir(const std::string& card) const { return m_root.File("memcard-backups/" + card); }
		const ScopedTempDir& root() const { return m_root; }

		void PutCard(const std::string& name, const std::string& image) { WriteFile(CardPath(name), image); }

		void SetNow(s64 ms) { m_now = ms; }

		// A snapshot zip for `card` as another version of the app would have left it.
		std::string PutSnapshot(const std::string& card, s64 taken_at, bool healthy, const std::string& image,
			const char* reason = "SESSION")
		{
			const std::string hash = SHA256Digest::HashHex(image.data(), image.size());
			const std::string path = BackupDir(card) + "/" + std::to_string(taken_at) + "-" + reason + ".zip";
			fs::create_directories(BackupDir(card));
			EXPECT_TRUE(WriteZip(path, {{"armsx2-mcbak.json", AndroidManifest(card, taken_at, reason, image.size(), hash, healthy, false, "SLUS-12345")},
										   {"card/" + card, image}}));
			return path;
		}

	private:
		ScopedTempDir m_root;
		Store m_store;
		s64 m_now = LocalMs(2026, 3, 10, 12);
	};

	size_t Count(Fixture& f, const std::string& card) { return f.store().List(card).size(); }

	// The real file operations, except that the listed renames (1-based) fail.
	class FailingOps final : public Catalog::FileOps
	{
	public:
		explicit FailingOps(std::set<int> fail)
			: m_fail(std::move(fail))
		{
		}
		bool Exists(const std::string& path) override { return m_real.Exists(path); }
		bool Remove(const std::string& path) override { return m_real.Remove(path); }
		bool Rename(const std::string& from, const std::string& to) override
		{
			m_renames++;
			if (m_fail.count(m_renames))
				return false;
			return m_real.Rename(from, to);
		}

	private:
		Catalog::RealFileOps m_real;
		std::set<int> m_fail;
		int m_renames = 0;
	};
} // namespace

TEST(MemcardVerify, AFormattedPs2CardIsGood)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	EXPECT_EQ(Verify(f.CardPath("a.ps2")), Health::Good);
}

TEST(MemcardVerify, AFormattedPs1CardIsGood)
{
	Fixture f;
	f.PutCard("a.mcr", Ps1CardImage());
	EXPECT_EQ(Verify(f.CardPath("a.mcr")), Health::Good);
}

TEST(MemcardVerify, AMissingCardIsMissing)
{
	Fixture f;
	EXPECT_EQ(Verify(f.CardPath("nope.ps2")), Health::Missing);
}

TEST(MemcardVerify, AShortFileIsUnreadableEvenWithTheRightHeader)
{
	Fixture f;
	f.PutCard("short.ps2", CardImage('a', 100 * 1024));
	EXPECT_EQ(Verify(f.CardPath("short.ps2")), Health::Unreadable);
	f.PutCard("empty.ps2", "");
	EXPECT_EQ(Verify(f.CardPath("empty.ps2")), Health::Unreadable);
	// Exactly the floor is fine.
	f.PutCard("floor.ps2", CardImage('a', 128 * 1024));
	EXPECT_EQ(Verify(f.CardPath("floor.ps2")), Health::Good);
}

TEST(MemcardVerify, ABlankCardIsUnreadableUntilItIsFormatted)
{
	Fixture f;
	f.PutCard("blank.ps2", std::string(CARD_SIZE, '\xff'));
	EXPECT_EQ(Verify(f.CardPath("blank.ps2")), Health::Unreadable);
	f.PutCard("zero.ps2", std::string(CARD_SIZE, '\0'));
	EXPECT_EQ(Verify(f.CardPath("zero.ps2")), Health::Unreadable);
}

TEST(MemcardVerify, ADamagedHeaderIsUnreadable)
{
	Fixture f;
	std::string image = CardImage('a');
	image[3] = 'X';
	f.PutCard("bad.ps2", image);
	EXPECT_EQ(Verify(f.CardPath("bad.ps2")), Health::Unreadable);
}

TEST(MemcardVerify, AFolderCardNeedsItsSuperblock)
{
	Fixture f;
	MakeFolderCard(f.CardPath("folder.ps2"), 'a');
	EXPECT_EQ(Verify(f.CardPath("folder.ps2")), Health::Good);

	fs::remove(f.CardPath("folder.ps2") + "/_pcsx2_superblock");
	EXPECT_EQ(Verify(f.CardPath("folder.ps2")), Health::Unreadable);

	fs::create_directories(f.CardPath("empty.ps2"));
	EXPECT_EQ(Verify(f.CardPath("empty.ps2")), Health::Unreadable);
}

TEST(MemcardHash, AFileCardHashesItsBytes)
{
	Fixture f;
	const std::string image = CardImage('a');
	f.PutCard("a.ps2", image);
	std::string hex;
	ASSERT_TRUE(HashCard(f.CardPath("a.ps2"), &hex));
	EXPECT_EQ(hex, SHA256Digest::HashHex(image.data(), image.size()));
}

TEST(MemcardHash, AFolderCardHashesPathLengthAndBytesInPathOrder)
{
	Fixture f;
	const std::string a = "alpha-contents";
	const std::string b = std::string(300, 'b');
	WriteFile(f.CardPath("f.ps2") + "/z.bin", b); // created first, but sorts last
	WriteFile(f.CardPath("f.ps2") + "/a/x.txt", a);

	SHA256Digest expect;
	expect.Update("a/x.txt");
	expect.Update(std::string_view("\0", 1));
	const u8 len_a[8] = {0, 0, 0, 0, 0, 0, 0, static_cast<u8>(a.size())};
	expect.Update(len_a, 8);
	expect.Update(a);
	expect.Update("z.bin");
	expect.Update(std::string_view("\0", 1));
	const u8 len_b[8] = {0, 0, 0, 0, 0, 0, 1, 44}; // 300
	expect.Update(len_b, 8);
	expect.Update(b);

	std::string hex;
	ASSERT_TRUE(HashCard(f.CardPath("f.ps2"), &hex));
	EXPECT_EQ(hex, expect.FinalHex());
}

TEST(MemcardHash, AFolderCardHashChangesWithContentNameOrAddedFile)
{
	Fixture f;
	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	std::string base, changed;
	ASSERT_TRUE(HashCard(f.CardPath("f.ps2"), &base));

	WriteFile(f.CardPath("f.ps2") + "/BASLUS-12345/icon.sys", std::string(100, 'z'));
	ASSERT_TRUE(HashCard(f.CardPath("f.ps2"), &changed));
	EXPECT_NE(changed, base);

	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	ASSERT_TRUE(HashCard(f.CardPath("f.ps2"), &changed));
	EXPECT_EQ(changed, base);

	fs::rename(f.CardPath("f.ps2") + "/_pcsx2_index", f.CardPath("f.ps2") + "/_pcsx2_index2");
	ASSERT_TRUE(HashCard(f.CardPath("f.ps2"), &changed));
	EXPECT_NE(changed, base);
}

TEST(MemcardHash, AMissingFileFails)
{
	Fixture f;
	std::string hex;
	Error error;
	EXPECT_FALSE(HashCard(f.CardPath("nope.ps2"), &hex, &error));
}

TEST(MemcardNames, OnlyPlainFileNamesAreCardNames)
{
	EXPECT_TRUE(IsPlainCardName("Mcd001.ps2"));
	EXPECT_TRUE(IsPlainCardName("My Card (2).ps2"));
	EXPECT_FALSE(IsPlainCardName(""));
	EXPECT_FALSE(IsPlainCardName("."));
	EXPECT_FALSE(IsPlainCardName(".."));
	EXPECT_FALSE(IsPlainCardName("../x.ps2"));
	EXPECT_FALSE(IsPlainCardName("a/b.ps2"));
	EXPECT_FALSE(IsPlainCardName("a\\b.ps2"));
}

TEST(MemcardSnapshot, ACardThatIsNotThereIsAbsentAndLeavesNothing)
{
	Fixture f;
	EXPECT_EQ(f.store().SnapshotIfChanged("nope.ps2", Reason::Session, ""), CardStatus::Absent);
	EXPECT_FALSE(fs::exists(f.BackupDir("nope.ps2")));
}

TEST(MemcardSnapshot, AnUnreadableCardIsNotSnapshotted)
{
	Fixture f;
	f.PutCard("blank.ps2", std::string(CARD_SIZE, '\xff'));
	EXPECT_EQ(f.store().SnapshotIfChanged("blank.ps2", Reason::Session, ""), CardStatus::Unreadable);
	EXPECT_FALSE(fs::exists(f.BackupDir("blank.ps2")));
}

TEST(MemcardSnapshot, AGoodCardIsSnapshottedWithItsManifest)
{
	Fixture f;
	const std::string image = CardImage('a');
	f.PutCard("a.ps2", image);
	f.SetNow(1'760'000'000'000);

	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, "SLUS-12345"), CardStatus::Snapshotted);

	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 1u);
	const Snapshot& s = list[0];
	EXPECT_EQ(s.card, "a.ps2");
	EXPECT_EQ(s.taken_at_ms, 1'760'003'600'000);
	EXPECT_EQ(s.reason, Reason::Session);
	EXPECT_EQ(s.size_bytes, image.size());
	EXPECT_EQ(s.hash, SHA256Digest::HashHex(image.data(), image.size()));
	EXPECT_TRUE(s.healthy);
	EXPECT_EQ(s.game, "SLUS-12345");
	EXPECT_FALSE(s.is_folder);
	EXPECT_EQ(fs::path(s.path).filename().string(), "1760003600000-session.zip");
	EXPECT_EQ(fs::path(s.path).parent_path().string(), f.BackupDir("a.ps2"));
	// Written under its final name; no leftover.
	for (const auto& entry : fs::directory_iterator(f.BackupDir("a.ps2")))
		EXPECT_EQ(entry.path().extension().string(), ".zip");
}

TEST(MemcardSnapshot, TheZipHoldsTheManifestAndTheCardInTheAndroidLayout)
{
	Fixture f;
	const std::string image = CardImage('a');
	f.PutCard("a.ps2", image);
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, "SLUS-12345"), CardStatus::Snapshotted);
	const std::string path = f.store().List("a.ps2")[0].path;

	EXPECT_EQ(ZipNames(path), (std::vector<std::string>{"armsx2-mcbak.json", "card/a.ps2"}));
	EXPECT_EQ(ZipText(path, "card/a.ps2"), image);

	rapidjson::Document doc;
	ASSERT_TRUE(Catalog::ParseJson(ZipText(path, "armsx2-mcbak.json"), &doc));
	ASSERT_TRUE(doc.IsObject());
	EXPECT_EQ(doc["schemaVersion"].GetInt(), 1);
	EXPECT_STREQ(doc["card"].GetString(), "a.ps2");
	EXPECT_STREQ(doc["reason"].GetString(), "SESSION");
	EXPECT_EQ(doc["sizeBytes"].GetInt64(), static_cast<s64>(image.size()));
	EXPECT_STREQ(doc["hash"].GetString(), SHA256Digest::HashHex(image.data(), image.size()).c_str());
	EXPECT_TRUE(doc["healthy"].GetBool());
	EXPECT_FALSE(doc["folder"].GetBool());
	EXPECT_STREQ(doc["game"].GetString(), "SLUS-12345");
	EXPECT_TRUE(doc.HasMember("takenAt"));
	EXPECT_TRUE(doc.HasMember("appVersion"));
}

TEST(MemcardSnapshot, AFolderCardIsSnapshottedFileByFile)
{
	Fixture f;
	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	ASSERT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Snapshotted);

	const auto list = f.store().List("f.ps2");
	ASSERT_EQ(list.size(), 1u);
	EXPECT_TRUE(list[0].is_folder);
	EXPECT_TRUE(list[0].healthy);
	EXPECT_EQ(list[0].size_bytes, 64u + 5u + 100u + 1000u);

	std::set<std::string> names;
	for (const std::string& n : ZipNames(list[0].path))
		names.insert(n);
	EXPECT_EQ(names, (std::set<std::string>{"armsx2-mcbak.json", "card/_pcsx2_superblock", "card/_pcsx2_index",
						 "card/BASLUS-12345/icon.sys", "card/BASLUS-12345/data/save.bin"}));
	EXPECT_EQ(ZipText(list[0].path, "card/BASLUS-12345/data/save.bin"), std::string(1000, 'b'));
}

TEST(MemcardSnapshot, ACardUnchangedSinceTheNewestSnapshotIsSkipped)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Unchanged);
	EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Unchanged);
	EXPECT_EQ(Count(f, "a.ps2"), 1u);

	// Rewritten with the same bytes (new time stamp): still the same card.
	f.PutCard("a.ps2", CardImage('a'));
	EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Unchanged);
	EXPECT_EQ(Count(f, "a.ps2"), 1u);
}

TEST(MemcardSnapshot, AChangedCardIsSnapshottedAgain)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	f.PutCard("a.ps2", CardImage('b'));
	EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_EQ(Count(f, "a.ps2"), 2u);
}

TEST(MemcardSnapshot, OnlyTheNewestSnapshotIsComparedSoAThenBThenAMakesThree)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	f.PutCard("a.ps2", CardImage('b'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	f.PutCard("a.ps2", CardImage('a'));
	EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_EQ(Count(f, "a.ps2"), 3u);
}

TEST(MemcardSnapshot, AFolderCardChangeIsNoticed)
{
	Fixture f;
	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	ASSERT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Unchanged);
	WriteFile(f.CardPath("f.ps2") + "/BASLUS-12345/data/save.bin", "changed");
	EXPECT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Snapshotted);
}

TEST(MemcardSnapshot, TwoSnapshotsInTheSameMillisecondGetDifferentNames)
{
	Fixture f;
	f.store().SetClock([]() { return static_cast<s64>(1'700'000'000'000); });
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	f.PutCard("a.ps2", CardImage('b'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 2u);
	EXPECT_NE(list[0].path, list[1].path);
}

TEST(MemcardSnapshot, AStaleUnfinishedFileIsSweptAndNeverListed)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	fs::create_directories(f.BackupDir("a.ps2"));
	WriteFile(f.BackupDir("a.ps2") + "/1-session.zip.part", "half a zip");
	EXPECT_EQ(Count(f, "a.ps2"), 0u);
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_FALSE(fs::exists(f.BackupDir("a.ps2") + "/1-session.zip.part"));
}

TEST(MemcardSnapshot, ASnapshotThatCannotBeWrittenIsReportedAndLeavesNoFile)
{
	ScopedTempDir root;
	fs::create_directories(root.File("memcards"));
	WriteFile(root.File("blocker"), "a file where the backup folder should be");
	Store store(root.File("memcards"), root.File("blocker"));
	WriteFile(root.File("memcards/a.ps2"), CardImage('a'));
	EXPECT_EQ(store.SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Failed);
}

TEST(MemcardBackUpNow, SnapshotsEvenWhenNothingChangedAndRefusesACardThatDoesNotVerify)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	std::string error;
	ASSERT_TRUE(f.store().BackUpNow("a.ps2", "", &error).has_value()) << error;
	const auto second = f.store().BackUpNow("a.ps2", "", &error);
	ASSERT_TRUE(second.has_value()) << error;
	EXPECT_EQ(second->reason, Reason::Manual);
	EXPECT_EQ(Count(f, "a.ps2"), 2u);

	f.PutCard("blank.ps2", std::string(CARD_SIZE, '\xff'));
	EXPECT_FALSE(f.store().BackUpNow("blank.ps2", "", &error).has_value());
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(f.store().BackUpNow("nope.ps2", "", &error).has_value());
}

TEST(MemcardList, IsNewestFirstAndReadsSnapshotsFromTheAndroidApp)
{
	Fixture f;
	f.PutSnapshot("a.ps2", 1'000'000, true, CardImage('a'));
	f.PutSnapshot("a.ps2", 3'000'000, true, CardImage('b'), "MANUAL");
	f.PutSnapshot("a.ps2", 2'000'000, false, CardImage('c'), "PRE_RESTORE");

	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 3u);
	EXPECT_EQ(list[0].taken_at_ms, 3'000'000);
	EXPECT_EQ(list[0].reason, Reason::Manual);
	EXPECT_TRUE(list[0].healthy);
	EXPECT_EQ(list[1].taken_at_ms, 2'000'000);
	EXPECT_EQ(list[1].reason, Reason::PreRestore);
	EXPECT_FALSE(list[1].healthy);
	EXPECT_EQ(list[2].taken_at_ms, 1'000'000);
	EXPECT_EQ(list[2].game, "SLUS-12345");
	EXPECT_EQ(list[2].card, "a.ps2");
}

TEST(MemcardList, AZipWithAnUnreadableManifestIsListedUnhealthyWithTheTimeFromItsName)
{
	Fixture f;
	fs::create_directories(f.BackupDir("a.ps2"));
	WriteFile(f.BackupDir("a.ps2") + "/1760000000000-session.zip", "this is not a zip");
	ASSERT_TRUE(WriteZip(f.BackupDir("a.ps2") + "/1750000000000-manual.zip", {{"card/a.ps2", "x"}}));
	WriteFile(f.BackupDir("a.ps2") + "/notes.txt", "not a backup");

	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].taken_at_ms, 1'760'000'000'000);
	EXPECT_FALSE(list[0].healthy);
	EXPECT_EQ(list[1].taken_at_ms, 1'750'000'000'000);
	EXPECT_FALSE(list[1].healthy);
	EXPECT_EQ(list[1].card, "a.ps2");
}

TEST(MemcardList, NewestHealthySkipsUnhealthyOnes)
{
	Fixture f;
	f.PutSnapshot("a.ps2", 1'000, true, CardImage('a'));
	f.PutSnapshot("a.ps2", 2'000, false, CardImage('b'), "PRE_RESTORE");
	f.PutSnapshot("a.ps2", 3'000, false, CardImage('c'), "PRE_RESTORE");
	const auto healthy = f.store().NewestHealthy("a.ps2");
	ASSERT_TRUE(healthy.has_value());
	EXPECT_EQ(healthy->taken_at_ms, 1'000);
	EXPECT_FALSE(f.store().NewestHealthy("none.ps2").has_value());
}

TEST(MemcardList, CardsWithBackupsLeavesOutTheWorkFolder)
{
	Fixture f;
	f.PutSnapshot("b.ps2", 1, true, CardImage('a'));
	f.PutSnapshot("a.ps2", 1, true, CardImage('a'));
	fs::create_directories(f.root().File("memcard-backups/.restore"));
	EXPECT_EQ(f.store().CardsWithBackups(), (std::vector<std::string>{"a.ps2", "b.ps2"}));
}

TEST(MemcardList, ACardNameWithForbiddenCharactersGetsASafeFolderAndStillListsRight)
{
	Fixture f;
	f.PutCard("a:b.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a:b.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	EXPECT_TRUE(fs::exists(f.root().File("memcard-backups/a_b.ps2")));
	const auto list = f.store().List("a:b.ps2");
	ASSERT_EQ(list.size(), 1u);
	EXPECT_EQ(list[0].card, "a:b.ps2");
	// A different card whose name sanitises to the same folder does not see it.
	EXPECT_TRUE(f.store().List("a_b.ps2").empty());
}

namespace
{
	std::set<s64> Kept(Fixture& f, const std::string& card)
	{
		std::set<s64> times;
		for (const Snapshot& s : f.store().List(card))
			times.insert(s.taken_at_ms);
		return times;
	}
} // namespace

TEST(MemcardPrune, KeepsTheNewestThreeFromOneAfternoon)
{
	Fixture f;
	std::set<s64> all;
	for (int minute = 0; minute < 10; minute++)
	{
		const s64 t = LocalMs(2026, 3, 10, 12, minute * 5);
		all.insert(t);
		f.PutSnapshot("a.ps2", t, true, CardImage(static_cast<char>('a' + minute)));
	}
	f.store().Prune("a.ps2");

	std::set<s64> expected;
	auto it = all.rbegin();
	for (int i = 0; i < 3; i++, ++it)
		expected.insert(*it);
	EXPECT_EQ(Kept(f, "a.ps2"), expected);
}

TEST(MemcardPrune, KeepsTheNewestOfEachOfTheNextFourDays)
{
	Fixture f;
	// One snapshot a day for ten days, 10 March back to 1 March, at noon.
	std::vector<s64> days;
	for (int day = 10; day >= 1; day--)
	{
		days.push_back(LocalMs(2026, 3, day, 12));
		f.PutSnapshot("a.ps2", days.back(), true, CardImage(static_cast<char>('a' + day)));
	}
	f.store().Prune("a.ps2");

	// The newest three (10, 9, 8 March) and the four days after them (7, 6, 5, 4); 3, 2, 1 go.
	std::set<s64> expected(days.begin(), days.begin() + 7);
	EXPECT_EQ(Kept(f, "a.ps2"), expected);
}

TEST(MemcardPrune, ADayWithSeveralSnapshotsKeepsOnlyItsNewestAfterTheNewestThree)
{
	Fixture f;
	std::vector<s64> day10, day9, day8, day7, day6, day5, day4;
	for (int h = 8; h <= 12; h++)
		day10.push_back(LocalMs(2026, 3, 10, h));
	for (int h = 9; h <= 10; h++)
		day9.push_back(LocalMs(2026, 3, 9, h));
	for (int h = 9; h <= 10; h++)
		day8.push_back(LocalMs(2026, 3, 8, h));
	for (int h = 9; h <= 10; h++)
		day7.push_back(LocalMs(2026, 3, 7, h));
	for (int h = 9; h <= 10; h++)
		day6.push_back(LocalMs(2026, 3, 6, h));
	for (int h = 9; h <= 10; h++)
		day5.push_back(LocalMs(2026, 3, 5, h));
	for (int h = 9; h <= 10; h++)
		day4.push_back(LocalMs(2026, 3, 4, h));
	char seed = 'a';
	for (const auto* day : {&day10, &day9, &day8, &day7, &day6, &day5, &day4})
	{
		for (s64 t : *day)
			f.PutSnapshot("a.ps2", t, true, CardImage(seed++));
	}
	f.store().Prune("a.ps2");

	// Newest three are all on the 10th (12:00, 11:00, 10:00). The next four distinct days are the
	// 9th, 8th, 7th and 6th, each represented by its newest (10:00). The 10th's earlier ones, the
	// 9:00 copies and the 5th and 4th are gone.
	const std::set<s64> expected = {
		day10[4], day10[3], day10[2],
		day9[1], day8[1], day7[1], day6[1]};
	EXPECT_EQ(Kept(f, "a.ps2"), expected);
}

TEST(MemcardPrune, TheNewestHealthySnapshotIsKeptHoweverOld)
{
	Fixture f;
	// Seven recent snapshots of a card that broke (not healthy), one per day, and a good one 40 days back.
	std::vector<s64> recent;
	for (int day = 10; day >= 4; day--)
	{
		recent.push_back(LocalMs(2026, 3, day, 12));
		f.PutSnapshot("a.ps2", recent.back(), false, CardImage(static_cast<char>('a' + day)), "PRE_RESTORE");
	}
	const s64 old_good = LocalMs(2026, 1, 20, 12);
	f.PutSnapshot("a.ps2", old_good, true, CardImage('z'));
	f.store().Prune("a.ps2");

	std::set<s64> expected(recent.begin(), recent.end()); // newest 3 + next 4 days = all 7
	expected.insert(old_good);
	EXPECT_EQ(Kept(f, "a.ps2"), expected);
}

TEST(MemcardPrune, EightDaysOfUnhealthyHistoryKeepsSevenAndNothingIsKeptForBeingHealthy)
{
	// The day rule alone: with no healthy snapshot to protect, the eighth day goes.
	Fixture f;
	for (int day = 10; day >= 4; day--)
		f.PutSnapshot("a.ps2", LocalMs(2026, 3, day, 12), false, CardImage(static_cast<char>('a' + day)), "PRE_RESTORE");
	f.PutSnapshot("a.ps2", LocalMs(2026, 3, 3, 12), false, CardImage('y'), "PRE_RESTORE");
	f.store().Prune("a.ps2");
	EXPECT_EQ(Count(f, "a.ps2"), 7u);
}

TEST(MemcardPrune, AutomaticSnapshotsPruneAsTheyGo)
{
	Fixture f;
	// Twelve different cards in the same afternoon (the fixture clock steps an hour; stay in a day by
	// stepping a minute instead).
	s64 now = LocalMs(2026, 3, 10, 9);
	f.store().SetClock([&now]() { return now += 60'000; });
	for (int i = 0; i < 12; i++)
	{
		f.PutCard("a.ps2", CardImage(static_cast<char>('a' + i)));
		ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	}
	EXPECT_EQ(Count(f, "a.ps2"), 3u);
}

TEST(MemcardPrune, ABrokenCardNeverReplacesTheLastGoodSnapshot)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	// The card breaks; launching the game ten times snapshots nothing.
	f.PutCard("a.ps2", std::string(CARD_SIZE, '\xff'));
	for (int i = 0; i < 10; i++)
		EXPECT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Unreadable);
	const auto healthy = f.store().NewestHealthy("a.ps2");
	ASSERT_TRUE(healthy.has_value());
	EXPECT_EQ(healthy->hash, SHA256Digest::HashHex(CardImage('a').data(), CardImage('a').size()));
}

TEST(MemcardRestore, PutsAFileCardBackAndKeepsTheBrokenOneAsAPreRestoreSnapshot)
{
	Fixture f;
	const std::string good = CardImage('a');
	f.PutCard("a.ps2", good);
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("a.ps2");

	const std::string broken = std::string(CARD_SIZE, '\x5a');
	f.PutCard("a.ps2", broken);

	const Outcome outcome = f.store().Restore(snap);
	ASSERT_TRUE(outcome.ok) << outcome.detail;
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), good);

	// No work files left in the cards folder.
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.restoring")));
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.previous")));

	// The broken card was kept, marked unhealthy, as a PRE_RESTORE snapshot.
	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].reason, Reason::PreRestore);
	EXPECT_FALSE(list[0].healthy);
	EXPECT_EQ(list[0].hash, SHA256Digest::HashHex(broken.data(), broken.size()));
	EXPECT_EQ(ZipText(list[0].path, "card/a.ps2"), broken);
	// The snapshot restored from is still there.
	EXPECT_EQ(list[1].path, snap.path);
}

TEST(MemcardRestore, PutsAFolderCardBackExactly)
{
	Fixture f;
	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	const auto original = Tree(f.CardPath("f.ps2"));
	ASSERT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("f.ps2");

	// Edit one file, delete another, add a stray.
	WriteFile(f.CardPath("f.ps2") + "/BASLUS-12345/icon.sys", "ruined");
	fs::remove(f.CardPath("f.ps2") + "/_pcsx2_index");
	WriteFile(f.CardPath("f.ps2") + "/stray.txt", "not in the backup");

	const Outcome outcome = f.store().Restore(snap);
	ASSERT_TRUE(outcome.ok) << outcome.detail;
	EXPECT_EQ(Tree(f.CardPath("f.ps2")), original);
	EXPECT_FALSE(fs::exists(f.CardPath("f.ps2.restoring")));
	EXPECT_FALSE(fs::exists(f.CardPath("f.ps2.previous")));

	const auto list = f.store().List("f.ps2");
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].reason, Reason::PreRestore);
	EXPECT_TRUE(list[0].is_folder);
}

TEST(MemcardRestore, WorksWhenTheCardIsGone)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("a.ps2");
	fs::remove(f.CardPath("a.ps2"));

	ASSERT_TRUE(f.store().Restore(snap).ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('a'));
	EXPECT_EQ(Count(f, "a.ps2"), 1u); // no card to take a pre-restore snapshot of
}

TEST(MemcardRestore, RestoresAnUnhealthySnapshotToo)
{
	Fixture f;
	const std::string odd = std::string(CARD_SIZE, '\x11');
	const std::string path = f.PutSnapshot("a.ps2", 1'000, false, odd, "PRE_RESTORE");
	f.PutCard("a.ps2", CardImage('a'));
	const auto snap = f.store().List("a.ps2")[0];
	ASSERT_FALSE(snap.healthy);
	ASSERT_TRUE(f.store().Restore(snap).ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), odd);
}

TEST(MemcardRestore, ARestoreThatCannotMoveTheCardAsideLeavesItAlone)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("a.ps2");
	f.PutCard("a.ps2", CardImage('b'));

	FailingOps ops({1}); // live -> .previous
	f.store().SetFileOps(&ops);
	const Outcome outcome = f.store().Restore(snap);
	EXPECT_FALSE(outcome.ok);
	EXPECT_FALSE(outcome.detail.empty());
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('b'));
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.restoring")));
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.previous")));
}

TEST(MemcardRestore, ARestoreThatCannotPutTheNewCardInPlaceRollsBack)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("a.ps2");
	f.PutCard("a.ps2", CardImage('b'));

	FailingOps ops({2}); // .restoring -> live
	f.store().SetFileOps(&ops);
	const Outcome outcome = f.store().Restore(snap);
	EXPECT_FALSE(outcome.ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('b')); // the original, back from .previous
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.restoring")));
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.previous")));
}

TEST(MemcardRestore, ARollbackAlsoWorksForAFolderCard)
{
	Fixture f;
	MakeFolderCard(f.CardPath("f.ps2"), 'a');
	ASSERT_EQ(f.store().SnapshotIfChanged("f.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("f.ps2");
	WriteFile(f.CardPath("f.ps2") + "/BASLUS-12345/icon.sys", "later");
	const auto later = Tree(f.CardPath("f.ps2"));

	FailingOps ops({2});
	f.store().SetFileOps(&ops);
	EXPECT_FALSE(f.store().Restore(snap).ok);
	EXPECT_EQ(Tree(f.CardPath("f.ps2")), later);
	EXPECT_FALSE(fs::exists(f.CardPath("f.ps2.restoring")));
	EXPECT_FALSE(fs::exists(f.CardPath("f.ps2.previous")));
}

TEST(MemcardRestore, IfEvenTheRollbackFailsTheUserIsToldWhereTheirCardIs)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ASSERT_EQ(f.store().SnapshotIfChanged("a.ps2", Reason::Session, ""), CardStatus::Snapshotted);
	const Snapshot snap = *f.store().NewestHealthy("a.ps2");
	f.PutCard("a.ps2", CardImage('b'));

	FailingOps ops({2, 3});
	f.store().SetFileOps(&ops);
	const Outcome outcome = f.store().Restore(snap);
	EXPECT_FALSE(outcome.ok);
	EXPECT_NE(outcome.detail.find("a.ps2.previous"), std::string::npos) << outcome.detail;
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2.previous")), CardImage('b'));
}

TEST(MemcardRestore, ABackupWhoseContentsDoNotMatchItsHashIsRefusedBeforeAnythingChanges)
{
	Fixture f;
	const std::string image = CardImage('a');
	// The manifest vouches for a different card than the one in the zip.
	const std::string other_hash = SHA256Digest::HashHex("something else");
	fs::create_directories(f.BackupDir("a.ps2"));
	const std::string path = f.BackupDir("a.ps2") + "/1000-session.zip";
	ASSERT_TRUE(WriteZip(path, {{"armsx2-mcbak.json", AndroidManifest("a.ps2", 1000, "SESSION", image.size(), other_hash, true, false, "")},
								   {"card/a.ps2", image}}));
	f.PutCard("a.ps2", CardImage('b'));

	const auto snap = f.store().List("a.ps2")[0];
	const Outcome outcome = f.store().Restore(snap);
	EXPECT_FALSE(outcome.ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('b'));
	EXPECT_EQ(Count(f, "a.ps2"), 1u); // no pre-restore snapshot for a restore that never started
	EXPECT_FALSE(fs::exists(f.CardPath("a.ps2.restoring")));
}

TEST(MemcardRestore, AHealthyBackupThatDoesNotUnpackToAVerifiedCardIsRefused)
{
	Fixture f;
	const std::string junk(CARD_SIZE, 'q');
	const std::string hash = SHA256Digest::HashHex(junk.data(), junk.size());
	fs::create_directories(f.BackupDir("a.ps2"));
	ASSERT_TRUE(WriteZip(f.BackupDir("a.ps2") + "/1000-session.zip",
		{{"armsx2-mcbak.json", AndroidManifest("a.ps2", 1000, "SESSION", junk.size(), hash, true, false, "")}, {"card/a.ps2", junk}}));
	f.PutCard("a.ps2", CardImage('b'));
	const auto snap = f.store().List("a.ps2")[0];
	EXPECT_FALSE(f.store().Restore(snap).ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('b'));
}

TEST(MemcardRestore, AnEntryThatEscapesTheCardFolderIsRefused)
{
	for (const char* evil : {"card/../evil.txt", "card/a/../../evil.txt", "card//evil.txt", "card/./x", "card/..\\evil.txt"})
	{
		Fixture f;
		fs::create_directories(f.BackupDir("f.ps2"));
		const std::string path = f.BackupDir("f.ps2") + "/1000-session.zip";
		ASSERT_TRUE(WriteZip(path, {{"armsx2-mcbak.json", AndroidManifest("f.ps2", 1000, "SESSION", 10, "", false, true, "")},
									   {"card/_pcsx2_superblock", "x"}, {evil, "pwned"}}));
		MakeFolderCard(f.CardPath("f.ps2"), 'a');
		const auto before = Tree(f.CardPath("f.ps2"));

		const auto snap = f.store().List("f.ps2")[0];
		ASSERT_TRUE(snap.is_folder);
		const Outcome outcome = f.store().Restore(snap);
		EXPECT_FALSE(outcome.ok) << evil;
		EXPECT_EQ(Tree(f.CardPath("f.ps2")), before) << evil;
		EXPECT_FALSE(fs::exists(f.root().File("memcards/evil.txt"))) << evil;
		EXPECT_FALSE(fs::exists(f.root().File("evil.txt"))) << evil;
		EXPECT_FALSE(fs::exists(f.CardPath("f.ps2.restoring"))) << evil;
	}
}

TEST(MemcardRestore, ACardNameThatIsAPathIsRefused)
{
	Fixture f;
	Snapshot snap;
	snap.card = "../escaped.ps2";
	snap.path = f.PutSnapshot("a.ps2", 1000, true, CardImage('a'));
	EXPECT_FALSE(f.store().Restore(snap).ok);
	EXPECT_FALSE(fs::exists(f.root().File("escaped.ps2")));
	snap.card = "";
	EXPECT_FALSE(f.store().Restore(snap).ok);
}

TEST(MemcardRestore, AMissingOrUnreadableZipIsRefused)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('b'));
	Snapshot snap;
	snap.card = "a.ps2";
	snap.path = f.root().File("nope.zip");
	EXPECT_FALSE(f.store().Restore(snap).ok);

	WriteFile(f.root().File("junk.zip"), "not a zip");
	snap.path = f.root().File("junk.zip");
	EXPECT_FALSE(f.store().Restore(snap).ok);

	ASSERT_TRUE(WriteZip(f.root().File("empty.zip"), {{"armsx2-mcbak.json", "{}"}}));
	snap.path = f.root().File("empty.zip");
	EXPECT_FALSE(f.store().Restore(snap).ok);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('b'));
}

TEST(MemcardRestore, AGoodCardThatCannotBeBackedUpFirstIsLeftAlone)
{
	ScopedTempDir root;
	fs::create_directories(root.File("memcards"));
	WriteFile(root.File("blocker"), "a file where the backup folder should be");
	Store store(root.File("memcards"), root.File("blocker"));
	WriteFile(root.File("memcards/a.ps2"), CardImage('b'));

	// The snapshot to restore lives elsewhere.
	Fixture other;
	Snapshot snap;
	snap.card = "a.ps2";
	snap.path = other.PutSnapshot("a.ps2", 1000, true, CardImage('a'));
	snap.healthy = true;
	snap.hash = SHA256Digest::HashHex(CardImage('a').data(), CardImage('a').size());

	const Outcome outcome = store.Restore(snap);
	EXPECT_FALSE(outcome.ok);
	EXPECT_EQ(ReadFile(root.File("memcards/a.ps2")), CardImage('b'));
	EXPECT_FALSE(fs::exists(root.File("memcards/a.ps2.restoring")));
}

TEST(MemcardRestore, ABrokenCardThatCannotBeBackedUpFirstIsStillRestored)
{
	ScopedTempDir root;
	fs::create_directories(root.File("memcards"));
	WriteFile(root.File("blocker"), "a file where the backup folder should be");
	Store store(root.File("memcards"), root.File("blocker"));
	WriteFile(root.File("memcards/a.ps2"), std::string(CARD_SIZE, '\xff'));

	Fixture other;
	Snapshot snap;
	snap.card = "a.ps2";
	snap.path = other.PutSnapshot("a.ps2", 1000, true, CardImage('a'));
	snap.healthy = true;
	snap.hash = SHA256Digest::HashHex(CardImage('a').data(), CardImage('a').size());

	ASSERT_TRUE(store.Restore(snap).ok);
	EXPECT_EQ(ReadFile(root.File("memcards/a.ps2")), CardImage('a'));
}

namespace
{
	struct Prompts
	{
		std::vector<RestorePrompt> asked;
		RestoreAnswer answer = RestoreAnswer::Restore;

		RestorePromptHandler Handler()
		{
			return [this](const RestorePrompt& p) {
				asked.push_back(p);
				return answer;
			};
		}
	};

	// A card with one healthy snapshot, then broken.
	void MakeBrokenCardWithBackup(Fixture& f, const std::string& name)
	{
		f.PutCard(name, CardImage('a'));
		ASSERT_EQ(f.store().SnapshotIfChanged(name, Reason::Session, "SLUS-12345"), CardStatus::Snapshotted);
		f.PutCard(name, std::string(CARD_SIZE, '\xff'));
	}
} // namespace

TEST(MemcardBoot, SnapshotsGoodCardsAndSkipsMissingOnes)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	Prompts prompts;
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2", "gone.ps2"}, "SLUS-12345", true, prompts.Handler());
	ASSERT_EQ(reports.size(), 2u);
	EXPECT_EQ(reports[0].status, CardStatus::Snapshotted);
	EXPECT_EQ(reports[1].status, CardStatus::Absent);
	EXPECT_TRUE(prompts.asked.empty());
	EXPECT_EQ(f.store().List("a.ps2")[0].game, "SLUS-12345");
}

TEST(MemcardBoot, AnUnreadableCardWithAHealthyBackupAsksAndRestores)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	Prompts prompts;
	prompts.answer = RestoreAnswer::Restore;

	const auto reports = PrepareCardsForBoot(f.store(), {"b.ps2", "a.ps2"}, "SLUS-12345", true, prompts.Handler());

	ASSERT_EQ(prompts.asked.size(), 1u);
	EXPECT_EQ(prompts.asked[0].card, "a.ps2");
	EXPECT_EQ(prompts.asked[0].slot, 2);
	EXPECT_TRUE(prompts.asked[0].snapshot.healthy);
	EXPECT_EQ(prompts.asked[0].snapshot.game, "SLUS-12345");

	ASSERT_EQ(reports.size(), 2u);
	EXPECT_TRUE(reports[1].prompted);
	EXPECT_TRUE(reports[1].restored);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('a'));
	// Restored: the original snapshot plus the pre-restore copy of the broken card, and no new
	// session snapshot of a card that equals one already kept.
	const auto list = f.store().List("a.ps2");
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(list[0].reason, Reason::PreRestore);
	EXPECT_EQ(list[1].reason, Reason::Session);
}

TEST(MemcardBoot, StartAnywayLeavesTheCardAsItIsAndSnapshotsNothing)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	Prompts prompts;
	prompts.answer = RestoreAnswer::StartAnyway;

	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, prompts.Handler());

	ASSERT_EQ(prompts.asked.size(), 1u);
	EXPECT_TRUE(reports[0].prompted);
	EXPECT_FALSE(reports[0].restored);
	EXPECT_EQ(reports[0].status, CardStatus::Unreadable);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), std::string(CARD_SIZE, '\xff'));
	EXPECT_EQ(Count(f, "a.ps2"), 1u);
}

TEST(MemcardBoot, WithNoHandlerTheBootGoesAheadWithTheCardAsItIs)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, RestorePromptHandler());
	ASSERT_EQ(reports.size(), 1u);
	EXPECT_FALSE(reports[0].prompted);
	EXPECT_FALSE(reports[0].restored);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), std::string(CARD_SIZE, '\xff'));
}

TEST(MemcardBoot, WithNoHandlerTheReportSaysNobodyWasAskedSoTheServiceTellsThePlayer)
{
	// armsx2-sdl started per game by a launcher, Big Picture closed: nobody registered a handler. The service
	// itself puts the message on screen (ReportUnansweredRestore), so every host gets it.
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, RestorePromptHandler());
	ASSERT_EQ(reports.size(), 1u);
	EXPECT_TRUE(reports[0].unanswered);
}

TEST(MemcardBoot, ACardNobodyHadToAnswerForIsNotReportedUnanswered)
{
	// A handler was asked.
	{
		Fixture f;
		MakeBrokenCardWithBackup(f, "a.ps2");
		Prompts prompts;
		const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, prompts.Handler());
		EXPECT_TRUE(reports[0].prompted);
		EXPECT_FALSE(reports[0].unanswered);
	}
	// Not booting: a running game is never asked, with or without a handler.
	{
		Fixture f;
		MakeBrokenCardWithBackup(f, "a.ps2");
		const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", /*allow_prompt=*/false, RestorePromptHandler());
		EXPECT_FALSE(reports[0].unanswered);
	}
	// A good card, or an unreadable one with no healthy backup, has nothing to ask.
	{
		Fixture f;
		f.PutCard("good.ps2", CardImage('a'));
		f.PutCard("new.ps2", std::string(CARD_SIZE, '\xff'));
		const auto reports = PrepareCardsForBoot(f.store(), {"good.ps2", "new.ps2"}, "", true, RestorePromptHandler());
		ASSERT_EQ(reports.size(), 2u);
		EXPECT_FALSE(reports[0].unanswered);
		EXPECT_FALSE(reports[1].unanswered);
	}
}

TEST(MemcardBoot, NoPromptWhileAGameIsAlreadyRunning)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	Prompts prompts;
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", /*allow_prompt=*/false, prompts.Handler());
	EXPECT_TRUE(prompts.asked.empty());
	EXPECT_FALSE(reports[0].restored);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), std::string(CARD_SIZE, '\xff'));
}

TEST(MemcardBoot, NoPromptWithoutAHealthyBackup)
{
	Fixture f;
	// A new blank card: nothing was ever backed up.
	f.PutCard("new.ps2", std::string(CARD_SIZE, '\xff'));
	// A card whose only snapshot is of a broken card.
	f.PutCard("sick.ps2", std::string(CARD_SIZE, '\xff'));
	f.PutSnapshot("sick.ps2", 1000, false, std::string(CARD_SIZE, '\xff'), "PRE_RESTORE");

	Prompts prompts;
	PrepareCardsForBoot(f.store(), {"new.ps2", "sick.ps2"}, "", true, prompts.Handler());
	EXPECT_TRUE(prompts.asked.empty());
}

TEST(MemcardBoot, EachBrokenCardIsAskedAbout)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	MakeBrokenCardWithBackup(f, "b.ps2");
	Prompts prompts;
	PrepareCardsForBoot(f.store(), {"a.ps2", "b.ps2"}, "", true, prompts.Handler());
	ASSERT_EQ(prompts.asked.size(), 2u);
	EXPECT_EQ(prompts.asked[0].slot, 1);
	EXPECT_EQ(prompts.asked[1].slot, 2);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), CardImage('a'));
	EXPECT_EQ(ReadFile(f.CardPath("b.ps2")), CardImage('a'));
}

TEST(MemcardBoot, TheSameCardInBothSlotsIsDoneOnce)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	Prompts prompts;
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2", "a.ps2"}, "", true, prompts.Handler());
	EXPECT_EQ(prompts.asked.size(), 1u);
	EXPECT_EQ(reports.size(), 1u);
}

TEST(MemcardBoot, ARestoreThatFailsFallsThroughToBootingWithTheCardAsItIs)
{
	Fixture f;
	MakeBrokenCardWithBackup(f, "a.ps2");
	FailingOps ops({1});
	f.store().SetFileOps(&ops);
	Prompts prompts;
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, prompts.Handler());
	ASSERT_EQ(prompts.asked.size(), 1u);
	EXPECT_TRUE(reports[0].prompted);
	EXPECT_FALSE(reports[0].restored);
	EXPECT_EQ(ReadFile(f.CardPath("a.ps2")), std::string(CARD_SIZE, '\xff'));
}

TEST(MemcardBoot, ARepeatBootOfAnUnchangedCardDoesNothing)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	Prompts prompts;
	PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, prompts.Handler());
	const auto reports = PrepareCardsForBoot(f.store(), {"a.ps2"}, "", true, prompts.Handler());
	EXPECT_EQ(reports[0].status, CardStatus::Unchanged);
	EXPECT_EQ(Count(f, "a.ps2"), 1u);
}

TEST(MemcardBoot, TheHandlerCanBeRegisteredAndCleared)
{
	int calls = 0;
	SetRestorePromptHandler([&calls](const RestorePrompt&) {
		calls++;
		return RestoreAnswer::StartAnyway;
	});
	SetRestorePromptHandler(RestorePromptHandler());
	EXPECT_EQ(calls, 0);
}

namespace
{
	// EmuFolders and EmuConfig as OnCardsReopening() reads them: the fixture's cards folder as the data root's
	// memcards, slot 1 holding "a.ps2" and automatic backups on. Put back afterwards.
	class ReopenHookScope
	{
	public:
		explicit ReopenHookScope(const Fixture& f)
			: m_data_root(EmuFolders::DataRoot)
			, m_memory_cards(EmuFolders::MemoryCards)
			, m_auto_backup(EmuConfig.McdAutoBackup)
			, m_slot_enabled(EmuConfig.Mcd[0].Enabled)
			, m_slot_type(EmuConfig.Mcd[0].Type)
			, m_slot_name(EmuConfig.Mcd[0].Filename)
			, m_slot2_enabled(EmuConfig.Mcd[1].Enabled)
		{
			EmuFolders::DataRoot = f.root().Dir();
			EmuFolders::MemoryCards = f.root().File("memcards");
			EmuConfig.McdAutoBackup = true;
			EmuConfig.Mcd[0].Enabled = true;
			EmuConfig.Mcd[0].Type = MemoryCardType::File;
			EmuConfig.Mcd[0].Filename = "a.ps2";
			EmuConfig.Mcd[1].Enabled = false;
		}

		~ReopenHookScope()
		{
			EmuFolders::DataRoot = m_data_root;
			EmuFolders::MemoryCards = m_memory_cards;
			EmuConfig.McdAutoBackup = m_auto_backup;
			EmuConfig.Mcd[0].Enabled = m_slot_enabled;
			EmuConfig.Mcd[0].Type = m_slot_type;
			EmuConfig.Mcd[0].Filename = m_slot_name;
			EmuConfig.Mcd[1].Enabled = m_slot2_enabled;
		}

	private:
		std::string m_data_root, m_memory_cards;
		bool m_auto_backup, m_slot_enabled;
		MemoryCardType m_slot_type;
		std::string m_slot_name;
		bool m_slot2_enabled;
	};
} // namespace

TEST(MemcardReopenHook, AReopenWhileNoVmIsBootingTakesNoSnapshot)
{
	// FileMcd_Reopen also runs when the memory card folder or a card is changed in the settings of a running
	// game, from VMManager::UpdateEmuFolders with the settings lock held. Hashing and zipping both cards there
	// stalls the CPU thread under that lock, so only the reopen of a boot backs the cards up. (This process has
	// no VM, which is not a boot either.)
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ReopenHookScope scope(f);

	OnCardsReopening();
	EXPECT_EQ(Count(f, "a.ps2"), 0u);
	EXPECT_FALSE(fs::exists(f.BackupDir("a.ps2")));
}

TEST(MemcardReopenHook, TheBootsReopenTakesTheSnapshot)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ReopenHookScope scope(f);

	OnCardsReopening(true);
	EXPECT_EQ(Count(f, "a.ps2"), 1u);

	// Unchanged cards cost nothing the second time.
	OnCardsReopening(true);
	EXPECT_EQ(Count(f, "a.ps2"), 1u);
}

TEST(MemcardReopenHook, ASwitchedOffBackupTakesNoSnapshotEvenAtBoot)
{
	Fixture f;
	f.PutCard("a.ps2", CardImage('a'));
	ReopenHookScope scope(f);
	EmuConfig.McdAutoBackup = false;

	OnCardsReopening(true);
	EXPECT_EQ(Count(f, "a.ps2"), 0u);
}

namespace
{
	bool LoadAutoBackup(MemorySettingsInterface& si)
	{
		Pcsx2Config config;
		SettingsLoadWrapper wrap(si);
		config.LoadSaveMemcards(wrap);
		return config.McdAutoBackup;
	}
} // namespace

TEST(MemcardSettings, AutoBackupIsOnByDefaultAndFollowsTheMemoryCardsSection)
{
	MemorySettingsInterface empty;
	EXPECT_EQ(LoadAutoBackup(empty), AutoBackupDefault());

	MemorySettingsInterface off;
	off.SetBoolValue("MemoryCards", "AutoBackup", false);
	EXPECT_FALSE(LoadAutoBackup(off));

	MemorySettingsInterface on;
	on.SetBoolValue("MemoryCards", "AutoBackup", true);
	EXPECT_TRUE(LoadAutoBackup(on));
}

TEST(MemcardSettings, AutoBackupIsOffWhereTheFrontEndHasNoBackupScreen)
{
	//                                   android libretro iOS
	EXPECT_TRUE(AutoBackupDefaultFor(false, false, false));
	EXPECT_FALSE(AutoBackupDefaultFor(true, false, false)); // the app's own snapshots run there
	EXPECT_FALSE(AutoBackupDefaultFor(false, true, false));
	EXPECT_FALSE(AutoBackupDefaultFor(false, false, true));
	EXPECT_FALSE(AutoBackupDefaultFor(true, true, true));

	// And a fresh configuration starts with this build's value.
	Pcsx2Config fresh;
	EXPECT_EQ(static_cast<bool>(fresh.McdAutoBackup), AutoBackupDefault());
#if !defined(__ANDROID__) && !defined(ENABLE_LIBRETRO)
	EXPECT_TRUE(AutoBackupDefault()); // desktop
#endif
}

TEST(MemcardSettings, TheBootHookReadsTheLoadedConfigNotTheSettingsLock)
{
	const bool saved = EmuConfig.McdAutoBackup;
	EmuConfig.McdAutoBackup = false;
	EXPECT_FALSE(IsAutoBackupEnabled());
	EmuConfig.McdAutoBackup = true;
	EXPECT_TRUE(IsAutoBackupEnabled());
	EmuConfig.McdAutoBackup = saved;
}
