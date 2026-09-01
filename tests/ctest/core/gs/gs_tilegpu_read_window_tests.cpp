// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// THE READ WINDOW a TileGpu texture read composes, narrowed to the rectangle its sampler can reach
// (gsTileGpuReadWindowRect).
//
// The road used to compose the whole size-fixed TEX0 footprint -- what the sampler is FREE to
// fetch. ComposeRingPages gives every page of that a ring slot before the memory model narrows
// anything, and the writeback-break census measured Ratchet & Clank UYA's effects scene handing
// 46,835 pages a frame to EnsureRingSlot while its draws' coordinates reach 2,857: 93.9% of the
// walk is window nothing samples (corpus 65.9%).
//
// WHAT IS AT STAKE IF THIS IS WRONG. A window narrowed one texel too far is not a smaller upload.
// A page the compose skips has no ring slot, and the executor points every unstaged page at the
// zero slot -- so the source built over it reads ZEROS where the guest bytes should be, and a
// sample landing there is a wrong pixel with nothing to catch it. That is why every clause below
// is a refusal rather than a correction: this function's job is to say when Classic's coverage
// rect may be USED, and the answer must be no wherever the rect is a hint rather than a proof.
//
// WHERE THE COVERAGE ITSELF COMES FROM. GSState::GetTextureMinMax, which the HW and SW renderers
// have both shipped on for years -- it already accounts for the CLAMP register, the four wrap
// modes and the linear filter's half-texel skirt. Nothing here recomputes it, and these tests do
// not re-derive it either: what they pin is the admission rule around it, and each guard is shown
// catching a rect that would be wrong to trust by comparing against a BRUTE-FORCE model of the
// sampler -- every texel the addressing mode can actually reach, enumerated, not restated.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
	constexpr int kTw = 256; ///< a 2^8 window, the shape the guards are checked at
	constexpr int kTh = 256;
	constexpr u32 kTwLog2 = 8;
	constexpr u32 kThLog2 = 8;

	GSVector4i Window(int w = kTw, int h = kTh)
	{
		return GSVector4i(0, 0, w, h);
	}

	/// The GS's own address computation for one axis, per the CLAMP register. This is the reference
	/// the rects are judged against -- deliberately the addressing mode's semantic, texel by texel,
	/// rather than a second copy of any rect arithmetic, so a bug in the coverage cannot agree with
	/// itself. Mirrors the sampler: REPEAT masks, CLAMP saturates, REGION_CLAMP saturates into
	/// [MIN, MAX], REGION_REPEAT masks with MIN and then ORs MAX in.
	int AddressTexel(int u, int mode, int tw_log2, int minv, int maxv)
	{
		const int size = 1 << tw_log2;
		switch (mode)
		{
			case CLAMP_REPEAT: return u & (size - 1);
			case CLAMP_CLAMP: return std::clamp(u, 0, size - 1);
			case CLAMP_REGION_CLAMP: return std::clamp(u, minv, maxv);
			case CLAMP_REGION_REPEAT: return (u & minv) | maxv;
			default: return u;
		}
	}

	/// Every texel of one axis the sampler can fetch for a coordinate span [lo, hi], as an
	/// inclusive interval hull. Brute force: walk the span, address each coordinate, keep the
	/// extremes. `linear` adds the filter's neighbouring tap, which the sampler takes at the
	/// ADDRESSED coordinate's neighbour and not at the raw one.
	void ReachedSpan(int lo, int hi, int mode, int tw_log2, int minv, int maxv, bool linear, int* out_lo, int* out_hi)
	{
		int a = 1 << 30, b = -(1 << 30);
		for (int u = lo; u <= hi; u++)
		{
			const int t = AddressTexel(u, mode, tw_log2, minv, maxv);
			a = std::min(a, t);
			b = std::max(b, t);
			if (linear)
			{
				const int n = AddressTexel(u + 1, mode, tw_log2, minv, maxv);
				a = std::min(a, n);
				b = std::max(b, n);
			}
		}
		*out_lo = a;
		*out_hi = b;
	}

	/// The set of texels REGION_REPEAT reaches on one axis, as a sorted list rather than a hull:
	/// that mode's reads are a bit PATTERN, and the difference between the pattern and its hull is
	/// the whole reason the guard exists.
	std::vector<int> ReachedSet(int lo, int hi, int mode, int tw_log2, int minv, int maxv)
	{
		std::vector<int> v;
		for (int u = lo; u <= hi; u++)
			v.push_back(AddressTexel(u, mode, tw_log2, minv, maxv));
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return v;
	}

	/// The one-texel slack gsTileGpuReadWindowRect declines Classic's tight edge for, applied to a
	/// rect the way it applies it: grow by one in every direction, then clip to the window. Spelt
	/// here so the expectations below say "the coverage plus the road's own slack" rather than
	/// carrying four magic numbers each.
	GSVector4i Slack(const GSVector4i& r, const GSVector4i& win)
	{
		return r.add32(GSVector4i(-1, -1, 1, 1)).rintersect(win);
	}

	/// gsTileGpuReadWindowRect with the two REGION registers left at zero, which is what every
	/// non-region call site means.
	GSVector4i Narrow(const GSVector4i& win, const GSVector4i& cov, u32 twl, u32 thl, u32 wms, u32 wmt)
	{
		return gsTileGpuReadWindowRect(win, cov, twl, thl, wms, wmt, 0, 0, 0, 0);
	}

	/// Does `r` (a half-open rect) contain every texel of the inclusive x span [lo, hi]?
	bool CoversX(const GSVector4i& r, int lo, int hi)
	{
		return r.x <= lo && r.z >= hi + 1;
	}
	bool CoversY(const GSVector4i& r, int lo, int hi)
	{
		return r.y <= lo && r.w >= hi + 1;
	}
} // namespace

