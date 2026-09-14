// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The formed texture coordinate saturates into a signed 12.4 field.
//
// gs-mag1 (SCPH-30001, 2026-09-13) sweeps one STQ triangle's coordinate over
// thirteen magnitudes, one texel to 65,536, and reads the sampled coordinate back
// through a naming texture. Every arm at 2,049 texels and above writes ONE word on
// all 22,528 of its pixels -- the same word at 2,049 as at 65,537, with the column
// ramp visible in the nominal and not in the reading:
//
//   LINEAR, positive    texel 2,047 at weight 15   (0x7FFF sixteenths)
//   NEAREST, positive   texel 2,047, no weight
//   negative            -2,048.0
//
// The exact plane clamped into [-2048, +2047.9375] reproduces 3,072 of 3,072
// columns, 270,336 readings; wrapping reproduces 8. It is not a shared exponent
// (the reading would move with the column and does not move at all) and not an
// eleven-bit field overflowing (that lands on texel 1 at mag-11, and the console
// names texel 2,047). The V lane saturates on its own -- v-12 reads the top of the
// field on V with U at its nominal, and mag-16 moves V while U stays pinned.
//
// ★ The clamp is on the FORMED SIXTEENTH, not on the coordinate before the linear
// filter's half-texel step: mag-11 under LINEAR reads weight 15, and clamping
// first and straddling afterwards would read weight 7. So it belongs after the
// half-texel step and the truncation to sixteenths, and before the tap pair is
// split out and the wrap or clamp addressing runs.
//
// Our renderer had no field at all: above 2,048 texels the 16.16 coordinate ran on
// and the REPEAT mask wrapped it, which agreed with the console on 0.39% of the
// deep arms' words.
//
// The UV register cannot reach the field -- it is 10.4, so it tops out at 1,023.9375
// texels -- so the rule is unobservable there by construction rather than excluded.
//
// The cases below are gs-mag1's deep arms in miniature: a 64-texel naming texture
// under REPEAT, an STQ triangle with Q exactly one, the coordinate ramping at
// gs-mag1's own 13/256 of a texel per pixel from just past the field's edge. Every
// pixel must read the same word, and that word must be the one the console wrote.
//
// The draw goes through the real GSRasterizer with the real generated setup and
// scanline, so the vertices are the input and the framebuffer is the output.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#if defined(ARCH_ARM64) && !defined(MULTI_ISA_SHARED_COMPILATION)

#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSState.h"
#include "common/HostSys.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/mman.h>
#else
#include "common/RedtapeWindows.h"
#endif

namespace
{
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
using SetupPrimPtr = void (*)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);

// The naming texture is 64 texels on a side, which is `1 << (sel.tw + 3)` at tw 3.
constexpr int kTex = 64;

// The rectangle the draw covers. Small on purpose: the reading is that every pixel
// of a saturated arm carries the same word, so sixteen columns say it as well as
// gs-mag1's 256 do.
constexpr int kSize = 16;

// gs-mag1's own ramp: thirteen 256ths of a texel per pixel, so the nominal moves
// across the region while the reading does not.
constexpr float kRamp = 13.0f / 256.0f;

// The V the arms hold, at a texel centre so the vertical filter weight is zero and
// the V lane is a control rather than a second variable.
constexpr float kRow = 8.5f;

/// texel (x, y) of the namer: red names the column, green the row, blue the pair,
/// so no two texels share a word and a blend of two of them is legible.
constexpr u32 Texel(int x, int y)
{
	return 0x80000000u | (static_cast<u32>((x ^ y) * 4) << 16)
	       | (static_cast<u32>(y * 4) << 8) | static_cast<u32>(x * 4);
}

/// One stage of the console's bilinear blend, per channel: `(a*(16-w) + b*w) >> 4`,
/// floored. gs-lerp3 pins the shape at 100.000% on 98,240 readings; it is quoted
/// here only to name the texel PAIR and the weight the coordinate landed on.
u32 Lerp16(u32 a, u32 b, int w)
{
	u32 out = 0;

	for (int ch = 0; ch < 4; ch++)
	{
		const u32 ca = (a >> (ch * 8)) & 0xff;
		const u32 cb = (b >> (ch * 8)) & 0xff;

		out |= ((ca * static_cast<u32>(16 - w) + cb * static_cast<u32>(w)) >> 4) << (ch * 8);
	}

	return out;
}

