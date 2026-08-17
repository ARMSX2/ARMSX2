// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"
#include "GS/Renderers/Tile/GSTileClutMirror.h"
#include "GS/Renderers/Tile/GSTileDrawLowering.h"
#include "GS/Renderers/Tile/GSTileExpandedCache.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <array>
#include <memory>
#include <vector>

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
	~GSRendererTile() override;

	void Destroy() override;
	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;
	void Draw() override;
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;
	void PreClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT) override;
	void PostClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT) override;
	void ReadbackTextureCache() override;

private:
	void RecordDrawLogEntry() const;
	GSVector4i ComputeDrawRect() const;
	GSTileDrawInput BuildLoweringInput();

	// -- M3f premise census (dev instrument; runs only under GSDrawLog) --------------
	// Whether a draw-key memo is worth building is one hit rate times one function's
	// cost, because gsTileLowerDraw is the only skippable half (see MeasureMemo).
	struct MemoCensus
	{
		std::array<GSTileDrawInput, 16> lru{};
		GSTileDrawInput prev{};
		u32 lru_count = 0;
		bool have_prev = false;
		u32 draws = 0;
		u32 prev_hit = 0; ///< input identical to the immediately preceding draw's
		u32 lru_hit = 0; ///< input found among the recent ones
		u64 lower_ns = 0;
		u64 lower_calls = 0;
		u64 probe_ns = 0;
		u64 probe_calls = 0;
		u64 build_ns = 0;
		u64 timer_ns = 0; ///< the clock pair's own cost, subtracted from build_ns
		u64 build_calls = 0;
		u32 sink = 0; ///< keeps the timing repetitions from folding away
	};

	bool MeasureMemo(const GSTileDrawInput& in);
	void ReportMemoCensus() const;

	// The native route.
	bool TryNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, GSTileFloorReason& reason);
	bool BuildTilePayload(bool want_zwalk, bool want_twalk, bool want_cwalk, GSTileFloorReason& reason);
	void DeindexVertices();
	/// direct_idx: -1 = tex is an ordinary source; else GSTileSwizzleForms::IndexFormat and
	/// tex is the OWNER texture addressed through the swizzle (direct_idx_params →
	/// cb_ps.TileDirectIdx). pal_direct: pal is the owner texture holding the CLUT's
	/// source words (pal_direct_params → cb_ps.TileDirectPal).
	void SubmitNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, const GIFRegTEX0& fixed_tex0,
		GSTexture* rt, GSTexture* ds, GSTexture* tex, GSTexture* pal, int direct_idx, const GSVector4i& direct_idx_params,
		bool pal_direct, const GSVector4i& pal_direct_params);
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
	// Split by the seam that asked, because the seams have unrelated fixes: the
	// presenter's pull is a footprint question, the floor handoff is a coverage
	// question, and the native route's own sync is an ordering question. (The old
	// frame-boundary policy question — whole-of-truth sync every vsync when the
	// presenter only reads the display — was answered by moving the pull to
	// GetOutput at its exact footprint; VSyncAll remains only where a complete CPU
	// image IS the contract.)
	enum class ReadbackSite : u8
	{
		Transfer, ///< a GIF transfer wrote pages the GPU still owned
		LocalRead, ///< the CPU read local memory back out
		Move, ///< local->local copy, both sides
		VSyncAll, ///< whole-of-truth sync: savestate/switch/teardown (ReadbackTextureCache)
		VSyncDisplay, ///< the presenter's read, pulled at its exact display footprint
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

		/// The same drains split by WHAT the pulled pages were waiting on at the moment
		/// of the pull, read off the model's per-page GPU-write epoch against the
		/// device's submission timeline. This is the measurement behind the three-state
		/// host-sync design (plan D3): a drain over pages whose producing submission the
		/// GPU has already retired ("quiescent") drains the recording buffer for
		/// nothing — its bytes were final before the pull was asked for, and a copy that
		/// waited only on itself would have served it. In-flight pages need a wait on
		/// their own submission, not on everything since. Pending pages — written by
		/// work still in the buffer being recorded — are the only class where the drain
		/// is the price of the architecture rather than of the bookkeeping.
		u32 drains_pending = 0;
		u32 drains_inflight = 0;
		u32 drains_quiescent = 0;
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

		/// Draws whose texture source was copied off the owning target on the device, so
		/// the render-to-texture reason cost nothing. Counted separately from the reasons
		/// because it is the ABSENCE of a stall: it can never appear in a sole column, and
		/// a census that only reports what still stalls cannot distinguish "this reason was
		/// served" from "this reason never arose".
		u32 tex_served = 0;
		/// Of the served draws, those whose source was REINTERPRETED out of a colour
		/// target through the GS swizzle on the device (a palettised view of CT32/CT24
		/// bytes) rather than copied — the format refusal this replaces.
		u32 tex_reinterpreted = 0;
		/// ... and those served with no build at all: the draw addresses the owner
		/// texture through the swizzle in its own fragment shader (PS_TILE_DIRECT_IDX).
		u32 tex_direct = 0;

		/// Why a draw that WOULD have drained for its texture did not get served off the
		/// device. Only draws facing a real drain are counted, so this is a partition of
		/// the render-to-texture bill and not a tally of predicate evaluations.
		///
		/// The order matters and is not the cheap order: sole-ownership is resolved BEFORE
		/// format, so a refusal on format still records whether one target held the whole
		/// window. That is the difference between "these bytes are somewhere else" and
		/// "these bytes are right there in a texture we own but in the wrong arrangement",
		/// and only the second is worth building a conversion for.
		u32 refuse_mip = 0; ///< a mip pyramid; each level would need its own donor
		u32 refuse_no_owner = 0; ///< no single surface holds the whole window
		u32 refuse_self_target = 0; ///< the owner IS this draw's target (admitted feedback) — sampling it while rendering into it is a device hazard
		u32 tex_core_served = 0; ///< admitted feedback windows served by the sampled core's sole owner (subrect donor)
		u32 feedback_admitted = 0; ///< window-overlap draws served natively because the sampled core is page-disjoint from the write
		u32 refuse_format = 0; ///< sole owner found, but its bytes are not this format's
		u32 refuse_format_same_base = 0; ///< ... of those, how many start at the owner's own base
		u32 refuse_layout = 0; ///< sole owner found, but its pixel space is not the window's
		u32 refuse_geometry = 0; ///< sole owner found, but it does not materialize the window
	};

	void ReportReadbackCensus();

	// -- Pass-structure simulator (EmuCore/GS/TilePassSim; gsrunner -tilepasssim) ------
	// Offline design instrument: scores the GS-semantic minimum pass structure of a run
	// (GSTilePassSim.h) without changing anything it observes. Shares the ledger's
	// discipline: an arm with the pin on is an attribution run, never a timed one.
	GSTilePassSim m_pass_sim;
	void PassSimObserveDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void ReportPassSim();

	// Spill machinery shared by every CPU-side seam.
	bool ReadbackModelPages(const GSPageBitmap& pages, ReadbackSite site);
	void InvalidateSwTexCache(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages);
	void SyncAllTruthToCpu(bool palettes);
	void SpillForFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);
	void ObserveFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r);

	// -- The palette on the device (GSTileClutMirror) ---------------------------------
	// A CLUT loaded off pages a native draw rendered is gathered out of the owner's
	// texture into a device palette instead of draining the page to CPU memory; the
	// mirror records which sixteen-entry slots the CPU's GSClut then holds stale, and
	// every CPU consumer of the palette syncs through SyncClutToCpu first.

	/// The blocks of the CLUT load in progress whose readback InvalidateLocalMem skipped
	/// because one colour surface owns them: PostClutLoad turns them into a device
	/// gather, or PreClutLoad reads them back after all if the load's shape is not one
	/// the gather serves.
	struct DeferredClutBlocks
	{
		GSPageBitmap pages;
		u32 blocks = 0;
		GSTileSurfaceId owner = kGSTileNoSurface;
		bool mixed = false; ///< some blocks deferred, some not — the CPU route for all
	};

	struct GpuPalette
	{
		u32 handle;
		GSTexture* tex; ///< the gathered N×1 palette, or null while it lives only in the owner
		u32 entries; ///< 16 or 256
		GSTileSurfaceId owner; ///< the surface whose texture holds the source words
		GSPageBitmap pages; ///< the source pages
		u64 stamp; ///< their content stamp at load time (GSTileTextureSource::GenStamp)
		u32 cbp; ///< the load's CBP
		u32 owner_bp; ///< the owner's layout, for the address arithmetic
		u32 owner_bwpg;
	};

	/// A sixteen-entry window into a 256-entry device palette, for a four-bit draw
	/// reading one slot of an eight-bit device load.
	struct GpuPaletteView
	{
		u32 handle;
		u32 first_texel;
		GSTexture* tex;
	};

	struct PaletteChoice
	{
		GSTexture* gpu = nullptr; ///< bind this palette; null = the CPU CLUT is the palette
		bool cpu_current = true; ///< the CPU CLUT holds the right entries for this draw
		bool direct = false; ///< `gpu` is the OWNER texture: sample it through the CLUT's word order (PS_TILE_DIRECT_PAL)
		GSVector4i direct_params = GSVector4i::zero(); ///< cb_ps.TileDirectPal when direct
	};

	/// The palette a palettised draw under TEX0 should use. With `allow_sync`, a mixed
	/// provenance is resolved by syncing the device slots down first (a readback), and
	/// an ungathered device palette is either sampled directly (when its source is
	/// intact and is not the draw's own target, given as draw_fb/draw_z) or gathered;
	/// without it the answer only says whether the CPU CLUT can be trusted (the
	/// lowering's alpha range must go conservative when it cannot).
	/// `allow_direct` says the draw can sample a device palette through the tile
	/// legs' word-order arithmetic (it carries the coordinate walk); without it a
	/// device-resident palette is gathered into an ordinary palette texture first.
	PaletteChoice ResolvePalette(const GIFRegTEX0& TEX0, bool allow_sync, GSTileSurfaceId draw_fb, GSTileSurfaceId draw_z,
		bool allow_direct = true);
	/// Copies every device-authoritative slot's entries the CPU does not yet hold into
	/// GSClut (a readback per device palette involved, gathering first where needed).
	void SyncClutToCpu();
	GpuPalette* FindGpuPalette(u32 handle);
	GSTexture* GpuPaletteViewFor(u32 handle, u32 first_texel);
	bool MaterializePalette(GpuPalette& gp);
	void MaterializePalettesOn(const GSPageBitmap& pages);
	void PruneGpuPalettes();
	void ReleaseGpuPalettes();
	/// Whether the device can address a colour target through the GS swizzle in the
	/// fragment shader (the direct legs); probed once.
	bool DirectSamplingServes();
	bool DirectPaletteServes();

	struct ClutCensus
	{
		u32 loads = 0; ///< palette loads executed
		u32 deferred = 0; ///< ... whose source blocks were all on one colour surface
		u32 gathered = 0; ///< ... gathered on the device (no readback)
		u32 refused_shape = 0; ///< deferred, but the load's shape is not served: read back after all
		u32 refused_mixed = 0; ///< deferred blocks beside undeferrable ones: read back after all
		u32 gpu_palette_draws = 0; ///< native draws that bound a device palette
		u32 direct_palette_draws = 0; ///< ... of which sampled the owner directly (no gather)
		u32 materialized = 0; ///< device palettes actually gathered (a pass of their own)
		u32 mixed_syncs = 0; ///< draws whose slots straddled loads and synced first
		u32 sync_backs = 0; ///< device palettes copied down to the CPU CLUT
		u32 sync_back_drains = 0; ///< ... of which drained the frame's buffer
	};

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
		/// Coverage of the instrument over the thing it instruments. A native draw the
		/// oracle does not compare is also a native draw whose result is never restored,
		/// so a gap here does not merely thin the ledger — it means an oracle run stops
		/// composing into the software frame, and every whole-run control built on that
		/// equivalence quietly stops being valid. Reported at teardown beside the census.
		u32 native_draws = 0;
		u32 compared_draws = 0;
		GSPageBitmap fp; ///< the compared write footprint (FRAME pages | ZBUF pages)
		GSTileSurfaceLayout fb_l{};
		GSTileSurfaceLayout z_l{};
		u32 sync_pages = 0; ///< pages the input sync pulled; 0 == perturbation-free row
		/// `raw` is the native arm BEFORE its result is pulled out of the GPU: the same
		/// footprint, captured between the draw and the readback. Differences the readback
		/// introduced are exactly gpu-minus-raw, which is what separates a draw that wrote
		/// the wrong bytes from a readback that published bytes CPU memory never held.
		OracleSnapshot pre, raw, gpu, sw;

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
	GSTilePaletteCache m_palette_cache;
	GSTileExpandedCache m_expand_cache;
	GSTileClutMirror m_clut_mirror;
	DeferredClutBlocks m_clut_deferred;
	std::vector<GpuPalette> m_gpu_palettes;
	std::vector<GpuPaletteView> m_gpu_palette_views;
	std::unique_ptr<GSDownloadTexture> m_clut_download;
	u32 m_gpu_palette_next = 1;
	bool m_clut_gather_serves = true; ///< cleared when the device refuses a gather
	bool m_direct_probed = false;
	bool m_direct_serves = false;
	bool m_direct_clut_serves = false;
	ClutCensus m_clut_census{};

	// Per-primitive plane payload for the current draw: the depth walk's blocks,
	// the texture-coordinate walk's blocks and the colour/fog walk's blocks
	// concatenated into one upload, built per draw and consumed synchronously by
	// RenderHW. Empty = no walk. The offsets are uvec4 element indices into it, or
	// NoWalk. m_twalk_fst is the software renderer's effective sel.fst for the draw
	// (the walk is a truncating integer DDA under it, a float one otherwise).
	static constexpr u32 NoWalk = 0xFFFFFFFFu;
	std::vector<u32> m_tile_payload;
	u32 m_zwalk_at = NoWalk;
	// The depth payload's form: false = the soft-float64 walk block (20 words per
	// primitive, byte-exact), true = the closed plane block (12 words, the fast
	// profile's plane-exact contract). Selects PS_TILE_ZWALK 1 vs 2.
	bool m_zwalk_plane = false;
	u32 m_twalk_at = NoWalk;
	u32 m_cwalk_at = NoWalk;
	bool m_twalk_fst = false;
	u32 m_native_budget_used = 0; ///< TileNativeDrawLimit's counter
	// The draw's vertices as GSRendererSW converts them (GSVertexSW::s_cvb), which is
	// what every walk payload seeds from; grown on demand, vector-aligned.
	GSVertexSW* m_sw_verts = nullptr;
	u32 m_sw_verts_cap = 0;
	GSVertexSW* SwVerts(u32 count);
	GSVramModel::RectFootprint m_rect_fp; // scratch for transfer/move footprints
	OracleState m_oracle;
	MemoCensus m_memo;

	std::array<ReadbackCounters, static_cast<u32>(ReadbackSite::Count)> m_readback{};
	NativeSyncReasons m_native_sync{};
	u32 m_readback_frames = 0;
	/// Cleared the first time the device refuses to reinterpret a target as an index
	/// texture (no pipeline: a backend without the route, or a shader that failed to
	/// build); the route is then never offered again this session.
	bool m_reinterpret_serves = true;
};

MULTI_ISA_UNSHARED_END
