// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// EE cache conformance against a real PS2, round 3 -- the two cheap leftovers
// after round 2. A probe was run on the console over ps2link and its capture
// reshaped into autocases_eecache3.h.
//
// 1. WHAT AN UNCACHED STORE DOES TO A RESIDENT LINE. Rounds 1 and 2 showed
//    that an uncached *load* sees stale RAM while a dirty line exists -- every
//    case that read through the 0x20000000 alias after a cached store got the
//    preset back. The store direction had never been asked, and it is the
//    direction that loses data. The R5900 does not snoop, and all three cases
//    say so:
//
//      * to a resident CLEAN line: RAM takes the store, the line does not, and
//        a cached read afterwards still returns the old value.
//      * to a resident DIRTY line: the write-back overwrites it. The uncached
//        store is simply lost.
//      * to a DIFFERENT WORD of a dirty line: also lost. This is the one worth
//        having. A game that pokes one word through UNCACHED_SEG loses the
//        poke if some *neighbouring* word of the same 64-byte line happens to
//        be dirty, and nothing about the two addresses suggests they interact.
//
//    PCSX2 gets all three right, for the right reason -- its write-back copies
//    the whole 64-byte line and its uncached path does not consult the cache --
//    so this is pinned as agreement rather than recorded as a divergence.
//
// 2. THE INSTRUCTION CACHE'S REPLACEMENT RULE. Round 1 pinned the D-cache's:
//    `way = LRF0 ^ LRF1`, and the filled way's LRF toggles. The I-cache had
//    never been asked, and it has no dirty bit and therefore one less reason
//    to care which way it evicts, so strict LRU or random would both have been
//    defensible answers.
//
//    Filling an instruction-cache set means executing from it -- there is no
//    fill op -- so the probe uses five ordinary compiled C functions, each
//    `aligned(0x2000)`. 8 KB apart leaves vaddr[12:6] alone, which is exactly
//    what puts all five in one set (round 2 measured that index width), and
//    each is padded past four lines so four independent sets are sampled at
//    once. It is the same rule: for every one of the twenty (depth, line)
//    observations, exactly one of the four possible initial LRF states
//    reproduces the occupants AND both LRF bits.
//
// 3. `cache 0x07` really is IXIN. Rounds 1 and 2 both listed it as inferred.
//    It is index-addressed, way-selected by bit 0, clears V, and leaves the
//    tag address readable -- exactly what DXIN does on the data side.
//
// PCSX2 has no instruction cache, so 2 and 3 are recorded against a model that
// answers nothing; the tripwires are what turn green if one ever appears. What
// IS checkable today is that the rule the console's I-cache follows is the
// rule PCSX2 already implements for its D-cache, and that is asserted directly
// against both engines' numbers.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Cache.h"
#include "Memory.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#include <cstdio>
#include <cstring>

#include "autocases_eecache3.h"

using namespace console_eecache3;
using recompiler_tests::EeRecTestHarness;

namespace
{
constexpr u32 kBase = 0x00020000;
constexpr u32 kProbeLine = kBase + 0x100;
constexpr u32 kStride = 0x1000;
constexpr u32 kCacheOpcode = 0x2Fu;

void RunCacheOp(u32 op, u32 addr)
{
	cpuRegs.GPR.r[mips::reg::t0].UD[0] = static_cast<s64>(static_cast<s32>(addr));
	cpuRegs.code = (kCacheOpcode << 26) | (static_cast<u32>(mips::reg::t0) << 21) |
	               ((op & 0x1F) << 16);
	R5900::Interpreter::OpcodeImpl::CACHE();
}

u32 ReadTag(u32 addr)
{
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x10, addr); // DXLTG
	return cpuRegs.CP0.n.TagLo;
}

const EeCache3Case& CaseById(int id)
{
	for (int i = 0; i < kEeCache3CaseCount; i++)
	{
		if (kEeCache3Cases[i].id == id)
			return kEeCache3Cases[i];
	}
	ADD_FAILURE() << "no console case " << id;
	return kEeCache3Cases[0];
}

