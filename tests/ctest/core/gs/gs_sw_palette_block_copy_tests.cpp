// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The palette block copy and the masked texel copy on the CPU sprite road must write exactly what the
// rasterizer writes.
//
// The palette block copy takes a paletted sprite draw into a 32-bit frame; the masked texel copy takes
// one into a 16-bit frame under any write mask, reading the texels straight from local memory rather
// than from the rasterizer's snapshot of the texture. Both are counted by SwPaletteBlockCopies.
//
// Each case is run twice through a real GSRendererHW, from the same local memory and the same GIF
// packets: once with the copy allowed and once with it switched off, so every draw goes through the
// rasterizer and its scanline. Local memory is then compared byte for byte, all 4 MB of it. The
// cases are random: mostly inside the shape the copy accepts (paletted UV sprites, one texel per
// pixel), with every register the copy must refuse on also varied, so refusals are compared too.
// A count of the draws the copy took proves the comparison exercised it.
//
// Built twice. In gs_vertex_tests there is no code memory, so the reference is the rasterizer's
// C++ scanline. In gs_sw_palette_block_copy_jit_tests (GS_PALETTE_COPY_TESTS_JIT) the process
// reserves code memory first, so the reference is the generated scanline games run.

#include "gs_hw_draw_harness.h"

#include "GS/GSPerfMon.h"
#include "Memory.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <cinttypes>
#include <cstdio>
#include <random>
#include <string>
#include <cstring>

using namespace GSHWDrawHarness;

namespace
{
	// The write masks Spider-Man 3's texture expansion draws under (FRAME.FBMSK).
	constexpr u32 sm3_masks[6] = {0xffffff80u, 0xffffc07fu, 0xffe03fffu, 0x001fffffu, 0xffffc000u, 0x00003fffu};

	bool IsFrame16(u32 psm)
	{
		return psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S;
	}

	struct Sprite
	{
		int x0, y0, x1, y1; // raw sixteenths, XYOFFSET included
		int u0, v0, u1, v1; // raw sixteenths
	};

	struct Draw
	{
		GIFRegTEX0 tex0;
		GIFRegTEX1 tex1;
		GIFRegMIPTBP1 miptbp1;
		GIFRegMIPTBP2 miptbp2;
		GIFRegCLAMP clamp;
		GIFRegTEXA texa;
		GIFRegFRAME frame;
		GIFRegTEST test;
		GIFRegALPHA alpha;
		GIFRegXYOFFSET xyoffset;
		GIFRegSCISSOR scissor;
		GIFRegCOLCLAMP colclamp;
		GIFRegDTHE dthe;
		GIFRegFBA fba;
		GIFRegSCANMSK scanmsk;
		u32 zmsk = 1; ///< ZBUF.ZMSK: set, so the draw writes no depth
		GIFRegRGBAQ rgbaq;
		GIFRegPRIM prim;
		std::vector<Sprite> sprites;
		/// With FST 0: sprite UVs are sent as the ST that converts back to them, times Q. A nonzero
		/// nudge, in texels, moves S off the sixteenth grid. The GS rounds ST down to a precision
		/// set by Q's exponent before any renderer sees it, so a nudge below that precision is lost.
		float st_nudge = 0.0f;
		/// With FST 1 and a nonnegative value: the alpha of each sprite's first and second vertex, which the
		/// scanline takes the flat colour's alpha from the second of.
		int alpha0 = -1, alpha1 = -1;
		bool must_refuse = false; ///< carries a change the copy has to send to the rasterizer
	};

	struct Case
	{
		u32 seed;
		std::vector<Draw> draws;
		/// When not empty, local memory starts as this cycle of words and not as random bytes.
		std::vector<u32> fill;
	};

	struct ArmResult
	{
		std::vector<u8> vm;
		std::vector<u64> copies; ///< per draw
		u64 total = 0;
		double unswizzled = 0; ///< bytes the rasterizer's texture snapshot converted, over the case
	};

	class GSSwPaletteBlockCopy : public Fixture
	{
	protected:
#ifdef GS_PALETTE_COPY_TESTS_JIT
		// The host memory map carries the code arena the rasterizer compiles into.
		static void SetUpTestSuite() { SysMemory::Allocate(); }
		static void TearDownTestSuite() { SysMemory::Release(); }
#endif

		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.UpscaleMultiplier = 1.0f;
			GSConfig.UserHacks_CPUSpriteRenderBW = 64; // admit every FBW
			GSConfig.UserHacks_CPUSpriteRenderLevel = 2; // as Spider-Man 3's GameDB entry: mipmapped and blended sprites too
			GSConfig.HWMipmap = true; // a draw is mipmapped only when the renderer honours mipmaps

			// The vertex conversion table the sprite road reads; GS start-up fills it.
			GSVertexSW::InitStatic();

#ifdef GS_PALETTE_COPY_TESTS_JIT
			ASSERT_TRUE(SysMemory::HasCodeMemory()) << "no code memory, so the reference would not be generated code";
#else
			ASSERT_FALSE(SysMemory::HasCodeMemory());
#endif
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