// -- what the narrowing does when it is allowed to ------------------------------------------------

TEST(GSTileGpuReadWindow, NarrowsToTheCoverageWhenTheModesAreIntervals)
{
	// The ordinary case: a CLAMP/CLAMP draw whose coordinates walk a 64x48 patch of a 256x256
	// window. The rect that comes back is the patch, and that is the whole lever -- 3 pages of a
	// 32-page window instead of 32.
	const GSVector4i cov(16, 32, 80, 80);
	const GSVector4i r = Narrow(Window(), cov, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_CLAMP);
	EXPECT_TRUE(r.eq(Slack(cov, Window())));
	EXPECT_TRUE(r.rintersect(cov).eq(cov)); // and it never comes back SMALLER than the coverage

	// ...and it really does contain every texel the sampler addresses over that span. The model,
	// not the rect restated.
	int lo, hi;
	ReachedSpan(16, 79, CLAMP_CLAMP, kTwLog2, 0, 0, /*linear=*/false, &lo, &hi);
	EXPECT_TRUE(CoversX(r, lo, hi));
	ReachedSpan(32, 79, CLAMP_CLAMP, kThLog2, 0, 0, /*linear=*/false, &lo, &hi);
	EXPECT_TRUE(CoversY(r, lo, hi));
}

TEST(GSTileGpuReadWindow, KeepsTheWholeWindowWhenTheCoverageIsTheWholeWindow)
{
	// A REPEAT draw whose span crosses a repeat period covers the texture and GetTextureMinMax
	// says so. Nothing to narrow, and the function must not invent a narrowing: the composed set
	// has to stay the window.
	const GSVector4i r = Narrow(Window(), Window(), kTwLog2, kThLog2, CLAMP_REPEAT, CLAMP_REPEAT);
	EXPECT_TRUE(r.eq(Window()));

	// The model agrees that a span crossing a period reaches every texel of the axis.
	int lo, hi;
	ReachedSpan(200, 400, CLAMP_REPEAT, kTwLog2, 0, 0, /*linear=*/false, &lo, &hi);
	EXPECT_EQ(lo, 0);
	EXPECT_EQ(hi, kTw - 1);
	EXPECT_TRUE(CoversX(r, lo, hi));
}

