// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The burst memo on the copy road (GSRendererHWBurstMemo.cpp), through the renderer's own GIF parser on the
// deviceless backend. Every test runs one script twice, with the memo off and on, and compares all of local
// memory and the draws the device was handed: a skipped copy has to leave what the copy leaves. The counts
// say the memo did skip, or did stop where it must.

#include "gs_hw_draw_harness.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <array>
#include <functional>
#include <random>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	constexpr size_t VM_SIZE = GSLocalMemory::m_vmsize;

	constexpr u32 DEST_FBP = 0x40; // pages; block 0x800
	constexpr u32 TEX_TBP = 0x1000; // blocks
	constexpr u32 CLUT_CBP = 0x1400;

	struct Sprite
	{
		int x0, y0, x1, y1; // pixels
		int u0, v0; // texels; the sprite copies texel for texel
	};

	/// One step of a script.
	struct Step
	{
		enum Kind
		{
			Copy, ///< a burst draw: textured sprites into the narrow frame at DEST_FBP
			Consume, ///< a draw that reaches the GPU and samples the narrow frame
			Write, ///< bytes written into local memory the way an upload is, with its marks and invalidation
			TargetAtTexture, ///< a GPU draw whose frame is the burst's texture
		} kind;
		std::vector<Sprite> sprites;
		u32 tex_psm = PSMCT32;
		u32 frame_psm = PSMCT32;
		u32 fbp = DEST_FBP; ///< Copy: the frame (pages)
		u32 tbp = TEX_TBP; ///< Copy: the texture (blocks)
		bool tme = true; ///< Copy: textured; an untextured one is a fill
		// Write: the rectangle of the surface (bp, bw, psm), and the value pattern's seed.
		u32 bp = 0, bw = 1, psm = PSMCT32;
		GSVector4i rect = GSVector4i::zero();
		u32 seed = 0;
	};

	struct Result
	{
		std::vector<u8> vm;
		u32 device_draws = 0;
		GSBurstMemo::Stats stats;
		/// The texture cache's source lookups and what they uploaded (GSPerfMon): what the invalidations left
		/// the sources needing.
		std::array<double, 5> texture_cache = {};
	};

	class GSBurstMemoDraw : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.UserHacks_CPUSpriteRenderBW = 2; // the narrow frame, as Spider-Man 3
			GSConfig.UserHacks_CPUSpriteRenderLevel = 2;
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
			r.FRAME.FBP = s.fbp;
			r.FRAME.FBW = 1;
			r.FRAME.PSM = s.frame_psm;
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
			r.TEX0.TBW = 1;
			r.TEX0.PSM = s.tex_psm;
			r.TEX0.TW = 6;
			r.TEX0.TH = 6;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			if (GSLocalMemory::m_psm[s.tex_psm].pal > 0)
			{
				r.TEX0.CBP = CLUT_CBP;
				r.TEX0.CPSM = PSMCT32;
				r.TEX0.CLD = 1;
			}
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.U64 = 0;
			r.RGBAQ.R = r.RGBAQ.G = r.RGBAQ.B = r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
			for (const Sprite& sp : s.sprites)
			{
				p.Vertex(sp.x0 * 16, sp.y0 * 16, 0, sp.u0 * 16, sp.v0 * 16);
				p.Vertex(sp.x1 * 16, sp.y1 * 16, 0, (sp.u0 + sp.x1 - sp.x0) * 16, (sp.v0 + sp.y1 - sp.y0) * 16);
			}
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = s.tme;
			prim.FST = 1;
			p.Send(gs, prim);
		}

		// A wide-frame textured sprite: it reaches the GPU, and samples `tbp` as a 32-bit texture.
		static void SendGpu(GSState& gs, u32 fbp, u32 tbp)
		{
			Packet p;
			Environment(p, fbp, PSMCT32);
			GIFReg r;
			r.U64 = 0;
			r.TEX0.TBP0 = tbp;
			r.TEX0.TBW = 1;
			r.TEX0.PSM = PSMCT32;
			r.TEX0.TW = 6;
			r.TEX0.TH = 6;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
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

		// A narrow-frame draw at the texture's address that the copy road turns away, so the GPU draws it and
		// a target is made there: the untextured sprite fill is not a copy (level 2 takes it, so use the depth
		// buffer to keep it on the GPU).
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
			// GSState::CheckWriteOverlap's CLUT invalidation, so a palette written here is loaded again.
			mem.m_clut.InvalidateRange(GSLocalMemory::GetStartBlockAddress(s.bp, s.bw, s.psm, s.rect),
				GSLocalMemory::GetEndBlockAddress(s.bp, s.bw, s.psm, s.rect));
			GIFRegBITBLTBUF blit = {};
			blit.DBP = s.bp;
			blit.DBW = s.bw;
			blit.DPSM = s.psm;
			m_gs->InvalidateVideoMem(blit, s.rect);
		}

		Result Run(const std::vector<Step>& script, const std::vector<u8>& vm, bool memo, bool control = false)
		{
			Result out;
			GSBurstMemo::SetForcedOff(!memo);
			GSBurstMemo::ResetStats();
			GSBurstMemo::SetControlCorruption(control);
			BringUp();
			if (HasFatalFailure())
				return out;
			std::memcpy(m_gs->m_mem.m_vm8, vm.data(), vm.size());
			m_gs->m_mem.MarkAllPagesWritten();
			const std::array<double, 5> texture_cache = TextureCacheCounters();

			for (const Step& s : script)
			{
				switch (s.kind)
				{
					case Step::Copy:
						SendCopy(*m_gs, s);
						break;
					case Step::Consume:
						SendGpu(*m_gs, 0x100, DEST_FBP << 5);
						break;
					case Step::Write:
						DoWrite(s);
						break;
					case Step::TargetAtTexture:
						SendTargetAt(*m_gs, TEX_TBP);
						break;
				}
			}

			out.vm.assign(m_gs->m_mem.m_vm8, m_gs->m_mem.m_vm8 + VM_SIZE);
			out.device_draws = m_device->m_draws;
			out.stats = GSBurstMemo::GlobalStats();
			out.texture_cache = TextureCacheCounters();
			for (size_t i = 0; i < texture_cache.size(); i++)
				out.texture_cache[i] -= texture_cache[i];
			GSBurstMemo::SetControlCorruption(false);
			GSBurstMemo::SetForcedOff(false);
			Destroy();
			return out;
		}

		static std::array<double, 5> TextureCacheCounters()
		{
			return {g_perfmon.GetCounter(GSPerfMon::TCSourceHit), g_perfmon.GetCounter(GSPerfMon::TCSourceMiss),
				g_perfmon.GetCounter(GSPerfMon::HashCacheHit), g_perfmon.GetCounter(GSPerfMon::HashCacheMiss),
				g_perfmon.GetCounter(GSPerfMon::Unswizzle)};
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

		/// A burst of four draws into two blocks of the narrow frame, each draw a sprite or two, the later
		/// draws writing over parts of the earlier ones.
		static std::vector<Step> Burst(u32 tex_psm = PSMCT32, u32 frame_psm = PSMCT32, u32 fbp = DEST_FBP, u32 tbp = TEX_TBP)
		{
			std::vector<Step> b;
			const std::vector<std::vector<Sprite>> draws = {
				{{0, 0, 8, 8, 0, 0}},
				{{8, 0, 16, 8, 8, 0}, {0, 8, 8, 16, 16, 0}},
				{{2, 2, 6, 6, 24, 24}},
				{{8, 0, 12, 4, 32, 40}},
			};
			for (const auto& sprites : draws)
			{
				Step s{Step::Copy};
				s.sprites = sprites;
				s.tex_psm = tex_psm;
				s.frame_psm = frame_psm;
				s.fbp = fbp;
				s.tbp = tbp;
				b.push_back(s);
			}
			return b;
		}

		/// Puts the burst's destination back to the bytes it held before the first burst ran, so the second
		/// burst starts from the same destination.
		static Step RestoreDestination(u32 frame_psm = PSMCT32)
		{
			Step s{Step::Write};
			s.bp = DEST_FBP << 5;
			s.bw = 1;
			s.psm = frame_psm;
			s.rect = GSVector4i(0, 0, 16, 16);
			s.seed = 77;
			return s;
		}

		static std::vector<Step> Concat(std::initializer_list<std::vector<Step>> parts)
		{
			std::vector<Step> out;
			for (const auto& p : parts)
				out.insert(out.end(), p.begin(), p.end());
			return out;
		}

		void ExpectSameAsMemoOff(const Result& off, const Result& on)
		{
			ASSERT_EQ(off.vm.size(), on.vm.size());
			EXPECT_EQ(DifferingBytes(off.vm, on.vm), 0u) << "local memory differs from the run without the memo";
			EXPECT_EQ(off.device_draws, on.device_draws) << "the device was handed other draws";
			EXPECT_EQ(off.stats.hit_draws, 0u);
		}
	};
} // namespace