		static void Send(GSState& gs, const Draw& d)
		{
			Packet p;
			GIFReg r;

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);
			r.XYOFFSET = d.xyoffset;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);
			r.SCISSOR = d.scissor;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);
			r.FRAME = d.frame;
			p.Reg(GIF_A_D_REG_FRAME_1, r);
			r.U64 = 0;
			r.ZBUF.ZBP = 0x1C0;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = d.zmsk;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);
			r.TEST = d.test;
			p.Reg(GIF_A_D_REG_TEST_1, r);
			r.ALPHA = d.alpha;
			p.Reg(GIF_A_D_REG_ALPHA_1, r);
			r.COLCLAMP = d.colclamp;
			p.Reg(GIF_A_D_REG_COLCLAMP, r);
			r.DTHE = d.dthe;
			p.Reg(GIF_A_D_REG_DTHE, r);
			r.FBA = d.fba;
			p.Reg(GIF_A_D_REG_FBA_1, r);
			r.SCANMSK = d.scanmsk;
			p.Reg(GIF_A_D_REG_SCANMSK, r);
			r.TEXA = d.texa;
			p.Reg(GIF_A_D_REG_TEXA, r);
			r.TEX1 = d.tex1;
			p.Reg(GIF_A_D_REG_TEX1_1, r);
			r.U64 = 0;
			r.MIPTBP1 = d.miptbp1;
			p.Reg(GIF_A_D_REG_MIPTBP1_1, r);
			r.U64 = 0;
			r.MIPTBP2 = d.miptbp2;
			p.Reg(GIF_A_D_REG_MIPTBP2_1, r);
			r.CLAMP = d.clamp;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
			r.TEX0 = d.tex0;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			r.RGBAQ = d.rgbaq;
			p.Reg(GIF_A_D_REG_RGBAQ, r);

			if (d.prim.FST)
			{
				for (const Sprite& s : d.sprites)
				{
					if (d.alpha0 >= 0 || d.alpha1 >= 0)
					{
						p.VertexRGBA(s.x0, s.y0, 0, s.u0, s.v0, d.rgbaq.R, d.rgbaq.G, d.rgbaq.B, static_cast<u8>(d.alpha0 >= 0 ? d.alpha0 : d.rgbaq.A));
						p.VertexRGBA(s.x1, s.y1, 0, s.u1, s.v1, d.rgbaq.R, d.rgbaq.G, d.rgbaq.B, static_cast<u8>(d.alpha1 >= 0 ? d.alpha1 : d.rgbaq.A));
					}
					else
					{
						p.Vertex(s.x0, s.y0, 0, s.u0, s.v0);
						p.Vertex(s.x1, s.y1, 0, s.u1, s.v1);
					}
				}
			}
			else
			{
				// S = U / (16 * width) * Q, which the vertex conversion turns back into U sixteenths.
				const float q = d.rgbaq.Q;
				const float su = q / static_cast<float>(16 << d.tex0.TW);
				const float sv = q / static_cast<float>(16 << d.tex0.TH);
				for (const Sprite& s : d.sprites)
				{
					p.VertexST(s.x0, s.y0, 0, (s.u0 + 16 * d.st_nudge) * su, s.v0 * sv, q);
					p.VertexST(s.x1, s.y1, 0, (s.u1 + 16 * d.st_nudge) * su, s.v1 * sv, q);
				}
			}

			p.Send(gs, d.prim);
		}

		ArmResult RunArm(const Case& c, const std::vector<u8>& vm, bool copy)
		{
			ArmResult out;
			BringUp();
			if (HasFatalFailure())
				return out;

			m_gs->GetSwPrimState().palette_block_copy = copy;
			std::memcpy(m_gs->m_mem.m_vm8, vm.data(), vm.size());
			m_gs->m_mem.MarkAllPagesWritten();

			const double unswizzle_before = g_perfmon.GetCounter(GSPerfMon::Unswizzle);
			for (const Draw& d : c.draws)
			{
				const double before = g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies);
				Send(*m_gs, d);
				out.copies.push_back(static_cast<u64>(g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies) - before));
				out.total += out.copies.back();
			}

			out.unswizzled = g_perfmon.GetCounter(GSPerfMon::Unswizzle) - unswizzle_before;
			out.vm.assign(m_gs->m_mem.m_vm8, m_gs->m_mem.m_vm8 + vm.size());
			Destroy();
			return out;
		}

		static std::vector<u8> RandomMemory(u32 seed)
		{
			std::vector<u8> vm(VM_SIZE);
			std::mt19937 rng(seed ^ 0x9e3779b9u);
			for (size_t i = 0; i < vm.size(); i += 4)
			{
				const u32 w = rng();
				std::memcpy(&vm[i], &w, 4);
			}
			return vm;
		}

		/// The texture bytes the rasterizer's snapshot converted in the last Compare(), reference arm and copy arm.
		double m_ref_unswizzled = 0;
		double m_got_unswizzled = 0;
		/// The copies each draw of the last Compare() took, copy arm.
		std::vector<u64> m_got_copies;

		// Runs both arms and compares local memory. Returns the draws the copy took.
		u64 Compare(const Case& c)
		{
			std::vector<u8> vm = RandomMemory(c.seed);
			for (size_t i = 0; !c.fill.empty() && i < vm.size(); i += 4)
				std::memcpy(&vm[i], &c.fill[(i / 4) % c.fill.size()], 4);
			const ArmResult ref = RunArm(c, vm, false);
			const ArmResult got = RunArm(c, vm, true);
			m_ref_unswizzled = ref.unswizzled;
			m_got_unswizzled = got.unswizzled;
			m_got_copies = got.copies;

			EXPECT_EQ(ref.total, 0u) << "case " << c.seed << ": the switch did not keep the copy off";
			for (size_t i = 0; i < c.draws.size() && i < got.copies.size(); i++)
			{
				if (c.draws[i].must_refuse)
					EXPECT_EQ(got.copies[i], 0u) << "case " << c.seed << " draw " << i << ": the copy took a draw it must refuse\n"
												 << Describe(c);
			}

			size_t first = vm.size();
			size_t count = 0;
			for (size_t i = 0; i < vm.size(); i++)
			{
				if (ref.vm[i] != got.vm[i])
				{
					if (first == vm.size())
						first = i;
					count++;
				}
			}

			EXPECT_EQ(count, 0u) << "case " << c.seed << ": " << count << " bytes differ, first at 0x" << std::hex
								 << first << " (rasterizer " << +ref.vm[first < vm.size() ? first : 0] << ", copy "
								 << +got.vm[first < vm.size() ? first : 0] << ")" << std::dec << "\n"
								 << Describe(c);
			return got.total;
		}

		static std::string Describe(const Case& c)
		{
			std::string s;
			char buf[512];
			for (const Draw& d : c.draws)
			{
				std::snprintf(buf, sizeof(buf),
					"  TEX0 %016" PRIx64 " TEX1 %016" PRIx64 " MIPTBP1 %016" PRIx64 " MIPTBP2 %016" PRIx64 " CLAMP %016" PRIx64 " FRAME %016" PRIx64
					" TEST %016" PRIx64 " ALPHA %016" PRIx64 " XYOFFSET %016" PRIx64 " SCISSOR %016" PRIx64
					" PRIM %03x COLCLAMP %d DTHE %d FBA %d SCANMSK %d sprites %zu\n",
					d.tex0.U64, d.tex1.U64, d.miptbp1.U64, d.miptbp2.U64, d.clamp.U64, d.frame.U64, d.test.U64, d.alpha.U64, d.xyoffset.U64,
					d.scissor.U64, static_cast<u32>(d.prim.U64), d.colclamp.CLAMP, d.dthe.DTHE, d.fba.FBA,
					d.scanmsk.MSK, d.sprites.size());
				s += buf;
				for (const Sprite& sp : d.sprites)
				{
					std::snprintf(buf, sizeof(buf), "    xy %d,%d-%d,%d uv %d,%d-%d,%d\n", sp.x0, sp.y0, sp.x1, sp.y1,
						sp.u0, sp.v0, sp.u1, sp.v1);
					s += buf;
				}
			}
			return s;
		}

		// A draw in the shape the copy is for: an 8x8 block of a 4-bit texture through a 32-bit
		// palette into a 32-bit frame, texel for pixel, half a texel into each texel.
		static Draw Tile(int csa)
		{
			Draw d = {};
			d.tex0.TBP0 = 0x2000;
			d.tex0.TBW = 1;
			d.tex0.PSM = PSMT4;
			d.tex0.TW = 5;
			d.tex0.TH = 5;
			d.tex0.TCC = 1;
			d.tex0.TFX = TFX_DECAL;
			d.tex0.CBP = 0x3000;
			d.tex0.CPSM = PSMCT32;
			d.tex0.CSA = csa;
			d.tex0.CLD = 1;
			d.frame.FBP = 0x40;
			d.frame.FBW = 32;
			d.frame.PSM = PSMCT32;
			d.test.ZTE = 1;
			d.test.ZTST = ZTST_ALWAYS;
			d.xyoffset.OFX = 2048 * 16;
			d.xyoffset.OFY = 2048 * 16;
			d.scissor.SCAX1 = 2047;
			d.scissor.SCAY1 = 2047;
			d.rgbaq.R = d.rgbaq.G = d.rgbaq.B = d.rgbaq.A = 0x80;
			d.rgbaq.Q = 1.0f;
			d.prim.PRIM = GS_SPRITE;
			d.prim.TME = 1;
			d.prim.FST = 1;
			d.sprites.push_back({(2048 + 16) * 16, (2048 + 8) * 16, (2048 + 24) * 16, (2048 + 16) * 16,
				8 * 16 + 8, 16 * 16 + 8, 16 * 16 + 8, 24 * 16 + 8});
			return d;
		}

		// Most draws are in the shape the copy takes, with everything it must get right varied:
		// texture and palette formats, sub-palette, palette storage mode, texture size and buffer
		// width, wrap mode, TEXA, colour clamp, offsets, scissor, sub-texel positions and texel
		// phases, sprite counts, sprites overlapping each other and the texture under the frame.
		// The rest carry one change the copy must refuse. A case is up to three draws in a row, so
		// palette caching and the rasterizer's persistent texture buffer carry across draws.
		static Case RandomCase(u32 seed)
		{
			std::mt19937 rng(seed);
			const auto pick = [&rng](int n) { return static_cast<int>(rng() % static_cast<u32>(n)); };
			const auto chance = [&pick](int percent) { return pick(100) < percent; };

			static constexpr u32 tex_psms[] = {PSMT4, PSMT8, PSMT8H, PSMT4HL, PSMT4HH};
			static constexpr u32 clut_psms[] = {PSMCT32, PSMCT16, PSMCT16S};
			static constexpr u32 frame_psms[] = {PSMCT24, PSMCT16, PSMCT16S};
			static constexpr u32 frame16_psms[] = {PSMCT16, PSMCT16S, PSMZ16, PSMZ16S};

			Case c;
			c.seed = seed;
			const int ndraws = 1 + pick(3);

			for (int n = 0; n < ndraws; n++)
			{
				Draw d = {};

				d.tex0.TBP0 = pick(0x4000);
				d.tex0.TBW = 1 + pick(16);
				d.tex0.PSM = tex_psms[pick(5)];
				d.tex0.TW = chance(90) ? 3 + pick(8) : pick(3);
				d.tex0.TH = chance(90) ? 3 + pick(8) : pick(3);
				// Without TCC the copy writes the vertex alpha; modulate is taken when the vertex colour
				// makes it decal, and otherwise refused.
				d.tex0.TCC = chance(75) ? 1 : 0;
				d.tex0.TFX = chance(75) ? TFX_DECAL : TFX_MODULATE;
				d.tex0.CBP = pick(0x4000);
				d.tex0.CPSM = clut_psms[pick(3)];
				d.tex0.CSM = chance(80) ? 0 : 1;
				d.tex0.CSA = pick(32);
				d.tex0.CLD = (n == 0 || chance(50)) ? 1 : pick(6);

				d.clamp.WMS = pick(2);
				d.clamp.WMT = pick(2);
				d.clamp.MINU = pick(1024);
				d.clamp.MAXU = pick(1024);
				d.clamp.MINV = pick(1024);
				d.clamp.MAXV = pick(1024);

				d.texa.TA0 = pick(256);
				d.texa.AEM = pick(2);
				d.texa.TA1 = pick(256);

				d.frame.FBP = pick(512);
				d.frame.FBW = chance(30) ? 32 : 1 + pick(32);
				// A 16-bit frame is taken under any write mask, the six Spider-Man 3 uses or a random one.
				const bool frame16 = chance(45);
				d.frame.PSM = frame16 ? frame16_psms[pick(4)] : chance(90) ? PSMCT32 : PSMZ32;
				if (frame16 && chance(80))
					d.frame.FBMSK = chance(50) ? sm3_masks[pick(6)] : static_cast<u32>(rng());
				if (frame16 && chance(10))
					d.fba.FBA = 1;

				// The texture sometimes sits under the frame, so a draw reads what it writes.
				if (chance(15))
					d.tex0.TBP0 = d.frame.FBP * 32 + pick(64);

				d.test.ZTE = 1;
				d.test.ZTST = ZTST_ALWAYS;
				d.test.DATM = pick(2);

				d.alpha.A = pick(3);
				d.alpha.B = pick(3);
				d.alpha.C = pick(3);
				d.alpha.D = pick(3);
				d.alpha.FIX = pick(256);

				d.xyoffset.OFX = chance(70) ? 2048 * 16 : pick(4096 * 16);
				d.xyoffset.OFY = chance(70) ? 2048 * 16 : pick(4096 * 16);

				const int fb_width = std::max<int>(d.frame.FBW * 64, 64);
				if (chance(70))
				{
					d.scissor.SCAX1 = fb_width - 1;
					d.scissor.SCAY1 = 1023;
				}
				else
				{
					d.scissor.SCAX0 = pick(256);
					d.scissor.SCAY0 = pick(256);
					d.scissor.SCAX1 = d.scissor.SCAX0 + pick(512);
					d.scissor.SCAY1 = d.scissor.SCAY0 + pick(512);
#ifndef GS_PALETTE_COPY_TESTS_JIT
					// The C++ scanline asserts on a notest span the scissor cuts off the vector
					// grid, which the rasterizer can hand it (DrawPaletteBlocks explains how), so
					// that case is left to the generated-code build.
					d.scissor.SCAX0 &= ~3;
					d.scissor.SCAX1 |= 3;
#endif
				}

				d.colclamp.CLAMP = pick(2);
				d.scanmsk.MSK = pick(2);

				d.rgbaq.R = chance(80) ? 0x80 : pick(256);
				d.rgbaq.G = chance(80) ? 0x80 : pick(256);
				d.rgbaq.B = chance(80) ? 0x80 : pick(256);
				d.rgbaq.A = chance(80) ? 0x80 : pick(256);
				d.rgbaq.Q = 1.0f;

				d.prim.PRIM = GS_SPRITE;
				d.prim.TME = 1;
				d.prim.FST = 1;

				// A mipmapped draw samples one level, and steps 2^level texels of the base texture per pixel.
				// The level is the one SetupMipLod() settles on; whether the copy takes the draw is left to
				// the selector (a filtered or trilinear lookup, or a level of detail from Q, goes to the rasterizer).
				int level = 0;
				const bool mipped = chance(30);
				if (mipped)
				{
					d.tex1.MXL = 1 + pick(6);
					d.tex1.MMIN = chance(85) ? 2 : pick(6);
					d.tex1.LCM = chance(85) ? 1 : 0;
					d.tex1.K = pick(d.tex1.MXL + 1) * 16 + (chance(70) ? 0 : pick(17) - 8);
					d.miptbp1.TBP1 = pick(0x4000);
					d.miptbp1.TBW1 = 1 + pick(16);
					d.miptbp1.TBP2 = pick(0x4000);
					d.miptbp1.TBW2 = 1 + pick(16);
					d.miptbp1.TBP3 = pick(0x4000);
					d.miptbp1.TBW3 = 1 + pick(16);
					d.miptbp2.TBP4 = pick(0x4000);
					d.miptbp2.TBW4 = 1 + pick(16);
					d.miptbp2.TBP5 = pick(0x4000);
					d.miptbp2.TBW5 = 1 + pick(16);
					d.miptbp2.TBP6 = pick(0x4000);
					d.miptbp2.TBW6 = 1 + pick(16);

					const int k = static_cast<int>(d.tex1.K) << 12;
					const int mxl = std::min<int>(d.tex1.MXL, 6) << 16;
					const int lod = std::max(std::min(k, mxl), 0);
					level = ((lod + 0x8000) & 0xffff0000) >> 16;
				}

				const int tw = 1 << d.tex0.TW;
				const int th = 1 << d.tex0.TH;
				const int nsprites = 1 + (chance(50) ? 0 : pick(20));
				const int span_x = std::min(fb_width, 700);

				// One change that must send the draw to the rasterizer, on about a third of draws.
				const int perturb = chance(35) ? pick(20) : -1;

				// A change to the first sprite has to reach the rasterizer's view of it, so that
				// sprite is kept whole: unclipped, and nowhere near the coordinate limits.
				const bool sprite_perturb = perturb >= 0 && perturb <= 4;
				if (sprite_perturb)
				{
					d.xyoffset.OFX = 2048 * 16;
					d.xyoffset.OFY = 2048 * 16;
					d.scissor.SCAX0 = 0;
					d.scissor.SCAY0 = 0;
					d.scissor.SCAX1 = fb_width - 1;
					d.scissor.SCAY1 = 1023;
				}

				for (int i = 0; i < nsprites; i++)
				{
					int w = 1 << pick(7);
					int h = 1 << pick(7);

					const int ox = static_cast<int>(d.xyoffset.OFX);
					const int oy = static_cast<int>(d.xyoffset.OFY);
					const int fx = chance(80) ? 0 : pick(16);
					const int fy = chance(80) ? 0 : pick(16);
					const int px = pick(std::max(span_x - 16, 1)) - 8;
					const int py = pick(500) - 8;

					if (perturb == 0 && i == 0)
						w = 3 + 2 * pick(30); // odd, so not a power of two
					if (perturb == 1 && i == 0)
						h = 3 + 2 * pick(30);

					const bool whole = sprite_perturb && i == 0;

					Sprite s;
					s.x0 = ox + (whole ? pick(64) : px) * 16 + fx;
					s.y0 = oy + (whole ? pick(64) : py) * 16 + fy;
					s.x1 = s.x0 + w * 16;
					s.y1 = s.y0 + h * 16;

					const int ufrac = chance(60) ? 8 : pick(16);
					const int vfrac = chance(60) ? 8 : pick(16);
					s.u0 = pick(whole ? 64 : chance(85) ? tw : 1024) * 16 + ufrac;
					s.v0 = pick(whole ? 64 : chance(85) ? th : 1024) * 16 + vfrac;

					int du = (w * 16) << level;
					int dv = (h * 16) << level;
					if (perturb == 2 && i == 0)
						du += pick(2) ? 16 : -1;
					if (perturb == 3 && i == 0)
						dv *= 2;
					s.u1 = s.u0 + du;
					s.v1 = s.v0 + dv;

					// Both corners swapped keeps one texel per pixel; one alone mirrors the texture.
					if (chance(10))
					{
						std::swap(s.x0, s.x1);
						std::swap(s.u0, s.u1);
					}
					if (chance(10))
					{
						std::swap(s.y0, s.y1);
						std::swap(s.v0, s.v1);
					}
					if (perturb == 4 && i == 0)
						std::swap(s.u0, s.u1);

					s.x0 = std::clamp(s.x0, 0, 0xFFFF);
					s.x1 = std::clamp(s.x1, 0, 0xFFFF);
					s.y0 = std::clamp(s.y0, 0, 0xFFFF);
					s.y1 = std::clamp(s.y1, 0, 0xFFFF);
					s.u0 = std::clamp(s.u0, 0, 0x3FFF);
					s.u1 = std::clamp(s.u1, 0, 0x3FFF);
					s.v0 = std::clamp(s.v0, 0, 0x3FFF);
					s.v1 = std::clamp(s.v1, 0, 0x3FFF);

					d.sprites.push_back(s);
				}

				// Some mipmapped draws send ST: divided by Q per pixel, so Q a power of two keeps it exact. A level
				// of detail taken from Q needs ST (the rasterizer asserts on UV coordinates with one).
				if ((level > 0 && chance(50)) || (mipped && !d.tex1.LCM))
				{
					d.prim.FST = 0;
					d.rgbaq.Q = chance(75) ? static_cast<float>(1 << pick(5)) : 3.0f;
				}

				switch (perturb)
				{
					case 5: d.tex0.TFX = TFX_MODULATE; d.rgbaq.R = 0x80 ^ (1 + pick(127)); break;
					case 6: d.tex0.TFX = 2 + pick(2); break; // highlight, highlight2
					case 7: d.tex1.MMAG = 1; d.tex1.MMIN = 1; break;
					case 8: d.clamp.WMS = 2 + pick(2); break;
					case 9: d.clamp.WMT = 2 + pick(2); break;
					case 10: d.frame.PSM = frame_psms[pick(3)]; break;
					case 11: d.frame.FBMSK = static_cast<u32>(rng()) | 1; break;
					case 12: d.test.ATE = 1; d.test.ATST = pick(8); d.test.AREF = pick(256); d.test.AFAIL = pick(4); break;
					case 13: d.test.DATE = 1; break;
					case 14: d.prim.ABE = 1; break;
					case 15: d.prim.FGE = 1; break;
					case 16: d.dthe.DTHE = 1; break;
					case 17: d.fba.FBA = 1; break;
					case 18: d.scanmsk.MSK = 2 + pick(2); break;
					case 19:
						// ST that lands on the sixteenth grid is taken, at a Q of one or not. S nudged
						// off it is refused unless the GS's own ST rounding puts it back.
						d.prim.FST = 0;
						d.rgbaq.Q = chance(50) ? 1.0f : (chance(50) ? 2.0f : 0.5f);
						if (chance(50))
							d.st_nudge = 1.0f / (1 << (4 + pick(4)));
						break;
					default: break;
				}
				// An alpha test the renderer proves always passes, or a blend it proves opaque, leaves
				// the scanline the copy's own shape, so those two are compared but not required to
				// refuse.
				d.must_refuse = perturb >= 0 && perturb != 12 && perturb != 14 && perturb != 19;
				// A mask, a frame alpha bit and a 16-bit frame are what the masked texel copy takes. What it
				// refuses stays 24-bit, and any 32-bit frame under a mask or a forced alpha bit.
				const bool final16 = IsFrame16(d.frame.PSM);
				if (perturb == 10 && final16)
					d.must_refuse = false;
				if ((perturb == 11 || perturb == 17) && final16)
					d.must_refuse = false;

				c.draws.push_back(d);
			}

			return c;
		}
		// RandomCase() with the textures moved to 32-bit ones, varying the same registers. Most draws go to a
		// 32-bit frame with no mask, where the 32-bit texel copy applies; the rest keep the 16-bit and masked frames
		// and non-32-bit texture formats, which it must leave to the rasterizer. `shaped` says which draws are in
		// the copy's shape.
		static Case RandomCase32(u32 seed, std::vector<bool>& shaped)
		{
			Case c = RandomCase(seed);
			std::mt19937 rng(seed ^ 0x5bd1e995u);
			const auto pick = [&rng](int n) { return static_cast<int>(rng() % static_cast<u32>(n)); };

			for (Draw& d : c.draws)
			{
				const int r = pick(100);
				d.tex0.PSM = r < 82 ? PSMCT32 : r < 86 ? PSMCT24 : r < 90 ? PSMCT16 : r < 94 ? PSMCT16S : r < 97 ? PSMZ32 : PSMZ24;
				d.tex0.CBP = d.tex0.CPSM = d.tex0.CSA = 0;

				// A region clamp or repeat that names texels beyond a 32-bit texture makes the rasterizer's snapshot
				// read past its end, so the reference itself differs from run to run on those draws. The copy refuses
				// them and the directed test covers that with a region inside the texture; the sweep makes them plain draws.
				if (d.clamp.WMS > CLAMP_CLAMP || d.clamp.WMT > CLAMP_CLAMP)
				{
					d.clamp.WMS &= 1;
					d.clamp.WMT &= 1;
					d.must_refuse = false;
				}

				// A 16-bit frame without a perturbation becomes a 32-bit one most of the time, mask and all.
				if (IsFrame16(d.frame.PSM) && !d.must_refuse && pick(100) < 85)
				{
					d.frame.PSM = pick(10) ? PSMCT32 : PSMZ32;
					d.frame.FBMSK = pick(8) ? 0 : static_cast<u32>(rng());
					d.fba.FBA = 0;
				}

				// A bilinear lookup whose weights come out zero or a half is taken (the box filter), so the linear
				// filter is no longer a change the copy must refuse for a 32-bit texture.
			if (d.tex1.MMAG == 1 && d.tex1.MMIN == 1 && d.tex1.MXL == 0)
				d.must_refuse = false;

			const bool mipped = d.tex1.MXL != 0;
				shaped.push_back(d.tex0.PSM == PSMCT32 && (d.frame.PSM == PSMCT32 || d.frame.PSM == PSMZ32) && d.frame.FBMSK == 0 && !d.must_refuse && !mipped);
			}

			return c;
		}
		// RandomCase32() with the bilinear lookups the 32-bit texel copy takes: about nine draws in ten are made
		// linear, each axis steps one or two texels a pixel (sometimes three, which is refused), and the coordinates'
		// fractions are put in the window the copy takes for the step about five times in six, else left random.
		// `candidate` says which draws are in the copy's shape: a 32-bit texture, a plain 32-bit frame, a bilinear
		// lookup with the weights the copy takes.
		static Case RandomCase32Box(u32 seed, std::vector<bool>& candidate)
		{
			std::vector<bool> unused;
			Case c = RandomCase32(seed, unused);
			std::mt19937 rng(seed ^ 0x2545f491u);
			const auto pick = [&rng](int n) { return static_cast<int>(rng() % static_cast<u32>(n)); };

			for (Draw& d : c.draws)
			{
				const bool shape = d.tex0.PSM == PSMCT32 && (d.frame.PSM == PSMCT32 || d.frame.PSM == PSMZ32) && d.frame.FBMSK == 0 &&
								   !d.must_refuse && d.tex1.MXL == 0 && d.prim.FST == 1;
				bool taken_shape = shape;

				if (pick(10) != 0)
				{
					d.tex1.MMAG = 1;
					d.tex1.MMIN = 1;
					d.must_refuse = false;

					// A drawing offset on whole pixels most of the time, for the same reason as the sprites below.
					if (pick(8) != 0)
					{
						const int dx = static_cast<int>(d.xyoffset.OFX) & 15;
						const int dy = static_cast<int>(d.xyoffset.OFY) & 15;
						d.xyoffset.OFX -= dx;
						d.xyoffset.OFY -= dy;
						for (Sprite& sp : d.sprites)
						{
							sp.x0 -= dx;
							sp.x1 -= dx;
							sp.y0 -= dy;
							sp.y1 -= dy;
						}
					}

					const int sx = pick(10) == 0 ? 3 : 1 + pick(2);
					const int sy = pick(10) == 0 ? 3 : 1 + pick(2);
					const auto window = [](int step) { return step == 2 ? 0 : step == 1 ? 8 : -1; };

					for (Sprite& sp : d.sprites)
					{
						// Row and column order of the corners is the generator's; scale each extent by the step.
						const int du = sp.u1 - sp.u0;
						const int dv = sp.v1 - sp.v0;
						const int fu = pick(6) ? window(sx) : pick(16);
						const int fv = pick(6) ? window(sy) : pick(16);
						if (fu >= 0)
							sp.u0 = (sp.u0 & ~15) | fu;
						if (fv >= 0)
							sp.v0 = (sp.v0 & ~15) | fv;
						sp.u1 = std::clamp(sp.u0 + du * sx, 0, 0x3FFF);
						sp.v1 = std::clamp(sp.v0 + dv * sy, 0, 0x3FFF);

						// A sprite on whole pixels most of the time: a fraction in its position moves the coordinate
						// off the window the copy takes.
						if (pick(8) != 0)
						{
							const int dx = sp.x1 - sp.x0;
							const int dy = sp.y1 - sp.y0;
							sp.x0 &= ~15;
							sp.y0 &= ~15;
							sp.x1 = sp.x0 + dx;
							sp.y1 = sp.y0 + dy;
						}
					}

					taken_shape = shape && sx <= 2 && sy <= 2;
				}
				candidate.push_back(taken_shape);
			}
			return c;
		}
	};
} // namespace

