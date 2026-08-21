// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The source cache's PIN DISCIPLINE (GSTileTextureSource::SetFrame). The Tile renderer looks a
// texture up and draws with it immediately; the TileGpu renderer looks it up while accumulating and
// issues nothing until the plan executes, so a texture a recorded draw names has to still hold the
// right bytes at the END of the frame. Three things would break that and all three are silent —
// a rebuild landing in the same entry and the same texture, an LRU eviction taking the slot, and a
// Recycle handing the texture to the next allocation — and the failure looks like a shading bug
// rather than a lifetime bug, corpus-wide, with nothing pointing at the cache.
//
// So the discipline is pinned here rather than left to the corpus, where its whole job is to make
// something NOT happen. The corner that matters most is the one a live frame reaches by accident:
// a window invalidated and looked up again inside a single frame. The pinned entry must refuse the
// rebuild — the caller falls back to its other road and the rebuild happens next frame naturally —
// and it must refuse without disturbing the texture the earlier draw is holding.
//
// The cache allocates and recycles through g_gs_device, so the fixture stands a device double up
// (GSTileStubDevice.h): GSDevice's pool, recycle and fetch logic is real (it is not virtual), and
// only surface creation is stubbed. That means the test exercises the same Recycle-to-pool path the
// renderer does, which is the point — "a recycled texture comes back out of the pool" is half of
// what the pin prevents.

#include "GSTileStubDevice.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
	using GSTileTest::StubDevice;

	// Records which textures it was asked to fill, so a test can say "this entry was rebuilt" or
	// "this entry was not" without inspecting the cache.
	class RecordingBuilder final : public GSTileSourceBuilder
	{
	public:
		std::vector<GSTexture*> built;
		bool refuse = false;

		bool BuildTileSource(GSTexture* tex, const GIFRegTEX0&, const GIFRegTEXA&) override
		{
			if (refuse)
				return false;
			built.push_back(tex);
			return true;
		}
	};

	GIFRegTEX0 MakeTex0(u32 tbp0, u32 tbw, u32 tw, u32 th)
	{
		GIFRegTEX0 t = {};
		t.TBP0 = tbp0;
		t.TBW = tbw;
		t.PSM = PSMCT32;
		t.TW = tw;
		t.TH = th;
		return t;
	}

	// The window's pages, as the renderer's model would name them. The exact set does not matter to
	// the discipline — only that a write to it moves the gen stamp.
	GSPageBitmap PagesOf(u32 first, u32 count)
	{
		GSPageBitmap b;
		for (u32 i = 0; i < count; i++)
			b.set(first + i);
		return b;
	}

	class TileSourcePinTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g_gs_device = std::make_unique<StubDevice>();
			m_source = std::make_unique<GSTileTextureSource>();
		}

		void TearDown() override
		{
			// The cache's textures go back to the device pool before the device dies -- the same
			// teardown order the renderer's Destroy() has, and GSDevice's destructor asserts it.
			m_source.reset();
			g_gs_device.reset();
		}

		std::unique_ptr<GSTileTextureSource> m_source;
		GSVramModel m_model;
		RecordingBuilder m_builder;

		static constexpr GSTileTextureSource::ContentToken kUnknown = {};
		static GSTileTextureSource::ContentToken Pages(u64 v)
		{
			return {GSTileTextureSource::ContentToken::Source::Pages, v};
		}
	};
} // namespace

