// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Oracle suite for the GS vertex front-end kernels (GS/GSVertexKick.h).
//
// Each kernel is checked against an independent scalar model of the documented
// GIF/GS semantics, over directed edge cases plus large randomized sweeps. The
// scalar models are deliberately written in plain integer C — no GSVector — so
// the vector kernels and the models can only agree by both being right. (During
// the GV campaign the optimized kernels were also held to the legacy ones over
// live replays via GS_VERTEX_CROSSCHECK builds — stripped at GV-CLOSE, recover
// from git history if needed.)

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

#include "GS/GSVertexKick.h"

namespace
{
	struct RefVertex
	{
		u32 m0[4];
		u32 m1[4];
	};

	u32 Rd32(const u8* rec, int dword)
	{
		u32 v;
		std::memcpy(&v, rec + dword * 4, 4);
		return v;
	}

	// Scalar model of packed {STQ, RGBAQ, XYZF2} parsing. rec = 48 bytes (3 qwords).
	RefVertex RefParseXYZF2(const u8* rec, u32 uv)
	{
		RefVertex o;
		o.m0[0] = Rd32(rec, 0); // S
		o.m0[1] = Rd32(rec, 1); // T
		o.m0[2] = (Rd32(rec, 4) & 0xff) | ((Rd32(rec, 5) & 0xff) << 8) |
		          ((Rd32(rec, 6) & 0xff) << 16) | ((Rd32(rec, 7) & 0xff) << 24); // RGBA
		const u32 q = Rd32(rec, 2);
		o.m0[3] = (q == 0) ? 0x00800000u : q; // integer-compare Q==+0.0 -> FLT_MIN; -0.0 passes

		o.m1[0] = (Rd32(rec, 8) & 0xffff) | ((Rd32(rec, 9) & 0xffff) << 16); // X | Y<<16
		o.m1[1] = (Rd32(rec, 10) >> 4) & 0x00ffffff; // Z (24-bit)
		o.m1[2] = uv;
		o.m1[3] = (Rd32(rec, 11) >> 4) & 0xff; // F
		return o;
	}

	// Scalar model of packed {STQ, RGBAQ, XYZ2} parsing: full 32-bit Z, UV and FOG
	// both preserved from the current vertex state.
	RefVertex RefParseXYZ2(const u8* rec, u64 uvfog)
	{
		RefVertex o = RefParseXYZF2(rec, static_cast<u32>(uvfog));
		o.m1[1] = Rd32(rec, 10); // Z, unshifted
		o.m1[3] = static_cast<u32>(uvfog >> 32); // FOG
		return o;
	}

	struct RefRect
	{
		int x, y, z, w;
	};

	// Every grid GSState::ConfigCullGrid can produce: 4 = native pixel centres,
	// 3/2/1 = the exact device grids at 2x/4x/8x, 0 = no grid.
	constexpr int kGridShifts[] = {0, 1, 2, 3, 4};

