// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "EECycleRateSampler.h"

#include "BuildVersion.h"
#include "Config.h"
#include "EECycleRate.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Pcsx2Defs.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "fmt/format.h"

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

	// --- shadow mode and the development trace ------------------------------------------

	// The controller runs and its decisions are recorded, but nothing is applied. This is
	// what the trace is for: it says what the governor would have done to a run whose
	// timing it did not change, which is the only way to judge a classifier against a
	// fixed-rate A/B of the same scene.
	bool s_shadow = false;

	std::FILE* s_trace = nullptr;
	bool s_trace_header_written = false;
	bool s_trace_game_written = false;
	u32 s_trace_window = 0;

	// The session summary, written once at shutdown whether or not a trace is open.
	// Target-time spent at each selector, indexed by (selector - MIN_EE_CYCLE_RATE).
	constexpr int kSelectorCount =
		Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_RATE - Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE + 1;
	double s_seconds_at_selector[kSelectorCount] = {};
	u32 s_windows_rejected = 0;
	u32 s_windows_seen = 0;
	bool s_ran_this_session = false;

	// Transitions and ineffective verdicts for the whole session. The controller's own
	// counters cannot answer this: they are per-run, and every lifecycle reset zeroes them
	// - including the pause that precedes a shutdown, which is why the session line used to
	// report zero of both however busy the run had been. The trace's per-window columns
	// still come from the controller, because there they mean the run they are in.
	u32 s_session_transitions = 0;
	u32 s_session_ineffective = 0;

	// Take the controller's counters into the session totals. Every caller is about to make
	// them unreachable - a reset zeroes them, and at shutdown the session is over - so
	// nothing is counted twice.
	void FoldControllerCountsIntoSession()
	{
		const EECycleRateController::Snapshot snap = s_controller.GetSnapshot();
		s_session_transitions += snap.transitions;
		s_session_ineffective += snap.ineffective_count;
	}

	// Published for the on-screen display. Written by the CPU thread at every window close
	// and every lifecycle reset; read by the GS thread.
	std::atomic<u32> s_overlay_word{0};

	void PublishOverlay()
	{
		EECycleRateSampler::OverlayState state;
		state.configured = EECycleRate::GetConfigured();
		state.effective = EECycleRate::GetEffective();
		state.state = s_controller.GetState();
		state.enabled = s_active && !s_shadow;
		state.shadow = s_shadow;
		s_overlay_word.store(EECycleRateSampler::PackOverlayWord(state), std::memory_order_relaxed);
	}

	const char* StateName(EECycleRateController::State state)
	{
		switch (state)
		{
			case EECycleRateController::State::Disabled:
				return "disabled";
			case EECycleRateController::State::Warmup:
				return "warmup";
			case EECycleRateController::State::Observe:
				return "observe";
			case EECycleRateController::State::Cooldown:
				return "cooldown";
			default:
				return "?";
		}
	}

	const char* DecisionName(EECycleRateDecision decision)
	{
		switch (decision)
		{
			case EECycleRateDecision::StepDown:
				return "stepdown";
			case EECycleRateDecision::StepUp:
				return "stepup";
			case EECycleRateDecision::RestoreBaseline:
				return "restorebaseline";
			case EECycleRateDecision::None:
			default:
				return "none";
		}
	}

	// The reason strings carry commas, and the replay fixture splits on every one. Trailing
	// columns it does not understand are harmless, extra columns are not, so the commas go.
	std::string CsvSafe(const char* text)
	{
		std::string out(text ? text : "");
		std::replace(out.begin(), out.end(), ',', ';');
		return out;
	}

	// Written once, before anything else, so a file that never gets a measurable window
	// still says which binary and which policy produced it.
	void EnsureTraceHeader()
	{
		if (!s_trace || s_trace_header_written)
			return;

		s_trace_header_written = true;

		const EECycleRatePolicy& p = s_controller.GetPolicy();
		std::fprintf(s_trace, "# armsx2 ee-cycle-rate trace v1 build=%s mode=%s configured=%d floor=%d\n",
			BuildVersion::GitHash, s_shadow ? "shadow" : "live", static_cast<int>(EECycleRate::GetConfigured()),
			static_cast<int>(EECycleRate::GetFloor()));
		std::fprintf(s_trace,
			"# policy window_s=%.3f warmup_s=%.3f cooldown_s=%.3f overload_p90=%.3f overload_late=%.3f "
			"min_own=%.3f max_blocked=%.3f down_dwell=%u headroom_p90=%.3f up_dwell=%u "
			"eff_margin=%.3f eff_windows=%u ineffective_hold_s=%.3f max_steps_down=%u\n",
			p.window_seconds, p.warmup_seconds, p.cooldown_seconds, p.overload_p90, p.overload_late,
			p.min_own_time, p.max_blocked, p.downshift_dwell, p.headroom_p90, p.upshift_dwell,
			p.effectiveness_margin, p.effectiveness_windows, p.ineffective_hold_seconds, p.max_steps_down);
		// The first twelve columns are the replay contract the controller's CSV fixture reads;
		// everything after them is diagnostics it ignores. Do not reorder the first twelve.
		std::fprintf(s_trace,
			"# valid,frames,target_seconds,p90,late,own,cpu,blocked,enabled,eligible,baseline,decision,"
			"window,usable,active_seconds,gs_wait_seconds,vu1_wait_seconds,state,configured,ctl_effective,"
			"applied_effective,overload_dwell,headroom_dwell,clock_seconds,inhibit_seconds,transitions,"
			"ineffective,applied,shadow,gen_ee,gen_iop,gen_vu0,gen_vu1,gen_vif,enabled_reason,eligible_reason\n");
	}

	// Which game, written once. Separately from the header, because the ELF CRC is not known
	// until the game has actually booted and a zero there identifies nothing: the line waits
	// for a real CRC, and `force` is the VM-shutdown case where waiting longer is pointless.
	void MaybeWriteTraceGame(bool force)
	{
		if (!s_trace || s_trace_game_written)
			return;

		const u32 crc = VMManager::GetCurrentCRC();
		if (crc == 0 && !force)
			return;

		std::fprintf(s_trace, "# game serial=%s crc=%08x\n", VMManager::GetDiscSerial().c_str(), crc);
		s_trace_game_written = true;
	}

	void OpenTrace()
	{
		const std::string& path = EmuConfig.Speedhacks.DynamicEECycleRateTrace;
		if (path.empty() || s_trace)
			return;

		// Append: a scene is usually captured over several runs, and a run that silently
		// truncated the one before it is a lost afternoon.
		s_trace = FileSystem::OpenCFile(path.c_str(), "ab");
		s_trace_header_written = false;
		s_trace_game_written = false;
		s_trace_window = 0;
		if (!s_trace)
			Console.Error("EE cycle rate: could not open trace '%s' for append.", path.c_str());
	}

	void CloseTrace()
	{
		if (!s_trace)
			return;

		std::fclose(s_trace);
		s_trace = nullptr;
		s_trace_header_written = false;
		s_trace_game_written = false;
	}

	void TraceLifecycle(const char* reason, bool shutting_down = false)
	{
		if (!s_trace)
			return;

		EnsureTraceHeader();
		MaybeWriteTraceGame(shutting_down);

		// A comment, so a trace stays replayable by the fixture whatever happened to the VM
		// while it was being recorded.
		std::fprintf(s_trace, "# reset,%s,configured=%d,effective=%d,transitions=%u\n",
			CsvSafe(reason).c_str(), static_cast<int>(EECycleRate::GetConfigured()),
			static_cast<int>(EECycleRate::GetEffective()), EECycleRate::GetTransitionCount());
		std::fflush(s_trace);
	}

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
	//
	// A trace path with the governor off is shadow mode: everything measures and decides,
	// nothing is applied.
	void RefreshActivation(const EECycleRateEligibility& eligibility)
	{
		s_shadow = !eligibility.enabled && (s_trace != nullptr);
		s_active = eligibility.enabled || s_shadow;
		s_ran_this_session |= s_active;
		s_measuring_waits.store(s_active, std::memory_order_relaxed);
	}

	// The session summary's raw material. Time is charged to the selector the window
	// actually ran at, which is why the caller passes it in rather than letting this read
	// the live one: by the time this is called the window's own decision may already have
	// moved the selector, and charging the window to the rate that replaced it would put
	// the cost of a slow scene against the step taken to answer it.
	void AccountWindow(const EECycleRateWindowSample& sample, s8 effective_during_window)
	{
		s_windows_seen++;
		if (!EECycleRateController::IsSampleUsable(sample))
		{
			s_windows_rejected++;
			return;
		}

		const int index = static_cast<int>(effective_during_window) - Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;
		if (index >= 0 && index < kSelectorCount)
			s_seconds_at_selector[index] += sample.target_seconds;
	}

	void TraceWindow(const EECycleRateWindowSample& sample, const EECycleRateEligibility& eligibility,
		const EECycleRate::EligibilityReasons& why, EECycleRateDecision decision, bool applied)
	{
		if (!s_trace)
			return;

		EnsureTraceHeader();
		MaybeWriteTraceGame(false);

		const EECycleRateController::Snapshot snap = s_controller.GetSnapshot();
		const EECycleRate::ResetGenerations gen = EECycleRate::GetResetGenerations();

		double active_seconds = 0.0;
		double gs_wait_seconds = 0.0;
		double vu1_wait_seconds = 0.0;
		for (u32 i = 0; i < s_frame_count; i++)
		{
			active_seconds += static_cast<double>(s_frames[i].active_ticks);
			gs_wait_seconds += static_cast<double>(s_frames[i].gs_wait_ticks);
			vu1_wait_seconds += static_cast<double>(s_frames[i].vu1_wait_ticks);
		}
		const double to_seconds = (s_tick_frequency != 0) ? (1.0 / static_cast<double>(s_tick_frequency)) : 0.0;
		active_seconds *= to_seconds;
		gs_wait_seconds *= to_seconds;
		vu1_wait_seconds *= to_seconds;

		// Columns 1-12 are the replay contract; everything after is diagnostics.
		std::fprintf(s_trace,
			"%d,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%s,"
			"%u,%d,%.6f,%.6f,%.6f,%s,%d,%d,%d,%u,%u,%.3f,%.3f,%u,%u,%d,%d,%u,%u,%u,%u,%u,%s,%s\n",
			sample.valid ? 1 : 0, sample.frames, sample.target_seconds, sample.p90_active_ratio, sample.late_ratio,
			sample.own_time_ratio, sample.cpu_time_ratio, sample.blocked_ratio, eligibility.enabled ? 1 : 0,
			eligibility.eligible ? 1 : 0, static_cast<int>(eligibility.baseline), DecisionName(decision),
			s_trace_window++, EECycleRateController::IsSampleUsable(sample) ? 1 : 0, active_seconds, gs_wait_seconds,
			vu1_wait_seconds, StateName(snap.state), static_cast<int>(EECycleRate::GetConfigured()),
			static_cast<int>(snap.effective), static_cast<int>(EECycleRate::GetEffective()), snap.overload_dwell,
			snap.headroom_dwell, snap.clock_seconds, snap.downshift_inhibit_seconds, snap.transitions,
			snap.ineffective_count, applied ? 1 : 0, s_shadow ? 1 : 0, gen.ee, gen.iop, gen.vu0, gen.vu1, gen.vif,
			CsvSafe(why.enabled).c_str(), CsvSafe(why.eligible).c_str());

		// Per window, not per session: a trace whose tail is missing because the run was
		// killed is exactly the trace of the run worth looking at.
		std::fflush(s_trace);
	}

	// Deliberately not inlined into OnFrameThrottled(). It is a big function - the window
	// arithmetic, an eligibility resolution, a trace row - and inlined it forces its whole
	// register save and its 576-byte stack frame onto the per-frame path, which runs it
	// thirty times for every once this does.
	__noinline void CloseWindow()
	{
		EECycleRateSampler::WindowInputs in;
		in.frames = s_frames;
		in.frame_count = s_frame_count;
		in.tick_frequency = s_tick_frequency;
		in.cpu_time_delta = PerformanceMetrics::GetCPUThreadCPUTime() - s_window_cpu_time;
		in.cpu_time_frequency = Threading::GetThreadTicksPerSecond();
		in.invalidated = s_window_invalid;

		const EECycleRateWindowSample sample = EECycleRateSampler::BuildWindowSample(in);

		// The rate the frames below were actually run at. Read before the decision, because
		// the decision may replace it.
		const s8 effective_during_window = EECycleRate::GetEffective();

		EECycleRate::EligibilityReasons why;
		const EECycleRateEligibility eligibility = EECycleRate::ResolveEligibility(EECycleRate::ReadEligibilityInputs(), &why);

		// Shadow mode answers "what would the governor have done to this run", so the
		// controller has to be told the switch is on - fed the real answer it would sit in
		// Disabled and classify nothing, which is the opposite of observing. The suspension
		// half is left alone: turbo and hardcore really do suspend it, and a shadow run
		// should show that happening.
		//
		// The trace records what the controller was fed rather than what the settings say,
		// because the twelve replay columns have to reproduce the decision column. The
		// header's mode= says which run it was.
		EECycleRateEligibility fed = eligibility;
		if (s_shadow)
		{
			fed.enabled = true;
			// ...and the reason column says so, rather than leaving the row claiming the
			// governor is enabled next to a reason explaining that it is off.
			why.enabled = "shadow mode: the switch is off and the controller is told it is on";
		}

		const EECycleRateDecision decision = s_controller.Update(sample, fed);

		bool applied = false;
		if (!s_shadow)
		{
			switch (decision)
			{
				case EECycleRateDecision::StepDown:
					applied = EECycleRate::ApplyEffective(s_controller.GetEffective(), "the host is missing the frame deadline");
					break;

				case EECycleRateDecision::StepUp:
					applied = EECycleRate::ApplyEffective(s_controller.GetEffective(), "there is deadline headroom again");
					break;

				case EECycleRateDecision::RestoreBaseline:
					applied = EECycleRate::ApplyEffective(EECycleRate::GetConfigured(), "the governor is no longer eligible");
					break;

				case EECycleRateDecision::None:
					break;
			}

			// Correct the controller's belief from what actually happened. It updates that
			// belief optimistically when it returns a decision, so a request the selector
			// refused - out of bounds, or coalesced with a settings apply that landed in the
			// same event test - would otherwise leave it acting on a rate we are not running.
			s_controller.OnApplied(EECycleRate::GetEffective());
		}

		AccountWindow(sample, effective_during_window);
		TraceWindow(sample, fed, why, decision, applied);

		RefreshActivation(eligibility);
		PublishOverlay();
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

	for (double& seconds : s_seconds_at_selector)
		seconds = 0.0;
	s_windows_rejected = 0;
	s_windows_seen = 0;
	s_session_transitions = 0;
	s_session_ineffective = 0;
	s_ran_this_session = false;

	OpenTrace();

	s_vm_started = true;
	OnLifecycleReset("VM start");

	if (s_shadow)
	{
		Console.WriteLn("EE cycle rate: governor running in SHADOW mode - windows are measured and decided, "
						"nothing is applied. Trace: %s",
			EmuConfig.Speedhacks.DynamicEECycleRateTrace.c_str());
	}
}