// The second time a burst runs from the same inputs, every draw is skipped, and memory ends as it does with
// every copy run.
TEST_F(GSBurstMemoDraw, ARepeatedBurstIsSkippedAndLeavesTheSameMemory)
{
	const std::vector<u8> vm = RandomMemory(1);
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination()}, Burst(), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u);
	EXPECT_EQ(on.stats.sequences, 1u);
	EXPECT_EQ(on.stats.copy_draws, 4u);
}

// A skipped draw invalidates the texture cache as its copy would: a texture made from the burst's destination is
// looked up and uploaded after a skipped burst exactly as after the copies. The second burst finds its destination
// holding the first one's output and is recorded; the third finds the same, after a draw has made a texture of the
// destination, and is skipped.
TEST_F(GSBurstMemoDraw, ASkippedBurstInvalidatesTheTexturesItWrites)
{
	const std::vector<u8> vm = RandomMemory(9);
	const std::vector<Step> script = Concat({Burst(), {Step{Step::Consume}}, Burst(), {Step{Step::Consume}}, Burst(),
		{Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u);
	EXPECT_GT(off.texture_cache[1] + off.texture_cache[3], 0.0) << "the consumer never looked a texture up";
	const char* names[] = {"source hits", "source misses", "hash cache hits", "hash cache misses", "texels uploaded"};
	for (size_t i = 0; i < off.texture_cache.size(); i++)
		EXPECT_EQ(on.texture_cache[i], off.texture_cache[i]) << names[i];
}

// A burst that begins right after a skipped one, with no other draw between, checks its keys against what the
// skipped burst wrote: here burst B copies from the destination burst A writes. B is recorded first, reading the
// destination's restored bytes; later A is skipped (its own key check hashes the destination's blocks first) and B
// follows at once. B must find the bytes A left, not the hashes from before them, and so run its copies.
TEST_F(GSBurstMemoDraw, ABurstAfterASkippedOneSeesWhatItWrote)
{
	const std::vector<u8> vm = RandomMemory(10);
	constexpr u32 OUT_FBP = 0x60;
	Step restore_out = RestoreDestination();
	restore_out.bp = OUT_FBP << 5;
	restore_out.seed = 78;
	const std::vector<Step> b_burst = Burst(PSMCT32, PSMCT32, OUT_FBP, DEST_FBP << 5);
	const std::vector<Step> script = Concat({{RestoreDestination(), restore_out}, b_burst, {Step{Step::Consume}},
		{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination(), restore_out}, Burst(), b_burst, {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u) << "A, the second time, and not B";
	EXPECT_EQ(on.stats.stop_key, 1u) << "B turned away by the bytes A wrote";
}

// At the copy road's default level it takes only textured sprites, so the memo leaves every other draw alone before
// looking anything up or keeping its vertices; a burst of textured sprites is still skipped there.
TEST_F(GSBurstMemoDraw, AtLevelZeroABurstOfTexturedSpritesIsStillSkipped)
{
	GSConfig.UserHacks_CPUSpriteRenderLevel = 0;
	const std::vector<u8> vm = RandomMemory(11);
	Step fill{Step::Copy};
	fill.sprites = {{0, 0, 8, 8, 0, 0}};
	fill.tme = false;
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination()}, Burst(), {Step{Step::Consume}, fill}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u);
	EXPECT_EQ(on.stats.not_copy_road, 3u) << "the two wide draws that sample the destination, and the narrow untextured fill";
}

// A palette block copy (an 8-bit texture into a 32-bit frame): the palette is part of what a draw read.
TEST_F(GSBurstMemoDraw, APalettedBurstIsSkippedWhenItsPaletteRepeats)
{
	const std::vector<u8> vm = RandomMemory(2);
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(PSMT8), {Step{Step::Consume}},
		{RestoreDestination()}, Burst(PSMT8), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u);
}

// A shape the road keeps drawing with a block copy stops being recorded: after enough of them ran in a row, a
// burst that repeats from the same inputs is not skipped. The masked texel copy below is not a block copy, and a
// burst of it is recorded however long it runs.
TEST_F(GSBurstMemoDraw, AShapeTheRoadKeepsBlockCopyingIsNotRecorded)
{
	const std::vector<u8> vm = RandomMemory(11);
	std::vector<Step> script;
	// Five bursts, each onto a destination holding other bytes, so every draw runs: 20 palette block copies of the
	// same shape in a row.
	for (u32 i = 0; i < 5; i++)
	{
		Step other = RestoreDestination();
		other.seed = 100 + i;
		script = Concat({script, {other}, Burst(PSMT8), {Step{Step::Consume}}});
	}
	script = Concat({script, {RestoreDestination()}, Burst(PSMT8), {Step{Step::Consume}}, {RestoreDestination()}, Burst(PSMT8),
		{Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_GE(on.stats.not_recorded_block_copy, 4u);
	EXPECT_EQ(on.stats.hit_draws, 0u);

	std::vector<Step> masked;
	for (u32 i = 0; i < 5; i++)
	{
		Step other = RestoreDestination(PSMCT16);
		other.seed = 100 + i;
		masked = Concat({masked, {other}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}}});
	}
	masked = Concat({masked, {RestoreDestination(PSMCT16)}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}}, {RestoreDestination(PSMCT16)},
		Burst(PSMT8, PSMCT16), {Step{Step::Consume}}});
	const Result moff = Run(masked, vm, false);
	const Result mon = Run(masked, vm, true);
	ExpectSameAsMemoOff(moff, mon);
	EXPECT_EQ(mon.stats.not_recorded_block_copy, 0u);
	EXPECT_EQ(mon.stats.hit_draws, 4u);
}

