// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSDrawRow.h"

#include <algorithm>
#include <cstring>

namespace GSDrawRows
{
	Mode s_mode = Mode::On;
	bool s_stats = false;
	Stats stats;

	void StateTable::PlaceReg(u16 i)
	{
		u32 slot = Reg(i).hash & m_mask;
		while (m_index[slot] != NONE)
			slot = (slot + 1) & m_mask;
		m_index[slot] = i;
	}

	u16 StateTable::FindRegSlow(const RegKey& k)
	{
		if (m_index.empty())
			return NONE;
		const u32 h = k.Hash();
		for (u32 slot = h & m_mask; m_index[slot] != NONE; slot = (slot + 1) & m_mask)
		{
			const u16 i = m_index[slot];
			RegState& rs = Reg(i);
			if (rs.hash == h && rs.key == k)
			{
				m_prev_reg = m_last_reg;
				m_last_reg = i;
				return i;
			}
		}
		return NONE;
	}

	u16 StateTable::InsertReg(const RegKey& k)
	{
		if (m_reg_count >= CAPACITY)
			return NONE;
		if (m_reg_count == m_regs.size() * CHUNK)
			m_regs.push_back(std::make_unique<RegState[]>(CHUNK));

		const u16 i = static_cast<u16>(m_reg_count++);
		RegState& rs = Reg(i);
		rs.key = k;
		rs.hash = k.Hash();
		rs.first = NONE;
		rs.last_hit = NONE;

		// Keep the index at most a quarter full, so that the first probe nearly always decides.
		if (m_index.size() < m_reg_count * 4u)
		{
			m_index.assign(std::max<size_t>(256, std::bit_ceil(static_cast<size_t>(m_reg_count) * 8)), NONE);
			m_mask = static_cast<u32>(m_index.size() - 1);
			for (u32 j = 0; j < m_reg_count; j++)
				PlaceReg(static_cast<u16>(j));
		}
		else
		{
			PlaceReg(i);
		}
		m_prev_reg = m_last_reg;
		m_last_reg = i;
		return i;
	}

	u16 StateTable::InsertVariant(u16 reg, const VarKey& v, u64 care)
	{
		if (m_entry_count >= CAPACITY)
			return NONE;
		if (m_entry_count == m_heads.size() * CHUNK)
		{
			m_heads.push_back(std::make_unique<StateHead[]>(CHUNK));
			m_bodies.push_back(std::make_unique<StateBody[]>(CHUNK));
			m_devices.push_back(std::make_unique<GSRowDeviceCache[]>(CHUNK));
		}

		const u16 i = static_cast<u16>(m_entry_count++);
		RegState& rs = Reg(reg);
		StateHead& h = Head(i);
		h.var = v;
		h.care = care;
		h.reg = reg;
		h.hits = 0;
		std::memset(&Device(i), 0, sizeof(GSRowDeviceCache));
		h.next = rs.first;
		rs.first = i;
		rs.last_hit = i;
		return i;
	}

	void StateTable::Flush()
	{
		// The chunks stay allocated: a flushed table refills to about the size it had.
		m_last_reg = NONE;
		m_prev_reg = NONE;
		m_reg_count = 0;
		m_entry_count = 0;
		m_index.clear();
		m_mask = 0;
		m_flushes++;
	}
} // namespace GSDrawRows
