// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The vu-aot sidecar format (vuaot/VuAotPersist.h): a plan file round-trips, and every kind of
// damage or mismatch is refused by the parser without reading past the buffer. The live save and
// load paths are tested in vuaot_live_tests.cpp (VuAotCache).

#if defined(_M_ARM64) || defined(__aarch64__)

#include "arm64/vuaot/VuAotPersist.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

namespace
{
using namespace vuaot::persist;

Identity Id()
{
	Identity id;
	id.contentHashLo = 0x1111222233334444ull;
	id.contentHashHi = 0x5555666677778888ull;
	id.sentinelLo = 0x0123456789abcdefull;
	id.sentinelHi = 0xfedcba9876543210ull;
	id.buildLo = 0xaaaabbbbccccddddull;
	id.buildHi = 0x9999000011112222ull;
	id.mvuAbi = 63;
	return id;
}

File Sample(std::mt19937& rng)
{
	File f;
	f.id = Id();
	f.tiers = 2;
	f.settled = true;
	f.jumpTargets[0x48] = {0x100, 0x180, 0x3ff8};
	f.jumpTargets[0x200] = {0x8};
	std::vector<u8> micro(0x4000), mask(kMicroWords);
	for (auto& b : micro)
		b = static_cast<u8>(rng());
	for (int r = 0; r < 3; r++)
	{
		Record rec;
		rec.kind = r == 0 ? kGeneration : kRoot;
		rec.flags = static_cast<u8>(kAccepted | (r == 0 ? kViaJump : 0) | (r == 2 ? kStickyDropped | kDropSticky : 0));
		rec.pc = 0x40 + 8 * r;
		for (u32 i = 0; i < kKeyBytes; i++)
			rec.key[i] = static_cast<u8>(rng());
		for (auto& m : mask)
			m = (rng() % 7) == 0;
		rec.SetReadSet(mask.data(), micro.data());
		EXPECT_TRUE(rec.MatchesMicro(micro.data()));
		f.records.push_back(rec);
	}
	return f;
}

// A file whose checksum is recomputed after an edit: what a parser must refuse on the field itself.
void Restamp(std::vector<u8>& bytes)
{
	const u64 sum = XXH3_64bits(bytes.data(), bytes.size() - 8);
	std::memcpy(bytes.data() + bytes.size() - 8, &sum, 8);
}
} // namespace

TEST(VuAotPersist, PlanRoundTrips)
{
	std::mt19937 rng(1);
	const File f = Sample(rng);
	const std::vector<u8> bytes = Serialize(f);
	File g;
	ASSERT_EQ(Parse(bytes.data(), bytes.size(), Id(), g), ParseResult::Ok);
	EXPECT_EQ(g.tiers, f.tiers);
	EXPECT_EQ(g.settled, f.settled);
	EXPECT_EQ(g.jumpTargets, f.jumpTargets);
	ASSERT_EQ(g.records.size(), f.records.size());
	for (size_t i = 0; i < f.records.size(); i++)
	{
		EXPECT_EQ(g.records[i].kind, f.records[i].kind);
		EXPECT_EQ(g.records[i].flags, f.records[i].flags);
		EXPECT_EQ(g.records[i].pc, f.records[i].pc);
		EXPECT_EQ(std::memcmp(g.records[i].key, f.records[i].key, kKeyBytes), 0);
		EXPECT_EQ(g.records[i].readBits, f.records[i].readBits);
		EXPECT_EQ(g.records[i].readValues, f.records[i].readValues);
	}
	EXPECT_EQ(Serialize(g), bytes) << "a parsed file serializes to the same bytes";
}

// The read set says which words a compile read; a change to any of them, and only to them, makes the
// record not match.
TEST(VuAotPersist, ReadSetMatchesOnlyTheWordsRead)
{
	std::vector<u8> micro(0x4000, 0), mask(kMicroWords, 0);
	for (u32 i = 0; i < 0x4000; i++)
		micro[i] = static_cast<u8>(i * 7);
	mask[3] = mask[100] = mask[kMicroWords - 1] = 1;
	Record r;
	r.SetReadSet(mask.data(), micro.data());
	EXPECT_EQ(r.ReadCount(), 3u);
	EXPECT_TRUE(r.MatchesMicro(micro.data()));
	micro[4 * 50] ^= 1; // a word nobody read
	EXPECT_TRUE(r.MatchesMicro(micro.data()));
	micro[4 * 100 + 2] ^= 0x80; // a word the compile read
	EXPECT_FALSE(r.MatchesMicro(micro.data()));
	micro[4 * 100 + 2] ^= 0x80;
	micro[0x4000 - 1] ^= 1; // the last word
	EXPECT_FALSE(r.MatchesMicro(micro.data()));
}