TEST_F(GSSwPaletteBlockCopy, TakesTheTileDraw)
{
	Case c;
	c.seed = 1;
	c.draws.push_back(Tile(5));
	EXPECT_EQ(Compare(c), 1u);
}

// Consecutive tiles that change only the sub-palette, as Call of Duty's texture expansion draws them.
TEST_F(GSSwPaletteBlockCopy, TakesARunOfSubPaletteTiles)
{
	Case c;
	c.seed = 2;
	for (int csa = 0; csa < 16; csa++)
	{
		Draw d = Tile(csa);
		d.tex0.CLD = csa == 0 ? 1 : 0;
		const int x = (csa & 3) * 8;
		const int y = (csa >> 2) * 8;
		Sprite& s = d.sprites[0];
		s.x0 = (2048 + x) * 16;
		s.y0 = (2048 + y) * 16;
		s.x1 = s.x0 + 8 * 16;
		s.y1 = s.y0 + 8 * 16;
		s.u0 = x * 16 + 8;
		s.v0 = y * 16 + 8;
		s.u1 = s.u0 + 8 * 16;
		s.v1 = s.v0 + 8 * 16;
		c.draws.push_back(d);
	}
	EXPECT_EQ(Compare(c), 16u);
}

// The frame over the draw's own texture: the copy reads the texture as it was before the draw.
TEST_F(GSSwPaletteBlockCopy, ReadsItsOwnTextureAsItWasBeforeTheDraw)
{
	Case c;
	c.seed = 3;
	Draw d = Tile(0);
	d.tex0.PSM = PSMT8;
	d.tex0.TBP0 = d.frame.FBP * 32;
	d.tex0.TBW = 32;
	d.sprites.clear();
	for (int i = 0; i < 4; i++)
		d.sprites.push_back({(2048 + i * 4) * 16, 2048 * 16, (2048 + i * 4 + 8) * 16, (2048 + 8) * 16,
			(i * 2) * 16 + 8, 8, (i * 2 + 8) * 16 + 8, 8 * 16 + 8});
	c.draws.push_back(d);
	EXPECT_EQ(Compare(c), 1u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesAFrameMask)
{
	Case c;
	c.seed = 4;
	c.draws.push_back(Tile(1));
	c.draws.back().frame.FBMSK = 0x00FF0000;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesLinearFiltering)
{
	Case c;
	c.seed = 5;
	c.draws.push_back(Tile(1));
	c.draws.back().tex1.MMAG = 1;
	c.draws.back().tex1.MMIN = 1;
	EXPECT_EQ(Compare(c), 0u);
}

// A width that is not a power of two walks the coordinate a hair low past its first column.
TEST_F(GSSwPaletteBlockCopy, RefusesAnExtentThatIsNotAPowerOfTwo)
{
	Case c;
	c.seed = 6;
	c.draws.push_back(Tile(1));
	Sprite& s = c.draws.back().sprites[0];
	s.x1 += 4 * 16;
	s.u1 += 4 * 16;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesAScaledTexture)
{
	Case c;
	c.seed = 7;
	c.draws.push_back(Tile(1));
	c.draws.back().sprites[0].u1 += 8 * 16;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesAMirroredTexture)
{
	Case c;
	c.seed = 8;
	c.draws.push_back(Tile(1));
	std::swap(c.draws.back().sprites[0].u0, c.draws.back().sprites[0].u1);
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesFog)
{
	Case c;
	c.seed = 9;
	c.draws.push_back(Tile(1));
	c.draws.back().prim.FGE = 1;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesARegionClamp)
{
	Case c;
	c.seed = 10;
	c.draws.push_back(Tile(1));
	c.draws.back().clamp.WMS = CLAMP_REGION_CLAMP;
	c.draws.back().clamp.MAXU = 31;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, TakesTextureColourWithVertexAlpha)
{
	Case c;
	c.seed = 11;
	c.draws.push_back(Tile(1));
	c.draws.back().tex0.TCC = 0;
	c.draws.back().rgbaq.A = 0x37;
	EXPECT_EQ(Compare(c), 1u);
}

// Modulate by a colour of 0x80 is decal; by any other colour it is not.
TEST_F(GSSwPaletteBlockCopy, TakesModulateByHalfAndRefusesAnyOtherColour)
{
	Case c;
	c.seed = 15;
	c.draws.push_back(Tile(1));
	c.draws.back().tex0.TFX = TFX_MODULATE;
	c.draws.push_back(Tile(2));
	c.draws.back().tex0.TFX = TFX_MODULATE;
	c.draws.back().tex0.TCC = 0;
	c.draws.back().rgbaq.A = 0x11;
	EXPECT_EQ(Compare(c), 2u);

	Case r;
	r.seed = 16;
	r.draws.push_back(Tile(1));
	r.draws.back().tex0.TFX = TFX_MODULATE;
	r.draws.back().tex0.TCC = 0;
	r.draws.back().rgbaq.G = 0x7F;
	EXPECT_EQ(Compare(r), 0u);
}

// Jak 3 expands paletted character textures with ST sprites whose S and T land exactly on texels.
// Converted, they are the UV draw.
TEST_F(GSSwPaletteBlockCopy, TakesSTCoordinatesOnTheSixteenthGrid)
{
	Case c;
	c.seed = 13;
	Draw d = Tile(1);
	d.tex0.PSM = PSMT8;
	d.tex0.TW = 9;
	d.tex0.TH = 8;
	d.tex0.TBW = 8;
	d.tex0.TCC = 0;
	d.tex0.TFX = TFX_MODULATE;
	d.prim.FST = 0;
	d.sprites.clear();
	for (int i = 0; i < 16; i++)
	{
		const int x = (i & 3) * 32;
		const int y = (i >> 2) * 32;
		d.sprites.push_back({(2048 + x) * 16, (2048 + y) * 16, (2048 + x + 32) * 16, (2048 + y + 32) * 16,
			(64 + x) * 16, (128 + y) * 16, (64 + x + 32) * 16, (128 + y + 32) * 16});
	}
	c.draws.push_back(d);
	d.rgbaq.Q = 2.0f;
	c.draws.push_back(d);
	EXPECT_EQ(Compare(c), 2u);
}

// ST whose conversion leaves a fraction below a sixteenth of a texel is not held to the grid.
TEST_F(GSSwPaletteBlockCopy, RefusesSTCoordinatesOffTheSixteenthGrid)
{
	Case c;
	c.seed = 17;
	c.draws.push_back(Tile(1));
	c.draws.back().prim.FST = 0;
	c.draws.back().st_nudge = 1.0f / 32.0f;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, Refuses24BitFrame)
{
	Case c;
	c.seed = 12;
	c.draws.push_back(Tile(1));
	c.draws.back().frame.PSM = PSMCT24;
	EXPECT_EQ(Compare(c), 0u);
}

// A 16-bit frame under no mask: the palette entry packed to five bits a colour and one of alpha.
TEST_F(GSSwPaletteBlockCopy, TakesA16BitFrame)
{
	u32 seed = 20;
	for (const u32 psm : {PSMCT16, PSMCT16S, PSMZ16, PSMZ16S})
	{
		Case c;
		c.seed = seed++;
		c.draws.push_back(Tile(5));
		c.draws.back().frame.PSM = psm;
		EXPECT_EQ(Compare(c), 1u) << "frame format " << psm;
	}
}

// The six masks Spider-Man 3 expands its textures under, for the 4-bit and 8-bit sources it uses them with.
TEST_F(GSSwPaletteBlockCopy, TakesTheMasksSpiderMan3Uses)
{
	u32 seed = 30;
	for (const u32 mask : sm3_masks)
	{
		for (const u32 psm : {PSMT4, PSMT8})
		{
			Case c;
			c.seed = seed++;
			Draw d = Tile(3);
			d.tex0.PSM = psm;
			d.frame.PSM = PSMCT16;
			d.frame.FBMSK = mask;
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << "mask " << std::hex << mask << " source " << psm;
		}
	}
}

// Spider-Man 3's draw: ST sprites at a constant Q of 64 that remap a 64x64 texture into 8x4 pieces of a
// 64-pixel-wide 16-bit frame, 8x2 and 4x2 at a time, four pieces to a draw.
static Draw Sm3Draw(u32 tex_psm, u32 mask)
{
	Draw d = {};
	d.tex0.TBP0 = 0x3f46;
	d.tex0.TBW = 1;
	d.tex0.PSM = tex_psm;
	d.tex0.TW = 6;
	d.tex0.TH = 6;
	d.tex0.TCC = 1;
	d.tex0.TFX = TFX_DECAL;
	d.tex0.CBP = 0x3000;
	d.tex0.CPSM = PSMCT32;
	d.tex0.CLD = 1;
	d.frame.FBP = 0x3f00 / 32;
	d.frame.FBW = 1;
	d.frame.PSM = PSMCT16;
	d.frame.FBMSK = mask;
	d.test.ZTE = 1;
	d.test.ZTST = ZTST_ALWAYS;
	d.xyoffset.OFX = 2048 * 16;
	d.xyoffset.OFY = 2048 * 16;
	d.scissor.SCAX1 = 63;
	d.scissor.SCAY1 = 63;
	d.rgbaq.R = d.rgbaq.G = d.rgbaq.B = d.rgbaq.A = 0x80;
	d.rgbaq.Q = 64.0f;
	d.prim.PRIM = GS_SPRITE;
	d.prim.TME = 1;
	d.prim.FST = 0;

	const auto sprite = [&d](int dx, int dy, int dw, int dh, int sx, int sy) {
		d.sprites.push_back({(2048 + dx) * 16, (2048 + dy) * 16, (2048 + dx + dw) * 16, (2048 + dy + dh) * 16,
			sx * 16, sy * 16, (sx + dw) * 16, (sy + dh) * 16});
	};
	for (int g = 0; g < 4; g++)
	{
		sprite(g * 8, 28, 8, 2, 56, 24);
		sprite(g * 8 + 4, 30, 4, 2, 60, 28);
		sprite(g * 8, 30, 4, 2, 56, 28);
	}
	return d;
}

TEST_F(GSSwPaletteBlockCopy, TakesSpiderMan3sSprites)
{
	u32 seed = 50;
	for (const u32 mask : sm3_masks)
	{
		for (const u32 psm : {PSMT4, PSMT8})
		{
			Case c;
			c.seed = seed++;
			c.draws.push_back(Sm3Draw(psm, mask));
			EXPECT_EQ(Compare(c), 1u) << "mask " << std::hex << mask << " source " << psm;
		}
	}
}

// Spider-Man 3's 4-bit draws: the same remap, but through a mipmap. The game holds the level of detail at a
// constant 1 (TEX1.K = 16, LCM = 1, MMIN = 2: nearest, from the level), gives the coordinates at the base
// texture's scale, two texels to a pixel, and keeps the real source at level 1, a 32x32 texture at its own address.
static Draw Sm3MipDraw(u32 tex_psm, u32 mask, int level = 1)
{
	Draw d = Sm3Draw(tex_psm, mask);
	d.tex1.LCM = 1;
	d.tex1.MXL = 3;
	d.tex1.MMIN = 2;
	d.tex1.K = 16 * level;
	d.miptbp1.TBP1 = 0x3f56;
	d.miptbp1.TBW1 = 1;
	d.miptbp1.TBP2 = 0x3f5a;
	d.miptbp1.TBW2 = 1;
	d.miptbp1.TBP3 = 0x3f5b;
	d.miptbp1.TBW3 = 1;
	d.rgbaq.Q = 8.0f;
	d.sprites.clear();

	const int step = 1 << level;
	const auto sprite = [&d, step](int dx, int dy, int dw, int dh, int sx, int sy) {
		d.sprites.push_back({(2048 + dx) * 16, (2048 + dy) * 16, (2048 + dx + dw) * 16, (2048 + dy + dh) * 16,
			sx * 16, sy * 16, (sx + dw * step) * 16, (sy + dh * step) * 16});
	};
	for (int g = 0; g < 4; g++)
		sprite(g * 4, 14, 4, 2, g & 1 ? 48 / step : 56 / step, 56 / step);
	return d;
}

TEST_F(GSSwPaletteBlockCopy, TakesSpiderMan3sMipmappedSprites)
{
	u32 seed = 140;
	for (const u32 mask : sm3_masks)
	{
		for (const u32 psm : {PSMT4, PSMT8})
		{
			Case c;
			c.seed = seed++;
			c.draws.push_back(Sm3MipDraw(psm, mask));
			EXPECT_EQ(Compare(c), 1u) << "mask " << std::hex << mask << " source " << psm;
		}
	}
}

// Each constant level, with UV coordinates and with ST coordinates, from every source format.
TEST_F(GSSwPaletteBlockCopy, TakesEveryConstantMipLevel)
{
	u32 seed = 160;
	for (int level = 0; level <= 3; level++)
	{
		for (const u32 psm : {PSMT4, PSMT8, PSMT8H, PSMT4HL, PSMT4HH})
		{
			for (const bool fst : {false, true})
			{
				Case c;
				c.seed = seed++;
				Draw d = Sm3MipDraw(psm, sm3_masks[seed % 6], level);
				d.prim.FST = fst;
				if (fst)
					d.rgbaq.Q = 1.0f;
				c.draws.push_back(d);
				EXPECT_EQ(Compare(c), 1u) << "level " << level << " source " << psm << (fst ? " UV" : " ST");
			}
		}
	}
}

// A mip level's wrap limits are the base texture's, shifted down by the level, repeating and clamping.
TEST_F(GSSwPaletteBlockCopy, WrapsAMipLevelAtTheLevelsSize)
{
	u32 seed = 280;
	for (const u32 wms : {CLAMP_REPEAT, CLAMP_CLAMP})
	{
		for (const u32 wmt : {CLAMP_REPEAT, CLAMP_CLAMP})
		{
			for (int level = 1; level <= 2; level++)
			{
				Case c;
				c.seed = seed++;
				Draw d = Sm3MipDraw(PSMT8, 0x00003fffu, level);
				d.clamp.WMS = wms;
				d.clamp.WMT = wmt;
				// Sprites that run past the level's far edge and start inside it.
				d.sprites.clear();
				const int step = 1 << level;
				const int span = 64 / step;
				for (int g = 0; g < 3; g++)
				{
					const int sx = (span - 2 - g) * step;
					const int sy = (span - 1 - g) * step;
					d.sprites.push_back({(2048 + g * 8) * 16, (2048 + 14) * 16, (2048 + g * 8 + 8) * 16, (2048 + 16) * 16,
						sx * 16, sy * 16, (sx + 8 * step) * 16, (sy + 2 * step) * 16});
				}
				c.draws.push_back(d);
				EXPECT_EQ(Compare(c), 1u) << "level " << level << " wms " << wms << " wmt " << wmt;
			}
		}
	}
}

TEST_F(GSSwPaletteBlockCopy, TakesAMipmappedDrawWithoutASnapshotOfAnyLevel)
{
	Case c;
	c.seed = 270;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	EXPECT_EQ(Compare(c), 1u);
	EXPECT_GT(m_ref_unswizzled, 0.0);
	EXPECT_EQ(m_got_unswizzled, 0.0);
}

TEST_F(GSSwPaletteBlockCopy, RefusesAFilteredMipLevel)
{
	Case c;
	c.seed = 271;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	c.draws.back().tex1.MMIN = 4; // linear, from the nearest level
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesATrilinearMipLevel)
{
	Case c;
	c.seed = 272;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	c.draws.back().tex1.MMIN = 3;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesALevelOfDetailTakenFromQ)
{
	Case c;
	c.seed = 273;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	c.draws.back().tex1.LCM = 0;
	EXPECT_EQ(Compare(c), 0u);
}

// The scanline divides ST by Q a pixel at a time; only a power of two keeps that exact.
TEST_F(GSSwPaletteBlockCopy, RefusesMipmappedSTCoordinatesDividedByAnInexactQ)
{
	Case c;
	c.seed = 274;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	c.draws.back().rgbaq.Q = 3.0f;
	EXPECT_EQ(Compare(c), 0u);
}

// A mip sprite that steps one base texel a pixel, not two, is not one texel of the level a pixel.
TEST_F(GSSwPaletteBlockCopy, RefusesAMipmappedSpriteAtTheWrongStep)
{
	Case c;
	c.seed = 275;
	c.draws.push_back(Sm3MipDraw(PSMT4, 0xffffc07fu));
	for (Sprite& s : c.draws.back().sprites)
	{
		s.u1 = s.u0 + (s.x1 - s.x0);
		s.v1 = s.v0 + (s.y1 - s.y0);
	}
	EXPECT_EQ(Compare(c), 0u);
}

// The decision is made before the rasterizer's texture snapshot: with the copy on nothing is converted,
// with it off the snapshot converts whole blocks of the texture.
TEST_F(GSSwPaletteBlockCopy, TakesTheDrawWithoutASnapshotOfTheTexture)
{
	Case c;
	c.seed = 56;
	c.draws.push_back(Sm3Draw(PSMT4, 0xffffc07fu));
	EXPECT_EQ(Compare(c), 1u);
	EXPECT_GT(m_ref_unswizzled, 0.0);
	EXPECT_EQ(m_got_unswizzled, 0.0);
}

// Every source format through every palette format, into both 16-bit frame layouts.
TEST_F(GSSwPaletteBlockCopy, TakesEverySourceAndPaletteFormatInto16BitFrames)
{
	u32 seed = 60;
	for (const u32 tex : {PSMT4, PSMT8, PSMT8H, PSMT4HL, PSMT4HH})
	{
		for (const u32 clut : {PSMCT32, PSMCT16, PSMCT16S})
		{
			for (const u32 frame : {PSMCT16, PSMCT16S})
			{
				Case c;
				c.seed = seed++;
				Draw d = Sm3Draw(tex, sm3_masks[seed % 6]);
				d.tex0.CPSM = clut;
				d.frame.PSM = frame;
				d.tex0.CSA = seed & 15;
				c.draws.push_back(d);
				EXPECT_EQ(Compare(c), 1u) << "source " << tex << " palette " << clut << " frame " << frame;
			}
		}
	}
}

// Without TCC the texel's colour goes out with the vertex's alpha, and the frame keeps one bit of it.
TEST_F(GSSwPaletteBlockCopy, TakesVertexAlphaInto16BitFrames)
{
	u32 seed = 100;
	for (const int alpha : {0x00, 0x37, 0x7f, 0x80, 0xc9, 0xff})
	{
		Case c;
		c.seed = seed++;
		// A mask that leaves the alpha bit writable (the sm3 masks that keep the top half of the pixel).
		Draw d = Sm3Draw(PSMT8, alpha & 1 ? 0u : 0x00003fffu);
		d.tex0.TCC = 0;
		d.rgbaq.A = alpha;
		c.draws.push_back(d);
		EXPECT_EQ(Compare(c), 1u) << "alpha " << alpha;
	}
}

// The flat alpha is the second vertex's, the one DrawSprite keeps, whatever the first vertex says.
TEST_F(GSSwPaletteBlockCopy, TakesTheSecondVerticesAlphaInto16BitFrames)
{
	u32 seed = 112;
	for (const bool frame16 : {true, false})
	{
		Case c;
		c.seed = seed++;
		Draw d = frame16 ? Sm3Draw(PSMT8, 0x00003fffu) : Tile(2);
		d.prim.FST = 1;
		d.tex0.TCC = 0;
		d.alpha0 = 0xff;
		d.alpha1 = 0x00;
		c.draws.push_back(d);
		Case c2 = c;
		c2.seed = seed++;
		c2.draws.back().alpha0 = 0x00;
		c2.draws.back().alpha1 = 0xff;
		EXPECT_EQ(Compare(c), 1u) << (frame16 ? "16-bit frame" : "32-bit frame");
		EXPECT_EQ(Compare(c2), 1u) << (frame16 ? "16-bit frame" : "32-bit frame");
	}
}

// FBA forces the frame's alpha bit on in a 16-bit frame, and is still refused in a 32-bit one.
TEST_F(GSSwPaletteBlockCopy, TakesFrameAlphaBitIn16BitFramesOnly)
{
	Case c;
	c.seed = 110;
	c.draws.push_back(Sm3Draw(PSMT8, 0x00003fffu));
	c.draws.back().fba.FBA = 1;
	EXPECT_EQ(Compare(c), 1u);

	Case r;
	r.seed = 111;
	r.draws.push_back(Tile(1));
	r.draws.back().fba.FBA = 1;
	EXPECT_EQ(Compare(r), 0u);
}

// The frame over the draw's own texture, in a 16-bit frame under a mask: every texel is read before the
// first pixel is written, as the snapshot would have it.
TEST_F(GSSwPaletteBlockCopy, ReadsItsOwnTextureAsItWasBeforeTheDrawIn16BitFrames)
{
	u32 seed = 120;
	for (const u32 psm : {PSMT8, PSMT4, PSMT8H})
	{
		Case c;
		c.seed = seed++;
		Draw d = Tile(0);
		d.tex0.PSM = psm;
		d.tex0.TBP0 = d.frame.FBP * 32;
		d.tex0.TBW = 32;
		d.frame.PSM = PSMCT16;
		d.frame.FBMSK = 0xffffc000u;
		d.sprites.clear();
		for (int i = 0; i < 4; i++)
			d.sprites.push_back({(2048 + i * 4) * 16, 2048 * 16, (2048 + i * 4 + 8) * 16, (2048 + 8) * 16,
				(i * 2) * 16 + 8, 8, (i * 2 + 8) * 16 + 8, 8 * 16 + 8});
		c.draws.push_back(d);
		EXPECT_EQ(Compare(c), 1u) << "source " << psm;
	}
}

TEST_F(GSSwPaletteBlockCopy, RefusesDitherInA16BitFrame)
{
	Case c;
	c.seed = 130;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffc000u));
	c.draws.back().dthe.DTHE = 1;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesA16BitFrameUnderADestinationAlphaTest)
{
	Case c;
	c.seed = 131;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffc000u));
	c.draws.back().test.DATE = 1;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesA16BitFrameWithALinearFilter)
{
	Case c;
	c.seed = 132;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffc000u));
	c.draws.back().tex1.MMAG = 1;
	c.draws.back().tex1.MMIN = 1;
	EXPECT_EQ(Compare(c), 0u);
}

