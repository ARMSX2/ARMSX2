// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu CLUT gather: a palette the game RENDERED and then loaded. Its words are in a target's
// texture and nowhere on the CPU, so the load stops being a readback (671 and 1272 a frame on the
// two GT4 dumps, which is also all of their mid-frame plan fragmentation) and its words reach the
// consumers on the device instead.
//
// Four things hold that road up, and every one of them fails silently:
//
//  1. THE ENTRY ORDER. The byte-road consumer gets its words by an image-to-buffer COPY of the
//     palette's blocks -- not a gather pass, because at a thousand loads a frame that is a thousand
//     render passes -- and a copy lands texels ROW-MAJOR, which is not the order the CSM1 loaders
//     read them in. So the shader re-applies that order at fetch, off
//     GSTileSwizzleForms::ClutEntryToCopyOffset. Get it wrong and every gathered palette is a
//     permutation of itself: plausible colours, wrong ones. Pinned here against a real GSClut load
//     of a self-naming palette, through GSOffset's own address function on the owner side.
//  2. THE COPY GEOMETRY. LocateClutBlocks says which rects of the owner hold the palette. Its
//     regions must cover exactly the words the load reads -- no more (the reserved run would
//     overflow) and no fewer (entries would read whatever the reservation left).
//  3. THE IDENTITY. A gathered palette's id is a hash of the LOAD, not of any words, and it shares
//     an id space with GSTilePaletteCache's word hashes. The expanded cache fuses
//     (index build id x palette id) pairs, so the two spaces meeting would fuse unrelated pairs.
//  4. THE PRUNE POINT. Tile recycles a palette the mirror no longer names, at the load. Under
//     deferred consumption that is unsound, and this file makes the hazard a fact rather than an
//     argument: the mirror stops naming a palette the moment the NEXT load lands, which is long
//     before the draw that named it has been issued.

#include "GS/GSClut.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileClutMirror.h"
#include "GS/Renderers/Tile/GSTileExpandedCache.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

using namespace GSTileSwizzleForms;

namespace
{
	// One owner surface a palette was rendered into, and the palette's base inside it.
	struct GatherCase
	{
		u32 cbp; ///< TEX0.CBP, the load's base in blocks
		u32 owner_bp; ///< the owner's page-aligned base
		u32 owner_bw; ///< the owner's FBW (pages per row for CT32)
		u32 entries; ///< 256 (an eight-bit index) or 16 (a four-bit one)
	};

	// Distinct and nonzero per word, so a palette entry sourced from the wrong word always reads as
	// a different value and unwritten memory (zero) can never masquerade as a valid entry.
	u32 SelfNaming(u32 word)
	{
		return 0x40000000u | (word << 4) | 1u;
	}

	// The word address of owner texel (x, y), by GSOffset -- the CPU readers' own address function,
	// which is what the copy's source rects have to agree with.
	u32 OwnerWord(const GatherCase& c, u32 x, u32 y)
	{
		const GSOffset off = GSOffset::fromKnownPSM(c.owner_bp, c.owner_bw, PSMCT32);
		return static_cast<u32>(off.pa(static_cast<int>(x), static_cast<int>(y)));
	}

	// The palette image the executor's copy produces: each region of LocateClutBlocks copied
	// row-major, one after the other, out of an owner whose every texel holds its own word address.
	// The merged arm needs nothing extra here -- it is one region of w x h texels copied by the same
	// vkCmdCopyImageToBuffer with bufferRowLength = w, which is what this loop already is.
	std::vector<u32> CopyPaletteBlocks(
		const FormSet& f, const GatherCase& c, bool merge = false, ClutBlockCopy* out_blocks = nullptr)
	{
		ClutBlockCopy blocks;
		EXPECT_TRUE(LocateClutBlocks(f, c.cbp, c.owner_bp, c.owner_bw, c.entries, merge, blocks));
		if (out_blocks)
			*out_blocks = blocks;
		std::vector<u32> out;
		for (u32 b = 0; b < blocks.region_count; b++)
		{
			for (u32 y = 0; y < blocks.h; y++)
			{
				for (u32 x = 0; x < blocks.w; x++)
					out.push_back(SelfNaming(OwnerWord(c, blocks.x[b] + x, blocks.y[b] + y)));
			}
		}
		return out;
	}

