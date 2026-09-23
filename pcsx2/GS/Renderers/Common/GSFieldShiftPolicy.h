// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <cstdlib>

// Does this game move its projection half a display line between fields?
//
// In field mode a game draws half-height fields. Some titles draw the same picture on both fields;
// others move the projection half a display line, so consecutive fields line up only after one is
// shifted. When the field render is presented directly (integer upscale >= 2, see
// GSInterlaceModePolicy.h) that shift is the only correction left. Apply it to the second kind;
// applying it to the first makes a still picture jitter a line every frame.
//
// The test: mean absolute difference between consecutive fields at three vertical alignments, 0 and
// +-S device rows. The compared pictures are merges, so the merge's own offset is removed via
// applied_delta_rows.

struct GSFieldShiftSample
{
	// Mean absolute difference (0..255) at no displacement and one native line each way.
	float mad_aligned;
	float mad_up;
	float mad_down;
};

enum class GSFieldShiftVote
{
	// All three alignments agree (black screen, static logo, fade). Not counted.
	Uninformative,
	NoShift,
	Shift,
};

// Below this the frame carries no usable structure at all.
inline constexpr float GS_FIELD_SHIFT_FLAT_MAD = 0.25f;
// "No shift" means the fields are the same picture, so the aligned difference should collapse
// towards zero. Demand a real gap.
inline constexpr float GS_FIELD_SHIFT_NOSHIFT_MARGIN = 1.5f;
// A shift game keeps real motion in every alignment, so its margin is small by nature.
inline constexpr float GS_FIELD_SHIFT_SHIFT_MARGIN = 1.02f;
// One pair this lopsided decides on its own.
inline constexpr float GS_FIELD_SHIFT_DECISIVE_MARGIN = 8.0f;

/// Only opposite fields say anything about a between-field shift. Two instances of the same field
/// are identical on a still screen, which would misread a shift title as no-shift. Dump replays do
/// not always alternate parity.
constexpr bool GSFieldShiftPairIsComparable(int prev_parity, int cur_parity)
{
	return prev_parity != cur_parity;
}

/// Mean absolute difference between two field signatures at a fixed vertical displacement.
/// Signatures are `rows` rows of `cols` bytes, tightly packed, one byte per sampled column.
/// `displacement` is added to the row index used in `prev`.
inline float GSFieldShiftMAD(
	const u8* cur, const u8* prev, int cols, int rows, int displacement, int margin)
{
	const int lo = margin;
	const int hi = rows - margin;
	if (cols <= 0 || hi <= lo)
		return 0.0f;

	u64 total = 0;
	for (int r = lo; r < hi; r++)
	{
		const u8* a = cur + static_cast<size_t>(r) * cols;
		const u8* b = prev + static_cast<size_t>(r + displacement) * cols;
		for (int c = 0; c < cols; c++)
			total += static_cast<u64>(std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));
	}

	return static_cast<float>(total) / static_cast<float>((hi - lo) * cols);
}

/// The three MADs at the game's own alignments. `step` is one native line in device rows (the
/// upscale). `applied_delta_rows` is the merge offset on the current field minus the one on the
/// previous field, in device rows -- pass 0 when neither field was offset.
///
/// merge_cur(row) = src_cur(row - a_cur) and src_cur(u) ~= src_prev(u + e) for the game's own
/// displacement e, so merge_cur(row) ~= merge_prev(row + e - (a_cur - a_prev)).
inline GSFieldShiftSample GSMeasureFieldShift(
	const u8* cur, const u8* prev, int cols, int rows, int step, int applied_delta_rows)
{
	// Displacements reach 2*step, and the clamp-filled top band and clipped bottom are each step
	// rows deep; four steps of margin clears both.
	const int margin = std::max(4 * step, 4);
	GSFieldShiftSample out = {};
	out.mad_aligned = GSFieldShiftMAD(cur, prev, cols, rows, -applied_delta_rows, margin);
	out.mad_up = GSFieldShiftMAD(cur, prev, cols, rows, step - applied_delta_rows, margin);
	out.mad_down = GSFieldShiftMAD(cur, prev, cols, rows, -step - applied_delta_rows, margin);
	return out;
}

