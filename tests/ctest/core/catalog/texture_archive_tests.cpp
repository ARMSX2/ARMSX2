// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The archive side of texture packs: the path policy, the tar+zstd extractor and the zip
// extractor. The archives are built here, with hand-written tar headers and libzstd, so a test can
// write the malformed ones a real tar never would; one real GNU tar output is committed as a
// fixture (fixtures/tex/real-gnu.tar.zst) to keep the hand-written headers honest.

#include "texture_pack_test_support.h"

#include "Catalog/TarZstdExtractor.h"
#include "Catalog/TextureArchivePath.h"
#include "Catalog/ZipTextureExtractor.h"

#include "fmt/format.h"

#include <optional>

using namespace Catalog;
using namespace CatalogTest;
namespace Path_ = Catalog::TextureArchivePath;

namespace
{
	std::span<const u8> Span(const std::string& s)
	{
		return {Bytes(s), s.size()};
	}

	// Compresses `tar` and extracts it, with the catalog's numbers set to the truth unless the
	// test says otherwise.
	ExtractResult ExtractTar(const std::string& tar, const std::string& dest, u64 texture_count,
		const ExtractProgress& progress = {}, const CancelToken& cancel = {})
	{
		const std::string zst = ZstdCompress(tar);
		return TarZstdExtractor::ExtractMemory(Span(zst), dest, TarZstdExpectations{tar.size(), texture_count}, progress, cancel);
	}

	bool Mentions(const ExtractResult& result, std::string_view needle)
	{
		return result.error.find(needle) != std::string::npos;
	}

	std::string OneFileTar(std::string_view name, std::string_view data = "data")
	{
		return TarBuilder().File(name, data).Finish();
	}
} // namespace

// ------------------------------------------------------------------------------------------
// Path policy
// ------------------------------------------------------------------------------------------

TEST(TextureArchivePath, ValidateAcceptsOrdinaryPathsAndCleansThem)
{
	EXPECT_EQ(Path_::ValidateTarPath("a/b.png"), "a/b.png");
	EXPECT_EQ(Path_::ValidateTarPath("./a/b.png"), "a/b.png");
	EXPECT_EQ(Path_::ValidateTarPath("dir/"), "dir");
	EXPECT_EQ(Path_::ValidateTarPath("./dir/sub/"), "dir/sub");
	EXPECT_EQ(Path_::ValidateTarPath("./"), ""); // the archive's own root entry
	EXPECT_EQ(Path_::ValidateTarPath(""), ""); // an empty name is the root too; the extractor only lets a bare folder entry have it
}

TEST(TextureArchivePath, ValidateRefusesEscapesAndOddComponents)
{
	EXPECT_FALSE(Path_::ValidateTarPath("/etc/passwd"));
	EXPECT_FALSE(Path_::ValidateTarPath("../x.png"));
	EXPECT_FALSE(Path_::ValidateTarPath("a/../../x.png"));
	EXPECT_FALSE(Path_::ValidateTarPath("a/../b.png"));
	EXPECT_FALSE(Path_::ValidateTarPath(".."));
	EXPECT_FALSE(Path_::ValidateTarPath("a/./b.png"));
	EXPECT_FALSE(Path_::ValidateTarPath("."));
	EXPECT_FALSE(Path_::ValidateTarPath("a//b.png"));
	EXPECT_FALSE(Path_::ValidateTarPath(std::string_view("a\0b.png", 7)));
	EXPECT_FALSE(Path_::ValidateTarPath("///"));
}

TEST(TextureArchivePath, ValidateEnforcesTheLengthLimits)
{
	// 32 components of 127 bytes: 4095 bytes in all, at the limits of both component count and path length.
	std::string path;
	for (int i = 0; i < 32; i++)
		path += (i ? "/" : "") + std::string(127, 'a');
	ASSERT_EQ(path.size(), 4095u);
	EXPECT_TRUE(Path_::ValidateTarPath(path));
	EXPECT_TRUE(Path_::ValidateTarPath(path + "a")); // exactly 4096 bytes
	EXPECT_FALSE(Path_::ValidateTarPath(path + "aa")); // 4097

	// 33 components is one too many, however short.
	std::string deep;
	for (int i = 0; i < 33; i++)
		deep += (i ? "/" : "") + std::string("d");
	EXPECT_FALSE(Path_::ValidateTarPath(deep));
	deep.resize(deep.size() - 2); // 32 components
	EXPECT_TRUE(Path_::ValidateTarPath(deep));

	// A component is at most 255 bytes of UTF-8.
	EXPECT_TRUE(Path_::ValidateTarPath(std::string(255, 'x')));
	EXPECT_FALSE(Path_::ValidateTarPath(std::string(256, 'x')));
	std::string two_byte;
	for (int i = 0; i < 127; i++)
		two_byte += "\xC3\xA9"; // 254 bytes
	EXPECT_TRUE(Path_::ValidateTarPath(two_byte));
	EXPECT_FALSE(Path_::ValidateTarPath(two_byte + "\xC3\xA9")); // 256 bytes, though only 128 characters
}