	// Scalar model of the accept/cull decision. Window entries are the degenerate
	// rects {x, y, x, y}; scissor is the cull-form rect. `shift` is the cull grid's
	// log2 sub-texel step for this prim's class, 0 for no grid.
	u32 RefCullTest(int n, int primclass, const RefRect& v0, const RefRect& v1, const RefRect& v2,
		const RefRect& scissor, int shift, bool aa1_expand)
	{
		auto runion = [](const RefRect& a, const RefRect& b) {
			return RefRect{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
		};

		RefRect bbox = v0;
		if (n >= 2)
			bbox = runion(bbox, v1);
		if (n >= 3)
			bbox = runion(bbox, v2);

		// Snap a rect inwards onto a grid of `step` sub-texels, +1 on the bottom/
		// right lanes so "x >= z" reads as "spans no grid point".
		auto snap = [aa1_expand](RefRect r, int step) {
			r.x = (r.x + step - 1) & ~(step - 1);
			r.y = (r.y + step - 1) & ~(step - 1);
			r.z = ((r.z - 1) & ~(step - 1)) + 1;
			r.w = ((r.w - 1) & ~(step - 1)) + 1;
			if (aa1_expand)
			{
				r.x -= 0x10;
				r.y -= 0x10;
				r.z += 0x10;
				r.w += 0x10;
			}
			return r;
		};

		const bool rounded = (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS);
		const RefRect raw = bbox;
		if (rounded)
		{
			// The rect keeps the shipped rounding at every scale: the pixel-centre
			// snap at native, the bottom/right sub-texel trim at any upscale.
			if (shift == 4)
			{
				bbox = snap(bbox, 16);
			}
			else
			{
				// mask {0,0,1,1}: only the bottom/right lanes can be decremented
				bbox.z -= ((bbox.z & 0xF) == 0) ? 1 : 0;
				bbox.w -= ((bbox.w & 0xF) == 0) ? 1 : 0;

				if (aa1_expand)
				{
					bbox.x -= 0x10;
					bbox.y -= 0x10;
					bbox.z += 0x10;
					bbox.w += 0x10;
				}
			}
		}

		const RefRect ex{bbox.x, bbox.y, bbox.z + 1, bbox.w + 1};
		// !rintersects: intersection {max,max,min,min} empty (x>=z || y>=w)
		u32 test = (std::max(ex.x, scissor.x) >= std::min(ex.z, scissor.z) ||
					   std::max(ex.y, scissor.y) >= std::min(ex.w, scissor.w)) ?
		               1 :
		               0;

		if (rounded)
		{
			test |= (bbox.x >= bbox.z || bbox.y >= bbox.w) ? 1 : 0;

			// The grid is asked separately, off the unrounded box, and only where it
			// is finer than the rect rounding.
			if (shift != 0 && shift != 4)
			{
				const RefRect g = snap(raw, 1 << shift);
				test |= (g.x >= g.z || g.y >= g.w) ? 1 : 0;
			}
		}

		if (primclass == GS_TRIANGLE_CLASS)
		{
			auto eq = [](const RefRect& a, const RefRect& b) { return a.x == b.x && a.y == b.y; };
			test |= (eq(v0, v1) || eq(v1, v2) || eq(v0, v2)) ? 1 : 0;
		}

		return test;
	}

	GSVector4i WindowEntry(int x, int y)
	{
		return GSVector4i(x, y, x, y);
	}

	void ExpectVertexEq(const RefVertex& ref, const GSVector4i& m0, const GSVector4i& m1, const char* what)
	{
		for (int i = 0; i < 4; i++)
		{
			EXPECT_EQ(ref.m0[i], m0.U32[i]) << what << " m0 lane " << i;
			EXPECT_EQ(ref.m1[i], m1.U32[i]) << what << " m1 lane " << i;
		}
	}
} // namespace

TEST(GsVertexParse, Xyzf2DirectedEdges)
{
	// Q == +0.0 rewrites to FLT_MIN; Q == -0.0 (0x80000000) must pass through
	// untouched (the rewrite is an integer compare).
	alignas(16) u8 rec[48] = {};
	const u32 minus_zero = 0x80000000u;
	std::memcpy(rec + 8, &minus_zero, 4); // r[0].U32[2] = Q

	GSVector4i m0, m1;
	GSVertexKernels::ParsePackedSTQRGBAXYZF2(reinterpret_cast<const GIFPackedReg*>(rec), 0xdeadbeefu, m0, m1);
	EXPECT_EQ(minus_zero, m0.U32[3]) << "-0.0 Q must not be rewritten";
	EXPECT_EQ(0xdeadbeefu, m1.U32[2]) << "UV must be preserved";

	std::memset(rec + 8, 0, 4); // Q = +0.0
	GSVertexKernels::ParsePackedSTQRGBAXYZF2(reinterpret_cast<const GIFPackedReg*>(rec), 0, m0, m1);
	EXPECT_EQ(0x00800000u, m0.U32[3]) << "+0.0 Q must become FLT_MIN";

	// RGBA byte gather ignores the upper 24 bits of each dword; Z/F take the >>4
	// shift and their masks.
	alignas(16) u8 rec2[48];
	for (size_t i = 0; i < sizeof(rec2); i++)
		rec2[i] = static_cast<u8>(0xA0 + i);
	const RefVertex ref = RefParseXYZF2(rec2, 0x11223344u);
	GSVertexKernels::ParsePackedSTQRGBAXYZF2(reinterpret_cast<const GIFPackedReg*>(rec2), 0x11223344u, m0, m1);
	ExpectVertexEq(ref, m0, m1, "patterned record");
}

TEST(GsVertexParse, Xyzf2RandomSweep)
{
	std::mt19937_64 rng(0x67766f31); // fixed seed: deterministic suite
	alignas(16) u8 rec[48];

	for (int iter = 0; iter < 1000000; iter++)
	{
		for (int i = 0; i < 6; i++)
		{
			const u64 v = rng();
			std::memcpy(rec + i * 8, &v, 8);
		}
		// Bias some iterations toward the Q==0 edge.
		if ((iter & 15) == 0)
			std::memset(rec + 8, 0, 4);

		const u32 uv = static_cast<u32>(rng());
		const RefVertex ref = RefParseXYZF2(rec, uv);

		GSVector4i m0, m1;
		GSVertexKernels::ParsePackedSTQRGBAXYZF2(reinterpret_cast<const GIFPackedReg*>(rec), uv, m0, m1);

		ASSERT_TRUE(std::memcmp(ref.m0, &m0, 16) == 0 && std::memcmp(ref.m1, &m1, 16) == 0)
			<< "XYZF2 divergence at iter " << iter;
	}
}

TEST(GsVertexParse, Xyz2RandomSweep)
{
	std::mt19937_64 rng(0x67766f32);
	alignas(16) u8 rec[48];

	for (int iter = 0; iter < 1000000; iter++)
	{
		for (int i = 0; i < 6; i++)
		{
			const u64 v = rng();
			std::memcpy(rec + i * 8, &v, 8);
		}
		if ((iter & 15) == 0)
			std::memset(rec + 8, 0, 4);

		const u64 uvfog = rng();
		const RefVertex ref = RefParseXYZ2(rec, uvfog);

		GSVector4i m0, m1;
		GSVertexKernels::ParsePackedSTQRGBAXYZ2(reinterpret_cast<const GIFPackedReg*>(rec), uvfog, m0, m1);

		ASSERT_TRUE(std::memcmp(ref.m0, &m0, 16) == 0 && std::memcmp(ref.m1, &m1, 16) == 0)
			<< "XYZ2 divergence at iter " << iter;
	}
}

#ifdef ARCH_ARM64
TEST(GsVertexParse, Xyzf2NeonRandomSweep)
{
	std::mt19937_64 rng(0x67766f33);
	alignas(16) u8 rec[48];

	for (int iter = 0; iter < 1000000; iter++)
	{
		for (int i = 0; i < 6; i++)
		{
			const u64 v = rng();
			std::memcpy(rec + i * 8, &v, 8);
		}
		if ((iter & 15) == 0)
			std::memset(rec + 8, 0, 4);

		const u32 uv = static_cast<u32>(rng());
		const RefVertex ref = RefParseXYZF2(rec, uv);

		GSVector4i m0, m1;
		GSVertexKernels::ParsePackedSTQRGBAXYZF2_Neon(reinterpret_cast<const GIFPackedReg*>(rec), uv, m0, m1);

		ASSERT_TRUE(std::memcmp(ref.m0, &m0, 16) == 0 && std::memcmp(ref.m1, &m1, 16) == 0)
			<< "NEON XYZF2 divergence at iter " << iter;
	}
}

TEST(GsVertexParse, Xyz2NeonRandomSweep)
{
	std::mt19937_64 rng(0x67766f34);
	alignas(16) u8 rec[48];

	for (int iter = 0; iter < 1000000; iter++)
	{
		for (int i = 0; i < 6; i++)
		{
			const u64 v = rng();
			std::memcpy(rec + i * 8, &v, 8);
		}
		if ((iter & 15) == 0)
			std::memset(rec + 8, 0, 4);

		const u64 uvfog = rng();
		const RefVertex ref = RefParseXYZ2(rec, uvfog);

		GSVector4i m0, m1;
		GSVertexKernels::ParsePackedSTQRGBAXYZ2_Neon(reinterpret_cast<const GIFPackedReg*>(rec), uvfog, m0, m1);

		ASSERT_TRUE(std::memcmp(ref.m0, &m0, 16) == 0 && std::memcmp(ref.m1, &m1, 16) == 0)
			<< "NEON XYZ2 divergence at iter " << iter;
	}
}
#endif // ARCH_ARM64

namespace
{
	template <u32 n, int primclass>
	void RunCullSweep(u64 seed, int iters)
	{
		std::mt19937_64 rng(seed);

		// Offset-subtracted 12.4 coords land in roughly [-0x10000, 0x10000); mix a
		// full-range band with a narrow band so equality/16-boundary collisions and
		// tiny-prim cases actually occur.
		auto coord = [&](int band) {
			if (band == 0)
				return static_cast<int>(rng() % 0x20000) - 0x10000;
			return static_cast<int>(rng() % 0x40) - 0x20;
		};

		for (int iter = 0; iter < iters; iter++)
		{
			const int band = iter & 1;
			int x0 = coord(band), y0 = coord(band);
			int x1 = coord(band), y1 = coord(band);
			int x2 = coord(band), y2 = coord(band);
			// Force exact-duplicate vertices regularly (strip restarts do this).
			if ((iter & 7) == 0)
			{
				x1 = x0;
				y1 = y0;
			}
			if ((iter & 31) == 0)
			{
				x2 = x1;
				y2 = y1;
			}
			// Force 16-aligned coords regularly (the interior-rounding edge).
			if ((iter & 3) == 0)
			{
				x0 &= ~0xF;
				y2 &= ~0xF;
			}

			int sx = coord(0), sy = coord(0);
			int sz = sx + static_cast<int>(rng() % 0x8000);
			int sw = sy + static_cast<int>(rng() % 0x8000);

			// Every grid the config can produce, plus the sprite-class split: a
			// sprite gets the grid only when it is the native one.
			constexpr bool rounded_class = (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS);
			const int tri_shift = kGridShifts[rng() % std::size(kGridShifts)];
			const int sprite_shift = (tri_shift == 4) ? 4 : 0;
			const GSVertexKernels::CullGrid grid = GSVertexKernels::MakeCullGrid(tri_shift, sprite_shift);
			const int shift = rounded_class ? grid.ShiftFor<primclass>() : tri_shift;
			const bool aa1 = (rng() & 7) == 0;

			const RefRect rv0{x0, y0, x0, y0}, rv1{x1, y1, x1, y1}, rv2{x2, y2, x2, y2};
			const RefRect rs{sx, sy, sz, sw};
			const u32 expected =
				RefCullTest(n, primclass, rv0, rv1, rv2, rs, shift, aa1) ? 1u : 0u;

			GSVector4i bbox;
			const u32 got = GSVertexKernels::CullTest<n, primclass>(WindowEntry(x0, y0), WindowEntry(x1, y1),
				WindowEntry(x2, y2), GSVector4i(sx, sy, sz, sw), grid, aa1, bbox);

			ASSERT_EQ(expected, got != 0 ? 1u : 0u)
				<< "cull divergence at iter " << iter << " n=" << n << " class=" << primclass
				<< " v0=(" << x0 << "," << y0 << ") v1=(" << x1 << "," << y1 << ") v2=(" << x2 << "," << y2
				<< ") scissor=(" << sx << "," << sy << "," << sz << "," << sw << ") shift=" << shift
				<< " aa1=" << aa1;
		}
	}
} // namespace

namespace
{
	// Scalar-outcode cull vs the legacy kernel (itself pinned against RefCullTest
	// above). Production-shaped inputs: scissor cull rects are 16k-8 .. 16k+8 with
	// SCAX0 <= SCAX1 (empty scissors take the m_scissor_invalid path and never
	// reach the cull decision), coords are offset-subtracted 12.4. The fast-path
	// gate is baked in: a rounded class runs with a grid, and no AA1.
	//
	// Every grid a rounded class can get is swept, which is the whole of the
	// scalar path's contract: at native the outcode compares bands against the
	// rounding-folded bounds, and at every finer grid it compares raw 12.4 against
	// the plain cull rect while the bands carry the grid test.
	template <u32 n, int primclass>
	void RunScalarCullSweep(u64 seed, int iters)
	{
		constexpr bool rounded = (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS);
		std::mt19937_64 rng(seed);

		for (int iter = 0; iter < iters; iter++)
		{
			// GS-shaped scissor: registers in [0, 2047], min <= max.
			int sax0 = static_cast<int>(rng() % 2048), sax1 = static_cast<int>(rng() % 2048);
			int say0 = static_cast<int>(rng() % 2048), say1 = static_cast<int>(rng() % 2048);
			if (sax0 > sax1)
				std::swap(sax0, sax1);
			if (say0 > say1)
				std::swap(say0, say1);
			const GSVector4i cull(sax0 * 16 - 8, say0 * 16 - 8, sax1 * 16 + 8, say1 * 16 + 8);

			// Coords: full range / narrow cluster / snapped near a scissor edge so
			// the band boundaries and the +/-8 cull margin both get exercised.
			auto coord = [&](int lo16, int hi16) {
				switch (rng() % 4)
				{
					case 0:
						return static_cast<int>(rng() % 0x20000) - 0x10000;
					case 1:
						return static_cast<int>(rng() % 0x40) - 0x20;
					case 2:
						return lo16 + static_cast<int>(rng() % 41) - 20;
					default:
						return hi16 + static_cast<int>(rng() % 41) - 20;
				}
			};

			int x0 = coord(sax0 * 16, sax1 * 16), y0 = coord(say0 * 16, say1 * 16);
			int x1 = coord(sax0 * 16, sax1 * 16), y1 = coord(say0 * 16, say1 * 16);
			int x2 = coord(sax0 * 16, sax1 * 16), y2 = coord(say0 * 16, say1 * 16);
			if ((iter & 7) == 0)
			{
				x1 = x0;
				y1 = y0;
			}
			if ((iter & 31) == 0)
			{
				x2 = x1;
				y2 = y1;
			}
			if ((iter & 3) == 0)
			{
				x0 &= ~0xF;
				y2 &= ~0xF;
			}

			// Shift 4 (native) through 1 (8x) for a rounded class; point/line never
			// consult the grid, so any value drives the same code there.
			const int shift = rounded ? (1 + static_cast<int>(rng() % 4)) : 4;
			const GSVertexKernels::CullGrid grid = GSVertexKernels::MakeCullGrid(shift, shift);
			const bool banded = rounded && (shift == 4);

			GSVector4i bbox;
			const u32 expected = GSVertexKernels::CullTest<n, primclass>(WindowEntry(x0, y0), WindowEntry(x1, y1),
				WindowEntry(x2, y2), cull, grid, false, bbox);

			const GSVertexKernels::CullBounds bounds =
				banded ? GSVertexKernels::MakeBandedCullBounds(cull) : GSVertexKernels::MakeRawCullBounds(cull);
			const auto entry = [&](int x, int y) {
				return banded ? GSVertexKernels::MakeCullMirrorEntry<true>(x, y, bounds, shift) :
								GSVertexKernels::MakeCullMirrorEntry<false>(x, y, bounds, shift);
			};
			const GSVertexKernels::CullMirrorEntry e0 = entry(x0, y0);
			const GSVertexKernels::CullMirrorEntry e1 = entry(x1, y1);
			const GSVertexKernels::CullMirrorEntry e2 = entry(x2, y2);

			const u32 got = GSVertexKernels::CullTestScalar<n, primclass>(e0, e1, e2);

			ASSERT_EQ(expected != 0 ? 1u : 0u, got != 0 ? 1u : 0u)
				<< "scalar cull divergence at iter " << iter << " n=" << n << " class=" << primclass
				<< " shift=" << shift << " v0=(" << x0 << "," << y0 << ") v1=(" << x1 << "," << y1 << ") v2=("
				<< x2 << "," << y2 << ") scissor=(" << sax0 << "," << say0 << "," << sax1 << "," << say1 << ")";
		}
	}
} // namespace

// A triangle strip half a native pixel tall, taken off WRC 3's frame 0 draw
// s_n 130: 44.6 native pixels wide, spanning native Y
// 172.375..172.875, which in 12.4 sub-texels is 2764..2766. Sample rows sit on
// the multiples of the grid step: 2768 at 1x and 2x, 2764 at 4x. So the strip
// paints nothing at either 1x or 2x and does paint at 4x.
//
// Slid up two sub-texels it straddles 2760 -- a 2x sample row that is not a 1x
// one -- and then it has to survive at 2x while still going at 1x.
TEST(GsVertexCull, HalfPixelStripAgainstTheGrid)
{
	const GSVector4i scissor(-8, -8, 16 * 640 + 8, 16 * 448 + 8);

	// x spans 5911..6098, which holds a sample column on every grid; y decides.
	const auto cull_at = [&](int shift, int y_lo, int y_hi) {
		const GSVertexKernels::CullGrid grid = GSVertexKernels::MakeCullGrid(shift, shift == 4 ? 4 : 0);
		GSVector4i bbox;
		return GSVertexKernels::CullTest<3, GS_TRIANGLE_CLASS>(WindowEntry(5911, y_lo), WindowEntry(6098, y_hi),
				   WindowEntry(6000, y_lo), scissor, grid, false, bbox) != 0;
	};

	// The real draw: no sample row inside it at 1x or 2x, one at 4x.
	EXPECT_TRUE(cull_at(4, 2764, 2766)) << "native: spans no pixel centre row";
	EXPECT_TRUE(cull_at(3, 2764, 2766)) << "2x: spans no device sample row";
	EXPECT_FALSE(cull_at(2, 2764, 2766)) << "4x: spans the sample row at 2764";

	// Moved onto the 2x sample row at 2760, which is not a 1x one.
	EXPECT_FALSE(cull_at(3, 2758, 2762)) << "2x: spans the device sample row at 2760";
	EXPECT_TRUE(cull_at(4, 2758, 2762)) << "native: still no pixel centre row";

	// No grid at all is the shipped upscale rule: it keeps everything with extent.
	EXPECT_FALSE(cull_at(0, 2764, 2766)) << "no grid: sub-texel extent is enough";
}

// The sprite class only takes the grid when the grid is the native one, because
// CorrectSpriteCoverageForUpscale moves a sprite's far edge after the cull runs.
TEST(GsVertexCull, SpriteClassDeclinesTheUpscaleGrid)
{
	const GSVector4i scissor(-8, -8, 16 * 640 + 8, 16 * 448 + 8);

	const auto cull_sprite = [&](int shift) {
		const GSVertexKernels::CullGrid grid = GSVertexKernels::MakeCullGrid(shift, shift == 4 ? 4 : 0);
		GSVector4i bbox;
		return GSVertexKernels::CullTest<2, GS_SPRITE_CLASS>(WindowEntry(5911, 2764), WindowEntry(6098, 2766),
				   WindowEntry(6098, 2766), scissor, grid, false, bbox) != 0;
	};

	EXPECT_TRUE(cull_sprite(4)) << "native: the pixel-centre grid still applies to sprites";
	EXPECT_FALSE(cull_sprite(3)) << "2x: sprites keep the scale-blind rule";
	EXPECT_FALSE(cull_sprite(2)) << "4x: sprites keep the scale-blind rule";
}

// The device grid, from the scale alone. 16/scale sub-texels where that is a
// power of two, and nothing at all where it is not -- at a fractional scale the
// sample points do not land on whole sub-texels, so no power-of-two grid contains
// them and any grid would cull prims that paint.
TEST(GsVertexCull, DeviceGridShiftFollowsTheScale)
{
	EXPECT_EQ(4, GSVertexKernels::DeviceCullGridShift(1.0f));
	EXPECT_EQ(3, GSVertexKernels::DeviceCullGridShift(2.0f));
	EXPECT_EQ(2, GSVertexKernels::DeviceCullGridShift(4.0f));
	EXPECT_EQ(1, GSVertexKernels::DeviceCullGridShift(8.0f));

	for (float scale : {1.05f, 1.5f, 1.75f, 3.0f, 5.0f, 6.0f, 7.0f, 16.0f})
		EXPECT_EQ(0, GSVertexKernels::DeviceCullGridShift(scale)) << "scale " << scale;
}

// The grid at shift 4 has to be bit-for-bit the rule the shipped code selected
// with `nativeres`, or 1x moves.
TEST(GsVertexCull, NativeGridMatchesTheShippedNativeRounding)
{
	const GSVertexKernels::CullGrid grid = GSVertexKernels::MakeCullGrid(4, 4);
	std::mt19937_64 rng(0x67763321);

	for (int iter = 0; iter < 200000; iter++)
	{
		const int x0 = static_cast<int>(rng() % 0x20000) - 0x10000;
		const int y0 = static_cast<int>(rng() % 0x20000) - 0x10000;
		const int x1 = x0 + static_cast<int>(rng() % 0x200) - 0x100;
		const int y1 = y0 + static_cast<int>(rng() % 0x200) - 0x100;
		const int x2 = x0 + static_cast<int>(rng() % 0x200) - 0x100;
		const int y2 = y0 + static_cast<int>(rng() % 0x200) - 0x100;

		const GSVector4i bbox_raw = WindowEntry(x0, y0).runion(WindowEntry(x1, y1)).runion(WindowEntry(x2, y2));
		const GSVector4i shipped =
			((bbox_raw + GSVector4i(0xF, 0xF, -1, -1)) & GSVector4i(~0xF)) + GSVector4i(0, 0, 1, 1);

		const GSVector4i got =
			GSVertexKernels::ComputeCullBBox<3, GS_TRIANGLE_CLASS>(WindowEntry(x0, y0), WindowEntry(x1, y1),
				WindowEntry(x2, y2), grid, false);

		ASSERT_TRUE(got.eq(shipped)) << "native rounding moved at iter " << iter;
	}
}

TEST(GsVertexCull, TriangleSweep)
{
	RunCullSweep<3, GS_TRIANGLE_CLASS>(0x67763301, 1000000);
}

TEST(GsVertexCull, SpriteSweep)
{
	RunCullSweep<2, GS_SPRITE_CLASS>(0x67763302, 500000);
}

TEST(GsVertexCull, LineSweep)
{
	RunCullSweep<2, GS_LINE_CLASS>(0x67763303, 500000);
}

TEST(GsVertexCull, ScalarTriangleSweep)
{
	RunScalarCullSweep<3, GS_TRIANGLE_CLASS>(0x67763311, 1000000);
}

TEST(GsVertexCull, ScalarSpriteSweep)
{
	RunScalarCullSweep<2, GS_SPRITE_CLASS>(0x67763312, 500000);
}

TEST(GsVertexCull, ScalarLineSweep)
{
	RunScalarCullSweep<2, GS_LINE_CLASS>(0x67763313, 500000);
}

TEST(GsVertexCull, ScalarPointSweep)
{
	RunScalarCullSweep<1, GS_POINT_CLASS>(0x67763314, 500000);
}

TEST(GsVertexCull, PointSweep)
{
	RunCullSweep<1, GS_POINT_CLASS>(0x67763304, 500000);
}

#ifdef ARCH_ARM64

// ---------------------------------------------------------------------------
// Fused vertex-trace bounds (FmmAcc/FmmAccumVertex/FmmFinish vs the legacy
// FindMinMax walk). The oracle here is a faithful transcription of
// GSVertexTraceFMM::FindMinMax<GS_TRIANGLE_CLASS> (GSVertexTraceFMM.cpp) over
// {m0, m1} vector pairs — the property under test is that the fused
// reformulation reproduces the legacy walk bit-exactly whenever FmmFinish
// accepts, and that it accepts on the benign (real-game) configurations.
// (End-to-end integration was additionally verified during the campaign by
// GS_VERTEX_CROSSCHECK replay over the full dump corpus.)
// ---------------------------------------------------------------------------

namespace
{
	struct FmmRefOut
	{
		GSVector4 min_p, max_p, min_t, max_t;
		GSVector4i min_c, max_c;
		u32 nan_value;
		bool wrote_nan;
	};

