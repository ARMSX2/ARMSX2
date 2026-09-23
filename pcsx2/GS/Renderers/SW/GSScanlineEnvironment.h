// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/GSVector.h"
#include "GS/Renderers/SW/GSColourWalk.h"

#include <cstdio>
#include <string>

union GSScanlineSelector
{
	struct
	{
		u32 fpsm  : 2; // 0
		u32 zpsm  : 2; // 2
		u32 ztst  : 2; // 4 (0: off, 1: write, 2: test (ge), 3: test (g))
		u32 atst  : 3; // 6
		u32 afail : 2; // 9
		u32 iip   : 1; // 11
		u32 tfx   : 3; // 12
		u32 tcc   : 1; // 15
		u32 fst   : 1; // 16
		u32 ltf   : 1; // 17
		u32 tlu   : 1; // 18
		u32 fge   : 1; // 19
		u32 date  : 1; // 20
		u32 abe   : 1; // 21
		u32 aba   : 2; // 22
		u32 abb   : 2; // 24
		u32 abc   : 2; // 26
		u32 abd   : 2; // 28
		u32 pabe  : 1; // 30
		u32 aa1   : 1; // 31

		u32 fwrite    : 1; // 32
		u32 ftest     : 1; // 33
		u32 rfb       : 1; // 34
		u32 zwrite    : 1; // 35
		u32 ztest     : 1; // 36
		u32 zoverflow : 1; // 37 (z max >= 0x80000000)
		u32 zclamp    : 1; // 38
		u32 wms       : 2; // 39
		u32 wmt       : 2; // 41
		u32 datm      : 1; // 43
		u32 colclamp  : 1; // 44
		u32 fba       : 1; // 45
		u32 dthe      : 1; // 46
		u32 prim      : 2; // 47

		u32 edge   : 1; // 49
		u32 tw     : 3; // 50 (encodes values between 3 -> 10, texture cache makes sure it is at least 3)
		u32 lcm    : 1; // 53
		u32 mmin   : 2; // 54
		u32 notest : 1; // 55 (no ztest, no atest, no date, no scissor test, and horizontally aligned to 4 pixels)
		// TODO: 1D texture flag? could save 2 texture reads and 4 lerps with bilinear, and also the texture coordinate clamp/wrap code in one direction
		u32 zequal : 1; // 56
		u32 breakpoint : 1; // Insert a trap to stop the program, helpful to stop debugger on a program

		// The GS picks MMAG or MMIN per pixel from that pixel's level of detail, so
		// the filter can change within one primitive. ltfx marks a primitive that
		// straddles the crossing: it runs the bilinear path with the weight forced to
		// zero where the nearest filter wins. ltfx_ge picks which side that is.
		u32 ltfx    : 1;
		u32 ltfx_ge : 1;

		// This coordinate walks in the GS's 12.15 truncating accumulator.
		// `fst` only says the scanline bit-casts a 16.16 integer. Three inputs
		// arrive that way: the 12.4 UV register, a sprite's ST after the vertex
		// conversion, and a constant-Q triangle's ST plane. The first two use the
		// accumulator, the third does not. See GSCoordinateWalk.h.
		u32 uvwalk : 1;
	};

	struct
	{
		u32 _pad1  : 22;
		u32 ababcd :  8;
		u32 _pad2  :  2;

		u32 fb    : 2;
		u32 _pad3 : 1;
		u32 zb    : 2;
	};

	struct
	{
		u32 lo;
		u32 hi;
	};

	u64 key;

	GSScanlineSelector() = default;
	GSScanlineSelector(u64 k)
		: key(k)
	{
	}

	operator u32() const { return lo; }
	operator u64() const { return key; }

	/// Whether the rasterizer can serve this draw as a bulk rectangle fill instead of
	/// running the per-pixel scanline. Each condition listed is one the fill cannot
	/// reproduce. Dither is one: the fill writes a constant colour, while the dither
	/// matrix is added per pixel in WriteFrame.
	bool IsSolidRect() const
	{
		return prim == GS_SPRITE_CLASS && iip == 0 && tfx == TFX_NONE && abe == 0 && ztst <= 1 && atst <= 1 && date == 0 && fge == 0 && dthe == 0;
	}

