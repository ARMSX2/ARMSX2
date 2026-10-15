// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The bookkeeping half of the GPU local-memory mirror's batching (GSVramMirrorVK.h). Everything here
// is plain CPU state and arithmetic: which blocks a window stages, which table entry names a staging
// slot and which names the mirror, the staging ring's accounting against the GPU's progress, the
// meta-buffer regions, the list of queued decodes and what cancelling one keeps. The Vulkan class
// owns the buffers this works in and records the commands; nothing here includes Vulkan, so the host
// tests can drive it with a fake GPU.
//
// The rule it implements (DESIGN-gpu-vram-mirror.md, section 3): every decoded texture is a snapshot of
// local memory at its own miss. At a miss the window's dirty blocks are copied into the staging ring and
// their dirty bits cleared, and the window gets a block table naming, per block, either the staging slot
// holding the version current at that miss or the mirror (whose copy of a clean block cannot change before
// the batch's decode runs, because the scatter runs after the decode). A block written again later in the
// batch gets a new slot; the windows that already point at the older slot keep it.

#include "GS/Renderers/Common/GSVramMirrorLayout.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

namespace GSVramMirror
{
	struct Config
	{
		/// 256-byte staging slots, a power of two. 65536 is 16 MiB.
		u32 staging_slots = 65536;
		u32 scratch_bytes = 24u << 20;
		/// One region of the meta buffer per batch under construction or in flight.
		u32 meta_regions = 8;
		u32 meta_region_bytes = 1u << 20;
	};

	/// What the mirror needs of a GSLocalMemory.
	struct Source
	{
		const u8* vm;   ///< the 4 MiB of local memory (GSLocalMemory::vm8())
		u64 memory_id;  ///< unique per GSLocalMemory, never reused
		u64* dirty;     ///< 16384 bits (256 words); a set bit means the mirror lacks the block's current bytes
		GSLocalMemory* mem = nullptr; ///< only the verify mode reads it (the CPU decode it compares against)
	};

	struct Stats
	{
		u64 windows = 0;
		u64 blocks_total = 0;
		u64 blocks_from_staging = 0; ///< table entries that name a staging slot
		u64 blocks_staged = 0;       ///< blocks copied into staging (a dirty block, once per restage)
		u64 blocks_restaged = 0;     ///< of those, blocks the same batch had already staged (a later write)
		u64 mixed_windows = 0;       ///< windows whose table names both staging slots and mirror blocks
		u64 flushes = 0;
		u64 scatter_blocks = 0;
		u64 resyncs = 0;
		u64 cancelled = 0;
		u64 need_submit = 0;
		u64 scratch_flushes = 0;
		u64 decoded_bytes = 0; ///< scratch bytes the decode wrote
	};

	/// What the batch asks of the GPU's progress. The Vulkan class answers from the device's fence
	/// counters; a test answers from a model.
	class FenceSource
	{
	public:
		virtual ~FenceSource() = default;
		/// The highest fence counter whose command buffer the GPU has finished.
		virtual u64 Completed() = 0;
		/// The counter of the command buffer being recorded (not yet submitted).
		virtual u64 Current() = 0;
		/// Looks for command buffers that have finished since the last look.
		virtual void Poll() = 0;
		/// Blocks until `counter` has completed.
		virtual void Wait(u64 counter) = 0;
	};

	/// What a verify run found in the marks (see Batch::EnableShadow).
	struct MarkCheck
	{
		u64 blocks = 0;  ///< clean blocks compared
		u64 stale = 0;   ///< clean blocks whose bytes differ from local memory's: a store nothing marked
		u64 unseen = 0;  ///< clean blocks the batch never staged since the memory was synced: a bitmap that lies
	};

	class Batch
	{
	public:
		enum class Result
		{
			Queued,      ///< the window is part of the pending batch
			NeedFlush,   ///< the pending batch is full: flush it, then queue again
			NeedSubmit,  ///< no room until the GPU retires an unsubmitted batch: submit, then queue again
			Unsupported, ///< cannot be decoded here: take the CPU path
		};

		struct Job
		{
			void* texture;       ///< null once cancelled
			WindowLayout layout;
			u32 table_word; ///< absolute word offset of the block table in the meta buffer
			u32 out_offset; ///< scratch byte offset
			u32 first_wg;
			u32 wg_count;
		};