	// Transcription of FindMinMax<GS_TRIANGLE_CLASS, iip, tme, fst, color> plus
	// its GetFMM argument fixups (real_fst = tme ? fst : false; real_iip = iip).
	void RefFindMinMaxTriangle(const GSVector4i* m0s, const GSVector4i* m1s, const u16* index, int count,
		bool iip, bool tme, bool fst_in, bool color, const GIFRegXYOFFSET& ofs, u32 tw, u32 th, FmmRefOut& out)
	{
		const bool fst = tme ? fst_in : false;

		const GSVector4 s_minmax = GSVector4(FLT_MAX, -FLT_MAX, 0.f, 0.f);
		GSVector4 tmin = s_minmax.xxxx();
		GSVector4 tmax = s_minmax.yyyy();
		GSVector4i tnan = GSVector4i::zero();
		GSVector4i cmin = GSVector4i::xffffffff();
		GSVector4i cmax = GSVector4i::zero();
		GSVector4i pmin = GSVector4i::xffffffff();
		GSVector4i pmax = GSVector4i::zero();

		const auto processVertices = [&](int i0, int i1, bool finalVertex)
		{
			if (color)
			{
				const GSVector4i c0 = GSVector4i::load(static_cast<int>(static_cast<u32>(m0s[i0].U32[2])));
				const GSVector4i c1 = GSVector4i::load(static_cast<int>(static_cast<u32>(m0s[i1].U32[2])));
				if (iip || finalVertex)
				{
					cmin = cmin.min_u8(c0.min_u8(c1));
					cmax = cmax.max_u8(c0.max_u8(c1));
				}
				// (n == 2 branch: line class only, not transcribed)
			}

			if (tme)
			{
				if (!fst)
				{
					GSVector4 stq0 = GSVector4::cast(m0s[i0]);
					GSVector4 stq1 = GSVector4::cast(m0s[i1]);

					const GSVector4 q = stq0.wwww(stq1);
					const GSVector4 st = stq0.xyxy(stq1) / q;

					stq0 = st.xyww(stq0);
					stq1 = st.zwww(stq1);

					const GSVector4i nan0 = GSVector4i::cast(stq0 != stq0);
					const GSVector4i nan1 = GSVector4i::cast(stq1 != stq1);

					tmin = tmin.blend32(tmin.min(stq0), GSVector4::cast(~nan0));
					tmin = tmin.blend32(tmin.min(stq1), GSVector4::cast(~nan1));
					tmax = tmax.blend32(tmax.max(stq0), GSVector4::cast(~nan0));
					tmax = tmax.blend32(tmax.max(stq1), GSVector4::cast(~nan1));

					tnan |= nan0 | nan1;
				}
				else
				{
					const GSVector4 st0 = GSVector4(m1s[i0].uph16()).xyxy();
					const GSVector4 st1 = GSVector4(m1s[i1].uph16()).xyxy();

					tmin = tmin.min(st0.min(st1));
					tmax = tmax.max(st0.max(st1));
				}
			}

			const GSVector4i xyzf0 = m1s[i0];
			const GSVector4i xyzf1 = m1s[i1];

			const GSVector4i xy0 = xyzf0.upl16();
			const GSVector4i zf0 = xyzf0.ywyw();
			const GSVector4i xy1 = xyzf1.upl16();
			const GSVector4i zf1 = xyzf1.ywyw();

			const GSVector4i p0 = xy0.blend32<0xc>(zf0);
			const GSVector4i p1 = xy1.blend32<0xc>(zf1);

			pmin = pmin.min_u32(p0.min_u32(p1));
			pmax = pmax.max_u32(p0.max_u32(p1));
		};

		int i = 0;
		if (iip)
		{
			for (; i < (count - 1); i += 2)
				processVertices(index[i + 0], index[i + 1], true);
			if (count & 1)
				processVertices(index[i], index[i], true);
		}
		else
		{
			for (; i < (count - 3); i += 6)
			{
				processVertices(index[i + 0], index[i + 3], false);
				processVertices(index[i + 1], index[i + 4], false);
				processVertices(index[i + 2], index[i + 5], true);
			}
			if (count & 1)
			{
				processVertices(index[i + 0], index[i + 1], false);
				processVertices(index[i + 2], index[i + 2], true);
			}
		}

		const GSVector4 o(ofs);
		const GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

		out.min_p = (GSVector4(pmin) - o) * s;
		out.max_p = (GSVector4(pmax) - o) * s;
		out.min_p = out.min_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(pmin.extract32<2>()))));
		out.max_p = out.max_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(pmax.extract32<2>()))));

		out.wrote_nan = true;
		out.nan_value = 0;
		if (tme)
		{
			GSVector4 sc;
			if (fst)
			{
				sc = GSVector4(1.0f / 16, 1.0f).xxyy();
				out.wrote_nan = false;
			}
			else
			{
				sc = GSVector4(1 << static_cast<int>(tw), 1 << static_cast<int>(th), 1, 1);
				out.nan_value = static_cast<u32>(tnan.mask()) & ~4u;
			}
			out.min_t = tmin * sc;
			out.max_t = tmax * sc;
		}
		else
		{
			out.min_t = GSVector4::zero();
			out.max_t = GSVector4::zero();
		}

		if (color)
		{
			out.min_c = cmin.u8to32();
			out.max_c = cmax.u8to32();
		}
		else
		{
			out.min_c = GSVector4i::zero();
			out.max_c = GSVector4i::zero();
		}
	}

	enum class FmmQMode
	{
		ConstantNormal,  // one random normal q for the whole draw (either sign)
		ConstantSpecial, // one q from {+-0, denormal, inf, nan} — fused must decline
		Varying,         // random normal q per vertex — fused must decline
	};

	u32 RandomNormalFloatBits(std::mt19937& rng)
	{
		const u32 exp = 1 + (rng() % 254);
		return (rng() & 0x807FFFFFu) | (exp << 23);
	}

	u32 RandomSpecialFloatBits(std::mt19937& rng)
	{
		switch (rng() % 5)
		{
			case 0: return 0x00000000u;                       // +0
			case 1: return 0x80000000u;                       // -0
			case 2: return (rng() & 0x807FFFFFu);             // denormal (or +-0)
			case 3: return 0x7F800000u | (rng() & 0x80000000u); // +-inf
			default: return 0x7FC00000u | (rng() & 0x8007FFFFu); // NaN
		}
	}

	// One randomized draw: build vertices, emit a subset of prims (mirroring the
	// GSState watermark/provoking accumulation), and compare FmmFinish against
	// the legacy-walk oracle bit-exactly. Returns false when no prim was emitted.
	bool RunFmmCase(std::mt19937& rng, bool iip, bool tme, bool fst, bool color, FmmQMode qmode, bool special_st,
		bool strip_shaped)
	{
		const int prim_count = 1 + (rng() % 8);
		const int vertex_count = strip_shaped ? (prim_count + 2) : (prim_count * 3);

		GSVector4i m0s[26];
		GSVector4i m1s[26];

		const u32 const_q = (qmode == FmmQMode::ConstantSpecial) ? RandomSpecialFloatBits(rng) : RandomNormalFloatBits(rng);

		for (int i = 0; i < vertex_count; i++)
		{
			const u32 s_bits = special_st && (rng() % 4 == 0) ? RandomSpecialFloatBits(rng) : RandomNormalFloatBits(rng);
			const u32 t_bits = special_st && (rng() % 4 == 0) ? RandomSpecialFloatBits(rng) : RandomNormalFloatBits(rng);
			const u32 q_bits = (qmode == FmmQMode::Varying) ? RandomNormalFloatBits(rng) : const_q;

			m0s[i] = GSVector4i(static_cast<int>(s_bits), static_cast<int>(t_bits), static_cast<int>(rng()),
				static_cast<int>(q_bits));
			m1s[i] = GSVector4i(static_cast<int>((rng() & 0xFFFF) | (rng() << 16)), static_cast<int>(rng()),
				static_cast<int>(rng()), static_cast<int>(rng()));
		}

		// Emit prims, skipping some (culled — excluded from both sides).
		u16 index[24 * 3];
		int icount = 0;
		u32 watermark = 0;

		GSVertexKernels::FmmAcc acc;
		GSVertexKernels::FmmAccReset(acc, tme, fst);

		for (int p = 0; p < prim_count; p++)
		{
			const u16 i0 = static_cast<u16>(strip_shaped ? p + 0 : p * 3 + 0);
			const u16 i1 = static_cast<u16>(strip_shaped ? p + 1 : p * 3 + 1);
			const u16 i2 = static_cast<u16>(strip_shaped ? p + 2 : p * 3 + 2);

			// Culled prims are excluded from both sides; an accepted prim after a
			// culled run still accumulates the culled prims' shared vertices (they
			// sit at/above the watermark), mirroring VertexKickDirect.
			if (rng() % 3 == 0)
				continue;

			index[icount++] = i0;
			index[icount++] = i1;
			index[icount++] = i2;

			// Mirror VertexKickDirect: buffer-accumulate referenced vertices at or
			// above the watermark (color only under iip), then the provoking vertex
			// with color always.
			for (u32 j = std::max<u32>(watermark, i2 >= 2 ? i2 - 2 : 0); j < i2; j++)
				GSVertexKernels::FmmAccumVertex(acc, m0s[j], m1s[j], tme, fst, iip);
			GSVertexKernels::FmmAccumVertex(acc, m0s[i2], m1s[i2], tme, fst, true);
			watermark = static_cast<u32>(i2) + 1;
		}

		if (icount == 0)
			return false;

		GIFRegXYOFFSET ofs;
		ofs.U64 = 0;
		ofs.OFX = rng() & 0xFFFF;
		ofs.OFY = rng() & 0xFFFF;
		const u32 tw = rng() % 11;
		const u32 th = rng() % 11;

		GSVertexKernels::FmmResult r;
		GSVertexKernels::FmmFinish(acc, tme, fst, color, ofs, tw, th, r);

		FmmRefOut ref;
		RefFindMinMaxTriangle(m0s, m1s, index, icount, iip, tme, fst, color, ofs, tw, th, ref);

		EXPECT_TRUE(GSVector4i::cast(r.min_p).eq(GSVector4i::cast(ref.min_p))) << "min_p divergence";
		EXPECT_TRUE(GSVector4i::cast(r.max_p).eq(GSVector4i::cast(ref.max_p))) << "max_p divergence";
		EXPECT_TRUE(GSVector4i::cast(r.min_t).eq(GSVector4i::cast(ref.min_t))) << "min_t divergence";
		EXPECT_TRUE(GSVector4i::cast(r.max_t).eq(GSVector4i::cast(ref.max_t))) << "max_t divergence";
		EXPECT_TRUE(r.min_c.eq(ref.min_c)) << "min_c divergence";
		EXPECT_TRUE(r.max_c.eq(ref.max_c)) << "max_c divergence";
		EXPECT_EQ(r.write_nan, ref.wrote_nan) << "write_nan policy divergence";
		if (r.write_nan)
			EXPECT_EQ(r.nan_value, ref.nan_value) << "nan divergence";
		return true;
	}

	void RunFmmSweep(u32 seed, int iters, FmmQMode qmode, bool special_st)
	{
		std::mt19937 rng(seed);

		for (int i = 0; i < iters; i++)
		{
			const bool iip = rng() & 1;
			const bool tme = rng() & 1;
			const bool fst = rng() & 1;
			const bool color = (rng() % 4) != 0;
			const bool strip = rng() & 1;

			RunFmmCase(rng, iip, tme, fst, color, qmode, special_st, strip);

			if (::testing::Test::HasFailure())
			{
				ADD_FAILURE() << "seed " << seed << " iter " << i;
				return;
			}
		}
	}
} // namespace

TEST(GsVertexFmm, ConstantQSweep)
{
	// Constant normal Q, finite S/T: must match the legacy walk bit-exactly.
	RunFmmSweep(0x67763320, 150000, FmmQMode::ConstantNormal, false);
}

TEST(GsVertexFmm, SpecialStSweep)
{
	// Inf/NaN S/T mixed in: the per-vertex NaN masking and tnan reporting must
	// match the legacy walk bit-exactly.
	RunFmmSweep(0x67763321, 150000, FmmQMode::ConstantNormal, true);
}

TEST(GsVertexFmm, SpecialQSweep)
{
	// Zero/denormal/inf/NaN Q: NaN quotients mask per lane exactly as legacy.
	RunFmmSweep(0x67763322, 150000, FmmQMode::ConstantSpecial, false);
}

TEST(GsVertexFmm, VaryingQSweep)
{
	// Per-vertex Q (the perspective-projection common case): one divide per
	// unique vertex must reproduce the legacy per-pair divide walk bit-exactly.
	RunFmmSweep(0x67763323, 150000, FmmQMode::Varying, false);
}

#endif // ARCH_ARM64