	std::string to_string() const
	{
		char str[1024];
		std::snprintf(str, std::size(str),
			"fpsm:%d zpsm:%d ztst:%d ztest:%d atst:%d afail:%d iip:%d rfb:%d fb:%d zb:%d zw:%d "
			"tfx:%d tcc:%d fst:%d ltf:%d tlu:%d wms:%d wmt:%d mmin:%d lcm:%d tw:%d "
			"fba:%d cclamp:%d date:%d datm:%d "
			"prim:%d abe:%d %d%d%d%d fge:%d dthe:%d notest:%d pabe:%d aa1:%d "
			"fwrite:%d ftest:%d zoverflow:%d zequal:%d zclamp:%d edge:%d",
			fpsm, zpsm, ztst, ztest, atst, afail, iip, rfb, fb, zb, zwrite,
			tfx, tcc, fst, ltf, tlu, wms, wmt, mmin, lcm, tw,
			fba, colclamp, date, datm,
			prim, abe, aba, abb, abc, abd, fge, dthe, notest, pabe, aa1,
			fwrite, ftest, zoverflow, zequal, zclamp, edge);
		return str;
	}

	void Print() const
	{
		fprintf(stderr, "%s\n", to_string().c_str());
	}
};

struct alignas(32) GSScanlineGlobalData // per batch variables, this is like a pixel shader constant buffer
{
	GSScanlineSelector sel;

	// - the data of vm, tex may change, multi-threaded drawing must be finished before that happens, clut and dimx are copies
	// - tex is a cached texture, it may be recycled to free up memory, its absolute address cannot be compiled into code
	// - row and column pointers are allocated once and never change or freed, thier address can be used directly

	void* vm;
	// Seven mip levels, and an eighth slot that repeats the last one.
	// At a level of detail at or above MXL the GS returns level MXL with weight
	// zero, so the trilinear blend's second tap is the same level and the blend
	// is inert. The scanline reads `tex[lodi + 1]` unconditionally; the duplicate
	// pointer avoids a clamp on the hot path.
	const void* tex[8];
	u32* clut;
	GSVector4i* dimx;

	GSOffset fbo;
	GSOffset zbo;
	const GSVector2i* fzbr;
	const GSVector2i* fzbc;

	GSVector4i aref;
	GSVector4i afix;
	struct { GSVector4i min, max, minmax, mask, invmask; } t; // [u] x 4 [v] x 4

#if _M_SSE >= 0x501

	u32 fm, zm;
	u32 frb, fga;
	GSVector8 mxl;
	GSVector8 k; // TEX1.K * 0x10000
	GSVector8 l; // TEX1.L * -0x10000
	GSVector8 ltfx_q; // the Q at which the level crosses zero; sel.ltfx
	struct { GSVector8i i, f; } lod; // lcm == 1

#else

	GSVector4i fm, zm;
	GSVector4i frb, fga;
	GSVector4 mxl;
	GSVector4 k; // TEX1.K * 0x10000
	GSVector4 l; // TEX1.L * -0x10000
	GSVector4 ltfx_q; // the Q at which the level crosses zero; sel.ltfx
	struct { GSVector4i i, f; } lod; // lcm == 1

#endif

	// The GS's logarithm as a table, see GSLevelOfDetail.h. `lodtab` is the row
	// for this draw's TEX1.L, `lodk` is TEX1.K in sixteenths of a level,
	// `lodshift` is 4 + TEX1.L, and `lodmxl` is `mxl` as an integer.
	const s32* lodtab;
	s32 lodk;
	s32 lodshift;
	s32 lodmxl;

	// The primitive-grain rule's per-draw input, per axis: TEX0's log2 width and
	// height plus two (GSCoordinateWalk.h). Zero on draws that do not take the rule
	// (all but an affine STQ triangle inside the texel-rounding gate). A real value
	// is at least two, so zero safely means "no rule".
	s32 coord_grain_floor[2] = {};

#ifdef ARCH_ARM64
	// Mini version of constant data for ARM64, we don't need all of it
	alignas(16) u32 const_test_128b[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) u16 const_movemaskw_mask[8] = {0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00, 0x3000, 0xc000};
#endif
};

struct alignas(32) GSScanlineLocalData // per prim variables, each thread has its own
{
#if _M_SSE >= 0x501

