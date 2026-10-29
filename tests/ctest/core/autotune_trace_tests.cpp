// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneController against the windows of real trials recorded on devices (MQ65, SD865, RG477V). Each table is a
// slice of one run's trace: the four windows that triggered the diagnosis, the four of the baseline (the last one
// carries the start of the trial), and the windows that followed, undo and settling included, so a trial that is
// looked at for longer sees what the device saw. The numbers are the trace's, unedited.
//
// The first-round tables are fed to a controller built with the settle that round ran under (3 s for the light
// levers).

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneController.h"

#include <cmath>
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

	// The default policy with the 3 s settle the first-round traces were recorded under.
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

	// MQ65 Shadow of the Colossus (windowed run), GS back thread, IOP thread already on
	const TraceWindow kSotcGsb[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.506, 72.5, 0.409, 92.7, 90.5, 0.0, 48.4, 86.5, 77.0, 1, 0.000, 21.33, 20.83, 0.00, 35.44},
		{0.545, 73.4, 0.500, 92.1, 91.6, 0.0, 48.5, 87.7, 77.9, 1, 0.000, 20.94, 20.82, 0.00, 35.42},
		{0.504, 72.8, 0.636, 95.5, 90.8, 0.0, 50.0, 88.4, 77.3, 1, 0.000, 21.88, 20.82, 0.00, 35.43},
		{0.500, 76.7, 0.545, 91.6, 92.1, 0.0, 47.9, 87.4, 77.8, 1, 0.000, 19.92, 20.03, 0.00, 35.36},
		{0.500, 73.4, 0.636, 90.6, 91.6, 0.0, 49.1, 88.1, 76.9, 1, 0.000, 20.60, 20.82, 0.00, 34.96},
		{0.500, 73.3, 0.636, 94.6, 91.7, 0.0, 49.0, 87.2, 77.0, 1, 0.000, 21.52, 20.87, 0.00, 35.03},
		{0.509, 68.8, 0.545, 93.2, 90.3, 0.0, 50.0, 87.9, 75.8, 1, 0.000, 22.60, 21.88, 0.00, 35.10},
		{0.504, 72.9, 0.591, 92.3, 91.5, 0.0, 48.7, 87.2, 76.9, 1, 0.000, 21.13, 20.95, 0.00, 35.23},  // trial starts
		{1.737, 1.9, 0.667, 4.1, 86.7, 0.0, 2.3, 3.8, 0.0, 1, 0.966, 35.83, 753.13, 0.12, 0.00},
		{0.544, 24.5, 1.000, 33.0, 32.3, 62.9, 14.4, 26.5, 15.3, 1, 0.668, 22.45, 22.00, 42.78, 20.83},
		{0.536, 37.4, 1.000, 48.3, 31.4, 66.4, 22.3, 42.8, 47.7, 1, 0.519, 21.56, 14.01, 29.64, 42.58},
		{0.530, 50.3, 1.000, 69.2, 38.3, 55.5, 30.0, 57.3, 52.3, 1, 0.294, 22.94, 12.71, 18.38, 34.69},
		{0.535, 49.9, 1.000, 65.5, 38.9, 55.2, 29.4, 58.0, 51.6, 1, 0.342, 21.89, 13.02, 18.45, 34.51},
		{0.519, 51.4, 1.000, 67.8, 38.0, 56.3, 30.4, 59.0, 52.5, 1, 0.314, 22.01, 12.34, 18.27, 34.11},
		{0.524, 51.0, 1.000, 66.6, 38.2, 56.3, 30.1, 58.4, 53.2, 1, 0.330, 21.79, 12.50, 18.42, 34.85},
		{0.564, 53.2, 1.000, 72.2, 40.0, 58.5, 31.8, 61.1, 55.2, 1, 0.268, 22.64, 12.53, 18.33, 34.59},
		{0.501, 53.3, 1.000, 70.3, 39.5, 58.9, 31.8, 61.8, 55.2, 1, 0.281, 22.02, 12.38, 18.46, 34.57},
		{0.548, 54.8, 1.000, 67.1, 41.2, 60.4, 32.6, 63.7, 57.0, 1, 0.323, 20.43, 12.55, 18.40, 34.70},
		{0.518, 51.5, 1.000, 69.6, 38.4, 56.0, 30.6, 58.5, 53.4, 1, 0.298, 22.52, 12.42, 18.12, 34.60},
		{0.507, 52.7, 1.000, 70.3, 38.2, 57.2, 31.2, 60.3, 54.5, 1, 0.287, 22.28, 12.11, 18.13, 34.55},
		{0.535, 49.9, 1.000, 64.5, 37.4, 55.2, 29.3, 58.2, 52.2, 1, 0.349, 21.55, 12.51, 18.43, 34.88},
		{0.510, 52.3, 1.000, 65.5, 38.5, 57.4, 31.1, 60.4, 55.0, 1, 0.335, 20.87, 12.27, 18.28, 35.03},
		{0.522, 51.1, 0.938, 72.2, 37.0, 55.3, 30.2, 58.0, 52.7, 1, 0.274, 23.57, 12.09, 18.07, 34.39},
		{0.506, 52.7, 1.000, 69.0, 38.2, 57.4, 30.9, 59.9, 54.3, 1, 0.299, 21.82, 12.09, 18.14, 34.34},
	};

	// RG477V Shadow of the Colossus at 5x, resolution 5x -> 4x, then the 5x windows after the undo
	const TraceWindow kSotcResolution4[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.535, 87.4, 0.750, 48.0, 27.8, 0.0, 28.1, 0.0, 0.0, 0, 0.511, 9.16, 5.32, 0.00, 0.00},
		{0.501, 86.5, 0.731, 47.9, 26.8, 0.0, 28.4, 0.0, 0.0, 0, 0.514, 9.24, 5.17, 0.00, 0.00},
		{0.502, 86.4, 0.769, 46.8, 26.6, 0.0, 27.2, 0.0, 0.0, 0, 0.523, 9.04, 5.13, 0.00, 0.00},
		{0.516, 84.1, 0.808, 46.5, 32.4, 0.0, 27.0, 0.0, 0.0, 0, 0.527, 9.22, 6.44, 0.00, 0.00},
		{0.522, 89.6, 0.643, 50.1, 28.7, 0.0, 28.8, 0.0, 0.0, 0, 0.494, 9.33, 5.35, 0.00, 0.00},
		{0.521, 89.6, 0.679, 49.2, 29.1, 0.0, 29.4, 0.0, 0.0, 0, 0.499, 9.15, 5.42, 0.00, 0.00},
		{0.505, 85.9, 0.769, 44.8, 27.2, 0.0, 26.3, 0.0, 0.0, 0, 0.545, 8.70, 5.28, 0.00, 0.00},
		{0.519, 83.6, 0.808, 43.0, 25.3, 0.0, 24.4, 0.0, 0.0, 0, 0.551, 8.59, 5.04, 0.00, 0.00},  // trial starts
		{0.510, 94.8, 0.148, 54.9, 37.2, 0.0, 30.0, 0.0, 0.0, 0, 0.223, 9.66, 6.54, 0.00, 0.00},
		{0.500, 100.1, 0.000, 64.7, 40.1, 0.0, 31.9, 0.0, 0.0, 0, 0.000, 10.79, 6.69, 0.00, 0.00},
		{0.501, 100.0, 0.000, 65.0, 40.5, 0.0, 32.6, 0.0, 0.0, 0, 0.000, 10.84, 6.76, 0.00, 0.00},
		{0.500, 100.0, 0.000, 64.3, 37.3, 0.0, 31.5, 0.0, 0.0, 0, 0.000, 10.73, 6.22, 0.00, 0.00},
		{0.501, 100.0, 0.000, 62.6, 36.6, 0.0, 29.1, 0.0, 0.0, 0, 0.000, 10.45, 6.11, 0.00, 0.00},
		{0.500, 100.0, 0.000, 64.0, 39.6, 0.0, 32.4, 0.0, 0.0, 0, 0.000, 10.68, 6.61, 0.00, 0.00},
		{0.512, 74.9, 0.400, 60.4, 40.4, 0.0, 39.4, 0.0, 0.0, 0, 0.258, 13.44, 9.00, 0.00, 0.00},
		{0.506, 75.8, 0.870, 60.1, 44.2, 0.0, 41.6, 0.0, 0.0, 0, 0.376, 13.24, 9.72, 0.00, 0.00},
		{0.500, 93.3, 0.385, 58.8, 42.1, 0.0, 35.5, 0.0, 0.0, 0, 0.239, 10.50, 7.52, 0.00, 0.00},
		{0.501, 99.9, 0.000, 63.5, 40.6, 0.0, 32.6, 0.0, 0.0, 0, 0.000, 10.60, 6.77, 0.00, 0.00},
		{0.500, 100.0, 0.000, 64.5, 38.8, 0.0, 34.2, 0.0, 0.0, 0, 0.010, 10.77, 6.47, 0.00, 0.00},
		{0.503, 72.9, 0.625, 60.9, 40.5, 0.0, 42.9, 0.0, 0.0, 0, 0.326, 13.92, 9.26, 0.00, 0.00},
		{0.520, 77.0, 0.958, 54.4, 43.7, 0.0, 41.4, 0.0, 0.0, 0, 0.451, 11.79, 9.47, 0.00, 0.00},
		{0.521, 76.9, 1.000, 46.3, 40.6, 0.0, 38.3, 0.0, 0.0, 0, 0.533, 10.04, 8.80, 0.00, 0.00},  // traced: revert
		{0.515, 55.0, 0.765, 35.9, 32.3, 0.0, 27.0, 0.0, 0.0, 0, 0.516, 10.89, 9.80, 0.00, 0.00},
		{0.512, 52.2, 1.000, 32.3, 27.8, 0.0, 24.8, 0.0, 0.0, 0, 0.675, 10.34, 8.88, 0.00, 0.00},
		{0.515, 51.9, 1.000, 31.5, 28.3, 0.0, 24.4, 0.0, 0.0, 0, 0.681, 10.14, 9.10, 0.00, 0.00},
		{0.514, 51.9, 1.000, 32.7, 27.8, 0.0, 24.9, 0.0, 0.0, 0, 0.670, 10.51, 8.94, 0.00, 0.00},
		{0.512, 52.1, 1.000, 31.8, 27.1, 0.0, 24.4, 0.0, 0.0, 0, 0.679, 10.19, 8.69, 0.00, 0.00},
		{0.507, 52.7, 1.000, 33.0, 28.1, 0.0, 25.4, 0.0, 0.0, 0, 0.667, 10.43, 8.90, 0.00, 0.00},
		{0.513, 52.0, 1.000, 32.0, 27.9, 0.0, 24.8, 0.0, 0.0, 0, 0.677, 10.27, 8.95, 0.00, 0.00},
		{0.515, 51.8, 1.000, 32.3, 27.3, 0.0, 25.1, 0.0, 0.0, 0, 0.671, 10.41, 8.79, 0.00, 0.00},
		{0.508, 52.5, 1.000, 33.2, 28.2, 0.0, 25.4, 0.0, 0.0, 0, 0.665, 10.54, 8.97, 0.00, 0.00},
		{0.510, 52.3, 1.000, 32.4, 27.6, 0.0, 25.2, 0.0, 0.0, 0, 0.672, 10.33, 8.80, 0.00, 0.00},
		{0.515, 51.9, 1.000, 32.3, 27.3, 0.0, 25.0, 0.0, 0.0, 0, 0.670, 10.38, 8.79, 0.00, 0.00},
		{0.518, 51.5, 1.000, 31.7, 27.0, 0.0, 24.4, 0.0, 0.0, 0, 0.683, 10.27, 8.74, 0.00, 0.00},
		{0.511, 52.3, 1.000, 32.3, 27.4, 0.0, 24.8, 0.0, 0.0, 0, 0.673, 10.30, 8.75, 0.00, 0.00},
		{0.507, 52.6, 1.000, 33.3, 28.4, 0.0, 26.3, 0.0, 0.0, 0, 0.664, 10.55, 9.01, 0.00, 0.00},  // traced: start
		{0.518, 12.9, 0.750, 7.9, 29.3, 7.1, 6.0, 0.0, 0.0, 0, 0.920, 10.26, 37.97, 9.24, 0.00},
		{0.516, 55.0, 1.000, 31.8, 17.1, 26.0, 24.6, 0.0, 0.0, 0, 0.675, 9.65, 5.19, 7.88, 0.00},
		{0.510, 55.6, 1.000, 32.2, 17.0, 26.2, 24.6, 0.0, 0.0, 0, 0.674, 9.67, 5.10, 7.88, 0.00},
	};

	// The windows of the same trial from the heavier part of the scene (EE about 13 ms where the rest has about
	// 10.5): the speed 4x gives once the game has got heavier.
	const TraceWindow kSotcHeavyAt4x[] = {
		{0.512, 74.9, 0.400, 60.4, 40.4, 0.0, 39.4, 0.0, 0.0, 0, 0.258, 13.44, 9.00, 0.00, 0.00},
		{0.506, 75.8, 0.870, 60.1, 44.2, 0.0, 41.6, 0.0, 0.0, 0, 0.376, 13.24, 9.72, 0.00, 0.00},
		{0.503, 72.9, 0.625, 60.9, 40.5, 0.0, 42.9, 0.0, 0.0, 0, 0.326, 13.92, 9.26, 0.00, 0.00},
		{0.520, 77.0, 0.958, 54.4, 43.7, 0.0, 41.4, 0.0, 0.0, 0, 0.451, 11.79, 9.47, 0.00, 0.00},
		{0.521, 76.9, 1.000, 46.3, 40.6, 0.0, 38.3, 0.0, 0.0, 0, 0.533, 10.04, 8.80, 0.00, 0.00},
	};

	// MQ65 Black, blending Basic -> Minimum (GameDB forces the download mode, so it is the only lever)
	const TraceWindow kBlackBlending[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.539, 68.2, 0.682, 77.7, 94.6, 0.0, 37.4, 0.0, 71.8, 1, 0.195, 19.02, 23.15, 0.00, 35.14},
		{0.544, 67.5, 0.955, 77.5, 94.0, 0.0, 37.0, 0.0, 71.3, 1, 0.201, 19.15, 23.23, 0.00, 35.22},
		{0.536, 68.4, 0.818, 77.6, 94.5, 0.0, 36.9, 0.0, 71.8, 1, 0.188, 18.91, 23.03, 0.00, 35.02},
		{0.545, 67.3, 0.636, 75.4, 93.0, 0.0, 35.8, 0.0, 70.6, 1, 0.218, 18.70, 23.07, 0.00, 35.00},
		{0.510, 65.5, 0.850, 76.0, 90.4, 0.0, 34.5, 0.0, 68.9, 1, 0.214, 19.35, 23.04, 0.00, 35.12},
		{0.539, 68.0, 0.864, 78.6, 94.1, 0.0, 35.6, 0.0, 71.4, 1, 0.183, 19.27, 23.07, 0.00, 35.03},
		{0.503, 66.4, 0.850, 73.6, 90.6, 0.0, 33.9, 0.0, 69.4, 1, 0.238, 18.51, 22.76, 0.00, 34.90},
		{0.534, 68.8, 0.773, 76.7, 93.8, 0.0, 34.8, 0.0, 72.0, 1, 0.198, 18.60, 22.75, 0.00, 34.92},  // trial starts
		{0.955, 7.0, 0.750, 7.8, 94.2, 0.0, 3.1, 0.0, 7.3, 1, 0.917, 18.74, 225.04, 0.00, 34.94},
		{0.506, 72.5, 0.727, 82.9, 93.6, 0.0, 38.0, 0.0, 65.5, 1, 0.138, 19.09, 21.54, 0.00, 30.18},
		{0.501, 69.9, 0.500, 88.6, 89.2, 0.0, 34.4, 0.0, 59.2, 1, 0.043, 21.15, 21.30, 0.00, 29.66},
		{0.510, 68.7, 0.818, 80.6, 93.4, 0.0, 37.6, 0.0, 63.0, 1, 0.167, 19.56, 22.69, 0.00, 29.23},
		{0.512, 71.7, 0.636, 80.8, 92.3, 0.0, 37.1, 0.0, 62.5, 1, 0.164, 18.81, 21.49, 0.00, 29.10},
		{0.508, 72.2, 0.682, 82.9, 93.0, 0.0, 37.1, 0.0, 63.0, 1, 0.140, 19.16, 21.47, 0.00, 29.11},
		{0.511, 71.9, 0.682, 82.9, 92.2, 0.0, 36.8, 0.0, 62.3, 1, 0.108, 19.25, 21.39, 0.00, 28.94},
		{0.504, 72.9, 0.682, 85.7, 94.5, 0.0, 37.4, 0.0, 63.9, 1, 0.106, 19.62, 21.62, 0.00, 29.24},
		{0.542, 73.9, 0.583, 82.4, 94.3, 0.0, 37.5, 0.0, 64.4, 1, 0.139, 18.61, 21.30, 0.00, 29.10},
		{0.544, 73.6, 0.833, 79.4, 92.6, 0.0, 37.1, 0.0, 63.8, 1, 0.175, 18.00, 21.00, 0.00, 28.92},  // traced: keep
		{0.529, 63.1, 0.600, 67.9, 80.0, 0.0, 30.5, 0.0, 54.1, 1, 0.298, 17.94, 21.13, 0.00, 28.58},
		{0.521, 70.4, 0.636, 80.1, 93.2, 0.0, 36.0, 0.0, 73.5, 1, 0.173, 18.98, 22.08, 0.00, 34.80},
		{0.518, 70.8, 0.727, 80.6, 92.1, 0.0, 36.6, 0.0, 61.9, 1, 0.163, 18.99, 21.69, 0.00, 29.17},
		{0.506, 72.5, 0.727, 84.0, 93.9, 0.0, 37.1, 0.0, 63.4, 1, 0.120, 19.33, 21.60, 0.00, 29.17},
		{0.546, 73.3, 0.750, 81.3, 93.3, 0.0, 36.7, 0.0, 63.6, 1, 0.158, 18.49, 21.23, 0.00, 28.94},
		{0.542, 73.9, 0.458, 85.5, 94.2, 0.0, 36.7, 0.0, 64.2, 1, 0.087, 19.31, 21.27, 0.00, 29.01},
		{0.536, 74.7, 0.667, 79.3, 94.0, 0.0, 37.3, 0.0, 65.0, 1, 0.179, 17.71, 20.99, 0.00, 29.04},
		{0.536, 74.7, 0.833, 79.6, 93.9, 0.0, 37.6, 0.0, 65.0, 1, 0.176, 17.78, 20.98, 0.00, 29.02},
		{0.501, 73.3, 0.818, 79.2, 92.6, 0.0, 36.9, 0.0, 63.9, 1, 0.184, 18.04, 21.08, 0.00, 29.11},
		{0.542, 73.9, 0.708, 79.2, 93.7, 0.0, 37.4, 0.0, 64.3, 1, 0.170, 17.89, 21.17, 0.00, 29.04},  // traced: exhausted
		{0.543, 73.7, 0.792, 78.4, 93.5, 0.0, 37.2, 0.0, 64.1, 1, 0.189, 17.74, 21.16, 0.00, 29.02},
		{0.538, 74.4, 0.708, 78.3, 94.1, 0.0, 37.3, 0.0, 64.8, 1, 0.190, 17.54, 21.10, 0.00, 29.07},
	};

	// MQ65 OutRun 2006, car select: download mode Accurate -> Asynchronous
	const TraceWindow kOutRunDownloadMode[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.503, 71.5, 1.000, 34.9, 63.1, 0.0, 13.7, 0.0, 42.2, 1, 0.629, 9.76, 17.64, 0.00, 11.79},
		{0.500, 71.9, 1.000, 34.8, 62.5, 0.0, 13.2, 0.0, 42.3, 1, 0.630, 9.69, 17.38, 0.00, 11.75},
		{0.509, 70.7, 1.000, 34.5, 61.2, 0.0, 12.9, 0.0, 41.5, 1, 0.619, 9.77, 17.31, 0.00, 11.75},
		{0.513, 70.2, 1.000, 35.0, 61.7, 0.0, 13.2, 0.0, 41.1, 1, 0.609, 9.98, 17.59, 0.00, 11.71},
		{0.507, 71.0, 1.000, 34.9, 61.2, 0.0, 13.1, 0.0, 41.5, 1, 0.617, 9.82, 17.24, 0.00, 11.67},
		{0.508, 70.9, 1.000, 35.0, 61.2, 0.0, 12.9, 0.0, 41.4, 1, 0.603, 9.87, 17.27, 0.00, 11.69},
		{0.513, 70.1, 1.000, 34.9, 60.2, 0.0, 12.9, 0.0, 40.8, 1, 0.599, 9.95, 17.16, 0.00, 11.64},
		{0.521, 69.1, 1.000, 34.1, 60.0, 0.0, 12.6, 0.0, 40.6, 1, 0.631, 9.87, 17.39, 0.00, 11.74},  // trial starts
		{0.501, 91.8, 0.042, 58.2, 87.5, 0.0, 16.0, 0.0, 52.4, 1, 0.064, 12.69, 19.07, 0.00, 11.43},
		{0.508, 102.5, 0.000, 61.9, 82.8, 0.0, 17.2, 0.0, 61.4, 1, 0.000, 12.08, 16.16, 0.00, 11.99},
		{0.500, 99.9, 0.000, 62.0, 79.8, 0.0, 18.1, 0.0, 59.2, 1, 0.000, 12.41, 15.96, 0.00, 11.85},
		{0.503, 99.4, 0.000, 60.4, 77.7, 0.0, 17.3, 0.0, 57.8, 1, 0.000, 12.14, 15.62, 0.00, 11.63},
		{0.517, 100.5, 0.000, 62.1, 80.2, 0.0, 17.6, 0.0, 59.3, 1, 0.000, 12.36, 15.95, 0.00, 11.79},
		{0.519, 100.1, 0.000, 60.0, 78.6, 0.0, 16.9, 0.0, 59.4, 1, 0.000, 11.98, 15.69, 0.00, 11.86},
		{0.500, 99.9, 0.000, 61.6, 80.0, 0.0, 17.5, 0.0, 60.1, 1, 0.000, 12.32, 16.01, 0.00, 12.03},
		{0.501, 99.8, 0.000, 61.3, 79.0, 0.0, 17.4, 0.0, 60.0, 1, 0.000, 12.29, 15.82, 0.00, 12.01},
		{0.519, 100.2, 0.000, 61.8, 79.7, 0.0, 17.2, 0.0, 60.0, 1, 0.000, 12.34, 15.90, 0.00, 11.97},
		{0.500, 99.9, 0.000, 61.7, 79.8, 0.0, 17.7, 0.0, 59.8, 1, 0.000, 12.35, 15.97, 0.00, 11.96},  // traced: keep
		{0.501, 99.9, 0.000, 61.1, 78.8, 0.0, 17.0, 0.0, 59.5, 1, 0.001, 12.24, 15.78, 0.00, 11.91},
		{0.503, 99.4, 0.000, 61.8, 80.0, 0.0, 17.2, 0.0, 59.7, 1, 0.000, 12.43, 16.11, 0.00, 12.02},
		{0.516, 100.8, 0.000, 60.7, 80.3, 0.0, 16.5, 0.0, 60.0, 1, 0.000, 12.05, 15.95, 0.00, 11.91},
		{0.503, 99.3, 0.000, 61.1, 77.5, 0.0, 17.3, 0.0, 58.0, 1, 0.000, 12.30, 15.60, 0.00, 11.67},
		{0.517, 100.6, 0.000, 60.3, 78.8, 0.0, 16.2, 0.0, 58.4, 1, 0.000, 11.99, 15.67, 0.00, 11.61},
		{0.503, 99.4, 0.000, 60.9, 78.3, 0.0, 17.6, 0.0, 58.7, 1, 0.000, 12.26, 15.75, 0.00, 11.81},
	};

	// RG477V Spider-Man 3 after a relaunch: from the start of the session (warm-up included) to 6 s after the second message
	const TraceWindow kSm3Relaunch[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.516, 64.7, 0.200, 44.8, 2.9, 0.4, 0.1, 0.0, 0.0, 0, 0.000, 11.57, 0.75, 0.10, 0.00},
		{0.502, 99.6, 0.033, 23.8, 3.1, 0.6, 0.0, 0.0, 0.0, 0, 0.004, 3.98, 0.53, 0.11, 0.00},
		{0.516, 16.2, 0.857, 54.5, 10.7, 65.0, 20.8, 0.0, 0.0, 0, 0.683, 56.29, 11.05, 67.04, 0.00},
		{0.501, 53.3, 0.938, 31.2, 27.0, 66.7, 27.0, 0.0, 0.0, 0, 0.645, 9.78, 8.45, 20.88, 0.00},
		{0.512, 75.0, 0.870, 43.6, 35.7, 90.5, 29.4, 0.0, 0.0, 0, 0.562, 9.70, 7.93, 20.14, 0.00},
		{0.519, 74.0, 0.913, 41.2, 37.0, 92.1, 29.9, 0.0, 0.0, 0, 0.587, 9.29, 8.35, 20.77, 0.00},
		{0.513, 74.7, 0.870, 44.5, 36.2, 91.9, 30.0, 0.0, 0.0, 0, 0.548, 9.94, 8.07, 20.52, 0.00},
		{0.508, 75.6, 0.826, 43.5, 36.4, 90.9, 30.4, 0.0, 0.0, 0, 0.549, 9.61, 8.03, 20.07, 0.00},
		{0.505, 72.7, 0.955, 41.3, 36.1, 91.5, 30.3, 0.0, 0.0, 0, 0.584, 9.48, 8.28, 21.00, 0.00},
		{0.511, 75.1, 0.870, 43.2, 36.3, 91.4, 30.0, 0.0, 0.0, 0, 0.562, 9.59, 8.06, 20.30, 0.00},
		{0.507, 72.4, 0.909, 40.6, 36.2, 92.4, 29.9, 0.0, 0.0, 0, 0.593, 9.36, 8.34, 21.28, 0.00},
		{0.514, 74.7, 0.826, 42.4, 36.8, 92.1, 30.2, 0.0, 0.0, 0, 0.572, 9.47, 8.23, 20.57, 0.00},
		{0.503, 72.9, 0.955, 41.3, 36.3, 92.8, 29.3, 0.0, 0.0, 0, 0.584, 9.44, 8.31, 21.24, 0.00},
		{0.506, 72.5, 0.909, 41.0, 35.0, 89.0, 29.0, 0.0, 0.0, 0, 0.589, 9.44, 8.06, 20.48, 0.00},
		{0.511, 71.9, 0.864, 42.0, 36.6, 92.2, 29.7, 0.0, 0.0, 0, 0.565, 9.76, 8.49, 21.40, 0.00},
		{0.519, 70.7, 0.955, 40.5, 36.0, 90.3, 29.2, 0.0, 0.0, 0, 0.593, 9.55, 8.50, 21.31, 0.00},
		{0.523, 73.4, 0.870, 41.5, 35.6, 90.0, 29.2, 0.0, 0.0, 0, 0.583, 9.42, 8.08, 20.45, 0.00},
		{0.510, 71.9, 0.909, 41.6, 36.7, 93.3, 30.6, 0.0, 0.0, 0, 0.581, 9.66, 8.51, 21.63, 0.00},
		{0.501, 73.3, 0.864, 40.8, 36.6, 91.4, 28.6, 0.0, 0.0, 0, 0.591, 9.29, 8.34, 20.81, 0.00},
		{0.504, 72.8, 0.955, 41.5, 36.8, 93.7, 30.1, 0.0, 0.0, 0, 0.584, 9.52, 8.44, 21.48, 0.00},
		{0.520, 70.6, 0.955, 41.3, 36.8, 91.2, 30.2, 0.0, 0.0, 0, 0.587, 9.75, 8.69, 21.54, 0.00},
		{0.514, 74.6, 0.870, 42.3, 36.0, 90.7, 29.2, 0.0, 0.0, 0, 0.570, 9.46, 8.06, 20.30, 0.00},
		{0.513, 71.6, 0.909, 40.5, 36.0, 91.8, 29.4, 0.0, 0.0, 0, 0.593, 9.45, 8.39, 21.40, 0.00},
		{0.515, 71.2, 0.955, 39.5, 35.8, 91.7, 29.3, 0.0, 0.0, 0, 0.603, 9.26, 8.39, 21.49, 0.00},
		{0.508, 75.5, 0.826, 43.3, 35.8, 91.1, 29.7, 0.0, 0.0, 0, 0.542, 9.57, 7.92, 20.13, 0.00},  // traced: exhausted
		{0.515, 71.2, 0.909, 41.2, 36.3, 92.4, 30.5, 0.0, 0.0, 0, 0.587, 9.64, 8.50, 21.64, 0.00},
		{0.508, 72.2, 0.955, 40.4, 36.9, 93.5, 30.0, 0.0, 0.0, 0, 0.596, 9.33, 8.52, 21.60, 0.00},
		{0.528, 63.2, 0.950, 37.7, 33.1, 85.0, 27.3, 0.0, 0.0, 0, 0.618, 9.93, 8.73, 22.42, 0.00},
		{0.520, 64.1, 0.900, 39.5, 34.1, 84.7, 26.7, 0.0, 0.0, 0, 0.593, 10.28, 8.86, 22.03, 0.00},  // traced: exhausted
		{0.512, 71.7, 0.955, 40.8, 37.3, 93.2, 29.5, 0.0, 0.0, 0, 0.588, 9.49, 8.68, 21.68, 0.00},
		{0.510, 68.7, 0.952, 39.3, 35.9, 89.8, 28.8, 0.0, 0.0, 0, 0.607, 9.53, 8.70, 21.79, 0.00},
		{0.521, 70.5, 0.955, 40.4, 35.9, 92.5, 28.5, 0.0, 0.0, 0, 0.589, 9.56, 8.48, 21.88, 0.00},
		{0.506, 69.2, 0.952, 40.8, 35.5, 90.8, 28.9, 0.0, 0.0, 0, 0.590, 9.84, 8.57, 21.90, 0.00},
		{0.514, 71.4, 0.955, 40.7, 36.8, 94.1, 29.6, 0.0, 0.0, 0, 0.589, 9.51, 8.60, 21.99, 0.00},
		{0.504, 72.9, 0.864, 42.1, 35.9, 90.2, 28.5, 0.0, 0.0, 0, 0.557, 9.64, 8.22, 20.65, 0.00},
		{0.502, 69.8, 0.857, 39.7, 35.9, 91.9, 28.1, 0.0, 0.0, 0, 0.600, 9.49, 8.57, 21.96, 0.00},
		{0.515, 71.3, 0.818, 40.3, 36.6, 94.1, 29.6, 0.0, 0.0, 0, 0.556, 9.43, 8.57, 22.03, 0.00},
	};

	// The tables below were recorded under the default policy, so their tests build the rig with
	// TraceRig rig{AutoTunePolicy()}.
	// MQ65 God of War II combat, GS back thread, second device round
	const TraceWindow kGow2GsbR2[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.527, 38.0, 1.000, 90.9, 91.1, 0.0, 77.1, 0.0, 44.4, 1, 0.064, 39.91, 39.96, 0.00, 19.47},
		{0.506, 39.5, 1.000, 89.7, 96.7, 0.0, 78.0, 0.0, 46.7, 1, 0.054, 37.85, 40.80, 0.00, 19.71},
		{0.506, 39.5, 1.000, 88.9, 96.6, 0.0, 76.6, 0.0, 46.1, 1, 0.059, 37.50, 40.75, 0.00, 19.47},
		{0.521, 38.4, 1.000, 85.9, 93.5, 0.0, 81.8, 0.0, 45.4, 1, 0.128, 37.32, 40.63, 0.00, 19.73},
		{0.502, 39.9, 1.000, 88.5, 96.5, 0.0, 78.0, 0.0, 46.4, 1, 0.099, 36.99, 40.34, 0.00, 19.41},
		{0.540, 40.1, 1.000, 89.2, 96.4, 0.0, 79.4, 0.0, 44.0, 1, 0.057, 37.07, 40.06, 0.00, 18.28},
		{0.503, 39.8, 1.000, 88.0, 95.3, 0.0, 77.0, 0.0, 47.3, 1, 0.094, 36.88, 39.93, 0.00, 19.82},
		{0.535, 40.5, 1.000, 89.6, 96.7, 0.0, 75.6, 0.0, 51.8, 1, 0.094, 36.90, 39.81, 0.00, 21.34},  // trial starts
		{1.907, 2.6, 1.000, 5.3, 80.0, 0.0, 5.2, 0.0, 2.0, 1, 0.865, 33.69, 508.93, 0.09, 12.93},
		{0.516, 16.2, 1.000, 72.6, 76.4, 38.8, 26.7, 0.0, 20.4, 1, 0.300, 74.86, 78.75, 40.00, 21.03},
		{0.544, 30.7, 1.000, 72.3, 87.7, 38.3, 57.9, 0.0, 44.9, 1, 0.263, 39.32, 47.73, 20.85, 24.41},
		{0.536, 34.2, 1.000, 85.2, 88.3, 44.2, 68.3, 0.0, 46.7, 1, 0.121, 41.55, 43.03, 21.53, 22.75},
		{0.521, 35.2, 1.000, 88.8, 84.7, 46.8, 69.7, 0.0, 47.2, 1, 0.104, 42.06, 40.14, 22.16, 22.38},
		{0.532, 37.6, 1.000, 89.8, 85.5, 52.7, 76.7, 0.0, 42.5, 1, 0.057, 39.83, 37.94, 23.36, 18.86},
		{0.515, 38.9, 1.000, 94.1, 83.8, 54.8, 78.7, 0.0, 55.9, 1, 0.031, 40.37, 35.94, 23.53, 23.99},
		{0.507, 39.5, 1.000, 94.9, 85.0, 55.4, 81.2, 0.0, 48.1, 1, 0.000, 40.14, 35.92, 23.44, 20.35},
		{0.510, 39.3, 1.000, 97.0, 85.7, 55.6, 81.7, 0.0, 48.6, 1, 0.000, 41.19, 36.39, 23.61, 20.63},
		{0.536, 40.5, 1.000, 94.5, 83.1, 59.8, 85.9, 0.0, 53.7, 1, 0.000, 38.93, 34.23, 24.64, 22.15},
		{0.517, 38.7, 1.000, 93.8, 84.8, 56.8, 81.5, 0.0, 47.1, 1, 0.000, 40.39, 36.51, 24.47, 20.27},
		{0.516, 38.8, 1.000, 97.6, 80.8, 57.1, 85.8, 0.0, 43.7, 1, 0.000, 41.92, 34.69, 24.52, 18.77},
		{0.505, 39.7, 1.000, 96.9, 83.0, 55.6, 83.3, 0.0, 58.1, 1, 0.000, 40.76, 34.91, 23.38, 24.44},
		{0.501, 40.0, 1.000, 97.0, 79.5, 57.2, 84.1, 0.0, 49.7, 1, 0.000, 40.45, 33.16, 23.87, 20.73},
		{0.511, 39.2, 1.000, 91.5, 83.3, 55.6, 79.4, 0.0, 48.1, 1, 0.000, 38.97, 35.46, 23.68, 20.48},
		{0.506, 39.6, 1.000, 95.2, 82.3, 56.5, 83.0, 0.0, 49.1, 1, 0.000, 40.11, 34.68, 23.81, 20.71},
		{0.509, 39.3, 1.000, 94.6, 80.0, 55.9, 81.3, 0.0, 48.6, 1, 0.000, 40.10, 33.91, 23.72, 20.59},  // traced: keep
		{0.512, 39.1, 1.000, 97.3, 77.6, 54.9, 83.8, 0.0, 48.8, 1, 0.000, 41.54, 33.12, 23.41, 20.84},
		{0.500, 40.0, 1.000, 96.1, 81.3, 57.4, 82.3, 0.0, 49.7, 1, 0.000, 40.08, 33.90, 23.94, 20.71},
		{0.511, 39.2, 1.000, 95.8, 81.3, 57.2, 81.6, 0.0, 48.7, 1, 0.000, 40.78, 34.61, 24.35, 20.76},
		{0.510, 39.3, 1.000, 97.2, 83.8, 59.3, 84.2, 0.0, 44.7, 1, 0.000, 41.31, 35.59, 25.21, 18.99},
		{0.513, 39.0, 1.000, 96.3, 83.9, 57.3, 83.1, 0.0, 47.5, 1, 0.000, 41.15, 35.83, 24.47, 20.31},
	};

	// MQ65 R&C UYA combat, GS back thread, second device round
	const TraceWindow kUyaGsbR2[] = {
		// seconds speed late   ee  mtgs  gsb mtvu  iop  gpu tmr wait_gs  ee_ms  gs_ms gsb_ms gpu_ms
		{0.517, 45.2, 1.000, 71.4, 93.3, 0.0, 28.4, 0.0, 50.1, 1, 0.263, 26.39, 34.47, 0.00, 18.50},
		{0.501, 46.6, 1.000, 73.8, 96.1, 0.0, 29.6, 0.0, 51.0, 1, 0.236, 26.43, 34.42, 0.00, 18.28},
		{0.505, 46.3, 1.000, 73.5, 95.3, 0.0, 29.3, 0.0, 50.7, 1, 0.237, 26.51, 34.36, 0.00, 18.30},
		{0.522, 44.7, 1.000, 71.2, 92.8, 0.0, 28.2, 0.0, 50.0, 1, 0.270, 26.54, 34.61, 0.00, 18.64},
		{0.506, 46.2, 1.000, 72.7, 94.2, 0.0, 29.5, 0.0, 50.7, 1, 0.251, 26.26, 34.04, 0.00, 18.31},
		{0.520, 44.9, 1.000, 70.9, 91.8, 0.0, 28.3, 0.0, 49.1, 1, 0.272, 26.36, 34.13, 0.00, 18.24},
		{0.500, 46.7, 1.000, 73.4, 94.8, 0.0, 29.4, 0.0, 50.9, 1, 0.247, 26.23, 33.89, 0.00, 18.20},
		{0.525, 47.7, 1.000, 74.1, 96.0, 0.0, 28.5, 0.0, 52.7, 1, 0.233, 25.92, 33.60, 0.00, 18.42},  // trial starts
		{1.920, 2.6, 1.000, 4.4, 80.9, 0.0, 2.4, 0.0, 2.0, 1, 0.922, 28.26, 517.57, 0.10, 12.88},
		{0.517, 19.4, 1.000, 31.5, 50.3, 66.0, 11.3, 0.0, 19.7, 1, 0.681, 27.09, 43.31, 56.88, 16.93},
		{0.518, 45.1, 1.000, 77.6, 64.5, 80.2, 32.2, 0.0, 59.6, 1, 0.201, 28.74, 23.88, 29.69, 22.06},
		{0.512, 52.1, 1.000, 92.7, 69.8, 80.4, 38.6, 0.0, 60.0, 1, 0.031, 29.66, 22.35, 25.73, 19.21},
		{0.501, 49.9, 1.000, 88.1, 69.7, 80.0, 37.5, 0.0, 58.6, 1, 0.098, 29.44, 23.28, 26.74, 19.58},
		{0.519, 51.4, 1.000, 92.7, 69.7, 79.6, 37.9, 0.0, 59.2, 1, 0.050, 30.06, 22.60, 25.81, 19.20},
		{0.507, 52.6, 1.000, 91.6, 71.6, 81.9, 39.1, 0.0, 60.5, 1, 0.010, 29.06, 22.71, 25.97, 19.18},
		{0.531, 50.2, 1.000, 91.9, 70.0, 78.1, 35.8, 0.0, 57.4, 1, 0.046, 30.54, 23.25, 25.94, 19.08},
		{0.523, 51.1, 1.000, 92.0, 71.0, 81.4, 37.8, 0.0, 59.6, 1, 0.020, 30.05, 23.20, 26.58, 19.47},
		{0.524, 51.0, 1.000, 88.1, 71.0, 80.5, 37.9, 0.0, 58.5, 1, 0.059, 28.84, 23.26, 26.37, 19.17},
		{0.525, 50.8, 1.000, 93.3, 70.8, 80.5, 36.7, 0.0, 58.3, 1, 0.008, 30.63, 23.24, 26.42, 19.14},
		{0.522, 51.1, 1.000, 86.2, 71.3, 82.8, 37.1, 0.0, 60.2, 1, 0.064, 28.14, 23.28, 27.03, 19.67},
		{0.528, 50.6, 1.000, 92.9, 69.0, 79.4, 36.6, 0.0, 57.8, 1, 0.047, 30.66, 22.77, 26.18, 19.08},  // traced: keep
		{0.510, 52.3, 1.000, 83.9, 70.8, 79.7, 40.8, 0.0, 59.6, 1, 0.015, 26.75, 22.59, 25.41, 19.02},
	};

} // namespace