u32 Obs(int id, const char* name)
{
	const EeCache3Case& c = CaseById(id);
	for (int i = 0; i < c.n_obs; i++)
	{
		if (std::strcmp(c.obs[i].name, name) == 0)
			return c.obs[i].raw;
	}
	ADD_FAILURE() << "case " << id << " has no observation " << name;
	return 0;
}

// The rule round 1 measured on the D-cache, run forward.
void Simulate(int k, int lrf0, int lrf1, int* occ, int* out_lrf0, int* out_lrf1)
{
	int lrf[2] = {lrf0, lrf1};
	occ[0] = -1;
	occ[1] = -1;
	for (int i = 0; i < k; i++)
	{
		const int way = lrf[0] ^ lrf[1];
		occ[way] = i;
		lrf[way] ^= 1;
	}
	*out_lrf0 = lrf[0];
	*out_lrf1 = lrf[1];
}
} // namespace

// ---------------------------------------------------------------------------
// The capture. Checked at build time as well as at regeneration time, so a
// hand-edited header cannot quietly change what the tests below believe.

TEST(EeCache3Console, ConsoleCaptureIsSelfConsistent)
{
	// The control: a write-back cache, and DHWBIN reaches RAM.
	EXPECT_EQ(Obs(1, "ram_after_preset"), CaseById(1).p);
	EXPECT_EQ(Obs(1, "ram_after_cached_store"), CaseById(1).p);
	EXPECT_EQ(Obs(1, "ram_after_dhwbin"), CaseById(1).m);

	// Five baits, 8 KB apart, five distinct tags, one shared set.
	for (int i = 0; i < kBaitCount; i++)
	{
		EXPECT_EQ(kBaits[i], kBaits[0] + static_cast<u32>(i) * 0x2000u);
		EXPECT_EQ((kBaits[i] >> 6) & 0x7Fu, kIcacheSet);
	}
	EXPECT_EQ(kIcacheLadderCount, kBaitCount * kSampledLines);
}

// ---------------------------------------------------------------------------
// Uncached stores. Three cases, one property: nothing snoops.

TEST(EeCache3Console, UncachedStoreLeavesACleanLineStale)
{
	const u32 preset = CaseById(20).p;
	const u32 magic = CaseById(20).m;

	// Console: RAM took the store, the resident line did not, and the line is
	// still valid afterwards.
	EXPECT_EQ(Obs(20, "cached_load_of_clean_line"), preset);
	EXPECT_EQ(Obs(20, "ram_after_uncached_store"), magic);
	EXPECT_EQ(Obs(20, "cached_read_after_uncached_store"), preset);
	EXPECT_EQ(Obs(20, "tag_way0_after") & kFlagValid, kFlagValid);
	EXPECT_EQ(Obs(20, "cached_read_after_invalidate"), magic);

	EeRecTestHarness h;
	resetCache();
	memWrite32(kProbeLine, preset);
	ASSERT_EQ(readCache32(kProbeLine), preset) << "the line did not fill";
	memWrite32(kProbeLine, magic); // the uncached store
	EXPECT_EQ(memRead32(kProbeLine), magic);
	EXPECT_EQ(readCache32(kProbeLine), preset)
		<< "PCSX2 snooped the uncached store into the cache";
	RunCacheOp(0x1A, kProbeLine); // DHIN
	EXPECT_EQ(readCache32(kProbeLine), magic);
}

TEST(EeCache3Console, WriteBackOverwritesAnUncachedStore)
{
	const u32 preset = CaseById(21).p;
	const u32 magic = CaseById(21).m;

	// Console: the store reached RAM, the cached read never saw it, and the
	// write-back put the dirty line on top of it. The store is lost.
	EXPECT_EQ(Obs(21, "ram_after_cached_store"), preset);
	EXPECT_EQ(Obs(21, "ram_after_uncached_store"), magic + 0x20);
	EXPECT_EQ(Obs(21, "cached_read_after_uncached_store"), magic);
	EXPECT_EQ(Obs(21, "ram_after_writeback"), magic);

	EeRecTestHarness h;
	resetCache();
	memWrite32(kProbeLine, preset);
	writeCache32(kProbeLine, magic); // resident and dirty
	ASSERT_EQ(memRead32(kProbeLine), preset) << "not a write-back cache";
	memWrite32(kProbeLine, magic + 0x20); // the uncached store
	EXPECT_EQ(readCache32(kProbeLine), magic);
	RunCacheOp(0x18, kProbeLine); // DHWBIN
	EXPECT_EQ(memRead32(kProbeLine), magic)
		<< "PCSX2 let the uncached store survive the write-back";
}

