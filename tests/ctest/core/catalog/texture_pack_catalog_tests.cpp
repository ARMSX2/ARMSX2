// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The texture-pack catalog: parsing the one textures.json (both the live file's shape and the
// unified one ps2ktxpak compiles), the rules that drop a bad entry, loading with mirrors and the
// saved copy, and the popularity counter. The fixtures are trimmed copies of the live file
// captured 2026-10-08 and of the file `ps2ktxpak compile catalog` writes.

#include "FakeFetcher.h"
#include "texture_pack_test_support.h"

#include "Catalog/TexturePackCatalog.h"

#include <chrono>

using namespace Catalog;
using namespace CatalogTest;

namespace
{
	const std::string SHA_UPPER = "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
	const std::string SHA_LOWER = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

	std::string ReplaceFirst(std::string text, std::string_view from, std::string_view to)
	{
		const size_t pos = text.find(from);
		EXPECT_NE(pos, std::string::npos) << "no '" << from << "' in the base entry";
		if (pos != std::string::npos)
			text.replace(pos, from.size(), to);
		return text;
	}

	// A good schema-2 entry; tests change one thing in it.
	std::string BaseEntry(std::string id = "p1")
	{
		return R"({"id":")" + id + R"(","name":"Pack One","gameTitle":"Game","serials":["slus 21410","SLES_52187"],)"
			R"("version":"v1","authors":["A"],"credits":"c","description":"d","license":"l",)"
			R"("downloadUrl":"https://dl.example.com/p1.tar.zst","format":"tar+zstd","archiveRevision":3,)"
			R"("decompressedSizeBytes":2000,"sourceUrl":"https://example.com/p1","sizeBytes":1000,)"
			R"("sha256":")" + SHA_UPPER + R"(","fileCount":10,"previewUrls":[]})";
	}

	std::string Catalog2(const std::vector<std::string>& entries, int schema = 2)
	{
		std::string out = "{\"schemaVersion\":" + std::to_string(schema) + ",\"entries\":[";
		for (size_t i = 0; i < entries.size(); i++)
			out += (i ? "," : "") + entries[i];
		return out + "]}";
	}

	std::optional<CatalogParseResult> Parse(const std::string& json, std::string* error = nullptr)
	{
		return ParseTexturePackCatalog(json, error);
	}

	// The pack from a catalog holding just this entry, or null.
	std::optional<TexturePack> ParseOne(const std::string& entry, int schema = 2)
	{
		auto result = Parse(Catalog2({entry}, schema));
		if (!result.has_value() || result->packs.size() != 1)
			return std::nullopt;
		return result->packs[0];
	}

	// Whether a pack with this change is dropped next to a good one.
	bool IsDropped(const std::string& changed_entry, int schema = 2)
	{
		auto result = Parse(Catalog2({BaseEntry("good"), changed_entry}, schema));
		return result.has_value() && result->packs.size() == 1 && result->packs[0].id == "good" && result->dropped_entries == 1;
	}

	const std::vector<std::string> kMirrors = TexturePackCatalog::DefaultMirrors();
} // namespace

// ------------------------------------------------------------------------------------------
// The live file (before creators were added)
// ------------------------------------------------------------------------------------------

TEST(TexturePackCatalogParse, TheLiveFileParses)
{
	std::string error;
	const auto result = Parse(ReadFixture("tex/textures.live-old-shape.json"), &error);
	ASSERT_TRUE(result.has_value()) << error;
	ASSERT_EQ(result->packs.size(), 4u);
	EXPECT_EQ(result->dropped_entries, 0u);

	const TexturePack& mk = result->packs[0];
	EXPECT_EQ(mk.id, "ai-upscaled-hd-texture-pack-created-mortal-kombat-armageddon-hd-v1-5-texture-pack-slus-21410-20260728");
	EXPECT_EQ(mk.name, "Mortal Kombat Armageddon HD V1.5 Texture Pack");
	EXPECT_EQ(mk.game_title, "Mortal Kombat: Armageddon");
	EXPECT_EQ(mk.serials, std::vector<std::string>{"SLUS-21410"});
	EXPECT_EQ(mk.format, TexturePackFormat::TarZstd);
	EXPECT_EQ(mk.archive_revision, 1u);
	EXPECT_EQ(mk.decompressed_size_bytes, 843048960u);
	EXPECT_EQ(mk.size_bytes, 591427517u);
	EXPECT_EQ(mk.file_count, 3800u);
	EXPECT_EQ(mk.sha256, "f87de4b99e6373bc2223f4570fb3e9a2436ac10b5e0467e5aca70bda042e46b7");
	EXPECT_EQ(mk.source_url, "https://github.com/ckiscxrsed/Mortal-Kombat-Armageddon-HD-Textures/releases/tag/v1.5");
	EXPECT_TRUE(mk.download_url.starts_with("https://dl.ps2ktxpak.net/packs/"));
	EXPECT_TRUE(mk.parts.empty());
	EXPECT_TRUE(mk.type.empty());
	EXPECT_TRUE(mk.completeness.empty());
	EXPECT_TRUE(mk.NeedsAstc());
}