	class ClutGatherTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_mem = new GSLocalMemory();
			m_clut = new GSClut(m_mem);
			m_texa.U64 = 0;
		}
		void TearDown() override
		{
			delete m_clut;
			delete m_mem;
		}

		// Fill the owner's pages with a self-naming CT32 image: texel (x, y) holds its own word
		// address, so an entry that came from the wrong word is always visibly wrong.
		void SeedOwner(const GatherCase& c, u32 w, u32 h)
		{
			for (u32 y = 0; y < h; y++)
			{
				for (u32 x = 0; x < w; x++)
					m_mem->WritePixel32(static_cast<int>(x), static_cast<int>(y),
						SelfNaming(OwnerWord(c, x, y)), c.owner_bp, c.owner_bw);
			}
		}

		// What GSClut's own CSM1 32-bit loader makes of that palette: entry e's 32-bit value.
		std::array<u32, 256> RealLoad(const GatherCase& c)
		{
			GIFRegTEX0 t = {};
			t.CBP = c.cbp;
			t.CPSM = PSMCT32;
			t.CSM = 0;
			t.CSA = 0;
			t.CLD = 1;
			t.PSM = (c.entries == 256) ? PSMT8 : PSMT4;
			const GIFRegTEXCLUT tc = {};
			m_clut->Reset();
			m_clut->WriteDecision(t, tc);
			m_clut->WriteLoad(t, tc);
			m_clut->Read32(t, m_texa);
			std::array<u32, 256> out{};
			for (u32 e = 0; e < c.entries; e++)
				out[e] = (*m_clut)[e];
			return out;
		}

		GSLocalMemory* m_mem = nullptr;
		GSClut* m_clut = nullptr;
		GIFRegTEXA m_texa = {};
	};
} // namespace

// -- 1. the entry order ------------------------------------------------------------------------

// The whole road, end to end on the CPU: a self-naming palette rendered into an owner, the blocks
// LocateClutBlocks names copied out of it row-major, and every entry fetched back through
// ClutEntryToCopyOffset -- against what GSClut's real loader and Read32 produce from the same bytes.
// A 32-bit palette entry is RGBA verbatim under Read32, which is exactly why the gather is
// restricted to that shape, so the two sides are comparable word for word.
TEST_F(ClutGatherTest, EveryEntryComesBackThroughTheCopiedBlocks)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);

	const GatherCase cases[] = {
		// The plain case: an eight-bit palette at the owner's own base.
		{0x0000, 0x0000, 1, 256},
		// ...and four blocks in, which is where the four blocks stop being the page's first square.
		{0x0004, 0x0000, 1, 256},
		// A base that is NOT four-block aligned: the four blocks are then not a 16x16 square at all,
		// which is the case the per-block copy exists for.
		{0x0001, 0x0000, 1, 256},
		{0x0006, 0x0000, 1, 256},
		{0x001d, 0x0000, 1, 256}, // ...and one whose blocks cross into the next page
		// Pages into a wider owner (GT4's shape: palettes rendered into a strip of the frame).
		{0x0020, 0x0000, 4, 256},
		{0x0043, 0x0000, 4, 256},
		{0x0080 + 7, 0x0080, 2, 256},
		// Four-bit palettes: sixteen words out of one block, at every alignment.
		{0x0000, 0x0000, 1, 16},
		{0x0003, 0x0000, 1, 16},
		{0x001f, 0x0000, 1, 16},
		{0x0025, 0x0020, 2, 16},
	};

	for (const GatherCase& c : cases)
	{
		SCOPED_TRACE(testing::Message() << "cbp " << c.cbp << " owner_bp " << c.owner_bp << " bw " << c.owner_bw
										<< " entries " << c.entries);
		// Big enough to hold every page the palette's blocks can land in.
		SeedOwner(c, c.owner_bw * 64, 128);
		const std::array<u32, 256> want = RealLoad(c);
		const std::vector<u32> copied = CopyPaletteBlocks(f, c);
		ASSERT_EQ(copied.size(), c.entries);

		for (u32 e = 0; e < c.entries; e++)
		{
			const u32 off = ClutEntryToCopyOffset(f, c.entries, e);
			ASSERT_LT(off, copied.size()) << "entry " << e;
			EXPECT_EQ(copied[off], want[e]) << "entry " << e;
		}
	}
}

