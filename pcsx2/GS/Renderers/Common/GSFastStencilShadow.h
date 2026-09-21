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
	// The facts the device rule is made of. Two of them are about what the backend can DRAW; the
	// road is about whether drawing it is worth anything.
	struct DeviceFacts
	{
		/// Only the Vulkan TFX shader carries the counter's output block.
		RenderAPI api = RenderAPI::None;

		/// The second factor needs a second fragment output.
		bool dual_source_blend = false;

		/// Which self-read road the device landed on -- GSSelfReadRoadDecision::road, so this
		/// answers "what does a frame read cost here" rather than "is a frame read legal here".
		GSSelfReadRoad road = GSSelfReadRoad::Copy;

		/// The road declares an attachment feedback loop rather than reaching the same spelling by
		/// the device's own preference -- GSSelfReadRoadDecision::loop_declared. True for both
		/// declaration arms and for the driver fact that selects the same road; see below.
		bool loop_declared = false;
	};

	// The device rule, in two halves.
	//
	// CAN THE BACKEND DRAW IT: Vulkan, because only its TFX shader has the counter's output block,
	// and dual-source blending, because that is where the second factor goes. Neither has moved.
	//
	// IS IT WORTH DRAWING: this half used to read `!texture_barrier`, and that was the bug.
	// `texture_barrier` answers two questions at once -- "may a draw read the render target from
	// inside the pass" and "does a frame read cost the renderer its cheap path" -- and only the
	// second one is the counter's business. Keying on the bit therefore coupled the counter to the
	// road: declaring the attachment feedback loop sets texture_barrier as a side effect
	// (GSSelfReadRoadPolicy's arm branch), which switched the counter off on a road that still
	// needs it. So the rule asks the road directly.
	//
	// The roads that qualify:
	//
	//  - Copy. Every frame read is a render-pass break plus a copy of the target, which is the
	//    cost the blend removes. Jak II at 2x pays for about 3,400 of them a frame. Today this is
	//    every Adreno part: Turnip and the Qualcomm driver both carry
	//    UseRenderTargetCopyForFeedback, which turns texture barriers off. This road is what the
	//    counter was written for and it is unchanged.
	//
	//  - A declared feedback loop. The read is in-pass, so it costs no copy, but auto-flush still
	//    cuts the volume into one- and two-triangle draws and the counter is still what stops it.
	//    Measured on an SD865 under Turnip, Jak II and Jak 3 at native and 2x (campaign
	//    gs-adreno-inpass-read, E4d and E4e): the declared road WITHOUT the counter runs +11.5% to
	//    +42.0% against the copy road, and WITH the counter forced on it runs +0.13% to +0.90%,
	//    which is the run-to-run noise measured in the same sitting. Draw counts with the counter
	//    come out equal to the copy road's to the draw -- 5,753 on Jak II and 2,892 on Jak 3 at
	//    native -- because the auto-flush split exemption comes back with it. Frames are
	//    bit-identical to the declared road without the counter, both titles, both scales, every
	//    frame. The declared road's entire measured cost on those titles was the counter's absence.
	//
	//    Both declaration arms qualify, not just the driver-ordered one. Arm 2 declares the loop
	//    and keeps the per-draw barriers, and it exists so that arm-1-vs-arm-2 isolates the
	//    ordering claim; if the counter switched off on one of them that comparison would move two
	//    things again, which is the mistake this rule is fixing.
	//
	//  - The backend's own per-draw barriers, with the read in-pass and nothing declared
	//    (GSSelfReadRoad::InPassBarrier). The read costs no copy, but auto-flush still cuts the
	//    volume into one- and two-triangle draws, and stopping that split is the rest of what the
	//    counter does. Measured on an Apple M2 Max under Honeykrisp, Classic Vulkan, frame-time
	//    p50 over 5 interleaved reps an arm (campaign upscale-unify, C31): Jak II 170.237 ms ->
	//    18.247 ms at 2x and 128.703 -> 16.246 at native; Jak 3 44.434 -> 7.726 at 2x; the Ratchet
	//    & Clank: Up Your Arsenal effects capture 30.491 -> 4.618 at 2x. That is -82.6% to -89.3%
	//    against a same-sitting base-vs-base noise floor of 0.04% to 0.83%. A second, lighter Jak
	//    II capture is -13.2%/-14.3%, because the counter's value tracks the counter draws' share
	//    of the frame rather than the title. The mechanism shows in the barrier count: Jak II at
	//    2x emits 126,285 per-draw feedback barriers a run without the counter and 386 with it.
	//
	//    ⚠️ Two limits, and they are the part of this bullet worth reading.
	//
	//    DESKTOP VULKAN IS ON THIS ROAD BY A POLICY WALK, NOT BY A MEASUREMENT. It keeps its
	//    texture barriers, and an in-tile read needs a Mali or Adreno part
	//    (DecideVulkanFramebufferFetch), so it has none available and falls to this road -- but no
	//    desktop GPU was timed. The exposure is scope rather than correctness: the blend
	//    arithmetic is fixed-point 8-bit on whole factors, which Vulkan guarantees rather than AGX,
	//    so what is unmeasured is how much it saves there, not whether it draws the same pixels.
	//
	//    NO HANDHELD IS ON THIS ROAD. Every Adreno is on Copy, and Mali parts report no
	//    dual-source blending and so fail the first half before the road is asked. This bullet
	//    buys frame time on the dev box and on desktop Vulkan and changes nothing on the handheld
	//    targets the campaign is measured against.
	//
	// The road that does not:
	//
	//  - The in-tile read (GSSelfReadRoad::InPassOrdered): Mali under rasterization-order
	//    attachment access, and an Adreno with OverrideTextureBarriers=1. Turning barriers back on
	//    does not reach the bullet above -- the part still advertises rasterization-order access,
	//    so the policy takes the in-tile read instead. Unmeasured on both, and the Mali parts
	//    cannot draw the counter anyway.
	//
	//    ⚠️ This is why the rule names InPassBarrier rather than asking `road != Copy`. The loose
	//    form reads as "any road that is not the copy road" and would take the counter onto both
	//    of these: a debug lever whose own output is unscored, and a road whose parts fail the
	//    dual-source term.
	//
	// ⚠️ Neither `!texture_barrier` nor the road on its own is this rule. D3D11 runs without
	// texture barriers as well, its copies are cheap, and its shader has no counter block, so
	// taking the road there would draw the counter wrong for no gain. See cheap_rt_feedback_read
	// for the same mistake made in the opposite direction.
	constexpr bool DeviceQualifies(const DeviceFacts& facts)
	{
		if (facts.api != RenderAPI::Vulkan || !facts.dual_source_blend)
			return false;

		return facts.road == GSSelfReadRoad::Copy
			|| facts.road == GSSelfReadRoad::InPassBarrier
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

	/// ⚠️ MEASUREMENT OVERRIDE, not a device fact and not a setting. The harness asked for the
	/// counter to be off for the whole process, so DeviceQualifies is overruled wherever the
	/// backend consults it.
	///
	/// Why it exists: it separates the counter's own contribution from the road's. Until the rule
	/// above started asking the road, the two could not be told apart at all -- the
	/// declared-feedback-loop road turns texture barriers ON (GSSelfReadRoadPolicy's arm branch)
	/// and the rule required them OFF, so declaring the loop switched this counter off as a side
	/// effect and a base-vs-declared A/B on Jak II moved two things at once. The declared road
	/// keeps the counter now; this switch is what takes it away deliberately.
	///
	/// ⚠️ OverrideTextureBarriers=1 is NOT the arm for that job, though it looks like it. Turning
	/// barriers on without an arm does not leave the copy road: the road policy falls past its
	/// Copy return, and since vk-turnip-attachment-self-read takes away the layout road but not
	/// the rasterization-order one, the in-tile read is still available and the road selected is
	/// InPassOrdered, spelling InputAttachment -- the in-pass self-read with no declared loop,
	/// which is the configuration that same profile rule exists to forbid. A device agrees with
	/// the walk: on an Adreno 650 the arm reports the input attachment in use and emits zero
	/// barriers, and zero is what separates this road from InPassBarrier
	/// (campaigns/sotc-sd865-flicker-2026-09-05). That configuration has been run and never
	/// scored against a reference, so its timings measure a workload of unknown shape -- which
	/// is reason enough not to use it here. This switch is the one that holds the road still.
	///
	/// Correct by construction when asked: the counter is an optimisation, and without it the
	/// renderer takes the ordinary render-target read it took before the counter existed. So
	/// frames under this flag must equal base's, and that equality is the proof the arm measured
	/// our workload rather than a broken road.
	///
	/// The force-ON twin is directly below. It answers the opposite question and the two resolve
	/// in Resolve, where this one wins.
	///
	/// Campaign gs-adreno-inpass-read, the fast-stencil-shadow decomposition.
	inline bool s_force_off = false;

	inline void SetForcedOff(bool value) { s_force_off = value; }
	inline bool IsForcedOff() { return s_force_off; }

	/// ⚠️ MEASUREMENT OVERRIDE, the twin of the above. Takes the counter on a road the rule
	/// declines, so long as the backend can actually draw it.
	///
	/// The job it was built for is done. E4d found the old `!texture_barrier` term costing real
	/// time -- declaring the feedback loop turned barriers on and dropped the counter, and on
	/// Jak II at 2x the counter's absence was worth +341.9% on the copy road but only +39.3% on
	/// the declared road, so the declared road was substituting for most of the counter rather
	/// than stacking on top of losing it. This switch reached the cell that settled it, counter ON
	/// with the loop declared, and E4e measured it at or inside noise of base with base's own draw
	/// counts and bit-identical frames. DeviceQualifies says that on its own now.
	///
	/// It then did the same job a second time, on the barrier-ordered road. C31 ran this switch
	/// against the default on an M2 Max and priced it -- -82.6% to -89.3% of frame-time p50 on Jak
	/// II, Jak 3 and the Ratchet effects capture, frames byte-identical on all 94 corpus cells --
	/// and DeviceQualifies now says so on its own, so on that device this switch changes nothing.
	///
	/// What is left for it is the one road the rule still declines: the in-tile read, which is
	/// Mali's default and where an Adreno with OverrideTextureBarriers=1 lands. The Mali parts
	/// cannot draw the counter at all, so the only reachable case is that Adreno lever -- and the
	/// lever's own output has been run and never scored, which is the caveat SetForcedOff above
	/// spells out. Measuring the counter there means scoring the road first.
	///
	/// ⚠️ Still gated on what the backend can DRAW, not merely on wanting it. The Vulkan TFX
	/// shader is the only one carrying the counter's output block and the second factor needs
	/// dual-source blending, so this override keeps the API and dual-source terms and lifts only
	/// the road term. Forcing it on D3D11 would draw the counter wrong -- the mistake
	/// DeviceQualifies already warns about in the opposite direction.
	inline bool s_force_on = false;

	inline void SetForcedOn(bool value) { s_force_on = value; }
	inline bool IsForcedOn() { return s_force_on; }

	/// What the backend should use, with both overrides resolved. Force-off wins over force-on:
	/// asking for both is a harness mistake, and the safe resolution is the one that changes least
	/// from the shipped picture.
	constexpr bool Resolve(bool forced_off, bool forced_on, const DeviceFacts& facts)
	{
		if (forced_off)
			return false;
		if (forced_on)
			return facts.api == RenderAPI::Vulkan && facts.dual_source_blend;
		return DeviceQualifies(facts);
	}
} // namespace GSFastStencilShadow