TEST(TexturePackCatalogParse, AnEntryWithoutCreatorsFallsBackToItsAuthors)
{
	const auto result = Parse(ReadFixture("tex/textures.live-old-shape.json"));
	ASSERT_TRUE(result.has_value());

	const TexturePack& mk = result->packs[0];
	ASSERT_EQ(mk.authors.size(), 2u);
	EXPECT_TRUE(mk.creators_from_authors);
	ASSERT_EQ(mk.creators.size(), 2u);
	EXPECT_EQ(mk.creators[0].name, "AI-upscaled HD texture pack created");
	EXPECT_EQ(mk.creators[1].name, "maintained by ckiscxrsed.");
	EXPECT_TRUE(mk.creators[0].page.empty());
	EXPECT_TRUE(mk.HasNamedCreators());
	EXPECT_EQ(mk.CreatorText(), "AI-upscaled HD texture pack created, maintained by ckiscxrsed.");
}

TEST(TexturePackCatalogParse, TheLiveFileKeepsMultipleSerialsAndPreviews)
{
	const auto result = Parse(ReadFixture("tex/textures.live-old-shape.json"));
	ASSERT_TRUE(result.has_value());

	const TexturePack& gundam = result->packs[1];
	EXPECT_EQ(gundam.serials, (std::vector<std::string>{"SLPS-25123", "SLPS-24122"}));
	EXPECT_TRUE(gundam.MatchesSerial("SLPS-24122"));
	EXPECT_FALSE(gundam.MatchesSerial("SLUS-21410"));

	const TexturePack& gta = result->packs[3];
	EXPECT_EQ(gta.preview_urls, std::vector<std::string>{"https://i.imgur.com/RRDySLw.png"});
}

// ------------------------------------------------------------------------------------------
// The unified file
// ------------------------------------------------------------------------------------------

TEST(TexturePackCatalogParse, TheUnifiedFileCarriesCreatorsTypeAndCompleteness)
{
	std::string error;
	const auto result = Parse(ReadFixture("tex/textures.unified.json"), &error);
	ASSERT_TRUE(result.has_value()) << error;
	ASSERT_EQ(result->packs.size(), 4u);

	const TexturePack& katamari = result->packs[0];
	EXPECT_EQ(katamari.type, "ai_upscale");
	EXPECT_EQ(katamari.completeness, "complete");
	EXPECT_FALSE(katamari.creators_from_authors);
	ASSERT_EQ(katamari.creators.size(), 2u);
	EXPECT_EQ(katamari.creators[0].name, "ironhulk33"); // the lead comes first
	EXPECT_EQ(katamari.creators[1].name, "Bl4ckH4nd");
	EXPECT_TRUE(katamari.creators[0].page.starts_with("https://"));
	EXPECT_TRUE(katamari.creators[0].avatar.starts_with("https://"));
	EXPECT_EQ(katamari.CreatorText(), "ironhulk33, Bl4ckH4nd");

	const TexturePack& port = result->packs[3];
	EXPECT_EQ(port.type, "port");
	EXPECT_EQ(port.completeness, "in_progress");
}

TEST(TexturePackCatalogParse, EmptyCreatorsMeansNobodyHasNamedThem)
{
	const auto result = Parse(ReadFixture("tex/textures.unified.json"));
	ASSERT_TRUE(result.has_value());

	const TexturePack& anonymous = result->packs[1];
	EXPECT_EQ(anonymous.name, "Altered Beast HD Remaster - Community Archive Texture Pack");
	EXPECT_TRUE(anonymous.creators.empty());
	EXPECT_FALSE(anonymous.HasNamedCreators());
	EXPECT_EQ(anonymous.CreatorText(), "");
	// Not the same as a catalog that never had the field: the creators did not come from authors,
	// and authors holds the placeholder older apps need, which must not be shown as a name.
	EXPECT_FALSE(anonymous.creators_from_authors);
	EXPECT_EQ(anonymous.authors, std::vector<std::string>{"Unknown creator"});
	EXPECT_TRUE(anonymous.type.empty());
}

TEST(TexturePackCatalogParse, ACreatorWithOnlyANameHasNoLinks)
{
	const auto result = Parse(ReadFixture("tex/textures.unified.json"));
	ASSERT_TRUE(result.has_value());

	const TexturePack& asi = result->packs[2];
	ASSERT_EQ(asi.creators.size(), 1u);
	EXPECT_EQ(asi.creators[0].name, "ASI Factory");
	EXPECT_TRUE(asi.creators[0].tip.empty());
	EXPECT_TRUE(asi.creators[0].socials.empty());
}

TEST(TexturePackCatalogParse, CreatorLinksMustBeHttps)
{
	const std::string entry = ReplaceFirst(BaseEntry(),
		R"("previewUrls":[])",
		R"("previewUrls":["http://insecure.example/p.png","https://ok.example/p.png"],)"
		R"("creators":[)"
		R"j({"name":" Lead ","page":"https://example.com/lead","avatar":"http://example.com/a.jpg","tip":"javascript:alert(1)","socials":"https://example.com/s"},)j"
		R"({"name":""},)"
		R"({"page":"https://example.com/nameless"},)"
		R"(42,)"
		R"({"name":"Second"}])");
	const auto pack = ParseOne(entry);
	ASSERT_TRUE(pack.has_value());

	ASSERT_EQ(pack->creators.size(), 2u); // the nameless and non-object ones are dropped
	EXPECT_EQ(pack->creators[0].name, "Lead");
	EXPECT_EQ(pack->creators[0].page, "https://example.com/lead");
	EXPECT_EQ(pack->creators[0].avatar, ""); // http
	EXPECT_EQ(pack->creators[0].tip, ""); // not even a web link
	EXPECT_EQ(pack->creators[0].socials, "https://example.com/s");
	EXPECT_EQ(pack->creators[1].name, "Second");
	EXPECT_EQ(pack->preview_urls, std::vector<std::string>{"https://ok.example/p.png"});
}

