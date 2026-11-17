// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Copy runs (GSRendererHWCopyRuns.cpp), through the renderer's own GIF parser on the deviceless backend. Every test
// runs one script twice: once with the palette block copy off, so that every copy goes through Draw() to the
// rasterizer and no run is ever armed, and once as shipped. Local memory and the draws the device was handed must
// come out the same, and the counts say the copies did continue a run, or stopped where they must.

#include "gs_hw_draw_harness.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <random>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	constexpr size_t VM_SIZE = GSLocalMemory::m_vmsize;

	constexpr u32 DEST_FBP = 0x40; // pages: the atlas, a 32-bit frame one page wide
	constexpr u32 TEX_TBP = 0x1000; // blocks: 4-bit tile pages
	constexpr u32 CLUT_CBP = 0x1400; // blocks: 256-entry 32-bit palettes

	/// One step of a script.
	struct Step
	{
		enum Kind
		{
			Copy, ///< one 8x8 tile (or a strip of them) into the atlas, from a 4- or 8-bit texture through the palette
			Consume, ///< a draw that reaches the GPU and samples the atlas
			Write, ///< bytes written into local memory the way an upload is, with its marks and invalidation
			TargetAt, ///< a GPU draw whose frame is at `bp`, which makes a target there
		} kind;
		int x = 0, y = 0; ///< Copy: the tile's top-left pixel in the atlas
		int sprites = 1; ///< Copy: tiles side by side, one sprite each
		u32 tbp = TEX_TBP; ///< Copy: texture address
		u32 tex_psm = PSMT4;
		u32 cbp = CLUT_CBP; ///< Copy: palette address
		u32 csa = 0; ///< Copy: palette offset
		u32 tfx = TFX_DECAL;
		bool abe = false; ///< Copy: blending on (a run is never armed under it)
		// Write: the rectangle of the surface (bp, bw, psm), and the value pattern's seed.
		u32 bp = 0, bw = 1, psm = PSMCT32;
		GSVector4i rect = GSVector4i::zero();
		u32 seed = 0;
	};

	struct Result
	{
		std::vector<u8> vm;
		u32 device_draws = 0;
		u64 continued = 0;
	};

	class GSCopyRun : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.UserHacks_CPUSpriteRenderBW = 1; // as Call of Duty 3
			GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
			GSVertexSW::InitStatic();
		}

		void Destroy()
		{
			m_gs = nullptr;
			m_device = nullptr;
			if (g_gs_renderer)
			{
				g_gs_renderer->Destroy();
				g_gs_renderer.reset();
			}
			if (g_gs_device)
			{
				g_gs_device->Destroy();
				g_gs_device.reset();
			}
		}

		static void SendCopy(GSState& gs, const Step& s)
		{
			Packet p;
			GIFReg r;
			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);
			r.U64 = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);
			r.U64 = 0;
			r.SCISSOR.SCAX1 = 63;
			r.SCISSOR.SCAY1 = 63;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);
			r.U64 = 0;
			r.FRAME.FBP = DEST_FBP;
			r.FRAME.FBW = 1;
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
			r.ALPHA.A = 0;
			r.ALPHA.B = 1;
			r.ALPHA.C = 0;
			r.ALPHA.D = 1;
			p.Reg(GIF_A_D_REG_ALPHA_1, r);
			r.U64 = 0;
			r.TEXA.TA0 = 0x80;
			r.TEXA.TA1 = 0x80;
			p.Reg(GIF_A_D_REG_TEXA, r);
			r.U64 = 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			r.U64 = 0;
			r.TEX0.TBP0 = s.tbp;
			r.TEX0.TBW = 2;
			r.TEX0.PSM = s.tex_psm;
			r.TEX0.TW = 7;
			r.TEX0.TH = 7;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = s.tfx;
			r.TEX0.CBP = s.cbp;
			r.TEX0.CPSM = PSMCT32;
			r.TEX0.CSA = s.csa;
			r.TEX0.CLD = 1;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
			for (int i = 0; i < s.sprites; i++)
			{
				const int x = s.x + 8 * i;
				const int u = 8 * ((s.csa + i) & 7);
				const int v = 8 * ((s.tbp >> 4) & 7);
				p.Vertex(x * 16, s.y * 16, 0, u * 16, v * 16);
				p.Vertex((x + 8) * 16, (s.y + 8) * 16, 0, (u + 8) * 16, (v + 8) * 16);
			}
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			prim.ABE = s.abe;
			p.Send(gs, prim);
		}

		// A wide-frame textured sprite: it reaches the GPU, and samples the atlas as an 8-bit texture.
		static void SendConsume(GSState& gs)
		{
			Packet p;
			Environment(p, 0x100, PSMCT32);
			GIFReg r;
			r.U64 = 0;
			r.TEX0.TBP0 = DEST_FBP << 5;
			r.TEX0.TBW = 2;
			r.TEX0.PSM = PSMT8;
			r.TEX0.TW = 6;
			r.TEX0.TH = 6;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			r.TEX0.CBP = CLUT_CBP + 0x40;
			r.TEX0.CPSM = PSMCT32;
			r.TEX0.CLD = 1;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(64 * 16, 64 * 16, 0, 64 * 16, 64 * 16);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(gs, prim);
		}

		// A wide-frame untextured fill at `bp` that keeps depth on, so the copy road never takes it: a target is
		// made there.
		static void SendTargetAt(GSState& gs, u32 bp)
		{
			Packet p;
			Environment(p, bp >> 5, PSMCT32);
			GIFReg r;
			r.U64 = 0;
			r.ZBUF.ZBP = 0x1E0;
			r.ZBUF.PSM = PSMZ32;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(64 * 16, 64 * 16, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(gs, prim);
		}

		void DoWrite(const Step& s)
		{
			GSLocalMemory& mem = m_gs->m_mem;
			const GSLocalMemory::psm_t& f = GSLocalMemory::m_psm[s.psm];
			std::mt19937 rng(s.seed);
			for (int y = s.rect.y; y < s.rect.w; y++)
				for (int x = s.rect.x; x < s.rect.z; x++)
					(mem.*f.wp)(x, y, rng(), s.bp, s.bw);
			const GSOffset off = mem.GetOffset(s.bp, s.bw, s.psm);
			mem.MarkPagesWritten(off, s.rect);
			mem.m_clut.InvalidateRange(GSLocalMemory::GetStartBlockAddress(s.bp, s.bw, s.psm, s.rect),
				GSLocalMemory::GetEndBlockAddress(s.bp, s.bw, s.psm, s.rect));
			GIFRegBITBLTBUF blit = {};
			blit.DBP = s.bp;
			blit.DBW = s.bw;
			blit.DPSM = s.psm;
			m_gs->InvalidateVideoMem(blit, s.rect);
		}

		Result Run(const std::vector<Step>& script, const std::vector<u8>& vm, bool palette_block_copy)
		{
			Result out;
			GSBurstMemo::SetForcedOff(true);
			BringUp();
			if (HasFatalFailure())
				return out;
			m_gs->GetSwPrimState().palette_block_copy = palette_block_copy;
			std::memcpy(m_gs->m_mem.m_vm8, vm.data(), vm.size());
			m_gs->m_mem.MarkAllPagesWritten();

			for (const Step& s : script)
			{
				switch (s.kind)
				{
					case Step::Copy:
						SendCopy(*m_gs, s);
						break;
					case Step::Consume:
						SendConsume(*m_gs);
						break;
					case Step::Write:
						DoWrite(s);
						break;
					case Step::TargetAt:
						SendTargetAt(*m_gs, s.bp);
						break;
				}
			}

			out.vm.assign(m_gs->m_mem.m_vm8, m_gs->m_mem.m_vm8 + VM_SIZE);
			out.device_draws = m_device->m_draws;
			out.continued = m_gs->CopyRunCopies();
			GSBurstMemo::SetForcedOff(false);
			Destroy();
			return out;
		}

		static std::vector<u8> RandomMemory(u32 seed)
		{
			std::vector<u8> vm(VM_SIZE);
			std::mt19937 rng(seed);
			for (size_t i = 0; i < vm.size(); i += 4)
			{
				const u32 w = rng();
				std::memcpy(&vm[i], &w, 4);
			}
			return vm;
		}

		static size_t DifferingBytes(const std::vector<u8>& a, const std::vector<u8>& b)
		{
			size_t n = 0;
			for (size_t i = 0; i < a.size() && i < b.size(); i++)
				n += a[i] != b[i];
			return n;
		}

		/// `n` tiles across the atlas, each from its own texture address and palette offset.
		static std::vector<Step> Tiles(int n, int first = 0, u32 tex_psm = PSMT4)
		{
			std::vector<Step> out;
			for (int i = first; i < first + n; i++)
			{
				Step s{Step::Copy};
				s.x = 8 * (i & 7);
				s.y = 8 * ((i >> 3) & 7);
				s.tbp = TEX_TBP + 0x20 * (i % 5);
				s.csa = static_cast<u32>(i * 3) & 15;
				s.cbp = CLUT_CBP + 4 * (i % 2);
				s.tex_psm = tex_psm;
				out.push_back(s);
			}
			return out;
		}

		static std::vector<Step> Concat(std::initializer_list<std::vector<Step>> parts)
		{
			std::vector<Step> out;
			for (const auto& p : parts)
				out.insert(out.end(), p.begin(), p.end());
			return out;
		}

		void ExpectSameAsRasterizer(const std::vector<Step>& script, u32 seed, u64 min_continued, u64 max_continued)
		{
			const std::vector<u8> vm = RandomMemory(seed);
			const Result ref = Run(script, vm, false);
			const Result on = Run(script, vm, true);
			ASSERT_EQ(ref.vm.size(), on.vm.size());
			EXPECT_EQ(DifferingBytes(ref.vm, on.vm), 0u) << "local memory differs from the rasterizer's";
			EXPECT_EQ(ref.device_draws, on.device_draws) << "the device was handed other draws";
			EXPECT_EQ(ref.continued, 0u);
			EXPECT_GE(on.continued, min_continued);
			EXPECT_LE(on.continued, max_continued);
		}
	};
} // namespace

