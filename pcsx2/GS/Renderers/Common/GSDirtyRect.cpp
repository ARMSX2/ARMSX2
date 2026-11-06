// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSDirtyRect.h"
#include "GS/GSUtil.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <vector>

GSDirtyRect::GSDirtyRect() :
	r(GSVector4i::zero()),
	psm(PSMCT32),
	bw(1),
	rgba({}),
	req_linear(false)
{
}

GSDirtyRect::GSDirtyRect(const GSVector4i& r, u32 psm, u32 bw, RGBAMask rgba, bool req_linear) :
	r(r),
	psm(psm),
	bw(bw),
	rgba(rgba),
	req_linear(req_linear)
{
}

GSVector4i GSDirtyRect::GetDirtyRect(GIFRegTEX0 TEX0, bool align) const
{
	GSVector4i _r;

	const GSVector2i& src = GSLocalMemory::m_psm[psm].bs;

	if (psm != TEX0.PSM)
	{
		const GSVector2i& dst = GSLocalMemory::m_psm[TEX0.PSM].bs;
		_r.left = (r.left * dst.x) / src.x;
		_r.top = (r.top * dst.y) / src.y;
		_r.right = (r.right * dst.x) / src.x;
		_r.bottom = (r.bottom * dst.y) / src.y;
	}
	else
	{
		_r = r;
	}

	return align ? _r.ralign<Align_Outside>(src) : _r;
}

void GSTargetDirty::SetLayout(u32 psm, u32 bw)
{
	if (!empty())
		return;
	const GSVector2i& bs = GSLocalMemory::m_psm[psm].bs;
	m_psm = psm;
	m_bw = bw;
	m_shift_x = std::countr_zero(static_cast<u32>(bs.x));
	m_shift_y = std::countr_zero(static_cast<u32>(bs.y));
	const int cols = MAX_COORD >> m_shift_x;
	if (cols != m_cols)
	{
		// Another row width: the rows are grown again on use.
		m_cols = cols;
		m_cells.reset();
		m_rows = 0;
	}
	m_has_layout = true;
}

void GSTargetDirty::GrowRows(int rows)
{
	rows = std::min((rows + 31) & ~31, MAX_COORD >> m_shift_y);
	if (rows <= m_rows)
		return;
	const size_t old_size = static_cast<size_t>(m_rows) * static_cast<size_t>(m_cols);
	const size_t new_size = static_cast<size_t>(rows) * static_cast<size_t>(m_cols);
	std::unique_ptr<u8[]> cells(new u8[new_size]);
	if (old_size)
		std::memcpy(cells.get(), m_cells.get(), old_size);
	std::memset(cells.get() + old_size, 0, new_size - old_size);
	m_cells = std::move(cells);
	m_rows = rows;
}


