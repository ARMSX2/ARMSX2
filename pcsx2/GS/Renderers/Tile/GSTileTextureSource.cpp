// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileTextureSource.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSXXH.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include "common/BitUtils.h"

namespace
{
// The register identity of a texture window. TW/TH above 10 are invalid encodings;
// they clamp to 1024 the same way the readers do.
u64 RegKey(const GIFRegTEX0& TEX0)
{
	return static_cast<u64>(TEX0.TBP0) | (static_cast<u64>(TEX0.TBW) << 14) |
		   (static_cast<u64>(TEX0.PSM) << 20) | (static_cast<u64>(std::min<u32>(TEX0.TW, 10)) << 26) |
		   (static_cast<u64>(std::min<u32>(TEX0.TH, 10)) << 30);
}

// The identity of everything OUTSIDE local memory that shaped the bytes: the TEXA
// expansion for the direct 16/24-bit formats (the mask Classic's hash-cache uses).
// A palettised window has none — its texture holds the raw indices, and the palette
// is a separate texture the shader indexes with them, so the same index texture
// serves every palette the game cycles through it (GSTilePaletteCache).
u64 AuxKey(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	if (psm.pal > 0)
		return 0;
	if (psm.fmt > 0)
		return TEXA.U64 & 0x000000FF000080FFULL;
	return 0;
}

// The mip levels' addressing identity, packed exactly (never hashed — a collision
// would serve wrong bytes). TBP0 is 14 bits and TBW six, so three levels fit a
// word with the count on top; PSM/TW/TH derive from the base by construction.
void MipKey(const GIFRegTEX0* level_tex0, u32 levels, u64 out[2])
{
	out[0] = 0;
	out[1] = 0;
	if (levels <= 1)
		return;
	for (u32 i = 1; i < levels; i++)
	{
		const u64 word = static_cast<u64>(level_tex0[i].TBP0) | (static_cast<u64>(level_tex0[i].TBW) << 14);
		out[(i - 1) / 3] |= word << (((i - 1) % 3) * 20);
	}
	out[0] |= static_cast<u64>(levels) << 60;
}
} // namespace

GSTileTextureSource::GSTileTextureSource() = default;

GSTileTextureSource::~GSTileTextureSource()
{
	Clear();
}

u64 GSTileTextureSource::GenStamp(const GSVramModel& model, const GSPageBitmap& pages)
{
	u64 stamp = 0;
	pages.forEachSetPage([&](u32 page) {
		const GSVramModel::PageGen& g = model.Gen(page);
		stamp += g.cpu_write + g.gpu_write;
	});
	return stamp;
}

u64 GSTileTextureSource::PageContentStamp(const GSLocalMemory& mem, const GSVramModel& model, const GSPageBitmap& pages)
{
	// Positional fold of the per-page content hashes: the pair includes the page
	// index so two pages exchanging contents cannot cancel. forEachSetPage walks
	// ascending, so equal page sets fold in equal order by construction.
	XXH3_state_t st;
	XXH3_64bits_reset(&st);
	pages.forEachSetPage([&](u32 page) {
		PageContent& pc = m_page_content[page];
		const GSVramModel::PageGen& g = model.Gen(page);
#ifdef PCSX2_DEVBUILD
		// The honesty contract: a page is hashed only when its CPU bytes are the
		// post-spill bytes for its current generations. GPU-newest-unsynced truth
		// here means the caller skipped the spill — the donor-route poison.
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
			pxAssert(!model.Truth(pi).test(page) || model.SyncedPages(pi).test(page));
#endif
		if (pc.hash == 0 || pc.seen_cpu != g.cpu_write || pc.seen_gpu != g.gpu_write)
		{
			// 8 KB linear — the page's raw swizzled bytes, NOT a deswizzle. This
			// runs once per written page per frame; the deswizzle it stands in
			// for ran once per window reading the page.
			pc.hash = GSXXH3_64bits(mem.vm8() + page * GS_PAGE_SIZE, GS_PAGE_SIZE) | 1;
			pc.seen_cpu = g.cpu_write;
			pc.seen_gpu = g.gpu_write;
		}
		const u64 pair[2] = {page, pc.hash};
		GSXXH3_64bits_update(&st, pair, sizeof(pair));
	});
	return GSXXH3_64bits_digest(&st) | 1; // never the "no previous build" zero
}

u8* GSTileTextureSource::GetScratch(u32 size)
{
	if (m_scratch_size < size)
	{
		// Vector-aligned for the swizzle readers' stores.
		m_scratch = std::make_unique<u8[]>(size + 64);
		m_scratch_size = size;
	}
	u8* p = m_scratch.get();
	return reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(p) + 63) & ~static_cast<uintptr_t>(63));
}

