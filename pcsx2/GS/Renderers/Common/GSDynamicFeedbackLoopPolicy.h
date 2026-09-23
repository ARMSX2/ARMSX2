// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// How a draw declares that it reads the attachment it writes, and which of the two spellings is
// the default. Not a user setting: which spelling a driver charges less for is a measurement.
//
//   DYNAMIC PER DRAW (the default). The pipeline carries
//   VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT and no create flag, and the declaration
//   is made per draw with vkCmdSetAttachmentFeedbackLoopEnableEXT: the colour aspect on draws that
//   read the target, nothing on draws that do not.
//
//   PIPELINE CREATE FLAG (the fallback). Every pipeline bound in the pass carries
//   VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT. Turnip reads the flag at bind,
//   refuses to tile the pass, and programs the coherent primitive mode from it per pipeline. The
//   feedback-loop carry keeps the flag set across the non-readers that follow a reader in a
//   latched pass, so the serialising mode and its per-draw flush apply to draws that read
//   nothing. On Turnip this costs up to ~2.8x frame time on titles with many non-readers behind
//   one reader, which is why per draw is the default.
//
// Why the per-draw spelling is cheaper on Turnip (from Mesa source):
//
//   * the untiling stays: dirty non-zero dynamic feedback-loop state sets rp.disable_gmem, never
//     cleared inside a pass, so one reading draw untiles the whole pass. This is required: the
//     coherent primitive mode inside a tiled pass is the incoherent case.
//   * the primitive mode follows `dyn.feedback_loops | pipeline_feedback_loops`, so with the
//     pipeline half zero it changes draw by draw.
//   * the pipeline half is zero: vk_graphics_state.c derives feedback_loop_not_input_only from
//     the create flags only, and filters those out when the state is dynamic. Turnip's render-pass
//     term (tu_pass.cc) is only set when an input attachment aliases a colour attachment, which
//     this road never builds.
//
// Spec: VUID-vkCmdDraw-None-09000/09002/09003 accept either spelling. All three also exempt an
// attachment in VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, which it always is on this
// road, so the layout is what makes the read legal, not either spelling.
//
// ⚠️ Mesa resets the dynamic feedback-loop value to zero when filling a pipeline's static state
// and copies it on every bind, so on any Mesa driver it must be re-set after binding the pipeline
// and before each draw. It cannot be set once per pass.
//
// The spelling changes nothing visible: the declared draws, the passes opened and the attachment
// layout are unchanged, so the pass collapse the carry buys is preserved.
//
// Where each spelling comes from:
//
//   * per draw -- the default on the drivers it was run on (Turnip, Honeykrisp), when the
//     feedback-loop layout road is live and VK_EXT_attachment_feedback_loop_dynamic_state is
//     present with its feature bit on.
//   * create flag, by device -- every other driver (desktop NVIDIA, AMD, Intel). Per draw was
//     never run there, so they keep the create flag and the extension is not requested. Not
//     reported.
//   * create flag, by fallback -- a measured driver on the layout road without the extension.
//     Reported, because on Turnip it is the expensive spelling.
//   * create flag, by force -- `pcsx2-gsrunner -loop-create-flag`. Nothing else sets it.
//
// Off the layout road (the copy road, and the in-tile road that states the loop with an input
// attachment) nothing is declared, and neither spelling applies.
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

	/// The per-draw spelling was run on this driver (Turnip, Honeykrisp). Every other driver keeps
	/// the create flag, and that is not reported as a fallback.
	bool device_measured = false;
};

/// True when the backend declares the loop per draw instead of per pipeline.
constexpr bool GSDeclaresLoopPerDraw(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.device_measured && in.layout_road_live &&
	       in.dynamic_state_available;
}

/// True when there is a loop to declare and the per-draw spelling cannot be given, so the
/// declaration falls back to the create flag. Reported so the caller can log it once: on Turnip
/// the fallback is the expensive spelling. A warning, not an error, since a driver without the
/// extension may pay nothing. A forced create flag is not a fallback and is not reported.
constexpr bool GSLoopSpellingFallsBackToCreateFlag(const GSDynamicFeedbackLoopInputs& in)
{
	return in.spelling == GSLoopDeclarationSpelling::DynamicPerDraw && in.device_measured && in.layout_road_live &&
	       !in.dynamic_state_available;
}

// These pin the default. One failing means the shipped road changed spelling by accident.

// Per draw wherever there is a loop to declare and the extension to declare it with, on a
// measured driver, with no flag or setting.
static_assert(GSDeclaresLoopPerDraw({.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag(
	{.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// Desktop Vulkan keeps the create flag silently, with or without the extension.
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true, .dynamic_state_available = true}));
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true}));

// Asked for explicitly, it is the same thing. `spelling` carries no third state.
static_assert(GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::DynamicPerDraw,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// Forced create flag: deliberate, so not a fallback and not reported.
static_assert(!GSDeclaresLoopPerDraw({.spelling = GSLoopDeclarationSpelling::PipelineCreateFlag,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.spelling = GSLoopDeclarationSpelling::PipelineCreateFlag,
	.layout_road_live = true, .dynamic_state_available = true, .device_measured = true}));

// Layout road without the extension on a measured driver: the reported fallback. Honeykrisp
// (no dynamic-state extension) is this row.
static_assert(!GSDeclaresLoopPerDraw({.layout_road_live = true, .device_measured = true}));
static_assert(GSLoopSpellingFallsBackToCreateFlag({.layout_road_live = true, .device_measured = true}));

// Off the layout road nothing is declared, whatever the device advertises. Silent.
static_assert(!GSDeclaresLoopPerDraw({.dynamic_state_available = true, .device_measured = true}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({.dynamic_state_available = true, .device_measured = true}));
static_assert(!GSDeclaresLoopPerDraw({}));
static_assert(!GSLoopSpellingFallsBackToCreateFlag({}));
