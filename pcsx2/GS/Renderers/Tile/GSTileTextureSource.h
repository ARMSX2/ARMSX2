// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"

#include <array>
#include <memory>

class GSLocalMemory;
class GSVramModel;

/// The build road for a caller that materialises on the DEVICE rather than deswizzling through CPU
/// memory. The cache keeps everything it already owned — identity, allocation, invalidation,
/// eviction, lifetime — and the builder does exactly one thing: fill the texture it is handed with
/// the window's texels. The TileGpu renderer implements it by queueing a materialise pass into the
/// frame's prep-op stream; nothing in the cache knows or cares that the fill is deferred, because
/// the pin discipline (SetFrame) is what makes deferral safe, not the build road.
class GSTileSourceBuilder
{
public:
	virtual ~GSTileSourceBuilder() = default;

	/// Fill `tex` — a Format::Color render target, tw x th texels of the window at TEX0/TEXA — with
	/// the window's texels. Returning false fails the build: the cache drops the entry and the
	/// caller gets null, exactly as an allocation failure does.
	virtual bool BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) = 0;
};

// The Tile renderer's texture sources: GPU copies of texture windows built straight
// from CPU local memory. A direct-colour window (16/24/32-bit) is deswizzled to RGBA8
// with the TEXA expansion applied by the swizzle readers on the CPU — the "AEM
// expansion already done by the CPU" leg of the backend, the one the gs-texture
// capture certified exact in every arm for nearest sampling. A PALETTISED window is
// an INDEX texture (one byte per texel, the palette-less readers) and the palette is
// a separate device texture the shader indexes with it (GSTilePaletteCache) — so the
// index texture is independent of the palette, and one build serves every palette
// the game cycles through it. There is no target aliasing here: a source whose pages
// carry GPU truth is spilled to CPU memory by the caller before it ever reaches this
// class unless the caller can prove a device-side substitute (see Donor), and a
// source overlapping the draw's own write footprint floors (M4 feedback territory).
//
// Caching, not correctness, is the point of the class: re-deswizzling every draw
// would put a CPU cost on exactly the titles the GS-thread tripwire watches. An
// entry is identified by the register view of the window (TBP0/TBW/PSM/TW/TH) and
// the TEXA bits when the format expands through them; it is invalidated by content
// through the interval model's per-page write generations — the sum of cpu+gpu
// generations over the window's pages is monotone under every mutation of those
// bytes, so stamp inequality is exactly "some byte moved".
class GSTileTextureSource
{
public:
	GSTileTextureSource();
	~GSTileTextureSource();

	/// A GPU-side substitute for the CPU deswizzle: a surface texture whose pixel space
	/// IS this window's, so the source is built by a region copy on the device instead of
	/// a drain-and-deswizzle through CPU memory. The render-to-texture case — a game
	/// renders a target and then samples it — and the one the readback census named as
	/// the largest single source of GPU stalls on the native route.
	///
	/// The equivalence is the CALLER's to prove, and it is exact rather than approximate:
	/// the whole window is GPU-owned by that one surface at whole-page granularity, and
	/// either the window's layout matches the surface's exactly and the format is one
	/// whose texture bytes already are what the swizzle reader would have produced (a
	/// straight copy), or the window is a palettised view of the surface's CT32/CT24
	/// bytes and the device reinterprets them through the GS swizzle into an index
	/// texture (`reinterpret`, GSDevice::TileReinterpretIndex — the arithmetic is
	/// GSTileSwizzleForms). This class performs the copy or the conversion and nothing else.
	struct Donor
	{
		GSTexture* tex = nullptr;
		int width = 0; ///< donor pixels available; a copied window must fit inside them
		int height = 0;
		bool reinterpret = false; ///< build by TileReinterpretIndex instead of a copy
		GSDevice::TileReinterpretParams params = {}; ///< both layouts, when reinterpreting
		/// Non-empty: the donor holds only THIS sub-rectangle of the window (the
		/// admitted feedback draw's sampled core, whose sole owner the caller
		/// proved). The build copies the rect exactly and backfills the rest with
		/// a scaled stretch of it — defined, plausible bytes for the margin taps
		/// the admission already prices — and the entry records the rect as its
		/// validity, so a draw needing more rebuilds instead of reading filler.
		GSVector4i copy_rect = GSVector4i::zero();
		/// >= 0: the window reads a CT24 view of the owner's CT32 bytes, so the
		/// copied alpha would be the target's raw byte where every consumer (walk
		/// legs and sampler alike) expects the TEXA expansion baked in, exactly
		/// as the CPU deswizzle bakes it. The build clears alpha to this value
		/// and copies RGB only. The caller guarantees AEM is off — with AEM the
		/// expansion is per-texel and no constant bake exists.
		int fill_alpha = -1;
	};