// The mapping has to be a bijection onto the copied image: two entries sharing an offset would make
// one of them silently wrong, and an offset outside the run would read the next palette's words.
TEST(TileGpuClutGather, TheEntryOrderIsABijectionOntoTheCopiedRun)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	for (const u32 entries : {256u, 16u})
	{
		std::vector<bool> hit(entries, false);
		for (u32 e = 0; e < entries; e++)
		{
			const u32 off = ClutEntryToCopyOffset(f, entries, e);
			ASSERT_LT(off, entries) << "entries " << entries << " entry " << e;
			EXPECT_FALSE(hit[off]) << "entries " << entries << " entry " << e << " reuses offset " << off;
			hit[off] = true;
		}
	}
}

// -- 1b. the entry order of a MERGED copy ------------------------------------------------------

// The same round trip for TileGpuClutMergeRegions: the four blocks copied as ONE 16x16 region, and
// every entry fetched back through ClutEntryToMergedOffset. This is the whole byte-truth of that
// lever -- the offset formula is the only thing between a correct palette and silent wrong colour,
// and it has exactly one other reader (tilegpu.glsl's merged palette mode, transcribed from here).
//
// The unaligned cases are in the list on purpose: asked to merge, they refuse, and the test then
// checks the fallback the same way the renderer and the shader pick it -- off `merged`, not off the
// lever.
TEST_F(ClutGatherTest, EveryEntryComesBackThroughTheMergedSquare)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);

	const GatherCase cases[] = {
		// Four-block aligned, so these merge.
		{0x0000, 0x0000, 1, 256},
		{0x0004, 0x0000, 1, 256},
		{0x0008, 0x0000, 1, 256},
		{0x001c, 0x0000, 1, 256}, // the page's last square
		{0x0020, 0x0000, 4, 256}, // a page into a wider owner (GT4's shape)
		{0x0040, 0x0000, 4, 256},
		{0x0080 + 4, 0x0080, 2, 256},
		// NOT aligned: the four blocks are not a square, the merge refuses, and the per-block order
		// is what comes back.
		{0x0001, 0x0000, 1, 256},
		{0x0006, 0x0000, 1, 256},
		{0x001d, 0x0000, 1, 256},
		{0x0080 + 7, 0x0080, 2, 256},
		// Sixteen-entry palettes never merge -- one region already, and a 8x2 rect is not a square.
		{0x0000, 0x0000, 1, 16},
		{0x0003, 0x0000, 1, 16},
		{0x0025, 0x0020, 2, 16},
	};

	for (const GatherCase& c : cases)
	{
		SCOPED_TRACE(testing::Message() << "cbp " << c.cbp << " owner_bp " << c.owner_bp << " bw " << c.owner_bw
										<< " entries " << c.entries);
		SeedOwner(c, c.owner_bw * 64, 128);
		const std::array<u32, 256> want = RealLoad(c);
		ClutBlockCopy blocks{};
		const std::vector<u32> copied = CopyPaletteBlocks(f, c, true, &blocks);
		ASSERT_EQ(copied.size(), c.entries);
		EXPECT_EQ(blocks.merged, c.entries == 256 && (c.cbp & 3) == 0);

		for (u32 e = 0; e < c.entries; e++)
		{
			const u32 off = blocks.merged ? ClutEntryToMergedOffset(f, c.entries, blocks.stride, e) :
											ClutEntryToCopyOffset(f, c.entries, e);
			ASSERT_LT(off, copied.size()) << "entry " << e;
			EXPECT_EQ(copied[off], want[e]) << "entry " << e;
		}
	}
}

// The merged mapping has to be a bijection too, and onto the same 256 words: an offset outside the
// square would read the next reservation, and two entries sharing one would make one of them
// silently wrong. Checked at both strides the road uses -- 16 for a palette copied as its own
// square, 64 for one read out of a whole copied owner page, where the 256 words are the eight rows
// of sixteen the square occupies inside the page.
TEST(TileGpuClutGather, TheMergedEntryOrderIsABijectionOntoTheSquare)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	for (const u32 stride : {16u, 64u})
	{
		std::vector<bool> hit(16u * stride, false);
		for (u32 e = 0; e < 256u; e++)
		{
			const u32 off = ClutEntryToMergedOffset(f, 256, stride, e);
			// Sixteen rows of sixteen columns, wherever the stride puts the rows.
			ASSERT_LT(off % stride, 16u) << "stride " << stride << " entry " << e;
			ASSERT_LT(off / stride, 16u) << "stride " << stride << " entry " << e;
			EXPECT_FALSE(hit[off]) << "stride " << stride << " entry " << e << " reuses offset " << off;
			hit[off] = true;
		}
	}
}