// The one that costs a game data: the two stores are to different words, and
// the loser never went near the cache.
TEST(EeCache3Console, WriteBackClobbersANeighbouringWord)
{
	const u32 preset = CaseById(22).p;
	const u32 magic = CaseById(22).m;

	EXPECT_EQ(Obs(22, "ram_word1_after_uncached_store"), magic + 0x20);
	EXPECT_EQ(Obs(22, "ram_word0_after_writeback"), magic);
	EXPECT_EQ(Obs(22, "ram_word1_after_writeback"), preset + 1);

	EeRecTestHarness h;
	resetCache();
	memWrite32(kProbeLine, preset);
	memWrite32(kProbeLine + 4, preset + 1);
	writeCache32(kProbeLine, magic);      // dirties word 0, fills all 64 bytes
	memWrite32(kProbeLine + 4, magic + 0x20); // uncached store to word 1
	ASSERT_EQ(memRead32(kProbeLine + 4), magic + 0x20);
	RunCacheOp(0x18, kProbeLine); // DHWBIN
	EXPECT_EQ(memRead32(kProbeLine), magic);
	EXPECT_EQ(memRead32(kProbeLine + 4), preset + 1)
		<< "PCSX2's write-back spared the neighbouring word";
}

// ---------------------------------------------------------------------------
// Replacement.

// Twenty observations, and for each one exactly one initial LRF state
// reproduces the occupants and both LRF bits. A rule that got occupancy right
// but not the LRF bits would fit two or four; one that got the ways wrong
// would fit none.
TEST(EeCache3Console, IcacheReplacementIsLrfXorWithToggle)
{
	ASSERT_EQ(kIcacheLadderCount, 20);
	for (int i = 0; i < kIcacheLadderCount; i++)
	{
		const EeCache3Fill& f = kIcacheLadder[i];
		SCOPED_TRACE(::testing::Message() << "k=" << f.k << " line=" << f.line);

		int fits = 0;
		for (int s = 0; s < 4; s++)
		{
			int occ[2], l0, l1;
			Simulate(f.k, s & 1, (s >> 1) & 1, occ, &l0, &l1);
			bool ok = true;
			const u32 tag[2] = {f.tag_way0, f.tag_way1};
			const int lrf[2] = {l0, l1};
			for (int way = 0; way < 2; way++)
			{
				if (occ[way] >= 0)
				{
					if ((tag[way] & 0xFFFFF000u) != kBaits[occ[way]] ||
					    !(tag[way] & kFlagValid))
						ok = false;
				}
				if (static_cast<int>((tag[way] >> 4) & 1u) != lrf[way])
					ok = false;
			}
			if (ok)
				fits++;
		}
		EXPECT_EQ(fits, 1) << "the LRF rule does not uniquely explain this row";
	}
}

// A full invalidate leaves the line naming its old address and keeps its LRF
// bit -- the instruction-side counterpart of round 1's DXIN result.
TEST(EeCache3Console, InvalidateKeepsTheTagAddressOnTheInstructionSide)
{
	for (int i = 0; i < kIcacheLadderCount; i++)
	{
		const EeCache3Fill& f = kIcacheLadder[i];
		if (f.k != 1)
			continue;
		// Only one bait has run, so way 1 is whatever survived FlushCache.
		EXPECT_EQ(f.tag_way1 & kFlagValid, 0u);
		EXPECT_NE(f.tag_way1 & 0xFFFFF000u, 0u)
			<< "FlushCache dropped the tag address";
	}
}

