// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What loading the MTVU block of a save state leaves for the VU1 thread.
//
// A save state carries VU1's micro and data memory as separate zip entries
// (vu1MicroMem.bin, vu1Memory.bin), read straight into VU1.Micro and VU1.Mem
// AFTER the internal-structures block, which is where the MTVU block lives.
// So whatever the MTVU block queues for the VU thread is built from memory as
// it was before the load. It used to queue copies of both memories; the VU
// thread wrote them back whenever it next read its ring, and when that was
// after the entries were read, VU1 ran the old program. From a fresh boot
// that program is all zeros, nothing it runs XGKICKs, and the 3D world stays
// black (Baldur's Gate: Dark Alliance, 11 of 64 MTVU loads).
//
// These tests pin the fixed behaviour: loading the MTVU block queues nothing
// for the VU thread, sets the VU thread's own copy of VIF1's row and column
// registers directly, and VU1 memory written after the block survives the VU
// thread catching up.

#include "harness/RecompilerTestEnvironment.h"

#include "MTVU.h"
#include "SaveState.h"
#include "VUmicro.h"
#include "Vif_Dma.h"

#include <cstring>
#include <memory>
#include <gtest/gtest.h>

namespace {

// A save-state stream over a plain buffer that exposes the MTVU block alone.
class MtvuBlockState final : public SaveStateBase
{
public:
	MtvuBlockState(VmStateBuffer& buf, bool saving)
		: SaveStateBase(buf)
		, m_saving(saving)
	{
	}

	bool FreezeMtvu() { return mtvuFreeze(); }

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

class MtvuSavestateLoad : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(recompiler_tests::RecompilerTestEnvironment::IsReady());
		ASSERT_TRUE(vu1Thread.IsOpen());
		vu1Thread.WaitVU();

		m_micro = std::make_unique<u8[]>(0x4000);
		m_mem = std::make_unique<u8[]>(0x4000);
		std::memcpy(m_micro.get(), VU1.Micro, 0x4000);
		std::memcpy(m_mem.get(), VU1.Mem, 0x4000);
		m_row = vif1.MaskRow;
		m_col = vif1.MaskCol;

		// The MTVU block as a save would write it.
		MtvuBlockState saver(m_block, true);
		ASSERT_TRUE(saver.FreezeMtvu());
	}

	void TearDown() override
	{
		if (!vu1Thread.IsOpen())
			vu1Thread.Open();
		vu1Thread.WaitVU();
		vu1Thread.Reset();
		std::memcpy(VU1.Micro, m_micro.get(), 0x4000);
		std::memcpy(VU1.Mem, m_mem.get(), 0x4000);
		vif1.MaskRow = m_row;
		vif1.MaskCol = m_col;
	}

	// Loads the MTVU block the way SaveState_UnzipFromZip does: VU1 memory
	// still holds the pre-load contents (`before`), and VIF1's row/column
	// registers were restored earlier in the same block (vif1Freeze).
	void LoadBlock(u8 before)
	{
		std::memset(VU1.Micro, before, 0x4000);
		std::memset(VU1.Mem, before, 0x4000);
		for (int i = 0; i < 4; i++)
		{
			vif1.MaskRow._u32[i] = 0x11110000u + i;
			vif1.MaskCol._u32[i] = 0x22220000u + i;
		}
		MtvuBlockState loader(m_block, false);
		ASSERT_TRUE(loader.FreezeMtvu());
	}

	// What the vu1MicroMem.bin / vu1Memory.bin entries do after the block.
	static void LoadVu1MemoryEntries(u8 loaded)
	{
		std::memset(VU1.Micro, loaded, 0x4000);
		std::memset(VU1.Mem, loaded, 0x4000);
	}

	static bool AllBytes(const u8* p, u8 v)
	{
		for (u32 i = 0; i < 0x4000; i++)
		{
			if (p[i] != v)
				return false;
		}
		return true;
	}

	SaveStateBase::VmStateBuffer m_block;
	std::unique_ptr<u8[]> m_micro;
	std::unique_ptr<u8[]> m_mem;
	u128 m_row{};
	u128 m_col{};
};

// With the VU thread stopped, nothing can drain the ring, so whatever the load
// queued is still there to see. The fixed load queues nothing, and the VU
// thread's row/column copies are already VIF1's.
TEST_F(MtvuSavestateLoad, QueuesNothingForTheVuThread)
{
	vu1Thread.Close();
	ASSERT_FALSE(vu1Thread.IsOpen());

	LoadBlock(0x00);
	LoadVu1MemoryEntries(0x5A);

	EXPECT_TRUE(vu1Thread.IsDone()) << "the MTVU block queued work built from pre-load VU1 state";
	EXPECT_EQ(std::memcmp(&vu1Thread.vif.MaskRow, &vif1.MaskRow, sizeof(u128)), 0);
	EXPECT_EQ(std::memcmp(&vu1Thread.vif.MaskCol, &vif1.MaskCol, sizeof(u128)), 0);
	EXPECT_TRUE(AllBytes(VU1.Micro, 0x5A));
	EXPECT_TRUE(AllBytes(VU1.Mem, 0x5A));
}

// With the VU thread running: once it has caught up, VU1 memory is what the
// save state's memory entries wrote, not what was there before the load.
TEST_F(MtvuSavestateLoad, Vu1MemoryFromTheEntriesSurvivesTheVuThread)
{
	for (int round = 0; round < 32; round++)
	{
		LoadBlock(0x00);
		LoadVu1MemoryEntries(0x5A);
		vu1Thread.WaitVU();
		ASSERT_TRUE(AllBytes(VU1.Micro, 0x5A)) << "round " << round << ": VU1 micro memory reverted to its pre-load contents";
		ASSERT_TRUE(AllBytes(VU1.Mem, 0x5A)) << "round " << round << ": VU1 data memory reverted to its pre-load contents";
	}
}

} // namespace