bool GSTargetDirty::Mark(const GSDirtyRect& rect, u32 psm, u32 bw)
{
	if (rect.r.rempty())
		return false;
	if (!m_has_layout || empty())
		SetLayout(psm, bw);

	const u8 value = static_cast<u8>((rect.rgba._u32 & 0xf) | (rect.req_linear ? LINEAR : 0));
	if (rect.rgba._u32 & 0xf)
	{
		GIFRegTEX0 layout = {};
		layout.PSM = m_psm;
		const GSVector4i r = (rect.psm == m_psm) ? rect.r : rect.GetDirtyRect(layout, false);
		const int mx = (1 << m_shift_x) - 1, my = (1 << m_shift_y) - 1;
		if (!r.rempty() && !(r.x & mx) && !(r.z & mx) && !(r.y & my) && !(r.w & my) && r.x >= 0 && r.y >= 0 &&
			r.z <= MAX_COORD && r.w <= MAX_COORD)
		{
			const int x0 = r.x >> m_shift_x, x1 = r.z >> m_shift_x, y0 = r.y >> m_shift_y, y1 = r.w >> m_shift_y;
			if (m_count == 0 && m_loose.empty())
			{
				// Nothing is dirty yet (a target that is new or was just updated): the rectangle is the whole set.
				if (y1 > m_rows)
					GrowRows(y1);
				for (int y = y0; y < y1; y++)
					std::memset(&Cell(x0, y), value, static_cast<size_t>(x1 - x0));
				m_count = static_cast<u32>((x1 - x0) * (y1 - y0));
				m_hist[value] = m_count;
				m_distinct = 1;
				m_last_value = value;
				m_channels |= value & 0xf;
				m_bx0 = x0, m_by0 = y0, m_bx1 = x1, m_by1 = y1;
				m_cover_valid = false;
				m_view_valid = false;
				return true;
			}
			bool any = false;
			for (int y = r.y >> m_shift_y; y < (r.w >> m_shift_y); y++)
				for (int x = r.x >> m_shift_x; x < (r.z >> m_shift_x); x++)
					any |= MarkBlock(x, y, value);
			return any;
		}
	}

	for (const GSDirtyRect& d : m_loose)
	{
		if (d.r.eq(rect.r) && d.psm == rect.psm && d.bw == rect.bw && d.rgba._u32 == rect.rgba._u32 && d.req_linear == rect.req_linear)
			return false;
	}
	m_loose.push_back(rect);
	m_channels |= rect.rgba._u32;
	m_view_valid = false;
	return true;
}

void GSTargetDirty::clear()
{
	if (m_count)
	{
		for (int y = m_by0; y < m_by1; y++)
			std::memset(&Cell(m_bx0, y), 0, static_cast<size_t>(m_bx1 - m_bx0));
		m_hist = {};
		m_distinct = 0;
		m_count = 0;
	}
	m_loose.clear();
	m_channels = 0;
	for (const u16 page : m_addr_pages)
		m_addr_full[page] = 0;
	m_addr_pages.clear();
	m_has_layout = false;
	m_view_valid = false;
	m_cover_valid = false;
}

bool GSTargetDirty::AddressesAlreadyFull(const AddressKey& key, const u32* full, const u32* part, const std::vector<u16>& pages)
{
	if (!(key == m_addr_key))
	{
		for (const u16 page : m_addr_pages)
			m_addr_full[page] = 0;
		m_addr_pages.clear();
		m_addr_key = key;
	}
	if (!m_addr_full)
	{
		m_addr_full.reset(new u32[GS_MAX_PAGES]);
		std::memset(m_addr_full.get(), 0, GS_MAX_PAGES * sizeof(u32));
	}
	if (m_addr_pages.empty())
		return false;
	for (const u16 page : pages)
	{
		if ((full[page] | part[page]) & ~m_addr_full[page])
			return false;
	}
	return true;
}


const std::vector<GSDirtyRect>& GSTargetDirty::View() const
{
	if (m_view_valid)
		return m_view;

	m_view.clear();
	GSDirtyRect single;
	if (m_count && m_loose.empty() && Single(&single))
	{
		// One rectangle of one byte: no need to look at the blocks.
		m_view.push_back(single);
	}
	else if (m_count)
	{
		// Runs of the row above that can still grow down: first column, end column, byte, rectangle index.
		struct Run
		{
			int x0, x1;
			u8 value;
			size_t index;
		};
		std::vector<Run> above, here;
		const int sx = m_shift_x, sy = m_shift_y;
		for (int y = m_by0; y < m_by1; y++)
		{
			here.clear();
			size_t cursor = 0;
			int x = m_bx0;
			while (x < m_bx1)
			{
				const u8 v = Cell(x, y);
				if (!v)
				{
					x++;
					continue;
				}
				const int x0 = x;
				while (x < m_bx1 && Cell(x, y) == v)
					x++;
				while (cursor < above.size() && above[cursor].x0 < x0)
					cursor++;
				if (cursor < above.size() && above[cursor].x0 == x0 && above[cursor].x1 == x && above[cursor].value == v)
				{
					m_view[above[cursor].index].r.w = (y + 1) << sy;
					here.push_back(above[cursor]);
					cursor++;
				}
				else
				{
					RGBAMask rgba;
					rgba._u32 = v & 0xf;
					m_view.push_back(GSDirtyRect(GSVector4i(x0 << sx, y << sy, x << sx, (y + 1) << sy), m_psm, m_bw, rgba, (v & LINEAR) != 0));
					here.push_back({x0, x, v, m_view.size() - 1});
				}
			}
			above.swap(here);
		}
	}
	m_view.insert(m_view.end(), m_loose.begin(), m_loose.end());
	m_view_valid = true;
	return m_view;
}

