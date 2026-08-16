// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's gouraud colour and fog come off the SW scanline's WALK,
// replayed per fragment from the primitive's own edge, not off the GPU's
// interpolator.
//
// Why. The GS interpolates a block of pixels at a time (GSBlockWalk.h): the
// scanline seeds at the span's first pixel, adds a truncated per-lane offset
// inside the block and one truncated step per block, with the blocks aligned to
// absolute screen x. A pixel's value therefore depends on WHERE ITS SPAN
// STARTED, which the interpolator cannot know -- and gs-block measured that
// silicon does the same: the same gradient from eight different left edges gives
// eight different values at one absolute pixel. What the fragment can know is
// its primitive's left edge, from the same per-primitive payload the depth walk
// already reads; the span's first pixel on this row is then one fused
// multiply-add and a ceiling away, exactly as GSRasterizer::DrawTriangleSection
// computes it, and the walk from there is closed-form integer arithmetic.
//
// This file holds the C++ MIRROR of that fragment kernel -- op for op the GLSL
// in bin/resources/shaders/vulkan/tfx.glsl (PS_TILE_CWALK), written so the
// transliteration is mechanical -- and anchors it against the thing it claims
// to reproduce: the ARM64 scanline JIT itself, compiled here and run over
// spans, pixel by pixel. The row-seed half (span left, prestep, the two fused
// multiply-adds) is the depth walk's, already arbitrated by gs-zgrad; the
// captures (gs-interp, gs-block) arbitrate the whole pair end to end.
//
// ⚠️ If you change the kernel here, change the shader, and vice versa.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture, and
// the JIT is the oracle here -- the C++ reference scanline is a second
// transcription, not the thing games run.

#include "common/Pcsx2Defs.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace glsl_mirror_cw
{
	// ---- GLSL primitive shims ----
	// fcvtzs: truncate toward zero, saturating. GLSL int(float) is undefined out
	// of range, so the shader clamps first; the JIT saturates. Same answer.
	static inline int cw_fcvtzs(float v)
	{
		if (!(v == v))
			return 0;
		if (v >= 2147483648.0f)
			return INT_MAX;
		if (v <= -2147483648.0f)
			return INT_MIN;
		return static_cast<int>(v);
	}

	// The scanline holds colour and fog in 16-bit lanes; every add wraps there.
	// GLSL: (v << 16) >> 16.
	static inline int cw_wrap16(int v)
	{
		return static_cast<int>(static_cast<s16>(static_cast<u16>(v & 0xFFFF)));
	}

	// The span's first pixel on row py, and the prestep from the section's
	// reference vertex to it -- GSRasterizer::DrawTriangleSection's own
	// arithmetic: dy in fp32, a FUSED edge walk (the compiled rasterizer's fmla),
	// ceil, the scissor's left, prestep as one fp32 subtract. Identical to the
	// depth walk's (tile_zwalk_depth), which gs-zgrad arbitrated.
	static inline void cw_span(float edge_x, float dedge_x, float p0x, float p0y, float scissor_left, int py,
		int& left, float& dy, float& prestep)
	{
		dy = static_cast<float>(py) - p0y;
		const float lraw = std::fmaf(dedge_x, dy, edge_x);
		const float lx = std::fmax(std::ceil(lraw), scissor_left);
		left = static_cast<int>(lx);
		prestep = lx - p0x;
	}

	// The row seed for one lane: edge + dedge*dy + dscan*prestep, as the compiled
	// DrawTriangleSection forms it -- two fused multiply-adds, in that order
	// (objdump of the shipped build: fmla v6, v19, v0.s[0] then fmla v6, v23,
	// v1.s[0] on the colour vector). The scanline then truncates it to an integer
	// on the seven-fraction grid (fcvtzs) and packs the low 16 bits.
	static inline int cw_row_seed(float seed, float dedge, float dscan, float dy, float prestep)
	{
		return cw_fcvtzs(std::fmaf(dscan, prestep, std::fmaf(dedge, dy, seed)));
	}

	// The walk at pixel x, closed form (GSBlockWalk.h):
	//
	//     value(x) = trunc(seed) + B*trunc(g*W) + trunc(g*(m - s))
	//     B = (x - xb0) div W,  m = (x - xb0) mod W,  xb0 = x0 & ~(W-1),  s = x0 & (W-1)
	//
	// with x0 the span's first pixel, W the block width (eight untextured, four
	// textured), g the per-pixel gradient on the attribute's grid, and every
	// truncation an fcvtzs of one fp32 product -- the setup's Fmul + Fcvtzs on
	// m_shift / m_shift_next / m_shift_prev / m_block8. Modular in 16 bits, as
	// the lanes are.
	static inline int cw_walk_value(int seed_i, float g, int log2w, int left, int x)
	{
		const int wm = (1 << log2w) - 1;
		const int s = left & wm;
		const int d = x - (left - s); // x - xb0, >= 0 for a covered pixel
		const int B = d >> log2w;
		const int m = d & wm;
		const int q = cw_fcvtzs(g * static_cast<float>(1 << log2w));
		const int off = cw_fcvtzs(g * static_cast<float>(m - s));
		return cw_wrap16(seed_i + B * q + off);
	}

	// Colour: the scanline clamps a lane at zero after every four-pixel STEP
	// (Smax with zero in the generator's Step, not in Init), so a value that
	// dipped below zero mid-span walks on from zero. Cumulative clamping of a
	// running sum with the first vector unclamped is v(k) minus the most negative
	// value seen from the second vector on -- and the walk is monotone per lane
	// (each increment has the gradient's sign), so that minimum sits at an
	// endpoint: v(1) or v(k). One extra evaluation of the closed form.
	static inline int cw_walk_colour(int seed_i, float g, int log2w, int left, int x)
	{
		int v = cw_walk_value(seed_i, g, log2w, left, x);
		const int k = (x >> 2) - (left >> 2);
		if (k >= 1)
		{
			const int x1 = (left & ~3) + 4 + (x & 3);
			const int v1 = cw_walk_value(seed_i, g, log2w, left, x1);
			v = v - std::min(0, std::min(v1, v));
		}
		return v;
	}

	// Fog: no clamp anywhere in the walk.
	static inline int cw_walk_fog(int seed_i, float g, int log2w, int left, int x)
	{
		return cw_walk_value(seed_i, g, log2w, left, x);
	}

	// What the scanline stores or multiplies: the lane read as unsigned 16-bit,
	// shifted down by the seven fraction bits (srl16<7> in every consumer).
	static inline int cw_c8(int v)
	{
		return (v & 0xFFFF) >> 7;
	}
} // namespace glsl_mirror_cw

