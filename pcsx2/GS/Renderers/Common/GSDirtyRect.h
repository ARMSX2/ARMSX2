// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"

#include <array>
#include <memory>
#include <vector>

union RGBAMask
{
	u32 _u32;
	struct
	{
		u32 r : 1;
		u32 g : 1;
		u32 b : 1;
		u32 a : 1;
	} c;
};

class GSDirtyRect
{
public:
	GSVector4i r;
	u32 psm;
	u32 bw;
	RGBAMask rgba;
	bool req_linear;

	GSDirtyRect();
	GSDirtyRect(const GSVector4i& r, u32 psm, u32 bw, RGBAMask rgba, bool req_linear);
	GSVector4i GetDirtyRect(GIFRegTEX0 TEX0, bool align) const;
};

/// What of a target is newer in local memory than on the GPU: the parts the CPU wrote since the target was last
/// updated from local memory, which the target's next use uploads.
///
/// One byte per block of the target, in the target's coordinates and format: the channels that are dirty, and
/// whether the upload of the block may filter (bilinear when upscaled). A write ORs its blocks' bits in, so
/// writes need no order and no search. A rectangle whose edges are not on block edges, or that lies outside the
/// 2048x2048 coordinate space, is kept as a rectangle beside the blocks ("loose"), so a write that covers part of
/// a block uploads that part and not the whole block.
///
/// Readers that want rectangles see View(): the blocks as rectangles (each row's runs of equal bytes, joined
/// down where runs of the same columns and byte meet), then the loose rectangles.
class GSTargetDirty
{
public:
	static constexpr u8 LINEAR = 0x10;
	static constexpr int MAX_COORD = 2048;

	using const_iterator = std::vector<GSDirtyRect>::const_iterator;

	/// The blocks as rectangles, then the loose rectangles.
	const std::vector<GSDirtyRect>& View() const;

	bool empty() const { return m_count == 0 && m_loose.empty(); }
	size_t size() const { return View().size(); }
	const GSDirtyRect& operator[](size_t i) const { return View()[i]; }
	const_iterator begin() const { return View().begin(); }
	const_iterator end() const { return View().end(); }

	/// What View() would say, without building it: whether it holds exactly one rectangle (and which), whether
	/// it holds more than one, and the channels of its first rectangle (the list must not be empty).
	bool Single(GSDirtyRect* out) const;
	bool MoreThanOne() const;
	RGBAMask FrontRGBA() const;

	/// Union of the rectangles, each as GSDirtyRect::GetDirtyRect(TEX0, true) has it, aligned to TEX0's blocks and
	/// cut to `size`.
	GSVector4i GetTotalRect(GIFRegTEX0 TEX0, const GSVector2i& size) const;
	u32 GetDirtyChannels() const;
	GSVector4i GetDirtyRect(size_t index, GIFRegTEX0 TEX0, const GSVector4i& clamp, bool align) const;

	/// Whether every pixel of `area` (in format `psm`) has all of `mask` dirty.
	bool Covers(const GSVector4i& area, u32 psm, u32 mask) const;

	void clear();

	/// Marks a rectangle in format `rect.psm`. `psm` and `bw` are the target's, and lay the blocks out when
	/// nothing is dirty yet. Returns whether anything became dirty that was not.
	bool Mark(const GSDirtyRect& rect, u32 psm, u32 bw);

	/// The block layout in use.
	u32 Psm() const { return m_psm; }
	u32 Bw() const { return m_bw; }
	/// Lays the blocks out for format `psm` and width `bw`, if nothing is dirty.
	void SetLayout(u32 psm, u32 bw);
	/// Block size of the layout, as shifts.
	int BlockShiftX() const { return m_shift_x; }
	int BlockShiftY() const { return m_shift_y; }
	/// ORs `value` (channels, LINEAR) into block (x, y) of the layout. Returns whether a bit was new.
	bool MarkBlock(int x, int y, u8 value);

	/// Which layout and area the address mirror below speaks for: a target's base, width, format, the area its
	/// writes were mapped over, and the channel mask the mirrored blocks hold in full.
	struct AddressKey
	{
		u32 bp = 0, bw = 0, psm = 0, mask = 0;
		GSVector4i area = GSVector4i::zero();
		bool operator==(const AddressKey& o) const
		{
			return bp == o.bp && bw == o.bw && psm == o.psm && mask == o.mask && area.eq(o.area);
		}
	};
	/// Makes the mirror speak for `key` (empty if it spoke for another), then says whether every block address of
	/// `pages` (bits of full | part) is one whose target blocks already hold all of key.mask: then a write to them adds
	/// nothing.
	bool AddressesAlreadyFull(const AddressKey& key, const u32* full, const u32* part, const std::vector<u16>& pages);
	/// Records that every target block at the block addresses of physical page `page` in `mask`, inside the key's area
	/// (AddressesAlreadyFull), holds all of its mask.
	void NoteAddressesFull(u32 page, u32 mask)
	{
		page %= GS_MAX_PAGES;
		if (!m_addr_full[page])
			m_addr_pages.push_back(static_cast<u16>(page));
		m_addr_full[page] |= mask;
	}
	/// The blocks of physical page `page` the prepared mirror holds in full.
	u32 AddressesFull(u32 page) const { return m_addr_full[page % GS_MAX_PAGES]; }

