// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The CPU->GPU upload merge's keep mask: which BYTES of a guest page a host->local transfer writes.
//
// This is the one number in the merge that can be silently wrong. The merge stages the page from
// the CPU shadow -- which by then carries the transfer -- and then runs a writeback that fills in
// the surface's remaining bytes under the complement of this mask. A bit set where the transfer did
// NOT write loses the surface's byte; a bit clear where it DID write loses the CPU's. Neither shows
// up as a crash, a validation error or a counter: both are a wrong pixel in a texture somebody
// samples several frames later.
//
// So the mask is not tested against a transcription of the address arithmetic. It is tested against
// GSLocalMemory ITSELF: zero the whole of guest memory, write all-ones over the rect through the
// PSM's own writer, and every byte that came out non-zero is a byte the transfer writes. That is
// the same oracle shape the swizzle-form tests use, and it means the mask cannot drift from the
// writer it has to agree with, whatever either of them is edited into.
//
// The all-ones pattern is what makes the oracle exact at SUB-BYTE granularity too. A 4-bit format
// writes half a byte, so a byte the transfer covers fully reads 0xFF and one it covers by a nibble
// reads 0x0F or 0xF0 -- and a half-written byte is exactly the case the builder must REFUSE,
// because a byte mask cannot express "the CPU owns this nibble and the surface owns the other".
//
// ⚠️ RED CHECK, run 2026-08-24 and recorded here because the arrangement it protects looks
// redundant: treating every block the rect ENTERS as fully written (the per-block answer the
// footprint already carries) fails exactly three of the nine tests below --
// TheKeepMaskIsExactlyWhatGSLocalMemoryWrites, APartiallyCoveredBlockClaimsOnlyTheBytesItLandsOn
// and APSMT4RectWhoseNibblesDoNotPairIsRefused -- and leaves the other six passing. The three that
// fail are precisely the ones that say the mask is per BYTE.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"
#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <memory>
#include <vector>

namespace
{
using UploadPageBytes = GSRendererTileGpu::UploadPageBytes;

constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm)
{
	return GSTileSurfaceLayout{bp, bw, psm, GSTileSurfaceKind::Color};
}

/// The oracle: guest memory after zeroing it and writing all-ones over `r` through `psm`'s own
/// per-pixel writer. Every byte that is non-zero is a byte the transfer touches; every byte that is
/// 0xFF is one it covers whole.
std::vector<u8> WriteThroughGSLocalMemory(GSLocalMemory& mem, const GSTileSurfaceLayout& l, const GSVector4i& r)
{
	std::memset(mem.vm8(), 0, GS_PAGE_SIZE * GS_MAX_PAGES);
	const GSOffset off = GSOffset::fromKnownPSM(l.bp, l.bw, static_cast<GS_PSM>(l.psm));
	const GSLocalMemory::writePixelAddr wpa = GSLocalMemory::m_psm[l.psm].wpa;
	for (int y = r.top; y < r.bottom; y++)
	{
		for (int x = r.left; x < r.right; x++)
			(mem.*wpa)(off.pa(x, y), 0xFFFFFFFFu);
	}
	return std::vector<u8>(mem.vm8(), mem.vm8() + GS_PAGE_SIZE * GS_MAX_PAGES);
}

/// Every page the rect touches, so the builder is asked about all of them.
GSPageBitmap PagesOf(const GSTileSurfaceLayout& l, const GSVector4i& r)
{
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(l, r, fp);
	return fp.pages;
}

/// Is byte `byte` of block `blk` set in this page's keep table?
bool KeepBit(const std::vector<u32>& words, const UploadPageBytes& pb, u32 blk, u32 byte)
{
	const u32 wib = byte >> 2;
	return ((words[pb.first_word + blk * 8 + (wib >> 3)] >> (((wib & 7) * 4) + (byte & 3))) & 1) != 0;
}

struct Case
{
	const char* name;
	u32 bp, bw, psm;
	GSVector4i rect;
};
} // namespace