	/// The content stamp of `pages` right now (sum of per-page write generations).
	static u64 GenStamp(const GSVramModel& model, const GSPageBitmap& pages);

	/// The identity of a window's CONTENT, as opposed to of its registers. A moved gen stamp says
	/// some byte on the window's PAGES was written, not that any texel changed, and on the corpus
	/// those are not the same number: games re-upload identical texture data every frame. So a
	/// rebuild is checked against this first, and a match serves the entry that already exists.
	///
	/// It is a (source, value) pair rather than a bare number because different windows have their
	/// truth in different places, and the numbering spaces must not meet. A window the CPU shadow
	/// holds gets a fingerprint of its page bytes. A window whose current truth is a rendered
	/// target has no honest fingerprint at all — ProbePageContent declines and returns zero — and
	/// the road that serves those keys on the OWNING TARGET's identity and version instead, which
	/// the page model already tracks. Tagging the source is what lets both live in one field
	/// without a target version ever being read as a page hash.
	struct ContentToken
	{
		enum class Source : u8
		{
			None = 0,   ///< cannot say: never equal to anything, including another None
			Pages = 1,  ///< fingerprint of the window's page bytes (ProbePageContent)
			Target = 2, ///< the owning target's id and version — the donor road's identity
		};
		Source source = Source::None;
		u64 value = 0;

		bool Known() const { return source != Source::None && value != 0; }
		bool operator==(const ContentToken& o) const { return Known() && source == o.source && value == o.value; }
	};

	/// The register-side identity of a texture window: everything an entry is keyed on
	/// except the content stamp. Exposed so a caller can group draws by the source they
	/// WOULD take without asking for one to be built.
	struct WindowKey
	{
		u64 reg = 0;
		u64 aux = 0;
		u64 mip[2] = {0, 0};

		bool operator==(const WindowKey& o) const
		{
			return reg == o.reg && aux == o.aux && mip[0] == o.mip[0] && mip[1] == o.mip[1];
		}
	};
	static WindowKey KeyFor(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1);

	/// The question Lookup answers internally, asked without the build: as the cache
	/// stands right now, would this window be served, and if not, is that because the
	/// entry's stamp has moved (a rebuild) or because there is no entry at all (a first
	/// build)? Const and free of side effects — no counter moves, no LRU stamp, nothing
	/// is allocated.
	///
	/// One deliberate conservatism: Lookup can serve a MOVED stamp without rebuilding
	/// when the window's pages hash to the bytes the last build read (RebuildsSamePages).
	/// That tier costs an 8 KB hash per written page, so the probe does not run it and
	/// reports `would_rebuild` — an upper bound on the rebuilds a caller will actually pay.
	/// A caller that needs the tier's answer rather than the bound asks `ProbePageContent`
	/// for the fingerprint and compares it against the one it recorded itself.
	struct ProbeResult
	{
		bool hit = false;           ///< an entry matches and its stamp is current: served with no work
		bool would_rebuild = false; ///< an entry matches and its stamp has moved: served after a rebuild
		u64 build_id = 0;           ///< the entry's content identity (hits only; zero otherwise)
		u64 gen_stamp = 0;          ///< the window's stamp the probe compared against, so a caller need not walk the pages twice
	};
	ProbeResult Probe(const GSVramModel& model, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		const GSPageBitmap& pages, const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1,
		const GSVector4i* sample_core = nullptr) const;

