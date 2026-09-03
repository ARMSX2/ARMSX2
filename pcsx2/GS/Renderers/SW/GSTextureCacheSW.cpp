// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/SW/GSTextureCacheSW.h"
#include "GS/GSExtra.h"
#include "GS/GSPerfMon.h"
#include "GS/GSPng.h"
#include "GS/GSUtil.h"

GSTextureCacheSW::GSTextureCacheSW() = default;

GSTextureCacheSW::~GSTextureCacheSW()
{
	RemoveAll();
}

GSTextureCacheSW::Texture* GSTextureCacheSW::Lookup(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, u32 tw0)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];

	auto& m = m_map[TEX0.TBP0 >> 5];

	for (auto i = m.begin(); i != m.end(); ++i)
	{
		Texture* t = *i;

		if (((TEX0.U32[0] ^ t->m_TEX0.U32[0]) | ((TEX0.U32[1] ^ t->m_TEX0.U32[1]) & 3)) != 0) // TBP0 TBW PSM TW TH
		{
			continue;
		}

		if ((psm.trbpp == 16 || psm.trbpp == 24) && TEX0.TCC && TEXA != t->m_TEXA)
		{
			continue;
		}

		if (tw0 != 0 && t->m_tw != tw0)
		{
			continue;
		}

		// Lookup hit
		m.MoveFront(i.Index());
		t->m_age = 0;
		return t;
	}

	// Lookup miss
	Texture* t = new Texture(tw0, TEX0, TEXA);

	m_textures.insert(t);

	t->m_pages.loopPages([this, t](u32 page)
	{
		t->m_erase_it[page] = m_map[page].InsertFront(t);
	});

	return t;
}

void GSTextureCacheSW::InvalidatePages(const GSOffset::PageLooper& pages, u32 psm)
{
	pages.loopPages([this, psm](u32 page)
	{
		for (Texture* t : m_map[page])
		{
			if (GSUtil::HasSharedBits(psm, t->m_sharedbits))
			{
				u32* RESTRICT valid = t->m_valid;

				if (t->m_repeating)
				{
					for (const GSVector2i& j : t->m_p2t[page])
					{
						valid[j.x] &= j.y;
					}
				}
				else
				{
					valid[page] = 0;
				}

				t->m_complete = false;
			}
		}
	});
}

void GSTextureCacheSW::RemoveAll()
{
	for (auto i : m_textures)
		delete i;

	m_textures.clear();

	for (auto& l : m_map)
	{
		l.clear();
	}
}

void GSTextureCacheSW::IncAge()
{
	for (auto i = m_textures.begin(); i != m_textures.end();)
	{
		Texture* const t = *i;

		if (++t->m_age > 10)
		{
			i = m_textures.erase(i);

			t->m_pages.loopPages([this, t](u32 page)
			{
				m_map[page].EraseIndex(t->m_erase_it[page]);
			});

			delete t;
		}
		else
		{
			++i;
		}
	}
}

//

GSTextureCacheSW::Texture::Texture(u32 tw0, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA)
	: m_TEX0(TEX0)
	, m_TEXA(TEXA)
	, m_buff(nullptr)
	, m_tw(tw0)
	, m_age(0)
	, m_complete(false)
	, m_p2t(nullptr)
{
	if (m_tw == 0)
	{
		m_tw = std::max<int>(m_TEX0.TW, GSLocalMemory::m_psm[m_TEX0.PSM].pal == 0 ? 3 : 5); // makes one row 32 bytes at least, matches the smallest block size that is allocated for m_buff
	}

	memset(m_valid, 0, sizeof(m_valid));
	m_valid_dirty.MakeEmpty();

	m_sharedbits = GSUtil::HasSharedBitsPtr(m_TEX0.PSM);

	m_offset = g_gs_renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	m_pages = m_offset.pageLooperForRect(GSVector4i(0, 0, 1 << TEX0.TW, 1 << TEX0.TH));

	m_repeating = m_TEX0.IsRepeating(); // repeating mode always works, it is just slightly slower

	if (m_repeating)
	{
		m_p2t = g_gs_renderer->m_mem.GetPage2TileMap(m_TEX0);
	}
}

GSTextureCacheSW::Texture::~Texture()
{
	if (m_buff)
	{
		_aligned_free(m_buff);
	}
}

