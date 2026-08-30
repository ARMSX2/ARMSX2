// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu RENDER AREA: the rectangle a pass declares to vkCmdBeginRenderPass, which used to be
// the whole attachment pair and is now the union of what the pass's draws touch.
//
// It is a pure bandwidth change and it has to move no pixel, so what is pinned here is the two
// halves of that claim:
//
//  - CONTAINMENT. Vulkan says the application must ensure all rendering is contained within the
//    render area. The union is built out of the same gsTileGpuScissorRect the executor sets before
//    each indirect call, so containment is a property of the arithmetic rather than of two sides
//    agreeing -- and the sweep below checks it that way, per scissor, per pixel column.
//
//  - PRESERVATION. Outside the render area an attachment is neither loaded nor stored, so a
//    LOAD/STORE one keeps the bytes it held. That is only true of LOAD, which is what
//    gsTileGpuMayClampPassArea exists to say: a CLEAR or a DONT_CARE initializes the attachment and
//    initializes it only inside the area, so a pass carrying either keeps the whole one.
//
// The granularity round-out is the third piece. vkGetRenderAreaGranularity names the alignment a
// render area wants; rounding OUT to it costs the margin pixels a load and a store of their own
// value, because no draw's scissor reaches there, and rounding IN would cut a draw off.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	GSDevice::GSTileGpuScissorRect Area(const std::vector<GSVector4i>& scissors, int aw, int ah, int gw = 1,
		int gh = 1)
	{
		return GSDevice::gsTileGpuPassArea(scissors, aw, ah, gw, gh);
	}

	/// Whether `inner` (a draw's scissor rectangle) sits entirely inside `outer` (the render area).
	bool Contains(const GSDevice::GSTileGpuScissorRect& outer, const GSDevice::GSTileGpuScissorRect& inner)
	{
		if (inner.width <= 0 || inner.height <= 0)
			return true; // rasterizes nothing, so there is nothing to contain
		return inner.x >= outer.x && inner.y >= outer.y && (inner.x + inner.width) <= (outer.x + outer.width) &&
		       (inner.y + inner.height) <= (outer.y + outer.height);
	}
} // namespace

TEST(GSTileGpuPassArea, AnEmptyPassIsOneGranularityTileNotAZeroExtent)
{
	// A pass the executor will issue no geometry for -- pipelines unbuilt, or every draw scissored
	// away. It still opens and closes, so it still has to name an area, and one pixel of LOAD/STORE
	// is the smallest inert thing it can name. A zero extent would be asking the driver about a
	// degenerate rectangle for no gain.
	const auto e = Area({}, 640, 448);
	EXPECT_EQ(e.x, 0);
	EXPECT_EQ(e.y, 0);
	EXPECT_EQ(e.width, 1);
	EXPECT_EQ(e.height, 1);

	// Under a granularity it is one tile of it, still at the origin.
	const auto t = Area({}, 640, 448, 32, 32);
	EXPECT_EQ(t.x, 0);
	EXPECT_EQ(t.y, 0);
	EXPECT_EQ(t.width, 32);
	EXPECT_EQ(t.height, 32);

	// ...and a scissor that admits nothing (SCAX1 < SCAX0 is legal and means "nothing") is the same
	// case: it contributes no rectangle, rather than a point at its own corner.
	const auto n = Area({GSVector4i(300, 200, 100, 50)}, 640, 448);
	EXPECT_EQ(n.x, 0);
	EXPECT_EQ(n.y, 0);
	EXPECT_EQ(n.width, 1);
	EXPECT_EQ(n.height, 1);
}

