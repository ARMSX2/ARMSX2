// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// ⚠️ MEASUREMENT OVERRIDE — how a draw says it reads the attachment it writes.
//
// Not a setting, and it does nothing unless a harness asks for it. Campaign
// gs-adreno-inpass-read E4b, lane C25.
//
// There are two spellings of the same declaration, and on Turnip they are charged differently.
//
//   PIPELINE CREATE FLAG (today). Every pipeline bound in the pass carries
//   VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT. Turnip reads that flag when the
//   pipeline is bound and refuses to tile the pass; it also programs the coherent primitive mode
//   from it, per pipeline. Since the feedback-loop carry keeps the flag word set across the
//   non-readers that follow a reader in a latched pass, every one of those pipelines carries the
//   flag too -- so the serialising mode applies to draws that never read anything.
//
//   DYNAMIC PER DRAW (this override). The pipeline carries
//   VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT and no create flag, and the declaration
//   is made per draw with vkCmdSetAttachmentFeedbackLoopEnableEXT: the colour aspect on the draws
//   that read the target, nothing on the draws that do not.
//
// Why that separates the two costs, from Turnip 26.1.2 source (not measured):
//
//   * the untiling stays. tu_cmd_buffer.cc ~8271-8281: when the dynamic feedback-loop state is
//     dirty and non-zero, Turnip sets cmd->state.rp.disable_gmem, reason
//     "MESA_VK_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE". It is never cleared inside a pass and
//     the gmem/sysmem choice is made at end of pass, so one reading draw untiles the whole pass
//     -- which is REQUIRED, because the coherent primitive mode inside a tiled pass is the
//     incoherent case.
//   * the primitive mode becomes per draw. tu_pipeline.cc ~4296-4305 emits the sysmem prim-mode
//     draw state from `dyn.feedback_loops | pipeline_feedback_loops`, so with the pipeline half
//     zero the mode follows the dynamic value draw by draw.
//   * and the pipeline half really is zero: vk_graphics_state.c ~1238-1249 derives
//     feedback_loop_not_input_only -- which is what tu_cmd_buffer.cc:5422 turns into
//     pipeline_disable_gmem -- from the pipeline CREATE flags only, and ~1226 filters those flags
//     out entirely when the pipeline declares the state dynamic. Turnip's own render-pass term
//     (tu_pass.cc ~492) is set only when an INPUT ATTACHMENT aliases a colour attachment, which
//     this road never builds, so the render pass does not put the flag back either.
//
// What the Vulkan spec says about doing it this way. The three draw-time feedback-loop rules
// (VUID-vkCmdDraw-None-09000 colour, 09002 depth, 09003 stencil) accept either "the create flag
// is set on the bound pipeline" or "the last vkCmdSetAttachmentFeedbackLoopEnableEXT included the
// aspect and the bound pipeline was created with VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT".
// The two spellings are alternatives, by the letter of the rule. All three are additionally
// conditioned on the attachment NOT being in VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
// -- which on this road it always is, for the whole pass -- so neither spelling is what makes the
// read legal here. The layout is.
//
// ⚠️ One consequence, from the Mesa runtime rather than the spec: vk_graphics_state.c ~2104-2112
// resets the dynamic feedback-loop value to zero as part of filling a pipeline's static state,
// and vk_dynamic_graphics_state_copy copies it on every bind. So on any Mesa driver the value
// must be re-set AFTER binding the pipeline and before the draw, every draw. It cannot be set
// once per pass.
//
// This override changes the spelling only. Which draws are declared, which passes are opened, and
// what the attachment layout is are all unchanged -- so the pass collapse the carry buys is
// preserved, and the population declared is the population declared before.
// ---------------------------------------------------------------------------------------------

/// How the backend states a feedback loop.
enum class GSLoopDeclarationSpelling : u8
{
	PipelineCreateFlag,
	DynamicPerDraw,
};

struct GSDynamicFeedbackLoopInputs
{
	GSLoopDeclarationSpelling spelling = GSLoopDeclarationSpelling::PipelineCreateFlag;

	/// The backend reaches the attachment through the feedback-loop image layout. Off that road
	/// there is no declaration to respell: the in-tile road states the loop with an input
	/// attachment and the copy road states nothing.
	bool layout_road_live = false;

	/// VK_EXT_attachment_feedback_loop_dynamic_state is present AND its feature bit is on.
	bool dynamic_state_available = false;
};

/// True when the backend declares the loop per draw instead of per pipeline.
constexpr bool GSDeclaresLoopPerDraw(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.layout_road_live &&
	       in.dynamic_state_available;
}

/// True when the per-draw spelling was asked for and cannot be given. Reported so the caller can
/// say so once: a silently inert arm is a device round that measures the other arm twice.
constexpr bool GSDynamicLoopRequestedButUnavailable(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && !GSDeclaresLoopPerDraw(in);
}

// The default spelling is the create flag, whatever else is true.
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSDynamicLoopRequestedButUnavailable({.layout_road_live = true, .dynamic_state_available = true}));

// Asked for, with the road and the extension, it applies.
static_assert(GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSDynamicLoopRequestedButUnavailable({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true, .dynamic_state_available = true}));

// Without the layout road there is no declaration to respell, and without the extension there is
// no way to respell it. Both are reported rather than silently ignored.
static_assert(!GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.dynamic_state_available = true}));
static_assert(GSDynamicLoopRequestedButUnavailable({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.dynamic_state_available = true}));
static_assert(!GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true}));
static_assert(GSDynamicLoopRequestedButUnavailable({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true}));

namespace GSDynamicFeedbackLoopPolicy
{
	/// The spelling this process uses.
	///
	/// A process-wide inline global rather than a setting, for the reason every policy override
	/// in this directory is one: which spelling a driver charges less for is a measurement result
	/// on one device. Set once before the VM starts, because the answer has to be final before
	/// the first pipeline exists -- a pipeline's dynamic-state list cannot be changed afterwards.
	inline GSLoopDeclarationSpelling s_spelling = GSLoopDeclarationSpelling::PipelineCreateFlag;

	inline void SetSpelling(GSLoopDeclarationSpelling value) { s_spelling = value; }
	inline GSLoopDeclarationSpelling GetSpelling() { return s_spelling; }
	inline bool WantsDynamicPerDraw() { return s_spelling == GSLoopDeclarationSpelling::DynamicPerDraw; }

	/// For the banner. A device round quotes this, so it says what was declared and how.
	inline const char* Name()
	{
		return (s_spelling == GSLoopDeclarationSpelling::DynamicPerDraw) ? "dynamic per draw" : "pipeline create flag";
	}
} // namespace GSDynamicFeedbackLoopPolicy
