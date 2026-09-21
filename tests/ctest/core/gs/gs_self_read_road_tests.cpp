// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the self-read road decision (GS/Renderers/Common/GSSelfReadRoadPolicy.h).
//
// A draw that reads the render target it is writing takes one of three roads: clone the target,
// read it in the pass with our own barriers ordering it, or read it in the pass and let the driver
// order it. Which one a device takes used to be four expressions spread over 150 lines of
// GSDeviceVK::CheckFeatures, and reading them in order was the only way to find out.
//
// What these tests are mostly for is the no-change half. Campaign gs-adreno-inpass-read adds a
// third road for ONE part -- Adreno under Turnip, declaring an attachment feedback loop so the
// driver runs the pass untiled with a coherent destination read -- and every other device must come
// out of the function with exactly the bits it had before. None of those devices is the one that
// takes the new road, so "it still works on desktop" cannot be checked by running the new arm.
// Hence a pure function and the old answers pinned here by name.
//
// The header carries the same matrix as static_asserts, which is what actually stops a regression
// compiling. These exist so a failure says which DEVICE moved rather than which line.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// Adreno on Turnip, shipped: the driver database's UseRenderTargetCopyForFeedback with the
	// override on auto. It advertises both extensions and neither helps it.
	constexpr GSSelfReadRoadInputs TurnipShipped()
	{
		GSSelfReadRoadInputs in;
		in.in_tile_read_available = true;
		in.layout_road_available = true;
		in.roaa_available = true;
		in.rt_self_read_is_broken = true;
		return in;
	}

	// Mali-G615 (RG 477V) at its defaults: exempt from the destination-read deny, barriers on, the
	// in-tile read on, and the layout spelling refused because the rasterization-order extension is
	// present.
	constexpr GSSelfReadRoadInputs MaliDefault()
	{
		GSSelfReadRoadInputs in;
		in.in_tile_read_available = true;
		in.layout_road_available = true;
		in.roaa_available = true;
		return in;
	}

	// A desktop GPU: the feedback-loop-layout extension, no rasterization-order extension, real
	// barriers. This is already the declared-loop SPELLING -- the pipeline create flag fires here
	// every day -- with our barriers doing the ordering.
	constexpr GSSelfReadRoadInputs Desktop()
	{
		GSSelfReadRoadInputs in;
		in.layout_road_available = true;
		return in;
	}

	constexpr GSSelfReadRoadInputs WithArm(GSSelfReadRoadInputs in, GSSelfReadArm arm)
	{
		in.arm = static_cast<u8>(arm);
		return in;
	}

	// The driver database recognising a driver build that was measured to order overlapping
	// self-reads inside a declared feedback loop. Today that is a Turnip build carrying the a6xx
	// feedback-loop fix, and nothing else.
	constexpr GSSelfReadRoadInputs WithDriverFact(GSSelfReadRoadInputs in)
	{
		in.driver_orders_declared_loop = true;
		return in;
	}
} // namespace

// --- no change off the arm ----------------------------------------------------------------------

TEST(GSSelfReadRoad, TurnipShippedTakesTheCopyRoad)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(TurnipShipped());
	EXPECT_EQ(d.road, GSSelfReadRoad::Copy);
	EXPECT_FALSE(d.texture_barrier);
	EXPECT_FALSE(d.in_tile_read);
	EXPECT_FALSE(d.force_feedback_loop_layout);
	EXPECT_FALSE(d.orders_overlapping_prims);
	EXPECT_FALSE(d.arm_applied);
	EXPECT_FALSE(d.arm_unavailable);
}

TEST(GSSelfReadRoad, TurnipWithBarriersForcedOnKeepsTheDocumentedABRoad)
{
	// OverrideTextureBarriers=1 is the documented way back onto the in-tile road for A/B work and
	// for a driver revision that fixes the read. The campaign must not have taken that away.
	GSSelfReadRoadInputs in = TurnipShipped();
	in.override_texture_barriers = 1;

	const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
	EXPECT_TRUE(d.texture_barrier);
	EXPECT_TRUE(d.in_tile_read);
	EXPECT_EQ(d.road, GSSelfReadRoad::InPassOrdered);
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::InputAttachment);
	EXPECT_FALSE(d.force_feedback_loop_layout);
}