// A run of tiles, each with its own texture address, palette address and offset, and a palette load before it: the
// first copy arms the run and every later one continues it.
TEST_F(GSCopyRun, TilesContinueTheRunAndWriteWhatTheRasterizerWrites)
{
	ExpectSameAsRasterizer(Tiles(24), 1, 23, 23);
}

// 8-bit tiles, two sprites to a draw.
TEST_F(GSCopyRun, EightBitStripsContinueTheRun)
{
	std::vector<Step> script = Tiles(12, 0, PSMT8);
	for (Step& s : script)
		s.sprites = 2;
	ExpectSameAsRasterizer(script, 2, 11, 11);
}

// A draw that reaches the GPU between two runs ends the first; an upload ends a run too. Each run's first copy goes
// through Draw().
TEST_F(GSCopyRun, AGpuDrawOrAnUploadEndsTheRun)
{
	Step write{Step::Write};
	write.bp = TEX_TBP;
	write.bw = 2;
	write.psm = PSMT4;
	write.rect = GSVector4i(0, 0, 64, 32);
	write.seed = 5;
	const std::vector<Step> script = Concat({Tiles(8), {Step{Step::Consume}}, Tiles(8, 8), {write}, Tiles(8, 16),
		{Step{Step::Consume}}});
	ExpectSameAsRasterizer(script, 3, 21, 21);
}

