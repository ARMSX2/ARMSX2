// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <cstdlib>

// Does this game move its projection half a display line between fields?
//
// In field mode a game draws a half-height field. Some titles draw the identical picture on both
// fields; others move the projection half a display line, so consecutive field renders are one
// native line apart and only line up after one of them is shifted. When the field render is
// presented directly (integer upscale of 2 or more, see GSInterlaceModePolicy.h) that shift is the
// only correction left: apply it for the second kind, and applying it to the first kind is what
// makes a still picture jitter a line every frame.
//
// The test is the offline experiment's: mean absolute difference between two consecutive fields at
// three vertical alignments, 0 and +-S device rows. The pictures compared are MERGES, so whatever
// offset the merge itself applied has to come back out -- that is what applied_delta_rows is for.

struct GSFieldShiftSample
{
	// Mean absolute difference, in 0..255 units, at the three alignments the game itself could
	// have drawn: no displacement, and one native line each way.
	float mad_aligned;
	float mad_up;
	float mad_down;
};

enum class GSFieldShiftVote
{
	// All three alignments agree -- a black screen, a static logo, a fade. Says nothing about the
	// game and must not be counted.
	Uninformative,
	NoShift,
	Shift,
};

// Below this the frame carries no usable structure at all.
inline constexpr float GS_FIELD_SHIFT_FLAT_MAD = 0.25f;
// "No shift" claims the two fields are the SAME PICTURE, so its aligned difference collapses
// towards zero while the alternatives do not. Demand a real gap before believing it.
inline constexpr float GS_FIELD_SHIFT_NOSHIFT_MARGIN = 1.5f;
// A shift game keeps real motion in every alignment, so its margin is small by nature.
inline constexpr float GS_FIELD_SHIFT_SHIFT_MARGIN = 1.02f;
// One pair this lopsided is the whole answer; there is nothing to gain from seven more.
inline constexpr float GS_FIELD_SHIFT_DECISIVE_MARGIN = 8.0f;

/// Only opposite fields say anything about a between-field shift. Two instances of the SAME field
/// one frame apart differ by the game's motion and nothing else, and on a still screen they are
/// identical -- which reads as "both fields draw the same picture" and is how a shift title gets
/// called a still one. A dump replay does not always alternate, so this is not theoretical.
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
	// Displacements reach 2*step, and the clamp-filled top band and the clipped bottom are each
	// step rows deep. Four steps of margin clears both with room over.
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

/// Fewer informative pairs than this and the vote is not worth taking.
inline constexpr int GS_FIELD_SHIFT_MIN_VOTES = 3;

/// SHIFT is the default: four of the six interlaced dumps shift, and it is the geometry Automatic
/// already produced before this path existed. NO-SHIFT has to be earned by three quarters of an
/// informative vote.
inline bool GSFieldShiftTallySaysNoShift(const GSFieldShiftTally& t)
{
	return t.noshift >= GS_FIELD_SHIFT_MIN_VOTES && t.noshift * 4 >= t.informative() * 3;
}
