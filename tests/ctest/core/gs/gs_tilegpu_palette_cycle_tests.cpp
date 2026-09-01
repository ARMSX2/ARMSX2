// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu palette-cycle (channel-shuffle) signature: GSTilePaletteCycleRun.
//
// Gran Turismo 4 splits its frame buffer into R, G and B by re-reading it as a PSMT8 texture
// through a ramp CLUT it re-renders every cycle. Executed literally that is 85% of the Online
// Public Beta's frame -- ~1,540 draws, ~1,250 CLUT block copies, 279 render passes -- against
// Classic, which recognises the idiom and substitutes one channel-fetch draw for the whole run.
//
// The detector is a conjunction of three facts TileGpu already has in hand at the point the draw
// is accumulated, and the tests below are about what it must REFUSE at least as much as what it
// must catch. A false positive here is worse than Classic's: Classic's skip loses pixels in a
// render target, while an unaccounted elision in TileGpu's byte-truth page model pushes wrong
// bytes into guest VRAM, where the EE reads them.
//
// The populations pinned here:
//
//  - THE GT4 SHAPE. A long run under one FBMSK arms exactly once and elides everything after,
//    however long it runs.
//  - A ONE-OFF PALETTED SELF-READ. A game reading its own scratch buffer back through a palette
//    satisfies S1 and S2 perfectly well; what separates it from a cycle is that it happens once.
//    Three of them in a row still must not arm -- the threshold is four.
//  - THE NFSU2 SHAPE. "Consecutive channel shuffles with blending, reducing the alpha channel over
//    time" (GSRendererHW.cpp's own words) is an ACCUMULATING run that must not be collapsed. What
//    saves it is that the game changes FBMSK along the way, which is why the run key carries FBMSK
//    and why a change starts a new run rather than extending one.
//  - THE sotc / xenosaga SHAPE. Those titles load 1,300-1,800 palettes a frame and the CLUT gather
//    serves none of them, so pal_record is always zero and S1 cannot hold whatever else does.
//    Structural safety, not a threshold.
//  - WHICH SUBSTITUTE a run asks for, off FBMSK alone.

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

namespace
{
	constexpr GSTileSurfaceId kOwner = 3;
	constexpr GSTileSurfaceId kOther = 4;

	// The shape GT4's shuffle draws present: a device palette whose source pages this draw is
	// overwriting, PSMT8 sprite, ABE on, the palette's owner is the surface written.
	GSTilePaletteCycleDraw Shuffle(u32 fbmsk = 0x00000000u, GSTileSurfaceId owner = kOwner)
	{
		GSTilePaletteCycleDraw d;
		d.pal_on_device = true;
		d.writes_pal_source = true;
		d.paletted_index = true;
		d.sprite = true;
		d.abe = true;
		d.owner_is_written = true;
		d.fbmsk = fbmsk;
		d.owner = owner;
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

TEST(TileGpuPaletteCycle, TheGt4ShapeArmsOnceAndElidesTheRest)
{
	GSTilePaletteCycleRun run;
	// The first three are drawn for real: below the threshold the shape is not distinguishable
	// from a one-off self-read.
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	// The fourth is where the substitute is issued, and it is elided with the rest.
	EXPECT_EQ(run.Observe(Shuffle()), V::Arm);
	EXPECT_TRUE(run.armed());
	// GT4-OPB runs 140 cycles of eleven draws under one latch; nothing here re-arms.
	for (int i = 0; i < 1500; i++)
		EXPECT_EQ(run.Observe(Shuffle()), V::Elide) << "draw " << i;
	EXPECT_EQ(run.length(), 1504u);
}

TEST(TileGpuPaletteCycle, ArmingThresholdIsFour)
{
	EXPECT_EQ(GSTilePaletteCycleRun::kArmAfter, 4u);
}

// A game reading its own scratch buffer back through a palette once. The corpus has this
// population (m_frame.self_reads), and it must be drawn, not elided.
TEST(TileGpuPaletteCycle, AOneOffPalettedSelfReadDoesNotArm)
{
	GSTilePaletteCycleRun run;
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Ordinary()), V::Pass);
	EXPECT_FALSE(run.armed());
	EXPECT_EQ(run.length(), 0u);
}

TEST(TileGpuPaletteCycle, ThreeInARowStillDoNotArm)
{
	GSTilePaletteCycleRun run;
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_FALSE(run.armed());
	// ...and one ordinary draw between them puts the count back to nothing, so three more do not
	// finish what the first three started.
	EXPECT_EQ(run.Observe(Ordinary()), V::Pass);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
	EXPECT_FALSE(run.armed());
}

// NFS Underground 2: consecutive channel shuffles that reduce the alpha channel over time. The
// run is only safe to collapse within one FBMSK, and the game changes FBMSK as it goes.
TEST(TileGpuPaletteCycle, AnFbmskChangeStartsANewRunAndNeverCollapsesAcrossIt)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(run.Observe(Shuffle(0x00000000u)), V::Building);
	// Three draws under one mask, then the mask moves: the count starts again, so the first three
	// are drawn and the fourth draw of the FRAME is not the fourth draw of a run.
	EXPECT_EQ(run.Observe(Shuffle(0xFF000000u)), V::Building);
	EXPECT_EQ(run.length(), 1u);
	EXPECT_EQ(run.fbmsk(), 0xFF000000u);
	for (int i = 0; i < 2; i++)
		EXPECT_EQ(run.Observe(Shuffle(0xFF000000u)), V::Building);
	EXPECT_FALSE(run.armed());
	// A run that DOES reach four under the second mask arms under that mask alone.
	EXPECT_EQ(run.Observe(Shuffle(0xFF000000u)), V::Arm);
	EXPECT_EQ(run.fbmsk(), 0xFF000000u);
}