TEST(TexturePackCatalogParse, ACreatorsFieldThatIsNotAListCountsAsAbsent)
{
	const auto pack = ParseOne(ReplaceFirst(BaseEntry(), R"("previewUrls":[])", R"("previewUrls":[],"creators":null)"));
	ASSERT_TRUE(pack.has_value());
	EXPECT_TRUE(pack->creators_from_authors);
	ASSERT_EQ(pack->creators.size(), 1u);
	EXPECT_EQ(pack->creators[0].name, "A");
}

// ------------------------------------------------------------------------------------------
// Field rules
// ------------------------------------------------------------------------------------------

TEST(TexturePackCatalogParse, NormalisesWhatTheCatalogSays)
{
	const auto pack = ParseOne(ReplaceFirst(BaseEntry(), R"("serials":["slus 21410","SLES_52187"])", R"("serials":["slus 21410","SLUS-21410","SLES_52187","bogus"," scus-97328 "])"));
	ASSERT_TRUE(pack.has_value());
	// Spelling fixed, repeats and non-serials gone.
	EXPECT_EQ(pack->serials, (std::vector<std::string>{"SLUS-21410", "SLES-52187", "SCUS-97328"}));
	// Digests are compared in lower case.
	EXPECT_EQ(pack->sha256, SHA_LOWER);
}

TEST(TexturePackCatalogParse, ReadsASplitPack)
{
	const std::string part_sha = "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF";
	std::string entry = BaseEntry();
	entry = ReplaceFirst(entry, R"("downloadUrl":"https://dl.example.com/p1.tar.zst",)", "");
	entry = ReplaceFirst(entry, R"("previewUrls":[])",
		R"("previewUrls":[],"parts":[{"downloadUrl":"https://a.example/1","sizeBytes":400,"sha256":")" + part_sha +
		R"("},{"downloadUrl":"https://a.example/2","sizeBytes":600,"sha256":")" + part_sha + R"("}])");
	const auto pack = ParseOne(entry);
	ASSERT_TRUE(pack.has_value());

	EXPECT_TRUE(pack->download_url.empty());
	ASSERT_EQ(pack->parts.size(), 2u);
	EXPECT_EQ(pack->parts[0].size_bytes, 400u);
	EXPECT_EQ(pack->parts[1].download_url, "https://a.example/2");
	EXPECT_EQ(pack->parts[0].sha256, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
	ASSERT_EQ(pack->EffectiveParts().size(), 2u);

	// A pack with a single file presents as one part.
	const auto single = ParseOne(BaseEntry());
	ASSERT_TRUE(single.has_value());
	const auto parts = single->EffectiveParts();
	ASSERT_EQ(parts.size(), 1u);
	EXPECT_EQ(parts[0].download_url, single->download_url);
	EXPECT_EQ(parts[0].size_bytes, 1000u);
	EXPECT_EQ(parts[0].sha256, single->sha256);
}

