// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's texture-coordinate walk (PS_TILE_TWALK), pinned against the
// software rasterizer it transcribes.
//
// The SW renderer WALKS a texture coordinate: ConvertVertexBuffer puts the vertices
// on the scanline's grid (16.16 texels in float, S/T divided by Q per vertex where
// GSRendererSW::Draw's q_div says so), GetScanlineGlobalData takes the bilinear
// half-texel off the vertices of an affine draw, GSRasterizer::DrawTriangleSection
// seeds each row from the section's own vertex through two fused multiply-adds and
// GSRasterizer::DrawSprite seeds the first row through one and every later row
// through a float add, and the GSDrawScanline JIT walks the row from the span's
// first pixel in blocks of four -- as truncating integers under its effective FST,
// as accumulating floats under perspective, followed by a truncated reciprocal.
//
// The shader (tile_twalk in tfx.glsl) replays that per fragment off a per-primitive
// payload; the C++ below is its mechanical mirror. It is anchored here not against
// a scanline span in isolation but against the RASTERIZER: real GSRasterizer::Draw
// calls over converted vertices, with the setup and scanline JITs, into a
// framebuffer whose texture is a ramp that ENCODES the coordinate the scanline
// sampled -- so a walked coordinate that disagrees with the rasterizer's on any
// covered pixel, on either axis, in the texel index or in the 4-bit weight, fails
// here. The payload builder's CPU half (GSTileTexWalk.h) is exercised on the same
// vertices, so the seeds the shader receives are what this suite proves.

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSDrawingContext.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/Tile/GSTileTexWalk.h"

// ---------------------------------------------------------------------------
// The GLSL mirror (tile_twalk and its consumers in tfx.glsl). Every function here
// is one shader function, same name, same arithmetic, in the same order.
// ---------------------------------------------------------------------------
namespace glsl_mirror_tw
{
	// fcvtzs: truncate toward zero, saturating.
	static inline int fcvtzs(float v)
	{
		if (!(v == v))
			return 0;
		if (v >= 2147483648.0f)
			return INT_MAX;
		if (v <= -2147483648.0f)
			return INT_MIN;
		return static_cast<int>(v);
	}

	// The integer walk: seed + B*trunc(g*4) + trunc(g*(m - s)), 32-bit lanes.
	static inline int iwalk(int seed_i, float g, int left, int x)
	{
		const int s = left & 3;
		const int d = x - (left - s);
		const int B = d >> 2;
		const int m = d & 3;
		const int q = fcvtzs(g * 4.0f);
		const int off = fcvtzs(g * static_cast<float>(m - s));
		return static_cast<int>(static_cast<u32>(seed_i) + static_cast<u32>(B) * static_cast<u32>(q) + static_cast<u32>(off));
	}

	// The float walk: seed + g*(m - s) as a product then a separate add (the setup's
	// Fmul into the lane-offset table, the scanline's Fadd), then g*4 added once per
	// block after the first -- sequentially, because each add's rounding depends on
	// the sum before it.
	static inline float fwalk(float seed, float g, int left, int x)
	{
		const int s = left & 3;
		const int d = x - (left - s);
		const int B = d >> 2;
		const int m = d & 3;
		volatile float of = g * static_cast<float>(m - s); // volatile: no fma contraction
		float v = seed + of;
		const float g4 = g * 4.0f;
		for (int i = 0; i < B; i++)
			v += g4;
		return v;
	}

	// A triangle row: the span's first pixel and the row seed for the s, t, q lanes
	// (tile_twalk's triangle branch).
	struct TriRow
	{
		int left;
		float row[3];
	};

