// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoTune/AutoTuneSampler.h"

#include "GSDumpReplayer.h"
#include "VMManager.h"

#include <algorithm>

AutoTuneEeWaits AutoTuneSampleEeWaits(double /*window_seconds*/)
{
	return {};
}

void AutoTuneSampler::NoteLimitedFrame(bool late)
{
	m_frames++;
	if (late)
		m_late++;
}

void AutoTuneSampler::Reset(uint32_t seen_sequence)
{
	m_last_sequence = seen_sequence;
	m_frames = 0;
	m_late = 0;
}

bool AutoTuneSampler::Poll(const PerformanceMetrics::WindowSnapshot& snap, const AutoTuneConditions& cond,
	const AutoTuneEeWaits& waits, AutoTuneWindow* out)
{
	if (snap.sequence == 0 || snap.sequence == m_last_sequence)
		return false;
	m_last_sequence = snap.sequence;

	AutoTuneWindow w;
	w.seconds = snap.seconds;
	w.ee_pct = snap.ee_usage;
	w.mtgs_pct = snap.gs_usage;
	w.gs_back_pct = snap.has_gs_back_thread ? snap.gs_back_usage : 0.0;
	w.mtvu_pct = snap.vu_usage;
	w.iop_pct = snap.has_iop_thread ? snap.iop_usage : 0.0;
	w.gpu_pct = snap.gpu_usage;
	w.has_gpu_timer = snap.has_gpu_timer;
	w.ee_wait_gs_ratio = waits.gs;
	w.ee_wait_vu1_ratio = waits.vu1;

	w.late_frame_ratio = m_frames ? static_cast<double>(std::min(m_late, m_frames)) / static_cast<double>(m_frames) : 0.0;
	m_frames = 0;
	m_late = 0;

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

	// The wait totals are consumed exactly once per window.
	const AutoTuneEeWaits waits = AutoTuneSampleEeWaits(snap.seconds);
	return Poll(snap, cond, waits, out);
}

void AutoTuneSampler::ResetLive()
{
	PerformanceMetrics::WindowSnapshot snap;
	Reset(PerformanceMetrics::TryGetWindowSnapshot(&snap) ? snap.sequence : m_last_sequence);
}