		/// Where PrepareFlush put the batch's records in the meta buffer (absolute word offsets).
		struct FlushPlan
		{
			u32 jobs_word = 0;
			u32 wg_word = 0;
			u32 scatter_word = 0;
			u32 job_count = 0;
			u32 wg_total = 0;
			u32 scatter_pairs = 0;
			u32 region_base_word = 0;
			u32 words_used = 0; ///< from region_base_word, for a non-coherent flush
			u64 decoded_bytes = 0;
			u64 staged_first = 0; ///< staging slots written since the previous flush: [staged_first, staged_end)
			u64 staged_end = 0;
		};

		static constexpr u32 kNoRegion = ~0u;
		static constexpr u32 kMaxJobs = 4096;
		static constexpr u32 kMaxWorkgroups = 60000;

		/// `staging` holds config.staging_slots 256-byte slots and `meta` config.meta_regions regions of
		/// config.meta_region_bytes each; the batch writes into both, the caller owns them.
		Batch(const Config& config, u8* staging, u32* meta, FenceSource* fences)
			: m_config(config)
			, m_staging(staging)
			, m_meta(meta)
			, m_fences(fences)
		{
			m_slot_epoch.assign(kMirrorBlocks, 0);
			m_slot_of_block.assign(kMirrorBlocks, 0);
			m_seen_epoch.assign(kMirrorBlocks, 0);
			m_staged_blocks.reserve(kMirrorBlocks);
			m_region_fence.assign(config.meta_regions, 0);
			m_region_claimed.assign(config.meta_regions, 0);
		}

		const Config& GetConfig() const { return m_config; }
		const Stats& GetStats() const { return m_stats; }
		const std::vector<Job>& Jobs() const { return m_jobs; }

		/// Whether the pending batch holds anything to record: a queued decode (even a cancelled one, whose
		/// slot in the workgroup list stays) or a staged block the mirror still has to take.
		bool HasPending() const { return !m_jobs.empty() || !m_staged_blocks.empty(); }
		u32 StagedBlockCount() const { return static_cast<u32>(m_staged_blocks.size()); }
		/// Whether `texture` has a live (not cancelled) queued decode.
		bool IsPending(const void* texture) const
		{
			for (const Job& job : m_jobs)
				if (job.texture == texture)
					return true;
			return false;
		}
		/// The staging slot that holds the newest version of `block` in the pending batch, or ~0u.
		u32 PendingSlotOf(u32 block) const { return m_slot_epoch[block] == m_epoch ? m_slot_of_block[block] : ~0u; }
		u64 StagingInUse() const { return m_stage_head - m_stage_tail; }
		u64 StagingFree() const { return m_config.staging_slots - StagingInUse(); }
		u32 ClaimedRegions() const
		{
			u32 n = 0;
			for (const u8 c : m_region_claimed)
				n += c;
			return n;
		}
		/// The block table of the job at `index`, as the shader reads it.
		const u32* TableOf(size_t index) const { return m_meta + m_jobs[index].table_word; }
		u64 MemoryId() const { return m_synced_memory_id; }

		/// Makes the next Queue treat every block of its memory as dirty and forget what was staged.
		void ForgetMemory() { m_synced_memory_id = 0; }

		void CountScratchFlush() { m_stats.scratch_flushes++; }