TEST(TextureArchivePath, CollisionKeyFoldsCaseAndUnicodeForms)
{
	EXPECT_EQ(Path_::CollisionKey("Dir/ABC.PNG"), Path_::CollisionKey("dir/abc.png"));
	// Precomposed and decomposed spellings of one name are one name.
	EXPECT_EQ(Path_::CollisionKey("caf\xC3\xA9.png"), Path_::CollisionKey("cafe\xCC\x81.png"));
	EXPECT_EQ(Path_::CollisionKey("CAF\xC3\x89.png"), Path_::CollisionKey("caf\xC3\xA9.png"));
	EXPECT_NE(Path_::CollisionKey("a.png"), Path_::CollisionKey("b.png"));
	EXPECT_NE(Path_::CollisionKey("cafe.png"), Path_::CollisionKey("caf\xC3\xA9.png"));
	// Bytes that are not UTF-8 pass through rather than being dropped.
	EXPECT_EQ(Path_::CollisionKey("a\xFF.png"), "a\xFF.png");
}

TEST(TextureArchivePath, TextureAndJunkClassification)
{
	for (const char* name : {"a.png", "A.PNG", "x/y.dds", "x/y.astc", "x/y.KTX"})
		EXPECT_TRUE(Path_::IsTextureFile(name)) << name;
	for (const char* name : {"a.jpg", "a.txt", "png", "a.png.bak", "a.ktx2"})
		EXPECT_FALSE(Path_::IsTextureFile(name)) << name;

	for (const char* name : {"__MACOSX/a.png", "x/__MACOSX/a.png", ".DS_Store", "x/.ds_store", "x/Thumbs.db"})
		EXPECT_TRUE(Path_::IsJunkEntry(name)) << name;
	for (const char* name : {"a.png", "x/y.png", "MACOSX/a.png"})
		EXPECT_FALSE(Path_::IsJunkEntry(name)) << name;
}

TEST(TextureArchivePath, ReplacementRelativePathStripsEveryWrapperStyle)
{
	const std::string github = "repo-0123456789012345678901234567890123456789";
	const std::pair<const char*, const char*> cases[] = {
		{"a.png", "a.png"},
		{"./a.png", "a.png"},
		{"tex/a.png", "tex/a.png"},
		{"replacements/a.png", "a.png"},
		{"Replacements/a.png", "a.png"},
		{"SLUS-21410/replacements/sub/a.png", "sub/a.png"},
		{"SLUS-21410/a.png", "a.png"},
		{"SLUS21410/a.png", "a.png"},
		{"slus-21410/sub/a.png", "sub/a.png"},
		{"SLUS_21410/a.png", "SLUS_21410/a.png"}, // an underscore is not a serial
		{"deep/er/replacements/x/replacements/a.png", "a.png"}, // the last wrapper wins
		{"x/replacements", "x/replacements"}, // a file named replacements is not a wrapper
		{"a\\replacements\\b.png", "b.png"}, // backslashes separate
		{"//a.png", "a.png"},
	};
	for (const auto& [in, want] : cases)
		EXPECT_EQ(Path_::ReplacementRelativePath(in), want) << in;

	EXPECT_EQ(Path_::ReplacementRelativePath(github + "/tex/a.png"), "tex/a.png");
	EXPECT_EQ(Path_::ReplacementRelativePath(github + "/a.png"), "a.png");
	EXPECT_EQ(Path_::ReplacementRelativePath("repo-0123456789012345678901234567890123456789ab/a.png"), "repo-0123456789012345678901234567890123456789ab/a.png");
	EXPECT_EQ(Path_::ReplacementRelativePath(github + "/SLUS-21410/replacements/a.png"), "a.png");
	EXPECT_FALSE(Path_::ReplacementRelativePath(""));
	EXPECT_FALSE(Path_::ReplacementRelativePath("/"));
	EXPECT_FALSE(Path_::ReplacementRelativePath("./"));
}

TEST(TextureArchivePath, SafeRelativePathRefusesAnythingThatLeavesTheFolder)
{
	EXPECT_TRUE(Path_::IsSafeRelativePath("a.png"));
	EXPECT_TRUE(Path_::IsSafeRelativePath("a/b/c.png"));
	EXPECT_FALSE(Path_::IsSafeRelativePath(""));
	EXPECT_FALSE(Path_::IsSafeRelativePath("/a.png"));
	EXPECT_FALSE(Path_::IsSafeRelativePath("../a.png"));
	EXPECT_FALSE(Path_::IsSafeRelativePath("a/../b.png"));
	EXPECT_FALSE(Path_::IsSafeRelativePath("a//b.png"));
	EXPECT_FALSE(Path_::IsSafeRelativePath("./a.png"));
}

// ------------------------------------------------------------------------------------------
// tar+zstd: what is accepted and where it lands
// ------------------------------------------------------------------------------------------

TEST(TarZstdExtractor, WritesTexturesAndSkipsEverythingElse)
{
	ScopedTempDir temp;
	TarBuilder b;
	b.Dir("./").Dir("replacements/").Dir("replacements/sub/");
	b.File("replacements/0123456789abcdef-00000001.png", "png bytes");
	b.File("replacements/sub/fedcba9876543210-0000ffff.dds", "dds bytes");
	b.File("replacements/readme.txt", "not a texture");
	b.File("replacements/preview.jpg", "not a texture");
	b.File("__MACOSX/replacements/._0123456789abcdef-00000001.png", "junk");
	b.File("replacements/Thumbs.db", "junk");

	const ExtractResult r = ExtractTar(b.Finish(), temp.File("out"), 2);
	ASSERT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(r.textures, 2u);
	EXPECT_EQ(ListFiles(temp.File("out")),
		(std::set<std::string>{"0123456789abcdef-00000001.png", "sub/fedcba9876543210-0000ffff.dds"}));
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/0123456789abcdef-00000001.png")), "png bytes");
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/sub/fedcba9876543210-0000ffff.dds")), "dds bytes");
}

