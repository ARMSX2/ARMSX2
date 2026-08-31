// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "GS/Renderers/Vulkan/VKStreamBuffer.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTypes.h" // kGSTileByteRoadFormats: the byte road's shader variants

#include "common/HashCombine.h"
#include "common/ReadbackSpinManager.h"

#include <array>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class VKSwapChain;

class GSDeviceVK final : public GSDevice
{
public:
	u64 GetOobWaitNs() const override { return m_oob_wait_ns; }
	u64 GetOobWaitCalls() const override { return m_oob_wait_calls; }
	u64 GetSyncWaitNs() const override { return m_sync_wait_ns; }
	u64 GetSyncWaitCalls() const override { return m_sync_wait_calls; }
	u64 GetRingWaitNs() const override { return m_ring_wait_ns; }
	u64 GetRingWaitCalls() const override { return m_ring_wait_calls; }
	u64 GetSourceSetWaitNs() const override { return m_source_set_wait_ns; }
	u64 GetSourceSetWaitCalls() const override { return m_source_set_wait_calls; }
	u64 GetTileGpuKicksOffered() const override { return m_tilegpu_kicks_offered; }
	u64 GetTileGpuKicksTaken() const override { return m_tilegpu_kicks_taken; }
	u64 GetTileGpuKickPredictorFrames() const override { return m_tilegpu_kick_picker.frames; }
	u64 GetTileGpuKickPredictorFramesOn() const override { return m_tilegpu_kick_picker.frames_on; }
	u64 GetTileGpuKickPredictorSwitches() const override { return m_tilegpu_kick_picker.switches; }
	u64 GetTileGpuKickPredictorBubbleNs() const override { return m_tilegpu_kick_picker.bubble_ns; }
	u64 GetTileGpuKickPredictorTaxNs() const override { return m_tilegpu_kick_picker.TaxNs(); }
	u64 GetTileGpuKickPredictorSubmits() const override { return m_tilegpu_kick_picker.submits_taken; }
	u64 GetTileGpuSeedRenderPasses() const override { return m_tilegpu_seed_render_passes; }
	u64 GetTileGpuMergeSeedRenderPasses() const override { return m_tilegpu_merge_seed_render_passes; }
	enum : u32
	{
		// How deep the command-buffer ring is, and therefore how many submissions can be in flight
		// at once (this minus one -- the buffer being recorded is not one of them).
		//
		// Raised 3 -> 8 with the TileGpu pass-tail kick's cadence (EmuCore/GS/TileGpuKickPassCadence)
		// because the two are one lever. The kick never blocks: it submits only when the NEXT
		// buffer in this ring has verifiably retired, and otherwise declines and retries at the
		// following pass. At a ring of three that guard refuses most offers on a short frame, and
		// the offers it takes run the recording thread into the ring's own recycle wait -- M2,
		// cadence 32, ring 3: Shadow of the Colossus +2.80 ms with its GPU idle RISING 0.14 -> 2.95
		// ms a frame; the same arm at ring 8 is -0.20 ms and no dump regresses through the ring.
		//
		// Safe by construction, which is why it is a constant and not a policy: a deeper ring only
		// ever DELAYS a resource's reuse, never permits one earlier. Every reuse in this backend is
		// gated on a fence counter, not on a ring index. What it costs is five more command pools,
		// command buffers and fences, five more slots in the timestamp and pipeline-statistics
		// query pools, and -- the only term worth naming -- five more per-frame descriptor-pool
		// chains, built lazily at 256 sets a link. Measured on the corpus's heaviest dumps, peak
		// RSS moves +12.4 MB on Stuntman (548 -> 560, +2.3%), +6.9 on Spider-Man 3 (+1.3%) and
		// +0.2 on GT4 Online Public Beta, against target memory already in the hundreds of MB.
		NUM_COMMAND_BUFFERS = 8,
	};

	struct OptionalExtensions
	{
		bool vk_ext_provoking_vertex : 1;
		bool vk_ext_memory_budget : 1;
		bool vk_ext_calibrated_timestamps : 1;
		bool vk_ext_rasterization_order_attachment_access : 1;
		bool vk_ext_roaa_depth : 1; ///< ROAA depth sub-feature (rasterizationOrderDepthAttachmentAccess); optional, often absent when color ROAA is present.
		bool vk_ext_full_screen_exclusive : 1;
		bool vk_ext_line_rasterization : 1;
		bool vk_swapchain_maintenance1 : 1;
		bool vk_swapchain_maintenance1_is_khr : 1;
		bool vk_khr_push_descriptor : 1;
		bool vk_khr_driver_properties : 1;
		bool vk_khr_shader_non_semantic_info : 1;
		bool vk_ext_attachment_feedback_loop_layout : 1;
		bool vk_ext_fragment_shader_interlock : 1;
		bool vk_ext_device_fault : 1;
		bool vk_ext_descriptor_indexing : 1; ///< Indexed state tables addressed as storage-buffer arrays (TileGpu).
		bool vk_khr_draw_indirect_count : 1; ///< Count-buffer indirect draws (TileGpu nicety, not part of the capability gate).
		bool tilegpu_device_capable : 1; ///< The whole TileGpu device contract (descriptor indexing + indirect draw stream) is present.
		bool tilegpu_bindless_targets : 1; ///< ...and dynamic indexing of a sampled-image array, which the rule-2 tap needs to bind targets.
		bool tilegpu_self_read : 1; ///< ...and rasterization-order colour access, which the in-pass destination read needs.
		bool tilegpu_dual_source : 1; ///< ...and dual-source blending, which the As blend factor rides as an index-1 output.
		/// Both are required by the LSFG frame-generation shaders and by NOTHING else in the
		/// renderer. They are requested anyway whenever the driver really has them, because the
		/// alternative is recreating the device when frame generation is switched on.
		bool vk_khr_vulkan_memory_model : 1;   ///< shaders declare the Vulkan memory model
		bool vk_ext_robustness2_null_descriptor : 1; ///< nullDescriptor only; not the robust-access bits
	};

