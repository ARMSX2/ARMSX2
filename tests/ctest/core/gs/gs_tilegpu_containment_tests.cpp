// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// When one surface may hold two views of GS memory, and where the second one lands inside it.
//
// gsTileContainView is a legality predicate over two layouts and a footprint, and every clause of
// it exists because breaking it writes the wrong guest pages rather than drawing the wrong thing --
// a contained view's draws land at a rectangle offset and its writeback then addresses the
// container's pages from the container's base, so a wrong row or a wrong column corrupts memory
// some unrelated later draw reads. On both Gran Turismo 4 dumps the containee is a PALETTE buffer,
// which means the symptom of a wrong offset is wrong colours everywhere the palette is used, on
// draws that have nothing to do with the frame that wrote it.
//
// The fixtures are the corpus's own alternations, taken from the draw streams the containment
// design was measured over, so the numbers here are the numbers a folded run has to reproduce:
//
//   gt4     0x03de0 / PSMCT32 / FBW=10 into 0x01180 / PSMCT24 / FBW=10, page delta 355 =
//           row 35 column 5, pixel (320, 1120). Cross-PSM AND offset -- the two rules that
//           remove nothing apart and 87% of the title's colour-key pass breaks together.
//   gt4opb  0x03040 into 0x01a40, both PSMCT32 / FBW=10, page delta 176 = row 17 column 6.
//           Plus the zero-offset case: 0x01a40 carries a PSMCT24 view and a PSMCT32 view of the
//           same 640x448 target, and folding the palette buffer buys nothing unless those two
//           are one surface as well.
//   dirge   0x02a00 / PSMCT16 / FBW=4 against 0x01c00 / PSMCT32 / FBW=8: REFUSED, and refused
//           for a reason no offset rule can repair. The same 8 KB guest page is 64x64 texels in
//           one pixel space and 64x32 in the other, so there is no rectangle of the container
//           that is the view.
//
// ⚠️ The predicate is LEGALITY and it still admits every one of those. What the renderer takes is
// narrower: gsTileContainMayPlaceAt refuses every displaced placement while
// kGSTileContainDisplacedViews is false, so only the same-base merge ships. The offset arithmetic
// below is tested all the same -- it is the road the displaced rung comes back on, and a wrong
// offset there is the failure that writes unrelated guest pages.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

namespace
{
	constexpr GSTileSurfaceLayout ColorAt(u32 bp, u8 bw, u8 psm)
	{
		return GSTileSurfaceLayout{bp, bw, psm, GSTileSurfaceKind::Color};
	}

	constexpr GSTileSurfaceLayout DepthAt(u32 bp, u8 bw, u8 psm)
	{
		return GSTileSurfaceLayout{bp, bw, psm, GSTileSurfaceKind::Depth};
	}

	// The corpus fixtures, by guest block pointer.
	constexpr u32 kGt4Frame = 0x01180; // page 140, PSMCT24, FBW 10, 640x224
	constexpr u32 kGt4Palette = 0x03de0; // page 495, PSMCT32, FBW 10, 65x33
	constexpr u32 kGt4Target = 0x01a40; // page 210, PSMCT32 and PSMCT24 both, FBW 10, 640x448
	constexpr u32 kOpbPalette = 0x03040; // page 386, PSMCT32, FBW 10, 65x33
	constexpr u32 kDirgeFrame = 0x01c00; // page 224, PSMCT32, FBW 8, 512x448
	constexpr u32 kDirgeScratch = 0x02a00; // page 336, PSMCT16, FBW 4, 164x175

	// The palette buffers both dumps alternate with: 65x33 pixels, so two page columns and two page
	// rows of a 32-bit pixel space.
	constexpr u32 kPaletteCols = 2;
	constexpr u32 kPaletteRows = 2;

	GSTileContainRefusal Contain(const GSTileSurfaceLayout& container, const GSTileSurfaceLayout& view, u32 cols,
		u32 rows, GSTileContainOffset& out, u32 budget = 0)
	{
		return gsTileContainView(container, view, cols, rows, budget, out);
	}
} // namespace

// -- the footprint helpers the predicate is fed ------------------------------------------------

