// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The alpha test decided at plan time, where the fragment alpha cannot vary.
//
// TileGpu's fragment stage discards a failing fragment outright. That is exact for
// AFAIL=KEEP and wrong for the three modes that still write something on failure, so the
// renderer only asks for a per-fragment test where the two sides of it land differently,
// and folds the cases it can decide up front into the write flags instead. ATST=NEVER was
// always folded. This pins the other half: a COMPARISON that provably fails, or provably
// passes, against an alpha that is the same for every fragment of the draw.
//
// Katamari Damacy is why. The King's head is composited out of two sprites over the same
// rectangle: the first stamps a lower Z into it, the second draws the head with
// ZTST=GEQUAL against that Z. The stamp is untextured with vertex alpha 0x00 on all four
// vertices, and asks for ATST=NOTEQUAL AREF=0 -- "0 != 0" is false, so every fragment
// fails -- with AFAIL=ZB_ONLY, "a failing fragment still writes depth". Read as a live
// per-fragment test the stamp discards every fragment and deposits no depth at all, and
// the head then fails its own GEQUAL over every pixel the purple star behind it covers.
// The head was missing from every archived TileGpu run of that scene.
//
// Three things are pinned here:
//
//  - the fold's comparisons are the SAME comparisons the software renderer makes
//    (GSState::TryAlphaTest's GetResult, with the alpha range collapsed to one value) and
//    the same ones tilegpu.glsl's fragment stage makes. A boundary that disagrees with
//    either silently changes a draw in some other game.
//  - what a folded all-fail draw writes is decided by AFAIL alone, exactly as ATST=NEVER
//    already was -- so the NEVER road must come out byte-identical.
//  - the seed skip's "this sprite provably overwrites the page" gate has to keep meaning
//    that. A fold to all-pass makes a draw MORE total; a fold to all-fail makes it total
//    only in the AFAIL modes that still write the frame buffer.

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <ios>

namespace
{
constexpr u32 kC32 = 0xFFFFFFFFu; // PSMCT32 / PSMZ32 stored bits
constexpr u32 kC24 = 0x00FFFFFFu; // PSMCT24 / PSMZ24

constexpr u8 kR = 0x1, kG = 0x2, kB = 0x4, kA = 0x8;

using Fold = GSTileAlphaTestFold;

constexpr Fold FoldConst(u32 atst, u32 aref, u32 alpha)
{
	return gsTileFoldAlphaTest(true, atst, aref, true, alpha);
}

// The software renderer's own decision, transcribed from GSState::TryAlphaTest's GetResult
// so the sweep below compares against an independent copy rather than against the code it
// is testing. UNKNOWN is what that function returns when the vertex-trace alpha RANGE
// straddles AREF; with a constant alpha the range is a point and it can never come back.
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
// When the fragment alpha is a draw-time constant.
// ---------------------------------------------------------------------------------------

// The three register combinations that make the alpha the test compares the same for every
// fragment. Each half is a separate reason and all of them have to hold.
TEST(TileGpuAlphaTestFold, ConstantAlphaNeedsNoTextureAlphaNoCoverageAndOneVertexAlpha)
{
	for (bool tme : {false, true})
	{
		for (bool tcc : {false, true})
		{
			for (bool aa1 : {false, true})
			{
				for (bool uniform : {false, true})
				{
					// The texture supplies the alpha only when it is sampled AND TCC says its
					// alpha is used; every texture function passes the vertex alpha through
					// when TCC is 0.
					const bool texture_supplies = tme && tcc;
					// AA1 modulates a fragment's alpha by its coverage, per pixel.
					const bool expect = uniform && !aa1 && !texture_supplies;
					EXPECT_EQ(gsTileAlphaIsDrawConstant(tme, tcc, aa1, uniform), expect)
						<< "tme " << tme << " tcc " << tcc << " aa1 " << aa1 << " uniform " << uniform;
				}
			}
		}
	}
}

// Katamari Damacy's depth stamp, register for register: an untextured sprite, vertex alpha
// 0x00 on every vertex, ATST=NOTEQUAL AREF=0 AFAIL=ZB_ONLY, ZTE=1 ZMSK=0, FBMSK=0x7f000000.
// Every fragment fails, so the draw lands no colour and its depth write survives.
TEST(TileGpuAlphaTestFold, KatamariDepthStampFailsEveryFragmentAndKeepsItsDepthWrite)
{
	ASSERT_TRUE(gsTileAlphaIsDrawConstant(/*tme*/ false, /*tcc*/ false, /*aa1*/ false, /*uniform*/ true));
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
TEST(TileGpuAlphaTestFold, ConstantAlphaThatPassesIsAlways)
{
	EXPECT_EQ(FoldConst(ATST_NOTEQUAL, 0, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_GEQUAL, 0x40, 0x80), Fold::AllPass);
	EXPECT_EQ(FoldConst(ATST_EQUAL, 0x80, 0x80), Fold::AllPass);

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
// alpha. With a constant alpha the reference can never answer UNKNOWN, so this is a total
// agreement check, not a subset one.
TEST(TileGpuAlphaTestFold, AgreesWithTheSoftwareTestOnEveryAtstArefAlpha)
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
// sweep above would report them as one failure among sixteen thousand.
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
}

// ---------------------------------------------------------------------------------------
// What must NOT change.
// ---------------------------------------------------------------------------------------

// A draw whose alpha the plan cannot pin keeps the per-fragment road, whatever its
// registers say. This is every textured TCC=1 draw and every gouraud-alpha one.
TEST(TileGpuAlphaTestFold, AlphaThatCanVaryKeepsThePerFragmentTest)
{
	for (u32 atst = ATST_LESS; atst <= ATST_NOTEQUAL; atst++)
	{
		for (u32 aref : {0u, 1u, 0x80u, 0xFEu, 0xFFu})
			EXPECT_EQ(gsTileFoldAlphaTest(true, atst, aref, /*constant*/ false, 0), Fold::Varies)
				<< "atst " << atst << " aref " << aref;
	}
	// NEVER and ALWAYS never needed the alpha in the first place, so they still fold.
	EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, 0x80, false, 0), Fold::AllFail);
	EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_ALWAYS, 0x80, false, 0), Fold::AllPass);
}

