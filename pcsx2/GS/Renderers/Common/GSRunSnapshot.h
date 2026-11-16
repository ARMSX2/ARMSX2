// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSAlphaBitLogicOp.h"
#include "GS/Renderers/Common/GSDevice.h"

// One copy of the render target for a run of draws on the copy road (GSDrawRoad.h).
//
// On that road a draw that reads its target samples a copy taken just before it, and taking the
// copy ends the render pass. Many of those draws read the target only for alpha bits their own
// frame-buffer mask keeps: the shader merges the kept bits back into what it writes, and tests
// destination alpha on bit 7. If no draw since the copy changed those bits, the copy still holds
// them, whatever happened to every other bit. So the copy is kept while the pass stays open, with
// the set of alpha bits written since it was taken, and a draw whose reads miss that set samples
// it again: no copy, no pass boundary.
//
// Why the kept copy is right for such a draw. Let S be the alpha bits written since the copy.
// A draw joins S with what it writes (DrawWrites). A draw with an emulated mask writes, on the
// bits it keeps, the value it read; it read them from the target itself or from a copy whose
// kept bits are current (below), so it leaves them as they were and they stay out of S. For a
// reader (DrawReads) whose bits miss S, every bit it reads has had no change since the copy, so
// the copy and the target agree on them and the shader computes what it would have from a fresh
// copy. Blending is not part of this: the blend unit reads the target itself.
//
// Only alpha is tracked. A draw that reads anything else from the copy (colour for blending in
// the shader, a mask on a colour channel, its own target as a texture, the alpha-fail read) takes
// a copy of its own, as before, and leaves the kept one alone.
namespace GSRunSnapshot
{
	/// What a draw reads from a copy of its target.
	struct Reads
	{
		/// The reads are confined to `alpha`. False for every other reader.
		bool alpha_only = false;
		/// The alpha bits read.
		u8 alpha = 0;

		constexpr bool operator==(const Reads&) const = default;
	};

	/// The alpha bits a draw may change in its target. 0xFF unless it provably keeps some.
	constexpr u8 WritesAlpha(bool alpha_written, bool keeps_mask, u8 mask_alpha)
	{
		if (!alpha_written)
			return 0;
		return keeps_mask ? static_cast<u8>(~mask_alpha) : 0xFF;
	}

	/// For a draw on the copy road that takes a copy (GSDrawRoad::clone_rt).
	inline Reads DrawReads(const GSHWDrawConfig& c)
	{
		const GSHWDrawConfig::PSSelector& ps = c.ps;
		GSHWDrawConfig::PSSelector other = ps;
		other.fbmask = 0;
		other.date = 0;
		// The scaled alpha of a target under RTA correction and a shuffle's swapped channels are
		// not the stored alpha byte this tracks. Extra passes select their own shaders.
		if (other.IsFeedbackLoopRT() || c.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE || c.offset_read_hits_write ||
			ps.rta_correction || ps.shuffle || ps.write_rg || ps.colclip_hw || c.alpha_second_pass.enable ||
			c.blend_multi_pass.enable)
		{
			return {};
		}

		Reads r = {true, 0};
		if (ps.fbmask)
		{
			if (c.cb_ps.FbMask.r | c.cb_ps.FbMask.g | c.cb_ps.FbMask.b)
				return {};
			r.alpha |= static_cast<u8>(c.cb_ps.FbMask.a);
		}
		if (ps.date >= 5)
			r.alpha |= 0x80;
		return r;
	}

	/// For any draw on the target.
	inline u8 DrawWrites(const GSHWDrawConfig& c)
	{
		const GSHWDrawConfig::PSSelector& ps = c.ps;
		const bool second = c.alpha_second_pass.enable;
		const bool alpha_written = c.colormask.wa || (second && c.alpha_second_pass.colormask.wa);
		// The kept bits survive only if the shader's alpha reaches the target as written: one pass,
		// no logic op, alpha not blended, and alpha not carrying a blend factor.
		const bool alpha_passes_through = !c.blend.enable || (c.blend.src_factor_alpha == GSDevice::CONST_ONE &&
																 c.blend.dst_factor_alpha == GSDevice::CONST_ZERO);
		const bool keeps_mask = ps.fbmask && !second && !c.blend_multi_pass.enable &&
		                        c.colormask.logic_op == GSAlphaBitLogicOp::Off && alpha_passes_through &&
		                        !ps.blend_factor_in_alpha && !ps.rta_correction && !ps.shuffle && !ps.write_rg &&
		                        !ps.colclip_hw && !ps.HasColorROV();
		return WritesAlpha(alpha_written, keeps_mask, static_cast<u8>(c.cb_ps.FbMask.a));
	}

	enum class Copy : u8
	{
		/// Sample the kept copy.
		Reuse,
		/// Copy the draw's area and keep it: the first reader, or the kept copy went stale.
		DrawArea,
		/// Copy the whole target and keep it. A second reader in the same pass is a run, and its
		/// draws land anywhere, so one more pass boundary pays for all that follow.
		Target,
		/// Copy for this draw alone and leave the kept copy as it is.
		Private,
	};

	/// Whether the kept copy still describes its target when a draw arrives.
	///
	/// `target_dirty`: the target has no clear or discard pending. One is only marked on the
	/// texture (GSDevice::ClearRenderTarget) and recorded when the next draw sets up its pass,
	/// after that draw has chosen its copy. So it is a write the stale set has not seen, and a
	/// fresh copy, which commits the clear first, is the only right one. A target handed out again
	/// by the texture pool arrives with one pending too.
	///
	/// `colclip_in_progress`: a colour-clip target stands in for the target, and its resolve
	/// writes the target.
	constexpr bool Live(bool kept, bool pass_still_open, bool target_dirty, bool colclip_in_progress)
	{
		return kept && pass_still_open && target_dirty && !colclip_in_progress;
	}

	/// `live`: a kept copy of this target exists and nothing but the draws accounted in `stale`
	/// wrote the target since. `covered`: the kept copy's area contains the draw's.
	constexpr Copy Decide(bool live, u8 stale, bool covered, const Reads& reads)
	{
		if (!reads.alpha_only)
			return Copy::Private;
		if (!live || (reads.alpha & stale))
			return Copy::DrawArea;
		return covered ? Copy::Reuse : Copy::Target;
	}

	// A plain draw changes every alpha bit; a masked one only what its mask lets through.
	static_assert(WritesAlpha(true, false, 0x81) == 0xFF);
	static_assert(WritesAlpha(true, true, 0x81) == 0x7E);
	static_assert(WritesAlpha(false, false, 0) == 0);

	static_assert(Live(true, true, true, false));
	static_assert(!Live(true, true, false, false));

	// The first reader copies its own area, as it always did.
	static_assert(Decide(false, 0, false, {true, 0x81}) == Copy::DrawArea);
	// A second reader outside it copies the target once; later ones reuse.
	static_assert(Decide(true, 0x7E, false, {true, 0x81}) == Copy::Target);
	static_assert(Decide(true, 0x7E, true, {true, 0x81}) == Copy::Reuse);
	// A bit written since the copy starts over.
	static_assert(Decide(true, 0xFE, true, {true, 0x81}) == Copy::DrawArea);
	static_assert(Decide(true, 0xFE, true, {true, 0x01}) == Copy::Reuse);
	// Any other reader copies for itself, live copy or not.
	static_assert(Decide(true, 0, true, {}) == Copy::Private);
	static_assert(Decide(false, 0, false, {}) == Copy::Private);
} // namespace GSRunSnapshot
