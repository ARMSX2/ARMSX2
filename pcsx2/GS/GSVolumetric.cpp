// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Volumetric (ENABLE_VOLUMETRIC): the GSVolumetric state shared by the frontend, the renderer and the
// device (GSVolumetric.h). The GS's own files only call into it.

#include "GS/GSVolumetric.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

// Atomic: a host may switch it while the GS runs (the light gun needs depth in stereo mode).
static std::atomic<bool> s_volumetric_enabled{false};
static GSVolumetric::StereoSettings s_stereo; // in effect, GS thread only
static int s_stereo_eye = 0;

static std::array<GSVolumetric::ShotAnchor, GSVolumetric::MAX_SHOT_ANCHORS> s_shot_anchors; // GS thread
static size_t s_shot_anchor_count = 0;
using SceneDepthGrid = std::array<float, GSVolumetric::SCENE_DEPTH_WIDTH * GSVolumetric::SCENE_DEPTH_HEIGHT>;
static SceneDepthGrid s_scene_depth; // GS thread
static bool s_scene_depth_valid = false;

// The host's latest settings, taken by ApplyStereo between frames.
static std::mutex s_stereo_pending_mutex;
static GSVolumetric::StereoSettings s_stereo_pending;
static std::array<GSVolumetric::ShotAnchor, GSVolumetric::MAX_SHOT_ANCHORS> s_shot_anchors_pending;
static size_t s_shot_anchor_count_pending = 0;
static SceneDepthGrid s_scene_depth_pending;
static bool s_scene_depth_valid_pending = false;

void GSVolumetric::SetStereo(const StereoSettings& settings)
{
	std::lock_guard lock(s_stereo_pending_mutex);
	s_stereo_pending = settings;
}

void GSVolumetric::ApplyStereo()
{
	std::lock_guard lock(s_stereo_pending_mutex);
	s_stereo = s_stereo_pending;
	s_shot_anchors = s_shot_anchors_pending;
	s_shot_anchor_count = s_shot_anchor_count_pending;
	s_scene_depth_valid = s_scene_depth_valid_pending;
	if (s_scene_depth_valid)
		s_scene_depth = s_scene_depth_pending;
}

void GSVolumetric::SetShotAnchors(const ShotAnchor* anchors, size_t count)
{
	std::lock_guard lock(s_stereo_pending_mutex);
	s_shot_anchor_count_pending = std::min(count, MAX_SHOT_ANCHORS);
	std::copy_n(anchors, s_shot_anchor_count_pending, s_shot_anchors_pending.begin());
}

std::span<const GSVolumetric::ShotAnchor> GSVolumetric::GetShotAnchors()
{
	return {s_shot_anchors.data(), s_shot_anchor_count};
}

void GSVolumetric::SetSceneDepth(const float* znorm)
{
	std::lock_guard lock(s_stereo_pending_mutex);
	s_scene_depth_valid_pending = znorm != nullptr;
	if (znorm)
		std::copy_n(znorm, s_scene_depth_pending.size(), s_scene_depth_pending.begin());
}

float GSVolumetric::SceneDepth(float u0, float v0, float u1, float v1)
{
	if (!s_scene_depth_valid)
		return -1.0f;
	const auto cell = [](float t, u32 n) { return std::clamp(static_cast<int>(t * static_cast<float>(n)), 0, static_cast<int>(n) - 1); };
	const int x0 = cell(u0, SCENE_DEPTH_WIDTH), x1 = cell(u1, SCENE_DEPTH_WIDTH);
	const int y0 = cell(v0, SCENE_DEPTH_HEIGHT), y1 = cell(v1, SCENE_DEPTH_HEIGHT);
	float nearest = -1.0f;
	for (int y = y0; y <= y1; y++)
	{
		for (int x = x0; x <= x1; x++)
		{
			const float z = s_scene_depth[y * SCENE_DEPTH_WIDTH + x];
			if (z < 0.999f) // a HUD stamped at the maximum is not the scene
				nearest = std::max(nearest, z);
		}
	}
	return nearest > 0.0f ? nearest : -1.0f; // 0: nothing with depth there
}

const GSVolumetric::StereoSettings& GSVolumetric::GetStereo()
{
	return s_stereo;
}

int GSVolumetric::CurrentEye()
{
	return s_stereo_eye;
}

// Written by the host thread, read by the GS thread per draw; a torn update only mixes two
// consecutive head positions.
static std::atomic<float> s_eye_view[2][4] = {};

