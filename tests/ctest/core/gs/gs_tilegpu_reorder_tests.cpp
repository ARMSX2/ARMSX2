// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu draw REORDERING: the scheduler that stages draws into runs, and the splice that puts the
// plan's per-draw arrays into the order those runs came out in.
//
// Why it exists at all: a render pass is its attachments, so a game that alternates between two
// targets ends a pass on every switch. Where the two targets share no GS page the alternation is a
// scheduling accident, not a data dependency -- the draws into each can be grouped and the passes
// collapse. Dirge of Cerberus plans about a thousand passes a frame that way and would plan under
// sixty.
//
// Why it is tested HERE rather than through a rendered frame: reordering is pixel-inert by
// construction, so a wrong reorder does not look wrong -- it looks like a frame that came out
// slightly different on one dump, months later, with nothing to point at. The construction is one
// property, and it is checkable exactly:
//
//   No two draws whose order the emission INVERTS may touch the same GS page.
//
// That is what the pair check below asserts, over every ordered pair of every stream here, which is
// the O(n^2) proof the design memo's model carried and this is the C++ of it. The other three
// properties are the splice's, and they are the ones a mistake would silently break: the emission
// order must be a permutation (no draw lost, none run twice), the prep-op slices must come out
// contiguous and intact (an op array that stops tiling drops ops that then never run), and
// state_index must be the draw's new position (the executor indexes the state table by it).
//
// ⚠️ What is NOT modelled here, and cannot be: whether a draw's break_before verdict survives the
// reorder. Those are decided during accumulation, in guest order, against the pass open at the
// time; the design's claim is that the same draws seed under either order, and the corpus is what
// answers that. This suite is about the arrangement, not about the seeds.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <numeric>
#include <vector>