// The console's instruction cache follows the rule PCSX2 already implements
// for its data cache. Driven from a reset model, whose LRF bits are both zero,
// against the console rows whose unique fit is that same state.
TEST(EeCache3Console, Pcsx2DcacheFollowsTheSameRuleTheConsoleIcacheDoes)
{
	int compared = 0;
	for (int i = 0; i < kIcacheLadderCount; i++)
	{
		const EeCache3Fill& f = kIcacheLadder[i];
		if (f.init_lrf0 != 0 || f.init_lrf1 != 0)
			continue;

		EeRecTestHarness h;
		resetCache();
		RunCacheOp(0x16, kProbeLine);     // DXIN way 0
		RunCacheOp(0x16, kProbeLine + 1); // DXIN way 1
		ASSERT_EQ(ReadTag(kProbeLine) & kFlagLrf, 0u) << "the model did not start at LRF 0";
		ASSERT_EQ(ReadTag(kProbeLine + 1) & kFlagLrf, 0u);

		for (int j = 0; j < f.k; j++)
			readCache32(kProbeLine + static_cast<u32>(j) * kStride);

		SCOPED_TRACE(::testing::Message() << "k=" << f.k << " line=" << f.line);
		EXPECT_EQ(ReadTag(kProbeLine) & kFlagLrf, f.tag_way0 & kFlagLrf)
			<< "way 0's LRF disagrees with the console's instruction cache";
		EXPECT_EQ(ReadTag(kProbeLine + 1) & kFlagLrf, f.tag_way1 & kFlagLrf)
			<< "way 1's LRF disagrees with the console's instruction cache";
		compared++;
	}
	EXPECT_GT(compared, 0) << "no console row starts from a reset LRF state";
}

// ---------------------------------------------------------------------------
// IXIN, and the model that answers nothing.

TEST(EeCache3Console, CacheOp07IsIxinOnConsoleAndANoOpInPcsx2)
{
	// Console: way-selected by bit 0, clears V, keeps the address.
	EXPECT_EQ(Obs(30, "way0_before") & kFlagValid, kFlagValid);
	EXPECT_EQ(Obs(30, "way0_after_ixin_way0") & kFlagValid, 0u);
	EXPECT_EQ(Obs(30, "way0_after_ixin_way0") & 0xFFFFF000u,
	          Obs(30, "way0_before") & 0xFFFFF000u);
	// The other way is untouched until it is addressed.
	EXPECT_EQ(Obs(30, "way1_after_ixin_way0"), Obs(30, "way1_before"));
	EXPECT_EQ(Obs(30, "way1_after_ixin_way1") & kFlagValid, 0u);

	// PCSX2 has no instruction cache, so 0x07 does nothing at all -- including
	// to TagLo, which is how the test can tell.
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x07, kProbeLine);
	EXPECT_EQ(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u)
		<< "PCSX2 now answers IXIN; record what it does";
}

// ---------------------------------------------------------------------------
// Tripwires.

TEST(EeCache3Console, DISABLED_InstructionCacheReplacementIsModelled)
{
	// Turns green when an I-cache exists and IXLTG reports a set's two ways.
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x00, kProbeLine);
	const u32 way0 = cpuRegs.CP0.n.TagLo;
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x00, kProbeLine + 1);
	EXPECT_TRUE((way0 | cpuRegs.CP0.n.TagLo) != 0)
		<< "no instruction cache to ask about replacement";
}

TEST(EeCache3Console, DISABLED_IxinInvalidatesAnInstructionLine)
{
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x07, kProbeLine);
	EXPECT_NE(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u);
}

TEST(EeCache3Console, DISABLED_DumpConsoleRound3)
{
	for (int i = 0; i < kEeCache3CaseCount; i++)
	{
		const EeCache3Case& c = kEeCache3Cases[i];
		printf("\n== case %d %s\n   p=%08x m=%08x\n", c.id, c.name, c.p, c.m);
		for (int k = 0; k < c.n_obs; k++)
			printf("   %-34s %08x%s\n", c.obs[k].name, c.obs[k].raw,
			       c.obs[k].is_tag ? "  (tag)" : "");
	}
	printf("\n== instruction-cache ladder, set %u, baits", kIcacheSet);
	for (int i = 0; i < kBaitCount; i++)
		printf(" %08x", kBaits[i]);
	printf("\n");
	for (int i = 0; i < kIcacheLadderCount; i++)
	{
		const EeCache3Fill& f = kIcacheLadder[i];
		printf("   k=%d line %d: way0 %08x way1 %08x   unique init LRF (%d,%d)\n",
		       f.k, f.line, f.tag_way0, f.tag_way1, f.init_lrf0, f.init_lrf1);
	}
}