// The merged shape, and the refusal. A 4-aligned 256-entry palette is one 16x16 region at a
// 16-aligned origin; anything else keeps the four 8x8 blocks the road already ships, whatever the
// lever says.
TEST(TileGpuClutGather, OnlyAFourAlignedPaletteMergesIntoOneSquare)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ClutBlockCopy b;

	for (const u32 cbp : {0x0000u, 0x0004u, 0x0020u, 0x0044u})
	{
		SCOPED_TRACE(testing::Message() << "cbp " << cbp);
		ASSERT_TRUE(LocateClutBlocks(f, cbp, 0x0000, 4, 256, true, b));
		EXPECT_TRUE(b.merged);
		EXPECT_EQ(b.region_count, 1u);
		EXPECT_EQ(b.w, 16u);
		EXPECT_EQ(b.h, 16u);
		EXPECT_EQ(b.stride, 16u);
		EXPECT_EQ(b.region_count * b.w * b.h, 256u); // still exactly the palette, no more and no less
		// 16-aligned in both axes, which is what makes the square a square.
		EXPECT_EQ(b.x[0] % 16, 0u);
		EXPECT_EQ(b.y[0] % 16, 0u);
	}

	// The refusal road: these three are the cases the per-block copy exists for, and the lever must
	// not touch them. 0x001d's blocks even cross into the next page.
	for (const u32 cbp : {0x0001u, 0x0006u, 0x001du})
	{
		SCOPED_TRACE(testing::Message() << "cbp " << cbp);
		ASSERT_TRUE(LocateClutBlocks(f, cbp, 0x0000, 1, 256, true, b));
		EXPECT_FALSE(b.merged);
		EXPECT_EQ(b.region_count, 4u);
		EXPECT_EQ(b.w, 8u);
		EXPECT_EQ(b.h, 8u);
		EXPECT_EQ(b.stride, 0u);

		// ...and the merged arm asked with the lever off is that same road, byte for byte.
		ClutBlockCopy off;
		ASSERT_TRUE(LocateClutBlocks(f, cbp, 0x0000, 1, 256, false, off));
		EXPECT_EQ(std::memcmp(&off, &b, sizeof(off)), 0);
	}

	// A sixteen-entry palette is already one region and its 8x2 rect is not a square: it never
	// merges, at any alignment.
	for (const u32 cbp : {0x0000u, 0x0004u, 0x0007u})
	{
		ASSERT_TRUE(LocateClutBlocks(f, cbp, 0x0000, 1, 16, true, b)) << "cbp " << cbp;
		EXPECT_FALSE(b.merged) << "cbp " << cbp;
		EXPECT_EQ(b.stride, 0u) << "cbp " << cbp;
		EXPECT_EQ(b.w, 8u) << "cbp " << cbp;
		EXPECT_EQ(b.h, 2u) << "cbp " << cbp;
	}
}

