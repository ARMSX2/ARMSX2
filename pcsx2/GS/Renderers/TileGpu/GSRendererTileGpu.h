// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileExpandedCache.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
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
	//
	// A CROSS-CHECK, and nothing downstream of it: the planner's inputs are AccumulateDraw and
	// the memory model, and no field of this object is ever read back into a plan. So it is a
	// lever -- EmuCore/GS/TileGpuPassSim, gsrunner -tilepasssim -- off by default, because
	// feeding it costs roughly 0.4 ms/frame and a timed run must not pay that. Every call site
	// is guarded by IsActive(), the Tile renderer's own idiom; the flag is read once, at
	// construction, so flipping it mid-run needs a renderer restart.
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
		u32 fge;                // 1 = PRIM.FGE, the fragment's colour walks toward the fog colour
		u32 fogcol;             // FOGCOL packed 0x00BBGGRR
		u32 atst;               // 0 = no alpha test; else TEST.ATST + 1 (2 = LESS ... 8 = NOTEQUAL)
		u32 aref;               // TEST.AREF, the value the fragment alpha is compared against
		u32 texa;               // 24-bit texel alpha: bit 0 = apply, bit 1 = TEXA.AEM, bits 8-15 = TEXA.TA0
		u32 region_u;           // CLAMP.MINU | (CLAMP.MAXU << 16), for the two REGION wrap modes
		u32 region_v;           // CLAMP.MINV | (CLAMP.MAXV << 16)
		u32 ltf;                // 1 = bilinear: the fragment blends the four texels around its coordinate
		u32 tex_target;         // slot in this pass's sampled-target array, or kNoTexSlot = decode bytes
		// Rule 3: slot in the FRAME's materialised-source array, or kNoSourceSlot. It took the row's
		// old explicit tail padding, which was there because alignas(16) pads the C++ side to 144
		// anyway while the shader's std430 array stride is a multiple of 8 -- an implicit tail would
		// have put the two sides on different strides and read every row but the first from the wrong
		// place. Spending it on a real field keeps the row at 144 and the strides in step.
		u32 tex_source;
	};
	static_assert(sizeof(StateRow) == 144, "TileGpu StateRow must be 144 bytes to match tilegpu.glsl std430");
	static_assert(offsetof(StateRow, tex_source) == 140,
		"TileGpu StateRow::tex_source must sit exactly where the row's tail padding was");

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
		bool break_before;    // this draw's prep ops cannot be hoisted over the open pass: it opens a new one
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
		bool fge;             // PRIM.FGE: this draw's fragments are fogged
		u32 fogcol;           // FOGCOL packed 0x00BBGGRR
		u32 atst;             // 0 = no per-fragment alpha test; else TEST.ATST + 1
		u32 aref;
		bool alpha_written;   // false = the colour write mask keeps alpha (AFAIL RGB_ONLY)
		u32 texa;             // 24-bit texel alpha: bit 0 apply, bit 1 AEM, bits 8-15 TA0
		u32 region_u, region_v; // CLAMP MIN | (MAX << 16) per axis, for the REGION wrap modes
		bool ltf;             // TEX1 asks for LINEAR on the side of the LOD this draw sits on

		// Rule 2 of the VRAM model's texel road: the resident target that holds this draw's whole
		// read window, or kGSTileNoSurface for the byte road. When set, nothing was composed into
		// the ring for this read -- the fragment stage samples the target's live pixels.
		GSTileSurfaceId tex_source;
		u32 tex_slot;         // its slot in the pass's bind table, resolved once the pass is known

		// Which texel road this draw takes (GSDevice::kGSTileGpuRoad*), zero when it is untextured.
		// The pass's variant is the OR over its draws, so this is what decides how much shader the
		// pass compiles -- see GSDevice::GSTileGpuPass::road_mask.
		u32 road_mask;

		// Rule 3 of the same road: this draw's slot in the FRAME's materialised-source bind table
		// (m_plan_sources), or kNoSourceSlot where rule 3 refused and the draw kept the byte road.
		// Frame-wide rather than per-pass, because a source belongs to a texture window and serves
		// draws in any number of passes. Set only after the cache has actually handed over an image.
		u32 src_slot;
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
	std::vector<u32> m_plan_bind_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::bind_keys)
	// Rule 3's frame-wide bind table: the (image, sampler) pairs this frame's draws sample, in the
	// slot order a state row's tex_source names. Deduped on the pair -- one window sampled through two
	// different TEX1/CLAMP settings is two slots, because the filtering and the wrap ride in the
	// descriptor and not in the shader.
	std::vector<GSDevice::GSTileGpuPassPlan::SourceBind> m_plan_sources;
	std::vector<PendingDraw> m_plan_pending;
	std::vector<GSDevice::GSTileGpuPass> m_plan_passes;
	std::vector<GSDevice::GSTileGpuTargetPair> m_plan_target_pairs;
	std::vector<GSDevice::GSTileGpuSnapshotCopy> m_plan_snapshots;
	std::vector<GSDevice::GSTileGpuPrepOp> m_plan_prep_ops;
	std::vector<GSDevice::GSTileGpuPageEntry> m_plan_page_entries;
	std::vector<u32> m_plan_tex_sources; // per pass: the rule-2 targets its draws sample, as target indices
	// The images this frame's Materialise prep ops build into (GSTileGpuPassPlan::prep_textures).
	// Deliberately NOT the target list: a materialised source is nothing's render target, no target
	// pair names it, and the page model does not own it -- sharing the index space would put a
	// non-surface into every place a target index is read as a surface.
	std::vector<GSTexture*> m_plan_prep_textures;
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

	// Rule 2 of the VRAM model's texel road: the ONE resident target that can serve this texture
	// read whole, or kGSTileNoSurface for the byte road. Everything it asks is a proof, not a
	// guess -- the page model names a sole owner of every page of the window, the owner's pixel
	// space is the read's own layout (same base, same stride, byte-compatible format,
	// page-aligned), its texture actually holds texels on those pages, and the window fits inside
	// it. Anything short of that is the byte road, which is always correct and never asks.
	//
	// The draw's own targets are excluded here rather than checked later: a draw sampling what its
	// pass renders into is the in-pass feedback road, and this stage does not have one.
	GSTileSurfaceId TargetForTextureRead(const GSTileSurfaceLayout& tex_l, u32 tw, u32 th,
		const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id, GSTileSurfaceId z_id) const;

	// Whether the device can bind a resident target as a draw's texture. Read once at
	// construction, because a rule-2 draw is served by work the renderer DOES NOT DO (its read
	// window is never composed into the ring) -- asking late, or asking a device that then
	// refuses, would leave the fragment stage decoding bytes nobody wrote.
	bool m_bindless_targets = false;

	// --- rule 3, the source cache: BUILT and SAMPLED ------------------------------------------
	// The three library caches the materialised-source road is built out of. A direct-colour window
	// the probe admits is MATERIALISED -- an ordinary RGBA8 image, built on the device out of the
	// same ring bytes the flat road reads, cached and invalidated by the same predicate -- and the
	// draw that admitted it then SAMPLES it: one hardware fetch in place of the swizzle arithmetic,
	// and under LINEAR one fetch in place of four. Every refusal (region wrap, pin, capacity, a
	// palette pair not yet admitted, a failed build) falls back to the byte road, which is always
	// correct and never asks. A paletted window takes the same road in two stages: the index image out
	// of the ring bytes, and the palette expansion on top of it, admitted on second sight across a
	// frame boundary so a window drawn once never pays for an expansion.
	GSTileTextureSource m_tex_source;
	GSTilePaletteCache m_palette_cache;
	GSTileExpandedCache m_expand_cache;

	// The pin discipline's clock (GSTileTextureSource::SetFrame). It counts PLANS, not video
	// frames: a cache entry a recorded draw names is held down until the plan carrying that draw
	// has been handed to the executor, which is BuildAndExecutePlan -- at VSync usually, but also
	// at any mid-frame flush. Starts at 1 because zero means "discipline off", which is what the
	// Tile renderer leaves these caches at.
	u64 m_source_frame = 1;

	// Move the pin clock on: every entry the plan just executed named is released. Called from
	// exactly one place, the end of BuildAndExecutePlan, and from construction to arm the
	// discipline in the first place.
	void AdvanceSourcePinFrame();

	// Rule 3's DONOR build road: the window's texels read straight out of the ONE live target that
	// owns its pages, through the GS swizzle, instead of out of the frame's byte ring. It is what
	// removes the round trip the byte road pays for exactly these windows -- a writeback compute
	// pass reswizzling the target into ring slots, then a materialise unswizzling them back -- for
	// bytes that never leave the GPU. It also gives them an IDENTITY: a window over a rendered
	// target has no honest page fingerprint (the CPU shadow does not hold those bytes), so every
	// moved gen stamp read as a rebuild and every identity derived from it churned with it.
	//
	// Serves PALETTED windows (PSMT8/PSMT4) over a page-aligned CT32/CT24 colour target, which is
	// the class rule 2 refuses by construction -- rule 2 binds a target only where the read's own
	// layout IS the target's, and a palettised read of a colour surface never is. A direct-colour
	// window at the target's own base and stride is rule 2's already; one at an offset or a
	// different stride would need a copy leg this road does not have, and is counted rather than
	// served (src_donor_direct).
	struct DonorPlan
	{
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 src_bp = 0;     ///< the window's TBP0 (blocks)
		u32 src_bwpg = 0;   ///< the window's width in pages, as the byte road computes it
		u32 owner_bp = 0;   ///< the owner's base (page-aligned blocks)
		u32 owner_bwpg = 0; ///< the owner's width in pages
		u32 psm = 0;        ///< the window's TEX0.PSM, which picks the index format
	};

	// Can one live target serve this window whole, as a donor? Every clause is a proof, in the
	// shape rule 2's are: one owner for EVERY plane of every page at whole-page granularity (the
	// reinterpretation reads any byte of the owner's cell, alpha included), a page-aligned CT32/CT24
	// colour layout so the owner's pixel space is the guest layout, its texture actually holding
	// texels on those pages, the window starting at or after the owner's base, and a format the
	// swizzle forms express. Anything short of that keeps the byte road, which is always correct.
	// The draw's own targets are excluded here rather than checked later: sampling what the pass
	// renders into is the feedback road, and this stage does not have one.
	bool DonorForTextureRead(const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id,
		GSTileSurfaceId z_id, DonorPlan& out);

	// "The owner's texels changed", as a monotonic counter, and the whole of the donor road's
	// content identity (GSTileTextureSource::TargetToken). It is the renderer's own because the page
	// model tracks BYTES: a page's write generation moves when anything claims the page, including a
	// depth draw aliasing a colour target's pages, which changes no colour texel at all. The counter
	// is bumped by the three things that write a pool texture -- a draw rendering into it, a seed
	// filling it, and the pool allocating or growing it -- and by nothing else, so it is exactly as
	// strict as it must be and no stricter. Globally monotonic, never per-surface: a surface id is a
	// recycled slot, and a per-surface counter would let a new surface reproduce its predecessor's
	// version.
	//
	// ⚠️ Being over-eager here can only cost a rebuild, never a wrong serve: the cache consults the
	// token ONLY when the gen stamp has already moved, so a version that bumps for nothing loses a
	// rescue and cannot gain a stale one.
	std::vector<u64> m_surface_version;
	u64 m_surface_version_counter = 0;
	void BumpSurfaceVersion(GSTileSurfaceId id);
	u64 SurfaceVersion(GSTileSurfaceId id) const;

	// The emission half of rule 3's build. The cache owns what to build, the identity it is
	// filed under and when it dies; this owns how the texels get there -- a materialise pass
	// queued into the frame's prep-op stream, executed at the head of the emitting draw's pass.
	// The split is the point: the cache is library code that knows nothing about deferred
	// execution, and the renderer is where deferred execution lives.
	struct SourceMaterialiser final : public GSTileSourceBuilder
	{
		GSRendererTileGpu* r = nullptr;
		u32 epoch = 0; ///< the page-table epoch the emitting draw reads through
		const DonorPlan* donor = nullptr; ///< non-null: build off the owner target, not off the ring
		bool BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) override;
	};

	// Queue the pass that fills `tex` with the window's texels read out of the donor's owner target.
	// Unlike a materialise this reads an IMAGE, so it is only safe at the head of a pass none of
	// whose already-recorded draws has written that image -- the caller runs DonorHoistCollides and
	// breaks the pass when it has, exactly as a writeback does.
	bool EmitDonorOp(GSTexture* tex, const GIFRegTEX0& tex0, const DonorPlan& donor);

	// Would a donor build queued for the draw being accumulated read pixels a draw already in the
	// open pass has written? Then it cannot run at that pass's head and its draw opens its own.
	// The writeback's own test, narrowed to the one hazard a donor has (it rewrites no ring slot,
	// so the read half of WritebackHoistCollides has nothing to say about it).
	bool DonorHoistCollides(GSTileSurfaceId owner, const GSPageBitmap& pages) const;

	// Queue the pass that fills `tex` with the window at tex0/texa, reading the ring at `epoch`.
	// Emitted AFTER the window's composition ops so array order puts it behind them at the pass
	// head; safe to hoist there for the same reason a seed is, since the ring is staged whole-frame
	// and the composition it depends on is in the same op range ahead of it.
	bool EmitMaterialiseOp(GSTexture* tex, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 epoch);

	// The same split for the paletted road's second stage. GSTileExpandedCache owns admission,
	// identity and lifetime; this owns the emission -- which for a renderer that records draws has to
	// be an op in the stream rather than a pass issued now, and an op that lands AFTER the materialise
	// whose index image it reads.
	struct SourceExpander final : public GSTileExpandBuilder
	{
		GSRendererTileGpu* r = nullptr;
		bool BuildTileExpansion(GSTexture* index, GSTexture* palette, GSTexture* dst) override;
	};

	// Queue the pass that fills `dst` with palette[index] per texel. Emitted from inside the expanded
	// cache's build, which the road below runs immediately after the index build, so array order puts
	// it behind the materialise it depends on.
	bool EmitExpandOp(GSTexture* dst, GSTexture* index, GSTexture* palette);

	// This frame's slot in the plan's prep-texture list for a texture a prep op names (a source image,
	// an index image, a palette). Deduped: one palette serves many windows and one index image serves
	// many palettes, and a prep op names each of them by index.
	u32 PrepTextureIndex(GSTexture* tex);

	// Build (or find) the materialised source for a window the probe admitted, and on success put
	// this draw ON it: `pd` takes a bind slot and road SOURCE, and its fragment stage samples the
	// image instead of decoding ring bytes. A direct-colour window is one image and one pass. A
	// PALETTED one is two of each -- the window materialises to an index image, and that image times
	// this draw's palette expands into the RGBA8 the draw actually samples -- because the source
	// cache's key deliberately excludes the palette, so one index build serves every palette a game
	// cycles through it. `window` indexes m_probe_windows. Returns false on every refusal, and a
	// refusal is not an error: the caller has already put the draw on the byte road and that road is
	// correct on its own.
	//
	// `donor`, when given, builds the source off the owner target named there instead of off the
	// ring, and files it under that target's identity rather than under a page fingerprint. The
	// caller takes that road BEFORE composing the window -- not composing it is the point.
	bool MaterialiseSourceRoad(PendingDraw& pd, u32 window, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
		u32 epoch, const DonorPlan* donor = nullptr);

	// This frame's slot for the (image, sampler) pair, appending it to m_plan_sources on first sight.
	// kNoSourceSlot once the frame's table is full -- a hard bound, not a hope, and counted.
	u32 SourceSlotFor(GSTexture* tex, u8 sampler);

	// The sampler a draw's TEX1/CLAMP asks for, as a GSHWDrawConfig::SamplerSelector key: bit 0 =
	// REPEAT on U, bit 1 = REPEAT on V, bit 2 = bilinear. Only the eight combinations rule 3 admits
	// exist -- the REGION modes refused before they got here.
	static u8 SourceSamplerKey(u32 wms, u32 wmt, bool ltf);

	static constexpr u32 kNoSourceSlot = GSDevice::GSTileGpuPassPlan::kNoSourceSlot;
	// The descriptor array rule 3 binds through (design §6), and a contract constant rather than a
	// tunable: the shader declares an array of exactly this size. A frame needing more distinct
	// sources refuses the overflow to the byte road -- counted, and 128 rather than 64 because the
	// corpus census found GT4 and OutRun pinned at 64 with draws refused behind them.
	static constexpr u32 kMaxSourceSlots = GSDevice::GSTileGpuPassPlan::kMaxSources;

	// One distinct texture window the frame probed, in first-appearance order.
	struct ProbedWindow
	{
		GSTileTextureSource::WindowKey key;
		u64 stamp = 0;     ///< GSTileTextureSource::GenStamp of the window's pages at first sight
		/// What this window's CONTENT is, and where that answer came from. Two different windows'
		/// truth lives in two different places and the numbering spaces must not meet: a window the
		/// CPU shadow holds gets a fingerprint of its page bytes, and one whose truth is a rendered
		/// target gets the owning target's id and version (the donor road's identity), which is why
		/// this is a tagged pair and not a bare number. Unknown -- the tier declined and no donor
		/// serves it -- is never equal to anything, including another unknown.
		GSTileTextureSource::ContentToken content;
		u64 build_id = 0;  ///< content identity: carried over while the CONTENT holds, stamp or no stamp
		bool admitted = false;     ///< a draw of THIS frame has been admitted on this window. It is the
		                           ///< pin's marker (a rebuild under it would land in the same texture an
		                           ///< already-recorded draw names) and the frame's distinct-source census.
		bool prev_seen = false;    ///< the frame before also probed this window
		bool prev_current = false; ///< ...and its bytes are the same ones, by stamp or by fingerprint
	};
	// This frame's windows and last frame's. Two frames is the whole history the counters need:
	// the stamp question is "did this window's content hold still since the frame before", and
	// the build identity a palette pair keys on only has to survive the frame boundary the
	// expanded cache admits across. A real cache holds 512 entries and would hit on windows
	// older than this, so the hit column here is a LOWER bound and the rebuild column an upper
	// one.
	std::vector<ProbedWindow> m_probe_windows;
	std::vector<ProbedWindow> m_probe_prev;
	u64 m_probe_build_counter = 0;

	// Rule 3's admission question. Computes the window's key and content stamp, asks the palette
	// pair whether it would be admitted, and either admits the draw or counts one named refusal.
	// Called only where rule 2 declined, and it writes nothing but counters and the window record --
	// the BUILD is a separate act, after the window's bytes have been composed, and it is the build
	// that puts the draw on the road. Returns the admitted window's index in m_probe_windows, or
	// kNoSourceSlot.
	//
	// `donor_token`, when Known, is the window's content identity taken from the target that owns
	// its pages -- and it replaces the page-fingerprint tier outright rather than joining it, both
	// because the fingerprint cannot answer for GPU-side bytes (it declines and returns nothing) and
	// because it costs an 8 KB hash per moved page to decline.
	u32 ProbeSourceRoad(const PendingDraw& pd, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, bool paletted,
		bool mip_active, const GSTileTextureSource::ContentToken& donor_token);

	// Which pages of a surface's texture actually hold texels. The memory model tracks BYTES, and
	// by it a page the surface owns no truth for is in perfect order -- its bytes are in the CPU
	// shadow, and everything reading through the byte road gets them. Nothing reads the texture
	// itself that way: the present hands the pool texture straight to the display, so a page of
	// the target that no draw covered and no seed filled reaches the screen as whatever the
	// allocator left there (on this device, magenta). The texture is a rectangle, so this is not a
	// rare corner: a target is allocated tall enough for the pages drawn so far and the rest of
	// its rows are simply never written.
	//
	// A page enters `filled` when a seed writes it or a draw covers it in full; `pages` is the
	// whole page set the texture spans, recomputed when the pool grows it. The first draw into a
	// surface seeds the difference, so the cost is one full-target seed per surface lifetime.
	struct SurfaceTexels
	{
		GSPageBitmap pages; // every page the texture rectangle spans
		GSPageBitmap filled;
		int height = 0; // the texture height `pages` was computed for
	};
	std::vector<SurfaceTexels> m_surface_texels;
	SurfaceTexels& Texels(GSTileSurfaceId id);
	void NoteTextureGeometry(GSTileSurfaceId id, int height);

	// Bring each enabled display buffer's texture up to date with its bytes, as a draw-less tail
	// of the frame's plan. The present is the one reader that takes the texture instead of the
	// bytes, so a page the display surface does not hold -- never written, or written into a
	// surface that has since taken truth of it -- reaches the screen as stale texels however
	// correct the byte model is. Fired from VSync, just before the plan is built.
	void MaterialiseDisplayBuffers();

	// Emit a prep op over `pages` for surface `id` (Writeback or Seed) on the pending draw being
	// accumulated; block masks per page come from the model for writebacks, full for seeds.
	void EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages);

	// Truth on `pages` cannot reach the byte store at all: count it and warn once. The depth plane
	// (no writeback shader) and surfaces whose layout has no byte road are the two roads here.
	void NoteLossyPages(const GSPageBitmap& pages);

	// Truth on `pages` is taken by a surface with no byte road: mark it synced without moving
	// bytes, and count it lossy.
	void LossySteal(const GSPageBitmap& pages);

	// Compose `pages` into the ring for the draw being accumulated, breaking the open pass if the
	// writebacks it emitted cannot be hoisted to that pass's head. The idiom a texture read has
	// always used, shared with the depth claim's spill.
	void ComposeForPendingDraw(const GSPageBitmap& pages, PendingDraw& pd);

	// One draw's colour and depth surfaces claim the same pages -- the game packed FRAME and ZBUF
	// close enough that a single draw's two footprints share them. Only one surface can be the
	// model's holder of a page, so the other's byte truth there is dropped: same move as a
	// road-less steal, counted apart because the cause is the game's memory layout rather than a
	// surface this stage cannot write back yet.
	void AliasSteal(const GSPageBitmap& pages);

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

	// Submit whatever the frame has planned so far, so the resident targets hold every draw up to
	// this point (counted as a mid-frame flush); a no-op when nothing is pending. Separately
	// callable because the flush RETIRES EVERY SYNCED CLAIM -- the ring slots those claims named
	// are spent, and their bytes were never in the CPU shadow -- so a caller whose question is
	// phrased in terms of `synced` has to ask it on the far side of this, never before.
	void FlushPendingPlan();

	// The rect union a run of draws has written into the current colour surface since the last
	// forced pass break: a DATE draw whose rect intersects it reads pixels the pass would also
	// write, so it opens a new pass (a fresh snapshot). Reset on surface change and on break.
	GSTileSurfaceId m_run_surface = kGSTileNoSurface;
	GSVector4i m_run_written = GSVector4i::zero();

	// --- the open pass, as accumulation sees it ----------------------------------------------
	// The executor runs a pass's prep ops before the pass opens, so a writeback emitted for the
	// draw being accumulated is hoisted over every draw of the open pass already planned. That
	// is what these track: what those draws have done, so the hoist can be tested instead of
	// assumed unsafe. A pass has exactly one colour and one depth surface (BuildAndExecutePlan
	// groups on the pair), so two write bitmaps cover every surface its draws can render into.
	// The read side is per RING SLOT, not per page: m_open_read says which pages the pass's
	// draws sample and m_open_read_slot which slot each of them read (entry index + 1), because
	// a page whose slot was superseded since is a different slot and rewriting it is invisible
	// to the earlier read. The key below mirrors the grouping conditions exactly -- keep the two
	// in step -- and everything resets whenever a pass breaks.
	GSTileSurfaceId m_open_color = kGSTileNoSurface;
	GSTileSurfaceId m_open_z = kGSTileNoSurface;
	bool m_open_z_used = false, m_open_z_write = false, m_open_z_test = false;
	GSPageBitmap m_open_color_written;
	GSPageBitmap m_open_z_written;
	GSPageBitmap m_open_read;
	std::array<u16, GS_MAX_PAGES> m_open_read_slot{}; // meaningful only where m_open_read is set

	// The rule-2 targets the open pass's draws sample, in the order they were first bound -- which
	// IS the slot numbering the executor and the shader use, so it is rebuilt the same way at plan
	// build (asserted there). A pass binds at most kMaxTexSourcesPerPass of them; a draw whose
	// source would be one too many opens a pass of its own.
	std::array<GSTileSurfaceId, GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass> m_open_tex_src{};
	u32 m_open_tex_count = 0;

	// The open pass is closed: the next draw accumulated is the first of a new one, so nothing
	// recorded above constrains it. Idempotent, and the state it leaves matches no draw, so it
	// also serves as the initial and between-frames state.
	void BreakOpenPass();

	// Would the writeback ops appended since `first_op` read or write something the open pass's
	// draws already touched? Then they cannot run at that pass's head and their draw must open
	// its own.
	bool WritebackHoistCollides(u32 first_op) const;

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
		u32 writeback_breaks = 0; // draws that opened a pass because their read needed a writeback
		u32 seed_ops = 0;
		u32 seed_pages = 0;
		u32 seed_breaks = 0;     // draws that opened a pass because their target needed seeding
		u32 date_breaks = 0;     // draws that opened a pass because their DATE read needed a fresh snapshot
		u32 snapshots = 0;       // passes that took a snapshot of their target
		u32 self_reads = 0;      // draws sampling pages their own pass target holds (snapshot semantics)
		u32 tex_binds = 0;       // draws served by rule 2: the read window came off a resident target
		u32 tex_bind_breaks = 0; // draws that opened a pass because their bind could not join the open one

		// Rule 3 as probed (ProbeSourceRoad) -- the admission question, asked for every rule-2-declined
		// draw whatever its format. The direct-colour half of `src_eligible` goes on to be built and
		// SERVED (src_served below); the paletted half is still measurement only.
		u32 tex_draws = 0;         // textured draws in a format either road samples (the denominator)
		u32 tex_unsupported = 0;   // TME draws in a format neither road samples: rule 3's format refusal
		u32 src_eligible = 0;      // rule-2-declined draws rule 3 would serve
		u32 src_hit = 0;           // ...served with no build at all (window and stamp both already seen)
		u32 src_rebuild = 0;       // ...served after a rebuild: the window is known and its bytes moved
		u32 src_fresh = 0;         // ...served after a first build: the window was not seen last frame
		u32 src_ref_region = 0;    // refused: REGION_CLAMP / REGION_REPEAT on either axis
		u32 src_mip_draws = 0;     // NOT a refusal: MXL != 0, eligible, served at level 0 like every
		                           // other road serves it today (mip LEVEL SELECTION is M4)
		u32 src_mip_live = 0;      // ...of those, draws where mipmapping is actually active: the ones
		                           // whose correct level is the one M4 will have to choose
		u32 src_ref_capacity = 0;  // refused: the frame's distinct sources exceed the source array
		u32 src_ref_palette = 0;   // refused: first sight of the (index, palette) pair, admitted next frame
		u32 src_ref_pin = 0;       // refused: rebuilding a window an earlier draw of this frame named
		u32 src_distinct = 0;      // distinct source windows the frame would hold

		// Rule 3 as BUILT (MaterialiseSourceRoad): what the cache actually did for the admitted
		// direct-colour windows. These are the real cache's numbers, where the columns above are
		// the probe's model of it -- the two agree or one of them is wrong.
		u32 src_builds = 0;        // materialise passes emitted: the window was fresh or its bytes moved
		u32 src_cache_hits = 0;    // served by an entry whose gen stamp was still current
		u32 src_cache_rescued = 0; // ...whose stamp had moved but whose content token had not
		u32 src_pin_refusals = 0;  // the cache refused a rebuild under a draw this frame has recorded
		u32 src_cap_refusals = 0;  // ...refused because every entry is pinned by this frame
		u32 src_build_failed = 0;  // allocation failed, or the op could not be queued
		u32 src_served = 0;        // draws that TOOK road SOURCE: their fragment stage sampled the image
		u32 src_bind_slots = 0;    // distinct (image, sampler) pairs the frame bound
		u32 src_ref_bind = 0;      // refused: the frame's bind table is full (kMaxSourceSlots)

		// The paletted road's second stage. A window materialises to an INDEX image (counted in
		// src_builds beside the direct-colour ones, since it is the same build); the colour comes from
		// the (index x palette) expansion, and THESE are its columns. High deferrals beside high hits is
		// the designed shape, not a fault: a pair is admitted on second sight across a frame boundary,
		// so a window drawn once in its life never pays for an expansion pass at all.
		u32 src_pal_draws = 0;      // draws admitted on a paletted window (the denominator here)
		u32 src_pal_missing = 0;    // ...whose palette texture could not be built: refused to the byte road
		u32 src_expand_hits = 0;    // served by an expansion the cache already held
		u32 src_expand_builds = 0;  // expansion passes emitted: the pair earned one this frame
		u32 src_expand_defer = 0;   // the pair was first-sighted at BUILD time: refused for one frame
		u32 src_expand_cap = 0;     // ...refused because every expansion entry is pinned by this frame
		u32 src_expand_failed = 0;  // allocation failed, or the op could not be queued

		// The DONOR road: rule 3's build taken off the owner target instead of off the ring. Its
		// draws are counted in src_served beside the ring-built ones -- the road a draw takes is
		// still SOURCE either way, and what changes is where the image came from and what did NOT
		// have to happen for it (no writeback, no ring slots, no page fingerprint).
		u32 src_donor_eligible = 0;  // admitted windows one live target could serve whole
		u32 src_donor_served = 0;    // ...that actually took it: no compose, no ring traffic
		u32 src_donor_builds = 0;    // reinterpretation passes emitted
		u32 src_donor_hits = 0;      // served by an entry whose gen stamp was still current
		u32 src_donor_rescued = 0;   // ...whose stamp had moved but whose TARGET identity had not
		u32 src_donor_refused = 0;   // eligible, then refused downstream (pin, capacity, palette, build)
		u32 src_donor_breaks = 0;    // draws that opened a pass because the build could not hoist
		u32 src_donor_direct = 0;    // direct-colour windows a sole owner holds that rule 2 did not
		                             // take: the population a donor COPY leg would serve, measured
		                             // rather than assumed (this road reinterprets, it does not copy)

		u32 src_extra_calls = 0;   // extra indirect calls the sampled-binding split actually cost
		u32 src_stamp_kept = 0;    // windows also probed last frame whose gen stamp held
		u32 src_stamp_moved = 0;   // ...and whose gen stamp moved (the raw number, C1's: the two
		                           // below are subsets of THIS one, not additions to it)
		u32 src_stamp_rescued = 0; // ...of those, the stamp moved but the pages' bytes hashed
		                           // identical, so the window is unchanged and keeps its identity
		u32 src_stamp_opaque = 0;  // ...of those, the fingerprint could not be taken (GPU-side truth
		                           // under the window), so the move has to be believed. Disjoint
		                           // from rescued; a rebuild the probe counts but cannot prove.
		                           // Rebuilds the probe believes = moved - rescued, of which
		                           // `opaque` are unproven and `moved - rescued - opaque` are
		                           // bytes that demonstrably changed.
		u32 alias_steal_pages = 0; // pages one draw claimed through both its surfaces (FRAME/ZBUF packing)
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
	bool m_warned_alias = false;

	// Set the first time BuildAndExecutePlan finds a non-empty plan and no executor to give it
	// to (the device failed the TileGpu contract at construction -- GSDeviceVK.cpp's
	// tilegpu_device_capable). Every draw is dropped whether this fires or not; it only gates
	// the one-time Console.Error that says so, so a contract-absent device cannot render black
	// with a clean exit code and no explanation.
	bool m_warned_no_executor = false;
};