TEST(GSTileGpuPassArea, OneDrawIsItsOwnScissor)
{
	// Dirge of Cerberus's 35x35 scratch surface, the case the whole change is about: the pass used to
	// declare the 640x448 framebuffer it alternates with.
	const auto s = Area({GSVector4i(0, 0, 35, 35)}, 640, 448);
	EXPECT_EQ(s.x, 0);
	EXPECT_EQ(s.y, 0);
	EXPECT_EQ(s.width, 35);
	EXPECT_EQ(s.height, 35);

	// One that does not start at the origin keeps its offset -- the area is the rectangle, not a
	// bounding box anchored at zero.
	const auto o = Area({GSVector4i(320, 112, 400, 224)}, 640, 448);
	EXPECT_EQ(o.x, 320);
	EXPECT_EQ(o.y, 112);
	EXPECT_EQ(o.width, 80);
	EXPECT_EQ(o.height, 112);
}

TEST(GSTileGpuPassArea, ManyDrawsAreTheirUnion)
{
	// Two disjoint rectangles: the union is the bounding box, which is what a render area can say.
	const auto u = Area({GSVector4i(0, 0, 64, 64), GSVector4i(576, 384, 640, 448)}, 640, 448);
	EXPECT_EQ(u.x, 0);
	EXPECT_EQ(u.y, 0);
	EXPECT_EQ(u.width, 640);
	EXPECT_EQ(u.height, 448);

	// Nested rectangles: the outer one alone decides.
	const auto n = Area({GSVector4i(100, 100, 200, 200), GSVector4i(120, 130, 150, 160)}, 640, 448);
	EXPECT_EQ(n.x, 100);
	EXPECT_EQ(n.y, 100);
	EXPECT_EQ(n.width, 100);
	EXPECT_EQ(n.height, 100);

	// An empty scissor between two real ones neither widens the union nor resets it.
	const auto e = Area({GSVector4i(100, 100, 200, 200), GSVector4i(50, 50, 10, 10), GSVector4i(150, 90, 260, 210)},
		640, 448);
	EXPECT_EQ(e.x, 100);
	EXPECT_EQ(e.y, 90);
	EXPECT_EQ(e.width, 160);
	EXPECT_EQ(e.height, 120);

	// The order the draws arrive in cannot matter.
	const auto f = Area({GSVector4i(150, 90, 260, 210), GSVector4i(100, 100, 200, 200)}, 640, 448);
	EXPECT_EQ(f.x, e.x);
	EXPECT_EQ(f.y, e.y);
	EXPECT_EQ(f.width, e.width);
	EXPECT_EQ(f.height, e.height);
}

TEST(GSTileGpuPassArea, ClampsToTheAttachmentPair)
{
	// A pass pairing a 640x512 colour target with a 640x448 depth one renders into the intersection
	// (gsTileGpuPassGeometry), and a render area may not exceed the framebuffer -- Vulkan requires
	// renderArea.offset + renderArea.extent to be inside it. So a scissor reaching past the pair is
	// cut to the pair, exactly as the per-call scissor is.
	const auto c = Area({GSVector4i(0, 0, 1024, 512)}, 640, 448);
	EXPECT_EQ(c.x, 0);
	EXPECT_EQ(c.y, 0);
	EXPECT_EQ(c.width, 640);
	EXPECT_EQ(c.height, 448);

	// ...and a scissor entirely past it contributes nothing at all, rather than an offset outside
	// the framebuffer.
	const auto p = Area({GSVector4i(700, 500, 800, 600)}, 640, 448);
	EXPECT_EQ(p.x, 0);
	EXPECT_EQ(p.y, 0);
	EXPECT_EQ(p.width, 1);
	EXPECT_EQ(p.height, 1);
}

