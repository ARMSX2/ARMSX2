// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the range bookkeeping that lets a non-coherent Vulkan stream ring clean its writes once
// before the submit that reads them instead of once per commit
// (GS/Renderers/Common/GSStreamRingFlushRange.h).
//
// This is the only place the deferred flush can be checked on this desk. Every host-visible memory
// type the dev box offers is coherent, so its rings never take the road at all and its identity
// grid says nothing about the arithmetic below. The cases are therefore written from the ring's
// own behaviour rather than from a run: commits march forward, the offset resets to zero when the
// ring wraps, and a flush empties the ledger.
//
// Rides gs_vertex_tests -- the tracker is a header-only struct, so it needs no extra linkage.

#include "GS/Renderers/Common/GSStreamRingFlushRange.h"

#include <gtest/gtest.h>

TEST(GSStreamRingFlush, EmptyToStart)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.IsEmpty());
	EXPECT_EQ(pending.count, 0u);
	EXPECT_EQ(pending.commits, 0u);
}

TEST(GSStreamRingFlush, OneCommitIsOneRange)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(256, 64));

	ASSERT_EQ(pending.count, 1u);
	EXPECT_FALSE(pending.IsEmpty());
	EXPECT_EQ(pending.ranges[0].begin, 256u);
	EXPECT_EQ(pending.ranges[0].end, 320u);
	EXPECT_EQ(pending.ranges[0].size(), 64u);
	EXPECT_EQ(pending.commits, 1u);
}

// The case the rung exists for: a draw-heavy title commits many small regions between two submits,
// and they collapse into one range covering the same bytes.
TEST(GSStreamRingFlush, AdjacentCommitsCoalesce)
{
	GSStreamRingFlushRanges pending;
	for (u32 i = 0; i < 1000; i++)
		EXPECT_TRUE(pending.Add(i * 48, 48));

	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].begin, 0u);
	EXPECT_EQ(pending.ranges[0].end, 48000u);
	EXPECT_EQ(pending.commits, 1000u);
}

// ReserveMemory aligns each commit's start, so consecutive regions are usually separated by a few
// bytes of padding. The gap is swallowed: cleaning it writes back bytes only the CPU has ever
// touched, and splitting the range in two would reintroduce the per-call price being removed.
TEST(GSStreamRingFlush, AlignmentGapsAreAbsorbed)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(0, 100));
	EXPECT_TRUE(pending.Add(128, 100));
	EXPECT_TRUE(pending.Add(256, 100));

	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].begin, 0u);
	EXPECT_EQ(pending.ranges[0].end, 356u);
	EXPECT_EQ(pending.commits, 3u);
}

TEST(GSStreamRingFlush, WrapMakesASecondRange)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(1024 * 1024, 4096));
	EXPECT_TRUE(pending.Add(1024 * 1024 + 4096, 4096));
	// The ring ran out of room at the top and restarted at zero behind the GPU.
	EXPECT_TRUE(pending.Add(0, 2048));

	ASSERT_EQ(pending.count, 2u);
	EXPECT_EQ(pending.ranges[0].begin, 1024u * 1024u);
	EXPECT_EQ(pending.ranges[0].end, 1024u * 1024u + 8192u);
	EXPECT_EQ(pending.ranges[1].begin, 0u);
	EXPECT_EQ(pending.ranges[1].end, 2048u);
	EXPECT_EQ(pending.commits, 3u);
}

TEST(GSStreamRingFlush, CommitsAfterAWrapExtendTheLowRange)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(8192, 512));
	EXPECT_TRUE(pending.Add(0, 512));
	EXPECT_TRUE(pending.Add(512, 512));
	EXPECT_TRUE(pending.Add(1024, 512));

	ASSERT_EQ(pending.count, 2u);
	EXPECT_EQ(pending.ranges[0].begin, 8192u);
	EXPECT_EQ(pending.ranges[0].end, 8704u);
	EXPECT_EQ(pending.ranges[1].begin, 0u);
	EXPECT_EQ(pending.ranges[1].end, 1536u);
	EXPECT_EQ(pending.commits, 4u);
}

