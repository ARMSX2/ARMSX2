// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The state table through the renderer on the None backend: a draw that takes a state entry hands the device the
// configuration the derivation would have handed it. A sequence of draws that repeats register states with varied
// facts is run three times on fresh renderers: with every draw derived, with the table on, and in check mode, where
// every hit is derived as well and compared byte for byte. The constant buffers are compared by check mode only: an
// entry holds zero in the words its derivation did not write, where the derived path keeps an earlier draw's value.

#include "gs_hw_draw_harness.h"
#include "GS/Renderers/HW/GSDrawRow.h"

#include <cstring>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFBP = 0x40;
	constexpr u32 kTBP = 0x2000;

	/// What the device was handed for one draw.
	struct Snap
	{
		u32 draws;
		GSHWDrawConfig::PSSelector ps;
		GSHWDrawConfig::PSConstantBuffer cb_ps;
		GSHWDrawConfig::VSConstantBuffer cb_vs;
		GSHWDrawConfig::BlendState blend;
		GSHWDrawConfig::ColorMaskSelector colormask;
		GSHWDrawConfig::DepthStencilSelector depth;
		GSHWDrawConfig::AlphaTestMode alpha_test;
		GSHWDrawConfig::DestinationAlphaMode destination_alpha;
		bool one_barrier, full_barrier;
	};

	bool SameBytes(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }

	class GSStateTableDraw : public Fixture
	{
	protected:
		void Restart()
		{
			m_gs = nullptr;
			m_device = nullptr;
			g_gs_renderer->Destroy();
			g_gs_renderer.reset();
			g_gs_device->Destroy();
			g_gs_device.reset();
			BringUp();
		}

		/// One draw: `n` sprites of one colour and alpha, optionally blended, alpha-tested or textured.
		void Draw(int x, u8 alpha, bool blend, bool atest, bool textured, int n = 1)
		{
			Packet p;
			Environment(p, kFBP, PSMCT32);
			if (textured)
				Texture(p, kTBP, PSMCT32);

			GIFReg r = {};
			r.U64 = 0;
			r.ALPHA.A = 0;
			r.ALPHA.B = 1;
			r.ALPHA.C = 0;
			r.ALPHA.D = 1;
			p.Reg(GIF_A_D_REG_ALPHA_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			r.TEST.ATE = atest;
			r.TEST.ATST = ATST_GEQUAL;
			r.TEST.AREF = 0x40;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.RGBAQ.R = 0x40 + static_cast<u8>(x & 0x3F);
			r.RGBAQ.G = 0x30;
			r.RGBAQ.A = alpha;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			for (int i = 0; i < n; i++)
			{
				const int x0 = (x + i * 20) * 16;
				p.Vertex(x0, 16 * 16, 0, 0, 0);
				p.Vertex(x0 + 16 * 16, 48 * 16, 0, 256, 512);
			}

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.ABE = blend;
			prim.TME = textured;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}

		/// Repeats a handful of register states with varied facts: alpha above, at and below 128, blended or not,
		/// alpha-tested, textured, several sprites.
		std::vector<Snap> Sequence()
		{
			std::vector<Snap> out;
			for (int pass = 0; pass < 3; pass++)
			{
				for (int i = 0; i < 12; i++)
				{
					const u8 alpha = (i % 3 == 0) ? 0x80 : (i % 3 == 1) ? 0x40 : 0xC0;
					Draw(10 + i * 30, alpha, (i & 1) != 0, (i & 2) != 0, (i & 4) != 0, 1 + (i & 3));
					const CaptureDevice& d = *m_device;
					Snap s;
					std::memset(&s, 0, sizeof(s));
					s.draws = d.m_draws;
					s.ps = d.m_ps;
					s.cb_ps = d.m_cb_ps;
					s.cb_vs = d.m_cb_vs;
					s.blend = d.m_blend;
					s.colormask = d.m_colormask;
					s.depth = d.m_depth;
					s.alpha_test = d.m_alpha_test;
					s.destination_alpha = d.m_destination_alpha;
					s.one_barrier = d.m_require_one_barrier;
					s.full_barrier = d.m_require_full_barrier;
					out.push_back(s);
				}
			}
			return out;
		}

		void TearDown() override
		{
			GSDrawRows::s_mode = GSDrawRows::Mode::On;
			GSDrawRows::s_stats = false;
			GSDrawRows::stats = {};
			Fixture::TearDown();
		}
	};
} // namespace