bool GSTileTextureSource::BuildInto(Entry& e, GSLocalMemory& mem, const GIFRegTEX0* level_tex0, u32 levels,
	const GIFRegTEXA& TEXA, const Donor* donor)
{
	const int tw = 1 << std::min<u32>(level_tex0[0].TW, 10);
	const int th = 1 << std::min<u32>(level_tex0[0].TH, 10);

	// A palettised window becomes an INDEX texture — one byte per texel holding the
	// index the swizzle would have handed the CLUT (four-bit indices widened to a
	// byte, 0..15) — and the palette rides beside it as its own texture. Every other
	// format is deswizzled to RGBA8 with TEXA applied, as before. A CPU-built index
	// texture is R8 (the backend views it with the index in every channel); a
	// device-REINTERPRETED one is an RGBA8 render target with the index replicated,
	// because the conversion is a draw — the shader reads .a either way.
	const bool indexed = GSLocalMemory::m_psm[level_tex0[0].PSM].pal > 0;
	const bool reinterpret = donor && donor->reinterpret;
	const bool subrect = donor && !reinterpret && !donor->copy_rect.rempty();
	// The subrect route backfills by a stretch DRAW, so its texture is a target.
	const bool needs_rt = reinterpret || subrect;
	const GSTexture::Format format =
		(indexed && !reinterpret) ? GSTexture::Format::UNorm8 : GSTexture::Format::Color;

	bool fresh_tex = false;
	if (!e.tex || e.tex->GetWidth() != tw || e.tex->GetHeight() != th ||
		e.tex->GetMipmapLevels() != static_cast<int>(levels) || e.tex->GetFormat() != format ||
		e.tex->IsRenderTarget() != needs_rt)
	{
		if (e.tex)
			g_gs_device->Recycle(e.tex);
		e.tex = needs_rt ? g_gs_device->CreateRenderTarget(tw, th, format, false, true)
						 : g_gs_device->CreateTexture(tw, th, static_cast<int>(levels), format, true);
		if (!e.tex)
			return false;
		// A recycled allocation holds arbitrary bytes: a content-hash match can
		// keep the build id, but never the upload.
		fresh_tex = true;
	}

	// The device routes. The caller proved the donor holds the window's bytes, so the
	// whole build is one image copy or one conversion draw and the CPU never sees the
	// bytes — which is the entire point: the alternative is not a slower copy, it is a
	// full GPU drain to get the bytes into local memory before deswizzling them straight
	// back out again.
	if (donor)
	{
		pxAssert(levels == 1);
		if (reinterpret)
		{
			if (!g_gs_device->TileReinterpretIndex(donor->tex, e.tex, donor->params))
				return false;
			m_reinterpret_builds++;
			e.valid_rect = GSVector4i(0, 0, tw, th);
		}
		else if (subrect)
		{
			// The donor holds only the sampled core. First a scaled stretch of the
			// core over the WHOLE texture — every byte defined and plausible for
			// the margin taps the admission prices — then the core itself, exact
			// and in place. The donor's pixel space is the window's (the caller
			// proved base and stride equal), so source and destination rects match.
			const GSVector4i& cr = donor->copy_rect;
			pxAssert(cr.z <= donor->width && cr.w <= donor->height && cr.z <= tw && cr.w <= th);
			const GSVector4 src_norm = GSVector4(cr) / GSVector4(GSVector4i(donor->width, donor->height).xyxy());
			ShaderConvertSelector sel(ShaderConvert::COPY);
			if (donor->fill_alpha >= 0)
			{
				// A CT24 window: every consumer expects the TEXA expansion baked
				// into alpha, as the CPU deswizzle bakes it. Clear alpha to TA0
				// and copy RGB only — the 1:1 nearest stretch of the core is an
				// exact copy, and the masked path needs a draw rather than an
				// image copy anyway.
				g_gs_device->ClearRenderTarget(e.tex, static_cast<u32>(donor->fill_alpha) << 24);
				sel = sel.SetMask(0x7);
			}
			g_gs_device->StretchRect(donor->tex, src_norm, e.tex, GSVector4(0, 0, tw, th), sel, Nearest);
			if (donor->fill_alpha >= 0)
				g_gs_device->StretchRect(donor->tex, src_norm, e.tex, GSVector4(cr), sel, Nearest);
			else
				g_gs_device->CopyRect(donor->tex, e.tex, cr, static_cast<u32>(cr.x), static_cast<u32>(cr.y));
			e.valid_rect = cr;
		}
		else
		{
			pxAssert(tw <= donor->width && th <= donor->height);
			g_gs_device->CopyRect(donor->tex, e.tex, GSVector4i(0, 0, tw, th), 0, 0);
			e.valid_rect = GSVector4i(0, 0, tw, th);
		}
		// The bytes now on the device never transited the CPU, so the content
		// verify below has nothing to compare a later CPU build against; and
		// unknown bytes are changed bytes as far as derived caches go.
		e.content_hash = 0;
		e.build_id = ++m_build_counter;
		m_builds++;
		m_donor_builds++;
		return true;
	}
	e.valid_rect = GSVector4i(0, 0, tw, th);

	// Content verify: hash exactly the rows Update() would upload and compare
	// against this entry's previous CPU build. A rebuild is forced by a moved
	// gen_stamp, which says "some byte on the window's PAGES moved" — not that
	// the window's texels did — and on GT4 97.7% of rebuilds deswizzle to
	// identical bytes (games re-upload the same texture data every frame). A
	// same-bytes single-level rebuild skips its device upload entirely — the
	// bytes make one pass through the hasher instead of one through staging plus
	// a device copy — and keeps its build id, so derived caches (the
	// palette-expanded RGBA cache) see stable identity through the churn.
	XXH3_state_t hash_state;
	XXH3_64bits_reset(&hash_state);
	const u8* level0_buff = nullptr;
	u32 level0_pitch = 0;
	GSVector4i level0_rect = GSVector4i::zero();

	for (u32 i = 0; i < levels; i++)
	{
		const GIFRegTEX0& TEX0 = level_tex0[i];
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
		const int lw = 1 << std::min<u32>(TEX0.TW, 10);
		const int lh = 1 << std::min<u32>(TEX0.TH, 10);

		// Readers work in whole blocks; deswizzle the block-aligned window and upload
		// the texture-sized top-left corner (windows smaller than a block land inside).
		// The palette-less readers (rtxP) write one byte per texel; the expanding ones
		// write four.
		const GSVector4i rect(0, 0, lw, lh);
		const GSVector4i block_rect(rect.ralign<Align_Outside>(psm.bs));
		const u32 bytes_per_texel = indexed ? 1 : 4;
		const u32 pitch = Common::AlignUpPow2(static_cast<u32>(block_rect.width()) * bytes_per_texel, 32);

		u8* buff = GetScratch(pitch * static_cast<u32>(block_rect.height()));
		if (!buff)
			return false;

		const GSOffset off = mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
		(indexed ? psm.rtxP : psm.rtx)(mem, off, block_rect, buff, pitch, TEXA);
		for (int y = 0; y < lh; y++)
			GSXXH3_64bits_update(&hash_state, buff + static_cast<u32>(y) * pitch,
				static_cast<u32>(lw) * bytes_per_texel);
		if (levels > 1)
		{
			// A pyramid's verdict is only known after the last level, so its
			// uploads go out as the levels deswizzle; a full match still keeps
			// the build id below (the redundant uploads carried identical bytes).
			e.tex->Update(rect, buff, pitch, static_cast<int>(i));
		}
		else
		{
			// Single level: defer the upload until the verdict. The scratch is
			// not reused before then.
			level0_buff = buff;
			level0_pitch = pitch;
			level0_rect = rect;
		}
	}

	const u64 h = GSXXH3_64bits_digest(&hash_state) | 1; // never the "no previous build" zero
	const bool same = (h == e.content_hash);
	if (e.content_hash)
		((same ? m_rebuilds_same_bytes : m_rebuilds_new_bytes))++;
	if (levels == 1 && (!same || fresh_tex))
		e.tex->Update(level0_rect, level0_buff, level0_pitch, 0);
	e.content_hash = h;
	if (!same || e.build_id == 0)
		e.build_id = ++m_build_counter;
	m_builds++;
	return true;
}