void GSTextureCacheSW::Texture::Reset(u32 tw0, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA)
{
	if (m_buff && (m_TEX0.TW != TEX0.TW || m_TEX0.TH != TEX0.TH))
	{
		// The texture's shape moved, so Update() has to hand the rasterizer a buffer that reads
		// zero outside the blocks it unswizzles. The allocation is kept and Update() zeroes
		// exactly the bytes previous draws wrote into it -- GSSwTextureDirty.h has the argument
		// for why that produces identical bytes to a freshly zeroed allocation.
		m_buff_stale = true;
	}

	m_tw = tw0;
	m_age = 0;
	m_complete = false;
	m_p2t = nullptr;
	m_TEX0 = TEX0;
	m_TEXA = TEXA;

	if (m_tw == 0)
	{
		m_tw = std::max<int>(m_TEX0.TW, GSLocalMemory::m_psm[m_TEX0.PSM].pal == 0 ? 3 : 5); // makes one row 32 bytes at least, matches the smallest block size that is allocated for m_buff
	}

	// Same rule as the pixel buffer, in words. Every word outside the tracked range is already
	// zero: Update() only ever sets bits in m_valid and InvalidatePages() only ever clears them,
	// so nothing can dirty a word this never saw. Reset() is reached from the hardware renderer's
	// SwPrimRender road alone -- the software renderer's own cache goes through Lookup() and
	// never resets a texture -- so this cannot change what that renderer sees either.
	const GSSwTextureDirty::Range vr = m_valid_dirty.ClearRange(GS_MAX_PAGES);
	if (vr.Size() > 0)
		memset(&m_valid[vr.begin], 0, vr.Size() * sizeof(m_valid[0]));
	m_valid_dirty.MakeEmpty();

	m_sharedbits = GSUtil::HasSharedBitsPtr(m_TEX0.PSM);

	m_offset = g_gs_renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	m_pages = m_offset.pageLooperForRect(GSVector4i(0, 0, 1 << TEX0.TW, 1 << TEX0.TH));

	m_repeating = m_TEX0.IsRepeating(); // repeating mode always works, it is just slightly slower

	if (m_repeating)
	{
		m_p2t = g_gs_renderer->m_mem.GetPage2TileMap(m_TEX0);
	}
}