TEST(TileGpuContainment, PageFootprintIsMeasuredFromTheSurfaceOrigin)
{
	// A view is as wide as the extent its draws reach, not as wide as one draw: the container has
	// to hold every column and row anything of that view ever touches.
	EXPECT_EQ(gsTileContainPageCols(65, 10), 2u);
	EXPECT_EQ(gsTileContainPageCols(64, 10), 1u);
	EXPECT_EQ(gsTileContainPageCols(640, 10), 10u);
	EXPECT_EQ(gsTileContainPageRows(33, PSMCT32), 2u);
	EXPECT_EQ(gsTileContainPageRows(32, PSMCT32), 1u);
	EXPECT_EQ(gsTileContainPageRows(448, PSMCT32), 14u);
	// 16-bit pages are 64 rows tall where 32-bit pages are 32.
	EXPECT_EQ(gsTileContainPageRows(448, PSMCT16), 7u);
	EXPECT_EQ(gsTileContainPageRows(175, PSMCT16), 3u);
}

TEST(TileGpuContainment, AnEmptyExtentStillOccupiesThePageItsBaseNames)
{
	EXPECT_EQ(gsTileContainPageCols(0, 10), 1u);
	EXPECT_EQ(gsTileContainPageRows(0, PSMCT32), 1u);
	// A view wider than its own stride wraps rather than overflowing, so the column clause is
	// about where it starts and the width is clamped before it gets there.
	EXPECT_EQ(gsTileContainPageCols(4096, 10), 10u);
	// A stride of zero has no page geometry; PoolSupports refuses it upstream and this must not
	// divide by it on the way to being refused.
	EXPECT_EQ(gsTileContainPageCols(640, 0), 1u);
}

// -- the corpus acceptances -----------------------------------------------------------------

TEST(TileGpuContainment, Gt4PaletteFoldsIntoTheFrameBufferAtRow35Column5)
{
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.col, 5u);
	EXPECT_EQ(off.row, 35u);
	EXPECT_EQ(off.x, 320);
	EXPECT_EQ(off.y, 1120);
	// 35 rows in plus the two the view needs: what the container's texture must be tall enough for.
	EXPECT_EQ(off.end_row, 37u);
}

TEST(TileGpuContainment, Gt4opbPaletteFoldsIntoTheTargetAtRow17Column6)
{
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kOpbPalette, 10, PSMCT32), kPaletteCols, kPaletteRows, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.col, 6u);
	EXPECT_EQ(off.row, 17u);
	EXPECT_EQ(off.x, 384);
	EXPECT_EQ(off.y, 544);
	EXPECT_EQ(off.end_row, 19u);
}

TEST(TileGpuContainment, TheZeroOffsetCt24Ct32PairIsOneSurface)
{
	// gt4opb draws 1120 PSMCT24 draws and 104 PSMCT32 draws into 0x01a40. Same base, same stride,
	// same family: identical page geometry and identical intra-page block order, so the container's
	// page->pixel map IS the view's and the offset is (0, 0).
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kGt4Target, 10, PSMCT24), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.col, 0u);
	EXPECT_EQ(off.row, 0u);
	EXPECT_EQ(off.x, 0);
	EXPECT_EQ(off.y, 0);
	EXPECT_EQ(off.end_row, 14u);

	// ...and the other way round, because which of the two appears first is the draw order's call.
	EXPECT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT24), ColorAt(kGt4Target, 10, PSMCT32), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.end_row, 14u);
}

TEST(TileGpuContainment, ACt32ContainerHoldsACt32ViewOfItself)
{
	// The identity case, which the caller reaches whenever a view already has its own surface.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kGt4Target, 10, PSMCT32), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.x, 0);
	EXPECT_EQ(off.y, 0);
}

// -- the refusals ------------------------------------------------------------------------------

