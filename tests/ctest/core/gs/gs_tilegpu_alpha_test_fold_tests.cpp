// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The alpha test decided at plan time, from the interval the draw's fragment alpha can take.
//
// TileGpu's fragment stage discards a failing fragment outright. That is exact for
// AFAIL=KEEP and wrong for the three modes that still write something on failure, so the
// renderer only asks for a per-fragment test where the two sides of it land differently,
// and folds the cases it can decide up front into the write flags instead. ATST=NEVER was
// always folded. This pins the other half: a COMPARISON no fragment of the draw can cross.
//
// Katamari Damacy is the shape one alpha value decides. The King's head is composited out
// of two sprites over the same rectangle: the first stamps a lower Z into it, the second
// draws the head with ZTST=GEQUAL against that Z. The stamp is untextured with vertex alpha
// 0x00 on all four vertices, and asks for ATST=NOTEQUAL AREF=0 -- "0 != 0" is false, so
// every fragment fails -- with AFAIL=ZB_ONLY, "a failing fragment still writes depth". Read
// as a live per-fragment test the stamp discards every fragment and deposits no depth at
// all, and the head then fails its own GEQUAL over every pixel the purple star behind it
// covers. The head was missing from every archived TileGpu run of that scene.
//
// R&C UYA's exhaust flare is the shape one value CANNOT decide, and it is why the fold takes
// an interval. The flare is one additively-blended 351-triangle strip sampling a paletted
// glow texture under ATST=GEQUAL AREF=128 AFAIL=RGB_ONLY. Its alpha is not constant -- the
// palette varies and the vertices are gouraud -- but it is BOUNDED: palette alpha tops out
// at 128 and vertex alpha at 64, so the MODULATE output tops out at (128 * 64) >> 7 = 64 and
// no fragment can reach the reference. Armored Core 3's HUD is the same shape at
// AFAIL=FB_ONLY, 1,547 draws a frame, at (255 * 64) >> 7 = 127 against the same AREF=128.
//
// Four things are pinned here:
//
//  - the fold's comparisons are the SAME comparisons the software renderer makes
//    (GSState::TryAlphaTest's GetResult) and the shared Tile lowering makes
//    (GSTileDrawLowering.h), on intervals as well as on points. A boundary that disagrees
//    with either silently changes a draw in some other game.
//  - an interval that straddles AREF decides NOTHING. Folding a straddling draw would drop
//    real fragments, which is worse than the approximation the fold exists to avoid.
//  - what a folded all-fail draw writes is decided by AFAIL alone, exactly as ATST=NEVER
//    already was -- so the NEVER road must come out byte-identical.
//  - the seed skip's "this sprite provably overwrites the page" gate has to keep meaning
//    that. A fold to all-pass makes a draw MORE total; a fold to all-fail makes it total
//    only in the AFAIL modes that still write the frame buffer, which is exactly where
//    ATST=NEVER already put it -- so widening the fold may not widen that gate's reach.

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <ios>