/// How far ahead the winning alignment is. 1.0 means a tie; a large number means the winner is the
/// only alignment that fits. Returns 0 for a pair with no usable structure.
inline float GSFieldShiftMargin(const GSFieldShiftSample& s)
{
	const float shifted = std::min(s.mad_up, s.mad_down);
	const float best = std::min(s.mad_aligned, shifted);
	const float second = std::max(s.mad_aligned, shifted);
	if (std::max({s.mad_aligned, s.mad_up, s.mad_down}) < GS_FIELD_SHIFT_FLAT_MAD)
		return 0.0f;
	if (best <= 0.0f)
		return GS_FIELD_SHIFT_DECISIVE_MARGIN * 2.0f;
	return second / best;
}

inline GSFieldShiftVote GSClassifyFieldShiftPair(const GSFieldShiftSample& s)
{
	if (std::max({s.mad_aligned, s.mad_up, s.mad_down}) < GS_FIELD_SHIFT_FLAT_MAD)
		return GSFieldShiftVote::Uninformative;

	const float shifted = std::min(s.mad_up, s.mad_down);
	if (s.mad_aligned * GS_FIELD_SHIFT_NOSHIFT_MARGIN <= shifted)
		return GSFieldShiftVote::NoShift;
	if (shifted * GS_FIELD_SHIFT_SHIFT_MARGIN <= s.mad_aligned)
		return GSFieldShiftVote::Shift;

	return GSFieldShiftVote::Uninformative;
}

struct GSFieldShiftTally
{
	int shift = 0;
	int noshift = 0;

	int informative() const { return shift + noshift; }
};

/// Fields a decision stands before measuring again (about ten seconds). A decision taken on a boot
/// logo or still menu says nothing about later scenes; a probe round costs a dozen small readbacks.
inline constexpr int GS_FIELD_SHIFT_RECHECK_FIELDS = 600;

/// Fewer informative pairs than this and the vote is not worth taking.
inline constexpr int GS_FIELD_SHIFT_MIN_VOTES = 3;

/// SHIFT is the default (most interlaced titles tested shift, and it matches Automatic's
/// geometry). NO-SHIFT needs three quarters of the informative votes.
inline bool GSFieldShiftTallySaysNoShift(const GSFieldShiftTally& t)
{
	return t.noshift >= GS_FIELD_SHIFT_MIN_VOTES && t.noshift * 4 >= t.informative() * 3;
}

// ---------------------------------------------------------------------------------------------
// The top band
//
// The field-direct merge shifts what it READS, not where it draws, so no destination row is left
// undrawn. The top rows then sample above the circuit's rect, and the sampler clamps to the
// TEXTURE's first row.
//
// That is the rect's first row only when the rect starts at the texture top (DISPFB.DBY plus any
// page offset to the target base is 0, the usual case). Otherwise the clamp returns rows the circuit
// does not own, so the merge draws the band itself, every row sampling the centre of the rect's
// first texel row.
//
// The band is exactly `shift_rows` destination rows deep (row centre r + 0.5 < dst_top +
// shift_rows), so it abuts the shifted main draw with no gap or overlap.

struct GSFieldShiftTopBand
{
	// Normalised v for every row of the band: the centre of the rect's first texel row.
	float src_v = 0.0f;
	// Destination rows [dst_top, dst_bottom), in device pixels.
	float dst_top = 0.0f;
	float dst_bottom = 0.0f;
	bool enabled = false;
};

/// rect_top_rows is where the circuit's rect starts in the texture, in native lines (DISPFB.DBY
/// plus the target page offset). Zero means the sampler's clamp suffices and no band is needed.
inline GSFieldShiftTopBand GSComputeFieldShiftTopBand(
	int rect_top_rows, float src_top_v, float dst_top, float shift_rows, int texture_height)
{
	GSFieldShiftTopBand band;
	if (rect_top_rows <= 0 || shift_rows <= 0.0f || texture_height <= 0)
		return band;

	band.src_v = src_top_v + 0.5f / static_cast<float>(texture_height);
	band.dst_top = dst_top;
	band.dst_bottom = dst_top + shift_rows;
	band.enabled = true;
	return band;
}

/// Where the shifted main draw starts once a band is drawn: below the band, with its source top
/// moved down by the shift. Without the cut, circuit 1 (blended) would be applied twice on the
/// band's rows whenever merge alpha is below one.
struct GSFieldShiftMainTop
{
	float dst_top = 0.0f;
	float src_top_v = 0.0f;
};

inline GSFieldShiftMainTop GSFieldShiftMainDrawBelowBand(
	const GSFieldShiftTopBand& band, float dst_top, float shifted_src_top_v, float src_shift_v)
{
	if (!band.enabled)
		return {dst_top, shifted_src_top_v};

	return {band.dst_bottom, shifted_src_top_v + src_shift_v};
}
