// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The writeback batch's decision: which ops become one dispatch, which share a barrier bracket,
// and -- the half that has to hold -- which must NOT be merged.
//
// A writeback op is a compute dispatch with a whole-buffer ring barrier on each side. The bracket
// is a cache flush plus invalidate plus a drain on a tiler, and it is most of the campaign's 3.67 us
// per-op fixed term; Spider-Man 3 records 1,101 ops a frame against 3,100 pages, so the commands
// around the composes cost about a fifth of what the composes do. GSTileGpuWritebackBatch answers
// two questions per op -- does it stay in the open barrier run, and does it fold into the dispatch
// already accumulating -- and the executor and the renderer's census both walk it.
//
// The refusals are the tests that matter, because every one of them is a wrong-pixel road:
//   - a page a run has already claimed. Two ops naming one page write disjoint BYTE lanes of the
//     same words (a PSMCT24 surface's RGB against another surface's alpha plane), which is a
//     read-modify-write race between z groups of one dispatch and between two unbracketed ones.
//   - a different source image, layout, byte mask or epoch. Every one of those is a descriptor or
//     a push constant the dispatch fixes for all its entries.
//   - a hole in the page-entry range. The shader reads its entry at first_entry + gl_WorkGroupID.z,
//     so a merged range that is not contiguous reads some other op's rows.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
using Batch = GSDevice::GSTileGpuWritebackBatch;
using Op = GSDevice::GSTileGpuPrepOp;
using Entry = GSDevice::GSTileGpuPageEntry;
using Kind = GSDevice::GSTileGpuPrepKind;

// A plan under construction: ops appended in order, each taking the next rows of one page-entry
// array, exactly as GSRendererTileGpu::EmitPrepOp builds them.
struct PlanBuilder
{
	std::vector<Op> ops;
	std::vector<Entry> entries;

	Op& Add(Kind kind, u32 target, const std::vector<u16>& pages, u32 psm = 0, u32 epoch = 0,
		u32 byte_mask = 0xFFFFFFFFu, u32 bp = 0, u32 bw = 8)
	{
		Op op = {};
		op.kind = kind;
		op.target = target;
		op.psm = psm;
		op.epoch = epoch;
		op.byte_mask = byte_mask;
		op.bp = bp;
		op.bw = bw;
		op.first_page_entry = static_cast<u32>(entries.size());
		for (const u16 p : pages)
			entries.push_back(Entry{p, 0, 0xFFFFFFFFu, GSDevice::kGSTileGpuNoKeepMask});
		op.page_entry_count = static_cast<u32>(pages.size());
		ops.push_back(op);
		return ops.back();
	}

	// A row of page entries nobody's op names, to put a HOLE in the array between two ops.
	void Gap(u32 rows)
	{
		for (u32 i = 0; i < rows; i++)
			entries.push_back(Entry{0, 0, 0, GSDevice::kGSTileGpuNoKeepMask});
	}

	u32 Dispatches() const
	{
		return Batch::CountDispatches(ops.data(), static_cast<u32>(ops.size()), entries.data());
	}
	u32 Runs() const { return Batch::CountRuns(ops.data(), static_cast<u32>(ops.size()), entries.data()); }
};

// The dispatch each offer produces, in order: {first_page_entry, page_entry_count, ops_merged}.
struct Recorded
{
	u32 first;
	u32 count;
	u32 ops;
};
std::vector<Recorded> Record(const PlanBuilder& p, u32* runs_out = nullptr)
{
	Batch b;
	std::vector<Recorded> out;
	u32 runs = 0;
	const auto take = [&](const Batch::Step& s) {
		if (s.flush_dispatch)
			out.push_back({b.Flushed().op.first_page_entry, b.Flushed().op.page_entry_count, b.Flushed().op_count});
		runs += s.open_run ? 1u : 0u;
	};
	for (const Op& op : p.ops)
	{
		if (op.kind != Kind::Writeback)
		{
			take(b.Finish());
			continue;
		}
		if (op.page_entry_count == 0)
			continue;
		take(b.Offer(op, p.entries.data()));
	}
	take(b.Finish());
	if (runs_out)
		*runs_out = runs;
	return out;
}
} // namespace

// The shape the road actually emits: consecutive composes for one owner, one after another with
// nothing between them. Three ops, one dispatch, one bracket.
TEST(GSTileGpuWritebackBatch, ARunOfOneOwnersOpsIsOneDispatch)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10, 11});
	p.Add(Kind::Writeback, 3, {12});
	p.Add(Kind::Writeback, 3, {13, 14, 15});

	const auto rec = Record(p);
	ASSERT_EQ(rec.size(), 1u);
	EXPECT_EQ(rec[0].first, 0u);
	EXPECT_EQ(rec[0].count, 6u); // every page of all three, in emission order
	EXPECT_EQ(rec[0].ops, 3u);
	EXPECT_EQ(p.Dispatches(), 1u);
	EXPECT_EQ(p.Runs(), 1u);
}

