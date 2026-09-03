// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the offset-self-read copy decision (GS/Renderers/Common/GSSelfReadCopyPolicy.h).
//
// A draw that samples the render target it is writing, at a location it is not writing, has to
// read from a copy on any backend whose destination read happens in tile memory: fetch cannot
// express the read, and the barrier the disjoint-rect shortcut asks for is dropped on the grounds
// that fetch replaces the destination read. That is the change.
//
// Two sites in GSRendererHW::HandleTextureHazards ask this question -- the disjoint-rect shortcut
// and the channel shuffle whose source page differs from its destination page -- and they get the
// same answer from the same function under the same key. The remedies differ (the shuffle needs a
// coordinate-identity copy, because its shader offset addresses the source), the decision does not.
//
// What these tests are mostly for is the other half. Three roads must be bit-for-bit what they
// were -- the destination read that fetch does serve, the desktop barrier road, and the
// render-target-copy road every Adreno and every default Mali is on -- and none of them can be
// observed on the one device that takes the changed road. So the decision is a pure function and
// the no-change cases are pinned here by name.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSSelfReadCopyPolicy.h"

#include <gtest/gtest.h>

namespace
{
	// A device that reads the destination in tile memory: Mali or Adreno on Vulkan with rasterization-order attachment access, Mali on GL through
	// GL_ARM_shader_framebuffer_fetch, an Apple GPU under Metal. framebuffer_fetch implies
	// texture_barrier on every one of them.
	constexpr GSSelfReadCopyInputs FetchRoad()
	{
		GSSelfReadCopyInputs in;
		in.framebuffer_fetch = true;
		in.texture_barrier = true;
		return in;
	}

	// A desktop GPU: a real texture barrier, no in-tile read. The M2 under Vulkan lands here,
	// which is why the M2 hash grid is an identity guard and not a measurement.
	constexpr GSSelfReadCopyInputs BarrierRoad()
	{
		GSSelfReadCopyInputs in;
		in.texture_barrier = true;
		return in;
	}

	// No texture barrier, so GSDeviceVK clones the render target for the read itself. Adreno under
	// ARMSX2 #442, the RG477V at its default settings, iOS Metal.
	constexpr GSSelfReadCopyInputs CopyRoad()
	{
		return GSSelfReadCopyInputs();
	}
} // namespace

// The bug: an offset read on the fetch road has neither a copy nor a barrier. It gets the copy.
TEST(GSSelfReadCopy, FetchRoadOffsetReadCopies)
{
	EXPECT_TRUE(SelfReadNeedsSourceCopy(FetchRoad()));
}

// The read fetch exists for. Forcing a copy here would undo step 2.0 entirely, so it is the one
// case that must survive untouched on the very device the fix is for.
TEST(GSSelfReadCopy, FetchRoadSamePixelReadIsUntouched)
{
	GSSelfReadCopyInputs in = FetchRoad();
	in.same_pixel_read = true;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(in));
}

// Desktop keeps the disjoint-rect shortcut and its barrier. The barrier is by-region and would not
// be sufficient on a tiler, but immediate-mode drivers over-synchronise it and this change is not
// the place to find out what happens when they stop.
TEST(GSSelfReadCopy, BarrierRoadIsUnchanged)
{
	EXPECT_FALSE(SelfReadNeedsSourceCopy(BarrierRoad()));

	GSSelfReadCopyInputs same_pixel = BarrierRoad();
	same_pixel.same_pixel_read = true;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(same_pixel));
}

// Without a texture barrier the backend already takes a copy of the target for the read, so there
// is nothing here to fix and nothing to change.
TEST(GSSelfReadCopy, RenderTargetCopyRoadIsUnchanged)
{
	EXPECT_FALSE(SelfReadNeedsSourceCopy(CopyRoad()));

	// Same answer if the device also advertises fetch: fetch without a barrier is turned off in
	// the Vulkan backend, and on Metal the iOS path is exactly this shape.
	GSSelfReadCopyInputs with_fetch = CopyRoad();
	with_fetch.framebuffer_fetch = true;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(with_fetch));
}