	// Global state accessors
	__fi VkInstance GetVulkanInstance() const { return m_instance; }
	__fi VkPhysicalDevice GetPhysicalDevice() const { return m_physical_device; }
	__fi VkDevice GetDevice() const { return m_device; }
	__fi VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
	__fi VmaAllocator GetAllocator() const { return m_allocator; }
	__fi u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_queue_family_index; }
	__fi u32 GetPresentQueueFamilyIndex() const { return m_present_queue_family_index; }
	__fi const VkPhysicalDeviceProperties& GetDeviceProperties() const { return m_device_properties; }
	__fi const OptionalExtensions& GetOptionalExtensions() const { return m_optional_extensions; }

	/// EmuCore/GS/TileGpuStrictMemory, read once at device creation. Set, every host-visible
	/// allocation the CPU writes through must land on a HOST_COHERENT memory type rather than
	/// merely preferring one -- so a CPU write reaches the GPU whether or not the flush that
	/// follows it is correctly ranged. Paired with robustBufferAccess, which the same key turns
	/// on. Diagnostic only; both cost performance and neither fixes anything.
	__fi bool StrictHostMemory() const { return m_strict_host_memory; }

	// The interaction between raster order attachment access and fbfetch is unclear.
	__fi bool UseFeedbackLoopLayout() const
	{
		return m_optional_extensions.vk_ext_attachment_feedback_loop_layout &&
		       !m_optional_extensions.vk_ext_rasterization_order_attachment_access;
	}

	// Helpers for getting constants
	__fi u32 GetBufferCopyOffsetAlignment() const
	{
		return static_cast<u32>(m_device_properties.limits.optimalBufferCopyOffsetAlignment);
	}
	__fi u32 GetBufferCopyRowPitchAlignment() const
	{
		return static_cast<u32>(m_device_properties.limits.optimalBufferCopyRowPitchAlignment);
	}

	/// Returns true if running on an NVIDIA GPU.
	__fi bool IsDeviceNVIDIA() const { return (m_device_properties.vendorID == 0x10DE); }

	/// Returns true if running on an AMD GPU.
	__fi bool IsDeviceAMD() const { return (m_device_properties.vendorID == 0x1002); }

	/// Returns true if running on an Intel GPU (vendorID 0x8086).
	__fi bool IsDeviceIntel() const { return (m_device_properties.vendorID == 0x8086u); }

	/// Returns true if running on a Broadcom V3D GPU (vendorID 0x14E4) — i.e. the
	/// Raspberry Pi's VideoCore under Mesa's V3DV, reached via the Linux arm64 build.
	__fi bool IsDeviceBroadcom() const { return (m_device_properties.vendorID == 0x14E4u); }

	/// Returns true if running on an ARM Mali GPU (vendorID 0x13B5).
	__fi bool IsDeviceMali() const { return (m_device_properties.vendorID == 0x13B5u); }

	/// Returns true if running on a Qualcomm Adreno GPU (vendorID 0x5143).
	__fi bool IsDeviceAdreno() const { return (m_device_properties.vendorID == 0x5143u); }

	// Adreno-5xx / pre-0x801EA000 driver bug: colorWriteMask is ignored while a depth
	// test is active (PPSSPP #10421). Cached in CheckFeatures, consumed in CreateTFXPipeline.
	bool m_broken_colormask_with_depth = false;

	/// Returns true if running on an Imagination PowerVR GPU (vendorID 0x1010).
	__fi bool IsDevicePowerVR() const { return (m_device_properties.vendorID == 0x1010u); }

	/// Returns true if running on a Samsung Xclipse (Exynos AMD-RDNA2) GPU.
	/// NOTE: 0x144D (Samsung) is unverified across driver revisions — a real Xclipse tester
	/// must confirm this fires; if it reports a different vendorID the gate is simply inert.
	__fi bool IsDeviceXclipse() const { return (m_device_properties.vendorID == 0x144Du); }

	/// Returns true if running on an Apple GPU, under either MoltenVK or Asahi's Honeykrisp.
	/// Unlike the checks above this gates on driverID, because Apple silicon does not report
	/// Apple's vendorID on every driver — Honeykrisp reports Mesa's 0x10005, so a vendorID
	/// check would silently miss it.
	__fi bool IsDeviceAppleGPU() const
	{
		return (m_device_driver_properties.driverID == VK_DRIVER_ID_MOLTENVK ||
				m_device_driver_properties.driverID == VK_DRIVER_ID_MESA_HONEYKRISP);
	}

	// Creates a simple render pass.
	VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format,
		VkAttachmentLoadOp color_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		VkAttachmentStoreOp color_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
		VkAttachmentStoreOp depth_store_op = VK_ATTACHMENT_STORE_OP_STORE,
		VkAttachmentLoadOp stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VkAttachmentStoreOp stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE, bool color_feedback_loop = false,
		bool depth_sampling = false, bool tilegpu_self_read = false);

	// Gets a non-clearing version of the specified render pass. Slow, don't call in hot path.
	VkRenderPass GetRenderPassForRestarting(VkRenderPass pass);

	/// The render-area granularity this render pass wants an area aligned to, memoized.
	///
	/// It is a driver query rather than a device constant -- vkGetRenderAreaGranularity takes the
	/// render pass -- and the TileGpu executor asks it once per pass, hundreds of times a frame over
	/// a handful of distinct passes, so the answer is kept.
	VkExtent2D GetRenderAreaGranularity(VkRenderPass pass);

	// These command buffers are allocated per-frame. They are valid until the command buffer
	// is submitted, after that you should call these functions again.
	__fi VkCommandBuffer GetCurrentCommandBuffer() const { return m_current_command_buffer; }
	__fi VKStreamBuffer& GetTextureUploadBuffer() { return m_texture_stream_buffer; }
	VkCommandBuffer GetCurrentInitCommandBuffer();

	/// Allocates a descriptor set from the pool reserved for the current frame.
	VkDescriptorSet AllocatePersistentDescriptorSet(VkDescriptorSetLayout set_layout);

	/// Allocates a descriptor set from the current frame's pool chain, growing the chain if every
	/// existing link is full. Returns VK_NULL_HANDLE only when the device cannot give us another
	/// pool, or when the layout is one no pool of this shape can serve.
	VkDescriptorSet AllocateDescriptorSetFromFramePool(VkDescriptorSetLayout set_layout);

	/// Frees a descriptor set allocated from the global pool.
	void FreePersistentDescriptorSet(VkDescriptorSet set);

	/// True when the device uses VK_KHR_push_descriptor for texture binding (everything except Mali,
	/// whose driver crashes inside vkCmdPushDescriptorSetKHR). When false, textures are bound via
	/// per-frame allocated descriptor sets (vkUpdateDescriptorSets + vkCmdBindDescriptorSets).
	__fi bool UsePushDescriptors() const { return m_use_push_descriptors; }


	// Gets the fence that will be signaled when the currently executing command buffer is
	// queued and executed. Do not wait for this fence before the buffer is executed.
	__fi VkFence GetCurrentCommandBufferFence() const { return m_frame_resources[m_current_frame].fence; }

	// Fence "counters" are used to track which commands have been completed by the GPU.
	// If the last completed fence counter is greater or equal to N, it means that the work
	// associated counter N has been completed by the GPU. The value of N to associate with
	// commands can be retreived by calling GetCurrentFenceCounter().
	u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }

	// Gets the fence that will be signaled when the currently executing command buffer is
	// queued and executed. Do not wait for this fence before the buffer is executed.
	u64 GetCurrentFenceCounter() const { return m_frame_resources[m_current_frame].fence_counter; }

	// The GSDevice submission-epoch view of the same counters. Completed is polled
	// (non-blocking fence status scan) so a caller between submits sees work that has
	// finished since the last wait, not just since the last submit.
	u64 GetSubmitEpoch() const override { return GetCurrentFenceCounter(); }
	bool TileReinterpretIndex(GSTexture* owner, GSTexture* dst, const TileReinterpretParams& p) override;
	bool TileClutFromTarget(GSTexture* owner, GSTexture* dst, const TileClutGatherParams& p) override;
	bool TileSwizzleFormsFit(bool& clut_ok) override;
	bool TileExpandPalette(GSTexture* index, GSTexture* palette, GSTexture* dst, u32 src_level, u32 dst_level) override;
	bool TileGpuExecutorAvailable() override;
	bool TileGpuBindlessTargets() override;
	bool TileGpuSelfRead() override;
	bool TileGpuDualSourceBlend() override;
	bool TileGpuPrefersDepthUniformPasses() override;
	u32 TileGpuMaxPassDraws() override;
	u32 TileGpuMaxSpecializationBinds() override;
	bool TileGpuSegregatesSelfRead() override;

	/// The writeback compute's workgroup edge in page WORDS, for this device. Not a GSDevice virtual
	/// like the answers above it, because nothing outside this backend asks: the two callers are the
	/// pipeline build that bakes it into the shader's local_size and the dispatch that divides the
	/// page by it, and both are here. Must stay a pure function of the device — the pipeline is
	/// compiled once and the dispatches come later, and a dim that changed between them would run a
	/// grid the shader was not built for.
	u32 TileGpuWritebackGroupDim() const;
	bool TileGpuClutMergeCompiled() override { return m_tilegpu_clut_merge; }
	bool TileGpuClutMergePagesCompiled() override { return m_tilegpu_clut_merge_pages; }
	bool ExecuteTileGpuPassPlan(const GSTileGpuPassPlan& plan) override;
	u64 GetCompletedSubmitEpoch() override
	{
		ScanForCommandBufferCompletion();
		return m_completed_fence_counter;
	}

	// Schedule a vulkan resource for destruction later on. This will occur when the command buffer
	// is next re-used, and the GPU has finished working with the specified resource.
	void DeferBufferDestruction(VkBuffer object, VmaAllocation allocation);
	void DeferFramebufferDestruction(VkFramebuffer object);
	void DeferImageDestruction(VkImage object, VmaAllocation allocation);
	void DeferImageViewDestruction(VkImageView object);

	/// Why the host is about to block on a GPU fence. `Ring` is the command-buffer ring's own
	/// recycle wait — the pipeline is full and the frame ahead has to retire — which is
	/// backpressure and belongs in nobody's drain bill. Everything else is a wait the GS thread
	/// paid out of turn to get an answer, and that is the population the readback work is judged
	/// against (see GSPerfMon::GpuBlockingWaits).
	///
	/// `SourceSet` is the TileGpu source-descriptor ring coming round to a set whose last reader
	/// has not retired (WriteTileGpuSourceSet). ⚠️ The judgement call, made deliberately and
	/// written down because the obvious reading points the other way: this is resource-recycling
	/// backpressure in the same SENSE as `Ring` — a fixed pool of reusable objects, wrapped — and
	/// it is nevertheless counted in GpuBlockingWaits alongside `Sync`, not excused with `Ring`.
	/// Two reasons. It stalls the GS thread in the MIDDLE of a frame, before the plan it is
	/// building has been recorded, so the frame it delays is the one being built and not the one
	/// ahead. And the acceptance metric for the whole readback campaign is
	/// GpuBlockingWaits/frame: a wait that leaves that number because it was renamed is a metric
	/// improving while the machine does not. It gets its own bucket so the residual is a read
	/// instead of an inference — itemised, not excused.
	enum class GpuWaitCause
	{
		Ring,
		Sync,
		SourceSet,
	};

	// Wait for a fence to be completed.
	// Also invokes callbacks for completion.
	void WaitForFenceCounter(u64 fence_counter, GpuWaitCause cause = GpuWaitCause::Sync);

	void WaitForGPUIdle();

