// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The vu-aot sidecar format (vuaot/VuAotPersist.h): a file of plans and region code round-trips, and
// every kind of damage or mismatch is refused by the parser without reading past the buffer. The
// live save and load paths are tested in vuaot_live_tests.cpp (VuAotCache).

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
	id.layoutHash = 0x5a5a1234u;
	return id;
}

// A code section of the shape the worker leaves: two nodes, every fixup form and target.
RegionCode SampleCode(std::mt19937& rng)
{
	RegionCode c;
	c.code.resize(256);
	for (auto& b : c.code)
		b = static_cast<u8>(rng());
	u8 k0[kKeyBytes], k1[kKeyBytes];
	for (u32 i = 0; i < kKeyBytes; i++)
	{
		k0[i] = static_cast<u8>(rng());
		k1[i] = static_cast<u8>(rng());
	}
	c.nodes.push_back({0x40, c.KeyIndex(k0), 0});
	c.nodes.push_back({0x48, c.KeyIndex(k1), 32});
	EXPECT_EQ(c.KeyIndex(k0), 0u) << "a key is stored once";
	c.exits.push_back({0x100, c.KeyIndex(k1)});
	c.pending.push_back({0x108, c.KeyIndex(k0)});
	c.blockRefs.push_back({0x110, c.KeyIndex(k1)});
	c.tails.push_back({1, c.KeyIndex(k0)});
	c.ranges.push_back({0x40, 0x48});
	Placeholder stub, exit, tail, pend;
	stub.kind = kPhStub;
	stub.index = 2;
	exit.kind = kPhExit;
	tail.kind = kPhTail;
	pend.kind = kPhPending;
	c.ph = {stub, exit, tail, pend};
	const auto fix = [&](u32 off, u8 form, u8 target, u8 width, u32 index, s64 value) {
		Fixup f;
		f.offset = off;
		f.form = form;
		f.target = target;
		f.width = width;
		f.index = index;
		f.value = value;
		c.fixups.push_back(f);
	};
	fix(0, kFormMovRun, kTargetIntra, 4, 0, 200);
	fix(16, kFormRel26, kTargetPlaceholder, 0, 1, 0);
	fix(20, kFormMovRun, kTargetPlaceholder, 4, 0, 0);
	fix(36, kFormMovRun, kTargetImage, 3, 0, 0x123450);
	fix(48, kFormRel26, kTargetImage, 0, 0, 0x2000);
	fix(52, kFormMovRun, kTargetData, 2, 0, 0x88);
	fix(60, kFormMovRun, kTargetBlockRef, 3, 0, 0);
	fix(200, kFormData64, kTargetProgram, 0, 0, 0);
	fix(208, kFormData64, kTargetNodeHandle, 0, 1, 0);
	c.regions = 1;
	c.regionNodes = 2;
	c.regionExits = 1;
	c.regionBytes = 256;
	return c;
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
		if (r != 1)
		{
			rec.flags |= kHasCode;
			rec.code = SampleCode(rng);
		}
		f.records.push_back(rec);
	}
	DeclinedRoot d;
	d.pc = 0x1f8;
	d.key[3] = 7;
	f.declinedRoots.push_back(d);
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
		const RegionCode &a = g.records[i].code, &b = f.records[i].code;
		EXPECT_EQ(g.records[i].HasCode(), f.records[i].HasCode());
		EXPECT_EQ(a.code, b.code);
		EXPECT_EQ(a.keys, b.keys);
		ASSERT_EQ(a.fixups.size(), b.fixups.size());
		EXPECT_EQ(std::memcmp(a.fixups.data(), b.fixups.data(), a.fixups.size() * sizeof(Fixup)), 0);
		EXPECT_EQ(a.nodes.size(), b.nodes.size());
		EXPECT_EQ(a.ph.size(), b.ph.size());
		EXPECT_EQ(a.regionBytes, b.regionBytes);
	}
	EXPECT_TRUE(g.records[0].HasCode() && !g.records[1].HasCode()) << "a record may hold its plan alone";
	ASSERT_EQ(g.declinedRoots.size(), 1u);
	EXPECT_EQ(g.declinedRoots[0].pc, 0x1f8u);
	EXPECT_EQ(g.declinedRoots[0].key, f.declinedRoots[0].key);
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
	// FileHeader: magic 0, format 4, abi 8, mvuAbi 12, keyBytes 16, tiers 20, settled 24, layout hash 28,
	// content hash 32, sentinel 48, build 64.
	const Case cases[] = {{0, ParseResult::BadMagic}, {4, ParseResult::BadFormatVersion}, {8, ParseResult::BadAbiVersion},
		{12, ParseResult::BadMvuAbi}, {16, ParseResult::BadKeySize}, {28, ParseResult::OtherLayout}, {32, ParseResult::OtherProgram}, {40, ParseResult::OtherProgram},
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
	other = Id();
	other.layoutHash ^= 0x100;
	EXPECT_EQ(Parse(good.data(), good.size(), other, g), ParseResult::OtherLayout);
}

