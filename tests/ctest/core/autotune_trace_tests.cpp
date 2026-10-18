// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneController against the windows of real trials from the first device round (MQ65, SD865, RG477V;
// devs/bmdhacks/campaigns/auto-tune-2026-10-18/device-rounds). Each table is a slice of one run's trace: the four
// windows that triggered the diagnosis, the four of the baseline (the last one carries the start of the trial), and
// the windows that followed, undo and settling included, so a trial that is looked at for longer sees what the
// device saw. The numbers are the trace's, unedited.
//
// The controller is built with the settle the round ran under (3 s for the light levers); the device-rounds'
// verdicts are about those windows.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneController.h"

#include <cstddef>
#include <vector>

namespace
{
	using Kind = AutoTuneAction::Kind;

	// One row of an AutoTuneTrace file.
	struct TraceWindow
	{
		double seconds, speed, late, ee, mtgs, gsb, mtvu, iop, gpu;
		int gpu_timer;
		double wait_gs, ee_ms, gs_ms, gsb_ms, gpu_ms;
	};

	AutoTuneWindow ToWindow(const TraceWindow& t)
	{
		AutoTuneWindow w;
		w.usable = true;
		w.seconds = t.seconds;
		w.speed_pct = t.speed;
		w.late_frame_ratio = t.late;
		w.ee_pct = t.ee;
		w.mtgs_pct = t.mtgs;
		w.gs_back_pct = t.gsb;
		w.mtvu_pct = t.mtvu;
		w.iop_pct = t.iop;
		w.gpu_pct = t.gpu;
		w.has_gpu_timer = t.gpu_timer != 0;
		w.ee_wait_gs_ratio = t.wait_gs;
		w.ee_ms = t.ee_ms;
		w.mtgs_ms = t.gs_ms;
		w.gs_back_ms = t.gsb_ms;
		w.gpu_ms = t.gpu_ms;
		return w;
	}

	// The policy the first device round ran under, for the settings that have since changed.
	AutoTunePolicy RoundOnePolicy()
	{
		AutoTunePolicy policy;
		policy.settle_seconds = 3.0;
		return policy;
	}

	class TraceRig
	{
	public:
		explicit TraceRig(const AutoTunePolicy& policy = RoundOnePolicy())
			: ctl(policy)
		{
			// Nothing but the lever a test is about is on offer: native resolution, no readbacks to give up, the
			// lowest blending, no IOP thread or governor.
			state.hw_renderer = true;
			state.resolution = 1.0;
			state.download_mode = AutoTuneDownloadMode::NoReadbacks;
			state.blending_level = 0;
		}

		AutoTuneController ctl;
		AutoTuneLeverState state;
		std::vector<AutoTuneAction> actions; // everything the controller said except None
		std::vector<size_t> action_at; // how many table windows had been fed when each of them came
		size_t fed = 0;

		void WarmUp()
		{
			AutoTuneWindow good;
			good.usable = true;
			good.speed_pct = 100.0;
			for (int i = 0; i < 20; i++)
				ctl.OnWindow(good, state);
		}

		AutoTuneAction Feed(const TraceWindow& t)
		{
			const AutoTuneAction a = ctl.OnWindow(ToWindow(t), state);
			fed++;
			if (a.kind != Kind::None)
			{
				actions.push_back(a);
				action_at.push_back(fed);
			}
			return a;
		}

		template <size_t N>
		void Feed(const TraceWindow (&rows)[N])
		{
			for (const TraceWindow& t : rows)
				Feed(t);
		}

		const AutoTuneAction* First(Kind kind) const
		{
			for (const AutoTuneAction& a : actions)
			{
				if (a.kind == kind)
					return &a;
			}
			return nullptr;
		}

		// How many table windows had been fed when `action` (a pointer into `actions`) came.
		size_t At(const AutoTuneAction* action) const { return action_at[static_cast<size_t>(action - actions.data())]; }

		size_t Count(Kind kind) const
		{
			size_t n = 0;
			for (const AutoTuneAction& a : actions)
				n += (a.kind == kind);
			return n;
		}
	};

	// ---------------------------------------------------------------------------------------- MQ65 traces

