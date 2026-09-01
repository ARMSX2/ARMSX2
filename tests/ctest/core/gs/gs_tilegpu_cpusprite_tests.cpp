// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu's CPU rasterization road: the admission test that decides whether a draw is executed by
// the software scanline core into guest memory instead of by the GPU.
//
// The road exists for one population and it is named by the game database, not by us.
// `cpuSpriteRenderBW` / `cpuSpriteRenderLevel` are Classic's fixes, and on Spider-Man 3 (bw 2,
// level 2) they divert 3,143 draws a frame onto three 64x64 scratch render targets. Classic runs
// all of them on the CPU and none on the GPU; TileGpu ran all of them on the GPU, which cost 33.0%
// of its serialized GPU frame in draws plus another 4.3% in the writeback those draws forced, and
// got the sun glow wrong on top of it -- the exact defect the database entry's own comment names.
//
// What is pinned here:
//
//  - The SHAPE gates are Classic's gates, in Classic's order, with Classic's boundaries. The fix
//    was tuned against those boundaries. A road that diverts a different population than the
//    entry's author measured is not the fix the entry asks for, whatever it does to a frame time.
//
//  - `bw` counts SIXTY-FOUR PIXEL UNITS, so Spider-Man 3's `2` is a target at most 128 pixels
//    wide, and the FBW=32 escape (Spider-Man 2 writes a 32 there for no reason anyone has
//    recovered) is a named exception rather than a range.
//
//  - LEVEL 2 IS WHAT ADMITS A BLENDED DRAW. That is the whole difference between a sun and no sun
//    on Spider-Man 3, so a refactor that quietly folds level 1 and level 2 together, or that reads
//    `level >= 2` as `level != 0`, has to fail here.
//
//  - DEPTH IS A HARD REFUSAL and it is tested before the cheap shape questions, because the road
//    writes colour bytes into GSLocalMemory and has no depth attachment to keep consistent with.
//
//  - The two reasons this function CANNOT answer -- a page whose newest bytes are on the GPU, and
//    a palette the device holds that the CPU has not synced -- are still part of the refusal
//    enumeration, because the census counts them beside the shape reasons and the renderer is
//    where they are decided. Declining is always safe: the draw takes the ordinary GPU road. The
//    one thing the road may never do is stall to make itself admissible.

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

namespace
{
using R = GSTileCpuRasterRefusal;

// Spider-Man 3's entry, and the draw shape its population has: a colour-writing, depth-free,
// FBW=1 (64 pixel wide) textured sprite.
constexpr u32 kSm3Bw = 2, kSm3Level = 2;

constexpr R Sm3(u32 fbw, bool colour = true, bool depth = false, bool sprite_tex = true,
	bool mipmap = false, bool opaque = true, bool shuffle = false)
{
	return gsTileGpuPlanCpuRaster(kSm3Bw, kSm3Level, colour, depth, sprite_tex, fbw, mipmap, opaque, shuffle);
}
} // namespace

// -- the master enable ---------------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, NoGameDbEntryIsNoRoad)
{
	// bw 0 is "this title has no entry", which is every title but a named handful. The road must
	// be structurally absent for them: not merely empty, but refused before any other question is
	// asked, so a title without the fix can never be moved by a bug in one of the later gates.
	for (u32 level = 0; level <= 2; level++)
	{
		EXPECT_EQ(gsTileGpuPlanCpuRaster(0, level, true, false, true, 1, false, true, false), R::Disabled);
		EXPECT_EQ(gsTileGpuPlanCpuRaster(0, level, true, false, false, 64, true, false, true), R::Disabled);
	}
}

// -- the population Spider-Man 3's entry names ---------------------------------------------------

TEST(GSTileGpuCpuSprite, Sm3ScratchTargetIsAdmitted)
{
	// FBW=1: the three 64x64 scratch targets the whole lever exists for.
	EXPECT_EQ(Sm3(1), R::None);
	// FBW=2 is the widest the entry allows: 128 pixels.
	EXPECT_EQ(Sm3(2), R::None);
}

TEST(GSTileGpuCpuSprite, Sm3SceneTargetIsRefused)
{
	// The 640x448 scene target is FBW=10, and it is 2,114 draws a frame. Diverting it would put
	// the whole frame on the CPU scanline core.
	EXPECT_EQ(Sm3(10), R::FrameWidth);
	EXPECT_EQ(Sm3(3), R::FrameWidth);
}

TEST(GSTileGpuCpuSprite, SpiderMan2FbwThirtyTwoEscape)
{
	// Spider-Man 2 writes FBW=32 on targets that are not 2048 pixels wide. Classic carries the
	// escape as a named exception, not as a range: 31 and 33 are still refused.
	EXPECT_EQ(Sm3(32), R::None);
	EXPECT_EQ(Sm3(31), R::FrameWidth);
	EXPECT_EQ(Sm3(33), R::FrameWidth);
}

