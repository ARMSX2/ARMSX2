// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the texture hashes that leave the emulator: the TEX0Hash and CLUTHash of a key made for dumping, replacing or
// upscaling (HashCacheKey::Create with raw_blocks false). Texture replacement packs and dumped textures are named by
// them (GSTextureReplacements::CreateTextureName), so a change of one bit renames every texture of every pack users
// have, and the packs stop matching. The values below were taken from gs-runs e93a11f457 (master 0aecc97c89 plus the
// burst memo) on known bytes; they must never change.
//
// Every other hash the texture cache makes (the block hashes, the key of a lookup that is not dumped, replaced or
// upscaled, the memo of hashes, the hash map's own hash) lives only inside one run and is free to change.
//
// The bytes: local memory filled from splitmix64 with a fixed seed, a palette of 256 entries from the same generator.
// The windows: every texture format, whole textures, windows with a region (unswizzled before hashing), windows
// smaller than a block, a TEXA that the 16 and 24-bit formats read, and a chain of three mip levels.

#include "gs_write_stamp_drivers.h"

#include "GS/GSXXH.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <cinttypes>
#include <cstdio>
#include <set>
#include <string>

using namespace GSWriteStamps;

namespace
{
	using HashCacheKey = GSTextureCache::HashCacheKey;
	using SourceRegion = GSTextureCache::SourceRegion;

	u64 SplitMix64(u64& state)
	{
		u64 z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}

	struct Case
	{
		const char* name;
		u32 psm;
		u32 tw, th; ///< log2
		int rx0, rw, ry0, rh; ///< region; rw 0 for none in x, rh 0 for none in y
		bool texa; ///< a TEXA with TA0, TA1 and AEM set
		bool mips; ///< three levels through MIPTBP1
		u64 tex0hash; ///< pinned
		u64 cluthash; ///< pinned, 0 for a format without a palette
	};

	// clang-format off
	const Case kCases[] = {
		{"ct32 64x64",           PSMCT32,  6, 6, 0, 0, 0, 0,   false, false, 0x09cf04d939ba035f, 0x0000000000000000},
		{"ct32 region",          PSMCT32,  7, 6, 8, 40, 4, 24, false, false, 0xea14cd6cee687767, 0x0000000000000000},
		{"ct32 4x4",             PSMCT32,  2, 2, 0, 0, 0, 0,   false, false, 0x47b03d88b9717bd5, 0x0000000000000000},
		{"ct32 mips",            PSMCT32,  6, 6, 0, 0, 0, 0,   false, true,  0x9b6cb5807d8b70e7, 0x0000000000000000},
		{"ct24 64x64",           PSMCT24,  6, 6, 0, 0, 0, 0,   false, false, 0x6ade6d17fd18ed41, 0x0000000000000000},
		{"ct24 64x64 texa",      PSMCT24,  6, 6, 0, 0, 0, 0,   true,  false, 0x9ea4360cdce2c269, 0x0000000000000000},
		{"ct16 64x64",           PSMCT16,  6, 6, 0, 0, 0, 0,   false, false, 0x319c467e139e16c4, 0x0000000000000000},
		{"ct16 64x64 texa",      PSMCT16,  6, 6, 0, 0, 0, 0,   true,  false, 0x1b611792682a6e5f, 0x0000000000000000},
		{"ct16s region",         PSMCT16S, 7, 7, 16, 64, 0, 0, true,  false, 0xc38423c2d2adf63e, 0x0000000000000000},
		{"t8 128x128",           PSMT8,    7, 7, 0, 0, 0, 0,   false, false, 0xbea7c9b7244b7c2b, 0x03bfd8c198809fbc},
		{"t8 region",            PSMT8,    7, 7, 4, 100, 3, 50, false, false, 0x0bc4baa4ef8f86f4, 0x03bfd8c198809fbc},
		{"t8 8x8",               PSMT8,    3, 3, 0, 0, 0, 0,   false, false, 0xef4bc25674db65e2, 0x03bfd8c198809fbc},
		{"t8 mips",              PSMT8,    7, 7, 0, 0, 0, 0,   false, true,  0x2d784029ce39c61b, 0x03bfd8c198809fbc},
		{"t4 128x128",           PSMT4,    7, 7, 0, 0, 0, 0,   false, false, 0x4b65d883145b86fd, 0x1ddb3caae93e4855},
		{"t4 region",            PSMT4,    7, 7, 0, 0, 20, 60, false, false, 0x21bfb39e6dfa2708, 0x1ddb3caae93e4855},
		{"t8h 64x64",            PSMT8H,   6, 6, 0, 0, 0, 0,   false, false, 0x4ad4e723db49d5d0, 0x03bfd8c198809fbc},
		{"t4hl 64x64",           PSMT4HL,  6, 6, 0, 0, 0, 0,   false, false, 0xd244ec7ce848a20d, 0x1ddb3caae93e4855},
		{"t4hh 64x64",           PSMT4HH,  6, 6, 0, 0, 0, 0,   false, false, 0xf61aa4fd364f40f3, 0x1ddb3caae93e4855},
		{"z32 64x64",            PSMZ32,   6, 6, 0, 0, 0, 0,   false, false, 0xed7c064ee459a3c7, 0x0000000000000000},
		{"z24 64x64",            PSMZ24,   6, 6, 0, 0, 0, 0,   false, false, 0x56a183ecc94e6cab, 0x0000000000000000},
		{"z16 64x64",            PSMZ16,   6, 6, 0, 0, 0, 0,   false, false, 0x18f9f0659b5b52ff, 0x0000000000000000},
		{"z16s 64x64",           PSMZ16S,  6, 6, 0, 0, 0, 0,   false, false, 0xbbd635ca383e2c73, 0x0000000000000000},
	};
	// clang-format on