namespace
{
using DepthMode = GSDevice::GSTileGpuDepthMode;
using Scheduler = GSTileGpuReorderScheduler;

constexpr GSTileSurfaceId kFrame = 1;   ///< the big framebuffer
constexpr GSTileSurfaceId kScratch = 2; ///< a small buffer beside it, page-disjoint
constexpr GSTileSurfaceId kZ = 3;

/// The two negotiable pass-key dimensions, spelt as the pass-key and pass-cap suites spell them so
/// a crossing of the three cannot come to mean different things in each.
constexpr bool kMerged = false;
constexpr bool kShared = false;

GSPageBitmap Pages(std::initializer_list<u32> pages)
{
	GSPageBitmap b;
	for (const u32 p : pages)
		b.set(p);
	return b;
}

/// A page RANGE, for the footprints that are a framebuffer rather than a handful of pages.
GSPageBitmap PageRange(u32 first, u32 last)
{
	GSPageBitmap b;
	for (u32 p = first; p <= last; p++)
		b.set(p);
	return b;
}

/// One accumulated draw, in the fields the scheduler and the splice read.
struct Draw
{
	GSTileSurfaceId color = kFrame;
	DepthMode depth = DepthMode::None;
	GSTileSurfaceId z = kZ;
	GSPageBitmap written;
	GSPageBitmap read;
	/// The class test's answer: false for a draw nothing has proved independent of the backlog (a
	/// prep-only pseudo-draw, an in-pass destination read, a DATE draw on the snapshot road, a draw
	/// that samples pages it renders into).
	bool reorderable = true;
	/// How many prep ops this draw owns. The splice moves the slice whole and renumbers its base.
	u32 prep_ops = 0;
};

GSTileGpuPassKey KeyOf(const Draw& d)
{
	return gsTileGpuPassKeyFor(d.color, d.z, d.depth, kMerged, false, kShared);
}

/// Drive the scheduler exactly as the renderer drives it: refuse, or admit and stage.
std::vector<u32> Schedule(const std::vector<Draw>& draws, u32 max_runs)
{
	Scheduler s;
	s.Reset(max_runs);
	for (u32 i = 0; i < draws.size(); i++)
	{
		const Draw& d = draws[i];
		const u32 run = d.reorderable ? s.Admit(KeyOf(d), d.written, d.read).run : s.OpenBarrierRun(KeyOf(d));
		s.Stage(i, run, d.written, d.read);
	}
	s.FlushAll();
	return s.Order();
}

/// (a) The emission order is a permutation of the draw list: every draw once, none invented.
void ExpectPermutation(const std::vector<Draw>& draws, const std::vector<u32>& order)
{
	ASSERT_EQ(order.size(), draws.size());
	std::vector<u32> seen = order;
	std::sort(seen.begin(), seen.end());
	std::vector<u32> all(draws.size());
	std::iota(all.begin(), all.end(), 0u);
	EXPECT_EQ(seen, all);
}

/// (b) Every pair the emission INVERTED is page-independent. The whole correctness argument, over
/// every ordered pair rather than over the ones a reader thought to check.
void ExpectInversionsIndependent(const std::vector<Draw>& draws, const std::vector<u32>& order)
{
	ASSERT_EQ(order.size(), draws.size());
	std::vector<u32> pos(draws.size(), 0);
	for (u32 i = 0; i < order.size(); i++)
		pos[order[i]] = i;
	for (u32 a = 0; a < draws.size(); a++)
	{
		for (u32 b = a + 1; b < draws.size(); b++)
		{
			if (pos[a] < pos[b])
				continue; // the stream order survived: nothing to prove
			// b now runs before a. Neither may see the other's writes, either way round.
			EXPECT_FALSE(draws[b].written.intersects(draws[a].written))
				<< "draws " << a << " and " << b << " were inverted but write the same page";
			EXPECT_FALSE(draws[b].written.intersects(draws[a].read))
				<< "draws " << a << " and " << b << " were inverted but " << b << " writes what " << a
				<< " reads";
			EXPECT_FALSE(draws[b].read.intersects(draws[a].written))
				<< "draws " << a << " and " << b << " were inverted but " << b << " reads what " << a
				<< " writes";
		}
	}
}

// -- the splice's half -------------------------------------------------------------------------

/// A stand-in for GSTileGpuPrepOp: the splice moves records and never looks inside one, so all the
/// test needs is something it can tell apart afterwards.
struct FakeOp
{
	u32 tag = 0;
	bool operator==(const FakeOp& o) const { return tag == o.tag; }
};

/// The plan's op array, laid out the way accumulation lays it out: each draw's slice appended in
/// draw order, so the slices tile the array.
struct OpPlan
{
	std::vector<FakeOp> ops;
	std::vector<u32> first;
	std::vector<u32> count;
};

OpPlan BuildOpPlan(const std::vector<Draw>& draws)
{
	OpPlan p;
	for (const Draw& d : draws)
	{
		p.first.push_back(static_cast<u32>(p.ops.size()));
		p.count.push_back(d.prep_ops);
		for (u32 k = 0; k < d.prep_ops; k++)
			p.ops.push_back(FakeOp{static_cast<u32>(p.ops.size()) + 1});
	}
	return p;
}

} // namespace

// -- the shapes the corpus actually has ----------------------------------------------------------

/// Dirge of Cerberus: a big framebuffer and a small scratch buffer, alternating, page-disjoint. The
/// shape the whole lever exists for.
TEST(GSTileGpuReorder, DisjointAlternationCollapsesToTwoRuns)
{
	std::vector<Draw> draws;
	for (u32 i = 0; i < 8; i++)
	{
		draws.push_back(Draw{kFrame, DepthMode::None, kZ, PageRange(224, 335), GSPageBitmap()});
		draws.push_back(Draw{kScratch, DepthMode::None, kZ, PageRange(341, 346), GSPageBitmap()});
	}
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	// Every framebuffer draw first, in stream order, then every scratch draw, in stream order.
	const std::vector<u32> expected{0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15};
	EXPECT_EQ(order, expected);
}

/// Gran Turismo 4's Online Public Beta: the same alternation to look at, but the framebuffer draws
/// SAMPLE the buffer the other draws render into. That is a data dependency, and no reordering
/// exists -- the order must come out unchanged.
TEST(GSTileGpuReorder, RenderThenSampleIsNotReorderable)
{
	std::vector<Draw> draws;
	for (u32 i = 0; i < 6; i++)
	{
		// the palette buffer, rendered
		draws.push_back(Draw{kScratch, DepthMode::None, kZ, Pages({96}), GSPageBitmap()});
		// ...and the framebuffer draw that reads it
		draws.push_back(Draw{kFrame, DepthMode::None, kZ, PageRange(0, 63), Pages({96})});
	}
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	std::vector<u32> identity(draws.size());
	std::iota(identity.begin(), identity.end(), 0u);
	EXPECT_EQ(order, identity) << "a read-after-write stream has no reorderable population";
}

