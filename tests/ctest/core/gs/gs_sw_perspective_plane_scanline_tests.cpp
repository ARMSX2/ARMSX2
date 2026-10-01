// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A perspective triangle drawn with the console's S, T and Q planes.
//
// gs_sw_perspective_plane_tests.cpp pins the arithmetic that builds a plane
// (GSPerspectivePlane.h). This suite pins that the software renderer carries it to
// the pixel. A triangle goes through the real GSRasterizer, once with the C++
// scanline and once with the generated one, vertices in and framebuffer out,
// against a texture whose every texel names itself, so a stored word says which
// texel a pixel sampled. The expected texel is the header's plane through the
// scanline's divide (a multiply by a reciprocal on a fifteen-bit grid, the
// coordinate's truncation to a sixteenth; there is no lag on this route), so
// what is on trial is the rasterizer's row seeds, the setup's steps and the
// scanline's floor to g/4 and its conversion to float.
//
// Each scene is chosen so that the exact plane names a different texel on 50 to 80
// pixels: without the plane rule these cases are red, and the last case pins that
// they are.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture, and the
// plane is not enabled on x86.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#include "GS/Renderers/SW/GSPerspectivePlane.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#if defined(ARCH_ARM64) && !defined(MULTI_ISA_SHARED_COMPILATION)

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSLevelOfDetail.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
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

// The naming texture is 1024 texels wide (`1 << (sel.tw + 3)` at tw 7) and 64 high.
// A texel's word carries its own coordinates, so the framebuffer says which texel a
// pixel sampled.
constexpr int kTexW = 1024;
constexpr int kTexH = 64;
constexpr int kTW = 10; // log2 kTexW
constexpr int kTH = 6;  // log2 kTexH

constexpr int kSize = 64; // the framebuffer is one 64-pixel page wide

constexpr u32 Texel(int x, int y)
{
	return 0x80000000u | (static_cast<u32>(y) << 16) | static_cast<u32>(x);
}

struct Scene
{
	const char* name;
	s32 x16[3], y16[3];
	float s[3], t[3], q[3];
};

// Six triangles, chosen by search so that the plane rule and the exact plane differ
// in the texel they name on 3% to 6% of the pixels (50, 63 and 80 of about 1,200 to
// 1,400 on the first three), and the last so that its twice-area is a power of two.
// A is led by Q >= 2 (E = 1), B has negative coordinates under REPEAT (E = 0), C is
// led by S (E = 1 with every Q below 2), D is the power-of-two area, E and F are A with
// some or all of its vertices negated whole.
const Scene kScenes[] = {
	{"A", {104, 962, 379}, {150, 328, 936}, {0.547561646f, 1.88167548f, 0.400869966f}, {2.19578218f, 1.75813711f, 1.84474099f}, {3.09918571f, 2.01724887f, 2.0887816f}},
	{"B", {100, 868, 176}, {77, 672, 925}, {-0.546903789f, 0.468648851f, 0.0164142307f}, {0.0105292341f, 0.810239851f, 0.0555147342f}, {0.659195065f, 1.73486447f, 0.913543701f}},
	{"C", {109, 977, 469}, {17, 221, 921}, {3.04391146f, 3.31216383f, 2.9509778f}, {0.666566133f, 0.681976318f, 0.623073995f}, {1.94477153f, 1.89738989f, 1.86753953f}},
	{"D", {128, 384, 128}, {128, 128, 384}, {0.5f, 2.3125f, 1.0f}, {0.25f, 0.75f, 3.0999999f}, {2.29999995f, 3.4000001f, 2.70000005f}},
	// A with every S, T and Q negated: a vertex with a negative Q is negated whole, so it draws as A does.
	{"E", {104, 962, 379}, {150, 328, 936}, {-0.547561646f, -1.88167548f, -0.400869966f}, {-2.19578218f, -1.75813711f, -1.84474099f}, {-3.09918571f, -2.01724887f, -2.0887816f}},
	// A with the first and last vertices negated: mixed signs, still A.
	{"F", {104, 962, 379}, {150, 328, 936}, {-0.547561646f, 1.88167548f, -0.400869966f}, {-2.19578218f, 1.75813711f, -1.84474099f}, {-3.09918571f, 2.01724887f, -2.0887816f}},
};

