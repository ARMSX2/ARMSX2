// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The pass-break cause census, pinned at its two load-bearing joints.
//
// The census exists to say WHY each of a frame's render passes ended, over a corpus where the counts
// run from 26 a frame to over a thousand. Its whole value rests on two properties, and neither of
// them is visible in a frame if it breaks:
//
//  1. The key decomposition names a cause for exactly the key pairs the pass grouping calls
//     different. If it named one for a pair the grouping calls the SAME, the census would invent
//     boundaries; if it named none for a pair the grouping calls different, a real boundary would go
//     unattributed and read as a merge nobody can collect. The depth clause is where that is easy to
//     get wrong -- GSTileGpuPassKey compares the depth SURFACE only when both sides attach depth --
//     so the equivalence is checked exhaustively over a small key space rather than by inspection.
//
//  2. Every boundary gsTileGpuPassEnd makes is one of exactly three things: the cap, the key, or the
//     draw's own break_before. That is what lets the census partition the pass count with no residue.
//     It is asserted in the plan build; here it is proved over randomized draw lists, where a fourth
//     cause would show up as a boundary none of the three explains.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
using Cause = GSTileGpuBreakCause;
using DepthMode = GSDevice::GSTileGpuDepthMode;

constexpr u32 Bit(Cause c)
{
	return gsTileGpuBreakBit(c);
}

// The whole key space this test sweeps: two colour surfaces, two depth surfaces, the depth
// attachment present or not, three depth pipeline variants, and the reader bit. 2*2*2*3*2 = 48 keys,
// so the pair sweep is 2,304 comparisons -- small enough to be exhaustive, which is the point.
std::vector<GSTileGpuPassKey> AllKeys()
{
	std::vector<GSTileGpuPassKey> keys;
	for (GSTileSurfaceId color : {GSTileSurfaceId(1), GSTileSurfaceId(2)})
	{
		for (GSTileSurfaceId depth : {GSTileSurfaceId(3), GSTileSurfaceId(4)})
		{
			for (bool used : {false, true})
			{
				for (DepthMode mode : {DepthMode::None, DepthMode::TestNoWrite, DepthMode::TestWrite})
				{
					for (bool reads : {false, true})
					{
						GSTileGpuPassKey k;
						k.color = color;
						k.depth = depth;
						k.depth_used = used;
						k.keyed_depth_mode = mode;
						k.keyed_self_read = reads;
						keys.push_back(k);
					}
				}
			}
		}
	}
	return keys;
}
} // namespace

// (1) The equivalence. Nonzero cause mask iff the grouping calls the two keys different.
TEST(GSTileGpuPassBreak, KeyFieldsFireForExactlyTheKeysTheGroupingSeparates)
{
	const std::vector<GSTileGpuPassKey> keys = AllKeys();
	for (const GSTileGpuPassKey& a : keys)
	{
		for (const GSTileGpuPassKey& b : keys)
		{
			const u32 m = gsTileGpuBreakKeyFields(a, b);
			EXPECT_EQ(m != 0, a != b) << "cause mask " << m << " disagrees with the pass key comparison";
		}
	}
}

// ...and the mask is symmetric, because a boundary is a boundary from either side of it.
TEST(GSTileGpuPassBreak, KeyFieldsAreSymmetric)
{
	const std::vector<GSTileGpuPassKey> keys = AllKeys();
	for (const GSTileGpuPassKey& a : keys)
	{
		for (const GSTileGpuPassKey& b : keys)
			EXPECT_EQ(gsTileGpuBreakKeyFields(a, b), gsTileGpuBreakKeyFields(b, a));
	}
}

// (1b) Directed: one field at a time, and the cause named is that field's.
TEST(GSTileGpuPassBreak, EachKeyFieldNamesItsOwnCause)
{
	GSTileGpuPassKey base;
	base.color = 1;
	base.depth = 3;
	base.depth_used = true;
	base.keyed_depth_mode = DepthMode::TestWrite;
	base.keyed_self_read = false;

	GSTileGpuPassKey other = base;
	other.color = 2;
	EXPECT_EQ(gsTileGpuBreakKeyFields(base, other), Bit(Cause::KeyColor));

	other = base;
	other.depth = 4;
	EXPECT_EQ(gsTileGpuBreakKeyFields(base, other), Bit(Cause::KeyDepthId));

	other = base;
	other.depth_used = false;
	EXPECT_EQ(gsTileGpuBreakKeyFields(base, other), Bit(Cause::KeyDepthUse));

	other = base;
	other.keyed_depth_mode = DepthMode::TestNoWrite;
	EXPECT_EQ(gsTileGpuBreakKeyFields(base, other), Bit(Cause::KeyDepthMode));

	other = base;
	other.keyed_self_read = true;
	EXPECT_EQ(gsTileGpuBreakKeyFields(base, other), Bit(Cause::KeySelfRead));
}