// A long run of draws the gate passes over puts the memo to rest (on Call of Duty 3 nothing else happens all frame),
// and the first copy of a shape the gate does not pass over wakes it: that burst is still recorded and skipped.
TEST_F(GSBurstMemoDraw, TheMemoRestsThroughBlockCopiesAndWakesForAnotherShape)
{
	const std::vector<u8> vm = RandomMemory(12);
	std::vector<Step> script;
	for (u32 i = 0; i < 280; i++)
		script = Concat({script, Burst(PSMT8)});
	script = Concat({script, {Step{Step::Consume}}, {RestoreDestination(PSMCT16)}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}},
		{RestoreDestination(PSMCT16)}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_GE(on.stats.rests, 1u) << "1,120 block copies in a row";
	EXPECT_EQ(on.stats.hit_draws, 4u) << "the masked burst, the second time";
}

// A new palette loaded between the bursts: nothing is skipped.
TEST_F(GSBurstMemoDraw, ANewPaletteRunsTheCopies)
{
	const std::vector<u8> vm = RandomMemory(3);
	Step palette{Step::Write};
	palette.bp = CLUT_CBP;
	palette.bw = 1;
	palette.psm = PSMCT32;
	palette.rect = GSVector4i(0, 0, 16, 16);
	palette.seed = 5;
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(PSMT8), {Step{Step::Consume}},
		{RestoreDestination(), palette}, Burst(PSMT8), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 0u);
	EXPECT_GE(on.stats.stop_key, 1u);
}

