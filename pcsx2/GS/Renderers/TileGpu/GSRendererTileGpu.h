// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <array>
#include <vector>

// The GS-on-GPU backend (GSHWRendererVariant::TileGpu): pass-planned indirect submission
// with VRAM truth on the GPU. Design: umbrella devs/bmdhacks/gs-on-gpu-design-brief-2026-08-17
// and gs-on-gpu-vram-model-2026-08-18 (the memory model).
//
// Deliberately a plain GSRenderer subclass, not GSRendererSW — Tile inherits the software
// renderer solely for its byte-exact software floor, and this design has no floor. It still
// gets GSState's register/PCRTC/savestate machinery, and will override Transfer<n> when the
// flat front end replaces the shipping GIF decode. It reuses Tile's page/hazard models
// (GSVramModel and friends) as libraries; it does not share Tile's per-draw delivery road.
//
// The memory model, in one paragraph. Guest bytes have three homes: the CPU shadow S (GSState's
// local memory, CPU-write-only), the frame's ring B (8 KB slots the GPU composes and reads,
// GPU-write-only past the executor's prefill), and the resident targets I (the target pool's
// textures, keyed by layout, PERSISTING across frames -- the frame boundary syncs nothing).
// GSVramModel says, per page and plane, which of S or some target holds the newest bytes; here
// "synced" means the frame's ring holds a page's byte truth, so a reader can take it from there.
// Every draw asks the model: pages its target does not hold newest are SEEDED into the target
// out of the ring first (bytes -> image); pages a texture read needs that some target holds
// newest are WRITTEN BACK into the ring first (image -> bytes); the ring slot for a page is
// prefilled from S when any of its bytes are CPU-newest. Both are GPU dispatches the plan
// carries to the executor in stream order. Uploads shrink or clear a target's claim (Tile's
// block sidecar), or, when they would overwrite bytes only a target holds at sub-block
// granularity, flush the plan and pull the page down first (the one stall road, counted).
// The GPU never reads S; the CPU never reads B; program order plus the ring is the whole
// synchronisation story.
//
// Stage-1 contract: wrong-fast where the executor is (no blending, no per-draw write masks);
// EXACT where the model is -- every page's truth is named, every crossing is counted.
class GSRendererTileGpu final : public GSRenderer
{
public:
	GSRendererTileGpu();
	~GSRendererTileGpu() override;

protected:
	void Destroy() override;
	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;

	// No coverage-alpha (AA1) path — same reasoning as GSRendererNull: without this override
	// the base GSState version pxFailRel("Not implemented")s on AA1 games (e.g. Shadow of the
	// Colossus). Revisited with blending.
	bool IsCoverageAlphaSupported() override;

	// The stream's non-draw memory events. Each drives the memory model (the truth of the
	// affected pages moves or is pulled) AND the pass-structure observer. The base hooks are
	// empty -- the actual transfer already happened in GSState's transfer path -- but the
	// upload hook fires BEFORE the shadow write lands, which is what lets an in-flight
	// version of the page's old bytes be captured (see SupersedeRingSlots). Move performs the real
	// guest copy through the base, which itself drives the two hooks.
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;

	// The whole-of-truth seams: every page a target holds newest comes down to the CPU shadow
	// (savestate, renderer switch, close), and a purge drops the targets after that.
	void ReadbackTextureCache() override;
	void PurgeTextureCache(bool sources, bool targets, bool hash_cache) override;

private:
	// The pass-structure observer: the round-2 GSTilePassSim model fed by the live GSState
	// decode, ported from the Tile renderer's PassSimObserveDraw feeder. It is the reference
	// accounting for the GS-semantic minimum pass structure; the plan below is the actual one.
	// Always on for this variant: it is the renderer's understanding of the frame, not an
	// optional arm, so it takes no config lever.
	GSTilePassSim m_pass_sim;

	// Set only for the duration of the base GSRenderer::Move() call. A GS->GS move reaches
	// the sim once as OnMove, which already accounts both halves; the base copy underneath
	// then fires InvalidateLocalMem(src) and InvalidateVideoMem(dst), whose sim forwards
	// would double-count the move. The two hooks gate their sim forward on this flag; the
	// memory model still sees both halves through them (a move IS a CPU read then a CPU write
	// as far as truth is concerned).
	bool m_pass_sim_in_move = false;