		/// Queues the window. On Queued the pending batch has its job and the staging copies of the window's
		/// dirty blocks (whose bits are now clear). `generation` is the life of the mirror the batch writes
		/// for: when it, or the memory, is not the one last synced, every block of the memory is first marked
		/// dirty in `source.dirty`.
		Result Queue(const GSOffset& off, const WindowLayout& layout, const Source& source, u64 generation, void* texture)
		{
			// A window that cannot fit even an empty batch goes to the CPU.
			if (layout.scratch_bytes > m_config.scratch_bytes)
				return Result::Unsupported;

			SyncTo(source, generation);
			// Verify mode: the first window of a batch checks every clean block of the memory, so a store
			// nothing marked is found even where no window looks; later windows check their own blocks.
			const bool swept = m_shadow_enabled && m_jobs.empty() && m_staged_blocks.empty();
			if (swept)
				AddCheck(CheckAll(source));

			BuildBlockNumbers(off, layout, &m_block_numbers);

			// How many distinct blocks of the window the mirror has not seen: the staging slots it needs.
			if (++m_seen_stamp == 0)
			{
				std::fill(m_seen_epoch.begin(), m_seen_epoch.end(), 0);
				m_seen_stamp = 1;
			}
			u32 need = 0;
			for (const u32 bn : m_block_numbers)
			{
				if (((source.dirty[bn >> 6] >> (bn & 63)) & 1) && m_seen_epoch[bn] != m_seen_stamp)
				{
					m_seen_epoch[bn] = m_seen_stamp;
					need++;
				}
			}

			if (!FitsBatch(layout, need))
			{
				// A full scratch (or job list, or meta region) is a flush: barrier A orders the reuse.
				if (m_jobs.empty() && m_staged_blocks.empty())
					return Result::Unsupported;
				return Result::NeedFlush;
			}

			if (m_batch_region == kNoRegion && !ClaimRegion())
			{
				m_stats.need_submit++;
				return Result::NeedSubmit;
			}

			switch (EnsureStagingSpace(need))
			{
				case 1:
					m_stats.need_submit++;
					return Result::NeedSubmit;
				case 2:
					return Result::Unsupported;
				default:
					break;
			}

			if (m_shadow_enabled && !swept)
				CheckWindow(source);

			// Stage what is new and write the block table. A block the batch already staged and nobody has
			// written since reads that staged copy; one written since gets a fresh slot, and the windows
			// that already point at the older slot keep their snapshot.
			const u32 region_base_words = m_batch_region * (m_config.meta_region_bytes / 4);
			u32* const table = m_meta + region_base_words + m_table_words;
			const u32 slot_mask = m_config.staging_slots - 1;
			u32 staged_here = 0, from_staging = 0, restaged = 0;
			for (u32 i = 0; i < layout.BlockCount(); i++)
			{
				const u32 bn = m_block_numbers[i];
				u64& word = source.dirty[bn >> 6];
				u32 entry;
				if ((word >> (bn & 63)) & 1)
				{
					const u32 slot = static_cast<u32>(m_stage_head++) & slot_mask;
					std::memcpy(m_staging + static_cast<size_t>(slot) * kBlockBytes, source.vm + static_cast<size_t>(bn) * kBlockBytes,
						kBlockBytes);
					word &= ~(1ull << (bn & 63));
					if (m_slot_epoch[bn] != m_epoch)
					{
						m_slot_epoch[bn] = m_epoch;
						m_staged_blocks.push_back(bn);
					}
					else
					{
						restaged++;
					}
					m_slot_of_block[bn] = slot;
					entry = kTableStagedBit | slot;
					staged_here++;
					from_staging++;
					if (m_shadow_enabled)
						ShadowTake(bn, source.vm);
				}
				else if (m_slot_epoch[bn] == m_epoch)
				{
					entry = kTableStagedBit | m_slot_of_block[bn];
					from_staging++;
				}
				else
				{
					entry = bn;
				}
				table[i] = entry;
			}

			Job job;
			job.texture = texture;
			job.layout = layout;
			job.table_word = region_base_words + m_table_words;
			job.out_offset = (m_scratch_used + 255u) & ~255u;
			job.first_wg = m_wg_total;
			job.wg_count = (layout.UnitCount() + 63) / 64;
			m_jobs.push_back(job);

			m_table_words += layout.BlockCount();
			m_wg_total += job.wg_count;
			m_scratch_used = job.out_offset + layout.scratch_bytes;

			m_stats.windows++;
			m_stats.blocks_total += layout.BlockCount();
			m_stats.blocks_staged += staged_here;
			m_stats.blocks_restaged += restaged;
			if (from_staging != 0 && from_staging != layout.BlockCount())
				m_stats.mixed_windows++;
			m_stats.blocks_from_staging += from_staging;
			m_stats.decoded_bytes += layout.scratch_bytes;
			return Result::Queued;
		}

