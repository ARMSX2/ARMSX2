// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu palette-cycle (channel-shuffle) signature: GSTilePaletteCycleRun.
//
// Gran Turismo 4 splits its frame buffer into channels by re-reading it as a PSMT8 texture through
// a CLUT it renders itself, and cycles the whole arrangement -- render the palette, consume it,
// render it again. Executed literally that is 85% of the Online Public Beta's frame: 139 cycles of
// eleven draws. Classic recognises the idiom and substitutes one channel-fetch draw for the whole
// run; this suite is about the recogniser.
//
// ⚠️ THE SHAPE PINNED HERE WAS MEASURED, NOT DESIGNED. The first version of this signature asked
// for the closed loop and Classic's draw shape of the SAME draw, and was empty on the population:
// per cycle it is eight consumer draws with the shape, then a producer that closes the loop and
// does not have the ABE the design assumed. So the loop is a fact about the RUN and the shape is a
// fact about the DRAW. The cases below carry that arrangement explicitly, because a future
// simplification back to a per-draw conjunction would pass every test that only exercised runs
// where the two coincide.
//
// The populations pinned here:
//
//  - THE GT4 SHAPE. Consumers, then a producer that destroys the palette, repeating: arms once and
//    elides everything after, however long it runs.
//  - A RUN THAT NEVER CLOSES THE LOOP. A game consuming a rendered palette that nobody is
//    rewriting is not a cycle, however many draws it takes. It must never arm.
//  - A ONE-OFF read of a gathered palette. Below the threshold, so it is drawn.
//  - THE NFSU2 SHAPE. "Consecutive channel shuffles with blending, reducing the alpha channel over
//    time" (GSRendererHW.cpp's own words) is an ACCUMULATING run that must not be collapsed. What
//    saves it is that the game changes FBMSK along the way, which is why the run key carries FBMSK
//    and why a change starts a new run rather than extending one.
//  - THE sotc / xenosaga SHAPE. Those titles load 1,300-1,800 palettes a frame and the CLUT gather
//    serves none of them, so pal_record is always zero and the shape clause cannot hold whatever
//    else does. Structural safety, not a threshold.
//  - WHICH SUBSTITUTE a run asks for, off FBMSK alone.

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

namespace
{
	constexpr GSTileSurfaceId kOwner = 15;
	constexpr GSTileSurfaceId kOther = 4;

	// A consumer: the gather served its palette, it is a paletted sprite, and it is not the draw
	// that destroys the palette. Eight of these open every gt4opb cycle.
	GSTilePaletteCycleDraw Consumer(u32 fbmsk = 0xFF000000u, GSTileSurfaceId owner = kOwner)
	{
		GSTilePaletteCycleDraw d;
		d.pal_on_device = true;
		d.paletted_index = true;
		d.sprite = true;
		d.destroys_pal_source = false;
		d.fbmsk = fbmsk;
		d.pal_owner = owner;
		return d;
	}

	// The producer that closes the loop: same shape, and it is about to overwrite the pages its own
	// palette was gathered from. One of these per gt4opb cycle.
	GSTilePaletteCycleDraw Producer(u32 fbmsk = 0xFF000000u, GSTileSurfaceId owner = kOwner)
	{
		GSTilePaletteCycleDraw d = Consumer(fbmsk, owner);
		d.destroys_pal_source = true;
		return d;
	}

	// Anything that is not the idiom at all: an ordinary textured draw.
	GSTilePaletteCycleDraw Ordinary()
	{
		return GSTilePaletteCycleDraw{};
	}

	using V = GSTilePaletteCycleVerdict;
} // namespace

TEST(TileGpuPaletteCycle, AnEmptyRunPassesEverything)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(run.Observe(Ordinary()), V::Pass) << "draw " << i;
	EXPECT_FALSE(run.armed());
	EXPECT_EQ(run.length(), 0u);
}

TEST(TileGpuPaletteCycle, ArmingThresholdIsFour)
{
	EXPECT_EQ(GSTilePaletteCycleRun::kArmAfter, 4u);
}

// The measured gt4opb cycle: eight consumers, then a producer that destroys the palette, then two
// more producers, repeating 139 times under one FBMSK and one palette owner.
TEST(TileGpuPaletteCycle, TheMeasuredGt4CycleArmsOnceAndElidesTheRest)
{
	GSTilePaletteCycleRun run;
	// The eight consumers reach the length threshold at draw four, but nothing has closed the loop
	// yet -- so far this is indistinguishable from a game reading a rendered palette.
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(run.Observe(Consumer()), V::Building) << "consumer " << i;
	EXPECT_FALSE(run.armed());
	EXPECT_TRUE(run.length() == 8u);
	// The ninth draw rewrites the palette it just read. That is the cycle.
	EXPECT_EQ(run.Observe(Producer()), V::Arm);
	EXPECT_TRUE(run.armed());
	EXPECT_TRUE(run.loop_seen());
	// 138 more cycles, and nothing re-arms.
	for (int c = 0; c < 138; c++)
	{
		EXPECT_EQ(run.Observe(Consumer()), V::Elide);
		EXPECT_EQ(run.Observe(Consumer()), V::Elide);
		EXPECT_EQ(run.Observe(Producer()), V::Elide);
	}
	EXPECT_EQ(run.length(), 9u + 138u * 3u);
}

