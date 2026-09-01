// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the TileGpu CPU palette road stops re-doing.
//
// A paletted draw the CLUT gather cannot serve expands its CLUT, copies the expansion into the
// plan's palette stream, and hashes it for a content id. Then the source road hashed the SAME
// words a second time -- reading them back out of the stream the first hash had just written -- to
// get a number the first hash had already computed and thrown away. On the R&C UYA effects scene
// that second hash ran 1,535 times a frame over 380,480 words.
//
// This file pins the lookup that takes the id instead of deriving it: same texture, same id, same
// cache state, and the read-generation memo still in front of it.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"

#include "GSTileStubDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>

namespace
{
	class PaletteCacheIdTest : public ::testing::Test
	{
	protected:
		void SetUp() override { g_gs_device = std::make_unique<GSTileTest::StubDevice>(); }
		void TearDown() override
		{
			m_cache.Clear();
			g_gs_device.reset();
		}

		static std::array<u32, 256> Palette(u32 salt)
		{
			std::array<u32, 256> w{};
			for (u32 e = 0; e < 256; e++)
				w[e] = 0xFF000000u | (e * 2654435761u) ^ (salt << 8);
			return w;
		}

		GSTilePaletteCache m_cache;
	};
} // namespace

// The overload's whole contract: same texture, same id, same cache state. The read generations are
// stepped so neither call can be answered by the last-answer memo -- the point is the CONTENT
// path, and a memo hit would prove nothing about it.
TEST_F(PaletteCacheIdTest, ThePrehashedLookupIsTheHashingLookup)
{
	const auto words = Palette(1);
	const u64 id = GSTilePaletteCache::ContentId(words.data(), 256);

	u64 hashed_id = 0;
	GSTexture* const built = m_cache.Lookup(words.data(), 256, 1, &hashed_id);
	ASSERT_NE(built, nullptr);
	EXPECT_EQ(hashed_id, id);
	EXPECT_EQ(m_cache.Builds(), 1u);

	u64 carried_id = 0;
	GSTexture* const same = m_cache.Lookup(words.data(), 256, 2, id, &carried_id);
	EXPECT_EQ(same, built) << "the carried id must find the entry the hash built";
	EXPECT_EQ(carried_id, id);
	EXPECT_EQ(m_cache.Builds(), 1u) << "no rebuild";
	EXPECT_EQ(m_cache.Hits(), 1u);

	// ...and the hashing form still finds what the carried one built, which is what keeps the two
	// callers of this cache sharing entries rather than each holding their own.
	const auto other = Palette(2);
	const u64 other_id = GSTilePaletteCache::ContentId(other.data(), 256);
	u64 out = 0;
	GSTexture* const carried_build = m_cache.Lookup(other.data(), 256, 3, other_id, &out);
	ASSERT_NE(carried_build, nullptr);
	EXPECT_EQ(m_cache.Builds(), 2u);
	u64 out2 = 0;
	EXPECT_EQ(m_cache.Lookup(other.data(), 256, 4, &out2), carried_build);
	EXPECT_EQ(out2, other_id);
	EXPECT_EQ(m_cache.Builds(), 2u);
}

// The memo in front of both forms is shared, and it must stay in front of the carried-id form too:
// it is where last_use and pinned_frame are refreshed, so skipping it would move eviction order
// even though it hands back the same texture.
TEST_F(PaletteCacheIdTest, TheReadGenerationMemoStillRunsForTheCarriedIdForm)
{
	const auto words = Palette(3);
	const u64 id = GSTilePaletteCache::ContentId(words.data(), 256);

	u64 out = 0;
	GSTexture* const built = m_cache.Lookup(words.data(), 256, 9, id, &out);
	ASSERT_NE(built, nullptr);
	const u64 hits_before = m_cache.Hits();

	// Same read generation: the memo answers, and the words are not even looked at.
	std::array<u32, 256> lies{};
	lies.fill(0xDEADBEEFu);
	u64 memo_id = 0;
	EXPECT_EQ(m_cache.Lookup(lies.data(), 256, 9, 0ull, &memo_id), built);
	EXPECT_EQ(memo_id, id);
	EXPECT_EQ(m_cache.Hits(), hits_before + 1);
	EXPECT_EQ(m_cache.Builds(), 1u);
}