// ST that is not on the sixteenth grid once converted is the rasterizer's, in a 16-bit frame too.
TEST_F(GSSwPaletteBlockCopy, RefusesA16BitFrameWithSTOffTheSixteenthGrid)
{
	Case c;
	c.seed = 135;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffc000u));
	c.draws.back().st_nudge = 1.0f / 32.0f;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, RefusesA16BitFrameWhoseSpritesAreScaled)
{
	Case c;
	c.seed = 133;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffc000u));
	c.draws.back().sprites[0].u1 += 16;
	EXPECT_EQ(Compare(c), 0u);
}

TEST_F(GSSwPaletteBlockCopy, LeavesA16BitFrameAloneUnderAFullMask)
{
	Case c;
	c.seed = 134;
	c.draws.push_back(Sm3Draw(PSMT8, 0xffffffffu));
	EXPECT_EQ(Compare(c), 0u);
}

#ifdef GS_PALETTE_COPY_TESTS_JIT
// Every corner of an 8x8 sprite on the 4-pixel grid makes the rasterizer pick notest; a scissor
// edge through the middle of it then hands the scanline a span that starts off the grid. The C++
// scanline asserts on that, so this runs against generated code only.
TEST_F(GSSwPaletteBlockCopy, RefusesANotestSpriteTheScissorCutsOffTheGrid)
{
	Case c;
	c.seed = 14;
	c.draws.push_back(Tile(1));
	c.draws.back().scissor.SCAX0 = 18;
	EXPECT_EQ(Compare(c), 0u);
}
#endif