// A masked texel copy (an 8-bit texture into a 16-bit frame, Spider-Man 3's kernel).
TEST_F(GSBurstMemoDraw, AMaskedTexelCopyBurstIsSkipped)
{
	const std::vector<u8> vm = RandomMemory(4);
	const std::vector<Step> script = Concat({{RestoreDestination(PSMCT16)}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}},
		{RestoreDestination(PSMCT16)}, Burst(PSMT8, PSMCT16), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 4u);
}

// Texels the third draw reads change between the bursts: the first two are skipped, the rest run.
TEST_F(GSBurstMemoDraw, ChangedSourceTexelsRunTheDrawsThatReadThem)
{
	const std::vector<u8> vm = RandomMemory(6);
	Step texels{Step::Write};
	texels.bp = TEX_TBP;
	texels.bw = 1;
	texels.psm = PSMCT32;
	texels.rect = GSVector4i(24, 24, 28, 28);
	texels.seed = 9;
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination(), texels}, Burst(), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 2u);
	EXPECT_GE(on.stats.stop_key, 1u);
}

// The destination does not hold what it held when the burst was recorded: the draws that first touch it
// run.
TEST_F(GSBurstMemoDraw, AnotherDestinationBeforeTheBurstRunsTheCopies)
{
	const std::vector<u8> vm = RandomMemory(7);
	Step other = RestoreDestination();
	other.seed = 78;
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{other}, Burst(), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 0u);
}

