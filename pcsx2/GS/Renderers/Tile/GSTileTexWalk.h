// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The texture-coordinate walk's CPU half.
//
// The software renderer does not evaluate a texture coordinate at a pixel, it walks
// it: the vertex coordinates go through ConvertVertexBuffer into the scanline's own
// units (16.16 texels held in float, divided by Q up front where Q is constant), take
// the bilinear half-texel off at the VERTICES where the walk is affine, and then the
// rasterizer seeds each row and the scanline steps along it in blocks of four. Under
// FST the step is integer and truncating; under perspective it is float and
// accumulating. The Tile fragment shader replays that walk per fragment from a
// per-primitive payload (PS_TILE_TWALK in tfx.glsl), and everything below is what the
// renderer computes on the CPU to build that payload — pure functions of the
// software renderer's inputs, so the payload builder and its tests share one body.
//
// Every rule here is transcribed from GSRendererSW::Draw / GetScanlineGlobalData and
// GSRasterizer::DrawSprite and says so at the site. The vertex conversion itself is
// NOT mirrored: the builder calls GSVertexSW::s_cvb, the software renderer's own
// converter, so the seeds it hands the setup carry the same roundings by identity.

#include "GS/GSVector.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSRegs.h"

#include <cmath>

// GSRendererSW::Draw's q_div: whether ConvertVertexBuffer divides S and T by Q per
// vertex (a constant Q other than one, or a sprite, outside mipmapping) — the
// division the scanline then never repeats.
inline u32 gsTileTexWalkQDiv(u32 primclass, bool mip, bool eq_q, float q_min)
{
	return (!mip && ((eq_q && q_min != 1.0f) || (!eq_q && primclass == GS_SPRITE_CLASS))) ? 1u : 0u;
}

// GetScanlineGlobalData's effective sel.fst: the scanline walks the coordinate as a
// truncating integer DDA when the primitive says FST, and outside mipmapping also
// when Q is constant or the primitive is a sprite (the per-vertex division above
// having done the perspective once). Under mipmapping only the register decides.
inline bool gsTileTexWalkFst(u32 primclass, bool prim_fst, bool mip, bool eq_q)
{
	return prim_fst || (!mip && (eq_q || primclass == GS_SPRITE_CLASS));
}

// GetScanlineGlobalData's half-texel pre-shift: an affine bilinear draw outside
// mipmapping takes the 0x8000 (half a texel in 16.16) off every VERTEX before setup,
// so the walk carries it; a mipmapping draw takes it per level in the sampler.
inline bool gsTileTexWalkPreshift(bool fst_eff, bool mip, bool ltf)
{
	return !mip && ltf && fst_eff;
}

// The pre-shift itself, on a converted vertex: (t - half).xyzw(t), so only s and t move.
inline void gsTileTexWalkApplyPreshift(GSVertexSW& v)
{
	const GSVector4 half(0x8000, 0x8000);
	const GSVector4 t = v.t;
	v.t = (t - half).xyzw(t);
}

// One sprite's walk parameters: what GSRasterizer::DrawSprite computes before its
// first DrawScanline, in the order it computes it.
struct GSTileTexWalkSprite
{
	float s0; // the s lane seeded at (left, top): fma(dt.x, left - x0, t0.x)
	float t0; // the t lane seeded at (left, top): fma(dt.y, top - y0, t0.y)
	float q;  // the q lane, constant over the sprite
	float dtx; // ds per pixel (the setup's dscan.t.x)
	float dty; // dt per row (the rasterizer's dedge.t.y), added once per row after the first
	int left;  // the span's first pixel, scissor-clamped: r.left
	int top;   // the first row, scissor-clamped: r.top
	int bottom;
	// True when every row's sequential float add lands exactly, so t at row y is
	// fma(dty, y - top, t0) with no loop: t0 and dty are both multiples of 4096 (the
	// U<<12 grid ConvertVertexBuffer places FST coordinates on) and the sums stay
	// under 2^36, where such multiples are exactly representable.
	bool rows_exact;
};

// Returns false when the sprite covers no row (DrawSprite returns before any
// arithmetic then, and no fragment exists to read the payload).
inline bool gsTileTexWalkSprite(const GSVertexSW& v0, const GSVertexSW& v1, const GSVector4i& scissor, GSTileTexWalkSprite& out)
{
	// DrawSprite's corner ordering: per axis, the smaller position with its own
	// coordinate.
	const GSVector4 mask = (v0.p < v1.p).xyzw(GSVector4::zero());
	const GSVector4 p0 = v1.p.blend32(v0.p, mask);
	const GSVector4 t0 = v1.t.blend32(v0.t, mask);
	const GSVector4 p1 = v0.p.blend32(v1.p, mask);
	const GSVector4 t1 = v0.t.blend32(v1.t, mask);

	GSVector4i r(p0.xyxy(p1).ceil());
	r = r.rintersect(scissor);
	if (r.rempty())
		return false;

	// dt = duv / dxy, per lane, IEEE; the seed is ONE fused multiply-add per lane
	// (objdump: fmla), t0 + dt * prestep with prestep = (left, top) - p0.
	const GSVector4 dxy = p1 - p0;
	const GSVector4 duv = t1 - t0;
	const GSVector4 dt = duv / dxy;
	const GSVector4 prestep = GSVector4(r.left, r.top) - p0;

	out.s0 = std::fmaf(dt.x, prestep.x, t0.x);
	out.t0 = std::fmaf(dt.y, prestep.y, t0.y);
	out.q = t0.z;
	out.dtx = dt.x;
	out.dty = dt.y;
	out.left = r.left;
	out.top = r.top;
	out.bottom = r.bottom;

	const auto on_grid = [](float v) { return std::isfinite(v) && std::fmod(v, 4096.0f) == 0.0f; };
	const float reach = std::fabs(out.t0) + static_cast<float>(r.bottom - r.top) * std::fabs(out.dty);
	out.rows_exact = on_grid(out.t0) && on_grid(out.dty) && reach < 68719476736.0f /* 2^36 */;
	return true;
}
