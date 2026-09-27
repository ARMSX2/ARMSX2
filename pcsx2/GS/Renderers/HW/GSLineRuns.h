// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSVertex.h"

#include <cstddef>
#include <vector>

/// A line draw as rectangles over exactly the pixels the GS lights (GSLineWalk.h): one rectangle
/// per run of pixels that share a minor coordinate, a colour and a fog value.
///
/// Colour and fog are flat within a run, taken from the exact gradient at the pixel and floored,
/// as the software scanline stores them. Depth and texture coordinates are the line's values at
/// each rectangle corner's own position. With aa1 set, the walk is the antialiased one: two pixels
/// per step, each carrying the GS's coverage as its alpha, so runs only merge where the coverage
/// holds.
///
/// The rectangles are four vertices each, corners in the order (major lo, minor lo),
/// (major hi, minor lo), (major lo, minor hi), (major hi, minor hi), with absolute 16-bit
/// positions. Pixels whose corners would not fit in 16 bits are dropped.
namespace GSLineRuns
{
	/// One rectangle is four vertices indexed 16-bit, so this many of them is the ceiling.
	inline constexpr u32 MAX_RECTS = 0x10000 / 4;

	struct Input
	{
		const GSVertex* vertices;
		const u16* indices; ///< two per line
		u32 line_count;
		int ofx; ///< XYOFFSET, 1/16 pixel
		int ofy;
		bool flat; ///< every pixel takes the second vertex's colour (IIP off)
		bool fog; ///< FGE
		bool abe; ///< ABE, which decides where AA1 coverage replaces the alpha
		bool aa1;
	};

	enum class Outcome
	{
		Converted, ///< dst holds `rects` rectangles.
		NothingLit, ///< No line in the draw lights a pixel.
		GroupEmpty, ///< A group of the draw list would have no rectangle.
		TooMany, ///< Past MAX_RECTS; `rects` is how many the draw needs.
	};

	struct Result
	{
		Outcome outcome;
		u32 rects;
	};

	/// The number of rectangles dst must hold for Convert(). Cheap: no line is walked.
	u32 Capacity(const Input& in);

	/// Writes the draw's rectangles to dst. `groups`, when not null, holds the line count of each
	/// group of the full-barrier draw list; on Converted it is rewritten to each group's rectangle
	/// count, and otherwise it is left alone. dst is scratch on any outcome but Converted.
	Result Convert(const Input& in, GSVertex* dst, std::vector<size_t>* groups);
} // namespace GSLineRuns