TEST(GSSelfReadRoad, MaliDefaultKeepsTheInTileRead)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(MaliDefault());
	EXPECT_TRUE(d.texture_barrier);
	EXPECT_TRUE(d.in_tile_read);
	EXPECT_EQ(d.road, GSSelfReadRoad::InPassOrdered);
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::InputAttachment);
	EXPECT_FALSE(d.orders_overlapping_prims);
}

TEST(GSSelfReadRoad, DesktopKeepsTheBarrierOrderedLayoutRoad)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(Desktop());
	EXPECT_TRUE(d.texture_barrier);
	EXPECT_FALSE(d.in_tile_read);
	EXPECT_EQ(d.road, GSSelfReadRoad::InPassBarrier);
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::FeedbackLoopLayout);
	EXPECT_FALSE(d.orders_overlapping_prims);
	EXPECT_FALSE(d.force_feedback_loop_layout);
}

TEST(GSSelfReadRoad, BarriersForcedOffIsTheCopyRoadEverywhere)
{
	for (GSSelfReadRoadInputs in : {TurnipShipped(), MaliDefault(), Desktop()})
	{
		in.override_texture_barriers = 0;
		const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
		EXPECT_FALSE(d.texture_barrier);
		EXPECT_FALSE(d.in_tile_read);
		EXPECT_EQ(d.road, GSSelfReadRoad::Copy);
	}
}

// --- the arm ------------------------------------------------------------------------------------

TEST(GSSelfReadRoad, ArmDeclaresTheLoopAndTurnsTheInTileReadOff)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared));
	EXPECT_TRUE(d.arm_applied);
	EXPECT_TRUE(d.texture_barrier);
	EXPECT_TRUE(d.force_feedback_loop_layout);
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::FeedbackLoopLayout);
	EXPECT_EQ(d.road, GSSelfReadRoad::InPassOrdered);
	EXPECT_TRUE(d.orders_overlapping_prims);

	// The in-tile read stays off on purpose. Leaving it on would compile texelFetch into the
	// shader while the render pass skipped its self-dependency and the pipeline carried the
	// rasterization-order blend flag -- two spellings of one read in one binary, which is how the
	// July 2026 arm became unreadable.
	EXPECT_FALSE(d.in_tile_read);
}

TEST(GSSelfReadRoad, ArmTwoDeclaresTheSameLoopAndKeepsTheBarriers)
{
	const GSSelfReadRoadDecision declared =
		DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared));
	const GSSelfReadRoadDecision kept =
		DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::DeclaredKeepBarriers));

	// Everything about the declaration matches; the ordering claim is the whole difference, which
	// is what makes the pair a diagnostic.
	EXPECT_EQ(declared.force_feedback_loop_layout, kept.force_feedback_loop_layout);
	EXPECT_EQ(declared.spelling, kept.spelling);
	EXPECT_EQ(declared.texture_barrier, kept.texture_barrier);
	EXPECT_EQ(declared.in_tile_read, kept.in_tile_read);
	EXPECT_TRUE(declared.orders_overlapping_prims);
	EXPECT_FALSE(kept.orders_overlapping_prims);
	EXPECT_EQ(kept.road, GSSelfReadRoad::InPassBarrier);
}

TEST(GSSelfReadRoad, ArmWithoutTheLayoutExtensionIsInertAndSaysSo)
{
	GSSelfReadRoadInputs in = TurnipShipped();
	in.layout_road_available = false;

	const GSSelfReadRoadDecision d = DecideSelfReadRoad(WithArm(in, GSSelfReadArm::Declared));
	EXPECT_FALSE(d.arm_applied);
	EXPECT_TRUE(d.arm_unavailable);
	EXPECT_EQ(d.road, GSSelfReadRoad::Copy);
	EXPECT_FALSE(d.force_feedback_loop_layout);
}

TEST(GSSelfReadRoad, AnExplicitBarrierOffStillOutranksTheArm)
{
	GSSelfReadRoadInputs in = TurnipShipped();
	in.override_texture_barriers = 0;

	const GSSelfReadRoadDecision d = DecideSelfReadRoad(WithArm(in, GSSelfReadArm::Declared));
	EXPECT_FALSE(d.arm_applied);
	EXPECT_TRUE(d.arm_unavailable);
	EXPECT_FALSE(d.texture_barrier);
}