TEST(TileGpuContainment, DirgesPairIsRefusedOnTheSwizzleFamily)
{
	// The whole of Dirge of Cerberus's 989.5 colour-key breaks a frame is this one alternation, and
	// containment does not touch it. Not "less than hoped" -- nothing: different family, different
	// stride, different bits per pixel, and the family clause refuses it before the stride one is
	// even asked.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kDirgeFrame, 8, PSMCT32), ColorAt(kDirgeScratch, 4, PSMCT16), 3, 3, off),
		GSTileContainRefusal::Family);
	EXPECT_EQ(Contain(ColorAt(kDirgeScratch, 4, PSMCT16), ColorAt(kDirgeFrame, 8, PSMCT32), 8, 14, off),
		GSTileContainRefusal::Family);
	// And nothing about the offset survives a refusal.
	EXPECT_EQ(off.end_row, 0u);
}

TEST(TileGpuContainment, TheSixteenBitFamiliesAreDistinctFromEachOtherAndFromCt32)
{
	// PSMCT16 and PSMCT16S are different swizzle tables, so a rectangle of one is not a rectangle
	// of the other however identical their page geometry looks.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(0x01000, 8, PSMCT16), ColorAt(0x01000, 8, PSMCT16S), 8, 7, off),
		GSTileContainRefusal::Family);
	EXPECT_EQ(Contain(ColorAt(0x01000, 8, PSMCT16S), ColorAt(0x01000, 8, PSMCT16), 8, 7, off),
		GSTileContainRefusal::Family);
	EXPECT_EQ(Contain(ColorAt(0x01000, 8, PSMCT32), ColorAt(0x01000, 8, PSMCT16), 8, 7, off),
		GSTileContainRefusal::Family);
	// ...and a format with no family at all is refused rather than treated as some default.
	EXPECT_EQ(Contain(ColorAt(0x01000, 8, PSMCT32), ColorAt(0x01000, 8, 0x3F), 8, 7, off),
		GSTileContainRefusal::Family);
}

TEST(TileGpuContainment, ADifferingStrideIsAPageRemapAndNotAnOffset)
{
	// Same family, same alignment, forward delta -- and still refused, because the container's
	// page (row, column) map is built on ITS stride and the view's rows would not follow it.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(0x01000, 8, PSMCT32), ColorAt(0x01400, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Stride);
	EXPECT_EQ(Contain(ColorAt(0x01000, 10, PSMCT32), ColorAt(0x01400, 8, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Stride);
	// A zero-stride container has no page geometry at all.
	EXPECT_EQ(Contain(ColorAt(0x01000, 0, PSMCT32), ColorAt(0x01000, 0, PSMCT32), 1, 1, off),
		GSTileContainRefusal::Stride);
}

TEST(TileGpuContainment, ABlockOffsetBaseIsRefusedOnEitherSide)
{
	// A base that is not a whole number of pages starts mid-page, where the intra-page swizzle
	// makes a rectangle view unexpressible. The same standing condition every road that treats a
	// pool texture as the guest's pixel space already carries.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Frame + 8, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Alignment);
	EXPECT_EQ(Contain(ColorAt(kGt4Frame + 8, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Alignment);
	// One block short of the next page is still mid-page.
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette - 1, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Alignment);
}

TEST(TileGpuContainment, AViewBeforeItsContainerIsRefused)
{
	// Every road that reads a container computes `block - base` unsigned, so a negative delta is
	// not a small offset, it is an enormous one.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(kGt4Palette, 10, PSMCT32), ColorAt(kGt4Frame, 10, PSMCT24), 10, 7, off),
		GSTileContainRefusal::Delta);
	// One page short is still short.
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Frame - 32, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Delta);
}

TEST(TileGpuContainment, AViewThatRunsOffTheContainersStrideIsRefused)
{
	// Column 9 of a 10-page stride with a two-page-wide view: the view's right-hand column would
	// wrap into the container's NEXT page row, and a rectangle offset cannot express that. This is
	// the clause whose failure writes the last page column of every row to the wrong guest page.
	GSTileContainOffset off;
	const u32 col9 = kGt4Frame + 9 * 32; // nine pages past the container's base
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(col9, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Column);
	// ...and one column earlier it fits exactly, which is what says the bound is `<=` and not `<`.
	const u32 col8 = kGt4Frame + 8 * 32;
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(col8, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.col, 8u);
	EXPECT_EQ(off.row, 0u);
	// A full-width view is legal at column 0 and at no other column.
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Frame, 10, PSMCT32), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Frame + 32, 10, PSMCT32), 10, 14, off),
		GSTileContainRefusal::Column);
}