// The whole contract, against GSLocalMemory's own writers, over the transfer shapes the corpus
// actually produces. The first four rows are Shadow of the Colossus' and Ace Combat 5's measured
// spill transfers (BYTEMASK_SHAPES census, 2026-08-24); the rest widen the format coverage.
TEST(GSTileGpuUploadMerge, TheKeepMaskIsExactlyWhatGSLocalMemoryWrites)
{
	auto mem = std::make_unique<GSLocalMemory>();
	static const Case cases[] = {
		{"sotc 16x16 PSMT4 into a block-aligned base", 11074, 0, PSMT4, GSVector4i(0, 0, 16, 16)},
		{"sotc 8x8 PSMT4", 12096, 0, PSMT4, GSVector4i(0, 0, 8, 8)},
		{"sotc 16x16 PSMT4 at 11032", 11032, 0, PSMT4, GSVector4i(0, 0, 16, 16)},
		{"an 8x2 PSMCT32 tail write", 9216, 0, PSMCT32, GSVector4i(0, 0, 8, 2)},
		{"a 16x4 PSMCT32 write", 14752, 1, PSMCT32, GSVector4i(0, 0, 16, 4)},
		{"a whole-block PSMCT32 write", 0, 1, PSMCT32, GSVector4i(0, 0, 8, 8)},
		{"a partial PSMCT32 block", 0, 1, PSMCT32, GSVector4i(3, 1, 7, 6)},
		{"a PSMCT24 write (no alpha byte)", 0, 1, PSMCT24, GSVector4i(0, 0, 16, 16)},
		{"a PSMT8H write (the alpha byte only)", 0, 1, PSMT8H, GSVector4i(0, 0, 16, 16)},
		{"a PSMCT16 write", 0, 1, PSMCT16, GSVector4i(0, 0, 16, 16)},
		{"a PSMCT16S write", 0, 1, PSMCT16S, GSVector4i(5, 3, 21, 19)},
		{"a PSMT8 write", 0, 1, PSMT8, GSVector4i(0, 0, 24, 24)},
		{"a PSMZ32 write", 0, 1, PSMZ32, GSVector4i(1, 1, 15, 9)},
		{"a multi-page PSMCT32 write", 0, 2, PSMCT32, GSVector4i(30, 20, 100, 50)},
	};

	for (const Case& c : cases)
	{
		SCOPED_TRACE(c.name);
		const GSTileSurfaceLayout l = Layout(c.bp, static_cast<u8>(c.bw), static_cast<u8>(c.psm));
		const std::vector<u8> after = WriteThroughGSLocalMemory(*mem, l, c.rect);

		std::vector<UploadPageBytes> out;
		std::vector<u32> words;
		ASSERT_TRUE(GSRendererTileGpu::BuildUploadByteMask(l, c.rect, PagesOf(l, c.rect), out, words));

		// Every byte of every page the builder named agrees with the oracle, and no byte the oracle
		// says was touched belongs to a page the builder left out.
		u64 oracle_touched = 0, mask_set = 0;
		for (u32 b = 0; b < GS_PAGE_SIZE * GS_MAX_PAGES; b++)
		{
			if (after[b] == 0)
				continue;
			// A half-written byte would have made the builder refuse, which the ASSERT above rules
			// out for these rows.
			ASSERT_EQ(after[b], 0xFF) << "byte " << b << " is only half written";
			oracle_touched++;
			const u32 page = b / GS_PAGE_SIZE;
			const u32 in_page = b % GS_PAGE_SIZE;
			const UploadPageBytes* pb = nullptr;
			for (const UploadPageBytes& e : out)
			{
				if (e.page == page)
					pb = &e;
			}
			ASSERT_NE(pb, nullptr) << "page " << page << " written but not named";
			EXPECT_TRUE(KeepBit(words, *pb, in_page >> 8, in_page & 255))
				<< "page " << page << " byte " << in_page << " written but not claimed";
			EXPECT_NE(pb->blocks & (1u << (in_page >> 8)), 0u);
		}
		for (const UploadPageBytes& e : out)
		{
			for (u32 blk = 0; blk < 32; blk++)
			{
				for (u32 byte = 0; byte < 256; byte++)
				{
					if (!KeepBit(words, e, blk, byte))
						continue;
					mask_set++;
					EXPECT_EQ(after[e.page * GS_PAGE_SIZE + blk * 256 + byte], 0xFF)
						<< "page " << e.page << " block " << blk << " byte " << byte << " claimed but not written";
				}
				// A block the mask calls WHOLE must have all 256 of its bytes written.
				if (e.blocks_whole & (1u << blk))
				{
					for (u32 byte = 0; byte < 256; byte++)
						EXPECT_EQ(after[e.page * GS_PAGE_SIZE + blk * 256 + byte], 0xFF);
				}
			}
		}
		EXPECT_EQ(oracle_touched, mask_set);
		EXPECT_GT(mask_set, 0u);
	}
}

