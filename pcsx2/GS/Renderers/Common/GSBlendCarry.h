// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

// Blend carry-over: a draw whose blend was elided as the identity takes the blend of the blended
// draw submitted just before it, so the backend does not switch pipelines twice around it.
//
// GS ALPHA with A = Cs, B = Cd, C = As, D = Cd is (Cs - Cd) * As + Cd. When every alpha the draw
// can produce is exactly 1.0 that is Cs, and GSState::IsOpaque() says so (its alpha range clause),
// so the renderer submits the draw with no blend. A run of such draws between draws of the same
// equation with a varying alpha makes the backend bind a no-blend pipeline for each of them and the
// blend-mix pipeline again after it, and each pipeline switch costs CPU time.
//
// The blend-mix form of the equation at As = 1.0 writes the same pixels as the elided form: the
// shader outputs Cs * As and the unit adds Cd * (1 - As), which at As = 1.0 is Cs + 0 exactly. So
// the elided draw can take the blended draw's blend bits and blend state, provided the two agree on
// everything else that selects a pipeline or reaches the output (the pixel shader selector outside
// the blend bits, colour mask, depth state, ALPHA register, render target and depth target) and
// both are plain 32-bit colour draws (no target read, barrier, second pass, destination-alpha test
// or colour clamp emulation).
//
// Pure functions over the draw configuration, so the decision can be tested off-device.
namespace GSBlendCarry
{
	using PSS = GSHWDrawConfig::PSSelector;

	/// The last blended draw of the donor class, and what it was submitted with.
	struct Donor
	{
		bool valid = false;
		PSS ps;
		GSHWDrawConfig::BlendState blend;
		GSHWDrawConfig::ColorMaskSelector colormask;
		GSHWDrawConfig::DepthStencilSelector depth;
		const void* rt = nullptr; ///< Only compared, never dereferenced.
		const void* ds = nullptr;
		u32 alpha_abcd = 0;
	};

	constexpr u32 PackAlpha(u32 a, u32 b, u32 c, u32 d)
	{
		return a | (b << 2) | (c << 4) | (d << 6);
	}

	/// The ALPHA register of both forms: A = Cs, B = Cd, C = As, D = Cd.
	constexpr u32 kIdentityAlpha = PackAlpha(0, 1, 0, 1);

	/// The blend-related selector bits: what differs between a draw submitted without its identity
	/// blend and the same draw with the blend-mix form of it. Everything outside this set must agree
	/// for one to take the other's.
	inline PSS WithoutBlendBits(PSS p)
	{
		p.blend_a = p.blend_b = p.blend_c = p.blend_d = 0;
		p.blend_hw = 0;
		p.a_masked = 0;
		p.blend_mix = 0;
		p.round_inv = 0;
		p.pabe = 0;
		p.no_color1 = 0;
		p.blend_factor_in_alpha = 0;
		p.af_in_src1 = 0;
		return p;
	}

	/// What the two forms have in common: a plain 32-bit colour draw with no feedback of any kind.
	inline bool PlainShape(const GSHWDrawConfig& c)
	{
		const PSS& p = c.ps;
		return p.dst_fmt == 0 && !p.colclip && !p.colclip_hw && !p.dither && !p.fbmask && !p.shuffle && !p.tex_is_fb && !p.no_color &&
		       !p.abe && !p.stencil_counter && c.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Off && !c.require_one_barrier &&
		       !c.require_full_barrier && !c.alpha_second_pass.enable && !c.blend_multi_pass.enable &&
		       c.colclip_mode == GSHWDrawConfig::ColClipMode::NoModify && c.tex_hazard == GSHWDrawConfig::TEX_HAZARD_NONE && !c.line_expand &&
		       c.colormask.logic_op == 0 && c.date_copy == 0 && c.logic_op_split == 0;
	}

	/// A draw submitted with its blend elided: plain, no blend, and every blend-set bit clear.
	inline bool IsElidedIdentity(const GSHWDrawConfig& c)
	{
		const PSS& p = c.ps;
		return !c.blend.enable && PlainShape(c) && p.blend_mix == 0 && p.blend_hw == 0 && !p.pabe && !p.a_masked &&
		       !p.blend_factor_in_alpha && !p.af_in_src1;
	}

	/// A blended draw of the equation above in the blend-mix form that an elided draw can take over.
	inline bool IsDonorClass(const GSHWDrawConfig& c, u32 alpha_abcd)
	{
		const PSS& p = c.ps;
		return alpha_abcd == kIdentityAlpha && c.blend.enable && PlainShape(c) && p.blend_mix == 1 && p.blend_hw == 0 && p.blend_c == 0 &&
		       !p.pabe && !p.a_masked && !p.blend_factor_in_alpha && !p.af_in_src1;
	}

	inline Donor MakeDonor(const GSHWDrawConfig& c, const void* rt, const void* ds, u32 alpha_abcd)
	{
		Donor d;
		d.valid = true;
		d.ps = c.ps;
		d.blend = c.blend;
		d.colormask = c.colormask;
		d.depth = c.depth;
		d.rt = rt;
		d.ds = ds;
		d.alpha_abcd = alpha_abcd;
		return d;
	}

	inline bool Matches(const Donor& d, const GSHWDrawConfig& c, const void* rt, const void* ds, u32 alpha_abcd)
	{
		return d.valid && d.rt == rt && d.ds == ds && d.alpha_abcd == alpha_abcd && d.colormask.key == c.colormask.key &&
		       d.depth.key == c.depth.key && WithoutBlendBits(d.ps) == WithoutBlendBits(c.ps);
	}

	/// Copies the donor's blend bits and blend state into `c`.
	inline void Adopt(const Donor& d, GSHWDrawConfig& c)
	{
		PSS& p = c.ps;
		p.blend_a = d.ps.blend_a;
		p.blend_b = d.ps.blend_b;
		p.blend_c = d.ps.blend_c;
		p.blend_d = d.ps.blend_d;
		p.blend_hw = d.ps.blend_hw;
		p.a_masked = d.ps.a_masked;
		p.blend_mix = d.ps.blend_mix;
		p.round_inv = d.ps.round_inv;
		p.pabe = d.ps.pabe;
		p.no_color1 = d.ps.no_color1;
		p.blend_factor_in_alpha = d.ps.blend_factor_in_alpha;
		p.af_in_src1 = d.ps.af_in_src1;
		c.blend = d.blend;
	}
} // namespace GSBlendCarry
