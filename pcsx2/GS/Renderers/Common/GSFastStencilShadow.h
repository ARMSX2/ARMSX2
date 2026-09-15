// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"

// The alpha stencil counter, drawn by the blend unit instead of by reading the render target.
//
// Jak II and Jak 3 count shadow-volume faces in the frame's alpha channel. Each face is a flat
// triangle that textures from the frame it writes, samples the pixel under itself and stores
// (Ad * Av) >> 7, with a vertex alpha Av of 130 for one face direction and 127 for the other.
// Emulated literally every such draw reads the render target, and auto-flush cuts the volume into
// draws of one or two triangles so that each face sees the result of the one before it.
//
// Where that read is a render-pass break plus a copy of the target, Jak II at 2x pays for about
// 3,400 of them a frame. The blend unit can do the multiply instead. The shader writes a step s to
// its first output's alpha and a factor a1 to its second output, and the alpha blend
// Ad * s + Ad * a1 (source DST_ALPHA, destination SRC1_ALPHA) gives Ad * (1 + 3/255) for Av 130 and
// Ad * 252/255 for Av 127. Both factors are whole 8-bit values, so a fixed-point 8-bit blend unit
// carries them without loss. The up step matches the console for Ad 64..191 and the down step for
// Ad 43..212 except exactly 128; these games keep the counter near 96. The blend unit also applies
// overlapping triangles in order within one draw, so the whole volume can arrive as one draw that
// reads nothing.
//
// Whether a device takes this road is GSDevice::FeatureSupport::fast_stencil_shadow, decided once
// from DeviceQualifies below.
namespace GSFastStencilShadow
{
	// The device rule is three facts:
	//  - Vulkan. Only the Vulkan TFX shader has the counter's output block, and Vulkan is where
	//    texture barriers off means a frame read costs a render-pass break plus a copy.
	//  - Texture barriers off. Frame reads are then served by the per-draw copy, which is the cost
	//    this removes. With barriers on the read stays inside the pass and the renderer keeps its
	//    per-primitive path.
	//  - Dual-source blending, for the second factor.
	//
	// Today that is every Adreno part. Turnip and the Qualcomm driver both carry
	// UseRenderTargetCopyForFeedback, which turns texture barriers off. Mali parts on that workaround
	// report no dual-source blending, and desktop GPUs keep their barriers. OverrideTextureBarriers=1
	// turns barriers back on, and with them this off.
	//
	// ⚠️ `!texture_barrier` on its own is not this rule. D3D11 runs without texture barriers as well,
	// its copies are cheap, and its shader has no counter block, so taking the road there would draw
	// the counter wrong for no gain. See cheap_rt_feedback_read for the same mistake made in the
	// opposite direction.
	constexpr bool DeviceQualifies(RenderAPI api, bool texture_barrier, bool dual_source_blend)
	{
		return api == RenderAPI::Vulkan && !texture_barrier && dual_source_blend;
	}
} // namespace GSFastStencilShadow