// A code section whose tables do not hold together is refused, checksum or not: a fixup or an entry past
// the code, an index past its table, a form that cannot carry its target.
TEST(VuAotPersist, CodeOfTheWrongShapeIsRefused)
{
	std::mt19937 rng(4);
	const auto parsed = [](const File& f) {
		const std::vector<u8> bytes = Serialize(f);
		File g;
		return Parse(bytes.data(), bytes.size(), Id(), g);
	};
	const File good = Sample(rng);
	ASSERT_EQ(parsed(good), ParseResult::Ok);
	using Edit = void (*)(RegionCode&);
	const struct
	{
		const char* what;
		Edit edit;
	} cases[] = {
		{"fixup past the end", [](RegionCode& c) { c.fixups[3].offset = static_cast<u32>(c.code.size() - 8); }},
		{"misaligned instruction", [](RegionCode& c) { c.fixups[1].offset = 18; }},
		{"run wider than 4", [](RegionCode& c) { c.fixups[3].width = 5; }},
		{"run of width 0", [](RegionCode& c) { c.fixups[3].width = 0; }},
		{"intra target past the code", [](RegionCode& c) { c.fixups[0].value = static_cast<s64>(c.code.size()); }},
		{"intra target in a short run", [](RegionCode& c) { c.fixups[0].width = 3; }},
		{"placeholder past the table", [](RegionCode& c) { c.fixups[1].index = 9; }},
		{"block ref past the table", [](RegionCode& c) { c.fixups[6].index = 1; }},
		{"node handle past the table", [](RegionCode& c) { c.fixups[8].index = 2; }},
		{"program word as a branch", [](RegionCode& c) { c.fixups[7].form = kFormRel26; }},
		{"image word as data", [](RegionCode& c) { c.fixups[3].form = kFormData64; }},
		{"unknown target", [](RegionCode& c) { c.fixups[3].target = kTargetCount; }},
		{"unknown form", [](RegionCode& c) { c.fixups[3].form = kFormCount; }},
		{"entry past the code", [](RegionCode& c) { c.nodes[1].entryOff = static_cast<u32>(c.code.size()); }},
		{"key past the table", [](RegionCode& c) { c.nodes[1].key = 7; }},
		{"tail of no node", [](RegionCode& c) { c.tails[0].node = 2; }},
		{"exit placeholder past the exits", [](RegionCode& c) { c.ph[1].index = 1; }},
		{"stub id past the stubs", [](RegionCode& c) { c.ph[0].index = kMaxStubs; }},
		{"unknown placeholder kind", [](RegionCode& c) { c.ph[0].kind = kPhCount; }},
		{"no nodes", [](RegionCode& c) { c.nodes.clear(); c.tails.clear(); c.fixups.pop_back(); }},
		{"code not whole instructions", [](RegionCode& c) { c.code.push_back(0); }},
		{"node PC past micro memory", [](RegionCode& c) { c.nodes[0].pc = 0x4000; }},
	};
	for (const auto& k : cases)
	{
		File bad = good;
		k.edit(bad.records[0].code);
		EXPECT_EQ(parsed(bad), ParseResult::Malformed) << k.what;
	}
	// and a code table count that runs past the file, checksum restamped
	File one;
	one.id = Id();
	Record r;
	r.flags = kHasCode;
	r.code = SampleCode(rng);
	std::vector<u8> mask(kMicroWords, 0), micro(0x4000, 0);
	r.SetReadSet(mask.data(), micro.data());
	one.records.push_back(r);
	std::vector<u8> bytes = Serialize(one);
	File g;
	ASSERT_EQ(Parse(bytes.data(), bytes.size(), Id(), g), ParseResult::Ok);
	// (header 80, no jump blocks 4, one record 4, its header 108, bitmap 512, no values: the code header)
	const size_t codeHeaderAt = 80 + 4 + 4 + 108 + kReadBitmapBytes;
	for (size_t field = 0; field < 10; field++)
	{
		std::vector<u8> bad = bytes;
		const u32 huge = 0x40000000u;
		std::memcpy(bad.data() + codeHeaderAt + 4 * field, &huge, 4);
		Restamp(bad);
		EXPECT_EQ(Parse(bad.data(), bad.size(), Id(), g), ParseResult::Malformed) << "count " << field;
	}
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
	const size_t declinedCountAt = recordCountAt + 4;
	EXPECT_EQ(edit(declinedCountAt, 1), ParseResult::Malformed);
	EXPECT_EQ(edit(declinedCountAt, 0xffffffffu), ParseResult::Malformed);
	// a trailing byte inside the checksummed body
	std::vector<u8> longer = bytes;
	longer.insert(longer.end() - 8, u8{0});
	Restamp(longer);
	EXPECT_EQ(Parse(longer.data(), longer.size(), Id(), g), ParseResult::Malformed);
}

#endif