// A copy whose palette is loaded from blocks an earlier copy of the run wrote reads them as that copy left them.
TEST_F(GSCopyRun, APaletteLoadedFromTheAtlasReadsWhatTheRunWrote)
{
	std::vector<Step> script = Tiles(16);
	for (size_t i = 8; i < script.size(); i++)
		script[i].cbp = (DEST_FBP << 5) + static_cast<u32>(i & 3);
	ExpectSameAsRasterizer(script, 4, 15, 15);
}

// A copy from a texture address where a target now lives is the decision's to make again: it leaves the run, Draw()
// decides it, and the copies after it start a new run.
TEST_F(GSCopyRun, ATextureAddressWithATargetLeavesTheRun)
{
	constexpr u32 TARGET_BP = 0x2000;
	Step target{Step::TargetAt};
	target.bp = TARGET_BP;
	std::vector<Step> tiles = Tiles(12);
	tiles[6].tbp = TARGET_BP;
	const std::vector<Step> script = Concat({{target}, tiles});
	ExpectSameAsRasterizer(script, 5, 1, 10);
}

// Under blending a copy never arms a run, whatever the copy road does with it.
TEST_F(GSCopyRun, NoRunUnderBlending)
{
	std::vector<Step> script = Tiles(8);
	for (Step& s : script)
		s.abe = true;
	ExpectSameAsRasterizer(script, 6, 0, 0);
}
