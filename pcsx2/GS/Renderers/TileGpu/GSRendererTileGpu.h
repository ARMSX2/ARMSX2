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
		// Explicit tail padding, not slack: alignas(16) would pad the C++ side to 144 anyway, while
		// the shader's std430 array stride is a multiple of 8, so an implicit tail would put the two
		// sides on different strides and every row but the first would be read from the wrong place.
		u32 pad0_;
	};
	static_assert(sizeof(StateRow) == 144, "TileGpu StateRow must be 144 bytes to match tilegpu.glsl std430");

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

		// Rule 3 of the same road, PROBED and not taken (see ProbeSourceRoad): the frame-wide
		// slot the materialised source of this draw's window would occupy, or kNoSourceSlot
		// where rule 3 refused. Read by nothing but the counters -- no state row carries it and
		// no descriptor is written from it, which is what makes this chunk a no-op on output.
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
	std::vector<u32> m_plan_tex_slots; // one per m_plan_draws entry (GSTileGpuPassPlan::tex_slots)
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

	// --- rule 3, the source cache: BUILT, not yet SAMPLED -------------------------------------
	// The three library caches the materialised-source road is built out of. Direct-colour windows
	// the probe admits are now MATERIALISED -- an ordinary RGBA8 image, built on the device out of
	// the same ring bytes the flat road reads, cached and invalidated by the same predicate. What
	// is still absent is the other half: no state row names a source, no descriptor binds one, and
	// every draw takes exactly the road it took before, so the images are correct-by-construction
	// and cost nothing but their build. That is deliberate -- it puts the build, the cache
	// lifetime and the pin discipline under the corpus before the sampled road can hide a defect
	// in any of them behind moved pixels.
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

	// The emission half of rule 3's build. The cache owns what to build, the identity it is
	// filed under and when it dies; this owns how the texels get there -- a materialise pass
	// queued into the frame's prep-op stream, executed at the head of the emitting draw's pass.
	// The split is the point: the cache is library code that knows nothing about deferred
	// execution, and the renderer is where deferred execution lives.
	struct SourceMaterialiser final : public GSTileSourceBuilder
	{
		GSRendererTileGpu* r = nullptr;
		u32 epoch = 0; ///< the page-table epoch the emitting draw reads through
		bool BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) override;
	};

	// Queue the pass that fills `tex` with the window at tex0/texa, reading the ring at `epoch`.
	// Emitted AFTER the window's composition ops so array order puts it behind them at the pass
	// head; safe to hoist there for the same reason a seed is, since the ring is staged whole-frame
	// and the composition it depends on is in the same op range ahead of it.
	bool EmitMaterialiseOp(GSTexture* tex, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 epoch);

	// Build (or find) the materialised source for a window the probe admitted. Direct colour only
	// at this chunk -- a paletted window's index texture and its palette expansion are a separate
	// pair of passes. `window` indexes m_probe_windows.
	void MaterialiseSourceRoad(u32 window, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, u32 epoch);

	static constexpr u32 kNoSourceSlot = 0xFFFFFFFFu;
	// The descriptor array rule 3 will bind through (design §6). A frame needing more distinct
	// sources than this refuses the overflow to the byte road -- counted, so the corpus says
	// whether 64 is the right number before anything is built against it.
	static constexpr u32 kMaxSourceSlots = 64;

	// One distinct texture window the frame probed, in first-appearance order.
	struct ProbedWindow
	{
		GSTileTextureSource::WindowKey key;
		u64 stamp = 0;     ///< GSTileTextureSource::GenStamp of the window's pages at first sight
		u64 content = 0;   ///< GSTileTextureSource::ProbePageContent of the same pages (0 = not asked / declined)
		u64 build_id = 0;  ///< content identity: carried over while the BYTES hold, stamp or no stamp
		u32 slot = kNoSourceSlot; ///< its source-array slot, once a draw was admitted on it
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
	// pair whether it would be admitted, and either admits the draw (giving its window a source
	// slot) or counts one named refusal. Called only where rule 2 declined, and it writes nothing
	// but counters and pd.src_slot -- the BUILD is a separate act, after the window's bytes have
	// been composed. Returns the admitted window's index in m_probe_windows, or kNoSourceSlot.
	u32 ProbeSourceRoad(PendingDraw& pd, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, bool paletted,
		bool mip_active);

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

		// Rule 3 as probed (ProbeSourceRoad) -- what the source cache WOULD have done. Nothing
		// here changes a draw's road; the whole block is measurement.
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

		u32 src_extra_calls = 0;   // extra indirect calls splitting the runs on the source would cost
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