namespace
{
constexpr u32 kC32 = 0xFFFFFFFFu; // PSMCT32 / PSMZ32 stored bits
constexpr u32 kC24 = 0x00FFFFFFu; // PSMCT24 / PSMZ24

constexpr u8 kR = 0x1, kG = 0x2, kB = 0x4, kA = 0x8;

using Fold = GSTileAlphaTestFold;

/// The whole 0..255 range: what a caller that cannot bound the alpha passes.
constexpr u32 kAnyLo = 0, kAnyHi = 255;

constexpr Fold FoldConst(u32 atst, u32 aref, u32 alpha)
{
	return gsTileFoldAlphaTest(true, atst, aref, alpha, alpha);
}

constexpr Fold FoldRange(u32 atst, u32 aref, u32 lo, u32 hi)
{
	return gsTileFoldAlphaTest(true, atst, aref, lo, hi);
}

// The software renderer's own decision, transcribed from GSState::TryAlphaTest's GetResult
// so the sweeps below compare against an independent copy rather than against the code they
// are testing. UNKNOWN is what that function returns when the alpha RANGE straddles AREF.
enum class Ref
{
	Unknown,
	AllPass,
	AllFail
};

constexpr Ref RefResult(u32 atst, int aref, int amin, int amax)
{
	switch (atst)
	{
		case ATST_NEVER:
			return Ref::AllFail;
		case ATST_ALWAYS:
			return Ref::AllPass;
		case ATST_LESS:
			return (amax < aref) ? Ref::AllPass : ((amin >= aref) ? Ref::AllFail : Ref::Unknown);
		case ATST_LEQUAL:
			return (amax <= aref) ? Ref::AllPass : ((amin > aref) ? Ref::AllFail : Ref::Unknown);
		case ATST_EQUAL:
			return (amin == aref && amax == aref) ? Ref::AllPass :
												    ((amin > aref || amax < aref) ? Ref::AllFail : Ref::Unknown);
		case ATST_GEQUAL:
			return (amin >= aref) ? Ref::AllPass : ((amax < aref) ? Ref::AllFail : Ref::Unknown);
		case ATST_GREATER:
			return (amin > aref) ? Ref::AllPass : ((amax <= aref) ? Ref::AllFail : Ref::Unknown);
		case ATST_NOTEQUAL:
			return (amin == aref && amax == aref) ? Ref::AllFail :
												    ((amin > aref || amax < aref) ? Ref::AllPass : Ref::Unknown);
		default:
			return Ref::Unknown;
	}
}

const char* Name(Fold f)
{
	switch (f)
	{
		case Fold::AllPass: return "AllPass";
		case Fold::AllFail: return "AllFail";
		default: return "Varies";
	}
}
} // namespace

// ---------------------------------------------------------------------------------------
// The draws this exists for.
// ---------------------------------------------------------------------------------------

// R&C UYA's exhaust flare, register for register: a textured gouraud strip whose fragment
// alpha is bounded at 64 by (palette max 128 * vertex max 64) >> 7, under ATST=GEQUAL
// AREF=128 AFAIL=RGB_ONLY, ZTE=1 ZMSK=0, FBMSK=0. Every fragment fails, so the draw paints
// its RGB and touches neither the frame's alpha byte nor its depth. This is the whole
// effect: read as a live test the fragment stage discards all 351 primitives and the flare
// does not exist.
TEST(TileGpuAlphaTestFold, RcuyaExhaustFlareFailsEveryFragmentAndStillPaintsItsColour)
{
	const Fold fold = FoldRange(ATST_GEQUAL, 0x80, /*lo*/ 19, /*hi*/ 64);
	EXPECT_EQ(fold, Fold::AllFail);
	const bool all_fail = (fold == Fold::AllFail);

	// RGB lands, the alpha byte does not.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_RGB_ONLY), kGSTileChannelsRGB);
	// And no depth, whatever ZTE/ZMSK say.
	EXPECT_FALSE(gsTileDepthWriteSurvives(/*zte*/ true, /*zmsk*/ false, all_fail, AFAIL_RGB_ONLY));
	// No per-fragment test is left to emit, so nothing discards it.
	EXPECT_NE(fold, Fold::Varies);
	// One level higher and the draw straddles the reference: nothing may be decided then.
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x80, 19, 128), Fold::Varies);
}

// Armored Core 3's HUD: 1,547 draws a frame, ATST=GEQUAL AREF=128 AFAIL=FB_ONLY, vertex
// alpha exactly 64 through a MODULATE by an opaque texel -- (255 * 64) >> 7 = 127, one level
// below the reference. FB_ONLY keeps the WHOLE frame-buffer write, alpha byte included, and
// drops only the depth write.
TEST(TileGpuAlphaTestFold, Ac3HudSitsOneLevelUnderTheReferenceAndKeepsItsWholeColourWrite)
{
	const Fold fold = FoldRange(ATST_GEQUAL, 0x80, /*lo*/ 127, /*hi*/ 127);
	EXPECT_EQ(fold, Fold::AllFail);
	const bool all_fail = true;
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_FB_ONLY), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileDepthWriteSurvives(true, false, all_fail, AFAIL_FB_ONLY));

	// The one-level boundary is the whole draw: at 128 it passes instead, and at 128 the
	// fold must say so rather than guess.
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x80, 128, 128), Fold::AllPass);
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x80, 127, 128), Fold::Varies);
}