TEST(GSTileGpuReadWindow, NarrowsARepeatDrawThatStaysInsideOnePeriod)
{
	// REPEAT is only narrowed by GetTextureMinMax when the whole span lies in one period -- then
	// the masked coordinates are an interval and the rect is exact. Span [300, 340] is period 1,
	// texels 44..84.
	int lo, hi;
	ReachedSpan(300, 340, CLAMP_REPEAT, kTwLog2, 0, 0, /*linear=*/false, &lo, &hi);
	EXPECT_EQ(lo, 44);
	EXPECT_EQ(hi, 84);

	const GSVector4i cov(lo, 0, hi + 1, kTh);
	const GSVector4i r = Narrow(Window(), cov, kTwLog2, kThLog2, CLAMP_REPEAT, CLAMP_REPEAT);
	EXPECT_TRUE(r.eq(Slack(cov, Window())));
	EXPECT_TRUE(CoversX(r, lo, hi));
	EXPECT_LT(r.z, kTw); // still a narrowing, not the whole axis back
}

TEST(GSTileGpuReadWindow, NarrowsToTheRegionARegionClampNames)
{
	// REGION_CLAMP saturates into [MINU, MAXU], so a draw whose coordinates run well outside the
	// region still reads only the region -- and the region is an interval, so the rect is exact.
	constexpr int kMinU = 40, kMaxU = 71;
	int lo, hi;
	ReachedSpan(-500, 900, CLAMP_REGION_CLAMP, kTwLog2, kMinU, kMaxU, /*linear=*/false, &lo, &hi);
	EXPECT_EQ(lo, kMinU);
	EXPECT_EQ(hi, kMaxU);

	const GSVector4i cov(kMinU, 0, kMaxU + 1, kTh);
	const GSVector4i r = Narrow(Window(), cov, kTwLog2, kThLog2, CLAMP_REGION_CLAMP, CLAMP_REPEAT);
	EXPECT_TRUE(r.eq(Slack(cov, Window())));
	EXPECT_TRUE(CoversX(r, lo, hi));
	EXPECT_LT(r.z, kTw);
}

TEST(GSTileGpuReadWindow, TheLinearSkirtIsInTheRectItIsHandedAndSurvivesTheNarrowing)
{
	// The half-texel skirt is GetTextureMinMax's, not this function's -- what is pinned here is
	// that a linear draw's rect is the nearest draw's grown by the filter's tap, and that the
	// narrowing passes both through unchanged. A rect that lost the skirt would sample a texel
	// the compose never staged, which is the exact failure this whole lever has to not have.
	int nlo, nhi, llo, lhi;
	ReachedSpan(64, 95, CLAMP_CLAMP, kTwLog2, 0, 0, /*linear=*/false, &nlo, &nhi);
	ReachedSpan(64, 95, CLAMP_CLAMP, kTwLog2, 0, 0, /*linear=*/true, &llo, &lhi);
	EXPECT_EQ(nlo, 64);
	EXPECT_EQ(nhi, 95);
	EXPECT_EQ(llo, 64);
	EXPECT_EQ(lhi, 96); // the tap

	const GSVector4i nearest(nlo, 0, nhi + 1, kTh);
	const GSVector4i linear(llo, 0, lhi + 1, kTh);
	const GSVector4i rn = Narrow(Window(), nearest, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_REPEAT);
	const GSVector4i rl = Narrow(Window(), linear, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_REPEAT);
	EXPECT_TRUE(rn.eq(Slack(nearest, Window())));
	EXPECT_TRUE(rl.eq(Slack(linear, Window())));
	EXPECT_TRUE(CoversX(rl, llo, lhi));
	// ...and the linear rect contains the nearest one, so the skirt is not lost by a clamp against
	// the window.
	EXPECT_TRUE(rl.rintersect(rn).eq(rn));
	EXPECT_GT(rl.z, rn.z);
}

// -- the guards, each shown catching its own violation ---------------------------------------------

