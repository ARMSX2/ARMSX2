// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// Should a TFX render pass be opened over the draws it will contain, instead of over the whole
// render target?
//
// ⚠️ TODAY THE ANSWER IS NO, ON EVERY DEVICE, AND THAT IS A MEASUREMENT AND NOT AN OVERSIGHT.
// The mechanism is implemented, gated and byte-identical; what is missing is a GPU that pays for
// render-pass AREA. Read the numbers before turning it on.
//
// The idea. The backend opens the colour pass at the full target and keeps it open across draws.
// On a tile-based renderer that should be a tile LOAD of the render area when the pass begins and
// a tile STORE of it when the pass ends, whatever the draws inside it touch -- so a pass sized to
// the whole target should pay for the whole target. Stuntman at 2x looks like the extreme case:
// every destination-alpha draw makes the backend end the colour pass so it can read the target as
// a texture, and the pass restarts at the full 1280x896 for a draw whose median footprint is 64
// device pixels; 2,863 passes a frame contain nothing but draws the depth test throws away, and
// they are 76% of the frame's counted load-and-store bill.
//
// The cost. A pass sized to its draws cannot hold a draw that reaches outside it -- rendering
// outside renderArea is undefined -- so it has to be ended and reopened when one does, and a pass
// break is real work on any GPU.
//
// The measurements, both of them. Two tilers, opposite vendors, same answer: the area is free and
// the passes are not.
//
//   SD865, Adreno 650 / Turnip (campaign lane Q-indy step 3, 2026-09-19), every pass opened at the
//   draw rect, frame p95:
//
//     indy   2x   tile bill 885.0 -> 163.5 Mpx   +0.59 ms   (passes 1357 -> 1376)
//     wrc3   2x               703.6 ->  27.2     +1.56 ms   (691 -> 1259)
//     nascar 2x                17.6 ->  20.6     +3.09 ms   (18 -> 430)
//     cod2   2x                12.1 ->  20.4     +1.82 ms   (17 -> 562)
//
//   M2 Max, Honeykrisp / Mesa 25.3.6 (lane C15, 2026-09-20), this code, windowed gsrunner so the
//   GPU timer reports, three runs an arm, GPU ms per dump loop:
//
//     stuntman 2x  area 77348 -> 1562 Mpx (-98.0%)   517.12 -> 517.58 ms  (+0.09%)
//     stuntman 4x      253369 -> 3833   (-98.5%)     696.73 -> 696.80     (+0.01%)
//     indy     2x        6079 -> 1881   (-69.0%)     699.64 -> 700.19     (+0.08%)
//     pop-ww   2x        7238 -> 2257   (-68.8%)      32.87 ->  36.20    (+10.2%)
//     wrc3     2x       10476 -> 3551   (-66.1%)      76.46 ->  99.68    (+30.4%)
//     cod2     2x        1111 -> 1544   (grew)         5.38 ->   8.02    (+49.0%)
//
//   The stuntman and indy deltas are inside their own three-run spread, so those two cells say
//   "no measurable change" rather than "slightly slower"; the other three are real by one to two
//   orders of magnitude.
//
// Stuntman at 4x is the cell that settles it: a quarter of a million megapixels of tile traffic
// removed, and the GPU time moves by one part in ten thousand. Whatever these drivers do with a
// render area, they do not load and store it. Turnip renders small passes directly to system
// memory; Honeykrisp has some equivalent, and the mechanism does not matter because the size of
// the effect is nil either way.
//
// What DOES cost, on both: the pass itself. Taking the three M2 cells whose delta clears its own
// spread and dividing GPU time by pass count, the extra passes price out at 27, 30 and 32
// microseconds each (pop-ww, wrc3, cod2), independent of their area. Stuntman opens 845 passes per
// presented frame at 2x, so roughly 25 ms of its ~129 ms frame goes on pass boundaries. The lever
// that moves these GPUs is FEWER render passes, not smaller ones, and
// GSPerfMon::RenderPassAreaPixels ranks titles without pricing them -- as Q-indy said, now
// confirmed on a second and very different tiler.
//
// Mali is the one family where the premise has not been tested. Turning it on there is the
// `in.is_mali` line below plus a measurement; gsrunner's -narrow-render-area forces the road on a
// device whose default is the other answer, which is what that flag exists for.
//
// None of this can move a pixel. The render area decides which pixels a pass MAY touch, not which
// it does; the scissor is clamped into it and the draw rect is padded. Byte identity across the
// 47-dump corpus at 1x and 2x is the gate that says so.
// ---------------------------------------------------------------------------------------------

/// Harness override. Auto is the device decision below; the other two force it, for A/B work on
/// a device whose default answer is the opposite one.
enum class GSRenderAreaOverride : u8
{
	Auto,
	ForceOn,
	ForceOff,
};

/// The device families, filled by the caller. Nothing reads them while every family answers the
/// same way; they are here so that turning one family on is an edit to this file and not to the
/// backend as well.
struct GSRenderAreaInputs
{
	GSRenderAreaOverride override_mode = GSRenderAreaOverride::Auto;
	/// Apple silicon under MoltenVK or Honeykrisp. Measured; does not pay for area.
	bool is_apple = false;
	/// ARM Mali. Not measured -- the one open candidate.
	bool is_mali = false;
	/// Imagination PowerVR. Not measured, no device.
	bool is_powervr = false;
	/// Qualcomm Adreno, proprietary blob or Turnip. Measured; does not pay for area.
	bool is_adreno = false;
};

constexpr bool GSDecideNarrowRenderArea(const GSRenderAreaInputs& in)
{
	if (in.override_mode != GSRenderAreaOverride::Auto)
		return in.override_mode == GSRenderAreaOverride::ForceOn;

	// No family takes this road automatically. The two that were expected to -- Adreno and Apple --
	// were both measured and neither pays for render-pass area; a desktop GPU has nothing to save
	// because a full-target pass is close to free there. Mali turns on by adding `in.is_mali` here,
	// once the RG477V has been asked the same question.
	return false;
}

static_assert(!GSDecideNarrowRenderArea({.is_apple = true}));
static_assert(!GSDecideNarrowRenderArea({.is_adreno = true}));
static_assert(!GSDecideNarrowRenderArea({.is_mali = true}));
static_assert(!GSDecideNarrowRenderArea({.is_powervr = true}));
static_assert(!GSDecideNarrowRenderArea({}));
// The override wins over every device answer, in both directions.
static_assert(GSDecideNarrowRenderArea({.override_mode = GSRenderAreaOverride::ForceOn, .is_adreno = true}));
static_assert(!GSDecideNarrowRenderArea({.override_mode = GSRenderAreaOverride::ForceOff, .is_apple = true}));

namespace GSRenderAreaPolicy
{
	/// The override the next device creation will resolve with.
	///
	/// This exists for the gsrunner, and it deliberately is not a setting: which road a device
	/// should take is a measurement result, not a user preference, and a user has no way to know
	/// which side of the trade their GPU is on. Set once before the VM starts; read when the
	/// device resolves its features. Mirrors GpuProfileDetector::SetForcedBugs, for the same
	/// reason -- without it the road a device does NOT take is unreachable and unmeasurable on
	/// that device, which is exactly the position the RG477V is in today.
	inline GSRenderAreaOverride s_override = GSRenderAreaOverride::Auto;

	inline void SetOverride(GSRenderAreaOverride value) { s_override = value; }
	inline GSRenderAreaOverride GetOverride() { return s_override; }
} // namespace GSRenderAreaPolicy