// A second wrap with the first still unflushed cannot happen -- reusing the high range's bytes
// means the GPU consumed them, which means a fence completed, which means a submit, which means a
// flush. Add says so rather than assuming it, and the ring flushes early on the false.
TEST(GSStreamRingFlush, SecondWrapReportsOverflow)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(8192, 512));
	EXPECT_TRUE(pending.Add(4096, 512));
	EXPECT_FALSE(pending.Add(0, 512));

	// Nothing was recorded by the rejected call, so the caller can flush exactly what it has.
	ASSERT_EQ(pending.count, 2u);
	EXPECT_EQ(pending.ranges[1].begin, 4096u);
	EXPECT_EQ(pending.ranges[1].end, 4608u);
	EXPECT_EQ(pending.commits, 2u);

	// And after the flush the same region is accepted.
	pending.Reset();
	EXPECT_TRUE(pending.Add(0, 512));
	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].begin, 0u);
	EXPECT_EQ(pending.commits, 1u);
}

TEST(GSStreamRingFlush, ResetEmptiesTheLedger)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(64, 64));
	EXPECT_TRUE(pending.Add(0, 64));
	ASSERT_EQ(pending.count, 2u);

	pending.Reset();
	EXPECT_TRUE(pending.IsEmpty());
	EXPECT_EQ(pending.count, 0u);
	EXPECT_EQ(pending.commits, 0u);

	// The next commit starts a fresh range rather than extending a stale one.
	EXPECT_TRUE(pending.Add(4096, 32));
	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].begin, 4096u);
	EXPECT_EQ(pending.ranges[0].end, 4128u);
}

TEST(GSStreamRingFlush, ZeroByteCommitChangesNothing)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(0, 0));
	EXPECT_TRUE(pending.IsEmpty());
	EXPECT_EQ(pending.commits, 0u);

	EXPECT_TRUE(pending.Add(128, 64));
	EXPECT_TRUE(pending.Add(192, 0));
	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].end, 192u);
	EXPECT_EQ(pending.commits, 1u);
}

// A commit that lands entirely inside what is already pending must not shrink the range. The ring
// does not do this today; the guard is here because a range that shrank would leave written bytes
// uncleaned, which is silent wrong output on the one device that takes this road.
TEST(GSStreamRingFlush, ContainedCommitDoesNotShrinkTheRange)
{
	GSStreamRingFlushRanges pending;
	EXPECT_TRUE(pending.Add(0, 4096));
	EXPECT_TRUE(pending.Add(1024, 16));

	ASSERT_EQ(pending.count, 1u);
	EXPECT_EQ(pending.ranges[0].begin, 0u);
	EXPECT_EQ(pending.ranges[0].end, 4096u);
	EXPECT_EQ(pending.commits, 2u);
}

// Six rings share the machinery and nothing else. A flush of one must not touch another's ledger,
// which is what makes "one flush per ring per submit" the number the device round reads.
TEST(GSStreamRingFlush, RingsAreIndependent)
{
	GSStreamRingFlushRanges vertex;
	GSStreamRingFlushRanges index;

	EXPECT_TRUE(vertex.Add(0, 512));
	EXPECT_TRUE(index.Add(8192, 128));
	EXPECT_TRUE(vertex.Add(512, 512));

	vertex.Reset();

	EXPECT_TRUE(vertex.IsEmpty());
	ASSERT_EQ(index.count, 1u);
	EXPECT_EQ(index.ranges[0].begin, 8192u);
	EXPECT_EQ(index.ranges[0].end, 8320u);
	EXPECT_EQ(index.commits, 1u);
}

// The frame shape the rung is aimed at, end to end: ac3 commits vertices and indices for 1,626
// draws between submits. The per-commit road issues 3,252 cleans a frame; this issues two.
TEST(GSStreamRingFlush, DrawHeavyFrameCollapsesToOneFlushPerRing)
{
	GSStreamRingFlushRanges vertex;
	GSStreamRingFlushRanges index;

	u32 vertex_offset = 0;
	u32 index_offset = 0;
	for (u32 draw = 0; draw < 1626; draw++)
	{
		EXPECT_TRUE(vertex.Add(vertex_offset, 128));
		vertex_offset += 128;
		EXPECT_TRUE(index.Add(index_offset, 24));
		index_offset += 32; // aligned up to 16 bytes, so a gap every draw
	}

	EXPECT_EQ(vertex.count, 1u);
	EXPECT_EQ(vertex.commits, 1626u);
	EXPECT_EQ(vertex.ranges[0].size(), 1626u * 128u);

	EXPECT_EQ(index.count, 1u);
	EXPECT_EQ(index.commits, 1626u);
	// The last commit's own 24 bytes, not the 32 it was aligned to.
	EXPECT_EQ(index.ranges[0].size(), 1625u * 32u + 24u);
}