TEST(TexturePackCatalogParse, DropsAnEntryThatBreaksARule)
{
	const std::string part_ok = R"({"downloadUrl":"https://a.example/1","sizeBytes":1000,"sha256":")" + SHA_UPPER + R"("})";
	const auto with_parts = [&](const std::string& parts_json) {
		return ReplaceFirst(ReplaceFirst(BaseEntry("bad"), R"("downloadUrl":"https://dl.example.com/p1.tar.zst",)", ""),
			R"("previewUrls":[])", R"("previewUrls":[],"parts":)" + parts_json);
	};
	const auto base = [] { return BaseEntry("bad"); };

	// Sanity: the split form we mutate below is itself good.
	ASSERT_TRUE(ParseOne(with_parts("[" + part_ok + "]")).has_value());

	const std::pair<const char*, std::string> bad[] = {
		{"no id", ReplaceFirst(base(), R"("id":"bad")", R"("id":" ")")},
		{"no name", ReplaceFirst(base(), R"("name":"Pack One")", R"("name":"")")},
		{"no valid serial", ReplaceFirst(base(), R"("serials":["slus 21410","SLES_52187"])", R"("serials":["XX-1","123456789"])")},
		{"serials not a list", ReplaceFirst(base(), R"("serials":["slus 21410","SLES_52187"])", R"("serials":"SLUS-21410")")},
		{"no authors", ReplaceFirst(base(), R"("authors":["A"])", R"("authors":[])")},
		{"blank authors", ReplaceFirst(base(), R"("authors":["A"])", R"("authors":[" "])")},
		{"no version", ReplaceFirst(base(), R"("version":"v1")", R"("version":"")")},
		{"http source", ReplaceFirst(base(), "https://example.com/p1", "http://example.com/p1")},
		{"no source", ReplaceFirst(base(), R"("sourceUrl":"https://example.com/p1",)", "")},
		{"http download", ReplaceFirst(base(), "https://dl.example.com/p1.tar.zst", "http://dl.example.com/p1.tar.zst")},
		{"no download", ReplaceFirst(base(), R"("downloadUrl":"https://dl.example.com/p1.tar.zst",)", "")},
		{"size zero", ReplaceFirst(base(), R"("sizeBytes":1000)", R"("sizeBytes":0)")},
		{"size negative", ReplaceFirst(base(), R"("sizeBytes":1000)", R"("sizeBytes":-5)")},
		{"size a string", ReplaceFirst(base(), R"("sizeBytes":1000)", R"("sizeBytes":"1000")")},
		{"size over 4 GiB", ReplaceFirst(base(), R"("sizeBytes":1000)", R"("sizeBytes":4294967297)")},
		{"sha short", ReplaceFirst(base(), SHA_UPPER, SHA_UPPER.substr(1))},
		{"sha not hex", ReplaceFirst(base(), SHA_UPPER, "Z" + SHA_UPPER.substr(1))},
		{"no sha", ReplaceFirst(base(), R"("sha256":")" + SHA_UPPER + R"(",)", "")},
		{"no files", ReplaceFirst(base(), R"("fileCount":10)", R"("fileCount":0)")},
		{"zip in schema 2", ReplaceFirst(base(), R"("format":"tar+zstd")", R"("format":"zip")")},
		{"no format in schema 2", ReplaceFirst(base(), R"("format":"tar+zstd",)", "")},
		{"unknown format", ReplaceFirst(base(), R"("format":"tar+zstd")", R"("format":"7z")")},
		{"no revision", ReplaceFirst(base(), R"("archiveRevision":3,)", "")},
		{"revision zero", ReplaceFirst(base(), R"("archiveRevision":3)", R"("archiveRevision":0)")},
		{"no decompressed size", ReplaceFirst(base(), R"("decompressedSizeBytes":2000,)", "")},
		{"decompressed size over 16 GiB", ReplaceFirst(base(), R"("decompressedSizeBytes":2000)", R"("decompressedSizeBytes":17179869185)")},
		{"empty parts", with_parts("[]")},
		{"parts not objects", with_parts("[1]")},
		{"parts do not add up", with_parts("[" + part_ok + "," + part_ok + "]")},
		{"part over http", with_parts("[" + ReplaceFirst(part_ok, "https://", "http://") + "]")},
		{"part without size", with_parts("[" + ReplaceFirst(part_ok, R"("sizeBytes":1000,)", "") + "]")},
		{"part with a bad digest", with_parts("[" + ReplaceFirst(part_ok, SHA_UPPER, "12") + "]")},
	};
	for (const auto& [what, entry] : bad)
		EXPECT_TRUE(IsDropped(entry)) << what;
}

TEST(TexturePackCatalogParse, TheSizeAndFileLimitsAreInclusive)
{
	// Exactly 4 GiB and exactly 16 GiB are the largest a catalog may claim.
	EXPECT_TRUE(ParseOne(ReplaceFirst(BaseEntry(), R"("sizeBytes":1000)", R"("sizeBytes":4294967296)")).has_value());
	EXPECT_TRUE(ParseOne(ReplaceFirst(BaseEntry(), R"("decompressedSizeBytes":2000)", R"("decompressedSizeBytes":17179869184)")).has_value());
}

TEST(TexturePackCatalogParse, SchemaOneIsZip)
{
	// Schema 1: no revision, no decompressed size; "zip" or no format at all.
	const std::string zip_entry =
		R"({"id":"z1","name":"Zip Pack","serials":["SLUS-21410"],"version":"v1","authors":["A"],)"
		R"("downloadUrl":"https://dl.example.com/z1.zip","sourceUrl":"https://example.com/z1","sizeBytes":500,)"
		R"("sha256":")" + SHA_UPPER + R"(","fileCount":7})";
	const auto bare = ParseOne(zip_entry, 1);
	ASSERT_TRUE(bare.has_value());
	EXPECT_EQ(bare->format, TexturePackFormat::Zip);
	EXPECT_EQ(bare->archive_revision, 0u);
	EXPECT_EQ(bare->decompressed_size_bytes, 0u);
	EXPECT_FALSE(bare->NeedsAstc());

	const auto explicit_zip = ParseOne(ReplaceFirst(zip_entry, R"("downloadUrl")", R"("format":"ZIP","downloadUrl")"), 1);
	ASSERT_TRUE(explicit_zip.has_value());
	EXPECT_EQ(explicit_zip->format, TexturePackFormat::Zip);

	// tar+zstd means nothing to a schema-1 reader, so the entry goes.
	EXPECT_FALSE(ParseOne(ReplaceFirst(zip_entry, R"("downloadUrl")", R"("format":"tar+zstd","downloadUrl")"), 1).has_value());
	EXPECT_FALSE(ParseOne(ReplaceFirst(zip_entry, R"("downloadUrl")", R"("format":"rar","downloadUrl")"), 1).has_value());
}

TEST(TexturePackCatalogParse, TheFirstOfTwoEntriesWithOneIdWins)
{
	const auto result = Parse(Catalog2({BaseEntry("dup"), ReplaceFirst(BaseEntry("dup"), R"("name":"Pack One")", R"("name":"Imposter")"), BaseEntry("other")}));
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->packs.size(), 2u);
	EXPECT_EQ(result->packs[0].name, "Pack One");
	EXPECT_EQ(result->dropped_entries, 1u);
}

TEST(TexturePackCatalogParse, RefusesAWholeSourceThatCannotBeTrusted)
{
	std::string error;
	EXPECT_FALSE(Parse("", &error).has_value());
	EXPECT_FALSE(Parse("   \n", &error).has_value());
	EXPECT_FALSE(Parse("not json", &error).has_value());
	EXPECT_NE(error.find("JSON"), std::string::npos) << error;
	EXPECT_FALSE(Parse("[]", &error).has_value());
	EXPECT_FALSE(Parse(R"({"entries":[]})", &error).has_value()); // no schemaVersion
	EXPECT_FALSE(Parse(R"({"schemaVersion":"2","entries":[]})", &error).has_value());
	EXPECT_FALSE(Parse(Catalog2({BaseEntry()}, 0), &error).has_value());
	EXPECT_FALSE(Parse(Catalog2({BaseEntry()}, 3), &error).has_value());
	EXPECT_NE(error.find("schemaVersion"), std::string::npos) << error;
	EXPECT_FALSE(Parse(R"({"schemaVersion":2})", &error).has_value());
	EXPECT_FALSE(Parse(R"({"schemaVersion":2,"entries":{}})", &error).has_value());

	// Nothing parsed is not an empty-but-valid catalog.
	EXPECT_FALSE(Parse(Catalog2({}), &error).has_value());
	EXPECT_FALSE(Parse(Catalog2({R"({"id":"x"})", "5", "null"}), &error).has_value());
	EXPECT_NE(error.find("no usable entries"), std::string::npos) << error;
}

TEST(TexturePackCatalogParse, AcceptsAByteOrderMark)
{
	EXPECT_TRUE(Parse("\xEF\xBB\xBF" + Catalog2({BaseEntry()})).has_value());
}

// ------------------------------------------------------------------------------------------
// Matching packs to games
// ------------------------------------------------------------------------------------------

TEST(TexturePackLookup, NormalizeSerialAcceptsTheSpellingsPeopleType)
{
	EXPECT_EQ(NormalizeSerial("SLUS-21410"), "SLUS-21410");
	EXPECT_EQ(NormalizeSerial("slus 21410"), "SLUS-21410");
	EXPECT_EQ(NormalizeSerial("SLUS_21410"), "SLUS-21410");
	EXPECT_EQ(NormalizeSerial(" slus21410 "), "SLUS-21410");
	EXPECT_FALSE(NormalizeSerial("SLUS-2141"));
	EXPECT_FALSE(NormalizeSerial("SLUS-214100"));
	EXPECT_FALSE(NormalizeSerial("1234-56789"));
	EXPECT_FALSE(NormalizeSerial("SLU5-21410"));
	EXPECT_FALSE(NormalizeSerial(""));
	EXPECT_FALSE(NormalizeSerial("PERSONA 3 FES"));
}

TEST(TexturePackLookup, MatchingAndInstallSerial)
{
	const auto pack = ParseOne(BaseEntry());
	ASSERT_TRUE(pack.has_value());
	ASSERT_EQ(pack->serials, (std::vector<std::string>{"SLUS-21410", "SLES-52187"}));

	EXPECT_TRUE(pack->MatchesSerial("SLUS-21410"));
	EXPECT_TRUE(pack->MatchesSerial("sles_52187"));
	EXPECT_FALSE(pack->MatchesSerial("SCUS-97328"));
	EXPECT_FALSE(pack->MatchesSerial(""));
	EXPECT_TRUE(pack->CoversAnyOf({"SCUS-97328", "SLES-52187"}));
	EXPECT_FALSE(pack->CoversAnyOf({"SCUS-97328"}));

	// The game in context wins, if the pack is for it.
	EXPECT_EQ(pack->InstallSerialFor("sles-52187", {"SLUS-21410"}), "SLES-52187");
	// Else a serial the player owns.
	EXPECT_EQ(pack->InstallSerialFor("SCUS-97328", {"SLES-52187"}), "SLES-52187");
	EXPECT_EQ(pack->InstallSerialFor("", {"SLES-52187"}), "SLES-52187");
	// Else the first one listed.
	EXPECT_EQ(pack->InstallSerialFor("", {}), "SLUS-21410");
	EXPECT_EQ(pack->InstallSerialFor("SCUS-97328", {"SCUS-97328"}), "SLUS-21410");
}

TEST(TexturePackLookup, FindPacksForSerial)
{
	const auto result = Parse(Catalog2({BaseEntry("one"), ReplaceFirst(BaseEntry("two"), R"("serials":["slus 21410","SLES_52187"])", R"("serials":["SCUS-97328"])")}));
	ASSERT_TRUE(result.has_value());

	const auto found = FindPacksForSerial(result->packs, "slus-21410");
	ASSERT_EQ(found.size(), 1u);
	EXPECT_EQ(found[0]->id, "one");
	EXPECT_EQ(FindPacksForSerial(result->packs, "SCUS-97328").size(), 1u);
	EXPECT_TRUE(FindPacksForSerial(result->packs, "SLPM-12345").empty());
}

// ------------------------------------------------------------------------------------------
// Loading: mirrors and the saved copy
// ------------------------------------------------------------------------------------------

namespace
{
	const std::string kGoodBody = Catalog2({BaseEntry()});
	const std::string kCacheName = TexturePackCatalog::CACHE_FILE_NAME;
} // namespace

TEST(TexturePackCatalogLoad, FetchesTheFirstMirrorAndSavesIt)
{
	ScopedTempDir temp;
	FakeFetcher http;
	http.Ok(kMirrors[0], kGoodBody);
	TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));

	const auto result = catalog.Load();
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(result.source, TexturePackCatalog::Source::Network);
	EXPECT_FALSE(result.IsStale());
	ASSERT_EQ(result.packs.size(), 1u);
	EXPECT_EQ(http.RequestCount(kMirrors[0]), 1u);
	EXPECT_EQ(http.RequestCount(kMirrors[1]), 0u);
	EXPECT_EQ(ReadFileOrEmpty(temp.File("cache/textures-v2.json")), kGoodBody);

	// It asks for JSON, in our name.
	const auto requests = http.Requests();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_EQ(requests[0].method, "GET");
	bool accepts_json = false;
	for (const auto& [name, value] : requests[0].headers)
		accepts_json = accepts_json || (name == "Accept" && value == "application/json");
	EXPECT_TRUE(accepts_json);
	EXPECT_TRUE(requests[0].user_agent.starts_with("ARMSX2/"));
}

TEST(TexturePackCatalogLoad, AFreshSavedCopyNeedsNoNetwork)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, kGoodBody));
	FakeFetcher http; // nothing scripted: any request would fail the load
	TexturePackCatalog catalog(http, cache);

	const auto result = catalog.Load();
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(result.source, TexturePackCatalog::Source::FreshCache);
	EXPECT_FALSE(result.IsStale());
	EXPECT_TRUE(http.Requests().empty());
	EXPECT_NE(result.fetched_at, 0);
}

