// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Atomic access to a plain word that the EE thread and the IOP thread both use.
//
// The SBUS registers (EE 0x1000F200-F260, IOP 0x1D000000-60) are one set of words in the EE's hardware
// page, read and written by both machines. A few other words are written by one thread and read by the
// other: the IOP's ICFG register, the GS video mode, the memory card busy frame. Nothing here changes
// where a word lives or how big it is: the storage is the same plain integer it always was, and this
// header only says how it is touched.
//
// Generated code does not touch the SBUS words. The EE's loads and stores of a hardware address call the
// registered handlers (the one hardware word its JIT loads inline is INTC_STAT, which is not an SBUS
// register), and the IOP's loads and stores call iopMemRead* and iopMemWrite* or, for RAM only, the store
// stub. So making these accesses atomic needs no change in the JIT.
//
// std::atomic_ref is lock-free for these sizes on every host we build for, so each call is one
// instruction (a load, a store, or one read-modify-write), and the relaxed forms are exactly the plain
// load and store.
//
// Memory orders follow section 3.9 of the IOP thread design: loads acquire, stores release, read-modify-
// writes acquire+release, so a register read that sees a flag also sees what the other side wrote before
// it raised the flag.

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <type_traits>

namespace SharedWord
{
	template <typename T>
	__forceinline_odr T LoadRelaxed(T& word)
	{
		static_assert(std::atomic_ref<T>::is_always_lock_free);
		return std::atomic_ref<T>(word).load(std::memory_order_relaxed);
	}

	template <typename T>
	__forceinline_odr void StoreRelaxed(T& word, T value)
	{
		static_assert(std::atomic_ref<T>::is_always_lock_free);
		std::atomic_ref<T>(word).store(value, std::memory_order_relaxed);
	}

	__forceinline_odr u32 LoadAcquire(u32& word)
	{
		return std::atomic_ref<u32>(word).load(std::memory_order_acquire);
	}

	__forceinline_odr void StoreRelease(u32& word, u32 value)
	{
		std::atomic_ref<u32>(word).store(value, std::memory_order_release);
	}

	// word |= bits. Returns the old value.
	__forceinline_odr u32 FetchOr(u32& word, u32 bits)
	{
		return std::atomic_ref<u32>(word).fetch_or(bits, std::memory_order_acq_rel);
	}

	// word &= mask. Returns the old value.
	__forceinline_odr u32 FetchAnd(u32& word, u32 mask)
	{
		return std::atomic_ref<u32>(word).fetch_and(mask, std::memory_order_acq_rel);
	}

	// word = fn(word) as one step, for updates that look at the old value more than once (the IOP's write
	// to the SBUS control register is one). fn may run more than once if the other thread changes the
	// word in between, so it must have no effect of its own. Returns the old value.
	template <typename Fn>
	__forceinline_odr u32 Update(u32& word, Fn&& fn)
	{
		std::atomic_ref<u32> ref(word);
		u32 old = ref.load(std::memory_order_relaxed);
		while (!ref.compare_exchange_weak(old, fn(old), std::memory_order_acq_rel, std::memory_order_relaxed))
		{
		}
		return old;
	}
} // namespace SharedWord