// -- depth --------------------------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, AnyDepthUseRefuses)
{
	// Whatever else is true. The road has no depth attachment.
	EXPECT_EQ(Sm3(1, /*colour=*/true, /*depth=*/true), R::Depth);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(kSm3Bw, kSm3Level, true, true, true, 1, false, true, false), R::Depth);
	// ...and it is asked before the shape questions, so a depth-using draw that would also have
	// failed on width reports depth.
	EXPECT_EQ(Sm3(10, /*colour=*/true, /*depth=*/true), R::Depth);
}

TEST(GSTileGpuCpuSprite, NoColourWriteRefuses)
{
	// A draw that lands no colour byte has nothing for this road to write. Classic's `no_rt`.
	EXPECT_EQ(Sm3(1, /*colour=*/false), R::NoColour);
	EXPECT_EQ(Sm3(1, /*colour=*/false, /*depth=*/true), R::NoColour);
}

// -- what each level buys ------------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, LevelZeroTakesOpaqueUnmipmappedTexturedSpritesOnly)
{
	constexpr u32 bw = 2;
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 0, true, false, true, 1, false, true, false), R::None);
	// not a textured sprite
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 0, true, false, false, 1, false, true, false), R::NotSpriteTex);
	// mipmapped
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 0, true, false, true, 1, true, true, false), R::Blended);
	// blended
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 0, true, false, true, 1, false, false, false), R::Blended);
}

TEST(GSTileGpuCpuSprite, LevelOneDropsTheSpriteRequirementOnly)
{
	constexpr u32 bw = 2;
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 1, true, false, false, 1, false, true, false), R::None);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 1, true, false, true, 1, true, true, false), R::Blended);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 1, true, false, true, 1, false, false, false), R::Blended);
}

TEST(GSTileGpuCpuSprite, LevelTwoAdmitsTheBlendedDrawTheSunIs)
{
	// ⚠️ This is the test the whole entry hangs on. Spider-Man 3's `cpuSpriteRenderLevel: 2` is
	// commented "Fixes the sun when using the above" in the game database, and the sun glow is a
	// BLENDED draw: level 2 is the only value that lets it onto the CPU road at all.
	constexpr u32 bw = 2;
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 2, true, false, true, 1, false, /*opaque=*/false, false), R::None);
	// ...and mipmapped, which level 2 also drops.
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 2, true, false, true, 1, /*mipmap=*/true, true, false), R::None);
	// ...and both at once, and untextured with it.
	EXPECT_EQ(gsTileGpuPlanCpuRaster(bw, 2, true, false, false, 1, true, false, false), R::None);
}

// -- the shuffle -----------------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, MidShuffleRefuses)
{
	EXPECT_EQ(Sm3(1, true, false, true, false, true, /*shuffle=*/true), R::Shuffle);
}

// -- ordering -------------------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, GateOrderIsClassics)
{
	// The reason reported for a draw failing several gates is the FIRST one in Classic's order.
	// The census reads these, and a reason that moves when an unrelated gate is added makes two
	// device runs incomparable.
	//
	// bw 0 outranks everything.
	EXPECT_EQ(gsTileGpuPlanCpuRaster(0, 0, false, true, false, 99, true, false, true), R::Disabled);
	// colour, then depth, then the sprite/texture question, then width, then the blend, then the
	// shuffle.
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 0, false, true, false, 99, true, false, true), R::NoColour);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 0, true, true, false, 99, true, false, true), R::Depth);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 0, true, false, false, 99, true, false, true), R::NotSpriteTex);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 0, true, false, true, 99, true, false, true), R::FrameWidth);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 0, true, false, true, 1, true, false, true), R::Blended);
	EXPECT_EQ(gsTileGpuPlanCpuRaster(2, 2, true, false, true, 1, true, false, true), R::Shuffle);
}

// -- the road's own invariant ------------------------------------------------------------------

TEST(GSTileGpuCpuSprite, RefusalIsTotalOverTheShapeSpace)
{
	// Every combination either admits or names a reason, and None is returned only where every
	// gate passed. A sweep rather than a table: the enumeration is what the census counts, and a
	// gate that returns None on a shape it meant to refuse is a silently wrong pixel.
	for (u32 bw = 0; bw <= 3; bw++)
	{
		for (u32 level = 0; level <= 2; level++)
		{
			for (u32 fbw = 0; fbw <= 33; fbw++)
			{
				for (int bits = 0; bits < 32; bits++)
				{
					const bool colour = (bits & 1) != 0;
					const bool depth = (bits & 2) != 0;
					const bool sprite_tex = (bits & 4) != 0;
					const bool mipmap = (bits & 8) != 0;
					const bool opaque = (bits & 16) != 0;
					const R r = gsTileGpuPlanCpuRaster(bw, level, colour, depth, sprite_tex, fbw,
						mipmap, opaque, false);
					const bool shape_ok = bw != 0 && colour && !depth &&
										  (level != 0 || sprite_tex) && (fbw <= bw || fbw == 32) &&
										  (level >= 2 || (!mipmap && opaque));
					EXPECT_EQ(r == R::None, shape_ok)
						<< "bw " << bw << " level " << level << " fbw " << fbw << " bits " << bits;
					// The model reasons are never produced here -- they are the renderer's.
					EXPECT_NE(r, R::GpuTruth);
					EXPECT_NE(r, R::ClutStale);
				}
			}
		}
	}
}