TEST(TileGpuContainment, AContainerMayNotGrowPastTheEndOfGsMemory)
{
	// GS memory is 512 pages and the container's own base is somewhere inside it, so the bound is
	// absolute and not relative: a container at page 490 with a stride of 8 has two page rows of
	// room, whatever the budget says.
	GSTileContainOffset off;
	const u32 base = 490 * 32;
	EXPECT_EQ(Contain(ColorAt(base, 8, PSMCT32), ColorAt(base + 8 * 32, 8, PSMCT32), 1, 2, off),
		GSTileContainRefusal::Wrap);
	// One page row less and it fits: 490 + 3*8 = 514 is past the end, 490 + 2*8 = 506 is not.
	EXPECT_EQ(Contain(ColorAt(base, 8, PSMCT32), ColorAt(base + 8 * 32, 8, PSMCT32), 1, 1, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.row, 1u);
	EXPECT_EQ(off.end_row, 2u);
	// The corpus's biggest real fold clears it, and only just: page 140 plus 37 rows of 10 is 510.
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(kGt4Frame / 32 + off.end_row * 10, 510u);
}

TEST(TileGpuContainment, ThePageBudgetRefusesAGrowthTheGeometryWouldAllow)
{
	// The budget is what stops a container becoming mostly hole. GT4's fold needs 370 pages of
	// container to reach a 65x33 buffer 35 page rows in.
	GSTileContainOffset off;
	const auto gt4 = [&off](u32 budget) {
		return Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows, off,
			budget);
	};
	EXPECT_EQ(gt4(100), GSTileContainRefusal::Extent);
	EXPECT_EQ(gt4(369), GSTileContainRefusal::Extent);
	EXPECT_EQ(gt4(370), GSTileContainRefusal::Admitted);
	// Zero is the whole of GS memory, and so is anything past it -- the key cannot ask for more
	// room than exists.
	EXPECT_EQ(gt4(0), GSTileContainRefusal::Admitted);
	EXPECT_EQ(gt4(GS_MAX_PAGES), GSTileContainRefusal::Admitted);
	EXPECT_EQ(gt4(GS_MAX_PAGES * 4), GSTileContainRefusal::Admitted);
	EXPECT_EQ(gsTileGpuContainPageBudget(0), GS_MAX_PAGES);
	EXPECT_EQ(gsTileGpuContainPageBudget(-1), GS_MAX_PAGES);
	EXPECT_EQ(gsTileGpuContainPageBudget(100000), GS_MAX_PAGES);
	EXPECT_EQ(gsTileGpuContainPageBudget(64), 64u);
}

TEST(TileGpuContainment, DepthIsRefusedOnEitherSide)
{
	// Colour and depth surfaces are different attachment types and different pool formats, and
	// the depth half of the pass key is worth about one break a frame across the whole corpus --
	// so it is deferred rather than served by the same predicate with the kind clause relaxed.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(DepthAt(kGt4Frame, 10, PSMZ32), DepthAt(kGt4Palette, 10, PSMZ32), 2, 2, off),
		GSTileContainRefusal::Kind);
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT32), DepthAt(kGt4Palette, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Kind);
	EXPECT_EQ(Contain(DepthAt(kGt4Frame, 10, PSMCT32), ColorAt(kGt4Palette, 10, PSMCT32), 2, 2, off),
		GSTileContainRefusal::Kind);
}

// -- what the two halves buy apart ---------------------------------------------------------------

TEST(TileGpuContainment, CrossPsmAndOffsetAreOneFeatureNotTwo)
{
	// Measured over the corpus draw streams: offset containment with PSM equality still required
	// removes nothing on either GT4 dump, and cross-PSM identity at a zero offset removes nothing
	// either. This pins that the predicate admits BOTH at once, since a version that admitted only
	// one would pass every clause test above and buy nothing at all.
	GSTileContainOffset off;
	// Cross-PSM and offset together: the gt4 fold.
	EXPECT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows, off),
		GSTileContainRefusal::Admitted);
	EXPECT_NE(off.x, 0);
	EXPECT_NE(off.y, 0);
	// Cross-PSM at a zero offset: the gt4opb same-base pair.
	EXPECT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kGt4Target, 10, PSMCT24), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.x, 0);
	EXPECT_EQ(off.y, 0);
	// Same PSM at an offset: the yugioh fold, 21 page rows on with no column offset.
	EXPECT_EQ(Contain(ColorAt(0x008c0, 10, PSMCT16S), ColorAt(0x02300, 10, PSMCT16S), 10, 7, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.col, 0u);
	EXPECT_EQ(off.row, 21u);
	// 16-bit pages are 64 tall, so the same page row is twice the pixels a 32-bit one would be.
	EXPECT_EQ(off.y, 21 * 64);
}

