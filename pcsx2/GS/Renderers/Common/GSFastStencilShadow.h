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

	/// ⚠️ MEASUREMENT OVERRIDE, not a device fact and not a setting. The harness asked for the
	/// counter to be off for the whole process, so DeviceQualifies is overruled wherever the
	/// backend consults it.
	///
	/// Why it exists: the declared-feedback-loop road turns texture barriers ON
	/// (GSSelfReadRoadPolicy's arm branch), and DeviceQualifies requires them OFF -- so declaring
	/// the loop disables this counter as a side effect, on every Adreno part, under either
	/// spelling of the declaration. A base-vs-declared A/B on Jak II therefore moves two things at
	/// once and cannot say which paid.
	///
	/// ⚠️ OverrideTextureBarriers=1 is NOT the arm for that job, though it looks like it. Turning
	/// barriers on without an arm does not leave the copy road: the road policy falls past its
	/// Copy return and selects InPassBarrier, spelling InputAttachment -- the in-pass self-read
	/// with no declared loop, which is the configuration the vk-turnip-attachment-self-read
	/// profile rule exists to forbid. It renders wrong, so its timings measure a different
	/// workload. This switch is the one that holds the road still.
	///
	/// Correct by construction when asked: the counter is an optimisation, and without it the
	/// renderer takes the ordinary render-target read it took before the counter existed. So
	/// frames under this flag must equal base's, and that equality is the proof the arm measured
	/// our workload rather than a broken road.
	///
	/// There is no force-ON twin. Qualifying is a device rule about what a frame read costs, and
	/// forcing the counter onto a device where reads are cheap would draw it wrong for no gain --
	/// see the D3D11 note on DeviceQualifies above.
	///
	/// Campaign gs-adreno-inpass-read, the fast-stencil-shadow decomposition.
	inline bool s_force_off = false;

	inline void SetForcedOff(bool value) { s_force_off = value; }
	inline bool IsForcedOff() { return s_force_off; }

	/// ⚠️ MEASUREMENT OVERRIDE, the twin of the above and the one E4d made necessary. Takes the
	/// counter on a device the rule declines, so long as the backend can actually draw it.
	///
	/// Why the rule declines it, and why that may be wrong: DeviceQualifies requires
	/// `!texture_barrier` because the JUSTIFICATION was written for the copy road -- with barriers
	/// off a frame read is a pass break plus a copy, which is the cost the blend removes. With
	/// barriers on "the read stays inside the pass", so the rule concludes the counter is not
	/// needed. **The blend does not stop working; it stops being obviously worth it.** Avoiding a
	/// read outright is still cheaper than a cheap read.
	///
	/// E4d measured what that assumption costs. Declaring the feedback loop turns barriers on and
	/// so drops the counter, and on jak2 at 2x the counter's absence is worth +341.9% on the copy
	/// road but only +39.3% on the declared road -- the declared road SUBSTITUTES for ~88% of the
	/// counter rather than stacking on top of losing it. The two are answers to one cost. That
	/// leaves the cell nobody has run: **counter ON with the loop declared.** If it lands at or
	/// below base, the coupling is accidental and the fix is to split texture_barrier's two jobs.
	///
	/// ⚠️ Still gated on what the backend can DRAW, not merely on wanting it. The Vulkan TFX
	/// shader is the only one carrying the counter's output block and the second factor needs
	/// dual-source blending, so this override keeps the API and dual-source terms and lifts only
	/// the barrier term. Forcing it on D3D11 would draw the counter wrong -- the mistake
	/// DeviceQualifies already warns about in the opposite direction.
	///
	/// Reaches the M2: that device keeps barriers on, so it has never been able to take the
	/// counter road at all, and this is the first switch under which an M2 gate can score it.
	inline bool s_force_on = false;

	inline void SetForcedOn(bool value) { s_force_on = value; }
	inline bool IsForcedOn() { return s_force_on; }

	/// What the backend should use, with both overrides resolved. Force-off wins over force-on:
	/// asking for both is a harness mistake, and the safe resolution is the one that changes least
	/// from the shipped picture.
	constexpr bool Resolve(bool forced_off, bool forced_on, RenderAPI api, bool texture_barrier,
		bool dual_source_blend)
	{
		if (forced_off)
			return false;
		if (forced_on)
			return api == RenderAPI::Vulkan && dual_source_blend;
		return DeviceQualifies(api, texture_barrier, dual_source_blend);
	}
} // namespace GSFastStencilShadow
