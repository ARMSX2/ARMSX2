// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"

// The GS-on-GPU backend (GSHWRendererVariant::TileGpu): pass-planned indirect submission
// with VRAM truth on the GPU. Design: umbrella devs/bmdhacks/gs-on-gpu-design-brief-2026-08-17.
//
// Deliberately a plain GSRenderer subclass, not GSRendererSW — Tile inherits the software
// renderer solely for its byte-exact software floor, and this design has no floor. It still
// gets GSState's register/PCRTC/savestate machinery, and will override Transfer<n> when the
// flat front end replaces the shipping GIF decode. It reuses Tile's page/hazard models
// (GSVramModel and friends) as libraries; it does not share Tile's per-draw delivery road.
//
// Stage-1 skeleton: replays a stream and renders nothing. Draw() grows the pass planner;
// GetOutput grows the presenter path when the delivery structure exists to feed it.
class GSRendererTileGpu final : public GSRenderer
{
public:
	GSRendererTileGpu();
	~GSRendererTileGpu() override;

protected:
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;

	// No draws yet, so no coverage-alpha (AA1) path — same reasoning as GSRendererNull:
	// without this override the base GSState version pxFailRel("Not implemented")s on AA1
	// games (e.g. Shadow of the Colossus). Revisited when the delivery structure draws.
	bool IsCoverageAlphaSupported() override;

private:
	// The pass-structure observer: the round-2 GSTilePassSim model fed by the live GSState
	// decode, ported from the Tile renderer's PassSimObserveDraw feeder. Stage 1.3a observes
	// DRAWS only — the dominant break source on the gate title (SotC: uploads never break,
	// the 8-pair budget never binds; hazards dominate) — so the pass structure it reports is
	// a lower bound where transfer events would add breaks. The transfer hooks (upload/move/
	// cpu-read) and the plan this becomes (emitted to the executor once it can record) land
	// in following commits. Always on for this variant: it is the renderer's understanding of
	// the frame, not an optional arm, so it takes no config lever.
	GSTilePassSim m_pass_sim;

	// The screen-space bbox of the current draw, scissor-clipped — the Tile renderer's
	// ComputeDrawRect, which reads only base state, replicated here (it is not a base method).
	GSVector4i ComputeDrawRect() const;

	// Feed one flushed primitive batch to the pass model: derive its page footprints,
	// classify its in-pass reads, and observe it. Fired once per Draw().
	void ObserveDraw();

	// Mean/p50 of the accumulated per-frame pass structure, emitted at teardown (the
	// aggregate the offline -tilepasssim arm reports, minus the Tile-only GIF-stream tail).
	void ReportPassStructure();
};
