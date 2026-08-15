// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileTextureSource.h"

#include "GS/GSLocalMemory.h"
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

// The identity of everything OUTSIDE local memory that shaped the RGBA8 bytes: the
// TEXA expansion for the direct 16/24-bit formats (the mask Classic's hash-cache
// uses), and the expanded-palette identity for the palettised ones — the CLUT write
// generation plus the read-side registers Read32 expands through (CPSM, CSA, and
// TEXA again, this time via the palette entries).
u64 AuxKey(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, u32 pal_gen)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	if (psm.pal > 0)
	{
		const u64 texa_bits = TEXA.U64 & 0x000000FF000080FFULL;
		// texa_bits occupy bits {0-7, 15, 32-39}; fold them below CPSM/CSA/gen.
		const u64 folded_texa = (texa_bits & 0xFFFFu) | ((texa_bits >> 16) & 0xFF0000u);
		return folded_texa | (static_cast<u64>(TEX0.CPSM) << 24) | (static_cast<u64>(TEX0.CSA) << 28) |
			   (static_cast<u64>(pal_gen & 0x3FFFFFFFu) << 33) | (1ULL << 63);
	}
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

	if (!e.tex || e.tex->GetWidth() != tw || e.tex->GetHeight() != th ||
		e.tex->GetMipmapLevels() != static_cast<int>(levels))
	{
		if (e.tex)
			g_gs_device->Recycle(e.tex);
		e.tex = g_gs_device->CreateTexture(tw, th, static_cast<int>(levels), GSTexture::Format::Color, true);
		if (!e.tex)
			return false;
	}

	// The device route. The caller proved the donor's pixel space IS this window's, so
	// the whole build is one image copy and the CPU never sees the bytes — which is the
	// entire point: the alternative is not a slower copy, it is a full GPU drain to get
	// the bytes into local memory before deswizzling them straight back out again.
	if (donor)
	{
		pxAssert(levels == 1 && tw <= donor->width && th <= donor->height);
		g_gs_device->CopyRect(donor->tex, e.tex, GSVector4i(0, 0, tw, th), 0, 0);
		m_builds++;
		m_donor_builds++;
		return true;
	}

	for (u32 i = 0; i < levels; i++)
	{
		const GIFRegTEX0& TEX0 = level_tex0[i];
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
		const int lw = 1 << std::min<u32>(TEX0.TW, 10);
		const int lh = 1 << std::min<u32>(TEX0.TH, 10);

		// Readers work in whole blocks; deswizzle the block-aligned window and upload
		// the texture-sized top-left corner (windows smaller than a block land inside).
		const GSVector4i rect(0, 0, lw, lh);
		const GSVector4i block_rect(rect.ralign<Align_Outside>(psm.bs));
		const u32 pitch = Common::AlignUpPow2(static_cast<u32>(block_rect.width()) * 4, 32);

		u8* buff = GetScratch(pitch * static_cast<u32>(block_rect.height()));
		if (!buff)
			return false;

		const GSOffset off = mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
		psm.rtx(mem, off, block_rect, buff, pitch, TEXA);
		e.tex->Update(rect, buff, pitch, static_cast<int>(i));
	}
	m_builds++;
	return true;
}

GSTexture* GSTileTextureSource::Lookup(GSLocalMemory& mem, const GSVramModel& model, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, const GSPageBitmap& pages, u32 pal_gen,
	const GIFRegTEX0* level_tex0, u32 levels, const Donor* donor)
{
	// Single-level callers pass no array; view the base register as a one-entry one.
	if (!level_tex0)
	{
		level_tex0 = &TEX0;
		levels = 1;
	}

	const u64 reg_key = RegKey(TEX0);
	const u64 aux_key = AuxKey(TEX0, TEXA, pal_gen);
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
			if (e.gen_stamp == stamp)
			{
				m_hits++;
				return e.tex;
			}
			// Same window, moved bytes: rebuild in place.
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
	m_use_counter = 0;
}