bool GSTargetDirty::CellsUniform(u8* value) const
{
	// With one byte value among the blocks, the block that changed last holds it.
	*value = m_last_value;
	return m_distinct == 1;
}

bool GSTargetDirty::Single(GSDirtyRect* out) const
{
	if (m_count == 0)
	{
		if (m_loose.size() != 1)
			return false;
		*out = m_loose[0];
		return true;
	}
	u8 v = 0;
	if (!m_loose.empty() || !CellsUniform(&v) ||
		static_cast<u64>(m_count) != static_cast<u64>(m_bx1 - m_bx0) * static_cast<u64>(m_by1 - m_by0))
		return false;
	RGBAMask rgba;
	rgba._u32 = v & 0xf;
	*out = GSDirtyRect(GSVector4i(m_bx0 << m_shift_x, m_by0 << m_shift_y, m_bx1 << m_shift_x, m_by1 << m_shift_y),
		m_psm, m_bw, rgba, (v & LINEAR) != 0);
	return true;
}

bool GSTargetDirty::MoreThanOne() const
{
	GSDirtyRect unused;
	return !empty() && !Single(&unused);
}

RGBAMask GSTargetDirty::FrontRGBA() const
{
	if (m_count)
	{
		// View()'s first rectangle starts at the first dirty block of the top row of the bounding box.
		u8 v = 0;
		if (!CellsUniform(&v))
		{
			for (int x = m_bx0; x < m_bx1 && !(v = Cell(x, m_by0)); x++)
				;
		}
		RGBAMask rgba;
		rgba._u32 = v & 0xf;
		return rgba;
	}
	return m_loose[0].rgba;
}

GSVector4i GSTargetDirty::GetTotalRect(GIFRegTEX0 TEX0, const GSVector2i& size) const
{
	if (empty())
		return GSVector4i::zero();

	GSVector4i r = GSVector4i::cxpr(INT_MAX, INT_MAX, 0, 0);
	for (const GSDirtyRect& d : m_loose)
		r = r.runion(d.GetDirtyRect(TEX0, true));

	// The blocks' rectangles share one format, and GetDirtyRect is monotonic in each edge, so the union of theirs
	// is the one of their bounding box.
	if (m_count)
	{
		const GSVector4i box(m_bx0 << m_shift_x, m_by0 << m_shift_y, m_bx1 << m_shift_x, m_by1 << m_shift_y);
		r = r.runion(GSDirtyRect(box, m_psm, m_bw, RGBAMask{}, false).GetDirtyRect(TEX0, true));
	}

	const GSVector2i& bs = GSLocalMemory::m_psm[TEX0.PSM].bs;
	return r.ralign<Align_Outside>(bs).rintersect(GSVector4i::loadh(size));
}

u32 GSTargetDirty::GetDirtyChannels() const
{
	return m_channels;
}

GSVector4i GSTargetDirty::GetDirtyRect(size_t index, GIFRegTEX0 TEX0, const GSVector4i& clamp, bool align) const
{
	GSVector4i r = (*this)[index].GetDirtyRect(TEX0, align);
	const GSVector2i& bs = GSLocalMemory::m_psm[TEX0.PSM].bs;
	if (align)
		r = r.ralign<Align_Outside>(bs);

	return r.rintersect(clamp);
}

