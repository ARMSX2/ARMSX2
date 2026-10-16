// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSVramMirrorVK.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <string>

using namespace GSVramMirror;

namespace
{
	std::atomic<u64> s_next_generation{1};

	constexpr u32 kBindingCount = 5;
	constexpr u32 kDescriptorSets = 4; // the current set, and room for a second while buffers are replaced

	constexpr u32 AlignWords8(u32 words) { return (words + 7u) & ~7u; }

	/// The shader source for one entry point: the #version line first, the entry macro after it, and the
	/// generated swizzle file where the #include line is.
	std::optional<std::string> BuildShaderSource(bool decode)
	{
		std::optional<std::string> source = GSDevice::ReadShaderSource("shaders/vulkan/vram_mirror.glsl");
		std::optional<std::string> swizzle = GSDevice::ReadShaderSource("shaders/vulkan/vram_mirror_swizzle.glsl");
		if (!source.has_value() || !swizzle.has_value())
		{
			Console.Error("GS/Vulkan: vram_mirror.glsl or vram_mirror_swizzle.glsl is missing from the resources.");
			return std::nullopt;
		}

		const size_t version_end = source->find('\n');
		if (source->compare(0, 8, "#version") != 0 || version_end == std::string::npos)
		{
			Console.Error("GS/Vulkan: vram_mirror.glsl does not start with a #version line.");
			return std::nullopt;
		}
		source->insert(version_end + 1, decode ? "#define VRAM_MIRROR_DECODE 1\n" : "#define VRAM_MIRROR_SCATTER 1\n");

		const std::string marker = "#include \"vram_mirror_swizzle.glsl\"";
		const size_t at = source->find(marker);
		if (at == std::string::npos)
		{
			Console.Error("GS/Vulkan: vram_mirror.glsl does not include vram_mirror_swizzle.glsl.");
			return std::nullopt;
		}
		source->replace(at, marker.size(), *swizzle);
		return source;
	}
} // namespace

bool GSVramMirrorVK::IsSupportedPSM(u32 psm)
{
	Format f;
	return FormatFromPSM(psm, &f);
}

std::unique_ptr<GSVramMirrorVK> GSVramMirrorVK::Create()
{
	return Create(Config());
}

std::unique_ptr<GSVramMirrorVK> GSVramMirrorVK::Create(const Config& config)
{
	std::unique_ptr<GSVramMirrorVK> mirror(new GSVramMirrorVK(config));
	if (!mirror->Initialize())
		return nullptr;
	return mirror;
}

GSVramMirrorVK::GSVramMirrorVK(const Config& config)
	: m_config(config)
	, m_generation(s_next_generation.fetch_add(1))
{
}

GSVramMirrorVK::~GSVramMirrorVK()
{
	Destroy();
}

bool GSVramMirrorVK::Initialize()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	if (!dev || !g_vulkan_shader_cache)
		return false;

	if ((m_config.staging_slots & (m_config.staging_slots - 1)) != 0 || m_config.staging_slots < 16 || m_config.staging_slots > (1u << 24) ||
		m_config.scratch_bytes < (1u << 16) || m_config.meta_regions < 1 || m_config.meta_region_bytes < (1u << 14))
	{
		Console.Error("GS/Vulkan: VRAM mirror configuration is out of range.");
		return false;
	}

	const VkPhysicalDeviceLimits& limits = dev->GetDeviceProperties().limits;
	if (limits.maxPerStageDescriptorStorageBuffers < kBindingCount || limits.maxComputeWorkGroupInvocations < 64 ||
		limits.maxComputeWorkGroupSize[0] < 64 || limits.maxComputeWorkGroupCount[0] < kMaxWorkgroups ||
		limits.maxStorageBufferRange < m_config.scratch_bytes || limits.maxPushConstantsSize < 16)
	{
		Console.Error("GS/Vulkan: device limits are too low for the VRAM mirror decode.");
		return false;
	}

	m_slot_epoch.assign(kMirrorBlocks, 0);
	m_slot_of_block.assign(kMirrorBlocks, 0);
	m_seen_epoch.assign(kMirrorBlocks, 0);
	m_staged_blocks.reserve(kMirrorBlocks);
	m_region_fence.assign(m_config.meta_regions, 0);
	m_region_claimed.assign(m_config.meta_regions, 0);

	if (!CreateBuffers() || !CreatePipelines() || !CreateDescriptors())
	{
		Destroy();
		return false;
	}

	Console.WriteLn("GS/Vulkan: VRAM mirror ready (generation %llu): mirror 4 MiB, staging %u KiB%s, meta %u x %u KiB%s, "
					"scratch %u MiB.",
		static_cast<unsigned long long>(m_generation), m_config.staging_slots / 4, m_staging_coherent ? " coherent" : " NON-coherent",
		m_config.meta_regions, m_config.meta_region_bytes >> 10, m_meta_coherent ? " coherent" : " NON-coherent",
		m_config.scratch_bytes >> 20);
	return true;
}