TEST(TarZstdExtractor, StripsEachWrapperStyle)
{
	const std::string github = "repo-0123456789012345678901234567890123456789";
	const std::pair<std::string, std::string> cases[] = {
		{"a.png", "a.png"},
		{"./a.png", "a.png"},
		{"replacements/a.png", "a.png"},
		{"SLUS-21410/replacements/sub/a.png", "sub/a.png"},
		{"SLUS-21410/a.png", "a.png"},
		{github + "/tex/a.png", "tex/a.png"},
		{"Foo/REPLACEMENTS/x/a.KTX", "x/a.KTX"},
		{"A.ASTC", "A.ASTC"},
	};
	for (const auto& [in, want] : cases)
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(OneFileTar(in), temp.File("out"), 1);
		ASSERT_TRUE(r.IsOk()) << in << ": " << r.error;
		EXPECT_EQ(ListFiles(temp.File("out")), std::set<std::string>{want}) << in;
	}
}

TEST(TarZstdExtractor, ReadsARealGnuTarWithLongNames)
{
	// ./SLUS-99999/{readme.txt,Thumbs.db,replacements/...}, three textures, three folder names
	// long enough to need GNU 'L' records. Made with `tar --format=gnu` and `zstd -19`.
	ScopedTempDir temp;
	const std::string path = FixturePath("tex/real-gnu.tar.zst");
	const ExtractResult r = TarZstdExtractor::ExtractFile(path, temp.File("out"), TarZstdExpectations{20480, 3});
	ASSERT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(r.textures, 3u);

	const std::string long_dir = std::string(60, 'd') + "/" + std::string(60, 'e') + "/" + std::string(60, 'f');
	EXPECT_EQ(ListFiles(temp.File("out")),
		(std::set<std::string>{"0123456789abcdef-00000001.png", "sub/fedcba9876543210-0000ffff.ktx",
			"sub/" + long_dir + "/aaaabbbbccccdddd-00000002.dds"}));
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/0123456789abcdef-00000001.png")).size(), 700u);
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/sub/fedcba9876543210-0000ffff.ktx")).size(), 1500u);
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/sub/" + long_dir + "/aaaabbbbccccdddd-00000002.dds")).size(), 300u);

	// The same bytes with the catalog's numbers wrong are refused.
	ScopedTempDir temp2;
	EXPECT_FALSE(TarZstdExtractor::ExtractFile(path, temp2.File("out"), TarZstdExpectations{20480, 4}).IsOk());
	EXPECT_FALSE(TarZstdExtractor::ExtractFile(path, temp2.File("out"), TarZstdExpectations{20481, 3}).IsOk());
	EXPECT_FALSE(TarZstdExtractor::ExtractFile(path, temp2.File("out"), TarZstdExpectations{20479, 3}).IsOk());
}

TEST(TarZstdExtractor, WritesLongNamesFromLRecords)
{
	ScopedTempDir temp;
	std::string name = "replacements/";
	for (int i = 0; i < 3; i++)
		name += std::string(100, static_cast<char>('a' + i)) + "/";
	name += "x.png";
	ASSERT_GT(name.size(), 100u);

	const ExtractResult r = ExtractTar(OneFileTar(name), temp.File("out"), 1);
	ASSERT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(ListFiles(temp.File("out")).size(), 1u);
}

TEST(TarZstdExtractor, CreatesTheDestinationFolder)
{
	ScopedTempDir temp;
	const ExtractResult r = ExtractTar(OneFileTar("a.png"), temp.File("not/yet/there"), 1);
	ASSERT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(ListFiles(temp.File("not/yet/there")), std::set<std::string>{"a.png"});
}

TEST(TarZstdExtractor, ReportsProgressEveryThirtyTwoFilesAndAtTheEnd)
{
	ScopedTempDir temp;
	TarBuilder b;
	for (int i = 0; i < 70; i++)
		b.File(fmt::format("t{}.png", i), "x");

	std::vector<std::pair<u64, u64>> calls;
	const ExtractResult r = ExtractTar(b.Finish(), temp.File("out"), 70, [&](u64 done, u64 total) { calls.emplace_back(done, total); });
	ASSERT_TRUE(r.IsOk()) << r.error;
	ASSERT_EQ(calls.size(), 3u);
	EXPECT_EQ(calls[0], (std::pair<u64, u64>{32, 70}));
	EXPECT_EQ(calls[1], (std::pair<u64, u64>{64, 70}));
	EXPECT_EQ(calls[2], (std::pair<u64, u64>{70, 70}));
}

// ------------------------------------------------------------------------------------------
// tar+zstd: paths that must not get through
// ------------------------------------------------------------------------------------------

