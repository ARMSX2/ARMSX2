// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// Two different questions get asked about a draw's alpha framebuffer mask, and after the exact
/// alpha drop they have two different answers.
///
/// The drop clears the alpha byte of FBMSK when the target already holds those bits at the value
/// this draw would write, so the write lands whole and the shader needs no read-back of the
/// destination. What that changes is the shader and the barrier: after the drop, `ps.fbmask` is
/// off and nothing in the pipeline is merging a destination alpha byte.
///
/// It does not change what the draw asked for. A decision like "does this draw partially mask
/// alpha, so the target must come out of RTA alpha scaling?" is about the draw, and reading it off
/// `ps.fbmask` gets the wrong answer once the drop has cleared that flag -- the target then stays
/// scaled where it used to de-correlate, and the round trip through the scaled representation moves
/// colour by a unit or two. Those decisions ask for AsRequested() instead, which gives back the
/// mask the drop took away.
///
/// The rule is a header of its own so the distinction is written down once and can be tested
/// without a GS device.
namespace GSDrawAlphaMask
{
	/// What ExactDropped() means when this draw dropped nothing.
	inline constexpr int NothingDropped = -1;

	/// The alpha mask this draw asked for.
	///
	/// `dropped` is the alpha byte the exact drop cleared, or NothingDropped. `shader_masks` and
	/// `shader_alpha_mask` are the live GSHWDrawConfig pair (`ps.fbmask`, `cb_ps.FbMask.a`) --
	/// what the shader will actually do, which is what every other reader wants.
	inline constexpr u32 AsRequested(int dropped, bool shader_masks, u32 shader_alpha_mask)
	{
		if (dropped != NothingDropped)
			return static_cast<u32>(dropped) & 0xFFu;

		return shader_masks ? (shader_alpha_mask & 0xFFu) : 0u;
	}

	/// Whether a mask holds back some alpha bits but not all of them. Neither end is partial: a
	/// zero mask writes the whole byte, an 0xFF mask writes none of it, and in both cases the
	/// target's alpha stays describable without reading the mask.
	inline constexpr bool IsPartial(u32 alpha_mask)
	{
		return alpha_mask != 0u && alpha_mask != 0xFFu;
	}
} // namespace GSDrawAlphaMask