TEST(GSSelfReadRoad, ArmOffNeverReportsAnythingUnavailable)
{
	for (const GSSelfReadRoadInputs& in : {TurnipShipped(), MaliDefault(), Desktop()})
	{
		const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
		EXPECT_FALSE(d.arm_applied);
		EXPECT_FALSE(d.arm_unavailable);
	}
}

TEST(GSSelfReadRoad, RoadNameNamesBothAxes)
{
	// The device record quotes this string, so it has to distinguish the two spellings of the
	// in-pass read rather than just the road.
	EXPECT_STRNE(GSSelfReadRoadName(DecideSelfReadRoad(MaliDefault())),
		GSSelfReadRoadName(DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared))));
	EXPECT_STRNE(GSSelfReadRoadName(DecideSelfReadRoad(Desktop())),
		GSSelfReadRoadName(DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared))));
	EXPECT_STRNE(GSSelfReadRoadName(DecideSelfReadRoad(TurnipShipped())),
		GSSelfReadRoadName(DecideSelfReadRoad(MaliDefault())));
}

// ---------------------------------------------------------------------------------------------
// The ordering claim is about the DRIVER, not the road.
//
// Declaring a feedback loop buys the layout and the validity relaxation. It does not buy ordering
// between overlapping fragments that sample the attachment they write. Turnip implements that
// ordering anyway (forced sysmem + FLUSH_PER_OVERLAP_AND_OVERWRITE); Honeykrisp advertises the same
// layout extension and has no ordering machinery at all, and moves 70 of 94 corpus cells when the
// claim is made on it.
//
// Note Honeykrisp's inputs are Desktop()'s inputs. That is the point: this is not one odd part, it
// is every device that has the layout extension and no rasterization-order extension -- which is
// most of them, desktop included. They work today because our barriers do the ordering. The claim
// is what removes those barriers, so nothing but an explicit arm may make it.

TEST(GSSelfReadRoad, ArmOffNeverClaimsOrdering)
{
	for (int bits = 0; bits < 16; bits++)
	{
		for (const s8 override_barriers : {s8(-1), s8(0), s8(1)})
		{
			GSSelfReadRoadInputs in;
			in.in_tile_read_available = (bits & 1) != 0;
			in.layout_road_available = (bits & 2) != 0;
			in.roaa_available = (bits & 4) != 0;
			in.rt_self_read_is_broken = (bits & 8) != 0;
			in.override_texture_barriers = override_barriers;

			const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
			EXPECT_FALSE(d.orders_overlapping_prims)
				<< "device bits " << bits << ", override " << int(override_barriers)
				<< ": no combination of extensions earns the ordering claim, because none of them "
				   "promise it";
		}
	}
}

TEST(GSSelfReadRoad, ArmTwoIsArmOneWithoutTheOrderingClaim)
{
	// What makes arm 2 a usable control: identical in every bit the pass is built from, so a
	// corpus difference between the two arms isolates the ordering and nothing else.
	for (const GSSelfReadRoadInputs& in : {TurnipShipped(), Desktop()})
	{
		const GSSelfReadRoadDecision one = DecideSelfReadRoad(WithArm(in, GSSelfReadArm::Declared));
		const GSSelfReadRoadDecision two =
			DecideSelfReadRoad(WithArm(in, GSSelfReadArm::DeclaredKeepBarriers));

		EXPECT_TRUE(one.orders_overlapping_prims);
		EXPECT_FALSE(two.orders_overlapping_prims);

		EXPECT_EQ(one.spelling, two.spelling);
		EXPECT_EQ(one.texture_barrier, two.texture_barrier);
		EXPECT_EQ(one.in_tile_read, two.in_tile_read);
		EXPECT_EQ(one.force_feedback_loop_layout, two.force_feedback_loop_layout);
		EXPECT_EQ(one.arm_applied, two.arm_applied);
		EXPECT_EQ(one.arm_unavailable, two.arm_unavailable);
	}
}

TEST(GSSelfReadRoad, DesktopShapeTakesTheLayoutSpellingAndOrdersWithBarriers)
{
	// Honeykrisp's shape is this shape. The road is correct here only because the barriers are
	// still being emitted -- which is exactly what the ordering claim would drop.
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(Desktop());
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::FeedbackLoopLayout);
	EXPECT_TRUE(d.texture_barrier);
	EXPECT_FALSE(d.orders_overlapping_prims);
}