	static inline TriRow tri_row(const isa_native::GSTileZPlane& zp, float scissor_left, int py)
	{
		const isa_native::GSTileZPlaneSection& c0 = zp.sec[0];
		const isa_native::GSTileZPlaneSection& c1 = zp.sec[(zp.nsections == 2) ? 1 : 0];
		const int boundary = (zp.nsections == 2) ? (c1.top & 0xFFFF) : 0xFFFF;
		const bool s1 = py >= boundary;
		const isa_native::GSTileZPlaneSection& sec = s1 ? c1 : c0;

		TriRow r;
		const float dy = static_cast<float>(py) - sec.p0y;
		const float lraw = std::fmaf(sec.dedge_x, dy, sec.edge_x);
		const float lx = std::fmax(std::ceil(lraw), scissor_left);
		r.left = static_cast<int>(lx);
		const float prestep = lx - sec.p0x;

		const float seed[3] = {sec.tseed.x, sec.tseed.y, sec.tseed.z};
		const float dedge[3] = {zp.dedge_t.x, zp.dedge_t.y, zp.dedge_t.z};
		const float dscan[3] = {zp.dscan_t.x, zp.dscan_t.y, zp.dscan_t.z};
		for (int i = 0; i < 3; i++)
			r.row[i] = std::fmaf(dscan[i], prestep, std::fmaf(dedge[i], dy, seed[i]));
		return r;
	}

	// A sprite row's t seed (tile_twalk's sprite branch): the row step added once per
	// row after the first, or the fused closed form where the CPU proved every add
	// exact.
	static inline float sprite_row_t(const GSTileTexWalkSprite& spr, int py)
	{
		const int rows = py - spr.top;
		if (spr.rows_exact)
			return std::fmaf(spr.dty, static_cast<float>(rows), spr.t0);
		float t = spr.t0;
		for (int i = 0; i < rows; i++)
			t += spr.dty;
		return t;
	}

	// The perspective leg (tile_stq_uv): the truncated reciprocal, the products,
	// the truncation, the lag.
	static inline void stq_uv(float s, float t, float q, bool lag_u, bool lag_v, int& u, int& v)
	{
		float y = 1.0f / q;
		u32 yb;
		std::memcpy(&yb, &y, 4);
		yb &= 0xfffffc00u;
		std::memcpy(&y, &yb, 4);
		volatile float qs = s * y; // volatile: no fma contraction with anything
		volatile float qt = t * y;
		u = fcvtzs(qs) - (lag_u ? 1 : 0);
		v = fcvtzs(qt) - (lag_v ? 1 : 0);
	}
} // namespace glsl_mirror_tw

// ---------------------------------------------------------------------------
// The mirror against the rasterizer.
// ---------------------------------------------------------------------------

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
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

// The canvas: PSMCT32 at FBW=4 (256 px), 256 rows.
constexpr int kW = 256;
constexpr int kH = 256;
constexpr u32 kFill = 0xEEEEEEEEu;

// The ramp texture: 16x16 texels, R = 16*u, G = 16*v, B = 0, A = 255. Under the
// scanline's bilinear (c00 + (((c01-c00)*f)>>4) per axis, nested) a coordinate
// inside [0, 15) texels stores R = 16*u + f_u and G = 16*v + f_v exactly, which is
// (uv >> 12) & 0xFF -- the coordinate in sixteenths, per axis, mod 256. Under
// nearest it stores 16*u, the texel index. CLAMP on both axes; the last texel
// column/row is excluded from the fraction check because CLAMP folds its
// neighbour onto itself.
constexpr int kTexLog2 = 4;
constexpr int kTexW = 1 << kTexLog2;
constexpr int kTexPitchLog2 = kTexLog2; // sel.tw + 3
constexpr int kSelTw = kTexPitchLog2 - 3;

class TileTWalkRasterTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 32 * 1024;
	static constexpr size_t kCodeBufferSize = 32 * kCodeSlotSize;

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
		GSVertexSW::InitStatic();
		for (int v = 0; v < kTexW; v++)
			for (int u = 0; u < kTexW; u++)
				s_tex[v * kTexW + u] = 0xFF000000u | (static_cast<u32>(16 * v) << 8) | static_cast<u32>(16 * u);
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

	// The reduced key GetScanlineGlobalData builds for the setup.
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

	// A DECAL draw into PSMCT32, nothing tested, nothing blended: the stored pixel
	// is the sampled texel, so the ramp texture reports the coordinate. fst is the
	// scanline's EFFECTIVE fst, which is what the selector carries.
	static GSScanlineSelector MakeSelector(u32 primclass, bool fst_eff, bool ltf)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.ababcd = 0xff;
		sel.prim = primclass;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.colclamp = 1;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		sel.fst = fst_eff ? 1 : 0;
		sel.ltf = ltf ? 1 : 0;
		sel.tlu = 0;
		sel.tw = kSelTw;
		sel.wms = CLAMP_CLAMP;
		sel.wmt = CLAMP_CLAMP;
		return sel;
	}

	static const GSPixelOffset4* GetOffsets()
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 4;
		frame.PSM = PSMCT32;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 400;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(int x, int y)
	{
		return GSLocalMemory::m_psm[PSMCT32].info.pa(x, y, 0, 4);
	}

	struct Jit
	{
		GSScanlineSelector sel;
		SetupPrimPtr setup = nullptr;
		DrawScanlinePtr draw = nullptr;
	};

	static Jit& GetJit(u32 primclass, bool fst_eff, bool ltf)
	{
		const int slot = static_cast<int>(primclass) * 4 + (fst_eff ? 2 : 0) + (ltf ? 1 : 0);
		Jit& j = s_jit[slot];
		if (!j.setup)
		{
			j.sel = MakeSelector(primclass, fst_eff, ltf);
			j.setup = CompileSetup(j.sel);
			j.draw = CompileScanline(j.sel);
		}
		return j;
	}

	// The register state a draw carries: no offset, the ramp texture's size, a
	// 32-bit depth buffer (its format only feeds ConvertVertexBuffer's z clamp).
	static void MakeContext(GSDrawingContext& ctx)
	{
		std::memset(&ctx, 0, sizeof(ctx));
		ctx.TEX0.TW = kTexLog2;
		ctx.TEX0.TH = kTexLog2;
		ctx.ZBUF.PSM = PSMZ32;
	}

	// The whole software path from a raw draw: ConvertVertexBuffer through the
	// renderer's own table (q_div as GSRendererSW::Draw picks it), the half-texel
	// pre-shift where GetScanlineGlobalData applies it, then GSRasterizer::Draw with
	// the two JITs over a canvas of kFill.
	//
	// The converted, pre-shifted vertices are handed back: they are what the Tile
	// payload builder seeds from, so the mirror runs on exactly them. Returns the
	// scanline's effective fst.
	static bool RunRasterizer(u32 primclass, bool prim_fst, bool ltf, bool eq_q, float q_min,
		const GSVertex* verts, u32 nverts, const u16* index, u32 nindex, const GSVector4i& scissor,
		std::vector<GSVertexSW>& out_sw, bool& fst_eff)
	{
		const u32 q_div = gsTileTexWalkQDiv(primclass, false, eq_q, q_min);
		fst_eff = gsTileTexWalkFst(primclass, prim_fst, false, eq_q);
		Jit& j = GetJit(primclass, fst_eff, ltf);
		if (!j.setup || !j.draw)
			return false;

		GSDrawingContext ctx;
		MakeContext(ctx);
		out_sw.resize(nverts + 1);
		GSVertexSW::s_cvb[primclass][1][prim_fst ? 1 : 0][q_div](&ctx, out_sw.data(), verts, nverts);
		if (gsTileTexWalkPreshift(fst_eff, false, ltf))
			for (u32 i = 0; i < nverts; i++)
				gsTileTexWalkApplyPreshift(out_sw[i]);

		u32* vm32 = s_mem->vm32();
		for (int y = 0; y < kH; y++)
			for (int x = 0; x < kW; x++)
				vm32[PixelAddr(x, y)] = kFill;

		isa_native::GSRasterizerData data;
		data.scissor = scissor;
		data.primclass = static_cast<GS_PRIM_CLASS>(primclass);
		data.vertex = out_sw.data();
		data.vertex_count = static_cast<int>(nverts);
		data.index = const_cast<u16*>(index);
		data.index_count = static_cast<int>(nindex);
		data.bbox = GSVector4i(0, 0, kW, kH);
		data.scanmsk_value = 0;

		GSScanlineGlobalData& global = data.global;
		std::memset(&global, 0, sizeof(global));
		global.sel = j.sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = s_tex;
		// GetScanlineGlobalData's CLAMP cooking, both axes.
		global.t.min.U16[0] = global.t.minmax.U16[0] = 0;
		global.t.max.U16[0] = global.t.minmax.U16[2] = kTexW - 1;
		global.t.mask.U32[0] = 0;
		global.t.min.U16[4] = global.t.minmax.U16[1] = 0;
		global.t.max.U16[4] = global.t.minmax.U16[3] = kTexW - 1;
		global.t.mask.U32[2] = 0;
		global.t.min = global.t.min.xxxxlh();
		global.t.max = global.t.max.xxxxlh();
		global.t.mask = global.t.mask.xxzz();
		global.t.invmask = ~global.t.mask;

		data.setup_prim = j.setup;
		data.draw_scanline = j.draw;
		data.draw_edge = nullptr;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);
		return true;
	}

	static u32 Pixel(int x, int y) { return s_mem->vm32()[PixelAddr(x, y)]; }

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
	static Jit s_jit[16];
	alignas(32) static u32 s_tex[kTexW * kTexW];
};