TEST(GSTileGpuReadWindow, AdmitsARegionRepeatWhosePatternFitsTheTexture)
{
	// REGION_REPEAT addresses (u & MINU) | MAXU. Every value it can produce has bits within
	// MAXU | MINU and contains the bits of MAXU, so while MAXU | MINU fits the axis the whole
	// pattern lies inside the texture -- and GetTextureMinMax's hull for it (UsesRegionRepeat) is
	// what the SOFTWARE renderer already decides which texels to fetch from, at this same
	// clamp_to_tsize. So it is admitted, and the brute-force pattern is the check.
	constexpr int kMinU = 0x0F, kMaxU = 0x40;
	const std::vector<int> reached = ReachedSet(0, 4095, CLAMP_REGION_REPEAT, kTwLog2, kMinU, kMaxU);
	ASSERT_FALSE(reached.empty());
	EXPECT_EQ(reached.front(), kMaxU);
	EXPECT_EQ(reached.back(), kMaxU | kMinU);

	// The hull GetTextureMinMax hands back for the whole-axis span.
	const GSVector4i cov(kMaxU, 0, (kMaxU | kMinU) + 1, kTh);
	const GSVector4i r = gsTileGpuReadWindowRect(
		Window(), cov, kTwLog2, kThLog2, CLAMP_REGION_REPEAT, CLAMP_REPEAT, kMinU, kMaxU, 0, 0);
	EXPECT_TRUE(r.eq(Slack(cov, Window())));
	EXPECT_LT(r.z, kTw); // a real narrowing: 0x3F..0x50 of a 256-wide axis
	for (int t : reached)
		EXPECT_TRUE(CoversX(r, t, t)) << "texel " << t << " reachable but outside the rect";
}

TEST(GSTileGpuReadWindow, RefusesARegionRepeatThatAddressesOutsideTheTexture_TheRedCheck)
{
	// MAXU at the axis size. The pattern's values all start at 256 on a 256-wide window, so every
	// texel it reads is outside the texture the road's window covers.
	constexpr int kMinU = 0x0F, kMaxU = 0x100;
	const std::vector<int> reached = ReachedSet(0, 4095, CLAMP_REGION_REPEAT, kTwLog2, kMinU, kMaxU);
	EXPECT_GE(reached.front(), kTw);

	// THE RED CHECK: the hull for that pattern, intersected with the window, is EMPTY -- and an
	// empty composed set stages no page, so every texel the draw samples comes back from the
	// executor's zero slot. GetTextureMinMax does not return an empty rect (it has one-texel
	// fixups for exactly this), so what would actually arrive is a one-texel rect at the edge:
	// a 32-page window composed down to one page, and 31 pages of zeros.
	const GSVector4i cov(kMaxU, 0, (kMaxU | kMinU) + 1, kTh);
	EXPECT_TRUE(cov.rintersect(Window()).rempty());

	// The guard refuses on the registers, before any of that can be reasoned about.
	EXPECT_TRUE(gsTileGpuReadWindowRect(
		Window(), cov, kTwLog2, kThLog2, CLAMP_REGION_REPEAT, CLAMP_REPEAT, kMinU, kMaxU, 0, 0)
					.eq(Window()));

	// ...and the condition is MAXU | MINU, not MAXU alone. MINU can carry the bits that run off the
	// end while MAXU sits well inside: (u & 0x1F0) | 0x08 reaches 0x1F8 on a 256-wide axis.
	constexpr int kMinU2 = 0x1F0, kMaxU2 = 0x08;
	const std::vector<int> reached2 = ReachedSet(0, 4095, CLAMP_REGION_REPEAT, kTwLog2, kMinU2, kMaxU2);
	EXPECT_LT(kMaxU2, kTw);                // MAXU alone looks harmless
	EXPECT_GE(reached2.back(), kTw);       // and the pattern still leaves the texture
	const GSVector4i cov2(kMaxU2, 0, (kMaxU2 | kMinU2) + 1, kTh);
	EXPECT_TRUE(gsTileGpuReadWindowRect(
		Window(), cov2, kTwLog2, kThLog2, CLAMP_REGION_REPEAT, CLAMP_REPEAT, kMinU2, kMaxU2, 0, 0)
					.eq(Window()));
}