		/// Takes `texture` out of the pending batch (it is about to be destroyed). The blocks its window
		/// staged stay in the batch and still reach the mirror: their dirty bits are already cleared, so
		/// dropping them would leave the mirror stale for good. Returns how many queued decodes it removed.
		u32 Cancel(const void* texture)
		{
			u32 n = 0;
			for (Job& job : m_jobs)
			{
				if (job.texture == texture)
				{
					job.texture = nullptr;
					m_stats.cancelled++;
					n++;
				}
			}
			return n;
		}

		/// Writes the job records, the workgroup -> job list and the (block, slot) scatter pairs after the
		/// block tables, in the batch's meta region. The caller records the commands from the plan, then
		/// calls FinishFlush with the fence counter of the command buffer they went into.
		FlushPlan PrepareFlush()
		{
			FlushPlan plan;
			const u32 region_words = m_config.meta_region_bytes / 4;
			plan.region_base_word = m_batch_region * region_words;
			u32* const meta = m_meta + plan.region_base_word;

			u32 w = AlignWords8(m_table_words);
			plan.jobs_word = plan.region_base_word + w;
			for (size_t j = 0; j < m_jobs.size(); j++)
			{
				const Job& job = m_jobs[j];
				u32* rec = meta + w + j * 8;
				rec[0] = static_cast<u32>(job.layout.format);
				rec[1] = job.table_word;
				rec[2] = job.layout.blocks_wide;
				rec[3] = job.layout.blocks_high;
				rec[4] = job.out_offset;
				rec[5] = job.layout.pitch;
				rec[6] = job.first_wg;
				rec[7] = job.layout.UnitCount();
				plan.decoded_bytes += job.layout.scratch_bytes;
			}
			w += static_cast<u32>(m_jobs.size()) * 8;
			w = AlignWords8(w);
			plan.wg_word = plan.region_base_word + w;
			for (size_t j = 0; j < m_jobs.size(); j++)
				for (u32 g = 0; g < m_jobs[j].wg_count; g++)
					meta[w + m_jobs[j].first_wg + g] = static_cast<u32>(j);
			w += m_wg_total;
			w = AlignWords8(w);
			plan.scatter_word = plan.region_base_word + w;
			for (size_t i = 0; i < m_staged_blocks.size(); i++)
			{
				meta[w + i * 2] = m_staged_blocks[i];
				meta[w + i * 2 + 1] = m_slot_of_block[m_staged_blocks[i]];
			}
			w += static_cast<u32>(m_staged_blocks.size()) * 2;

			plan.job_count = static_cast<u32>(m_jobs.size());
			plan.wg_total = m_wg_total;
			plan.scatter_pairs = static_cast<u32>(m_staged_blocks.size());
			plan.words_used = w;
			plan.staged_first = m_stage_flushed;
			plan.staged_end = m_stage_head;
			m_stage_flushed = m_stage_head;
			return plan;
		}

		/// The batch's commands are recorded into the command buffer with this fence counter: its meta
		/// region and staging slots come free when that completes. Starts the next batch.
		void FinishFlush(u64 fence)
		{
			m_region_fence[m_batch_region] = fence;
			m_region_claimed[m_batch_region] = 0;
			m_batch_region = kNoRegion;
			m_stage_marks.emplace_back(fence, m_stage_head);

			m_stats.flushes++;
			m_stats.scatter_blocks += m_staged_blocks.size();
			ResetBatch();
		}

		// ---- Verify mode ----------------------------------------------------------------------------------
		//
		// A CPU copy of what the mirror holds once everything queued has run: every staged block is copied
		// into it as it is staged. With it the marks can be checked on the CPU, exactly where the mirror
		// is read: a block whose dirty bit is clear is a promise that the mirror holds local memory's
		// current bytes, so its shadow copy must equal local memory's. A store nothing marked breaks the
		// promise and is found the next time the block is looked at. (The GPU half, that the mirror
		// really holds the shadow, is the Vulkan class's readback.)

		void EnableShadow()
		{
			m_shadow_enabled = true;
			m_shadow.assign(kMirrorBytes, 0);
			m_shadow_valid.assign(kMirrorBlocks / 64, 0);
		}
		bool ShadowEnabled() const { return m_shadow_enabled; }
		const u8* Shadow() const { return m_shadow.data(); }
		const std::vector<u64>& ShadowValid() const { return m_shadow_valid; }
		const MarkCheck& Checks() const { return m_checks; }
		u64 ChecksWindowBlocks() const { return m_check_window_blocks; }
		u64 ChecksSweeps() const { return m_check_sweeps; }