/// The floats the vertex conversion hands the scanline: S and T scaled by
/// 2^(16 + TW) and 2^(16 + TH), Q as it is (GSRendererSW ConvertVertexBuffer).
GSVertexSW Vertex(const Scene& sc, int i)
{
	GSVertexSW v = GSVertexSW::zero();

	v.p = GSVector4(static_cast<float>(sc.x16[i]) / 16.0f, static_cast<float>(sc.y16[i]) / 16.0f, 0.0f, 0.0f);
	v.p.F64[1] = 0.0;
	v.c = GSVector4(16384.0f, 16384.0f, 16384.0f, 16384.0f);
	v.t = GSVector4(std::ldexp(sc.s[i], 16 + kTW), std::ldexp(sc.t[i], 16 + kTH), sc.q[i], 0.0f);

	return v;
}

/// What the scanline's divide makes of one pixel's plane values: the multiply by a
/// reciprocal on a fifteen-bit grid rounded as floor(x + 0.7), then the coordinate's truncation
/// toward zero to a sixteenth (no lag on the plane route), and REPEAT addressing. The ARM64 scanline
/// (GSDrawScanlineCodeGenerator SampleTexture) and its C++ twin are the two things
/// this reads.
void Reference(const GSPerspectivePlane& p, int x, int y, int& u, int& v)
{
	const float sh = static_cast<float>(GSPerspectivePlanePixel(GSPerspectivePlaneValue(p, 0, x, y)));
	const float th = static_cast<float>(GSPerspectivePlanePixel(GSPerspectivePlaneValue(p, 1, x, y)));
	const float qh = static_cast<float>(GSPerspectivePlanePixel(GSPerspectivePlaneValue(p, 2, x, y)));

	float r = 1.0f / qh;
	u32 rb;

	std::memcpy(&rb, &r, sizeof(rb));
	rb = (rb + GS_RECIP_ROUND_UP) & ~((1u << GS_RECIP_GRID_SHIFT) - 1);
	std::memcpy(&r, &rb, sizeof(r));

	s32 cu = static_cast<s32>(sh * r * std::ldexp(1.0f, 16 + kTW));
	s32 cv = static_cast<s32>(th * r * std::ldexp(1.0f, 16 + kTH));

	cu += cu < 0 ? 0xfff : 0;
	cv += cv < 0 ? 0xfff : 0;

	// The 12.4 field saturates; these scenes stay well inside it.
	u = (cu >> 16) & (kTexW - 1);
	v = (cv >> 16) & (kTexH - 1);
}

class SwPerspectivePlaneTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 64 * kCodeSlotSize;

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

	// The setup generator takes a reduced key, exactly as GetScanlineGlobalData
	// builds it, so this compiles the variant a real draw would get.
	static SetupPrimPtr CompileSetup(GSScanlineSelector full)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.iip = full.iip;
		sel.tfx = full.tfx;
		sel.tcc = full.tcc;
		sel.fst = full.fst;
		sel.uvwalk = full.uvwalk;
		sel.stqplane = full.stqplane;
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

	// DECAL with TCC on and NEAREST: the stored pixel is the sampled texel and
	// nothing else. Not `notest`: a coverage test keeps a ragged span from writing a
	// whole vector past its end.
	static GSScanlineSelector MakeSelector(bool plane)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.ztst = ZTST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		sel.fst = 0;
		sel.stqplane = plane ? 1 : 0;
		sel.ltf = 0;
		sel.tlu = 0;
		sel.tw = kTW - 3;
		sel.wms = CLAMP_REPEAT;
		sel.wmt = CLAMP_REPEAT;
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 0;
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

	/// Draws one scene through the real rasterizer, with the C++ scanline or the
	/// generated one, and returns the framebuffer (zero where nothing was drawn).
	static bool Draw(const Scene& sc, bool jit, bool plane, std::vector<u32>& out)
	{
		const GSScanlineSelector sel = MakeSelector(plane);

		SetupPrimPtr setup = &isa_native::GSDrawScanline::CSetupPrim;
		DrawScanlinePtr draw = static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline);

		if (jit)
		{
			const int slot = plane ? 1 : 0;

			if (!s_setup[slot])
			{
				s_setup[slot] = CompileSetup(sel);
				s_draw[slot] = CompileScanline(sel);
			}

			setup = s_setup[slot];
			draw = s_draw[slot];

			if (!setup || !draw)
				return false;
		}

		u32* vm32 = s_mem->vm32();
		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
				vm32[PixelAddr(x, y)] = 0;
		}

		alignas(32) static u32 tex[kTexW * kTexH];
		for (int y = 0; y < kTexH; y++)
		{
			for (int x = 0; x < kTexW; x++)
				tex[y * kTexW + x] = Texel(x, y);
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
		data.global.plane_shift[0] = 16 + kTW;
		data.global.plane_shift[1] = 16 + kTH;

		// REPEAT on both axes: the minimum carries the AND mask, the maximum the OR,
		// and the blend mask picks the repeat form (GSRendererSW).
		data.global.t.min = GSVector4i::zero();
		data.global.t.max = GSVector4i::zero();
		data.global.t.min.U16[0] = kTexW - 1;
		data.global.t.min.U16[4] = kTexH - 1;
		data.global.t.min = data.global.t.min.xxxxlh();
		data.global.t.max = data.global.t.max.xxxxlh();
		data.global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		data.global.t.invmask = ~data.global.t.mask;

		GSVertexSW vertex[3] = {Vertex(sc, 0), Vertex(sc, 1), Vertex(sc, 2)};
		u16 index[3] = {0, 1, 2};

		data.vertex = vertex;
		data.vertex_count = 3;
		data.index = index;
		data.index_count = 3;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);

		out.assign(static_cast<size_t>(kSize) * kSize, 0);
		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
				out[static_cast<size_t>(y) * kSize + x] = vm32[PixelAddr(x, y)];
		}

		return true;
	}

	/// Every pixel the draw stored, against the texel the header's plane names there.
	/// Returns how many pixels were stored.
	static void ExpectThePlane(const Scene& sc, const std::vector<u32>& got, int& drawn)
	{
		float s[3], t[3], q[3];
		s32 x16[3], y16[3];

		for (int i = 0; i < 3; i++)
		{
			s[i] = sc.s[i];
			t[i] = sc.t[i];
			q[i] = sc.q[i];
			x16[i] = sc.x16[i];
			y16[i] = sc.y16[i];
		}

		GSPerspectivePlane plane;

		GSPerspectivePlaneSetup(x16, y16, s, t, q, plane);
		ASSERT_TRUE(plane.valid);

		drawn = 0;

		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
			{
				const u32 word = got[static_cast<size_t>(y) * kSize + x];

				if (word == 0)
					continue;

				drawn++;

				int u, v;

				Reference(plane, x, y, u, v);

				EXPECT_EQ(word, Texel(u, v)) << sc.name << " pixel (" << x << ", " << y << "): sampled texel ("
											 << (word & 0xffff) << ", " << ((word >> 16) & 0x7fff) << "), the plane says ("
											 << u << ", " << v << ")";
			}
		}
	}

	static SetupPrimPtr s_setup[2];
	static DrawScanlinePtr s_draw[2];
	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

SetupPrimPtr SwPerspectivePlaneTest::s_setup[2] = {};
DrawScanlinePtr SwPerspectivePlaneTest::s_draw[2] = {};
u8* SwPerspectivePlaneTest::s_code = nullptr;
size_t SwPerspectivePlaneTest::s_code_used = 0;
GSLocalMemory* SwPerspectivePlaneTest::s_mem = nullptr;