TEST(TarZstdExtractor, RefusesPathsThatEscapeTheDestination)
{
	for (const char* name : {"../evil.png", "a/../../evil.png", "/abs/evil.png", "replacements/../../evil.png", "a/./b.png", "a//b.png"})
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(OneFileTar(name), temp.File("work/out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed) << name;
		EXPECT_TRUE(Mentions(r, "rejected path")) << name << ": " << r.error;
		// Nothing was written anywhere, in particular not beside the destination.
		EXPECT_TRUE(ListFiles(temp.Dir()).empty()) << name;
	}
}

TEST(TarZstdExtractor, RefusesABackslashPathThatEscapesOnceItIsASeparator)
{
	// No '/' in it, so the tar path rules pass it; as a path with separators it climbs out.
	ScopedTempDir temp;
	const ExtractResult r = ExtractTar(OneFileTar("x\\..\\..\\evil.png"), temp.File("work/out"), 1);
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "cannot place")) << r.error;
	EXPECT_TRUE(ListFiles(temp.Dir()).empty());
}

TEST(TarZstdExtractor, EnforcesTheComponentAndDepthLimits)
{
	std::string thirty_two;
	for (int i = 0; i < 32; i++)
		thirty_two += "d/";
	thirty_two += "x.png";
	// 32 directories + the file = 33 components: one too many.
	ScopedTempDir a;
	EXPECT_EQ(ExtractTar(OneFileTar(thirty_two), a.File("out"), 1).status, ExtractStatus::Failed);

	// 31 directories + the file = 32 components.
	ScopedTempDir b;
	EXPECT_TRUE(ExtractTar(OneFileTar(thirty_two.substr(2)), b.File("out"), 1).IsOk());

	// A 255 byte name is fine (".png" included), 256 is not.
	ScopedTempDir c;
	EXPECT_TRUE(ExtractTar(OneFileTar(std::string(251, 'n') + ".png"), c.File("out"), 1).IsOk());
	ScopedTempDir d;
	EXPECT_EQ(ExtractTar(OneFileTar(std::string(252, 'n') + ".png"), d.File("out"), 1).status, ExtractStatus::Failed);
}

TEST(TarZstdExtractor, ALongNameCannotBeLongerThan4096Bytes)
{
	// 32 components of 127 bytes and 31 separators are 4095 bytes; one more makes 4096, ending in .png.
	std::string name;
	for (int i = 0; i < 32; i++)
		name += (i ? "/" : "") + std::string(127, 'a');
	name += "a";
	name.replace(name.size() - 4, 4, ".png");
	ASSERT_EQ(name.size(), 4096u);

	// The 'L' record takes it, and so does the path policy. (Writing it can still fail: the full
	// path no longer fits PATH_MAX once the destination is in front of it.)
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(OneFileTar(name), temp.File("out"), 1);
		EXPECT_FALSE(Mentions(r, "GNU L payload")) << r.error;
		EXPECT_FALSE(Mentions(r, "rejected path")) << r.error;
	}

	// The 'L' payload holds the name and its NUL, 4097 bytes at most, so 4097 bytes of name is out.
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(OneFileTar(name + "x"), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(Mentions(r, "GNU L payload")) << r.error;
	}
}

TEST(TarZstdExtractor, EnforcesTheEntryLimit)
{
	// 100,000 entries are fine; the 100,001st is not. Directories keep the archive cheap to write.
	const auto many = [](int dirs) {
		TarBuilder b;
		for (int i = 0; i < dirs; i++)
			b.Dir("d" + std::to_string(i) + "/");
		b.File("t.png", "x");
		return b.Finish();
	};

	ScopedTempDir ok;
	const ExtractResult at_limit = ExtractTar(many(99999), ok.File("out"), 1);
	EXPECT_TRUE(at_limit.IsOk()) << at_limit.error;

	ScopedTempDir bad;
	const ExtractResult over = ExtractTar(many(100000), bad.File("out"), 1);
	EXPECT_EQ(over.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(over, "entry count")) << over.error;
}

TEST(TarZstdExtractor, EnforcesTheFileSizeLimit)
{
	constexpr u64 limit = 512ull * 1024 * 1024;
	static_assert(TarZstdExtractor::MAX_FILE_BYTES == limit);

	// One byte over: refused from the header, before any payload is read.
	{
		ScopedTempDir temp;
		TarEntry e;
		e.name = "big.png";
		e.declared_size = limit + 1;
		e.omit_payload = true;
		const ExtractResult r = ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(Mentions(r, "payload exceeds")) << r.error;
	}
	// Exactly the limit passes the size check; with no payload behind it, the archive then ends early.
	{
		ScopedTempDir temp;
		TarEntry e;
		e.name = "big.png";
		e.declared_size = limit;
		e.omit_payload = true;
		const ExtractResult r = ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_FALSE(Mentions(r, "payload exceeds")) << r.error;
	}
	// Non-textures count against the limit too.
	{
		ScopedTempDir temp;
		TarEntry e;
		e.name = "big.bin";
		e.declared_size = limit + 1;
		e.omit_payload = true;
		EXPECT_TRUE(Mentions(ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 0), "payload exceeds"));
	}
}

// ------------------------------------------------------------------------------------------
// tar+zstd: collisions
// ------------------------------------------------------------------------------------------

