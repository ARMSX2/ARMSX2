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
#include "GS/Renderers/Tile/GSVramModel.h"
#include "GS/Renderers/Tile/GSTileTypes.h"
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

	// -- the SIXTEEN-BIT owner's half of the same three helpers -------------------------------

	// One 16-bit owner surface a palette was rendered into. `psm` is PSMCT16 or PSMCT16S; owner_bw is
	// the FBW, which for a 16-bit surface is pages per row directly (a 16-bit page is 64 wide too).
	struct GatherCase16
	{
		u32 cbp;
		u32 owner_bp;
		u32 owner_bw;
		u32 entries;
		u32 psm;
	};

	// The texel content the owner's TEXTURE holds, as an RGBA8 word. Built by UNPACKING a self-naming
	// 5551 cell, so that packing it back is exactly that cell -- the round trip gsTileUnpack5551 /
	// gsTilePack5551 is pinned as exact in gs_tilegpu_ct16_road_tests, which is what lets a test seed
	// guest memory and a target image from one number.
	u16 SelfNamingCell(u32 halfword_addr)
	{
		// The alpha bit set, and fifteen bits of the address: distinct over any footprint smaller than
		// 32 K halfwords, which every palette's is. The test asserts distinctness anyway.
		return static_cast<u16>(0x8000u | (halfword_addr & 0x7FFFu));
	}

	// The halfword address of owner texel (x, y), by GSOffset -- the same address WritePixel16 uses,
	// so the seed below IS the pull road's store.
	u32 OwnerHalfword(const GatherCase16& c, u32 x, u32 y)
	{
		const GSOffset off = GSOffset::fromKnownPSM(c.owner_bp, c.owner_bw, static_cast<GS_PSM>(c.psm));
		return static_cast<u32>(off.pa(static_cast<int>(x), static_cast<int>(y)));
	}

	// The palette image the executor's copy produces off a 16-bit owner: each region of
	// LocateClutBlocks16 read row-major out of the RGBA8 target image into a `stride`-wide tile at
	// that region's copy_off. That is what vkCmdCopyImageToBuffer does with bufferRowLength = stride
	// and bufferOffset = base + copy_off, which is the whole reason copy_off exists.
	std::vector<u32> CopyPaletteCells(const FormSet& f, const GatherCase16& c, ClutBlockCopy& blocks)
	{
		EXPECT_TRUE(LocateClutBlocks16(f, c.psm, c.cbp, c.owner_bp, c.owner_bw, c.entries, blocks));
		const u32 words = blocks.region_count * blocks.w * blocks.h;
		std::vector<u32> out(words, 0u);
		for (u32 b = 0; b < blocks.region_count; b++)
		{
			for (u32 y = 0; y < blocks.h; y++)
			{
				for (u32 x = 0; x < blocks.w; x++)
				{
					// The target is RGBA8, so a copied word is R | G<<8 | B<<16 | A<<24 -- which is
					// exactly the u32 the image holds.
					const u32 cell = gsTileUnpack5551(SelfNamingCell(OwnerHalfword(c, blocks.x[b] + x, blocks.y[b] + y)));
					out[blocks.copy_off[b] + y * blocks.stride + x] = cell;
				}
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

		// The 16-bit twin of SeedOwner, and it is the PULL ROAD'S OWN STORE: GSTileTargetPool's
		// readback calls WriteFrame16 with the target's RGBA8 pixel, and gsTilePack5551 is that
		// function's arithmetic. So the guest bytes this leaves behind are by definition the bytes a
		// readback of this target would have written -- which is what makes the comparison below byte
		// identity with the pull road rather than an argument about it.
		void SeedOwner16(const GatherCase16& c, u32 w, u32 h)
		{
			for (u32 y = 0; y < h; y++)
			{
				for (u32 x = 0; x < w; x++)
				{
					const u32 rgba = gsTileUnpack5551(SelfNamingCell(OwnerHalfword(c, x, y)));
					// ⚠️ WritePixel16 is PSMCT16's address function alone -- the two 16-bit families
					// have DIFFERENT block tables, and the S one has its own writer. Seeding both
					// through the plain one is a test that measures the CT16 road twice and never
					// touches the 16S one.
					if (c.psm == PSMCT16)
					{
						m_mem->WritePixel16(static_cast<int>(x), static_cast<int>(y), gsTilePack5551(rgba),
							c.owner_bp, c.owner_bw);
					}
					else
					{
						m_mem->WritePixel16S(static_cast<int>(x), static_cast<int>(y), gsTilePack5551(rgba),
							c.owner_bp, c.owner_bw);
					}
				}
			}
		}

		// What GSClut's own CSM1 32-bit loader makes of a palette sitting in those bytes. The load's
		// CPSM is PSMCT32 whatever the owner stores -- that is the whole point: the palette is a byte
		// REINTERPRETATION of a 16-bit surface, never a texel read of one.
		std::array<u32, 256> RealLoad16(const GatherCase16& c)
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

// -- 1a. the block-granular admission ---------------------------------------------------------

// TileGpuClutBlockGather's whole claim, both halves, with the memory model and the copy road asked
// together: a page whose truth the CPU has taken back BLOCK BY BLOCK still holds the palette, and
// the bytes gathered out of it are the loader's own.
//
// The refusal half is the one that matters. A containment written the wrong way round admits a
// palette whose blocks the owner does NOT hold, and the gather then copies whatever those texels
// happen to be -- a whole frame in plausible, wrong colours, with nothing to catch it.
TEST_F(ClutGatherTest, APaletteSurvivesItsPageBeingTakenBackBlockByBlock)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);

	// A 16-entry palette four blocks into a one-page-wide CT32 owner: sixteen words of block 4, and
	// nothing else in that page belongs to it.
	const GatherCase c{0x0004, 0x0000, 1, 16};
	const GSTileSurfaceLayout owner{c.owner_bp, static_cast<u8>(c.owner_bw), PSMCT32, GSTileSurfaceKind::Color};
	const GSVector4i rect(0, 0, 64, 32);
	const GSPageBitmap owner_pages = GSVramModel::PagesForRect(owner, rect);

	// The load's own footprint, as InvalidateLocalMem builds it: one block at CBP, addressed through
	// a block-granular base of its own.
	const GSTileSurfaceLayout load{c.cbp, 1, PSMCT32, GSTileSurfaceKind::Color};
	const GSVector4i one_block(0, 0, 8, 8);
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(load, one_block, fp);
	ASSERT_FALSE(fp.overflowed);
	ASSERT_EQ(fp.pages.count(), 1u);
	const GSVramModel::RectFootprint::Edge* e = fp.FindEdge(0);
	ASSERT_NE(e, nullptr);
	ASSERT_EQ(e->blocks, u32(1) << 4);

	// Everything of page 0 except the palette's block goes back to the CPU. Written as a footprint
	// directly rather than as a rect, because "every block but this one" is not a rectangle.
	GSVramModel::RectFootprint taken;
	taken.pages.set(0);
	taken.edge_count = 1;
	taken.edges[0].page = 0;
	taken.edges[0].blocks = GSVramModel::kFullBlockMask & ~e->blocks;
	taken.edges[0].full = taken.edges[0].blocks;

	{
		GSVramModel m;
		const GSTileSurfaceId id = m.Create(owner, rect, 1);
		m.OnNativeDraw(id, owner_pages, kGSTilePlanesAll);
		m.OnReadback(owner_pages); // synced, so the write below is lossless
		m.OnCpuWrite(taken, kGSTilePlanesAll);
		ASSERT_EQ(m.TruthMask(0, GSVramModel::PlaneIndex(GSTilePlaneRGB)), e->blocks);

		// The two questions, side by side. This is the whole lever.
		EXPECT_EQ(m.SoleGpuOwner(fp.pages, kGSTilePlanesAll), kGSTileNoSurface);
		ASSERT_EQ(m.SoleGpuOwnerOfBlocks(fp, kGSTilePlanesAll), id);
		EXPECT_TRUE(m.CheckInvariants());
	}

	// ...and admitted, the bytes are the loader's. The owner is seeded with a self-naming image and
	// the gather's copy is compared entry for entry against a real GSClut load of the same palette,
	// which is the same oracle the whole-page road is held to.
	SeedOwner(c, c.owner_bw * 64, 128);
	const std::array<u32, 256> want = RealLoad(c);
	const std::vector<u32> copied = CopyPaletteBlocks(f, c);
	ASSERT_EQ(copied.size(), c.entries);
	for (u32 en = 0; en < c.entries; en++)
	{
		const u32 off = ClutEntryToCopyOffset(f, c.entries, en);
		ASSERT_LT(off, copied.size()) << "entry " << en;
		EXPECT_EQ(copied[off], want[en]) << "entry " << en;
	}

	// THE REFUSAL, half one: the CPU took back the palette's OWN block and left the rest. Nothing
	// about the page has changed except which block, and the answer has to flip.
	{
		GSVramModel m;
		const GSTileSurfaceId id = m.Create(owner, rect, 1);
		m.OnNativeDraw(id, owner_pages, kGSTilePlanesAll);
		m.OnReadback(owner_pages);
		GSVramModel::RectFootprint just_the_palette;
		just_the_palette.pages.set(0);
		just_the_palette.edge_count = 1;
		just_the_palette.edges[0].page = 0;
		just_the_palette.edges[0].blocks = e->blocks;
		just_the_palette.edges[0].full = e->blocks;
		m.OnCpuWrite(just_the_palette, kGSTilePlanesAll);
		EXPECT_EQ(m.SoleGpuOwnerOfBlocks(fp, kGSTilePlanesAll), kGSTileNoSurface);
		// ...and the rest of the page is still wholly the owner's, so this is a refusal about the
		// palette's blocks and not about the page having any CPU bytes at all.
		GSVramModel::RectFootprint rest;
		rest.pages.set(0);
		rest.edge_count = 1;
		rest.edges[0].page = 0;
		rest.edges[0].blocks = GSVramModel::kFullBlockMask & ~e->blocks;
		rest.edges[0].full = rest.edges[0].blocks;
		EXPECT_EQ(m.SoleGpuOwnerOfBlocks(rest, kGSTilePlanesAll), id);
		EXPECT_TRUE(m.CheckInvariants());
	}

	// THE REFUSAL, half two: a load spanning two owners. A 256-entry palette is four blocks, and a
	// window whose blocks are two targets' has no single texture whatever the block masks say.
	{
		GSVramModel m;
		const auto a = GSTileSurfaceLayout{0, 1, PSMCT32, GSTileSurfaceKind::Color};
		const auto b = GSTileSurfaceLayout{32, 1, PSMCT32, GSTileSurfaceKind::Color};
		const GSTileSurfaceId ida = m.Create(a, rect, 1);
		const GSTileSurfaceId idb = m.Create(b, rect, 2);
		const GSPageBitmap pa = GSVramModel::PagesForRect(a, rect);
		const GSPageBitmap pb = GSVramModel::PagesForRect(b, rect);
		ASSERT_FALSE(pa.intersects(pb));
		m.OnNativeDraw(ida, pa, kGSTilePlanesAll);
		m.OnNativeDraw(idb, pb, kGSTilePlanesAll);

		// A 256-entry palette at block 30 of page 0: blocks 30, 31 of page 0 and 0, 1 of page 1.
		const GSTileSurfaceLayout across{30, 1, PSMCT32, GSTileSurfaceKind::Color};
		GSVramModel::RectFootprint two;
		two.pages.clear();
		two.edge_count = 0;
		for (u32 blk = 0; blk < 4; blk++)
		{
			GSVramModel::RectFootprint one;
			GSVramModel::FootprintForRect(
				GSTileSurfaceLayout{across.bp + blk, 1, PSMCT32, GSTileSurfaceKind::Color}, one_block, one);
			ASSERT_EQ(one.edge_count, 1u);
			bool merged = false;
			for (u32 i = 0; i < two.edge_count; i++)
			{
				if (two.edges[i].page == one.edges[0].page)
				{
					two.edges[i].blocks |= one.edges[0].blocks;
					two.edges[i].full |= one.edges[0].full;
					merged = true;
				}
			}
			if (!merged)
				two.edges[two.edge_count++] = one.edges[0];
			two.pages |= one.pages;
		}
		ASSERT_EQ(two.pages.count(), 2u);
		EXPECT_EQ(m.SoleGpuOwnerOfBlocks(two, kGSTilePlanesAll), kGSTileNoSurface);
		EXPECT_TRUE(m.CheckInvariants());
	}
}

// The claim the HELD SECOND CALL rests on, and it is a claim about the loaders, not about the model:
// a sixteen-entry CSM1 32-bit palette is sixteen words at the top of the block at CBP and nothing
// else, on both gather roads. GSState invalidates TWO blocks for that shape (its `blocks` count is
// halved once for a 16-bit CPSM and once for a four-bit index, so a 32-bit CPSM with a four-bit
// index lands on two), and the second one is read by nobody -- which is why letting it refuse the
// load refuses the whole population.
TEST(TileGpuClutGather, ASixteenEntryPaletteLivesEntirelyInTheBlockAtCbp)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ASSERT_TRUE(f.clut16_valid);

	for (const u32 cbp : {0x0000u, 0x0001u, 0x0003u, 0x001fu, 0x0025u})
	{
		SCOPED_TRACE(testing::Message() << "cbp " << cbp);
		// A 32-bit owner: one 8x2 region, sixteen words, inside the 8x8 block at CBP.
		ClutBlockCopy b32{};
		ASSERT_TRUE(LocateClutBlocks(f, cbp, 0, 1, 16, false, b32));
		EXPECT_EQ(b32.region_count, 1u);
        EXPECT_EQ(b32.w * b32.h, 16u);
		// The region's texels all address block CBP itself, which is what "the loader reads one
		// block" means in the unit the memory model tracks truth in.
		const GSOffset o32 = GSOffset::fromKnownPSM(0, 1, PSMCT32);
		for (u32 y = 0; y < b32.h; y++)
		{
			for (u32 x = 0; x < b32.w; x++)
			{
				EXPECT_EQ(static_cast<u32>(o32.bn(static_cast<int>(b32.x[0] + x), static_cast<int>(b32.y[0] + y))),
					cbp)
					<< "x " << x << " y " << y;
			}
		}

		// ...and a 16-bit owner: one 16x2 region, thirty-two cells, again inside the block at CBP.
		for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
		{
			ClutBlockCopy b16{};
			ASSERT_TRUE(LocateClutBlocks16(f, psm, cbp, 0, 1, 16, b16));
			EXPECT_EQ(b16.region_count, 1u);
			EXPECT_EQ(b16.w * b16.h, 32u);
			const GSOffset o16 = GSOffset::fromKnownPSM(0, 1, static_cast<GS_PSM>(psm));
			for (u32 y = 0; y < b16.h; y++)
			{
				for (u32 x = 0; x < b16.w; x++)
				{
					EXPECT_EQ(
						static_cast<u32>(o16.bn(static_cast<int>(b16.x[0] + x), static_cast<int>(b16.y[0] + y))), cbp)
						<< "psm " << psm << " x " << x << " y " << y;
				}
			}
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

// -- 1c. the entry order off a SIXTEEN-BIT owner -----------------------------------------------

// The whole 16-bit road, end to end on the CPU, and the ORACLE is the pull road itself.
//
// A palette loaded off a 16-bit target is never "entry = texel": it is 32-bit guest words read out
// of a surface that stores two cells to a word, so a palette word is the pack of two texels eight
// apart in x. Every step below is one of the real roads rather than a restatement of it --
//
//   step 1  the target's texel content, as an RGBA8 image
//   step 2  stored into guest memory with WritePixel16(gsTilePack5551(rgba)), which IS what
//           GSTileTargetPool's readback does (it calls WriteFrame16, whose arithmetic gsTilePack5551
//           is), so the guest bytes are by definition what a pull of this target would have written
//   step 3  the executor's copy simulated: each region of LocateClutBlocks16 read row-major into a
//           tile, honouring stride and copy_off
//   step 4  every entry fetched through ClutEntryToCt16Offset and assembled by gsTileClut16Word
//   step 5  compared against GSClut's real WriteLoad + Read32 over the bytes from step 2
//
// -- so agreement here is byte identity with the pull road, proved rather than argued. The one thing
// it cannot catch is a wrong idea shared by steps 2 and 5, and there is none available: step 5 is a
// stock GSClut load of ordinary guest memory.
//
// PSMCT16 and PSMCT16S both, because they differ only in the block table and a design that "handles
// 16-bit" with one of them mis-addresses every surface of the other silently. CBP & 3 over all four
// values, because the non-4-aligned 256-entry case is what Spider-Man 3 actually loads -- all 48 of
// its 256-entry palettes a frame sit at CBP & 3 == 1 and none at 0 -- so it is the main case here,
// not an edge.
TEST_F(ClutGatherTest, EveryEntryComesBackThroughTheSixteenBitOwner)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);
	ASSERT_TRUE(f.clut16_valid);

	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (const u32 bw : {1u, 4u})
		{
			for (const u32 align : {0u, 1u, 2u, 3u})
			{
				for (const u32 entries : {256u, 16u})
				{
					// A base four blocks in plus the alignment under test, so the palette is never at
					// the owner's own origin and the page arithmetic is exercised.
					const GatherCase16 c{0x0004u + align, 0x0000u, bw, entries, psm};
					SCOPED_TRACE(testing::Message() << "psm " << psm << " bw " << bw << " cbp " << c.cbp
													<< " (cbp&3 " << (c.cbp & 3) << ") entries " << entries);
					// Big enough to hold every page the palette's blocks can land in. A 16-bit page is
					// 64x64, and four blocks from CBP never leave the second one.
					SeedOwner16(c, bw * 64, 128);
					const std::array<u32, 256> want = RealLoad16(c);

					// The seed has to name every entry differently, or a wrong offset could pass by
					// reading a word that happens to hold the same value.
					for (u32 i = 0; i < entries; i++)
					{
						for (u32 j = i + 1; j < entries; j++)
							ASSERT_NE(want[i], want[j]) << "the seed is degenerate at entries " << i << " and " << j;
					}

					ClutBlockCopy blocks{};
					const std::vector<u32> copied = CopyPaletteCells(f, c, blocks);
					ASSERT_EQ(copied.size(), (entries == 256) ? 512u : 32u);

					for (u32 e = 0; e < entries; e++)
					{
						const u32 off = ClutEntryToCt16Offset(f, entries, blocks.stride, e);
						ASSERT_LT(off + 8, copied.size()) << "entry " << e;
						EXPECT_EQ(gsTileClut16Word(copied[off], copied[off + 8]), want[e]) << "entry " << e;
					}
				}
			}
		}
	}
}

