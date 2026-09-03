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

	/// Whether the shader has to quantize the colour on its own account.
	///
	/// A draw that keeps a framebuffer mask runs the shader's masked-write road, and that road
	/// turns the colour into integers on all four channels before it merges the destination in --
	/// not only on the channels the mask touches. Off that road the colour stays fractional and
	/// the output stage rounds it to nearest, which is a unit of colour of difference on every
	/// pixel the draw covers, and another unit wherever a later draw blends against it. So a draw
	/// the drop took off the road has to quantize anyway.
	///
	/// Both arguments are the shader's four-channel mask nibble (`ps.fbmask`): `requested` as the
	/// draw asked for it, `emulated` as it stands after the drop. A drop that leaves some other
	/// channel partially masked leaves the draw on the road, where the quantization already
	/// happens, so only a drop to nothing needs it put back.
	inline constexpr bool NeedsColorQuantize(u32 requested, u32 emulated)
	{
		return requested != 0u && emulated == 0u;
	}

	/// Whether this draw's primary colour output alpha already carries a value of its own, so
	/// nothing downstream may claim the byte for something else.
	///
	/// The blend-mix factor substitution does exactly that on a GPU with no dual-source blend unit:
	/// with nothing keeping the pass's alpha, it overwrites the byte with the blend factor
	/// (ps.blend_factor_in_alpha), or takes the target into RTA scaling so the byte already reads
	/// as one. Its own guard was "no shuffle and no fbmask", which was complete until the exact
	/// alpha drop started clearing ps.fbmask on a draw that still means to write a particular
	/// alpha byte. Reading the mask the draw ASKED for closes it: a dropped draw counts as spoken
	/// for, exactly as it did before the drop existed.
	///
	/// `shader_masks_any_channel` is the live ps.fbmask flag; `requested_alpha_mask` is
	/// AsRequested() above. Neither the M2 nor any desktop GPU takes this road -- they all have a
	/// dual-source blend unit -- so the coupling is only reachable on Mali and under
	/// EmuCore/GS/DisableDualSourceBlend.
	inline constexpr bool AlphaOutputIsSpokenFor(bool shader_masks_any_channel, u32 requested_alpha_mask)
	{
		return shader_masks_any_channel || requested_alpha_mask != 0;
	}

	/// Whether a mask holds back some alpha bits but not all of them. Neither end is partial: a
	/// zero mask writes the whole byte, an 0xFF mask writes none of it, and in both cases the
	/// target's alpha stays describable without reading the mask.
	inline constexpr bool IsPartial(u32 alpha_mask)
	{
		return alpha_mask != 0u && alpha_mask != 0xFFu;
	}
} // namespace GSDrawAlphaMask