// The two formats that write half a byte on EVERY texel: no rect makes them expressible, so the
// builder must refuse before it does any work. A merge planned off one of them would either write
// the surface's whole byte over the CPU's nibble or skip it and lose the surface's.
TEST(GSTileGpuUploadMerge, TheFourBitAlphaViewsAreRefusedOutright)
{
	for (const u32 psm : {static_cast<u32>(PSMT4HL), static_cast<u32>(PSMT4HH)})
	{
		SCOPED_TRACE(psm);
		const GSTileSurfaceLayout l = Layout(0, 1, static_cast<u8>(psm));
		const GSVector4i r(0, 0, 16, 16);
		std::vector<UploadPageBytes> out;
		std::vector<u32> words;
		EXPECT_FALSE(GSRendererTileGpu::BuildUploadByteMask(l, r, PagesOf(l, r), out, words));
	}
}

// PSMT4 is different: its texels pair into whole bytes, so whether a rect is expressible depends on
// the rect. One that splits a pair must be refused; the paired ones above are not.
TEST(GSTileGpuUploadMerge, APSMT4RectWhoseNibblesDoNotPairIsRefused)
{
	auto mem = std::make_unique<GSLocalMemory>();
	const GSTileSurfaceLayout l = Layout(0, 1, PSMT4);
	const GSVector4i r(0, 0, 1, 1); // one texel: half of byte 0
	// The oracle says so first, so the refusal is not just this test agreeing with itself.
	const std::vector<u8> after = WriteThroughGSLocalMemory(*mem, l, r);
	EXPECT_EQ(after[0] & 0xF0, 0u);
	EXPECT_NE(after[0] & 0x0F, 0u);

	std::vector<UploadPageBytes> out;
	std::vector<u32> words;
	EXPECT_FALSE(GSRendererTileGpu::BuildUploadByteMask(l, r, PagesOf(l, r), out, words));
}

// A block the rect covers entirely, in a format that writes every byte of its unit: the whole block
// is the CPU's, and the merge drops it from the writeback's block mask altogether rather than
// masking it byte by byte.
TEST(GSTileGpuUploadMerge, AWhollyCoveredBlockIsReportedWhole)
{
	const GSTileSurfaceLayout l = Layout(0, 1, PSMCT32);
	const GSVector4i r(0, 0, 8, 8); // exactly block 0 of a 32-bit page
	std::vector<UploadPageBytes> out;
	std::vector<u32> words;
	ASSERT_TRUE(GSRendererTileGpu::BuildUploadByteMask(l, r, PagesOf(l, r), out, words));
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].blocks, 1u);
	EXPECT_EQ(out[0].blocks_whole, 1u);
	for (u32 w = 0; w < 8; w++)
		EXPECT_EQ(words[out[0].first_word + w], 0xFFFFFFFFu);
}

// ...and the case the whole mechanism exists for. A block the rect only ENTERS is neither the CPU's
// nor the surface's: it is split, byte by byte, and the mask has to say which is which.
//
// This is the red check's target: a builder that used the footprint's per-BLOCK coverage would
// report this block whole and hand the surface's bytes to nobody.
TEST(GSTileGpuUploadMerge, APartiallyCoveredBlockClaimsOnlyTheBytesItLandsOn)
{
	auto mem = std::make_unique<GSLocalMemory>();
	const GSTileSurfaceLayout l = Layout(0, 1, PSMCT32);
	const GSVector4i r(0, 0, 4, 8); // the left half of block 0
	const std::vector<u8> after = WriteThroughGSLocalMemory(*mem, l, r);

	std::vector<UploadPageBytes> out;
	std::vector<u32> words;
	ASSERT_TRUE(GSRendererTileGpu::BuildUploadByteMask(l, r, PagesOf(l, r), out, words));
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].blocks, 1u);
	EXPECT_EQ(out[0].blocks_whole, 0u) << "a block the rect only enters is not the CPU's whole";

	u32 claimed = 0;
	for (u32 byte = 0; byte < 256; byte++)
	{
		if (KeepBit(words, out[0], 0, byte))
			claimed++;
		EXPECT_EQ(KeepBit(words, out[0], 0, byte), after[byte] == 0xFF);
	}
	EXPECT_EQ(claimed, 4u * 8u * 4u) << "32 texels of four bytes each";
}