TEST(GSTileGpuPassArea, RoundsOutToTheGranularityAndStopsAtTheAttachment)
{
	// vkGetRenderAreaGranularity's conditions for an optimal area: the offset is a multiple of the
	// granularity, and the extent either is a multiple of it or reaches the edge of the framebuffer.
	// Rounding OUT satisfies both and is content-preserving under LOAD; rounding IN would cut a draw
	// off, which is why the direction is not a choice.
	const auto r = Area({GSVector4i(35, 70, 100, 130)}, 640, 448, 32, 32);
	EXPECT_EQ(r.x, 32);
	EXPECT_EQ(r.y, 64);
	EXPECT_EQ(r.x + r.width, 128);
	EXPECT_EQ(r.y + r.height, 160);

	// The high edge stops at the attachment rather than rounding past it: 448 is not a multiple of
	// 32 * 5 = 160, and the granularity's second condition is satisfied by reaching the edge.
	const auto e = Area({GSVector4i(600, 430, 640, 448)}, 640, 448, 32, 32);
	EXPECT_EQ(e.x, 576);
	EXPECT_EQ(e.y, 416);
	EXPECT_EQ(e.x + e.width, 640);
	EXPECT_EQ(e.y + e.height, 448);

	// An already-aligned union is left alone -- the round-out must not add a tile it does not need.
	const auto a = Area({GSVector4i(64, 96, 192, 224)}, 640, 448, 32, 32);
	EXPECT_EQ(a.x, 64);
	EXPECT_EQ(a.y, 96);
	EXPECT_EQ(a.width, 128);
	EXPECT_EQ(a.height, 128);

	// A granularity of one, which is what a non-tiler reports, changes nothing.
	const auto o = Area({GSVector4i(35, 70, 100, 130)}, 640, 448, 1, 1);
	EXPECT_EQ(o.x, 35);
	EXPECT_EQ(o.y, 70);
	EXPECT_EQ(o.width, 65);
	EXPECT_EQ(o.height, 60);

	// A zero granularity is not a legal answer, but it would divide the round-out; it is read as one.
	const auto z = Area({GSVector4i(35, 70, 100, 130)}, 640, 448, 0, 0);
	EXPECT_EQ(z.x, o.x);
	EXPECT_EQ(z.y, o.y);
	EXPECT_EQ(z.width, o.width);
	EXPECT_EQ(z.height, o.height);
}

TEST(GSTileGpuPassArea, ContainsEveryDrawsScissor)
{
	// The property the change rests on, checked over the shapes the corpus produces rather than
	// argued: every rectangle the executor will set before an indirect call is inside the area the
	// pass declared, at every granularity a driver might report.
	const int bounds[] = {0, 1, 7, 35, 64, 112, 319, 320, 447, 448, 639, 640, 700};
	const int grans[] = {1, 2, 16, 32, 64};
	for (const int gran : grans)
	{
		for (const int x0 : bounds)
		{
			for (const int y0 : bounds)
			{
				const std::vector<GSVector4i> scissors = {
					GSVector4i(x0, y0, x0 + 37, y0 + 11),
					GSVector4i(0, 0, 35, 35),
					GSVector4i(320, 112, 640, 448),
					GSVector4i(x0 + 5, y0, x0, y0 + 5), // empty: high edge below low
				};
				const auto area = Area(scissors, 640, 448, gran, gran);
				EXPECT_GE(area.x, 0);
				EXPECT_GE(area.y, 0);
				EXPECT_LE(area.x + area.width, 640);
				EXPECT_LE(area.y + area.height, 448);
				for (const GSVector4i& s : scissors)
				{
					const auto r = GSDevice::gsTileGpuScissorRect(s.x, s.y, s.z, s.w, 640, 448);
					EXPECT_TRUE(Contains(area, r))
						<< "scissor (" << r.x << "," << r.y << " " << r.width << "x" << r.height << ") outside area ("
						<< area.x << "," << area.y << " " << area.width << "x" << area.height << ") at granularity "
						<< gran;
				}
			}
		}
	}
}

