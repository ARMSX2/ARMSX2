// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The ways the tests of the write stamps and of the texture hash memo write into GS local memory through
// the hardware renderer, each driven the way a game reaches it: a GIF transfer, the clear sprite, the
// two CPU hacks and the palette sprite on the software scanline road.

#pragma once

#include "gs_hw_draw_harness.h"
#include "gs_write_stamp_watch.h"

#include "GS/GSPerfMon.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <cstring>
#include <random>
#include <vector>

namespace GSWriteStamps
{
	using namespace GSHWDrawHarness;

	/// A host-to-local transfer, sent a piece at a time through the GIF the way the EE sends it.
	class Upload
	{
	public:
		Upload(GSState& gs, u32 dbp, u32 dbw, u32 dpsm, int x, int y, int w, int h)
			: m_gs(gs)
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			r.BITBLTBUF.DBP = dbp;
			r.BITBLTBUF.DBW = dbw;
			r.BITBLTBUF.DPSM = dpsm;
			p.Reg(GIF_A_D_REG_BITBLTBUF, r);

			r.U64 = 0;
			r.TRXPOS.DSAX = x;
			r.TRXPOS.DSAY = y;
			m_pos = r.TRXPOS;
			p.Reg(GIF_A_D_REG_TRXPOS, r);

			r.U64 = 0;
			r.TRXREG.RRW = w;
			r.TRXREG.RRH = h;
			p.Reg(GIF_A_D_REG_TRXREG, r);

			r.U64 = 0;
			r.TRXDIR.XDIR = 0;
			p.Reg(GIF_A_D_REG_TRXDIR, r);

			p.Send(gs, GIFRegPRIM{});
		}

		/// Bytes of the transfer: its rectangle at the format's size, rounded up to a quadword.
		static size_t Size(u32 psm, int w, int h)
		{
			const size_t bytes = (static_cast<size_t>(w) * h * GSLocalMemory::m_psm[psm].trbpp + 7) / 8;
			return (bytes + 15) & ~static_cast<size_t>(15);
		}

		/// `qwords` quadwords of 0xFF in one IMAGE tag.
		void Data(u32 qwords)
		{
			std::vector<GIFPackedReg> buf(qwords + 1);
			std::memset(buf.data(), 0xFF, buf.size() * sizeof(GIFPackedReg));
			GIFTag tag = {};
			tag.NLOOP = qwords;
			tag.EOP = 1;
			tag.FLG = GIF_FLG_IMAGE;
			std::memcpy(&buf[0], &tag, sizeof(tag));
			m_gs.Transfer<0>(reinterpret_cast<const u8*>(buf.data()), static_cast<u32>(buf.size()));
		}

		/// Ends the slice that has arrived: a changed TRXPOS makes the parser write out what it holds, and
		/// the register is put back at once. The transfer carries on from where the slice stopped.
		void CutSlice()
		{
			Packet p;
			GIFReg r = {};
			r.U64 = 0;
			r.TRXPOS.DSAX = m_pos.DSAX + 1;
			p.Reg(GIF_A_D_REG_TRXPOS, r);
			r.TRXPOS = m_pos;
			p.Reg(GIF_A_D_REG_TRXPOS, r);
			p.Send(m_gs, GIFRegPRIM{});
		}

	private:
		GSState& m_gs;
		GIFRegTRXPOS m_pos;
	};

	class WriterFixture : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
		}

		void TearDown() override
		{
			g_gs_front.reset();
			Fixture::TearDown();
		}

		void Fill(u8 value) { std::memset(m_gs->m_mem.m_vm8, value, GSLocalMemory::m_vmsize); }

		// Words that differ from each other, so that a store of any other value is a change.
		void FillRandom()
		{
			std::mt19937 rng(7);
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 4)
			{
				const u32 v = rng();
				std::memcpy(m_gs->m_mem.m_vm8 + i, &v, 4);
			}
		}

		// A change of a texture cache setting or the hack the renderer follows reaches it through here.
		void UseHack(const char* before_draw)
		{
			GSConfig.BeforeDrawFunctionId = GSLookupBeforeDrawFunctionId(before_draw);
			ASSERT_GE(GSConfig.BeforeDrawFunctionId, 0) << before_draw;
			m_gs->UpdateRenderFixes();
		}

		static void ExpectCovered(const Watch& watch, const char* what)
		{
			EXPECT_TRUE(watch.UnmarkedChangedPages().empty())
				<< what << ": changed but unmarked pages " << Watch::Describe(watch.UnmarkedChangedPages());
		}

		// A sprite of one colour over the frame, which the renderer takes for a clear of local memory.
		static void ClearDraw(GSState& gs, u32 frame_psm, int x0, int y0, int x1, int y1, bool depth)
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			r.SCISSOR.SCAX1 = 639;
			r.SCISSOR.SCAY1 = 447;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = 0;
			r.FRAME.FBW = 10;
			r.FRAME.PSM = frame_psm;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = depth ? 0 : 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);

			r.U64 = 0;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r); // colour 0, the zero clear

			p.Vertex(x0 << 4, y0 << 4, 0, 0, 0);
			p.Vertex(x1 << 4, y1 << 4, 0, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(gs, prim);
		}

		static void SpriteDraw(GSState& gs, int x0, int y0, int x1, int y1, u32 fbp)
		{
			Packet p;
			Environment(p, fbp, PSMCT32);
			p.Vertex(x0 << 4, y0 << 4, 0, 0, 0);
			p.Vertex(x1 << 4, y1 << 4, 0, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(gs, prim);
		}
	};

	class SwPrimFixture : public WriterFixture
	{
	protected:
		void SetUp() override
		{
			WriterFixture::SetUp();
			GSConfig.UserHacks_CPUSpriteRenderBW = 64; // admit every FBW
			GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
			GSVertexSW::InitStatic();
		}

		// An 8 x 8 block of a 4-bit texture through a 32-bit palette into a 32-bit frame, texel for texel.
		void Tile(int fbp)
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);

			r.U64 = 0;
			r.XYOFFSET.OFX = 2048 * 16;
			r.XYOFFSET.OFY = 2048 * 16;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX1 = 2047;
			r.SCISSOR.SCAY1 = 2047;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = fbp;
			r.FRAME.FBW = 32;
			r.FRAME.PSM = PSMCT32;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = 0x1C0;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.COLCLAMP.CLAMP = 1;
			p.Reg(GIF_A_D_REG_COLCLAMP, r);

			r.U64 = 0;
			r.TEX1.LCM = 1;
			p.Reg(GIF_A_D_REG_TEX1_1, r);

			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);

			r.U64 = 0;
			r.TEX0.TBP0 = 0x2000;
			r.TEX0.TBW = 1;
			r.TEX0.PSM = PSMT4;
			r.TEX0.TW = 5;
			r.TEX0.TH = 5;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			r.TEX0.CBP = 0x3000;
			r.TEX0.CPSM = PSMCT32;
			r.TEX0.CLD = 1;
			p.Reg(GIF_A_D_REG_TEX0_1, r);

			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			p.Vertex((2048 + 16) * 16, (2048 + 8) * 16, 0, 8 * 16 + 8, 16 * 16 + 8);
			p.Vertex((2048 + 24) * 16, (2048 + 16) * 16, 0, 16 * 16 + 8, 24 * 16 + 8);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}
	};
} // namespace GSWriteStamps