// The depth id is invisible where neither pass attaches depth. Two keys differing only there are the
// SAME pass, and a census naming a cause for them would report a target switch that never happened.
TEST(GSTileGpuPassBreak, DepthIdIsSilentWhenNeitherPassAttachesDepth)
{
	GSTileGpuPassKey a;
	a.color = 1;
	a.depth = 3;
	a.depth_used = false;
	GSTileGpuPassKey b = a;
	b.depth = 4;
	EXPECT_TRUE(a == b);
	EXPECT_EQ(gsTileGpuBreakKeyFields(a, b), 0u);
}

// ...and a key that gains the depth attachment names the PRESENCE and not the id, even when the id
// differs too. One boundary, one attachment change: reporting both would double-count the merge.
TEST(GSTileGpuPassBreak, GainingDepthNamesPresenceNotIdentity)
{
	GSTileGpuPassKey a;
	a.color = 1;
	a.depth = 3;
	a.depth_used = false;
	GSTileGpuPassKey b = a;
	b.depth = 4;
	b.depth_used = true;
	EXPECT_EQ(gsTileGpuBreakKeyFields(a, b), Bit(Cause::KeyDepthUse));
}

// (2) The three-cause partition. Over randomized draw lists, every boundary gsTileGpuPassEnd makes is
// explained by the cap, the key or the draw's own break_before -- and every draw the walk did NOT cut
// at is explained by none of them.
TEST(GSTileGpuPassBreak, EveryCutIsCapOrKeyOrBreakBefore)
{
	std::mt19937 rng(20260901u);
	const std::vector<GSTileGpuPassKey> keys = AllKeys();
	for (u32 trial = 0; trial < 200; trial++)
	{
		const u32 count = 1u + (rng() % 120u);
		// A cap of 0 is "uncapped", which is what every non-Adreno device answers, so it has to be in
		// the sweep beside the caps that bite.
		const u32 cap = std::array<u32, 4>{0u, 1u, 4u, 64u}[rng() % 4];
		std::vector<u32> key_index(count);
		std::vector<char> breaks(count);
		for (u32 d = 0; d < count; d++)
		{
			// A small key alphabet, so passes are long enough for the cap to reach and short enough
			// for key cuts to be common.
			key_index[d] = rng() % 5u;
			breaks[d] = (rng() % 8u) == 0;
		}
		const auto key_at = [&](u32 d) { return keys[key_index[d]]; };
		const auto breaks_at = [&](u32 d) { return breaks[d] != 0; };

		u32 i = 0;
		u32 passes = 0;
		while (i < count)
		{
			const u32 j = gsTileGpuPassEnd(i, count, cap, key_at, breaks_at);
			passes++;
			ASSERT_GT(j, i);
			// Interior draws joined the pass, so none of the three may hold for them.
			for (u32 d = i + 1; d < j; d++)
			{
				EXPECT_FALSE(breaks_at(d));
				EXPECT_EQ(gsTileGpuBreakKeyFields(key_at(i), key_at(d)), 0u);
				EXPECT_TRUE(cap == 0 || (d - i) < cap);
			}
			if (j < count)
			{
				const bool capped = cap != 0 && (j - i) >= cap;
				const u32 keym = gsTileGpuBreakKeyFields(key_at(i), key_at(j));
				EXPECT_TRUE(capped || keym != 0 || breaks_at(j))
					<< "a pass ended and none of cap/key/break_before explains it";
			}
			i = j;
		}
		EXPECT_EQ(passes, gsTileGpuCountPasses(count, cap, key_at, breaks_at));
	}
}

// (3) The tally's arithmetic: sole + multi partitions the boundaries, and present never under-counts.
TEST(GSTileGpuPassBreak, TallyPartitionsBoundariesIntoSoleAndMulti)
{
	std::array<u32, kGSTileGpuBreakCauses> present{};
	std::array<u32, kGSTileGpuBreakCauses> sole{};
	u32 multi = 0;
	const u32 masks[] = {
		Bit(Cause::KeyColor),
		Bit(Cause::KeyColor) | Bit(Cause::SeedColor),
		Bit(Cause::Cap),
		Bit(Cause::Date) | Bit(Cause::TexBind) | Bit(Cause::KeyColor),
		Bit(Cause::Writeback),
	};
	for (const u32 m : masks)
		gsTileGpuTallyBreak(m, present.data(), sole.data(), &multi);

	u32 sole_total = 0;
	for (const u32 s : sole)
		sole_total += s;
	EXPECT_EQ(sole_total + multi, static_cast<u32>(std::size(masks)));
	EXPECT_EQ(multi, 2u);
	EXPECT_EQ(present[static_cast<u32>(Cause::KeyColor)], 3u);
	EXPECT_EQ(sole[static_cast<u32>(Cause::KeyColor)], 1u);
	EXPECT_EQ(sole[static_cast<u32>(Cause::SeedColor)], 0u);
	EXPECT_EQ(present[static_cast<u32>(Cause::Writeback)], 1u);
	EXPECT_EQ(sole[static_cast<u32>(Cause::Writeback)], 1u);
}