TEST(TileGpuContainment, ThePixelOffsetTakesTheContainersPageHeight)
{
	// Rule 2 makes the container's page geometry the view's, which is why the y offset may be
	// computed from either -- but only the container's is correct if that ever stops being true,
	// and a 16-bit container is where the difference shows.
	GSTileContainOffset off;
	EXPECT_EQ(Contain(ColorAt(0x00000, 4, PSMCT16), ColorAt(0x00000 + 4 * 32, 4, PSMCT16), 4, 1, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.row, 1u);
	EXPECT_EQ(off.y, 64);
	EXPECT_EQ(Contain(ColorAt(0x00000, 4, PSMCT32), ColorAt(0x00000 + 4 * 32, 4, PSMCT32), 4, 1, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.row, 1u);
	EXPECT_EQ(off.y, 32);
}

// -- the alias table ---------------------------------------------------------------------------

TEST(TileGpuContainment, AContainedViewDefaultsToNoSurfaceAtTheOrigin)
{
	// The default has to be inert: a lookup that misses must not read as "surface 0 at (0, 0)".
	const GSTileContainedView fresh;
	EXPECT_EQ(fresh.id, kGSTileNoSurface);
	EXPECT_EQ(fresh.x_off, 0);
	EXPECT_EQ(fresh.y_off, 0);

	GSTileViewAliasTable table;
	const GSTileSurfaceLayout view = ColorAt(kGt4Palette, 10, PSMCT32);
	EXPECT_EQ(table.find(view.pack()), table.end());
	table[view.pack()] = GSTileContainedView{7, 320, 1120};
	// The key is the packed layout, which is injective over (bp, bw, psm, kind) -- so a different
	// PSM at the same base is a different view and gets its own entry.
	const GSTileSurfaceLayout other = ColorAt(kGt4Palette, 10, PSMCT24);
	EXPECT_NE(other.pack(), view.pack());
	EXPECT_EQ(table.find(other.pack()), table.end());
	ASSERT_NE(table.find(view.pack()), table.end());
	EXPECT_EQ(table[view.pack()].id, 7);
	EXPECT_EQ(table[view.pack()].x_off, 320);
	EXPECT_EQ(table[view.pack()].y_off, 1120);
}

// -- the offset geometry: what a contained view's draws are actually moved by --------------------
//
// Risks R-1 and R-2 of the design's register live entirely in this arithmetic. R-1 is a page delta
// divided by the wrong stride or multiplied by the wrong page height, and it does not draw the view
// in the wrong place so much as write the WRONG GUEST PAGES -- the writeback addresses the
// container's pages from the container's base, so a view one row out corrupts memory some unrelated
// later draw reads. R-2 is a column offset that wraps: the right-hand pages of every row land in
// the next page row. Both fixtures below are palette buffers, so either failure surfaces as wrong
// colours everywhere the palette is used and nowhere near the frame that caused it.

TEST(TileGpuContainment, ThePixelOffsetRoundTripsToThePageDelta)
{
	// The single strongest guard against R-1, stated as the invariant it really is: an admitted
	// placement's pixel offset, read back through the container's own page geometry, must be the
	// page delta it came from. A wrong stride divide, a wrong page height or a swapped row/column
	// all break this and nothing else has to be enumerated.
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S};
	for (const u32 psm : kPsms)
	{
		const u32 page_h = gsTilePageHeight(psm);
		for (u32 bw = 1; bw <= 12; bw++)
		{
			for (u32 delta = 0; delta < 64; delta++)
			{
				GSTileContainOffset off;
				const GSTileSurfaceLayout container = ColorAt(0, static_cast<u8>(bw), static_cast<u8>(psm));
				const GSTileSurfaceLayout view = ColorAt(delta * 32, static_cast<u8>(bw), static_cast<u8>(psm));
				if (Contain(container, view, 1, 1, off) != GSTileContainRefusal::Admitted)
					continue;
				ASSERT_EQ(off.x % 64, 0) << "psm " << psm << " bw " << bw << " delta " << delta;
				ASSERT_EQ(off.y % static_cast<s32>(page_h), 0) << "psm " << psm << " bw " << bw << " delta " << delta;
				const u32 back = static_cast<u32>(off.y) / page_h * bw + static_cast<u32>(off.x) / 64;
				ASSERT_EQ(back, delta) << "psm " << psm << " bw " << bw << " delta " << delta;
			}
		}
	}
}

TEST(TileGpuContainment, AnAdmittedViewNeverReachesPastTheContainersStride)
{
	// R-2 as an invariant rather than as a case list: whatever the predicate admits, the view's
	// right edge is inside the container's own width, so no row of it wraps into the next page row.
	for (u32 bw = 1; bw <= 12; bw++)
	{
		for (u32 delta = 0; delta < 64; delta++)
		{
			for (u32 cols = 1; cols <= bw; cols++)
			{
				GSTileContainOffset off;
				const GSTileSurfaceLayout container = ColorAt(0, static_cast<u8>(bw), PSMCT32);
				const GSTileSurfaceLayout view = ColorAt(delta * 32, static_cast<u8>(bw), PSMCT32);
				if (Contain(container, view, cols, 1, off) != GSTileContainRefusal::Admitted)
					continue;
				ASSERT_LE(off.x + static_cast<s32>(cols * 64), static_cast<s32>(bw * 64))
					<< "bw " << bw << " delta " << delta << " cols " << cols;
			}
		}
	}
}

TEST(TileGpuContainment, TheVertexOffsetMovesTheDrawTheWayTheRectDoes)
{
	// XYOFFSET is SUBTRACTED by the vertex transform, and it is 4.4 fixed point where the pixel
	// offset is not. Both halves of that are easy to get backwards, and either way round the draw
	// lands somewhere plausible -- a sixteenth of the distance, or the same distance the wrong way.
	EXPECT_EQ(gsTileContainVertexOffset(0, 0), 0);
	EXPECT_EQ(gsTileContainVertexOffset(2048, 0), 2048); // an uncontained view is untouched
	// The guest's own convention: XYOFFSET 2048 (128 px) is the usual centred window. A view at
	// pixel (320, 1120) of its container needs 320 and 1120 pixels taken OFF that.
	EXPECT_EQ(gsTileContainVertexOffset(2048, 320), 2048 - 320 * 16);
	EXPECT_EQ(gsTileContainVertexOffset(2048, 1120), 2048 - 1120 * 16);
	// ...which is what makes guest pixel p land at container pixel p + off: the transform computes
	// (vertex - xyoffset), and the vertex of guest pixel p is (p * 16 + xyoffset).
	for (const s32 pixels : {0, 1, 320, 384, 544, 1120})
	{
		for (const s32 p : {0, 7, 64, 447})
		{
			const s32 vertex = p * 16 + 2048;
			EXPECT_EQ((vertex - gsTileContainVertexOffset(2048, pixels)) / 16, p + pixels);
		}
	}
}

TEST(TileGpuContainment, TheGt4FixturesDrawWhereTheirPagesAre)
{
	// The two corpus folds end to end: from the layouts, through the predicate, to the numbers a
	// draw of the view actually carries. A 65x33 palette buffer drawn at its own origin under the
	// usual centred XYOFFSET.
	constexpr s32 kCentred = 2048; // 128 px, the guest's window origin
	const GSVector4i draw(0, 0, 65, 33);

	GSTileContainOffset off;
	ASSERT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows,
				  off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(gsTileContainVertexOffset(kCentred, off.x), kCentred - 320 * 16);
	EXPECT_EQ(gsTileContainVertexOffset(kCentred, off.y), kCentred - 1120 * 16);
	EXPECT_TRUE((draw + GSVector4i(off.x, off.y, off.x, off.y)).eq(GSVector4i(320, 1120, 385, 1153)));
	// ...and the bottom of that rect is what the framebuffer-height backstop measures the pass's
	// attachment against, so the container has to be at least this tall. 37 page rows of 32.
	EXPECT_EQ(static_cast<s32>(off.end_row) * 32, 1184);
	EXPECT_LE(1153, static_cast<s32>(off.end_row) * 32);

	ASSERT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kOpbPalette, 10, PSMCT32), kPaletteCols, kPaletteRows,
				  off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(gsTileContainVertexOffset(kCentred, off.x), kCentred - 384 * 16);
	EXPECT_EQ(gsTileContainVertexOffset(kCentred, off.y), kCentred - 544 * 16);
	EXPECT_TRUE((draw + GSVector4i(off.x, off.y, off.x, off.y)).eq(GSVector4i(384, 544, 449, 577)));
	EXPECT_EQ(static_cast<s32>(off.end_row) * 32, 608);
	EXPECT_LE(577, static_cast<s32>(off.end_row) * 32);

	// The scissor rides the same translation, and it has to: it is the hardware scissor, so an
	// untranslated one would reject every fragment of a view whose container placed it outside it.
	const GSVector4i scissor(0, 0, 64, 32);
	EXPECT_TRUE((scissor + GSVector4i(off.x, off.y, off.x, off.y)).eq(GSVector4i(384, 544, 448, 576)));
}

