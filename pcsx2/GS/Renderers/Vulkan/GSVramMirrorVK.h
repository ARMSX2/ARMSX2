// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSVramMirrorLayout.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <deque>
#include <memory>
#include <vector>

class GSTextureVK;

/// A GPU copy of GS local memory, and the compute decode that turns paletted texture windows into
/// index textures from it.
///
/// The copy exists only as a source for building ordinary textures, the way the upload ring is today:
/// draws never read it, and the CPU copy of local memory stays authoritative. Everything the mirror
/// records goes into the INIT command buffer, which runs ahead of the frame's draws, so no render pass
/// is broken.
///
/// How it works, per window (see DESIGN-gpu-vram-mirror.md in the umbrella):
///   - DecodeWindow runs at a texture-cache miss. It walks the window's blocks (the CPU's own
///     GSOffset walk), copies the ones the mirror has not seen into a staging ring and clears their
///     dirty bits, and writes the window's block table: per block either a mirror block or the staging
///     slot holding the version current at THIS miss. So every decoded texture is a snapshot of local
///     memory at its own miss, even when a later window in the same batch overwrote the block.
///   - Flush records the batch: one decode dispatch for every window, one scatter dispatch that writes
///     the newest staged version of each block into the mirror, and a buffer-to-image copy per window.
///
/// Nothing here asserts or remembers a thread identity: the flush can run on the back thread or, via
/// the readback path, on the MTGS thread, with the other drained.
///
/// Not in this class yet (the texture-cache step): the hooks that flush before a pending texture is
/// touched, the flush at the top of SubmitCommandBuffer, growth of the staging ring, and the
/// setting that creates it.
class GSVramMirrorVK
{
public:
	struct Config
	{
		/// 256-byte staging slots, a power of two. 65536 is 16 MiB.
		u32 staging_slots = 65536;
		u32 scratch_bytes = 24u << 20;
		/// One region of the meta buffer per batch under construction or in flight.
		u32 meta_regions = 8;
		u32 meta_region_bytes = 1u << 20;
	};

	enum class Result
	{
		Queued,      ///< the window is part of the pending batch
		NeedSubmit,  ///< no room until the GPU retires an unsubmitted batch: submit, then retry
		Unsupported, ///< not decodable here (format, size, texture): take the CPU path
	};

	/// What the mirror needs of a GSLocalMemory. S2 puts these in GSLocalMemory itself.
	struct Source
	{
		const u8* vm;    ///< the 4 MiB of local memory (GSLocalMemory::m_vm8)
		u64 memory_id;   ///< unique per GSLocalMemory, never reused
		u64* dirty;      ///< 16384 bits (256 words); a set bit means the mirror lacks the block's current bytes
	};

	struct Request
	{
		const GSOffset* off; ///< mem.GetOffset(TBP0, TBW, PSM)
		GSVector4i rect;     ///< the texels asked for, in the surface's texel space
		GSTextureVK* texture; ///< an R8 (UNorm8) single-level texture of exactly rect's size
		Source source;
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

	/// Milliseconds of GPU time in the last timed flush, from timestamps.
	struct Timings
	{
		double decode_ms = 0;  ///< the decode dispatch
		double barrier_ms = 0; ///< the barriers between decode and scatter
		double scatter_ms = 0; ///< the scatter dispatch
		double copy_ms = 0;    ///< the buffer-to-image copies and the closing barrier
		u64 decoded_bytes = 0;
		u64 scattered_bytes = 0;
	};

	/// Null when this device cannot run the decode (limits, shader compile, allocation): the caller
	/// keeps the CPU path. Every allocation is checked and the failure is logged once; none of them
	/// is a skipped dispatch.
	static std::unique_ptr<GSVramMirrorVK> Create();
	static std::unique_ptr<GSVramMirrorVK> Create(const Config& config);
	~GSVramMirrorVK();

	GSVramMirrorVK(const GSVramMirrorVK&) = delete;
	GSVramMirrorVK& operator=(const GSVramMirrorVK&) = delete;

	static bool IsSupportedPSM(u32 psm);

	/// Queues the window's decode. On Queued the texture is in the pending batch: its tracked layout is
	/// already ShaderReadOnly (where it will be once the flush runs) and the caller must not touch the
	/// image until Flush has been recorded.
	Result DecodeWindow(const Request& request);