// The PAGE merge (TileGpuClutMergePages), which is the same map with a wider tile: a burst of
// mergeable palettes sharing one owner page is copied as ONE 64x32 region, and each of them reads
// its words out of that region at its square's page-relative origin, stride 64.
//
// Two things have to hold and both fail silently. The eight squares of a full page must land at the
// eight distinct intra-page origins {0,16,32,48} x {0,16} -- two palettes claiming one origin would
// read each other's words -- and each palette's entries must come back through
// ClutEntryToMergedOffset at stride 64 with that origin added, which is what the renderer computes
// as page_base + y_in_page * 64 + x_in_page.
TEST_F(ClutGatherTest, EightPalettesTileOnePageAndReadBackOutOfIt)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);

	// One CT32 page of an owner four pages wide, holding eight 4-aligned 256-entry palettes: a page
	// is 32 blocks, and a 4-aligned group is four of them, so CBP 0x20, 0x24 ... 0x3c tile page 1.
	// (GT4's shape, one page over: a bank of palettes rendered into a strip of the frame buffer.)
	const u32 owner_bp = 0x0000, owner_bw = 4;
	SeedOwner({0x0020, owner_bp, owner_bw, 256}, owner_bw * 64, 128);

	// The page copy itself: 64x32 texels out of the owner, row-major, exactly as the executor emits
	// it for one region of copy_w 64 by copy_h 32.
	std::vector<u32> page;
	u32 page_px = 0, page_py = 0;
	{
		ClutBlockCopy first;
		ASSERT_TRUE(LocateClutBlocks(f, 0x0020, owner_bp, owner_bw, 256, true, first));
		page_px = first.x[0] & ~63u;
		page_py = first.y[0] & ~31u;
		for (u32 y = 0; y < 32; y++)
		{
			for (u32 x = 0; x < 64; x++)
				page.push_back(SelfNaming(OwnerWord({0x0020, owner_bp, owner_bw, 256}, page_px + x, page_py + y)));
		}
	}
	ASSERT_EQ(page.size(), 64u * 32u);

	std::vector<bool> origin_seen(64u * 32u, false);
	for (u32 k = 0; k < 8; k++)
	{
		const GatherCase c{0x0020 + k * 4, owner_bp, owner_bw, 256};
		SCOPED_TRACE(testing::Message() << "cbp " << c.cbp);
		ClutBlockCopy b;
		ASSERT_TRUE(LocateClutBlocks(f, c.cbp, c.owner_bp, c.owner_bw, c.entries, true, b));
		ASSERT_TRUE(b.merged);
		// Every square of the group is in the same page as the first, which is what makes the group
		// a group -- the renderer keys on exactly this.
		ASSERT_EQ(b.x[0] & ~63u, page_px);
		ASSERT_EQ(b.y[0] & ~31u, page_py);

		// The renderer's own arithmetic for this palette's place in the page's reservation.
		const u32 stream_offset = (b.y[0] - page_py) * 64 + (b.x[0] - page_px);
		ASSERT_LT(stream_offset, page.size());
		EXPECT_FALSE(origin_seen[stream_offset]) << "two palettes claim intra-page offset " << stream_offset;
		origin_seen[stream_offset] = true;

		const std::array<u32, 256> want = RealLoad(c);
		for (u32 e = 0; e < 256; e++)
		{
			const u32 off = stream_offset + ClutEntryToMergedOffset(f, 256, 64, e);
			ASSERT_LT(off, page.size()) << "entry " << e;
			EXPECT_EQ(page[off], want[e]) << "entry " << e;
		}
	}

	// The eight origins are the page's eight square slots and nothing else: four across, two down.
	u32 seen = 0;
	for (u32 y = 0; y < 32; y += 16)
	{
		for (u32 x = 0; x < 64; x += 16)
			seen += origin_seen[y * 64 + x] ? 1u : 0u;
	}
	EXPECT_EQ(seen, 8u);
}

// The renderer hands the fragment stage the tile stride pre-chewed into two words, and the shader
// folds the multiply into bit positions rather than doing it. That is a transcription with no
// compiler between the two sides, so it is pinned here against the CPU spec at both strides the road
// uses. A disagreement is a palette that is a permutation of itself: plausible colours, wrong ones.
TEST(TileGpuClutGather, TheShadersFoldedOffsetIsTheSpecAtBothStrides)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	for (const u32 stride : {16u, 64u})
	{
		// GSRendererTileGpu's map from the copy's stride to the state row's two fields.
		const u32 pal_mul = (stride == 64) ? 7u : 1u;
		const u32 pal_shift = (stride == 64) ? 2u : 0u;
		for (u32 e = 0; e < 256u; e++)
		{
			// tilegpu.glsl's merged arm, verbatim.
			const u32 w = f.clut_i8_word.Eval(e);
			const u32 c = f.inv_col32.Eval(w & 63);
			const u32 shader = ((w & 0x80u) << pal_shift) | (c + pal_mul * (c & 0x38u)) | ((w & 0x40u) >> 3u);
			EXPECT_EQ(shader, ClutEntryToMergedOffset(f, 256, stride, e)) << "stride " << stride << " entry " << e;
		}
	}
}

