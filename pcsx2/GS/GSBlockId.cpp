// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSBlockId.h"
#include "GS/GSXXH.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_M_ARM64) || defined(__aarch64__)
#define GS_BLOCK_ID_ARM64 1
#include <arm_neon.h>
#if defined(__clang__) || defined(__GNUC__)
#include <arm_acle.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#endif
#endif

namespace
{
	constexpr size_t kBlockSize = 256;

	u64 HashXXH3(const void* block)
	{
		return GSXXH3_64bits(block, kBlockSize);
	}

	u64 HashXXH3Masked(const void* block, u32 mask)
	{
		alignas(16) u32 masked[kBlockSize / sizeof(u32)];
		std::memcpy(masked, block, kBlockSize);
		for (u32& w : masked)
			w &= mask;
		return GSXXH3_64bits(masked, kBlockSize);
	}

	void HashManyXXH3(const u8* const* blocks, u64* out, u32 count, u32 mask)
	{
		for (u32 i = 0; i < count; i++)
			out[i] = (mask == 0xFFFFFFFFu) ? HashXXH3(blocks[i]) : HashXXH3Masked(blocks[i], mask);
	}

	u64 CombineXXH3(const u64* ids, u32 count)
	{
		return GSXXH3_64bits(ids, static_cast<size_t>(count) * sizeof(u64));
	}

#ifdef GS_BLOCK_ID_ARM64

#if defined(__clang__)
#define GS_TARGET_AES __attribute__((target("aes")))
#define GS_TARGET_CRC __attribute__((target("crc")))
#elif defined(__GNUC__)
#define GS_TARGET_AES __attribute__((target("+aes")))
#define GS_TARGET_CRC __attribute__((target("+crc")))
#else
#define GS_TARGET_AES
#define GS_TARGET_CRC
#endif

