// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The batching bookkeeping of the GPU local-memory mirror (GSVramMirrorBatch.h): which blocks a window
// stages, what its block table names, how a write between two windows of one batch is kept apart, what
// cancelling a decode leaves behind, the staging ring and meta regions against a model of the GPU's
// progress, and the verify mode's shadow. Everything here is CPU state; the Vulkan class records the
// commands from it (gsrunner -gpu-decode-selftest and -vram-mirror-verify cover that half on a device).

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSVramMirrorBatch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <vector>

using namespace GSVramMirror;

namespace
{
	/// A GPU that only counts: a command buffer is submitted when the test says so, and finishes when it says so.
	class FakeGpu final : public FenceSource
	{
	public:
		u64 Completed() override { return completed; }
		u64 Current() override { return current; }
		void Poll() override { polls++; }
		void Wait(u64 counter) override
		{
			waits.push_back(counter);
			completed = std::max(completed, counter);
		}

		/// The recorded command buffer goes to the GPU; it has not finished.
		void Submit() { current++; }
		/// ... and the GPU finishes everything submitted.
		void FinishAll() { completed = current - 1; }

		u64 completed = 0;
		u64 current = 1;
		int polls = 0;
		std::vector<u64> waits;
	};

	struct Window
	{
		u32 psm;
		u32 tbp;
		u32 tbw;
		GSVector4i rect;
	};