// The page merge's safety condition, which is the part of it that can silently corrupt.
//
// A run of n merged squares reserved n * 256 words. The region that replaces it is 64 wide by 16h
// tall and writes 1024h words from the run's base. Those agree only at n == 4h -- every slot of h
// whole rows of the page's 4x2 square grid, none of them twice -- so a run that covers a band only
// partly must NOT merge: the region would write past the end of the run into whatever reserved next,
// and the palettes there would come back as somebody else's texels.
//
// Not a threshold anyone chose. The arithmetic forces it, which is why the answer is a predicate with
// a test and not a tunable.
TEST(TileGpuClutGather, OnlyAWholeBandOfAPageMergesIntoOneRegion)
{
	u32 y = 0xdead, h = 0xbeef;

	// The two shapes that tile: one row of four, and both rows.
	{
		const u32 x[4] = {0, 16, 32, 48}, sy[4] = {0, 0, 0, 0};
		ASSERT_TRUE(gsTileGpuClutPageBand(x, sy, 4, y, h));
		EXPECT_EQ(y, 0u);
		EXPECT_EQ(h, 16u);
		EXPECT_EQ(4u * 256u, 64u * h); // the region writes exactly what the run reserved
	}
	{
		const u32 x[4] = {48, 0, 32, 16}, sy[4] = {16, 16, 16, 16}; // order is the capture's, not the page's
		ASSERT_TRUE(gsTileGpuClutPageBand(x, sy, 4, y, h));
		EXPECT_EQ(y, 16u);
		EXPECT_EQ(h, 16u);
	}
	{
		const u32 x[8] = {0, 16, 32, 48, 0, 16, 32, 48}, sy[8] = {0, 0, 0, 0, 16, 16, 16, 16};
		ASSERT_TRUE(gsTileGpuClutPageBand(x, sy, 8, y, h));
		EXPECT_EQ(y, 0u);
		EXPECT_EQ(h, 32u);
		EXPECT_EQ(8u * 256u, 64u * h);
	}

	// Four squares that are NOT a row: a 64x32 region would cover them and write twice what the run
	// reserved; a 64x16 one would miss two of them.
	{
		const u32 x[4] = {0, 16, 0, 16}, sy[4] = {0, 0, 16, 16};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, 4, y, h));
	}
	// A slot claimed twice -- two palettes reading each other's words -- even though the count is right.
	{
		const u32 x[4] = {0, 16, 16, 48}, sy[4] = {0, 0, 0, 0};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, 4, y, h));
	}
	// Counts that cannot tile a band at all.
	for (const u32 n : {1u, 2u, 3u, 5u, 6u, 7u})
	{
		const u32 x[8] = {0, 16, 32, 48, 0, 16, 32, 48}, sy[8] = {0, 0, 0, 0, 16, 16, 16, 16};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, n, y, h)) << "n " << n;
	}
	// Origins that are not square origins of a page: a 16-alignment or a page bound that stopped
	// holding would otherwise index a slot that does not exist.
	{
		const u32 x[4] = {0, 8, 32, 48}, sy[4] = {0, 0, 0, 0};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, 4, y, h));
	}
	{
		const u32 x[4] = {0, 16, 32, 64}, sy[4] = {0, 0, 0, 0};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, 4, y, h));
	}
	{
		const u32 x[4] = {0, 16, 32, 48}, sy[4] = {0, 0, 0, 32};
		EXPECT_FALSE(gsTileGpuClutPageBand(x, sy, 4, y, h));
	}
}

// -- 2. the copy geometry ----------------------------------------------------------------------

// The regions have to cover exactly the words the load reads. One texel more and the copy runs off
// the end of the reservation into the next palette; one fewer and some entry reads whatever the
// reservation was filled with.
TEST(TileGpuClutGather, TheCopyRegionsCoverExactlyThePalette)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ClutBlockCopy b;

	ASSERT_TRUE(LocateClutBlocks(f, 0x40, 0x40, 1, 256, false, b));
	EXPECT_EQ(b.region_count * b.w * b.h, 256u);
	EXPECT_EQ(b.region_count, 4u); // 256 words = four blocks, contiguous from CBP
	EXPECT_EQ(b.w, 8u);
	EXPECT_EQ(b.h, 8u);

	ASSERT_TRUE(LocateClutBlocks(f, 0x40, 0x40, 1, 16, false, b));
	EXPECT_EQ(b.region_count * b.w * b.h, 16u);
	EXPECT_EQ(b.region_count, 1u); // 16 words = the first two texel rows of one block
	EXPECT_EQ(b.w, 8u);
	EXPECT_EQ(b.h, 2u);

	// A palette below the owner's base is out of the reinterpretation's reach (the shader computes
	// `blk - dst_bp` unsigned), and so is an entry count the CSM1 32-bit loaders never produce.
	EXPECT_FALSE(LocateClutBlocks(f, 0x20, 0x40, 1, 256, false, b));
	EXPECT_FALSE(LocateClutBlocks(f, 0x40, 0x40, 1, 64, false, b));
	EXPECT_FALSE(LocateClutBlocks(f, 0x40, 0x40, 1, 0, false, b));

	// Forms that did not fit turn the road off rather than addressing the wrong bytes.
	FormSet unfit = f;
	unfit.clut_valid = false;
	EXPECT_FALSE(LocateClutBlocks(unfit, 0x40, 0x40, 1, 256, false, b));
}