u8* TileTWalkRasterTest::s_code = nullptr;
size_t TileTWalkRasterTest::s_code_used = 0;
GSLocalMemory* TileTWalkRasterTest::s_mem = nullptr;
TileTWalkRasterTest::Jit TileTWalkRasterTest::s_jit[16];
alignas(32) u32 TileTWalkRasterTest::s_tex[kTexW * kTexW];

// The scanline's consumers of the walked value: what a DECAL pixel encodes.
struct Sample
{
	int u16s; // (u >> 12) & 0xFF: sixteenths mod 256 (bilinear) or texel*16 (nearest)
	int v16s;
	int ut, vt; // texel index (u >> 16), before wrap
};

static Sample Consume(int u, int v, bool ltf, bool persp)
{
	// Under perspective + bilinear the scanline takes the half texel after the
	// divide; under FST it was taken at the vertices, so the walked value carries it.
	if (ltf && persp)
	{
		u -= 0x8000;
		v -= 0x8000;
	}
	Sample s;
	s.ut = u >> 16;
	s.vt = v >> 16;
	if (ltf)
	{
		s.u16s = (u >> 12) & 0xFF;
		s.v16s = (v >> 12) & 0xFF;
	}
	else
	{
		s.u16s = ((u >> 16) & 0xF) << 4;
		s.v16s = ((v >> 16) & 0xF) << 4;
	}
	return s;
}

// Compare a covered pixel against the mirror. Any disagreement fails; only the
// fraction is skipped where CLAMP folds the last texel onto itself, and the index
// where it lies outside the clamp range (folded, not encoded).
static void ExpectPixel(int x, int y, u32 px, const Sample& m, bool ltf, const char* what, int& checked)
{
	if (px == kFill)
		return;
	const int r16 = static_cast<int>(px & 0xFF);
	const int g16 = static_cast<int>((px >> 8) & 0xFF);
	if (ltf)
	{
		if (!(m.ut >= 0 && m.ut < kTexW - 1 && m.vt >= 0 && m.vt < kTexW - 1))
			return;
	}
	else
	{
		if (m.ut < 0 || m.ut >= kTexW || m.vt < 0 || m.vt >= kTexW)
			return;
	}
	checked++;
	EXPECT_EQ(r16, m.u16s) << what << " at (" << x << "," << y << ") u";
	EXPECT_EQ(g16, m.v16s) << what << " at (" << x << "," << y << ") v";
}

// -- Sprites -----------------------------------------------------------------

struct SpriteCase
{
	int x0_16, y0_16, x1_16, y1_16; // 12.4 positions
	int u0_16, v0_16, u1_16, v1_16; // 12.4 UV (FST)
	float s0, t0, s1, t1, q;        // STQ
	bool stq;
};

static void MakeSpriteVerts(const SpriteCase& c, GSVertex out[2])
{
	std::memset(out, 0, sizeof(GSVertex) * 2);
	out[0].XYZ.X = static_cast<u16>(c.x0_16);
	out[0].XYZ.Y = static_cast<u16>(c.y0_16);
	out[1].XYZ.X = static_cast<u16>(c.x1_16);
	out[1].XYZ.Y = static_cast<u16>(c.y1_16);
	out[0].XYZ.Z = 1000;
	out[1].XYZ.Z = 1000;
	out[0].U = static_cast<u16>(c.u0_16);
	out[0].V = static_cast<u16>(c.v0_16);
	out[1].U = static_cast<u16>(c.u1_16);
	out[1].V = static_cast<u16>(c.v1_16);
	out[0].ST.S = c.s0;
	out[0].ST.T = c.t0;
	out[1].ST.S = c.s1;
	out[1].ST.T = c.t1;
	out[0].RGBAQ.Q = c.q;
	out[1].RGBAQ.Q = c.q;
	out[0].RGBAQ.U32[0] = 0x80808080u;
	out[1].RGBAQ.U32[0] = 0x80808080u;
}

