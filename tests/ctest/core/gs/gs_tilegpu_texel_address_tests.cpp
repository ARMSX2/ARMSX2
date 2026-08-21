// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu backend samples guest textures out of raw bytes: its fragment shader
// (bin/resources/shaders/vulkan/tilegpu.glsl) turns a texel coordinate into a guest
// address with the GS's own page/block/column swizzle, evaluated from the closed forms
// GSTileSwizzleForms fits at runtime. Nothing on that road can be checked by looking at
// the picture -- a wrong address produces plausible texture from the wrong place, which
// is exactly what a block-granular defect looks like on screen.
//
// So the shader's arithmetic is transcribed here, once per format it serves, and pinned
// against GSLocalMemory's own PixelAddress functions -- the addresses the software
// renderer and every CPU reader use. A disagreement here is a disagreement with the
// bytes the rest of the emulator believes in.
//
// The coordinate ranges deliberately cross page boundaries in both axes, and the layouts
// include a base that sits blocks into a page and the buffer widths the corpus actually
// uses (TBW 1 through 8, and the halved pages-per-row a paletted format addresses by).
//
// The alpha-byte views (PSMT8H/PSMT4HL/PSMT4HH) get a second layer on top of the address
// check, because their whole difference from PSMCT32 is WHICH BITS of the word the index
// comes out of -- and an address comparison is blind to that by construction. Those are
// pinned against the CPU readers themselves.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace GSTileSwizzleForms;

namespace
{
	// -- the shader, transcribed -------------------------------------------------------
	// One function per tilegpu.glsl texel reader. Each returns the guest address in the
	// unit the shader indexes with: words for the 32-bit road, bytes for PSMT8, nibbles
	// for PSMT4. `tbw` is what the renderer puts in the state row -- pages per texture
	// row, which for a paletted format is TBW >> 1 (GSOffset's bw >> (pageShiftX - 6)).

	u32 ShaderTexel32Word(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 5) * tbw + (u >> 6);
		const u32 blk = tbp0 + page * 32 + f.block48.Eval((u >> 3) & 7, (v >> 3) & 3);
		return blk * 64 + f.col32.Eval(u & 7, v & 7);
	}

	u32 ShaderIndex8Byte(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 6) * tbw + (u >> 7);
		const u32 blk = tbp0 + page * 32 + f.block48.Eval((u >> 4) & 7, (v >> 4) & 3);
		return blk * 256 + f.col8.Eval(u & 15, v & 15);
	}

	u32 ShaderIndex4Nibble(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 7) * tbw + (u >> 7);
		const u32 blk = tbp0 + page * 32 + f.block84.Eval((u >> 5) & 3, (v >> 4) & 7);
		return blk * 512 + f.col4.Eval(u & 31, v & 15);
	}

	// tilegpu_index_hi, transcribed: the alpha-byte views hold their index in the TOP BITS of an
	// ordinary CT32 word, so the address is the direct road's (ShaderTexel32Word) and only the
	// extraction differs. `fmt` is the state row's index_format -- 3 = PSMT8H, 4 = PSMT4HL,
	// 5 = PSMT4HH.
	u32 ShaderIndexHi(u32 word, u32 fmt)
	{
		if (fmt == 3)
			return (word >> 24) & 0xFF;
		if (fmt == 4)
			return (word >> 24) & 0xF;
		return (word >> 28) & 0xF;
	}

	// The state row's index_format for a PSM, as the renderer derives it: 0 for a direct 32-bit
	// texel, else the shader format number plus one.
	u32 StateRowIndexFormat(u32 psm)
	{
		const int f = IndexFormatFor(psm);
		return (f < 0) ? 0u : static_cast<u32>(f) + 1u;
	}

	class TileGpuTexelAddressTest : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			s_mem = new GSLocalMemory();
			// Structured rather than random in the top byte: a wrong shift shows up as a shifted
			// image rather than as noise. Every value of the alpha byte occurs, and both nibbles of
			// it vary independently, which is what tells 4HL and 4HH apart.
			u32* const vm = s_mem->vm32();
			u64 x = 0x9E3779B97F4A7C15ull;
			for (u32 i = 0; i < 4 * 1024 * 1024 / 4; i++)
			{
				x ^= x << 13;
				x ^= x >> 7;
				x ^= x << 17;
				vm[i] = static_cast<u32>(x);
			}
		}

		static void TearDownTestSuite()
		{
			delete s_mem;
			s_mem = nullptr;
		}

		static GSLocalMemory* s_mem;
	};

	GSLocalMemory* TileGpuTexelAddressTest::s_mem = nullptr;
} // namespace

