// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins what GSTextureCache::AgeHashCache keeps of the hash cache: entries unused for 30 frames go,
// and past the entry limit or the memory limit the oldest unreferenced entries go first, the memory
// limit never taking the cache below its floor of entries.
//
// The entries are made with InjectHashCacheTexture, which adds an unreferenced entry for a key that
// has none. Each texture is one texel high, so its size in bytes says which generation it came from
// and the bytes the cache holds say which entries are left.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

using namespace GSHWDrawHarness;

namespace
{
	class GSHashCacheLimits : public Fixture
	{
	protected:
		void TearDown() override
		{
			GSTextureCache::SetHashCacheLimits(GSTextureCache::HashCacheLimits());
			Fixture::TearDown();
		}

		/// `count` entries of `width` x 1 texels, none of the keys used before.
		void Inject(u32 count, int width)
		{
			for (u32 i = 0; i < count; i++)
			{
				GSTexture* const tex = g_gs_device->CreateTexture(width, 1, 1, GSTexture::Format::Color);
				ASSERT_NE(tex, nullptr);
				GSTextureCache::HashCacheKey key;
				key.TEX0Hash = ++m_next_key;
				g_texture_cache->InjectHashCacheTexture(key, tex, {0, 255}, false);
			}
		}

		static u64 Bytes() { return g_texture_cache->GetTotalHashCacheMemoryUsage(); }
		static size_t Entries() { return g_texture_cache->GetHashCacheEntryCount(); }

		u64 m_next_key = 0;
	};

	TEST_F(GSHashCacheLimits, DefaultsKeepTheOldEntryLimitAsTheFloor)
	{
		const GSTextureCache::HashCacheLimits limits = GSTextureCache::GetHashCacheLimits();
		EXPECT_EQ(limits.floor_entries, 800u);
		EXPECT_GT(limits.max_entries, limits.floor_entries);
		EXPECT_GT(limits.max_bytes, 0u);
	}

	TEST_F(GSHashCacheLimits, EntryLimitDropsTheOldestFirst)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 2;
		limits.max_entries = 10;
		limits.max_bytes = 0;
		GSTextureCache::SetHashCacheLimits(limits);

		// Three frames of eight entries, 40, 80 and 120 bytes each.
		Inject(8, 10);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 8u);
		EXPECT_EQ(Bytes(), 8u * 40u);

		// 16 entries: the six oldest go, all of them the first frame's.
		Inject(8, 20);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 10u);
		EXPECT_EQ(Bytes(), 2u * 40u + 8u * 80u);

		// 18 entries: the other two of the first frame, then six of the second.
		Inject(8, 30);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 10u);
		EXPECT_EQ(Bytes(), 2u * 80u + 8u * 120u);
	}

	TEST_F(GSHashCacheLimits, MemoryLimitDropsTheOldestFirstAndKeepsTheFloor)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 4;
		limits.max_entries = 0;
		limits.max_bytes = 0;
		GSTextureCache::SetHashCacheLimits(limits);

		// Ten entries of 100 bytes in one frame, then ten of 200 bytes in the next: 3000 bytes.
		Inject(10, 25);
		g_texture_cache->IncAge();
		Inject(10, 50);

		// No memory limit: nothing goes.
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 20u);
		EXPECT_EQ(Bytes(), 3000u);

		// 2200 bytes: eight of the oldest entries, 100 bytes each, go (3000 - 800).
		limits.max_bytes = 2200;
		GSTextureCache::SetHashCacheLimits(limits);
		g_texture_cache->IncAge();
		EXPECT_EQ(Bytes(), 2200u);
		EXPECT_EQ(Entries(), 12u);

		// A limit nothing can meet stops at the floor, and what is left is the youngest.
		limits.max_bytes = 1;
		GSTextureCache::SetHashCacheLimits(limits);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 4u);
		EXPECT_EQ(Bytes(), 4u * 200u);
	}

	TEST_F(GSHashCacheLimits, EntryLimitHoldsWhateverTheFloorAndTheMemoryLimitSay)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 800;
		limits.max_entries = 50;
		limits.max_bytes = 1ull << 30;
		GSTextureCache::SetHashCacheLimits(limits);

		// The floor only keeps entries from the memory limit; it does not lift the entry limit.
		Inject(80, 10);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 50u);
	}

	TEST_F(GSHashCacheLimits, TheLimitsOfTheOldCacheAreStillAvailable)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 800;
		limits.max_entries = 800;
		limits.max_bytes = 0;
		GSTextureCache::SetHashCacheLimits(limits);

		Inject(1000, 4);
		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 800u);
	}

	TEST_F(GSHashCacheLimits, NoLimitsKeepEntriesUntilTheyAgeOut)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 800;
		limits.max_entries = 0;
		limits.max_bytes = 0;
		GSTextureCache::SetHashCacheLimits(limits);

		Inject(5000, 4);
		for (int frame = 0; frame < 30; frame++)
			g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 5000u);

		g_texture_cache->IncAge();
		EXPECT_EQ(Entries(), 0u);
		EXPECT_EQ(Bytes(), 0u);
	}

	TEST_F(GSHashCacheLimits, StatsRecordThePeakBeforeTheEntriesAreDropped)
	{
		BringUp();
		GSTextureCache::HashCacheLimits limits;
		limits.floor_entries = 2;
		limits.max_entries = 5;
		limits.max_bytes = 0;
		GSTextureCache::SetHashCacheLimits(limits);

		const GSTextureCache::HashCacheStats before = GSTextureCache::GetHashCacheStats();
		Inject(9, 10);
		g_texture_cache->IncAge();
		const GSTextureCache::HashCacheStats after = GSTextureCache::GetHashCacheStats();

		EXPECT_EQ(Entries(), 5u);
		EXPECT_GE(after.peak_entries, 9u);
		EXPECT_GE(after.peak_bytes, 9u * 40u);
		EXPECT_EQ(after.frames, before.frames + 1);
	}
} // namespace