/// A draw the class test refuses is a SEQUENCE POINT: nothing may be deferred across it, because
/// nothing has proved it independent of the backlog.
TEST(GSTileGpuReorder, NonReorderableDrawIsASequencePoint)
{
	std::vector<Draw> draws{
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap()},
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), GSPageBitmap()},
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap(), false}, // the barrier
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap()},
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), GSPageBitmap()},
	};
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	// The first pair is grouped, the barrier goes out in place, and the second pair is grouped after
	// it. No draw crosses index 2 in either direction.
	const std::vector<u32> expected{0, 1, 2, 3, 4};
	EXPECT_EQ(order, expected);
	std::vector<u32> pos(draws.size(), 0);
	for (u32 i = 0; i < order.size(); i++)
		pos[order[i]] = i;
	for (u32 d = 0; d < 2; d++)
		EXPECT_LT(pos[d], pos[2]);
	for (u32 d = 3; d < 5; d++)
		EXPECT_GT(pos[d], pos[2]);
}

/// A draw that writes a queued run's pages empties the backlog before it goes anywhere, and the
/// backlog comes out in OPEN order.
TEST(GSTileGpuReorder, HazardFlushesTheWholeBacklogInOpenOrder)
{
	std::vector<Draw> draws{
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap()},   // run 0
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), GSPageBitmap()},      // run 1
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap()},   // joins run 0
		// ...and now a third surface writing run 1's page: WAW, so everything goes out first.
		Draw{4, DepthMode::None, kZ, Pages({64}), GSPageBitmap()},
	};
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	const std::vector<u32> expected{0, 2, 1, 3};
	EXPECT_EQ(order, expected);
}

/// A draw may freely overlap the pages of the run it is JOINING -- order inside a run is preserved,
/// so there is nothing to prove. This is where a page bitmap beats a texture-pointer comparison,
/// which cannot tell which run a queued reader lives in.
TEST(GSTileGpuReorder, ARunMayOverlapItself)
{
	std::vector<Draw> draws;
	for (u32 i = 0; i < 4; i++)
	{
		// same target, same pages, and each one reads what the last wrote
		draws.push_back(Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), PageRange(0, 15)});
	}
	const std::vector<u32> order = Schedule(draws, 4);
	std::vector<u32> identity(draws.size());
	std::iota(identity.begin(), identity.end(), 0u);
	EXPECT_EQ(order, identity);
	Scheduler s;
	s.Reset(4);
	for (u32 i = 0; i < draws.size(); i++)
	{
		const Scheduler::Verdict v = s.Admit(KeyOf(draws[i]), draws[i].written, draws[i].read);
		EXPECT_EQ(v.hazards, 0u) << "draw " << i << " was refused against its own run";
		EXPECT_EQ(v.run, 0u);
		s.Stage(i, v.run, draws[i].written, draws[i].read);
	}
}

/// Two draws that READ the same page may not be inverted, which is not the usual hazard analysis
/// and is not conservatism. A read here can BUILD: the first reader of a page in guest order is the
/// one whose prep ops compose its ring slot, materialise its source image or gather its palette,
/// and every reader after it emits nothing and samples what that one built. Invert them and the
/// cache hit runs before the build, sampling an image nothing has written yet.
TEST(GSTileGpuReorder, TwoReadersOfOnePageAreNotInverted)
{
	std::vector<Draw> draws{
		// the framebuffer, sampling a texture at page 200
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), Pages({200})},
		// a page-disjoint scratch buffer -- but it samples the SAME texture
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), Pages({200})},
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), Pages({200})},
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), Pages({200})},
	};
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	std::vector<u32> identity(draws.size());
	std::iota(identity.begin(), identity.end(), 0u);
	EXPECT_EQ(order, identity) << "readers of one page were reordered against each other";

	// ...and the channel says so rather than the refusal being a side effect of another one.
	Scheduler s;
	s.Reset(4);
	s.Stage(0, s.Admit(KeyOf(draws[0]), draws[0].written, draws[0].read).run, draws[0].written, draws[0].read);
	const Scheduler::Verdict v = s.Admit(KeyOf(draws[1]), draws[1].written, draws[1].read);
	EXPECT_EQ(v.hazards, Scheduler::kHazardRar);
}