class TileTWalkSprite : public TileTWalkRasterTest
{
protected:
	// One sprite through the rasterizer and the mirror. Sprites are FST to the
	// scanline whatever their coordinates say (q_div divides an STQ sprite per
	// vertex), so the mirror is always the integer walk.
	void Check(const SpriteCase& c, bool ltf, const GSVector4i& scissor, const char* what, int& checked)
	{
		GSVertex verts[2];
		MakeSpriteVerts(c, verts);
		static const u16 index[2] = {0, 1};
		std::vector<GSVertexSW> sw;
		bool fst_eff = false;
		ASSERT_TRUE(RunRasterizer(GS_SPRITE_CLASS, !c.stq, ltf, true, c.q, verts, 2, index, 2, scissor, sw, fst_eff)) << what;
		ASSERT_TRUE(fst_eff) << what;

		GSTileTexWalkSprite spr;
		if (!gsTileTexWalkSprite(sw[0], sw[1], scissor, spr))
		{
			for (int y = 0; y < kH; y++)
				for (int x = 0; x < kW; x++)
					ASSERT_EQ(Pixel(x, y), kFill) << what << ": rasterizer drew a sprite the mirror called empty";
			return;
		}
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
			{
				const u32 px = Pixel(x, y);
				if (px == kFill)
					continue;
				const float t = glsl_mirror_tw::sprite_row_t(spr, y);
				const int u = glsl_mirror_tw::iwalk(glsl_mirror_tw::fcvtzs(spr.s0), spr.dtx, spr.left, x);
				const int v = glsl_mirror_tw::iwalk(glsl_mirror_tw::fcvtzs(t), 0.0f, spr.left, x);
				ExpectPixel(x, y, px, Consume(u, v, ltf, false), ltf, what, checked);
			}
		}
	}
};

TEST_F(TileTWalkSprite, OneToOneBlit)
{
	// A 16x16 blit at integer positions with the texture 1:1: every walk exact,
	// rows exact by the grid rule.
	SpriteCase c{};
	c.x0_16 = 8 * 16;
	c.y0_16 = 8 * 16;
	c.x1_16 = 24 * 16;
	c.y1_16 = 24 * 16;
	c.u0_16 = 0;
	c.v0_16 = 0;
	c.u1_16 = 16 * 16;
	c.v1_16 = 16 * 16;
	c.q = 1.0f;
	int checked = 0;
	Check(c, false, GSVector4i(0, 0, kW, kH), "1:1 nearest", checked);
	EXPECT_GT(checked, 200);
	checked = 0;
	Check(c, true, GSVector4i(0, 0, kW, kH), "1:1 bilinear", checked);
	EXPECT_GT(checked, 150);
}

TEST_F(TileTWalkSprite, ScaledBlitAccumulatesPerRow)
{
	// The R&C shape, taller: 13 texel rows stretched over 200 pixel rows, so the
	// row step is not on the grid and every row is a sequential float add whose
	// rounding compounds down the sprite (the fused closed form disagrees on
	// dozens of rows). Half-pixel corners.
	SpriteCase c{};
	c.x0_16 = 4 * 16 - 8;
	c.y0_16 = 4 * 16 - 8;
	c.x1_16 = 18 * 16 - 8;
	c.y1_16 = 204 * 16 - 8;
	c.u0_16 = 0;
	c.v0_16 = 0;
	c.u1_16 = 14 * 16;
	c.v1_16 = 13 * 16;
	c.q = 1.0f;
	int checked = 0;
	Check(c, true, GSVector4i(0, 0, kW, kH), "scaled bilinear", checked);
	EXPECT_GT(checked, 2000);
	checked = 0;
	Check(c, false, GSVector4i(0, 0, kW, kH), "scaled nearest", checked);
	EXPECT_GT(checked, 2000);
}

