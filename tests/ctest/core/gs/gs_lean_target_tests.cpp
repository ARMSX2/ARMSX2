// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The lean draw path's target group.
//
// Two walks of the texture cache run for every draw and almost always do nothing: the lookup of the draw's
// target, which finds it at the front of its list, and the invalidation of sources under what the draw
// wrote, which finds none. The lean path answers both without walking. These tests pin the two claims that
// rests on: the conditions under which the lookup's answer is the front target with nothing else changed,
// and that the pages the invalidation visits are inside the page run it tests.

#include "gs_hw_draw_harness.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <cstring>
#include <set>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFBP = 0x40; // FRAME.FBP: block 0x800
	constexpr u32 kBlock = kFBP << 5;

	GIFRegTEX0 Layout(u32 bp, u32 bw, u32 psm)
	{
		GIFRegTEX0 layout = {};
		layout.TBP0 = bp;
		layout.TBW = bw;
		layout.PSM = psm;
		return layout;
	}

	class GSLeanTarget : public Fixture
	{
	protected:
		/// A 640x448 colour target at kBlock with everything valid and nothing dirty.
		GSTextureCache::Target* MakeTarget(u32 bp = kBlock)
		{
			auto* target = g_texture_cache->CreateTarget(Layout(bp, 10, PSMCT32), GSVector2i(640, 448), GSVector2i(640, 448),
				1.0f, GSTextureCache::RenderTarget, true, 0, false, false, false);
			if (!target)
				return nullptr;
			target->m_valid = GSVector4i(0, 0, 640, 448);
			target->m_drawn_since_read = target->m_valid;
			target->SetEndBlock(GSLocalMemory::GetEndBlockAddress(bp, 10, PSMCT32, target->m_valid));
			target->m_dirty.clear();
			target->m_valid_rgb = true;
			target->m_valid_alpha_low = true;
			target->m_valid_alpha_high = true;
			return target;
		}

		GSTextureCache::Target* Lookup(const GIFRegTEX0& layout, float scale, bool preserve, const GSVector4i& draw, bool used = true)
		{
			return g_texture_cache->LookupDrawTarget(layout, GSVector2i(640, 448), scale, GSTextureCache::RenderTarget, used, 0,
				false, preserve, preserve, draw);
		}

		/// The walk alone, as the lookup ran before the lean path.
		GSTextureCache::Target* Walk(const GIFRegTEX0& layout, float scale, bool preserve, const GSVector4i& draw, bool used = true)
		{
			return g_texture_cache->LookupDrawTargetWalk(layout, GSVector2i(640, 448), scale, GSTextureCache::RenderTarget, used, 0,
				false, preserve, preserve, draw, false, false, false, nullptr, nullptr, -1);
		}

		/// The lookup, then the walk after it with the same arguments: where the lookup took the front answer, the walk
		/// must return the same target and change nothing (not the target, not its texture's state, not the order or
		/// the membership of either list). True if the front answer was taken.
		bool LookupThenWalkAgrees(const GIFRegTEX0& layout, float scale, bool preserve, const GSVector4i& draw, bool used)
		{
			GSTextureCache::Target* const front =
				g_texture_cache->FrontDrawTarget(layout, scale, GSTextureCache::RenderTarget, preserve, draw);
			GSTextureCache::Target* const looked = Lookup(layout, scale, preserve, draw, used);
			if (!front)
				return false;
			EXPECT_EQ(looked, front);

			std::vector<GSTextureCache::Target*> before;
			for (int i = 0; i < 2; i++)
				for (GSTextureCache::Target* t : g_texture_cache->GetDst(i))
					before.push_back(t);
			alignas(GSTextureCache::Target) u8 bytes[sizeof(GSTextureCache::Target)];
			std::memcpy(bytes, static_cast<const void*>(front), sizeof(bytes));
			const GSTexture::State state = front->m_texture->GetState();

			EXPECT_EQ(Walk(layout, scale, preserve, draw, used), front);

			std::vector<GSTextureCache::Target*> after;
			for (int i = 0; i < 2; i++)
				for (GSTextureCache::Target* t : g_texture_cache->GetDst(i))
					after.push_back(t);
			EXPECT_EQ(before, after);
			EXPECT_EQ(std::memcmp(bytes, static_cast<const void*>(front), sizeof(bytes)), 0);
			EXPECT_EQ(front->m_texture->GetState(), state);
			return true;
		}

		void SpriteDraw(bool textured, int x0, int y0, int x1, int y1)
		{
			Packet p;
			Environment(p, kFBP, PSMCT32);
			if (textured)
				Texture(p, 0x3000, PSMCT32);
			p.Vertex(x0 * 16, y0 * 16, 1, 0, 0);
			p.Vertex(x1 * 16, y1 * 16, 1, 64 * 16, 64 * 16);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = textured ? 1 : 0;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}
	};
} // namespace