// ★ THE C++ SCANLINE DRAWS THE CONSOLE'S PLANE.
//
// Every pixel the draw stores must name the texel the header's plane names, through
// the same divide. On the exact plane this is red: the scenes differ from it on 50 to
// 80 pixels each.
TEST_F(SwPerspectivePlaneTest, TheCppScanlineDrawsThePlane)
{
	for (const Scene& sc : kScenes)
	{
		std::vector<u32> got;
		ASSERT_TRUE(Draw(sc, false, true, got));

		int drawn = 0;
		ExpectThePlane(sc, got, drawn);

		EXPECT_GT(drawn, 100) << sc.name << ": the triangle drew almost nothing";
	}
}

// ★ AND SO DOES THE GENERATED ONE.
TEST_F(SwPerspectivePlaneTest, TheGeneratedScanlineDrawsThePlane)
{
	for (const Scene& sc : kScenes)
	{
		std::vector<u32> got;
		ASSERT_TRUE(Draw(sc, true, true, got));

		int drawn = 0;
		ExpectThePlane(sc, got, drawn);

		EXPECT_GT(drawn, 100) << sc.name << ": the triangle drew almost nothing";
	}
}

// ★ NEGATING A VERTEX WHOLE DRAWS THE SAME PICTURE.
//
// E is A with every vertex negated and F is A with two of them negated. A vertex with
// a negative Q is negated whole by the setup, so both must store A's words exactly.
TEST_F(SwPerspectivePlaneTest, ANegatedVertexDrawsAsTheOriginalDoes)
{
	for (int jit = 0; jit <= 1; jit++)
	{
		std::vector<u32> a, e, f;
		ASSERT_TRUE(Draw(kScenes[0], jit != 0, true, a));
		ASSERT_TRUE(Draw(kScenes[4], jit != 0, true, e));
		ASSERT_TRUE(Draw(kScenes[5], jit != 0, true, f));

		EXPECT_EQ(a, e) << (jit ? "generated" : "C++") << " scanline, every vertex negated";
		EXPECT_EQ(a, f) << (jit ? "generated" : "C++") << " scanline, two vertices negated";
	}
}

// The two transcriptions of one design must store the same word at every pixel; a
// drift between them is invisible where only the generated one ships.
TEST_F(SwPerspectivePlaneTest, TheTwoScanlinesAgreeWordForWord)
{
	for (const Scene& sc : kScenes)
	{
		std::vector<u32> cpp, jit;
		ASSERT_TRUE(Draw(sc, false, true, cpp));
		ASSERT_TRUE(Draw(sc, true, true, jit));

		EXPECT_EQ(cpp, jit) << sc.name;
	}
}

// ★ THE SCENES CAN TELL THE PLANE FROM THE EXACT PLANE.
//
// The plane rule is off here (the selector bit clear), so this is the renderer as it
// was. Against the header's plane it must disagree on dozens of pixels on each of the
// first three scenes; if it did not, the cases above would pass on the exact plane
// too and would not be a test of the rule.
TEST_F(SwPerspectivePlaneTest, WithoutTheRuleTheScenesDisagreeWithThePlane)
{
	for (const Scene& sc : kScenes)
	{
		if (sc.name[0] == 'D')
			continue;

		std::vector<u32> got;
		ASSERT_TRUE(Draw(sc, true, false, got));

		float s[3], t[3], q[3];
		s32 x16[3], y16[3];

		for (int i = 0; i < 3; i++)
		{
			s[i] = sc.s[i];
			t[i] = sc.t[i];
			q[i] = sc.q[i];
			x16[i] = sc.x16[i];
			y16[i] = sc.y16[i];
		}

		GSPerspectivePlane plane;

		GSPerspectivePlaneSetup(x16, y16, s, t, q, plane);

		int differing = 0;

		for (int y = 0; y < kSize; y++)
		{
			for (int x = 0; x < kSize; x++)
			{
				const u32 word = got[static_cast<size_t>(y) * kSize + x];

				if (word == 0)
					continue;

				int u, v;

				Reference(plane, x, y, u, v);
				differing += (word != Texel(u, v)) ? 1 : 0;
			}
		}

		EXPECT_GT(differing, 30) << sc.name;
	}
}
} // namespace


