// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSVramMirrorBatch.h"
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
///     memory at its own miss, even when a later window in the same batch overwrote the block. The
///     bookkeeping is GSVramMirror::Batch (GSVramMirrorBatch.h), which has no Vulkan in it.
///   - Flush records the batch: one decode dispatch for every queued window, one scatter dispatch that
///     writes the newest staged version of each block into the mirror, and a buffer-to-image copy per
///     window.
///
/// A texture with a decode in the batch is "pending" (GSTextureVK::IsDecodePending): it is not
/// finished until the flush has run. The flush runs
///   - first thing in GSDeviceVK::SubmitCommandBuffer, so every command buffer that submits has it ahead of
///     its draws;
///   - before anything else records into the init command buffer for a pending texture
///     (GSTextureVK::GetCommandBufferForUpdate, which GenerateMipmap uses too);
///   - when a pending texture goes back to the pool (GSDevice::Recycle);
///   - inside DecodeWindow, when the batch is full.
/// Destroying a pending texture cancels its decode instead (CancelTexture): at teardown the last submit
/// has happened, and a flush would record into a command buffer nobody submits.
///
/// Nothing here asserts or remembers a thread identity: the flush can run on the back thread or, via
/// the readback path, on the MTGS thread, with the other drained.
class GSVramMirrorVK
{
public:
	using Config = GSVramMirror::Config;
	using Source = GSVramMirror::Source;
	using Stats = GSVramMirror::Stats;

	enum class Result
	{
		Queued,      ///< the window is part of the pending batch
		NeedSubmit,  ///< no room until the GPU retires an unsubmitted batch: submit, then retry
		Unsupported, ///< not decodable here (format, size, texture): take the CPU path
	};

	struct Request
	{
		const GSOffset* off; ///< mem.GetOffset(TBP0, TBW, PSM)
		GSVector4i rect;     ///< the texels asked for, in the surface's texel space
		GSTextureVK* texture; ///< an R8 (UNorm8) single-level texture of exactly rect's size
		Source source;
	};

	/// Milliseconds of GPU time in one timed flush, from timestamps.
	struct Timings
	{
		double decode_ms = 0;  ///< the decode dispatch
		double barrier_ms = 0; ///< the barriers between decode and scatter
		double scatter_ms = 0; ///< the scatter dispatch
		double copy_ms = 0;    ///< the buffer-to-image copies and the closing barrier
		u64 decoded_bytes = 0;
		u64 scattered_bytes = 0;
	};

	/// The sum of every timed flush that has finished.
	struct TimingTotals
	{
		u64 flushes = 0;
		double decode_ms = 0;
		double barrier_ms = 0;
		double scatter_ms = 0;
		double copy_ms = 0;
		u64 decoded_bytes = 0;
		u64 scattered_bytes = 0;
	};

	/// What the verify mode found (SetVerify). Process-wide, so it can be read after the device is gone.
	struct VerifyReport
	{
		u64 windows = 0;            ///< decoded textures read back and compared with the CPU decode
		u64 texels = 0;
		u64 bad_windows = 0;
		u64 bad_texels = 0;
		u64 flushes = 0;            ///< flushes whose mirror was read back
		u64 mirror_blocks = 0;      ///< shadow-valid blocks compared with the mirror's bytes
		u64 mirror_bad_blocks = 0;
		u64 mark_blocks = 0;        ///< clean blocks compared with local memory (window blocks and sweeps)
		u64 mark_stale = 0;         ///< ... that differed: a store nothing marked
		u64 mark_unseen = 0;        ///< ... that the mirror never took
		u64 mark_sweeps = 0;
		u64 mark_window_blocks = 0;
		bool Clean() const { return bad_windows == 0 && mirror_bad_blocks == 0 && mark_stale == 0 && mark_unseen == 0; }
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

	/// The mirror's view of a GSLocalMemory: its bytes, its unique id and its block-dirty bitmap.
	static Source SourceOf(GSLocalMemory& mem);

	/// Queues the window's decode. On Queued the texture is in the pending batch: its tracked layout is
	/// already ShaderReadOnly (where it will be once the flush runs) and the caller must not touch the
	/// image until Flush has been recorded. A texture that a draw or copy of the command buffer being built
	/// already used is refused (Unsupported), because its update would have to go into the draw buffer.
	Result DecodeWindow(const Request& request);

	/// Records the pending batch into the current init command buffer. Does nothing if there is none.
	void Flush();
	bool HasPending() const { return m_batch && m_batch->HasPending(); }

	/// Takes `texture` out of the pending batch (it is about to be destroyed). The blocks its window
	/// staged stay in the batch and still reach the mirror: their dirty bits are already cleared, so
	/// dropping them would leave the mirror stale for good.
	void CancelTexture(GSTextureVK* texture);