	// MQ65 God of War II combat, GS back thread
	const TraceWindow kGow2Gsb[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.503, 39.8, 1.000, 87.7, 93.9, 0.0, 70.0, 0.0, 47.4, 1, 0.098, 36.79, 39.40, 0.00, 19.89},
		{0.541, 40.1, 1.000, 89.0, 94.1, 0.0, 71.9, 0.0, 50.5, 1, 0.011, 37.03, 39.17, 0.00, 21.00},
		{0.541, 40.1, 1.000, 88.4, 94.0, 0.0, 71.4, 0.0, 43.7, 1, 0.091, 36.79, 39.14, 0.00, 18.18},
		{0.502, 39.9, 1.000, 87.7, 93.9, 0.0, 69.3, 0.0, 48.0, 1, 0.100, 36.68, 39.26, 0.00, 20.06},
		{0.501, 39.9, 1.000, 88.4, 94.1, 0.0, 71.9, 0.0, 47.2, 1, 0.080, 36.92, 39.33, 0.00, 19.72},
		{0.511, 39.2, 1.000, 87.8, 93.5, 0.0, 69.3, 0.0, 50.4, 1, 0.098, 37.37, 39.78, 0.00, 21.47},
		{0.508, 39.4, 1.000, 87.5, 94.1, 0.0, 70.3, 0.0, 46.9, 1, 0.101, 37.09, 39.87, 0.00, 19.86},
		{0.518, 38.6, 1.000, 85.7, 91.6, 0.0, 68.2, 0.0, 45.4, 1, 0.119, 37.03, 39.58, 0.00, 19.63},  // trial starts
		{1.845, 2.7, 1.000, 5.4, 85.3, 0.0, 4.8, 0.0, 2.0, 1, 0.888, 33.32, 524.75, 0.12, 12.53},
		{0.501, 16.7, 1.000, 75.0, 77.8, 37.1, 25.1, 0.0, 21.4, 1, 0.246, 75.14, 77.97, 37.15, 21.47},
		{0.512, 32.6, 1.000, 77.7, 86.3, 42.1, 61.6, 0.0, 51.6, 1, 0.204, 39.81, 44.24, 21.56, 26.42},
		{0.548, 36.5, 1.000, 91.1, 86.4, 47.1, 68.0, 0.0, 53.3, 1, 0.059, 41.63, 39.47, 21.53, 24.34},
		{0.528, 37.9, 1.000, 90.1, 84.9, 51.4, 73.6, 0.0, 47.8, 1, 0.070, 39.63, 37.32, 22.61, 21.01},
		{0.503, 39.8, 1.000, 92.0, 80.8, 56.6, 84.6, 0.0, 46.6, 1, 0.000, 38.57, 33.86, 23.72, 19.55},
		{0.523, 28.7, 1.000, 94.5, 62.5, 39.7, 55.4, 0.0, 39.8, 1, 0.008, 54.88, 36.29, 23.06, 23.11},
		{0.533, 37.6, 1.000, 92.8, 76.6, 49.9, 72.6, 0.0, 53.4, 1, 0.040, 41.21, 34.02, 22.13, 23.69},
		{0.519, 32.1, 1.000, 79.5, 70.6, 38.4, 57.2, 0.0, 50.7, 1, 0.163, 41.24, 36.66, 19.91, 26.32},
		{0.526, 34.9, 1.000, 83.0, 69.9, 45.8, 65.6, 0.0, 53.6, 1, 0.108, 39.70, 33.44, 21.92, 25.63},
		{0.506, 36.3, 1.000, 87.4, 77.7, 46.3, 66.8, 0.0, 47.6, 1, 0.078, 40.21, 35.76, 21.30, 21.88},
		{0.520, 41.7, 1.000, 94.6, 79.1, 57.6, 83.5, 0.0, 57.8, 1, 0.000, 37.83, 31.62, 23.04, 23.11},
		{0.515, 38.9, 1.000, 95.4, 76.4, 55.9, 77.8, 0.0, 49.7, 1, 0.000, 40.95, 32.81, 23.98, 21.35},  // traced: keep
		{0.535, 37.4, 1.000, 92.6, 76.9, 50.6, 73.2, 0.0, 48.5, 1, 0.000, 41.33, 34.30, 22.58, 21.62},
		{0.513, 39.1, 1.000, 94.8, 78.0, 55.9, 77.3, 0.0, 55.0, 1, 0.000, 40.51, 33.34, 23.88, 23.48},
		{0.531, 37.7, 1.000, 92.2, 71.6, 52.6, 75.7, 0.0, 48.0, 1, 0.000, 40.76, 31.67, 23.25, 21.21},
		{0.504, 36.4, 1.000, 94.1, 77.5, 50.5, 80.1, 0.0, 42.6, 1, 0.000, 43.07, 35.48, 23.11, 19.52},
		{0.536, 37.4, 1.000, 89.9, 73.5, 52.6, 74.2, 0.0, 51.3, 1, 0.000, 40.11, 32.80, 23.47, 22.88},
		{0.521, 38.4, 1.000, 94.8, 78.7, 55.5, 77.1, 0.0, 48.5, 1, 0.000, 41.14, 34.16, 24.11, 21.03},
		{0.503, 36.5, 1.000, 95.0, 73.0, 52.3, 77.6, 0.0, 41.5, 1, 0.000, 43.44, 33.38, 23.94, 18.97},
		{0.530, 37.7, 1.000, 92.4, 74.2, 55.0, 77.3, 0.0, 48.3, 1, 0.000, 40.85, 32.82, 24.31, 21.33},
		{0.531, 37.7, 1.000, 92.2, 73.8, 51.4, 76.5, 0.0, 50.7, 1, 0.000, 40.82, 32.68, 22.77, 22.43},
	};

	// MQ65 R&C UYA combat, GS back thread
	const TraceWindow kUyaGsb[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.516, 42.1, 1.000, 69.3, 86.7, 0.0, 30.1, 0.0, 48.7, 1, 0.068, 27.47, 34.38, 0.00, 19.32},
		{0.511, 45.8, 1.000, 72.4, 93.4, 0.0, 28.8, 0.0, 51.1, 1, 0.263, 26.40, 34.06, 0.00, 18.65},
		{0.509, 45.9, 1.000, 72.2, 92.8, 0.0, 29.0, 0.0, 50.4, 1, 0.263, 26.24, 33.73, 0.00, 18.31},
		{0.520, 44.9, 1.000, 70.7, 89.9, 0.0, 27.7, 0.0, 48.8, 1, 0.288, 26.29, 33.41, 0.00, 18.15},
		{0.515, 48.6, 1.000, 75.7, 96.4, 0.0, 28.9, 0.0, 53.1, 1, 0.216, 25.99, 33.08, 0.00, 18.22},
		{0.522, 47.9, 1.000, 74.8, 95.8, 0.0, 32.6, 0.0, 53.3, 1, 0.228, 26.01, 33.34, 0.00, 18.54},
		{0.520, 48.1, 1.000, 75.3, 95.0, 0.0, 28.3, 0.0, 52.3, 1, 0.233, 26.11, 32.96, 0.00, 18.16},
		{0.529, 47.3, 1.000, 74.3, 93.5, 0.0, 31.8, 0.0, 51.4, 1, 0.249, 26.21, 32.98, 0.00, 18.14},  // trial starts
		{1.978, 2.5, 1.000, 4.0, 79.6, 0.0, 1.6, 0.0, 1.8, 1, 0.941, 26.58, 524.71, 0.11, 12.02},
		{0.505, 19.8, 1.000, 32.7, 53.6, 72.2, 11.4, 0.0, 20.8, 1, 0.594, 27.55, 45.15, 60.77, 17.55},
		{0.513, 45.5, 1.000, 78.6, 65.5, 82.6, 31.5, 0.0, 58.6, 1, 0.167, 28.82, 24.00, 30.29, 21.49},
		{0.504, 53.0, 1.000, 93.8, 71.6, 83.6, 40.8, 0.0, 61.4, 1, 0.000, 29.55, 22.53, 26.32, 19.33},
		{0.517, 51.7, 1.000, 94.6, 72.4, 81.2, 36.3, 0.0, 59.1, 1, 0.000, 30.55, 23.37, 26.20, 19.08},
		{0.534, 50.0, 1.000, 90.2, 70.9, 82.2, 35.8, 0.0, 58.9, 1, 0.066, 30.08, 23.63, 27.41, 19.66},
		{0.530, 50.3, 1.000, 88.6, 69.6, 79.8, 34.9, 0.0, 57.8, 1, 0.091, 29.37, 23.06, 26.44, 19.16},
		{0.524, 50.9, 1.000, 91.3, 70.2, 81.5, 36.7, 0.0, 58.4, 1, 0.075, 29.91, 23.00, 26.71, 19.13},
		{0.527, 50.7, 1.000, 91.4, 72.4, 84.6, 36.6, 0.0, 60.0, 1, 0.014, 30.12, 23.83, 27.85, 19.75},
		{0.505, 33.1, 1.000, 50.1, 46.0, 52.2, 25.1, 0.0, 44.5, 1, 0.000, 25.27, 23.21, 26.34, 22.46},
		{0.515, 48.6, 1.000, 95.9, 69.3, 80.6, 34.8, 0.0, 56.8, 1, 0.017, 32.91, 23.79, 27.65, 19.49},
		{0.511, 45.7, 1.000, 81.5, 63.8, 75.7, 31.1, 0.0, 52.9, 1, 0.167, 29.77, 23.31, 27.65, 19.33},
		{0.524, 44.5, 1.000, 77.7, 61.1, 72.9, 30.6, 0.0, 50.9, 1, 0.201, 29.10, 22.87, 27.32, 19.05},  // traced: keep
		{0.514, 51.9, 1.000, 84.7, 71.3, 82.8, 39.4, 0.0, 60.6, 1, 0.017, 27.23, 22.90, 26.59, 19.46},
		{0.505, 52.9, 1.000, 98.2, 71.9, 84.7, 42.3, 0.0, 60.9, 1, 0.000, 30.99, 22.70, 26.74, 19.20},
		{0.513, 52.1, 1.000, 96.1, 72.2, 84.3, 37.9, 0.0, 61.1, 1, 0.000, 30.79, 23.13, 27.00, 19.59},
		{0.521, 51.2, 1.000, 94.6, 70.7, 82.7, 38.6, 0.0, 60.4, 1, 0.013, 30.83, 23.03, 26.96, 19.67},
		{0.509, 52.4, 1.000, 95.0, 71.6, 82.7, 37.0, 0.0, 60.7, 1, 0.013, 30.22, 22.77, 26.34, 19.31},
		{0.511, 52.2, 1.000, 93.0, 71.6, 83.2, 39.1, 0.0, 60.2, 1, 0.020, 29.73, 22.89, 26.59, 19.25},
		{0.505, 52.8, 1.000, 92.5, 72.0, 84.7, 38.7, 0.0, 61.4, 1, 0.014, 29.20, 22.75, 26.76, 19.38},
		{0.501, 53.3, 1.000, 95.9, 72.0, 83.8, 38.3, 0.0, 61.6, 1, 0.000, 30.00, 22.52, 26.20, 19.26},
		{0.508, 52.6, 1.000, 93.1, 71.7, 83.5, 38.9, 0.0, 60.6, 1, 0.010, 29.55, 22.76, 26.50, 19.24},
	};
} // namespace