u32 GSTargetDirty::CountCovered(const GSVector4i& cells, u32 mask) const
{
	u32 n = 0;
	const int y1 = std::min(cells.w, m_rows);
	for (int y = cells.y; y < y1; y++)
		for (int x = cells.x; x < cells.z; x++)
			n += (Cell(x, y) & mask) == mask;
	return n;
}

bool GSTargetDirty::Covers(const GSVector4i& area, u32 psm, u32 mask) const
{
	if (area.rempty() || empty() || !m_has_layout)
		return false;

	const GSVector2i& bs = GSLocalMemory::m_psm[psm].bs;
	const bool in_space = area.x >= 0 && area.y >= 0 && area.z <= MAX_COORD && area.w <= MAX_COORD;
	if (psm == m_psm && in_space)
	{
		const GSVector4i a = area.ralign<Align_Outside>(bs);
		const GSVector4i cells(a.x >> m_shift_x, a.y >> m_shift_y, a.z >> m_shift_x, a.w >> m_shift_y);
		if (!m_cover_valid || !m_cover_cells.eq(cells) || m_cover_mask != mask)
		{
			m_cover_cells = cells;
			m_cover_mask = mask;
			m_cover_count = CountCovered(cells, mask);
			m_cover_valid = true;
		}
		if (static_cast<u64>(m_cover_count) == static_cast<u64>(cells.z - cells.x) * static_cast<u64>(cells.w - cells.y))
			return true;

		// The blocks leave a gap: only a loose rectangle that counts can fill it.
		bool loose_counts = false;
		for (const GSDirtyRect& d : m_loose)
			loose_counts |= d.psm == psm && (d.rgba._u32 & mask) == mask;
		if (!loose_counts)
			return false;
	}

	return RectsCover(View(), psm, mask, area);
}

bool GSTargetDirty::RectsCover(const std::vector<GSDirtyRect>& list, u32 psm, u32 mask, const GSVector4i& area_v)
{
	struct IRect
	{
		int x, y, z, w;
	};
	const IRect area = {area_v.x, area_v.y, area_v.z, area_v.w};
	if (area.z <= area.x || area.w <= area.y)
		return false;

	std::vector<IRect> rects;
	s64 area_sum = 0;
	for (const GSDirtyRect& d : list)
	{
		if (d.psm != psm || (d.rgba._u32 & mask) != mask)
			continue;
		const IRect r = {std::max(d.r.x, area.x), std::max(d.r.y, area.y), std::min(d.r.z, area.z), std::min(d.r.w, area.w)};
		if (r.z <= r.x || r.w <= r.y)
			continue;
		if (r.x == area.x && r.y == area.y && r.z == area.z && r.w == area.w)
			return true;
		rects.push_back(r);
		area_sum += static_cast<s64>(r.z - r.x) * (r.w - r.y);
	}

	// The union is no larger than the sum of the areas.
	if (area_sum < static_cast<s64>(area.z - area.x) * (area.w - area.y))
		return false;

	// Sweep across the distinct left and right edges. Inside one strip between two neighbouring edges a
	// rectangle either spans the whole strip or misses it, so the strip is covered when the rectangles
	// that span it, taken from the top down, leave no gap between area.y and area.w.
	std::sort(rects.begin(), rects.end(), [](const IRect& a, const IRect& b) { return a.y < b.y; });
	std::vector<int> xs;
	xs.reserve(rects.size() * 2 + 2);
	xs.push_back(area.x);
	xs.push_back(area.z);
	for (const IRect& r : rects)
	{
		xs.push_back(r.x);
		xs.push_back(r.z);
	}
	std::sort(xs.begin(), xs.end());
	xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

	for (size_t i = 0; i + 1 < xs.size(); i++)
	{
		const int x0 = xs[i], x1 = xs[i + 1];
		int reach = area.y;
		for (size_t k = 0; k < rects.size() && reach < area.w; k++)
		{
			const IRect& r = rects[k];
			if (r.x > x0 || r.z < x1)
				continue;
			if (r.y > reach)
				return false;
			reach = std::max(reach, r.w);
		}
		if (reach < area.w)
			return false;
	}
	return true;
}