	/// The tier `Probe` skips, on its own: the fingerprint of the RAW bytes under a window,
	/// which is what a moved gen stamp has to be checked against before it is believed. A
	/// game re-uploading identical texture data every frame moves the stamp without moving a
	/// texel, and on the corpus that is the common case, not the corner — so a caller that
	/// derives a build identity from the stamp alone reports churn that the cache itself
	/// would not have (RebuildsSamePages caught 97.7% of GT4's rebuilds this way).
	///
	/// Not const: the per-page hashes memoise, keyed on the page's write generations, so a
	/// page rewritten with identical bytes is hashed once per frame however many windows
	/// read it. Cost is one 8 KB hash per page whose generations moved since it was last
	/// hashed, plus the fold.
	///
	/// Returns 0 when the tier cannot answer honestly — some page under the window holds
	/// truth on a plane that has not been synced back to the CPU shadow, so the bytes here
	/// are not the bytes anything would build from. Lookup gets that guarantee from its
	/// caller's spill; a probe spills nothing, so it asks the model and declines rather than
	/// filing pre-spill bytes under the current generations, which is the poison the donor
	/// route's refusal exists to avoid. 0 never collides with a real fingerprint.
	u64 ProbePageContent(const GSLocalMemory& mem, const GSVramModel& model, const GSPageBitmap& pages);