TEST(TarZstdExtractor, RefusesTwoEntriesForOneFile)
{
	struct Case
	{
		const char* what;
		std::string tar;
	};
	const Case cases[] = {
		{"same path twice", TarBuilder().File("a.png", "1").File("a.png", "2").Finish()},
		{"case variants", TarBuilder().File("A.png", "1").File("a.png", "2").Finish()},
		{"NFC and NFD", TarBuilder().File("caf\xC3\xA9.png", "1").File("cafe\xCC\x81.png", "2").Finish()},
		{"two wrappers, one name",
			TarBuilder().File("one/replacements/a.png", "1").File("two/replacements/a.png", "2").Finish()},
		{"wrapped and bare",
			TarBuilder().File("SLUS-21410/replacements/a.png", "1").File("a.png", "2").Finish()},
		{"bare and wrapped",
			TarBuilder().File("a.png", "1").File("SLUS-21410/replacements/a.png", "2").Finish()},
	};
	for (const Case& c : cases)
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(c.tar, temp.File("out"), 2);
		EXPECT_EQ(r.status, ExtractStatus::Failed) << c.what;
		EXPECT_TRUE(Mentions(r, "duplicate")) << c.what << ": " << r.error;
	}
}

TEST(TarZstdExtractor, RefusesAFileThatIsAlsoAFolder)
{
	// A non-texture file still occupies its name.
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(TarBuilder().File("a", "x").File("a/b.png", "y").Finish(), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(Mentions(r, "conflict")) << r.error;
	}
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(TarBuilder().File("a/b.png", "y").File("a", "x").Finish(), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(Mentions(r, "duplicate")) << r.error;
	}
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(TarBuilder().File("a", "x").Dir("a/").Finish(), temp.File("out"), 0);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
	}
	{
		ScopedTempDir temp;
		const ExtractResult r = ExtractTar(TarBuilder().Dir("a/").Dir("a/").Finish(), temp.File("out"), 0);
		EXPECT_EQ(r.status, ExtractStatus::Failed);
	}
}

TEST(TarZstdExtractor, AFolderEntryMayFollowItsContents)
{
	ScopedTempDir temp;
	const ExtractResult r = ExtractTar(TarBuilder().File("a/b.png", "y").Dir("a/").Finish(), temp.File("out"), 1);
	EXPECT_TRUE(r.IsOk()) << r.error;
}

// ------------------------------------------------------------------------------------------
// tar+zstd: the tar profile
// ------------------------------------------------------------------------------------------

TEST(TarZstdExtractor, RefusesEntryTypesOutsideTheProfile)
{
	for (const char type : {'1', '2', '3', '4', '6', '7', 'x', 'g', 'S', 'K', 'V'})
	{
		ScopedTempDir temp;
		TarEntry e;
		e.name = "a.png";
		e.typeflag = type;
		e.data = "x";
		const ExtractResult r = ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 1);
		EXPECT_EQ(r.status, ExtractStatus::Failed) << type;
		EXPECT_TRUE(Mentions(r, "unsupported entry type")) << type << ": " << r.error;
		EXPECT_TRUE(ListFiles(temp.File("out")).empty()) << type;
	}
}

TEST(TarZstdExtractor, AcceptsAnOldStyleNulTypeflagAndPosixMagic)
{
	ScopedTempDir temp;
	TarEntry e;
	e.name = "a.png";
	e.typeflag = '\0';
	e.data = "x";
	e.magic_version = std::string("ustar\0" "00", 8);
	const ExtractResult r = ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 1);
	EXPECT_TRUE(r.IsOk()) << r.error;
}

TEST(TarZstdExtractor, RefusesMalformedHeaders)
{
	const auto run = [](const TarEntry& e) {
		ScopedTempDir temp;
		return ExtractTar(TarBuilder().Entry(e).Finish(), temp.File("out"), 1);
	};
	TarEntry base;
	base.name = "a.png";
	base.data = "x";

	TarEntry checksum = base;
	checksum.bad_checksum = true;
	EXPECT_TRUE(Mentions(run(checksum), "checksum"));

	TarEntry magic = base;
	magic.magic_version = std::string("star\0\0" "00", 8);
	EXPECT_TRUE(Mentions(run(magic), "magic"));

	TarEntry base256 = base;
	base256.base256_size = true;
	EXPECT_TRUE(Mentions(run(base256), "size field"));

	// A ustar prefix would put the file in a folder this reader does not look at.
	TarEntry prefix = base;
	prefix.magic_version = std::string("ustar\0" "00", 8);
	prefix.prefix = "some/prefix";
	EXPECT_TRUE(Mentions(run(prefix), "prefix"));

	TarEntry dir_with_payload;
	dir_with_payload.name = "d/";
	dir_with_payload.typeflag = '5';
	dir_with_payload.data = "x";
	EXPECT_EQ(run(dir_with_payload).status, ExtractStatus::Failed);
}

TEST(TarZstdExtractor, OnlyTheRootFolderMayHaveNoName)
{
	ScopedTempDir temp;
	TarEntry dir;
	dir.name = "";
	dir.typeflag = '5';
	EXPECT_TRUE(ExtractTar(TarBuilder().Entry(dir).File("a.png", "x").Finish(), temp.File("out"), 1).IsOk());

	TarEntry file;
	file.name = "";
	file.data = "x";
	const ExtractResult r = ExtractTar(TarBuilder().Entry(file).Finish(), temp.File("out2"), 0);
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "empty name")) << r.error;
}