TEST(GSTileGpuReadWindow, RefusesARegionRepeatOnTheVAxisToo_TheRedCheck)
{
	// The same guard on the other axis, because a mode test written once is a mode test that covers
	// one axis. The U half here is an honest CLAMP interval, so only WMT refuses -- and the refusal
	// has to take the whole rect with it, not just its V half: the page footprint is one rectangle
	// and half a narrowing is a narrowing.
	constexpr int kMinV = 0x0F, kMaxV = 0x100;
	const std::vector<int> reached = ReachedSet(0, 4095, CLAMP_REGION_REPEAT, kThLog2, kMinV, kMaxV);
	EXPECT_GE(reached.front(), kTh);

	const GSVector4i cov(16, kMaxV, 80, (kMaxV | kMinV) + 1);
	// THE RED CHECK: without the guard the U half alone narrows the composed set to three pages of
	// a 32-page window, while the V axis reads texels the window does not hold at all.
	EXPECT_FALSE(cov.rintersect(Window()).eq(Window()));
	EXPECT_TRUE(gsTileGpuReadWindowRect(
		Window(), cov, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_REGION_REPEAT, 0, 0, kMinV, kMaxV)
					.eq(Window()));

	// ...and a V pattern that DOES fit is admitted, so the guard is the pattern and not the mode.
	constexpr int kMinV2 = 0x0F, kMaxV2 = 0x40;
	const GSVector4i cov2(16, kMaxV2, 80, (kMaxV2 | kMinV2) + 1);
	const GSVector4i r2 = gsTileGpuReadWindowRect(
		Window(), cov2, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_REGION_REPEAT, 0, 0, kMinV2, kMaxV2);
	EXPECT_TRUE(r2.eq(Slack(cov2, Window())));
	for (int t : ReachedSet(0, 4095, CLAMP_REGION_REPEAT, kThLog2, kMinV2, kMaxV2))
		EXPECT_TRUE(CoversY(r2, t, t)) << "texel " << t << " reachable but outside the rect";
}

TEST(GSTileGpuReadWindow, RefusesAWindowTheRegisterSizesAbove1024_TheRedCheck)
{
	// The road's window is 1 << min(TW, 10) -- the GS's own 1024 cap -- while GetTextureMinMax
	// works in 1 << TW. At TW = 12 those are 1024 and 4096, so a coverage rect at x = 2000 is
	// inside the register's texture and outside the road's window entirely.
	const GSVector4i win = Window(1024, 1024);
	const GSVector4i cov(2000, 0, 2400, 1024);

	// THE RED CHECK: intersected against the road's window that rect is empty, and an empty
	// composed set stages no page at all.
	EXPECT_TRUE(cov.rintersect(win).rempty());

	// The guard refuses on the size alone, before the intersection can produce anything.
	EXPECT_TRUE(Narrow(win, cov, 12, 10, CLAMP_CLAMP, CLAMP_CLAMP).eq(win));
	EXPECT_TRUE(Narrow(win, cov, 10, 12, CLAMP_CLAMP, CLAMP_CLAMP).eq(win));
	// ...and it is the size that refuses, not the rect: the same oversized TW with a rect that
	// WOULD have intersected is refused too, because the two rects are in different spaces and an
	// intersection between them means nothing.
	EXPECT_TRUE(Narrow(win, GSVector4i(0, 0, 64, 64), 11, 10, CLAMP_CLAMP, CLAMP_CLAMP).eq(win));
	// At TW = TH = 10 the same rect is admitted, so the guard is the size and not a blanket.
	EXPECT_TRUE(Narrow(win, GSVector4i(0, 0, 64, 64), 10, 10, CLAMP_CLAMP, CLAMP_CLAMP)
					.eq(Slack(GSVector4i(0, 0, 64, 64), win)));
}