private:
	// Helper method to create a Vulkan instance.
	static VkInstance CreateVulkanInstance(const WindowInfo& wi, OptionalExtensions* oe, bool enable_debug_utils,
		bool enable_validation_layer);

	// Enable/disable debug message runtime.
	bool EnableDebugUtils();
	void DisableDebugUtils();

	void SubmitCommandBuffer(VKSwapChain* present_swap_chain);
	void MoveToNextCommandBuffer();

	enum class WaitType
	{
		None,
		Sleep,
		Spin,
	};

	static WaitType GetWaitType(bool wait, bool spin);
	void ExecuteCommandBuffer(WaitType wait_for_completion);

	// Allocates a temporary CPU staging buffer, fires the callback with it to populate, then copies to a GPU buffer.
	bool AllocatePreinitializedGPUBuffer(u32 size, VkBuffer* gpu_buffer, VmaAllocation* gpu_allocation,
		VkBufferUsageFlags gpu_usage, const std::function<void(void*)>& fill_callback);
	
	// Helper function for uploading indices.
	void UploadIndices(VKStreamBuffer& buffer, const void* index, size_t count);

	union RenderPassCacheKey
	{
		struct
		{
			u32 color_format : 8;
			u32 depth_format : 8;
			u32 color_load_op : 2;
			u32 color_store_op : 1;
			u32 depth_load_op : 2;
			u32 depth_store_op : 1;
			u32 stencil_load_op : 2;
			u32 stencil_store_op : 1;
			u32 color_feedback_loop : 1;
			u32 depth_sampling : 1;
			/// TileGpu's declared in-pass destination read: the colour attachment is referenced as an
			/// input attachment too, sits in GENERAL for the pass's whole life, and the subpass carries
			/// the rasterization-order colour-access flag with NO self-dependency. A key bit of its own
			/// rather than a reuse of color_feedback_loop because that one's shape is decided by two
			/// other toggles (UseFeedbackLoopLayout, framebuffer_fetch) and this one's is fixed: it is
			/// the shape the ROAA correctness probe passed on both device tiers, and nothing else.
			u32 tilegpu_self_read : 1;
		};

		u32 key;
	};

	using ExtensionList = std::vector<const char*>;
	static bool SelectInstanceExtensions(ExtensionList* extension_list, const WindowInfo& wi, OptionalExtensions* oe,
		bool enable_debug_utils);
	bool SelectDeviceExtensions(ExtensionList* extension_list, bool enable_surface);
	bool SelectDeviceFeatures();
	bool CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer);
	bool ProcessDeviceExtensions();

	bool CreateAllocator();
	bool CreateCommandBuffers();
	bool CreateGlobalDescriptorPool();
	/// One link of a frame's descriptor-pool chain. See AllocateDescriptorSetFromFramePool.
	VkDescriptorPool CreateFrameDescriptorPool();

	VkRenderPass CreateCachedRenderPass(RenderPassCacheKey key);

	void CommandBufferCompleted(u32 index);
	void ActivateCommandBuffer(u32 index);
	void ScanForCommandBufferCompletion();
	void WaitForCommandBufferCompletion(u32 index, GpuWaitCause cause = GpuWaitCause::Sync);
	/// VK_EXT_device_fault post-mortem: on VK_ERROR_DEVICE_LOST, logs the driver's
	/// structured fault records (addresses, kinds, vendor codes) before the exit.
	void ReportDeviceFault();

	bool InitSpinResources();
	void DestroySpinResources();
	void WaitForSpinCompletion(u32 index);
	void SpinCommandCompleted(u32 index);
	void SubmitSpinCommand(u32 index, u32 cycles);
	void CalibrateSpinTimestamp();
	u64 GetCPUTimestamp();

	// For pipeline statistics
	enum class QueryState
	{
		None,
		Querying,
		Ready,
	};

	struct FrameResources
	{
		// [0] - Init (upload) command buffer, [1] - draw command buffer
		VkCommandPool command_pool = VK_NULL_HANDLE;
		std::array<VkCommandBuffer, 2> command_buffers{VK_NULL_HANDLE, VK_NULL_HANDLE};
		// Per-frame descriptor pools, reset wholesale each time the frame is reused. A CHAIN, not
		// one pool: allocation walks it and appends another link when the current one is full, so
		// the frame's capacity is whatever the frame turns out to need. See
		// AllocateDescriptorSetFromFramePool. Created lazily, so a device that never allocates
		// from it never has one.
		std::vector<VkDescriptorPool> descriptor_pools;
		// Which link allocations are coming from, and how many sets it has served since it was
		// reset. The count decides when the link is full -- drivers are not reliable about saying
		// so -- and it also separates "full" from "no link of this shape can ever serve that
		// layout": a request an EMPTY link refuses is unservable, and growing for it would append
		// pools forever.
		u32 descriptor_pool_cursor = 0;
		u32 descriptor_pool_cursor_sets = 0;
		VkFence fence = VK_NULL_HANDLE;
		u64 fence_counter = 0;
		s32 spin_id = -1;
		u32 submit_timestamp = 0;
		bool init_buffer_used = false;
		bool needs_fence_wait = false;
		bool timestamp_written = false;
		QueryState pipeline_statistics_query = QueryState::None;

		std::vector<std::function<void()>> cleanup_resources;
	};

	struct SpinResources
	{
		VkCommandPool command_pool = VK_NULL_HANDLE;
		VkCommandBuffer command_buffer = VK_NULL_HANDLE;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		u32 cycles = 0;
		bool in_progress = false;
	};

	VkInstance m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VmaAllocator m_allocator = VK_NULL_HANDLE;

	VkCommandBuffer m_current_command_buffer = VK_NULL_HANDLE;

	VkDescriptorPool m_global_descriptor_pool = VK_NULL_HANDLE;

	// A layout an EMPTY frame descriptor pool refused: the pool shape reserves no descriptors of
	// some type it declares, so growing the chain for it would never help. Warned once.
	bool m_frame_pool_layout_refused_warned = false;

	// Set false for Mali (vendorID 0x13B5) in CreateDevice: its driver crashes inside
	// vkCmdPushDescriptorSetKHR, so texture binding falls back to per-frame descriptor sets.
	bool m_use_push_descriptors = true;

	// MediaTek-SoC detection now lives in the base GSDevice (SetMediaTekSoC/IsMediaTekSoC),
	// so both backends and GS.cpp's Android GameDB overrides can read it.

	VkQueue m_graphics_queue = VK_NULL_HANDLE;
	VkQueue m_present_queue = VK_NULL_HANDLE;
	u32 m_graphics_queue_family_index = 0;
	u32 m_present_queue_family_index = 0;

	ReadbackSpinManager m_spin_manager;
	VkQueue m_spin_queue = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_spin_descriptor_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_spin_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_spin_pipeline = VK_NULL_HANDLE;
	VkBuffer m_spin_buffer = VK_NULL_HANDLE;
	VmaAllocation m_spin_buffer_allocation = VK_NULL_HANDLE;
	VkDescriptorSet m_spin_descriptor_set = VK_NULL_HANDLE;
	std::array<SpinResources, NUM_COMMAND_BUFFERS> m_spin_resources;

	// The out-of-band command buffer: one-shot work submitted on its own fence while the
	// frame's buffer goes on being recorded. Because queue submissions execute in order,
	// what it records runs after everything already submitted and BEFORE the buffer being
	// recorded, so it may only touch images none of the recording buffers touch (see
	// GSTextureVK::StampGpuTouch). Used by the readback path for images the GPU has
	// finished with: a copy that waits on itself instead of on the whole frame.
	struct OutOfBandResources
	{
		VkCommandPool command_pool = VK_NULL_HANDLE;
		VkCommandBuffer command_buffer = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool recording = false;
	};
	OutOfBandResources m_oob;
	u64 m_oob_wait_ns = 0;
	u64 m_oob_wait_calls = 0;
	u64 m_sync_wait_ns = 0;
	u64 m_sync_wait_calls = 0;
	u64 m_ring_wait_ns = 0;
	u64 m_ring_wait_calls = 0;
	// Its own bucket, disjoint from the Sync one -- but inside GpuBlockingWaits with it, unlike the
	// ring's. See GpuWaitCause for why a recycling wait is counted there.
	u64 m_source_set_wait_ns = 0;
	u64 m_source_set_wait_calls = 0;
#ifdef _WIN32
	double m_queryperfcounter_to_ns = 0;
#endif
	double m_spin_timestamp_scale = 0;
	double m_spin_timestamp_offset = 0;
	u32 m_spin_queue_family_index = 0;
	u32 m_command_buffer_render_passes = 0;
	u32 m_spin_timer = 0;
	bool m_spinning_supported = false;
	bool m_spin_queue_is_graphics_queue = false;
	bool m_spin_buffer_initialized = false;

	VkQueryPool m_timestamp_query_pool = VK_NULL_HANDLE;
	float m_accumulated_gpu_time = 0.0f;
	bool m_gpu_timing_enabled = false;
	bool m_gpu_timing_supported = false;

	VkQueryPool m_pipeline_statistics_query_pool = VK_NULL_HANDLE;
	GPUPipelineStatistics m_accumulated_gpu_pipeline_statistics{};
	bool m_gpu_pipeline_statistics_enabled = false;
	bool m_gpu_pipeline_statistics_supported = false;
	bool m_wants_new_timestamp_calibration = false;
	VkTimeDomainEXT m_calibrated_timestamp_type = VK_TIME_DOMAIN_DEVICE_EXT;

	std::array<FrameResources, NUM_COMMAND_BUFFERS> m_frame_resources;
	u64 m_next_fence_counter = 1;
	u64 m_completed_fence_counter = 0;
	u32 m_current_frame = 0;

	bool m_last_submit_failed = false;

	std::map<u32, VkRenderPass> m_render_pass_cache;
	std::map<VkRenderPass, VkExtent2D> m_render_area_granularity;

	VkDebugUtilsMessengerEXT m_debug_messenger_callback = VK_NULL_HANDLE;

	VkPhysicalDeviceFeatures m_device_features = {};
	VkPhysicalDeviceProperties m_device_properties = {};
	VkPhysicalDeviceDriverPropertiesKHR m_device_driver_properties = {};
	OptionalExtensions m_optional_extensions = {};
	bool m_colorclip_fallback_to_hdr = false;
	bool m_strict_host_memory = false;

	u32 m_max_framebuffer_width = 0;
	u32 m_max_framebuffer_height = 0;