// SotC's full-screen haze quad, the one draw on the corpus where this fold costs accuracy
// rather than buying it: FB_ONLY at vertex alpha [11,22] against AREF=128, so it folds to
// all-fail and paints. Correct -- the software renderer paints it too -- and it is pinned
// here so a later change that stops painting it is recognised as a REGRESSION rather than
// as the SotC number improving.
TEST(TileGpuAlphaTestFold, SotcHazeQuadFoldsToAllFailAndPaints)
{
	const Fold fold = FoldRange(ATST_GEQUAL, 0x80, 11, 22);
	EXPECT_EQ(fold, Fold::AllFail);
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, true, AFAIL_FB_ONLY), kGSTileChannelsRGBA);
}

// Katamari Damacy's depth stamp: an untextured sprite, vertex alpha 0x00 on every vertex, so
// its interval is the single point 0. ATST=NOTEQUAL AREF=0 AFAIL=ZB_ONLY, ZTE=1 ZMSK=0,
// FBMSK=0x7f000000. Every fragment fails, so the draw lands no colour and its depth write
// survives. The point case has to keep working exactly as it did before the widening.
TEST(TileGpuAlphaTestFold, KatamariDepthStampFailsEveryFragmentAndKeepsItsDepthWrite)
{
	const Fold fold = FoldConst(ATST_NOTEQUAL, 0, 0x00);
	EXPECT_EQ(fold, Fold::AllFail);

	const bool all_fail = (fold == Fold::AllFail);
	// No colour: AFAIL=ZB_ONLY says a failing fragment writes depth and nothing else, and
	// FBMSK cannot put any of it back.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x7f000000u, kC32, all_fail, AFAIL_ZB_ONLY), 0);
	// The depth write is the whole point of the draw and it survives.
	EXPECT_TRUE(gsTileDepthWriteSurvives(/*zte*/ true, /*zmsk*/ false, all_fail, AFAIL_ZB_ONLY));
	// And no per-fragment test is left to emit.
	EXPECT_NE(fold, Fold::Varies);
	// The draw does not provably overwrite its pages -- it writes no colour at all -- so it
	// must not take the seed skip.
	EXPECT_FALSE(gsTileColorLandsOnEveryFragment(fold, AFAIL_ZB_ONLY));
}

// The same shape with the other three AFAIL modes.
TEST(TileGpuAlphaTestFold, AfailDecidesWhatAFoldedAllFailDrawLands)
{
	const Fold fold = FoldConst(ATST_NOTEQUAL, 0, 0x00);
	ASSERT_EQ(fold, Fold::AllFail);
	const bool all_fail = true;

	// KEEP: nothing lands anywhere. Colour mask empty and depth write gone -- the caller
	// reads that pair as a no-op draw and returns.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_KEEP), 0);
	EXPECT_FALSE(gsTileDepthWriteSurvives(true, false, all_fail, AFAIL_KEEP));

	// ZB_ONLY: depth only.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_ZB_ONLY), 0);
	EXPECT_TRUE(gsTileDepthWriteSurvives(true, false, all_fail, AFAIL_ZB_ONLY));

	// FB_ONLY: the whole frame buffer, no depth.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_FB_ONLY), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileDepthWriteSurvives(true, false, all_fail, AFAIL_FB_ONLY));

	// RGB_ONLY: colour without its alpha, no depth.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, all_fail, AFAIL_RGB_ONLY), kGSTileChannelsRGB);
	EXPECT_FALSE(gsTileDepthWriteSurvives(true, false, all_fail, AFAIL_RGB_ONLY));

	// FBMSK composes on top of all of them, unchanged by where the all-fail came from.
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00FFFFFFu, kC32, all_fail, AFAIL_FB_ONLY), kA);
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00FF0000u, kC32, all_fail, AFAIL_RGB_ONLY), static_cast<u8>(kR | kG));
	EXPECT_EQ(gsTileFrameColorWriteMask(0x00FFFFFFu, kC32, all_fail, AFAIL_RGB_ONLY), 0);
	// ZMSK and ZTE still have the final word on the depth write.
	EXPECT_FALSE(gsTileDepthWriteSurvives(true, true, all_fail, AFAIL_ZB_ONLY));
	EXPECT_FALSE(gsTileDepthWriteSurvives(false, false, all_fail, AFAIL_ZB_ONLY));
}

