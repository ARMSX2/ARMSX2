// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "common/Pcsx2Defs.h"

/// When a depth test rejects nothing.
///
/// GSRendererHW::IsDepthAlwaysPassing answers this from the draw alone: no test at all, or GEQUAL
/// at the largest Z the format can hold. A GREATER test is refused there whatever the geometry,
/// because the answer depends on what the depth buffer holds and the draw does not know.
///
/// The exception is a depth buffer that still holds its clear value in every pixel; then the
/// comparison is arithmetic. Xenosaga opens every frame with a full-screen sprite testing GREATER
/// at maximum Z against a depth buffer cleared in that same draw.
namespace GSDepthCoverage
{
	/// Whether every pixel of a draw passes the depth test, against a buffer that holds buffer_z
	/// in every pixel.
	///
	/// draw_z must already be clamped to the format's maximum. draw_z_is_flat must mean every
	/// vertex carries that same Z -- a min/max range is not enough, because the vertex trace's Z
	/// bounds are not accurate to the bit (see GSVertexTrace::CorrectDepthTrace).
	///
	/// A draw that writes depth can defeat itself: one primitive's write is the next primitive's
	/// buffer value. So a draw whose primitives might overlap is refused unless it leaves depth
	/// alone.
	inline constexpr bool AllPixelsPassConstantDepth(u32 ztst, bool draw_z_is_flat, u32 draw_z,
		u32 buffer_z, bool writes_depth, bool primitives_may_overlap)
	{
		if (!draw_z_is_flat)
			return false;
		if (writes_depth && primitives_may_overlap)
			return false;

		switch (ztst)
		{
			case ZTST_GEQUAL:
				return draw_z >= buffer_z;
			case ZTST_GREATER:
				return draw_z > buffer_z;
			default:
				// NEVER and ALWAYS are handled by the caller.
				return false;
		}
	}
} // namespace GSDepthCoverage