	// AES: four lanes, each takes every fourth 16-byte word (the four registers of one 64-byte load) through two
	// rounds, the second keyed by a constant, so a word's change is spread over the whole lane state before the next
	// word arrives. (One round is not enough: a one-byte change leaves a four-byte difference that a later word or
	// another lane can cancel; 2 of 2,048 one-bit flips of a random block collided that way.) The lanes start from
	// different constants, and pass two more rounds after they are chained into one state.
	template <bool masked>
	GS_TARGET_AES __forceinline u64 HashAESImpl(const void* block, u32 mask)
	{
		const u8* p = static_cast<const u8*>(block);
		uint8x16_t s0 = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0x243F6A8885A308D3ULL), vcreate_u64(0x13198A2E03707344ULL)));
		uint8x16_t s1 = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0xA4093822299F31D0ULL), vcreate_u64(0x082EFA98EC4E6C89ULL)));
		uint8x16_t s2 = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0x452821E638D01377ULL), vcreate_u64(0xBE5466CF34E90C6CULL)));
		uint8x16_t s3 = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0xC0AC29B7C97C50DDULL), vcreate_u64(0x3F84D5B5B5470917ULL)));
		const uint8x16_t m = vreinterpretq_u8_u32(vdupq_n_u32(mask));
		const uint8x16_t k = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0x9216D5D98979FB1BULL), vcreate_u64(0xD1310BA698DFB5ACULL)));
		for (int j = 0; j < 4; j++)
		{
			uint8x16x4_t d = vld1q_u8_x4(p + j * 64);
			if constexpr (masked)
			{
				d.val[0] = vandq_u8(d.val[0], m);
				d.val[1] = vandq_u8(d.val[1], m);
				d.val[2] = vandq_u8(d.val[2], m);
				d.val[3] = vandq_u8(d.val[3], m);
			}
			s0 = vaesmcq_u8(vaeseq_u8(vaesmcq_u8(vaeseq_u8(s0, d.val[0])), k));
			s1 = vaesmcq_u8(vaeseq_u8(vaesmcq_u8(vaeseq_u8(s1, d.val[1])), k));
			s2 = vaesmcq_u8(vaeseq_u8(vaesmcq_u8(vaeseq_u8(s2, d.val[2])), k));
			s3 = vaesmcq_u8(vaeseq_u8(vaesmcq_u8(vaeseq_u8(s3, d.val[3])), k));
		}
		uint8x16_t t = vaesmcq_u8(vaeseq_u8(s0, s1));
		t = vaesmcq_u8(vaeseq_u8(t, s2));
		t = vaesmcq_u8(vaeseq_u8(t, s3));
		t = vaesmcq_u8(vaeseq_u8(t, k));
		t = vaesmcq_u8(vaeseq_u8(t, k));
		const uint64x2_t q = vreinterpretq_u64_u8(t);
		return vgetq_lane_u64(q, 0) ^ vgetq_lane_u64(q, 1);
	}

	GS_TARGET_AES u64 HashAES(const void* block) { return HashAESImpl<false>(block, 0); }
	GS_TARGET_AES u64 HashAESMasked(const void* block, u32 mask) { return HashAESImpl<true>(block, mask); }

	GS_TARGET_AES void HashManyAES(const u8* const* blocks, u64* out, u32 count, u32 mask)
	{
		if (mask == 0xFFFFFFFFu)
		{
			for (u32 i = 0; i < count; i++)
				out[i] = HashAESImpl<false>(blocks[i], 0);
		}
		else
		{
			for (u32 i = 0; i < count; i++)
				out[i] = HashAESImpl<true>(blocks[i], mask);
		}
	}

	// The identities are themselves the outputs of the block identity, so a change of one is a change of its whole
	// 64 bits, not a structured few bytes: one round per two identities spreads it before the next pair arrives.
	// Two more rounds after the last, with the count, and the halves folded.
	GS_TARGET_AES u64 CombineAES(const u64* ids, u32 count)
	{
		uint8x16_t s = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0x6A09E667F3BCC908ULL), vcreate_u64(0xBB67AE8584CAA73BULL)));
		const uint8x16_t k = vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(0x3C6EF372FE94F82BULL), vcreate_u64(0xA54FF53A5F1D36F1ULL)));
		u32 i = 0;
		for (; i + 2 <= count; i += 2)
			s = vaesmcq_u8(vaeseq_u8(s, vreinterpretq_u8_u64(vld1q_u64(ids + i))));
		const u64 last = (i < count) ? ids[i] : 0;
		s = vaesmcq_u8(vaeseq_u8(s, vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(last), vcreate_u64(count)))));
		s = vaesmcq_u8(vaeseq_u8(s, k));
		s = vaesmcq_u8(vaeseq_u8(s, k));
		const uint64x2_t q = vreinterpretq_u64_u8(s);
		return vgetq_lane_u64(q, 0) ^ vgetq_lane_u64(q, 1);
	}

	__forceinline u64 Fmix64(u64 k)
	{
		k ^= k >> 33;
		k *= 0xFF51AFD7ED558CCDULL;
		k ^= k >> 33;
		k *= 0xC4CEB9FE1A85EC53ULL;
		k ^= k >> 33;
		return k;
	}

	// CRC: CRC32 and CRC32C of the even 8-byte words, and of the odd ones; four independent chains of sixteen.
	template <bool masked>
	GS_TARGET_CRC __forceinline u64 HashCRCImpl(const void* block, u32 mask)
	{
		const u8* p = static_cast<const u8*>(block);
		const u64 m = static_cast<u64>(mask) | (static_cast<u64>(mask) << 32);
		u32 a0 = ~0u, a1 = ~0u, b0 = ~0u, b1 = ~0u;
		for (size_t i = 0; i < kBlockSize; i += 16)
		{
			u64 x, y;
			std::memcpy(&x, p + i, 8);
			std::memcpy(&y, p + i + 8, 8);
			if constexpr (masked)
			{
				x &= m;
				y &= m;
			}
			a0 = __crc32d(a0, x);
			b0 = __crc32cd(b0, x);
			a1 = __crc32d(a1, y);
			b1 = __crc32cd(b1, y);
		}
		const u64 even = (static_cast<u64>(a0) << 32) | b0;
		const u64 odd = (static_cast<u64>(a1) << 32) | b1;
		return Fmix64(even) ^ odd;
	}

	GS_TARGET_CRC u64 HashCRC(const void* block) { return HashCRCImpl<false>(block, 0); }
	GS_TARGET_CRC u64 HashCRCMasked(const void* block, u32 mask) { return HashCRCImpl<true>(block, mask); }

	GS_TARGET_CRC void HashManyCRC(const u8* const* blocks, u64* out, u32 count, u32 mask)
	{
		if (mask == 0xFFFFFFFFu)
		{
			for (u32 i = 0; i < count; i++)
				out[i] = HashCRCImpl<false>(blocks[i], 0);
		}
		else
		{
			for (u32 i = 0; i < count; i++)
				out[i] = HashCRCImpl<true>(blocks[i], mask);
		}
	}

	bool HasAES()
	{
#if defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO)
		return true;
#elif defined(__linux__) || defined(__ANDROID__)
		return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#else
		return false;
#endif
	}

	bool HasCRC()
	{
#if defined(__ARM_FEATURE_CRC32)
		return true;
#elif defined(__linux__) || defined(__ANDROID__)
		return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
		return false;
#endif
	}