TEST(TarZstdExtractor, ChecksGnuLongNameRecords)
{
	const auto run = [](const TarBuilder& b) {
		ScopedTempDir temp;
		return ExtractTar(b.Finish(), temp.File("out"), 1);
	};

	// Not NUL terminated.
	{
		TarBuilder b;
		b.LongNamePayload("long/name.png");
		b.File("short.png", "x");
		EXPECT_TRUE(Mentions(run(b), "NUL-terminated"));
	}
	// Empty, and too large (4098 bytes).
	{
		TarBuilder b;
		b.LongNamePayload("");
		b.File("short.png", "x");
		EXPECT_TRUE(Mentions(run(b), "GNU L payload"));
	}
	{
		TarBuilder b;
		b.LongNamePayload(std::string(4097, 'a') + '\0');
		b.File("short.png", "x");
		EXPECT_TRUE(Mentions(run(b), "GNU L payload"));
	}
	// Two in a row.
	{
		TarBuilder b;
		b.LongName("one.png").LongName("two.png");
		b.File("short.png", "x");
		EXPECT_TRUE(Mentions(run(b), "without a following entry"));
	}
	// The record's name is the one that counts, and goes through the same path rules.
	{
		TarBuilder b;
		b.LongName("../escape.png");
		b.File("short.png", "x");
		EXPECT_TRUE(Mentions(run(b), "rejected path"));
	}
}

TEST(TarZstdExtractor, ChecksTheEndOfTheArchive)
{
	const auto run = [](const std::string& tar) {
		ScopedTempDir temp;
		return ExtractTar(tar, temp.File("out"), 1);
	};
	TarBuilder b;
	b.File("a.png", "x");

	EXPECT_TRUE(run(b.Finish()).IsOk());
	EXPECT_TRUE(run(b.Finish(2, 0)).IsOk()); // no blocking factor padding at all

	// One end block, then another entry where the second belongs.
	{
		TarBuilder c;
		c.File("a.png", "x");
		std::string tar = c.Finish(1, 0);
		tar += TarHeader(TarEntry{.name = "b.png"});
		EXPECT_TRUE(Mentions(run(tar), "second end block"));
	}
	// Only one end block and the stream ends.
	EXPECT_EQ(run(b.Finish(1, 0)).status, ExtractStatus::Failed);
	// No end block at all.
	EXPECT_EQ(run(b.Finish(0, 0)).status, ExtractStatus::Failed);
	// Padding that is not zero.
	{
		std::string tar = b.Finish(2, 0);
		tar += std::string(100, '\0') + "x";
		EXPECT_TRUE(Mentions(run(tar), "nonzero trailing padding"));
	}
	// A payload that stops short.
	{
		TarEntry e;
		e.name = "a.png";
		e.declared_size = 5000;
		e.data = "short";
		e.omit_payload = false;
		EXPECT_EQ(run(TarBuilder().Entry(e).Finish(0, 0)).status, ExtractStatus::Failed);
	}
}

TEST(TarZstdExtractor, HoldsTheArchiveToTheCatalogsNumbers)
{
	TarBuilder b;
	b.File("a.png", "x").File("b.png", "y");
	const std::string tar = b.Finish();
	const std::string zst = ZstdCompress(tar);

	const auto run = [&](u64 bytes, u64 count) {
		ScopedTempDir temp;
		return TarZstdExtractor::ExtractMemory(Span(zst), temp.File("out"), TarZstdExpectations{bytes, count});
	};

	EXPECT_TRUE(run(tar.size(), 2).IsOk());

	ExtractResult r = run(tar.size(), 3);
	EXPECT_TRUE(Mentions(r, "texture count mismatch")) << r.error;
	r = run(tar.size(), 1);
	EXPECT_TRUE(Mentions(r, "texture count mismatch")) << r.error;

	// More bytes promised than there are: the stream ends short of it.
	r = run(tar.size() + 1, 2);
	EXPECT_TRUE(Mentions(r, "size mismatch")) << r.error;
	// Fewer promised than there are: the decoder is stopped at the cap.
	r = run(tar.size() - 1, 2);
	EXPECT_TRUE(Mentions(r, "exceeds")) << r.error;
	// Nonsense promises are refused before anything is read.
	EXPECT_EQ(run(0, 2).status, ExtractStatus::Failed);
	EXPECT_EQ(run(TarZstdExtractor::MAX_DECOMPRESSED_BYTES + 1, 2).status, ExtractStatus::Failed);
}

// ------------------------------------------------------------------------------------------
// tar+zstd: the zstd layer
// ------------------------------------------------------------------------------------------

TEST(TarZstdExtractor, WantsExactlyOneFrameAndNothingAfterIt)
{
	const std::string tar = OneFileTar("a.png");
	const std::string zst = ZstdCompress(tar);
	const TarZstdExpectations expected{tar.size(), 1};
	const auto run = [&](const std::string& input) {
		ScopedTempDir temp;
		return TarZstdExtractor::ExtractMemory(Span(input), temp.File("out"), expected);
	};

	EXPECT_TRUE(run(zst).IsOk());

	ExtractResult r = run(zst + "x");
	EXPECT_TRUE(Mentions(r, "trailing")) << r.error;
	r = run(zst + zst); // two frames
	EXPECT_TRUE(Mentions(r, "trailing")) << r.error;
	r = run(zst.substr(0, zst.size() / 2));
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	r = run(zst.substr(0, zst.size() - 1));
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	r = run(std::string());
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	r = run(std::string(1000, 'x'));
	EXPECT_EQ(r.status, ExtractStatus::Failed);
}