	/// Returns the source for this texture window — RGBA8 for a direct-colour format,
	/// an 8-bit index texture for a palettised one — building or rebuilding it if
	/// absent or stale, or null on allocation failure. The caller has already spilled
	/// GPU truth under the window.
	///
	/// For mip draws, level_tex0 carries min(MXL,6)+1 register views (level 0 first,
	/// then GetTex0Layer's MIPTBP-derived views) and the built texture holds one GPU
	/// mip level per entry — the GS floors level sizes at one texel exactly like a
	/// GPU chain, so the geometries agree by construction (the route floors the rare
	/// pyramid deeper than the base). `pages` must already union every level's
	/// footprint.
	///
	/// `donor`, when given, builds the source by copying on the device instead of reading
	/// CPU memory — see Donor. It is consulted only on a BUILD; a cache hit is a hit
	/// either way, because the content stamp is a property of the bytes and not of the
	/// route that last fetched them.
	///
	/// `build_id`, when given, receives the returned texture's content identity:
	/// equal ids mean the same texel bytes. A rebuild whose deswizzled bytes hash
	/// identical to the previous build KEEPS the id (and skips its upload); a
	/// changed build, an evicted slot reused, or a device-built source gets a new
	/// one. Derived caches (the palette-expanded RGBA cache) key on it.
	/// `sample_core`, when given, is the rectangle of the window this draw's
	/// samples are proven to stay inside (plus the accepted one-texel margin): a
	/// hit additionally requires the cached entry's valid region to contain it,
	/// so a texture built from a subrect donor never serves a draw that samples
	/// beyond what was copied.
	GSTexture* Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
		const GIFRegTEXA& TEXA, const GSPageBitmap& pages,
		const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1, const Donor* donor = nullptr,
		u64* build_id = nullptr, const GSVector4i* sample_core = nullptr);

	/// The frame the pin discipline stamps entries with, and the whole of that discipline's
	/// configuration. Zero — the default, and what the Tile renderer leaves it at — turns the
	/// discipline OFF: no entry is ever stamped, every pin branch is inert, and the class behaves
	/// exactly as it did before it existed.
	///
	/// It exists for a renderer that RECORDS draws and issues them later. Tile looks a texture up
	/// and draws with it immediately; TileGpu looks it up during accumulation and issues nothing
	/// until the plan executes at VSync, so a texture a recorded draw names must still hold the
	/// right bytes at the END of the frame. Three things would break that — a rebuild landing in
	/// the same entry and the same texture, an LRU eviction taking the slot, and a Recycle handing
	/// the texture to the next CreateTexture — and one mechanism stops all three: an entry a
	/// caller has been given this frame is stamped, and a stamped entry is never rebuilt in place,
	/// evicted or recycled. A lookup that would have to do any of those REFUSES (counted), the
	/// caller falls back to whatever road it had, and the rebuild happens next frame naturally.
	///
	/// The caller advances the frame when the recorded plan has EXECUTED, not at the video frame
	/// boundary — from that moment the textures the last plan named are free again.
	void SetFrame(u64 frame) { m_frame = frame; }

	/// What LookupBuilt did, so a caller can count the road rather than infer it from a pointer.
	enum class BuiltOutcome
	{
		Hit,             ///< an entry matched and its stamp was current: no work at all
		Rescued,         ///< the stamp had moved but the content token had not: served, nothing rebuilt
		Built,           ///< the builder was called (a fresh window, or one whose content moved)
		RefusedPinned,   ///< the entry would have been rebuilt in place under a recorded draw
		RefusedCapacity, ///< every entry is pinned this frame; nothing may be evicted
		Failed,          ///< allocation failed, or the builder refused
	};

	/// The device-side build road: same identity, same invalidation predicate and same cache as
	/// Lookup, but the texels are produced by `builder` instead of by a CPU deswizzle, so this
	/// never touches GSLocalMemory. The texture is a Format::Color render target at the window's
	/// tw x th, which is what a build-on-the-GPU pass renders into.
	///
	/// `content` is the window's content identity as the CALLER can prove it (see ContentToken),
	/// and it is the tier that decides whether a moved gen stamp is a real rebuild. It must be
	/// taken BEFORE the caller composes or spills anything under the window: after that, a page
	/// fingerprint is a fingerprint of bytes the composition has already superseded. An unknown
	/// token is never equal to anything, so it degrades to "rebuild", never to a wrong serve.
	///
	/// Returns null on every refusal and on failure; `outcome` says which, and the caller counts
	/// it. A refusal is not an error — it is the fail-closed leg of the pin discipline, and the
	/// caller's other road is expected to be correct on its own.
	GSTexture* LookupBuilt(const GSVramModel& model, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		const GSPageBitmap& pages, GSTileSourceBuilder& builder, const ContentToken& content,
		u64* build_id = nullptr, BuiltOutcome* outcome = nullptr);

	/// Rebuilds refused because the entry is pinned by a recorded draw of this frame.
	u64 PinRefusals() const { return m_pin_refusals; }
	/// Lookups refused because every entry in the cache is pinned by this frame.
	u64 CapacityRefusals() const { return m_capacity_refusals; }
	/// Entries currently stamped with the live frame — the working set the pin holds down.
	u32 PinnedEntries() const;

	/// Builds served off the device rather than out of CPU memory (copies and
	/// reinterpretations both; the latter counted again below).
	u64 DonorBuilds() const { return m_donor_builds; }
	/// Of those, index textures the device reinterpreted out of a colour target.
	u64 ReinterpretBuilds() const { return m_reinterpret_builds; }

	/// Of the CPU rebuilds forced by a moved gen_stamp, how many deswizzled to
	/// EXACTLY the bytes already on the device. Measured 97.7% on GT4-fast (148 of
	/// 152 rebuilds per frame): a moved stamp says some byte on the window's PAGES
	/// moved — games re-upload identical texture data every frame — not that the
	/// window's texels did. A same-bytes rebuild skips its device upload and keeps
	/// its build id, which is what lets derived caches ride out the churn.
	u64 RebuildsSameBytes() const { return m_rebuilds_same_bytes; }
	u64 RebuildsNewBytes() const { return m_rebuilds_new_bytes; }

	/// Rebuilds refused one tier earlier still: the stamp moved but the raw bytes
	/// of the window's pages hashed identical to the last CPU build, so the
	/// deswizzle itself was skipped, not just the upload. Page hashes live in one
	/// per-page array revalidated lazily off the model's write generations, so a
	/// page rewritten with identical bytes is hashed once per frame however many
	/// windows read it — where the deswizzle it replaces ran once per WINDOW,
	/// gather-reading the swizzled layout. Same-bytes-rebuild counts above only
	/// see the residue this tier lets through (a page that changed outside the
	/// window's texels).
	u64 RebuildsSamePages() const { return m_rebuilds_same_pages; }

	/// Recycles every cached texture (reset / teardown / hot-switch).
	void Clear();

	u64 Hits() const { return m_hits; }
	u64 Builds() const { return m_builds; }