// Random cases, byte-compared. The draws the copy took must be a real share of the total, or the
// comparison proved nothing about it.
TEST_F(GSSwPaletteBlockCopy, MatchesTheRasterizerOnRandomDraws)
{
	constexpr u32 cases = 800;
	u64 draws = 0;
	u64 copies = 0;
	u64 draws16 = 0;
	u64 copies16 = 0;
	u64 drawsmip = 0;
	u64 copiesmip = 0;

	for (u32 seed = 1000; seed < 1000 + cases; seed++)
	{
		const Case c = RandomCase(seed);
		draws += c.draws.size();
		copies += Compare(c);
		for (size_t i = 0; i < c.draws.size() && i < m_got_copies.size(); i++)
		{
			if (IsFrame16(c.draws[i].frame.PSM))
			{
				draws16++;
				copies16 += m_got_copies[i];

				if (c.draws[i].tex1.MXL != 0 && c.draws[i].tex1.MMIN == 2 && c.draws[i].tex1.K >= 8)
				{
					drawsmip++;
					copiesmip += m_got_copies[i];
				}
			}
		}
		if (HasFailure())
			break;
	}

	std::printf("palette block copy: %" PRIu64 " of %" PRIu64 " random draws taken, %" PRIu64 " of %" PRIu64 " into 16-bit frames, %" PRIu64 " of %" PRIu64 " of those mipmapped\n",
		copies, draws, copies16, draws16, copiesmip, drawsmip);
	EXPECT_GE(copies * 5, draws) << "fewer than a fifth of the random draws reached the copy";
	EXPECT_GE(copies16 * 5, draws16) << "fewer than a fifth of the random draws into 16-bit frames reached the masked texel copy";
	EXPECT_GE(copiesmip * 10, drawsmip) << "fewer than a tenth of the random mipmapped draws reached the masked texel copy";
}