TEST(TexturePackCatalogLoad, ForceRefreshSkipsTheSavedCopy)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, kGoodBody));
	FakeFetcher http;
	http.Ok(kMirrors[0], Catalog2({BaseEntry("new")}));
	TexturePackCatalog catalog(http, cache);

	const auto result = catalog.Load(true);
	ASSERT_TRUE(result.IsOk());
	EXPECT_EQ(result.source, TexturePackCatalog::Source::Network);
	ASSERT_EQ(result.packs.size(), 1u);
	EXPECT_EQ(result.packs[0].id, "new");
}

TEST(TexturePackCatalogLoad, ASavedCopyOlderThanSixHoursIsRefetched)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, kGoodBody));
	// Age it past the limit instead of waiting.
	const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(7);
	std::filesystem::last_write_time(cache.PathFor(kCacheName), old_time);

	FakeFetcher http;
	http.Ok(kMirrors[0], Catalog2({BaseEntry("new")}));
	TexturePackCatalog catalog(http, cache);
	const auto result = catalog.Load();
	ASSERT_TRUE(result.IsOk());
	EXPECT_EQ(result.source, TexturePackCatalog::Source::Network);
	EXPECT_EQ(result.packs[0].id, "new");
}

TEST(TexturePackCatalogLoad, FallsThroughToTheNextMirror)
{
	const std::string too_big(TexturePackCatalog::MAX_CATALOG_BYTES + 1, ' ');
	struct Case
	{
		const char* what;
		std::function<void(FakeFetcher&)> first_mirror;
	};
	const Case cases[] = {
		{"server error", [](FakeFetcher& h) { h.Status(kMirrors[0], 503); }},
		{"not found", [](FakeFetcher& h) { h.Status(kMirrors[0], 404, "nope"); }},
		{"non-200 success", [](FakeFetcher& h) { h.Status(kMirrors[0], 204); }},
		{"connection failure", [](FakeFetcher& h) { h.Fail(kMirrors[0], "connection refused"); }},
		{"cut off", [](FakeFetcher& h) { h.Truncated(kMirrors[0], kGoodBody, 10); }},
		{"not json", [](FakeFetcher& h) { h.Ok(kMirrors[0], "<html>captive portal</html>"); }},
		{"future schema", [](FakeFetcher& h) { h.Ok(kMirrors[0], Catalog2({BaseEntry()}, 9)); }},
		{"nothing usable", [](FakeFetcher& h) { h.Ok(kMirrors[0], Catalog2({R"({"id":"x"})"})); }},
		{"over 8 MiB", [&](FakeFetcher& h) { h.Ok(kMirrors[0], too_big); }},
		{"redirect to http", [](FakeFetcher& h) { h.Redirect(kMirrors[0], "http://elsewhere.example/textures.json"); }},
	};
	for (const Case& c : cases)
	{
		ScopedTempDir temp;
		FakeFetcher http;
		c.first_mirror(http);
		http.Ok(kMirrors[1], kGoodBody);
		TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));

		const auto result = catalog.Load();
		ASSERT_TRUE(result.IsOk()) << c.what << ": " << result.error;
		EXPECT_EQ(result.source, TexturePackCatalog::Source::Network) << c.what;
		EXPECT_EQ(http.RequestCount(kMirrors[1]), 1u) << c.what;
		EXPECT_EQ(ReadFileOrEmpty(temp.File("cache/textures-v2.json")), kGoodBody) << c.what;
	}
}