TEST(TileGpuPaletteCycle, AnArmedRunIsBrokenByAnFbmskChange)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Shuffle(0x00000000u));
	ASSERT_EQ(run.Observe(Shuffle(0x00000000u)), V::Arm);
	ASSERT_TRUE(run.armed());
	// The mask moves: the armed run is over, and the new one has to earn its own arming.
	EXPECT_EQ(run.Observe(Shuffle(0x00FFFFFFu)), V::Building);
	EXPECT_FALSE(run.armed());
}

TEST(TileGpuPaletteCycle, AnOwnerChangeStartsANewRun)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 3; i++)
		run.Observe(Shuffle(0u, kOwner));
	ASSERT_EQ(run.Observe(Shuffle(0u, kOwner)), V::Arm);
	// Tomb Raider: Underworld puts R, G and B in separate palettes. A run keyed on FBMSK alone
	// would elide a second palette's draws under the first's substitute.
	EXPECT_EQ(run.Observe(Shuffle(0u, kOther)), V::Building);
	EXPECT_EQ(run.owner(), kOther);
	EXPECT_FALSE(run.armed());
}

// sotc and xenosaga load 1,300-1,800 palettes a frame and the CLUT gather serves none of them, so
// there is no device record and pal_record is zero on every draw. S1 cannot hold, whatever the
// rest of the draw looks like -- which is a much stronger safety property than the threshold.
TEST(TileGpuPaletteCycle, WithoutADevicePaletteS1CannotHold)
{
	GSTilePaletteCycleRun run;
	GSTilePaletteCycleDraw d = Shuffle();
	d.pal_on_device = false;
	for (int i = 0; i < 64; i++)
		EXPECT_EQ(run.Observe(d), V::Pass) << "draw " << i;
	EXPECT_FALSE(run.armed());
}

TEST(TileGpuPaletteCycle, WithoutWritingThePaletteSourceS1CannotHold)
{
	GSTilePaletteCycleRun run;
	GSTilePaletteCycleDraw d = Shuffle();
	d.writes_pal_source = false;
	for (int i = 0; i < 64; i++)
		EXPECT_EQ(run.Observe(d), V::Pass) << "draw " << i;
	EXPECT_FALSE(run.armed());
}

// Each S2 clause on its own is enough to refuse the draw. Stated one at a time so a clause that
// stops being read shows up here rather than as a moved pixel on a title nobody runs.
TEST(TileGpuPaletteCycle, EveryShapeClauseRefusesOnItsOwn)
{
	struct Case
	{
		const char* name;
		void (*mutate)(GSTilePaletteCycleDraw&);
	};
	const Case cases[] = {
		{"not a paletted index format", [](GSTilePaletteCycleDraw& d) { d.paletted_index = false; }},
		{"not a sprite", [](GSTilePaletteCycleDraw& d) { d.sprite = false; }},
		{"alpha blending off", [](GSTilePaletteCycleDraw& d) { d.abe = false; }},
		{"the palette's owner is not the target", [](GSTilePaletteCycleDraw& d) { d.owner_is_written = false; }},
	};
	for (const Case& c : cases)
	{
		GSTilePaletteCycleRun run;
		GSTilePaletteCycleDraw d = Shuffle();
		c.mutate(d);
		for (int i = 0; i < 16; i++)
			EXPECT_EQ(run.Observe(d), V::Pass) << c.name << ", draw " << i;
		EXPECT_FALSE(run.armed()) << c.name;
	}
}

// The substitute's shape, off the run's FBMSK alone. 0x00FFFFFF is the alpha-destination variant
// -- three draws, one channel into each of three page-offset targets -- and everything else is
// the in-place brightness ramp, one draw over the source's own rect.
TEST(TileGpuPaletteCycle, FbmskSelectsTheSubstituteShape)
{
	EXPECT_TRUE(gsTilePaletteCycleIsAlphaSplit(0x00FFFFFFu));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0x00000000u));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0xFF000000u));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0xFFFFFFFFu));
	EXPECT_FALSE(gsTilePaletteCycleIsAlphaSplit(0x80FFFFFFu));
}

TEST(TileGpuPaletteCycle, ResetEndsTheRun)
{
	GSTilePaletteCycleRun run;
	for (int i = 0; i < 4; i++)
		run.Observe(Shuffle());
	ASSERT_TRUE(run.armed());
	// The frame boundary. A run cannot span one: the plan its draws would be elided out of does
	// not, and the substitute that stands for them is a plan entry.
	run.Reset();
	EXPECT_FALSE(run.armed());
	EXPECT_EQ(run.length(), 0u);
	EXPECT_EQ(run.owner(), kGSTileNoSurface);
	EXPECT_EQ(run.Observe(Shuffle()), V::Building);
}