private:
	// 64 capacity-thrashed on GT4: 418 window lookups per frame over a working set
	// past 64 meant 208 fresh builds PER FRAME — each a full deswizzle plus an
	// upload — with rebuild-in-place (a genuinely moved stamp) measured at 2/frame.
	// The deswizzle bill this created is visible in the SD865 GS-thread profile
	// (ReadTexture32/24). Capacity is CPU-cheap (the scan compares four words); the
	// device memory is bounded by the working set either way, because a thrashing
	// cache allocates the same textures every frame instead of holding them.
	static constexpr u32 kMaxEntries = 512;

	struct Entry
	{
		u64 reg_key = 0;
		u64 aux_key = 0;
		u64 mip_key[2] = {0, 0}; ///< exact pack of the level TBP0/TBW words + level count ({0,0} = single level)
		u64 gen_stamp = 0;
		u64 last_use = 0;
		u64 build_id = 0; ///< stamped when a build CHANGES content; equal ids = same texel bytes
		u64 content_hash = 0; ///< XXH3 of the last CPU build's texel rows (0 = none / device-built)
		ContentToken content;  ///< the content identity this entry was last built under (see ContentToken)
		u64 pinned_frame = 0; ///< the frame a caller was handed this entry in; 0 = never (and the discipline off)
		GSVector4i valid_rect = GSVector4i::zero(); ///< region holding real window bytes (subrect donors copy less than the window)
		GSPageBitmap pages;
		GSTexture* tex = nullptr;
		bool alive = false;
	};

	/// Is this entry held down by a draw the caller has already recorded but not yet issued?
	/// False for every entry while m_frame is zero, which is what keeps the discipline invisible
	/// to a renderer that draws immediately.
	bool Pinned(const Entry& e) const { return m_frame != 0 && e.pinned_frame == m_frame; }

	// One cached content hash per GS page, revalidated lazily: the hash is current
	// exactly while the page's write generations match the ones it was taken under.
	// The generations key the bytes from this class's viewpoint because Lookup's
	// contract has the caller spill GPU truth under the window first — so at every
	// hashing site the page's CPU bytes are the post-spill bytes for those
	// generations, and a readback that installs bytes without bumping a generation
	// can only install the same bytes a previous spill at those generations did.
	struct PageContent
	{
		u64 hash = 0; ///< XXH3 of the page's 8 KB (|1 — 0 means never hashed)
		u32 seen_cpu = 0;
		u32 seen_gpu = 0;
	};

	u64 PageContentStamp(const GSLocalMemory& mem, const GSVramModel& model, const GSPageBitmap& pages);
	bool BuildInto(Entry& e, GSLocalMemory& mem, const GIFRegTEX0* level_tex0, u32 levels, const GIFRegTEXA& TEXA,
		const Donor* donor);
	bool BuildDeviceSource(Entry& e, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, GSTileSourceBuilder& builder,
		int tw, int th);
	u8* GetScratch(u32 size);

	std::array<Entry, kMaxEntries> m_entries;
	std::array<PageContent, GS_MAX_PAGES> m_page_content;
	std::unique_ptr<u8[]> m_scratch;
	u32 m_scratch_size = 0;
	u64 m_use_counter = 0;
	u64 m_build_counter = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
	u64 m_donor_builds = 0;
	u64 m_reinterpret_builds = 0;
	u64 m_rebuilds_same_bytes = 0;
	u64 m_rebuilds_new_bytes = 0;
	u64 m_rebuilds_same_pages = 0;
	u64 m_frame = 0; ///< the pin discipline's clock; 0 = off (see SetFrame)
	u64 m_pin_refusals = 0;
	u64 m_capacity_refusals = 0;
};