// A boundary with no cause is the failure the plan build asserts against, so the tally must not
// silently absorb one: an empty mask credits nothing at all, including the multi column.
TEST(GSTileGpuPassBreak, TallyCreditsNothingForAnEmptyMask)
{
	std::array<u32, kGSTileGpuBreakCauses> present{};
	std::array<u32, kGSTileGpuBreakCauses> sole{};
	u32 multi = 7;
	gsTileGpuTallyBreak(0u, present.data(), sole.data(), &multi);
	for (u32 c = 0; c < kGSTileGpuBreakCauses; c++)
	{
		EXPECT_EQ(present[c], 0u);
		EXPECT_EQ(sole[c], 0u);
	}
	// Zero has no bits, so it is not "more than one cause" either -- the counter must not move.
	EXPECT_EQ(multi, 7u);
}

// The names are what a census line is read through, so each cause has one, they are distinct, and
// none of them falls through to the placeholder.
TEST(GSTileGpuPassBreak, EveryCauseHasADistinctName)
{
	std::vector<std::string> names;
	for (u32 c = 0; c < kGSTileGpuBreakCauses; c++)
	{
		const std::string name = gsTileGpuBreakCauseName(static_cast<Cause>(c));
		EXPECT_FALSE(name.empty());
		EXPECT_NE(name, "?") << "cause " << c << " has no name";
		names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	EXPECT_EQ(std::unique(names.begin(), names.end()), names.end()) << "two causes share a name";
	// The mask has to fit a u32, which is what every counter and every carried field assumes.
	EXPECT_LE(kGSTileGpuBreakCauses, 32u);
}

// GSTileGpuDateCover, the DATE staleness counterfactual. Its whole claim is that keeping a handful
// of rects instead of one bounding union can only ever REMOVE pass breaks, never add a wrong pixel
// -- so the property that has to hold is that the cover never forgets a rect it was given. If it
// could, a DATE draw would be told the snapshot is clean over pixels something wrote, and the draw
// would read a stale destination alpha.
TEST(GSTileGpuPassBreak, DateCoverNeverForgetsARectItWasGiven)
{
	std::mt19937 rng(20260901u);
	for (u32 trial = 0; trial < 300; trial++)
	{
		GSTileGpuDateCover cover;
		std::vector<GSVector4i> given;
		const u32 n = 1u + (rng() % 40u);
		for (u32 i = 0; i < n; i++)
		{
			const int x = static_cast<int>(rng() % 600u);
			const int y = static_cast<int>(rng() % 440u);
			const int w = 1 + static_cast<int>(rng() % 64u);
			const int h = 1 + static_cast<int>(rng() % 64u);
			const GSVector4i r(x, y, x + w, y + h);
			given.push_back(r);
			cover.add(r);
			// More rects than it holds must coalesce, not drop.
			EXPECT_LE(cover.count, GSTileGpuDateCover::kMaxRects);
		}
		for (const GSVector4i& r : given)
			EXPECT_TRUE(cover.intersects(r)) << "the cover forgot a rect it was given";
	}
}

// ...and it never claims MORE than the union it replaces: anything the cover says is written was
// inside the union too. That is the direction that makes the counterfactual's saving real rather
// than an artefact -- a cover that over-reported would under-count the breaks it could skip.
TEST(GSTileGpuPassBreak, DateCoverIsContainedInTheUnionItReplaces)
{
	std::mt19937 rng(7u);
	for (u32 trial = 0; trial < 300; trial++)
	{
		GSTileGpuDateCover cover;
		GSVector4i uni = GSVector4i::zero();
		const u32 n = 1u + (rng() % 30u);
		for (u32 i = 0; i < n; i++)
		{
			const int x = static_cast<int>(rng() % 600u);
			const int y = static_cast<int>(rng() % 440u);
			const GSVector4i r(x, y, x + 1 + static_cast<int>(rng() % 40u), y + 1 + static_cast<int>(rng() % 40u));
			cover.add(r);
			uni = uni.rempty() ? r : uni.runion(r);
		}
		for (u32 t = 0; t < 40; t++)
		{
			const int x = static_cast<int>(rng() % 640u);
			const int y = static_cast<int>(rng() % 480u);
			const GSVector4i probe(x, y, x + 8, y + 8);
			if (cover.intersects(probe))
				EXPECT_FALSE(uni.rintersect(probe).rempty()) << "the cover claims a rect the union does not";
		}
	}
}

// An empty cover intersects nothing, and a cleared one goes back to empty -- the two states the
// break site drives it through on every pass boundary.
TEST(GSTileGpuPassBreak, DateCoverStartsAndClearsEmpty)
{
	GSTileGpuDateCover cover;
	EXPECT_TRUE(cover.empty());
	EXPECT_FALSE(cover.intersects(GSVector4i(0, 0, 640, 448)));
	cover.add(GSVector4i(10, 10, 20, 20));
	EXPECT_FALSE(cover.empty());
	EXPECT_TRUE(cover.intersects(GSVector4i(0, 0, 640, 448)));
	// An empty rect is not a write and must not become one.
	GSTileGpuDateCover other;
	other.add(GSVector4i(5, 5, 5, 5));
	EXPECT_TRUE(other.empty());
	cover.clear();
	EXPECT_TRUE(cover.empty());
	EXPECT_FALSE(cover.intersects(GSVector4i(0, 0, 640, 448)));
}