// ...and the same oracle with the palette a whole number of PAGES into the owner, which the matrix
// above never reaches: it puts every case at CBP = owner_bp + 4..7 blocks, so the relative page index
// is zero and the two terms that turn it into a texel origin -- `(pg % bwpg) * 64` for x and
// `(pg / bwpg) * 64` for y -- are multiplied by nothing. A 16-bit page is 64 texels TALL where a
// 32-bit one is 32, so the y term is exactly the transcription a reader coming from LocateClutBlocks
// would get wrong, and it would be wrong ONLY here.
//
// The shapes are the real ones. LEGO Star Wars loads a 16-entry palette at CBP 0x30a0 off a PSMCT16
// target based at 0x2320 with FBW 8 -- 108 pages in, which lands at texel (256, 832) -- and it is the
// only title on the corpus besides Spider-Man 3 that gathers off a 16-bit owner at all.
TEST_F(ClutGatherTest, TheSixteenBitOwnerSurvivesAPageOffsetIntoIt)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut16_valid);

	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		// (owner_bw, pages in) pairs: down one page row, across one page, and both -- so a swapped
		// pair of page terms is caught rather than cancelling out.
		const struct
		{
			u32 bw, pages;
		} at[] = {{1u, 1u}, {2u, 1u}, {2u, 2u}, {2u, 3u}, {4u, 5u}};
		for (const auto& a : at)
		{
			for (const u32 entries : {256u, 16u})
			{
				const GatherCase16 c{0x0000u + a.pages * 32u + 1u, 0x0000u, a.bw, entries, psm};
				SCOPED_TRACE(testing::Message() << "psm " << psm << " bw " << a.bw << " pages in " << a.pages
												<< " cbp " << c.cbp << " entries " << entries);
				SeedOwner16(c, a.bw * 64, (a.pages / a.bw + 2) * 64);
				const std::array<u32, 256> want = RealLoad16(c);
				ClutBlockCopy blocks{};
				const std::vector<u32> copied = CopyPaletteCells(f, c, blocks);
				// The page terms actually moved the origin, or this case is the pg == 0 one again.
				EXPECT_TRUE(blocks.x[0] >= 64 || blocks.y[0] >= 64) << "the page offset did not move the rect";
				for (u32 e = 0; e < entries; e++)
				{
					const u32 off = ClutEntryToCt16Offset(f, entries, blocks.stride, e);
					ASSERT_LT(off + 8, copied.size()) << "entry " << e;
					EXPECT_EQ(gsTileClut16Word(copied[off], copied[off + 8]), want[e]) << "entry " << e;
				}
			}
		}
	}
}

