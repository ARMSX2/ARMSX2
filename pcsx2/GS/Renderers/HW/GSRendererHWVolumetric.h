// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Volumetric (GSVolumetric.h, ENABLE_VOLUMETRIC): the hardware renderer's depth capture and stereo state, held by
// GSRendererHW as m_vr. The code using it is in GSRendererHWVolumetric.cpp.

#pragma once

#include "GS/Renderers/HW/GSTextureCache.h"

#include <array>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

class GSTexture;

// Which depth target a colour target's 3D geometry (Z writes on, depth varying across the draw)
// was last drawn with, keyed by the colour target's base block. Games often share one Z buffer
// between their frame buffers and clear it, or stamp the HUD into it, before the finished frame is
// displayed, so each pairing keeps a snapshot of the depth taken when its frame was done (see
// GSRendererHW::SnapshotDepthBeforeDraw).
struct GSVolumetricDepthPairing
{
	u32 rt_bp = 0;
	u32 rt_bw = 0;
	u32 z_bp = 0;
	u32 z_bw = 0;
	u32 z_psm = 0;
	u64 last_draw = 0;
	bool dirty = false; // 3D depth written since the last snapshot
	GSTexture* snapshot = nullptr; // R32F copy of the whole depth target
	float snapshot_scale = 1.0f; // the depth target's scale when copied
	GSTexture* colour_snapshot = nullptr; // the colour target at the same moment
	float colour_snapshot_scale = 1.0f;
	u32 snapshot_vsync = 0; // depth_vsync when the snapshot was taken
};

struct GSRendererHWVolumetricState
{
	// Stereo: this draw works in screen space and gets no parallax shift (see VolumetricClassifyDraw).
	bool stereo_screen_space = false;
	bool stereo_source = false; // ... because it textures from a stereo picture
	bool stereo_display_target = false; // this draw's target is (or was recently) displayed
	bool display_copy = false; // this draw copies a displayed picture into another target
	// A 2D overlay placed in the scene: the depth it goes to (else < 0), on a shot or (overlay_fx) as
	// an effect at the scene's depth under it.
	float overlay_znorm = -1.0f;
	bool overlay_fx = false;
	// The 2D overlays drawn into displayed pictures, by top left corner: the first and
	// last vsync each was drawn in, without a gap. One drawn for a while is a HUD piece, not a shot's.
	std::unordered_map<u64, std::pair<u32, u32>> overlay_seen;
	u32 overlay_vsync = 0;
	std::array<u32, 4> stereo_display_bp = {~0u, ~0u, ~0u, ~0u}; // recent display framebuffer bases

	// A few pairings cover the scene, its post-processing buffers and double-buffered frames.
	std::array<GSVolumetricDepthPairing, 8> depth_pairings{};
	// Stereo: the eye each recently drawn colour target (by base block) was drawn for.
	std::array<std::pair<u32, int>, 8> rt_stereo_eye{};
	u32 rt_stereo_eye_next = 0;
	// The display targets GetOutput() returned this vsync, per circuit.
	std::array<GSTextureCache::Target*, 2> output_rt{};
	// Rings of R32F depth and RGBA8 pre-HUD canvases, so the host can still be reading earlier ones.
	std::array<GSTexture*, 3> depth_canvas{};
	std::array<GSTexture*, 3> prehud_canvas{};
	std::array<GSTexture*, 3> live_depth_canvas{};
	u32 depth_canvas_index = 0;
	u32 depth_vsync = 1; // counts MergeDepth calls; 0 never matches a snapshot

	// GSVolumetric::SetTrace: Z-writing draws per (rt bp, z bp, flat z, ztst) since the last print.
	std::map<std::tuple<u32, u32, bool, u32>, u32> depth_trace;
	u32 depth_trace_vsyncs = 0;
	bool depth_trace_frame = false; // logging every draw of the current frame
	float trace_q_min = 0.0f, trace_q_max = 0.0f; // Q range of perspective textured draws
};