TEST(TexturePackCatalogLoad, FollowsAnHttpsRedirect)
{
	ScopedTempDir temp;
	FakeFetcher http;
	http.Redirect(kMirrors[0], "https://cdn.example.com/textures.json");
	http.Ok("https://cdn.example.com/textures.json", kGoodBody);
	TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));
	EXPECT_EQ(catalog.Load().source, TexturePackCatalog::Source::Network);
}

TEST(TexturePackCatalogLoad, WhenEveryMirrorFailsTheOldCopyIsStillOffered)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, kGoodBody));
	FakeFetcher http;
	http.Status(kMirrors[0], 500).Fail(kMirrors[1], "no route to host");

	TexturePackCatalog::Config config;
	config.cache_max_age = std::chrono::seconds(0); // treat the copy as old
	TexturePackCatalog catalog(http, cache, config);

	const auto result = catalog.Load();
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(result.source, TexturePackCatalog::Source::StaleCache);
	EXPECT_TRUE(result.IsStale());
	EXPECT_EQ(result.packs.size(), 1u);
	EXPECT_EQ(http.RequestCount(kMirrors[0]), 1u);
	EXPECT_EQ(http.RequestCount(kMirrors[1]), 1u);
}

TEST(TexturePackCatalogLoad, NoMirrorAndNoCopyIsAnError)
{
	ScopedTempDir temp;
	FakeFetcher http;
	http.Status(kMirrors[0], 500).Fail(kMirrors[1], "no route to host");
	TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));

	const auto result = catalog.Load();
	EXPECT_FALSE(result.IsOk());
	EXPECT_EQ(result.source, TexturePackCatalog::Source::None);
	EXPECT_TRUE(result.packs.empty());
	EXPECT_FALSE(result.cancelled);
	EXPECT_FALSE(result.error.empty());
}