	constexpr u32 kTBP0 = 0x1000; // block 4096, page 128
	constexpr u32 kTBW = 2;
	constexpr u32 kMipTBP1 = 0x2000, kMipTBP2 = 0x2800, kMipTBP3 = 0x2C00;

	class GSTextureNamePin : public SwPrimFixture
	{
	protected:
		void FillKnown()
		{
			u64 state = 0x5EED0F7E57ABCDEFULL;
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 8)
			{
				const u64 v = SplitMix64(state);
				std::memcpy(m_gs->m_mem.m_vm8 + i, &v, 8);
			}
			m_gs->m_mem.MarkAllPagesWritten();

			for (u32 i = 0; i < 256; i++)
				m_clut[i] = static_cast<u32>(SplitMix64(state));
		}

		void SetMips(const Case& c)
		{
			Packet p;
			GIFReg r = {};
			r.U64 = 0;
			r.TEX0.TBP0 = kTBP0;
			r.TEX0.TBW = kTBW;
			r.TEX0.PSM = c.psm;
			r.TEX0.TW = c.tw;
			r.TEX0.TH = c.th;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			r.MIPTBP1.TBP1 = kMipTBP1;
			r.MIPTBP1.TBW1 = kTBW;
			r.MIPTBP1.TBP2 = kMipTBP2;
			r.MIPTBP1.TBW2 = kTBW;
			r.MIPTBP1.TBP3 = kMipTBP3;
			r.MIPTBP1.TBW3 = kTBW;
			p.Reg(GIF_A_D_REG_MIPTBP1_1, r);
			p.Send(*m_gs, GIFRegPRIM{});
		}

		HashCacheKey Key(const Case& c)
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = kTBP0;
			tex0.TBW = kTBW;
			tex0.PSM = c.psm;
			tex0.TW = c.tw;
			tex0.TH = c.th;

			GIFRegTEXA texa = {};
			if (c.texa)
			{
				texa.TA0 = 0x33;
				texa.AEM = 1;
				texa.TA1 = 0xC4;
			}

			SourceRegion region = {};
			if (c.rw)
				region.SetX(c.rx0, c.rx0 + c.rw);
			if (c.rh)
				region.SetY(c.ry0, c.ry0 + c.rh);

