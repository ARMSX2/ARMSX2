// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The record of which catalog packs are installed (texture-packs.json): what it stores, how it
// survives a damaged file, how it is reconciled with the disk, and the Install/Update/Installed
// decision made from it.

#include "texture_pack_test_support.h"

#include "Catalog/Json.h"
#include "Catalog/TexturePackState.h"

#include "fmt/format.h"

#include <thread>

using namespace Catalog;
using namespace CatalogTest;

namespace
{
	TexturePackRecord Record(std::string id, std::string serial, std::string version = "v1")
	{
		TexturePackRecord r;
		r.pack_id = std::move(id);
		r.serial = std::move(serial);
		r.version = std::move(version);
		r.name = "Name of " + r.pack_id;
		r.format = "tar+zstd";
		r.schema = 2;
		r.archive_revision = 4;
		r.sha256 = "abcd";
		return r;
	}

	TexturePack TarPack(u64 revision, std::string sha, std::string version = "v1")
	{
		TexturePack p;
		p.id = "p";
		p.format = TexturePackFormat::TarZstd;
		p.archive_revision = revision;
		p.sha256 = std::move(sha);
		p.version = std::move(version);
		return p;
	}

	TexturePack ZipPack(std::string version)
	{
		TexturePack p;
		p.id = "p";
		p.format = TexturePackFormat::Zip;
		p.version = std::move(version);
		return p;
	}
} // namespace

TEST(TexturePackState, StartsEmptyAndWithoutAFile)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	EXPECT_TRUE(state.All().empty());
	EXPECT_FALSE(state.Find("anything"));
	EXPECT_FALSE(state.FindForSerial("SLUS-21410"));
}

TEST(TexturePackState, RecordsAndReadsBack)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));

	// A second object over the same file sees it: this is a file, not a cache.
	TexturePackState again(temp.File("texture-packs.json"));
	const auto found = again.Find("a");
	ASSERT_TRUE(found.has_value());
	EXPECT_EQ(found->serial, "SLUS-21410");
	EXPECT_EQ(found->version, "v1");
	EXPECT_EQ(found->name, "Name of a");
	EXPECT_EQ(found->format, "tar+zstd");
	EXPECT_EQ(found->schema, 2);
	EXPECT_EQ(found->archive_revision, 4u);
	EXPECT_EQ(found->sha256, "abcd");
	EXPECT_TRUE(found->IsTarZstd());
	EXPECT_EQ(again.FindForSerial("slus-21410")->pack_id, "a");
}

TEST(TexturePackState, WritesTheFormatAndroidWrites)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "slus-21410"))); // the serial is stored upper-case

	rapidjson::Document doc;
	ASSERT_TRUE(Catalog::ParseJson(ReadFileOrEmpty(temp.File("texture-packs.json")), &doc));
	ASSERT_TRUE(doc.IsObject() && doc.HasMember("a"));
	const auto& a = doc["a"];
	EXPECT_STREQ(a["serial"].GetString(), "SLUS-21410");
	EXPECT_STREQ(a["version"].GetString(), "v1");
	EXPECT_STREQ(a["name"].GetString(), "Name of a");
	EXPECT_STREQ(a["format"].GetString(), "tar+zstd");
	EXPECT_EQ(a["schema"].GetInt(), 2);
	EXPECT_EQ(a["archiveRevision"].GetUint64(), 4u);
	EXPECT_STREQ(a["sha256"].GetString(), "abcd");
}

TEST(TexturePackState, ARecordFromBeforeTheNewCatalogIsALegacyZipInstall)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("texture-packs.json").c_str(),
		R"({"old":{"serial":"slus-21410","version":"2024-01","name":"Old Pack"},"junk":5})"));
	TexturePackState state(temp.File("texture-packs.json"));

	const auto all = state.All();
	ASSERT_EQ(all.size(), 1u); // the non-object value is skipped
	const TexturePackRecord& r = all.at("old");
	EXPECT_EQ(r.serial, "SLUS-21410");
	EXPECT_EQ(r.format, "zip");
	EXPECT_EQ(r.schema, 1);
	EXPECT_EQ(r.archive_revision, 0u);
	EXPECT_EQ(r.sha256, "");
	EXPECT_FALSE(r.IsTarZstd());
}