bool GSVramMirrorVK::CreateHostBuffer(
	u32 size, const char* name, VkBuffer* buffer, VmaAllocation* allocation, u8** mapped, bool* coherent)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, static_cast<VkDeviceSize>(size),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};

	// The GPU reads these through a storage buffer after the CPU wrote them, which is the class of
	// memory the Mali tile-renderer work saw go stale unless it was HOST_COHERENT. So coherent is
	// required first; only if the driver has no such type for this buffer is any host-visible type
	// taken, and then every written range is flushed before the submit.
	VmaAllocationCreateInfo aci = {};
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VmaAllocationInfo ai = {};
	VkResult res = vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, buffer, allocation, &ai);
	if (res != VK_SUCCESS)
	{
		aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		res = vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, buffer, allocation, &ai);
	}
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vmaCreateBuffer(%s) failed: ", name);
		return false;
	}

	VkMemoryPropertyFlags flags = 0;
	vmaGetMemoryTypeProperties(dev->GetAllocator(), ai.memoryType, &flags);
	*coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	*mapped = static_cast<u8*>(ai.pMappedData);
	Vulkan::SetObjectName(dev->GetDevice(), *buffer, "%s", name);

	// Pay the first-touch page faults here rather than on the first frames, as the stream rings do.
	if (dev->GetDeviceProperties().deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		std::memset(ai.pMappedData, 0, ai.size);
		if (!*coherent)
			vmaFlushAllocation(dev->GetAllocator(), *allocation, 0, ai.size);
	}
	return true;
}

bool GSVramMirrorVK::CreateBuffers()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();

	const auto make_device_buffer = [&](u32 size, VkBufferUsageFlags usage, const char* name, VkBuffer* buffer,
										VmaAllocation* allocation) {
		const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, static_cast<VkDeviceSize>(size),
			usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		aci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		const VkResult res = vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, buffer, allocation, nullptr);
		if (res != VK_SUCCESS)
		{
			LOG_VULKAN_ERROR(res, "vmaCreateBuffer(%s) failed: ", name);
			return false;
		}
		Vulkan::SetObjectName(dev->GetDevice(), *buffer, "%s", name);
		return true;
	};

	return make_device_buffer(kMirrorBytes,
			   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			   "VRAM mirror", &m_mirror, &m_mirror_alloc) &&
	       make_device_buffer(m_config.scratch_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			   "VRAM mirror decode scratch", &m_scratch, &m_scratch_alloc) &&
	       CreateHostBuffer(m_config.staging_slots * kBlockBytes, "VRAM mirror staging", &m_staging, &m_staging_alloc,
			   &m_staging_ptr, &m_staging_coherent) &&
	       CreateHostBuffer(m_config.meta_regions * m_config.meta_region_bytes, "VRAM mirror meta", &m_meta, &m_meta_alloc,
			   &m_meta_ptr, &m_meta_coherent);
}