// The 32-bit texel copy: a sprite draw that writes each texel of a 32-bit texture one-to-one into a 32-bit frame.
// There is no palette, so the copy reads the texels straight from local memory and writes them back, and the
// rasterizer's snapshot of the texture is never taken. The cases below are the same comparison as the palette
// block copy's: both arms from one local memory, every byte compared.

// A 32-bit blit: a 32x16 piece of a 256x256 32-bit texture into a 128-pixel-wide 32-bit frame, texel for pixel.
static Draw Blit32(int frac = 0)
{
	Draw d = {};
	d.tex0.TBP0 = 0x2000;
	d.tex0.TBW = 4;
	d.tex0.PSM = PSMCT32;
	d.tex0.TW = 8;
	d.tex0.TH = 8;
	d.tex0.TCC = 1;
	d.tex0.TFX = TFX_DECAL;
	d.frame.FBP = 0x40;
	d.frame.FBW = 2;
	d.frame.PSM = PSMCT32;
	d.test.ZTE = 1;
	d.test.ZTST = ZTST_ALWAYS;
	d.xyoffset.OFX = 2048 * 16;
	d.xyoffset.OFY = 2048 * 16;
	d.scissor.SCAX1 = 127;
	d.scissor.SCAY1 = 255;
	d.rgbaq.R = d.rgbaq.G = d.rgbaq.B = d.rgbaq.A = 0x80;
	d.rgbaq.Q = 1.0f;
	d.prim.PRIM = GS_SPRITE;
	d.prim.TME = 1;
	d.prim.FST = 1;
	d.sprites.push_back({(2048 + 8) * 16, (2048 + 4) * 16, (2048 + 40) * 16, (2048 + 20) * 16,
		(24) * 16 + frac, (100) * 16 + frac, (24 + 32) * 16 + frac, (100 + 16) * 16 + frac});
	return d;
}

static Sprite Blit32Sprite(int dx, int dy, int w, int h, int sx, int sy, int frac = 0)
{
	return {(2048 + dx) * 16, (2048 + dy) * 16, (2048 + dx + w) * 16, (2048 + dy + h) * 16,
		sx * 16 + frac, sy * 16 + frac, (sx + w) * 16 + frac, (sy + h) * 16 + frac};
}

TEST_F(GSSwPaletteBlockCopy, TakesA32BitBlit)
{
	Case c;
	c.seed = 200;
	c.draws.push_back(Blit32());
	EXPECT_EQ(Compare(c), 1u);
}

// No snapshot of the texture is taken: with the copy on nothing is converted, with it off the snapshot converts
// whole blocks of the texture.
TEST_F(GSSwPaletteBlockCopy, TakesA32BitBlitWithoutASnapshotOfTheTexture)
{
	Case c;
	c.seed = 201;
	c.draws.push_back(Blit32());
	EXPECT_EQ(Compare(c), 1u);
	EXPECT_GT(m_ref_unswizzled, 0.0);
	EXPECT_EQ(m_got_unswizzled, 0.0);
}

// The sub-texel phase of the coordinates does not matter to a nearest lookup that steps one texel a pixel.
TEST_F(GSSwPaletteBlockCopy, TakesEverySubTexelPhase)
{
	u32 seed = 210;
	for (int frac = 0; frac < 16; frac++)
	{
		Case c;
		c.seed = seed++;
		c.draws.push_back(Blit32(frac));
		EXPECT_EQ(Compare(c), 1u) << "frac " << frac;
	}
}

// Stuntman's blits are a run of consecutive draws, each a different piece of the texture.
TEST_F(GSSwPaletteBlockCopy, TakesARunOfBlits)
{
	Case c;
	c.seed = 220;
	for (int i = 0; i < 24; i++)
	{
		Draw d = Blit32();
		d.sprites.clear();
		d.sprites.push_back(Blit32Sprite((i & 3) * 32, (i >> 2) * 16, 32, 16, (i * 24) & 0xe0, (i * 40) & 0xf0));
		c.draws.push_back(d);
	}
	EXPECT_EQ(Compare(c), 24u);
}

TEST_F(GSSwPaletteBlockCopy, TakesEveryFrameAndTextureWidth)
{
	u32 seed = 230;
	for (int fbw = 1; fbw <= 16; fbw++)
	{
		for (const int tbw : {1, 4, 16})
		{
			Case c;
			c.seed = seed++;
			Draw d = Blit32();
			d.frame.FBW = fbw;
			d.tex0.TBW = tbw;
			d.scissor.SCAX1 = fbw * 64 - 1;
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << "fbw " << fbw << " tbw " << tbw;
		}
	}
}

// Both 32-bit frame layouts, and the 32-bit depth layout as a frame.
TEST_F(GSSwPaletteBlockCopy, TakesTheFrameFormatsWithA32BitPixel)
{
	u32 seed = 260;
	for (const u32 psm : {PSMCT32, PSMZ32})
	{
		Case c;
		c.seed = seed++;
		c.draws.push_back(Blit32());
		c.draws.back().frame.PSM = psm;
		EXPECT_EQ(Compare(c), 1u) << "frame format " << psm;
	}
}

// Without TCC the texel's colour goes out with the vertex's alpha, the second vertex's.
TEST_F(GSSwPaletteBlockCopy, TakesTextureColourWithVertexAlphaFrom32BitTextures)
{
	u32 seed = 270;
	for (const int alpha : {0x00, 0x37, 0x7f, 0x80, 0xc9, 0xff})
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		d.tex0.TCC = 0;
		d.rgbaq.A = alpha;
		c.draws.push_back(d);
		d.alpha0 = 0xff - alpha;
		d.alpha1 = alpha;
		c.draws.push_back(d);
		EXPECT_EQ(Compare(c), 2u) << "alpha " << alpha;
	}
}

// With TCC the texel's own alpha goes out, whatever the vertex says and whatever TEXA says: TEXA is not read for a
// 32-bit texture.
TEST_F(GSSwPaletteBlockCopy, WritesTheTexelsOwnAlphaWhateverTEXASays)
{
	u32 seed = 280;
	for (const int ta : {0, 1, 0x80, 0xff})
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		d.texa.TA0 = ta;
		d.texa.TA1 = 0xff - ta;
		d.texa.AEM = ta & 1;
		d.rgbaq.A = ta ^ 0x55;
		c.draws.push_back(d);
		EXPECT_EQ(Compare(c), 1u) << "TA0 " << ta;
	}
}

TEST_F(GSSwPaletteBlockCopy, TakesModulateByHalfOf32BitTexturesAndRefusesAnyOtherColour)
{
	Case c;
	c.seed = 290;
	c.draws.push_back(Blit32());
	c.draws.back().tex0.TFX = TFX_MODULATE;
	c.draws.push_back(Blit32());
	c.draws.back().tex0.TFX = TFX_MODULATE;
	c.draws.back().tex0.TCC = 0;
	c.draws.back().rgbaq.A = 0x11;
	EXPECT_EQ(Compare(c), 2u);

	Case r;
	r.seed = 291;
	r.draws.push_back(Blit32());
	r.draws.back().tex0.TFX = TFX_MODULATE;
	r.draws.back().rgbaq.G = 0x7F;
	EXPECT_EQ(Compare(r), 0u);
}

// Repeat and clamp when the sprite reads past the texture's edge, on each axis.
TEST_F(GSSwPaletteBlockCopy, WrapsAt32BitTexturesEdges)
{
	u32 seed = 300;
	for (const int wms : {CLAMP_REPEAT, CLAMP_CLAMP})
	{
		for (const int wmt : {CLAMP_REPEAT, CLAMP_CLAMP})
		{
			for (const int tw : {3, 5, 8})
			{
				Case c;
				c.seed = seed++;
				Draw d = Blit32();
				d.clamp.WMS = wms;
				d.clamp.WMT = wmt;
				d.tex0.TW = tw;
				d.tex0.TH = tw;
				d.sprites.clear();
				// Starts inside the texture and runs past its right and bottom edges.
				d.sprites.push_back(Blit32Sprite(4, 4, 32, 16, (1 << tw) - 11, (1 << tw) - 5));
				c.draws.push_back(d);
				// Starts past both edges.
				d.sprites[0] = Blit32Sprite(4, 40, 16, 16, (1 << tw) + 3, (1 << tw) + 9);
				c.draws.push_back(d);
				// Starts at the origin and is bigger than the texture.
				d.sprites[0] = Blit32Sprite(0, 64, 32, 32, 0, 0);
				c.draws.push_back(d);
				Compare(c);
			}
		}
	}
}

// The frame over the draw's own texture: every texel is read before the first pixel is written, as the snapshot
// would have it.
TEST_F(GSSwPaletteBlockCopy, ReadsItsOwn32BitTextureAsItWasBeforeTheDraw)
{
	u32 seed = 330;
	for (const int shift : {0, 1, 2, 8, 9, 16, 17, 33})
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		d.tex0.TBP0 = d.frame.FBP * 32;
		d.tex0.TBW = d.frame.FBW;
		d.tex0.TW = 7;
		d.tex0.TH = 7;
		d.sprites.clear();
		// Each sprite reads what the one before wrote, and what its own pixels are about to overwrite.
		d.sprites.push_back(Blit32Sprite(0, 0, 32, 32, shift, 0));
		d.sprites.push_back(Blit32Sprite(8, 8, 32, 32, 0, shift));
		d.sprites.push_back(Blit32Sprite(shift, shift, 16, 16, 0, 0));
		c.draws.push_back(d);
		d.tex0.TBP0 += shift;
		c.draws.push_back(d);
		Compare(c);
		EXPECT_GE(m_got_copies[0], 1u) << "shift " << shift;
	}
}

TEST_F(GSSwPaletteBlockCopy, TakesOverlappingSpritesInOrder)
{
	Case c;
	c.seed = 340;
	Draw d = Blit32();
	d.sprites.clear();
	d.sprites.push_back(Blit32Sprite(0, 0, 32, 32, 0, 0));
	d.sprites.push_back(Blit32Sprite(16, 16, 32, 32, 64, 64));
	d.sprites.push_back(Blit32Sprite(8, 24, 16, 16, 128, 32));
	c.draws.push_back(d);
	EXPECT_EQ(Compare(c), 1u);
}

