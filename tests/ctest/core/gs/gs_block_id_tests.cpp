// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins GSBlockId, the identity of a 256-byte block of GS local memory that the texture cache's internal keys and the
// burst memo use: a function of the bytes alone, sensitive to every bit and to the order of the words, the same through
// each of its entry points, and on every implementation this CPU has.

#include "GS/GSBlockId.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <set>
#include <vector>

namespace
{
	constexpr size_t kBlock = 256;
	constexpr GSBlockId::Kind kKinds[] = {GSBlockId::Kind::AES, GSBlockId::Kind::CRC, GSBlockId::Kind::XXH3};

	struct alignas(16) Block
	{
		u8 b[kBlock];
	};

	Block RandomBlock(std::mt19937_64& rng)
	{
		Block blk;
		for (size_t i = 0; i < kBlock; i += 8)
		{
			const u64 v = rng();
			std::memcpy(blk.b + i, &v, 8);
		}
		return blk;
	}

	TEST(GSBlockId, TheSelectedImplementationIsOneThisCpuHas)
	{
		Block blk = {};
		u64 out = 0;
		EXPECT_TRUE(GSBlockId::HashWith(GSBlockId::Selected(), blk.b, &out));
		EXPECT_EQ(out, GSBlockId::Hash(blk.b));
		std::printf("GSBlockId: %s\n", GSBlockId::KindName(GSBlockId::Selected()));
	}

	TEST(GSBlockId, EqualBytesGiveEqualIdentitiesWhereverTheyLie)
	{
		std::mt19937_64 rng(1);
		std::vector<u8> buf(kBlock * 3 + 16);
		const Block blk = RandomBlock(rng);
		u8* a = buf.data();
		u8* b = buf.data() + kBlock + 16; // another address, still 16-byte aligned relative to the first
		std::memcpy(a, blk.b, kBlock);
		std::memcpy(b, blk.b, kBlock);
		EXPECT_EQ(GSBlockId::Hash(a), GSBlockId::Hash(b));
	}

	TEST(GSBlockId, AFullMaskIsTheUnmaskedIdentityAndAMaskReadsOnlyItsBits)
	{
		std::mt19937_64 rng(2);
		for (int trial = 0; trial < 64; trial++)
		{
			Block blk = RandomBlock(rng);
			EXPECT_EQ(GSBlockId::HashMasked(blk.b, 0xFFFFFFFFu), GSBlockId::Hash(blk.b));

			for (const u32 mask : {0x00FFFFFFu, 0xFF000000u, 0x0F000000u, 0xF0000000u})
			{
				Block masked = blk;
				for (size_t i = 0; i < kBlock; i += 4)
				{
					u32 w;
					std::memcpy(&w, masked.b + i, 4);
					w &= mask;
					std::memcpy(masked.b + i, &w, 4);
				}
				EXPECT_EQ(GSBlockId::HashMasked(blk.b, mask), GSBlockId::Hash(masked.b)) << std::hex << mask;

				// Bits outside the mask do not count; bits inside it do.
				Block other = blk;
				other.b[(trial * 4) % kBlock + 0] ^= static_cast<u8>(~mask);
				other.b[(trial * 4) % kBlock + 1] ^= static_cast<u8>(~mask >> 8);
				other.b[(trial * 4) % kBlock + 2] ^= static_cast<u8>(~mask >> 16);
				other.b[(trial * 4) % kBlock + 3] ^= static_cast<u8>(~mask >> 24);
				EXPECT_EQ(GSBlockId::HashMasked(other.b, mask), GSBlockId::HashMasked(blk.b, mask)) << std::hex << mask;
				other = blk;
				const u32 inside = mask & (0u - mask); // the lowest bit of the mask
				u32 w;
				std::memcpy(&w, other.b + (trial * 4) % kBlock, 4);
				w ^= inside;
				std::memcpy(other.b + (trial * 4) % kBlock, &w, 4);
				EXPECT_NE(GSBlockId::HashMasked(other.b, mask), GSBlockId::HashMasked(blk.b, mask)) << std::hex << mask;
			}
		}
	}

	TEST(GSBlockId, AFilledBlockHasTheIdentityOfItsBytes)
	{
		for (const u32 word : {0u, 0xFFFFFFFFu, 0x80808080u, 0x12345678u, 0x00FF00FFu})
		{
			Block blk;
			for (size_t i = 0; i < kBlock; i += 4)
				std::memcpy(blk.b + i, &word, 4);
			EXPECT_EQ(GSBlockId::HashFill(word), GSBlockId::Hash(blk.b)) << std::hex << word;
		}
	}

