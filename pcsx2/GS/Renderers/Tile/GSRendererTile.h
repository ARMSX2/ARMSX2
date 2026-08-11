// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"
#include "GS/Renderers/Tile/GSVramModel.h"

MULTI_ISA_UNSHARED_START

// The tiler-native hardware renderer. Vulkan-only and opt-in; the selection decision is
// GSTileSelectionPolicy.h and the constructing branch is OpenGSRenderer.
//
// What ships here today is the SW-fallback floor plus the interval memory model
// observing every flow — transfers, draws, local reads, moves — from the renderer's
// seams. With no native draw path yet, no page ever becomes GPU-truth and the model is
// pure observation (the spill placeholders assert that), but the footprint derivation,
// the flow discipline and the per-frame invariant police all run for real against the
// whole dump corpus. The floor is not scaffolding: it stays as the permanent lowest
// rung of the realization lattice. The native path lands as a Draw() route that sends
// provable draws to the GPU and falls through to this floor for everything it cannot
// express.
class GSRendererTile final : public GSRendererSW
{
public:
	GSRendererTile(int threads);

	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;

private:
	void RecordFloorDrawLogEntry() const;
	void ObserveFloorDraw();

	GSVramModel m_vram_model;
	GSVramModel::RectFootprint m_rect_fp; // scratch for transfer/move footprints
};

MULTI_ISA_UNSHARED_END
