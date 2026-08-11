// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"
#include "GS/Renderers/Tile/GSTileDrawLowering.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSVramModel.h"

MULTI_ISA_UNSHARED_START

// The tiler-native hardware renderer. Vulkan-only and opt-in; the selection decision is
// GSTileSelectionPolicy.h and the constructing branch is OpenGSRenderer.
//
// M2 shape: the interval memory model tracks, per plane, which of the 512 GS pages
// have their newest bytes on the GPU; the lowering predicate sends opaque untextured
// draws to a native GPU route (upload destination pages, render through the device's
// draw-config contract, claim the footprint as GPU truth); everything else takes the
// SW floor, which stays byte-identical to GSRendererSW by inheritance. Every seam
// where the CPU side reads or writes local memory — transfers, local reads, moves,
// floor draws, present — spills exactly the pages whose truth lives on the GPU.
//
// The standing coherence invariant: CPU-side actors only ever touch pages that are
// CPU-truth when their work is queued. Queued floor draws can therefore never race a
// GPU readback (readbacks touch only GPU-truth pages); the one full rasterizer sync
// sits at the head of the native route, before it reads local memory for uploads.
class GSRendererTile final : public GSRendererSW
{
public:
	GSRendererTile(int threads);

	void Destroy() override;
	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;

private:
	void RecordDrawLogEntry() const;
	GSVector4i ComputeDrawRect() const;
	GSTileDrawPlan LowerCurrentDraw();

	// The native route.
	bool TryNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, GSTileFloorReason& reason);
	void SubmitNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, GSTexture* rt, GSTexture* ds);
	void FlattenProvokingColor();
	GSTileSurfaceId EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect, const GSPageBitmap& pages, bool& ok);
	GSPageBitmap PagesNeedingUpload(GSTileSurfaceId id, const GSPageBitmap& pages, u8 relevant_planes) const;

	// Spill machinery shared by every CPU-side seam.
	bool ReadbackModelPages(const GSPageBitmap& pages);
	void InvalidateSwTexCache(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages);
	void SyncAllTruthToCpu();
	void SpillForFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void ObserveFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);

	GSVramModel m_vram_model;
	GSTileTargetPool m_target_pool;
	GSVramModel::RectFootprint m_rect_fp; // scratch for transfer/move footprints
};

MULTI_ISA_UNSHARED_END