	/// Forgets which memory the mirror describes, so the next DecodeWindow marks every block dirty.
	/// For a test that needs the effect without a second memory.
	void ForgetMemory();

	const Stats& GetStats() const { return m_batch->GetStats(); }
	const Config& GetConfig() const { return m_config; }
	u64 Generation() const { return m_generation; }
	const GSVramMirror::Batch& GetBatch() const { return *m_batch; }

	/// GPU timestamps around the phases of each flush (see Flush for which drivers they are valid on). Off by
	/// default; costs a few query writes.
	bool EnableTiming(bool enable);
	/// The last timed flush, once its command buffer has completed. False if there is none yet.
	bool ReadTimings(Timings* out);
	/// Every timed flush that has completed so far.
	TimingTotals GetTimingTotals();

	/// Verify mode, for gsrunner's -vram-mirror-verify. Set it before the device is created.
	///
	/// A CPU copy of what the mirror should hold is kept (GSVramMirror::Batch::EnableShadow). Every
	/// window queued is checked against it where the mirror will be read: a clean block (the dirty bitmap
	/// says the mirror took it) must equal local memory's bytes, or some store was never marked. At every
	/// flush the mirror and each decoded texture are read back to the host, and compared, once the
	/// command buffer has completed, with the shadow and with the CPU's own decode of the window as it
	/// was at the miss. VerifyReport has the counts.
	static void SetVerify(bool enable);
	static bool VerifyEnabled();
	static VerifyReport GetVerifyReport();
	/// Waits for every readback in flight and compares it. (Destroying the mirror does this too.)
	void VerifyDrain();

	/// Test support: reads the whole mirror back (flushes, submits and waits).
	bool DebugReadMirror(std::vector<u8>* out);

private:
	explicit GSVramMirrorVK(const Config& config);

	/// The batch's view of the device's fence counters.
	class DeviceFences final : public GSVramMirror::FenceSource
	{
	public:
		u64 Completed() override;
		u64 Current() override;
		void Poll() override;
		void Wait(u64 counter) override;
	};

	bool Initialize();
	bool CreateBuffers();
	bool CreatePipelines();
	bool CreateDescriptors();
	void Destroy();

	bool CreateHostBuffer(u32 size, const char* name, VkBuffer* buffer, VmaAllocation* allocation, u8** mapped,
		bool* coherent);

	void PollTimings();

	struct VerifyEntry
	{
		u32 offset;
		u32 width;
		u32 height;
		u32 format;
		std::vector<u8> expected;
	};
	struct VerifyRecord
	{
		u64 fence = 0;
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		u8* mapped = nullptr;
		u32 size = 0;
		std::vector<VerifyEntry> textures;
		std::vector<u8> shadow;        ///< what the mirror should hold after this flush
		std::vector<u64> shadow_valid; ///< ... where that is known
	};
	void RecordVerifyReadback(VkCommandBuffer cb, std::vector<VerifyEntry>&& textures, const std::vector<GSTextureVK*>& live,
		u64 fence);
	void VerifyPoll(bool wait);
	void VerifyCompare(VerifyRecord& record);
	void PublishChecks();

	static constexpr u32 kQueryCount = 5;
	static constexpr u32 kTimingSets = 8;

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
	/// sets and the spin set, and a set taken from it would starve the vertex ring's growth path. The
	/// staging ring does not grow (a full ring is a submit, not a bigger ring), so there is one set for
	/// the life of the mirror; the pool has room for more so that a growing ring could take a new one
	/// without touching the one in-flight command buffers use.
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	VkDescriptorSet m_set = VK_NULL_HANDLE;

	// Timing: a ring of query sets, so that flushes in flight do not overwrite each other's stamps.
	struct TimingSet
	{
		u64 fence = 0;
		bool pending = false;
		u64 decoded = 0;
		u64 scattered = 0;
	};
	VkQueryPool m_query_pool = VK_NULL_HANDLE;
	bool m_timing = false;
	TimingSet m_timing_sets[kTimingSets];
	u32 m_timing_next = 0;
	Timings m_last_timings;
	u64 m_last_timings_fence = 0;
	bool m_have_last_timings = false;
	TimingTotals m_timing_totals;

	DeviceFences m_fences;
	std::unique_ptr<GSVramMirror::Batch> m_batch;

	// Verify mode: the CPU decode of each queued window (parallel to the batch's jobs), and the readbacks
	// whose command buffers have not been compared yet.
	bool m_verify = false;
	std::vector<std::vector<u8>> m_verify_expected;
	std::deque<VerifyRecord> m_verify_records;
	GSVramMirror::MarkCheck m_published_checks;
	u64 m_published_check_window_blocks = 0;
	u64 m_published_check_sweeps = 0;

	u64 m_last_flush_fence = 0;
};