	/// Records the pending batch into the current init command buffer. Does nothing if there is none.
	void Flush();
	bool HasPending() const { return !m_jobs.empty() || !m_staged_blocks.empty(); }

	/// Takes `texture` out of the pending batch (it is about to be destroyed). The blocks its window
	/// staged stay in the batch and still reach the mirror: their dirty bits are already cleared, so
	/// dropping them would leave the mirror stale for good.
	void CancelTexture(const GSTextureVK* texture);

	/// Forgets which memory the mirror describes, so the next DecodeWindow marks every block dirty.
	/// For a test that needs the effect without a second memory.
	void ForgetMemory();

	const Stats& GetStats() const { return m_stats; }
	const Config& GetConfig() const { return m_config; }
	u64 Generation() const { return m_generation; }

	/// GPU timestamps around the phases of each flush (see Flush for which drivers they are valid on). Off by
	/// default; costs a few query writes.
	bool EnableTiming(bool enable);
	/// The last timed flush, once its command buffer has completed. False if there is none yet.
	bool ReadTimings(Timings* out);

	/// Test support: reads the whole mirror back (flushes, submits and waits).
	bool DebugReadMirror(std::vector<u8>* out);

private:
	explicit GSVramMirrorVK(const Config& config);

	bool Initialize();
	bool CreateBuffers();
	bool CreatePipelines();
	bool CreateDescriptors();
	void Destroy();

	bool CreateHostBuffer(u32 size, const char* name, VkBuffer* buffer, VmaAllocation* allocation, u8** mapped,
		bool* coherent);

	void SyncTo(const Source& source);
	bool ClaimRegion();
	void ReclaimStaging();
	/// 0: room; 1: the unsubmitted batch is in the way; 2: never fits.
	int EnsureStagingSpace(u32 slots_needed);
	bool FitsBatch(const GSVramMirror::WindowLayout& layout, u32 new_staged_upper) const;
	void ResetBatch();

	struct Job
	{
		GSTextureVK* texture;
		GSVramMirror::WindowLayout layout;
		u32 table_word; ///< absolute word offset of the block table in the meta buffer
		u32 out_offset; ///< scratch byte offset
		u32 first_wg;
		u32 wg_count;
	};

	static constexpr u32 kNoRegion = ~0u;
	static constexpr u32 kMaxJobs = 4096;
	static constexpr u32 kMaxWorkgroups = 60000;
	static constexpr u32 kQueryCount = 5;

	Config m_config;
	u64 m_generation = 0;

	// Vulkan objects. The mirror and scratch are device local; staging and meta are host coherent.
	VkBuffer m_mirror = VK_NULL_HANDLE;
	VmaAllocation m_mirror_alloc = VK_NULL_HANDLE;
	VkBuffer m_scratch = VK_NULL_HANDLE;
	VmaAllocation m_scratch_alloc = VK_NULL_HANDLE;
	VkBuffer m_staging = VK_NULL_HANDLE;
	VmaAllocation m_staging_alloc = VK_NULL_HANDLE;
	u8* m_staging_ptr = nullptr;
	bool m_staging_coherent = true;
	VkBuffer m_meta = VK_NULL_HANDLE;
	VmaAllocation m_meta_alloc = VK_NULL_HANDLE;
	u8* m_meta_ptr = nullptr;
	bool m_meta_coherent = true;

	VkDescriptorSetLayout m_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_decode_pipeline = VK_NULL_HANDLE;
	VkPipeline m_scatter_pipeline = VK_NULL_HANDLE;
	/// The mirror's own pool: the device's global pool budgets its storage buffers exactly for the TFX
	/// sets and the spin set, and a set taken from it would starve the vertex ring's growth path.
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkDescriptorSet m_set = VK_NULL_HANDLE;

	VkQueryPool m_query_pool = VK_NULL_HANDLE;
	bool m_timing = false;
	bool m_timing_pending = false;
	u64 m_timing_fence = 0;
	u64 m_timing_decoded = 0;
	u64 m_timing_scattered = 0;

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
	u64 m_stage_flushed = 0; ///< head at the last flush, for non-coherent flushes
	std::deque<std::pair<u64, u64>> m_stage_marks; ///< (fence counter, head after that flush)

	// Meta regions: one per batch, free again once the fence of the flush that used it has completed.
	std::vector<u64> m_region_fence;
	std::vector<u8> m_region_claimed;

	u64 m_last_flush_fence = 0;
	Stats m_stats;
};
