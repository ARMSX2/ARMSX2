// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <array>
#include <cstdint>

namespace GSTargetRebase
{
using Rect = std::array<int, 4>;
constexpr uint32_t C32 = 0;
constexpr uint32_t Z32 = 48;
constexpr uint32_t BlocksPerPage = 32;
constexpr uint32_t MaxBlocks = 16384;
constexpr int PageWidth = 64;
constexpr int PageHeight = 32;

struct Layout
{
	uint32_t bp, bw, psm;
};

struct Safety
{
	bool clean = false;
	bool non_shuffle = false;
	bool zero_width_page_offset = false;
	bool no_current_source_reference = false;
	bool no_bound_depth_reference = false;
	bool existing_color_target = false;
	bool native_scale = false;
	bool native_extent_matches_valid = false;
	bool valid_rgb = false;
};

enum class Reason
{
	Preserve,
	UnsupportedLayout,
	WidthMismatch,
	UnsafeState,
	InvalidValidity,
	InvalidPageRect,
	RequiresColorC32Native,
	RequiresValidRGB,
	RequiresFullWidthSuffix,
	RequiresExactAllocation,
	Wrapped,
	PhysicalOverlap,
};

struct Plan
{
	Reason reason = Reason::UnsupportedLayout;
	Rect preserved{};
	Rect rebased{};
	uint32_t preserved_start = 0;
	uint32_t preserved_end = 0;
	uint32_t incoming_start = 0;
	uint32_t incoming_end = 0;
};

inline bool Bounded(const Rect& rect, uint32_t bw)
{
	return bw > 0 && bw <= 32 && rect[0] >= 0 && rect[1] >= 0 &&
		rect[2] > rect[0] && rect[3] > rect[1] &&
		rect[2] <= static_cast<int>(bw * PageWidth) && rect[3] <= 4096;
}

inline bool Contains(const Rect& outer, const Rect& inner, uint32_t bw)
{
	return Bounded(outer, bw) && Bounded(inner, bw) &&
		outer[0] <= inner[0] && outer[1] <= inner[1] &&
		outer[2] >= inner[2] && outer[3] >= inner[3];
}

inline bool FullPageRect(const Rect& rect, uint32_t bw)
{
	return Bounded(rect, bw) && !(rect[0] % PageWidth) && !(rect[2] % PageWidth) &&
		!(rect[1] % PageHeight) && !(rect[3] % PageHeight);
}

struct Envelope
{
	bool valid = false;
	uint32_t start = 0;
	uint32_t end = 0; // Exclusive GS block address.
};

inline Envelope PageEnvelope(const Layout& layout, const Rect& rect)
{
	if (layout.bp >= MaxBlocks || (layout.bp % BlocksPerPage) || !FullPageRect(rect, layout.bw))
		return {};

	const uint64_t first_page = static_cast<uint64_t>(rect[1] / PageHeight) * layout.bw + rect[0] / PageWidth;
	const uint64_t end_page = static_cast<uint64_t>(rect[3] / PageHeight - 1) * layout.bw + rect[2] / PageWidth;
	const uint64_t start = layout.bp + first_page * BlocksPerPage;
	const uint64_t end = layout.bp + end_page * BlocksPerPage;
	if (start >= end || end > MaxBlocks)
		return {};
	return {true, static_cast<uint32_t>(start), static_cast<uint32_t>(end)};
}

inline Reason CheckSuffix(const Layout& existing, const Rect& valid, const Rect& drawn,
	const Safety& safety)
{
	if (!Contains(valid, drawn, existing.bw))
		return Reason::InvalidValidity;
	if (!safety.valid_rgb)
		return Reason::RequiresValidRGB;
	const int full_width = static_cast<int>(existing.bw * PageWidth);
	if (valid[0] != 0 || valid[1] != 0 || valid[2] != full_width ||
		drawn[0] != 0 || drawn[1] <= 0 || drawn[2] != full_width || drawn[3] != valid[3])
		return Reason::RequiresFullWidthSuffix;
	if (!safety.native_extent_matches_valid)
		return Reason::RequiresExactAllocation;
	return Reason::Preserve;
}

// Full pages make C32 and Z32 envelopes comparable despite their block swizzles.
// Rebase only a clean native color suffix: it has no invalid prefix after the copy.
inline Plan MakeRebasePlan(const Layout& existing, const Rect& valid, const Rect& drawn,
	const Layout& incoming, const Rect& incoming_draw, const Safety& safety)
{
	if (existing.psm != C32 || incoming.psm != Z32)
		return {Reason::UnsupportedLayout};
	if (existing.bw != incoming.bw)
		return {Reason::WidthMismatch};
	if (!safety.clean || !safety.non_shuffle || !safety.zero_width_page_offset ||
		!safety.no_current_source_reference || !safety.no_bound_depth_reference)
		return {Reason::UnsafeState};
	if (!safety.existing_color_target || !safety.native_scale)
		return {Reason::RequiresColorC32Native};
	const Reason suffix = CheckSuffix(existing, valid, drawn, safety);
	if (suffix != Reason::Preserve)
		return {suffix};
	if (!FullPageRect(drawn, existing.bw) || !FullPageRect(incoming_draw, incoming.bw))
		return {Reason::InvalidPageRect};

	const Envelope retained = PageEnvelope(existing, drawn);
	const Envelope overwritten = PageEnvelope(incoming, incoming_draw);
	if (!retained.valid || !overwritten.valid)
		return {Reason::Wrapped};
	if (!(retained.end <= overwritten.start || overwritten.end <= retained.start))
		return {Reason::PhysicalOverlap};

	return {Reason::Preserve, drawn, {0, 0, drawn[2], drawn[3] - drawn[1]},
		retained.start, retained.end, overwritten.start, overwritten.end};
}
} // namespace GSTargetRebase