TEST(TexturePackCatalogLoad, ABadBodyNeverReplacesAGoodCopy)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, kGoodBody));
	FakeFetcher http;
	http.Ok(kMirrors[0], "garbage").Ok(kMirrors[1], Catalog2({BaseEntry()}, 9));

	TexturePackCatalog::Config config;
	config.cache_max_age = std::chrono::seconds(0);
	TexturePackCatalog catalog(http, cache, config);

	EXPECT_EQ(catalog.Load().source, TexturePackCatalog::Source::StaleCache);
	EXPECT_EQ(ReadFileOrEmpty(cache.PathFor(kCacheName)), kGoodBody);
}

TEST(TexturePackCatalogLoad, AnUnusableSavedCopyIsReplacedFromTheNetwork)
{
	ScopedTempDir temp;
	ResponseCache cache(temp.File("cache"));
	ASSERT_TRUE(cache.Store(kCacheName, "{ truncated"));
	FakeFetcher http;
	http.Ok(kMirrors[0], kGoodBody);
	TexturePackCatalog catalog(http, cache);

	const auto result = catalog.Load(); // fresh by age, but unusable
	ASSERT_TRUE(result.IsOk()) << result.error;
	EXPECT_EQ(result.source, TexturePackCatalog::Source::Network);
	EXPECT_EQ(ReadFileOrEmpty(cache.PathFor(kCacheName)), kGoodBody);
}

TEST(TexturePackCatalogLoad, ACancelledLoadSaysSo)
{
	ScopedTempDir temp;
	FakeFetcher http;
	http.Ok(kMirrors[0], kGoodBody);
	TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));

	CancelToken cancel;
	cancel.Cancel();
	const auto result = catalog.Load(false, cancel);
	EXPECT_FALSE(result.IsOk());
	EXPECT_TRUE(result.cancelled);
	EXPECT_TRUE(http.Requests().empty());
	EXPECT_TRUE(ReadFileOrEmpty(temp.File("cache/textures-v2.json")).empty());
}

TEST(TexturePackCatalogLoad, OnlyEverAsksForTheCatalog)
{
	// One file carries the creators now; the old links file must not be requested.
	ScopedTempDir temp;
	FakeFetcher http;
	http.Status(kMirrors[0], 500).Ok(kMirrors[1], kGoodBody);
	TexturePackCatalog catalog(http, ResponseCache(temp.File("cache")));
	ASSERT_TRUE(catalog.Load().IsOk());
	for (const auto& request : http.Requests())
		EXPECT_EQ(request.url.find("texture-pack-links"), std::string::npos) << request.url;
}

TEST(TexturePackCatalogLoad, TheMirrorsAreTheDocumentedOnes)
{
	EXPECT_EQ(kMirrors, (std::vector<std::string>{"https://dl.ps2ktxpak.net/textures.json",
						  "https://f005.backblazeb2.com/file/armsx2-textures/textures.json"}));
}

// ------------------------------------------------------------------------------------------
// Popularity
// ------------------------------------------------------------------------------------------

TEST(TexturePackPopularity, ParsesTheLiveList)
{
	const std::vector<std::string> ids = ParsePopularList(ReadFixture("tex/popular.txt"));
	ASSERT_EQ(ids.size(), 10u);
	EXPECT_EQ(ids[0], "panda-venom-god-of-war-ii-20260722");
	EXPECT_EQ(ids[1], "panda-venom-god-of-war-20260722");
	EXPECT_EQ(ids[9], "bl4ckh4nd-tekken-5-slus-21059-20260722");
}

