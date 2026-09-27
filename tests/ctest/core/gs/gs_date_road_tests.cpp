// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins which destination-alpha-test (DATE) mode a draw gets on a declared feedback loop whose
// driver orders the read of the fragment's own pixel.
//
// On that road DetermineBarriers drops a draw's barriers, so reading destination alpha in the
// shader (Full) costs nothing. Stencil DATE there always runs its setup draw -- StencilOne skips it
// only when a barrier survives -- and that ends the render pass twice per draw. Stuntman on
// Turnip 26.3, the first Turnip with a stencil buffer, drew 60% more passes for it.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFBP = 0x40;

	class GSDateRoad : public Fixture
	{
	protected:
		/// The Adreno 650 on Turnip 26.3 with the a6xx feedback-loop fix, as far as DATE is concerned.
		void DeclaredLoopDevice(bool driver_orders_overlap)
		{
			GSDevice::FeatureSupport& f = m_device->MutableFeatures();
			f.texture_barrier = true;
			f.framebuffer_fetch = false;
			f.framebuffer_fetch_orders_overlap = false;
			f.feedback_loop_layout = true;
			f.declared_feedback_loop_orders_overlap = driver_orders_overlap;
			f.stencil_buffer = true;
			f.primitive_id = true;
		}

		/// Sprites in one draw, all with vertex alpha `alpha`.
		void Sprites(u8 alpha, std::initializer_list<GSVector4i> rects, bool date)
		{
			Packet p;
			Environment(p, kFBP, PSMCT32);

			GIFReg r = {};
			r.RGBAQ.R = 0x40;
			r.RGBAQ.G = 0x60;
			r.RGBAQ.B = 0x20;
			r.RGBAQ.A = alpha;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			r.TEST.DATE = date;
			r.TEST.DATM = 0; // pass where destination alpha is below 0x80
			p.Reg(GIF_A_D_REG_TEST_1, r);

			for (const GSVector4i& rc : rects)
			{
				p.Vertex(rc.x * 16, rc.y * 16, 1, 0, 0);
				p.Vertex(rc.z * 16, rc.w * 16, 1, 0, 0);
			}

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		/// A target whose alpha is 0 on the left and 0x80 on the right, then a DATE draw of two
		/// overlapping sprites writing alpha 0x80 across both halves. Overlap keeps the draw off the
		/// no-overlap barrier branch, and the written alpha (always >= 0x80 under DATM 0) is what
		/// sends it to StencilOne wherever there is a stencil buffer.
		void DrawOverlappingDATE()
		{
			Sprites(0x00, {GSVector4i(0, 0, 640, 448)}, false);
			Sprites(0x80, {GSVector4i(320, 0, 640, 448)}, false);

			const u32 before = m_device->m_draws;
			Sprites(0x80, {GSVector4i(100, 100, 500, 300), GSVector4i(200, 150, 600, 350)}, true);
			ASSERT_EQ(m_device->m_draws, before + 1);
		}
	};
} // namespace

// The driver orders the read: Full, with no barrier left for the backend to honour, so nothing
// ends the pass.
TEST_F(GSDateRoad, DriverOrderedDeclaredLoopReadsDestinationAlphaInThePass)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	DeclaredLoopDevice(true);
	DrawOverlappingDATE();

	EXPECT_EQ(m_device->m_destination_alpha, GSHWDrawConfig::DestinationAlphaMode::Full);
	EXPECT_FALSE(m_device->m_require_one_barrier);
	EXPECT_FALSE(m_device->m_require_full_barrier);
}

// The same declared loop with barriers kept (the a7xx preference, or a driver not known to order
// one) keeps the stencil choice: there the barrier is real and stencil DATE is what saves it.
TEST_F(GSDateRoad, BarrierOrderedDeclaredLoopKeepsStencil)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	DeclaredLoopDevice(false);
	DrawOverlappingDATE();

	EXPECT_EQ(m_device->m_destination_alpha, GSHWDrawConfig::DestinationAlphaMode::StencilOne);
}
