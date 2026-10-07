// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that GSXXH3BlockState, the hasher the texture hash feeds one 256-byte GS block at a time,
// produces the value XXH3 gives for the same bytes laid end to end.
//
// XXH3's streaming update copies each 256-byte input into its own 256-byte buffer before it
// consumes it. The block hasher keeps a pointer to the previous block instead and consumes it from
// the caller's memory when the next block arrives, so the hash values (and with them the texture
// cache's keys and the replacement texture names) must not move. The reference here is the one-shot
// XXH3_64bits over the concatenated bytes and the plain streaming update, neither of which goes
// through the hasher under test.

#include "GS/GSXXH.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

namespace
{
	constexpr size_t kBlockSize = 256;
	constexpr size_t kBlocksInMemory = 16384; // 4 MiB, the size of GS local memory

	class GSXXHBlockHasher : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_rng.seed(1);
			m_mem.resize(kBlocksInMemory * kBlockSize);
			for (u8& b : m_mem)
				b = static_cast<u8>(m_rng());
		}

		const u8* Block(u32 index) const { return m_mem.data() + static_cast<size_t>(index) * kBlockSize; }

		u32 RandomBlock() { return static_cast<u32>(m_rng() % kBlocksInMemory); }

		/// What XXH3 gives for the bytes, hashed in one piece.
		static u64 OneShot(const std::vector<u8>& bytes) { return XXH3_64bits(bytes.data(), bytes.size()); }

		/// What the plain streaming update gives when it is fed the same pieces.
		struct Reference
		{
			Reference() { XXH3_64bits_reset(&state); }

			void Add(const void* data, size_t len)
			{
				bytes.insert(bytes.end(), static_cast<const u8*>(data), static_cast<const u8*>(data) + len);
				XXH3_64bits_update(&state, data, len);
			}

			XXH3_state_t state;
			std::vector<u8> bytes;
		};

		std::mt19937_64 m_rng;
		std::vector<u8> m_mem;
	};

	TEST_F(GSXXHBlockHasher, ScatteredBlocksEqualTheirConcatenation)
	{
		for (int trial = 0; trial < 2000; trial++)
		{
			const int count = 1 + static_cast<int>(m_rng() % 600);

			GSXXH3BlockState hasher;
			GSXXH3_block_reset(hasher);
			Reference ref;
			for (int i = 0; i < count; i++)
			{
				const u8* block = Block(RandomBlock());
				GSXXH3_64bits_block(&hasher, block);
				ref.Add(block, kBlockSize);
			}

			const u64 got = GSXXH3_64bits_digest(&hasher);
			ASSERT_EQ(got, OneShot(ref.bytes)) << "trial " << trial << ", " << count << " blocks";
			ASSERT_EQ(got, XXH3_64bits_digest(&ref.state)) << "trial " << trial << ", " << count << " blocks";
		}
	}

	TEST_F(GSXXHBlockHasher, EveryShortSequenceEqualsItsConcatenation)
	{
		// One block is already past the 240-byte limit below which XXH3 hashes a different way,
		// and the digest has to take the last block out of the pending pointer.
		for (int count = 1; count <= 20; count++)
		{
			GSXXH3BlockState hasher;
			GSXXH3_block_reset(hasher);
			std::vector<u8> bytes;
			for (int i = 0; i < count; i++)
			{
				const u8* block = Block(RandomBlock());
				GSXXH3_64bits_block(&hasher, block);
				bytes.insert(bytes.end(), block, block + kBlockSize);
			}
			ASSERT_EQ(GSXXH3_64bits_digest(&hasher), OneShot(bytes)) << count << " blocks";
		}
	}

	TEST_F(GSXXHBlockHasher, NoInputEqualsTheEmptyHash)
	{
		GSXXH3BlockState hasher;
		GSXXH3_block_reset(hasher);
		EXPECT_EQ(GSXXH3_64bits_digest(&hasher), XXH3_64bits(nullptr, 0));
	}

	TEST_F(GSXXHBlockHasher, AWholeTextureOfBlocks)
	{
		// A 1 MiB texture is 4096 blocks, in the order a swizzled walk visits them.
		GSXXH3BlockState hasher;
		GSXXH3_block_reset(hasher);
		std::vector<u8> bytes;
		for (u32 i = 0; i < 4096; i++)
		{
			const u8* block = Block((i * 7919) % kBlocksInMemory);
			GSXXH3_64bits_block(&hasher, block);
			bytes.insert(bytes.end(), block, block + kBlockSize);
		}
		EXPECT_EQ(GSXXH3_64bits_digest(&hasher), OneShot(bytes));
	}

	TEST_F(GSXXHBlockHasher, BlocksMixedWithOtherSizes)
	{
		// A texture's mip chain mixes in-place blocks with expanded rows of any length, so a
		// run of blocks can be cut by an update of 0, 1, 255, 256, 257 or many bytes, and a
		// run can begin after one.
		static constexpr size_t kSizes[] = {0, 1, 7, 63, 64, 65, 255, 256, 257, 511, 512, 1000, 4096};
		for (int trial = 0; trial < 2000; trial++)
		{
			GSXXH3BlockState hasher;
			GSXXH3_block_reset(hasher);
			std::vector<u8> bytes;

			const int steps = 1 + static_cast<int>(m_rng() % 40);
			for (int s = 0; s < steps; s++)
			{
				if (m_rng() % 3)
				{
					const u8* block = Block(RandomBlock());
					GSXXH3_64bits_block(&hasher, block);
					bytes.insert(bytes.end(), block, block + kBlockSize);
				}
				else
				{
					const size_t len = kSizes[m_rng() % (sizeof(kSizes) / sizeof(kSizes[0]))];
					const u8* data = Block(RandomBlock()) + (m_rng() % 200); // not block aligned
					GSXXH3_64bits_update(&hasher, data, len);
					bytes.insert(bytes.end(), data, data + len);
				}
			}
			ASSERT_EQ(GSXXH3_64bits_digest(&hasher), OneShot(bytes)) << "trial " << trial << ", " << steps << " steps";
		}
	}

	TEST_F(GSXXHBlockHasher, DigestLeavesTheStateUsable)
	{
		// XXH3_64bits_digest does not consume the state; neither does this one.
		GSXXH3BlockState hasher;
		GSXXH3_block_reset(hasher);
		std::vector<u8> bytes;
		for (int i = 0; i < 12; i++)
		{
			const u8* block = Block(RandomBlock());
			GSXXH3_64bits_block(&hasher, block);
			bytes.insert(bytes.end(), block, block + kBlockSize);
			ASSERT_EQ(GSXXH3_64bits_digest(&hasher), OneShot(bytes)) << (i + 1) << " blocks";
		}
	}
} // namespace
