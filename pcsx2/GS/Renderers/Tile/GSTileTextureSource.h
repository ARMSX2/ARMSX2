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

// The Tile renderer's texture sources: RGBA8 GPU copies of texture windows built
// straight from CPU local memory, with the palette and TEXA expansion applied by the
// swizzle readers on the CPU. There is deliberately no GPU-side palette, no format
// variants, and no target aliasing here — a source whose pages carry GPU truth is
// spilled to CPU memory by the caller before it ever reaches this class, and a
// source overlapping the draw's own write footprint floors (that is M4 feedback
// territory). The shader therefore always sees a plain color texture, which is the
// "index and AEM expansion already done by the CPU" leg of the backend — the one
// the gs-texture capture certified exact in every arm for nearest sampling.
//
// Caching, not correctness, is the point of the class: re-deswizzling every draw
// would put a CPU cost on exactly the titles the GS-thread tripwire watches. An
// entry is identified by the register view of the window (TBP0/TBW/PSM/TW/TH), the
// TEXA bits when the format expands through them, and the palette identity (CLUT
// write generation + the read-side CPSM/CSA/TEXA) for palettised formats; it is
// invalidated by content through the interval model's per-page write generations —
// the sum of cpu+gpu generations over the window's pages is monotone under every
// mutation of those bytes, so stamp inequality is exactly "some byte moved".
class GSTileTextureSource
{
public:
	GSTileTextureSource();
	~GSTileTextureSource();

	/// The content stamp of `pages` right now (sum of per-page write generations).
	static u64 GenStamp(const GSVramModel& model, const GSPageBitmap& pages);

	/// Returns the RGBA8 source for this texture window, building or rebuilding it
	/// if absent or stale, or null on allocation failure. The caller has already
	/// spilled GPU truth under the window and, for palettised formats, refreshed the
	/// CLUT read buffer (GSClut::Read32) — pal_gen is the CLUT write generation.
	///
	/// For mip draws, level_tex0 carries min(MXL,6)+1 register views (level 0 first,
	/// then GetTex0Layer's MIPTBP-derived views) and the built texture holds one GPU
	/// mip level per entry — the GS floors level sizes at one texel exactly like a
	/// GPU chain, so the geometries agree by construction (the route floors the rare
	/// pyramid deeper than the base). `pages` must already union every level's
	/// footprint, and the palette applies to all levels alike.
	GSTexture* Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
		const GIFRegTEXA& TEXA, const GSPageBitmap& pages, u32 pal_gen,
		const GIFRegTEX0* level_tex0 = nullptr, u32 levels = 1);

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

	bool BuildInto(Entry& e, GSLocalMemory& mem, const GIFRegTEX0* level_tex0, u32 levels, const GIFRegTEXA& TEXA);
	u8* GetScratch(u32 size);

	std::array<Entry, kMaxEntries> m_entries;
	std::unique_ptr<u8[]> m_scratch;
	u32 m_scratch_size = 0;
	u64 m_use_counter = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
};