TEST(GSTileGpuReadWindow, RefusesAnEmptyIntersection_TheRedCheck)
{
	// GetTextureMinMax has its own one-texel fixups for a clamping region entirely outside the
	// texture (THPS, NFSMW and Lupin 3rd are named in its comment). Rather than reason about which
	// of them applied, an intersection that comes out empty is refused outright.
	//
	// THE RED CHECK: an empty composed set is not "compose less", it is compose NOTHING -- every
	// page of the window then points at the executor's zero slot and every texel the draw samples
	// is zero. This is the one failure mode that is silent AND total.
	const GSVector4i cov(kTw + 8, kTh + 8, kTw + 24, kTh + 24);
	EXPECT_TRUE(cov.rintersect(Window()).rempty());
	EXPECT_TRUE(Narrow(Window(), cov, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_CLAMP).eq(Window()));

	// A degenerate rect (zero width, nonzero height) is the same refusal for the same reason -- and
	// the ORDER matters: the slack is taken after the emptiness test, so it can never widen a
	// degenerate rect into a plausible-looking two-texel one and rescue what the guard refused.
	EXPECT_TRUE(GSVector4i(32, 0, 32, kTh).rintersect(Window()).rempty());
	EXPECT_TRUE(Narrow(Window(), GSVector4i(32, 0, 32, kTh), kTwLog2, kThLog2, CLAMP_CLAMP,
		CLAMP_CLAMP)
					.eq(Window()));
}

TEST(GSTileGpuReadWindow, ClampsACoverageThatLeavesTheWindowRatherThanTrustingIt)
{
	// A rect that overhangs the window on one side is admitted -- clipped to the window, which can
	// only ever compose MORE than the overhanging part would have -- because the overhang names
	// texels the window does not hold and the road never composed those anyway.
	const GSVector4i cov(-32, 64, kTw + 32, 96);
	const GSVector4i r = Narrow(Window(), cov, kTwLog2, kThLog2, CLAMP_CLAMP, CLAMP_CLAMP);
	EXPECT_EQ(r.x, 0);
	EXPECT_EQ(r.z, kTw);
	EXPECT_EQ(r.y, 63); // the coverage's 64, one texel of declined slack out
	EXPECT_EQ(r.w, 97);
	// The result is inside the window, always -- that is what makes the narrowing incapable of
	// adding a page, which is what keeps mip levels out of it: `tex_pages` is built from the base
	// TEX0 alone, so a rect that cannot leave level 0's window cannot reach another level's pages.
	EXPECT_TRUE(r.rintersect(Window()).eq(r));
}

TEST(GSTileGpuReadWindow, IsAlwaysInsideTheWindow)
{
	// The invariant every caller rests on, swept rather than argued: whatever comes in, what comes
	// out is a sub-rect of the window -- so the composed page set is a subset of the window's and
	// no road downstream can be handed a page the window never covered.
	const GSVector4i win = Window();
	for (int mode_s = 0; mode_s <= 3; mode_s++)
	{
		for (int mode_t = 0; mode_t <= 3; mode_t++)
		{
			for (int x = -64; x <= kTw + 64; x += 37)
			{
				for (int w = 0; w <= kTw + 64; w += 53)
				{
					const GSVector4i cov(x, 0, x + w, kTh);
					// MAXU/MAXV at the axis size on the region axes, so the sweep exercises the
					// refusal as well as the admission.
					const GSVector4i r = gsTileGpuReadWindowRect(win, cov, kTwLog2, kThLog2,
						static_cast<u32>(mode_s), static_cast<u32>(mode_t), 0x0F, kTw, 0x0F, kTh);
					EXPECT_TRUE(r.rintersect(win).eq(r))
						<< "modes " << mode_s << "/" << mode_t << " x=" << x << " w=" << w;
					EXPECT_FALSE(r.rempty()) << "modes " << mode_s << "/" << mode_t << " x=" << x << " w=" << w;
					if (mode_s == CLAMP_REGION_REPEAT || mode_t == CLAMP_REGION_REPEAT)
						EXPECT_TRUE(r.eq(win)) << "a region-repeat pattern off the end must not narrow";
				}
			}
		}
	}
}
