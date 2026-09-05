// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <bit>

/// What a render target's alpha byte is known to hold, bit by bit.
///
/// GSTextureCache::Target already tracks an alpha min/max range, and that range cannot carry a
/// bit through a mask. A write that masks bit 7 leaves bit 7 exactly as it was, but min/max has
/// to widen to cover both the old and the new values, so a target written only through a partial
/// alpha mask sits at 0..255 forever. A per-bit (bits, value) pair can still say "bit 7 is zero
/// everywhere" across such a write.
///
/// The pair is a strict refinement of the range, maintained beside it at the same sites. Nothing
/// reads it except the exact FBMSK-drop rule; the range stays authoritative everywhere else.
///
/// These rules live in a header of their own so they can be tested without a GS device.
namespace GSAlphaKnownBits
{
	/// What last decided a target's pair. Census only -- nothing reads it to choose anything, and
	/// it costs a byte per Target. It exists because "the target does not know these bits" is the
	/// largest class the exact alpha rules refuse, and sizing any further work on that class means
	/// knowing which event destroyed the knowledge rather than guessing.
	enum class Reason : u8
	{
		NeverEstablished = 0, ///< the Target was constructed and nothing has claimed a bit since
		Unchanged, ///< sentinel for an event that left the pair alone; never stored on a Target
		CreateCleared, ///< the target was created with a clear, so every pixel held the clear alpha
		Clear, ///< a draw was turned into a clear of the whole target
		DrawFullCover, ///< a draw that reached every pixel of the valid rect
		DrawPartialCover, ///< a draw that did not, so a written bit survives only where it agreed
		ChannelShuffle, ///< alpha was rewritten from another channel; nothing describes the result
		Upload, ///< local memory was written back over the target
		Grow, ///< the valid rect grew, or the texture was reallocated larger
		Inherit, ///< the contents came from another target
		Move, ///< a rectangle was moved in, which is not a claim about the valid rect
		Clobber, ///< the texture was replaced outright
		HwHack, ///< a game-specific hack rewrote the target
	};

	struct Known
	{
		u8 bits = 0; ///< alpha bits every pixel in the target's valid rect is known to hold
		u8 value = 0; ///< what those bits hold; bits outside `bits` are always zero here

		static constexpr Known Nothing() { return {0, 0}; }
		static constexpr Known All(u8 v) { return {0xFF, v}; }

		constexpr bool operator==(const Known& r) const { return bits == r.bits && value == r.value; }
	};

	/// The bits that are the same in every value of [lo, hi]: everything above the top bit where
	/// lo and hi differ. Comparing the two endpoints bit by bit is not enough on its own --
	/// [0x00, 0x02] has bit 0 clear at both ends and set in between.
	inline constexpr u8 ConstantBits(u8 lo, u8 hi)
	{
		const unsigned diff = static_cast<unsigned>(lo ^ hi);
		return (diff == 0) ? 0xFF : static_cast<u8>((0xFFu << std::bit_width(diff)) & 0xFFu);
	}

	/// An inclusive alpha range, as the pair's arithmetic wants it: lo <= hi, both bounds real
	/// values the draw can write.
	struct Range
	{
		u8 lo, hi;
	};

	/// The bounds of {a | 0x80 : a in [lo, hi]} -- what a draw writes once FBA has forced alpha
	/// bit 7 on.
	///
	/// ORing 0x80 into the two endpoints is a bound only while the range does not straddle 128.
	/// [0x00, 0x80] becomes 0x80..0xFF, not the 0x80..0x80 the endpoint OR produces, and
	/// [0x64, 0xC8] comes out inverted at 0xE4..0xC8. Either one, handed to ConstantBits, claims
	/// bits the draw does not hold. AA1 coverage is what makes a straddling range common: the
	/// vertex trace widens to 0..128 for it.
	inline constexpr Range AfterFBA(u8 lo, u8 hi)
	{
		if (lo < 128 && hi >= 128)
			return {128, 255};
		return {static_cast<u8>(lo | 0x80), static_cast<u8>(hi | 0x80)};
	}