// ---------------------------------------------------------------------------
// The mirror against the JIT.
// ---------------------------------------------------------------------------

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSState.h"
#include "common/HostSys.h"

#ifndef _WIN32
#include <sys/mman.h>
#else
#include "common/RedtapeWindows.h"
#endif

namespace
{
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
using SetupPrimPtr = void (*)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);

// One row of PSMCT32 at FBW=1.
constexpr int kRowPixels = 64;

enum class Variant
{
	Untextured,    // TFX_NONE: eight-pixel block, the split walk on a four-lane host
	Textured,      // TFX_MODULATE against a 0x80 texel, which is the identity: four-pixel block
	UntexturedFog, // TFX_NONE with fog: colour and fog walks, blended by the scanline
};

class TileCWalkJitTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 8 * kCodeSlotSize;

	static void SetUpTestSuite()
	{
#ifdef _WIN32
		s_code = static_cast<u8*>(VirtualAlloc(nullptr, kCodeBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#elif defined(__APPLE__)
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#else
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#endif
		s_code_used = 0;
		s_mem = new GSLocalMemory();
	}

	static void TearDownTestSuite()
	{
		delete s_mem;
		s_mem = nullptr;
		if (s_code)
		{
#ifdef _WIN32
			VirtualFree(s_code, 0, MEM_RELEASE);
#else
			munmap(s_code, kCodeBufferSize);
#endif
			s_code = nullptr;
		}
	}

	static u8* Slot()
	{
		if (!s_code || s_code_used + kCodeSlotSize > kCodeBufferSize)
			return nullptr;
		u8* slot = s_code + s_code_used;
		s_code_used += kCodeSlotSize;
		return slot;
	}

	static DrawScanlinePtr CompileScanline(GSScanlineSelector sel)
	{
		u8* slot = Slot();
		if (!slot)
			return nullptr;
		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSDrawScanlineCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));
		return reinterpret_cast<DrawScanlinePtr>(const_cast<u8*>(cg.GetCode()));
	}

	// The reduced key GetScanlineGlobalData builds, so this is the same setup
	// variant a real draw would compile.
	static SetupPrimPtr CompileSetup(GSScanlineSelector full)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.iip = full.iip;
		sel.tfx = full.tfx;
		sel.tcc = full.tcc;
		sel.fst = full.fst;
		sel.fge = full.fge;
		sel.prim = full.prim;
		sel.fb = full.fb;
		sel.zb = full.zb;
		sel.zoverflow = full.zoverflow;
		sel.zequal = full.zequal;
		sel.notest = full.notest;

		u8* slot = Slot();
		if (!slot)
			return nullptr;
		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSSetupPrimCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));
		return reinterpret_cast<SetupPrimPtr>(const_cast<u8*>(cg.GetCode()));
	}

	// A gouraud triangle into PSMCT32, nothing tested, nothing blended: the
	// stored pixel is the walked colour (through the identity modulate for the
	// textured variant), so nothing between the DDA and the readback can hide a
	// difference.
	static GSScanlineSelector MakeSelector(Variant variant)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 1;
		sel.fwrite = 1;
		sel.colclamp = 1;
		switch (variant)
		{
			case Variant::Untextured:
				sel.tfx = TFX_NONE;
				break;
			case Variant::UntexturedFog:
				sel.tfx = TFX_NONE;
				sel.fge = 1;
				break;
			case Variant::Textured:
				sel.tfx = TFX_MODULATE;
				sel.tcc = 1;
				sel.fst = 1;
				sel.ltf = 0;
				sel.tlu = 0;
				sel.tw = 0;
				break;
		}
		return sel;
	}

	static const GSPixelOffset4* GetOffsets()
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 1;
		frame.PSM = PSMCT32;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 256;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(int x, int y)
	{
		return GSLocalMemory::m_psm[PSMCT32].info.pa(x, y, 0, 1);
	}

	struct Span
	{
		float seed[4]; // r, g, b, a on the scanline's grid (colour * 128), at the span's first pixel
		float grad[4]; // per pixel, same grid
		float fseed;   // fog byte * 128 at the span's first pixel
		float fgrad;
	};

	// Runs one span through the JIT pair for `variant`; fills px[] with what the
	// scanline stored across the whole row (untouched pixels read as the fill).
	static bool RunJit(Variant variant, const Span& span, int left, int pixels, u32 fogcol, u32 px[kRowPixels])
	{
		const int slot = static_cast<int>(variant);
		if (!s_setup[slot])
		{
			s_sel[slot] = MakeSelector(variant);
			s_setup[slot] = CompileSetup(s_sel[slot]);
			s_draw[slot] = CompileScanline(s_sel[slot]);
		}
		if (!s_setup[slot] || !s_draw[slot])
			return false;

		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < kRowPixels; x++)
			vm32[PixelAddr(x, 0)] = 0xEEEEEEEEu;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};
		alignas(32) u32 tex[4] = {0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u};

		global.sel = s_sel[slot];
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		if (variant == Variant::Textured)
		{
			// min = max = 0 with an all-zero mask is CLAMP to texel zero on both
			// axes, so every lane fetches tex[0] whatever the coordinate is -- and
			// modulating by 0x80 is the identity on the stored byte.
			global.tex[0] = tex;
			global.t.min = GSVector4i::zero();
			global.t.max = GSVector4i::zero();
			global.t.mask = GSVector4i::zero();
		}
		if (variant == Variant::UntexturedFog)
		{
			global.frb = fogcol & 0x00ff00ff;
			global.fga = (fogcol >> 8) & 0x00ff00ff;
		}
		local.gd = &global;

		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		dscan.c = GSVector4(span.grad[0], span.grad[1], span.grad[2], span.grad[3]);
		dscan.t.w = span.fgrad;

		s_setup[slot](vertex, index, dscan, local);

		GSVertexSW scan = GSVertexSW::zero();
		scan.c = GSVector4(span.seed[0], span.seed[1], span.seed[2], span.seed[3]);
		scan.t.w = span.fseed;

		s_draw[slot](pixels, left, 0, scan, local);

		for (int x = 0; x < kRowPixels; x++)
			px[x] = vm32[PixelAddr(x, 0)];
		return true;
	}

	// The mirror's answer for the same span at pixel x: the stored byte per
	// channel, and the 16-bit fog lane.
	static void Mirror(Variant variant, const Span& span, int left, int x, int c8[4], int& f16)
	{
		using namespace glsl_mirror_cw;
		const int log2w = (variant == Variant::Textured) ? 2 : 3;
		for (int ch = 0; ch < 4; ch++)
			c8[ch] = cw_c8(cw_walk_colour(cw_fcvtzs(span.seed[ch]), span.grad[ch], log2w, left, x));
		f16 = cw_walk_fog(cw_fcvtzs(span.fseed), span.fgrad, log2w, left, x);
	}

	// What the scanline stores for a walked lane: the untextured path packs the
	// nine-bit srl16<7> result with an unsigned-saturating narrow (Sqxtun); the
	// modulate path multiplies (c8 << 7) as a SIGNED lane, so a value that ran
	// off the top of the grid saturates to zero there instead. Both regimes are
	// unreachable from any real plane (the walk stays within a unit of a colour
	// in 0..255) and are pinned only so the fuzz below can include them.
	static u32 StoredByte(Variant variant, int c8)
	{
		if (variant == Variant::Textured)
			return (c8 >= 256) ? 0u : static_cast<u32>(c8);
		return static_cast<u32>(std::min(c8, 255));
	}

	// The scanline's fog blend, on the lane as the walk left it (nine bits wide
	// where the walk ran off the top, since srl16<7> comes before the fog and
	// the saturating pack after): frb.lerp16<0>(rb, f) = fog + (((c - fog) * f)
	// >> 15), the shift arithmetic. The pack (Sqxtun) then saturates to a byte.
	// Alpha takes no fog and only the pack.
	static u32 FogBlend(int c9, u32 fog8, int f16)
	{
		const int d = (c9 - static_cast<int>(fog8)) * f16;
		return Sat8(static_cast<int>(fog8) + (d >> 15));
	}

	static u32 Sat8(int v)
	{
		return static_cast<u32>(std::clamp(v, 0, 255));
	}

	static u32 Pixel(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

	// The envelope the mirror is exact in: every 16-bit lane the scanline steps
	// through from the second vector on stays inside int16, and so does every
	// covered pixel. A real plane never leaves it (colours are at most 255 and
	// the walk tracks its plane within a unit or two), and outside it the
	// scanline's per-step clamp acts on WRAPPED values, which is garbage nobody
	// draws and the closed form does not chase. Fog has no clamp, so it is
	// modular everywhere and needs no such guard.
	static bool ColourWalkStaysInInt16(const Span& span, int log2w, int left, int pixels)
	{
		const int wm = (1 << log2w) - 1;
		const int s = left & wm;
		const int xb0 = left - s;
		const int first = std::min(left, (left & ~3) + 4);
		const int last = std::max(left + pixels - 1, ((left + pixels - 1) | 3));
		for (int ch = 0; ch < 4; ch++)
		{
			const s64 seed = glsl_mirror_cw::cw_fcvtzs(span.seed[ch]);
			const s64 q = glsl_mirror_cw::cw_fcvtzs(span.grad[ch] * static_cast<float>(1 << log2w));
			for (int x = first; x <= last; x++)
			{
				const int d = x - xb0;
				const s64 v = seed + static_cast<s64>(d >> log2w) * q +
				              glsl_mirror_cw::cw_fcvtzs(span.grad[ch] * static_cast<float>((d & wm) - s));
				if (v < -32768 || v > 32767)
					return false;
			}
		}
		return true;
	}

	static void ExpectSpan(Variant variant, const Span& span, int left, int pixels, u32 fogcol, const char* what)
	{
		u32 px[kRowPixels];
		ASSERT_TRUE(RunJit(variant, span, left, pixels, fogcol, px)) << what << ": JIT compile failed";

		for (int x = left; x < left + pixels; x++)
		{
			int c8[4];
			int f16;
			Mirror(variant, span, left, x, c8, f16);

			u32 want;
			if (variant == Variant::UntexturedFog)
			{
				// Fog is a lerp on the stored byte, and the fog lane's 16-bit value
				// is the walk's; the JIT stores the blend, so the walk is checked
				// through it. Alpha takes no fog.
				const u32 fr = fogcol & 0xff, fg = (fogcol >> 8) & 0xff, fb = (fogcol >> 16) & 0xff;
				want = Pixel(FogBlend(c8[0], fr, f16), FogBlend(c8[1], fg, f16), FogBlend(c8[2], fb, f16),
					Sat8(c8[3]));
			}
			else
			{
				want = Pixel(StoredByte(variant, c8[0]), StoredByte(variant, c8[1]),
					StoredByte(variant, c8[2]), StoredByte(variant, c8[3]));
			}
			EXPECT_EQ(px[x], want) << what << ": pixel " << x << " (left " << left << ", " << pixels << " px)"
									<< std::hex << " got " << px[x] << " want " << want << std::dec
									<< " seed {" << span.seed[0] << ", " << span.seed[1] << ", " << span.seed[2] << ", " << span.seed[3]
									<< "} grad {" << span.grad[0] << ", " << span.grad[1] << ", " << span.grad[2] << ", " << span.grad[3]
									<< "} fog seed " << span.fseed << " grad " << span.fgrad << " -> f16 " << f16
									<< " c8 {" << c8[0] << ", " << c8[1] << ", " << c8[2] << ", " << c8[3] << "}"
									<< std::hex << " fogcol " << fogcol << std::dec;
		}
	}

	static GSScanlineSelector s_sel[3];
	static SetupPrimPtr s_setup[3];
	static DrawScanlinePtr s_draw[3];
	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

GSScanlineSelector TileCWalkJitTest::s_sel[3] = {};
SetupPrimPtr TileCWalkJitTest::s_setup[3] = {nullptr, nullptr, nullptr};
DrawScanlinePtr TileCWalkJitTest::s_draw[3] = {nullptr, nullptr, nullptr};
u8* TileCWalkJitTest::s_code = nullptr;
size_t TileCWalkJitTest::s_code_used = 0;
GSLocalMemory* TileCWalkJitTest::s_mem = nullptr;
} // namespace