TEST(TarZstdExtractor, RefusesAWindowOverTwoToTheTwentySeventh)
{
	const std::string tar = OneFileTar("a.png");
	const TarZstdExpectations expected{tar.size(), 1};

	ScopedTempDir ok;
	const std::string w27 = ZstdCompressWithWindow(tar, 27);
	EXPECT_TRUE(TarZstdExtractor::ExtractMemory(Span(w27), ok.File("out"), expected).IsOk());

	ScopedTempDir bad;
	const std::string w28 = ZstdCompressWithWindow(tar, 28);
	const ExtractResult r = TarZstdExtractor::ExtractMemory(Span(w28), bad.File("out"), expected);
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "zstd")) << r.error;
}

TEST(TarZstdExtractor, ReadsFromAFileAndReportsAMissingOne)
{
	ScopedTempDir temp;
	const std::string tar = OneFileTar("a.png");
	const std::string zst = ZstdCompress(tar);
	ASSERT_TRUE(FileSystem::WriteBinaryFile(temp.File("pack.zst").c_str(), zst.data(), zst.size()));

	const TarZstdExpectations expected{tar.size(), 1};
	EXPECT_TRUE(TarZstdExtractor::ExtractFile(temp.File("pack.zst"), temp.File("out"), expected).IsOk());
	const ExtractResult missing = TarZstdExtractor::ExtractFile(temp.File("nope.zst"), temp.File("out2"), expected);
	EXPECT_EQ(missing.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(missing, "Could not open")) << missing.error;
}

TEST(TarZstdExtractor, StopsWhenCancelled)
{
	TarBuilder b;
	for (int i = 0; i < 100; i++)
		b.File(fmt::format("t{}.png", i), "x");
	const std::string tar = b.Finish();

	// Before it starts.
	{
		ScopedTempDir temp;
		CancelToken cancel;
		cancel.Cancel();
		const ExtractResult r = ExtractTar(tar, temp.File("out"), 100, {}, cancel);
		EXPECT_EQ(r.status, ExtractStatus::Cancelled);
		EXPECT_TRUE(ListFiles(temp.File("out")).empty());
	}
	// From the progress callback, part way.
	{
		ScopedTempDir temp;
		CancelToken cancel;
		const ExtractResult r = ExtractTar(tar, temp.File("out"), 100, [&](u64 done, u64) {
			if (done >= 32)
				cancel.Cancel();
		}, cancel);
		EXPECT_EQ(r.status, ExtractStatus::Cancelled);
		const size_t written = ListFiles(temp.File("out")).size();
		EXPECT_GE(written, 32u);
		EXPECT_LT(written, 100u);
	}
}

// ------------------------------------------------------------------------------------------
// zip (schema 1)
// ------------------------------------------------------------------------------------------

TEST(ZipTextureExtractor, ExtractsTexturesStrippingTheWrapper)
{
	ScopedTempDir temp;
	ASSERT_TRUE(WriteZip(temp.File("pack.zip"),
		{
			{"SLUS-21410/replacements/0123456789abcdef-00000001.png", "one"},
			{"SLUS-21410/replacements/sub/fedcba9876543210-0000ffff.dds", "two"},
			{"SLUS-21410/replacements/readme.txt", "no"},
			{"SLUS-21410/replacements/", ""},
			{"__MACOSX/SLUS-21410/replacements/._0123456789abcdef-00000001.png", "junk"},
			{"SLUS-21410\\replacements\\win\\1111111111111111-00000003.ktx", "three"},
		}));

	std::vector<std::pair<u64, u64>> calls;
	const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"), [&](u64 d, u64 t) { calls.emplace_back(d, t); });
	ASSERT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(r.textures, 3u);
	EXPECT_EQ(ListFiles(temp.File("out")),
		(std::set<std::string>{"0123456789abcdef-00000001.png", "sub/fedcba9876543210-0000ffff.dds", "win/1111111111111111-00000003.ktx"}));
	EXPECT_EQ(ReadFileOrEmpty(temp.File("out/sub/fedcba9876543210-0000ffff.dds")), "two");
	ASSERT_FALSE(calls.empty());
	EXPECT_EQ(calls.back(), (std::pair<u64, u64>{3, 3}));
}

TEST(ZipTextureExtractor, RefusesUnsafePathsAndDuplicates)
{
	{
		ScopedTempDir temp;
		ASSERT_TRUE(WriteZip(temp.File("pack.zip"), {{"../evil.png", "x"}}));
		const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("work/out"));
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(ListFiles(temp.File("work")).empty());
	}
	{
		ScopedTempDir temp;
		ASSERT_TRUE(WriteZip(temp.File("pack.zip"), {{"a\\..\\..\\evil.png", "x"}}));
		EXPECT_EQ(ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("work/out")).status, ExtractStatus::Failed);
		EXPECT_TRUE(ListFiles(temp.File("work")).empty());
	}
	{
		ScopedTempDir temp;
		ASSERT_TRUE(WriteZip(temp.File("pack.zip"), {{"one/replacements/a.png", "1"}, {"two/replacements/A.PNG", "2"}}));
		const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"));
		EXPECT_EQ(r.status, ExtractStatus::Failed);
		EXPECT_TRUE(Mentions(r, "duplicate")) << r.error;
	}
}

