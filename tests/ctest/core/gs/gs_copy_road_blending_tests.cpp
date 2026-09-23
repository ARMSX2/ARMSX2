// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the copy-road blending cap (GS/Renderers/Common/GSCopyRoadBlendingPolicy.h).
//
// One title, Splashdown, renders at Minimum blending accuracy on a device that pays for its
// destination read on every draw that takes one -- by copying the render target, or by emitting a
// pipeline barrier. On the copy road that read costs it 1,177 target copies and 8.48 ms a frame;
// on an Adreno 740's barrier road it costs 59,023 barriers and +40%. The Minimum picture was
// judged acceptable, and the capped barrier road measured byte-identical to the copy
// road it was judged on. Where the driver orders the
// read for us the read is free, and the title renders exactly as it did.
//
// So almost all of what these tests are for is the "everywhere else". The change is visible on two
// roads, on one title; every other combination of device facts and every other title has to be
// bit-for-bit what it was, and most of those combinations cannot be observed on any machine this
// suite runs on. Pinning them by name here is what makes "nothing else moved" a statement rather
// than a hope.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSCopyRoadBlendingPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// Blending accuracy levels, as the database and this policy spell them.
	constexpr int kMinimum = 0;
	constexpr int kBasic = 1;
	constexpr int kMaximum = 5;

	// The road the picture was judged on: no in-pass read at all, so the target is cloned
	// once per feedback draw. Every Adreno under ARMSX2 #442 on Vulkan, a GLES part with no fetch
	// extension, and the M2 with OverrideTextureBarriers=0 -- which is how the M2 reproduces the
	// road for the byte-identity gate.
	constexpr GSCopyRoadBlendingInputs CopyRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.road = GSSelfReadRoad::Copy;
		return in;
	}

	// The in-tile destination read: Mali with ARM fetch or Vulkan rasterization-order attachment
	// access, Metal programmable blending. The driver orders it and the read is free.
	constexpr GSCopyRoadBlendingInputs FetchRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.road = GSSelfReadRoad::InPassOrdered;
		return in;
	}

	// A real texture barrier, one per draw. The M2 at its defaults, every desktop GPU, and -- the
	// reason this file changed -- a Turnip a7xx, where the driver answers each barrier with a cache
	// flush and a wait for the pipeline to drain.
	constexpr GSCopyRoadBlendingInputs BarrierRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.road = GSSelfReadRoad::InPassBarrier;
		return in;
	}

	// The declared feedback loop on a driver build measured to order one: the a6xx road. Same
	// InPassOrdered answer as the in-tile read, reached a different way.
	constexpr GSCopyRoadBlendingInputs DriverOrderedLoopRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.road = GSSelfReadRoad::InPassOrdered;
		return in;
	}

	// The per-primitive-group copy: D3D11 and desktop GL without a barrier. It copies, but on a
	// GPU where a copy costs a copy rather than a render-pass boundary.
	constexpr GSCopyRoadBlendingInputs PerPrimitiveCopyRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.road = GSSelfReadRoad::Copy;
		in.multidraw_fb_copy = true;
		return in;
	}

	// Splashdown's entry: cap the level at Minimum, with the player on the shipped default.
	constexpr GSCopyRoadBlendingInputs WithSplashdownEntry(GSCopyRoadBlendingInputs in)
	{
		in.title_cap = kMinimum;
		in.configured_level = kBasic;
		return in;
	}

	// Every road, by the bits a backend publishes, so a sweep can say which one it is talking
	// about. Index order matches the loops below.
	constexpr GSSelfReadRoad kRoads[] = {
		GSSelfReadRoad::Copy, GSSelfReadRoad::InPassBarrier, GSSelfReadRoad::InPassOrdered};
} // namespace

// The change, on the two roads it applies to.
TEST(GSCopyRoadBlending, CopyRoadTakesTheCap)
{
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(CopyRoad())), kMinimum);
}

// A barrier is not a free read on a tiler, so the barrier road takes the cap too.
TEST(GSCopyRoadBlending, BarrierRoadTakesTheCap)
{
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(BarrierRoad())), kMinimum);
}

// The roads that must not move. The driver-ordered read is free by either entrance, and the
// immediate-mode per-primitive copy is a blit.
TEST(GSCopyRoadBlending, EveryDriverOrderedRoadIsUnchanged)
{
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(FetchRoad())), kBasic);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(DriverOrderedLoopRoad())), kBasic);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(PerPrimitiveCopyRoad())), kBasic);
}