class SwCoordinateFieldTest : public ::testing::Test
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

	static SetupPrimPtr CompileSetup(GSScanlineSelector full)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.iip = full.iip;
		sel.tfx = full.tfx;
		sel.tcc = full.tcc;
		sel.fst = full.fst;
		sel.uvwalk = full.uvwalk;
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

	// DECAL with TCC on: the stored pixel is the sampled texel and nothing else, so
	// the framebuffer reports the coordinate directly. gs-mag1's arms are the same
	// function against the same kind of namer.
	static GSScanlineSelector MakeSelector(bool linear)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		// The STQ road with Q identically one, which is what GSRendererSW hands a
		// triangle whose Q does not vary: the scanline reads a 16.16 integer and
		// never touches the perspective reciprocal.
		sel.fst = 1;
		sel.uvwalk = 0;
		sel.ltf = linear ? 1 : 0;
		sel.tlu = 0;
		sel.tw = 3; // 1 << (tw + 3) == 64 texels wide
		sel.wms = CLAMP_REPEAT;
		sel.wmt = CLAMP_REPEAT;
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 1;
		sel.colclamp = 1;
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

	/// One vertex. `u` and `v` are the texture coordinate in texels, as the vertex
	/// carries it -- the vertex conversion multiplies S by `0x10000 << TW` and S is
	/// u / (1 << TW), so what reaches the scanline is u in 16.16 whatever TW is.
	static GSVertexSW Vertex(float x, float y, float u, float v, bool linear)
	{
		GSVertexSW out = GSVertexSW::zero();

		out.p = GSVector4(x, y, 0.0f, 0.0f);
		out.p.F64[1] = 0.0;
		out.c = GSVector4(16384.0f, 16384.0f, 16384.0f, 16384.0f);
		out.t = GSVector4(u * 65536.0f, v * 65536.0f, 1.0f, 0.0f);

		if (linear)
		{
			// GSRendererSW folds the linear filter's half-texel straddle into the
			// vertex on the affine road (~1318), because the scanline's own
			// subtraction is gated on the perspective one.
			out.t -= GSVector4(32768.0f, 32768.0f, 0.0f, 0.0f);
		}

		return out;
	}

	/// Draws one arm and returns what it stored, one word per pixel. `u0` is the
	/// coordinate at the rectangle's first column, in texels.
	static bool Arm(bool linear, float u0, u32 out[kSize * kSize])
	{
		// Compiled once per filter: the generators are deterministic, and a pair
		// per test would outrun the code buffer.
		const int slot = linear ? 1 : 0;
		const GSScanlineSelector sel = MakeSelector(linear);

		if (!s_setup[slot])
		{
			s_setup[slot] = CompileSetup(sel);
			s_draw[slot] = CompileScanline(sel);
		}

		SetupPrimPtr setup = s_setup[slot];
		DrawScanlinePtr draw = s_draw[slot];

		if (!setup || !draw)
			return false;

		u32* vm32 = s_mem->vm32();
		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
				vm32[PixelAddr(x, y)] = 0;
		}

		alignas(32) static u32 tex[kTex * kTex];
		for (int y = 0; y < kTex; y++)
		{
			for (int x = 0; x < kTex; x++)
				tex[y * kTex + x] = Texel(x, y);
		}

		isa_native::GSRasterizerData data;
		data.primclass = GS_TRIANGLE_CLASS;
		data.scissor = GSVector4i(0, 0, kSize, kSize);
		data.bbox = GSVector4i(0, 0, kSize, kSize);
		data.scanmsk_value = 0;
		data.setup_prim = setup;
		data.draw_scanline = draw;
		data.draw_edge = nullptr;

		data.global.sel = sel;
		data.global.vm = s_mem->vm8();
		data.global.fzbr = GetOffsets()->row;
		data.global.fzbc = GetOffsets()->col;
		data.global.fm = GSVector4i(0);
		data.global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		data.global.tex[0] = tex;

		// REPEAT on both axes over sixty-four texels, as GSRendererSW derives it:
		// the minimum carries the AND mask, the maximum the OR, and the blend mask
		// picks the repeat form.
		data.global.t.min = GSVector4i::zero();
		data.global.t.max = GSVector4i::zero();
		data.global.t.min.U16[0] = kTex - 1;
		data.global.t.min.U16[4] = kTex - 1;
		data.global.t.min = data.global.t.min.xxxxlh();
		data.global.t.max = data.global.t.max.xxxxlh();
		data.global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		data.global.t.invmask = ~data.global.t.mask;

		// One triangle, oversized and scissored back to the rectangle, so every row
		// has the full span and there is no shared edge for a fill rule to
		// arbitrate. U ramps along x at gs-mag1's rate; V is held.
		const float span = static_cast<float>(kSize) * 4.0f;

		GSVertexSW vertex[3] = {
			Vertex(0.0f, 0.0f, u0, kRow, linear),
			Vertex(span, 0.0f, u0 + span * kRamp, kRow, linear),
			Vertex(0.0f, span, u0, kRow, linear),
		};

		u16 index[3] = {0, 1, 2};

		data.vertex = vertex;
		data.vertex_count = 3;
		data.index = index;
		data.index_count = 3;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);

		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
				out[y * kSize + x] = vm32[PixelAddr(x, y)];
		}

		return true;
	}

	/// Every pixel of a saturated arm carries the same word, and that word is the
	/// one the console wrote. Both halves are the reading.
	static void ExpectEveryPixel(const u32 got[kSize * kSize], u32 want)
	{
		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
			{
				EXPECT_EQ(got[y * kSize + x], want)
					<< "pixel (" << x << ", " << y << ")";
			}
		}
	}

	static SetupPrimPtr s_setup[2];
	static DrawScanlinePtr s_draw[2];
	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