TEST(ZipTextureExtractor, ReportsAnUnreadableArchiveAndAnEmptyOne)
{
	ScopedTempDir temp;
	ASSERT_TRUE(FileSystem::WriteStringToFile(temp.File("junk.zip").c_str(), "this is not a zip"));
	EXPECT_EQ(ZipTextureExtractor::ExtractFile(temp.File("junk.zip"), temp.File("out")).status, ExtractStatus::Failed);
	EXPECT_EQ(ZipTextureExtractor::ExtractFile(temp.File("missing.zip"), temp.File("out")).status, ExtractStatus::Failed);

	// A zip with no textures extracts fine and says 0; the installer decides what that means.
	ASSERT_TRUE(WriteZip(temp.File("empty.zip"), {{"readme.txt", "hi"}}));
	const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("empty.zip"), temp.File("out2"));
	EXPECT_TRUE(r.IsOk());
	EXPECT_EQ(r.textures, 0u);
}

namespace
{
	// A zip of [count] small PNGs whose central directory says each is [declared] bytes: what a hostile zip does to
	// pass the per-file limit and still ask for more than the disk has. (The data itself is a few bytes, so a
	// reader that believes the declaration fails when it gets to the first file; the extractor must not get that
	// far.)
	void WriteZipDeclaring(const std::string& path, int count, u32 declared)
	{
		std::vector<std::pair<std::string, std::string>> files;
		for (int i = 0; i < count; i++)
			files.emplace_back(fmt::format("{:016x}-00000001.png", i + 1), std::string(1000, 'x'));
		ASSERT_TRUE(WriteZip(path, files));

		std::string bytes = ReadFileOrEmpty(path);
		int patched = 0;
		for (size_t at = bytes.find("PK\x01\x02"); at != std::string::npos; at = bytes.find("PK\x01\x02", at + 4))
		{
			ASSERT_LE(at + 28, bytes.size());
			for (int b = 0; b < 4; b++)
				bytes[at + 24 + static_cast<size_t>(b)] = static_cast<char>((declared >> (8 * b)) & 0xFF); // uncompressed size
			patched++;
		}
		ASSERT_EQ(patched, count);
		ASSERT_TRUE(FileSystem::WriteStringToFile(path.c_str(), bytes));
	}
} // namespace

TEST(ZipTextureExtractor, RefusesAZipWhoseDeclaredSizesAddUpToMoreThanTheCap)
{
	// 33 entries of 511 MiB: each under the per-file limit, 16.4 GiB together.
	ScopedTempDir temp;
	WriteZipDeclaring(temp.File("pack.zip"), 33, 511u * 1024 * 1024);

	const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"));
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "exceeds")) << r.error;
	// Refused before anything was written, the destination folder included.
	EXPECT_FALSE(FileSystem::DirectoryExists(temp.File("out").c_str()));
	EXPECT_EQ(r.textures, 0u);
}

TEST(ZipTextureExtractor, AcceptsADeclaredTotalJustUnderTheCapUpToTheFirstFile)
{
	// 32 entries of 511 MiB is 15.97 GiB: allowed by the sum. The declared sizes are false here, so extraction
	// stops at the first file, but it got as far as writing it.
	ScopedTempDir temp;
	WriteZipDeclaring(temp.File("pack.zip"), 32, 511u * 1024 * 1024);

	const ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"), {}, {},
		[](const std::string&) { return std::optional<u64>(u64(1) << 50); });
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "shorter")) << r.error;
}

TEST(ZipTextureExtractor, NeedsRoomForTheDeclaredSizesBeforeWritingAnything)
{
	ScopedTempDir temp;
	ASSERT_TRUE(WriteZip(temp.File("pack.zip"), {{"a.png", std::string(100000, 'a')}, {"b.png", std::string(100000, 'b')}}));
	const u64 needed = 200000 + ZipTextureExtractor::FREE_SPACE_MARGIN;

	// One byte short: refused, nothing written.
	std::vector<std::string> asked;
	ExtractResult r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"), {}, {}, [&](const std::string& path) {
		asked.push_back(path);
		return std::optional<u64>(needed - 1);
	});
	EXPECT_EQ(r.status, ExtractStatus::Failed);
	EXPECT_TRUE(Mentions(r, "free")) << r.error;
	EXPECT_TRUE(ListFiles(temp.File("out")).empty());
	ASSERT_EQ(asked.size(), 1u);
	EXPECT_EQ(asked[0], temp.File("out")); // the filesystem the files would be written to

	// Exactly enough: extracted.
	r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"), {}, {}, [&](const std::string&) { return std::optional<u64>(needed); });
	EXPECT_TRUE(r.IsOk()) << r.error;
	EXPECT_EQ(r.textures, 2u);

	// A filesystem that cannot say is tried.
	r = ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out2"), {}, {}, [&](const std::string&) { return std::optional<u64>(); });
	EXPECT_TRUE(r.IsOk()) << r.error;
}

TEST(ZipTextureExtractor, StopsWhenCancelled)
{
	ScopedTempDir temp;
	ASSERT_TRUE(WriteZip(temp.File("pack.zip"), {{"a.png", "1"}, {"b.png", "2"}}));
	CancelToken cancel;
	cancel.Cancel();
	EXPECT_EQ(ZipTextureExtractor::ExtractFile(temp.File("pack.zip"), temp.File("out"), {}, cancel).status, ExtractStatus::Cancelled);
}