// A 24-bit transfer leaves the alpha byte to whoever owns that plane, so a block it covers whole is
// still not the CPU's whole -- three bytes of every word, and the fourth stays the surface's. Same
// question the other way for the byte-3 view.
TEST(GSTileGpuUploadMerge, TheNarrowFormatsClaimOnlyTheirOwnBytesOfEachWord)
{
	struct
	{
		u32 psm;
		u32 want_per_word; // the 4-bit byte mask every word of a fully covered block should carry
	} const rows[] = {
		{PSMCT24, 0x7},
		{PSMZ24, 0x7},
		{PSMT8H, 0x8},
		{PSMCT32, 0xF},
	};
	for (const auto& row : rows)
	{
		SCOPED_TRACE(row.psm);
		const GSTileSurfaceLayout l = Layout(0, 1, static_cast<u8>(row.psm));
		const GSVector4i r(0, 0, 8, 8);
		std::vector<UploadPageBytes> out;
		std::vector<u32> words;
		ASSERT_TRUE(GSRendererTileGpu::BuildUploadByteMask(l, r, PagesOf(l, r), out, words));
		ASSERT_EQ(out.size(), 1u);
		// The block index is the format's own -- a Z-swizzled page puts block (0,0) somewhere else
		// entirely (the depth block XOR), so the table is read at the block the builder named.
		ASSERT_NE(out[0].blocks, 0u);
		const u32 blk = static_cast<u32>(std::countr_zero(out[0].blocks));
		EXPECT_EQ(out[0].blocks, 1u << blk) << "one 8x8 rect is one block";
		const u32 want = row.want_per_word * 0x11111111u;
		for (u32 w = 0; w < 8; w++)
			EXPECT_EQ(words[out[0].first_word + blk * 8 + w], want);
		EXPECT_EQ(out[0].blocks_whole, (row.want_per_word == 0xF) ? (1u << blk) : 0u);
	}
}

// A page the caller did not ask about contributes nothing, even where the rect covers it: the merge
// asks only about the pages its spill named, and a table for anything else would be a keep mask a
// writeback of some other page could pick up.
TEST(GSTileGpuUploadMerge, OnlyTheAskedPagesGetATable)
{
	const GSTileSurfaceLayout l = Layout(0, 2, PSMCT32);
	const GSVector4i r(0, 0, 128, 64); // four pages
	const GSPageBitmap all = PagesOf(l, r);
	ASSERT_EQ(all.count(), 4u);

	GSPageBitmap one;
	one.set(2);
	std::vector<UploadPageBytes> out;
	std::vector<u32> words;
	ASSERT_TRUE(GSRendererTileGpu::BuildUploadByteMask(l, r, one, out, words));
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0].page, 2u);
	EXPECT_EQ(words.size(), GSDevice::kGSTileGpuKeepMaskWordsPerPage);
}

// An empty rect, and a format the transfer engine can name but this road has no geometry for, are
// both refusals rather than empty successes -- an empty success would let a merge be planned with
// no mask at all, which is the writeback overwriting everything.
TEST(GSTileGpuUploadMerge, AnEmptyRectOrAnUnknownFormatIsARefusal)
{
	std::vector<UploadPageBytes> out;
	std::vector<u32> words;
	const GSTileSurfaceLayout ct32 = Layout(0, 1, PSMCT32);
	GSPageBitmap one;
	one.set(0);
	EXPECT_FALSE(GSRendererTileGpu::BuildUploadByteMask(ct32, GSVector4i(4, 4, 4, 4), one, out, words));
	EXPECT_FALSE(GSRendererTileGpu::BuildUploadByteMask(Layout(0, 1, 63), GSVector4i(0, 0, 8, 8), one, out, words));
}

// The model side of the merge: a write whose bytes the GPU has already been given moves no truth at
// all. Calling OnCpuWrite for such a page instead would clear truth the CPU shadow does not hold,
// which is the silent-corruption end of this road.
TEST(GSTileGpuUploadMerge, AWriteServedOnTheGpuMovesNoTruth)
{
	GSVramModel m;
	const GSTileSurfaceId id = m.Create(Layout(0, 1, PSMCT32), GSVector4i(0, 0, 64, 32), 1);
	GSPageBitmap page;
	page.set(0);
	m.OnNativeDraw(id, page, kGSTilePlanesAll);
	ASSERT_TRUE(m.Truth(GSVramModel::PlaneIndex(GSTilePlaneRGB)).test(0));

	const u32 gen_before = m.Gen(0).cpu_write;
	m.OnCpuWriteServedOnGpu(page);

	EXPECT_EQ(m.Gen(0).cpu_write, gen_before + 1);
	EXPECT_TRUE(m.Truth(GSVramModel::PlaneIndex(GSTilePlaneRGB)).test(0));
	EXPECT_EQ(m.OwnerOf(0, GSVramModel::PlaneIndex(GSTilePlaneRGB)), id);
	EXPECT_EQ(m.TruthMask(0, GSVramModel::PlaneIndex(GSTilePlaneRGB)), GSVramModel::kFullBlockMask);
	EXPECT_FALSE(m.SyncedPages(GSVramModel::PlaneIndex(GSTilePlaneRGB)).test(0));
	EXPECT_TRUE(m.CheckInvariants());

	// ...and the other half of the same statement, which is what makes the CPU-read window a
	// heuristic rather than an invariant: a page the merge served is STILL one a later CPU read has
	// to pull. Serving it moved no truth, so it did not move the answer to "what does the shadow not
	// have" either. Nothing about TileGpuMergeCpuReadWindow can hand anybody stale bytes; all it can
	// do is trade one wait now against several later.
	EXPECT_TRUE(m.ReadbackNeeded(page, kGSTilePlanesAll).test(0));
}