public:
	enum FeedbackLoopFlag : u8
	{
		FeedbackLoopFlag_None = 0,
		FeedbackLoopFlag_ReadAndWriteRT = 1,
		FeedbackLoopFlag_ReadDepth = 2,
		FeedbackLoopFlag_ReadAndWriteDepth = 4,
		/// TileGpu's declared in-pass destination read. Like FeedbackLoopFlag_ReadAndWriteRT it puts the
		/// colour target in the feedback-loop layout, and unlike it the framebuffer's render pass is
		/// built to the declared shape -- one input-attachment reference, the rasterization-order
		/// subpass flag, and NO self-dependency. That last difference is why it cannot simply reuse the
		/// existing flag: subpass dependencies count towards render-pass compatibility, so a framebuffer
		/// built with the self-dependency cannot begin a pass that has none.
		FeedbackLoopFlag_TileGpuSelfRead = 8,
	};

	enum class ResourceType
	{
		SRV, // Shader resource view (read only)
		UAV, // Unordered access (read/write)
	};

	static constexpr GSTextureVK::Layout GetResourceLayout(ResourceType type)
	{
		switch (type)
		{
			default:
				pxFailRel("Impossible.");
			case ResourceType::SRV:
				return GSTextureVK::Layout::ShaderReadOnly;
			case ResourceType::UAV:
				return GSTextureVK::Layout::ReadWriteImage;
		}
	}

	struct alignas(8) PipelineSelector
	{
		GSHWDrawConfig::PSSelector ps;

		union
		{
			struct
			{
				u32 topology : 2;
				u32 rt : 1;
				u32 ds : 1;
				u32 line_width : 1;
				u32 feedback_loop_flags : 3;
			};

			u32 key;
		};

		GSHWDrawConfig::BlendState bs;
		GSHWDrawConfig::VSSelector vs;
		GSHWDrawConfig::DepthStencilSelector dss;
		GSHWDrawConfig::ColorMaskSelector cms;
		u8 pad;

		__fi bool operator==(const PipelineSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const PipelineSelector& p) const { return !BitEqual(*this, p); }

		__fi PipelineSelector() { std::memset(this, 0, sizeof(*this)); }

		__fi bool IsRTFeedbackLoop() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteRT) != 0); }
		__fi bool IsDepthFeedbackLoop() const { return ((feedback_loop_flags & FeedbackLoopFlag_ReadAndWriteDepth) != 0); }
		__fi bool IsTestingAndSamplingDepth() const { return ((feedback_loop_flags & (FeedbackLoopFlag_ReadDepth | FeedbackLoopFlag_ReadAndWriteDepth)) != 0); }
	};
	static_assert(sizeof(PipelineSelector) == 32, "Pipeline selector is 32 bytes");

	struct PipelineSelectorHash
	{
		std::size_t operator()(const PipelineSelector& e) const noexcept
		{
			std::size_t hash = 0;
			HashCombine(hash, e.vs.key, e.ps.key_hi, e.ps.key_lo, e.dss.key, e.cms.key, e.bs.key, e.key);
			return hash;
		}
	};

	enum : u32
	{
		NUM_TFX_DYNAMIC_OFFSETS = 2,
		NUM_UTILITY_SAMPLERS = 1,
		CONVERT_PUSH_CONSTANTS_SIZE = 96,

		NUM_CAS_PIPELINES = 2,
		NUM_FSR1_PIPELINES = 2, // [0] RCAS, [1] EASU
	};
	enum TFX_DESCRIPTOR_SET : u32
	{
		TFX_DESCRIPTOR_SET_UBO,
		TFX_DESCRIPTOR_SET_TEXTURES,

		NUM_TFX_DESCRIPTOR_SETS,
	};
	enum TFX_TEXTURES : u32
	{
		TFX_TEXTURE_TEXTURE = 0,
		TFX_TEXTURE_PALETTE,
		TFX_TEXTURE_RT,
		TFX_TEXTURE_PRIMID,
		TFX_TEXTURE_DEPTH,
		TFX_TEXTURE_RT_ROV,
		TFX_TEXTURE_DEPTH_ROV,

		NUM_TFX_TEXTURES
	};

