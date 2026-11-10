// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the rule that orders a cached texture's refill against the queued draws that
// still read it (GS/Renderers/SW/GSSwTextureReaders.h).
//
// The software renderer's workers sample a texture from the cache's buffer. Call of
// Duty 2 and 3 draw 8x8 tiles onto a page that also holds the tiles' texture, so one
// draw in three invalidates the texture the draw before it is still sampling. With
// three workers the two sit on different workers, the invalidating draw finishes
// first, and the next tile refilled the buffer under the first one: two replays of one
// dump gave different pictures.
//
// The model is the three calls the renderer makes, in its order:
//   a draw that samples the texture   DrainBeforeUpdate(), then Acquire()
//   a draw that writes its pages      Invalidated()
//   a draw's data is destroyed        Release()
//
// Rides gs_vertex_tests -- the rule is a header-only struct, so it needs no extra linkage.

#include "GS/Renderers/SW/GSSwTextureReaders.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace
{
	// What GSRendererSW::CheckSourcePages and SharedData::UsePages do for a draw that
	// samples the texture. Returns whether the workers had to be drained first.
	bool QueueSampler(GSSwTextureReaders& t)
	{
		const bool drain = t.DrainBeforeUpdate();
		t.Acquire();
		return drain;
	}
} // namespace

// The Call of Duty sequence. Tile 1 samples the texture and is queued. Tile 2 draws onto
// the texture's page and finishes at once on another worker, so nobody is drawing onto
// the page any more when tile 3 arrives. Tile 3's refill must still wait for tile 1.
TEST(GSSwTextureReaders, RefillWaitsForAnEarlierReader)
{
	GSSwTextureReaders t;

	EXPECT_FALSE(QueueSampler(t)) << "first use: nothing is queued";

	t.Invalidated(); // tile 2 wrote the texture's page; tile 1 still holds the buffer

	EXPECT_TRUE(QueueSampler(t)) << "tile 3 would refill the buffer tile 1 is reading";
}

// The drain settles it: the same invalidation does not drain a second time.
TEST(GSSwTextureReaders, OneDrainPerInvalidation)
{
	GSSwTextureReaders t;

	QueueSampler(t);
	t.Invalidated();
	ASSERT_TRUE(QueueSampler(t));

	t.Release(); // the drain finished tile 1

	EXPECT_FALSE(QueueSampler(t));
	EXPECT_FALSE(QueueSampler(t));
}

// The reader finished by itself before the next draw sampled the texture. Nothing reads
// the buffer, so the refill needs no drain, and the stale flag does not survive to
// drain a later draw.
TEST(GSSwTextureReaders, NoDrainOnceTheReaderHasFinished)
{
	GSSwTextureReaders t;

	QueueSampler(t);
	t.Invalidated();
	t.Release();

	EXPECT_FALSE(QueueSampler(t));

	// That sampler is still queued; no invalidation since.
	EXPECT_FALSE(QueueSampler(t));
}

// An invalidation with no queued reader costs nothing later, however many draws then
// share the texture.
TEST(GSSwTextureReaders, InvalidationWithoutReadersIsFree)
{
	GSSwTextureReaders t;

	t.Invalidated();

	for (int i = 0; i < 8; i++)
		EXPECT_FALSE(QueueSampler(t)) << i;
}

// Draws that share a texture do not drain each other: only blocks nobody has loaded
// yet are filled, and no queued draw's rectangle covers those. This is the common case
// (an atlas, a font) and it must stay free.
TEST(GSSwTextureReaders, SharingWithoutInvalidationNeverDrains)
{
	GSSwTextureReaders t;

	for (int i = 0; i < 64; i++)
		EXPECT_FALSE(QueueSampler(t)) << i;

	for (int i = 0; i < 64; i++)
		t.Release();

	EXPECT_EQ(t.queued.load(), 0u);
}

// Every invalidation under a reader is answered once, in whatever order the readers
// come and go.
TEST(GSSwTextureReaders, RepeatedInvalidationsEachDrainOnce)
{
	GSSwTextureReaders t;
	int drains = 0;

	QueueSampler(t);

	for (int tile = 0; tile < 100; tile++)
	{
		t.Invalidated();

		if (QueueSampler(t))
		{
			drains++;

			// The drain: everything queued before this draw is finished.
			t.Release();
		}
	}

	EXPECT_EQ(drains, 100);
	EXPECT_EQ(t.queued.load(), 1u);
}

// The count is released on the workers while the GS thread acquires. It must not lose
// updates.
TEST(GSSwTextureReaders, CountIsExactAcrossThreads)
{
	GSSwTextureReaders t;
	constexpr int rounds = 100000;

	for (int i = 0; i < 2 * rounds; i++)
		t.Acquire();

	std::vector<std::thread> workers;

	for (int w = 0; w < 2; w++)
	{
		workers.emplace_back([&t]() {
			for (int i = 0; i < rounds; i++)
				t.Release();
		});
	}

	for (int i = 0; i < rounds; i++)
	{
		t.Acquire();
		t.Release();
	}

	for (std::thread& w : workers)
		w.join();

	EXPECT_EQ(t.queued.load(), 0u);
}