// ---------------------------------------------------------------------------------
// Every sampler mode.
// ---------------------------------------------------------------------------------

namespace
{
// A span in the plane rule's own units: counts of g/4 for S, T and Q at the first
// pixel and their whole-count steps per pixel, under a block exponent kE.
struct PlaneSpan
{
	s32 s0, t0, q0;
	s32 ds, dt, dq;
};

constexpr int kE = 1;    // the block exponent; g/4 = 2^(kE - 16)
constexpr int kSTW = 4;  // the sampler tests' texture is sixteen texels square
constexpr int kSTH = 4;

struct Row
{
	u32 px[4];

	bool operator==(const Row& o) const
	{
		return px[0] == o.px[0] && px[1] == o.px[1] && px[2] == o.px[2] && px[3] == o.px[3];
	}
};

/// Everything a sampler mode needs that is not in the selector.
struct Mode
{
	const char* name;
	bool ltf = false;
	int ltfx = 0;      ///< 0 off, 1 crossover with the linear side above the threshold, 2 below it
	int mmin = 0;      ///< 0 off, 1 round, 2 trilinear
	bool lcm = true;   ///< constant level of detail
	float ltfx_q = 0.0f;
};

const u32* AddressTexture()
{
	static u32 tex[256];

	for (int i = 0; i < 256; i++)
	{
		const u32 b = static_cast<u32>(i);
		tex[i] = b | (b << 8) | (b << 16) | (b << 24);
	}

	return tex;
}

const u32* InvertedTexture()
{
	static u32 tex[256];

	for (int i = 0; i < 256; i++)
	{
		const u32 b = static_cast<u32>(255 - i);
		tex[i] = b | (b << 8) | (b << 16) | (b << 24);
	}

	return tex;
}
} // namespace

namespace
{
// ★ AT WHOLE COUNTS OF g/4 THE PLANE RULE IS THE FLOAT PIPELINE, IN EVERY SAMPLER MODE.
//
// The floor to g/4 is the rule's only per-pixel arithmetic. A span whose seed and step
// are whole counts of g/4 has nothing for it to floor, and every value the float
// pipeline would carry is then exactly representable, so the rule and the float
// pipeline must store the same word at every pixel: through the nearest and the
// linear filter, the per-pixel filter crossover, and a constant and a Q-driven level
// of detail. That is what pins the conversion the scanline adds (the shift, the
// scale that takes the quotient to 16.16 texels, and the Q that the level and the
// crossover read) against code that is not new. The C++ scanline and the generated
// one are both run, and must agree.
class SwPerspectivePlaneSamplerTest : public SwPerspectivePlaneTest
{
protected:
	static GSScanlineSelector ModeSelector(const Mode& m, bool plane)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		sel.fst = 0;
		sel.stqplane = plane ? 1 : 0;
		sel.ltf = m.ltf ? 1 : 0;
		sel.tlu = 0;
		sel.tw = 1; // sixteen texels wide
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 1;
		sel.colclamp = 1;
		sel.mmin = m.mmin;
		sel.lcm = m.lcm ? 1 : 0;

		if (m.ltfx)
		{
			sel.ltfx = 1;
			sel.ltfx_ge = m.ltfx == 2 ? 1 : 0;
		}

		return sel;
	}

	static void Run(const Mode& m, const PlaneSpan& sp, bool plane, bool jit, Row& out)
	{
		const GSScanlineSelector sel = ModeSelector(m, plane);

		SetupPrimPtr setup = &isa_native::GSDrawScanline::CSetupPrim;
		DrawScanlinePtr draw = static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline);

