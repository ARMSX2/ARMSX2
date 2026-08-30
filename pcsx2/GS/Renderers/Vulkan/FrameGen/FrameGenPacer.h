// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/frame_gen_pacer.h.
// Logic unchanged; only the scalar aliases and settings accessors are ours. See FrameGenTypes.h.

#pragma once

#include "FrameGenTypes.h"

#include <chrono>
#include <optional>

namespace Vulkan
{
	struct FrameGenPlan
	{
		size_t generations{};
		bool warm{};
	};

	/// Decides how many frames to interpolate for the frame about to be presented.
	///
	/// The naive alternative — always generate LsfgMultiplier-1 frames — is what makes frame
	/// generation feel worse than no frame generation on a game whose rendered rate moves. This
	/// watches the real cadence and adapts: it smooths the measured interval, ignores bursts,
	/// stabilises after a stall, and probes the generation count upward only while the output is
	/// short of target, backing off with an escalating delay when a probe makes things worse.
	class FrameGenPacer
	{
	public:
		[[nodiscard]] FrameGenPlan Plan(size_t capacity);

		void Reset();

	private:
		using Clock = std::chrono::steady_clock;

		void Stabilize(Clock::time_point now);
		void DeferEvaluations(Clock::duration amount);
		void UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate, size_t ceiling);

		std::optional<Clock::time_point> last_frame;
		std::optional<Clock::time_point> stable_until;
		std::optional<Clock::time_point> probe_until;
		std::optional<Clock::time_point> next_probe;
		std::optional<Clock::time_point> deficit_since;
		f32 smoothed_interval{};
		f32 output_credit{};
		f32 probe_base_rate{};
		f32 unloaded_base_rate{};
		size_t issued_generations{};
		size_t probe_previous_limit{};
		size_t limit{};
		u32 probe_failures{};
	};
} // namespace Vulkan