	TEST(GSBlockId, EveryImplementationSeesEveryBitAndTheOrderOfTheWords)
	{
		std::mt19937_64 rng(3);
		for (const GSBlockId::Kind kind : kKinds)
		{
			const Block blk = RandomBlock(rng);
			u64 base;
			if (!GSBlockId::HashWith(kind, blk.b, &base))
				continue;

			std::set<u64> seen{base};
			for (size_t bit = 0; bit < kBlock * 8; bit++)
			{
				Block flipped = blk;
				flipped.b[bit / 8] ^= static_cast<u8>(1u << (bit % 8));
				u64 h;
				ASSERT_TRUE(GSBlockId::HashWith(kind, flipped.b, &h));
				EXPECT_TRUE(seen.insert(h).second) << GSBlockId::KindName(kind) << " bit " << bit;
			}

			// Two words swapped, at every distance of 8-byte words.
			for (size_t i = 0; i < 32; i++)
			{
				for (size_t j = i + 1; j < 32; j++)
				{
					Block swapped = blk;
					std::memcpy(swapped.b + i * 8, blk.b + j * 8, 8);
					std::memcpy(swapped.b + j * 8, blk.b + i * 8, 8);
					u64 h;
					ASSERT_TRUE(GSBlockId::HashWith(kind, swapped.b, &h));
					EXPECT_NE(h, base) << GSBlockId::KindName(kind) << " words " << i << " and " << j;
				}
			}
		}
	}

	TEST(GSBlockId, TheSameChangeInBothHalvesOfTheWordsIsSeen)
	{
		// The CRC form keeps the even and the odd words apart, and CRC is linear: the same change made to an even
		// word and to the odd word after it changes both halves by the same amount, which a plain XOR of the halves
		// would cancel. The mix in between must not.
		std::mt19937_64 rng(4);
		for (const GSBlockId::Kind kind : kKinds)
		{
			for (int trial = 0; trial < 256; trial++)
			{
				const Block blk = RandomBlock(rng);
				u64 base;
				if (!GSBlockId::HashWith(kind, blk.b, &base))
					break;
				const u64 delta = rng() | 1;
				const size_t w = (trial % 16) * 2;
				Block changed = blk;
				u64 x, y;
				std::memcpy(&x, changed.b + w * 8, 8);
				std::memcpy(&y, changed.b + w * 8 + 8, 8);
				x ^= delta;
				y ^= delta;
				std::memcpy(changed.b + w * 8, &x, 8);
				std::memcpy(changed.b + w * 8 + 8, &y, 8);
				u64 h;
				ASSERT_TRUE(GSBlockId::HashWith(kind, changed.b, &h));
				EXPECT_NE(h, base) << GSBlockId::KindName(kind) << " trial " << trial;
			}
		}
	}

	TEST(GSBlockId, ManyDifferentBlocksHaveDifferentIdentities)
	{
		// A smoke test of the spread: 2^17 random blocks and 2^17 blocks that differ from one another in one small
		// counter, no two alike. At 2^-64 a pair, a collision here would mean a broken construction, not bad luck.
		std::mt19937_64 rng(5);
		for (const GSBlockId::Kind kind : kKinds)
		{
			u64 probe;
			Block zero = {};
			if (!GSBlockId::HashWith(kind, zero.b, &probe))
				continue;
			std::set<u64> seen;
			for (u32 i = 0; i < (1u << 17); i++)
			{
				const Block blk = RandomBlock(rng);
				u64 h;
				GSBlockId::HashWith(kind, blk.b, &h);
				ASSERT_TRUE(seen.insert(h).second) << GSBlockId::KindName(kind) << " random " << i;
			}
			for (u32 i = 0; i < (1u << 17); i++)
			{
				Block blk = {};
				std::memcpy(blk.b + (i % 61) * 4, &i, 4);
				u64 h;
				GSBlockId::HashWith(kind, blk.b, &h);
				ASSERT_TRUE(seen.insert(h).second) << GSBlockId::KindName(kind) << " counter " << i;
			}
		}
	}
	TEST(GSBlockId, ManyIsEachOneAtATime)
	{
		std::mt19937_64 rng(6);
		std::vector<Block> blks;
		for (int i = 0; i < 37; i++)
			blks.push_back(RandomBlock(rng));
		std::vector<const u8*> ptrs;
		for (const Block& b : blks)
			ptrs.push_back(b.b);
		for (const u32 mask : {0xFFFFFFFFu, 0x00FFFFFFu, 0xFF000000u})
		{
			std::vector<u64> out(blks.size());
			GSBlockId::HashMany(ptrs.data(), out.data(), static_cast<u32>(ptrs.size()), mask);
			for (size_t i = 0; i < blks.size(); i++)
				EXPECT_EQ(out[i], GSBlockId::HashMasked(blks[i].b, mask)) << i << " mask " << std::hex << mask;
		}
	}

