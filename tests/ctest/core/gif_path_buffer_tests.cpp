// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// GIF path 2/3 buffer sizes, and that a save state load keeps the running build's size and limit.

#include "Config.h"
#include "Gif_Unit.h"
#include "SaveState.h"

#include <cstring>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

namespace
{
	constexpr u64 kGiB = 1ull << 30;
	constexpr size_t kHeadSize = sizeof(Gif_Path) - sizeof(Gif_Path_MTVU);

	TEST(GifPathBuffer, TwoOrMoreGigabytesAndUnknownGetThirtyTwoMegabytes)
	{
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(0), 32u * _1mb);
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(8 * kGiB), 32u * _1mb);
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(3 * kGiB), 32u * _1mb);
	}

	TEST(GifPathBuffer, TheLowMemoryLineIsTwoGigabytes)
	{
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(Pcsx2Config::LOW_MEMORY_HOST_BYTES), 32u * _1mb);
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(Pcsx2Config::LOW_MEMORY_HOST_BYTES - 1), 9u * _1mb);
		EXPECT_EQ(Gif_Unit::EePathBufferBytes(kGiB), 9u * _1mb);
	}

	TEST(GifPathBuffer, ThisHostsPathsTwoAndThreeHaveTheSizeTheRuleGives)
	{
		const u32 expected = Gif_Unit::EePathBufferBytes(GetPhysicalMemory());
		EXPECT_EQ(gifUnit.gifPath[GIF_PATH_2].buffSize, expected);
		EXPECT_EQ(gifUnit.gifPath[GIF_PATH_3].buffSize, expected);
		EXPECT_EQ(gifUnit.gifPath[GIF_PATH_3].buffLimit, expected - (_1mb + _1kb));
		EXPECT_EQ(gifUnit.gifPath[GIF_PATH_1].buffSize, 9u * _1mb);
	}

	// A save-state stream over a plain buffer that exposes one GIF path's block.
	class GifPathState final : public SaveStateBase
	{
	public:
		GifPathState(VmStateBuffer& buf, bool saving)
			: SaveStateBase(buf)
			, m_saving(saving)
		{
		}

		bool FreezePath(u32 path) { return gifPathFreeze(path); }

		void FreezeMem(void* data, int size) override
		{
			if (!size)
				return;
			if (m_saving)
			{
				if (static_cast<u32>(m_idx + size) > m_memory.size())
					m_memory.resize(static_cast<u32>(m_idx + size));
				std::memcpy(&m_memory[m_idx], data, size);
			}
			else
			{
				ASSERT_LE(static_cast<u32>(m_idx + size), m_memory.size());
				std::memcpy(data, &m_memory[m_idx], size);
			}
			m_idx += size;
		}

		bool IsSaving() const override { return m_saving; }

	private:
		bool m_saving;
	};

	class GifPathStateLoad : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			Gif_Path& live = gifUnit.gifPath[GIF_PATH_3];
			std::memcpy(m_head, &live, kHeadSize);
		}

		void TearDown() override
		{
			Gif_Path& live = gifUnit.gifPath[GIF_PATH_3];
			std::memcpy(&live, m_head, kHeadSize);
		}

		// The block a build with `buff_size`-byte buffers saves for path 3 with a fill of `cur_size` bytes, followed by
		// `data` (shorter than the fill for a state whose fill does not fit and is never read).
		static SaveStateBase::VmStateBuffer SavedBlock(u32 buff_size, u32 cur_size, const std::vector<u8>& data)
		{
			// On the heap: the struct carries the MTVU packet queue, several MB.
			const std::unique_ptr<Gif_Path> owner = std::make_unique<Gif_Path>();
			Gif_Path& saved = *owner;
			saved.Init(GIF_PATH_3, buff_size, _1mb + _1kb);
			saved.curSize = cur_size;
			saved.curOffset = cur_size;

			SaveStateBase::VmStateBuffer block;
			const u32 fake_packets = 0;
			const auto append = [&block](const void* p, size_t n) {
				if (n == 0)
					return;
				const size_t at = block.size();
				block.resize(at + n);
				std::memcpy(&block[at], p, n);
			};
			append(&fake_packets, sizeof(fake_packets));
			append(&saved, kHeadSize);
			append(data.data(), data.size());
			return block;
		}

		alignas(Gif_Path) u8 m_head[kHeadSize];
	};

	TEST_F(GifPathStateLoad, AStateFromASmallerBufferKeepsThisBuildsSizeAndLimit)
	{
		Gif_Path& live = gifUnit.gifPath[GIF_PATH_3];
		const u32 size = live.buffSize;
		const u32 limit = live.buffLimit;
		u8* const buffer = live.buffer;

		std::vector<u8> data(64);
		for (size_t i = 0; i < data.size(); i++)
			data[i] = static_cast<u8>(0xA0 + i);
		SaveStateBase::VmStateBuffer block = SavedBlock(9 * _1mb, static_cast<u32>(data.size()), data);

		GifPathState loader(block, false);
		ASSERT_TRUE(loader.FreezePath(GIF_PATH_3));
		EXPECT_TRUE(loader.IsOkay());
		EXPECT_EQ(live.buffSize, size);
		EXPECT_EQ(live.buffLimit, limit);
		EXPECT_EQ(live.buffer, buffer);
		EXPECT_EQ(live.curSize, data.size());
		EXPECT_EQ(std::memcmp(live.buffer, data.data(), data.size()), 0);
		EXPECT_EQ(live.getReadAmount(), 0);
	}

	// An older build copies the size and limit out of the state over its own 9 MB buffer's, so what a state records
	// has to be safe for it whatever this build's buffers are.
	TEST_F(GifPathStateLoad, ASavedStateRecordsTheSizeOlderBuildsAllocate)
	{
		Gif_Path& live = gifUnit.gifPath[GIF_PATH_3];
		const u32 size = live.buffSize;
		const u32 limit = live.buffLimit;

		SaveStateBase::VmStateBuffer block;
		GifPathState saver(block, true);
		ASSERT_TRUE(saver.FreezePath(GIF_PATH_3));

		alignas(Gif_Path) u8 head[kHeadSize];
		ASSERT_GE(block.size(), sizeof(u32) + kHeadSize);
		std::memcpy(head, &block[sizeof(u32)], kHeadSize);
		const Gif_Path* const saved = reinterpret_cast<const Gif_Path*>(head);
		EXPECT_EQ(saved->buffSize, 9u * _1mb);
		EXPECT_EQ(saved->buffLimit, 9u * _1mb - (_1mb + _1kb));
		EXPECT_EQ(live.buffSize, size);
		EXPECT_EQ(live.buffLimit, limit);
	}

	TEST_F(GifPathStateLoad, AStateHoldingMoreThanTheBufferFailsAndLeavesThePathEmpty)
	{
		Gif_Path& live = gifUnit.gifPath[GIF_PATH_3];
		const u32 size = live.buffSize;
		u8* const buffer = live.buffer;

		SaveStateBase::VmStateBuffer block = SavedBlock(size + _1mb * 8, size + 16, {});

		GifPathState loader(block, false);
		EXPECT_FALSE(loader.FreezePath(GIF_PATH_3));
		EXPECT_TRUE(loader.HasError());
		EXPECT_EQ(live.buffSize, size);
		EXPECT_EQ(live.buffer, buffer);
		EXPECT_EQ(live.curSize, 0u);
		EXPECT_EQ(live.curOffset, 0u);
	}
} // namespace