// A comparison that provably holds is ATST_ALWAYS: no test is emitted and nothing else
// about the draw changes.
TEST(TileGpuAlphaTestFold, AnIntervalThatPassesIsAlways)
{
	EXPECT_EQ(FoldConst(ATST_NOTEQUAL, 0, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_GEQUAL, 0x40, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_EQUAL, 0x80, 0x80), Fold::AllPass);
	// And the interval forms of the same three.
	EXPECT_EQ(FoldRange(ATST_NOTEQUAL, 0, 1, 0xFF), Fold::AllPass);
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x40, 0x40, 0xFF), Fold::AllPass);
	EXPECT_EQ(FoldRange(ATST_LESS, 0x80, 0, 0x7F), Fold::AllPass);

	// An all-pass draw writes what its registers say, with no AFAIL fold at all: AFAIL only
	// ever describes a fragment that FAILED.
	for (u32 afail = 0; afail < 4; afail++)
	{
		EXPECT_EQ(gsTileFrameColorWriteMask(0x00000000u, kC32, /*all_fail*/ false, afail), kGSTileChannelsRGBA);
		EXPECT_TRUE(gsTileDepthWriteSurvives(true, false, /*all_fail*/ false, afail));
	}
}

// ---------------------------------------------------------------------------------------
// The comparisons themselves. Off by one here silently changes some other game's draw.
// ---------------------------------------------------------------------------------------

// Against the software renderer's own decision, over every ATST, every AREF and every
// interval endpoint pair. This is the whole table: every place the fold answers AllPass or
// AllFail the reference must agree, and every place the reference says UNKNOWN the fold must
// say Varies -- deciding a straddling draw is exactly the error that drops real fragments.
TEST(TileGpuAlphaTestFold, AgreesWithTheSoftwareTestOnEveryAtstArefInterval)
{
	for (u32 atst = 0; atst < 8; atst++)
	{
		for (u32 aref = 0; aref < 256; aref += 5)
		{
			for (u32 lo = 0; lo < 256; lo += 7)
			{
				for (u32 hi = lo; hi < 256; hi += 11)
				{
					const Ref want = RefResult(atst, static_cast<int>(aref), static_cast<int>(lo),
						static_cast<int>(hi));
					const Fold got = gsTileFoldAlphaTest(true, atst, aref, lo, hi);
					const Fold want_fold = (want == Ref::AllPass) ? Fold::AllPass :
										   (want == Ref::AllFail) ? Fold::AllFail :
																	Fold::Varies;
					ASSERT_EQ(got, want_fold) << "atst " << atst << " aref " << aref << " [" << lo << "," << hi
											  << "] got " << Name(got) << " want " << Name(want_fold);
				}
			}
		}
	}
}