	// --- the memory model ------------------------------------------------------------------
	GSVramModel m_vram_model;
	GSTileTargetPool m_target_pool;

	// The screen-space bbox of the current draw, scissor-clipped — the Tile renderer's
	// ComputeDrawRect, which reads only base state, replicated here (it is not a base method).
	GSVector4i ComputeDrawRect() const;

	// Feed one flushed primitive batch to the pass model: derive its page footprints,
	// classify its in-pass reads, and observe it. Fired once per Draw().
	void ObserveDraw();

	// Mean/p50 of the accumulated per-frame pass structure and of the memory model's traffic,
	// emitted at teardown.
	void ReportPassStructure();
	void ReportModelTraffic();

	// --- the pass plan the executor consumes --------------------------------------------
	// One row of the executor's indexed state table: the per-draw state a shader reads via
	// gl_InstanceIndex (the indirect draw's first_instance). Its layout is this backend's
	// contract with its own shader (tilegpu.glsl's StateRow, std430, 80 bytes), opaque to the
	// executor (which stages it by state_stride). The transform is the HW tfx VertexScale/
	// VertexOffset reused verbatim; the texture block is wrong-fast — direct 32-bit and
	// paletted, MODULATE/DECAL, nearest — and grows to the rest of the fixed-function state.
	struct alignas(16) StateRow
	{
		float vertex_scale[2];  // maps raw 12.4 screen XY to clip, per the tfx VS
		float vertex_offset[2];
		u32 z_write;
		u32 z_test;
		u32 tex_enable;         // 1 = sample the guest texture, 0 = vertex-colour only
		u32 fst;                // 1 = FST/UV coords, 0 = STQ coords
		u32 tbp0;               // TEX0.TBP0, texture base in blocks
		u32 tbw;                // pages per texture row (TBW, or TBW>>1 for paletted); AccumulateDraw
		u32 tw;                 // texture width  in texels (1 << TW)
		u32 th;                 // texture height in texels (1 << TH)
		u32 tfx;                // TEX0.TFX texture function
		u32 tcc;                // TEX0.TCC: 1 = texture carries alpha, 0 = alpha from vertex
		u32 wms;                // CLAMP.WMS horizontal wrap mode
		u32 wmt;                // CLAMP.WMT vertical wrap mode
		u32 index_format;       // 0 = direct 32-bit texel, 1 = PSMT8 index, 2 = PSMT4 index
		u32 pal_offset;         // word offset of this draw's palette in the frame stream
		u32 epoch;              // page-table epoch this draw's byte reads go through
		u32 date;               // destination-alpha test: 0 off, 1 pass on alpha bit 7 clear, 2 on set
		s32 ofx, ofy;           // XYOFFSET, 12.4 fixed: the vertex-to-pixel origin the scissor is in
		s32 sc_x0, sc_y0;       // the GS scissor in target pixels, [x0, x1) x [y0, y1) -- four VS clip planes
		s32 sc_x1, sc_y1;
		u32 pad0_, pad1_;
	};
	static_assert(sizeof(StateRow) == 112, "TileGpu StateRow must be 112 bytes to match tilegpu.glsl std430");

	// One draw's inputs the plan build resolves once the frame is complete: which surfaces it
	// renders into (model ids -> pool textures), the coordinate origin, the draw rect, and the
	// range of reconciliation ops the model emitted for it. draw_index points at the matching
	// m_plan_draws row.
	struct PendingDraw
	{
		GSTileSurfaceId color_surface;
		GSTileSurfaceId z_surface; // valid only if z_used
		bool z_used;
		bool z_write, z_test;
		bool break_before;    // a seed of this draw's target precedes it: it opens a new pass
		s32 ofx, ofy;         // XYOFFSET, 12.4 fixed
		GSVector4i rect;      // scissor-clipped draw bbox
		GSVector4i scissor;   // the GS scissor (SCISSOR register), exclusive right/bottom
		u32 draw_index;
		u32 first_prep_op;    // reconciliation ops that must run before this draw's pass
		u32 prep_op_count;