// The shape the shader had before this path: the interpolator's value at the
// pixel, truncated. It disagrees with the JIT wherever a block boundary falls
// inside the span, which is what this suite exists to make loud.
TEST_F(TileCWalkJitTest, TheInterpolatorIsNotTheWalk)
{
	// A gradient of 0.9 units per pixel from a seed half a unit under a stored
	// byte's boundary (30080 = 235 << 7), starting at pixel 5. The plane crosses
	// the boundary before pixel 6; the walk seeds at trunc(30079.5) = 30079 and
	// adds trunc(0.9 * 1) = 0 there, so it stores 234 where the plane says 235.
	// The stored byte only shows the walk where the units cross a multiple of
	// 128, which is why the residue is sparse and why a seed placed just under a
	// boundary is what makes it deterministic here.
	const Span span = {{30079.5f, 30079.5f, 30079.5f, 30079.5f}, {0.9f, 0.9f, 0.9f, 0.9f}, 0.0f, 0.0f};
	u32 px[kRowPixels];
	ASSERT_TRUE(RunJit(Variant::Untextured, span, 5, 24, 0, px));

	int interpolator_hits = 0, walk_hits = 0;
	for (int x = 5; x < 29; x++)
	{
		const int plane = static_cast<int>(30079.5f + 0.9f * static_cast<float>(x - 5)) >> 7;
		int c8[4];
		int f16;
		Mirror(Variant::Untextured, span, 5, x, c8, f16);
		interpolator_hits += (px[x] & 0xff) == static_cast<u32>(std::min(plane, 255)) ? 1 : 0;
		walk_hits += (px[x] & 0xff) == static_cast<u32>(std::min(c8[0], 255)) ? 1 : 0;
	}
	EXPECT_EQ(walk_hits, 24);
	EXPECT_LT(interpolator_hits, 24) << "the block walk and the plane agree on every pixel here; pick a span that separates them";
	EXPECT_EQ(px[6] & 0xff, 234u);
}