GSTexture* GSTileTextureSource::Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, const GSPageBitmap& pages,
	const GIFRegTEX0* level_tex0, u32 levels, const Donor* donor, u64* build_id, const GSVector4i* sample_core)
{
	// Single-level callers pass no array; view the base register as a one-entry one.
	if (!level_tex0)
	{
		level_tex0 = &TEX0;
		levels = 1;
	}

	const u64 reg_key = RegKey(TEX0);
	const u64 aux_key = AuxKey(TEX0, TEXA);
	u64 mip_key[2];
	MipKey(level_tex0, levels, mip_key);
	const u64 stamp = GenStamp(model, pages);

	Entry* lru = nullptr;
	Entry* free_slot = nullptr;
	for (Entry& e : m_entries)
	{
		if (!e.alive)
		{
			if (!free_slot)
				free_slot = &e;
			continue;
		}
		if (e.reg_key == reg_key && e.aux_key == aux_key && e.mip_key[0] == mip_key[0] && e.mip_key[1] == mip_key[1])
		{
			e.last_use = ++m_use_counter;
			// A subrect-donor entry is valid only inside what it copied: it serves
			// a draw whose proven sample core it contains, and nobody else — in
			// particular a draw that arrives with NO core (whole-window semantics)
			// must rebuild rather than read the backfill.
			const GSVector4i full(0, 0, 1 << std::min<u32>(TEX0.TW, 10), 1 << std::min<u32>(TEX0.TH, 10));
			const bool rect_ok = e.valid_rect.eq(full) ||
								 (sample_core && e.valid_rect.rintersect(*sample_core).eq(*sample_core));
			if (e.gen_stamp == stamp && rect_ok)
			{
				m_hits++;
				if (build_id)
					*build_id = e.build_id;
				return e.tex;
			}
			// Same window, moved stamp. Before paying the deswizzle, ask the
			// cheaper question: did the pages' CONTENT move, or were they only
			// rewritten with the bytes they already held? (Same population the
			// row-hash tier measured at 97.7% — this refuses those one tier
			// earlier, before the gather-read of the swizzled window.) Only a
			// CPU-built entry can answer — a device-built one never recorded
			// what the CPU bytes were — and only a donor-LESS lookup may ask:
			// the donor route deliberately skips the caller's spill, so hashing
			// its pages would file PRE-spill bytes under the current write
			// generations and poison every later post-spill consumer of the
			// same pages. The SotC bloom chain caught exactly this — its window
			// alternates donor/CPU routes within a frame, and the stale hash
			// matched a fingerprint whose bytes had since been pulled.
			u64 page_content = 0;
			if (!donor && e.content_hash != 0 && e.page_content != 0)
			{
				page_content = PageContentStamp(mem, model, pages);
				if (page_content == e.page_content)
				{
					e.gen_stamp = stamp;
					m_rebuilds_same_pages++;
					if (build_id)
						*build_id = e.build_id;
					return e.tex;
				}
			}
			if (!BuildInto(e, mem, level_tex0, levels, TEXA, donor))
			{
				e.alive = false;
				if (e.tex)
				{
					g_gs_device->Recycle(e.tex);
					e.tex = nullptr;
				}
				return nullptr;
			}
			e.gen_stamp = stamp;
			e.pages = pages;
			// A CPU build records the page fingerprint it was built under (already
			// computed if the serve above was attempted); a device build records
			// nothing — its bytes never transited the CPU.
			e.page_content =
				(e.content_hash != 0) ? (page_content != 0 ? page_content : PageContentStamp(mem, model, pages)) : 0;
			// BuildInto stamped the id — a NEW one only if the bytes moved.
			if (build_id)
				*build_id = e.build_id;
			return e.tex;
		}
		if (!lru || e.last_use < lru->last_use)
			lru = &e;
	}

	Entry& e = free_slot ? *free_slot : *lru;
	if (e.alive && e.tex && !free_slot)
	{
		g_gs_device->Recycle(e.tex);
		e.tex = nullptr;
	}
	e.alive = true;
	e.reg_key = reg_key;
	e.aux_key = aux_key;
	e.mip_key[0] = mip_key[0];
	e.mip_key[1] = mip_key[1];
	e.pages = pages;
	e.last_use = ++m_use_counter;
	e.content_hash = 0; // a fresh window never compares against the slot's previous occupant
	e.page_content = 0;
	e.build_id = 0;
	if (!BuildInto(e, mem, level_tex0, levels, TEXA, donor))
	{
		e.alive = false;
		if (e.tex)
		{
			g_gs_device->Recycle(e.tex);
			e.tex = nullptr;
		}
		return nullptr;
	}
	e.gen_stamp = stamp;
	if (e.content_hash != 0)
		e.page_content = PageContentStamp(mem, model, pages);
	if (build_id)
		*build_id = e.build_id;
	return e.tex;
}

void GSTileTextureSource::Clear()
{
	for (Entry& e : m_entries)
	{
		if (e.tex)
		{
			g_gs_device->Recycle(e.tex);
			e.tex = nullptr;
		}
		e.alive = false;
	}
	// The page hashes key on the model's write generations, and the one caller
	// that resets those (GSRendererTile::Reset) clears this class in the same
	// act — so the hashes must die here, or a replay reaching the same counts
	// with different bytes would serve a stale texture.
	m_page_content.fill({});
	m_use_counter = 0;
}