private:
	std::unique_ptr<VKSwapChain> m_swap_chain;
	bool m_resize_requested = false;
	bool m_is_presenting = false;

	VkDescriptorSetLayout m_utility_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_utility_pipeline_layout = VK_NULL_HANDLE;
	// Cached last-set utility push constants, replayed after a command-buffer rollover restart
	// (present or render pass) so mobile Vulkan drivers don't draw with stale coordinates.
	std::array<u8, CONVERT_PUSH_CONSTANTS_SIZE> m_utility_push_constants{};
	u32 m_utility_push_constants_size = 0;

	VkDescriptorSetLayout m_tfx_ubo_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_texture_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tfx_pipeline_layout = VK_NULL_HANDLE;

	VKStreamBuffer m_vertex_stream_buffer;
	VKStreamBuffer m_index_stream_buffer;
	VKStreamBuffer m_expand_index_stream_buffer;
	VKStreamBuffer m_vertex_uniform_stream_buffer;
	VKStreamBuffer m_fragment_uniform_stream_buffer;
	VKStreamBuffer m_texture_stream_buffer;
	VkBuffer m_expand_index_buffer = VK_NULL_HANDLE;
	VmaAllocation m_expand_index_buffer_allocation = VK_NULL_HANDLE;

	VkSampler m_point_sampler = VK_NULL_HANDLE;
	VkSampler m_linear_sampler = VK_NULL_HANDLE;

	std::unordered_map<u32, VkSampler> m_samplers;

	std::vector<VkPipeline> m_convert;
	/// Tile renderer: a render target read as an indexed texture, one pipeline per
	/// GSTileSwizzleForms::IndexFormat (0..4), and a palette gathered off a render
	/// target (5: sixteen entries, 6: two hundred and fifty-six) — compiled together on
	/// first use with the swizzle forms fitted from the tree's tables injected as
	/// defines (see tile_convert.glsl).
	std::array<VkPipeline, 7> m_tile_reinterpret{};
	bool m_tile_reinterpret_tried = false;
	GSTileSwizzleForms::FormSet m_tile_forms{};
	bool m_tile_forms_fitted = false;
	std::string m_tile_form_defines;
	const std::string& TileFormDefines();
	/// Tile renderer: an index texture expanded through its palette
	/// (ps_tile_expand_palette). Two combined image samplers — index and palette —
	/// so it carries its own descriptor and pipeline layouts (the utility layout has
	/// one); compiled with them on first use, like the reinterpret pipelines.
	VkDescriptorSetLayout m_tile_expand_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tile_expand_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_tile_expand_pipeline = VK_NULL_HANDLE;
	bool m_tile_expand_tried = false;
	std::array<GSTextureVK*, 2> m_tile_expand_textures{};
	bool ApplyTileExpandState(bool already_execed = false);

	/// The TileGpu executor's geometry pipeline: a raw-GSVertex draw whose per-draw state (the
	/// screen->NDC transform, the texture block) is a row of an indexed state table in a storage
	/// buffer, selected by the indirect draw's first_instance. Indexed [GSTileGpuTopology][depth
	/// mode]: outer is triangle/line/point, inner GSTileGpuDepthMode. Compiled on first use.
	/// Push-constant range shared by every TileGpu layout. The WRITEBACK is what sizes it: it needs
	/// all nine (the ring's table, entry and keep-mask bases plus the op's own five), where the seed
	/// and the geometry program use five. Vulkan guarantees 128 bytes, so this is nowhere near a
	/// limit -- but a shader declaring MORE words than the range is a validation error, so the two
	/// move together.
	static constexpr u32 kTileGpuPushWords = 9;
	/// The donor build's own range: the two layouts it reinterprets between, and nothing else. It
	/// has a layout of its own (one sampler, no state SSBO) because it reads no bytes, so it shares
	/// neither the range above nor the reason for it.
	static constexpr u32 kTileGpuDonorPushWords = 4;
	static constexpr u32 kTileGpuNoBlend = 0xFFFFFFFFu; ///< CreateTileGpuPipeline: no blending
	VkPipelineLayout m_tilegpu_pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tilegpu_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSet m_tilegpu_state_descriptor_set = VK_NULL_HANDLE;
	// Set 1, bound per pass -- pushed where the device pushes descriptors, a per-frame set
	// otherwise. Binding 0 is the pass's snapshot of its own colour target (combined sampler);
	// binding 1 is its rule-2 sampled targets, a fixed array of kMaxTexSourcesPerPass combined
	// samplers the fragment shader indexes by literal. They share one set because a pipeline
	// layout may carry at most one push-descriptor set. The null texture stands in for slots the
	// pass does not use.
	VkDescriptorSetLayout m_tilegpu_snapshot_ds_layout = VK_NULL_HANDLE;
	// Set 2, written once per plan: rule 3's frame-wide array of kMaxSources materialised sources,
	// combined image samplers so the wrap and the filter ride in the descriptor. It cannot be a push
	// set (a pipeline layout carries at most one and set 1 is it) and it cannot come from the frame
	// pool either, which only exists on the non-push path -- so it gets a small pool of its own and a
	// RING of persistent sets, each stamped with the submit epoch it was last written under and taken
	// again only once that submission has completed. Rewriting a set an in-flight pass still reads
	// gives intermittently wrong textures, worst on the fastest device; the stamp is what stops it.
	// The ring's depth is a RUNTIME count (EmuCore/GS/TileGpuSourceSetRingDepth over the built-in
	// default, resolved by gsTileGpuSourceSetRingDepth), so the arrays are sized for the largest
	// depth the key admits and m_tilegpu_source_set_count says how much of them is real. The max
	// doubles as the "no set" sentinel WriteTileGpuSourceSet returns, since no live index reaches it.
	static constexpr u32 kTileGpuSourceSetsMax = kGSTileGpuSourceSetRingMax;
	static constexpr u32 kTileGpuSourceSamplers = 8; ///< wrap U x wrap V x filter, the eight rule 3 admits
	VkDescriptorSetLayout m_tilegpu_source_ds_layout = VK_NULL_HANDLE;
	/// Set 3: the pass's own colour attachment as an input attachment. Bound only by a pass that
	/// declares the in-pass destination read. Never a push set -- see its creation site.
	VkDescriptorSetLayout m_tilegpu_dest_ds_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_tilegpu_source_pool = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, kTileGpuSourceSetsMax> m_tilegpu_source_sets{};
	std::array<u64, kTileGpuSourceSetsMax> m_tilegpu_source_set_epoch{};
	std::array<VkSampler, kTileGpuSourceSamplers> m_tilegpu_source_sampler{};
	u32 m_tilegpu_source_next_set = 0;
	/// How many of the arrays above hold real sets. Zero until the pool is built.
	u32 m_tilegpu_source_set_count = 0;
	/// Take the next set of the ring that no in-flight submission is reading, and fill it with
	/// `sources` (every unused slot the null texture, so the shader's static use of the array never
	/// needs descriptorBindingPartiallyBound). Returns its index, or kTileGpuSourceSetsMax on failure.
	u32 WriteTileGpuSourceSet(std::span<const GSTileGpuPassPlan::SourceBind> sources);
	// [topology][depth mode]; the depth index is the DRAW's GSTileGpuDepthMode, off the run key
	// (GSTileGpuPassPlan::depth_modes), because the depth state is per draw and not per pass.
	// These are the no-blend pipelines; blending variants are created on first use per
	// (topology, depth mode, GS ALPHA index) and cached below -- the GS blend equation maps onto
	// fixed-function factors through GSDevice::m_blendMap, with As as the shader's dual-source
	// output and FIX as the blend constant (dynamic).
	std::array<std::array<VkPipeline, GSDevice::kGSTileGpuDepthModes>, 3> m_tilegpu_pipeline{};
	// key: topology<<0 (2 bits) | depth<<2 (2) | blend<<8 (7, < 81) | colour write mask<<16 (4) |
	// road mask<<20 (3, byte/target/source) | texel-arm mask<<23 (6) | pass self-read mask<<32 (3) |
	// this pass DECLARES the read<<35 | this DRAW reads<<36 | the frame quantises<<37 | the run's
	// frozen per-draw GS state<<38 (18, GSTileGpuPassPlan's spec half shifted down) -- so bits 4-7,
	// 15, 29-31 and 56 up are
	// still free. Sixty-four bits because the three self-read fields no longer fit in thirty-two.
	std::unordered_map<u64, VkPipeline> m_tilegpu_blend_pipelines;
	// `declares` is the PASS's answer, not this run's: the declaration is an input-attachment
	// reference in the render pass, so every pipeline the pass binds has to be built against it
	// whether or not this run's fragment stage reads anything. The four mask arguments are the RUN's
	// (GSTileGpuPassPlan::variant_keys), which is a subset of the pass's union; `spec` is the same
	// key's frozen per-draw state, which has no pass union to be a subset OF -- it is run-only.
	VkPipeline GetTileGpuPipeline(u32 topology, u32 depth_mode, u32 blend_key, u32 road_mask, u32 texel_mask,
		u32 self_mask, bool quantise, bool declares, const GSDevice::GSTileGpuFragmentSpec& spec);
	VkPipeline TileGpuPipelineFallback(u32 topology, u32 depth_mode, bool declares);
	bool m_tilegpu_declared_fallback_warned = false;
	// The other road a declaring pass can lose its draws on: no descriptor set for the pass's own
	// colour attachment. Its own flag, so a pipeline-build failure cannot silence it, and totals so
	// teardown can say what a run lost.
	bool m_tilegpu_declared_pool_warned = false;
	u32 m_tilegpu_declared_pool_dropped_passes = 0;
	u32 m_tilegpu_declared_pool_dropped_draws = 0;
	// The per-pass set (snapshot + sampled targets) on the road that allocates it from the frame pool
	// instead of pushing it. Every pass of every frame takes that road on a device without push
	// descriptors, so it needs totals of its own rather than sharing the declared read's.
	bool m_tilegpu_pass_pool_warned = false;
	u32 m_tilegpu_pass_pool_dropped_passes = 0;
	u32 m_tilegpu_pass_pool_dropped_draws = 0;
	// A whole frame the executor's draw gate refused: no pipelines, or a plan whose per-draw streams do
	// not agree with each other. Its passes still open, clear or load, and store, so it costs almost
	// what a drawn frame costs and comes out deterministically black -- and every column built before
	// the executor reports the frame the planner intended. Counted because nothing else can see it.
	bool m_tilegpu_undrawn_warned = false;
	u32 m_tilegpu_undrawn_frames = 0;
	u32 m_tilegpu_undrawn_draws = 0;
	// A plan the executor left mid-flight because a render pass object could not be had. The caller
	// discards the return value and the renderer has already recorded the plan as done, so this is the
	// one road whose damage OUTLIVES the frame: the byte model believes reconciliations happened that
	// never reached the GPU.
	bool m_tilegpu_abandoned_warned = false;
	u32 m_tilegpu_abandoned_plans = 0;
	// A fragment variant that failed to compile: its draws run the full module instead, which is a
	// superset and so correct, and about twice the Adreno 650's instruction-size threshold. The only
	// existing report of this is inside a PCSX2_DEVBUILD block, which is not what a device round runs.
	bool m_tilegpu_variant_compile_warned = false;
	u32 m_tilegpu_variant_compile_failures = 0;
	// A pipeline that failed to build OUTSIDE a declaring pass: the run falls back to the eager
	// full-road pipeline, which writes all four channels whatever the draw's FBMSK said.
	bool m_tilegpu_pipeline_fallback_warned = false;
	u32 m_tilegpu_pipeline_fallback_runs = 0;
	// ...and INSIDE one, where there is no compatible fallback and the run's draws are dropped.
	// TileGpuPipelineFallback already says so once; this is what a run lost.
	u32 m_tilegpu_declared_build_dropped_draws = 0;
	// A writeback the frame pool would give no descriptor set for. Its ring slot keeps what it was
	// prefilled with while the renderer's model records the composition as done.
	bool m_tilegpu_writeback_pool_warned = false;
	u32 m_tilegpu_writeback_pool_dropped_ops = 0;
	/// EmuCore/GS/TileGpuSerializeOps and TileGpuSerializeMask are announced once per run, not once per
	/// plan: a plan is a frame and the line exists so a device log can testify which arm engaged, which
	/// one line does.
	bool m_tilegpu_serialize_announced = false;
	/// ...and the contradiction is said once too, on its own flag, because it fires on the road where
	/// the announcement above reports the OTHER key.
	bool m_tilegpu_serialize_mask_refused = false;
	/// EmuCore/GS/TileGpuPoisonAllocations, announced once per run for the same reason.
	bool m_tilegpu_poison_announced = false;
	VkShaderModule m_tilegpu_vs = VK_NULL_HANDLE; // kept alive for lazily-built blend variants
	VkShaderModule m_tilegpu_fs = VK_NULL_HANDLE; // the full module: the eager pipelines and the fallback
	// One fragment module per (road mask, texel-arm mask) a pass has actually asked for, compiled on
	// first use from m_tilegpu_shader_source. The masks name which texel roads and which of the byte
	// road's decode arms the module carries, so most passes run a program a fraction of the full
	// shader's size; the vertex stage takes no road and stays a single module. Keyed by the
	// NORMALISED masks (TileGpuVariantKey), never the plan's raw ones.
	std::unordered_map<u32, VkShaderModule> m_tilegpu_fs_variants;
	std::string m_tilegpu_shader_source; // the device defines + tilegpu.glsl, kept for those compiles
	/// The plan's road mask reduced to what this device can actually serve, so a mask bit never asks
	/// for a road the shader would leave out anyway (and two masks that compile the same program do
	/// not become two modules).
	u32 TileGpuRoadMask(u32 plan_road_mask) const;
	/// The plan's texel-arm mask, normalised against the road mask it rides with: no byte road means
	/// no arms, and a byte road that named none takes the whole set — a superset is slow, a subset is
	/// wrong, and a plan that says "byte road, no arms" is a bug the shader must not render around.
	static u32 TileGpuTexelMask(u32 road_mask, u32 plan_texel_mask);
	/// The normalised masks as one module/pipeline key: roads in bits 0-2, arms in bits 3-9,
	/// what the pass reads its own destination for in bits 10-12.
	/// ...and whether its frame format quantises, at bit 13, and the run's frozen per-draw GS state
	/// in bits 14-31 -- the SAME bit positions GSTileGpuPassPlan::PackVariantKey puts them in, so the
	/// two keys cannot drift apart. The masks arrive normalised; the spec half does not need it.
	static constexpr u32 TileGpuVariantKey(
		u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSDevice::GSTileGpuFragmentSpec& spec)
	{
		return road_mask | (texel_mask << 3) | (self_mask << 10) | (quantise ? (1u << 13) : 0u) |
		       (GSDevice::GSTileGpuPassPlan::PackVariantKey(0, 0, 0, false, spec) &
				   GSDevice::GSTileGpuPassPlan::kVariantSpecMask);
	}
	VkShaderModule GetTileGpuFragmentShader(
		u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSDevice::GSTileGpuFragmentSpec& spec);
	VkShaderModule CompileTileGpuFragmentModule(
		u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSDevice::GSTileGpuFragmentSpec& spec);
	/// `color_write_mask` is the 4-bit rgba mask of channels the draw lands (bit 0 = R .. bit 3 = A),
	/// realized verbatim as the attachment's VkPipelineColorBlendAttachmentState::colorWriteMask.
	/// `declares` builds against the pass's declaring render pass (the colour attachment is an input
	/// attachment too); `reads` additionally puts the rasterization-order flag on the colour-blend
	/// state, which is what makes THIS pipeline's destination reads ordered.
	VkPipeline CreateTileGpuPipeline(u32 topology, u32 depth_mode, u32 blend_index, u32 color_write_mask,
		u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, bool declares, bool reads, u32 dualsrc_road,
		const GSDevice::GSTileGpuFragmentSpec& spec);
	// Indirect-submission streams (created on first executor use, alongside the pipelines): the
	// draw commands (VkDrawIndexedIndirectCommand array), the per-draw state table the VS reads by
	// first_instance, and the frame's ring -- the guest pages the plan reads or reconciles as 8 KB
	// slots, the epoch page tables naming them, prep-op page lists and palettes -- which the FS
	// samples textures from through the GS swizzle. Only allocated when the TileGpu executor runs.
	VKStreamBuffer m_tilegpu_indirect_stream_buffer;
	VKStreamBuffer m_tilegpu_state_stream_buffer;
	VKStreamBuffer m_tilegpu_vram_stream_buffer;
	/// Carry every stream a TileGpu plan stages into onto the command buffer now recording. Call it
	/// at EVERY point the executor ends a command buffer partway through a plan -- the five streams
	/// are the vertices, the indices, the state table, the indirect commands and the ring, and a
	/// plan holds live references to all of them from the moment it stages until its last draw is
	/// recorded. Without this the ring frees them at the first of the plan's submissions instead of
	/// the last; VKStreamBuffer::RetainForCurrentCommandBuffer's comment has the mechanism.
	void RetainTileGpuStreamsForCurrentCommandBuffer();
	bool m_tilegpu_tried = false;
	bool m_tilegpu_tex = false; // the page-swizzle forms fitted, so the byte sampling path compiled in
	// TileGpuClutMergeRegions as it stood when this session's module source was assembled -- see
	// GSDevice::TileGpuClutMergeCompiled for why the renderer reads this and not the setting.
	bool m_tilegpu_clut_merge = false;
	bool m_tilegpu_clut_merge_pages = false;
	bool CompileTileGpuPipeline();
	// Target -> bytes: a compute pass that reswizzles a resident colour target's listed pages into
	// the ring slots the epoch page table names, block- and byte-masked, so a later draw sampling
	// those pages reads the target's pixels through the unchanged texel path. Binding 0 = the target
	// (combined sampler), 1 = the ring SSBO (read-modify-write). Compiled on first use, only when
	// the swizzle forms fitted (m_tilegpu_tex).
	//
	// One pipeline per colour swizzle universe (gsTileByteRoadFormat: 0 = PSMCT32/PSMCT24,
	// 1 = PSMCT16, 2 = PSMCT16S). The page geometry, the block table and the cell width are all
	// compiled in, because a runtime branch on them would be paid per invocation of every page.
	VkPipelineLayout m_tilegpu_writeback_pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tilegpu_writeback_ds_layout = VK_NULL_HANDLE;
	std::array<VkPipeline, kGSTileByteRoadFormats> m_tilegpu_writeback_pipeline{};
	std::array<bool, kGSTileByteRoadFormats> m_tilegpu_writeback_tried{};
	bool CompileTileGpuWritebackPipeline(u32 road_fmt);
	// Bytes -> target: a fragment pass over a colour target that unswizzles the op's pages out of
	// the ring into the target's pixel space (fragments outside the page set discard). Shares the
	// geometry pipeline's layout and descriptor set. Compiled on first use, same gate, and one
	// pipeline per universe for the same reason as the writeback -- the two must be the same list,
	// or a page seeded by one and written back by the other addresses two sets of bytes.
	std::array<VkPipeline, kGSTileByteRoadFormats> m_tilegpu_seed_pipeline{};
	std::array<bool, kGSTileByteRoadFormats> m_tilegpu_seed_tried{};
	bool CompileTileGpuSeedPipeline(u32 road_fmt);
	// Bytes -> a DEPTH target: the same pass writing gl_FragDepth into a Z buffer's attachment, with
	// no colour attachment at all (compare ALWAYS, depth write on). Its own list because the format
	// enumeration is its own -- gsTileDepthRoadFormat's eight, not gsTileByteRoadFormat's four -- and
	// because it pairs with NO writeback: a depth attachment still cannot be turned back into bytes.
	std::array<VkPipeline, kGSTileDepthRoadFormats> m_tilegpu_seed_depth_pipeline{};
	std::array<bool, kGSTileDepthRoadFormats> m_tilegpu_seed_depth_tried{};
	bool CompileTileGpuSeedDepthPipeline(u32 depth_fmt);
	// Bytes -> a texture source: a fragment pass that unswizzles a whole texture window out of the
	// ring into an ordinary RGBA8 image, one fragment per texel, for the hardware sampler to read.
	// One pipeline per source format (0 = PSMCT32, 1 = PSMCT24, 2 = PSMT8, 3 = PSMT4, 4 = PSMT8H,
	// 5 = PSMT4HL, 6 = PSMT4HH), same layout and descriptor set as the seed. Compiled on first use
	// of that format, same gate. The five paletted formats write an INDEX image; its colour arrives
	// from the Expand op that follows.
	static constexpr u32 kTileGpuSrcFormats = 7;
	std::array<VkPipeline, kTileGpuSrcFormats> m_tilegpu_materialise_pipeline{};
	std::array<bool, kTileGpuSrcFormats> m_tilegpu_materialise_tried{};
	bool CompileTileGpuMaterialisePipeline(u32 src_fmt);
	/// The source format a prep op's TEX0.PSM names, or kTileGpuSrcFormats for one this road does
	/// not build. One place, because the renderer's refusals and the executor's pipeline choice have
	/// to agree about which formats exist.
	static u32 TileGpuSrcFormatForPsm(u32 psm);
	// Indices + palette -> colour: rule 3's paletted second stage, recorded raw beside the materialise
	// rather than through GSDevice::TileExpandPalette, whose descriptor binding goes through the
	// device state cache and disturbs the sets the executor established for the passes around it. Its
	// own two-sampler set layout for the same reason -- nothing here may depend on another road's
	// compile having happened.
	VkDescriptorSetLayout m_tilegpu_expand_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tilegpu_expand_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline m_tilegpu_expand_pipeline = VK_NULL_HANDLE;
	bool m_tilegpu_expand_tried = false;
	bool CompileTileGpuExpandPipeline();
	// The two roads that READ A RESIDENT TARGET through the GS swizzle and write a prep texture: the
	// DONOR build (a texture source reinterpreted out of its owner) and the CLUT GATHER (a palette
	// gathered out of the target the game rendered it into). Both are recorded raw beside the expand
	// and for the same reason -- the device's own TileReinterpretIndex / TileClutFromTarget bind
	// through its state cache and would disturb the executor's sets -- and both take exactly one
	// sampler (the owner) and four push words (the two layouts), so they share ONE descriptor set
	// layout and ONE pipeline layout. Compiled on first use, and only where the swizzle forms fitted,
	// since the arithmetic IS the forms.
	VkDescriptorSetLayout m_tilegpu_readtarget_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tilegpu_readtarget_pipeline_layout = VK_NULL_HANDLE;
	bool CompileTileGpuReadTargetLayout();
	VkPipeline CompileTileGpuReadTargetPipeline(const char* shader, const std::string& defines, const char* what);
	static constexpr u32 kTileGpuIdxFormats = 5;
	std::array<VkPipeline, kTileGpuIdxFormats> m_tilegpu_donor_pipeline{};
	std::array<bool, kTileGpuIdxFormats> m_tilegpu_donor_tried{};
	bool CompileTileGpuDonorPipeline(u32 idx_fmt);
	// One pipeline per palette size, because the entry -> word order is a different fitted form for
	// each: index 0 = 256 entries (eight-bit), 1 = 16 (four-bit).
	static constexpr u32 kTileGpuClutSizes = 2;
	std::array<VkPipeline, kTileGpuClutSizes> m_tilegpu_clut_pipeline{};
	std::array<bool, kTileGpuClutSizes> m_tilegpu_clut_tried{};
	bool CompileTileGpuClutGatherPipeline(u32 size_idx);
	/// Whether tilegpu_byte_sel must take its constant-shift form on this device: Honeykrisp
	/// miscompiles the dynamic one on a word loaded from the vram SSBO. Asked in one place because
	/// every shader that extracts a byte from that buffer has to answer it the same way.
	bool TileGpuStaticByteSel() const;
	/// Whether this device's fragment programs may freeze TEXA as a compile-time constant. False on
	/// Honeykrisp, for the reason GSTileGpuFragmentSpec::NarrowToDriver states: the second face of
	/// the miscompile class TileGpuStaticByteSel above already works around. Asked in one place, at
	/// the same layer and off the same driverID, because the #define block and both cache keys have
	/// to answer it identically or a program and its key stop describing each other.
	bool TileGpuFreezeTexa() const;
	std::array<VkPipeline, static_cast<int>(PresentShader::Count)> m_present{};
	std::array<VkPipeline, 2> m_merge{};
	std::array<VkPipeline, NUM_INTERLACE_SHADERS> m_interlace{};
	VkPipeline m_colclip_setup_pipelines[2][2] = {}; // [depth][feedback_loop]
	VkPipeline m_colclip_finish_pipelines[2][2] = {}; // [depth][feedback_loop]
	VkRenderPass m_primid_image_setup_render_passes[2][2] = {}; // [depth][clear]
	VkPipeline m_primid_image_setup_pipelines[2][4] = {}; // [depth][datm]
	VkPipeline m_fxaa_pipeline = {};
	VkPipeline m_shadeboost_pipeline = {};

	VkPipeline GetConvertPipeline(ShaderConvertSelector shader) const
	{
		return m_convert[shader.Index()];
	}

	VkPipeline GetConvertPipeline(ShaderConvert shader) const
	{
		return m_convert[ShaderConvertSelector(shader).Index()];
	}

	std::unordered_map<u32, VkShaderModule> m_tfx_vertex_shaders;
	std::unordered_map<GSHWDrawConfig::PSSelector, VkShaderModule, GSHWDrawConfig::PSSelectorHash>
		m_tfx_fragment_shaders;
	std::unordered_map<PipelineSelector, VkPipeline, PipelineSelectorHash> m_tfx_pipelines;
	u32 m_tfx_pipeline_compile_counter = 0;

	VkRenderPass m_utility_color_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_date_setup_render_pass = VK_NULL_HANDLE;
	VkRenderPass m_swap_chain_render_pass = VK_NULL_HANDLE;

	VkRenderPass m_tfx_render_pass[2][2][2][3][2][2][3][3] = {}; // [rt][ds][colclip][date][fbl][dsp][rt_op][ds_op]

	VkDescriptorSetLayout m_cas_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_cas_pipeline_layout = VK_NULL_HANDLE;
	std::array<VkPipeline, NUM_CAS_PIPELINES> m_cas_pipelines = {};
	VkDescriptorSetLayout m_fsr1_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_fsr1_pipeline_layout = VK_NULL_HANDLE;
	std::array<VkPipeline, NUM_FSR1_PIPELINES> m_fsr1_pipelines = {};
	VkDescriptorSetLayout m_sgsr_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_sgsr_pipeline_layout = VK_NULL_HANDLE;
	/// One per variant: plain and edge-direction. Still one pass each.
	std::array<VkPipeline, NUM_SGSR_PIPELINES> m_sgsr_pipelines = {};
	VkPipeline m_imgui_pipeline = VK_NULL_HANDLE;

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;
	GSHWDrawConfig::VSPushConstants m_vs_pc_cache;

	std::string m_tfx_source;

	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) final;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) final;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) final;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) final;
	bool DoApplyShaderChain(GSTexture* sTex, GSTexture* dTex) override;

	/// librashader filter chain state. The handle is void* rather than
	/// libra_vk_filter_chain_t so this header doesn't need librashader.h — that header
	/// only exists when the Rust toolchain built the lib (ARMSX2_HAS_LIBRASHADER).
	/// The chain is rebuilt only when the preset path changes: creating it compiles the
	/// whole slang chain, while the per-frame call is just command recording.
	void* m_shader_chain = nullptr;
	std::string m_shader_chain_preset;
	bool m_shader_chain_failed = false;
	size_t m_shader_frame_count = 0;
	/// Last parameter-override generation pushed into m_shader_chain. Zeroed whenever the
	/// chain is (re)created, because a new chain starts at the preset's initial values and
	/// has to be re-fed regardless of whether the store changed.
	u64 m_shader_param_generation = 0;
	void DestroyShaderChain();
	void ReleaseShaderChain() override { DestroyShaderChain(); }
	void ApplyShaderChainParams();

	bool DoCAS(
		GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants) final;

	bool DoFSR1EASU(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants) final;
	bool DoFSR1RCAS(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants) final;
	bool DoSGSR(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_SGSR_CONSTANTS>& constants,
		bool edge_direction) final;
	/// Shared body of the two above: same layout, same push range, different pipeline and
	/// different input-side synchronisation.
	bool DoFSR1Pass(
		GSTexture* sTex, GSTexture* dTex, bool easu_pass, const std::array<u32, NUM_FSR1_CONSTANTS>& constants);

	VkSampler GetSampler(GSHWDrawConfig::SamplerSelector ss);
	void ClearSamplerCache() final;

	VkShaderModule GetTFXVertexShader(GSHWDrawConfig::VSSelector sel);
	VkShaderModule GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel);
	VkPipeline CreateTFXPipeline(const PipelineSelector& p);
	VkPipeline GetTFXPipeline(const PipelineSelector& p);

	VkShaderModule GetUtilityVertexShader(const std::string& source, const char* replace_main);
	VkShaderModule GetUtilityFragmentShader(const std::string& source, const char* replace_main, u32* out_spv_words);

	bool CreateDeviceAndSwapChain();
	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreatePipelineLayouts();
	bool CreateRenderPasses();

	bool CompileConvertPipelines();
	bool CompileTileReinterpretPipelines();
	bool CompileTileExpandPipeline();
	bool CompilePresentPipelines();
	bool CompileInterlacePipelines();
	bool CompileMergePipelines();
	bool CompilePostProcessingPipelines();
	bool CompileCASPipelines();
	bool CompileFSR1Pipelines();
	bool CompileSGSRPipeline();

	bool CompileImGuiPipeline();
	void RenderImGui();
	void RenderBlankFrame();

	void DestroyResources();

