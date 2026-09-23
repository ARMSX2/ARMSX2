// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the DATE road override (GS/Renderers/Common/GSDateRoadPolicy.h).
//
// The override pins every destination-alpha-test draw to primitive-ID tracking so that the DATE
// mechanism can be held still while the colour self-read road changes under it. Two things need
// pinning, and neither can be seen on the machine the gates run on:
//
//   * left alone it does nothing at all, on any draw, on any device -- which is what lets the
//     94-cell byte-identity gate mean "this override is inert" rather than "this override is inert on
//     the one road that machine takes";
//   * asked for, it still does not break the two fallbacks the renderer already has. A device
//     with no primitive-ID target has no image to prefill, and a draw that discards alternate
//     lines in the shader cannot use primitive-ID tracking at all, because the discard and the
//     tracking disagree about what "this pixel was written" means.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSDateRoadPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// An ordinary DATE draw on a device that has everything: the population the override is
	// aimed at.
	constexpr GSDateRoadInputs OrdinaryDateDraw()
	{
		GSDateRoadInputs in;
		in.date_enabled = true;
		in.device_has_primitive_id = true;
		return in;
	}

	constexpr GSDateRoadInputs Forced(GSDateRoadInputs in)
	{
		in.override_mode = GSDateRoadOverride::PrimID;
		return in;
	}
} // namespace

// Auto is today's behaviour, and today's behaviour is every road the renderer would have picked.
TEST(GSDateRoad, AutoChangesNothing)
{
	EXPECT_FALSE(GSDateRoadForcesPrimID(OrdinaryDateDraw()));

	GSDateRoadInputs no_date = OrdinaryDateDraw();
	no_date.date_enabled = false;
	EXPECT_FALSE(GSDateRoadForcesPrimID(no_date));

	GSDateRoadInputs already = OrdinaryDateDraw();
	already.already_primid = true;
	EXPECT_FALSE(GSDateRoadForcesPrimID(already));
}

// Asked for, it takes the draws that would have gone to a stencil road or to the barrier road.
TEST(GSDateRoad, ForcedTakesTheDrawsThatWouldHaveGoneElsewhere)
{
	EXPECT_TRUE(GSDateRoadForcesPrimID(Forced(OrdinaryDateDraw())));
}

// A draw with no destination alpha test is not a DATE draw and the override has no opinion about
// it. Stated because the alternative -- "force primid on everything" -- would turn a flag meant
// to hold one mechanism still into one that creates a mechanism where there was none.
TEST(GSDateRoad, ADrawWithNoDateIsNotTouched)
{
	GSDateRoadInputs in = Forced(OrdinaryDateDraw());
	in.date_enabled = false;
	EXPECT_FALSE(GSDateRoadForcesPrimID(in));
}

// A draw already on the road is left alone, so a census of "did the override fire" counts the
// draws that MOVED rather than the draws that ended up there. Indiana Jones is the case: its 394
// DATE draws a frame are primitive-ID tracking on both arms already.
TEST(GSDateRoad, ADrawAlreadyOnTheRoadDoesNotFire)
{
	GSDateRoadInputs in = Forced(OrdinaryDateDraw());
	in.already_primid = true;
	EXPECT_FALSE(GSDateRoadForcesPrimID(in));
}

// Without a primitive-ID render target there is nothing to prefill, and the road the renderer
// falls back to is the only answer there is. The override does not get to ask for a road the
// device cannot take.
TEST(GSDateRoad, NoPrimitiveIDTargetMeansNoForcing)
{
	GSDateRoadInputs in = Forced(OrdinaryDateDraw());
	in.device_has_primitive_id = false;
	EXPECT_FALSE(GSDateRoadForcesPrimID(in));
}

// SCANMSK line discard and primitive-ID tracking are mutually exclusive: the shader discards
// alternate lines, and the tracking image would record those pixels as written by a primitive
// that produced no fragment there. The renderer already refuses the pairing; so does this.
TEST(GSDateRoad, ScanmskDiscardKeepsItsOwnRoad)
{
	GSDateRoadInputs in = Forced(OrdinaryDateDraw());
	in.scanmsk_discards_lines = true;
	EXPECT_FALSE(GSDateRoadForcesPrimID(in));
}

// Swept: firing is exactly "asked, a DATE draw, not already there, and both fallbacks clear".
// If a term is ever added that can fire without one of those, this is what catches it.
TEST(GSDateRoad, FiringIsTheConjunctionAndNothingElse)
{
	for (int bits = 0; bits < 32; bits++)
	{
		GSDateRoadInputs in;
		in.override_mode = (bits & 1) != 0 ? GSDateRoadOverride::PrimID : GSDateRoadOverride::Auto;
		in.date_enabled = (bits & 2) != 0;
		in.already_primid = (bits & 4) != 0;
		in.device_has_primitive_id = (bits & 8) != 0;
		in.scanmsk_discards_lines = (bits & 16) != 0;

		const bool expected = (in.override_mode == GSDateRoadOverride::PrimID) && in.date_enabled &&
		                      !in.already_primid && in.device_has_primitive_id && !in.scanmsk_discards_lines;
		EXPECT_EQ(GSDateRoadForcesPrimID(in), expected) << "bits=" << bits;
	}
}

// The process-wide switch: default auto, settable, and its banner name says which arm a log is.
TEST(GSDateRoad, TheProcessOverrideDefaultsToAuto)
{
	EXPECT_EQ(GSDateRoadPolicy::GetOverride(), GSDateRoadOverride::Auto);
	EXPECT_STREQ(GSDateRoadPolicy::Name(), "auto");

	GSDateRoadPolicy::SetOverride(GSDateRoadOverride::PrimID);
	EXPECT_EQ(GSDateRoadPolicy::GetOverride(), GSDateRoadOverride::PrimID);
	EXPECT_STREQ(GSDateRoadPolicy::Name(), "primid (forced)");

	GSDateRoadPolicy::SetOverride(GSDateRoadOverride::Auto);
	EXPECT_EQ(GSDateRoadPolicy::GetOverride(), GSDateRoadOverride::Auto);
}