// The 16-bit mapping has to be a bijection onto the tile, and it is the PAIR that has to be one: the
// low cell at `off` and the high cell at `off + 8` are both read, so a collision on either is an
// entry made of somebody else's cells and an offset past the end is the next palette's.
TEST(TileGpuClutGather, TheSixteenBitEntryOrderIsABijectionOntoTheTile)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ASSERT_TRUE(f.clut16_valid);
	// (entries, stride, tile height) -- the two shapes LocateClutBlocks16 produces.
	const struct
	{
		u32 entries, stride, h;
	} shapes[] = {{256u, 32u, 16u}, {16u, 16u, 2u}};
	for (const auto& sh : shapes)
	{
		std::vector<bool> hit(sh.stride * sh.h, false);
		for (u32 e = 0; e < sh.entries; e++)
		{
			const u32 off = ClutEntryToCt16Offset(f, sh.entries, sh.stride, e);
			ASSERT_LT(off, hit.size()) << "entries " << sh.entries << " entry " << e;
			ASSERT_LT(off + 8, hit.size()) << "entries " << sh.entries << " entry " << e << " twin";
			// The twin is the texel eight to the right, so it must stay on the same tile ROW.
			ASSERT_EQ(off / sh.stride, (off + 8) / sh.stride) << "entries " << sh.entries << " entry " << e;
			EXPECT_FALSE(hit[off]) << "entries " << sh.entries << " entry " << e << " reuses low cell " << off;
			EXPECT_FALSE(hit[off + 8]) << "entries " << sh.entries << " entry " << e << " reuses high cell";
			hit[off] = true;
			hit[off + 8] = true;
		}
		// ...and between them the entries name every word of the tile, which is what makes the copy's
		// extent exactly the palette.
		for (size_t i = 0; i < hit.size(); i++)
			EXPECT_TRUE(hit[i]) << "entries " << sh.entries << " word " << i << " is copied and never read";
	}
}