	/// The pair after a draw wrote the alpha bits `written` (the ones the mask let through) with
	/// a fragment alpha somewhere in [src_lo, src_hi].
	///
	/// full_cover means every pixel of the valid rect took the write, so the written bits become
	/// whatever the source is constant on, whatever was there before. Otherwise only some pixels
	/// took it, and a written bit stays known only where the source agrees with what was already
	/// known. The bits the mask held back keep their knowledge either way, which is the point.
	inline constexpr Known AfterWrite(Known prev, u8 written, u8 src_lo, u8 src_hi, bool full_cover)
	{
		const u8 held = static_cast<u8>(~written);
		const u8 src_const = ConstantBits(src_lo, src_hi);
		const u8 kept_bits = static_cast<u8>(held & prev.bits);
		const u8 kept_value = static_cast<u8>(prev.value & kept_bits);

		u8 bits, value;
		if (full_cover)
		{
			bits = static_cast<u8>((written & src_const) | kept_bits);
			value = static_cast<u8>((src_lo & written & src_const) | kept_value);
		}
		else
		{
			const u8 agree = static_cast<u8>(written & src_const & prev.bits & ~(src_lo ^ prev.value));
			bits = static_cast<u8>(agree | kept_bits);
			value = static_cast<u8>((src_lo & agree) | kept_value);
		}
		return {bits, static_cast<u8>(value & bits)};
	}

	/// The pair after local memory was written back over the target. covers_valid says the upload
	/// reached the whole valid rect, in which case it replaces what was known rather than being
	/// intersected with it.
	inline constexpr Known AfterUpload(Known prev, u8 lo, u8 hi, bool covers_valid)
	{
		const u8 fresh_bits = ConstantBits(lo, hi);
		const u8 fresh_value = static_cast<u8>(lo & fresh_bits);
		if (covers_valid)
			return {fresh_bits, fresh_value};

		const u8 agree = static_cast<u8>(fresh_bits & prev.bits & ~(fresh_value ^ prev.value));
		return {agree, static_cast<u8>(prev.value & agree)};
	}

	/// The pair after the valid rect grew, or after the texture was reallocated larger. The pixels
	/// gained have had nothing written to them, so they hold the zero the allocation was cleared
	/// to: only bits already known to be zero survive.
	inline constexpr Known AfterGrow(Known prev)
	{
		return {static_cast<u8>(prev.bits & ~prev.value), 0};
	}

	/// Whether masking `masked` off an alpha write is the identity: the target already holds those
	/// bits at a known value, and this write would put that same value there.
	inline constexpr bool MaskIsIdentity(Known target, u8 masked, u8 src_lo, u8 src_hi)
	{
		if (masked == 0)
			return false;
		if ((target.bits & masked) != masked)
			return false;
		if ((ConstantBits(src_lo, src_hi) & masked) != masked)
			return false;
		return (src_lo & masked) == (target.value & masked);
	}

	/// Whether the pair and an alpha range can describe the same pixels.
	///
	/// Necessary, not sufficient: the pair pins bits, the range bounds values, and deciding
	/// emptiness of their intersection exactly needs a search the assert does not deserve. What it
	/// does catch is the failure that matters -- the two drifting apart because a write site
	/// updated one and not the other.
	inline constexpr bool RangeAdmits(int lo, int hi, Known k)
	{
		if ((k.value & ~k.bits) != 0)
			return false;
		if (lo > hi)
			return false;
		if (lo == hi)
			return (static_cast<u8>(lo) & k.bits) == k.value;
		// The smallest and largest values the pair allows must reach into the range.
		return k.value <= hi && static_cast<int>(k.value | static_cast<u8>(~k.bits)) >= lo;
	}
} // namespace GSAlphaKnownBits