	class GSVramMirrorBatchTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_mem = std::make_unique<GSLocalMemory>();
			std::mt19937_64 rng(0xba7c4);
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 8)
			{
				const u64 v = rng();
				std::memcpy(m_mem->vm8() + i, &v, 8);
			}
			Make(Config());
		}

		void Make(const Config& config)
		{
			m_config = config;
			m_staging.assign(static_cast<size_t>(config.staging_slots) * kBlockBytes, 0);
			m_meta.assign(static_cast<size_t>(config.meta_regions) * config.meta_region_bytes / 4, 0);
			m_gpu = FakeGpu();
			m_batch = std::make_unique<Batch>(config, m_staging.data(), m_meta.data(), &m_gpu);
			m_next_texture = 1;
		}

		Source Src() { return {m_mem->vm8(), m_mem->Id(), m_mem->BlockDirtyWords(), m_mem.get()}; }

		bool Dirty(u32 block) const { return (m_mem->BlockDirtyWords()[block >> 6] >> (block & 63)) & 1; }
		void SetDirty(u32 block) { m_mem->BlockDirtyWords()[block >> 6] |= 1ull << (block & 63); }
		void ClearAllDirty() { std::fill(m_mem->BlockDirtyWords(), m_mem->BlockDirtyWords() + kMirrorBlocks / 64, 0ull); }

		/// Changes a few bytes of a block and, unless `mark` is false, marks it the way a writer does.
		void Write(u32 block, u8 value, bool mark = true)
		{
			u8* p = m_mem->vm8() + static_cast<size_t>(block) * kBlockBytes;
			for (u32 i = 0; i < 8; i++)
				p[i * 16] = static_cast<u8>(value + i);
			if (mark)
				SetDirty(block);
		}

		std::vector<u32> BlocksOf(const Window& w)
		{
			const GSOffset off = m_mem->GetOffset(w.tbp, w.tbw, w.psm);
			WindowLayout layout;
			EXPECT_TRUE(ComputeLayout(off, w.rect, &layout));
			std::vector<u32> blocks;
			BuildBlockNumbers(off, layout, &blocks);
			return blocks;
		}

		/// A texture is any distinct pointer to the batch.
		void* NewTexture() { return reinterpret_cast<void*>(m_next_texture++ * 16); }

		Batch::Result Queue(const Window& w, void* texture, u64 generation = 1)
		{
			const GSOffset off = m_mem->GetOffset(w.tbp, w.tbw, w.psm);
			WindowLayout layout;
			EXPECT_TRUE(ComputeLayout(off, w.rect, &layout));
			return m_batch->Queue(off, layout, Src(), generation, texture);
		}

		/// Queue, answering NeedFlush the way the Vulkan class does.
		Batch::Result QueueFlushing(const Window& w, void* texture)
		{
			Batch::Result r = Queue(w, texture);
			if (r == Batch::Result::NeedFlush)
			{
				Flush();
				m_batch->CountScratchFlush();
				r = Queue(w, texture);
			}
			return r;
		}

		/// Records the batch (nothing here) in the command buffer being built.
		Batch::FlushPlan Flush()
		{
			const Batch::FlushPlan plan = m_batch->PrepareFlush();
			m_batch->FinishFlush(m_gpu.current);
			return plan;
		}

		const u8* StagedBlock(u32 slot) const { return m_staging.data() + static_cast<size_t>(slot) * kBlockBytes; }
		const u8* MemBlock(u32 block) const { return m_mem->vm8() + static_cast<size_t>(block) * kBlockBytes; }

		// One page of PSMT8: 32 blocks.
		static Window Page() { return {PSMT8, 0, 2, GSVector4i(0, 0, 128, 64)}; }
		// Four blocks of PSMT8: 2 x 2.
		static Window Four() { return {PSMT8, 0, 2, GSVector4i(0, 0, 32, 32)}; }

		std::unique_ptr<GSLocalMemory> m_mem;
		Config m_config;
		std::vector<u8> m_staging;
		std::vector<u32> m_meta;
		FakeGpu m_gpu;
		std::unique_ptr<Batch> m_batch;
		uintptr_t m_next_texture = 1;
	};

	TEST_F(GSVramMirrorBatchTest, AFreshBatchHasNothingToRecord)
	{
		EXPECT_FALSE(m_batch->HasPending());
		EXPECT_EQ(m_batch->Jobs().size(), 0u);
		EXPECT_FALSE(m_batch->IsPending(NewTexture()));
	}

	TEST_F(GSVramMirrorBatchTest, AColdWindowStagesEveryBlockInReadTextureOrder)
	{
		// A fresh memory is entirely dirty (the constructor's memset is not a mark), so nothing is in the mirror.
		void* tex = NewTexture();
		const Window w = Page();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(blocks.size(), 32u);

		ASSERT_EQ(Queue(w, tex), Batch::Result::Queued);
		EXPECT_TRUE(m_batch->HasPending());
		EXPECT_TRUE(m_batch->IsPending(tex));
		EXPECT_EQ(m_batch->StagedBlockCount(), 32u);

		const u32* table = m_batch->TableOf(0);
		for (u32 i = 0; i < blocks.size(); i++)
		{
			ASSERT_NE(table[i] & kTableStagedBit, 0u) << "block " << i << " should come from staging";
			const u32 slot = table[i] & ~kTableStagedBit;
			EXPECT_EQ(std::memcmp(StagedBlock(slot), MemBlock(blocks[i]), kBlockBytes), 0) << "block " << i;
			EXPECT_FALSE(Dirty(blocks[i])) << "staging a block takes its dirty bit";
		}
		EXPECT_EQ(m_batch->GetStats().windows, 1u);
		EXPECT_EQ(m_batch->GetStats().blocks_staged, 32u);
		EXPECT_EQ(m_batch->GetStats().blocks_from_staging, 32u);
		EXPECT_EQ(m_batch->GetStats().mixed_windows, 0u);
		EXPECT_EQ(m_batch->StagingInUse(), 32u);
	}

	TEST_F(GSVramMirrorBatchTest, ACleanBlockReadsTheMirrorByItsBlockNumber)
	{
		const Window w = Page();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();

		// The first window took the page into the mirror; a second one over it stages nothing.
		void* second = NewTexture();
		ASSERT_EQ(Queue(w, second), Batch::Result::Queued);
		const u32* table = m_batch->TableOf(0);
		for (u32 i = 0; i < blocks.size(); i++)
			EXPECT_EQ(table[i], blocks[i]) << "a mirror entry is the plain block number (no staged bit)";
		EXPECT_EQ(m_batch->StagedBlockCount(), 0u);
		EXPECT_EQ(m_batch->GetStats().blocks_staged, 32u) << "only the first window staged";
		EXPECT_TRUE(m_batch->HasPending()) << "the decode itself is still to be recorded";
	}

	TEST_F(GSVramMirrorBatchTest, AWindowOverDirtyAndCleanBlocksMixesTheTwo)
	{
		const Window w = Page();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();

		Write(blocks[3], 0x10);
		Write(blocks[20], 0x20);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		const u32* table = m_batch->TableOf(0);
		u32 from_staging = 0;
		for (u32 i = 0; i < blocks.size(); i++)
		{
			if (table[i] & kTableStagedBit)
			{
				from_staging++;
				EXPECT_TRUE(i == 3 || i == 20) << "block " << i;
				EXPECT_EQ(std::memcmp(StagedBlock(table[i] & ~kTableStagedBit), MemBlock(blocks[i]), kBlockBytes), 0);
			}
			else
			{
				EXPECT_EQ(table[i], blocks[i]);
			}
		}
		EXPECT_EQ(from_staging, 2u);
		EXPECT_EQ(m_batch->GetStats().mixed_windows, 1u);
		EXPECT_EQ(m_batch->StagedBlockCount(), 2u);
	}

	TEST_F(GSVramMirrorBatchTest, TwoWindowsOverTheSameDirtyBlocksStageThemOnce)
	{
		const Window w = Four();
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->GetStats().blocks_staged, 4u);
		EXPECT_EQ(m_batch->StagingInUse(), 4u);
		for (u32 i = 0; i < 4; i++)
			EXPECT_EQ(m_batch->TableOf(0)[i], m_batch->TableOf(1)[i]) << "the second window reads the first one's staged copy";
	}

	TEST_F(GSVramMirrorBatchTest, AWriteBetweenTwoWindowsOfOneBatchGetsANewSlotAndTheFirstKeepsItsSnapshot)
	{
		// The same-frame overwrite: a transfer lands after one draw sampled a block and before the next miss.
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		std::vector<u8> before(MemBlock(blocks[1]), MemBlock(blocks[1]) + kBlockBytes);

		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Write(blocks[1], 0x77);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);

		const u32 first_entry = m_batch->TableOf(0)[1];
		const u32 second_entry = m_batch->TableOf(1)[1];
		ASSERT_NE(first_entry & kTableStagedBit, 0u);
		ASSERT_NE(second_entry & kTableStagedBit, 0u);
		EXPECT_NE(first_entry, second_entry) << "a rewritten block restages into another slot";
		EXPECT_EQ(std::memcmp(StagedBlock(first_entry & ~kTableStagedBit), before.data(), kBlockBytes), 0)
			<< "the first window's slot still holds the bytes as they were at its miss";
		EXPECT_EQ(std::memcmp(StagedBlock(second_entry & ~kTableStagedBit), MemBlock(blocks[1]), kBlockBytes), 0);
		EXPECT_NE(std::memcmp(before.data(), MemBlock(blocks[1]), kBlockBytes), 0);
		// The untouched blocks of the second window share the first's slots.
		for (u32 i : {0u, 2u, 3u})
			EXPECT_EQ(m_batch->TableOf(0)[i], m_batch->TableOf(1)[i]) << "block " << i;

		EXPECT_EQ(m_batch->GetStats().blocks_restaged, 1u);
		EXPECT_EQ(m_batch->StagedBlockCount(), 4u) << "the scatter list names a block once";
		EXPECT_EQ(m_batch->PendingSlotOf(blocks[1]), second_entry & ~kTableStagedBit);

		// The scatter writes the newest version of the block, and only that one.
		const Batch::FlushPlan plan = m_batch->PrepareFlush();
		ASSERT_EQ(plan.scatter_pairs, 4u);
		bool found = false;
		for (u32 i = 0; i < plan.scatter_pairs; i++)
		{
			if (m_meta[plan.scatter_word + i * 2] == blocks[1])
			{
				EXPECT_EQ(m_meta[plan.scatter_word + i * 2 + 1], second_entry & ~kTableStagedBit);
				found = true;
			}
		}
		EXPECT_TRUE(found);
	}

	TEST_F(GSVramMirrorBatchTest, TheSlotMapIsClearedAtEveryFlush)
	{
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_NE(m_batch->PendingSlotOf(blocks[0]), ~0u);
		Flush();
		EXPECT_EQ(m_batch->PendingSlotOf(blocks[0]), ~0u);

		// After the flush the block is in the mirror, so a window reads the mirror, not the old slot.
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		for (u32 i = 0; i < 4; i++)
			EXPECT_EQ(m_batch->TableOf(0)[i] & kTableStagedBit, 0u);
	}

	TEST_F(GSVramMirrorBatchTest, FlushWritesTheJobRecordsTheWorkgroupListAndTheScatterPairs)
	{
		const Window a = Four();
		const Window b = {PSMT4, 0, 2, GSVector4i(0, 0, 64, 64)};
		ASSERT_EQ(Queue(a, NewTexture()), Batch::Result::Queued);
		ASSERT_EQ(Queue(b, NewTexture()), Batch::Result::Queued);

		const std::vector<Batch::Job> jobs = m_batch->Jobs();
		ASSERT_EQ(jobs.size(), 2u);
		const Batch::FlushPlan plan = m_batch->PrepareFlush();
		EXPECT_EQ(plan.job_count, 2u);
		EXPECT_EQ(plan.wg_total, jobs[0].wg_count + jobs[1].wg_count);
		EXPECT_EQ(plan.decoded_bytes, jobs[0].layout.scratch_bytes + static_cast<u64>(jobs[1].layout.scratch_bytes));

		for (u32 j = 0; j < 2; j++)
		{
			const u32* rec = &m_meta[plan.jobs_word + j * 8];
			EXPECT_EQ(rec[0], static_cast<u32>(jobs[j].layout.format));
			EXPECT_EQ(rec[1], jobs[j].table_word);
			EXPECT_EQ(rec[2], jobs[j].layout.blocks_wide);
			EXPECT_EQ(rec[3], jobs[j].layout.blocks_high);
			EXPECT_EQ(rec[4], jobs[j].out_offset);
			EXPECT_EQ(rec[5], jobs[j].layout.pitch);
			EXPECT_EQ(rec[6], jobs[j].first_wg);
			EXPECT_EQ(rec[7], jobs[j].layout.UnitCount());
			EXPECT_EQ(jobs[j].out_offset % 256, 0u) << "each window's scratch starts on a 256-byte boundary";
		}
		EXPECT_GE(jobs[1].out_offset, jobs[0].out_offset + jobs[0].layout.scratch_bytes) << "scratch ranges do not overlap";
		for (u32 j = 0; j < 2; j++)
			for (u32 g = 0; g < jobs[j].wg_count; g++)
				EXPECT_EQ(m_meta[plan.wg_word + jobs[j].first_wg + g], j) << "workgroup " << jobs[j].first_wg + g << " belongs to job " << j;

		// Every staged block is named once, with a slot that holds its bytes.
		std::set<u32> blocks_seen;
		for (u32 i = 0; i < plan.scatter_pairs; i++)
		{
			const u32 block = m_meta[plan.scatter_word + i * 2];
			const u32 slot = m_meta[plan.scatter_word + i * 2 + 1];
			EXPECT_TRUE(blocks_seen.insert(block).second) << "block " << block << " named twice";
			EXPECT_EQ(std::memcmp(StagedBlock(slot), MemBlock(block), kBlockBytes), 0);
		}
		EXPECT_EQ(plan.scatter_pairs, m_batch->StagedBlockCount());
		m_batch->FinishFlush(m_gpu.current);
	}

	TEST_F(GSVramMirrorBatchTest, FinishingAFlushStartsAnEmptyBatch)
	{
		void* tex = NewTexture();
		ASSERT_EQ(Queue(Four(), tex), Batch::Result::Queued);
		Flush();
		EXPECT_FALSE(m_batch->HasPending());
		EXPECT_FALSE(m_batch->IsPending(tex)) << "a flushed decode is no longer pending";
		EXPECT_EQ(m_batch->Jobs().size(), 0u);
		EXPECT_EQ(m_batch->GetStats().flushes, 1u);
		EXPECT_EQ(m_batch->GetStats().scatter_blocks, 4u);
	}

	// ---- Cancel ------------------------------------------------------------------------------------------

	TEST_F(GSVramMirrorBatchTest, CancellingADecodeKeepsTheBlocksItStagedForTheScatter)
	{
		// Their dirty bits were cleared at the miss. Dropping them would leave the mirror stale for good.
		void* tex = NewTexture();
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, tex), Batch::Result::Queued);
		ASSERT_TRUE(m_batch->IsPending(tex));

		EXPECT_EQ(m_batch->Cancel(tex), 1u);
		EXPECT_FALSE(m_batch->IsPending(tex));
		EXPECT_EQ(m_batch->Jobs()[0].texture, nullptr) << "the copy is dropped";
		EXPECT_EQ(m_batch->GetStats().cancelled, 1u);
		EXPECT_TRUE(m_batch->HasPending()) << "the staged blocks still have to reach the mirror";
		EXPECT_EQ(m_batch->StagedBlockCount(), 4u);
		for (const u32 b : blocks)
			EXPECT_FALSE(Dirty(b)) << "the bits stay cleared; the mirror will have the blocks";

		const Batch::FlushPlan plan = m_batch->PrepareFlush();
		EXPECT_EQ(plan.scatter_pairs, 4u);
		std::set<u32> scattered;
		for (u32 i = 0; i < plan.scatter_pairs; i++)
			scattered.insert(m_meta[plan.scatter_word + i * 2]);
		EXPECT_EQ(scattered, std::set<u32>(blocks.begin(), blocks.end()));
		m_batch->FinishFlush(m_gpu.current);

		// And a window decoded afterwards reads those blocks from the mirror.
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		for (u32 i = 0; i < blocks.size(); i++)
			EXPECT_EQ(m_batch->TableOf(0)[i], blocks[i]);
	}

	TEST_F(GSVramMirrorBatchTest, CancellingOneTextureLeavesTheOthersPending)
	{
		void* a = NewTexture();
		void* b = NewTexture();
		const Window w = Four();
		ASSERT_EQ(Queue(w, a), Batch::Result::Queued);
		ASSERT_EQ(Queue(w, b), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Cancel(a), 1u);
		EXPECT_FALSE(m_batch->IsPending(a));
		EXPECT_TRUE(m_batch->IsPending(b));
		EXPECT_EQ(m_batch->Cancel(a), 0u) << "cancelling twice does nothing";
		EXPECT_EQ(m_batch->Cancel(NewTexture()), 0u) << "a texture that was never queued does nothing";
	}

	TEST_F(GSVramMirrorBatchTest, CancellingAfterTheFlushDoesNothing)
	{
		void* tex = NewTexture();
		ASSERT_EQ(Queue(Four(), tex), Batch::Result::Queued);
		Flush();
		EXPECT_EQ(m_batch->Cancel(tex), 0u);
		EXPECT_EQ(m_batch->GetStats().cancelled, 0u);
	}

	TEST_F(GSVramMirrorBatchTest, AllDecodesCancelledStillFlushesBecauseOfTheStagedBlocks)
	{
		void* tex = NewTexture();
		ASSERT_EQ(Queue(Four(), tex), Batch::Result::Queued);
		m_batch->Cancel(tex);
		EXPECT_TRUE(m_batch->HasPending());
		Flush();
		EXPECT_FALSE(m_batch->HasPending());
	}

	// ---- Resync (a mirror over a memory it has not seen) ----------------------------------------------------

	TEST_F(GSVramMirrorBatchTest, AMemoryTheMirrorHasNotSeenIsEntirelyDirtyWhateverItsBitmapSays)
	{
		const Window w = Four();
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		ClearAllDirty(); // a bitmap that claims the mirror has everything

		// A new life of the mirror (a device loss with the renderer kept): generation 2 over the same memory.
		const u64 resyncs = m_batch->GetStats().resyncs;
		ASSERT_EQ(Queue(w, NewTexture(), 2), Batch::Result::Queued);
		EXPECT_EQ(m_batch->GetStats().resyncs, resyncs + 1);
		for (u32 i = 0; i < 4; i++)
			EXPECT_NE(m_batch->TableOf(0)[i] & kTableStagedBit, 0u) << "block " << i << " is restaged, not read from an empty mirror";

		// Another memory: a new GSLocalMemory has a new id.
		Flush();
		auto other = std::make_unique<GSLocalMemory>();
		ASSERT_NE(other->Id(), m_mem->Id());
		ClearAllDirty();
		std::fill(other->BlockDirtyWords(), other->BlockDirtyWords() + kMirrorBlocks / 64, 0ull);
		const Source src = {other->vm8(), other->Id(), other->BlockDirtyWords(), other.get()};
		const GSOffset off = other->GetOffset(w.tbp, w.tbw, w.psm);
		WindowLayout layout;
		ASSERT_TRUE(ComputeLayout(off, w.rect, &layout));
		ASSERT_EQ(m_batch->Queue(off, layout, src, 2, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->GetStats().resyncs, resyncs + 2);
		for (u32 i = 0; i < 4; i++)
			EXPECT_NE(m_batch->TableOf(0)[i] & kTableStagedBit, 0u);
	}

	TEST_F(GSVramMirrorBatchTest, AResyncDropsTheStagedBlocksOfTheMemoryItReplaced)
	{
		const Window w = Four();
		void* old_tex = NewTexture();
		ASSERT_EQ(Queue(w, old_tex), Batch::Result::Queued);
		ASSERT_EQ(m_batch->StagedBlockCount(), 4u);

		auto other = std::make_unique<GSLocalMemory>();
		const Source src = {other->vm8(), other->Id(), other->BlockDirtyWords(), other.get()};
		const GSOffset off = other->GetOffset(w.tbp, w.tbw, w.psm);
		WindowLayout layout;
		ASSERT_TRUE(ComputeLayout(off, w.rect, &layout));
		ASSERT_EQ(m_batch->Queue(off, layout, src, 1, NewTexture()), Batch::Result::Queued);
		// The old window's blocks are not scattered into a mirror that now describes the other memory; only the
		// new window's own staging remains.
		EXPECT_EQ(m_batch->StagedBlockCount(), 4u);
		EXPECT_TRUE(m_batch->IsPending(old_tex)) << "the old window still decodes, from its own table";
	}

	TEST_F(GSVramMirrorBatchTest, ForgetMemoryMakesTheNextWindowMarkEverythingDirty)
	{
		const Window w = Four();
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		m_batch->ForgetMemory();
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->GetStats().resyncs, 2u);
	}

	// ---- The GPU's progress -------------------------------------------------------------------------------------

	TEST_F(GSVramMirrorBatchTest, AStagingRingFullOfUnsubmittedWorkIsASubmitNotAWait)
	{
		Config c;
		c.staging_slots = 64;
		Make(c);

		ASSERT_EQ(Queue(Page(), NewTexture()), Batch::Result::Queued); // 32 slots
		Flush();                                                       // recorded in command buffer 1, not submitted
		// A second page needs 32 more. 64 - 32 is exactly enough, so make it need more: another page elsewhere.
		const Window other = {PSMT8, 64, 2, GSVector4i(0, 0, 128, 64)};
		ASSERT_EQ(Queue(other, NewTexture()), Batch::Result::Queued); // 64 slots in use
		const Window third = {PSMT8, 128, 2, GSVector4i(0, 0, 128, 64)};
		EXPECT_EQ(Queue(third, NewTexture()), Batch::Result::NeedSubmit);
		EXPECT_TRUE(m_gpu.waits.empty()) << "waiting would wait forever for work nobody submitted";
		EXPECT_GE(m_batch->GetStats().need_submit, 1u);

		// The test submits (command buffer 1 goes to the GPU, 2 is being recorded) and the GPU finishes 1.
		Flush();
		m_gpu.Submit();
		m_gpu.FinishAll();
		EXPECT_EQ(Queue(third, NewTexture()), Batch::Result::Queued) << "the retired command buffer gave its slots back";
	}

	TEST_F(GSVramMirrorBatchTest, AStagingRingWaitsForASubmittedButUnfinishedFlush)
	{
		Config c;
		c.staging_slots = 64;
		Make(c);

		ASSERT_EQ(Queue(Page(), NewTexture()), Batch::Result::Queued);
		const Window other = {PSMT8, 64, 2, GSVector4i(0, 0, 128, 64)};
		ASSERT_EQ(Queue(other, NewTexture()), Batch::Result::Queued);
		Flush();
		m_gpu.Submit(); // the flush's command buffer is on the GPU but not finished

		const Window third = {PSMT8, 128, 2, GSVector4i(0, 0, 128, 64)};
		ASSERT_EQ(Queue(third, NewTexture()), Batch::Result::Queued);
		ASSERT_EQ(m_gpu.waits.size(), 1u) << "a submitted flush's slots come back by waiting for it";
		EXPECT_EQ(m_gpu.waits[0], 1u);
	}

	TEST_F(GSVramMirrorBatchTest, AWindowNeedingMoreSlotsThanTheRingHasIsUnsupported)
	{
		Config c;
		c.staging_slots = 16;
		Make(c);
		EXPECT_EQ(Queue(Page(), NewTexture()), Batch::Result::Unsupported); // 32 blocks, 16 slots
		EXPECT_FALSE(m_batch->HasPending());
		EXPECT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued) << "a window that fits still does";
	}

	TEST_F(GSVramMirrorBatchTest, EveryMetaRegionHeldByTheUnsubmittedBufferIsASubmit)
	{
		Config c;
		c.meta_regions = 2;
		Make(c);

		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		Flush(); // region 0, command buffer 1 (being recorded)
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		Flush(); // region 1, command buffer 1
		EXPECT_EQ(m_batch->ClaimedRegions(), 0u);
		EXPECT_EQ(Queue(Four(), NewTexture()), Batch::Result::NeedSubmit);
		EXPECT_TRUE(m_gpu.waits.empty());

		m_gpu.Submit();
		m_gpu.FinishAll();
		EXPECT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
	}

	TEST_F(GSVramMirrorBatchTest, AMetaRegionOfASubmittedFlushIsWaitedFor)
	{
		Config c;
		c.meta_regions = 2;
		Make(c);

		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		Flush();
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		Flush();
		m_gpu.Submit(); // both flushes are on the GPU, unfinished
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_gpu.waits.size(), 1u);
	}

	TEST_F(GSVramMirrorBatchTest, ARegionIsFreeOnceItsFenceHasCompleted)
	{
		Config c;
		c.meta_regions = 1;
		Make(c);
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		Flush();
		m_gpu.Submit();
		m_gpu.FinishAll();
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued);
		EXPECT_TRUE(m_gpu.waits.empty()) << "nothing to wait for";
	}

	TEST_F(GSVramMirrorBatchTest, AFullBatchAsksForAFlushAndAnEmptyOneRefuses)
	{
		Config c;
		c.scratch_bytes = 1u << 16; // 64 KiB of scratch
		Make(c);

		// 128 x 64 texels of PSMT8 is 8 KiB of scratch.
		const Window w = Page();
		u32 queued = 0;
		Batch::Result r;
		while ((r = Queue(w, NewTexture())) == Batch::Result::Queued)
			queued++;
		EXPECT_EQ(r, Batch::Result::NeedFlush);
		EXPECT_EQ(queued, 8u);

		Flush();
		EXPECT_EQ(QueueFlushing(w, NewTexture()), Batch::Result::Queued);

		// A window bigger than the scratch buffer can never run.
		Window big = {PSMT8, 0, 2, GSVector4i(0, 0, 128, 1024)}; // 128 KiB
		Flush();
		EXPECT_EQ(Queue(big, NewTexture()), Batch::Result::Unsupported);
	}

	TEST_F(GSVramMirrorBatchTest, AFullMetaRegionAsksForAFlush)
	{
		Config c;
		c.meta_region_bytes = 1u << 14; // 4096 words
		Make(c);
		const Window w = Page();
		Batch::Result r;
		u32 queued = 0;
		while ((r = Queue(w, NewTexture())) == Batch::Result::Queued)
		{
			queued++;
			ASSERT_LT(queued, 200u);
		}
		EXPECT_EQ(r, Batch::Result::NeedFlush);
		EXPECT_GT(queued, 1u);
	}

	// ---- Verify mode ------------------------------------------------------------------------------------------

	TEST_F(GSVramMirrorBatchTest, TheShadowHoldsWhatTheMirrorWillHold)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		for (const u32 b : blocks)
		{
			EXPECT_TRUE((m_batch->ShadowValid()[b >> 6] >> (b & 63)) & 1);
			EXPECT_EQ(std::memcmp(m_batch->Shadow() + static_cast<size_t>(b) * kBlockBytes, MemBlock(b), kBlockBytes), 0);
		}
		// A write after the staging marked: the mirror will hold the OLD bytes until the block is restaged, and the
		// shadow says the same.
		std::vector<u8> old(MemBlock(blocks[0]), MemBlock(blocks[0]) + kBlockBytes);
		Write(blocks[0], 0x55);
		EXPECT_EQ(std::memcmp(m_batch->Shadow() + static_cast<size_t>(blocks[0]) * kBlockBytes, old.data(), kBlockBytes), 0);
		EXPECT_TRUE(Dirty(blocks[0]));
		EXPECT_EQ(m_batch->CheckAll(Src()).stale, 0u) << "a marked block is not a promise broken";
	}

	TEST_F(GSVramMirrorBatchTest, AStoreNothingMarkedIsFoundWhenTheBlockIsNextLookedAt)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const Window elsewhere = {PSMT8, 200, 2, GSVector4i(0, 0, 32, 32)};
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();

		// A batch that is already under way, so the window below is checked on its own blocks and not by the
		// sweep that opens a batch.
		ASSERT_EQ(Queue(elsewhere, NewTexture()), Batch::Result::Queued);
		const u64 sweeps = m_batch->ChecksSweeps();
		Write(blocks[2], 0x33, /*mark=*/false);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->ChecksSweeps(), sweeps);
		EXPECT_EQ(m_batch->Checks().stale, 1u);
		EXPECT_EQ(m_batch->ChecksWindowBlocks(), 4u) << "the window's four clean blocks were compared";
	}

	TEST_F(GSVramMirrorBatchTest, AMarkedStoreIsNotReported)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		Write(blocks[2], 0x33);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().stale, 0u);
		EXPECT_EQ(m_batch->Checks().unseen, 0u);
	}

	TEST_F(GSVramMirrorBatchTest, TheSweepFindsAnUnmarkedStoreOutsideTheWindow)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const Window elsewhere = {PSMT8, 200, 2, GSVector4i(0, 0, 32, 32)};
		const std::vector<u32> far_blocks = BlocksOf(elsewhere);
		ASSERT_EQ(Queue(elsewhere, NewTexture()), Batch::Result::Queued);
		Flush();

		Write(far_blocks[1], 0x44, /*mark=*/false);
		// The next window is somewhere else, so only the sweep at the start of a batch can see the store.
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().stale, 1u);
		EXPECT_GE(m_batch->ChecksSweeps(), 1u);
	}

	TEST_F(GSVramMirrorBatchTest, ACleanBitForABlockTheMirrorNeverTookIsReported)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(Four(), NewTexture()), Batch::Result::Queued); // syncs, marks all dirty, stages the four
		Flush();

		// A bitmap that lies: a block nobody staged is called clean.
		const u32 liar = 9000;
		m_mem->BlockDirtyWords()[liar >> 6] &= ~(1ull << (liar & 63));
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().unseen, 1u);
		(void)blocks;
	}

	TEST_F(GSVramMirrorBatchTest, AResyncForgetsWhatTheShadowKnew)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		auto other = std::make_unique<GSLocalMemory>();
		const Source src = {other->vm8(), other->Id(), other->BlockDirtyWords(), other.get()};
		const GSOffset off = other->GetOffset(w.tbp, w.tbw, w.psm);
		WindowLayout layout;
		ASSERT_TRUE(ComputeLayout(off, w.rect, &layout));
		// A memory whose bitmap is all clear would, without the resync, be called mirrored.
		std::fill(other->BlockDirtyWords(), other->BlockDirtyWords() + kMirrorBlocks / 64, 0ull);
		ASSERT_EQ(m_batch->Queue(off, layout, src, 1, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().unseen, 0u);
		EXPECT_EQ(m_batch->Checks().stale, 0u);
	}

	TEST_F(GSVramMirrorBatchTest, CancelledWindowsStillFeedTheShadow)
	{
		m_batch->EnableShadow();
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		void* tex = NewTexture();
		ASSERT_EQ(Queue(w, tex), Batch::Result::Queued);
		m_batch->Cancel(tex);
		for (const u32 b : blocks)
			EXPECT_TRUE((m_batch->ShadowValid()[b >> 6] >> (b & 63)) & 1) << "the scatter still takes block " << b;
	}

	TEST_F(GSVramMirrorBatchTest, WithTheShadowOffNothingIsChecked)
	{
		const Window w = Four();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		Write(blocks[0], 0x66, false);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().stale, 0u);
		EXPECT_EQ(m_batch->ChecksSweeps(), 0u);
		EXPECT_FALSE(m_batch->ShadowEnabled());
	}

	// ---- Against the real marks ----------------------------------------------------------------------------------

	TEST_F(GSVramMirrorBatchTest, TheMemorysOwnMarksDriveTheMirror)
	{
		// The wiring S5 adds: the batch reads and clears the bitmap GSLocalMemory sets with its page marks.
		m_batch->EnableShadow();
		const Window w = Page();
		const std::vector<u32> blocks = BlocksOf(w);
		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		Flush();
		for (const u32 b : blocks)
			EXPECT_FALSE(m_mem->BlockDirty(b));

		// A write through the memory's own writer, which marks.
		const GSOffset off = m_mem->GetOffset(w.tbp, w.tbw, w.psm);
		const GSVector4i r(0, 0, 16, 16); // exactly the first block
		m_mem->MarkPagesWritten(off, r);
		std::memset(m_mem->vm8() + static_cast<size_t>(blocks[0]) * kBlockBytes, 0xee, kBlockBytes);
		EXPECT_TRUE(m_mem->BlockDirty(blocks[0]));

		ASSERT_EQ(Queue(w, NewTexture()), Batch::Result::Queued);
		EXPECT_EQ(m_batch->Checks().stale, 0u);
		EXPECT_NE(m_batch->TableOf(0)[0] & kTableStagedBit, 0u) << "the marked block restages";
		for (u32 i = 1; i < blocks.size(); i++)
			EXPECT_EQ(m_batch->TableOf(0)[i] & kTableStagedBit, 0u) << "block " << i << " was not marked, so it stays in the mirror";
	}
} // namespace