// ATE=0 is not a test at all: every fragment passes and AFAIL says nothing.
TEST(TileGpuAlphaTestFold, AlphaTestOffPassesEverything)
{
	for (u32 atst = 0; atst < 8; atst++)
	{
		for (bool constant : {false, true})
			EXPECT_EQ(gsTileFoldAlphaTest(false, atst, 0x80, constant, 0), Fold::AllPass) << "atst " << atst;
	}
}

// The ATST=NEVER road is a pure rename: it folded to all-fail before this existed and it
// folds to all-fail now, for every alpha the draw could carry and whether or not the plan
// can pin one.
TEST(TileGpuAlphaTestFold, NeverRoadIsUnchanged)
{
	for (u32 aref = 0; aref < 256; aref++)
	{
		for (bool constant : {false, true})
		{
			EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, aref, constant, aref), Fold::AllFail);
			EXPECT_EQ(gsTileFoldAlphaTest(true, ATST_NEVER, aref, constant, 0xFF - aref), Fold::AllFail);
		}
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
				// With no constant to fold, the new gate sees exactly what the old one saw.
				const Fold fold = gsTileFoldAlphaTest(ate, atst, 0x80, /*constant*/ false, 0);
				EXPECT_EQ(gsTileColorLandsOnEveryFragment(fold, afail), old_gate)
					<< "ate " << ate << " atst " << atst << " afail " << afail;
			}
		}
	}
	// And where a constant DOES decide it, the gate can only get stronger, never weaker: an
	// all-pass fold is a total write, an all-fail one is total in the same two AFAIL modes
	// ATST=NEVER already was.
	for (u32 atst = ATST_LESS; atst <= ATST_NOTEQUAL; atst++)
	{
		for (u32 aref = 0; aref < 256; aref++)
		{
			for (u32 alpha = 0; alpha < 256; alpha += 17)
			{
				const Fold fold = FoldConst(atst, aref, alpha);
				for (u32 afail = 0; afail < 4; afail++)
				{
					if (gsTileColorLandsOnEveryFragment(fold, afail))
					{
						EXPECT_TRUE(fold == Fold::AllPass ||
									(fold == Fold::AllFail &&
										(afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY)));
					}
				}
			}
		}
	}
}
