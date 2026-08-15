// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"
#include "GS/Renderers/Tile/GSTileDrawLowering.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
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
	bool BuildTilePayload(bool want_zwalk, bool want_stq, GSTileFloorReason& reason);
	void DeindexVertices();
	void SubmitNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, const GIFRegTEX0& fixed_tex0,
		GSTexture* rt, GSTexture* ds, GSTexture* tex);
	void FlattenProvokingColor();
	GSTileSurfaceId EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect, const GSPageBitmap& pages, bool& ok);
	GSPageBitmap PagesNeedingUpload(GSTileSurfaceId id, const GSPageBitmap& pages, u8 relevant_planes) const;

	// -- Readback attribution (dev instrument; reported only under GSDrawLog) ---------
	//
	// The readback bill is denominated in DRAINS, not pages: a synchronous readback
	// submits the current command buffer and waits on it, which costs a full GPU drain
	// (measured ~0.28 ms on M2, ~0.24 ms on SD865) whether it carries one page or a
	// hundred. Pages and calls are recorded alongside because the ratio between them is
	// the lever — a seam paying many drains for few pages is asking to be batched, and
	// a seam paying one drain for many pages is already as cheap as this design allows.
	//
	// Split by the seam that asked, because the seams have unrelated fixes: the frame
	// boundary is a policy question (it syncs the whole of GPU truth when the presenter
	// only reads the display footprints), the floor handoff is a coverage question, and
	// the native route's own sync is an ordering question.
	enum class ReadbackSite : u8
	{
		Transfer, ///< a GIF transfer wrote pages the GPU still owned
		LocalRead, ///< the CPU read local memory back out
		Move, ///< local->local copy, both sides
		VSyncAll, ///< the unconditional whole-of-truth frame-boundary sync
		FloorDraw, ///< native -> SW floor handoff
		NativeDraw, ///< the native route's own upload/steal sync
		Oracle, ///< the per-draw oracle's syncs; an instrument, never a real cost
		Count
	};

	struct ReadbackCounters
	{
		u32 calls = 0; ///< seams that asked with a non-empty page set
		u32 pages = 0; ///< pages moved
		u32 drains = 0; ///< submit-and-wait stalls actually paid
	};

	/// Why the NATIVE route pulled, broken out. The three reasons share one stall (the
	/// route syncs their union once), so drains are not divisible between them — but the
	/// SOLE counts are, and those are what say whether fixing a reason removes a stall.
	struct NativeSyncReasons
	{
		u32 upload_pages = 0, steal_pages = 0, tex_pages = 0;
		u32 upload_sole = 0; ///< draws where an upload source was the ONLY reason
		u32 steal_sole = 0; ///< ... a steal from another surface (the M3e aliasing tiers)
		u32 tex_sole = 0; ///< ... a texture source on target-owned pages (render-to-texture)
	};

	void ReportReadbackCensus();

	// Spill machinery shared by every CPU-side seam.
	bool ReadbackModelPages(const GSPageBitmap& pages, ReadbackSite site);
	void InvalidateSwTexCache(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages);
	void SyncAllTruthToCpu();
	void SpillForFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void ObserveFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);

	// -- The per-draw lockstep oracle (GSTileOracle) ---------------------------------
	// Dev instrument, inert unless EmuCore/GS/TileDrawOracle is set. Runs BOTH arms
	// over every native draw from byte-identical inputs and records what they disagree
	// about. See GSTileOracle.h for why the comparison is three-way.

	/// A copy of some pages of CPU local memory. The page set travels with the bytes so
	/// a restore can never be paired with a footprint it was not taken from.
	struct OracleSnapshot
	{
		GSPageBitmap pages;
		std::vector<u8> bytes;

		void Capture(const u8* vm8, const GSPageBitmap& pgs);
		void Restore(u8* vm8) const;
	};

	struct OracleState
	{
		bool armed = false;
		GSPageBitmap fp; ///< the compared write footprint (FRAME pages | ZBUF pages)
		GSTileSurfaceLayout fb_l{};
		GSTileSurfaceLayout z_l{};
		u32 sync_pages = 0; ///< pages the input sync pulled; 0 == perturbation-free row
		OracleSnapshot pre, gpu, sw;

		// The native route rewrites the vertex buffer in place (de-indexing, flat-colour
		// flattening), so the SW arm cannot be handed what is left of it.
		std::vector<GSVertex> verts;
		std::vector<u16> indices;
		u32 v_head = 0, v_tail = 0, v_next = 0, i_tail = 0;

		// Scratch for the three-state pixel reads, kept across draws.
		std::vector<u32> c_pre, c_sw, c_gpu;
		std::vector<u32> z_pre, z_sw, z_gpu;
	};

	bool OracleBeginDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void OracleCompareDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void OracleAbandonDraw();
	void OracleSaveVertices();
	void OracleRestoreVertices();
	void OracleReadPixels(const GSVector4i& r, u32 bp, u32 bw, u32 psm, std::vector<u32>& out) const;
	void OracleInvalidateFootprint();

	GSVramModel m_vram_model;
	GSTileTargetPool m_target_pool;
	GSTileTextureSource m_tex_source;

	// Per-primitive plane payload for the current draw: the depth walk's blocks
	// and the coordinate plane's blocks concatenated into one upload, built per
	// draw and consumed synchronously by RenderHW. Empty = neither walk. The two
	// offsets are uvec4 element indices into it, or NoWalk.
	static constexpr u32 NoWalk = 0xFFFFFFFFu;
	std::vector<u32> m_tile_payload;
	u32 m_zwalk_at = NoWalk;
	u32 m_stq_at = NoWalk;
	GSVramModel::RectFootprint m_rect_fp; // scratch for transfer/move footprints
	OracleState m_oracle;

	std::array<ReadbackCounters, static_cast<u32>(ReadbackSite::Count)> m_readback{};
	NativeSyncReasons m_native_sync{};
	u32 m_readback_frames = 0;
};

MULTI_ISA_UNSHARED_END
