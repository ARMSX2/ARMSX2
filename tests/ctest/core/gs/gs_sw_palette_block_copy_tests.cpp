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
			r.ZBUF.ZMSK = 1;
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
			const std::vector<u8> vm = RandomMemory(c.seed);
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

// Without TCC the texel's colour goes out with the vertex's alpha.
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

// A mask that keeps every bit writes nothing; both arms leave memory alone.
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