// --- the driver fact ------------------------------------------------------------------------------
//
// The road the campaign measured, reached with no setting touched. What earns it is not an
// extension and not a vendor: it is the driver build saying, in driverInfo, that it is one we
// measured. Everything below is about keeping that narrow -- a device that does not carry the fix
// must come out of this function exactly where it was.

TEST(GSSelfReadRoad, TheDriverFactTakesArmOnesRoadWithNoKeySet)
{
	const GSSelfReadRoadDecision fact = DecideSelfReadRoad(WithDriverFact(TurnipShipped()));
	const GSSelfReadRoadDecision arm1 = DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared));

	// Bit for bit, because arm 1 is what E11/E12 measured and the fact is the claim that
	// measurement licensed. A fact road that differed anywhere would be an unmeasured road.
	EXPECT_EQ(fact.road, arm1.road);
	EXPECT_EQ(fact.spelling, arm1.spelling);
	EXPECT_EQ(fact.texture_barrier, arm1.texture_barrier);
	EXPECT_EQ(fact.in_tile_read, arm1.in_tile_read);
	EXPECT_EQ(fact.force_feedback_loop_layout, arm1.force_feedback_loop_layout);
	EXPECT_EQ(fact.orders_overlapping_prims, arm1.orders_overlapping_prims);
	EXPECT_EQ(fact.loop_declared, arm1.loop_declared);

	EXPECT_EQ(fact.road, GSSelfReadRoad::InPassOrdered);
	EXPECT_TRUE(fact.orders_overlapping_prims);
	EXPECT_TRUE(fact.loop_declared);
}

// The RT-copy workaround is the reason every Adreno is on the copy road today, and the fact has to
// outrank it or the fixed driver is stuck behind a rule about the broken one.
TEST(GSSelfReadRoad, TheDriverFactOutranksTheRtCopyWorkaround)
{
	const GSSelfReadRoadInputs in = WithDriverFact(TurnipShipped());
	ASSERT_TRUE(in.rt_self_read_is_broken);

	const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
	EXPECT_NE(d.road, GSSelfReadRoad::Copy);
	EXPECT_TRUE(d.texture_barrier);
}

// Nobody asked for an arm, so neither arm diagnostic may fire. arm_unavailable prints an error
// naming a setting the user never set, and arm_applied is what the depth probe used to key on.
TEST(GSSelfReadRoad, TheDriverFactIsNotAnArm)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(WithDriverFact(TurnipShipped()));
	EXPECT_FALSE(d.arm_applied);
	EXPECT_FALSE(d.arm_unavailable);
	EXPECT_TRUE(d.selected_by_driver_fact);
}

// Both levers that stop the arm stop the fact, and they stop it the same way.
TEST(GSSelfReadRoad, TheDriverFactStillLosesToBarriersForcedOff)
{
	GSSelfReadRoadInputs in = WithDriverFact(TurnipShipped());
	in.override_texture_barriers = 0;

	const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
	EXPECT_EQ(d.road, GSSelfReadRoad::Copy);
	EXPECT_FALSE(d.texture_barrier);
	EXPECT_FALSE(d.orders_overlapping_prims);
	EXPECT_FALSE(d.loop_declared);
}

TEST(GSSelfReadRoad, TheDriverFactNeedsTheLayoutExtension)
{
	GSSelfReadRoadInputs in = WithDriverFact(TurnipShipped());
	in.layout_road_available = false;

	// Exactly the answer this device shape gets today, which is the copy road.
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
	const GSSelfReadRoadDecision without = DecideSelfReadRoad([] {
		GSSelfReadRoadInputs base = TurnipShipped();
		base.layout_road_available = false;
		return base;
	}());
	EXPECT_EQ(d.road, without.road);
	EXPECT_EQ(d.spelling, without.spelling);
	EXPECT_EQ(d.texture_barrier, without.texture_barrier);
	EXPECT_EQ(d.orders_overlapping_prims, without.orders_overlapping_prims);
	EXPECT_FALSE(d.loop_declared);
}

