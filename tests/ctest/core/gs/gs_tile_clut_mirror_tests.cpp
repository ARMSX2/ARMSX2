// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's palette-on-the-GPU machinery, the parts that are pure:
//
//   - GSTileClutMirror, the provenance record that says whether the CPU's CLUT RAM or
//     a device palette holds each sixteen-entry slot, and what a draw reading a set of
//     slots should bind;
//   - GSClut::EntryToWordCSM1_32, the word order the CSM1 32-bit loaders read a
//     palette in, derived by running the loaders — pinned here as a bijection and as
//     the map a real load-then-read round trip through GSClut itself produces;
//   - the fit of that map as GF(2)-linear forms, which is how the device-side gather
//     addresses the source words (a fit that fails would turn the route off).

#include "GS/GSClut.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileClutMirror.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include <gtest/gtest.h>

#include <memory>

using Verdict = GSTileClutMirror::Verdict;

TEST(GSTileClutMirror, StartsCpuEverywhere)
{
	GSTileClutMirror m;
	EXPECT_FALSE(m.AnyGpu());
	EXPECT_EQ(m.Resolve(0, 16).verdict, Verdict::Cpu);
	EXPECT_EQ(m.Resolve(7, 1).verdict, Verdict::Cpu);
	EXPECT_TRUE(m.CpuCurrent(0, 16));
}

TEST(GSTileClutMirror, AnEightBitDeviceLoadServesTheWholePalette)
{
	GSTileClutMirror m;
	m.OnGpuLoad(0, 16, 7);
	const auto r = m.Resolve(0, 16);
	EXPECT_EQ(r.verdict, Verdict::Gpu);
	EXPECT_EQ(r.handle, 7u);
	EXPECT_EQ(r.first_texel, 0u);
	// A four-bit draw at CSA 5 reads slot 5 of the same palette, eighty texels in.
	const auto r5 = m.Resolve(5, 1);
	EXPECT_EQ(r5.verdict, Verdict::Gpu);
	EXPECT_EQ(r5.handle, 7u);
	EXPECT_EQ(r5.first_texel, 80u);
	EXPECT_FALSE(m.CpuCurrent(0, 16));
	EXPECT_TRUE(m.AnyUnsynced());
	EXPECT_TRUE(m.References(7));
	EXPECT_FALSE(m.References(8));
}

TEST(GSTileClutMirror, ALaterFourBitLoadSplitsTheProvenance)
{
	GSTileClutMirror m;
	m.OnGpuLoad(0, 16, 7);
	m.OnGpuLoad(5, 1, 9); // a device four-bit load into slot 5
	EXPECT_EQ(m.Resolve(0, 16).verdict, Verdict::Mixed); // two loads
	EXPECT_EQ(m.Resolve(5, 1).handle, 9u);
	EXPECT_EQ(m.Resolve(5, 1).first_texel, 0u);
	EXPECT_EQ(m.Resolve(4, 1).handle, 7u);
	EXPECT_EQ(m.Resolve(4, 1).first_texel, 64u);
	m.OnCpuLoad(3, 1); // a CPU four-bit load into slot 3
	EXPECT_EQ(m.Resolve(3, 1).verdict, Verdict::Cpu);
	EXPECT_EQ(m.Resolve(2, 2).verdict, Verdict::Mixed); // device beside CPU
	EXPECT_TRUE(m.References(7));
	EXPECT_TRUE(m.References(9));
	m.OnGpuLoad(0, 16, 11); // a fresh full load retires both
	EXPECT_FALSE(m.References(7));
	EXPECT_FALSE(m.References(9));
	EXPECT_EQ(m.Resolve(0, 16).handle, 11u);
}

TEST(GSTileClutMirror, SyncingMakesTheCpuCurrentWithoutChangingTheVerdict)
{
	GSTileClutMirror m;
	m.OnGpuLoad(0, 16, 7);
	u32 visited = 0;
	m.ForEachUnsyncedSlot([&](u32 slot, u32 handle, u32 texel) {
		EXPECT_EQ(handle, 7u);
		EXPECT_EQ(texel, slot * 16);
		visited++;
	});
	EXPECT_EQ(visited, 16u);
	m.MarkSynced(7);
	EXPECT_FALSE(m.AnyUnsynced());
	EXPECT_TRUE(m.CpuCurrent(0, 16));
	// The device palette is still the answer for a native draw — both are right now.
	EXPECT_EQ(m.Resolve(0, 16).verdict, Verdict::Gpu);
	// A new device load into one slot un-syncs that slot alone.
	m.OnGpuLoad(2, 1, 8);
	EXPECT_TRUE(m.AnyUnsynced());
	EXPECT_TRUE(m.CpuCurrent(0, 2));
	EXPECT_FALSE(m.CpuCurrent(2, 1));
	visited = 0;
	m.ForEachUnsyncedSlot([&](u32 slot, u32 handle, u32 texel) {
		EXPECT_EQ(slot, 2u);
		EXPECT_EQ(handle, 8u);
		EXPECT_EQ(texel, 0u);
		visited++;
	});
	EXPECT_EQ(visited, 1u);
}

