// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"

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

protected:
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;

	// No draws yet, so no coverage-alpha (AA1) path — same reasoning as GSRendererNull:
	// without this override the base GSState version pxFailRel("Not implemented")s on AA1
	// games (e.g. Shadow of the Colossus). Revisited when the delivery structure draws.
	bool IsCoverageAlphaSupported() override;
};
