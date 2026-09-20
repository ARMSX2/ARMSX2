// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the declared-feedback-loop scope override (GS/Renderers/Common/GSDeclaredLoopScopePolicy.h).
//
// On the declared road a draw that reads its own render target declares an attachment feedback
// loop, and on Turnip that declaration is what orders the read -- at the price of a coherent
// primitive mode that makes the GPU wait for any earlier primitive covering the same sample. The
// scope override confines the declaration, and therefore that price, to the draws whose own
// primitives overlap; every other reader goes back on the copy road, which serves it exactly.
//
// Two things need pinning here and neither is visible on the machine the gates run on:
//
//   * off the declared road the override decides nothing at all, so a device that never declares
//     cannot be changed by it -- which is what makes the byte-identity gate a statement about the
//     whole binary rather than about one road;
//   * PRIM_OVERLAP_UNKNOWN is NOT treated as overlapping, deliberately, unlike the barrier
//     decision next door. A barrier is cheap insurance; a serialised draw is not, and an UNKNOWN
//     draw is where the copy road leaves it today.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSDeclaredLoopScopePolicy.h"

#include <gtest/gtest.h>

namespace
{
	constexpr GSDeclaredLoopScopeInputs OnTheRoad(bool overlap_yes)
	{
		GSDeclaredLoopScopeInputs in;
		in.declared_road = true;
		in.prim_overlap_yes = overlap_yes;
		return in;
	}

	constexpr GSDeclaredLoopScopeInputs Scoped(GSDeclaredLoopScopeInputs in)
	{
		in.scope = GSDeclaredLoopScope::OverlapOnly;
		return in;
	}
} // namespace

// The road as the arm shipped it: every reader declares, and nothing is withheld.
TEST(GSDeclaredLoopScope, TheDefaultScopeDeclaresEveryReader)
{
	EXPECT_TRUE(GSDrawDeclaresFeedbackLoop(OnTheRoad(false)));
	EXPECT_TRUE(GSDrawDeclaresFeedbackLoop(OnTheRoad(true)));
	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(OnTheRoad(false)));
	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(OnTheRoad(true)));
}

// Scoped, the overlapping draws keep it and everything else gives it up.
TEST(GSDeclaredLoopScope, ScopedOnlyTheOverlappingDrawsDeclare)
{
	EXPECT_TRUE(GSDrawDeclaresFeedbackLoop(Scoped(OnTheRoad(true))));
	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(Scoped(OnTheRoad(true))));

	EXPECT_FALSE(GSDrawDeclaresFeedbackLoop(Scoped(OnTheRoad(false))));
	EXPECT_TRUE(GSDrawWithholdsFeedbackLoop(Scoped(OnTheRoad(false))));
}

// Off the declared road there is no declaration to narrow, so the scope decides nothing and
// nothing is withheld -- on a device taking the copy road, the shipped in-tile read, or a
// backend that has no declared road at all. This is the row that makes the byte-identity gate
// mean what it says.
TEST(GSDeclaredLoopScope, OffTheRoadTheScopeIsInert)
{
	GSDeclaredLoopScopeInputs off_default;
	GSDeclaredLoopScopeInputs off_scoped = Scoped(GSDeclaredLoopScopeInputs());
	GSDeclaredLoopScopeInputs off_scoped_overlap = Scoped(GSDeclaredLoopScopeInputs());
	off_scoped_overlap.prim_overlap_yes = true;

	EXPECT_FALSE(GSDrawDeclaresFeedbackLoop(off_default));
	EXPECT_FALSE(GSDrawDeclaresFeedbackLoop(off_scoped));
	EXPECT_FALSE(GSDrawDeclaresFeedbackLoop(off_scoped_overlap));

	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(off_default));
	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(off_scoped));
	EXPECT_FALSE(GSDrawWithholdsFeedbackLoop(off_scoped_overlap));
}

// Withholding is the complement of declaring, on the road and only on it. Stated as its own test
// because the renderer publishes the withholding form -- the draw config's zero state has to mean
// "behave as before" -- and an inversion that drifted would turn the inert case into the arm.
TEST(GSDeclaredLoopScope, WithholdingIsTheComplementOnTheRoad)
{
	for (int bits = 0; bits < 8; bits++)
	{
		GSDeclaredLoopScopeInputs in;
		in.scope = (bits & 1) != 0 ? GSDeclaredLoopScope::OverlapOnly : GSDeclaredLoopScope::All;
		in.declared_road = (bits & 2) != 0;
		in.prim_overlap_yes = (bits & 4) != 0;

		const bool declares = in.declared_road &&
		                      (in.scope == GSDeclaredLoopScope::All || in.prim_overlap_yes);
		EXPECT_EQ(GSDrawDeclaresFeedbackLoop(in), declares) << "bits=" << bits;
		EXPECT_EQ(GSDrawWithholdsFeedbackLoop(in), in.declared_road && !declares) << "bits=" << bits;
	}
}

// The process-wide switch: default "all readers", settable, and its banner name says which arm a
// log is.
TEST(GSDeclaredLoopScope, TheProcessScopeDefaultsToEveryReader)
{
	EXPECT_EQ(GSDeclaredLoopScopePolicy::GetScope(), GSDeclaredLoopScope::All);
	EXPECT_STREQ(GSDeclaredLoopScopePolicy::Name(), "all readers");

	GSDeclaredLoopScopePolicy::SetScope(GSDeclaredLoopScope::OverlapOnly);
	EXPECT_EQ(GSDeclaredLoopScopePolicy::GetScope(), GSDeclaredLoopScope::OverlapOnly);
	EXPECT_STREQ(GSDeclaredLoopScopePolicy::Name(), "overlap-only");

	GSDeclaredLoopScopePolicy::SetScope(GSDeclaredLoopScope::All);
	EXPECT_EQ(GSDeclaredLoopScopePolicy::GetScope(), GSDeclaredLoopScope::All);
}
