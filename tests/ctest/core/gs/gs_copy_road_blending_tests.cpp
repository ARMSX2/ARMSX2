// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the copy-road blending cap (GS/Renderers/Common/GSCopyRoadBlendingPolicy.h).
//
// One title, Splashdown, renders at Minimum blending accuracy on a device that has to read the
// render target from a per-draw copy of it, because on that road accurate blending costs it 1,177
// target copies and 8.48 ms a frame and the owner passed the Minimum picture on 2026-09-24.
// Everywhere else it renders exactly as it did.
//
// So almost all of what these tests are for is the "everywhere else". The change is visible on one
// road, on one title; every other combination of device facts and every other title has to be
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

	// The road the owner judged the picture on: no in-tile read, no texture barrier, and the
	// no-barrier fallback copies once per draw. Every Adreno under ARMSX2 #442 on Vulkan, a GLES
	// part with no fetch extension, and the M2 with OverrideTextureBarriers=0 -- which is how the
	// M2 reproduces the road for the byte-identity gate.
	constexpr GSCopyRoadBlendingInputs CopyRoad()
	{
		return GSCopyRoadBlendingInputs();
	}

	// The in-tile destination read: Mali with ARM fetch or Vulkan rasterization-order attachment
	// access, Metal programmable blending. The read is free, so there is nothing to buy.
	constexpr GSCopyRoadBlendingInputs FetchRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.framebuffer_fetch = true;
		in.texture_barrier = true;
		return in;
	}

	// A real texture barrier. The M2 at its defaults and every desktop GPU. Also where an Adreno
	// lands the day the declared-read road ships and it keeps its barriers.
	constexpr GSCopyRoadBlendingInputs BarrierRoad()
	{
		GSCopyRoadBlendingInputs in;
		in.texture_barrier = true;
		return in;
	}

	// The per-primitive-group copy: D3D11 and desktop GL without a barrier. It copies, but on a
	// GPU where a copy costs a copy rather than a render-pass boundary.
	constexpr GSCopyRoadBlendingInputs PerPrimitiveCopyRoad()
	{
		GSCopyRoadBlendingInputs in;
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
} // namespace

// The change, on the one road it applies to.
TEST(GSCopyRoadBlending, CopyRoadTakesTheCap)
{
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(CopyRoad())), kMinimum);
}

// The three roads that must not move. These are the byte-identity gate, expressed as facts: the
// M2's own road, the road the handheld with working fetch takes, and the desktop copy road.
TEST(GSCopyRoadBlending, EveryOtherRoadIsUnchanged)
{
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(FetchRoad())), kBasic);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(BarrierRoad())), kBasic);
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(PerPrimitiveCopyRoad())), kBasic);
}

// Every title but one asks for nothing, and a title that asks for nothing is untouched on every
// road including the changed one. This is the other half of the gate: 46 of the 47 corpus dumps.
TEST(GSCopyRoadBlending, ATitleWithNoEntryIsUntouchedEverywhere)
{
	for (int bits = 0; bits < 8; bits++)
	{
		GSCopyRoadBlendingInputs in;
		in.framebuffer_fetch = (bits & 1) != 0;
		in.texture_barrier = (bits & 2) != 0;
		in.multidraw_fb_copy = (bits & 4) != 0;
		in.configured_level = kBasic;

		EXPECT_EQ(CopyRoadBlendingLevel(in), kBasic) << "bits=" << bits;
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
}

// The road predicate on its own, so the four destination reads GSRenderer's device-loss report
// names are four distinct answers here as well.
TEST(GSCopyRoadBlending, OnlyThePerDrawCopyRoadQualifies)
{
	EXPECT_TRUE(FeedbackReadTakesAPerDrawCopy(CopyRoad()));
	EXPECT_FALSE(FeedbackReadTakesAPerDrawCopy(FetchRoad()));
	EXPECT_FALSE(FeedbackReadTakesAPerDrawCopy(BarrierRoad()));
	EXPECT_FALSE(FeedbackReadTakesAPerDrawCopy(PerPrimitiveCopyRoad()));
}

// Fetch outranks the rest. A Vulkan backend clears framebuffer_fetch when the barriers go, so this
// combination does not arise there -- but Metal sets fetch with texture_barrier of its own, and the
// rule should not depend on which backend is asking.
TEST(GSCopyRoadBlending, FetchAloneIsEnoughToLeaveTheRoad)
{
	GSCopyRoadBlendingInputs fetch_without_barrier = CopyRoad();
	fetch_without_barrier.framebuffer_fetch = true;
	EXPECT_FALSE(FeedbackReadTakesAPerDrawCopy(fetch_without_barrier));
	EXPECT_EQ(CopyRoadBlendingLevel(WithSplashdownEntry(fetch_without_barrier)), kBasic);
}

// The cap applies for exactly one combination of the inputs, at every level pair. The sweep is the
// statement the guard devices cannot make: nothing off the per-draw copy road moved, at any
// setting.
TEST(GSCopyRoadBlending, CapsOnlyOnTheCopyRoad)
{
	for (int bits = 0; bits < 8; bits++)
	{
		for (int cap = 0; cap <= kMaximum; cap++)
		{
			for (int level = 0; level <= kMaximum; level++)
			{
				GSCopyRoadBlendingInputs in;
				in.framebuffer_fetch = (bits & 1) != 0;
				in.texture_barrier = (bits & 2) != 0;
				in.multidraw_fb_copy = (bits & 4) != 0;
				in.title_cap = cap;
				in.configured_level = level;

				const bool on_the_road = (bits == 0);
				const int expected = (on_the_road && level > cap) ? cap : level;
				EXPECT_EQ(CopyRoadBlendingLevel(in), expected)
					<< "bits=" << bits << " cap=" << cap << " level=" << level;
			}
		}
	}
}