TEST_F(TileTWalkSprite, FuzzAgainstTheRasterizer)
{
	std::mt19937 rng(20260816u);
	int checked = 0;
	int drawn = 0;
	for (int i = 0; i < 400; i++)
	{
		SpriteCase c{};
		// Positions on the 12.4 grid inside the canvas, either corner order.
		const int x0 = static_cast<int>(rng() % (kW * 16 - 32));
		const int y0 = static_cast<int>(rng() % (kH * 16 - 32));
		const int w = 16 + static_cast<int>(rng() % (48 * 16));
		const int h = 16 + static_cast<int>(rng() % (48 * 16));
		const bool flip = (rng() & 1) != 0;
		c.x0_16 = flip ? std::min(x0 + w, kW * 16 - 1) : x0;
		c.x1_16 = flip ? x0 : std::min(x0 + w, kW * 16 - 1);
		c.y0_16 = y0;
		c.y1_16 = std::min(y0 + h, kH * 16 - 1);
		// UVs inside [0.5, 14.5] texels on the 12.4 grid, any direction.
		c.u0_16 = 8 + static_cast<int>(rng() % (14 * 16));
		c.u1_16 = 8 + static_cast<int>(rng() % (14 * 16));
		c.v0_16 = 8 + static_cast<int>(rng() % (14 * 16));
		c.v1_16 = 8 + static_cast<int>(rng() % (14 * 16));
		c.stq = (rng() % 4) == 0;
		if (c.stq)
		{
			// STQ, constant Q away from one: ConvertVertexBuffer divides per vertex.
			c.q = 0.25f + static_cast<float>(rng() % 1000) / 500.0f;
			c.s0 = (static_cast<float>(c.u0_16) / 256.0f) * c.q; // sixteenths / 16 texels / 16 (TW), times q
			c.s1 = (static_cast<float>(c.u1_16) / 256.0f) * c.q;
			c.t0 = (static_cast<float>(c.v0_16) / 256.0f) * c.q;
			c.t1 = (static_cast<float>(c.v1_16) / 256.0f) * c.q;
			c.u0_16 = c.u1_16 = c.v0_16 = c.v1_16 = 0;
		}
		else
		{
			c.q = 1.0f;
		}
		const bool ltf = (rng() & 1) != 0;
		// A scissor that sometimes clips the sprite's top-left, so the seed's
		// (left, top) is the scissor's, not the corner's.
		GSVector4i scissor(0, 0, kW, kH);
		if ((rng() % 3) == 0)
			scissor = GSVector4i(static_cast<int>(rng() % 64), static_cast<int>(rng() % 64), kW, kH);
		char what[96];
		std::snprintf(what, sizeof(what), "sprite %d %s %s", i, c.stq ? "stq" : "uv", ltf ? "ltf" : "nn");
		const int before = checked;
		Check(c, ltf, scissor, what, checked);
		if (checked > before)
			drawn++;
		if (::testing::Test::HasFailure())
			break;
	}
	EXPECT_GT(drawn, 200);
	EXPECT_GT(checked, 20000);
}

// -- Triangles -----------------------------------------------------------------

struct TriCase
{
	int x_16[3], y_16[3];
	int u_16[3], v_16[3];   // FST
	float s[3], t[3], q[3]; // STQ
	bool stq;
};

static void MakeTriVerts(const TriCase& c, GSVertex out[3])
{
	std::memset(out, 0, sizeof(GSVertex) * 3);
	for (int i = 0; i < 3; i++)
	{
		out[i].XYZ.X = static_cast<u16>(c.x_16[i]);
		out[i].XYZ.Y = static_cast<u16>(c.y_16[i]);
		out[i].XYZ.Z = 1000;
		out[i].U = static_cast<u16>(c.u_16[i]);
		out[i].V = static_cast<u16>(c.v_16[i]);
		out[i].ST.S = c.s[i];
		out[i].ST.T = c.t[i];
		out[i].RGBAQ.Q = c.q[i];
		out[i].RGBAQ.U32[0] = 0x80808080u;
	}
}

