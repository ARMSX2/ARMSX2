// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include "GS/Renderers/HW/GSAlphaKnownBits.h"

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

	/// What an exact-alpha rule can do with a draw that has passed every structural precondition
	/// -- alpha the only partially masked channel, 32 bits both sides, no shuffle, no AA1 coverage
	/// alpha, the target not RTA-scaled.
	enum class ExactVerdict : u8
	{
		TargetUnknown, ///< the target does not know every bit the mask holds back; nothing to do
		Drop, ///< the mask is the identity: clear it and let the source's own alpha land
		Substitute, ///< the masked bits are known but the source does not already carry them
	};

	/// Which of the two the draw gets. `masked` is the alpha byte of the mask (non-zero and not
	/// the whole byte, by the caller's preconditions), `target` what the render target is known to
	/// hold, and [src_lo, src_hi] the fragment alpha the draw would write.
	///
	/// The drop wins wherever both apply. It is the cheaper of the two -- no shader bit, no
	/// constant, no permutation -- and where the source already carries the target's bits the two
	/// write the same byte.
	///
	/// Substitution needs less than the drop, not more: the drop writes the source's own bits
	/// through the hole the mask used to cover, so it needs the source to be constant there and to
	/// agree with the target. Substitution writes the target's known bits instead, so what the
	/// source holds on those bits never reaches the framebuffer and does not have to be anything
	/// in particular. Both need the same thing of the target: that its knowledge is exact.
	inline constexpr ExactVerdict DecideExact(GSAlphaKnownBits::Known target, u8 masked, u8 src_lo, u8 src_hi)
	{
		if (masked == 0 || (target.bits & masked) != masked)
			return ExactVerdict::TargetUnknown;

		if (GSAlphaKnownBits::MaskIsIdentity(target, masked, src_lo, src_hi))
			return ExactVerdict::Drop;

		return ExactVerdict::Substitute;
	}

	/// The two constants a substituting shader needs, so it can produce the masked write's own
	/// result -- (src.a & ~M) | (known & M) -- out of one AND and one OR and no negation.
	///
	/// `keep` is every bit the source keeps, as a full 32-bit word, so the AND leaves an alpha
	/// above 255 alone exactly as the masked-write road's `& ~FbMask` does. `value` is what the
	/// target is known to hold on the masked bits, already narrowed to them.
	struct Substitution
	{
		u32 keep = 0;
		u32 value = 0;

		constexpr bool operator==(const Substitution& r) const { return keep == r.keep && value == r.value; }
	};

	inline constexpr Substitution SubstitutionFor(u8 masked, u8 known_value)
	{
		return {~static_cast<u32>(masked), static_cast<u32>(known_value & masked)};
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

	/// Whether an exact alpha-mask drop that was held over the blend selection still stands.
	///
	/// A drop is worth taking for one thing: the barrier it removes, and with it, on a device with
	/// no framebuffer fetch, the render-target clone the barrier becomes. So if the blend the draw
	/// ended up with needs a barrier for its own reasons, the barrier is there whether the mask is
	/// or not, and the drop has bought nothing -- the draw is better off back on the road it asked
	/// for, with the same shader, the same blend and the same pixels it had before the rule
	/// existed. `blend_requires_barrier` is the post-selection barrier state, one or full.
	inline constexpr bool DropStandsAfterBlend(bool blend_requires_barrier)
	{
		return !blend_requires_barrier;
	}

	/// Whether a held substitution still stands after the blend selection.
	///
	/// It wants what the drop wants -- no barrier of the blend's own, or the barrier is there
	/// either way and the substitution has bought nothing -- and one thing more. Colclip hardware
	/// makes the masked-write road read the destination as `sample * 65535` on all four channels
	/// including alpha, so the byte that road merges is not the tracked one and writing the
	/// tracked one is not the same answer. The predicate at the framebuffer-mask site cannot see
	/// that flag, because EmulateBlending sets it afterwards; holding the decision over the blend
	/// is what makes it visible in time to refuse.
	inline constexpr bool SubstitutionStandsAfterBlend(bool blend_requires_barrier, bool colclip_hw)
	{
		return DropStandsAfterBlend(blend_requires_barrier) && !colclip_hw;
	}

	/// Whether a draw whose exact alpha-mask decision is still held has a one-barrier road.
	///
	/// The decision takes the mask off the shader but only defers the barrier it required --
	/// ResolveHeldAlphaMask puts mask and barrier back together if anything downstream needs a
	/// barrier anyway. So every road chosen in between (the blend, the alpha test) has to be
	/// chosen as if the barrier were there. Reading the live flag instead sends the draw down a
	/// road it would not have taken with the mask on, and those roads are not always the same
	/// pixels: the two-pass alpha-test road composites RGB out of order where overlapping
	/// primitives meet, which is the whole reason the feedback road is preferred when a barrier
	/// is already paid for.
	///
	/// `live_one_barrier` is m_conf.require_one_barrier as it stands; `mask_held` says a decision
	/// is outstanding.
	inline constexpr bool OneBarrierWithHeldMask(bool live_one_barrier, bool mask_held)
	{
		return live_one_barrier || mask_held;
	}

	/// Whether a mask holds back some alpha bits but not all of them. Neither end is partial: a
	/// zero mask writes the whole byte, an 0xFF mask writes none of it, and in both cases the
	/// target's alpha stays describable without reading the mask.
	inline constexpr bool IsPartial(u32 alpha_mask)
	{
		return alpha_mask != 0u && alpha_mask != 0xFFu;
	}
} // namespace GSDrawAlphaMask