// A merge may never span a hole: the shader walks entries linearly from first_entry, so a gap
// would put rows nobody's op owns inside the dispatch.
TEST(GSTileGpuWritebackBatch, ADiscontiguousEntryRangeDoesNotMerge)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10, 11});
	p.Gap(2);
	p.Add(Kind::Writeback, 3, {12});

	const auto rec = Record(p);
	ASSERT_EQ(rec.size(), 2u);
	EXPECT_EQ(rec[0].count, 2u);
	EXPECT_EQ(rec[1].first, 4u);
	EXPECT_EQ(rec[1].count, 1u);
	// ...but they still share one bracket: nothing between them touches the ring.
	EXPECT_EQ(p.Runs(), 1u);
}

// Two owners in one run: two dispatches, because the source image is a descriptor the dispatch
// fixes -- and still ONE barrier bracket, which is the larger half of what a batch saves.
TEST(GSTileGpuWritebackBatch, TwoOwnersShareOneBracketAndTakeTwoDispatches)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});
	p.Add(Kind::Writeback, 4, {20});
	p.Add(Kind::Writeback, 3, {11});

	u32 runs = 0;
	const auto rec = Record(p, &runs);
	EXPECT_EQ(rec.size(), 3u);
	EXPECT_EQ(runs, 1u);
	EXPECT_EQ(p.Dispatches(), 3u);
	EXPECT_EQ(p.Runs(), 1u);
}

// ⚠️ THE REFUSAL THE WHOLE DESIGN RESTS ON. A page a run has already claimed re-opens the bracket,
// so the second op's read-modify-write of those words is ordered behind the first's.
TEST(GSTileGpuWritebackBatch, APageCollisionSplitsTheRun)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10, 11}, /*psm*/ 0, /*epoch*/ 0, /*byte_mask*/ 0x00FFFFFFu);
	p.Add(Kind::Writeback, 4, {11}); // the alpha plane's owner, same page, disjoint bytes

	u32 runs = 0;
	const auto rec = Record(p, &runs);
	EXPECT_EQ(rec.size(), 2u);
	EXPECT_EQ(runs, 2u); // two brackets: the second op waits on the first
	EXPECT_EQ(p.Runs(), 2u);
}

// ...and the collision test is over the whole open run, not just the op before it.
TEST(GSTileGpuWritebackBatch, ACollisionWithAnEarlierOpOfTheRunAlsoSplitsIt)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});
	p.Add(Kind::Writeback, 4, {20});
	p.Add(Kind::Writeback, 5, {10}); // collides with the FIRST op, not the one before it

	EXPECT_EQ(p.Runs(), 2u);
	EXPECT_EQ(p.Dispatches(), 3u);
}

// A run may not span an op of any other kind: a seed, a materialise or an expand reads the ring,
// so the composes before it must be published first.
TEST(GSTileGpuWritebackBatch, AnyOtherKindEndsTheRun)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});
	p.Add(Kind::Seed, 3, {10});
	p.Add(Kind::Writeback, 3, {11});

	EXPECT_EQ(p.Dispatches(), 2u);
	EXPECT_EQ(p.Runs(), 2u);
}

// Every field the dispatch fixes refuses a merge on its own. Each arm is the same two ops with one
// field moved, and each must come out as two dispatches inside one bracket.
TEST(GSTileGpuWritebackBatch, EveryFixedParameterRefusesAMerge)
{
	struct Arm
	{
		const char* what;
		u32 target, psm, epoch, byte_mask, bp, bw;
	};
	static constexpr Arm kBase = {"base", 3, 0, 0, 0xFFFFFFFFu, 0, 8};
	static constexpr Arm kArms[] = {
		{"target", 4, 0, 0, 0xFFFFFFFFu, 0, 8},
		{"psm", 3, 1, 0, 0xFFFFFFFFu, 0, 8},
		{"epoch", 3, 0, 1, 0xFFFFFFFFu, 0, 8},
		{"byte_mask", 3, 0, 0, 0x00FFFFFFu, 0, 8},
		{"bp", 3, 0, 0, 0xFFFFFFFFu, 32, 8},
		{"bw", 3, 0, 0, 0xFFFFFFFFu, 0, 4},
	};
	for (const Arm& a : kArms)
	{
		PlanBuilder p;
		p.Add(Kind::Writeback, kBase.target, {10}, kBase.psm, kBase.epoch, kBase.byte_mask, kBase.bp, kBase.bw);
		p.Add(Kind::Writeback, a.target, {11}, a.psm, a.epoch, a.byte_mask, a.bp, a.bw);
		EXPECT_EQ(p.Dispatches(), 2u) << a.what << " must not merge";
		EXPECT_EQ(p.Runs(), 1u) << a.what << " must still share the bracket";
	}
	// ...and the control: the same two ops with nothing moved ARE one dispatch, so the arms above
	// are testing the field and not the harness.
	PlanBuilder p;
	p.Add(Kind::Writeback, kBase.target, {10}, kBase.psm, kBase.epoch, kBase.byte_mask, kBase.bp, kBase.bw);
	p.Add(Kind::Writeback, kBase.target, {11}, kBase.psm, kBase.epoch, kBase.byte_mask, kBase.bp, kBase.bw);
	EXPECT_EQ(p.Dispatches(), 1u);
}

