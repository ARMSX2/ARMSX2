// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// ⚠️ MEASUREMENT OVERRIDE — pin every destination-alpha-test draw to the primitive-ID road.
//
// Not a setting, and it does nothing unless a harness asks for it. Built to measure the
// Adreno in-pass read.
//
// What it is for. The PS2's destination alpha test (DATE) has four emulated roads, and which one
// a draw takes is decided per draw from what the device can do:
//
//   StencilOne      a stencil pre-pass marks the pixels that will pass, then the draw runs masked
//   Stencil         the same, read-only, for a draw the pre-pass cannot mark in one go
//   PrimIDTracking  a full-size R32F image is prefilled from the target and tracks which
//                   primitive last wrote each pixel
//   Full            the draw reads the target's alpha itself, needing an in-pass destination read
//
// The roads are not interchangeable in cost or in what else they drag in. Full needs the in-pass
// read, so on a build that has just been given one -- which is what the declared-feedback-loop
// arm is -- draws that used to take a stencil road move onto Full by themselves. That is a second
// mechanism changing at the same time as the one under test, and Stuntman and Indiana Jones are
// the two dumps where it happens, which is to say the two dumps whose numbers then mean nothing
// in particular.
//
// So this pins the DATE mechanism while the colour road changes: every DATE draw takes
// PrimIDTracking, which is the road both handheld targets take today, on both arms. The arm's
// delta is then about the colour road and nothing else.
//
// It is deliberately blunt -- no per-draw heuristic, no "only the ones that would have changed"
// -- because an instrument that picks its own population is an instrument that has to be argued
// about afterwards.
//
// The existing fallbacks stand. A device with no primitive-ID target still takes the road it
// takes; SCANMSK line-discard draws still refuse the primitive-ID road, because the shader's
// discard and the primitive-ID tracking cannot both be right. Those two are the reason this is a
// function over explicit inputs rather than an assignment at the call site.
//
// It is expected to MOVE PIXELS where it fires, and that is not a defect: the four roads are
// four different approximations of one PS2 rule and they disagree at the edges. Any byte-identity
// gate on this flag is a count, not a pass/fail.
// ---------------------------------------------------------------------------------------------

/// Which DATE road this process takes. Auto is the per-draw decision the renderer already makes.
enum class GSDateRoadOverride : u8
{
	Auto,
	PrimID,
};

struct GSDateRoadInputs
{
	GSDateRoadOverride override_mode = GSDateRoadOverride::Auto;

	/// The draw has a destination alpha test at all. Everything here is about which road it
	/// takes, never about whether it happens.
	bool date_enabled = false;

	/// The draw's road selection already landed on primitive-ID tracking. Nothing to do, and
	/// saying so keeps "did the override fire" honest in a census.
	bool already_primid = false;

	/// -> GSDevice::FeatureSupport::primitive_id. Without it there is no primitive-ID image to
	/// prefill and the existing fallback is the only answer.
	bool device_has_primitive_id = false;

	/// PSSelector::scanmsk bit 1 -- the draw discards alternate lines in the shader. The renderer
	/// already refuses the primitive-ID road for these (the discard and the tracking disagree
	/// about what "this pixel was written" means), and the override does not get to override
	/// that.
	bool scanmsk_discards_lines = false;
};

/// True when this draw's DATE road should be replaced by primitive-ID tracking.
constexpr bool GSDateRoadForcesPrimID(const GSDateRoadInputs& in)
{
	if (in.override_mode != GSDateRoadOverride::PrimID)
		return false;

	if (!in.date_enabled || in.already_primid)
		return false;

	// Both of these are the renderer's own rules, not this override's to break.
	if (!in.device_has_primitive_id || in.scanmsk_discards_lines)
		return false;

	return true;
}

// Auto changes nothing, whatever else is true. This is the inertness statement, and it is the
// same one the 94-cell byte-identity gate makes about the whole binary.
static_assert(!GSDateRoadForcesPrimID({.date_enabled = true, .device_has_primitive_id = true}));
static_assert(!GSDateRoadForcesPrimID({}));

// Asked for, it fires on a draw that would have taken a stencil or a barrier road.
static_assert(GSDateRoadForcesPrimID({.override_mode = GSDateRoadOverride::PrimID,
	.date_enabled = true, .device_has_primitive_id = true}));

// And not on a draw with no destination alpha test, nor on one already on the road.
static_assert(!GSDateRoadForcesPrimID({.override_mode = GSDateRoadOverride::PrimID,
	.device_has_primitive_id = true}));
static_assert(!GSDateRoadForcesPrimID({.override_mode = GSDateRoadOverride::PrimID,
	.date_enabled = true, .already_primid = true, .device_has_primitive_id = true}));

// The two fallbacks the override does not get to break.
static_assert(!GSDateRoadForcesPrimID({.override_mode = GSDateRoadOverride::PrimID,
	.date_enabled = true, .device_has_primitive_id = false}));
static_assert(!GSDateRoadForcesPrimID({.override_mode = GSDateRoadOverride::PrimID,
	.date_enabled = true, .device_has_primitive_id = true, .scanmsk_discards_lines = true}));

namespace GSDateRoadPolicy
{
	/// The road the next draw resolves with.
	///
	/// A process-wide inline global rather than a setting, for the reason every policy override
	/// in this directory is one: which road to take is a measurement result, not a user
	/// preference, and a user cannot tell which of four approximations their title wants. Set
	/// once before the VM starts; read per draw, in GSRendererHW::EmulateDATESelectMethod.
	inline GSDateRoadOverride s_override = GSDateRoadOverride::Auto;

	inline void SetOverride(GSDateRoadOverride value) { s_override = value; }
	inline GSDateRoadOverride GetOverride() { return s_override; }

	/// For the banner. A measurement log quotes this, so it names the configuration rather than the setting.
	inline const char* Name()
	{
		return (s_override == GSDateRoadOverride::PrimID) ? "primid (forced)" : "auto";
	}
} // namespace GSDateRoadPolicy
