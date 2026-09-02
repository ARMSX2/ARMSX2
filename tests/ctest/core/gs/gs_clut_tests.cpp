// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the CLUT (GSClut), from the gs-clut hardware
// capture (SCPH-30001, 2026-08-11, umbrella/hardware-oracle/captures/gs-clut).
//
// Two measured rules, and one measured non-rule kept as a guard:
//
//   - For an eight-bit index into a 32-bit palette, CSA is ORed into the CLUT
//     group an entry reads: group = (entry >> 4) | CSA. The shipped
//     add-and-clamp (min(group + CSA, 15)) agrees at CSA 0 and 15 and is wrong
//     everywhere between -- 1,536/1,536 readings for the OR against 1,312 for
//     the clamp, 1,504 of them separating. The capture's decoupled rows load
//     at one CSA and sample at another with CLD=0, which is what these tests
//     reproduce.
//   - CPSM decodes two bits: bit 1 selects the entry width (set means 16-bit)
//     and bit 3 selects the S block layout. Silicon loads the four
//     undocumented values 0x03/0x08/0x09/0x0B accordingly; our dispatch sent
//     them to WriteCLUT_NULL and the read expansion skipped them entirely.
//     (Measured one case per storage mode; CSM1 scope.)
//   - For a FOUR-bit index the CSA roles cancel (the load slot and the read
//     slot are the same), so the memory read does not move. Our model was
//     already right (512/512) -- pinned here so the OR fix never leaks into
//     the four-bit path.

#include "GS/GSClut.h"
#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <array>

namespace
{
// Distinct, nonzero seed per texel so a palette entry sourced from the wrong
// word always reads as a different value, and unwritten memory (zero) never
// masquerades as a valid entry.
u32 Seed32(int x, int y)
{
	return 0x40000000u | ((u32)(y & 0xff) << 16) | ((u32)(x & 0xff) << 4) | 1u;
}

u16 Seed16(int x, int y)
{
	return (u16)(0x8000u | ((y & 0x1f) << 8) | ((x & 0x1f) << 2) | 1u);
}

GIFRegTEX0 ClutTEX0(u32 cbp, u32 cpsm, u32 csa, u32 cld, u32 psm)
{
	GIFRegTEX0 t = {};
	t.CBP = cbp;
	t.CPSM = cpsm;
	t.CSM = 0;
	t.CSA = csa;
	t.CLD = cld;
	t.PSM = psm;
	return t;
}

class GSClutTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_mem = new GSLocalMemory();
		m_clut = new GSClut(m_mem);
		m_texa.U64 = 0;
		m_texa.TA0 = 0x33;
		m_texa.TA1 = 0xcc;
	}

	void TearDown() override
	{
		delete m_clut;
		delete m_mem;
	}

	// Seed a 32x32 texel rect of the palette format at cbp, covering every
	// block a CSM1 load of any index width reads.
	void SeedPalette(u32 psm, u32 cbp)
	{
		const GSLocalMemory::psm_t& fmt = GSLocalMemory::m_psm[psm];
		for (int y = 0; y < 32; y++)
		{
			for (int x = 0; x < 32; x++)
			{
				const u32 v = (fmt.trbpp == 16) ? Seed16(x, y) : Seed32(x, y);
				(m_mem->*fmt.wp)(x, y, v, cbp, 1);
			}
		}
	}

	// The GSState sequence for a CSM1 load: decision, then the palette load
	// from local memory, then the read-side expansion.
	void Load(const GIFRegTEX0& t)
	{
		const GIFRegTEXCLUT tc = {};
		EXPECT_TRUE(m_clut->WriteTest(t, tc));
		m_clut->WriteDecision(t, tc);
		m_clut->WriteLoad(t, tc);
		m_clut->Read32(t, m_texa);
	}

	std::array<u32, 256> Snapshot() const
	{
		std::array<u32, 256> out;
		for (int i = 0; i < 256; i++)
			out[i] = (*m_clut)[i];
		return out;
	}

	// Reset, load under cpsm, and return the expanded palette -- so a refused
	// load leaves provable zeros rather than a previous case's contents.
	std::array<u32, 256> LoadAndExpand(u32 cpsm, u32 psm, u32 cbp)
	{
		m_clut->Reset();
		Load(ClutTEX0(cbp, cpsm, 0, 1, psm));
		return Snapshot();
	}

	GSLocalMemory* m_mem = nullptr;
	GSClut* m_clut = nullptr;
	GIFRegTEXA m_texa = {};
};

// gs-clut result 2: sampling an eight-bit index through a 32-bit palette at
// CSA k reads group (entry >> 4) | k of the loaded buffer. The capture's
// decoupled rows: load everything at CSA 0, then re-expand at another CSA
// with CLD=0 so nothing reloads.
TEST_F(GSClutTest, CSAOrsIntoTheGroupForT32I8)
{
	SeedPalette(PSMCT32, 0);
	Load(ClutTEX0(0, PSMCT32, 0, 1, PSMT8));
	const std::array<u32, 256> base = Snapshot();

	for (const u32 csa : {1u, 2u, 3u, 5u, 7u, 10u, 14u, 15u})
	{
		m_clut->Read32(ClutTEX0(0, PSMCT32, csa, 0, PSMT8), m_texa);
		for (int e = 0; e < 256; e++)
		{
			const int group = ((e >> 4) | csa) & 15;
			ASSERT_EQ((*m_clut)[e], base[(group << 4) | (e & 15)])
				<< "entry " << e << " at CSA " << csa;
		}
	}
}