			const GSVector2i lod(0, 2);
			const bool paletted = GSLocalMemory::m_psm[c.psm].pal > 0;
			return HashCacheKey::Create(tex0, texa, paletted ? m_clut : nullptr, c.mips ? &lod : nullptr, region, false);
		}

		alignas(16) u32 m_clut[256] = {};
	};

	TEST_F(GSTextureNamePin, DumpAndReplacementHashesAreUnchanged)
	{
		BringUp();
		FillKnown();

		std::string table;
		bool all_match = true;
		for (const Case& c : kCases)
		{
			if (c.mips)
				SetMips(c);
			const HashCacheKey key = Key(c);
			const bool match = key.TEX0Hash == c.tex0hash && key.CLUTHash == c.cluthash;
			all_match &= match;
			EXPECT_TRUE(match) << c.name << ": TEX0Hash " << std::hex << key.TEX0Hash << " (pinned " << c.tex0hash << "), CLUTHash "
							   << key.CLUTHash << " (pinned " << c.cluthash << ")";

			char line[160];
			std::snprintf(line, sizeof(line), "%s: 0x%016" PRIx64 ", 0x%016" PRIx64 "\n", c.name, key.TEX0Hash, key.CLUTHash);
			table += line;

			// The same key again: a second lookup may be answered by the memo of hashes, which must give the same name.
			const HashCacheKey again = Key(c);
			EXPECT_TRUE(again == key) << c.name << ": a second lookup of unchanged bytes gave another key";
		}
		if (!all_match)
			std::fprintf(stderr, "The keys this build makes:\n%s", table.c_str());

		// The pinned values are all different, so a hash that stopped reading its bytes would not pass.
		std::set<u64> distinct;
		for (const Case& c : kCases)
			distinct.insert(c.tex0hash);
		EXPECT_EQ(distinct.size(), std::size(kCases));
	}
	TEST_F(GSTextureNamePin, ThePaletteHashFollowsEveryRewriteOfTheRenderersPalette)
	{
		// A key made with the renderer's own expanded palette takes the palette's hash from a memo while the
		// palette's read serial has not moved. Every rewrite must show, and the hash is always XXH3 of the entries.
		BringUp();
		FillKnown();
		GSClut& clut = m_gs->m_mem.m_clut;
		GIFRegTEX0 tex0 = {};
		tex0.TBP0 = kTBP0;
		tex0.TBW = kTBW;
		tex0.PSM = PSMT8;
		tex0.TW = 7;
		tex0.TH = 7;
		const GSVector2i lod(0, 0);
		(void)lod;

		u64 state = 99;
		u64 last = 0;
		for (int round = 0; round < 8; round++)
		{
			// A new palette in local memory at CBP 0x3000, loaded and expanded the way a draw does it.
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
					m_gs->m_mem.WritePixel32(x, y, static_cast<u32>(SplitMix64(state)), 0x3000, 1);
			GIFRegTEX0 load = tex0;
			load.CBP = 0x3000;
			load.CPSM = PSMCT32;
			load.CLD = 1;
			const GIFRegTEXCLUT tc = {};
			clut.WriteTest(load, tc);
			clut.WriteDecision(load, tc);
			clut.WriteLoad(load, tc);
			clut.Read32(load, GIFRegTEXA{});

			const u32* entries = static_cast<const u32*>(clut);
			for (const bool raw : {false, true})
			{
				const HashCacheKey key = HashCacheKey::Create(tex0, GIFRegTEXA{}, entries, nullptr, SourceRegion{}, raw);
				EXPECT_EQ(key.CLUTHash, XXH3_64bits(entries, 256 * sizeof(u32))) << "round " << round << " raw " << raw;
				EXPECT_NE(key.CLUTHash, last) << "round " << round;
			}
			last = HashCacheKey::Create(tex0, GIFRegTEXA{}, entries, nullptr, SourceRegion{}, false).CLUTHash;
		}
	}
} // namespace