TEST(AutoTuneTrace, TheGow2BackThreadIsNotKeptOnItsCostAlone)
{
	// The GS thread's cost falls because the work is split over two threads, and the speed falls with it,
	// within the noise of the first four windows (two hitch windows make it wide).
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

	// With eight windows the speed is under the baseline by more than the noise: it made things worse, and
	// that is remembered. The first four windows could not say, so the trial is extended once.
	EXPECT_TRUE(revert->measured);
	EXPECT_FALSE(revert->inconclusive);
	EXPECT_NEAR(revert->gain, -0.058, 0.01);
	EXPECT_EQ(rig.At(revert) - rig.At(start), 9u + 4u + 4u);
}

TEST(AutoTuneTrace, TheUyaBackThreadIsKeptAfterALongerLook)
{
	// The same lever on R&C: the GS cost falls, and the first four windows show a slightly lower speed because of one
	// hitch window. The first look cannot tell, so it looks again, and again: a lower cost does not keep the lever,
	// the speed has to rise. With twelve windows it has.
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

	// What the player is told is the speed that was measured, not the amount the GS thread's cost fell by.
	EXPECT_NEAR(keep->gain, keep->speed_after / keep->speed_before - 1.0, 1e-9);
	EXPECT_NEAR(keep->gain, 0.116, 0.02);
	EXPECT_GE(keep->gain, rig.ctl.GetPolicy().gain_fraction);

	// The first look is four windows after the 9 of the settle. At the second, eight windows in, the speed is up
	// but not beyond the noise, which two hitch windows make wide; it keeps on the third.
	EXPECT_GE(rig.At(keep) - rig.At(start), 9u + 4u + 4u + 4u);
}