// Every left lane and every block phase, both widths, both signs of gradient,
// with the ramp landing on and off the stored byte's boundaries.
TEST_F(TileCWalkJitTest, DirectedSpans)
{
	const float grads[] = {0.0f, 1.0f, 0.5f, 0.3f, 60.3f, 127.9f, 128.0f, 128.1f, 255.0f, 1000.7f, -1.0f, -0.3f, -60.3f, -128.1f, -1000.7f};
	int n = 0;
	for (Variant variant : {Variant::Untextured, Variant::Textured})
	{
		for (float g : grads)
		{
			for (int left = 0; left < 8; left++)
			{
				for (int pixels : {1, 3, 4, 5, 8, 9, 13, 24, 40})
				{
					if (left + pixels > kRowPixels)
						continue;
					// A seed that keeps a descending walk above zero and an
					// ascending one below the top over the span, plus a fraction;
					// the steepest gradients on the longest spans leave the
					// envelope and are skipped rather than reduced.
					const float base = (g < 0.0f) ? 30000.0f : 1000.0f;
					const Span span = {{base + 0.7f, base + 3.2f, base + 127.9f, base + 64.0f}, {g, g * 0.5f, g * 0.25f, g * 0.125f}, 0.0f, 0.0f};
					if (!ColourWalkStaysInInt16(span, variant == Variant::Textured ? 2 : 3, left, pixels))
						continue;
					char what[128];
					std::snprintf(what, sizeof(what), "%s g=%g", variant == Variant::Textured ? "textured" : "untextured", g);
					ExpectSpan(variant, span, left, pixels, 0, what);
					n++;
				}
			}
		}
	}
	EXPECT_GT(n, 1000);
}