// The attachment-feedback-loop layout samples the attachment through an ordinary sampler, which is
// not the road the fix was reasoned about.
TEST(GSSelfReadCopy, FeedbackLoopLayoutIsUnchanged)
{
	GSSelfReadCopyInputs in = FetchRoad();
	in.feedback_loop_layout = true;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(in));
}

// The copy happens for exactly one combination of the inputs. This sweep is the statement the
// guard devices need: "nothing off the fetch road moved", which is invisible on any machine we can
// run here.
TEST(GSSelfReadCopy, CopiesOnlyOnTheFetchRoad)
{
	for (int bits = 0; bits < 16; bits++)
	{
		GSSelfReadCopyInputs in;
		in.same_pixel_read = (bits & 1) != 0;
		in.framebuffer_fetch = (bits & 2) != 0;
		in.texture_barrier = (bits & 4) != 0;
		in.feedback_loop_layout = (bits & 8) != 0;

		const bool expected = !in.same_pixel_read && in.framebuffer_fetch && in.texture_barrier &&
		                      !in.feedback_loop_layout;
		EXPECT_EQ(SelfReadNeedsSourceCopy(in), expected) << "bits=" << bits;
	}
}

// The four draws the census names, by their ledger parameters: tex_hazard=RT, self_read=BARRIER,
// on a device with the in-tile destination read. OutRun 2006's rear-view mirror (draw 283: writes
// 352,0 160x120, reads 0,0 321x241) and MGS3's three-step blur inside target 02000 (draws 610, 611
// and 614). They are the only draws in the 22-dump corpus that reach this road in a title that
// renders non-deterministically, and the RG477V is the first machine that can execute the branch,
// so this is where the branch is exercised before the device sees it.
TEST(GSSelfReadCopy, TheCensusDrawsTakeTheCopy)
{
	// A draw that reached HandleTextureHazards' disjoint-rect shortcut has already been rejected
	// by CanUseTexIsFB, which is what same_pixel_read=false records.
	GSSelfReadCopyInputs census_draw = FetchRoad();
	census_draw.same_pixel_read = false;

	EXPECT_TRUE(SelfReadNeedsSourceCopy(census_draw));
}

// The channel-shuffle page offset is the second site. It reaches the policy with same_pixel_read
// false by construction -- a shuffle whose source page equals its destination page is resolved as
// tex_is_fb long before this road -- so it must get the same answer as the disjoint-rect road on
// every device. If those two ever disagree, one of the sites has stopped sharing the decision.
TEST(GSSelfReadCopy, ShuffleOffsetRoadMatchesTheDisjointRectRoad)
{
	for (int bits = 0; bits < 8; bits++)
	{
		GSSelfReadCopyInputs in;
		in.same_pixel_read = false;
		in.framebuffer_fetch = (bits & 1) != 0;
		in.texture_barrier = (bits & 2) != 0;
		in.feedback_loop_layout = (bits & 4) != 0;

		const bool expected = in.framebuffer_fetch && in.texture_barrier && !in.feedback_loop_layout;
		EXPECT_EQ(SelfReadNeedsSourceCopy(in), expected) << "bits=" << bits;
	}
}

// The shuffle escape on the fetch road, by name.
TEST(GSSelfReadCopy, FetchRoadShuffleOffsetCopies)
{
	GSSelfReadCopyInputs shuffle = FetchRoad();
	shuffle.same_pixel_read = false;
	EXPECT_TRUE(SelfReadNeedsSourceCopy(shuffle));
}

// And the two roads every guard device is on keep the shuffle escape exactly as it was. The M2 is
// the barrier road; Adreno and a default Mali are the copy road, where the backend clones the
// target for this draw already.
TEST(GSSelfReadCopy, ShuffleOffsetIsUnchangedOffTheFetchRoad)
{
	GSSelfReadCopyInputs barrier = BarrierRoad();
	barrier.same_pixel_read = false;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(barrier));

	GSSelfReadCopyInputs copy = CopyRoad();
	copy.same_pixel_read = false;
	EXPECT_FALSE(SelfReadNeedsSourceCopy(copy));
}
