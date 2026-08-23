// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSTileTargetPool::ReadbackPages — the store road must write inside the pages it was asked for.
//
// A readback brings a surface's pixels back down into CPU local memory. Both of its roads (the
// out-of-band copy and the drain) end in the same two writers: GSLocalMemory::WritePixel32 for a
// colour surface, WritePixel16 for a 16-bit depth one. Those writers walk GSOffset's pixel index,
// and that index counts pixels in the LAYOUT'S OWN format — 16-bit cells for a 16-bit format,
// 32-bit cells for a 32-bit one. WritePixel32 indexes vm32(), so pairing it with a 16-bit layout
// multiplies every destination byte offset by two and writes four bytes where two belong.
//
// That is not a wrong pixel, it is a wrong PAGE. Measured on OutRun 2006 (2026-08-22, TileGpu on
// M2/Asahi): a PSMCT16S surface based at page 380 wrote GS pages 293 and 297 — 87 pages outside
// its own footprint, through the wrapped VM alias so no watchpoint on the primary mapping even
// saw it. Those two pages are the game's PSMT8H fog-index plane; zeroed, they read as fog index 0,
// which is the maximum-haze end of the ramp, and the frame grows sky-lavender stripes across the
// bottom band that spread from 0 to 2 to 7 pages over three frames as the corruption is seeded
// into surfaces and written back out again.
//
// The upload road never had this problem: it goes through GSLocalMemory::ReadTexture, which
// dispatches on the format. Only the readback's store hard-codes the 32-bit writer for colour.
//
// So the pool declares which layouts its store road can address, and these tests hold that
// declaration to the property it stands for, in both directions: nothing it accepts may leave its
// pages, and it may not refuse a layout that would have stayed inside them.

#include "GS/Renderers/Tile/GSTileTargetPool.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <iomanip>
#include <vector>

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

struct Case
{
	const char* name;
	GSTileSurfaceLayout layout;
};

// Every layout family the pool serves (PoolSupports: 16 bpp and up), based far enough into GS
// memory that a doubled byte offset cannot land back inside the surface by luck.
const Case kCases[] = {
	{"PSMCT32", Layout(4800, 10, PSMCT32, GSTileSurfaceKind::Color)},
	{"PSMCT24", Layout(4800, 10, PSMCT24, GSTileSurfaceKind::Color)},
	{"PSMCT16", Layout(4800, 10, PSMCT16, GSTileSurfaceKind::Color)},
	{"PSMCT16S", Layout(12160, 10, PSMCT16S, GSTileSurfaceKind::Color)},
	{"PSMZ32", Layout(4800, 10, PSMZ32, GSTileSurfaceKind::Depth)},
	{"PSMZ24", Layout(4800, 10, PSMZ24, GSTileSurfaceKind::Depth)},
	{"PSMZ16", Layout(4800, 10, PSMZ16, GSTileSurfaceKind::Depth)},
	{"PSMZ16S", Layout(12160, 10, PSMZ16S, GSTileSurfaceKind::Depth)},
};

// The pages a readback of `pages` would actually write, spelled out the way ReadbackPages spells
// it: CollectRuns for the pixel rects, GSOffset::pa for each pixel's index in the layout's own
// units, and the cell width of the writer the store road picks for this surface.
//
// The cell width is the LAYOUT'S storage width, for both kinds. That is the whole property: a
// writer whose cell is wider than the format's multiplies every destination offset, and a writer
// whose cell matches lands inside the pages the runs came from. Colour used to be pinned at four
// bytes here because the colour road only had WritePixel32; it now has the 16-bit pack as well.
GSPageBitmap StoreTouchedPages(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	std::vector<GSVector4i> runs;
	GSTileTargetPool::CollectRuns(layout, pages, GSVramModel::kFullBlockMask, runs);

	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	const u64 cell = gsTileStorageBpp(layout.psm) / 8;

	GSPageBitmap touched;
	for (const GSVector4i& r : runs)
	{
		for (int y = r.y; y < r.w; y++)
		{
			for (int x = r.x; x < r.z; x++)
			{
				const u64 first = static_cast<u64>(off.pa(x, y)) * cell;
				// GS local memory is a wrapped mapping, so an address past 4 MB does not fault --
				// it aliases. Fold it the way the hardware mapping does.
				for (u64 b = first; b < first + cell; b++)
					touched.set(static_cast<u32>((b / GS_PAGE_SIZE) % GS_MAX_PAGES));
			}
		}
	}
	return touched;
}

// A few pages inside the surface, one page-row down from its base so the run set is not the
// degenerate first row.
GSPageBitmap SomePagesOf(const GSTileSurfaceLayout& layout)
{
	const u32 base = layout.bp >> 5;
	GSPageBitmap pages;
	pages.set(base + layout.bw + 3);
	pages.set(base + layout.bw + 7);
	return pages;
}
} // namespace

TEST(TileGpuReadbackStore, AcceptedLayoutsStoreOnlyInsideTheirOwnPages)
{
	for (const Case& c : kCases)
	{
		if (!GSTileTargetPool::ReadbackAddressable(c.layout))
			continue;
		const GSPageBitmap pages = SomePagesOf(c.layout);
		const GSPageBitmap escaped = StoreTouchedPages(c.layout, pages).andnot(pages);
		EXPECT_TRUE(escaped.empty()) << c.name << ": readback store wrote " << escaped.count()
									 << " page(s) it was not asked for";
	}
}

