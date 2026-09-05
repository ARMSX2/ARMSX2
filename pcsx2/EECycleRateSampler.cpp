// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRateSampler.h"

#include "VMManager.h"

#include "common/HostSys.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
	// Whether the blocking sites should time themselves at all. Read from the MTVU thread
	// as well as the CPU thread, because MTVU reaches MTGS::WaitGS and the ring-full stall
	// on its own; relaxed, because a window either side of the flag moving is a window the
	// lifecycle reset that moved it has already thrown away.
	std::atomic<bool> s_measuring_waits{false};

	// Charged to the frame in progress and read out at the frame boundary. CPU-thread
	// owned: BeginWait() hands out a start only to the CPU thread, so only the CPU thread
	// can reach the accumulate.
	u64 s_frame_gs_wait_ticks = 0;
	u64 s_frame_vu1_wait_ticks = 0;

	// A ratio the caller can still use. Anything the arithmetic cannot produce a finite,
	// non-negative number for comes back as a zero on an invalidated window, not as a NaN
	// that IsSampleUsable would have to catch downstream.
	float SafeRatio(double numerator, double denominator)
	{
		if (!(denominator > 0.0) || !std::isfinite(numerator) || !std::isfinite(denominator))
			return 0.0f;

		const double ratio = numerator / denominator;
		return std::isfinite(ratio) ? static_cast<float>(std::max(0.0, ratio)) : 0.0f;
	}
} // namespace

EECycleRateWindowSample EECycleRateSampler::BuildWindowSample(const WindowInputs& in)
{
	EECycleRateWindowSample out;
	out.frames = in.frame_count;

	if (in.frames == nullptr || in.frame_count == 0 || in.frame_count > kMaxWindowFrames || in.tick_frequency == 0)
		return out; // valid stays false

	// Per-frame ratios for the percentile, and running sums for everything else. One pass,
	// because the window is at most kMaxWindowFrames long and this runs twice a second.
	float ratios[kMaxWindowFrames];
	u32 ratio_count = 0;
	u32 late_frames = 0;
	double target_ticks = 0.0;
	double active_ticks = 0.0;
	double own_ticks = 0.0;
	double blocked_ticks = 0.0;

	for (u32 i = 0; i < in.frame_count; i++)
	{
		const EECycleRateFrameSample& f = in.frames[i];
		if (f.target_ticks == 0)
			return out; // a frame with no budget is a broken window, not a fast one

		// A wait that spanned the frame boundary can be charged more time than the frame's
		// own active window holds. Clamp per frame rather than at the sum, so one such
		// frame cannot make the whole window's own-time negative.
		const u64 blocked = std::min<u64>(f.gs_wait_ticks + f.vu1_wait_ticks, f.active_ticks);

		ratios[ratio_count++] = static_cast<float>(static_cast<double>(f.active_ticks) / static_cast<double>(f.target_ticks));
		late_frames += f.late ? 1 : 0;
		target_ticks += static_cast<double>(f.target_ticks);
		active_ticks += static_cast<double>(f.active_ticks);
		own_ticks += static_cast<double>(f.active_ticks - blocked);
		blocked_ticks += static_cast<double>(blocked);
	}

	std::sort(ratios, ratios + ratio_count);
	const u32 rank = static_cast<u32>(std::ceil(0.9 * static_cast<double>(ratio_count)));
	out.p90_active_ratio = ratios[(rank == 0 ? 1u : rank) - 1u];

	out.target_seconds = target_ticks / static_cast<double>(in.tick_frequency);
	out.late_ratio = SafeRatio(static_cast<double>(late_frames), static_cast<double>(in.frame_count));
	out.own_time_ratio = SafeRatio(own_ticks, target_ticks);
	out.blocked_ratio = SafeRatio(blocked_ticks, active_ticks);

	// The CPU-time cross-check is optional: a platform or a harness that cannot read the
	// thread's CPU time reports zero rather than invalidating the window, because nothing
	// in the policy gates on it - it is there to be read in a trace when a downshift looks
	// wrong.
	if (in.cpu_time_frequency != 0)
	{
		const double cpu_seconds = static_cast<double>(in.cpu_time_delta) / static_cast<double>(in.cpu_time_frequency);
		out.cpu_time_ratio = SafeRatio(cpu_seconds, out.target_seconds);
	}

	out.valid = !in.invalidated;
	return out;
}

u64 EECycleRateSampler::BeginWait()
{
	if (!s_measuring_waits.load(std::memory_order_relaxed))
		return 0;

	if (!VMManager::Internal::IsOnCPUThread())
		return 0;

	return GetCPUTicks();
}

void EECycleRateSampler::EndGSWait(u64 begin_ticks)
{
	if (begin_ticks == 0)
		return;

	const u64 now = GetCPUTicks();
	if (now > begin_ticks)
		s_frame_gs_wait_ticks += now - begin_ticks;
}

void EECycleRateSampler::EndVU1Wait(u64 begin_ticks)
{
	if (begin_ticks == 0)
		return;

	const u64 now = GetCPUTicks();
	if (now > begin_ticks)
		s_frame_vu1_wait_ticks += now - begin_ticks;
}