// Random spans over the whole realistic envelope -- seeds anywhere on the grid,
// gradients up to a colour's whole range per pixel, every left phase and
// length -- and past it into negative seeds, where the scanline's post-step
// clamp fires and the mirror's correction has to say the same thing.
TEST_F(TileCWalkJitTest, FuzzAgainstTheJit)
{
	std::mt19937_64 rng(0x5EED0C01u);
	auto uni = [&](float lo, float hi) {
		return lo + (hi - lo) * static_cast<float>(rng() % 1000003u) / 1000002.0f;
	};

	int accepted = 0;
	for (int i = 0; i < 12000; i++)
	{
		const Variant variant = (i % 3 == 0) ? Variant::Textured : Variant::Untextured;
		const int left = static_cast<int>(rng() % 24);
		const int pixels = 1 + static_cast<int>(rng() % 40);
		Span span;
		for (int ch = 0; ch < 4; ch++)
		{
			// Every fourth span dips into negative seeds and steeper gradients:
			// the clamp regime, which the mirror must reproduce even though no
			// real plane reaches it. Spans that would push a lane past the
			// sixteen-bit range are regenerated (see ColourWalkStaysInInt16).
			const bool wild = (i % 4 == 3);
			span.seed[ch] = wild ? uni(-3000.0f, 32700.0f) : uni(0.0f, 32640.0f);
			span.grad[ch] = wild ? uni(-4000.0f, 4000.0f) : uni(-700.0f, 700.0f);
		}
		span.fseed = 0.0f;
		span.fgrad = 0.0f;
		if (!ColourWalkStaysInInt16(span, variant == Variant::Textured ? 2 : 3, left, pixels))
			continue;
		char what[64];
		std::snprintf(what, sizeof(what), "fuzz %d %s", i, variant == Variant::Textured ? "textured" : "untextured");
		ExpectSpan(variant, span, left, pixels, 0, what);
		accepted++;
	}
	EXPECT_GT(accepted, 6000);
}

