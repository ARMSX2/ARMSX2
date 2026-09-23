// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// ---------------------------------------------------------------------------------------------
// ⚠️ MEASUREMENT OVERRIDE — which draws on the declared-feedback-loop road actually declare it.
//
// Not a setting, and it does nothing unless a harness asks for it. Built to measure the
// Adreno in-pass read.
//
// The road. A draw that reads the render target it writes can declare an attachment feedback
// loop and sample the live attachment (GSSelfReadRoadPolicy.h). On Turnip the declaration is the
// ordering: the driver refuses to tile a pass holding a pipeline that declares a texture feedback
// loop, and on the untiled path it programs the coherent primitive mode that makes overlapping
// primitives see each other.
//
// The bill. That coherent mode -- FLUSH_PER_OVERLAP_AND_OVERWRITE -- makes the GPU wait for any
// earlier primitive covering the same sample before running the current one. It is the right
// price for a draw whose primitives overlap, and it is pure loss for a draw whose primitives do
// not, which on this corpus is nearly all of them: Splashdown issues 1,169 render-target readers
// a frame and the declared road measured +188% on it at native.
//
// The scope. A reader whose own primitives do not overlap is served correctly by a once-per-draw
// clone of the target -- that is exactly what a52115e207 relies on, and it is the road the device
// takes today. What a clone cannot serve is a draw that overlaps itself, because every primitive
// after the first composites against colour an earlier one already replaced; that commit had to
// give up accurate blending on those draws for want of an ordered read, and this road is what
// gives it back. So: declare the loop for the draws that need the ordering, and leave the rest on
// the copy road. The serialisation then lands on Ace Combat 5's dozen overlapping sprites instead
// of Splashdown's thousand readers, on precisely the draws no other road renders correctly.
//
// PRIM_OVERLAP_UNKNOWN counts as NOT overlapping here, and that is a deliberate asymmetry with
// DetermineBarriers, which counts UNKNOWN as overlapping because a barrier is cheap insurance.
// The insurance this buys is not cheap, and an UNKNOWN draw is where the copy road already leaves
// it today -- a52115e207 kept UNKNOWN promoted for the same reason. An arm that declared every
// UNKNOWN draw would be most of the blanket arm again and would measure it.
//
// ⚠️ This scopes the DECLARATION, not the CARRY. Where the backend carries the feedback-loop flag
// across the draws that follow a declared one in the same pass (GSFeedbackLoopCarryPolicy.h), the
// pipeline create flag still reaches those draws, and on Turnip that flag is what untiles the
// pass. To confine the primitive mode to the declared draws on such a device this has to be run
// together with the carry override.
// ---------------------------------------------------------------------------------------------

/// Which readers on the declared road declare. All is the road as it stands.
enum class GSDeclaredLoopScope : u8
{
	All,
	OverlapOnly,
};

struct GSDeclaredLoopScopeInputs
{
	GSDeclaredLoopScope scope = GSDeclaredLoopScope::All;

	/// The device is on the declared-feedback-loop road at all. Off it, there is no declaration
	/// to scope and this decides nothing.
	bool declared_road = false;

	/// GSState::PRIM_OVERLAP_YES -- the draw's own primitives are known to overlap each other.
	/// UNKNOWN and NO are both false here; see the asymmetry note above.
	bool prim_overlap_yes = false;
};

/// True when this draw declares the feedback loop it reads through.
constexpr bool GSDrawDeclaresFeedbackLoop(const GSDeclaredLoopScopeInputs& in)
{
	if (!in.declared_road)
		return false;

	if (in.scope == GSDeclaredLoopScope::All)
		return true;

	return in.prim_overlap_yes;
}

/// True when the backend must withhold the declaration it would otherwise have made, and serve
/// this draw's self-read from a copy of the target instead. This is the form the renderer
/// publishes, because the draw config's zero state has to mean "behave as before".
constexpr bool GSDrawWithholdsFeedbackLoop(const GSDeclaredLoopScopeInputs& in)
{
	return in.declared_road && !GSDrawDeclaresFeedbackLoop(in);
}

// Off the road nothing is withheld, whatever the scope says -- the scope has no road to narrow.
static_assert(!GSDrawWithholdsFeedbackLoop({.scope = GSDeclaredLoopScope::OverlapOnly}));
static_assert(!GSDrawWithholdsFeedbackLoop({.scope = GSDeclaredLoopScope::OverlapOnly, .prim_overlap_yes = true}));
static_assert(!GSDrawDeclaresFeedbackLoop({.prim_overlap_yes = true}));

// On the road with the default scope every reader declares, exactly as the road shipped it.
static_assert(GSDrawDeclaresFeedbackLoop({.declared_road = true}));
static_assert(GSDrawDeclaresFeedbackLoop({.declared_road = true, .prim_overlap_yes = true}));
static_assert(!GSDrawWithholdsFeedbackLoop({.declared_road = true}));
static_assert(!GSDrawWithholdsFeedbackLoop({.declared_road = true, .prim_overlap_yes = true}));

// Scoped, the overlapping draws keep the declaration and every other reader gives it up.
static_assert(GSDrawDeclaresFeedbackLoop(
	{.scope = GSDeclaredLoopScope::OverlapOnly, .declared_road = true, .prim_overlap_yes = true}));
static_assert(!GSDrawDeclaresFeedbackLoop({.scope = GSDeclaredLoopScope::OverlapOnly, .declared_road = true}));
static_assert(GSDrawWithholdsFeedbackLoop({.scope = GSDeclaredLoopScope::OverlapOnly, .declared_road = true}));
static_assert(!GSDrawWithholdsFeedbackLoop(
	{.scope = GSDeclaredLoopScope::OverlapOnly, .declared_road = true, .prim_overlap_yes = true}));

namespace GSDeclaredLoopScopePolicy
{
	/// The scope the next draw resolves with.
	///
	/// A process-wide inline global rather than a setting, for the reason every policy override
	/// in this directory is one: which draws should pay a driver's serialising primitive mode is
	/// a measurement result on one device, not a user preference. Set once before the VM starts;
	/// read per draw, in GSRendererHW::DetermineBarriers.
	inline GSDeclaredLoopScope s_scope = GSDeclaredLoopScope::All;

	inline void SetScope(GSDeclaredLoopScope value) { s_scope = value; }
	inline GSDeclaredLoopScope GetScope() { return s_scope; }

	/// For the banner. A measurement log quotes this, so it names the configuration rather than the setting.
	inline const char* Name()
	{
		return (s_scope == GSDeclaredLoopScope::OverlapOnly) ? "overlap-only" : "all readers";
	}
} // namespace GSDeclaredLoopScopePolicy