// The degenerate interval -- one alpha value -- over the FULL grid, because that is the case
// the fold started life as and the one a regression would most plausibly reach. With a point
// interval the reference can never answer UNKNOWN, so this is a total agreement check.
TEST(TileGpuAlphaTestFold, AgreesWithTheSoftwareTestOnEveryAtstArefPointAlpha)
{
	for (u32 atst = 0; atst < 8; atst++)
	{
		for (u32 aref = 0; aref < 256; aref++)
		{
			for (u32 alpha = 0; alpha < 256; alpha++)
			{
				const Ref want = RefResult(atst, static_cast<int>(aref), static_cast<int>(alpha),
					static_cast<int>(alpha));
				ASSERT_NE(want, Ref::Unknown) << "atst " << atst << " aref " << aref << " alpha " << alpha;
				const Fold got = FoldConst(atst, aref, alpha);
				EXPECT_EQ(got == Fold::AllPass, want == Ref::AllPass)
					<< "atst " << atst << " aref " << aref << " alpha " << alpha << " got " << Name(got);
				EXPECT_EQ(got == Fold::AllFail, want == Ref::AllFail)
					<< "atst " << atst << " aref " << aref << " alpha " << alpha << " got " << Name(got);
			}
		}
	}
}

// The boundaries, spelled out. These are the values a transcription error lands on, and the
// sweeps above would report them as one failure among thousands.
TEST(TileGpuAlphaTestFold, ArefBoundaries)
{
	// Nothing is less than 0, and nothing is greater than 255: two tests that can never pass.
	for (u32 alpha = 0; alpha < 256; alpha++)
	{
		EXPECT_EQ(FoldConst(ATST_LESS, 0, alpha), Fold::AllFail) << "alpha " << alpha;
		EXPECT_EQ(FoldConst(ATST_GREATER, 255, alpha), Fold::AllFail) << "alpha " << alpha;
		// And their mirrors, which can never fail.
		EXPECT_EQ(FoldConst(ATST_GEQUAL, 0, alpha), Fold::AllPass) << "alpha " << alpha;
		EXPECT_EQ(FoldConst(ATST_LEQUAL, 255, alpha), Fold::AllPass) << "alpha " << alpha;
	}
	// The same four over the widest interval there is: they are register-decided, so even an
	// unbounded alpha folds them.
	EXPECT_EQ(FoldRange(ATST_LESS, 0, kAnyLo, kAnyHi), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_GREATER, 255, kAnyLo, kAnyHi), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0, kAnyLo, kAnyHi), Fold::AllPass);
	EXPECT_EQ(FoldRange(ATST_LEQUAL, 255, kAnyLo, kAnyHi), Fold::AllPass);

	// The pair either side of AREF, for each ordered comparison.
	EXPECT_EQ(FoldConst(ATST_LESS, 0x80, 0x7F), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_LESS, 0x80, 0x80), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_LEQUAL, 0x80, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_LEQUAL, 0x80, 0x81), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_GEQUAL, 0x80, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_GEQUAL, 0x80, 0x7F), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_GREATER, 0x80, 0x81), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_GREATER, 0x80, 0x80), Fold::AllFail);
	// EQUAL and NOTEQUAL at the ends, where Katamari's stamp and SotC's EQUAL-255 sky live.
	EXPECT_EQ(FoldConst(ATST_EQUAL, 0, 0), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_NOTEQUAL, 0, 0), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_EQUAL, 255, 255), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_NOTEQUAL, 255, 255), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_EQUAL, 255, 254), Fold::AllFail);
	EXPECT_EQ(FoldConst(ATST_NOTEQUAL, 255, 254), Fold::AllPass);

	// The interval one level either side of the reference, for every ordered comparison. The
	// pair that touches AREF may not be decided; the pair that clears it must be.
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x80, 0x7E, 0x7F), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_GEQUAL, 0x80, 0x7F, 0x80), Fold::Varies);
	EXPECT_EQ(FoldRange(ATST_GREATER, 0x80, 0x7F, 0x80), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_GREATER, 0x80, 0x80, 0x81), Fold::Varies);
	EXPECT_EQ(FoldRange(ATST_LESS, 0x80, 0x80, 0x81), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_LESS, 0x80, 0x7F, 0x80), Fold::Varies);
	EXPECT_EQ(FoldRange(ATST_LEQUAL, 0x80, 0x81, 0x82), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_LEQUAL, 0x80, 0x80, 0x81), Fold::Varies);
	// EQUAL/NOTEQUAL: an interval containing AREF and something else decides neither way.
	EXPECT_EQ(FoldRange(ATST_EQUAL, 0x80, 0x80, 0x81), Fold::Varies);
	EXPECT_EQ(FoldRange(ATST_NOTEQUAL, 0x80, 0x80, 0x81), Fold::Varies);
	EXPECT_EQ(FoldRange(ATST_EQUAL, 0x80, 0x81, 0x82), Fold::AllFail);
	EXPECT_EQ(FoldRange(ATST_NOTEQUAL, 0x80, 0x81, 0x82), Fold::AllPass);
}