TEST(TexturePackState, ANewInstallForASerialReplacesThePreviousRecord)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("first", "SLUS-21410")));
	ASSERT_TRUE(state.Record(Record("elsewhere", "SLES-52187")));
	ASSERT_TRUE(state.Record(Record("second", "slus-21410"))); // any spelling of the same serial

	const auto all = state.All();
	EXPECT_EQ(all.size(), 2u);
	EXPECT_FALSE(all.count("first"));
	EXPECT_TRUE(all.count("second"));
	EXPECT_TRUE(all.count("elsewhere")); // another serial is untouched
	EXPECT_EQ(state.FindForSerial("SLUS-21410")->pack_id, "second");

	ASSERT_TRUE(state.Record(Record("second", "SLUS-21410", "v2")));
	EXPECT_EQ(state.All().size(), 2u);
	EXPECT_EQ(state.Find("second")->version, "v2");
}

TEST(TexturePackState, ForgetSerialDropsOnlyThatGame)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));
	ASSERT_TRUE(state.Record(Record("b", "SLES-52187")));

	EXPECT_TRUE(state.ForgetSerial("slus-21410"));
	EXPECT_FALSE(state.Find("a"));
	EXPECT_TRUE(state.Find("b"));
	EXPECT_TRUE(state.ForgetSerial("SCUS-97328")); // nothing to forget is not a failure
	EXPECT_EQ(state.All().size(), 1u);
}

TEST(TexturePackState, AnUnreadableFileFallsBackToTheLastGoodCopy)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));
	ASSERT_TRUE(state.Record(Record("b", "SLES-52187"))); // the .bak now holds just "a"

	// Truncate the main file the way a power cut would.
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("texture-packs.json").c_str(), R"({"a":{"serial":"SLUS-2)"));
	const auto all = state.All();
	EXPECT_TRUE(all.count("a")) << "the copy kept before the last write is what reads back";

	// Writing again from the fallback must not turn the damaged file into the new .bak.
	ASSERT_TRUE(state.Record(Record("c", "SCUS-97328")));
	TexturePackState fresh(temp.File("texture-packs.json"));
	EXPECT_TRUE(fresh.All().count("a"));
	EXPECT_TRUE(fresh.All().count("c"));

	// Both files damaged: empty, not a crash.
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("texture-packs.json").c_str(), "not json"));
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("texture-packs.json.bak").c_str(), "[1,2]"));
	EXPECT_TRUE(state.All().empty());
}

TEST(TexturePackState, ReportsAFailedWrite)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("blocker").c_str(), "a file where a folder should be"));
	TexturePackState state(temp.File("blocker/texture-packs.json"));
	EXPECT_FALSE(state.Record(Record("a", "SLUS-21410")));
	EXPECT_TRUE(state.All().empty());
}

TEST(TexturePackState, RevisionAdvancesWhenSomethingIsWritten)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	const u32 before = TexturePackState::Revision();
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));
	const u32 after_record = TexturePackState::Revision();
	EXPECT_GT(after_record, before);
	state.ForgetSerial("SLES-52187"); // nothing there: no write
	EXPECT_EQ(TexturePackState::Revision(), after_record);
	state.ForgetSerial("SLUS-21410");
	EXPECT_GT(TexturePackState::Revision(), after_record);
}

TEST(TexturePackState, ReconcileDropsRecordsWhoseFolderIsGone)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));
	ASSERT_TRUE(state.Record(Record("b", "SLES-52187")));

	// SLES-52187 was deleted in a file manager; SCUS-97328 is a folder somebody copied in by hand.
	state.Reconcile({"SLUS-21410", "SCUS-97328"});
	const auto all = state.All();
	EXPECT_EQ(all.size(), 1u);
	EXPECT_TRUE(all.count("a"));
	EXPECT_FALSE(state.FindForSerial("SCUS-97328")); // no record is invented for it
}

TEST(TexturePackState, AnEmptyScanIsNotBelievedWhileRecordsExist)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("a", "SLUS-21410")));
	ASSERT_TRUE(state.Record(Record("b", "SLES-52187")));

	state.Reconcile({}); // a failed scan, not sixty packs deleted at once
	EXPECT_EQ(state.All().size(), 2u);

	TexturePackState empty(temp.File("other.json"));
	empty.Reconcile({});
	EXPECT_TRUE(empty.All().empty());
}

TEST(TexturePackState, ManyThreadsRecordingDoNotLoseEachOther)
{
	ScopedTempDir temp;
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++)
	{
		threads.emplace_back([&, t]() {
			TexturePackState state(temp.File("texture-packs.json")); // each with its own object
			for (int i = 0; i < 10; i++)
				state.Record(Record(fmt::format("p{}-{}", t, i), fmt::format("SLU{}-{:05}", static_cast<char>('A' + t), i)));
		});
	}
	for (auto& thread : threads)
		thread.join();

	TexturePackState state(temp.File("texture-packs.json"));
	EXPECT_EQ(state.All().size(), 80u);
}