	TEST(GSBlockId, ACombinedSequenceSeesEveryIdentityItsOrderAndItsLength)
	{
		std::mt19937_64 rng(7);
		std::vector<u64> ids(64);
		for (u64& id : ids)
			id = rng();
		std::set<u64> seen;
		for (u32 n = 0; n <= ids.size(); n++)
			EXPECT_TRUE(seen.insert(GSBlockId::Combine(ids.data(), n)).second) << "length " << n;
		const u64 base = GSBlockId::Combine(ids.data(), 64);
		for (u32 i = 0; i < 64; i++)
		{
			for (u32 bit = 0; bit < 64; bit += 7)
			{
				std::vector<u64> changed = ids;
				changed[i] ^= u64(1) << bit;
				EXPECT_TRUE(seen.insert(GSBlockId::Combine(changed.data(), 64)).second) << "id " << i << " bit " << bit;
			}
			for (u32 j = i + 1; j < 64; j++)
			{
				std::vector<u64> swapped = ids;
				std::swap(swapped[i], swapped[j]);
				EXPECT_NE(GSBlockId::Combine(swapped.data(), 64), base) << i << " and " << j;
			}
		}
		// A sequence of one repeated identity: only the count tells them apart.
		std::vector<u64> same(20, 0x1234);
		std::set<u64> by_count;
		for (u32 n = 1; n <= 20; n++)
			EXPECT_TRUE(by_count.insert(GSBlockId::Combine(same.data(), n)).second) << n;
	}
	TEST(GSBlockId, TheChoiceIsMadeOnceFromWhatThisCpuHasAndWasTimed)
	{
		const GSBlockId::Kind chosen = GSBlockId::Selected();
		for (int i = 0; i < 100; i++)
			ASSERT_EQ(GSBlockId::Selected(), chosen);
		u64 probe;
		Block blk = {};
		EXPECT_TRUE(GSBlockId::HashWith(chosen, blk.b, &probe));
		EXPECT_GT(GSBlockId::TimedNanosecondsForTests(chosen), 0u);
		EXPECT_GT(GSBlockId::TimedNanosecondsForTests(GSBlockId::Kind::XXH3), 0u) << "XXH3 is on every CPU and is always timed";
		for (const GSBlockId::Kind kind : kKinds)
		{
			// A kind the CPU has was timed; one it lacks was not.
			const bool has = GSBlockId::HashWith(kind, blk.b, &probe);
			EXPECT_EQ(GSBlockId::TimedNanosecondsForTests(kind) > 0, has) << GSBlockId::KindName(kind);
		}
	}

	TEST(GSBlockId, EachImplementationThisCpuHasCanBeChosenAndGivesStableIdentities)
	{
		const GSBlockId::Kind chosen = GSBlockId::Selected();
		std::mt19937_64 rng(8);
		std::vector<Block> blks;
		for (int i = 0; i < 16; i++)
			blks.push_back(RandomBlock(rng));
		std::vector<const u8*> ptrs;
		for (const Block& b : blks)
			ptrs.push_back(b.b);

		u32 tried = 0;
		for (const GSBlockId::Kind kind : kKinds)
		{
			if (!GSBlockId::ForceForTests(kind))
				continue;
			tried++;
			ASSERT_EQ(GSBlockId::Selected(), kind);
			std::vector<u64> ids(blks.size()), again(blks.size());
			GSBlockId::HashMany(ptrs.data(), ids.data(), static_cast<u32>(ptrs.size()), 0xFFFFFFFFu);
			for (size_t i = 0; i < blks.size(); i++)
			{
				u64 direct;
				ASSERT_TRUE(GSBlockId::HashWith(kind, blks[i].b, &direct));
				EXPECT_EQ(ids[i], direct) << GSBlockId::KindName(kind);
				EXPECT_EQ(GSBlockId::Hash(blks[i].b), direct) << GSBlockId::KindName(kind);
				EXPECT_EQ(GSBlockId::HashMasked(blks[i].b, 0xFFFFFFFFu), direct) << GSBlockId::KindName(kind);
			}
			GSBlockId::HashMany(ptrs.data(), again.data(), static_cast<u32>(ptrs.size()), 0xFFFFFFFFu);
			EXPECT_EQ(ids, again) << GSBlockId::KindName(kind);
			EXPECT_EQ(GSBlockId::Combine(ids.data(), 16), GSBlockId::Combine(again.data(), 16)) << GSBlockId::KindName(kind);
			Block fill;
			for (size_t i = 0; i < kBlock; i += 4)
				std::memcpy(fill.b + i, "\x12\x34\x56\x78", 4);
			EXPECT_EQ(GSBlockId::HashFill(0x78563412u), GSBlockId::Hash(fill.b)) << GSBlockId::KindName(kind);
		}
		EXPECT_GE(tried, 1u);
		ASSERT_TRUE(GSBlockId::ForceForTests(chosen)) << "put the run's choice back";
	}
} // namespace