/// The run cap: a further key with no room for a run empties the backlog rather than dropping the
/// draw or growing past the cap.
TEST(GSTileGpuReorder, RunCapFlushesRatherThanGrowing)
{
	std::vector<Draw> draws{
		Draw{1, DepthMode::None, kZ, Pages({0}), GSPageBitmap()},
		Draw{2, DepthMode::None, kZ, Pages({1}), GSPageBitmap()},
		Draw{3, DepthMode::None, kZ, Pages({2}), GSPageBitmap()}, // the third key, cap is two
		Draw{1, DepthMode::None, kZ, Pages({0}), GSPageBitmap()},
	};
	Scheduler s;
	s.Reset(2);
	for (u32 i = 0; i < draws.size(); i++)
	{
		const Scheduler::Verdict v = s.Admit(KeyOf(draws[i]), draws[i].written, draws[i].read);
		EXPECT_LT(v.run, 2u) << "the scheduler opened a run past the cap";
		if (i == 2)
			EXPECT_EQ(v.runs_emitted, 2u) << "the cap should have emitted both open runs";
		s.Stage(i, v.run, draws[i].written, draws[i].read);
	}
	s.FlushAll();
	ExpectPermutation(draws, s.Order());
	ExpectInversionsIndependent(draws, s.Order());
}

/// One run is the identity, whatever the stream looks like. This is the arm the byte-identity gate
/// runs the corpus under before anything is allowed to move.
TEST(GSTileGpuReorder, OneRunIsTheIdentity)
{
	std::vector<Draw> draws;
	for (u32 i = 0; i < 12; i++)
	{
		const bool even = (i % 2) == 0;
		draws.push_back(Draw{even ? kFrame : kScratch, DepthMode::None, kZ,
			even ? PageRange(0, 15) : Pages({64}), GSPageBitmap()});
	}
	const std::vector<u32> order = Schedule(draws, 1);
	std::vector<u32> identity(draws.size());
	std::iota(identity.begin(), identity.end(), 0u);
	EXPECT_EQ(order, identity);
}

/// The depth attachment is part of the key, so two draws into one colour surface under different
/// depth surfaces are different runs -- and their Z footprints are what keeps them apart.
TEST(GSTileGpuReorder, DepthFootprintsAreTestedLikeColourOnes)
{
	std::vector<Draw> draws{
		// writes colour 0-15 and depth 128-143
		Draw{kFrame, DepthMode::TestWrite, kZ, PageRange(0, 15) | PageRange(128, 143), PageRange(128, 143)},
		Draw{kScratch, DepthMode::None, 0, Pages({64}), GSPageBitmap()},
		// a second draw into the scratch surface that WRITES the first draw's depth pages
		Draw{kScratch, DepthMode::None, 0, PageRange(130, 131), GSPageBitmap()},
	};
	const std::vector<u32> order = Schedule(draws, 4);
	ExpectPermutation(draws, order);
	ExpectInversionsIndependent(draws, order);
	std::vector<u32> pos(draws.size(), 0);
	for (u32 i = 0; i < order.size(); i++)
		pos[order[i]] = i;
	EXPECT_LT(pos[0], pos[2]) << "a draw writing another's depth pages was floated ahead of it";
}

// -- the splice ----------------------------------------------------------------------------------