// Fog rides the same walk without the clamp, and reaches the pixel through the
// scanline's own lerp; the colour is held flat so the fog lane is what varies.
TEST_F(TileCWalkJitTest, FogWalksTheBlockToo)
{
	std::mt19937_64 rng(0xF06F06u);
	auto uni = [&](float lo, float hi) {
		return lo + (hi - lo) * static_cast<float>(rng() % 1000003u) / 1000002.0f;
	};

	const float fgrads[] = {0.0f, 1.0f, 0.5f, 60.3f, 128.1f, 1000.7f, -0.3f, -60.3f, -1000.7f};
	int n = 0;
	for (float fg : fgrads)
	{
		for (int left = 0; left < 8; left++)
		{
			for (int pixels : {1, 4, 5, 9, 24})
			{
				const float fbase = (fg < 0.0f) ? 30000.0f : 800.0f;
				const Span span = {{200.0f * 128.0f, 40.0f * 128.0f, 128.0f * 128.0f, 77.0f * 128.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, fbase + 0.6f, fg};
				char what[64];
				std::snprintf(what, sizeof(what), "fog g=%g", fg);
				ExpectSpan(Variant::UntexturedFog, span, left, pixels, 0x00203040u, what);
				n++;
			}
		}
	}
	for (int i = 0; i < 1500; i++)
	{
		const int left = static_cast<int>(rng() % 24);
		const int pixels = 1 + static_cast<int>(rng() % 40);
		Span span;
		for (int ch = 0; ch < 4; ch++)
		{
			span.seed[ch] = uni(0.0f, 32640.0f);
			span.grad[ch] = uni(-500.0f, 500.0f);
		}
		span.fseed = uni(0.0f, 32640.0f);
		span.fgrad = uni(-500.0f, 500.0f);
		if (!ColourWalkStaysInInt16(span, 3, left, pixels))
			continue;
		char what[64];
		std::snprintf(what, sizeof(what), "fog fuzz %d", i);
		ExpectSpan(Variant::UntexturedFog, span, left, pixels, static_cast<u32>(rng()), what);
		n++;
	}
	EXPECT_GT(n, 300);
}

// ---------------------------------------------------------------------------
// The plane export: GSComputeTriangleZPlane hands the Tile renderer the colour
// and fog lanes the rasterizer's own setup produced, per section.
// ---------------------------------------------------------------------------

namespace
{
GSVertexSW MakeVertex(float x, float y, u32 z, u32 r, u32 g, u32 b, u32 a, u32 fog)
{
	GSVertexSW v = GSVertexSW::zero();
	v.p = GSVector4(x, y, 0.0f, 0.0f);
	v.p.F64[1] = static_cast<double>(z);
	v.c = GSVector4(static_cast<float>(r << 7), static_cast<float>(g << 7), static_cast<float>(b << 7), static_cast<float>(a << 7));
	v.t = GSVector4(0.0f, 0.0f, 0.0f, static_cast<float>(fog << 7));
	return v;
}
} // namespace

TEST(TileCWalkPlane, ExportsTheSectionSeedsAndTheSharedGradients)
{
	// A right triangle, colour ramping 0 -> 255 along x on the top edge and
	// constant down the left edge; fog the other way round.
	const GSVertexSW v[3] = {
		MakeVertex(0.0f, 0.0f, 100, 0, 10, 20, 30, 0),
		MakeVertex(128.0f, 0.0f, 100, 255, 10, 20, 30, 0),
		MakeVertex(0.0f, 128.0f, 100, 0, 10, 20, 30, 255),
	};
	const GSVector4 fscissor_y = GSVector4(0.0f, 448.0f, 0.0f, 448.0f);
	isa_native::GSTileZPlane zp;
	ASSERT_TRUE(isa_native::GSComputeTriangleZPlane(v, fscissor_y, zp));
	ASSERT_EQ(zp.nsections, 1); // flat top: one section

	// Per pixel: 255 * 128 / 128 = 255 units on red; nothing on the others.
	EXPECT_FLOAT_EQ(zp.dscan_c.x, 255.0f);
	EXPECT_FLOAT_EQ(zp.dscan_c.y, 0.0f);
	EXPECT_FLOAT_EQ(zp.dscan_c.z, 0.0f);
	EXPECT_FLOAT_EQ(zp.dscan_c.w, 0.0f);
	// Per row at constant x: red is constant down the left edge; fog climbs.
	EXPECT_FLOAT_EQ(zp.dedge_c.x, 0.0f);
	EXPECT_FLOAT_EQ(zp.dedge_f, 255.0f);
	EXPECT_FLOAT_EQ(zp.dscan_f, 0.0f);
	// The section seeds from the left vertex of the top edge: its exact colour.
	EXPECT_FLOAT_EQ(zp.sec[0].cseed.x, 0.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].cseed.y, 10.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].cseed.z, 20.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].cseed.w, 30.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].fseed, 0.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].p0x, 0.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].p0y, 0.0f);

	// And the walk on that plane, through the mirror: row 64's span starts at
	// x = 0 (the left edge is vertical), red is 255 units per pixel, so pixel x
	// stores (255 * x) >> 7 exactly -- the block walk is the identity on an
	// integer gradient.
	using namespace glsl_mirror_cw;
	int left;
	float dy, prestep;
	cw_span(zp.sec[0].edge_x, zp.sec[0].dedge_x, zp.sec[0].p0x, zp.sec[0].p0y, 0.0f, 64, left, dy, prestep);
	EXPECT_EQ(left, 0);
	const int seed = cw_row_seed(zp.sec[0].cseed.x, zp.dedge_c.x, zp.dscan_c.x, dy, prestep);
	EXPECT_EQ(seed, 0);
	for (int x = 0; x < 64; x++)
		EXPECT_EQ(cw_c8(cw_walk_colour(seed, zp.dscan_c.x, 3, left, x)), (255 * x) >> 7) << "x=" << x;
	const int fseed = cw_row_seed(zp.sec[0].fseed, zp.dedge_f, zp.dscan_f, dy, prestep);
	EXPECT_EQ(fseed, 64 * 255);
}

