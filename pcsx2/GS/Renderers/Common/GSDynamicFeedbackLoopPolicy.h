// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// How a draw says it reads the attachment it writes -- and which of the two ways of saying it we
// use by default. Built and priced while measuring the Adreno in-pass read. Not a user setting: which spelling a driver charges less for is a measurement result.
//
// There are two spellings of the same declaration, and on Turnip they are charged differently.
//
//   DYNAMIC PER DRAW (the default). The pipeline carries
//   VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT and no create flag, and the declaration
//   is made per draw with vkCmdSetAttachmentFeedbackLoopEnableEXT: the colour aspect on the draws
//   that read the target, nothing on the draws that do not.
//
//   PIPELINE CREATE FLAG (the fallback). Every pipeline bound in the pass carries
//   VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT. Turnip reads that flag when the
//   pipeline is bound and refuses to tile the pass; it also programs the coherent primitive mode
//   from it, per pipeline. Since the feedback-loop carry keeps the flag word set across the
//   non-readers that follow a reader in a latched pass, every one of those pipelines carries the
//   flag too -- so the serialising mode applies to draws that never read anything, and on our
//   patched Turnip its per-draw flush (which waits for idle) fires on every draw in the pass.
//
// ⚠️ What the create-flag spelling costs, measured. SD865, Turnip axfl1-005, gsrunner
// `-perf -loop 10`, p50 frame time in ms, same binary, same driver, same dump:
//
//   cell        create flag   per draw
//   wrc3@1x         51.8        18.5      <- 2.8x, one title over its whole budget
//   indy@1x          9.86        9.48     <- 4%, and it is the title the road is bought for
//
// The cost lands wherever a latched pass carries many non-readers behind one reader, so it is a
// per-title cliff rather than a uniform tax: one 20-dump measurement ran on the create flag by
// accident -- the runner flag that selected per-draw was an opt-in, and the user's path has no
// flags -- and WRC3 read +170.9% at p95 against the starting baseline while eighteen of twenty
// titles got faster. Every other declared-road number was taken through a harness that passed
// that flag, so the spelling was part of the road being priced and was never part of the road
// being shipped. Making the measured spelling the default one closes that gap.
//
// Why the two costs separate, from Turnip 26.1.2 source (not measured):
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
// The spelling changes nothing a pixel can see. Which draws are declared, which passes are
// opened, and what the attachment layout is are all unchanged -- so the pass collapse the carry
// buys is preserved, and the population declared is the population declared before. Measured
// identity-neutral three times: twice on the M2, and once on the SD865, where the
// binary with the per-draw flag matched the binary with no override at all on 20 of 20 cells.
//
// Where each spelling comes from:
//
//   * per draw -- the default on the drivers it was run on (Turnip, and Honeykrisp for the
//     identity runs), taken whenever the feedback-loop LAYOUT road is live and
//     VK_EXT_attachment_feedback_loop_dynamic_state is present with its feature bit on.
//   * create flag, by device -- every other driver, which is desktop NVIDIA, AMD and Intel
//     Vulkan. They take the layout road by default and have always declared the loop with the
//     create flag; nobody has run the per-draw spelling on them, so they keep it, and the
//     extension is not even requested there. Not reported: it is the spelling they always had.
//   * create flag, by fallback -- a measured driver on the layout road without that extension. The
//     declaration still has to be made, and the create flag is the only spelling left. Said out
//     loud, because on Turnip it is the expensive one and a silent fallback is a device that
//     quietly runs the 2.8x arm.
//   * create flag, by force -- `pcsx2-gsrunner -loop-create-flag`, to measure the fallback on
//     purpose. Nothing else sets it.
//
// Off the layout road (the copy road, and the in-tile road that states the loop with an input
// attachment) there is no declaration to spell at all, and neither spelling applies. That is the
// ordinary case on most devices and it is not reported.
// ---------------------------------------------------------------------------------------------

/// How the backend states a feedback loop.
enum class GSLoopDeclarationSpelling : u8
{
	PipelineCreateFlag,
	DynamicPerDraw,
};

/// The spelling taken unless something forces the other one. One place, so the harness override,
/// the input struct and the asserts below cannot drift apart.
inline constexpr GSLoopDeclarationSpelling kDefaultLoopDeclarationSpelling =
	GSLoopDeclarationSpelling::DynamicPerDraw;

struct GSDynamicFeedbackLoopInputs
{
	GSLoopDeclarationSpelling spelling = kDefaultLoopDeclarationSpelling;

	/// The backend reaches the attachment through the feedback-loop image layout. Off that road
	/// there is no declaration to respell: the in-tile road states the loop with an input
	/// attachment and the copy road states nothing.
	bool layout_road_live = false;

	/// VK_EXT_attachment_feedback_loop_dynamic_state is present AND its feature bit is on.
	bool dynamic_state_available = false;

	/// The driver the per-draw spelling was run on: Turnip (priced on the SD865) and Honeykrisp
	/// (identity runs on the M2). Every other driver keeps the create flag it always had, and the
	/// fallback is not reported for it, because it is not a fallback there.
	bool device_measured = false;
};

/// True when the backend declares the loop per draw instead of per pipeline.
constexpr bool GSDeclaresLoopPerDraw(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.device_measured && in.layout_road_live &&
	       in.dynamic_state_available;
}

/// True when there is a loop to declare and the per-draw spelling cannot be given, so the
/// declaration falls back to the create flag. Reported so the caller can say so once: on Turnip
/// the fallback is the 2.8x arm, and a device that takes it silently is a device nobody knows is
/// on it. It is a warning and not an error, because that price is a Turnip measurement and a
/// driver without the extension may well pay nothing -- the M2's does not. A FORCED create flag
/// is not a fallback and does not report -- somebody asked for it.
constexpr bool GSLoopSpellingFallsBackToCreateFlag(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.device_measured && in.layout_road_live &&
	       !in.dynamic_state_available;
}

// ⚠️ These pin the DEFAULT, and it changed on 2026-09-22. It used to be the create flag,
// because the spelling was an instrument nothing but a harness reached; it is now per draw,
// because the create flag is what the user's flagless path was taking and it reads 51.8 ms
// against 18.5 on wrc3@1x (SD865, axfl1-005, p50). Any one of these failing means the shipped
// road changed spelling by accident, which is a silent 2.8x on one title in twenty.

// The default spelling is per draw wherever there is a loop to declare and the extension to
// declare it with, on a driver it was run on -- no flag, no key, no setting.
static_assert(GSDeclaresLoopPerDraw({.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag(
	{.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// Desktop Vulkan: the layout road is its default road, and it keeps the create flag it always had,
// silently, whether or not its driver has the extension.
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true}));

// Asked for explicitly, it is the same thing. `spelling` carries no third state.
static_assert(GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// Forced back to the create flag on a device that could have done either: deliberate, so not a
// fallback and not reported.
static_assert(!GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::PipelineCreateFlag,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.spelling = GSLoopDeclarationSpelling::PipelineCreateFlag,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// On the layout road without the extension the loop still has to be declared, so the create flag
// is what is left -- and on a measured driver that IS the fallback, the one case worth a line in
// the log. The M2 is this row: Honeykrisp has no dynamic-state extension.
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true, .device_measured = true}));
static_assert(GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true, .device_measured = true}));

// Off the layout road nothing is declared either way, whatever the device advertises. This is the
// ordinary case -- every copy-road device is here -- so it is silent.
static_assert(!GSDeclaresLoopPerDraw({.dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.dynamic_state_available = true, .device_measured = true}));
static_assert(!GSDeclaresLoopPerDraw({}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({}));