// The prep op names TWO lists the way a Donor does -- the owner among the plan's targets -- and
// carries a DESTINATION word offset in `bp` rather than a guest base. The kinds also have to stay
// distinct: the executor switches on them, and a duplicated value runs one road's arm for another
// road's op.
TEST(TileGpuClutGather, ThePrepKindsStayDistinctAndTheCopyNamesItsDestination)
{
	using K = GSDevice::GSTileGpuPrepKind;
	const K kinds[] = {K::Writeback, K::Seed, K::Materialise, K::Expand, K::Donor, K::ClutGather, K::ClutBlockCopy};
	for (size_t i = 0; i < std::size(kinds); i++)
	{
		for (size_t j = i + 1; j < std::size(kinds); j++)
			EXPECT_NE(static_cast<u32>(kinds[i]), static_cast<u32>(kinds[j])) << i << " vs " << j;
	}

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = K::ClutBlockCopy;
	op.donor_target = 5; // targets: the owner it copies out of
	op.bp = 1024; // the palette stream word offset it copies INTO -- not a guest address
	op.copy_count = 4;
	op.copy_w = 8;
	op.copy_h = 8;
	// It names no prep texture and no page list: the destination is a buffer range, and this road
	// reads no bytes so it goes through no page table.
	EXPECT_EQ(op.target, 0u);
	EXPECT_EQ(op.page_entry_count, 0u);
	EXPECT_EQ(op.epoch, 0u);
	EXPECT_EQ(op.copy_count * op.copy_w * op.copy_h, 256u);
}

// -- 3. the identity ---------------------------------------------------------------------------

