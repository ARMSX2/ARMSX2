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
// The calls are the compiler's __atomic builtins on the plain storage. That is what std::atomic_ref
// would be, and it is not used because the Android NDK's libc++ (r28, LLVM 19) does not have it. The
// builtins are lock-free for these sizes on every host we build for, so each call is one instruction (a
// load, a store, or one read-modify-write), and the relaxed forms are exactly the plain load and store.
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

	// word |= bits. Returns the old value.
	__forceinline_odr u32 FetchOr(u32& word, u32 bits)
	{
		return __atomic_fetch_or(&word, bits, __ATOMIC_ACQ_REL);
	}

	// word &= mask. Returns the old value.
	__forceinline_odr u32 FetchAnd(u32& word, u32 mask)
	{
		return __atomic_fetch_and(&word, mask, __ATOMIC_ACQ_REL);
	}

	// A load of a word through a const reference. The load does not change the word, so the const is
	// dropped; the builtins take a plain pointer for the word they load from.
	template <typename T>
	__forceinline_odr T LoadRelaxedConst(const T& word)
	{
		return LoadRelaxed(const_cast<T&>(word));
	}

	// A field that the host thread writes and the IOP thread reads (the pad input fields, design 8.4).
	// The storage is one T, so a struct or an array of these has the layout it had with plain T, and
	// every access is a relaxed atomic load or store, which is the plain ldr or str. Relaxed is all a pad
	// needs: the console samples its pad asynchronously too, and a pad byte read a poll late is a pad
	// byte read a poll late. Copying is deleted so that no code copies a field the other thread is
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

		// Read-modify-write in two steps, as the plain `field &= x` was. There is one writing thread, so
		// the pair needs no atomic read-modify-write instruction.
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