// A scissor that cuts the sprite on each side, on the vector grid and off it.
TEST_F(GSSwPaletteBlockCopy, ClipsToTheScissor)
{
	u32 seed = 350;
	for (const int x0 : {0, 3, 8, 11, 16})
	{
		for (const int x1 : {20, 27, 31, 39, 127})
		{
			Case c;
			c.seed = seed++;
			Draw d = Blit32();
			d.scissor.SCAX0 = x0;
			d.scissor.SCAX1 = x1;
			d.scissor.SCAY0 = (x0 & 1) * 7;
			d.scissor.SCAY1 = 8 + (x1 & 7);
#ifndef GS_PALETTE_COPY_TESTS_JIT
			// The C++ scanline asserts on a span the scissor cuts off the vector grid; the generated-code build
			// takes the unaligned edges.
			d.scissor.SCAX0 &= ~3;
			d.scissor.SCAX1 |= 3;
#endif
			c.draws.push_back(d);
			Compare(c);
		}
	}
}

TEST_F(GSSwPaletteBlockCopy, LeavesMemoryAloneForASpriteOutsideTheScissor)
{
	Case c;
	c.seed = 360;
	c.draws.push_back(Blit32());
	c.draws.back().scissor.SCAX0 = 100;
	c.draws.back().scissor.SCAX1 = 123;
	Compare(c);
}

TEST_F(GSSwPaletteBlockCopy, WrapsAFrameAtTheEndOfLocalMemory)
{
	u32 seed = 370;
	for (const int fbp : {0x1ff, 0x1fe, 0x1fd, 0x1fc})
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		d.frame.FBP = fbp;
		d.frame.FBW = 8;
		d.scissor.SCAX1 = 511;
		d.sprites.clear();
		d.sprites.push_back(Blit32Sprite(0, 0, 64, 64, 0, 0));
		d.sprites.push_back(Blit32Sprite(200, 20, 128, 64, 32, 32));
		c.draws.push_back(d);
		Compare(c);
		EXPECT_EQ(m_got_copies[0], 1u) << "fbp " << fbp;
	}
}

TEST_F(GSSwPaletteBlockCopy, ReadsATextureAtTheEndOfLocalMemory)
{
	u32 seed = 380;
	for (const int tbp : {0x3fff, 0x3ff0, 0x3f00, 0x3e00, 0x3c00})
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		d.tex0.TBP0 = tbp;
		d.sprites.clear();
		d.sprites.push_back(Blit32Sprite(0, 0, 64, 64, 0, 0));
		d.sprites.push_back(Blit32Sprite(0, 64, 64, 64, 128, 128));
		c.draws.push_back(d);
		Compare(c);
	}
}

// ST sprites whose S and T land exactly on texels are the UV draw, at a Q of one or a power of two.
TEST_F(GSSwPaletteBlockCopy, TakesSTCoordinatesOnTheSixteenthGridFrom32BitTextures)
{
	Case c;
	c.seed = 390;
	Draw d = Blit32();
	d.prim.FST = 0;
	c.draws.push_back(d);
	d.rgbaq.Q = 2.0f;
	c.draws.push_back(d);
	d.rgbaq.Q = 0.25f;
	c.draws.push_back(d);
	EXPECT_EQ(Compare(c), 3u);
}

TEST_F(GSSwPaletteBlockCopy, Refuses32BitBlitSTOffTheSixteenthGrid)
{
	Case c;
	c.seed = 391;
	c.draws.push_back(Blit32());
	c.draws.back().prim.FST = 0;
	c.draws.back().st_nudge = 1.0f / 32.0f;
	EXPECT_EQ(Compare(c), 0u);
}

// What the rasterizer does that the copy does not model: each is one change to the blit, and none is taken.
TEST_F(GSSwPaletteBlockCopy, Refuses32BitBlitsWithAnythingElseOnTheDraw)
{
	struct Change
	{
		const char* name;
		void (*apply)(Draw&);
	};
	static const Change changes[] = {
		{"frame mask", [](Draw& d) { d.frame.FBMSK = 0x00FF0000; }},
		{"alpha mask", [](Draw& d) { d.frame.FBMSK = 0xFF000000; }},
		{"colour mask", [](Draw& d) { d.frame.FBMSK = 0x00FFFFFF; }},
		{"alpha test", [](Draw& d) { d.test.ATE = 1; d.test.ATST = ATST_GREATER; d.test.AREF = 0x40; d.test.AFAIL = 0; }},
		{"alpha test, keep colour", [](Draw& d) { d.test.ATE = 1; d.test.ATST = ATST_LESS; d.test.AREF = 0x90; d.test.AFAIL = 3; }},
		{"destination alpha test", [](Draw& d) { d.test.DATE = 1; d.test.DATM = 1; }},
		{"blend", [](Draw& d) { d.prim.ABE = 1; d.alpha.A = 0; d.alpha.B = 1; d.alpha.C = 0; d.alpha.D = 1; }},
		{"fog", [](Draw& d) { d.prim.FGE = 1; }},
		{"dither", [](Draw& d) { d.dthe.DTHE = 1; }},
		{"frame alpha bit", [](Draw& d) { d.fba.FBA = 1; }},
		{"scan mask", [](Draw& d) { d.scanmsk.MSK = 2; }},
		{"linear filter at weights of a half", [](Draw& d) { d.tex1.MMAG = 1; d.tex1.MMIN = 1; }},
		{"linear filter, odd weights", [](Draw& d) { d.tex1.MMAG = 1; d.tex1.MMIN = 1; for (Sprite& sp : d.sprites) { sp.u0 += 8; sp.u1 += 8; sp.v0 += 8; sp.v1 += 8; sp.u0 += 1; sp.u1 += 1; } }},
		{"linear filter, a quarter step", [](Draw& d) { d.tex1.MMAG = 1; d.tex1.MMIN = 1; for (Sprite& sp : d.sprites) { sp.u0 += 8; sp.u1 += 8 + 8 * 16; sp.v0 += 8; sp.v1 += 8 + 8 * 16; } }},
		{"linear filter, three texels a pixel", [](Draw& d) { d.tex1.MMAG = 1; d.tex1.MMIN = 1; for (Sprite& sp : d.sprites) { sp.u1 = sp.u0 + 3 * (sp.u1 - sp.u0); sp.v1 = sp.v0 + 3 * (sp.v1 - sp.v0); } }},
		{"two texels a pixel, nearest", [](Draw& d) { for (Sprite& sp : d.sprites) { sp.u1 = sp.u0 + 2 * (sp.u1 - sp.u0); sp.v1 = sp.v0 + 2 * (sp.v1 - sp.v0); } }},
		{"region clamp u", [](Draw& d) { d.clamp.WMS = CLAMP_REGION_CLAMP; d.clamp.MAXU = 31; }},
		{"region repeat v", [](Draw& d) { d.clamp.WMT = CLAMP_REGION_REPEAT; d.clamp.MINV = 7; d.clamp.MAXV = 64; }},
		{"highlight", [](Draw& d) { d.tex0.TFX = 2; }},
		{"highlight 2", [](Draw& d) { d.tex0.TFX = 3; }},
		{"modulate by other colour", [](Draw& d) { d.tex0.TFX = TFX_MODULATE; d.rgbaq.R = 0x40; }},
		{"depth test", [](Draw& d) { d.test.ZTE = 1; d.test.ZTST = ZTST_GEQUAL; }},
		{"depth write", [](Draw& d) { d.test.ZTE = 1; d.test.ZTST = ZTST_ALWAYS; d.zmsk = 0; }},
		{"width not a power of two", [](Draw& d) { d.sprites[0].x1 += 4 * 16; d.sprites[0].u1 += 4 * 16; }},
		{"height not a power of two", [](Draw& d) { d.sprites[0].y1 += 2 * 16; d.sprites[0].v1 += 2 * 16; }},
		{"scaled", [](Draw& d) { d.sprites[0].u1 += 16 * 16; }},
		{"mirrored", [](Draw& d) { std::swap(d.sprites[0].u0, d.sprites[0].u1); }},
		{"24-bit frame", [](Draw& d) { d.frame.PSM = PSMCT24; }},
		{"16-bit frame", [](Draw& d) { d.frame.PSM = PSMCT16; }},
		{"16-bit skewed frame", [](Draw& d) { d.frame.PSM = PSMCT16S; }},
		{"24-bit texture", [](Draw& d) { d.tex0.PSM = PSMCT24; }},
		{"16-bit texture", [](Draw& d) { d.tex0.PSM = PSMCT16; }},
		{"16-bit skewed texture", [](Draw& d) { d.tex0.PSM = PSMCT16S; }},
		{"32-bit depth texture", [](Draw& d) { d.tex0.PSM = PSMZ32; }},
		{"mipmap at level one", [](Draw& d) { d.tex1.LCM = 1; d.tex1.MXL = 2; d.tex1.MMIN = 2; d.tex1.K = 16; d.miptbp1.TBP1 = 0x2100; d.miptbp1.TBW1 = 2; }},
	};

	u32 seed = 400;
	for (const Change& ch : changes)
	{
		Case c;
		c.seed = seed++;
		Draw d = Blit32();
		ch.apply(d);
		c.draws.push_back(d);
		EXPECT_EQ(Compare(c), 0u) << ch.name;
	}
}

// RandomCase32() draws, byte-compared against the rasterizer. Those in the copy's shape are mostly taken; the
// rest it must leave to the rasterizer.
TEST_F(GSSwPaletteBlockCopy, MatchesTheRasterizerOnRandomDrawsFrom32BitTextures)
{
	constexpr u32 cases = 800;
	u64 draws = 0;
	u64 in_shape = 0; // 32-bit texture, 32-bit frame, no perturbation, no mipmap: the draws the copy is for
	u64 in_shape_taken = 0;
	u64 taken_elsewhere = 0; // taken although not in that shape; only an alpha test or a blend the renderer proves away may be
	u64 other_formats_taken = 0;

	for (u32 seed = 5000; seed < 5000 + cases; seed++)
	{
		std::vector<bool> shaped;
		const Case c = RandomCase32(seed, shaped);
		draws += c.draws.size();
		Compare(c);
		for (size_t i = 0; i < c.draws.size() && i < m_got_copies.size(); i++)
		{
			const Draw& d = c.draws[i];
			if (shaped[i])
			{
				in_shape++;
				in_shape_taken += m_got_copies[i];
			}
			else if (m_got_copies[i] != 0 && d.tex0.PSM != PSMCT32)
			{
				other_formats_taken += m_got_copies[i];
			}
			else if (m_got_copies[i] != 0)
			{
				taken_elsewhere += m_got_copies[i];
			}
		}
		if (HasFailure())
			break;
	}

	std::printf("32-bit texel copy: %" PRIu64 " of %" PRIu64 " random draws in the copy's shape taken (%" PRIu64 " draws in all), %" PRIu64 " others from 32-bit textures taken, %" PRIu64 " from other formats\n",
		in_shape_taken, in_shape, draws, taken_elsewhere, other_formats_taken);
	EXPECT_GE(in_shape, 300u) << "the generator made too few draws in the copy's shape";
	EXPECT_GE(in_shape_taken * 2, in_shape) << "fewer than half the random draws in the copy's shape were taken";
	EXPECT_EQ(other_formats_taken, 0u) << "a texture format other than 32-bit was copied as 32-bit texels";
}