// A write lands on a block the burst already wrote, between two of its draws: the rest of the burst runs.
TEST_F(GSBurstMemoDraw, AWriteIntoTheBurstStopsTheSkip)
{
	const std::vector<u8> vm = RandomMemory(8);
	Step into{Step::Write};
	into.bp = DEST_FBP << 5;
	into.bw = 1;
	into.psm = PSMCT32;
	into.rect = GSVector4i(0, 0, 2, 2); // block 0, which draw 0 wrote and draw 2 writes again
	into.seed = 11;
	const std::vector<Step> first = Burst();
	std::vector<Step> second = Burst();
	second.insert(second.begin() + 2, into);
	const std::vector<Step> script = Concat({{RestoreDestination()}, first, {Step{Step::Consume}},
		{RestoreDestination()}, second, {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 2u);
	EXPECT_EQ(on.stats.stop_write, 1u);
}

// A target appears at the burst's texture: the copy road would weigh it, so nothing is skipped.
TEST_F(GSBurstMemoDraw, ATargetAtTheTextureRunsTheCopies)
{
	const std::vector<u8> vm = RandomMemory(9);
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination(), Step{Step::TargetAtTexture}}, Burst(), {Step{Step::Consume}}});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true);
	ExpectSameAsMemoOff(off, on);
	EXPECT_EQ(on.stats.hit_draws, 0u);
	EXPECT_GE(on.stats.stop_route, 1u);
}

// The control: with a wrong byte written into every block a skipped draw writes, memory differs. The
// comparison the other tests make sees a skip that writes the wrong bytes.
TEST_F(GSBurstMemoDraw, TheControlCorruptionIsSeen)
{
	const std::vector<u8> vm = RandomMemory(10);
	const std::vector<Step> script = Concat({{RestoreDestination()}, Burst(), {Step{Step::Consume}},
		{RestoreDestination()}, Burst()});
	const Result off = Run(script, vm, false);
	const Result on = Run(script, vm, true, true);
	EXPECT_EQ(on.stats.hit_draws, 4u);
	EXPECT_GT(DifferingBytes(off.vm, on.vm), 0u);
}