// The folded form the shader will spell, against the CPU spec, at both strides. The pieces occupy
// disjoint bit ranges -- cx at 0-2, the +8 twin at bit 3, the block's x bit at 4, cy at 5-7 for
// stride 32, the block's y bit at 8 -- so every add in the spec is an OR here, and a transcription
// error is a palette that is a permutation of itself.
TEST(TileGpuClutGather, TheSixteenBitFoldedOffsetIsTheSpecAtBothStrides)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ASSERT_TRUE(f.clut16_valid);
	const struct
	{
		u32 entries, stride, pal_mul, pal_shift16;
	} shapes[] = {
		{256u, 32u, 3u, 2u}, // S/8 - 1 = 3, log2(S) - 3 = 2
		{16u, 16u, 1u, 1u}, // S/8 - 1 = 1, log2(S) - 3 = 1
	};
	for (const auto& sh : shapes)
	{
		for (u32 e = 0; e < sh.entries; e++)
		{
			const u32 w = (sh.entries == 256) ? f.clut_i8_word.Eval(e) : f.clut_i4_word.Eval(e);
			const u32 c = f.inv_col32.Eval(w & 63);
			// The shader's arm, verbatim: block bit 1 (w bit 7) carries x and bit 0 (w bit 6) carries
			// y, the opposite way round from the 32-bit merged arm one line of source away.
			const u32 shader = ((w & 0x40u) << sh.pal_shift16) | (c + sh.pal_mul * (c & 0x38u)) | ((w & 0x80u) >> 3u);
			EXPECT_EQ(shader, ClutEntryToCt16Offset(f, sh.entries, sh.stride, e))
				<< "stride " << sh.stride << " entry " << e;
		}
	}

	// ...and the refusal the folded form rests on. At stride 16 a 256-entry palette's block x bit
	// lands on bit 4 and collides with cy, so the answer is a permutation of itself with no symptom.
	// LocateClutBlocks16 never produces that pair: a 256-entry palette is always stride 32.
	ClutBlockCopy b;
	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (const u32 cbp : {0x0000u, 0x0001u, 0x0004u, 0x0007u})
		{
			ASSERT_TRUE(LocateClutBlocks16(f, psm, cbp, 0x0000, 1, 256, b)) << "cbp " << cbp;
			EXPECT_EQ(b.stride, 32u) << "cbp " << cbp;
			ASSERT_TRUE(LocateClutBlocks16(f, psm, cbp, 0x0000, 1, 16, b)) << "cbp " << cbp;
			EXPECT_EQ(b.stride, 16u) << "cbp " << cbp;
		}
	}
	// The collision itself, stated as a fact rather than trusted: at stride 16 the 256 entries do not
	// even land on 256 distinct words.
	std::vector<bool> hit(16u * 32u, false);
	u32 distinct = 0;
	for (u32 e = 0; e < 256u; e++)
	{
		const u32 off = ClutEntryToCt16Offset(f, 256, 16, e);
		ASSERT_LT(off, hit.size());
		distinct += hit[off] ? 0u : 1u;
		hit[off] = true;
	}
	EXPECT_LT(distinct, 256u) << "stride 16 with 256 entries is a bijection after all; the refusal above "
								 "rests on it not being one";
}