TEST(TileGpuContainment, AZeroOffsetTranslatesNothing)
{
	// The shipped arm: every offset is (0, 0) -- with the lever off because no view is folded at
	// all, and with it on because only the same-base merge is admitted -- so every one of these
	// expressions runs and changes nothing. That is what makes the corpus byte-identical rather
	// than merely unmeasurably different.
	const GSVector4i draw(3, 5, 641, 449);
	EXPECT_TRUE((draw + GSVector4i(0, 0, 0, 0)).eq(draw));
	EXPECT_EQ(gsTileContainVertexOffset(2048, 0), 2048);
	EXPECT_EQ(gsTileContainVertexOffset(-1, 0), -1);
}

// -- what the renderer actually takes: the same-base merge and nothing else ----------------------

TEST(TileGpuContainment, ADisplacedPlacementIsRefusedAdmissionHoweverLegalItIs)
{
	// The shipped rule. Displacement breaks five corpus titles by two measured mechanisms -- the
	// writeback carries the CONTAINER's byte mask, and a displaced view's texture reads stop
	// matching the container so they fall off rule 2 and the donor road onto the byte road -- and
	// neither has a fix in the tree yet. So the placement takes the zero-offset merge only.
	ASSERT_FALSE(kGSTileContainDisplacedViews);

	// gt4opb's fold B, the displaced palette buffer: legal, admitted by the predicate, refused by
	// the placement. Refused for BOTH kinds of view -- the standing rule does not care whether the
	// caller vetoed this one (a depth-carrying draw, a display base) or not.
	GSTileContainOffset off;
	ASSERT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kOpbPalette, 10, PSMCT32), kPaletteCols, kPaletteRows,
				  off),
		GSTileContainRefusal::Admitted);
	EXPECT_NE(off.x | off.y, 0);
	EXPECT_FALSE(gsTileContainMayPlaceAt(off.x, off.y, false));
	EXPECT_FALSE(gsTileContainMayPlaceAt(off.x, off.y, true));

	// gt4's fold, displaced in the other direction (row offset, column offset, and a container it
	// has to grow): same answer.
	ASSERT_EQ(Contain(ColorAt(kGt4Frame, 10, PSMCT24), ColorAt(kGt4Palette, 10, PSMCT32), kPaletteCols, kPaletteRows,
				  off),
		GSTileContainRefusal::Admitted);
	EXPECT_FALSE(gsTileContainMayPlaceAt(off.x, off.y, false));

	// A row offset alone and a column offset alone are both displacements. Neither half is the safe
	// half: the column-only arm was measured and it breaks four of the five titles on its own.
	EXPECT_FALSE(gsTileContainMayPlaceAt(64, 0, false));
	EXPECT_FALSE(gsTileContainMayPlaceAt(0, 32, false));

	// ...and the merge it keeps: gt4opb's 0x01a40 seen as PSMCT24 and as PSMCT32 is one surface at
	// (0, 0), which is the placement worth -117.00 render passes a drawn frame on that dump.
	ASSERT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kGt4Target, 10, PSMCT24), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.x, 0);
	EXPECT_EQ(off.y, 0);
	EXPECT_TRUE(gsTileContainMayPlaceAt(off.x, off.y, false));
	// A zero offset moves nothing, so even a view the caller holds to zero keeps it. That is what
	// makes rule 9 and the depth veto cost nothing, and it is the same reason the standing rule can
	// refuse displacement without refusing containment.
	EXPECT_TRUE(gsTileContainMayPlaceAt(off.x, off.y, true));
}