TEST(AutoTuneTrace, TheGow2BackThreadOfTheSecondRoundIsRejectedAlthoughTheGsCostFell)
{
	// Clean windows (no renderer-reopen hitch in the measurement): the speed stays flat while the GS cost falls,
	// because the work is split over two threads. The EE, nearly saturated, is the limit before and after. A lower
	// cost at the same speed must not be kept, or the lever puts a big core to work for nothing.
	TraceRig rig{AutoTunePolicy()};
	rig.state.gs_back_thread_allowed = true;
	rig.WarmUp();
	rig.Feed(kGow2GsbR2);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::GsBackThread);

	EXPECT_EQ(rig.Count(Kind::Keep), 0u) << "a lower GS cost with the same speed is not a keep";
	const AutoTuneAction* revert = rig.First(Kind::Revert);
	ASSERT_NE(revert, nullptr);
	EXPECT_EQ(revert->lever, AutoTuneLever::GsBackThread);
	EXPECT_TRUE(revert->measured) << "the speed is clearly flat: it did not help, and that is remembered";
	EXPECT_FALSE(revert->inconclusive);
	EXPECT_LT(std::fabs(revert->gain), 0.02);
	EXPECT_GT(revert->cost_gain, 0.10) << "the cost did fall, which is why it was looked at for longer";
}