// The 16-bit copy's regions have to cover exactly the words the load reads, and land as the four
// quadrants of ONE tile rather than as four runs. That second half is what copy_off buys and what
// makes one shader arm serve both alignments.
TEST(TileGpuClutGather, TheSixteenBitCopyRegionsCoverExactlyTheTile)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut16_valid);
	ClutBlockCopy b;

	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		SCOPED_TRACE(testing::Message() << "psm " << psm);
		// 4-aligned: the four blocks ARE a 2x2 block square, so one 32x16 region says the whole
		// palette and there is nothing to offset.
		ASSERT_TRUE(LocateClutBlocks16(f, psm, 0x0040, 0x0040, 1, 256, b));
		EXPECT_EQ(b.region_count, 1u);
		EXPECT_EQ(b.w, 32u);
		EXPECT_EQ(b.h, 16u);
		EXPECT_EQ(b.stride, 32u);
		EXPECT_EQ(b.copy_off[0], 0u);
		EXPECT_EQ(b.region_count * b.w * b.h, 512u); // exactly the 512 cells of a 256-entry palette
		EXPECT_EQ(b.x[0] % 32, 0u); // 32-aligned in x and 16 in y, which is what makes it a square
		EXPECT_EQ(b.y[0] % 16, 0u);

		// NOT 4-aligned: four 16x8 regions with destination offsets into the same 32x16 tile.
		ASSERT_TRUE(LocateClutBlocks16(f, psm, 0x0041, 0x0040, 1, 256, b));
		EXPECT_EQ(b.region_count, 4u);
		EXPECT_EQ(b.w, 16u);
		EXPECT_EQ(b.h, 8u);
		EXPECT_EQ(b.stride, 32u);
		EXPECT_EQ(b.region_count * b.w * b.h, 512u);
		// Block j at (x = (j>>1)*16, y = (j&1)*8) of the tile -- the quadrant the entry map selects.
		EXPECT_EQ(b.copy_off[0], 0u);
		EXPECT_EQ(b.copy_off[1], 8u * 32u);
		EXPECT_EQ(b.copy_off[2], 16u);
		EXPECT_EQ(b.copy_off[3], 8u * 32u + 16u);

		// Sixteen entries: two texel rows of one block, all sixteen columns.
		ASSERT_TRUE(LocateClutBlocks16(f, psm, 0x0043, 0x0040, 1, 16, b));
		EXPECT_EQ(b.region_count, 1u);
		EXPECT_EQ(b.w, 16u);
		EXPECT_EQ(b.h, 2u);
		EXPECT_EQ(b.stride, 16u);
		EXPECT_EQ(b.copy_off[0], 0u);
		EXPECT_EQ(b.region_count * b.w * b.h, 32u);

		// Every word of the tile is written exactly once by the regions, at both alignments -- one
		// word more and the copy runs into the next reservation, one fewer and an entry reads it.
		for (const u32 cbp : {0x0040u, 0x0041u, 0x0042u, 0x0043u})
		{
			ASSERT_TRUE(LocateClutBlocks16(f, psm, cbp, 0x0040, 1, 256, b)) << "cbp " << cbp;
			std::vector<u32> written(512u, 0u);
			for (u32 r = 0; r < b.region_count; r++)
			{
				for (u32 y = 0; y < b.h; y++)
				{
					for (u32 x = 0; x < b.w; x++)
					{
						const u32 at = b.copy_off[r] + y * b.stride + x;
						ASSERT_LT(at, written.size()) << "cbp " << cbp;
						written[at]++;
					}
				}
			}
			for (size_t i = 0; i < written.size(); i++)
				EXPECT_EQ(written[i], 1u) << "cbp " << cbp << " word " << i;
		}
	}

	// The refusals. A 32-bit owner does not come down this road at all -- that is the whole reason it
	// is a separate function -- and neither does an entry count the CSM1 loaders never produce or a
	// palette below the owner's base.
	EXPECT_FALSE(LocateClutBlocks16(f, PSMCT32, 0x0040, 0x0040, 1, 256, b));
	EXPECT_FALSE(LocateClutBlocks16(f, PSMCT24, 0x0040, 0x0040, 1, 256, b));
	EXPECT_FALSE(LocateClutBlocks16(f, PSMZ16, 0x0040, 0x0040, 1, 256, b));
	EXPECT_FALSE(LocateClutBlocks16(f, PSMCT16, 0x0020, 0x0040, 1, 256, b));
	EXPECT_FALSE(LocateClutBlocks16(f, PSMCT16, 0x0040, 0x0040, 1, 64, b));
	EXPECT_FALSE(LocateClutBlocks16(f, PSMCT16, 0x0040, 0x0040, 1, 0, b));

	// Forms that did not fit turn the road off rather than addressing the wrong bytes -- and
	// clut16_valid does it ALONE, without touching the two flags the other roads read.
	FormSet unfit = f;
	unfit.clut16_valid = false;
	EXPECT_FALSE(LocateClutBlocks16(unfit, PSMCT16, 0x0040, 0x0040, 1, 256, b));
	EXPECT_TRUE(LocateClutBlocks(unfit, 0x0040, 0x0040, 1, 256, false, b))
		<< "clut16_valid gated the 32-bit road, which it must never do";
}