// The corner a live frame reaches by accident: a window looked up, invalidated, and looked up again
// inside ONE frame. The second lookup must refuse rather than rebuild in place, because the draw the
// first lookup was for has been recorded and not yet issued — it would sample bytes it never asked
// for. The refusal has to leave the first texture exactly as it was, and the rebuild has to happen
// on the next frame without any further prompting.
TEST_F(TileSourcePinTest, RebuiltInFrameRefusesAndHappensNextFrame)
{
	const GIFRegTEX0 tex0 = MakeTex0(0, 4, 8, 8);
	const GIFRegTEXA texa = {};
	const GSPageBitmap pages = PagesOf(0, 4);

	m_source->SetFrame(1);

	GSTileTextureSource::BuiltOutcome outcome{};
	GSTexture* first = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(0x1111), nullptr, &outcome);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
	ASSERT_EQ(m_builder.built.size(), 1u);

	// The window's bytes move under the recorded draw.
	m_model.OnCpuWrite(pages, kGSTilePlanesColor);

	GSTexture* second = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(0x2222), nullptr, &outcome);
	EXPECT_EQ(second, nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::RefusedPinned);
	EXPECT_EQ(m_source->PinRefusals(), 1u);
	EXPECT_EQ(m_builder.built.size(), 1u) << "the pinned entry was rebuilt in place";

	// Next frame: the plan that named it has run, so the same lookup builds without being asked
	// twice — and into the SAME entry, which is what makes this a rebuild and not a second entry.
	m_source->SetFrame(2);
	GSTexture* third = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(0x2222), nullptr, &outcome);
	ASSERT_NE(third, nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
	ASSERT_EQ(m_builder.built.size(), 2u);
	EXPECT_EQ(m_builder.built[1], first) << "the rebuild should reuse the entry's own texture";
	EXPECT_EQ(m_source->PinRefusals(), 1u);
}

// A moved gen stamp is not by itself a rebuild: if the caller can prove the bytes did not change,
// the entry is served as it stands and nothing is refused, pinned or not. Without this the pin
// would refuse the whole corpus, because on most dumps the stamp moves every frame.
TEST_F(TileSourcePinTest, SameContentTokenServesAPinnedEntry)
{
	const GIFRegTEX0 tex0 = MakeTex0(0, 4, 8, 8);
	const GIFRegTEXA texa = {};
	const GSPageBitmap pages = PagesOf(0, 4);

	m_source->SetFrame(7);
	GSTileTextureSource::BuiltOutcome outcome{};
	GSTexture* first = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(0xABCD), nullptr, &outcome);
	ASSERT_NE(first, nullptr);
	ASSERT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);

	m_model.OnCpuWrite(pages, kGSTilePlanesColor);
	GSTexture* again = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(0xABCD), nullptr, &outcome);
	EXPECT_EQ(again, first);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Rescued);
	EXPECT_EQ(m_source->PinRefusals(), 0u);
	EXPECT_EQ(m_builder.built.size(), 1u);

	// An unknown token is never equal to anything, including another unknown one, so a window whose
	// truth the caller cannot fingerprint falls through to the refusal rather than to a stale serve.
	m_model.OnCpuWrite(pages, kGSTilePlanesColor);
	GSTexture* opaque = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, kUnknown, nullptr, &outcome);
	EXPECT_EQ(opaque, nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::RefusedPinned);
}

// A page fingerprint and a target version are different numbering spaces, and the token carries
// which one it is so they can never be read as each other. That is what lets a future donor build
// key on (owner target, version) while page-fingerprinted windows keep their own identity.
TEST_F(TileSourcePinTest, ContentTokenSourcesDoNotCollide)
{
	const GIFRegTEX0 tex0 = MakeTex0(0, 4, 8, 8);
	const GIFRegTEXA texa = {};
	const GSPageBitmap pages = PagesOf(0, 4);

	m_source->SetFrame(3);
	GSTileTextureSource::BuiltOutcome outcome{};
	ASSERT_NE(m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(99), nullptr, &outcome), nullptr);

	// Same value, different provenance: not the same content.
	m_source->SetFrame(4);
	m_model.OnCpuWrite(pages, kGSTilePlanesColor);
	const GSTileTextureSource::ContentToken target_token{GSTileTextureSource::ContentToken::Source::Target, 99};
	ASSERT_NE(m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, target_token, nullptr, &outcome), nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
	EXPECT_EQ(m_builder.built.size(), 2u);

	// ...and the target-sourced token rescues its own kind.
	m_source->SetFrame(5);
	m_model.OnCpuWrite(pages, kGSTilePlanesColor);
	ASSERT_NE(m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, target_token, nullptr, &outcome), nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Rescued);
	EXPECT_EQ(m_builder.built.size(), 2u);
}