// The key still wins where it is set, and arm 2 is why. Arm 2 declares the same loop and keeps the
// barriers, so it is the reference picture the ordering claim is measured against; on our own
// driver -- the only one carrying the fact -- it has to stay reachable or there is nothing to
// compare arm 1 with.
TEST(GSSelfReadRoad, TheKeyStillOutranksTheDriverFact)
{
	const GSSelfReadRoadInputs in = WithDriverFact(TurnipShipped());

	const GSSelfReadRoadDecision two = DecideSelfReadRoad(WithArm(in, GSSelfReadArm::DeclaredKeepBarriers));
	EXPECT_EQ(two.road, GSSelfReadRoad::InPassBarrier);
	EXPECT_FALSE(two.orders_overlapping_prims) << "arm 2 exists to keep the barriers; the fact must not re-add the claim";
	EXPECT_TRUE(two.arm_applied);
	EXPECT_FALSE(two.selected_by_driver_fact);

	const GSSelfReadRoadDecision one = DecideSelfReadRoad(WithArm(in, GSSelfReadArm::Declared));
	EXPECT_EQ(one.road, GSSelfReadRoad::InPassOrdered);
	EXPECT_TRUE(one.arm_applied);
	EXPECT_FALSE(one.selected_by_driver_fact);
}

// Every other device, with the fact absent -- which is every device that is not running our driver
// build. Same sweep as ArmOffNeverClaimsOrdering, and it is the same guarantee: an extension is
// still never a reason to drop the barriers.
TEST(GSSelfReadRoad, WithoutTheFactNothingChangedAnywhere)
{
	for (int bits = 0; bits < 16; bits++)
	{
		for (const s8 override_barriers : {s8(-1), s8(0), s8(1)})
		{
			GSSelfReadRoadInputs in;
			in.in_tile_read_available = (bits & 1) != 0;
			in.layout_road_available = (bits & 2) != 0;
			in.roaa_available = (bits & 4) != 0;
			in.rt_self_read_is_broken = (bits & 8) != 0;
			in.override_texture_barriers = override_barriers;
			ASSERT_FALSE(in.driver_orders_declared_loop) << "the fact must default to absent";

			const GSSelfReadRoadDecision d = DecideSelfReadRoad(in);
			EXPECT_FALSE(d.orders_overlapping_prims)
				<< "device bits " << bits << ", override " << int(override_barriers);
			EXPECT_FALSE(d.loop_declared)
				<< "device bits " << bits << ", override " << int(override_barriers);
			EXPECT_FALSE(d.selected_by_driver_fact)
				<< "device bits " << bits << ", override " << int(override_barriers);
		}
	}
}

// Honeykrisp's shape with the fact set is not a case that can happen -- the fact names a Turnip
// build -- but the function must not have a back door either: what selects the road is the input,
// and on the M2 that input is false, so the M2 is on Desktop()'s answer and stays there.
TEST(GSSelfReadRoad, TheM2ShapeIsUnchangedBecauseItsDriverCarriesNoFact)
{
	const GSSelfReadRoadDecision d = DecideSelfReadRoad(Desktop());
	EXPECT_EQ(d.road, GSSelfReadRoad::InPassBarrier);
	EXPECT_EQ(d.spelling, GSSelfReadSpelling::FeedbackLoopLayout);
	EXPECT_FALSE(d.orders_overlapping_prims);
	EXPECT_FALSE(d.loop_declared);
}

// The banner. A device round quotes this line, and the declared road now has two entrances, so the
// two must not print the same phrase -- a record that cannot say what put the machine on the road
// is a record of an unknown configuration.
TEST(GSSelfReadRoad, TheBannerSaysWhichEntranceWasUsed)
{
	const char* by_fact = GSSelfReadRoadName(DecideSelfReadRoad(WithDriverFact(TurnipShipped())));
	const char* by_key =
		GSSelfReadRoadName(DecideSelfReadRoad(WithArm(TurnipShipped(), GSSelfReadArm::Declared)));
	EXPECT_STRNE(by_fact, by_key);
	EXPECT_STRNE(by_fact, GSSelfReadRoadName(DecideSelfReadRoad(Desktop())));
	EXPECT_STRNE(by_fact, GSSelfReadRoadName(DecideSelfReadRoad(TurnipShipped())));

	// Desktop reaches the layout spelling without declaring anything, so its phrase must not gain a
	// reason it did not have.
	EXPECT_STREQ(GSSelfReadRoadName(DecideSelfReadRoad(Desktop())),
		"in-pass, barrier-ordered, declared feedback loop");
}