// The 32-bit road is BYTE-IDENTICAL to what it was before the 16-bit one existed.
//
// LocateClutBlocks16 is a sibling rather than an argument to LocateClutBlocks precisely so this is
// true by construction, but "by construction" is what everybody says before the regression. The
// golden below was taken from the tree at 54be736885, before a line of the 16-bit road was written,
// over a sweep that crosses three owner bases, three owner widths, both entry counts, both merge
// answers and 64 CBPs each -- 2,304 calls -- folding every field of every result.
//
// If this trips, something changed the shipped copy geometry. The answer is not to re-take the
// number: three levers already ship on that geometry and a frame is what says whether it moved.
TEST(TileGpuClutGather, TheCt32CopyIsUnmovedByTheSixteenBitRoad)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	ASSERT_TRUE(f.clut_valid);
	u64 h = 0xCBF29CE484222325ull;
	const auto fold = [&h](u32 v) { h ^= v; h *= 0x100000001B3ull; };
	for (const u32 owner_bp : {0x0000u, 0x0080u, 0x0800u})
	{
		for (const u32 bwpg : {1u, 2u, 4u})
		{
			for (const u32 entries : {256u, 16u})
			{
				for (int merge = 0; merge < 2; merge++)
				{
					for (u32 d = 0; d < 64; d++)
					{
						ClutBlockCopy b;
						const bool ok = LocateClutBlocks(f, owner_bp + d, owner_bp, bwpg, entries, merge != 0, b);
						fold(ok ? 1u : 0u);
						fold(b.region_count);
						fold(b.w);
						fold(b.h);
						fold(b.merged ? 1u : 0u);
						fold(b.stride);
						for (u32 i = 0; i < ClutBlockCopy::kMaxRegions; i++)
						{
							fold(b.x[i]);
							fold(b.y[i]);
						}
						// ...and the new field is zero on this road, which is what makes it invisible
						// to an executor that goes on deriving region b's destination as b * w * h.
						for (u32 i = 0; i < ClutBlockCopy::kMaxRegions; i++)
							EXPECT_EQ(b.copy_off[i], 0u) << "cbp " << (owner_bp + d) << " region " << i;
					}
				}
			}
		}
	}
	EXPECT_EQ(h, 0xa2f1be67c0b2fcc5ull) << "the 32-bit CLUT copy geometry moved";
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

// The op's destination arithmetic, which is the executor's and has to be pinned here because the
// executor itself is a Vulkan command buffer no test can build.
//
// Two derivations in one pair of accessors. Zero stride is what the 32-bit road has always produced
// and must go on meaning exactly what it meant: rows `copy_w` wide, region b at `b * copy_w * copy_h`.
// A non-zero stride PLACES the regions instead, which is what lets a 16-bit owner's four scattered
// blocks land as the four quadrants of one tile. The first half is the whole no-behaviour-change
// claim of the field's landing, so it is asserted over the shapes LocateClutBlocks actually emits
// rather than over invented ones.
TEST(TileGpuClutGather, ACopyWithNoStridePlacesItsRegionsExactlyWhereItAlwaysDid)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);

	// Every shape the 32-bit road produces: the four-block run, the merged square, and the 16-entry
	// pair of rows, at both owner widths the suite exercises.
	const GatherCase cases[] = {
		{0x0040, 0x0000, 1, 256}, {0x0040, 0x0000, 4, 256}, {0x0041, 0x0000, 1, 256},
		{0x0040, 0x0000, 1, 16}, {0x0043, 0x0000, 1, 16}, {0x0080, 0x0040, 4, 256},
	};
	for (const GatherCase& c : cases)
	{
		for (const bool merge : {false, true})
		{
			ClutBlockCopy blocks;
			ASSERT_TRUE(LocateClutBlocks(f, c.cbp, c.owner_bp, c.owner_bw, c.entries, merge, blocks));
			GSDevice::GSTileGpuPrepOp op = {};
			op.kind = GSDevice::GSTileGpuPrepKind::ClutBlockCopy;
			op.copy_count = blocks.region_count;
			op.copy_w = blocks.w;
			op.copy_h = blocks.h;
			for (u32 b = 0; b < blocks.region_count; b++)
				op.copy_off[b] = blocks.copy_off[b];
			// The 32-bit road leaves copy_stride at zero and copy_off at zero, and that is the field's
			// whole safety argument: the accessors then answer what the executor computed inline before
			// they existed.
			EXPECT_EQ(op.copy_stride, 0u) << "cbp " << c.cbp << " merge " << merge;
			EXPECT_EQ(op.CopyRowLength(), op.copy_w);
			for (u32 b = 0; b < blocks.region_count; b++)
			{
				EXPECT_EQ(op.copy_off[b], 0u) << "cbp " << c.cbp << " region " << b;
				EXPECT_EQ(op.CopyRegionOffset(b), b * op.copy_w * op.copy_h) << "cbp " << c.cbp << " region " << b;
			}
		}
	}
}