SetupPrimPtr SwCoordinateFieldTest::s_setup[2] = {};
DrawScanlinePtr SwCoordinateFieldTest::s_draw[2] = {};
u8* SwCoordinateFieldTest::s_code = nullptr;
size_t SwCoordinateFieldTest::s_code_used = 0;
GSLocalMemory* SwCoordinateFieldTest::s_mem = nullptr;

// ★ mag-11 under NEAREST: the coordinate stops at texel 2,047, the top of the
// field. On a 64-texel namer under REPEAT that is texel 63. Before the fix the
// coordinate ran on and the REPEAT mask wrapped it to texel 1.
TEST_F(SwCoordinateFieldTest, APositiveCoordinatePastTheFieldStopsAtTexel2047)
{
	u32 got[kSize * kSize];
	ASSERT_TRUE(Arm(false, 2049.0f + 5.0f / 32.0f, got));

	ExpectEveryPixel(got, Texel(2047 & (kTex - 1), 8));
}

// ★ mag-11 under LINEAR: the same field seen through the other filter -- texel
// 2,047 at weight FIFTEEN, which is 0x7FFF sixteenths, the register's top value.
// The weight is the reading that says the clamp is taken AFTER the half-texel
// step: clamping before it would straddle down to weight 7.
TEST_F(SwCoordinateFieldTest, APositiveCoordinatePastTheFieldStopsAtWeightFifteen)
{
	u32 got[kSize * kSize];
	ASSERT_TRUE(Arm(true, 2049.0f + 5.0f / 32.0f, got));

	// The tap pair at the top of the field is texel 2,047 and its neighbour, which
	// REPEAT wraps to texel 0, blended at fifteen sixteenths.
	ExpectEveryPixel(got, Lerp16(Texel(2047 & (kTex - 1), 8), Texel(2048 & (kTex - 1), 8), 15));
}

// ★ neg-12: the bottom of the field is -2,048.0 exactly -- the texel with no
// weight, not one sixteenth above it. Before the fix the coordinate ran past and
// the mask wrapped it.
TEST_F(SwCoordinateFieldTest, ANegativeCoordinatePastTheFieldStopsAtMinus2048)
{
	u32 got[kSize * kSize];
	ASSERT_TRUE(Arm(false, -2049.0f - 5.0f / 32.0f, got));

	ExpectEveryPixel(got, Texel((-2048) & (kTex - 1), 8));
}

// And the same bottom under LINEAR. A weight of zero collapses the tap pair onto
// the single texel, so this reads the field's edge exactly rather than a blend --
// which is what makes it a reading of where the edge is.
TEST_F(SwCoordinateFieldTest, ANegativeCoordinatePastTheFieldStopsAtMinus2048UnderLinear)
{
	u32 got[kSize * kSize];
	ASSERT_TRUE(Arm(true, -2049.0f - 5.0f / 32.0f, got));

	ExpectEveryPixel(got, Texel((-2048) & (kTex - 1), 8));
}

// The control gs-mag1 carries at every magnitude below the field: an arm at an
// ordinary coordinate must not move. mag-4 reads 256 of 256 columns on the console
// and on our own arm, so a clamp that reached it would show here.
TEST_F(SwCoordinateFieldTest, ACoordinateInsideTheFieldIsUntouched)
{
	u32 got[kSize * kSize];
	ASSERT_TRUE(Arm(false, 17.0f + 5.0f / 32.0f, got));

	for (int y = 0; y < kSize; y++)
	{
		for (int x = 0; x < kSize; x++)
		{
			// The nominal ramps, so each column names its own texel: the coordinate
			// walks here and the field does not touch it.
			const float u = 17.0f + 5.0f / 32.0f + static_cast<float>(x) * kRamp;
			const int texel = static_cast<int>(u) & (kTex - 1);

			EXPECT_EQ(got[y * kSize + x], Texel(texel, 8)) << "pixel (" << x << ", " << y << ")";
		}
	}
}
} // namespace

#endif // ARCH_ARM64 && !MULTI_ISA_SHARED_COMPILATION