protected:
	using GSDevice::DoStretchRect; // Suppress overloaded virtual function warning
	virtual void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;
	virtual void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
		PresentShader shader, Filter filter) override;
public:
	GSDeviceVK();
	~GSDeviceVK() override;

	__fi static GSDeviceVK* GetInstance() { return static_cast<GSDeviceVK*>(g_gs_device.get()); }

	// Returns a list of Vulkan-compatible GPUs.
	using GPUList = std::vector<std::pair<VkPhysicalDevice, GSAdapterInfo>>;
	static GPUList EnumerateGPUs();
	static GPUList EnumerateGPUs(VkInstance instance);
	static std::vector<GSAdapterInfo> GetAdapterInfo();

	/// Returns true if Vulkan is suitable as a default for the devices in the system.
	static bool IsSuitableDefaultRenderer();

	__fi VkRenderPass GetTFXRenderPass(bool rt, bool ds, bool colclip, bool stencil, bool fbl, bool dsp,
		VkAttachmentLoadOp rt_op, VkAttachmentLoadOp ds_op) const
	{
		return m_tfx_render_pass[rt][ds][colclip][stencil][fbl][dsp][rt_op][ds_op];
	}
	__fi VkSampler GetPointSampler() const { return m_point_sampler; }
	__fi VkSampler GetLinearSampler() const { return m_linear_sampler; }

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;

	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;

	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;
	void DestroySurface() override;
	std::string GetDriverInfo() const override;

	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;

	PresentResult DoBeginPresent(bool frame_skip) override;
	void EndPresent() override;
	bool IsPresenting() const;

	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;

	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;

	void EnableExtendedStats(bool enabled) override;
	std::vector<std::string> GetExtendedStats() const override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void PushDrawLabel(const std::string_view label) override;
	void PopDrawLabel() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	// Helpers and utility draws.
	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);
	void DrawIndexedPrimitiveVSExpand(int offset, int count, bool vs_indexing, int vs_indexing_expansion);

	// Main GS primitive draws.
	void Draw(const GSHWDrawConfig& config);
	void Draw(const GSHWDrawConfig& config, int offset, int count);

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;
	void DoHintReadbackSource(GSTexture* tex) override;

	void DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, Filter filter) override;
	void DoDrawMultiStretchRects(
		const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader) override;
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTextureVK* dTex, ShaderConvertSelector shader);

	void BeginRenderPassForStretchRect(
		GSTextureVK* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard = true);
	void DoStretchRect(GSTextureVK* sTex, const GSVector4& sRect, GSTextureVK* dTex, const GSVector4& dRect,
		VkPipeline pipeline, Filter filter, bool allow_discard);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

	void BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex, const GSVector4i& dRect,
		u32 dLevel, Filter filter);

	void DoUpdateCLUTTexture(
		GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void DoConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
		GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void DoFilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTextureVK* SetupPrimitiveTrackingDATE(GSHWDrawConfig& config);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count, size_t align_multiplier = 1);
	void IASetIndexBuffer(const void* index, size_t count);
	void VSSetIndexBuffer(const void* index, size_t count);

	void PSSetROVs(GSTexture* rt, GSTexture* ds, bool write_rt, bool write_ds);
	void PSSetShaderResource(int i, GSTexture* sr, bool check_state, ResourceType type = ResourceType::SRV);
	void PSSetSampler(GSHWDrawConfig::SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor,
		FeedbackLoopFlag feedback_loop = FeedbackLoopFlag_None, const GSVector2i& viewport_size = {});

	void SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb);
	void SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb);
	void SetVSPushConstants(u32 base_vertex, u32 base_index = 0, bool force_update = false);
	bool BindDrawPipeline(const PipelineSelector& p);

	void DoRenderHW(GSHWDrawConfig& config) override;
	void UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe);
	void UploadHWDrawVerticesAndIndices(GSHWDrawConfig& config);
	VkImageMemoryBarrier GetColorBufferFeedbackBarrier(GSTextureVK* rt) const;
	VkImageMemoryBarrier GetDepthStencilBufferFeedbackBarrier(GSTextureVK* ds) const;
	VkDependencyFlags GetFeedbackBarrierDependencyFlags() const;
	void SendHWDraw(const GSHWDrawConfig& config, GSTextureVK* draw_rt, GSTextureVK* draw_ds,
		bool one_barrier, bool full_barrier);

	//////////////////////////////////////////////////////////////////////////
	// Vulkan State
	//////////////////////////////////////////////////////////////////////////