// The discipline is opt-in and its default is off, because the Tile renderer draws immediately and
// must not pay for any of this. With no frame set, an entry is never stamped and a rebuild in place
// is exactly what happens.
TEST_F(TileSourcePinTest, DisciplineOffRebuildsInPlace)
{
	const GIFRegTEX0 tex0 = MakeTex0(0, 4, 8, 8);
	const GIFRegTEXA texa = {};
	const GSPageBitmap pages = PagesOf(0, 4);

	GSTileTextureSource::BuiltOutcome outcome{};
	GSTexture* first = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(1), nullptr, &outcome);
	ASSERT_NE(first, nullptr);

	m_model.OnCpuWrite(pages, kGSTilePlanesColor);
	GSTexture* second = m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(2), nullptr, &outcome);
	EXPECT_EQ(second, first);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
	EXPECT_EQ(m_source->PinRefusals(), 0u);
	EXPECT_EQ(m_builder.built.size(), 2u);
}

// Capacity is a hard bound with a counted refusal, not a hope. A frame whose distinct windows fill
// the cache cannot evict its way out, because everything in it is named by a draw that has not been
// issued — so the overflow refuses and the caller keeps its other road.
TEST_F(TileSourcePinTest, EveryEntryPinnedRefusesRatherThanEvicting)
{
	const GIFRegTEXA texa = {};
	m_source->SetFrame(11);
	GSTileTextureSource::BuiltOutcome outcome{};

	// 512 entries, one distinct window each, all in one frame.
	for (u32 i = 0; i < 512; i++)
	{
		// TBP0 is 14 bits and the key packs it as such, so the bases have to stay inside that or
		// two "distinct" windows alias onto one entry.
		const GIFRegTEX0 t = MakeTex0(i * 8, 1, 3, 3);
		ASSERT_NE(m_source->LookupBuilt(m_model, t, texa, PagesOf(i % 512, 1), m_builder, Pages(i + 1), nullptr,
					  &outcome),
			nullptr)
			<< "window " << i;
		ASSERT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built) << "window " << i;
	}
	EXPECT_EQ(m_source->PinnedEntries(), 512u);

	const GIFRegTEX0 overflow = MakeTex0(8192, 1, 3, 3);
	EXPECT_EQ(m_source->LookupBuilt(m_model, overflow, texa, PagesOf(7, 1), m_builder, Pages(9999), nullptr, &outcome),
		nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::RefusedCapacity);
	EXPECT_EQ(m_source->CapacityRefusals(), 1u);
	EXPECT_EQ(m_builder.built.size(), 512u);

	// Next frame nothing is pinned, so the same window takes a slot by eviction as it always would.
	m_source->SetFrame(12);
	EXPECT_NE(m_source->LookupBuilt(m_model, overflow, texa, PagesOf(7, 1), m_builder, Pages(9999), nullptr, &outcome),
		nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
	EXPECT_EQ(m_source->CapacityRefusals(), 1u);
}

// A builder that refuses drops the entry rather than leaving a live one with an unfilled texture —
// the caller gets null and its other road, and the next frame builds from scratch.
TEST_F(TileSourcePinTest, RefusedBuildLeavesNoEntry)
{
	const GIFRegTEX0 tex0 = MakeTex0(0, 4, 8, 8);
	const GIFRegTEXA texa = {};
	const GSPageBitmap pages = PagesOf(0, 4);

	m_source->SetFrame(1);
	m_builder.refuse = true;
	GSTileTextureSource::BuiltOutcome outcome{};
	EXPECT_EQ(m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(5), nullptr, &outcome), nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Failed);
	EXPECT_EQ(m_source->PinnedEntries(), 0u);

	m_builder.refuse = false;
	EXPECT_NE(m_source->LookupBuilt(m_model, tex0, texa, pages, m_builder, Pages(5), nullptr, &outcome), nullptr);
	EXPECT_EQ(outcome, GSTileTextureSource::BuiltOutcome::Built);
}
