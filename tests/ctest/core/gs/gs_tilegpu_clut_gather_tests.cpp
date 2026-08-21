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

#include <gtest/gtest.h>

#include <array>
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
	std::vector<u32> CopyPaletteBlocks(const FormSet& f, const GatherCase& c)
	{
		ClutBlockCopy blocks;
		EXPECT_TRUE(LocateClutBlocks(f, c.cbp, c.owner_bp, c.owner_bw, c.entries, blocks));
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

// -- 2. the copy geometry ----------------------------------------------------------------------

// The regions have to cover exactly the words the load reads. One texel more and the copy runs off
// the end of the reservation into the next palette; one fewer and some entry reads whatever the
// reservation was filled with.
TEST(TileGpuClutGather, TheCopyRegionsCoverExactlyThePalette)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.clut_valid);
	ClutBlockCopy b;

	ASSERT_TRUE(LocateClutBlocks(f, 0x40, 0x40, 1, 256, b));
	EXPECT_EQ(b.region_count * b.w * b.h, 256u);
	EXPECT_EQ(b.region_count, 4u); // 256 words = four blocks, contiguous from CBP
	EXPECT_EQ(b.w, 8u);
	EXPECT_EQ(b.h, 8u);

	ASSERT_TRUE(LocateClutBlocks(f, 0x40, 0x40, 1, 16, b));
	EXPECT_EQ(b.region_count * b.w * b.h, 16u);
	EXPECT_EQ(b.region_count, 1u); // 16 words = the first two texel rows of one block
	EXPECT_EQ(b.w, 8u);
	EXPECT_EQ(b.h, 2u);

	// A palette below the owner's base is out of the reinterpretation's reach (the shader computes
	// `blk - dst_bp` unsigned), and so is an entry count the CSM1 32-bit loaders never produce.
	EXPECT_FALSE(LocateClutBlocks(f, 0x20, 0x40, 1, 256, b));
	EXPECT_FALSE(LocateClutBlocks(f, 0x40, 0x40, 1, 64, b));
	EXPECT_FALSE(LocateClutBlocks(f, 0x40, 0x40, 1, 0, b));

	// Forms that did not fit turn the road off rather than addressing the wrong bytes.
	FormSet unfit = f;
	unfit.clut_valid = false;
	EXPECT_FALSE(LocateClutBlocks(unfit, 0x40, 0x40, 1, 256, b));
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