// The whole point of S1 being a separate conjunct: a game that consumes a rendered palette nobody
// is rewriting is not cycling anything, and there is nothing to collapse.
TEST(TileGpuPaletteCycle, ARunThatNeverClosesTheLoopNeverArms)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 500; i++)
		EXPECT_EQ(run.Observe(Consumer()), V::Building) << "draw " << i;
	EXPECT_FALSE(run.armed());
	EXPECT_FALSE(run.loop_seen());
}

// A run may not arm on the loop alone either: one draw destroying a palette it read is an ordinary
// thing for a game to do once.
TEST(TileGpuPaletteCycle, AOneOffLoopClosingDrawDoesNotArm)
{
	GSTilePaletteCycleRun run;
	EXPECT_EQ(run.Observe(Producer()), V::Building);
	EXPECT_EQ(run.Observe(Ordinary()), V::Pass);
	EXPECT_FALSE(run.armed());
	EXPECT_EQ(run.length(), 0u);
	EXPECT_FALSE(run.loop_seen());
}

TEST(TileGpuPaletteCycle, AnOrdinaryDrawBetweenThemStartsTheCountOver)
{
	GSTilePaletteCycleRun run;
	EXPECT_EQ(run.Observe(Producer()), V::Building);
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
	EXPECT_EQ(run.Observe(Ordinary()), V::Pass);
	// Three more do not finish what the first three started, and the loop the first run saw does
	// not carry into the second.
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
	EXPECT_FALSE(run.armed());
	EXPECT_FALSE(run.loop_seen());
}

// NFS Underground 2: consecutive channel shuffles that reduce the alpha channel over time. The run
// is only safe to collapse within one FBMSK, and the game changes FBMSK as it goes.
TEST(TileGpuPaletteCycle, AnFbmskChangeStartsANewRunAndNeverCollapsesAcrossIt)
{
	GSTilePaletteCycleRun run;
	EXPECT_EQ(run.Observe(Producer(0x00000000u)), V::Building);
	for (int i = 0; i < 2; i++)
		EXPECT_EQ(run.Observe(Consumer(0x00000000u)), V::Building);
	// Three draws under one mask, one of them loop-closing, then the mask moves. The count starts
	// again AND the loop the first run saw is forgotten with it, so the next draw is the first
	// draw of a run that has closed nothing.
	EXPECT_EQ(run.Observe(Consumer(0xFF000000u)), V::Building);
	EXPECT_EQ(run.length(), 1u);
	EXPECT_EQ(run.fbmsk(), 0xFF000000u);
	EXPECT_FALSE(run.loop_seen());
	for (int i = 0; i < 8; i++)
		EXPECT_EQ(run.Observe(Consumer(0xFF000000u)), V::Building);
	EXPECT_FALSE(run.armed());
	// A run that closes its OWN loop under the second mask arms under that mask alone.
	EXPECT_EQ(run.Observe(Producer(0xFF000000u)), V::Arm);
	EXPECT_EQ(run.fbmsk(), 0xFF000000u);
}

TEST(TileGpuPaletteCycle, AnArmedRunIsBrokenByAnFbmskChange)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer(0x00000000u));
	ASSERT_EQ(run.Observe(Producer(0x00000000u)), V::Arm);
	ASSERT_TRUE(run.armed());
	// The mask moves: the armed run is over, and the new one has to earn its own arming.
	EXPECT_EQ(run.Observe(Consumer(0x00FFFFFFu)), V::Building);
	EXPECT_FALSE(run.armed());
}

// Tomb Raider: Underworld puts R, G and B in three separate palettes. A run keyed on FBMSK alone
// would elide a second palette's draws under the first's substitute.
TEST(TileGpuPaletteCycle, AnOwnerChangeStartsANewRun)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer(0u, kOwner));
	ASSERT_EQ(run.Observe(Producer(0u, kOwner)), V::Arm);
	EXPECT_EQ(run.Observe(Consumer(0u, kOther)), V::Building);
	EXPECT_EQ(run.owner(), kOther);
	EXPECT_FALSE(run.armed());
	EXPECT_FALSE(run.loop_seen());
}