TEST(TexturePackStateAction, NothingInstalledMeansInstall)
{
	EXPECT_EQ(TexturePackState::ActionFor(std::nullopt, TarPack(1, "abcd")), InstallAction::Install);
	EXPECT_EQ(TexturePackState::ActionFor(std::nullopt, ZipPack("v1")), InstallAction::Install);
}

TEST(TexturePackStateAction, ATarZstdPackComparesRevisionsThenDigests)
{
	TexturePackRecord installed = Record("p", "SLUS-21410"); // revision 4, digest abcd

	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(5, "abcd")), InstallAction::Update);
	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(5, "ffff")), InstallAction::Update);
	// A lagging mirror must not walk anyone backwards.
	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(3, "ffff")), InstallAction::Installed);
	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(4, "abcd")), InstallAction::Installed);
	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(4, "ABCD")), InstallAction::Installed); // case does not matter
	// Same revision, different bytes: the catalog changed under its own revision.
	EXPECT_EQ(TexturePackState::ActionFor(installed, TarPack(4, "ffff")), InstallAction::Conflict);
}

TEST(TexturePackStateAction, ALegacyZipInstallAlwaysUpdatesToTarZstd)
{
	TexturePackRecord legacy;
	legacy.pack_id = "p";
	legacy.serial = "SLUS-21410";
	legacy.version = "v1";
	EXPECT_EQ(TexturePackState::ActionFor(legacy, TarPack(1, "abcd")), InstallAction::Update);

	// A tar+zstd format claimed by a schema-1 record is still not a tar+zstd install.
	legacy.format = "tar+zstd";
	legacy.schema = 1;
	EXPECT_EQ(TexturePackState::ActionFor(legacy, TarPack(1, "abcd")), InstallAction::Update);
}

TEST(TexturePackStateAction, AZipCatalogPackNeverDowngradesATarZstdInstall)
{
	const TexturePackRecord installed = Record("p", "SLUS-21410");
	EXPECT_EQ(TexturePackState::ActionFor(installed, ZipPack("anything")), InstallAction::Installed);

	TexturePackRecord legacy = Record("p", "SLUS-21410", "v1");
	legacy.format = "zip";
	legacy.schema = 1;
	EXPECT_EQ(TexturePackState::ActionFor(legacy, ZipPack("v1")), InstallAction::Installed);
	EXPECT_EQ(TexturePackState::ActionFor(legacy, ZipPack("v2")), InstallAction::Update);
}

TEST(TexturePackStateAction, RecordForDescribesTheInstalledArchive)
{
	TexturePack pack = TarPack(7, "feed", "2.0");
	pack.id = "pack-id";
	pack.name = "Pack Name";
	const TexturePackRecord r = TexturePackState::RecordFor(pack, "slus-21410");
	EXPECT_EQ(r.pack_id, "pack-id");
	EXPECT_EQ(r.serial, "SLUS-21410");
	EXPECT_EQ(r.name, "Pack Name");
	EXPECT_EQ(r.version, "2.0");
	EXPECT_EQ(r.format, "tar+zstd");
	EXPECT_EQ(r.schema, 2);
	EXPECT_EQ(r.archive_revision, 7u);
	EXPECT_EQ(r.sha256, "feed");
	EXPECT_EQ(TexturePackState::ActionFor(r, pack), InstallAction::Installed);

	TexturePack zip = ZipPack("v3");
	const TexturePackRecord z = TexturePackState::RecordFor(zip, "SLUS-21410");
	EXPECT_EQ(z.format, "zip");
	EXPECT_EQ(z.schema, 1);
	EXPECT_FALSE(z.IsTarZstd());
}

TEST(TexturePackStateAction, ACatalogRowIsLookedUpByItsOwnPackId)
{
	ScopedTempDir temp;
	TexturePackState state(temp.File("texture-packs.json"));
	ASSERT_TRUE(state.Record(Record("p", "SLUS-21410"))); // revision 4, digest abcd

	EXPECT_EQ(state.ActionForPack(TarPack(4, "abcd")), InstallAction::Installed);
	EXPECT_EQ(state.ActionForPack(TarPack(5, "abcd")), InstallAction::Update);

	// Another pack for the same game is not the one installed: choosing it replaces the first.
	TexturePack other = TarPack(9, "ffff");
	other.id = "other";
	EXPECT_EQ(state.ActionForPack(other), InstallAction::Install);
}
