// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoTune/AutoTuneSampler.h"

#include "EECycleRateSampler.h"
#include "GSDumpReplayer.h"
#include "IopThread/IopThreadVm.h"
#include "VMManager.h"

#include "common/HostSys.h"

#include <algorithm>
#include <optional>

namespace
{
	// A CPU-thread span longer than this many metrics windows (plus slack for the polling granularity)
	// has a pause or a stall in it, so the share of it the EE spent waiting says nothing about the window.
	constexpr double kMaxSpanInWindows = 2.0;
	constexpr double kSpanSlackSeconds = 0.25;

	// `wait` out of `span`, as a share. Unknown if the total went backwards (the counters started over).
	// A wait that began before the earlier reading can put slightly more than the span in, so it is capped.
	std::optional<double> ShareOfSpan(double now, double before, double span)
	{
		if (now < before)
			return std::nullopt;
		return std::min(1.0, (now - before) / span);
	}
} // namespace

AutoTuneTally AutoTuneReadLiveTally()
{
	AutoTuneTally t;
	const double tick_frequency = static_cast<double>(GetTickFrequency());
	t.time_s = static_cast<double>(GetCPUTicks()) / tick_frequency;

	EECycleRateTally raw;
	if (EECycleRateSampler::GetTally(&raw))
	{
		t.frames = raw.frames;
		t.overran_frames = raw.overran_frames;
		t.waits_measured = true;
		t.gs_wait_s = static_cast<double>(raw.gs_wait_ticks) / tick_frequency;
		t.vu1_wait_s = static_cast<double>(raw.vu1_wait_ticks) / tick_frequency;

		// Both counters are written by the EE thread (this one) and kept whether or not anything asks.
		const IopThread::Stats iop = IopThread::GetStats();
		t.iop_wait_s = static_cast<double>(iop.lag_stall_ns + iop.emu_hold_stall_ns) * 1e-9;
	}
	return t;
}

void AutoTuneSampler::Reset(uint32_t seen_sequence, const AutoTuneTally& baseline)
{
	m_last_sequence = seen_sequence;
	m_previous = baseline;
	m_have_previous = true;
}

bool AutoTuneSampler::Poll(const PerformanceMetrics::WindowSnapshot& snap, const AutoTuneConditions& cond,
	const AutoTuneTally& now, AutoTuneWindow* out)
{
	if (snap.sequence == 0 || snap.sequence == m_last_sequence)
		return false;
	m_last_sequence = snap.sequence;

	const AutoTuneTally before = m_previous;
	const bool have_before = m_have_previous;
	m_previous = now;
	m_have_previous = true;

	AutoTuneWindow w;
	w.seconds = snap.seconds;
	w.ee_pct = snap.ee_usage;
	w.mtgs_pct = snap.gs_usage;
	w.gs_back_pct = snap.has_gs_back_thread ? snap.gs_back_usage : 0.0;
	w.mtvu_pct = snap.vu_usage;
	w.iop_pct = snap.has_iop_thread ? snap.iop_usage : 0.0;
	w.gpu_pct = snap.gpu_usage;
	w.has_gpu_timer = snap.has_gpu_timer;
	w.ee_ms = snap.ee_ms;
	w.mtgs_ms = snap.gs_ms;
	w.gs_back_ms = snap.has_gs_back_thread ? snap.gs_back_ms : 0.0;
	w.gpu_ms = snap.has_gpu_timer ? snap.gpu_ms : 0.0;

	if (have_before && now.frames >= before.frames && now.overran_frames >= before.overran_frames)
	{
		const uint64_t frames = now.frames - before.frames;
		const uint64_t overran = std::min(now.overran_frames - before.overran_frames, frames);
		w.late_frame_ratio = frames ? static_cast<double>(overran) / static_cast<double>(frames) : 0.0;
	}

	const double span = now.time_s - before.time_s;
	if (have_before && now.waits_measured && before.waits_measured && span > 0.0 &&
		span <= kMaxSpanInWindows * snap.seconds + kSpanSlackSeconds)
	{
		w.ee_wait_gs_ratio = ShareOfSpan(now.gs_wait_s, before.gs_wait_s, span);
		w.ee_wait_vu1_ratio = ShareOfSpan(now.vu1_wait_s, before.vu1_wait_s, span);
		w.ee_wait_iop_ratio = ShareOfSpan(now.iop_wait_s, before.iop_wait_s, span);
	}

	// A window says something about steady play only while the limiter is aiming at the nominal speed.
	w.usable = cond.vm_running && cond.nominal_limiter && !cond.frame_advancing && !cond.gs_dump &&
	           cond.target_speed > 0.0 && snap.seconds > 0.0f;
	w.speed_pct = cond.target_speed > 0.0 ? static_cast<double>(snap.speed) / cond.target_speed : 0.0;

	*out = w;
	return true;
}

bool AutoTuneSampler::PollLive(AutoTuneWindow* out)
{
	PerformanceMetrics::WindowSnapshot snap;
	if (!PerformanceMetrics::TryGetWindowSnapshot(&snap) || snap.sequence == m_last_sequence)
		return false;

	AutoTuneConditions cond;
	cond.vm_running = VMManager::GetState() == VMState::Running;
	cond.nominal_limiter = VMManager::GetLimiterMode() == LimiterModeType::Nominal;
	cond.frame_advancing = VMManager::Internal::IsFrameAdvancing();
	cond.gs_dump = GSDumpReplayer::IsReplayingDump();
	cond.target_speed = VMManager::GetTargetSpeed();

	return Poll(snap, cond, AutoTuneReadLiveTally(), out);
}

void AutoTuneSampler::ResetLive()
{
	PerformanceMetrics::WindowSnapshot snap;
	Reset(PerformanceMetrics::TryGetWindowSnapshot(&snap) ? snap.sequence : m_last_sequence, AutoTuneReadLiveTally());
}