TEST(AutoTuneTrace, TheUyaBackThreadOfTheSecondRoundIsKeptOnItsSpeed)
{
	// The lever that is worth keeping: the GS cost falls and the speed rises by about 10%.
	TraceRig rig{AutoTunePolicy()};
	rig.state.gs_back_thread_allowed = true;
	rig.WarmUp();
	rig.Feed(kUyaGsbR2);

	const AutoTuneAction* keep = rig.First(Kind::Keep);
	ASSERT_NE(keep, nullptr);
	EXPECT_EQ(keep->lever, AutoTuneLever::GsBackThread);
	EXPECT_TRUE(keep->measured);
	EXPECT_NEAR(keep->gain, 0.10, 0.02);
	EXPECT_GT(keep->cost_gain, 0.15);
}

TEST(AutoTuneTrace, ABackThreadThatCostAQuarterOfTheSpeedIsRememberedAsRejectedAtOnce)
{
	// Shadow of the Colossus on the MQ65 with the IOP thread on: the GS back thread has nowhere to run, so the
	// speed drops by about a quarter while the GS thread's cost falls. That is a clear loss: it has to be
	// remembered as a rejection at the first look, not extended and undone as "inconclusive", which is not remembered.
	TraceRig rig;
	rig.state.gs_back_thread_allowed = true;
	rig.state.iop_thread_on = true;
	rig.WarmUp();
	rig.Feed(kSotcGsb);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::GsBackThread);

	const AutoTuneAction* revert = rig.First(Kind::Revert);
	ASSERT_NE(revert, nullptr);
	EXPECT_EQ(revert->lever, AutoTuneLever::GsBackThread);
	EXPECT_TRUE(revert->measured) << "a clear loss is a rejection, and is remembered";
	EXPECT_FALSE(revert->inconclusive);
	EXPECT_LT(revert->gain, -0.25);
	EXPECT_GT(revert->cost_gain, 0.05) << "the cost did fall, which is what made the old rule call it unclear";

	// Cut short at the first look: the 10 windows of the settle and the four windows measured, no extension.
	EXPECT_EQ(rig.At(revert) - rig.At(start), 10u + 4u);
}