// ...and the placed form: a stride and per-region offsets have to tile the reservation EXACTLY.
// Every word of the destination written once, none written twice, none written past the run the
// renderer reserved -- because a copy that overruns rewrites the next palette in the stream, and one
// that leaves a hole gives some entry whatever the reservation was allocated with (zero).
TEST(TileGpuClutGather, APlacedCopyTilesItsReservationExactly)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut16_valid);

	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (const u32 entries : {16u, 256u})
		{
			for (u32 cbp = 0x0040; cbp < 0x0044; cbp++)
			{
				ClutBlockCopy blocks;
				ASSERT_TRUE(LocateClutBlocks16(f, psm, cbp, 0x0040, 1, entries, blocks))
					<< "psm " << psm << " entries " << entries << " cbp " << cbp;
				GSDevice::GSTileGpuPrepOp op = {};
				op.kind = GSDevice::GSTileGpuPrepKind::ClutBlockCopy;
				op.copy_count = blocks.region_count;
				op.copy_w = blocks.w;
				op.copy_h = blocks.h;
				op.copy_stride = blocks.stride;
				for (u32 b = 0; b < blocks.region_count; b++)
					op.copy_off[b] = blocks.copy_off[b];

				// The reservation the renderer makes is region_count * w * h words, which is also the
				// tile: stride x (reservation / stride).
				const u32 reserved = blocks.region_count * blocks.w * blocks.h;
				ASSERT_NE(op.CopyRowLength(), 0u);
				EXPECT_EQ(op.CopyRowLength(), blocks.stride);
				EXPECT_EQ(reserved % op.CopyRowLength(), 0u);

				std::vector<u32> written(reserved, 0);
				for (u32 b = 0; b < op.copy_count; b++)
				{
					const u32 base = op.CopyRegionOffset(b);
					for (u32 y = 0; y < op.copy_h; y++)
					{
						for (u32 x = 0; x < op.copy_w; x++)
						{
							const u32 at = base + y * op.CopyRowLength() + x;
							ASSERT_LT(at, reserved) << "psm " << psm << " entries " << entries << " cbp " << cbp
													<< " region " << b << " (" << x << ", " << y << ")";
							written[at]++;
						}
					}
				}
				for (u32 w = 0; w < reserved; w++)
				{
					EXPECT_EQ(written[w], 1u) << "psm " << psm << " entries " << entries << " cbp " << cbp
											  << " word " << w;
				}
			}
		}
	}
}

// The state row the renderer hands the fragment stage for a gathered palette, against the arithmetic
// the shader will do with it. Three numbers, and every one of them is silent when it is wrong: a
// mode names the entry order, and mul/shift name the tile's row stride in the two forms the arm folds
// into bit positions. Get any of them wrong and the palette is a permutation of itself.
//
// The 16-bit half is the new claim; the 32-bit half is the pin that says moving this derivation into
// a function did not move a number.
TEST(TileGpuClutGather, TheStateRowNamesTheOrderTheShaderWillFetchIn)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ASSERT_TRUE(f.clut16_valid);

	// The 32-bit road, term for term as the renderer spelled it inline: an unmerged copy is mode 1 or
	// 2 by entry count and reads neither stride form; a merged one is mode 3 at stride 16 or 64.
	const struct
	{
		u32 entries, stride, mode, mul, shift;
	} ct32[] = {
		{256u, 0u, 1u, 1u, 0u},
		{16u, 0u, 2u, 1u, 0u},
		{256u, 16u, 3u, 1u, 0u},
		{256u, 64u, 3u, 7u, 2u},
	};
	for (const auto& t : ct32)
	{
		const GSTileGpuPalRow row = gsTileGpuClutPalRow(false, t.entries, t.stride);
		EXPECT_EQ(row.mode, t.mode) << "entries " << t.entries << " stride " << t.stride;
		EXPECT_EQ(row.mul, t.mul) << "entries " << t.entries << " stride " << t.stride;
		EXPECT_EQ(row.shift, t.shift) << "entries " << t.entries << " stride " << t.stride;
		EXPECT_LT(row.mode, 4u) << "a 32-bit owner must never ask for the sixteen-bit arm";
	}
	// ...and mode 3's two strides really are the merged order at that stride.
	for (const u32 stride : {16u, 64u})
	{
		const GSTileGpuPalRow row = gsTileGpuClutPalRow(false, 256, stride);
		for (u32 e = 0; e < 256; e++)
		{
			const u32 w = f.clut_i8_word.Eval(e);
			const u32 c = f.inv_col32.Eval(w & 63);
			const u32 shader = ((w & 0x80u) << row.shift) | (c + row.mul * (c & 0x38u)) | ((w & 0x40u) >> 3u);
			EXPECT_EQ(shader, ClutEntryToMergedOffset(f, 256, stride, e)) << "stride " << stride << " entry " << e;
		}
	}

	// The 16-bit road. The shapes are LocateClutBlocks16's own, so this cannot drift from what the
	// copy actually produces: a 256-entry palette is always stride 32 and a 16-entry one always 16.
	for (const u32 psm : {static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (const u32 entries : {16u, 256u})
		{
			ClutBlockCopy blocks;
			ASSERT_TRUE(LocateClutBlocks16(f, psm, 0x0041, 0x0040, 1, entries, blocks));
			const GSTileGpuPalRow row = gsTileGpuClutPalRow(true, entries, blocks.stride);
			EXPECT_EQ(row.mode, (entries == 256) ? 4u : 5u) << "psm " << psm << " entries " << entries;
			for (u32 e = 0; e < entries; e++)
			{
				const u32 w = (entries == 256) ? f.clut_i8_word.Eval(e) : f.clut_i4_word.Eval(e);
				const u32 c = f.inv_col32.Eval(w & 63);
				// tilegpu.glsl's mode 4/5 arm verbatim, including the y shift it spells as
				// `pal_shift + 1` rather than carrying a state-row word for.
				const u32 shader =
					((w & 0x40u) << (row.shift + 1u)) | (c + row.mul * (c & 0x38u)) | ((w & 0x80u) >> 3u);
				EXPECT_EQ(shader, ClutEntryToCt16Offset(f, entries, blocks.stride, e))
					<< "psm " << psm << " entries " << entries << " entry " << e;
			}
		}
	}
}

