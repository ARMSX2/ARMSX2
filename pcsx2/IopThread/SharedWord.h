// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Atomic access to plain words that the EE thread and the IOP thread both use.
//
// The SBUS registers (EE 0x1000F200-F260, IOP 0x1D000000-60) are one set of words in the EE's hardware
// page, read and written by both machines. A few other words are written by one thread and read by the
// other: the IOP's ICFG register, the GS video mode, the memory card busy frame. Storage is unchanged;
// this header only says how it is touched.
//
// Generated code does not touch the SBUS words. The EE's loads and stores of a hardware address call the
// registered handlers (the one hardware word its JIT loads inline is INTC_STAT, which is not an SBUS
// register), and the IOP's go through iopMemRead*/iopMemWrite* or, for RAM only, the store stub. So
// making these accesses atomic needs no change in the JIT.
//
// The calls are the compiler's __atomic builtins on the plain storage, not std::atomic_ref, which the
// Android NDK's libc++ (r28, LLVM 19) does not have. The builtins are lock-free for these sizes on every
// host we build for, and the relaxed forms are exactly the plain load and store.
//
// Memory orders: loads acquire, stores release, read-modify-writes acquire+release, so a register read
// that sees a flag also sees what the other side wrote before it raised the flag.

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <type_traits>

namespace SharedWord
{
	template <typename T>
	__forceinline_odr T LoadRelaxed(T& word)
	{
		static_assert(__atomic_always_lock_free(sizeof(T), 0));
		T value;
		__atomic_load(&word, &value, __ATOMIC_RELAXED);
		return value;
	}

	template <typename T>
	__forceinline_odr void StoreRelaxed(T& word, T value)
	{
		static_assert(__atomic_always_lock_free(sizeof(T), 0));
		__atomic_store(&word, &value, __ATOMIC_RELAXED);
	}

	__forceinline_odr u32 LoadAcquire(u32& word)
	{
		return __atomic_load_n(&word, __ATOMIC_ACQUIRE);
	}

	__forceinline_odr void StoreRelease(u32& word, u32 value)
	{
		__atomic_store_n(&word, value, __ATOMIC_RELEASE);
	}

	__forceinline_odr u32 FetchOr(u32& word, u32 bits)
	{
		return __atomic_fetch_or(&word, bits, __ATOMIC_ACQ_REL);
	}

	__forceinline_odr u32 FetchAnd(u32& word, u32 mask)
	{
		return __atomic_fetch_and(&word, mask, __ATOMIC_ACQ_REL);
	}

	// A load through a const reference; the const is cast away only because the builtins take a plain pointer.
	template <typename T>
	__forceinline_odr T LoadRelaxedConst(const T& word)
	{
		return LoadRelaxed(const_cast<T&>(word));
	}

	// A field that the host thread writes and the IOP thread reads (the pad input fields). The storage
	// is one T, so a struct or an array of these keeps the layout of plain T, and every access is a
	// relaxed atomic load or store (a plain ldr or str). Relaxed is enough: the console samples its pad
	// asynchronously too. Copying is deleted so that no code copies a field the other thread is
	// writing; take the value (`T v = field`) instead.
	template <typename T>
	class Relaxed
	{
	public:
		constexpr Relaxed() = default;
		constexpr Relaxed(T value)
			: m_value(value)
		{
		}
		Relaxed(const Relaxed&) = delete;
		Relaxed& operator=(const Relaxed&) = delete;

		operator T() const { return LoadRelaxedConst(m_value); }
		T Load() const { return LoadRelaxedConst(m_value); }

		Relaxed& operator=(T value)
		{
			StoreRelaxed(m_value, value);
			return *this;
		}

		// Two separate relaxed accesses, not an atomic read-modify-write: there is one writing thread.
		Relaxed& operator&=(T value)
		{
			StoreRelaxed(m_value, static_cast<T>(LoadRelaxed(m_value) & value));
			return *this;
		}
		Relaxed& operator|=(T value)
		{
			StoreRelaxed(m_value, static_cast<T>(LoadRelaxed(m_value) | value));
			return *this;
		}

	private:
		T m_value{};
	};

	// word = fn(word) as one step, for updates that look at the old value more than once (the IOP's write
	// to the SBUS control register is one). fn may run more than once if the other thread changes the
	// word in between, so it must have no effect of its own. Returns the old value.
	template <typename Fn>
	__forceinline_odr u32 Update(u32& word, Fn&& fn)
	{
		u32 old = __atomic_load_n(&word, __ATOMIC_RELAXED);
		while (!__atomic_compare_exchange_n(&word, &old, fn(old), /*weak=*/true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		{
		}
		return old;
	}
} // namespace SharedWord
