// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSBlockId.h"
#include "GS/GSXXH.h"

#include <cstring>

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
	};

	Impl Select()
	{
#ifdef GS_BLOCK_ID_ARM64
		if (HasAES())
			return {GSBlockId::Kind::AES, HashAES, HashAESMasked};
		if (HasCRC())
			return {GSBlockId::Kind::CRC, HashCRC, HashCRCMasked};
#endif
		return {GSBlockId::Kind::XXH3, HashXXH3, HashXXH3Masked};
	}

	const Impl s_impl = Select();
} // namespace

GSBlockId::Kind GSBlockId::Selected()
{
	return s_impl.kind;
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
	return s_impl.hash(block);
}

u64 GSBlockId::HashMasked(const void* block, u32 mask)
{
	return s_impl.masked(block, mask);
}

u64 GSBlockId::HashFill(u32 word)
{
	alignas(16) u32 block[kBlockSize / sizeof(u32)];
	for (u32& w : block)
		w = word;
	return s_impl.hash(block);
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