// sotc and xenosaga load 1,300-1,800 palettes a frame and the CLUT gather serves none of them, so
// there is no device record and pal_record is zero on every draw. The shape clause cannot hold,
// whatever the rest of the draw looks like -- a much stronger safety property than the threshold.
TEST(TileGpuPaletteCycle, WithoutADevicePaletteTheShapeCannotHold)
{
	GSTilePaletteCycleRun run;
	GSTilePaletteCycleDraw d = Producer();
	d.pal_on_device = false;
	for (int i = 0; i < 64; i++)
		EXPECT_EQ(run.Observe(d), V::Pass) << "draw " << i;
	EXPECT_FALSE(run.armed());
}

// Each shape clause on its own is enough to refuse the draw. Stated one at a time so a clause that
// stops being read shows up here rather than as a moved pixel on a title nobody runs.
TEST(TileGpuPaletteCycle, EveryShapeClauseRefusesOnItsOwn)
{
	struct Case
	{
		const char* name;
		void (*mutate)(GSTilePaletteCycleDraw&);
	};
	const Case cases[] = {
		{"the gather did not serve it", [](GSTilePaletteCycleDraw& d) { d.pal_on_device = false; }},
		{"not a paletted index format", [](GSTilePaletteCycleDraw& d) { d.paletted_index = false; }},
		{"not a sprite", [](GSTilePaletteCycleDraw& d) { d.sprite = false; }},
	};
	for (const Case& c : cases)
	{
		GSTilePaletteCycleRun run;
		GSTilePaletteCycleDraw d = Producer();
		c.mutate(d);
		for (int i = 0; i < 16; i++)
			EXPECT_EQ(run.Observe(d), V::Pass) << c.name << ", draw " << i;
		EXPECT_FALSE(run.armed()) << c.name;
	}
}

// The substitute's shape, off the run's FBMSK alone. 0x00FFFFFF is the alpha-destination variant
// -- three draws, one channel into each of three page-offset targets -- and everything else is the
// in-place brightness ramp, one draw over the source's own rect.
TEST(TileGpuPaletteCycle, FbmskSelectsTheSubstituteShape)
{
	EXPECT_TRUE(gsTilePaletteCycleIsAlphaSplit(0x00FFFFFFu));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0x00000000u));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0xFF000000u));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0xFFFFFFFFu));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0x80FFFFFFu));
}

// A run whose substitute could not be built must be DRAWN, and go on being drawn. Eliding one with
// nothing in its place is the arrangement that loses a lot of picture silently: measured before the
// substitute existed, it cost gt4opb 13.5 levels of mean difference against Classic and gt4 9.9.
// The refusal is sticky so the renderer pays the failed attempt once and not on every draw of a
// thousand-draw run.
TEST(TileGpuPaletteCycle, ARefusedRunIsDrawnAndStaysDrawn)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer());
	ASSERT_EQ(run.Observe(Producer()), V::Arm);
	run.Refuse();
	EXPECT_TRUE(run.refused());
	EXPECT_FALSE(run.armed());
	for (int i = 0; i < 64; i++)
		EXPECT_EQ(run.Observe(Consumer()), V::Building) << "draw " << i;
	EXPECT_FALSE(run.armed());
}

// ...but the refusal belongs to the RUN, not to the renderer: the next run gets its own attempt,
// because what could not be substituted is a property of the shape that run presented.
TEST(TileGpuPaletteCycle, ARefusalDoesNotOutliveItsRun)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer(0x00FFFFFFu));
	ASSERT_EQ(run.Observe(Producer(0x00FFFFFFu)), V::Arm);
	run.Refuse();
	// A new FBMSK is a new run, and it may arm.
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(run.Observe(Consumer(0xFF000000u)), V::Building);
	EXPECT_FALSE(run.refused());
	EXPECT_EQ(run.Observe(Producer(0xFF000000u)), V::Arm);
}

TEST(TileGpuPaletteCycle, ResetClearsARefusal)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer());
	ASSERT_EQ(run.Observe(Producer()), V::Arm);
	run.Refuse();
	run.Reset();
	EXPECT_FALSE(run.refused());
}

TEST(TileGpuPaletteCycle, ResetEndsTheRun)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Consumer());
	ASSERT_EQ(run.Observe(Producer()), V::Arm);
	ASSERT_TRUE(run.armed());
	// The frame boundary. A run cannot span one: the plan its draws would be elided out of does
	// not, and the substitute that stands for them is a plan entry.
	run.Reset();
	EXPECT_FALSE(run.armed());
	EXPECT_FALSE(run.loop_seen());
	EXPECT_EQ(run.length(), 0u);
	EXPECT_EQ(run.owner(), kGSTileNoSurface);
	EXPECT_EQ(run.Observe(Consumer()), V::Building);
}