bool GSVramMirrorVK::CreatePipelines()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const VkDevice device = dev->GetDevice();

	Vulkan::DescriptorSetLayoutBuilder dslb;
	for (u32 binding = 0; binding < kBindingCount; binding++)
		dslb.AddBinding(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
	if ((m_set_layout = dslb.Create(device)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(device, m_set_layout, "VRAM mirror descriptor layout");

	Vulkan::PipelineLayoutBuilder plb;
	plb.AddDescriptorSet(m_set_layout);
	plb.AddPushConstants(VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(u32));
	if ((m_pipeline_layout = plb.Create(device)) == VK_NULL_HANDLE)
		return false;
	Vulkan::SetObjectName(device, m_pipeline_layout, "VRAM mirror pipeline layout");

	for (const bool decode : {true, false})
	{
		const std::optional<std::string> source = BuildShaderSource(decode);
		if (!source.has_value())
			return false;

		VkShaderModule module = g_vulkan_shader_cache->GetComputeShader(*source);
		if (module == VK_NULL_HANDLE)
		{
			Console.Error("GS/Vulkan: the VRAM mirror %s shader did not compile.", decode ? "decode" : "scatter");
			return false;
		}

		Vulkan::ComputePipelineBuilder cpb;
		cpb.SetPipelineLayout(m_pipeline_layout);
		cpb.SetShader(module, "main");
		VkPipeline pipeline = cpb.Create(device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		vkDestroyShaderModule(device, module, nullptr);
		if (pipeline == VK_NULL_HANDLE)
		{
			Console.Error("GS/Vulkan: the VRAM mirror %s pipeline could not be created.", decode ? "decode" : "scatter");
			return false;
		}
		Vulkan::SetObjectName(device, pipeline, decode ? "VRAM mirror decode" : "VRAM mirror scatter");
		(decode ? m_decode_pipeline : m_scatter_pipeline) = pipeline;
	}
	return true;
}

bool GSVramMirrorVK::CreateDescriptors()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const VkDevice device = dev->GetDevice();

	const VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindingCount * kDescriptorSets};
	const VkDescriptorPoolCreateInfo pci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kDescriptorSets, 1, &pool_size};
	VkResult res = vkCreateDescriptorPool(device, &pci, nullptr, &m_pool);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool(VRAM mirror) failed: ");
		return false;
	}

	const VkDescriptorSetAllocateInfo allocate = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_pool, 1, &m_set_layout};
	res = vkAllocateDescriptorSets(device, &allocate, &m_set);
	if (res != VK_SUCCESS)
	{
		// Checked, and fatal for the mirror: the caller keeps the CPU path. Never a silently skipped dispatch.
		LOG_VULKAN_ERROR(res, "vkAllocateDescriptorSets(VRAM mirror) failed: ");
		m_set = VK_NULL_HANDLE;
		return false;
	}

	// 3 and 4 are the same buffer, bound twice so the shader can store 16 bytes at a time (PSMT8, PSMT4)
	// or 8 bytes at a time (the H formats). Two descriptors may alias a buffer, and no invocation writes
	// the same byte through both.
	const VkDescriptorBufferInfo infos[kBindingCount] = {
		{m_mirror, 0, VK_WHOLE_SIZE},
		{m_staging, 0, VK_WHOLE_SIZE},
		{m_meta, 0, VK_WHOLE_SIZE},
		{m_scratch, 0, VK_WHOLE_SIZE},
		{m_scratch, 0, VK_WHOLE_SIZE},
	};
	VkWriteDescriptorSet writes[kBindingCount];
	for (u32 i = 0; i < kBindingCount; i++)
	{
		writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_set, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			nullptr, &infos[i], nullptr};
	}
	vkUpdateDescriptorSets(device, kBindingCount, writes, 0, nullptr);

	if (dev->IsGPUTimingSupported())
	{
		const VkQueryPoolCreateInfo qci = {
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, kQueryCount, 0};
		if (vkCreateQueryPool(device, &qci, nullptr, &m_query_pool) != VK_SUCCESS)
			m_query_pool = VK_NULL_HANDLE;
	}
	return true;
}