// Every title but one asks for nothing, and a title that asks for nothing is untouched on every
// road including the changed ones. This is the other half of the gate: 46 of the 47 corpus dumps.
TEST(GSCopyRoadBlending, ATitleWithNoEntryIsUntouchedEverywhere)
{
	for (const GSSelfReadRoad road : kRoads)
	{
		for (int multidraw = 0; multidraw < 2; multidraw++)
		{
			GSCopyRoadBlendingInputs in;
			in.road = road;
			in.multidraw_fb_copy = (multidraw != 0);
			in.configured_level = kBasic;

			EXPECT_EQ(CopyRoadBlendingLevel(in), kBasic)
				<< "road=" << static_cast<int>(road) << " multidraw=" << multidraw;
		}
	}
}

// A ceiling, not a setting. A player who already asked for less than the cap keeps what they asked
// for; the cap only ever lowers.
TEST(GSCopyRoadBlending, TheCapOnlyLowers)
{
	GSCopyRoadBlendingInputs already_lower = CopyRoad();
	already_lower.title_cap = kBasic;
	already_lower.configured_level = kMinimum;
	EXPECT_EQ(CopyRoadBlendingLevel(already_lower), kMinimum);

	GSCopyRoadBlendingInputs above = CopyRoad();
	above.title_cap = kBasic;
	above.configured_level = kMaximum;
	EXPECT_EQ(CopyRoadBlendingLevel(above), kBasic);

	GSCopyRoadBlendingInputs above_on_barriers = BarrierRoad();
	above_on_barriers.title_cap = kBasic;
	above_on_barriers.configured_level = kMaximum;
	EXPECT_EQ(CopyRoadBlendingLevel(above_on_barriers), kBasic);
}

// The road predicate on its own, so the four destination reads GSRenderer's device-loss report
// names come out as the two answers this file has: the reads we pay for per draw, and the reads
// the driver gives us.
TEST(GSCopyRoadBlending, EveryRoadThatChargesPerDrawQualifies)
{
	EXPECT_TRUE(DestinationReadCostsPerDraw(CopyRoad()));
	EXPECT_TRUE(DestinationReadCostsPerDraw(BarrierRoad()));
	EXPECT_FALSE(DestinationReadCostsPerDraw(FetchRoad()));
	EXPECT_FALSE(DestinationReadCostsPerDraw(DriverOrderedLoopRoad()));
	EXPECT_FALSE(DestinationReadCostsPerDraw(PerPrimitiveCopyRoad()));
}

// The road the cap reads is the road the backend published, by the same three bits GS.cpp hands
// it. A device on a driver-ordered read reaches that answer two ways and must keep its picture on
// both.
TEST(GSCopyRoadBlending, ThePublishedBitsNameTheRoadTheCapActsOn)
{
	// no in-tile read, no barrier -> the copy road.
	EXPECT_EQ(GSSelfReadRoadFromPublishedBits(false, false, false), GSSelfReadRoad::Copy);
	// a barrier and nothing ordering it -> we pay per draw.
	EXPECT_EQ(GSSelfReadRoadFromPublishedBits(false, true, false), GSSelfReadRoad::InPassBarrier);
	// the in-tile read.
	EXPECT_EQ(GSSelfReadRoadFromPublishedBits(true, true, false), GSSelfReadRoad::InPassOrdered);
	// the declared loop on a driver measured to order it.
	EXPECT_EQ(GSSelfReadRoadFromPublishedBits(false, true, true), GSSelfReadRoad::InPassOrdered);

	GSCopyRoadBlendingInputs in;
	in.road = GSSelfReadRoadFromPublishedBits(false, true, false);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(in)), kMinimum);

	in.road = GSSelfReadRoadFromPublishedBits(false, true, true);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(in)), kBasic);
}

// The cap applies for exactly the roads that charge, at every level pair. The sweep is the
// statement the guard devices cannot make: nothing off those roads moved, at any setting.
TEST(GSCopyRoadBlending, CapsOnlyWhereTheReadCostsSomething)
{
	for (const GSSelfReadRoad road : kRoads)
	{
		for (int multidraw = 0; multidraw < 2; multidraw++)
		{
			for (int cap = 0; cap <= kMaximum; cap++)
			{
				for (int level = 0; level <= kMaximum; level++)
				{
					GSCopyRoadBlendingInputs in;
					in.road = road;
					in.multidraw_fb_copy = (multidraw != 0);
					in.title_cap = cap;
					in.configured_level = level;

					const bool charges = (road == GSSelfReadRoad::InPassBarrier) ||
					                     (road == GSSelfReadRoad::Copy && multidraw == 0);
					const int expected = (charges && level > cap) ? cap : level;
					EXPECT_EQ(CopyRoadBlendingLevel(in), expected)
						<< "road=" << static_cast<int>(road) << " multidraw=" << multidraw
						<< " cap=" << cap << " level=" << level;
				}
			}
		}
	}
}