		if (jit)
		{
			setup = CompileSetup(sel);
			draw = CompileScanline(sel);
			ASSERT_TRUE(setup && draw) << "the code buffer ran out";
		}

		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));

		for (int i = 0; i < 8; i++)
			global.tex[i] = (i & 1) ? InvertedTexture() : AddressTexture();

		// REPEAT on both axes over sixteen texels.
		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.minmax = GSVector4i::zero();
		global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		for (int i = 0; i < 8; i++)
			global.t.min.U16[i] = 15;
		global.t.minmax.U16[0] = 15;
		global.t.minmax.U16[1] = 15;

		global.ltfx_q = GSVector4(m.ltfx_q);
		global.plane_shift[0] = 16 + kSTW;
		global.plane_shift[1] = 16 + kSTH;

		// A constant level of detail of one and a half, or Q-driven with TEX1.L = 0.
		global.lod.i = GSVector4i(1);
		global.lod.f = GSVector4i(0x8000).xxxxlh();
		global.lodtab = GSLevelOfDetailTable[0];
		global.lodk = 0;
		global.lodshift = 4;
		global.lodmxl = 4 << 16;

		local.gd = &global;

		if (sel.mmin && sel.lcm)
		{
			GSVector4i v = global.t.minmax.srl16(global.lod.i.extract32<0>());
			v = v.upl16(v);
			local.temp.uv_minmax[0] = v.upl32(v);
			local.temp.uv_minmax[1] = v.uph32(v);
		}

		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		GSVertexSW scan = GSVertexSW::zero();

		if (plane)
		{
			// Integers in units of g/2^14: a count of g/4 is 2^12 of them.
			dscan.t = GSVector4::cast(GSVector4i(sp.ds << GS_PLANE_PIXEL_SHIFT, sp.dt << GS_PLANE_PIXEL_SHIFT,
				sp.dq << GS_PLANE_PIXEL_SHIFT, 0));
			scan.t = GSVector4::cast(GSVector4i(sp.s0 << GS_PLANE_PIXEL_SHIFT, sp.t0 << GS_PLANE_PIXEL_SHIFT,
				sp.q0 << GS_PLANE_PIXEL_SHIFT, 0));

			local.pwalk.stscale = GSVector4(std::ldexp(1.0f, 16 + kSTW), std::ldexp(1.0f, 16 + kSTH), 1.0f, 1.0f);
			local.pwalk.qscale = GSVector4(std::ldexp(1.0f, kE - 16));
		}
		else
		{
			// The same values as floats: S and T scaled to 16.16 texels, Q as it is.
			const float su = std::ldexp(1.0f, kE - 16 + 16 + kSTW);
			const float sv = std::ldexp(1.0f, kE - 16 + 16 + kSTH);
			const float sq = std::ldexp(1.0f, kE - 16);

			dscan.t = GSVector4(sp.ds * su, sp.dt * sv, sp.dq * sq, 0.0f);
			scan.t = GSVector4(sp.s0 * su, sp.t0 * sv, sp.q0 * sq, 0.0f);
		}

		setup(vertex, index, dscan, local);
		draw(4, 0, 0, scan, local);

		for (int x = 0; x < 4; x++)
			out.px[x] = vm32[PixelAddr(x, 0)];
	}

	static void ExpectModeAgrees(const Mode& m, const PlaneSpan& sp)
	{
		Row cpp_float{}, cpp_plane{}, jit_float{}, jit_plane{};

		Run(m, sp, false, false, cpp_float);
		Run(m, sp, true, false, cpp_plane);
		Run(m, sp, false, true, jit_float);
		Run(m, sp, true, true, jit_plane);

		EXPECT_EQ(cpp_float, jit_float) << m.name << ": the float pipeline's two scanlines disagree";

		for (int i = 0; i < 4; i++)
		{
			EXPECT_EQ(cpp_plane.px[i], cpp_float.px[i]) << m.name << ": C++ scanline, pixel " << i;
			EXPECT_EQ(jit_plane.px[i], cpp_float.px[i]) << m.name << ": generated scanline, pixel " << i;
		}

		// Not four words of nothing: the span has to make the sampler do something.
		EXPECT_TRUE(cpp_float.px[0] != cpp_float.px[3] || cpp_float.px[1] != cpp_float.px[2]) << m.name;
	}
};