// The pass-split guard, which no corpus dump exercises. Spider-Man 3 is the only title that gathers
// a 16-bit palette at all and its target-road draws are 16 of 43,747 a frame, so this predicate ships
// with a measured count of zero -- which is exactly why it is a function with a test and not three
// conditions inside AccumulateDraw.
//
// What it protects: a pass's fragment program is the union of its draws' texel arms, and two of those
// unions are past the Adreno 650's instruction cliff. It must break for those two and for nothing
// else, because every needless break costs a render pass.
TEST(TileGpuClutGather, APassCarriesOneGatherArmAndNeverTheSixteenBitOneOverTheTargetRoad)
{
	// The mode -> arm map, against the modes the shader actually switches on.
	EXPECT_EQ(gsTileGpuClutGatherArm(0), 0u);
	EXPECT_EQ(gsTileGpuClutGatherArm(1), 1u); // 256-entry, blocks as a run
	EXPECT_EQ(gsTileGpuClutGatherArm(2), 1u); // 16-entry, ditto
	EXPECT_EQ(gsTileGpuClutGatherArm(3), 1u); // merged tile
	EXPECT_EQ(gsTileGpuClutGatherArm(4), 2u); // 16-bit owner, 256 entries
	EXPECT_EQ(gsTileGpuClutGatherArm(5), 2u); // 16-bit owner, 16 entries
	// ...and the map is the one the renderer's own derivation produces, at every stride either road
	// emits, so the two spellings cannot drift apart.
	EXPECT_EQ(gsTileGpuClutGatherArm(gsTileGpuClutPalRow(false, 256, 0).mode), 1u);
	EXPECT_EQ(gsTileGpuClutGatherArm(gsTileGpuClutPalRow(false, 16, 0).mode), 1u);
	EXPECT_EQ(gsTileGpuClutGatherArm(gsTileGpuClutPalRow(false, 256, 64).mode), 1u);
	EXPECT_EQ(gsTileGpuClutGatherArm(gsTileGpuClutPalRow(true, 256, 32).mode), 2u);
	EXPECT_EQ(gsTileGpuClutGatherArm(gsTileGpuClutPalRow(true, 16, 16).mode), 2u);

	// An empty pass takes anything, and so does a draw that asks for nothing.
	for (u32 arm = 0; arm <= 2; arm++)
	{
		for (const bool tgt : {false, true})
		{
			EXPECT_FALSE(gsTileGpuClutArmBreaks(0, false, arm, tgt)) << "arm " << arm << " target " << tgt;
			EXPECT_FALSE(gsTileGpuClutArmBreaks(arm, tgt, 0, false)) << "arm " << arm << " target " << tgt;
		}
	}

	// THE TWO IT MUST BREAK ON.
	EXPECT_TRUE(gsTileGpuClutArmBreaks(1, false, 2, false)); // 32-bit pass, 16-bit draw
	EXPECT_TRUE(gsTileGpuClutArmBreaks(2, false, 1, false)); // and the other way round
	EXPECT_TRUE(gsTileGpuClutArmBreaks(0, true, 2, false)); // target-road pass, 16-bit gather draw
	EXPECT_TRUE(gsTileGpuClutArmBreaks(2, false, 0, true)); // 16-bit pass, target-road draw
	EXPECT_TRUE(gsTileGpuClutArmBreaks(2, false, 1, true));
	EXPECT_TRUE(gsTileGpuClutArmBreaks(1, true, 2, true));

	// AND THE ONES IT MUST NOT, which is where a lazier predicate would cost passes for nothing. The
	// 32-bit order with the target road is 109 units at byte+target[IDX4+PALGATHER] and 114 with the
	// source road on top -- both comfortably under, so it is deliberately unrestricted.
	EXPECT_FALSE(gsTileGpuClutArmBreaks(1, false, 1, false));
	EXPECT_FALSE(gsTileGpuClutArmBreaks(1, true, 1, true));
	EXPECT_FALSE(gsTileGpuClutArmBreaks(1, false, 1, true));
	EXPECT_FALSE(gsTileGpuClutArmBreaks(0, true, 1, true));
	EXPECT_FALSE(gsTileGpuClutArmBreaks(2, false, 2, false)); // two 16-bit gathers share a pass

	// The whole accumulator loop, folded over the worst sequence there is: a 16-bit gather, a
	// target-road draw, a 16-bit gather again, a 32-bit gather. Two things have to hold, and neither
	// is visible from the rows above.
	//
	//  - A break is never needed TWICE for one draw. BreakOpenPass empties the pass, and the emptied
	//    pass has to accept the draw that forced the break; a predicate that refused it again would
	//    spin the accumulator, and it would spin only on a title nothing in the corpus exercises.
	//  - The state (16-bit arm AND target road in one pass) is never reached, which is what makes the
	//    over-cliff union unreachable rather than merely unlikely. It is also why one such DRAW is
	//    not a case: rule 2 binds a target only where the read's own layout IS the target's, and a
	//    palettised read of a colour surface never is (TargetForTextureRead), so no single draw
	//    carries both.
	const struct
	{
		u32 arm;
		bool target;
	} sequence[] = {{2, false}, {0, true}, {2, false}, {2, false}, {1, false}, {0, true}, {1, false}, {0, false}};
	u32 open_arm = 0;
	bool open_target = false;
	for (const auto& d : sequence)
	{
		if (gsTileGpuClutArmBreaks(open_arm, open_target, d.arm, d.target))
		{
			open_arm = 0;
			open_target = false;
			ASSERT_FALSE(gsTileGpuClutArmBreaks(open_arm, open_target, d.arm, d.target))
				<< "the break did not admit the draw that forced it: arm " << d.arm << " target " << d.target;
		}
		if (d.arm != 0)
			open_arm = d.arm;
		open_target = open_target || d.target;
		EXPECT_FALSE(open_arm == 2u && open_target)
			<< "a pass came to carry the sixteen-bit gather arm AND the rule-2 target road, which is the "
			   "over-cliff union this guard exists to make unreachable";
	}
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