bool GSTextureCacheSW::Texture::Update(const GSVector4i& rect)
{
	m_last_clear_bytes = 0;
	m_last_blocks = 0;

	if (m_complete)
	{
		return true;
	}

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_TEX0.PSM];

	GSVector2i bs = psm.bs;

	int shift = psm.pal == 0 ? 2 : 0;

	int tw = std::max<int>(1 << m_TEX0.TW, bs.x);
	int th = std::max<int>(1 << m_TEX0.TH, bs.y);

	GSVector4i r = rect;

	r = r.ralign<Align_Outside>(bs);

	if (r.eq(GSVector4i(0, 0, tw, th)))
	{
		m_complete = true; // lame, but better than nothing
	}

	if (!m_buff || m_buff_stale)
	{
		const u32 pitch = (1 << m_tw) << shift;
		const size_t size = pitch * th * 4;

		if (m_buff_size < size)
		{
			// No buffer, or one too small to hold this draw. Grow-only: the allocation settles on
			// the largest shape the road asks for and stops churning.
			if (m_buff)
				_aligned_free(m_buff);

			m_buff = _aligned_malloc(size, VECTOR_ALIGNMENT);
			if (!m_buff)
			{
				m_buff_size = 0;
				m_dirty.MakeEmpty();
				m_buff_stale = false;
				return false;
			}

			m_buff_size = size;

			// This _shouldn't_ be necessary, but apparently our texture min/max is wrong somewhere,
			// and we end up sampling from "random" malloc memory, which breaks GS dump runs.
			std::memset(m_buff, 0, size);
			m_last_clear_bytes = static_cast<u32>(size);
		}
		else
		{
			// The allocation is kept and put back the way a fresh zeroed one would have been:
			// everything outside the tracked range is already zero, so zeroing the range makes the
			// whole capacity zero. INVARIANT: after Update() returns, every byte the rasterizer may
			// read -- inside the unswizzled rect, in the rest of the nominal buffer, or anywhere in
			// the x4 guard band -- holds exactly what the allocate-and-memset path would have left
			// there.
			const GSSwTextureDirty::Range dr = m_dirty.ClearRange(m_buff_size);
			if (dr.Size() > 0)
				std::memset(static_cast<u8*>(m_buff) + dr.begin, 0, dr.Size());

			m_last_clear_bytes = static_cast<u32>(dr.Size());
		}

		m_dirty.MakeEmpty();
		m_buff_stale = false;
	}

	GSLocalMemory& mem = g_gs_renderer->m_mem;

	GSOffset off = m_offset;

	u32 blocks = 0;

	GSLocalMemory::readTextureBlock rtxbP = psm.rtxbP;

	u32 pitch = (1 << m_tw) << shift;

	u8* const buff_base = (u8*)m_buff;

	u8* dst = buff_base + pitch * r.top;

	int block_pitch = pitch * bs.y;

	// One block write covers bs.y rows of (bs.x << shift) bytes at stride `pitch` -- the same
	// shape block_pitch above already assumes. Taken before shift picks up blockShiftX, which
	// turns it from "bytes per pixel" into "bytes per block column".
	const size_t block_extent = GSSwTextureDirty::BlockExtent(bs.y, pitch, static_cast<size_t>(bs.x) << shift);

	shift += off.blockShiftX();
	int bottom = r.bottom >> off.blockShiftY();
	int right = r.right >> off.blockShiftX();

	GSOffset::BNHelper bn = off.bnMulti(r.left, r.top);

	if (m_repeating)
	{
		for (; bn.blkY() < bottom; bn.nextBlockY(), dst += block_pitch)
		{
			for (; bn.blkX() < right; bn.nextBlockX())
			{
				int i = (bn.blkY() << 7) + bn.blkX();
				u32 block = bn.value();

				u32 row = i >> 5;
				u32 col = 1 << (i & 31);

				if ((m_valid[row] & col) == 0)
				{
					m_valid[row] |= col;

					const ptrdiff_t block_off = (dst - buff_base) + (static_cast<ptrdiff_t>(bn.blkX()) << shift);
					if (block_off >= 0)
						m_dirty.Add(static_cast<size_t>(block_off), static_cast<size_t>(block_off) + block_extent);

					m_valid_dirty.Add(row, row + 1);

					rtxbP(mem, block, &dst[bn.blkX() << shift], pitch, m_TEXA);

					blocks++;
				}
			}
		}
	}
	else
	{
		for (; bn.blkY() < bottom; bn.nextBlockY(), dst += block_pitch)
		{
			for (; bn.blkX() < right; bn.nextBlockX())
			{
				u32 block = bn.value();

				u32 row = block >> 5;
				u32 col = 1 << (block & 31);

				if ((m_valid[row] & col) == 0)
				{
					m_valid[row] |= col;

					const ptrdiff_t block_off = (dst - buff_base) + (static_cast<ptrdiff_t>(bn.blkX()) << shift);
					if (block_off >= 0)
						m_dirty.Add(static_cast<size_t>(block_off), static_cast<size_t>(block_off) + block_extent);

					m_valid_dirty.Add(row, row + 1);

					rtxbP(mem, block, &dst[bn.blkX() << shift], pitch, m_TEXA);

					blocks++;
				}
			}
		}
	}

	m_last_blocks = blocks;

	if (blocks > 0)
	{
		g_perfmon.Put(GSPerfMon::Unswizzle, bs.x * bs.y * blocks << shift);
	}

	return true;
}

bool GSTextureCacheSW::Texture::Save(const std::string& fn) const
{
	const u32* RESTRICT clut = g_gs_renderer->m_mem.m_clut;

	const u32 w = 1 << m_TEX0.TW;
	const u32 h = 1 << m_TEX0.TH;

	const GSPng::Format format = (IsDevBuild || GSConfig.SaveAlpha) ? GSPng::RGB_A_PNG : GSPng::RGB_PNG;
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_TEX0.PSM];
	const u8* RESTRICT src = (u8*)m_buff;
	const u32 src_pitch = 1u << (m_tw + (psm.pal == 0 ? 2 : 0));
	if (psm.pal == 0)
	{
		// no clut => dump directly
		return GSPng::Save(format, fn, src, w, h, src_pitch, GSConfig.PNGCompressionLevel);
	}
	else
	{
		const std::unique_ptr<u32[]> dumptex = std::make_unique<u32[]>(w * h);
		u32* dst = dumptex.get();

		for (u32 j = 0; j < h; j++)
		{
			for (u32 i = 0; i < w; i++)
				*(dst++) = clut[src[i]];

			src += src_pitch;
		}

		return GSPng::Save(format, fn, reinterpret_cast<const u8*>(dumptex.get()),
			w, h, w * sizeof(u32), GSConfig.PNGCompressionLevel);
	}
}