// The batch is a state machine over a range, and the range is a PASS. Reset must leave it as new,
// or the second pass's first op inherits the first pass's page set and splits for nothing.
TEST(GSTileGpuWritebackBatch, ResetForgetsTheRun)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});

	Batch b;
	EXPECT_TRUE(b.Offer(p.ops[0], p.entries.data()).open_run);
	b.Reset();
	// The same op again: a fresh batch must open a run rather than see a collision, and must have
	// dropped the pending dispatch rather than flush it.
	const Batch::Step s = b.Offer(p.ops[0], p.entries.data());
	EXPECT_TRUE(s.open_run);
	EXPECT_FALSE(s.flush_dispatch);
	EXPECT_FALSE(s.close_run);
}

// The last dispatch of a range is published by Finish and by nothing else -- the failure mode is a
// plan whose final composes are recorded with no barrier after them.
TEST(GSTileGpuWritebackBatch, FinishPublishesTheLastDispatch)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});

	Batch b;
	const Batch::Step opened = b.Offer(p.ops[0], p.entries.data());
	EXPECT_FALSE(opened.flush_dispatch);
	const Batch::Step done = b.Finish();
	EXPECT_TRUE(done.flush_dispatch);
	EXPECT_TRUE(done.close_run);
	EXPECT_EQ(b.Flushed().op.page_entry_count, 1u);
	// ...and a second Finish publishes nothing, so a caller that closes twice cannot double-record.
	const Batch::Step again = b.Finish();
	EXPECT_FALSE(again.flush_dispatch);
	EXPECT_FALSE(again.close_run);
}

// The census walk and the executor's walk must agree, and the merged range must be exactly the
// union of what the ops named -- no row dropped, none read twice. This is the arithmetic the
// byte-identity gate would otherwise be the only thing checking.
TEST(GSTileGpuWritebackBatch, TheMergedRangeCoversEveryEntryExactlyOnce)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10, 11});
	p.Add(Kind::Writeback, 3, {12});
	p.Add(Kind::Writeback, 4, {20, 21});
	p.Add(Kind::Writeback, 4, {22});
	p.Add(Kind::Seed, 3, {10});
	p.Add(Kind::Writeback, 3, {30});

	const auto rec = Record(p);
	ASSERT_EQ(rec.size(), 3u);
	// Every row a WRITEBACK op named, once; the seed's own rows sit in the same array and must not
	// be swept into a dispatch by a merge that crossed it.
	std::vector<u32> want(p.entries.size(), 0), covered(p.entries.size(), 0);
	for (const Op& op : p.ops)
	{
		if (op.kind != Kind::Writeback)
			continue;
		for (u32 i = 0; i < op.page_entry_count; i++)
			want[op.first_page_entry + i] = 1;
	}
	for (const Recorded& r : rec)
	{
		for (u32 i = 0; i < r.count; i++)
			covered[r.first + i]++;
	}
	for (u32 i = 0; i < covered.size(); i++)
		EXPECT_EQ(covered[i], want[i]) << "entry row " << i;
	EXPECT_EQ(p.Dispatches(), static_cast<u32>(rec.size()));
}

// An op with no pages is not an op: EmitPrepOp never files one, and the walk must not let it end a
// run (which would cost a bracket for nothing).
TEST(GSTileGpuWritebackBatch, AnEmptyOpIsSkippedRatherThanEndingTheRun)
{
	PlanBuilder p;
	p.Add(Kind::Writeback, 3, {10});
	p.Add(Kind::Writeback, 3, {}); // page_entry_count == 0
	p.Add(Kind::Writeback, 3, {11});

	// Two dispatches, because the empty op's entry range makes the third op's rows discontiguous
	// with the first's only if it consumed rows -- it consumes none, so this is one dispatch.
	EXPECT_EQ(p.Dispatches(), 1u);
	EXPECT_EQ(p.Runs(), 1u);
}
