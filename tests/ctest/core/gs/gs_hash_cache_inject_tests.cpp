// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that a replacement texture which finishes loading after its hash-cache placeholder was
// evicted still ages out of the hash cache.
//
// The pack loader works asynchronously. When a load finishes, GSTextureCache::InjectHashCacheTexture
// swaps the texture into the placeholder entry. If the placeholder has already aged out, it adds a
// new entry instead. No Source holds that entry, so it has to start unreferenced, or AgeHashCache
// skips it forever and its memory stays booked until the whole cache is flushed.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/HW/GSTextureCache.h"

using namespace GSHWDrawHarness;

namespace
{
	// AgeHashCache drops an unreferenced entry once its age passes 30 frames.
	constexpr int kHashCacheMaxAge = 30;

	class GSHashCacheInject : public Fixture
	{
	};

	TEST_F(GSHashCacheInject, EntryInjectedAfterEvictionAgesOut)
	{
		BringUp();

		GSTexture* const tex = g_gs_device->CreateTexture(64, 64, 1, GSTexture::Format::Color);
		ASSERT_NE(tex, nullptr);
		const u64 tex_bytes = tex->GetMemUsage();
		ASSERT_GT(tex_bytes, 0u);

		// No placeholder exists for this key, as if it had been evicted while the file was loading.
		ASSERT_EQ(g_texture_cache->GetHashCacheReplacementMemoryUsage(), 0u);
		g_texture_cache->InjectHashCacheTexture(GSTextureCache::HashCacheKey(), tex, {0, 255});
		EXPECT_EQ(g_texture_cache->GetHashCacheReplacementMemoryUsage(), tex_bytes);

		// It gets the same grace as any other unreferenced entry, so a draw can still find it...
		for (int frame = 0; frame < kHashCacheMaxAge; frame++)
			g_texture_cache->IncAge();
		EXPECT_EQ(g_texture_cache->GetHashCacheReplacementMemoryUsage(), tex_bytes);

		// ...and if none does, it goes.
		g_texture_cache->IncAge();
		EXPECT_EQ(g_texture_cache->GetHashCacheReplacementMemoryUsage(), 0u);
	}
} // namespace