// -- rule 9: the display base is never displaced -------------------------------------------------

TEST(TileGpuContainment, ADisplayBaseIsRefusedADisplacedPlacement)
{
	// GetOutput hands the frontend a texture and a y_offset with no column offset beside it, and
	// MaterialiseDisplayBuffers' seed draw carries the display rect untranslated, so a displaced
	// display view could be neither presented nor seeded. Rule 9 holds it to a zero offset -- which
	// still admits the cross-PSM merge at the same base, where nothing moves.
	const std::array<u32, 2> both{kGt4Target, kGt4Frame};
	EXPECT_TRUE(gsTileContainIsDisplayBase(kGt4Target, both));
	EXPECT_TRUE(gsTileContainIsDisplayBase(kGt4Frame, both));
	EXPECT_FALSE(gsTileContainIsDisplayBase(kGt4Palette, both));

	// A circuit that is off names no base, and no view can collide with the sentinel: a base is
	// five bits of BP shifted up and cannot reach the top of the word.
	const std::array<u32, 2> one{kGt4Target, kGSTileNoDisplayBase};
	EXPECT_TRUE(gsTileContainIsDisplayBase(kGt4Target, one));
	EXPECT_FALSE(gsTileContainIsDisplayBase(kGt4Frame, one));
	EXPECT_FALSE(gsTileContainIsDisplayBase(0, {kGSTileNoDisplayBase, kGSTileNoDisplayBase}));
	EXPECT_LT(GS_MAX_BLOCKS, kGSTileNoDisplayBase);

	// Asked of the BASE, not the layout: the PCRTC scans a base out under its own FBW and PSM, and
	// a view sharing only the base is the same bytes seen differently. gt4opb's 0x01a40 carries a
	// PSMCT24 view and a PSMCT32 view, and if either is scanned out both are refused -- which is
	// why this takes a base rather than a packed layout.
	EXPECT_NE(ColorAt(kGt4Target, 10, PSMCT24).pack(), ColorAt(kGt4Target, 10, PSMCT32).pack());
	EXPECT_TRUE(gsTileContainIsDisplayBase(ColorAt(kGt4Target, 10, PSMCT24).bp, both));
	EXPECT_TRUE(gsTileContainIsDisplayBase(ColorAt(kGt4Target, 10, PSMCT32).bp, both));

	// The refusal is the CALLER's and not the predicate's: the geometry stays perfectly legal, so
	// nothing in the predicate would have stopped the placement -- which is why the caller has to.
	GSTileContainOffset off;
	ASSERT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kOpbPalette, 10, PSMCT32), kPaletteCols, kPaletteRows,
				  off),
		GSTileContainRefusal::Admitted);
	EXPECT_NE(off.x | off.y, 0); // ...and it displaces, which is the part rule 9 refuses

	// The merge it keeps: 0x01a40's CT24 and CT32 views are one surface at (0, 0), so even if the
	// PCRTC is scanning that base out, nothing about the view moves and the fold stands.
	ASSERT_EQ(Contain(ColorAt(kGt4Target, 10, PSMCT32), ColorAt(kGt4Target, 10, PSMCT24), 10, 14, off),
		GSTileContainRefusal::Admitted);
	EXPECT_EQ(off.x, 0);
	EXPECT_EQ(off.y, 0);
	EXPECT_TRUE(gsTileContainIsDisplayBase(kGt4Target, both));
}