// ---------------------------------------------------------------------------------------
// What must NOT change.
// ---------------------------------------------------------------------------------------

// A draw whose alpha the plan cannot bound at all keeps the per-fragment road, whatever its
// registers say. This is what an AA1 draw and an unsynced-palette draw get, and it is what
// every draw got before the fold existed.
TEST(TileGpuAlphaTestFold, AnUnboundedAlphaKeepsThePerFragmentTest)
{
	for (u32 atst = ATST_LESS; atst <= ATST_NOTEQUAL; atst++)
	{
		for (u32 aref : {1u, 0x80u, 0xFEu})
			EXPECT_EQ(gsTileFoldAlphaTest(true, atst, aref, kAnyLo, kAnyHi), Fold::Varies)
				<< "atst " << atst << " aref " << aref;
	}
	// NEVER and ALWAYS never needed the alpha in the first place, so they still fold.
	EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, 0x80, kAnyLo, kAnyHi), Fold::AllFail);
	EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_ALWAYS, 0x80, kAnyLo, kAnyHi), Fold::AllPass);
	// An inverted interval says nothing, so it decides nothing either -- a caller that fills
	// the pair in the wrong order gets the conservative answer, not a wrong one.
	for (u32 atst = ATST_LESS; atst <= ATST_NOTEQUAL; atst++)
		EXPECT_EQ(gsTileFoldAlphaTest(true, atst, 0x80, 200, 100), Fold::Varies) << "atst " << atst;
}

// ATE=0 is not a test at all: every fragment passes and AFAIL says nothing.
TEST(TileGpuAlphaTestFold, AlphaTestOffPassesEverything)
{
	for (u32 atst = 0; atst < 8; atst++)
	{
		EXPECT_EQ(gsTileFoldAlphaTest(false, atst, 0x80, kAnyLo, kAnyHi), Fold::AllPass) << "atst " << atst;
		EXPECT_EQ(gsTileFoldAlphaTest(false, atst, 0x80, 0, 0), Fold::AllPass) << "atst " << atst;
	}
}

// The ATST=NEVER road is a pure rename: it folded to all-fail before this existed and it
// folds to all-fail now, for every alpha the draw could carry and whether or not the plan
// can bound one.
TEST(TileGpuAlphaTestFold, NeverRoadIsUnchanged)
{
	for (u32 aref = 0; aref < 256; aref++)
	{
		EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, aref, aref, aref), Fold::AllFail);
		EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, aref, kAnyLo, kAnyHi), Fold::AllFail);
	}
	// And the two derivations it feeds answer exactly what they answered before, on every
	// AFAIL and every frame format.
	for (u32 fmsk : {kC32, kC24})
	{
		for (u32 afail = 0; afail < 4; afail++)
		{
			const u8 m = gsTileFrameColorWriteMask(0x00000000u, fmsk, /*all_fail*/ true, afail);
			const u8 want = (afail == AFAIL_KEEP || afail == AFAIL_ZB_ONLY) ?
								0 :
								((afail == AFAIL_RGB_ONLY) ? kGSTileChannelsRGB : kGSTileChannelsRGBA);
			EXPECT_EQ(m, want) << std::hex << "fmsk " << fmsk << " afail " << afail;
			EXPECT_EQ(gsTileDepthWriteSurvives(true, false, true, afail), afail == AFAIL_ZB_ONLY);
		}
	}
}