// EmuCore/GS/TileGpuMergeCpuReadWindow, over the whole table it has to answer. None of this is
// checkable from a corpus run: three dumps of twenty-one reach the clause at all, so an off-by-one
// that halves Dirge of Cerberus's protection, or a monotone arm that does not reproduce the old
// road, would both go green on the grid and be wrong on a device.
TEST(GSTileGpuUploadMerge, TheCpuReadRefusalIsWindowed)
{
	// A page nothing has ever read is admitted at every window, including the monotone one. This is
	// the population the merge lives on and no setting may take it away.
	for (const int w : {-1, 0, 1, 2, 4, 65535, 1 << 20})
		EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(0, 100, w)) << "window " << w;

	// The window is a count of frames the mark survives: refused at ages 0..w-1, admitted at w.
	// `now` and the mark are both frame + 1, so a mark of `now` is age 0 -- read this frame.
	for (const int w : {1, 2, 4, 16})
	{
		for (u32 age = 0; age < static_cast<u32>(w); age++)
			EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(1000 - age, 1000, w)) << "window " << w << " age " << age;
		for (u32 age = static_cast<u32>(w); age < static_cast<u32>(w) + 4; age++)
			EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(1000 - age, 1000, w)) << "window " << w << " age " << age;
	}

	// The two control arms. 0 (and anything below it) is delete-equivalent: the clause never refuses,
	// however fresh the mark. 65535 and above is session-monotone: it refuses for ever, which is the
	// road that shipped before this key existed and the only arm an A/B against it can be read from.
	for (const u32 age : {0u, 1u, 5u, 1000u, 1u << 20})
	{
		EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(1u << 21, (1u << 21) + age, 0));
		EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(1u << 21, (1u << 21) + age, -1));
		EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(1u << 21, (1u << 21) + age, 65535));
		EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(1u << 21, (1u << 21) + age, 1 << 20));
	}

	// The frame counter wraps at 2^32, and the age survives it EXACTLY: modulo 2^32 the unsigned
	// subtraction is the true frame delta, so a mark stamped 20 frames before the wrap and read 4
	// frames after it reads as age 20 and no earlier. There is no wrap case to get wrong, which is
	// the thing worth pinning -- a signed or clamped spelling would have one.
	EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFF0u, 4, 2));   // age 20, window 2: cold
	EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFF0u, 4, 21));   // age 20, window 21: fresh
	EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFF0u, 4, 20));  // age 20, window 20: the edge
	EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFF0u, 4, 65535));
	// ...and the same either side of it, un-wrapped.
	EXPECT_TRUE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFFEu, 0xFFFFFFFFu, 2));
	EXPECT_FALSE(gsTileGpuMergeRefusesForCpuRead(0xFFFFFFFCu, 0xFFFFFFFFu, 2));
}

// The age histogram the window was chosen from is only worth reading if its buckets are the ones its
// labels claim. A bucket boundary that drifts turns "dirge refuses on fresh marks" into a different
// sentence with the same printout.
TEST(GSTileGpuUploadMerge, TheCpuReadAgeBucketsAreWhatTheCensusPrints)
{
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(0), 0u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(1), 1u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(2), 2u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(3), 2u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(4), 3u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(15), 3u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(16), 4u);
	EXPECT_EQ(gsTileGpuMergeCpuAgeBucket(~0u), 4u);
	// Every age lands in exactly one of the five the census prints.
	for (const u32 age : {0u, 1u, 2u, 3u, 4u, 7u, 15u, 16u, 100u, 1u << 20})
		EXPECT_LT(gsTileGpuMergeCpuAgeBucket(age), kGSTileGpuMergeCpuAgeBuckets);
}