#endif // GS_BLOCK_ID_ARM64

	struct Impl
	{
		GSBlockId::Kind kind;
		u64 (*hash)(const void*);
		u64 (*masked)(const void*, u32);
		void (*many)(const u8* const*, u64*, u32, u32);
		u64 (*combine)(const u64*, u32);
	};

	bool ImplFor(GSBlockId::Kind kind, Impl* out)
	{
		switch (kind)
		{
#ifdef GS_BLOCK_ID_ARM64
			case GSBlockId::Kind::AES:
				if (!HasAES())
					return false;
				*out = {GSBlockId::Kind::AES, HashAES, HashAESMasked, HashManyAES, CombineAES};
				return true;
			case GSBlockId::Kind::CRC:
				if (!HasCRC())
					return false;
				*out = {GSBlockId::Kind::CRC, HashCRC, HashCRCMasked, HashManyCRC, CombineXXH3};
				return true;
#endif
			case GSBlockId::Kind::XXH3:
				*out = {GSBlockId::Kind::XXH3, HashXXH3, HashXXH3Masked, HashManyXXH3, CombineXXH3};
				return true;
			default:
				return false;
		}
	}

	// The choice is made once, on the first use (the GS thread, on whatever core it runs then), by timing each
	// implementation the CPU has on the same 32 blocks: the best of five passes of 128 blocks each, about 100 to
	// 300 microseconds in all on an A55 and a few tens on a big core. AES is kept unless another is more than a tenth
	// faster. The choice never changes afterwards: a thread that later moves to a core of another kind keeps it (on a
	// big.LITTLE part the result may then be the second best there, never wrong), since every identity of a run must
	// come from one function.
	constexpr u32 kTimingBlocks = 32;
	constexpr u32 kTimingRepeats = 4;
	constexpr u32 kTimingPasses = 5;

	u64 TimeImpl(const Impl& impl, const u8* const* blocks)
	{
		u64 out[kTimingBlocks];
		u64 sink = 0;
		u64 best = ~u64(0);
		for (u32 pass = 0; pass < kTimingPasses; pass++)
		{
			const auto t0 = std::chrono::steady_clock::now();
			for (u32 r = 0; r < kTimingRepeats; r++)
			{
				impl.many(blocks, out, kTimingBlocks, 0xFFFFFFFFu);
				sink ^= out[r];
			}
			const auto t1 = std::chrono::steady_clock::now();
			best = std::min<u64>(best, static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
		}
		volatile u64 keep = sink;
		(void)keep;
		return best;
	}

	Impl s_impl;
	std::atomic<bool> s_ready{false};
	std::mutex s_select_mutex;
	u64 s_timing_ns[3] = {}; // per kind, for the log and the tests; 0 where not timed

	void SelectByTiming()
	{
		alignas(16) static u8 buffer[kTimingBlocks * kBlockSize];
		u64 state = 0x9E3779B97F4A7C15ULL;
		for (size_t i = 0; i < sizeof(buffer); i += 8)
		{
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			std::memcpy(buffer + i, &state, 8);
		}
		const u8* blocks[kTimingBlocks];
		for (u32 i = 0; i < kTimingBlocks; i++)
			blocks[i] = buffer + i * kBlockSize;

		const GSBlockId::Kind kinds[] = {GSBlockId::Kind::AES, GSBlockId::Kind::CRC, GSBlockId::Kind::XXH3};
		Impl best_impl = {};
		u64 best_ns = ~u64(0);
		u64 aes_ns = 0;
		Impl aes_impl = {};
		for (const GSBlockId::Kind kind : kinds)
		{
			Impl impl;
			if (!ImplFor(kind, &impl))
				continue;
			const u64 ns = std::max<u64>(1, TimeImpl(impl, blocks));
			s_timing_ns[static_cast<int>(kind)] = ns;
			if (kind == GSBlockId::Kind::AES)
			{
				aes_ns = ns;
				aes_impl = impl;
			}
			if (ns < best_ns)
			{
				best_ns = ns;
				best_impl = impl;
			}
		}
		if (aes_ns && aes_ns * 10 <= best_ns * 11)
			best_impl = aes_impl;
		s_impl = best_impl;

		const double per_block = 1.0 / (kTimingBlocks * kTimingRepeats);
		Console.WriteLnFmt("GS: block identity {} (ns per block: AES {:.1f}, CRC32 {:.1f}, XXH3 {:.1f}; 0 = not on this CPU)",
			GSBlockId::KindName(s_impl.kind), s_timing_ns[0] * per_block, s_timing_ns[1] * per_block, s_timing_ns[2] * per_block);
	}

	__forceinline const Impl& Current()
	{
		if (!s_ready.load(std::memory_order_acquire)) [[unlikely]]
		{
			std::lock_guard<std::mutex> lock(s_select_mutex);
			if (!s_ready.load(std::memory_order_relaxed))
			{
				SelectByTiming();
				s_ready.store(true, std::memory_order_release);
			}
		}
		return s_impl;
	}
} // namespace