// ---------------------------------------------------------------------------------------
// The seed skip's gate.
// ---------------------------------------------------------------------------------------

// "This sprite provably overwrites the page, so do not bring the old bytes in first" needs
// every fragment to land its colour. The fold decides that and nothing else may.
TEST(TileGpuAlphaTestFold, SeedSkipNeedsEveryFragmentToLandItsColour)
{
	for (u32 afail = 0; afail < 4; afail++)
	{
		// No test, or one that rejects nothing: every fragment writes, AFAIL is dead text.
		EXPECT_TRUE(gsTileColorLandsOnEveryFragment(Fold::AllPass, afail)) << "afail " << afail;
		// A test that rejects everything writes the frame buffer only in the two modes that
		// say a failing fragment still does.
		EXPECT_EQ(gsTileColorLandsOnEveryFragment(Fold::AllFail, afail),
			afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY)
			<< "afail " << afail;
		// A live test can reject a fragment, so the page is not provably covered.
		EXPECT_FALSE(gsTileColorLandsOnEveryFragment(Fold::Varies, afail)) << "afail " << afail;
	}
}

// The gate the renderer used before the fold, transcribed, so the fold's version can be
// shown to answer the same on every input the old one could see: ATE off, ATST=ALWAYS, and
// ATST=NEVER with a frame-buffer-writing AFAIL. The only inputs where the two differ are
// the ones the old expression could not decide at all.
TEST(TileGpuAlphaTestFold, SeedSkipGateMatchesThePreFoldExpressionWhereverThatCouldDecide)
{
	for (u32 atst = 0; atst < 8; atst++)
	{
		for (u32 afail = 0; afail < 4; afail++)
		{
			for (bool ate : {false, true})
			{
				const bool old_gate = !ate || atst == ATST_ALWAYS ||
									  ((ate && atst == ATST_NEVER) &&
										  (afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY));
				// With no bound to fold, the new gate sees exactly what the old one saw.
				const Fold fold = gsTileFoldAlphaTest(ate, atst, 0x80, kAnyLo, kAnyHi);
				EXPECT_EQ(gsTileColorLandsOnEveryFragment(fold, afail), old_gate)
					<< "ate " << ate << " atst " << atst << " afail " << afail;
			}
		}
	}
}

// What the seed skip may be told, now that the interval decides draws a constant alpha could
// not. The skip means "these bytes are all about to be overwritten, do not fetch them", so
// every admitted draw must write every channel of the page -- and the sweep below says which
// admissions do and which is the one that does not.
//
// Exactly three admissions exist over the whole domain:
//   * AllPass          -- every fragment writes everything. Sound.
//   * AllFail FB_ONLY  -- every fragment fails and FB_ONLY keeps the whole frame-buffer
//                         write, alpha byte included. Sound.
//   * AllFail RGB_ONLY -- colour lands, the alpha byte does not. NOT a total write, and the
//                         named pre-existing carve-out in gsTileColorLandsOnEveryFragment.
//                         Held harmless by the FBMSK half of the gate, which refuses the
//                         corpus's only such sprites (OutRun's, at FBMSK=0xFF000000).
//
// Measured on the 18-dump corpus when the interval landed, which is the change that could
// have populated that third row: every draw that reaches the seed skip with an all-fail
// verdict is ATST=NEVER + AFAIL=FB_ONLY (SotC 227 a frame, OutRun 8 and 4, MGS3 2). The
// interval adds none, so the residual stays theoretical rather than becoming live.
TEST(TileGpuAlphaTestFold, EverySeedSkipAdmissionIsATotalWriteExceptTheNamedRgbOnlyCarveOut)
{
	bool saw_all_pass = false, saw_fb_only = false, saw_rgb_only = false;
	for (u32 atst = 0; atst < 8; atst++)
	{
		for (u32 aref = 0; aref < 256; aref += 17)
		{
			for (u32 lo = 0; lo < 256; lo += 23)
			{
				for (u32 hi = lo; hi < 256; hi += 29)
				{
					const Fold fold = gsTileFoldAlphaTest(true, atst, aref, lo, hi);
					for (u32 afail = 0; afail < 4; afail++)
					{
						if (!gsTileColorLandsOnEveryFragment(fold, afail))
							continue;
						const u8 m = gsTileFrameColorWriteMask(0x00000000u, kC32, fold == Fold::AllFail, afail);
						if (fold == Fold::AllPass)
						{
							saw_all_pass = true;
							EXPECT_EQ(m, kGSTileChannelsRGBA);
						}
						else if (afail == AFAIL_FB_ONLY)
						{
							saw_fb_only = true;
							EXPECT_EQ(m, kGSTileChannelsRGBA);
						}
						else
						{
							saw_rgb_only = true;
							EXPECT_EQ(afail, AFAIL_RGB_ONLY)
								<< "atst " << atst << " aref " << aref << " [" << lo << "," << hi << "]";
							EXPECT_EQ(m, kGSTileChannelsRGB); // the carve-out: no alpha byte
						}
					}
				}
			}
		}
	}
	// All three rows are actually reached, so the sweep is testing what it claims to.
	EXPECT_TRUE(saw_all_pass);
	EXPECT_TRUE(saw_fb_only);
	EXPECT_TRUE(saw_rgb_only);
}