TEST(AutoTuneTrace, TheGameGettingHeavierDuringATrialIsNotARememberedRejection)
{
	// Shadow of the Colossus on the RG477V at 5x, 86% speed. Resolution 5x -> 4x runs at 100% for the settle
	// and the first windows, and then the game gets heavier: EE time per frame rises from 10.5 to 13.5 ms and the
	// speed drops to 73 ... 77%. Against the 86% baseline that reads as a loss, with the windows in two tight
	// lumps (so the median-based noise is small), but the loss is the scene, not the step. It must not be
	// written to the game's file as "resolution 4x did not help".
	TraceRig rig;
	rig.state.resolution = 5.0;
	rig.WarmUp();
	rig.Feed(kSotcResolution4);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::Resolution);
	EXPECT_EQ(start->value, 4.0);
	EXPECT_EQ(start->bottleneck, AutoTuneBottleneck::Gpu) << "no GPU timer: the EE blocked on the GS";

	const AutoTuneAction* revert = rig.First(Kind::Revert);
	ASSERT_NE(revert, nullptr);
	EXPECT_EQ(revert->lever, AutoTuneLever::Resolution);
	EXPECT_FALSE(revert->measured) << "not written to the game's file";
	EXPECT_TRUE(revert->inconclusive);
	EXPECT_TRUE(revert->retry) << "to be measured again against a fresh baseline";
}