// A gathered palette's id and a word hash share one field, and the reserved bit is the only thing
// keeping them apart -- including in the case that would actually happen, where the two underlying
// values collide.
TEST(TileGpuClutGather, AGatheredPaletteIdCanNeverEqualAWordHash)
{
	const std::array<u32, 16> words{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	const u64 raw = GSTilePaletteCache::ContentId(words.data(), 16);
	const u64 cpu = GSTilePaletteCache::OwnPalId(raw);
	// The colliding case, constructed: the same underlying value on both sides.
	EXPECT_NE(cpu, GSTilePaletteCache::ForeignPalId(raw));
	EXPECT_NE(cpu, GSTilePaletteCache::ForeignPalId(cpu));

	// And in general, over the whole space: the bit partitions it.
	for (const u64 v : {u64(0), u64(1), u64(0x7FFFFFFFFFFFFFFFull), u64(0x8000000000000000ull), ~u64(0), raw})
	{
		EXPECT_EQ(GSTilePaletteCache::OwnPalId(v) & GSTilePaletteCache::kForeignPalIdBit, 0ull);
		EXPECT_NE(GSTilePaletteCache::ForeignPalId(v) & GSTilePaletteCache::kForeignPalIdBit, 0ull);
		EXPECT_NE(GSTilePaletteCache::OwnPalId(v), GSTilePaletteCache::ForeignPalId(v));
	}
	// A foreign id is never zero, which matters because the source road reads zero as "no palette".
	EXPECT_NE(GSTilePaletteCache::ForeignPalId(0), 0ull);
}

// A gathered pair admits on FIRST sight, and it has to: the id carries the owner's version, which
// moves every time the game re-renders the palette, so the pair can literally never recur (measured:
// gt4 631 of 631 eligible loads new, gt4opb 1259 of 1259) and a second-sight wait would refuse the
// road outright. The probe and the build must give the cache the same answer, which is why both
// sites ask the same question.
TEST(TileGpuClutGather, AGatheredPairIsAdmittedOnFirstSight)
{
	GSTileExpandedCache cache;
	const u64 build_id = 7;
	const u64 pal_id = GSTilePaletteCache::ForeignPalId(0x1234'5678'9abc'def0ull);
	EXPECT_EQ(cache.ProbeAdmit(build_id, pal_id, GSTileExpandedCache::AdmitWhen::FirstSight),
		GSTileExpandedCache::Admission::Served);

	// ...where an ordinary pair still waits for the frame boundary it earns its expansion across.
	GSTileExpandedCache other;
	EXPECT_EQ(other.ProbeAdmit(build_id, pal_id, GSTileExpandedCache::AdmitWhen::SecondSight),
		GSTileExpandedCache::Admission::Deferred);
}

// -- 4. the prune point ------------------------------------------------------------------------

// Why the prune moved to the tail of the plan. Tile recycles a palette the mirror no longer names,
// at the load -- and the mirror stops naming one the moment the NEXT load writes the same slots,
// which on the prize titles is a thousand times a frame and always long before the draws that named
// it have been issued. The design probe measured this hazard as zero, but only because a stalling
// load flushed the plan before it stamped: that flush is exactly what the gather deletes, so the
// hazard becomes reachable the day the road lands. Here it is, as a fact about the mirror.
TEST(TileGpuClutGather, TheNextLoadStopsTheMirrorNamingAPaletteADrawStillReads)
{
	GSTileClutMirror mirror;
	// Load A: an eight-bit palette at CSA 0 stamps every slot.
	mirror.OnGpuLoad(0, GSTileClutMirror::kSlots, 11);
	EXPECT_TRUE(mirror.References(11));
	const GSTileClutMirror::Resolution ra = mirror.Resolve(0, GSTileClutMirror::kSlots);
	EXPECT_EQ(ra.verdict, GSTileClutMirror::Verdict::Gpu);
	EXPECT_EQ(ra.handle, 11u);
	// ...a draw is RECORDED on it here, and will not be issued until the plan is built.

	// Load B, still in the same plan window, takes the same slots.
	mirror.OnGpuLoad(0, GSTileClutMirror::kSlots, 12);
	// Tile's predicate now says palette 11 is free, and it is not: the recorded draw still samples
	// its texture. Pruning on this answer -- at the load -- would recycle it under that draw.
	EXPECT_FALSE(mirror.References(11));
	EXPECT_TRUE(mirror.References(12));

	// A four-bit load displaces one slot of a 256-entry palette and leaves the rest, which is why
	// the record is held BY ID and not by slot: slot displacement must not invalidate a reference.
	mirror.OnGpuLoad(5, 1, 13);
	EXPECT_TRUE(mirror.References(12));
	EXPECT_TRUE(mirror.References(13));
	EXPECT_EQ(mirror.Resolve(0, GSTileClutMirror::kSlots).verdict, GSTileClutMirror::Verdict::Mixed);
}

// The two shapes a gathered load can take, resolved the way a draw resolves them. A four-bit draw on
// one slot of a 256-entry gathered load reads that palette's entries 16k..16k+15 -- the bias the
// state row carries -- and that is the one case rule 3 has no copy leg for, so it keeps the byte
// road rather than sampling the wrong sixteen texels.
TEST(TileGpuClutGather, ASubSlotViewOfAGatheredPaletteCarriesItsEntryBias)
{
	GSTileClutMirror mirror;
	mirror.OnGpuLoad(0, GSTileClutMirror::kSlots, 4);
	for (u32 slot = 0; slot < GSTileClutMirror::kSlots; slot++)
	{
		const GSTileClutMirror::Resolution r = mirror.Resolve(slot, 1);
		ASSERT_EQ(r.verdict, GSTileClutMirror::Verdict::Gpu) << slot;
		EXPECT_EQ(r.handle, 4u);
		EXPECT_EQ(r.first_texel, slot * GSTileClutMirror::kEntriesPerSlot) << slot;
	}
	// A sixteen-entry load owns one slot, and a draw on it starts at entry zero of that palette.
	mirror.OnGpuLoad(9, 1, 5);
	const GSTileClutMirror::Resolution r = mirror.Resolve(9, 1);
	EXPECT_EQ(r.handle, 5u);
	EXPECT_EQ(r.first_texel, 0u);
}