// ---------------------------------------------------------------------------------------------- keep on cost

TEST(AutoTuneTrace, TheGow2BackThreadIsNotKeptOnItsCostAlone)
{
	// The GS thread's cost fell 16% because the work was split over two threads, and the speed fell with it:
	// 39.9% before, 37.6% over the first four windows (-5.6%, noise 9.4% from two hitch windows). A/B on the device:
	// 38.1% against 38.1%, and a big core burned. The old rule kept it, because the speed drop was inside the noise.
	TraceRig rig;
	rig.state.gs_back_thread_allowed = true;
	rig.WarmUp();
	rig.Feed(kGow2Gsb);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::GsBackThread);
	EXPECT_EQ(start->bottleneck, AutoTuneBottleneck::GsCpu);

	EXPECT_EQ(rig.Count(Kind::Keep), 0u) << "a lever that did not speed the game up is not kept because a cost fell";
	const AutoTuneAction* revert = rig.First(Kind::Revert);
	ASSERT_NE(revert, nullptr) << "and the trial ends";
	EXPECT_EQ(revert->lever, AutoTuneLever::GsBackThread);
	EXPECT_LT(revert->gain, 0.0);
}

TEST(AutoTuneTrace, TheUyaBackThreadIsKeptAfterALongerLook)
{
	// The same lever on R&C: GS cost 33.4 -> 27.5 ms, and over the first four windows a speed of 46.6% -> 45.1%
	// (-3.2%) because of one 33% hitch window. A/B on the device: +11%. The first look cannot tell, so it looks
	// again; with eight windows the speed is up.
	TraceRig rig;
	rig.state.gs_back_thread_allowed = true;
	rig.WarmUp();
	rig.Feed(kUyaGsb);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::GsBackThread);

	const AutoTuneAction* keep = rig.First(Kind::Keep);
	ASSERT_NE(keep, nullptr);
	EXPECT_EQ(keep->lever, AutoTuneLever::GsBackThread);
	EXPECT_TRUE(keep->measured);
	EXPECT_GT(keep->speed_after, keep->speed_before) << "kept with the speed up, not with it down";
	EXPECT_GT(keep->cost_gain, 0.10);

	// The first look is four windows after the 9 of the settle; it kept on the second one, four windows later.
	EXPECT_GE(rig.At(keep) - rig.At(start), 9u + 4u + 4u);
}