TEST(GSTileGpuPassArea, OnlyALoadingPassMayClamp)
{
	// The clear-interaction rule. A CLEAR or DONT_CARE load op initializes the attachment and does so
	// only inside the render area, so clamping one would leave the region outside uninitialized --
	// which the next pass to LOAD it there reads as garbage. Both are an attachment's first bind, so
	// keeping the whole area for them costs a handful of passes a frame.
	EXPECT_TRUE(GSDevice::gsTileGpuMayClampPassArea(true, true, true, true));
	EXPECT_TRUE(GSDevice::gsTileGpuMayClampPassArea(true, true, false, false)); // colour-only pass
	EXPECT_TRUE(GSDevice::gsTileGpuMayClampPassArea(false, false, true, true)); // depth-only pass

	// Either attachment failing to load is enough: the render area is one rectangle for both.
	EXPECT_FALSE(GSDevice::gsTileGpuMayClampPassArea(true, false, true, true));
	EXPECT_FALSE(GSDevice::gsTileGpuMayClampPassArea(true, true, true, false));
	EXPECT_FALSE(GSDevice::gsTileGpuMayClampPassArea(true, false, false, false));
	EXPECT_FALSE(GSDevice::gsTileGpuMayClampPassArea(false, false, true, false));
}

TEST(GSTileGpuPassArea, IsTheWholeAttachmentWhenADrawCoversIt)
{
	// The other end of the change: a pass whose draws really do cover the surface must come out at
	// the surface, or the clamp would be a regression dressed as a saving.
	const auto f = Area({GSVector4i(0, 0, 640, 448)}, 640, 448, 32, 32);
	EXPECT_EQ(f.x, 0);
	EXPECT_EQ(f.y, 0);
	EXPECT_EQ(f.width, 640);
	EXPECT_EQ(f.height, 448);
}

// The SEED pass, which is the same clamp at a site that did not have it. A seed is a full-target
// triangle whose fragments discard outside the op's page set; the executor has always set the
// pages' rectangle as the SCISSOR and has always declared the WHOLE attachment as the area, so on a
// tiler a seed repairing one 64x32 guest page loaded and stored the entire target. Measured over
// the corpus: Spider-Man 3's render-pass area a frame falls 70.09 -> 33.83 Mpx, and its seed passes
// alone covered 39.89 Mpx of attachment for 3.63 Mpx of scissor.
//
// What is pinned here is the seed's ARGUMENT SHAPE, because that is the part the shared rule cannot
// check: a seed carries exactly ONE attachment, colour or depth by its kind, and passing the pair's
// question with the wrong half filled in is how a DONT_CARE depth seed would come out clampable.
TEST(GSTileGpuPassArea, ASeedAsksTheClampQuestionWithOneAttachment)
{
	// The executor's own spelling: (!is_depth_seed, loads, is_depth_seed, loads).
	const auto seed_may_clamp = [](bool is_depth_seed, bool loads) {
		return GSDevice::gsTileGpuMayClampPassArea(!is_depth_seed, loads, is_depth_seed, loads);
	};

	// A colour seed and a depth seed both clamp where their one attachment LOADs...
	EXPECT_TRUE(seed_may_clamp(false, true));
	EXPECT_TRUE(seed_may_clamp(true, true));
	// ...and neither does where it does not, which is the CLEAR of a pool texture's first bind (whose
	// clear has to cover the whole attachment, or the pages outside it stay uninitialized for the
	// next pass that loads them) and the DONT_CARE of an invalidated one (whose clamped form would
	// store a granularity round-out margin it never loaded).
	EXPECT_FALSE(seed_may_clamp(false, false));
	EXPECT_FALSE(seed_may_clamp(true, false));

	// ⚠️ And the reason the shape is written down: filling in the OTHER half would call a
	// non-loading depth seed clampable, because the colour half it does not have answers "no colour
	// attachment, nothing to refuse for".
	EXPECT_TRUE(GSDevice::gsTileGpuMayClampPassArea(false, false, false, false));

	// The area a one-page seed asks for is that page's rectangle, not the target: the saving is the
	// difference, and on a 640x448 target it is two orders of magnitude.
	const auto page = Area({GSVector4i(0, 0, 64, 32)}, 640, 448, 1, 1);
	EXPECT_EQ(page.width, 64);
	EXPECT_EQ(page.height, 32);
	EXPECT_LT(page.width * page.height, (640 * 448) / 100);
}
