// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

// Where the CLUT RAM's entries live: the Tile renderer's provenance record for a
// palette the GPU may have loaded.
//
// The GS loads its palette from local memory at TEX0-write time (GSClut::WriteLoad),
// and when the bytes it loads from were rendered by a native draw, they are on the
// GPU: the CPU copy is stale, and reading it back is a drain (GT4 does exactly this
// seventy times a frame). The Tile renderer can instead gather the palette out of
// the owner surface's texture on the device — but the CPU's GSClut then holds a
// palette that is wrong for those entries, and every CPU consumer of it (the SW floor,
// the alpha-range scan, a savestate) has to be told. This class is that telling: the
// 256 entries of a 32-bit CLUT as sixteen slots of sixteen (the CSM1 loaders' own
// unit — an eight-bit load at CSA 0 writes all sixteen, a four-bit load writes the
// one at CSA), each slot naming the load that last wrote it: the CPU (its RAM is
// right), or a device palette by handle (its texels are right; the RAM is stale
// unless a sync has since copied them down).
//
// Pure — handles are opaque numbers the renderer maps to textures — so the verdicts
// are unit-testable without a device.
class GSTileClutMirror
{
public:
	static constexpr u32 kSlots = 16;
	static constexpr u32 kEntriesPerSlot = 16;
	static constexpr u32 kCpu = 0; ///< the handle meaning "the CPU's CLUT RAM is authoritative"

	enum class Verdict : u8
	{
		Cpu, ///< every requested slot is CPU-authoritative: use the CPU CLUT
		Gpu, ///< every requested slot lives in ONE device palette: use it
		Mixed, ///< device slots from different loads, or beside CPU ones: sync first
	};

	struct Resolution
	{
		Verdict verdict;
		u32 handle; ///< the device palette (Gpu only)
		u32 first_texel; ///< where the first requested slot's entries start in it (Gpu only)
	};

	void Reset() { m_slots.fill(Slot{}); }

	/// A load from CPU-authoritative bytes wrote slots [first, first + count).
	void OnCpuLoad(u32 first, u32 count)
	{
		for (u32 s = first; s < first + count && s < kSlots; s++)
			m_slots[s] = Slot{};
	}

	/// A load from device bytes wrote slots [first, first + count); `handle` names a
	/// palette texture holding those slots' entries in entry order, the first slot's
	/// sixteen at texel 0. The CPU RAM for those slots is stale until MarkSynced.
	void OnGpuLoad(u32 first, u32 count, u32 handle)
	{
		for (u32 s = first; s < first + count && s < kSlots; s++)
		{
			m_slots[s].handle = handle;
			m_slots[s].base_slot = first;
			m_slots[s].synced = false;
		}
	}

	/// The palette a draw reading slots [first, first + count) needs.
	Resolution Resolve(u32 first, u32 count) const
	{
		bool any_gpu = false;
		bool any_cpu = false;
		u32 handle = kCpu;
		u32 base = 0;
		bool one_load = true;
		for (u32 s = first; s < first + count && s < kSlots; s++)
		{
			const Slot& sl = m_slots[s];
			if (sl.handle == kCpu)
			{
				any_cpu = true;
				continue;
			}
			if (!any_gpu)
			{
				handle = sl.handle;
				base = sl.base_slot;
			}
			else if (sl.handle != handle || sl.base_slot != base)
			{
				one_load = false;
			}
			any_gpu = true;
		}
		if (!any_gpu)
			return {Verdict::Cpu, kCpu, 0};
		if (any_cpu || !one_load)
			return {Verdict::Mixed, kCpu, 0};
		return {Verdict::Gpu, handle, (first - base) * kEntriesPerSlot};
	}

	/// True when the CPU CLUT holds the right entries for slots [first, first + count):
	/// every one CPU-authoritative or synced since its device load.
	bool CpuCurrent(u32 first, u32 count) const
	{
		for (u32 s = first; s < first + count && s < kSlots; s++)
		{
			if (m_slots[s].handle != kCpu && !m_slots[s].synced)
				return false;
		}
		return true;
	}

	bool AnyGpu() const
	{
		for (const Slot& s : m_slots)
			if (s.handle != kCpu)
				return true;
		return false;
	}

	bool AnyUnsynced() const
	{
		for (const Slot& s : m_slots)
			if (s.handle != kCpu && !s.synced)
				return true;
		return false;
	}

	/// The device palette's entries for every unsynced slot: fn(slot, handle, texel) —
	/// the slot's sixteen entries start at `texel` of `handle`'s palette.
	template <typename F>
	void ForEachUnsyncedSlot(F&& fn) const
	{
		for (u32 s = 0; s < kSlots; s++)
		{
			const Slot& sl = m_slots[s];
			if (sl.handle != kCpu && !sl.synced)
				fn(s, sl.handle, (s - sl.base_slot) * kEntriesPerSlot);
		}
	}

	/// The CPU RAM now holds every slot on `handle`.
	void MarkSynced(u32 handle)
	{
		for (Slot& s : m_slots)
			if (s.handle == handle)
				s.synced = true;
	}

	/// Whether any slot still names `handle` (the renderer keeps its palette alive
	/// exactly that long).
	bool References(u32 handle) const
	{
		for (const Slot& s : m_slots)
			if (s.handle == handle)
				return true;
		return false;
	}

	u32 SlotHandle(u32 slot) const { return m_slots[slot].handle; }

private:
	struct Slot
	{
		u32 handle = kCpu;
		u32 base_slot = 0; ///< the first slot of the load that wrote this one
		bool synced = true; ///< the CPU RAM holds this slot's entries too
	};

	std::array<Slot, kSlots> m_slots{};
};