// The plain case: same address, width, format and scale, part of the target drawn over.
TEST_F(GSLeanTarget, TheFrontTargetAnswersTheLookup)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);
	target->m_used = false;
	target->readbacks_since_draw = 3;

	const GSVector4i draw(16, 16, 128, 128);
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, GSTextureCache::RenderTarget, false, draw), target);

	EXPECT_EQ(Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, false, draw), target);
	EXPECT_TRUE(target->m_used);
	EXPECT_EQ(target->readbacks_since_draw, 0);
	EXPECT_TRUE(target->m_32_bits_fmt);
}

// Each condition, one at a time: where the walk can do more than return the target, the front answer is refused.
TEST_F(GSLeanTarget, TheFrontAnswerIsRefusedWhereTheWalkCanDoMore)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);
	const GSVector4i part(16, 16, 128, 128);
	const GSVector4i whole(0, 0, 640, 448);
	const int rt = GSTextureCache::RenderTarget;

	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock + 0x20, 10, PSMCT32), 1.0f, rt, true, part), nullptr) << "another address";
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 8, PSMCT32), 1.0f, rt, true, part), nullptr) << "another width";
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT24), 1.0f, rt, true, part), nullptr) << "another format";
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 2.0f, rt, true, part), nullptr) << "another scale";
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, GSTextureCache::DepthStencil, true, part), nullptr)
		<< "the other list";
	// Covering the whole valid area with nothing preserved drops dirt and invalidates the texture.
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, rt, false, whole), nullptr);
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, rt, true, whole), target);

	target->m_valid_rgb = false;
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, rt, true, part), nullptr) << "RGB to repopulate";
	target->m_valid_rgb = true;

	// A second target in front of it: the walk would see that one first.
	ASSERT_NE(MakeTarget(kBlock + 0x1000), nullptr);
	EXPECT_EQ(g_texture_cache->FrontDrawTarget(Layout(kBlock, 10, PSMCT32), 1.0f, rt, true, part), nullptr) << "not at the front";
}

// Why two of the refusals are there: the walk does do more in those cases.
TEST_F(GSLeanTarget, TheWalkDoesMoreWhereTheFrontAnswerIsRefused)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);
	const GSVector4i whole(0, 0, 640, 448);

	// Covering the whole valid area with nothing preserved: the dirt goes and the texture is invalidated.
	g_gs_device->ClearRenderTarget(target->m_texture, 0x11223344);
	target->m_texture->SetState(GSTexture::State::Dirty);
	RGBAMask rgba;
	rgba._u32 = 0xf;
	g_texture_cache->MarkTargetDirty(target, GSVector4i(0, 0, 64, 32), PSMCT32, 10, rgba, false);
	ASSERT_FALSE(target->m_dirty.empty());
	ASSERT_EQ(Walk(Layout(kBlock, 10, PSMCT32), 1.0f, false, whole), target);
	EXPECT_TRUE(target->m_dirty.empty());
	EXPECT_EQ(target->m_texture->GetState(), GSTexture::State::Invalidated);

	// A format with another depth: the walk converts the target and takes the new format.
	target->m_texture->SetState(GSTexture::State::Dirty);
	ASSERT_EQ(Walk(Layout(kBlock, 10, PSMCT16), 1.0f, true, GSVector4i(16, 16, 128, 128)), target);
	EXPECT_EQ(target->m_TEX0.PSM, static_cast<u32>(PSMCT16));
}

// Over shapes that take the front answer, the walk run after the lookup agrees with it.
TEST_F(GSLeanTarget, TheWalkAgreesWithTheFrontAnswer)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);

	const GSVector4i part(16, 16, 128, 128);
	u32 taken = 0;
	for (int i = 0; i < 4; i++)
	{
		taken += LookupThenWalkAgrees(Layout(kBlock, 10, PSMCT32), 1.0f, (i & 1) != 0, part, (i & 2) != 0);
		taken += LookupThenWalkAgrees(Layout(kBlock, 10, PSMCT32), 1.0f, true, GSVector4i(0, 0, 640, 448), true);
	}
	EXPECT_EQ(taken, 8u);
}

// The walk alone and the lookup give the same target in the same state.
TEST_F(GSLeanTarget, OffAndOnLeaveTheSameTarget)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	const GSVector4i part(16, 16, 128, 128);

	bool flags[2][3];
	for (int mode = 0; mode < 2; mode++)
	{
		g_texture_cache->RemoveAll(true, true, true);
		auto* target = MakeTarget();
		ASSERT_NE(target, nullptr);
		target->m_used = false;
		target->m_32_bits_fmt = false;
		target->readbacks_since_draw = 2;
		ASSERT_EQ(mode ? Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, true, part) : Walk(Layout(kBlock, 10, PSMCT32), 1.0f, true, part), target);
		flags[mode][0] = target->m_used;
		flags[mode][1] = target->m_32_bits_fmt;
		flags[mode][2] = target->readbacks_since_draw == 0;
	}
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(flags[0][i], flags[1][i]) << i;
}