TEST(AutoTuneTrace, AfterTheDriftTheStepIsMeasuredAgainAgainstAFreshBaselineAndKept)
{
	TraceRig rig;
	rig.state.resolution = 5.0;
	rig.WarmUp();
	rig.Feed(kSotcResolution4);

	// The undone trial is not in the session's list of failures, so the same step is tried again against a
	// baseline taken in the heavy scene (52%).
	ASSERT_EQ(rig.Count(Kind::StartTrial), 2u);
	const AutoTuneAction& again = rig.actions[rig.actions.size() - 1];
	ASSERT_EQ(again.kind, Kind::StartTrial);
	EXPECT_EQ(again.lever, AutoTuneLever::Resolution);
	EXPECT_EQ(again.value, 4.0);
	EXPECT_NEAR(again.speed_before, 52.0, 1.0);

	// The trace has no 4x windows of the heavy scene beyond the ones it measured; those are what the retry sees.
	for (int i = 0; i < 20; i++)
		rig.Feed(kSotcHeavyAt4x[static_cast<size_t>(i) % (sizeof(kSotcHeavyAt4x) / sizeof(kSotcHeavyAt4x[0]))]);
	const AutoTuneAction* keep = rig.First(Kind::Keep);
	ASSERT_NE(keep, nullptr) << "77% against 52% is a clear gain";
	EXPECT_EQ(keep->lever, AutoTuneLever::Resolution);
	EXPECT_GT(keep->gain, 0.40);
	EXPECT_EQ(rig.Count(Kind::Revert), 1u);
}