TEST_F(GSStateTableDraw, AnEntryHandsTheDeviceWhatTheDerivationWould)
{
	for (const float scale : {1.0f, 2.0f})
	{
		GSConfig.UpscaleMultiplier = scale;
		if (!m_gs)
			BringUp();
		else
			Restart();

		GSDrawRows::s_mode = GSDrawRows::Mode::Off;
		const std::vector<Snap> derived = Sequence();

		Restart();
		GSDrawRows::s_mode = GSDrawRows::Mode::On;
		GSDrawRows::s_stats = true;
		GSDrawRows::stats = {};
		const std::vector<Snap> taken = Sequence();
		EXPECT_GT(GSDrawRows::stats.hits, 0u);

		ASSERT_EQ(derived.size(), taken.size());
		for (size_t i = 0; i < derived.size(); i++)
		{
			const Snap& a = derived[i];
			const Snap& b = taken[i];
			EXPECT_EQ(a.draws, b.draws) << "draw " << i << " scale " << scale;
			EXPECT_TRUE(SameBytes(&a.ps, &b.ps, sizeof(a.ps))) << "draw " << i << " scale " << scale;
			EXPECT_TRUE(SameBytes(&a.blend, &b.blend, sizeof(a.blend))) << "draw " << i << " scale " << scale;
			EXPECT_TRUE(SameBytes(&a.colormask, &b.colormask, sizeof(a.colormask))) << "draw " << i << " scale " << scale;
			EXPECT_TRUE(SameBytes(&a.depth, &b.depth, sizeof(a.depth))) << "draw " << i << " scale " << scale;
			EXPECT_EQ(a.alpha_test, b.alpha_test) << "draw " << i << " scale " << scale;
			EXPECT_EQ(a.destination_alpha, b.destination_alpha) << "draw " << i << " scale " << scale;
			EXPECT_EQ(a.one_barrier, b.one_barrier) << "draw " << i << " scale " << scale;
			EXPECT_EQ(a.full_barrier, b.full_barrier) << "draw " << i << " scale " << scale;
		}

		EXPECT_GT(GSDrawRows::stats.rows, 0u) << "scale " << scale;

		Restart();
		GSDrawRows::s_mode = GSDrawRows::Mode::Check;
		GSDrawRows::stats = {};
		Sequence();
		EXPECT_GT(GSDrawRows::stats.hits, 0u) << "scale " << scale;
		EXPECT_EQ(GSDrawRows::stats.check_mismatches, 0u) << "scale " << scale;
		EXPECT_EQ(GSDrawRows::stats.prep_mismatches, 0u) << "scale " << scale;
		EXPECT_GT(GSDrawRows::stats.row_checked, 0u) << "scale " << scale;
		EXPECT_EQ(GSDrawRows::stats.row_mismatches, 0u) << "scale " << scale;
	}
}

// A draw that samples the frame it draws into reads what the run wrote: it takes the full path, and the draw after
// it opens a new run.
TEST_F(GSStateTableDraw, ADrawThatSamplesItsOwnFrameIsNotARow)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	GSDrawRows::s_mode = GSDrawRows::Mode::On;
	GSDrawRows::s_stats = true;
	GSDrawRows::stats = {};

	// Textured, so not a memory clear; the second lands inside what the first made valid, so it is a row.
	Draw(10, 0x80, false, false, true);
	Draw(10, 0x80, false, false, true);
	const u64 rows_before = GSDrawRows::stats.rows;
	EXPECT_GT(rows_before, 0u);

	// The texture is the frame itself.
	Packet p;
	Environment(p, kFBP, PSMCT32);
	Texture(p, kFBP << 5, PSMCT32);
	p.Vertex(200 * 16, 16 * 16, 0, 0, 0);
	p.Vertex(216 * 16, 48 * 16, 0, 256, 512);
	GIFRegPRIM prim = {};
	prim.PRIM = GS_SPRITE;
	prim.TME = 1;
	prim.FST = 1;
	p.Send(*m_gs, prim);
	EXPECT_EQ(GSDrawRows::stats.rows, rows_before);
}