TEST(TexturePackPopularity, KeepsValidDistinctIdsUpToTen)
{
	std::string body = "good-one 5\r\n\r\nBad-Upper 4\nx 3\n-leading 2\ngood-one 1\n  spaced-id   9 \n.dot 1\nok.dot_under-dash 2\n";
	EXPECT_EQ(ParsePopularList(body), (std::vector<std::string>{"good-one", "spaced-id", "ok.dot_under-dash"}));

	body.clear();
	for (int i = 0; i < 15; i++)
		body += "pack-" + std::to_string(i) + " 1\n";
	const auto ids = ParsePopularList(body);
	ASSERT_EQ(ids.size(), 10u);
	EXPECT_EQ(ids.back(), "pack-9");

	EXPECT_TRUE(ParsePopularList("").empty());
}

TEST(TexturePackPopularity, PackIdRules)
{
	EXPECT_TRUE(IsValidPackId("ab"));
	EXPECT_FALSE(IsValidPackId("a"));
	EXPECT_FALSE(IsValidPackId(""));
	EXPECT_TRUE(IsValidPackId(std::string(255, 'a')));
	EXPECT_FALSE(IsValidPackId(std::string(256, 'a')));
	EXPECT_TRUE(IsValidPackId("0ab"));
	EXPECT_TRUE(IsValidPackId("a.b_c-d"));
	for (const char* bad : {".ab", "-ab", "_ab", "Ab", "a b", "a/b", "a\n", "a%20b", "\xC3\xA9x"})
		EXPECT_FALSE(IsValidPackId(bad)) << bad;
}

TEST(TexturePackPopularity, FetchesTheList)
{
	const std::string url = std::string(TEXTURE_STATS_BASE_URL) + "popular";
	{
		FakeFetcher http;
		http.Ok(url, ReadFixture("tex/popular.txt"), "text/plain");
		const auto ids = FetchPopularPackIds(http);
		ASSERT_TRUE(ids.has_value());
		EXPECT_EQ(ids->size(), 10u);
	}
	{
		// "Nothing has been downloaded yet today" is an answer, not a failure.
		FakeFetcher http;
		http.Ok(url, "", "text/plain");
		const auto ids = FetchPopularPackIds(http);
		ASSERT_TRUE(ids.has_value());
		EXPECT_TRUE(ids->empty());
	}
	for (const auto& setup : std::vector<std::function<void(FakeFetcher&)>>{
			 [&](FakeFetcher& h) { h.Status(url, 404); },
			 [&](FakeFetcher& h) { h.Status(url, 500); },
			 [&](FakeFetcher& h) { h.Fail(url, "timeout"); },
			 [&](FakeFetcher& h) { h.Ok(url, std::string(64 * 1024 + 1, 'a')); }, // over the 64 KiB cap
		 })
	{
		FakeFetcher http;
		setup(http);
		EXPECT_FALSE(FetchPopularPackIds(http).has_value());
	}
}

TEST(TexturePackPopularity, TheHitIsSentOnlyWhenAllowed)
{
	const std::string url = std::string(TEXTURE_STATS_BASE_URL) + "hit";
	FakeFetcher http;
	http.Status(url, 200);

	EXPECT_FALSE(PostInstallHit(http, "some-pack-20260101", false));
	EXPECT_TRUE(http.Requests().empty());

	EXPECT_TRUE(PostInstallHit(http, "some-pack-20260101", true));
	const auto requests = http.Requests();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_EQ(requests[0].method, "POST");
	EXPECT_EQ(requests[0].url, url);
	EXPECT_EQ(requests[0].post_body, "some-pack-20260101");
	EXPECT_EQ(requests[0].post_content_type, "text/plain; charset=utf-8");
}

TEST(TexturePackPopularity, ACancelledHitIsNotSent)
{
	FakeFetcher http;
	http.Status(std::string(TEXTURE_STATS_BASE_URL) + "hit", 200);

	CancelToken cancel;
	cancel.Cancel();
	EXPECT_FALSE(PostInstallHit(http, "some-pack-20260101", true, cancel));
	EXPECT_TRUE(http.Requests().empty());
}

TEST(TexturePackPopularity, AnInvalidIdIsNeverSent)
{
	FakeFetcher http;
	http.Status(std::string(TEXTURE_STATS_BASE_URL) + "hit", 200);
	EXPECT_FALSE(PostInstallHit(http, "Not Valid", true));
	EXPECT_FALSE(PostInstallHit(http, "", true));
	EXPECT_TRUE(http.Requests().empty());
}

TEST(TexturePackPopularity, TheHitIsNotRetriedOrFollowed)
{
	const std::string url = std::string(TEXTURE_STATS_BASE_URL) + "hit";
	{
		FakeFetcher http;
		http.Status(url, 500);
		EXPECT_FALSE(PostInstallHit(http, "some-pack", true));
		EXPECT_EQ(http.RequestCount(url), 1u);
	}
	{
		FakeFetcher http;
		http.Fail(url, "connection reset");
		EXPECT_FALSE(PostInstallHit(http, "some-pack", true));
		EXPECT_EQ(http.RequestCount(url), 1u);
	}
	{
		FakeFetcher http;
		http.Redirect(url, "https://elsewhere.example/hit");
		EXPECT_FALSE(PostInstallHit(http, "some-pack", true));
		EXPECT_EQ(http.Requests().size(), 1u);
	}
}