namespace
{
u32 Seed(u32 w)
{
	return 0x40000000u | (w << 8) | (w ^ 0x5Au);
}
} // namespace

TEST(GSClutEntryToWord, IsABijectionAndMatchesARealLoadThroughGSClut)
{
	u16 i8[256];
	u16 i4[16];
	GSClut::EntryToWordCSM1_32(i8, i4);

	// Bijections onto their word ranges.
	bool seen8[256] = {};
	for (u32 e = 0; e < 256; e++)
	{
		ASSERT_LT(i8[e], 256);
		EXPECT_FALSE(seen8[i8[e]]) << "word " << i8[e] << " twice";
		seen8[i8[e]] = true;
	}
	bool seen4[16] = {};
	for (u32 e = 0; e < 16; e++)
	{
		ASSERT_LT(i4[e], 16);
		EXPECT_FALSE(seen4[i4[e]]);
		seen4[i4[e]] = true;
	}

	// The map through GSClut itself: write a seeded palette into local memory at a
	// block-aligned CBP, load it the way the GS does, read it back the way a draw does,
	// and each entry must equal the word EntryToWord names.
	auto mem = std::make_unique<GSLocalMemory>();
	const u32 cbp = 0x3e00; // GT4's palette page, as it happens
	for (u32 w = 0; w < 256; w++)
		reinterpret_cast<u32*>(mem->m_vm8)[cbp * 64 + w] = Seed(w); // 64 words per block, blocks are contiguous
	GIFRegTEX0 t = {};
	t.CBP = cbp;
	t.CPSM = PSMCT32;
	t.CSM = 0;
	t.CSA = 0;
	t.CLD = 1;
	t.PSM = PSMT8;
	GIFRegTEXCLUT tc = {};
	GIFRegTEXA ta = {};
	mem->m_clut.WriteLoad(t, tc);
	mem->m_clut.Read32(t, ta);
	const u32* pal = mem->m_clut;
	for (u32 e = 0; e < 256; e++)
		EXPECT_EQ(pal[e], Seed(i8[e])) << "entry " << e;

	// The four-bit map, at a non-zero CSA so the slot offset is exercised.
	t.PSM = PSMT4;
	t.CSA = 5;
	mem->m_clut.WriteLoad(t, tc);
	mem->m_clut.Read32(t, ta);
	pal = mem->m_clut;
	for (u32 e = 0; e < 16; e++)
		EXPECT_EQ(pal[e], Seed(i4[e])) << "entry " << e;
}

TEST(GSClutEntryToWord, FitsGF2LinearForms)
{
	u16 i8[256];
	u16 i4[16];
	GSClut::EntryToWordCSM1_32(i8, i4);
	GSTileSwizzleForms::XorForm1 f8, f4;
	ASSERT_TRUE(GSTileSwizzleForms::Fit1([&](u32 e) { return static_cast<u32>(i8[e]); }, 8, f8));
	ASSERT_TRUE(GSTileSwizzleForms::Fit1([&](u32 e) { return static_cast<u32>(i4[e]); }, 4, f4));
	for (u32 e = 0; e < 256; e++)
		EXPECT_EQ(f8.Eval(e), i8[e]);
	for (u32 e = 0; e < 16; e++)
		EXPECT_EQ(f4.Eval(e), i4[e]);
}

TEST(GSClutSetEntries32, RoundTripsThroughRead32)
{
	auto mem = std::make_unique<GSLocalMemory>();
	u32 words[256];
	for (u32 e = 0; e < 256; e++)
		words[e] = Seed(e) ^ 0x00FF00FFu;
	mem->m_clut.SetEntries32(0, 256, words);
	GIFRegTEX0 t = {};
	t.CPSM = PSMCT32;
	t.PSM = PSMT8;
	GIFRegTEXA ta = {};
	mem->m_clut.Read32(t, ta);
	const u32* pal = mem->m_clut;
	for (u32 e = 0; e < 256; e++)
		EXPECT_EQ(pal[e], words[e]) << "entry " << e;
	// A four-bit read at CSA 3 sees slot 3.
	t.PSM = PSMT4;
	t.CSA = 3;
	mem->m_clut.Read32(t, ta);
	pal = mem->m_clut;
	for (u32 e = 0; e < 16; e++)
		EXPECT_EQ(pal[e], words[48 + e]) << "entry " << e;
}