		/// Compares every clean block with the shadow.
		MarkCheck CheckAll(const Source& source) const
		{
			MarkCheck result;
			for (u32 bn = 0; bn < kMirrorBlocks; bn++)
				CheckBlock(source, bn, &result);
			return result;
		}

	private:
		static constexpr u32 AlignWords8(u32 words) { return (words + 7u) & ~7u; }

		void SyncTo(const Source& source, u64 generation)
		{
			if (m_synced_memory_id == source.memory_id && m_synced_generation == generation)
				return;

			// The mirror's bytes belong to some other memory, or to none: every block of this one is unseen,
			// whatever the bitmap says. The staged blocks of the pending batch come from the memory that was
			// replaced, so they are not scattered; their windows still decode from their own table entries.
			std::fill(source.dirty, source.dirty + kMirrorBlocks / 64, ~0ull);
			m_staged_blocks.clear();
			if (++m_epoch == 0)
			{
				std::fill(m_slot_epoch.begin(), m_slot_epoch.end(), 0);
				m_epoch = 1;
			}
			m_synced_memory_id = source.memory_id;
			m_synced_generation = generation;
			m_stats.resyncs++;
			if (m_shadow_enabled)
				std::fill(m_shadow_valid.begin(), m_shadow_valid.end(), 0);
		}

		bool ClaimRegion()
		{
			for (int attempt = 0; attempt < 2; attempt++)
			{
				const u64 completed = m_fences->Completed();
				for (u32 i = 0; i < m_config.meta_regions; i++)
				{
					if (!m_region_claimed[i] && m_region_fence[i] <= completed)
					{
						m_region_claimed[i] = 1;
						m_batch_region = i;
						m_table_words = 0;
						return true;
					}
				}
				if (attempt == 0)
					m_fences->Poll();
			}

			// Every region is in use. One whose flush was already submitted will come free, so wait for it.
			// Regions held by the command buffer still being built cannot, and that is a submit.
			u32 pick = kNoRegion;
			for (u32 i = 0; i < m_config.meta_regions; i++)
			{
				if (!m_region_claimed[i] && m_region_fence[i] < m_fences->Current() &&
					(pick == kNoRegion || m_region_fence[i] < m_region_fence[pick]))
					pick = i;
			}
			if (pick == kNoRegion)
				return false;
			m_fences->Wait(m_region_fence[pick]);
			m_region_claimed[pick] = 1;
			m_batch_region = pick;
			m_table_words = 0;
			return true;
		}

		void ReclaimStaging()
		{
			const u64 completed = m_fences->Completed();
			while (!m_stage_marks.empty() && m_stage_marks.front().first <= completed)
			{
				m_stage_tail = m_stage_marks.front().second;
				m_stage_marks.pop_front();
			}
		}

		/// 0: room; 1: the unsubmitted batch is in the way; 2: never fits.
		int EnsureStagingSpace(u32 slots_needed)
		{
			if (slots_needed == 0)
				return 0;
			if (slots_needed > m_config.staging_slots)
				return 2;

			ReclaimStaging();
			while (m_config.staging_slots - (m_stage_head - m_stage_tail) < slots_needed)
			{
				// Slots come back when the GPU finishes the command buffer whose flush read them. A flush in the
				// buffer still being built has not been submitted, and waiting for it would wait forever.
				if (m_stage_marks.empty() || m_stage_marks.front().first >= m_fences->Current())
					return 1;
				m_fences->Wait(m_stage_marks.front().first);
				ReclaimStaging();
			}
			return 0;
		}

		bool FitsBatch(const WindowLayout& layout, u32 new_staged_upper) const
		{
			const u32 jobs = static_cast<u32>(m_jobs.size()) + 1;
			if (jobs > kMaxJobs)
				return false;
			const u32 wgs = m_wg_total + (layout.UnitCount() + 63) / 64;
			if (wgs > kMaxWorkgroups)
				return false;
			const u64 scratch = static_cast<u64>((m_scratch_used + 255u) & ~255u) + layout.scratch_bytes;
			if (scratch > m_config.scratch_bytes)
				return false;

			const u32 pairs = std::min<u32>(kMirrorBlocks, static_cast<u32>(m_staged_blocks.size()) + new_staged_upper);
			const u64 words = AlignWords8(m_table_words + layout.BlockCount()) + static_cast<u64>(jobs) * 8 + AlignWords8(wgs) +
			                  static_cast<u64>(pairs) * 2;
			return words <= m_config.meta_region_bytes / 4;
		}

