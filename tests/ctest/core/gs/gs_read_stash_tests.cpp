// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The read made ahead (GSTextureCache::StashRead): when the EE reads a few pixels of a target and then,
// before anything draws to it again, reads more of it, InvalidateLocalMem reads the target's whole drawn
// area on the second read. Once a target has done that after a given first read, the next time that
// first read comes the whole drawn area is read in the same GPU round trip and kept, and the second read
// takes it from there. The kept pixels are the bytes the second read would download only while nothing
// that could change the target has happened in between, so these tests pin when they are used and when
// they are not: used for the second read of a burst; not after a draw, a host-to-local write or a new
// frame; and no longer made for a first read that stops being followed by a second one.

#include "gs_hw_draw_harness.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/HW/GSTextureCache.h"

using namespace GSHWDrawHarness;

namespace
{
	class ReadStash : public Fixture
	{
	protected:
		static constexpr u32 kBP = 0x1180;
		static constexpr u32 kBW = 10;
		static constexpr int kW = 640;
		static constexpr int kH = 448;

		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.HWDownloadMode = GSHardwareDownloadMode::Enabled;
		}

		// An untextured sprite over the frame at kBP, so it is a colour target drawn since its last read. The
		// first covers the frame; later ones a corner of it, so the target stays the same object.
		void DrawFrame()
		{
			const int w = m_frames_drawn++ ? 64 : kW;
			const int h = m_frames_drawn > 1 ? 64 : kH;
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX1 = kW - 1;
			r.SCISSOR.SCAY1 = kH - 1;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = kBP / 32;
			r.FRAME.FBW = kBW;
			r.FRAME.PSM = PSMCT32;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ16;
			r.ZBUF.ZMSK = 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);

			r.U64 = 0;
			r.RGBAQ.R = 0x40;
			r.RGBAQ.G = 0x60;
			r.RGBAQ.B = 0x20;
			r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(w << 4, h << 4, 0, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		// What GSState::InitReadFIFO asks the texture cache for when the EE reads w x h pixels at (x, y).
		void EERead(int x, int y, int w, int h)
		{
			g_texture_cache->InvalidateLocalMem(m_gs->m_mem.GetOffset(kBP, kBW, PSMCT32), GSVector4i(x, y, x + w, y + h));
		}

		// A probe of four pixels inside the corner later draws cover, then a second probe: SOCOM II's pair.
		void Burst()
		{
			EERead(17, 17, 1, 4);
			EERead(20, 17, 1, 4);
		}

		u64 Served() const { return g_texture_cache->GetReadStashStats().served; }
		int m_frames_drawn = 0;
		u64 Stashed() const { return g_texture_cache->GetReadStashStats().stashed; }
	};

	TEST_F(ReadStash, TheSecondReadOfABurstComesFromTheFirstRoundTrip)
	{
		BringUp();
		DrawFrame();
		Burst(); // learns that this first read is followed by a second one
		EXPECT_EQ(Stashed(), 0u);
		EXPECT_EQ(Served(), 0u);

		DrawFrame();
		Burst();
		EXPECT_EQ(Stashed(), 1u);
		EXPECT_EQ(Served(), 1u);
	}

	TEST_F(ReadStash, ADrawInBetweenIsNotServed)
	{
		BringUp();
		DrawFrame();
		Burst();

		DrawFrame();
		EERead(17, 17, 1, 4);
		DrawFrame();
		EERead(20, 17, 1, 4);
		EXPECT_EQ(Stashed(), 1u);
		EXPECT_EQ(Served(), 0u);
	}

	TEST_F(ReadStash, AWriteIntoLocalMemoryInBetweenIsNotServed)
	{
		BringUp();
		DrawFrame();
		Burst();

		DrawFrame();
		EERead(17, 17, 1, 4);
		g_texture_cache->InvalidateVideoMem(m_gs->m_mem.GetOffset(0x3000, 1, PSMCT32), GSVector4i(0, 0, 8, 8));
		EERead(20, 17, 1, 4);
		EXPECT_EQ(Served(), 0u);
	}

	TEST_F(ReadStash, ANewFrameInBetweenIsNotServed)
	{
		BringUp();
		DrawFrame();
		Burst();

		DrawFrame();
		EERead(17, 17, 1, 4);
		g_perfmon.EndFrame(false);
		EERead(20, 17, 1, 4);
		EXPECT_EQ(Served(), 0u);
	}

	TEST_F(ReadStash, AFirstReadNoLongerFollowedStopsBeingStashed)
	{
		BringUp();
		DrawFrame();
		Burst();

		// The first read alone: the stash made for it goes unused...
		DrawFrame();
		EERead(17, 17, 1, 4);
		EXPECT_EQ(Stashed(), 1u);

		// ...so the next time it is not made.
		DrawFrame();
		EERead(17, 17, 1, 4);
		DrawFrame();
		EERead(17, 17, 1, 4);
		EXPECT_EQ(Stashed(), 1u);
	}
} // namespace