GSBlockId::Kind GSBlockId::Selected()
{
	return Current().kind;
}

const char* GSBlockId::KindName(Kind kind)
{
	switch (kind)
	{
		case Kind::AES:
			return "AES";
		case Kind::CRC:
			return "CRC32";
		default:
			return "XXH3";
	}
}

u64 GSBlockId::Hash(const void* block)
{
	return Current().hash(block);
}

u64 GSBlockId::HashMasked(const void* block, u32 mask)
{
	return Current().masked(block, mask);
}

void GSBlockId::HashMany(const u8* const* blocks, u64* out, u32 count, u32 mask)
{
	Current().many(blocks, out, count, mask);
}

u64 GSBlockId::Combine(const u64* ids, u32 count)
{
	return Current().combine(ids, count);
}

u64 GSBlockId::HashFill(u32 word)
{
	alignas(16) u32 block[kBlockSize / sizeof(u32)];
	for (u32& w : block)
		w = word;
	return Current().hash(block);
}

bool GSBlockId::ForceForTests(Kind kind)
{
	Impl impl;
	if (!ImplFor(kind, &impl))
		return false;
	std::lock_guard<std::mutex> lock(s_select_mutex);
	s_impl = impl;
	s_ready.store(true, std::memory_order_release);
	return true;
}

u64 GSBlockId::TimedNanosecondsForTests(Kind kind)
{
	Current();
	return s_timing_ns[static_cast<int>(kind)];
}

bool GSBlockId::HashWith(Kind kind, const void* block, u64* out)
{
	switch (kind)
	{
#ifdef GS_BLOCK_ID_ARM64
		case Kind::AES:
			if (!HasAES())
				return false;
			*out = HashAES(block);
			return true;
		case Kind::CRC:
			if (!HasCRC())
				return false;
			*out = HashCRC(block);
			return true;
#endif
		case Kind::XXH3:
			*out = HashXXH3(block);
			return true;
		default:
			return false;
	}
}