TEST(TileGpuReadbackStore, RefusesExactlyTheLayoutsItCannotAddress)
{
	// Both directions. The first half is the bug; the second stops the repair from degenerating
	// into "refuse everything", which would silently retire the readback road altogether.
	for (const Case& c : kCases)
	{
		const GSPageBitmap pages = SomePagesOf(c.layout);
		const bool stays_inside = StoreTouchedPages(c.layout, pages).andnot(pages).empty();
		EXPECT_EQ(GSTileTargetPool::ReadbackAddressable(c.layout), stays_inside)
			<< c.name << ": the pool's declaration disagrees with where its store road lands";
	}
}

TEST(TileGpuReadbackStore, EverySixteenBitFamilyIsAddressable)
{
	// This was `SixteenBitColourIsTheRefusedCase`: 16-bit colour was the one gap, because the
	// colour road only carried WritePixel32. It now carries the 5551 pack through WriteFrame16, so
	// every family the pool can back has a store, and the refusal is gone rather than merely
	// narrowed. Named on its own so a future widening of the case table cannot quietly lose it.
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT16, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT16S, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMZ16, GSTileSurfaceKind::Depth)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMZ16S, GSTileSurfaceKind::Depth)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT32, GSTileSurfaceKind::Color)));
	EXPECT_TRUE(GSTileTargetPool::ReadbackAddressable(Layout(4800, 10, PSMCT24, GSTileSurfaceKind::Color)));
}

// The 16-bit colour store's own arithmetic, pinned against GSLocalMemory rather than against a
// literal: the pack ReadbackPages hands WriteFrame16 and the unpack the seed shader performs are
// each other's inverse over the five bits the format keeps, and the pack is the software
// renderer's own.
TEST(TileGpuReadbackStore, FiveFiveFiveOnePackAndUnpackAreInverse)
{
	for (u32 c = 0; c < 0x10000u; c++)
	{
		const u16 h = static_cast<u16>(c);
		const u32 expanded = gsTileUnpack5551(h);
		EXPECT_EQ(gsTilePack5551(expanded), h) << "16-bit cell 0x" << std::hex << c;
		// Only the bits the format stores come back: five per colour channel at the top of each
		// byte, and alpha as the one bit, 0x80 or 0x00.
		EXPECT_EQ(expanded & 0x07070700u, 0u) << "16-bit cell 0x" << std::hex << c;
		EXPECT_TRUE((expanded >> 24) == 0x80u || (expanded >> 24) == 0x00u);
	}
}

// The readback road issues one pool call per (owner surface, truth block mask) and hands it the
// UNION of the plane masks that fell into that group, instead of one call per plane. That is
// byte-identical only if the mask's meaning is "select the bytes of the cell to write", which
// is true of WritePixel32 by construction and true of the 16-bit road only because the
// narrowing to the 5551 cell distributes over OR. Pin the distribution over every union of the
// masks PlaneByteMask can produce, under both the full-byte formats and the 24-bit ones (whose
// alpha planes narrow to nothing).
TEST(TileGpuReadbackStore, SixteenBitNarrowingDistributesOverMergedPlaneMasks)
{
	// PlaneByteMask's outputs: RGB, alpha-low, alpha-high, Z -- and the 24-bit variants, where
	// the format's own byte mask clears the alpha planes entirely.
	const u32 plane_masks[] = {0x00FFFFFFu, 0x0F000000u, 0xF0000000u, 0xFFFFFFFFu, 0x00000000u};
	for (u32 a : plane_masks)
	{
		for (u32 b : plane_masks)
		{
			EXPECT_EQ(GSTileTargetPool::NarrowWriteMaskTo16(a | b),
				static_cast<u16>(
					GSTileTargetPool::NarrowWriteMaskTo16(a) | GSTileTargetPool::NarrowWriteMaskTo16(b)))
				<< "masks 0x" << std::hex << a << " | 0x" << b;
			for (u32 c : plane_masks)
			{
				EXPECT_EQ(GSTileTargetPool::NarrowWriteMaskTo16(a | b | c),
					static_cast<u16>(GSTileTargetPool::NarrowWriteMaskTo16(a) |
									 GSTileTargetPool::NarrowWriteMaskTo16(b) |
									 GSTileTargetPool::NarrowWriteMaskTo16(c)))
					<< "masks 0x" << std::hex << a << " | 0x" << b << " | 0x" << c;
			}
		}
	}
	// The three colour planes of one 32-bit owner -- the merge the corpus actually performs --
	// come out as the whole 16-bit cell, colour bits and the alpha bit.
	EXPECT_EQ(GSTileTargetPool::NarrowWriteMaskTo16(0x00FFFFFFu | 0x0F000000u | 0xF0000000u), 0xFFFFu);
	// A group that collected nothing writes nothing; ReadbackPages early-outs on it.
	EXPECT_EQ(GSTileTargetPool::NarrowWriteMaskTo16(0u), 0u);
}

TEST(TileGpuReadbackStore, PackIsGSLocalMemorysOwnFrameWrite)
{
	// gsTilePack5551 is used by the pool's store road and by the writeback shader's CPU
	// specification; WriteFrame16 is what the software renderer stores a 16-bit frame with. A
	// disagreement between them is a renderer that writes different bytes for the same pixel.
	const u32 samples[] = {0x00000000u, 0xFFFFFFFFu, 0x80402010u, 0x7F7F7F7Fu, 0x01020304u,
		0x8000FF00u, 0x00FFFFFFu, 0xFF000000u, 0x12345678u, 0xF8F8F8F8u};
	for (u32 c : samples)
	{
		const u32 rb = c & 0x00f800f8u;
		const u32 ga = c & 0x8000f800u;
		const u16 want = static_cast<u16>((ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3));
		EXPECT_EQ(gsTilePack5551(c), want) << "word 0x" << std::hex << c;
	}
}
