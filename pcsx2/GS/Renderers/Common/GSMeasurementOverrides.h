// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDynamicFeedbackLoopPolicy.h"
#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"

#include <string>

/// Switches that move the Vulkan self-read road, or size the vertex ring, for an A/B. Only
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

	/// -no-stencil-buffer: create depth as plain D32F and report no stencil buffer, as a device
	/// under DriverWorkaround::DisableStencilBuffer does (Turnip before Mesa 26.2). Puts the old
	/// driver's destination-alpha choices on a device that has D32S8.
	bool disable_stencil_buffer = false;

	/// -vertex-ring-kib N: the Vulkan vertex ring's starting size, in KiB (0 = the shipped size),
	/// clamped to 64 KiB .. the growth cap.
	/// It still grows to its cap on demand, so a small start drives the growth path on every title.
	u32 vertex_ring_start_kib = 0;

	/// -vertex-ring-no-grow: the vertex ring keeps its starting size and waits for the GPU when full.
	bool vertex_ring_no_growth = false;

	/// -texture-ring-no-grow: the texture upload ring keeps its starting size and waits for the GPU
	/// when full.
	bool texture_ring_no_growth = false;

	/// -stream-rings-cached: put the stream rings on a cached road as if the driver database granted
	/// DriverWorkaround::PreferCachedStreamRingMemory, to price that road on a device without the rule.
	bool stream_rings_cached = false;

	/// -readback-kick-passes N: in a readback frame, the Vulkan mid-frame kick waits for at least
	/// N unsubmitted render passes (0 = the shipped spacing). Read once, when the device is created.
	u32 readback_kick_passes = 0;

	/// -alpha-bit-logic-op: take the alpha-bit logic op (GSAlphaBitLogicOp.h) on any Vulkan device
	/// with the logicOp feature, not only where a read waits per draw. For checking its pictures
	/// against the read on a GPU that does not need it.
	bool alpha_bit_logic_op = false;

	/// -date-copy-with-ad-blend: let a destination-alpha draw that blends with destination alpha
	/// share the stencil copy, as before that was refused (GSRendererHW::EmulateDATESelectMethod).
	/// For pricing the refusal on one binary. Unlike the switches above, read per draw.
	bool date_copy_with_dst_alpha_blend = false;

	/// -no-provoking-vertex: run Vulkan as a device without VK_EXT_provoking_vertex, as the rule
	/// broken_provoking_vertex does for Qualcomm's stock Adreno driver. Pipelines keep the API's
	/// first-vertex default, so the paths a provoking-first device takes (HandleFlatShadedVertices,
	/// the expanded-line vertex shader) run on a machine whose driver has the extension.
	bool no_provoking_vertex = false;

	/// -no-dual-source: run Vulkan as a device without dualSrcBlend, as Arm's stock Mali driver is.
	/// GSRendererHW then blends every SRC1 equation in the shader instead of through the second
	/// fragment output, so a device that has dual-source blending can be A/B'd against one that
	/// does not on a single binary.
	bool no_dual_source = false;

	/// -runs off: the Vulkan run machine (GSRunsVK.h) is not created, so every draw takes the classic path. The
	/// baseline of the machine's A/Bs and of its picture checks.
	bool runs_off = false;

	/// -runs-split none|dear: whether a run ends where a draw's dear states (the hardware's lod, an alpha test) differ
	/// from the run's. Default dear.
	bool runs_split_dear = true;

	/// -runs-split-mask M: a run also ends where any state in M (bits as -runs-read-mask) changes from one draw to the
	/// next, so those states are constants of every run's program. For pricing a split rule state by state.
	u32 runs_split_mask = 0;

	/// -runs-nofill: each run's host draw is scissored to one pixel, so a no-fill arm prices a pass without its fill.
	bool runs_nofill = false;

	/// -runs-max-draws N: a run ends after N draws (0 = the machine's own limit). 1 draws every merged draw alone under
	/// the run program, which separates the program's arithmetic from the merge in a picture check.
	u32 runs_max_draws = 0;

	/// -runs-general / -runs-read-mask M: the run program's states in M (bit i = the i-th state of a row's flags, in
	/// tfx_runs.glsl's order: tfx tcc fog iip atst way lod rtasrc) are read from the rows even where the run holds one
	/// value of them. -runs-general is M = 0xFF, the one general program. Prices the specialisation state by state and
	/// separates its effect from the merge's in a picture check.
	u32 runs_read_mask = 0;

	/// -runs-draw-range LO HI: only the TFX draws numbered LO..HI-1 since the device was created may join a run (the
	/// rest take the classic path), and each of them gets a D line in the log. Finds the draw behind a picture difference.
	u64 runs_draw_lo = 0;
	u64 runs_draw_hi = 0;

	/// -runs-sync-compile: a run program is built on the GS thread when a run first needs it, instead of on the
	/// device's compile queue while the draws that need it are drawn classically. Prices the asynchronous build.
	bool runs_sync_compile = false;

	/// -runs-log <path>: one line per render pass with the machine's counts, and one per compiled run program.
	std::string runs_log;

	GSLoopDeclarationSpelling LoopSpelling() const
	{
		return loop_create_flag ? GSLoopDeclarationSpelling::PipelineCreateFlag : kDefaultLoopDeclarationSpelling;
	}

	bool Any() const
	{
		return self_read_arm != GSSelfReadArm::Off || declare_depth_loop || loop_create_flag || disable_stencil_buffer ||
		       alpha_bit_logic_op || date_copy_with_dst_alpha_blend || no_provoking_vertex || no_dual_source ||
		       stream_rings_cached || runs_off || !runs_split_dear || runs_nofill;
	}
};

inline GSMeasurementOverrides g_gs_measurement_overrides;