// Every byte flipped anywhere is refused (the checksum, or the magic and version in front of it), and every
// truncation is refused, without reading past the end (ASan builds check the latter).
TEST(VuAotPersist, DamageIsRefused)
{
	std::mt19937 rng(2);
	const std::vector<u8> good = Serialize(Sample(rng));
	File g;
	for (size_t i = 0; i < good.size(); i++)
	{
		std::vector<u8> bad = good;
		bad[i] ^= 0x10;
		EXPECT_NE(Parse(bad.data(), bad.size(), Id(), g), ParseResult::Ok) << "byte " << i;
	}
	for (size_t n = 0; n < good.size(); n++)
	{
		std::vector<u8> cut(good.begin(), good.begin() + n);
		EXPECT_NE(Parse(cut.data(), cut.size(), Id(), g), ParseResult::Ok) << "length " << n;
	}
	EXPECT_EQ(Parse(nullptr, 0, Id(), g), ParseResult::Truncated);
	std::vector<u8> cut(good.begin(), good.begin() + good.size() / 2);
	EXPECT_EQ(Parse(cut.data(), cut.size(), Id(), g), ParseResult::BadChecksum) << "half a file";
}

// Each identity field, edited with the checksum recomputed, is refused as that field.
TEST(VuAotPersist, IdentityFieldsAreChecked)
{
	std::mt19937 rng(3);
	const std::vector<u8> good = Serialize(Sample(rng));
	File g;
	struct Case
	{
		size_t offset;
		ParseResult expect;
	};
	// FileHeader: magic 0, format 4, abi 8, mvuAbi 12, keyBytes 16, tiers 20, settled 24, reserved 28,
	// content hash 32, sentinel 48, build 64.
	const Case cases[] = {{0, ParseResult::BadMagic}, {4, ParseResult::BadFormatVersion}, {8, ParseResult::BadAbiVersion},
		{12, ParseResult::BadMvuAbi}, {16, ParseResult::BadKeySize}, {32, ParseResult::OtherProgram}, {40, ParseResult::OtherProgram},
		{48, ParseResult::OtherSentinel}, {56, ParseResult::OtherSentinel}, {64, ParseResult::OtherBuild}, {72, ParseResult::OtherBuild}};
	for (const Case& c : cases)
	{
		std::vector<u8> bad = good;
		bad[c.offset] ^= 1;
		Restamp(bad);
		EXPECT_EQ(Parse(bad.data(), bad.size(), Id(), g), c.expect) << "offset " << c.offset << ": " << ParseResultName(c.expect);
	}
	// and the same file is refused by a process that expects another identity
	Identity other = Id();
	other.buildLo ^= 1;
	EXPECT_EQ(Parse(good.data(), good.size(), other, g), ParseResult::OtherBuild);
	other = Id();
	other.mvuAbi++;
	EXPECT_EQ(Parse(good.data(), good.size(), other, g), ParseResult::BadMvuAbi);
}

// Counts that point past the end, or past the parser's bounds, are refused before any allocation they
// would ask for, checksum or not.
TEST(VuAotPersist, CountsPastTheEndAreRefused)
{
	File f;
	f.id = Id();
	f.jumpTargets[8] = {16};
	std::vector<u8> bytes = Serialize(f);
	File g;
	ASSERT_EQ(Parse(bytes.data(), bytes.size(), Id(), g), ParseResult::Ok);
	const size_t jumpCountAt = 80, firstTargetCountAt = 88, recordCountAt = 96;
	auto edit = [&](size_t at, u32 v) {
		std::vector<u8> bad = bytes;
		std::memcpy(bad.data() + at, &v, 4);
		Restamp(bad);
		return Parse(bad.data(), bad.size(), Id(), g);
	};
	EXPECT_EQ(edit(jumpCountAt, 0xffffffffu), ParseResult::Malformed);
	EXPECT_EQ(edit(jumpCountAt, 2), ParseResult::Malformed);
	EXPECT_EQ(edit(firstTargetCountAt, 0x10000000u), ParseResult::Malformed);
	EXPECT_EQ(edit(recordCountAt, 1), ParseResult::Malformed);
	EXPECT_EQ(edit(recordCountAt, 0x7fffffffu), ParseResult::Malformed);
	// a trailing byte inside the checksummed body
	std::vector<u8> longer = bytes;
	longer.insert(longer.end() - 8, u8{0});
	Restamp(longer);
	EXPECT_EQ(Parse(longer.data(), longer.size(), Id(), g), ParseResult::Malformed);
}

#endif
