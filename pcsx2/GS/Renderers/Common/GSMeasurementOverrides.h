// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h"
#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

/// Switches that move the Vulkan self-read road for an A/B, and nothing else. Only
/// pcsx2-gsrunner sets them, from its command line, before the VM starts; the Vulkan backend
/// reads them once, while it resolves its features and before any pipeline, image or render pass
/// exists. None is read per draw.
///
/// They are not settings on purpose: which road a driver wants is a property of the driver, and
/// a user who forced the declared loop on a desktop GPU would drop the barriers and break
/// blending. Every default is the shipped behaviour, so an untouched instance changes nothing.
struct GSMeasurementOverrides
{
	/// -declare-feedback-loop <1|2>: the arm fed to DecideSelfReadRoad. Off is the device's own road.
	GSSelfReadArm self_read_arm = GSSelfReadArm::Off;

	/// -declare-depth-feedback-loop: also declare the depth feedback loop on a device whose colour
	/// loop is declared. Turnip has a recorded tiler hang sampling the live depth buffer while it
	/// is the depth attachment, so expect a possible device lockup.
	bool declare_depth_loop = false;

	/// -loop-create-flag: declare the loop with the pipeline create flag rather than per draw.
	/// Must be final before the device is created, because it decides whether the dynamic-state
	/// extension is requested at all.
	bool loop_create_flag = false;

	GSLoopDeclarationSpelling LoopSpelling() const
	{
		return loop_create_flag ? GSLoopDeclarationSpelling::PipelineCreateFlag : kDefaultLoopDeclarationSpelling;
	}

	bool Any() const { return self_read_arm != GSSelfReadArm::Off || declare_depth_loop || loop_create_flag; }
};

inline GSMeasurementOverrides g_gs_measurement_overrides;