	struct skip { GSVector8 z, s, t, q; GSVector8i rb, ga, f, _pad; } d[8];
	struct step { GSVector4 stq; struct { u32 rb, ga; } c; struct { u64 z; u32 f; } p; } d8;
	struct { u32 z, f; } p;
	struct { GSVector8i rb, ga; } c;
	// One unit of the 16.16 texture coordinate on each axis that walks forward,
	// zero on an axis that is still or walking back. See GSDrawScanline.cpp.
	struct { GSVector8i u, v; } tclag;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector8 z0, z1;
		GSVector8i f;
		GSVector8 s, t, q;
		GSVector8i rb, ga;
		GSVector8i zs, zd;
		GSVector8i uf, vf;
		GSVector8i cov;

		// mipmapping

		struct { GSVector8i i, f; } lod;
		GSVector8i uv[2];
		GSVector8i uv_minmax[2];
		GSVector8i trb, tga;
		GSVector8i test;
	} temp;

#else

	// z, s, t and q are indexed by position inside the vector (left & 3) and use
	// the first four entries; colour and fog by position inside the eight-pixel
	// block (left & 7) and use all eight. See GSColourWalk.h.
	struct skip { GSVector4 z, s, t, q; GSVector4i rb, ga, f, _pad; } d[8];
	struct step { GSVector4 z, stq; GSVector4i c, f; } d4;
	// An eight-pixel block is two vectors here, so the per-vector step alternates
	// (GSColourWalk.h). Indexed by position inside the block (left & 7), then by
	// phase. The two phases sum to the block step; the walk starts at phase 0.
	struct blockstep { GSVector4i rb, ga, f, _pad; } dw[8][2];
	struct { GSVector4i rb, ga; } c;
	struct { GSVector4i z, f; } p;
	// One unit of the 16.16 texture coordinate on each axis that walks forward,
	// zero on an axis that is still or walking back. See GSDrawScanline.cpp.
	struct { GSVector4i u, v; } tclag;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector4 z0, z1;
		GSVector4i f;
		GSVector4 s, t, q;
		GSVector4i rb, ga;
		GSVector4i zs, zd;
		GSVector4i uf, vf;
		GSVector4i cov;

		// mipmapping

		struct { GSVector4i i, f; } lod;
		GSVector4i uv[2];
		GSVector4i uv_minmax[2];
		GSVector4i trb, tga;
		GSVector4i test;
	} temp;

#endif

	//

	/// The setup's colour interpolator decision for this primitive: gradients,
	/// anchor and block grid. Kept here so a test's setup_prim hook can read it.
	GSColourWalk cwalk;

	const GSScanlineGlobalData* gd;
};

namespace GSScanlineConstantData
{
	static constexpr float log2_coef[] = {
		0.204446009836232697516f,
		-1.04913055217340124191f,
		2.28330284476918490682f,
		1.0f
	};
};

// Constant shared by all threads (to reduce cache miss)
struct alignas(64) GSScanlineConstantData256B
{
	// All AVX processors support unaligned access with little to no penalty as long as you don't cross a cache line.
	// Take advantage of that to store single vectors that we index with single-element alignment
	alignas(32) u8 m_test[24] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	float m_log2_coef[4] = {};
	alignas(64) float m_shift[16] = {
		8.0f, -7.0f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f,
		0.0f,  1.0f,  2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,
	};
	// Eight lanes and an eight-pixel block are the same span (GSBlockWalk.h), so
	// this build needs no second lane-offset table.

	constexpr GSScanlineConstantData256B()
	{
		using namespace GSScanlineConstantData;
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			m_log2_coef[n] = log2_coef[n];
		}
	}
};

struct alignas(64) GSScanlineConstantData128B
{
	alignas(16) u32 m_test[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) float m_shift[5][4] = {
		{ 4.0f  , 4.0f  , 4.0f  , 4.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f},
	};
	// The lane offsets of m_shift[1..4] as integers. The affine texture coordinate
	// multiplies its floored per-pixel step by these, because the GS accumulator
	// is seed + n * floor(step), which is not floor(n * step). See GSCoordinateWalk.h.
	alignas(16) s32 m_lane[4][4] = {
		{  0,  1,  2,  3},
		{ -1,  0,  1,  2},
		{ -2, -1,  0,  1},
		{ -3, -2, -1,  0},
	};
	alignas(16) float m_log2_coef[4][4] = {};

	constexpr GSScanlineConstantData128B()
	{
		using namespace GSScanlineConstantData;
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			for (size_t i = 0; i < 4; ++i)
				m_log2_coef[n][i] = log2_coef[n];
		}
	}
};

extern const GSScanlineConstantData256B g_const_256b;
extern const GSScanlineConstantData128B g_const_128b;