TEST_F(SwPerspectivePlaneSamplerTest, NearestMatchesTheFloatPipeline)
{
	ExpectModeAgrees(Mode{"nearest"}, PlaneSpan{14110, 9000, 42598, 1863, 1100, -300});
}

TEST_F(SwPerspectivePlaneSamplerTest, LinearMatchesTheFloatPipeline)
{
	Mode m{"linear"};
	m.ltf = true;

	ExpectModeAgrees(m, PlaneSpan{14110, 9000, 42598, 1863, 1100, -300});
}

// Q crosses the threshold between pixels 1 and 2: the linear filter takes one side.
// The threshold is in true Q, so the scanline's Q must come out of the count in the
// right unit for the compare to land where the float pipeline's does.
TEST_F(SwPerspectivePlaneSamplerTest, TheFilterCrossoverReadsQInItsOwnUnit)
{
	for (int side = 1; side <= 2; side++)
	{
		Mode m{"crossover"};
		m.ltf = true;
		m.ltfx = side;
		// Counts of 12000, 12400, 12800, 13200 are Q = 0.366, 0.378, 0.391, 0.403.
		m.ltfx_q = 12600.0f * std::ldexp(1.0f, kE - 16);

		ExpectModeAgrees(m, PlaneSpan{5000, 3000, 12000, 700, 420, 400});
	}
}

TEST_F(SwPerspectivePlaneSamplerTest, ConstantLevelOfDetailMatchesTheFloatPipeline)
{
	for (int mmin = 1; mmin <= 2; mmin++)
	{
		Mode m{"constant lod"};
		m.mmin = mmin;
		m.lcm = true;
		m.ltf = mmin == 2;

		ExpectModeAgrees(m, PlaneSpan{14110, 9000, 42598, 1863, 1100, -300});
	}
}

// The level comes from Q's own exponent and mantissa (GSLevelOfDetail.h), so the
// scanline has to hand it Q as a float and not as a count: a count of g/4 has a
// different exponent by 2^(E - 16). Q near 0.4 puts the level at about 1.9.
TEST_F(SwPerspectivePlaneSamplerTest, QDrivenLevelOfDetailReadsQAsAFloat)
{
	for (int mmin = 1; mmin <= 2; mmin++)
	{
		Mode m{"q-driven lod"};
		m.mmin = mmin;
		m.lcm = false;
		m.ltf = mmin == 2;

		ExpectModeAgrees(m, PlaneSpan{5200, 3300, 13107, 640, 380, 150});
	}
}

// ★ THE PLANE ROUTE HAS NO COORDINATE LAG.
//
// The lag the accumulator routes trail by (the UV register, STQ at Q = 1) is one unit
// of the 16.16 coordinate off a forward walk, which moves a sample that lands exactly
// on a texel boundary one texel down. On the plane route the coordinate lands on
// boundaries all the time (a value floored to g/4 over a Q of exactly two), and the
// console does not trail there: gs-sm3d's whole-draw arms read 100.0000% of 771,234
// pixels with no lag and 92.3% with it. Q is 2.0 here (count 65,536 at E = 1), so the
// divide is exact and S/Q steps one texel a pixel from texel 3: a lag would read
// 2, 3, 4, 5.
TEST_F(SwPerspectivePlaneSamplerTest, ThePlaneRouteDoesNotLag)
{
	const PlaneSpan sp{3 * 4096, 5 * 4096, 65536, 4096, 0, 0};

	for (int jit = 0; jit <= 1; jit++)
	{
		Row got{};

		Run(Mode{"lagless"}, sp, true, jit != 0, got);

		for (int i = 0; i < 4; i++)
		{
			const u32 want = AddressTexture()[5 * 16 + 3 + i];

			EXPECT_EQ(got.px[i], want) << (jit ? "generated" : "C++") << " scanline, pixel " << i;
		}
	}
}
} // namespace

#endif // ARCH_ARM64 && !MULTI_ISA_SHARED_COMPILATION