/// (c) The prep-op rebuild: slices come out contiguous, in the new order, with their records
/// unchanged and their bases monotone. An array that stops tiling drops ops, and an op inside no
/// pass's range is silently never run.
TEST(GSTileGpuReorder, SpliceKeepsOpSlicesIntactAndMonotone)
{
	std::vector<Draw> draws{
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap(), true, 2},
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), GSPageBitmap(), true, 3},
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap(), true, 0},
		Draw{kScratch, DepthMode::None, kZ, Pages({64}), GSPageBitmap(), true, 1},
	};
	const OpPlan before = BuildOpPlan(draws);
	const std::vector<u32> order = Schedule(draws, 4);
	const std::vector<u32> expected{0, 2, 1, 3};
	ASSERT_EQ(order, expected);

	std::vector<u32> new_first(draws.size(), 0);
	std::vector<FakeOp> out;
	gsTileGpuSplicePrepOps(
		before.ops, order, [&](u32 d) { return before.first[d]; }, [&](u32 d) { return before.count[d]; },
		[&](u32 d, u32 base) { new_first[d] = base; }, out);

	EXPECT_EQ(out.size(), before.ops.size()) << "the splice dropped or duplicated an op";
	u32 running = 0;
	for (const u32 d : order)
	{
		// Monotone, and the slice starts exactly where the last one ended: that contiguity is what
		// the plan build's own pass range arithmetic reads.
		EXPECT_EQ(new_first[d], running) << "draw " << d << "'s slice is not contiguous with the last";
		for (u32 k = 0; k < before.count[d]; k++)
			EXPECT_EQ(out[new_first[d] + k], before.ops[before.first[d] + k])
				<< "draw " << d << "'s op " << k << " changed";
		running += before.count[d];
	}
	EXPECT_EQ(running, out.size());
}

/// A zero-length slice -- the dual-source companion's, whose ops belong to its principal and have
/// already run -- lands exactly at the end of the principal's, which is what keeps the pass's
/// [first, first + count) range covering both.
TEST(GSTileGpuReorder, SpliceKeepsAnEmptySliceAtItsPrincipalsEnd)
{
	std::vector<Draw> draws{
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap(), true, 3}, // the principal
		Draw{kFrame, DepthMode::None, kZ, PageRange(0, 15), GSPageBitmap(), true, 0}, // its companion
	};
	const OpPlan before = BuildOpPlan(draws);
	const std::vector<u32> order{0, 1};
	std::vector<u32> new_first(draws.size(), 0);
	std::vector<FakeOp> out;
	gsTileGpuSplicePrepOps(
		before.ops, order, [&](u32 d) { return before.first[d]; }, [&](u32 d) { return before.count[d]; },
		[&](u32 d, u32 base) { new_first[d] = base; }, out);
	EXPECT_EQ(new_first[1], new_first[0] + before.count[0]);
}

/// (d) The gather takes a draw's PAYLOAD from its old index and its state index from its new
/// POSITION. Those two are different numbers once anything moves, and taking the old index for the
/// state row is the mistake that reads another draw's transform instead of failing.
TEST(GSTileGpuReorder, SpliceTakesPayloadByIndexAndStateIndexByPosition)
{
	std::vector<Draw> draws;
	for (u32 i = 0; i < 6; i++)
	{
		const bool even = (i % 2) == 0;
		draws.push_back(Draw{even ? kFrame : kScratch, DepthMode::None, kZ,
			even ? PageRange(0, 15) : Pages({64}), GSPageBitmap()});
	}
	const std::vector<u32> order = Schedule(draws, 4);
	const std::vector<u32> expected{0, 2, 4, 1, 3, 5};
	ASSERT_EQ(order, expected) << "the two page-disjoint surfaces should have grouped";

	// The plan's per-draw stream, tagged so a mis-paired gather is visible. first_index stands in for
	// everything a draw record carries about ITSELF -- its geometry, its blend key, its scissor.
	std::vector<GSDevice::GSTileGpuIndirectDraw> plan(draws.size());
	for (u32 i = 0; i < plan.size(); i++)
	{
		plan[i] = GSDevice::GSTileGpuIndirectDraw{};
		plan[i].first_index = 100 + i;
		plan[i].state_index = i;
	}

	std::vector<GSDevice::GSTileGpuIndirectDraw> out;
	gsTileGpuGatherByOrder(plan, order, out);
	for (u32 pos = 0; pos < out.size(); pos++)
		out[pos].state_index = pos;

	ASSERT_EQ(out.size(), plan.size());
	for (u32 pos = 0; pos < out.size(); pos++)
	{
		EXPECT_EQ(out[pos].first_index, 100 + order[pos]) << "position " << pos << " took the wrong draw";
		EXPECT_EQ(out[pos].state_index, pos) << "position " << pos << " kept a stale state index";
	}
}