public:
	VkFormat LookupNativeFormat(GSTexture::Format format) const;

	__fi VkFramebuffer GetCurrentFramebuffer() const { return m_current_framebuffer; }

	/// Ends any render pass, executes the command buffer, and invalidates cached state.
	void ExecuteCommandBuffer(bool wait_for_completion);
	void ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...);
	void ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason);
	void ExecuteCommandBufferAndRestartPresent(bool wait_for_completion, const char* reason, ...);
	void ExecuteCommandBufferForReadback();

	/// Set dirty flags on everything to force re-bind at next draw time.
	void InvalidateCachedState();

	/// Binds all dirty state to the command buffer.
	bool ApplyUtilityState(bool already_execed = false);
	bool ApplyTFXState(bool already_execed = false);

	void SetIndexBuffer(VkBuffer buffer);
	void SetBlendConstants(u8 color);
	void SetLineWidth(float width);

	void SetUtilityTexture(GSTexture* tex, VkSampler sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void UnbindTexture(GSTextureVK* tex);

	// Ends a render pass if we're currently in one.
	// When Bind() is next called, the pass will be restarted.
	// Calling this function is allowed even if a pass has not begun.
	bool InRenderPass();
	void StampRenderTargetTouch();
	/// The frame's tile load-and-store bill, one pass at a time (GSPerfMon::RenderPassAreaPixels).
	void CountRenderPassArea(const GSVector4i& rect);
	void BeginRenderPass(VkRenderPass rp, const GSVector4i& rect);

	/// Begin recording out-of-band work (see OutOfBandResources). Returns VK_NULL_HANDLE
	/// if the facility is unavailable. Must be paired with SubmitOutOfBandAndWait().
	VkCommandBuffer BeginOutOfBandCommandBuffer();
	/// End, submit on its own fence, and wait for it. Returns false on a submit failure
	/// (the device is then in the usual last-submit-failed state).
	bool SubmitOutOfBandAndWait();

	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const VkClearValue* cv, u32 cv_count);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, u32 clear_color);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, float depth, u8 stencil);
	void EndRenderPass();

	void SetViewport(const VkViewport& viewport);
	void SetScissor(const GSVector4i& scissor);
	void InvalidateCachedViewportScissor();
	void SetPipeline(VkPipeline pipeline);

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_TFX_TEXTURE_0 = (1 << 0), // 0, 1, 2, 3, 4, 5, 6
		DIRTY_FLAG_TFX_UBO = (1 << 7),
		DIRTY_FLAG_UTILITY_TEXTURE = (1 << 8),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 9),
		DIRTY_FLAG_LINE_WIDTH = (1 << 10),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 11),
		DIRTY_FLAG_VIEWPORT = (1 << 12),
		DIRTY_FLAG_SCISSOR = (1 << 13),
		DIRTY_FLAG_PIPELINE = (1 << 14),
		DIRTY_FLAG_VS_CONSTANT_BUFFER = (1 << 15),
		DIRTY_FLAG_PS_CONSTANT_BUFFER = (1 << 16),
		DIRTY_FLAG_VS_PUSH_CONSTANTS = (1 << 17),

		DIRTY_FLAG_TFX_TEXTURE_TEX = (DIRTY_FLAG_TFX_TEXTURE_0 << 0),
		DIRTY_FLAG_TFX_TEXTURE_PALETTE = (DIRTY_FLAG_TFX_TEXTURE_0 << 1),
		DIRTY_FLAG_TFX_TEXTURE_RT = (DIRTY_FLAG_TFX_TEXTURE_0 << 2),
		DIRTY_FLAG_TFX_TEXTURE_PRIMID = (DIRTY_FLAG_TFX_TEXTURE_0 << 3),
		DIRTY_FLAG_TFX_TEXTURE_DEPTH = (DIRTY_FLAG_TFX_TEXTURE_0 << 4),
		DIRTY_FLAG_TFX_TEXTURE_RT_ROV = (DIRTY_FLAG_TFX_TEXTURE_0 << 5),
		DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV = (DIRTY_FLAG_TFX_TEXTURE_0 << 6),

		DIRTY_FLAG_TFX_TEXTURES = DIRTY_FLAG_TFX_TEXTURE_TEX | DIRTY_FLAG_TFX_TEXTURE_PALETTE |
		                          DIRTY_FLAG_TFX_TEXTURE_RT | DIRTY_FLAG_TFX_TEXTURE_PRIMID |
		                          DIRTY_FLAG_TFX_TEXTURE_DEPTH | DIRTY_FLAG_TFX_TEXTURE_RT_ROV |
		                          DIRTY_FLAG_TFX_TEXTURE_DEPTH_ROV,

		DIRTY_BASE_STATE = DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR |
		                   DIRTY_FLAG_BLEND_CONSTANTS | DIRTY_FLAG_LINE_WIDTH,
		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_TEXTURES,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE,
		DIRTY_CONSTANT_BUFFER_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER | DIRTY_FLAG_VS_PUSH_CONSTANTS,
		ALL_DIRTY_STATE = DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE,
	};

	enum class PipelineLayout
	{
		Undefined,
		TFX,
		Utility,
		TileExpand
	};

	void InitializeState();
	bool CreatePersistentDescriptorSets();

	void SetInitialState(VkCommandBuffer cmdbuf);
	void ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf);

	// Which bindings/state has to be updated before the next draw.
	u32 m_dirty_flags = 0;
	FeedbackLoopFlag m_current_framebuffer_feedback_loop = FeedbackLoopFlag_None;
	bool m_warned_slow_spin = false;

	VkBuffer m_index_buffer = VK_NULL_HANDLE;

	GSTextureVK* m_current_render_target = nullptr;
	GSTextureVK* m_current_depth_target = nullptr;
	VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
	VkRenderPass m_current_render_pass = VK_NULL_HANDLE;
	GSVector4i m_current_render_pass_area = GSVector4i::zero();

	// Mid-frame submission for readback-prone frames: when a game synchronously reads
	// GS memory back (local->host TRXDIR), the readback fence-waits on everything
	// recorded before it. Submitting accumulated work at render-pass boundaries lets
	// the GPU execute concurrently with GS-thread recording, so that wait finds the
	// work already complete (OutRun 2006 SD865: 3 sun-occlusion readbacks/frame cost
	// ~8ms/frame stalled without this).
	//
	// Two independent quantities, counted in different units on purpose:
	//   - the cadence, in render passes since the last submit, is how often we offer to
	//     kick. Uniform across a frame, so no part of a frame is favoured.
	//   - the arming window, in FRAMES since the last readback, is whether we bother at
	//     all. Its only job is "a game that never reads back sees zero change", so it has
	//     to decay in the unit the caller thinks in. It used to count render passes, which
	//     made one fixed budget mean wildly different things per title: 128 passes is
	//     ~3 frames of OutRun 2006 but only ~3/4 of a Rogue Galaxy frame, so RG had the
	//     kick switch itself off partway through every frame at nobody's request.
	// ~0u = no readback seen yet, window shut.
	u32 m_render_passes_since_submit = 0;
	u32 m_readback_frame = ~0u;

	// The TileGpu executor's kick arms off its OWN window, and this is why.
	//
	// m_readback_frame above is stamped in one place, ExecuteCommandBufferForReadback — the DRAIN
	// road. That is sound for Classic and exact-Tile, whose readbacks reach it, and self-defeating
	// for the TileGpu kick, whose entire job is to convert drains into out-of-band round trips. A
	// working kick drives the only thing that refreshes m_readback_frame to zero; three frames later
	// the window shuts, the kick stops, the drains come back, the window reopens. That oscillation
	// would not look like a bug from the outside — it would look like the lever doing about half of
	// what it should.
	//
	// So this one is refreshed by BOTH roads (SubmitOutOfBandAndWait as well as
	// ExecuteCommandBufferForReadback), and it is a separate field rather than a wider stamp on
	// m_readback_frame because that one is Classic's and exact-Tile's — both reach DoRenderHW — and
	// this must not move their kick cadence. Nothing outside ExecuteTileGpuPassPlan reads it.
	u32 m_tilegpu_readback_frame = ~0u;
	u64 m_tilegpu_kicks_offered = 0;
	u64 m_tilegpu_kicks_taken = 0;
	bool m_tilegpu_kick_announced = false;

	// The per-frame cadence predictor (EmuCore/GS/TileGpuAdaptiveKick) and the two things a frame
	// has to carry for it. The picker lives here rather than in the renderer because every input it
	// needs is a device counter and its one consumer is the kick lambda below.
	//
	// ⚠️ Unlike the construction-read TileGpu keys, the executor reads `on` ONCE PER FRAME, at the
	// head of ExecuteTileGpuPassPlan, and holds it for every pass of that frame. Re-reading it
	// mid-frame would be sound (a cadence is not a pass boundary and moves no pixel) but it would
	// make a frame's submission pattern depend on when in the frame the verdict flipped, which is
	// not a thing any run record could attribute.
	GSTileGpuKickPolicyPicker m_tilegpu_kick_picker;
	/// The blocking-wait total at the head of the previous plan, so this frame's wait is a delta.
	u64 m_tilegpu_kick_wait_mark = 0;
	/// Submits the CADENCE was the marginal trigger for this frame, and what they cost the GS
	/// thread -- offers included, because an offer's ScanForCommandBufferCompletion is a cost the
	/// cadence pays whether or not the fence gate lets the submit go.
	u32 m_tilegpu_kick_cadence_submits = 0;
	/// Render passes this plan opened -- the cadence's unit of work, and the counterfactual's divisor.
	u32 m_tilegpu_kick_frame_passes = 0;
	u64 m_tilegpu_kick_cadence_ns = 0;

	// Render passes opened for a Seed or SeedDepth op, and the merge's share of them. Counted at
	// the BeginRenderPass itself rather than off the op array, so a seed the executor declines
	// (no pipeline, an empty scissor) is not counted as a pass nobody paid for.
	u64 m_tilegpu_seed_render_passes = 0;
	u64 m_tilegpu_merge_seed_render_passes = 0;

	// One entry per prep op of the plan being executed: the ring word of that op's 512-entry
	// per-page block-mask table, or 0 for the ops that carry no table (every op but a batched merge
	// seed). A member rather than a local so the staging pass and the record pass agree on it
	// without either re-deriving a running offset the other's `continue` paths would break.
	std::vector<u32> m_tilegpu_seed_block_bases;

	// Textures recently used as synchronous-readback sources (see DoHintReadbackSource).
	// A draw INTO one of these is almost certainly the producer of the next readback,
	// so DoRenderHW kicks the command buffer first: the queued backlog drains while the
	// producing pass records, leaving the readback to wait on one small pass + copy
	// instead of the whole backlog. Compared by pointer only, never dereferenced —
	// a recycled allocation at worst causes one extra readback-window submit.
	std::array<GSTexture*, 2> m_recent_readback_sources = {};

	GSVector4i m_scissor = GSVector4i::zero();
	VkViewport m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	float m_current_line_width = 1.0f;
	u8 m_blend_constant_color = 0;

	std::array<GSTextureVK*, NUM_TFX_TEXTURES> m_tfx_textures{};
	VkSampler m_tfx_sampler = VK_NULL_HANDLE;
	u32 m_tfx_sampler_sel = 0;
	VkDescriptorSet m_tfx_ubo_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_texture_descriptor_set = VK_NULL_HANDLE;
	VkDescriptorSet m_tfx_rt_descriptor_set = VK_NULL_HANDLE;
	std::array<u32, NUM_TFX_DYNAMIC_OFFSETS> m_tfx_dynamic_offsets{};

	const GSTextureVK* m_utility_texture = nullptr;
	VkSampler m_utility_sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_utility_descriptor_set = VK_NULL_HANDLE;

	PipelineLayout m_current_pipeline_layout = PipelineLayout::Undefined;
	VkPipeline m_current_pipeline = VK_NULL_HANDLE;

	std::unique_ptr<GSTextureVK> m_null_texture;
	VkFramebuffer m_null_framebuffer;

	// current pipeline selector - we save this in the struct to avoid re-zeroing it every draw
	PipelineSelector m_pipeline_selector = {};
};
