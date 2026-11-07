// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"

// The colour-wrap counter, counted in the stencil buffer.
//
// Shadow of the Colossus and Spider-Man 3 count shadow-volume faces in the colour channels: an
// untextured draw of flat triangles blended Cs * 1 + Cd with colour clamping off, each triangle's
// colour a step of +s or -s (255 is -1 on a 32-bit target; 0x0b and 0xf9 are +8 and -8 once a
// 16-bit target drops the low three bits). The renderer's general road for colour wrap draws it
// into a 16-bit-per-channel copy of the target and folds the sum back mod 256, which costs a 64-bit
// read-modify-write blend per covered pixel; these draws cover the target 11 to 42 times.
//
// The step need not be the same in every channel, and channels the draw does not write do not count:
// God of War II and Stuntman count in fewer channels than they carry. Per written channel c,
// what the draw computes per pixel is Cd + s_c * (n_plus - n_minus) mod 256, where n_plus
// and n_minus count the primitives of each sign whose fragment there passes the depth test. The
// stencil unit's INCR_WRAP and DECR_WRAP keep n_plus - n_minus mod 256 exactly, at any depth of
// overlap, and s * k mod 256 depends on k only mod 256. So the backend draws the +s primitives with
// INCR_WRAP and the -s ones with DECR_WRAP into a cleared stencil, then one full-area pass writes
// (Cd + s * k) mod 256 into the colour channels the draw writes. Addition mod 256 commutes, and the
// draw writes no depth, so grouping the primitives by sign changes nothing a fragment sees.
//
// The draw's alpha, where it writes one, is the vertex alpha wherever a fragment passes, with no
// blend: the counter pass writes it as usual, which is order-independent once every primitive has
// the same alpha.
namespace GSStencilCounter
{
	/// What one primitive adds to the counter.
	enum class Sign : u8
	{
		Zero,        ///< adds 0 to every channel
		Plus,        ///< adds +step
		Minus,       ///< adds -step (256 - step)
		NotACounter, ///< channels differ, or a step other than +-step
	};

	/// The mask the pixel shader applies to the source colour before the add, mirroring the
	/// preprocessor logic of ps_color_clamp_wrap in the Vulkan tfx.glsl: a 16-bit destination keeps
	/// the top five bits of each channel. Only meaningful for the draws SetupStencilCounterDraw lets
	/// through (no fog, no texture, colclip_hw), whose source colour is the vertex colour.
	inline u8 SourceColourMask(const GSHWDrawConfig::PSSelector& ps)
	{
		const bool fmt16 = (ps.dst_fmt == GSLocalMemory::PSM_FMT_16);
		const bool sw_path = ps.blend_a || ps.blend_b || ps.blend_d || (ps.dither > 0 && ps.dither < 3) ||
		                     ps.fbmask || ps.quantize_color || ps.substitute_alpha;
		if (sw_path)
			return (fmt16 && ps.dither != 3 && (ps.blend_mix == 0 || ps.dither > 0)) ? 0xF8 : 0xFF;
		return (fmt16 && ps.dither != 3 && ps.blend_mix == 0 && ps.blend_hw == 0) ? 0xF8 : 0xFF;
	}

	/// The step a primitive adds to each colour channel the draw writes (channels it does not write are 0).
	struct Step
	{
		u8 c[3] = {};

		constexpr bool IsZero() const { return (c[0] | c[1] | c[2]) == 0; }
		constexpr bool operator==(const Step& o) const { return c[0] == o.c[0] && c[1] == o.c[1] && c[2] == o.c[2]; }
		constexpr Step Negated() const
		{
			return Step{{static_cast<u8>(256 - c[0]), static_cast<u8>(256 - c[1]), static_cast<u8>(256 - c[2])}};
		}
	};

	/// The step a primitive of colour (r, g, b) adds to the channels in `rgb_mask` (bit 0 red), after the
	/// shader's source mask.
	constexpr Step StepOf(u8 r, u8 g, u8 b, u8 rgb_mask, u8 mask)
	{
		return Step{{static_cast<u8>((rgb_mask & 1) ? (r & mask) : 0), static_cast<u8>((rgb_mask & 2) ? (g & mask) : 0),
			static_cast<u8>((rgb_mask & 4) ? (b & mask) : 0)}};
	}

	/// Classifies one primitive's step against the draw's +step, which the first primitive that adds
	/// anything sets (`have_step` false until then). A counter's primitives all add +step or -step to
	/// every written channel at once; any other mix is not one count. A step that is its own negative
	/// (every channel 0 or 128) counts as Plus.
	constexpr Sign Classify(const Step& s, Step& step, bool& have_step)
	{
		if (s.IsZero())
			return Sign::Zero;
		if (!have_step)
		{
			step = s;
			have_step = true;
		}
		if (s == step)
			return Sign::Plus;
		if (s == step.Negated())
			return Sign::Minus;
		return Sign::NotACounter;
	}

	/// The value the resolve writes into one channel: the count k is the stencil, n_plus - n_minus
	/// mod 256.
	constexpr u8 Resolve(u8 cd, u8 step, u8 k)
	{
		return static_cast<u8>(cd + step * k);
	}
} // namespace GSStencilCounter