void GSVramMirrorVK::Destroy()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	if (!dev || dev->GetDevice() == VK_NULL_HANDLE)
		return;

	// A flush recorded into the command buffer being built has not reached the GPU, and the buffers it
	// names must outlive it. Anything older only needs to have finished.
	if (m_last_flush_fence != 0)
	{
		if (m_last_flush_fence >= dev->GetCurrentFenceCounter())
			dev->ExecuteCommandBuffer(true);
		else
			dev->WaitForFenceCounter(m_last_flush_fence);
	}

	const VkDevice device = dev->GetDevice();
	const VmaAllocator allocator = dev->GetAllocator();
	if (m_query_pool != VK_NULL_HANDLE)
		vkDestroyQueryPool(device, m_query_pool, nullptr);
	if (m_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(device, m_pool, nullptr); // frees its set
	if (m_decode_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(device, m_decode_pipeline, nullptr);
	if (m_scatter_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(device, m_scatter_pipeline, nullptr);
	if (m_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, m_pipeline_layout, nullptr);
	if (m_set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, m_set_layout, nullptr);
	for (auto [buffer, allocation] : {std::pair{m_mirror, m_mirror_alloc}, std::pair{m_scratch, m_scratch_alloc},
			 std::pair{m_staging, m_staging_alloc}, std::pair{m_meta, m_meta_alloc}})
	{
		if (buffer != VK_NULL_HANDLE)
			vmaDestroyBuffer(allocator, buffer, allocation);
	}
	m_query_pool = VK_NULL_HANDLE;
	m_pool = VK_NULL_HANDLE;
	m_set = VK_NULL_HANDLE;
	m_decode_pipeline = m_scatter_pipeline = VK_NULL_HANDLE;
	m_pipeline_layout = VK_NULL_HANDLE;
	m_set_layout = VK_NULL_HANDLE;
	m_mirror = m_scratch = m_staging = m_meta = VK_NULL_HANDLE;
}

void GSVramMirrorVK::ForgetMemory()
{
	m_synced_memory_id = 0;
}

void GSVramMirrorVK::SyncTo(const Source& source)
{
	if (m_synced_memory_id == source.memory_id && m_synced_generation == m_generation)
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
	m_synced_generation = m_generation;
	m_stats.resyncs++;
}

bool GSVramMirrorVK::ClaimRegion()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	for (int attempt = 0; attempt < 2; attempt++)
	{
		const u64 completed = dev->GetCompletedFenceCounter();
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
			dev->ScanForCommandBufferCompletion();
	}

	// Every region is in use. One whose flush was already submitted will come free, so wait for it.
	// Regions held by the command buffer still being built cannot, and that is a submit.
	u32 pick = kNoRegion;
	for (u32 i = 0; i < m_config.meta_regions; i++)
	{
		if (!m_region_claimed[i] && m_region_fence[i] < dev->GetCurrentFenceCounter() &&
			(pick == kNoRegion || m_region_fence[i] < m_region_fence[pick]))
			pick = i;
	}
	if (pick == kNoRegion)
		return false;
	dev->WaitForFenceCounter(m_region_fence[pick]);
	m_region_claimed[pick] = 1;
	m_batch_region = pick;
	m_table_words = 0;
	return true;
}

void GSVramMirrorVK::ReclaimStaging()
{
	const u64 completed = GSDeviceVK::GetInstance()->GetCompletedFenceCounter();
	while (!m_stage_marks.empty() && m_stage_marks.front().first <= completed)
	{
		m_stage_tail = m_stage_marks.front().second;
		m_stage_marks.pop_front();
	}
}

int GSVramMirrorVK::EnsureStagingSpace(u32 slots_needed)
{
	if (slots_needed == 0)
		return 0;
	if (slots_needed > m_config.staging_slots)
		return 2;

	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	ReclaimStaging();
	while (m_config.staging_slots - (m_stage_head - m_stage_tail) < slots_needed)
	{
		// Slots come back when the GPU finishes the command buffer whose flush read them. A flush in the
		// buffer still being built has not been submitted, and waiting for it would wait forever.
		if (m_stage_marks.empty() || m_stage_marks.front().first >= dev->GetCurrentFenceCounter())
			return 1;
		dev->WaitForFenceCounter(m_stage_marks.front().first);
		ReclaimStaging();
	}
	return 0;
}

bool GSVramMirrorVK::FitsBatch(const WindowLayout& layout, u32 new_staged_upper) const
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

void GSVramMirrorVK::ResetBatch()
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

GSVramMirrorVK::Result GSVramMirrorVK::DecodeWindow(const Request& rq)
{
	if (!rq.off || !rq.texture || !rq.source.vm || !rq.source.dirty || m_mirror == VK_NULL_HANDLE)
		return Result::Unsupported;

	WindowLayout layout;
	if (!ComputeLayout(*rq.off, rq.rect, &layout))
		return Result::Unsupported;

	// The texture is written by a buffer-to-image copy of exactly the window.
	GSTextureVK* tex = rq.texture;
	if (!tex->IsTexture() || tex->GetFormat() != GSTexture::Format::UNorm8 || tex->GetMipmapLevels() != 1 ||
		tex->GetWidth() != rq.rect.width() || tex->GetHeight() != rq.rect.height())
		return Result::Unsupported;

	// A window that cannot fit even an empty batch goes to the CPU.
	if (layout.scratch_bytes > m_config.scratch_bytes)
		return Result::Unsupported;

	SyncTo(rq.source);

	BuildBlockNumbers(*rq.off, layout, &m_block_numbers);

	// How many distinct blocks of the window the mirror has not seen: the staging slots it needs.
	if (++m_seen_stamp == 0)
	{
		std::fill(m_seen_epoch.begin(), m_seen_epoch.end(), 0);
		m_seen_stamp = 1;
	}
	u32 need = 0;
	for (const u32 bn : m_block_numbers)
	{
		if (((rq.source.dirty[bn >> 6] >> (bn & 63)) & 1) && m_seen_epoch[bn] != m_seen_stamp)
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
		Flush();
		m_stats.scratch_flushes++;
		if (!FitsBatch(layout, need))
			return Result::Unsupported;
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

	// Stage what is new and write the block table. A block the batch already staged and nobody has
	// written since reads that staged copy; one written since gets a fresh slot, and the windows
	// that already point at the older slot keep their snapshot.
	const u32 region_base_words = m_batch_region * (m_config.meta_region_bytes / 4);
	u32* const table = reinterpret_cast<u32*>(m_meta_ptr) + region_base_words + m_table_words;
	const u32 slot_mask = m_config.staging_slots - 1;
	u32 staged_here = 0, from_staging = 0, restaged = 0;
	for (u32 i = 0; i < layout.BlockCount(); i++)
	{
		const u32 bn = m_block_numbers[i];
		u64& word = rq.source.dirty[bn >> 6];
		u32 entry;
		if ((word >> (bn & 63)) & 1)
		{
			const u32 slot = static_cast<u32>(m_stage_head++) & slot_mask;
			std::memcpy(m_staging_ptr + static_cast<size_t>(slot) * kBlockBytes, rq.source.vm + static_cast<size_t>(bn) * kBlockBytes,
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
	job.texture = tex;
	job.layout = layout;
	job.table_word = region_base_words + m_table_words;
	job.out_offset = (m_scratch_used + 255u) & ~255u;
	job.first_wg = m_wg_total;
	job.wg_count = (layout.UnitCount() + 63) / 64;
	m_jobs.push_back(job);

	m_table_words += layout.BlockCount();
	m_wg_total += job.wg_count;
	m_scratch_used = job.out_offset + layout.scratch_bytes;

	// Where the image will be once the flush has run, which is where the draw buffer finds it.
	tex->OverrideImageLayout(GSTextureVK::Layout::ShaderReadOnly);

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

void GSVramMirrorVK::CancelTexture(const GSTextureVK* texture)
{
	for (Job& job : m_jobs)
	{
		if (job.texture == texture)
		{
			job.texture = nullptr;
			m_stats.cancelled++;
		}
	}
}

bool GSVramMirrorVK::EnableTiming(bool enable)
{
	if (enable && m_query_pool == VK_NULL_HANDLE)
		return false;
	m_timing = enable;
	return true;
}

bool GSVramMirrorVK::ReadTimings(Timings* out)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	if (!m_timing_pending || m_query_pool == VK_NULL_HANDLE || m_timing_fence > dev->GetCompletedFenceCounter())
		return false;

	u64 t[kQueryCount];
	const VkResult res = vkGetQueryPoolResults(dev->GetDevice(), m_query_pool, 0, kQueryCount, sizeof(t), t, sizeof(u64),
		VK_QUERY_RESULT_64_BIT);
	if (res != VK_SUCCESS)
		return false;

	const double ns = dev->GetDeviceProperties().limits.timestampPeriod;
	out->decode_ms = (t[1] - t[0]) * ns * 1e-6;
	out->barrier_ms = (t[2] - t[1]) * ns * 1e-6;
	out->scatter_ms = (t[3] - t[2]) * ns * 1e-6;
	out->copy_ms = (t[4] - t[3]) * ns * 1e-6;
	out->decoded_bytes = m_timing_decoded;
	out->scattered_bytes = m_timing_scattered;
	return true;
}

void GSVramMirrorVK::Flush()
{
	if (!HasPending())
		return;

	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const VkCommandBuffer cb = dev->GetCurrentInitCommandBuffer();
	const u32 region_words = m_config.meta_region_bytes / 4;
	const u32 region_base = m_batch_region * region_words;
	u32* const meta = reinterpret_cast<u32*>(m_meta_ptr) + region_base;

	// Everything that follows the block tables: the job records, the workgroup -> job list, and the
	// (block, slot) pairs the scatter copies. FitsBatch reserved the room.
	u32 w = AlignWords8(m_table_words);
	const u32 jobs_word = region_base + w;
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
	}
	w += static_cast<u32>(m_jobs.size()) * 8;
	w = AlignWords8(w);
	const u32 wg_word = region_base + w;
	for (size_t j = 0; j < m_jobs.size(); j++)
		for (u32 g = 0; g < m_jobs[j].wg_count; g++)
			meta[w + m_jobs[j].first_wg + g] = static_cast<u32>(j);
	w += m_wg_total;
	w = AlignWords8(w);
	const u32 scatter_word = region_base + w;
	for (size_t i = 0; i < m_staged_blocks.size(); i++)
	{
		meta[w + i * 2] = m_staged_blocks[i];
		meta[w + i * 2 + 1] = m_slot_of_block[m_staged_blocks[i]];
	}
	w += static_cast<u32>(m_staged_blocks.size()) * 2;
	pxAssert(w <= region_words);

	const VmaAllocator allocator = dev->GetAllocator();
	if (!m_meta_coherent)
		vmaFlushAllocation(allocator, m_meta_alloc, static_cast<VkDeviceSize>(region_base) * 4, static_cast<VkDeviceSize>(w) * 4);
	if (!m_staging_coherent && m_stage_head != m_stage_flushed)
	{
		// The slots written since the last flush: one range, or two if it wrapped.
		const u64 count = m_stage_head - m_stage_flushed;
		const u32 first = static_cast<u32>(m_stage_flushed) & (m_config.staging_slots - 1);
		const u32 head_part = static_cast<u32>(std::min<u64>(count, m_config.staging_slots - first));
		vmaFlushAllocation(allocator, m_staging_alloc, static_cast<VkDeviceSize>(first) * kBlockBytes,
			static_cast<VkDeviceSize>(head_part) * kBlockBytes);
		if (count > head_part)
			vmaFlushAllocation(allocator, m_staging_alloc, 0, static_cast<VkDeviceSize>(count - head_part) * kBlockBytes);
	}
	m_stage_flushed = m_stage_head;

	// Timestamps 0-3 bracket the compute work and use the compute stage. malisx2 (a job-manager Mali) puts a
	// timestamp on the fragment job chain unless its stage is exclusively vertex/tiler/compute, and a
	// fragment-chain stamp is written without waiting for the dispatches: with bottom-of-pipe stamps the
	// G57 timed none of the decode, with compute-stage ones the decode time agrees with the wall time. The
	// last stamp follows the copies, which are not compute work, and closes the flush. Honeykrisp separates
	// the phases with neither stage (only the copies show in its timestamps), so wall time is the measure
	// there.
	const bool timed = m_timing && m_query_pool != VK_NULL_HANDLE;
	if (timed)
		vkCmdResetQueryPool(cb, m_query_pool, 0, kQueryCount);

	// Barrier A: the previous flush's scatter writes to the mirror are visible to this flush's decode
	// reads (compute -> compute), and the previous flush's copy reads of scratch are finished before
	// this decode writes it (transfer -> compute).
	{
		const VkMemoryBarrier a = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
			VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &a, 0, nullptr, 0, nullptr);
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, 0);

	// One decode dispatch for every pending window.
	u64 decoded_bytes = 0;
	for (const Job& job : m_jobs)
		decoded_bytes += job.layout.scratch_bytes;
	if (!m_jobs.empty())
	{
		const u32 push[4] = {jobs_word, wg_word, m_config.staging_slots - 1, 0};
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_decode_pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_set, 0, nullptr);
		vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, m_wg_total, 1, 1);
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, 1);

	// Barrier B (its own call): scratch, compute write -> transfer read; mirror, the decode's compute
	// read -> the scatter's compute write.
	{
		const VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	// Barrier C (a separate call, and not merged into B): Undefined -> TransferDst for every image. One
	// vkCmdPipelineBarrier has one source stage, and a pooled image may still be sampled by the fragment
	// shaders of an earlier submission; with a compute-only source the copy would race those reads.
	// The helper behind DoUpdate gets this from mapping Undefined to BOTTOM_OF_PIPE.
	std::vector<VkImageMemoryBarrier> to_dst;
	std::vector<VkImageMemoryBarrier> to_read;
	to_dst.reserve(m_jobs.size());
	to_read.reserve(m_jobs.size());
	for (const Job& job : m_jobs)
	{
		if (!job.texture)
			continue;
		const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		to_dst.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			job.texture->GetImage(), range});
		to_read.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, job.texture->GetImage(), range});
	}
	if (!to_dst.empty())
	{
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, static_cast<u32>(to_dst.size()), to_dst.data());
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, 2);

	// The scatter: the newest staged version of each block into the mirror, after the decode read it.
	const u32 pairs = static_cast<u32>(m_staged_blocks.size());
	if (pairs != 0)
	{
		const u32 push[4] = {scatter_word, 0, m_config.staging_slots - 1, pairs};
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_scatter_pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_set, 0, nullptr);
		vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, (pairs + 3) / 4, 1, 1);
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, 3);

	// The copies: today's buffer-to-image copy, from the scratch buffer. The window starts mid-block in
	// general, so the source offset is not aligned to anything; only the destination (0, 0) is.
	for (const Job& job : m_jobs)
	{
		if (!job.texture)
			continue;
		const WindowLayout& l = job.layout;
		const VkBufferImageCopy region = {static_cast<VkDeviceSize>(job.out_offset) + l.CopyOffset(), l.pitch, 0,
			{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
			{static_cast<u32>(l.rect.width()), static_cast<u32>(l.rect.height()), 1}};
		vkCmdCopyBufferToImage(cb, m_scratch, job.texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	// Barrier D (one call): the images, TransferDst -> ShaderReadOnly, transfer -> fragment shader; and
	// the mirror, this flush's scatter write -> the next flush's decode read.
	{
		const VkMemoryBarrier d = {
			VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &d, 0, nullptr,
			static_cast<u32>(to_read.size()), to_read.data());
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pool, 4);

	const u64 fence = dev->GetCurrentFenceCounter();
	if (timed)
	{
		m_timing_pending = true;
		m_timing_fence = fence;
		m_timing_decoded = decoded_bytes;
		m_timing_scattered = static_cast<u64>(pairs) * kBlockBytes;
	}
	m_last_flush_fence = fence;
	m_region_fence[m_batch_region] = fence;
	m_region_claimed[m_batch_region] = 0;
	m_batch_region = kNoRegion;
	m_stage_marks.emplace_back(fence, m_stage_head);

	m_stats.flushes++;
	m_stats.scatter_blocks += pairs;
	ResetBatch();
}

bool GSVramMirrorVK::DebugReadMirror(std::vector<u8>* out)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	Flush();

	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, kMirrorBytes,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
	VmaAllocationCreateInfo aci = {};
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	VkBuffer buffer;
	VmaAllocation allocation;
	VmaAllocationInfo ai;
	if (vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, &buffer, &allocation, &ai) != VK_SUCCESS)
		return false;

	const VkCommandBuffer cb = dev->GetCurrentInitCommandBuffer();
	const VkMemoryBarrier to_transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0,
		nullptr, 0, nullptr);
	const VkBufferCopy copy = {0, 0, kMirrorBytes};
	vkCmdCopyBuffer(cb, m_mirror, buffer, 1, &copy);
	const VkMemoryBarrier to_host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
	dev->ExecuteCommandBuffer(true);

	vmaInvalidateAllocation(dev->GetAllocator(), allocation, 0, kMirrorBytes);
	out->assign(static_cast<const u8*>(ai.pMappedData), static_cast<const u8*>(ai.pMappedData) + kMirrorBytes);
	vmaDestroyBuffer(dev->GetAllocator(), buffer, allocation);
	return true;
}
