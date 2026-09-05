// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRateSampler.h"

#include "EECycleRate.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/HostSys.h"
#include "common/Threading.h"

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

	// --- window state, all CPU-thread owned --------------------------------------------

	// Sampling at all. One predictable branch at the top of the per-frame entry point, so
	// a build with the governor off pays a load and a not-taken branch per frame.
	bool s_active = false;

	// A VM exists and its CPU providers are built. Every lifecycle seam below can be
	// reached before that - a settings apply runs from inside VMManager::Initialize(),
	// before UpdateCPUImplementations() - and a rate transition would reset a recompiler
	// that is not there yet.
	bool s_vm_started = false;

	EECycleRateController s_controller;

	// The window in progress. Frames go in as they are measured; the arithmetic happens
	// once, at close.
	EECycleRateFrameSample s_frames[EECycleRateSampler::kMaxWindowFrames];
	u32 s_frame_count = 0;
	u64 s_window_target_ticks = 0;
	bool s_window_invalid = false;
	u64 s_window_cpu_time = 0;

	// GetTickFrequency() and the window length in its units, resolved once at VM start
	// rather than per frame.
	u64 s_tick_frequency = 0;
	u64 s_window_close_ticks = 0;

	void StartWindow()
	{
		s_frame_count = 0;
		s_window_target_ticks = 0;
		s_window_invalid = false;
		s_frame_gs_wait_ticks = 0;
		s_frame_vu1_wait_ticks = 0;
		s_window_cpu_time = PerformanceMetrics::GetCPUThreadCPUTime();
	}

	// Decides whether the sampler runs, from the eligibility answer for right now. Only
	// the "was the governor asked for" half matters here: a suspension - turbo, a frame
	// advance, hardcore mode - still has to produce windows, because the controller
	// answers a suspension by asking for the baseline back.
	void RefreshActivation(const EECycleRateEligibility& eligibility)
	{
		s_active = eligibility.enabled;
		s_measuring_waits.store(s_active, std::memory_order_relaxed);
	}

	void CloseWindow()
	{
		EECycleRateSampler::WindowInputs in;
		in.frames = s_frames;
		in.frame_count = s_frame_count;
		in.tick_frequency = s_tick_frequency;
		in.cpu_time_delta = PerformanceMetrics::GetCPUThreadCPUTime() - s_window_cpu_time;
		in.cpu_time_frequency = Threading::GetThreadTicksPerSecond();
		in.invalidated = s_window_invalid;

		const EECycleRateWindowSample sample = EECycleRateSampler::BuildWindowSample(in);
		const EECycleRateEligibility eligibility = EECycleRate::ResolveEligibility();
		const EECycleRateDecision decision = s_controller.Update(sample, eligibility);

		switch (decision)
		{
			case EECycleRateDecision::StepDown:
				EECycleRate::ApplyEffective(s_controller.GetEffective(), "the host is missing the frame deadline");
				break;

			case EECycleRateDecision::StepUp:
				EECycleRate::ApplyEffective(s_controller.GetEffective(), "there is deadline headroom again");
				break;

			case EECycleRateDecision::RestoreBaseline:
				EECycleRate::ApplyEffective(EECycleRate::GetConfigured(), "the governor is no longer eligible");
				break;

			case EECycleRateDecision::None:
				break;
		}

		// Correct the controller's belief from what actually happened. It updates that
		// belief optimistically when it returns a decision, so a request the selector
		// refused - out of bounds, or coalesced with a settings apply that landed in the
		// same event test - would otherwise leave it acting on a rate we are not running.
		s_controller.OnApplied(EECycleRate::GetEffective());

		RefreshActivation(eligibility);
		StartWindow();
	}

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

void EECycleRateSampler::OnVMStart()
{
	s_tick_frequency = GetTickFrequency();
	s_window_close_ticks =
		static_cast<u64>(s_controller.GetPolicy().window_seconds * static_cast<double>(s_tick_frequency));

	s_vm_started = true;
	OnLifecycleReset("VM start");
}

void EECycleRateSampler::OnVMShutdown()
{
	// Put the selector back before the recompilers go away, so the next VM starts from the
	// configured value rather than inheriting a decision about a machine that is gone.
	// SyncToConfigured() rather than ApplyEffective(): there is no generated code left to
	// keep honest, and a cache reset on the way out is pure cost.
	EECycleRate::SyncToConfigured();

	s_vm_started = false;
	s_active = false;
	s_measuring_waits.store(false, std::memory_order_relaxed);
	s_controller.Reset(EECycleRate::GetConfigured());
	StartWindow();
}

void EECycleRateSampler::OnLifecycleReset(const char* reason)
{
	if (!s_vm_started)
		return;

	// Give the player's rate back first. This is also the path an eligibility loss takes
	// when it stops Throttle() from sampling at all - the limiter going unlimited means no
	// window ever closes, so the controller's own RestoreBaseline would never be asked for.
	// ApplyEffective is a no-op, with no cache reset, when we are already at the baseline.
	EECycleRate::ApplyEffective(EECycleRate::GetConfigured(), reason);

	const EECycleRateEligibility eligibility = EECycleRate::ResolveEligibility();
	RefreshActivation(eligibility);

	s_controller.Reset(eligibility.baseline);
	StartWindow();
}

void EECycleRateSampler::OnFrameThrottled(u64 active_ticks, u64 target_ticks, bool late)
{
	if (!s_active)
		return;

	// No work period was open - the frame before this one was not paced, or a pause landed
	// in the middle of it. Its length is unknown, not zero, so the window it falls in is
	// unmeasurable.
	if (active_ticks == 0 || target_ticks == 0)
	{
		s_window_invalid = true;
	}
	else
	{
		// Always in range: the window closes the moment the count reaches the bound, so a
		// frame never arrives with the array already full.
		EECycleRateFrameSample& frame = s_frames[s_frame_count++];
		frame.active_ticks = active_ticks;
		frame.target_ticks = target_ticks;
		frame.gs_wait_ticks = s_frame_gs_wait_ticks;
		frame.vu1_wait_ticks = s_frame_vu1_wait_ticks;
		frame.late = late;

		s_window_target_ticks += target_ticks;
	}

	s_frame_gs_wait_ticks = 0;
	s_frame_vu1_wait_ticks = 0;

	// The array bound is the second close condition, not a drop condition: a limiter target
	// short enough to fit more than kMaxWindowFrames into the window closes it early, and
	// the sample still describes exactly the frames it counted.
	if (s_window_target_ticks >= s_window_close_ticks || s_frame_count >= kMaxWindowFrames)
		CloseWindow();
}

void EECycleRateSampler::OnFrameNotThrottled()
{
	if (!s_active)
		return;

	s_window_invalid = true;
	s_frame_gs_wait_ticks = 0;
	s_frame_vu1_wait_ticks = 0;
}