		void ResetBatch()
		{
			m_jobs.clear();
			m_staged_blocks.clear();
			m_table_words = 0;
			m_wg_total = 0;
			m_scratch_used = 0;
			if (++m_epoch == 0)
			{
				std::fill(m_slot_epoch.begin(), m_slot_epoch.end(), 0);
				m_epoch = 1;
			}
		}

		void ShadowTake(u32 bn, const u8* vm)
		{
			std::memcpy(m_shadow.data() + static_cast<size_t>(bn) * kBlockBytes, vm + static_cast<size_t>(bn) * kBlockBytes, kBlockBytes);
			m_shadow_valid[bn >> 6] |= 1ull << (bn & 63);
		}

		void CheckBlock(const Source& source, u32 bn, MarkCheck* result) const
		{
			if ((source.dirty[bn >> 6] >> (bn & 63)) & 1)
				return;
			result->blocks++;
			if (!((m_shadow_valid[bn >> 6] >> (bn & 63)) & 1))
			{
				result->unseen++;
				return;
			}
			if (std::memcmp(m_shadow.data() + static_cast<size_t>(bn) * kBlockBytes, source.vm + static_cast<size_t>(bn) * kBlockBytes,
					kBlockBytes) != 0)
				result->stale++;
		}

		void AddCheck(const MarkCheck& c)
		{
			m_checks.blocks += c.blocks;
			m_checks.stale += c.stale;
			m_checks.unseen += c.unseen;
			m_check_sweeps++;
		}

		/// The clean blocks of the window being queued, before its dirty ones are staged.
		void CheckWindow(const Source& source)
		{
			MarkCheck c;
			for (const u32 bn : m_block_numbers)
				CheckBlock(source, bn, &c);
			m_checks.blocks += c.blocks;
			m_checks.stale += c.stale;
			m_checks.unseen += c.unseen;
			m_check_window_blocks += c.blocks;
		}

		Config m_config;
		u8* m_staging;
		u32* m_meta;
		FenceSource* m_fences;
		Stats m_stats;

		// What the mirror describes: the memory it last synced against, and which life of the mirror that
		// was. A memory the mirror has not seen (a new GSLocalMemory, or a new mirror over a live one after
		// a device loss) is entirely dirty, whatever its bitmap says.
		u64 m_synced_memory_id = 0;
		u64 m_synced_generation = 0;

		// The batch under construction.
		std::vector<Job> m_jobs;
		u32 m_batch_region = kNoRegion;
		u32 m_table_words = 0;
		u32 m_wg_total = 0;
		u32 m_scratch_used = 0;
		u32 m_epoch = 1;
		std::vector<u32> m_slot_epoch;    ///< per block: the epoch it was last staged in
		std::vector<u32> m_slot_of_block; ///< per block: that staging slot
		std::vector<u32> m_staged_blocks; ///< blocks staged in this epoch, in first-staged order
		std::vector<u32> m_seen_epoch;    ///< per block, scratch for counting distinct dirty blocks
		u32 m_seen_stamp = 0;
		std::vector<u32> m_block_numbers;

		// Staging ring: slots are counted forever and wrap by masking. Slots stay in use until the GPU
		// finishes the command buffer whose flush read them.
		u64 m_stage_head = 0;
		u64 m_stage_tail = 0;
		u64 m_stage_flushed = 0;
		std::deque<std::pair<u64, u64>> m_stage_marks; ///< (fence counter, head after that flush)

		// Meta regions: one per batch, free again once the fence of the flush that used it has completed.
		std::vector<u64> m_region_fence;
		std::vector<u8> m_region_claimed;

		// Verify mode.
		bool m_shadow_enabled = false;
		std::vector<u8> m_shadow;
		std::vector<u64> m_shadow_valid;
		MarkCheck m_checks;
		u64 m_check_window_blocks = 0;
		u64 m_check_sweeps = 0;
	};
} // namespace GSVramMirror