class TileTWalkTriangle : public TileTWalkRasterTest
{
protected:
	void Check(const TriCase& c, bool ltf, const GSVector4i& scissor, const char* what, int& checked)
	{
		GSVertex verts[3];
		MakeTriVerts(c, verts);
		static const u16 index[3] = {0, 1, 2};
		const bool eq_q = c.q[0] == c.q[1] && c.q[1] == c.q[2];
		std::vector<GSVertexSW> sw;
		bool fst_eff = false;
		ASSERT_TRUE(RunRasterizer(GS_TRIANGLE_CLASS, !c.stq, ltf, eq_q, c.q[0], verts, 3, index, 3, scissor, sw, fst_eff)) << what;
		const bool persp = !fst_eff;

		const GSVector4 fscissor(scissor);
		isa_native::GSTileZPlane zp;
		const bool has_plane = isa_native::GSComputeTriangleZPlane(sw.data(), fscissor.ywyw(), zp) && zp.nsections != 0;
		if (!has_plane)
		{
			for (int y = 0; y < kH; y++)
				for (int x = 0; x < kW; x++)
					ASSERT_EQ(Pixel(x, y), kFill) << what << ": rasterizer drew a triangle the mirror called empty";
			return;
		}
		// The lag: per axis, the per-pixel step's sign, once per primitive.
		const bool lag_u = zp.dscan_t.x > 0.0f;
		const bool lag_v = zp.dscan_t.y > 0.0f;
		for (int y = 0; y < kH; y++)
		{
			for (int x = 0; x < kW; x++)
			{
				const u32 px = Pixel(x, y);
				if (px == kFill)
					continue;
				const glsl_mirror_tw::TriRow row = glsl_mirror_tw::tri_row(zp, fscissor.x, y);
				int u, v;
				if (!persp)
				{
					u = glsl_mirror_tw::iwalk(glsl_mirror_tw::fcvtzs(row.row[0]), zp.dscan_t.x, row.left, x) - (lag_u ? 1 : 0);
					v = glsl_mirror_tw::iwalk(glsl_mirror_tw::fcvtzs(row.row[1]), zp.dscan_t.y, row.left, x) - (lag_v ? 1 : 0);
				}
				else
				{
					const float s = glsl_mirror_tw::fwalk(row.row[0], zp.dscan_t.x, row.left, x);
					const float t = glsl_mirror_tw::fwalk(row.row[1], zp.dscan_t.y, row.left, x);
					const float q = glsl_mirror_tw::fwalk(row.row[2], zp.dscan_t.z, row.left, x);
					glsl_mirror_tw::stq_uv(s, t, q, lag_u, lag_v, u, v);
				}
				ExpectPixel(x, y, px, Consume(u, v, ltf, persp), ltf, what, checked);
			}
		}
	}
};

TEST_F(TileTWalkTriangle, FlatTopFstBothFilters)
{
	TriCase c{};
	c.x_16[0] = 10 * 16;
	c.y_16[0] = 10 * 16;
	c.x_16[1] = 100 * 16;
	c.y_16[1] = 10 * 16;
	c.x_16[2] = 40 * 16;
	c.y_16[2] = 90 * 16;
	c.u_16[0] = 8;
	c.v_16[0] = 8;
	c.u_16[1] = 14 * 16;
	c.v_16[1] = 20;
	c.u_16[2] = 3 * 16 + 5;
	c.v_16[2] = 14 * 16;
	for (int i = 0; i < 3; i++)
		c.q[i] = 1.0f;
	int checked = 0;
	Check(c, false, GSVector4i(0, 0, kW, kH), "flat-top fst nn", checked);
	EXPECT_GT(checked, 1000);
	checked = 0;
	Check(c, true, GSVector4i(0, 0, kW, kH), "flat-top fst ltf", checked);
	EXPECT_GT(checked, 1000);
}

TEST_F(TileTWalkTriangle, PerspectiveWalkIsALoop)
{
	// A wide triangle with Q varying sixfold across it: the float lanes accumulate
	// over up to sixty blocks and the reciprocal is per pixel.
	TriCase c{};
	c.stq = true;
	c.x_16[0] = 2 * 16 + 3;
	c.y_16[0] = 5 * 16 + 9;
	c.x_16[1] = 250 * 16 + 1;
	c.y_16[1] = 20 * 16 + 4;
	c.x_16[2] = 120 * 16 + 7;
	c.y_16[2] = 200 * 16 + 11;
	c.q[0] = 1.0f;
	c.q[1] = 3.0f;
	c.q[2] = 0.5f;
	// S in [0,1) spans the 16 texels (TW = 4); the numerators carry q.
	c.s[0] = 0.05f * c.q[0];
	c.t[0] = 0.05f * c.q[0];
	c.s[1] = 0.9f * c.q[1];
	c.t[1] = 0.1f * c.q[1];
	c.s[2] = 0.3f * c.q[2];
	c.t[2] = 0.9f * c.q[2];
	int checked = 0;
	Check(c, false, GSVector4i(0, 0, kW, kH), "persp nn", checked);
	EXPECT_GT(checked, 5000);
	checked = 0;
	Check(c, true, GSVector4i(0, 0, kW, kH), "persp ltf", checked);
	EXPECT_GT(checked, 5000);
}