void EECycleRateSampler::OnVMShutdown()
{
	FoldControllerCountsIntoSession();

	if (s_ran_this_session)
	{
		// The one line the governor writes outside a trace. Everything else it has to say
		// per session is either a transition, which logs itself, or in the trace.
		std::string at_rate;
		for (int i = 0; i < kSelectorCount; i++)
		{
			if (s_seconds_at_selector[i] <= 0.0)
				continue;
			at_rate += fmt::format("{}{}={:.1f}s", at_rate.empty() ? "" : " ",
				i + Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE, s_seconds_at_selector[i]);
		}

		Console.WriteLn("EE cycle rate: %s session over - %u windows (%u rejected), %u transitions, "
						"%u ineffective; time at rate: %s",
			s_shadow ? "shadow" : "governor", s_windows_seen, s_windows_rejected, s_session_transitions,
			s_session_ineffective, at_rate.empty() ? "none" : at_rate.c_str());
	}

	TraceLifecycle("VM shutdown", true);
	CloseTrace();

	// Put the selector back before the recompilers go away, so the next VM starts from the
	// configured value rather than inheriting a decision about a machine that is gone.
	// SyncToConfigured() rather than ApplyEffective(): there is no generated code left to
	// keep honest, and a cache reset on the way out is pure cost.
	EECycleRate::SyncToConfigured();

	s_vm_started = false;
	s_active = false;
	s_shadow = false;
	s_measuring_waits.store(false, std::memory_order_relaxed);
	s_controller.Reset(EECycleRate::GetConfigured());
	PublishOverlay();
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

	FoldControllerCountsIntoSession();
	s_controller.Reset(eligibility.baseline);
	PublishOverlay();
	StartWindow();
	TraceLifecycle(reason);
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

// The word is four bits of configured selector, four of effective, two of controller state
// and two flags. The selectors are biased by -MIN_EE_CYCLE_RATE so the whole -3..3 range is
// unsigned, which is what makes the round trip exact rather than sign-extension-dependent.
namespace
{
	constexpr int kSelectorBias = -Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;

	constexpr u32 kConfiguredShift = 0;
	constexpr u32 kEffectiveShift = 4;
	constexpr u32 kStateShift = 8;
	constexpr u32 kEnabledBit = 1u << 10;
	constexpr u32 kShadowBit = 1u << 11;

	constexpr u32 kSelectorMask = 0xF;
	constexpr u32 kStateMask = 0x3;
} // namespace

u32 EECycleRateSampler::PackOverlayWord(const OverlayState& state)
{
	const u32 configured = static_cast<u32>(
		std::clamp<int>(static_cast<int>(state.configured) + kSelectorBias, 0, static_cast<int>(kSelectorMask)));
	const u32 effective = static_cast<u32>(
		std::clamp<int>(static_cast<int>(state.effective) + kSelectorBias, 0, static_cast<int>(kSelectorMask)));

	return (configured << kConfiguredShift) | (effective << kEffectiveShift) |
		   ((static_cast<u32>(state.state) & kStateMask) << kStateShift) | (state.enabled ? kEnabledBit : 0u) |
		   (state.shadow ? kShadowBit : 0u);
}

EECycleRateSampler::OverlayState EECycleRateSampler::UnpackOverlayWord(u32 word)
{
	OverlayState state;
	state.configured = static_cast<s8>(static_cast<int>((word >> kConfiguredShift) & kSelectorMask) - kSelectorBias);
	state.effective = static_cast<s8>(static_cast<int>((word >> kEffectiveShift) & kSelectorMask) - kSelectorBias);
	state.state = static_cast<EECycleRateController::State>((word >> kStateShift) & kStateMask);
	state.enabled = (word & kEnabledBit) != 0;
	state.shadow = (word & kShadowBit) != 0;
	return state;
}

u32 EECycleRateSampler::GetOverlayWord()
{
	return s_overlay_word.load(std::memory_order_relaxed);
}