		// Texture inputs, resolved at accumulation. tex_enable is set for the formats this stage
		// samples (direct 32-bit and paletted); every other textured draw falls back to the
		// vertex-colour path. See AccumulateDraw.
		bool tex_enable;
		bool fst;
		u32 tbp0, tbw, tw, th, tfx, tcc, wms, wmt;
		u32 index_format, pal_offset;
		u32 epoch;
		u32 date;             // 0 off, 1 = DATM 0, 2 = DATM 1; the pass takes a snapshot for it
	};

	// Per-frame accumulation (filled in Draw via AccumulateDraw, consumed + reset in the plan
	// build).
	std::vector<GSVertex> m_plan_vertices;
	std::vector<u16> m_plan_indices;
	std::vector<StateRow> m_plan_states;
	std::vector<u32> m_plan_palettes; // expanded CLUTs (GSClut::Read32), concatenated per frame
	std::vector<GSDevice::GSTileGpuIndirectDraw> m_plan_draws;
	std::vector<GSDevice::GSTileGpuTopology> m_plan_topologies; // one per m_plan_draws entry
	std::vector<u32> m_plan_blend_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::blend_keys)
	std::vector<PendingDraw> m_plan_pending;
	std::vector<GSDevice::GSTileGpuPass> m_plan_passes;
	std::vector<GSDevice::GSTileGpuTargetPair> m_plan_target_pairs;
	std::vector<GSDevice::GSTileGpuSnapshotCopy> m_plan_snapshots;
	std::vector<GSDevice::GSTileGpuPrepOp> m_plan_prep_ops;
	std::vector<GSDevice::GSTileGpuPageEntry> m_plan_page_entries;
	std::vector<GSTileSurfaceId> m_plan_target_surfaces; // the plan's target list, as surfaces
	std::vector<GSTexture*> m_plan_targets; // ...resolved to pool textures at the plan build
	std::vector<u32> m_plan_target_of_surface; // surface id -> index into the target list (kNoTarget = absent)

	// --- the frame's byte ring -------------------------------------------------------------
	// One entry per (page, epoch range) the frame's plan reads or reconciles; the executor gives
	// each an 8 KB slot. `src` names the bytes it starts from: the CPU shadow's page (read at
	// plan build), a version copy in m_ring_versions (the page's bytes as they were before an
	// upload superseded them mid-frame), or nothing (the GPU composes every byte).
	struct RingEntry
	{
		u16 page;
		u16 epoch_first;
		u16 epoch_last;   // closed when a later CPU write supersedes the slot; else the last epoch
		bool prefill_s;   // start from S (or the version copy) -- some byte of the page is CPU-newest
		bool versioned;   // src is m_ring_versions + version_offset, not S
		u32 version_offset;
	};
	std::vector<RingEntry> m_ring_entries;
	std::vector<u8> m_ring_versions; // version copies, 8 KB each, appended as uploads supersede live slots
	std::array<u16, GS_MAX_PAGES> m_ring_live{}; // page -> index+1 of its live entry this frame, 0 = none
	u32 m_epoch = 0; // current page-table epoch; bumps when a CPU write supersedes a live slot

	// Give `page` a ring slot for the current epoch (or find its live one), and mark the S
	// prefill if any of its bytes are CPU-newest right now. Returns the entry index.
	u32 EnsureRingSlot(u32 page);

	// A CPU write is about to land on `pages`: every live slot among them captures the page's
	// current S bytes as a version copy and closes at this epoch, so draws already planned keep
	// reading the bytes they saw; the epoch advances if anything closed. Called from the upload
	// hook BEFORE GSState writes the shadow (that is why the copy is exact).
	void SupersedeRingSlots(const GSPageBitmap& pages);

	// Compose `pages`' byte truth into ring slots for the current epoch: writebacks for every
	// colour surface holding unsynced truth on them (appended as prep ops on the pending draw),
	// S prefill for the rest, and the model's synced bit set for what the ring now holds. Depth
	// and unsupported-format owners have no writeback road yet: their bytes stay whatever the
	// slot was prefilled with, counted as lossy.
	void ComposeRingPages(const GSPageBitmap& pages);

	// Ensure a surface for `layout` covering `pages` exists in the model and the pool (grown as
	// needed). Returns kGSTileNoSurface on allocation failure.
	GSTileSurfaceId EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect, const GSPageBitmap& pages);

	// Pages of `pages` whose bytes surface `id`'s texture does not hold newest for every plane
	// in `planes` (Tile's PagesNeedingUpload).
	GSPageBitmap PagesNeedingSeed(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const;

	// Emit a prep op over `pages` for surface `id` (Writeback or Seed) on the pending draw being
	// accumulated; block masks per page come from the model for writebacks, full for seeds.
	void EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages);

	// Truth on `pages` is taken (or read) by a surface with no byte road: mark it synced without
	// moving bytes, and count it lossy.
	void LossySteal(const GSPageBitmap& pages);

	// Resolve a surface's pool texture into the plan's target list (once per frame per surface).
	u32 PlanTargetIndex(GSTileSurfaceId id);

	// Copy one flushed batch's geometry into the frame streams, drive the memory model for its
	// target and texture footprints (seeds, writebacks, claims), and record its indirect draw +
	// PendingDraw. Fired from Draw() beside ObserveDraw().
	void AccumulateDraw();

	// Group the accumulated draws into passes, resolve the state rows and targets, build the
	// ring, and submit the assembled plan to the device executor. Fired from VSync before the
	// base presents (so the rendered targets are ready when GetOutput runs), and mid-frame by
	// the stall roads (a CPU reader of GPU-newest pages needs everything before it rendered).
	void BuildAndExecutePlan();

	// Pull `pages` (GPU-newest, any owner) down into the CPU shadow: flush the plan so the
	// targets hold every draw so far, then the pool's synchronous readback per owner. The one
	// stall road; `site` names the caller for the counters.
	enum class StallSite : u32
	{
		UploadSubBlock = 0, ///< an upload partially overwrites blocks only a target holds
		LocalRead,          ///< a local->host transfer (or a move's source) of GPU-newest pages
		Clut,               ///< a CLUT load from GPU-newest pages
		SyncAll,            ///< whole-of-truth: savestate / switch / purge
		Count
	};
	void ReadbackToShadow(const GSPageBitmap& pages, StallSite site);
	void SyncAllTruthToCpu();

	// The rect union a run of draws has written into the current colour surface since the last
	// forced pass break: a DATE draw whose rect intersects it reads pixels the pass would also
	// write, so it opens a new pass (a fresh snapshot). Reset on surface change and on break.
	GSTileSurfaceId m_run_surface = kGSTileNoSurface;
	GSVector4i m_run_written = GSVector4i::zero();

	// The model's per-frame traffic, mean/p50 at teardown beside the pass structure. These are
	// the structural counters the stage-1 gate reads.
	struct ModelFrame
	{
		u32 surfaces_live = 0;
		u32 ring_pages = 0;      // ring entries this frame
		u32 ring_prefill = 0;    // ...of which prefilled from S / a version copy
		u32 ring_versions = 0;   // version copies taken (uploads superseding a live slot)
		u32 epochs = 0;
		u32 writeback_ops = 0;
		u32 writeback_pages = 0;
		u32 seed_ops = 0;
		u32 seed_pages = 0;
		u32 seed_breaks = 0;     // draws that opened a pass because their target needed seeding
		u32 date_breaks = 0;     // draws that opened a pass because their DATE read needed a fresh snapshot
		u32 snapshots = 0;       // passes that took a snapshot of their target
		u32 self_reads = 0;      // draws sampling pages their own pass target holds (snapshot semantics)
		u32 lossy_pages = 0;     // truth moved without a byte road (depth / unsupported-format owners)
		u32 skipped_draws = 0;   // draws no surface could be built for (format / stride)
		u32 stalls[static_cast<u32>(StallSite::Count)] = {};
		u32 stall_pages[static_cast<u32>(StallSite::Count)] = {};
		u32 flushes = 0;         // mid-frame plan submissions
		u32 passes = 0;
	};
	ModelFrame m_frame = {};
	std::vector<ModelFrame> m_model_frames;
	bool m_warned_lossy = false;
};