TEST(AutoTuneTrace, BasicToMinimumIsStillTriedAndKeptWhereTheGsThreadIsTheLimit)
{
	// Black on the MQ65: the GS thread is the limit (MTGS at 94%). Minimum blending cuts the GS thread's time and
	// the speed rises.
	TraceRig rig;
	rig.state.blending_level = 1;
	rig.WarmUp();
	rig.Feed(kBlackBlending);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->lever, AutoTuneLever::Blending);
	EXPECT_EQ(start->value, 0.0);
	EXPECT_EQ(start->bottleneck, AutoTuneBottleneck::GsCpu);

	const AutoTuneAction* keep = rig.First(Kind::Keep);
	ASSERT_NE(keep, nullptr);
	EXPECT_EQ(keep->lever, AutoTuneLever::Blending);
	EXPECT_GT(keep->gain, 0.05);
}

TEST(AutoTuneTrace, AReadbackStallGetsTheDownloadModeBeforeTheBackThread)
{
	// OutRun 2006 on the MQ65: the EE waits on the GS 60% of the time while MTGS is at 61% and the GPU at 41%.
	// That reads as the GPU, whose first lever at native resolution would be the GS back thread, but a readback
	// stall is cured by the download mode (Asynchronous), not by a second GS thread. The download mode goes first.
	TraceRig rig;
	rig.state.gs_back_thread_allowed = true;
	rig.state.download_mode = AutoTuneDownloadMode::Accurate;
	rig.state.blending_level = 1;
	rig.WarmUp();
	rig.Feed(kOutRunDownloadMode);

	const AutoTuneAction* start = rig.First(Kind::StartTrial);
	ASSERT_NE(start, nullptr);
	EXPECT_EQ(start->bottleneck, AutoTuneBottleneck::Gpu);
	EXPECT_EQ(start->lever, AutoTuneLever::DownloadMode);
	EXPECT_EQ(start->value, static_cast<double>(AutoTuneDownloadMode::Asynchronous));

	const AutoTuneAction* keep = rig.First(Kind::Keep);
	ASSERT_NE(keep, nullptr);
	EXPECT_GT(keep->gain, 0.30);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 1u) << "and nothing else had to be tried first";
}