// gs-clut result 2, four-bit half: CSA selects both the slot the load writes
// and the slot the sampler reads, so it cancels and the expanded palette is
// the same at every CSA. Already right before the OR fix; pinned so the OR
// never leaks into this path.
TEST_F(GSClutTest, CSACancelsForT32I4)
{
	SeedPalette(PSMCT32, 0);
	Load(ClutTEX0(0, PSMCT32, 0, 1, PSMT4));
	const std::array<u32, 256> base = Snapshot();

	for (const u32 csa : {1u, 5u, 15u})
	{
		m_clut->Reset();
		Load(ClutTEX0(0, PSMCT32, csa, 1, PSMT4));
		for (int e = 0; e < 16; e++)
			ASSERT_EQ((*m_clut)[e], base[e]) << "entry " << e << " at CSA " << csa;
	}
}

// gs-clut result 6: CPSM decodes bit 1 (entry width) and bit 3 (S layout);
// the undecoded bits do nothing. So 0x03 loads as PSMCT16, 0x0B as PSMCT16S,
// and 0x08/0x09 as PSMCT32 -- where we refused all four.
TEST_F(GSClutTest, UndocumentedCPSMValuesLoad16Bit)
{
	SeedPalette(PSMCT16, 0);
	const std::array<u32, 256> documented = LoadAndExpand(PSMCT16, PSMT8, 0);
	EXPECT_EQ(LoadAndExpand(0x03, PSMT8, 0), documented);

	SeedPalette(PSMCT16S, 32);
	const std::array<u32, 256> documented_s = LoadAndExpand(PSMCT16S, PSMT8, 32);
	EXPECT_EQ(LoadAndExpand(0x0B, PSMT8, 32), documented_s);
}

TEST_F(GSClutTest, UndocumentedCPSMValuesLoad32Bit)
{
	SeedPalette(PSMCT32, 0);
	const std::array<u32, 256> documented = LoadAndExpand(PSMCT32, PSMT8, 0);
	EXPECT_EQ(LoadAndExpand(0x08, PSMT8, 0), documented);
	EXPECT_EQ(LoadAndExpand(0x09, PSMT8, 0), documented);
}

// The CSM2 dispatch decodes the same two bits (capture cases with CSM=1 and
// the undocumented values loading).
TEST_F(GSClutTest, UndocumentedCPSMValuesLoadCSM2)
{
	GIFRegTEXCLUT tc;
	tc.U64 = 0;
	tc.CBW = 4;

	// A 256-entry CSM2 strip at row 0 of a 256-pixel-wide region.
	const GSLocalMemory::psm_t& fmt32 = GSLocalMemory::m_psm[PSMCT32];
	for (int x = 0; x < 256; x++)
		(m_mem->*fmt32.wp)(x, 0, Seed32(x & 31, x >> 5), 0, 4);

	const auto load_csm2 = [&](u32 cpsm) {
		m_clut->Reset();
		GIFRegTEX0 t = ClutTEX0(0, cpsm, 0, 1, PSMT8);
		t.CSM = 1;
		EXPECT_TRUE(m_clut->WriteTest(t, tc));
		m_clut->WriteDecision(t, tc);
		m_clut->WriteLoad(t, tc);
		m_clut->Read32(t, m_texa);
		return Snapshot();
	};

	const std::array<u32, 256> documented = load_csm2(PSMCT32);
	EXPECT_EQ(load_csm2(0x08), documented);
	EXPECT_EQ(load_csm2(0x09), documented);

	const GSLocalMemory::psm_t& fmt16 = GSLocalMemory::m_psm[PSMCT16];
	for (int x = 0; x < 256; x++)
		(m_mem->*fmt16.wp)(x, 0, Seed16(x & 31, x >> 5), 0, 4);

	const std::array<u32, 256> documented16 = load_csm2(PSMCT16);
	EXPECT_EQ(load_csm2(0x03), documented16);
}

// The four-bit dispatch row decodes the same two bits.
TEST_F(GSClutTest, UndocumentedCPSMValuesLoadI4)
{
	SeedPalette(PSMCT16, 0);
	m_clut->Reset();
	Load(ClutTEX0(0, PSMCT16, 0, 1, PSMT4));
	std::array<u32, 16> documented;
	for (int i = 0; i < 16; i++)
		documented[i] = (*m_clut)[i];

	m_clut->Reset();
	Load(ClutTEX0(0, 0x03, 0, 1, PSMT4));
	for (int i = 0; i < 16; i++)
		ASSERT_EQ((*m_clut)[i], documented[i]) << "entry " << i;
}
} // namespace