TEST(GSTileGpuTexelAddress, Direct32MatchesPixelAddress32)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			for (u32 v = 0; v < 72; v++)
			{
				for (u32 u = 0; u < static_cast<u32>(tbw) * 64 + 8; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress32(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderTexel32Word(f, u, v, tbp0, tbw), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

TEST(GSTileGpuTexelAddress, Paletted8MatchesPixelAddress8)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	// TBW is the GS register; the state row carries pages per row, TBW >> 1. TBW=1 gives
	// zero pages per row, which is GSOffset's arithmetic and not a clamp -- a 128-wide
	// 8-bit texture never leaves its first page column, so the term never contributes.
	for (const u32 tbw : {1u, 2u, 4u, 6u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			const u32 pages_per_row = tbw >> 1;
			const u32 width = std::max<u32>(pages_per_row, 1) * 128 + 16;
			for (u32 v = 0; v < 136; v++)
			{
				for (u32 u = 0; u < width; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress8(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderIndex8Byte(f, u, v, tbp0, pages_per_row), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

TEST(GSTileGpuTexelAddress, Paletted4MatchesPixelAddress4)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 6u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			const u32 pages_per_row = tbw >> 1;
			const u32 width = std::max<u32>(pages_per_row, 1) * 128 + 16;
			for (u32 v = 0; v < 264; v++)
			{
				for (u32 u = 0; u < width; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress4(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderIndex4Nibble(f, u, v, tbp0, pages_per_row), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

// -- the alpha-byte views: PSMT8H, PSMT4HL, PSMT4HH ---------------------------------------------
//
// These three are palette indices stored in the TOP BITS of ordinary 32-bit words. Everything about
// their addressing is PSMCT32's -- 64x32 pages, 8x8 blocks, 8x8-texel columns, TBW pages per row --
// and only the extraction out of the word differs. That similarity is the trap: they carry a palette
// like PSMT8/PSMT4 do, so a road that decides page geometry by asking "is it paletted" gives them
// TBW >> 1 and addresses the wrong page in every row but the first.

// The page term first, on its own, because it is the clause the shape of the formats invites getting
// wrong and the only one whose failure is invisible at TBW <= 1.
TEST(GSTileGpuTexelAddress, AlphaViewsTakeTheCt32PageTerm)
{
	for (const u32 tbw : {0u, 1u, 2u, 4u, 6u, 8u})
	{
		const u32 direct = PagesPerRow(PSMCT32, tbw);
		EXPECT_EQ(direct, tbw) << "tbw=" << tbw; // GSOffset's own value, unhalved and unfloored
		EXPECT_EQ(PagesPerRow(PSMT8H, tbw), direct) << "tbw=" << tbw;
		EXPECT_EQ(PagesPerRow(PSMT4HL, tbw), direct) << "tbw=" << tbw;
		EXPECT_EQ(PagesPerRow(PSMT4HH, tbw), direct) << "tbw=" << tbw;
		// ...and the two genuinely 128-wide formats still halve, so the helper is not simply
		// returning TBW for everything.
		EXPECT_EQ(PagesPerRow(PSMT8, tbw), tbw >> 1) << "tbw=" << tbw;
		EXPECT_EQ(PagesPerRow(PSMT4, tbw), tbw >> 1) << "tbw=" << tbw;
	}
}

// The address itself against PixelAddress32, which is what every CPU reader of these three formats
// calls (ReadPixel8H/4HL/4HH all take a PixelAddress32).
TEST(GSTileGpuTexelAddress, AlphaViewAddressesMatchPixelAddress32)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			const u32 pages_per_row = PagesPerRow(PSMT8H, tbw);
			for (u32 v = 0; v < 72; v++)
			{
				for (u32 u = 0; u < tbw * 64 + 8; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress32(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderTexel32Word(f, u, v, tbp0, pages_per_row), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

// The BIT POSITIONS, against the CPU readers themselves. An address check cannot see these: read the
// low byte instead of the high one and every address still lands where it should.
TEST_F(TileGpuTexelAddressTest, AlphaViewIndicesMatchCpuPixelReaders)
{
	const u32* const vm = s_mem->vm32();

	// Every address the sweep below touches, over a base blocks into a page.
	for (const u32 addr : {0u, 1u, 7u, 63u, 64u, 255u, 2048u, 65535u, 1048575u})
	{
		const u32 w = vm[addr];
		EXPECT_EQ(ShaderIndexHi(w, 3), s_mem->ReadPixel8H(addr)) << "addr=" << addr;
		EXPECT_EQ(ShaderIndexHi(w, 4), s_mem->ReadPixel4HL(addr)) << "addr=" << addr;
		EXPECT_EQ(ShaderIndexHi(w, 5), s_mem->ReadPixel4HH(addr)) << "addr=" << addr;
	}
	// And exhaustively over the word, which is what actually pins 4HL to bits 24-27 and 4HH to
	// 28-31 rather than to each other's.
	for (u32 b = 0; b < 256; b++)
	{
		const u32 w = 0x00ABCDEFu | (b << 24);
		EXPECT_EQ(ShaderIndexHi(w, 3), b);
		EXPECT_EQ(ShaderIndexHi(w, 4), b & 0xF);
		EXPECT_EQ(ShaderIndexHi(w, 5), b >> 4);
	}
}

// The whole window, texel by texel, against the CPU palette-LESS deswizzler (psm.rtxP) -- the same
// reader the source cache's other build road calls. This is address and extraction together, which
// is what the fragment stage actually performs.
TEST_F(TileGpuTexelAddressTest, AlphaViewWindowsMatchCpuIndexReader)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	const u32* const vm = s_mem->vm32();

	struct Window
	{
		u32 tbp0;
		u32 tbw; // the TEX0 register value
		int tw;
		int th;
	};
	// Bases block-into-a-page and page-aligned; widths spanning several page columns (which is the
	// only geometry where a wrong page term shows) and heights spanning several page rows.
	static constexpr Window kWindows[] = {
		{0, 1, 64, 64},
		{32, 2, 128, 96},
		{37, 2, 128, 64},
		{0x1400, 4, 256, 128},
		{96, 8, 512, 64},
	};

	std::vector<u8> cpu;
	GIFRegTEXA TEXA = {}; // an index reader does not consult it
	for (const u32 psm : {static_cast<u32>(PSMT8H), static_cast<u32>(PSMT4HL), static_cast<u32>(PSMT4HH)})
	{
		const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
		const u32 fmt = StateRowIndexFormat(psm);
		for (const Window& w : kWindows)
		{
			const u32 pages_per_row = PagesPerRow(psm, w.tbw);
			const u32 pitch = static_cast<u32>(w.tw);
			cpu.assign(pitch * static_cast<u32>(w.th), 0);

			const GSOffset off = s_mem->GetOffset(w.tbp0, w.tbw, psm);
			p.rtxP(*s_mem, off, GSVector4i(0, 0, w.tw, w.th), cpu.data(), static_cast<int>(pitch), TEXA);

			for (int v = 0; v < w.th; v++)
			{
				const u8* const row = cpu.data() + static_cast<u32>(v) * pitch;
				for (int u = 0; u < w.tw; u++)
				{
					const u32 addr =
						ShaderTexel32Word(f, static_cast<u32>(u), static_cast<u32>(v), w.tbp0, pages_per_row);
					ASSERT_EQ(ShaderIndexHi(vm[addr], fmt), static_cast<u32>(row[u]))
						<< "psm=" << psm << " tbp0=" << w.tbp0 << " tbw=" << w.tbw << " u=" << u << " v=" << v;
				}
			}
		}
	}
}

// The state row's index_format is the shader format number plus one, and the fragment stage's branch
// chain is written against those exact values. Nothing links the two files, so the mapping is pinned
// here: a renumbering of IndexFormat that did not move the GLSL would sample PSMT4HL's nibble out of
// a PSMT8H texture, which renders -- just not what was asked for.
TEST(GSTileGpuTexelAddress, StateRowIndexFormatMatchesTheShadersBranchOrder)
{
	EXPECT_EQ(StateRowIndexFormat(PSMCT32), 0u);
	EXPECT_EQ(StateRowIndexFormat(PSMCT24), 0u);
	EXPECT_EQ(StateRowIndexFormat(PSMT8), 1u);
	EXPECT_EQ(StateRowIndexFormat(PSMT4), 2u);
	EXPECT_EQ(StateRowIndexFormat(PSMT8H), 3u);
	EXPECT_EQ(StateRowIndexFormat(PSMT4HL), 4u);
	EXPECT_EQ(StateRowIndexFormat(PSMT4HH), 5u);
}

// The palette the shader has to be handed for each format. The renderer sizes a draw's CLUT
// expansion off psm.pal, so a format landing on the wrong count either copies 240 words of the next
// draw's palette into this one or reads sixteen entries where the indices go to 255.
TEST(GSTileGpuTexelAddress, AlphaViewPaletteSizes)
{
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT8H].pal, 256u);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HL].pal, 16u);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HH].pal, 16u);
	// The same sizes their non-H twins take, which is what lets one palette road serve both.
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT8H].pal, GSLocalMemory::m_psm[PSMT8].pal);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HL].pal, GSLocalMemory::m_psm[PSMT4].pal);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HH].pal, GSLocalMemory::m_psm[PSMT4].pal);

	// And the page geometry that justifies reusing the direct-32 address: identical pages, blocks
	// and columns to PSMCT32, unlike PSMT8/PSMT4.
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT8H].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HL].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT4HH].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	EXPECT_EQ(GSLocalMemory::m_psm[PSMT8H].bs, GSLocalMemory::m_psm[PSMCT32].bs);
	EXPECT_NE(GSLocalMemory::m_psm[PSMT8].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	EXPECT_NE(GSLocalMemory::m_psm[PSMT4].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
}