TEST(AutoTuneTrace, NothingLeftToTryIsSaidOnceEvenWhenTheClassFlipsNearTheSaturationLine)
{
	// Spider-Man 3 on the RG477V: the back thread is on, the download mode and the blending are known not to
	// help, so there is nothing to try. The back thread sits at 91-94% of a core against the 90% line; two
	// windows at 85% pull the four-window mean under it and the class flips from GS (CPU) to GPU. The "nothing
	// left to try" message must still be said once, not again after the flip.
	// The warm-up is 10.4 s so that it ends at the 21st window of the trace, as it did in the recorded session.
	AutoTunePolicy policy = RoundOnePolicy();
	policy.warmup_seconds = 10.4;
	TraceRig rig(policy);
	rig.state.gs_back_thread_on = true;
	rig.state.gs_back_thread_allowed = true;
	rig.state.download_mode = AutoTuneDownloadMode::Accurate;
	rig.state.blending_level = 1;
	rig.state.rejected = {{AutoTuneLever::DownloadMode, static_cast<double>(AutoTuneDownloadMode::Asynchronous)}, {AutoTuneLever::Blending, 0.0}};
	rig.Feed(kSm3Relaunch);

	ASSERT_EQ(rig.Count(Kind::Exhausted), 1u);
	EXPECT_EQ(rig.First(Kind::Exhausted)->bottleneck, AutoTuneBottleneck::GsCpu);
	EXPECT_EQ(rig.Count(Kind::StartTrial), 0u);
}