TEST(TileCWalkPlane, TwoSectionsSeedFromTheirOwnVertices)
{
	// Three distinct rows: the second section seeds from the middle vertex.
	const GSVertexSW v[3] = {
		MakeVertex(10.0f, 0.0f, 100, 200, 0, 0, 0, 5),
		MakeVertex(0.0f, 50.0f, 100, 100, 0, 0, 0, 6),
		MakeVertex(60.0f, 100.0f, 100, 0, 0, 0, 0, 7),
	};
	const GSVector4 fscissor_y = GSVector4(0.0f, 448.0f, 0.0f, 448.0f);
	isa_native::GSTileZPlane zp;
	ASSERT_TRUE(isa_native::GSComputeTriangleZPlane(v, fscissor_y, zp));
	ASSERT_EQ(zp.nsections, 2);
	EXPECT_FLOAT_EQ(zp.sec[0].cseed.x, 200.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[0].fseed, 5.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[1].cseed.x, 100.0f * 128.0f);
	EXPECT_FLOAT_EQ(zp.sec[1].fseed, 6.0f * 128.0f);
	EXPECT_EQ(zp.sec[1].top, 50);
	// The gradients are one plane, shared by both sections.
	EXPECT_NE(zp.dscan_c.x, 0.0f);
	EXPECT_NE(zp.dedge_c.x, 0.0f);
}

