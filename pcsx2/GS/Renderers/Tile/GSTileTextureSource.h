// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
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
	/// the window's layout matches the surface's exactly, the whole window is GPU-owned by
	/// that one surface, and the format is one whose texture bytes already are what the
	/// swizzle reader would have produced. This class performs the copy and nothing else.
	struct Donor
	{
		GSTexture* tex = nullptr;
		int width = 0; ///< donor pixels available; the window must fit inside them
		int height = 0;
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
	GSTexture* Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
		const GIFRegTEXA& TEXA, const GSPageBitmap& pages,
		const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1, const Donor* donor = nullptr);

	/// Builds served off the device rather than out of CPU memory.
	u64 DonorBuilds() const { return m_donor_builds; }

	/// Recycles every cached texture (reset / teardown / hot-switch).
	void Clear();

	u64 Hits() const { return m_hits; }
	u64 Builds() const { return m_builds; }

private:
	static constexpr u32 kMaxEntries = 64;

	struct Entry
	{
		u64 reg_key = 0;
		u64 aux_key = 0;
		u64 mip_key[2] = {0, 0}; ///< exact pack of the level TBP0/TBW words + level count ({0,0} = single level)
		u64 gen_stamp = 0;
		u64 last_use = 0;
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
	u64 m_hits = 0;
	u64 m_builds = 0;
	u64 m_donor_builds = 0;
};
