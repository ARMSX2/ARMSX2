// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include "GS/Renderers/HW/GSAlphaKnownBits.h"

/// Rules for a draw's alpha framebuffer mask, which has two meanings once the exact alpha drop
/// has run: what the shader does, and what the draw asked for.
///
/// The drop clears the alpha byte of FBMSK when the target already holds those bits at the value
/// this draw would write, so the shader needs no destination read-back. After the drop,
/// `ps.fbmask` is off.
///
/// Decisions about the draw itself (e.g. "does it partially mask alpha, so the target must leave
/// RTA alpha scaling?") must use AsRequested(), not `ps.fbmask`. Reading the cleared flag keeps the
/// target scaled and the scaled round trip moves colour by a unit or two.
///
/// Kept in its own header so it can be tested without a GS device.
namespace GSDrawAlphaMask
{
	/// What ExactDropped() means when this draw dropped nothing.
	inline constexpr int NothingDropped = -1;

	/// What the exact alpha-mask rules decided about a draw.
	///
	/// An unknown target means nothing can be done. The two Substitute values both mean the target
	/// knows the masked bits, so the shader can write them itself; they differ in whether the
	/// source alpha was constant on those bits or varied across the draw.
	enum ExactAlphaDrop : u8
	{
		ExactAlphaDropNotConsidered = 0, ///< not an alpha-only partial mask on a 32-bit target
		ExactAlphaDropTaken, ///< the mask was the identity; it was cleared
		ExactAlphaDropIneligible, ///< an alpha-only partial mask, refused on a precondition
		ExactAlphaDropTargetUnknown, ///< the target does not know the bits the mask holds back
		ExactAlphaDropSubstituteVarying, ///< known, and the fragment alpha straddles one of those bits
		ExactAlphaDropSubstituteLoadBearing, ///< known and constant, and different: the mask is doing work
	};

	/// Whether a verdict is one of the two the substitution acts on: the target knows every bit
	/// the mask holds back, but the source does not already carry them, so the drop cannot apply.
	inline constexpr bool IsExactAlphaSubstitute(u8 decision)
	{
		return decision == ExactAlphaDropSubstituteVarying || decision == ExactAlphaDropSubstituteLoadBearing;
	}

	/// The alpha mask this draw asked for.
	///
	/// `dropped` is the alpha byte the exact drop cleared, or NothingDropped. `shader_masks` and
	/// `shader_alpha_mask` are the live GSHWDrawConfig pair (`ps.fbmask`, `cb_ps.FbMask.a`), i.e.
	/// what the shader will actually do.
	inline constexpr u32 AsRequested(int dropped, bool shader_masks, u32 shader_alpha_mask)
	{
		if (dropped != NothingDropped)
			return static_cast<u32>(dropped) & 0xFFu;

		return shader_masks ? (shader_alpha_mask & 0xFFu) : 0u;
	}

	/// Whether the shader has to quantize the colour on its own account.
	///
	/// The shader's masked-write path converts all four channels to integers before merging the
	/// destination. Without it the colour stays fractional and the output stage rounds to
	/// nearest, which differs by a unit of colour. So a draw the drop took off that path must
	/// still quantize.
	///
	/// Both arguments are the four-channel mask nibble (`ps.fbmask`): `requested` as the draw
	/// asked for it, `emulated` after the drop. If any channel is still masked the draw stays on
	/// the masked-write path, so only a drop to nothing needs quantization put back.
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
	/// The drop wins wherever both apply: it is cheaper (no shader bit, no constant) and writes the
	/// same byte.
	///
	/// The drop writes the source's own bits where the mask was, so the source must be constant
	/// there and agree with the target. Substitution writes the target's known bits instead, so the
	/// source can hold anything on them. Both require the target's knowledge to be exact.
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
	/// above 255 alone, matching the masked-write path's `& ~FbMask`. `value` is what the
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
	/// Without dual-source blending, the blend-mix factor substitution overwrites the alpha byte
	/// with the blend factor (ps.blend_factor_in_alpha) or moves the target into RTA scaling. A
	/// draw whose alpha mask was dropped still writes a specific alpha byte, so this must check
	/// the requested mask, not only the live ps.fbmask.
	///
	/// `shader_masks_any_channel` is the live ps.fbmask flag; `requested_alpha_mask` is
	/// AsRequested() above. Only reachable on GPUs without dual-source blend.
	inline constexpr bool AlphaOutputIsSpokenFor(bool shader_masks_any_channel, u32 requested_alpha_mask)
	{
		return shader_masks_any_channel || requested_alpha_mask != 0;
	}

	/// Whether an exact alpha-mask drop that was held over the blend selection still stands.
	///
	/// The drop only pays off by removing a barrier (and, without framebuffer fetch, the
	/// render-target copy it implies). If the chosen blend needs a barrier anyway, the draw goes
	/// back to its requested mask. `blend_requires_barrier` is the post-selection barrier state,
	/// one or full.
	inline constexpr bool DropStandsAfterBlend(bool blend_requires_barrier)
	{
		return !blend_requires_barrier;
	}

	/// Whether a held substitution still stands after the blend selection.
	///
	/// Same condition as the drop, plus no colclip hardware: colclip makes the masked-write path
	/// read the destination as `sample * 65535` on all four channels, so the merged alpha byte is
	/// not the tracked one. EmulateBlending sets that flag after the framebuffer-mask site, which
	/// is why the decision is held until after blend selection.
	inline constexpr bool SubstitutionStandsAfterBlend(bool blend_requires_barrier, bool colclip_hw)
	{
		return DropStandsAfterBlend(blend_requires_barrier) && !colclip_hw;
	}

	/// Whether a draw whose exact alpha-mask decision is still held has a one-barrier road.
	///
	/// A held decision takes the mask off the shader but only defers its barrier;
	/// ResolveHeldAlphaMask restores both if anything downstream needs a barrier. So the blend and
	/// alpha-test choices in between must be made as if the barrier were there. Otherwise the
	/// draw can pick the two-pass alpha-test path, which composites RGB out of order where
	/// overlapping primitives meet.
	///
	/// `live_one_barrier` is m_conf.require_one_barrier as it stands; `mask_held` says a decision
	/// is outstanding.
	inline constexpr bool OneBarrierWithHeldMask(bool live_one_barrier, bool mask_held)
	{
		return live_one_barrier || mask_held;
	}

	/// Whether a mask holds back some alpha bits but not all of them. 0 (write all) and 0xFF
	/// (write none) are not partial.
	inline constexpr bool IsPartial(u32 alpha_mask)
	{
		return alpha_mask != 0u && alpha_mask != 0xFFu;
	}
} // namespace GSDrawAlphaMask