// The page run tested instead of the walk holds every page the walk visits.
TEST(GSLeanTargetPages, TheSpanHoldsEveryPageTheLoopVisits)
{
	GSLocalMemory mem;
	const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ16, PSMZ16S, PSMT8, PSMT4};
	const GSVector4i rects[] = {
		GSVector4i(0, 0, 640, 448), GSVector4i(0, 0, 1, 1), GSVector4i(63, 31, 65, 33), GSVector4i(100, 200, 300, 208),
		GSVector4i(0, 0, 64, 32), GSVector4i(511, 0, 513, 448), GSVector4i(17, 3, 600, 400), GSVector4i(0, 0, 2048, 2048),
		GSVector4i(320, 224, 320, 224)};
	u32 spans = 0;
	for (const u32 psm : psms)
	{
		for (const u32 bp : {0u, 0x20u, 0x800u, 0x1234u, 0x3fe0u, 0x3fffu})
		{
			for (const u32 bw : {1u, 8u, 10u, 16u, 32u})
			{
				const GSOffset off = mem.GetOffset(bp, bw, psm);
				for (const GSVector4i& r : rects)
				{
					const GSOffset::PageLooper looper = off.pageLooperForRect(r);
					std::set<u32> visited;
					looper.loopPages([&visited](u32 page) { visited.insert(page); });
					u32 first = 0, count = 0;
					if (!looper.pageSpan(first, count))
						continue; // no claim made: the walk runs
					spans++;
					for (const u32 page : visited)
					{
						// The page is `pos % GS_MAX_PAGES` for some pos in [first, first + count).
						const u32 delta = (page + GS_MAX_PAGES - (first % GS_MAX_PAGES)) % GS_MAX_PAGES;
						EXPECT_TRUE(count >= GS_MAX_PAGES || delta < count)
							<< "psm " << psm << " bp " << bp << " bw " << bw << " rect " << r.x << "," << r.y << "," << r.z << "," << r.w
							<< " page " << page << " span " << first << "+" << count;
					}
				}
			}
		}
	}
	EXPECT_GT(spans, 1000u);
}

TEST(GSLeanTargetPages, AnyPageUsedReadsTheRunItIsGiven)
{
	auto map = std::make_unique<GSTextureCache::SourceMap>();
	EXPECT_FALSE(map->AnyPageUsed(0, GS_MAX_PAGES));
	EXPECT_FALSE(map->AnyPageUsed(0, 4 * GS_MAX_PAGES));

	const auto set = [&map](u32 page) { map->m_page_used[page >> 6] |= u64(1) << (page & 63); };
	set(70);
	EXPECT_TRUE(map->AnyPageUsed(70, 1));
	EXPECT_TRUE(map->AnyPageUsed(0, 71));
	EXPECT_FALSE(map->AnyPageUsed(0, 70));
	EXPECT_FALSE(map->AnyPageUsed(71, 441)); // 71..511
	EXPECT_TRUE(map->AnyPageUsed(71, 512));
	EXPECT_TRUE(map->AnyPageUsed(500, 100)); // wraps to 0..87
	EXPECT_FALSE(map->AnyPageUsed(500, 82)); // wraps to 0..69
	EXPECT_TRUE(map->AnyPageUsed(70 + 3 * GS_MAX_PAGES, 1)); // a position past the end of memory
	EXPECT_FALSE(map->AnyPageUsed(70, 0));

	set(511);
	set(0);
	EXPECT_TRUE(map->AnyPageUsed(511, 1));
	EXPECT_TRUE(map->AnyPageUsed(510, 2));
	EXPECT_FALSE(map->AnyPageUsed(1, 69));
	EXPECT_TRUE(map->AnyPageUsed(64, 64));
	EXPECT_FALSE(map->AnyPageUsed(128, 383));
}

// The end block a target keeps for its valid area is GetEndBlockAddress of what the target is now, whatever
// changed since it was last asked: the valid area, the base block, the width or the format.
TEST_F(GSLeanTarget, TheKeptEndBlockFollowsTheLayoutAndTheValidArea)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);

	const auto fresh = [target]() {
		return GSLocalMemory::GetEndBlockAddress(target->m_TEX0.TBP0, target->m_TEX0.TBW, target->m_TEX0.PSM, target->m_valid);
	};
	EXPECT_EQ(target->EndBlockOfValid(), fresh());
	EXPECT_EQ(target->EndBlockOfValid(), fresh());

	target->m_valid = GSVector4i(0, 0, 320, 224);
	EXPECT_EQ(target->EndBlockOfValid(), fresh());
	target->m_valid = GSVector4i(0, 0, 640, 448);
	target->m_TEX0.TBW = 8;
	EXPECT_EQ(target->EndBlockOfValid(), fresh());
	target->m_TEX0.PSM = PSMCT16;
	EXPECT_EQ(target->EndBlockOfValid(), fresh());
	target->SetTBP0(kBlock + 0x40);
	EXPECT_EQ(target->EndBlockOfValid(), fresh());

	// Through the two callers.
	target->UpdateValidity(GSVector4i(0, 0, 640, 480), true);
	EXPECT_EQ(target->EndBlock(), fresh());
	target->ResizeValidity(GSVector4i(0, 0, 512, 256));
	EXPECT_EQ(target->EndBlock(), fresh());
}
