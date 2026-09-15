// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/GSVector.h"

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
// Ad 43..212 except exactly 128, where it stores 126 and the console 127. The Jak games keep the
// counter near 96; Ratchet & Clank: Up Your Arsenal's effect counter, which also takes this road, sits
// at 128 and can drift a few levels low. The blend unit also applies
// overlapping triangles in order within one draw, so the whole volume can arrive as one draw that
// reads nothing.
//
// Whether a device takes this road is GSDevice::FeatureSupport::fast_stencil_shadow, decided once
// from DeviceQualifies below. Whether a draw takes it is IsCounterShape, from the registers, which
// the auto-flush predicate can see, and VerticesQualify, from the vertex trace, which only the
// renderer has.
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

	// The counter's registers: flat-shaded triangles, textured with nearest sampling from the 32-bit
	// frame they write, modulating with texture alpha, writing alpha alone, with an alpha test that
	// cannot fail, no destination alpha test, no depth write, no FBA, no fog and no AA1. The
	// primitive class and the vertices are the caller's to check.
	inline bool IsCounterShape(const GIFRegPRIM& prim, const GIFRegTEX0& tex0, const GIFRegTEX1& tex1,
		const GIFRegTEST& test, const GIFRegFRAME& frame, const GIFRegZBUF& zbuf, const GIFRegFBA& fba)
	{
		return prim.TME && !prim.IIP && !prim.FGE && !prim.AA1 &&
		       frame.PSM == PSMCT32 && frame.FBMSK == 0x00FFFFFF &&
		       tex0.TBP0 == frame.Block() && tex0.PSM == PSMCT32 && tex0.TFX == TFX_MODULATE && tex0.TCC &&
		       tex1.MMAG == 0 && tex1.MMIN == 0 &&
		       !test.DATE && (!test.ATE || test.ATST == ATST_ALWAYS) &&
		       zbuf.ZMSK && !fba.FBA;
	}

	// What the registers cannot say, from the draw's vertex bounds. Every vertex alpha lies in
	// 127..130: the shader makes 127 the down step, 130 the up step, and 128 or 129 no change, which
	// is exact for 128 and, below Ad 128, for 129. The games only send 127 and 130. And every vertex
	// samples the pixel under it, by the bounding-box test CanUseTexIsFB applies to the same pattern.
	inline bool VerticesQualify(int alpha_min, int alpha_max, const GSVector4& pos_min, const GSVector4& pos_max,
		const GSVector4& tex_min, const GSVector4& tex_max)
	{
		if (alpha_min < 127 || alpha_max > 130)
			return false;

		const GSVector4 diff(pos_min.upld(pos_max) - tex_min.upld(tex_max));
		return (diff.abs() < GSVector4(1.0f)).alltrue();
	}
} // namespace GSFastStencilShadow