void GSVolumetric::SetEyeView(int eye, float offset_x, float offset_y, float depth_factor, float distance_ratio)
{
	s_eye_view[eye][0].store(offset_x, std::memory_order_relaxed);
	s_eye_view[eye][1].store(offset_y, std::memory_order_relaxed);
	s_eye_view[eye][2].store(depth_factor, std::memory_order_relaxed);
	s_eye_view[eye][3].store(distance_ratio, std::memory_order_relaxed);
}

GSVector4 GSVolumetric::EyeShift(int eye)
{
	const float k = s_eye_view[eye][2].load(std::memory_order_relaxed);
	return GSVector4(s_eye_view[eye][0].load(std::memory_order_relaxed) * k,
		s_eye_view[eye][1].load(std::memory_order_relaxed) * k, s_eye_view[eye][3].load(std::memory_order_relaxed), 0.0f);
}

static std::mutex s_present_dump_mutex;
static std::string s_present_dump_prefix;

void GSVolumetric::RequestPresentDump(std::string prefix)
{
	std::lock_guard lock(s_present_dump_mutex);
	s_present_dump_prefix = std::move(prefix);
}

std::string GSVolumetric::TakePresentDumpRequest()
{
	std::lock_guard lock(s_present_dump_mutex);
	return std::exchange(s_present_dump_prefix, std::string());
}

static std::atomic<int> s_draw_trace_frames{0};
static std::atomic<int> s_draw_trace_pending{0};
static std::atomic<u32> s_draw_trace_number{0};

void GSVolumetric::ArmDrawTrace(int frames)
{
	s_draw_trace_pending.store(frames, std::memory_order_release);
}

u32 GSVolumetric::DrawTraceNumber()
{
	return s_draw_trace_number.load(std::memory_order_relaxed);
}

bool GSVolumetric::DrawTraceActive()
{
	return s_draw_trace_frames.load(std::memory_order_relaxed) > 0;
}

static bool s_volumetric_trace = false;
static GSVolumetricFrame s_volumetric_frame;

void GSVolumetric::SetTrace(bool enabled)
{
	s_volumetric_trace = enabled;
}

bool GSVolumetric::IsTraceEnabled()
{
	return s_volumetric_trace;
}

void GSVolumetric::SetEnabled(bool enabled)
{
	s_volumetric_enabled.store(enabled, std::memory_order_relaxed);
}

bool GSVolumetric::IsEnabled()
{
	return s_volumetric_enabled.load(std::memory_order_relaxed);
}

static std::atomic<bool> s_depth_extras{false};

void GSVolumetric::SetDepthExtras(bool enabled)
{
	s_depth_extras.store(enabled, std::memory_order_relaxed);
}

bool GSVolumetric::DepthExtras()
{
	return s_depth_extras.load(std::memory_order_relaxed);
}

GSVolumetricFrame& GSVolumetric::CurrentFrame()
{
	return s_volumetric_frame;
}

void GSVolumetric::BeginVSync()
{
	// Whatever the merge does not refresh must not be presented with this frame.
	s_volumetric_frame.depth = nullptr;
	s_volumetric_frame.prehud = nullptr;
	s_volumetric_frame.live_depth = nullptr;
}

void GSVolumetric::EndMerge(int displayed_eye, bool idle_frame, bool multiview)
{
	// Draw trace: the armed frames start at a frame boundary.
	if (const int pending = s_draw_trace_pending.exchange(0, std::memory_order_acq_rel); pending > 0)
	{
		s_draw_trace_frames.store(pending, std::memory_order_relaxed);
		s_draw_trace_number.fetch_add(1, std::memory_order_relaxed);
		Console.WriteLn("Draw trace: start (%d frames)", pending);
	}
	else if (s_draw_trace_frames.load(std::memory_order_relaxed) > 0 &&
			 s_draw_trace_frames.fetch_sub(1, std::memory_order_relaxed) == 1)
	{
		Console.WriteLn("Draw trace: end");
	}

	// Stereo: the presented frame belongs to the eye its target was drawn for (a double-buffered
	// game shows the buffer drawn during the previous frame); the next frame is drawn for the
	// other eye. With multiview the frame holds both eyes, one per layer.
	s_volumetric_frame.stereo_layers = s_stereo.enabled && multiview;
	if (s_stereo.enabled && !s_volumetric_frame.stereo_layers)
	{
		s_volumetric_frame.stereo_eye = displayed_eye >= 0 ? displayed_eye : s_stereo_eye;
		if (!idle_frame)
			s_stereo_eye ^= 1;
	}
	else
	{
		s_volumetric_frame.stereo_eye = -1;
	}
}

void GSVolumetric::SetPresentedRect(const GSVector4& src_uv, const GSVector4& draw_rect)
{
	s_volumetric_frame.src_uv = src_uv;
	s_volumetric_frame.draw_rect = draw_rect;
}