#endif // ARCH_ARM64

// ---------------------------------------------------------------------------
// The kernel on its own, no JIT: pins that hold on every host.
// ---------------------------------------------------------------------------

TEST(TileCWalkKernel, ClosedFormIsTheBlockWalk)
{
	using namespace glsl_mirror_cw;
	// g = 60.3, seed 3.7, span from x0 = 5 (block 8: s = 5, xb0 = 0).
	const int seed = cw_fcvtzs(3.7f);
	EXPECT_EQ(seed, 3);
	// Inside the first block, m - s runs -5..2 for x = 0..7; covered pixels start at 5.
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 3, 5, 5), 3);                              // trunc(0)
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 3, 5, 6), 3 + 60);                         // trunc(60.3)
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 3, 5, 7), 3 + 120);                        // trunc(120.6)
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 3, 5, 8), 3 + 482 + cw_fcvtzs(-5 * 60.3f)); // next block: q = trunc(482.4), m - s = -5
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 3, 5, 16), 3 + 2 * 482 + cw_fcvtzs(-5 * 60.3f));
	// Textured: block four.
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 2, 5, 5), 3);
	EXPECT_EQ(cw_walk_value(seed, 60.3f, 2, 5, 8), 3 + cw_fcvtzs(4 * 60.3f) + cw_fcvtzs(-1 * 60.3f));
}

TEST(TileCWalkKernel, ColourClampsAtZeroOnlyAfterAStep)
{
	using namespace glsl_mirror_cw;
	// A descending walk that crosses zero: the first vector keeps its negative
	// lanes (the JIT's Init has no clamp), every later vector clamps.
	const int seed = 100;
	const float g = -60.0f;
	// x = 2 in the first vector (left 0): 100 + trunc(-120) = -20, kept.
	EXPECT_EQ(cw_walk_colour(seed, g, 3, 0, 2), -20);
	EXPECT_EQ(cw_c8(-20), (0x10000 - 20) >> 7);
	// x = 4, second vector: -140, clamped to 0.
	EXPECT_EQ(cw_walk_colour(seed, g, 3, 0, 4), 0);
	EXPECT_EQ(cw_walk_colour(seed, g, 3, 0, 40), 0);
	// An ascending walk from below zero: the second vector's clamp lifts the
	// rest of the walk by what it discarded.
	EXPECT_EQ(cw_walk_colour(-500, 60.0f, 3, 0, 4), 0);        // -500 + 240 = -260 -> 0
	EXPECT_EQ(cw_walk_colour(-500, 60.0f, 3, 0, 8), 240);      // walks on from 0: + (480 - 240)
	EXPECT_EQ(cw_walk_fog(-500, 60.0f, 3, 0, 8), -500 + 480);  // fog: no clamp
}

TEST(TileCWalkKernel, LanesWrapAtSixteenBits)
{
	using namespace glsl_mirror_cw;
	EXPECT_EQ(cw_wrap16(32768), -32768);
	EXPECT_EQ(cw_wrap16(-32769), 32767);
	EXPECT_EQ(cw_wrap16(65536 + 5), 5);
	EXPECT_EQ(cw_c8(-1), 511);
	EXPECT_EQ(cw_c8(32640), 255);
	EXPECT_EQ(cw_c8(127), 0);
	EXPECT_EQ(cw_c8(128), 1);
}

TEST(TileCWalkKernel, SpanLeftIsTheRasterizersCeilAndScissor)
{
	using namespace glsl_mirror_cw;
	int left;
	float dy, prestep;
	// Edge x = 3.25 at the reference row 10, sloping 0.5 per row: row 12 -> 4.25 -> ceil 5.
	cw_span(3.25f, 0.5f, 3.25f, 10.0f, 0.0f, 12, left, dy, prestep);
	EXPECT_EQ(left, 5);
	EXPECT_FLOAT_EQ(dy, 2.0f);
	EXPECT_FLOAT_EQ(prestep, 5.0f - 3.25f);
	// The scissor's left wins where the edge is left of it.
	cw_span(3.25f, 0.5f, 3.25f, 10.0f, 8.0f, 12, left, dy, prestep);
	EXPECT_EQ(left, 8);
	EXPECT_FLOAT_EQ(prestep, 8.0f - 3.25f);
	// An edge landing exactly on an integer starts there (ceil is the identity).
	cw_span(4.0f, 0.0f, 4.0f, 0.0f, 0.0f, 3, left, dy, prestep);
	EXPECT_EQ(left, 4);
	EXPECT_FLOAT_EQ(prestep, 0.0f);
}
