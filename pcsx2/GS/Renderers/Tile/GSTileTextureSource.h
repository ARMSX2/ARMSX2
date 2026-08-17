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
	};

	/// The content stamp of `pages` right now (sum of per-page write generations).
	static u64 GenStamp(const GSVramModel& model, const GSPageBitmap& pages);

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
	GSTexture* Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
		const GIFRegTEXA& TEXA, const GSPageBitmap& pages,
		const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1, const Donor* donor = nullptr,
		u64* build_id = nullptr);

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
		GSPageBitmap pages;
		GSTexture* tex = nullptr;
		bool alive = false;
	};

	bool BuildInto(Entry& e, GSLocalMemory& mem, const GIFRegTEX0* level_tex0, u32 levels, const GIFRegTEXA& TEXA,
		const Donor* donor);
	u8* GetScratch(u32 size);

	std::array<Entry, kMaxEntries> m_entries;
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
};