// Stuntman builds the half-size copies of its road textures with a bilinear sprite that steps two texels a
// pixel from a whole-texel coordinate, so every pixel is the average of a 2x2 square. The weights come out at
// exactly a half, and the copy takes it. A bilinear sprite that steps one texel a pixel takes the texel itself
// when the coordinate sits half a texel in, and blends two otherwise.
static Draw Box32(int sx = 2, int sy = 2, int frac_u = -1, int frac_v = -1)
{
	Draw d = Blit32();
	d.tex1.MMAG = 1;
	d.tex1.MMIN = 1;
	d.sprites.clear();
	// A coordinate on a whole texel for a step of two, half a texel in for a step of one, as the game sends them.
	const int fu = frac_u >= 0 ? frac_u : sx == 2 ? 0 : 8;
	const int fv = frac_v >= 0 ? frac_v : sy == 2 ? 0 : 8;
	d.sprites.push_back({(2048 + 8) * 16, (2048 + 4) * 16, (2048 + 8 + 16) * 16, (2048 + 4 + 16) * 16,
		16 + fu, 16 + fv, 16 + fu + 16 * 16 * sx, 16 + fv + 16 * 16 * sy});
	return d;
}

TEST_F(GSSwPaletteBlockCopy, TakesTheBoxFilterStuntmanUses)
{
	Case c;
	c.seed = 500;
	c.draws.push_back(Box32());
	EXPECT_EQ(Compare(c), 1u);
	EXPECT_GT(m_ref_unswizzled, 0.0);
	EXPECT_EQ(m_got_unswizzled, 0.0);
}

// Stuntman's sprites: the destination's corner at the origin, the source's at texel (1, 1), and the sizes it uses.
TEST_F(GSSwPaletteBlockCopy, TakesEverySizeOfStuntmansBoxFilter)
{
	u32 seed = 510;
	for (const int w : {8, 16, 32, 64, 128})
	{
		for (const int h : {8, 16, 32, 64, 128})
		{
			Case c;
			c.seed = seed++;
			Draw d = Box32();
			d.tex0.TW = 10;
			d.tex0.TH = 10;
			d.tex0.TBW = 16;
			d.frame.FBW = 2;
			d.scissor.SCAX1 = 127;
			d.scissor.SCAY1 = 255;
			d.sprites.clear();
			d.sprites.push_back({2048 * 16, 2048 * 16, (2048 + w) * 16, (2048 + h) * 16, 16, 16, 16 + 2 * w * 16, 16 + 2 * h * 16});
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << w << "x" << h;
		}
	}
}

// Stuntman's rows of them: a run of consecutive box filters, each from a different place in a large texture.
TEST_F(GSSwPaletteBlockCopy, TakesARunOfBoxFilters)
{
	Case c;
	c.seed = 520;
	for (int i = 0; i < 24; i++)
	{
		Draw d = Box32();
		d.tex0.TBP0 = 0x2000 + i * 0x40;
		d.tex0.TW = 9;
		d.tex0.TH = 9;
		d.tex0.TBW = 8;
		c.draws.push_back(d);
	}
	EXPECT_EQ(Compare(c), 24u);
}

TEST_F(GSSwPaletteBlockCopy, TakesBoxFiltersFromEveryTextureAndFrameWidth)
{
	u32 seed = 540;
	for (int fbw = 1; fbw <= 16; fbw += 3)
	{
		for (const int tbw : {1, 3, 8, 16})
		{
			Case c;
			c.seed = seed++;
			Draw d = Box32();
			d.frame.FBW = fbw;
			d.tex0.TBW = tbw;
			d.scissor.SCAX1 = fbw * 64 - 1;
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << "fbw " << fbw << " tbw " << tbw;
		}
	}
}

// Each axis on its own step: both two, one two and one one, both one (the bilinear lookup whose weights are zero).
TEST_F(GSSwPaletteBlockCopy, TakesEachCombinationOfStepsPerAxis)
{
	u32 seed = 570;
	for (const int sx : {1, 2})
	{
		for (const int sy : {1, 2})
		{
			Case c;
			c.seed = seed++;
			c.draws.push_back(Box32(sx, sy));
			EXPECT_EQ(Compare(c), 1u) << "steps " << sx << "," << sy;
		}
	}
}

// The coordinate's fraction only matters through its top four bits: the window the copy takes is a sixteenth wide.
TEST_F(GSSwPaletteBlockCopy, TakesTheBilinearWindowAndNoOtherFraction)
{
	u32 seed = 580;
	for (const int sx : {1, 2})
	{
		for (int frac = 0; frac < 16; frac++)
		{
			Case c;
			c.seed = seed++;
			c.draws.push_back(Box32(sx, sx, frac, frac));
			const u64 want = frac == (sx == 2 ? 0 : 8) ? 1u : 0u;
			EXPECT_EQ(Compare(c), want) << "step " << sx << " fraction " << frac << "/16";
		}
	}
}

// The taps wrap and clamp on their own: the second sits past the texture's edge when the first is its last texel.
TEST_F(GSSwPaletteBlockCopy, WrapsTheSecondTapAtTheTexturesEdge)
{
	u32 seed = 620;
	u64 taken = 0;
	u64 draws = 0;
	for (const int wms : {CLAMP_REPEAT, CLAMP_CLAMP})
	{
		for (const int wmt : {CLAMP_REPEAT, CLAMP_CLAMP})
		{
			for (const int tw : {3, 4, 5, 8})
			{
				for (const int sx : {1, 2})
				{
					Case c;
					c.seed = seed++;
					Draw d = Box32(sx, sx);
					d.clamp.WMS = wms;
					d.clamp.WMT = wmt;
					d.tex0.TW = tw;
					d.tex0.TH = tw;
					d.sprites.clear();
					const int n = 1 << tw;
					const int f = sx == 2 ? 0 : 8;
					const auto sprite = [&](int dx, int dy, int w, int h, int u, int v) {
						d.sprites.push_back({(2048 + dx) * 16, (2048 + dy) * 16, (2048 + dx + w) * 16, (2048 + dy + h) * 16,
							u * 16 + f, v * 16 + f, (u + w * sx) * 16 + f, (v + h * sx) * 16 + f});
					};
					// Runs off the right and bottom edges, starts at the second texel, and starts past the edges.
					sprite(0, 0, 8, 8, n - 6, n - 6);
					sprite(8, 8, 8, 8, 1, 1);
					sprite(16, 0, 8, 8, n + 1, n + 3);
					sprite(0, 16, 16, 16, 2, 2);
					c.draws.push_back(d);
					taken += Compare(c);
					draws += c.draws.size();
				}
			}
		}
	}
	std::printf("32-bit texel copy, texture edges: %" PRIu64 " of %" PRIu64 " draws taken\n", taken, draws);
	EXPECT_GT(taken, 0u) << "no draw reached the copy, so no wrap was compared";
}

// A coordinate under a texel, where the scanline truncates toward zero and the box filter's first tap is texel -1:
// left to the rasterizer.
TEST_F(GSSwPaletteBlockCopy, RefusesABoxFilterThatStartsBeforeTheTexture)
{
	Case c;
	c.seed = 650;
	Draw d = Box32();
	d.sprites[0].u0 = 8;
	d.sprites[0].u1 = 8 + 16 * 32;
	c.draws.push_back(d);
	d.sprites[0].u0 = 0;
	d.sprites[0].u1 = 16 * 32;
	d.sprites[0].v0 = 4;
	d.sprites[0].v1 = 4 + 16 * 32;
	c.draws.push_back(d);
	EXPECT_EQ(Compare(c), 0u);
}

// The box filter over the draw's own texture: every texel is read before the first pixel is written.
TEST_F(GSSwPaletteBlockCopy, ReadsItsOwnTextureAsItWasBeforeTheBoxFilter)
{
	u32 seed = 660;
	for (const int shift : {0, 1, 2, 3, 8, 16, 33})
	{
		Case c;
		c.seed = seed++;
		Draw d = Box32();
		d.tex0.TBP0 = d.frame.FBP * 32 + shift;
		d.tex0.TBW = d.frame.FBW;
		d.tex0.TW = 7;
		d.tex0.TH = 7;
		d.sprites.clear();
		d.sprites.push_back({2048 * 16, 2048 * 16, (2048 + 32) * 16, (2048 + 32) * 16, 16, 16, 16 + 64 * 16, 16 + 64 * 16});
		d.sprites.push_back({(2048 + 8) * 16, (2048 + 8) * 16, (2048 + 24) * 16, (2048 + 24) * 16, 16, 16, 16 + 32 * 16, 16 + 32 * 16});
		c.draws.push_back(d);
		Compare(c);
		EXPECT_EQ(m_got_copies[0], 1u) << "shift " << shift;
	}
}

// With and without TCC, and every alpha: the average is taken on the texel's alpha like any channel, or the
// vertex's alpha stands in for it.
TEST_F(GSSwPaletteBlockCopy, AveragesAlphaLikeAnyChannelAndTakesTheVertexsWithoutTCC)
{
	u32 seed = 670;
	for (const bool tcc : {true, false})
	{
		for (const int alpha : {0x00, 0x44, 0x80, 0xff})
		{
			Case c;
			c.seed = seed++;
			Draw d = Box32();
			d.tex0.TCC = tcc;
			d.rgbaq.A = alpha;
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << "tcc " << tcc << " alpha " << alpha;
		}
	}
}

// Texture bytes at the extremes, where an average that rounded up, or carried between channels, would show: local
// memory is filled with a short cycle of two such words, so every 2x2 square mixes them.
TEST_F(GSSwPaletteBlockCopy, AveragesBytewiseWithoutCarryOrRoundUp)
{
	const u32 patterns[] = {0x00000000u, 0xffffffffu, 0x00ff00ffu, 0xff00ff00u, 0x01010101u, 0xfefefefeu, 0x80808080u, 0x7f7f7f7fu, 0x01ff02fdu};
	u32 seed = 700;
	for (const u32 a : patterns)
	{
		for (const u32 b : patterns)
		{
			Case c;
			c.seed = seed++;
			c.fill = {a, b, b, a, a, a, b, a};
			Draw d = Box32();
			d.sprites.clear();
			d.sprites.push_back({2048 * 16, 2048 * 16, (2048 + 8) * 16, (2048 + 8) * 16, 16, 16, 16 + 16 * 16, 16 + 16 * 16});
			c.draws.push_back(d);
			EXPECT_EQ(Compare(c), 1u) << std::hex << a << " " << b;
		}
	}
}

// Random cases with bilinear lookups, byte-compared like the others. Most draws in the copy's shape are taken: a
// refusal that is not forced by the shape is a sprite the copy cannot reproduce (an extent that is not a power of
// two, a texel outside the blocks the snapshot would hold, a scissor edge off the vector grid).
TEST_F(GSSwPaletteBlockCopy, MatchesTheRasterizerOnRandomBilinearDraws)
{
	constexpr u32 cases = 1500;
	u64 candidates = 0;
	u64 candidates_taken = 0;
	u64 box_taken = 0;
	u64 elsewhere_taken = 0;

	for (u32 seed = 9000; seed < 9000 + cases; seed++)
	{
		std::vector<bool> candidate;
		const Case c = RandomCase32Box(seed, candidate);
		Compare(c);
		for (size_t i = 0; i < c.draws.size() && i < m_got_copies.size(); i++)
		{
			const Draw& d = c.draws[i];
			if (candidate[i])
			{
				candidates++;
				candidates_taken += m_got_copies[i];
				if (d.tex1.MMAG == 1)
					box_taken += m_got_copies[i];
			}
			else
			{
				elsewhere_taken += m_got_copies[i];
			}
		}
		if (HasFailure())
			break;
	}

	std::printf("32-bit texel copy, bilinear: %" PRIu64 " of %" PRIu64 " random draws in the copy's shape taken, %" PRIu64 " of them bilinear, %" PRIu64 " others taken\n",
		candidates_taken, candidates, box_taken, elsewhere_taken);
	EXPECT_GE(candidates, 600u);
	EXPECT_GE(candidates_taken * 4, candidates);
	EXPECT_GE(box_taken, 150u) << "too few bilinear draws reached the copy";
}
