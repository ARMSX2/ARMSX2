// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

// The alpha stencil counter, drawn by the blend unit instead of by reading the render target.
//
// Jak II and Jak 3 count shadow-volume faces in the frame's alpha channel. Each face is a flat
// triangle that textures from the frame it writes, samples the pixel under itself and stores
// (Ad * Av) >> 7, with Av 130 for one face direction and 127 for the other. Emulated literally,
// every such draw reads the render target, and auto-flush cuts the volume into draws of one or two
// triangles so each face sees the result of the one before it.
//
// The blend unit can do the multiply instead. The shader writes a step s to its first output's
// alpha and a factor a1 to its second output, and the alpha blend Ad * s + Ad * a1 (source
// DST_ALPHA, destination SRC1_ALPHA) gives Ad * (1 + 3/255) for Av 130 and Ad * 252/255 for Av 127.
// Both factors are whole 8-bit values, so an 8-bit fixed-point blend unit carries them without
// loss. The up step matches the console for Ad 64..191 and the down step for Ad 43..212 except
// exactly 128, where it stores 126 and the console 127. The Jak games keep the counter near 96;
// Ratchet & Clank: Up Your Arsenal's effect counter, which also takes this road, sits at 128 and
// can drift a few levels low. The blend unit applies overlapping triangles in order within one
// draw, so the whole volume can arrive as one draw that reads nothing.
//
// Whether a device takes this road is GSDevice::FeatureSupport::fast_stencil_shadow, decided once
// from DeviceQualifies. Whether a draw takes it is IsCounterShape, from the registers (visible to
// the auto-flush predicate), and VerticesQualify, from the vertex trace (renderer only).
namespace GSFastStencilShadow
{
	// The facts the device rule is made of.
	struct DeviceFacts
	{
		/// Only the Vulkan TFX shader carries the counter's output block.
		RenderAPI api = RenderAPI::None;

		/// The second factor needs a second fragment output.
		bool dual_source_blend = false;

		/// The device's self-read road (GSSelfReadRoadDecision::road): what a frame read costs
		/// here, not whether one is legal.
		GSSelfReadRoad road = GSSelfReadRoad::Copy;

		/// The road declares an attachment feedback loop (GSSelfReadRoadDecision::loop_declared),
		/// whether by setting or by driver fact.
		bool loop_declared = false;

		/// The barrier-ordered road was measured on this device (Apple silicon under Honeykrisp).
		bool barrier_road_measured = false;
	};

	// The device rule, in two halves.
	//
	// CAN THE BACKEND DRAW IT: Vulkan, because only its TFX shader has the counter's output block,
	// and dual-source blending, because that is where the second factor goes.
	//
	// IS IT WORTH DRAWING: asked of the road, not of `texture_barrier`. That bit answers both "may a
	// draw read the target in-pass" and "does a frame read cost the cheap path", and declaring the
	// feedback loop sets it as a side effect, which would switch the counter off on a road that
	// still needs it.
	//
	// The roads that qualify:
	//
	//  - Copy. Every frame read is a render-pass break plus a copy of the target, the cost the
	//    blend removes. This is every Adreno part the declared road does not reach (Turnip and the
	//    Qualcomm driver both carry UseRenderTargetCopyForFeedback).
	//
	//  - A declared feedback loop, from either declaration arm or from the driver fact that selects
	//    the same road. The read costs no copy, but auto-flush still cuts the volume into one- and
	//    two-triangle draws, and the counter is what stops that; frames are identical with and
	//    without it. Both arms qualify so that comparing them isolates the ordering claim alone.
	//
	//  - InPassBarrier: the backend's own per-draw barriers, read in-pass, nothing declared. Same
	//    reason: the counter stops the auto-flush split and the per-draw barriers with it.
	//    Scoped to barrier_road_measured (the M2). Desktop Vulkan and MoltenVK also land here but
	//    were not timed, so they keep no counter while barriers are on. The Adreno 7xx declared
	//    road also lands on InPassBarrier and qualifies through loop_declared.
	//
	// The road that does not:
	//
	//  - The in-tile read (InPassOrdered): Mali under rasterization-order attachment access, and
	//    an Adreno with OverrideTextureBarriers=1 (the part still advertises rasterization-order
	//    access, so it takes the in-tile read, not InPassBarrier). Unmeasured, and the Mali parts
	//    fail the dual-source term anyway. This is why the rule names InPassBarrier rather than
	//    testing `road != Copy`.
	//
	// ⚠️ Neither `!texture_barrier` nor the road alone is this rule. D3D11 runs without texture
	// barriers too, its copies are cheap, and its shader has no counter block, so taking the road
	// there would draw the counter wrong for no gain. See cheap_rt_feedback_read for the same
	// mistake in the opposite direction.
	constexpr bool DeviceQualifies(const DeviceFacts& facts)
	{
		if (facts.api != RenderAPI::Vulkan || !facts.dual_source_blend)
			return false;

		return facts.road == GSSelfReadRoad::Copy
			|| (facts.road == GSSelfReadRoad::InPassBarrier && facts.barrier_road_measured)
			|| facts.loop_declared;
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