TEST_F(TileTWalkTriangle, ConstantQIsFstToTheScanline)
{
	// STQ with one Q over the primitive, away from one: ConvertVertexBuffer divides
	// per vertex and the scanline walks integers -- the mirror must take the
	// integer walk on the divided seeds, half-texel at the vertices under bilinear.
	TriCase c{};
	c.stq = true;
	c.x_16[0] = 20 * 16 + 5;
	c.y_16[0] = 30 * 16 + 1;
	c.x_16[1] = 200 * 16 + 9;
	c.y_16[1] = 40 * 16 + 13;
	c.x_16[2] = 90 * 16 + 2;
	c.y_16[2] = 180 * 16 + 6;
	for (int i = 0; i < 3; i++)
		c.q[i] = 2.75f;
	c.s[0] = 0.04f * 2.75f;
	c.t[0] = 0.9f * 2.75f;
	c.s[1] = 0.87f * 2.75f;
	c.t[1] = 0.85f * 2.75f;
	c.s[2] = 0.5f * 2.75f;
	c.t[2] = 0.05f * 2.75f;
	int checked = 0;
	Check(c, false, GSVector4i(0, 0, kW, kH), "const-q nn", checked);
	EXPECT_GT(checked, 3000);
	checked = 0;
	Check(c, true, GSVector4i(0, 0, kW, kH), "const-q ltf", checked);
	EXPECT_GT(checked, 3000);
}

TEST_F(TileTWalkTriangle, FuzzAgainstTheRasterizer)
{
	std::mt19937 rng(20260817u);
	int checked = 0;
	int drawn = 0;
	for (int i = 0; i < 300; i++)
	{
		TriCase c{};
		for (int k = 0; k < 3; k++)
		{
			c.x_16[k] = static_cast<int>(rng() % (kW * 16));
			c.y_16[k] = static_cast<int>(rng() % (kH * 16));
			c.u_16[k] = 8 + static_cast<int>(rng() % (14 * 16));
			c.v_16[k] = 8 + static_cast<int>(rng() % (14 * 16));
		}
		c.stq = (rng() % 2) == 0;
		const bool const_q = c.stq && (rng() % 4) == 0;
		for (int k = 0; k < 3; k++)
		{
			// Perspective: Q in [0.25, 4), coordinates inside the ramp; FST: q = 1.
			c.q[k] = c.stq ? (0.25f + static_cast<float>(rng() % 1000) / 267.0f) : 1.0f;
			if (const_q)
				c.q[k] = c.q[0];
			c.s[k] = (0.03f + static_cast<float>(rng() % 1000) / 1180.0f) * c.q[k];
			c.t[k] = (0.03f + static_cast<float>(rng() % 1000) / 1180.0f) * c.q[k];
		}
		const bool ltf = (rng() & 1) != 0;
		GSVector4i scissor(0, 0, kW, kH);
		if ((rng() % 3) == 0)
			scissor = GSVector4i(static_cast<int>(rng() % 96), static_cast<int>(rng() % 96), kW - static_cast<int>(rng() % 64), kH - static_cast<int>(rng() % 64));
		char what[96];
		std::snprintf(what, sizeof(what), "tri %d %s%s %s", i, c.stq ? "stq" : "uv", const_q ? "(const q)" : "", ltf ? "ltf" : "nn");
		const int before = checked;
		Check(c, ltf, scissor, what, checked);
		if (checked > before)
			drawn++;
		if (::testing::Test::HasFailure())
			break;
	}
	EXPECT_GT(drawn, 150);
	EXPECT_GT(checked, 100000);
}

} // namespace

#endif // ARCH_ARM64
