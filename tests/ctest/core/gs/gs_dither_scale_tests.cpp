// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins which scale the scaled dither reads: the render target's.
//
// Scaled dither (Dithering = 1) indexes the 4x4 matrix by the NATIVE pixel that owns each device
// pixel, and at a fractional scale rotates the matrix by a phase chosen for that scale. Both are
// questions about the render target's grid. The pixel shader constant ScaleFactor carries two
// scales: x and y are the TEXTURE's (they address the texture), z is the render target's. A texture
// read from GS memory is at scale 1 whatever the upscale, so a dither that took its scale from x
// indexed the matrix per device pixel and picked the phase for 1x on every memory-textured draw,
// while an untextured draw in the same frame took the target's scale -- two dither patterns side
// by side.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kDstFBP = 0x40;
	// Well clear of the frame, the Z buffer and anything else the draw creates, so the texture
	// cache finds no target there and reads GS memory.
	constexpr u32 kMemoryTBP = 0x3000;

	// The matrix the PS2 libraries load.
	GIFRegDIMX StandardDIMX()
	{
		GIFRegDIMX d = {};
		d.DM00 = -4; d.DM01 = 2; d.DM02 = -3; d.DM03 = 3;
		d.DM10 = 0; d.DM11 = -2; d.DM12 = 1; d.DM13 = -1;
		d.DM20 = -3; d.DM21 = 3; d.DM22 = -4; d.DM23 = 2;
		d.DM30 = 1; d.DM31 = -1; d.DM32 = 0; d.DM33 = -2;
		return d;
	}

	class GSDitherScale : public Fixture
	{
	protected:
		void DitheredDraw(bool textured)
		{
			Packet p;
			Environment(p, kDstFBP, PSMCT16);
			if (textured)
				Texture(p, kMemoryTBP, PSMCT32);

			GIFReg r = {};
			r.DIMX = StandardDIMX();
			p.Reg(GIF_A_D_REG_DIMX, r);
			r.U64 = 0;
			r.DTHE.DTHE = 1;
			p.Reg(GIF_A_D_REG_DTHE, r);

			p.Vertex(0, 0, 1, 0, 0);
			p.Vertex(256 * 16, 224 * 16, 1, 256 * 16, 224 * 16);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = textured ? 1 : 0;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}
	};
} // namespace

TEST_F(GSDitherScale, AMemoryTexturedDrawDithersOnTheTargetsGrid)
{
	GSConfig.UpscaleMultiplier = 1.5f;
	GSConfig.Dithering = 1;
	BringUp();

	const u32 before = m_device->m_draws;
	DitheredDraw(true);
	ASSERT_EQ(m_device->m_draws, before + 1) << "the draw never reached the backend";
	ASSERT_EQ(m_device->m_ps.dither, 1u) << "the draw is not dithered, so it tests nothing";

	// The premise: the texture is at 1x and the target at 1.5x.
	ASSERT_EQ(m_device->m_cb_ps.ScaleFactor.x * 16.0f, 1.0f);
	ASSERT_EQ(m_device->m_cb_ps.ScaleFactor.z, 1.5f);

	// Every whole scale takes phase 0 (no matrix cell is wider than another there), so a 0 here is
	// the phase for the texture's 1x. The standard matrix at 1.5x rotates by a non-zero phase.
	EXPECT_NE(m_device->m_cb_ps.DitherPhase, 0u);
}

// The untextured draw beside it takes the same phase: one frame, one dither pattern.
TEST_F(GSDitherScale, TexturedAndUntexturedDrawsAgree)
{
	GSConfig.UpscaleMultiplier = 1.5f;
	GSConfig.Dithering = 1;
	BringUp();

	DitheredDraw(false);
	const u32 untextured_phase = m_device->m_cb_ps.DitherPhase;
	const float untextured_scale = m_device->m_cb_ps.ScaleFactor.z;

	DitheredDraw(true);
	EXPECT_EQ(m_device->m_cb_ps.DitherPhase, untextured_phase);
	EXPECT_EQ(m_device->m_cb_ps.ScaleFactor.z, untextured_scale);
}