// -- the level itself: who decides how wide the scheduler runs ------------------------------------
//
// Everything above drives the scheduler at a width a caller picked. This is where that width comes
// from, and it is a composition of two sources that can disagree: the INI key
// EmuCore/GS/TileGpuReorderRuns and the game database's coalesceRenderPasses bit.
//
// Pinned rather than left to the renderer's constructor because the failure is silent in BOTH
// directions and neither shows in an image. A gate that stops firing gives back a thousand render
// passes a frame on the one title that needs them collected, and nobody notices until a device
// round; a forced arm the database quietly overrides stops being an arm, and every A/B taken
// through it measures the same build twice while reporting two numbers.

namespace
{
constexpr int kAuto = Pcsx2Config::GSOptions::TileGpuReorderRunsAuto;
constexpr int kGameDb = Pcsx2Config::GSOptions::TileGpuReorderRunsGameDB;
} // namespace

/// AUTO is what ships, so a title the database says nothing about must get exactly the behaviour
/// that shipped before the gate existed: nothing runs, nothing is counted, nothing costs.
TEST(GSTileGpuReorderLevel, AutoIsOffWhereTheDatabaseIsSilent)
{
	EXPECT_EQ(gsTileGpuReorderRuns(kAuto, false), 0);
}

/// ...and a title it does name takes the database width. This is the whole lever: Dirge of Cerberus
/// (SLUS-21419) carries coalesceRenderPasses, so it reorders, and the other twenty-one dumps in the
/// corpus do not.
TEST(GSTileGpuReorderLevel, AutoTakesTheDatabaseWidthWhereTheBitIsSet)
{
	EXPECT_EQ(gsTileGpuReorderRuns(kAuto, true), kGameDb);
	EXPECT_EQ(kGameDb, 4) << "every R8 device number was measured at four runs";
}

/// A forced 0 outranks the database. Without this the campaign's OFF arm silently stops being an
/// arm on exactly the one title the A/B is about, and the pair of numbers it produces are two
/// readings of the same build.
TEST(GSTileGpuReorderLevel, AnExplicitOffOutranksTheDatabase)
{
	EXPECT_EQ(gsTileGpuReorderRuns(0, true), 0);
	EXPECT_EQ(gsTileGpuReorderRuns(0, false), 0);
}

/// ...and so does a forced width, in the other direction: an ungated title can still be measured
/// with reordering on, which is how every non-dirge row of the reorder census was taken.
TEST(GSTileGpuReorderLevel, AnExplicitWidthOutranksTheDatabase)
{
	EXPECT_EQ(gsTileGpuReorderRuns(4, false), 4);
	EXPECT_EQ(gsTileGpuReorderRuns(2, true), 2) << "a forced width is not raised to the database's";
	EXPECT_EQ(gsTileGpuReorderRuns(8, true), 8);
}

/// The census arm keeps its sign and its width on both kinds of title. -1 is AUTO now, so a census
/// is spelt from -2 down; -1 was the one width that could never have reported anything, since a
/// scheduler holding a single run open is the identity (OneRunIsTheIdentity above).
TEST(GSTileGpuReorderLevel, TheCensusArmSurvivesTheGate)
{
	EXPECT_EQ(gsTileGpuReorderRuns(-4, false), -4);
	EXPECT_EQ(gsTileGpuReorderRuns(-4, true), -4) << "a census must not turn into a reorder on a gated title";
	EXPECT_EQ(gsTileGpuReorderRuns(-2, true), -2);
}

/// The shipped default IS auto. Stated as its own test because the whole design rests on it: a
/// default of 0 would make the shipped value and an explicit "-set ...=0" the same integer, and the
/// gate could then never tell "the player said off" from "the player said nothing".
TEST(GSTileGpuReorderLevel, TheShippedDefaultIsAuto)
{
	const Pcsx2Config::GSOptions shipped;
	EXPECT_EQ(shipped.TileGpuReorderRuns, kAuto);
	EXPECT_NE(kAuto, 0) << "AUTO must be distinguishable from a forced off";
}
