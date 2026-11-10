// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The lean draw path's target group.
//
// The lookup of a draw's target walks the target list and almost always finds the target at the front with
// nothing to do. The lean path answers that case without walking. These tests pin the conditions under which
// the lookup's answer is the front target with nothing else changed.

#include "gs_hw_draw_harness.h"
#include "GS/Renderers/HW/GSTextureCache.h"

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
		void TearDown() override
		{
			GSTextureCache::SetLeanMode(GSTextureCache::LeanMode::On);
			Fixture::TearDown();
		}

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
	GSTextureCache::SetLeanMode(GSTextureCache::LeanMode::Off);
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
	ASSERT_EQ(Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, false, whole), target);
	EXPECT_TRUE(target->m_dirty.empty());
	EXPECT_EQ(target->m_texture->GetState(), GSTexture::State::Invalidated);

	// A format with another depth: the walk converts the target and takes the new format.
	target->m_texture->SetState(GSTexture::State::Dirty);
	ASSERT_EQ(Lookup(Layout(kBlock, 10, PSMCT16), 1.0f, true, GSVector4i(16, 16, 128, 128)), target);
	EXPECT_EQ(target->m_TEX0.PSM, static_cast<u32>(PSMCT16));
}

// The checker itself: over shapes that take the front answer and shapes that do not, the walk agrees.
TEST_F(GSLeanTarget, TheWalkAgreesWithTheFrontAnswer)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	auto* target = MakeTarget();
	ASSERT_NE(target, nullptr);
	GSTextureCache::SetLeanMode(GSTextureCache::LeanMode::Check);

	const GSVector4i part(16, 16, 128, 128);
	for (int i = 0; i < 4; i++)
	{
		EXPECT_EQ(Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, (i & 1) != 0, part, (i & 2) != 0), target);
		EXPECT_EQ(Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, true, GSVector4i(0, 0, 640, 448)), target);
	}

	const GSTextureCache::LeanReport report = GSTextureCache::GetLeanReport();
	EXPECT_EQ(report.target_calls, 8u);
	EXPECT_EQ(report.target_hits, 8u);
	EXPECT_EQ(report.mismatches, 0u);
}

// With the lean path off and on the same lookups give the same target in the same state.
TEST_F(GSLeanTarget, OffAndOnLeaveTheSameTarget)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	const GSVector4i part(16, 16, 128, 128);

	bool flags[2][3];
	for (int mode = 0; mode < 2; mode++)
	{
		GSTextureCache::SetLeanMode(mode ? GSTextureCache::LeanMode::On : GSTextureCache::LeanMode::Off);
		g_texture_cache->RemoveAll(true, true, true);
		auto* target = MakeTarget();
		ASSERT_NE(target, nullptr);
		target->m_used = false;
		target->m_32_bits_fmt = false;
		target->readbacks_since_draw = 2;
		ASSERT_EQ(Lookup(Layout(kBlock, 10, PSMCT32), 1.0f, true, part), target);
		flags[mode][0] = target->m_used;
		flags[mode][1] = target->m_32_bits_fmt;
		flags[mode][2] = target->readbacks_since_draw == 0;
	}
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(flags[0][i], flags[1][i]) << i;
}

// Whole draws through the renderer with the checker on.
TEST_F(GSLeanTarget, DrawsUnderTheCheckerAgree)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	GSTextureCache::SetLeanMode(GSTextureCache::LeanMode::Check);

	for (int i = 0; i < 6; i++)
		SpriteDraw((i & 1) != 0, 8 * i, 8 * i, 64 + 8 * i, 64 + 8 * i);

	const GSTextureCache::LeanReport report = GSTextureCache::GetLeanReport();
	EXPECT_GT(report.target_calls, 0u);
	EXPECT_GT(report.target_hits, 0u);
	EXPECT_EQ(report.mismatches, 0u);
}