	/// Rewrites every rectangle: `f` gets View() as a vector to change, and what it leaves is marked again.
	template <typename F>
	void EditRects(F&& f, u32 psm, u32 bw)
	{
		std::vector<GSDirtyRect> rects = View();
		clear();
		f(rects);
		for (const GSDirtyRect& r : rects)
			Mark(r, psm, bw);
	}

	/// Whether the rectangles of `rects` that name every channel of `mask`, in format `psm`, together cover every
	/// pixel of `area`.
	static bool RectsCover(const std::vector<GSDirtyRect>& rects, u32 psm, u32 mask, const GSVector4i& area);

private:
	u8& Cell(int x, int y) { return m_cells[static_cast<size_t>(y) * m_cols + static_cast<size_t>(x)]; }
	u8 Cell(int x, int y) const { return m_cells[static_cast<size_t>(y) * m_cols + static_cast<size_t>(x)]; }
	void GrowRows(int rows);
	bool CellsUniform(u8* value) const;
	u32 CountCovered(const GSVector4i& cells, u32 mask) const;

	std::unique_ptr<u8[]> m_cells; ///< m_rows rows of m_cols bytes
	int m_cols = 0, m_rows = 0;
	int m_shift_x = 3, m_shift_y = 3;
	u32 m_psm = 0, m_bw = 0;
	bool m_has_layout = false;

	u32 m_count = 0;                       ///< blocks with any bit
	std::array<u32, 32> m_hist{};          ///< blocks per byte value
	u32 m_distinct = 0;                    ///< byte values some block holds
	u32 m_channels = 0;                    ///< channels of every block and loose rectangle (dirt only grows until clear())
	u8 m_last_value = 0;                   ///< the byte the last changed block took
	int m_bx0 = 0, m_by0 = 0, m_bx1 = 0, m_by1 = 0; ///< bounding box of the dirty blocks, in blocks

	std::vector<GSDirtyRect> m_loose;

	/// By block address, the blocks whose target blocks hold all of m_addr_key.mask (see NoteAddressesFull). Only
	/// ever a subset of the truth, so it can only skip work that would change nothing.
	std::unique_ptr<u32[]> m_addr_full; ///< GS_MAX_PAGES words once used
	std::vector<u16> m_addr_pages;
	AddressKey m_addr_key;

	mutable std::vector<GSDirtyRect> m_view;
	mutable bool m_view_valid = false;

	/// The count of blocks inside m_cover_cells that hold every channel of m_cover_mask, kept up to date by
	/// MarkBlock once a Covers() call has asked for that area.
	mutable GSVector4i m_cover_cells = GSVector4i::zero();
	mutable u32 m_cover_mask = 0;
	mutable u32 m_cover_count = 0;
	mutable bool m_cover_valid = false;
};

inline bool GSTargetDirty::MarkBlock(int x, int y, u8 value)
{
	if (x < 0 || y < 0 || x >= m_cols || y >= (MAX_COORD >> m_shift_y) || !value)
		return false;
	if (y >= m_rows)
		GrowRows(y + 1);

	u8& cell = Cell(x, y);
	const u8 old = cell;
	const u8 now = old | value;
	if (now == old)
		return false;
	cell = now;

	if (old == 0)
	{
		if (m_count == 0)
		{
			m_bx0 = x, m_by0 = y, m_bx1 = x + 1, m_by1 = y + 1;
		}
		else
		{
			m_bx0 = std::min(m_bx0, x), m_by0 = std::min(m_by0, y);
			m_bx1 = std::max(m_bx1, x + 1), m_by1 = std::max(m_by1, y + 1);
		}
		m_count++;
	}
	else
	{
		m_distinct -= (--m_hist[old] == 0);
	}
	m_distinct += (m_hist[now]++ == 0);
	m_last_value = now;
	m_channels |= now & 0xf;

	if (m_cover_valid && x >= m_cover_cells.x && x < m_cover_cells.z && y >= m_cover_cells.y && y < m_cover_cells.w)
	{
		const bool was = (old & m_cover_mask) == m_cover_mask;
		const bool is = (now & m_cover_mask) == m_cover_mask;
		m_cover_count += static_cast<u32>(is) - static_cast<u32>(was);
	}

	m_view_valid = false;
	return true;
}