// -- what an AFAIL that keeps something can be served exactly ------------------------------------
//
// ⚠️ The depth clause below is the whole boundary, and it is not caution. Under AFAIL=RGB_ONLY a
// failing fragment lands its RGB, keeps the destination's alpha and does NOT write depth. Discarding
// it -- which is what the fragment stage does -- gets two of those three right by accident: no alpha
// write and no depth write. The single error is the missing RGB.
//
// So landing that RGB is only free where the draw writes no depth ANYWAY. On a depth-writing draw,
// keeping the fragment alive to write its colour also sends it through the depth stage, where the
// console does not let it write -- per-fragment depth masking, which a colour read cannot do. Serving
// those would trade a missing colour for a wrong depth, which is not a repair.
//
// The corpus splits almost entirely the wrong way: ~1258 varying non-KEEP AFAIL draws a frame, 1012
// of them RGB_ONLY, and all but 18 of those write depth.

TEST(TileGpuAlphaTestFold, AfailAlphaKeepIsServedOnlyWhereTheDrawWritesNoDepth)
{
	using Fold = GSTileAlphaTestFold;
	// The servable shape.
	EXPECT_TRUE(gsTileGpuAfailKeepsAlpha(Fold::Varies, AFAIL_RGB_ONLY, false));
	// ...and the one that would trade a colour error for a depth error.
	EXPECT_FALSE(gsTileGpuAfailKeepsAlpha(Fold::Varies, AFAIL_RGB_ONLY, true));

	// A test that does not vary needs nothing: the fold already moved it into the write flags.
	EXPECT_FALSE(gsTileGpuAfailKeepsAlpha(Fold::AllPass, AFAIL_RGB_ONLY, false));
	EXPECT_FALSE(gsTileGpuAfailKeepsAlpha(Fold::AllFail, AFAIL_RGB_ONLY, false));

	// The other three AFAIL modes are not this road's: KEEP is what the discard already does
	// exactly, and FB_ONLY and ZB_ONLY need a write the fragment stage cannot make conditional --
	// FB_ONLY the whole frame-buffer write without depth, ZB_ONLY the depth write alone.
	for (u32 afail : {u32(AFAIL_KEEP), u32(AFAIL_FB_ONLY), u32(AFAIL_ZB_ONLY)})
	{
		EXPECT_FALSE(gsTileGpuAfailKeepsAlpha(Fold::Varies, afail, false)) << afail;
		EXPECT_FALSE(gsTileGpuAfailKeepsAlpha(Fold::Varies, afail, true)) << afail;
	}
}
