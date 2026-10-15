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

	std::atomic<bool> s_verify{false};

	// The verify report, process-wide so it can be read after the device is gone.
	struct VerifyCounters
	{
		std::atomic<u64> windows{0}, texels{0}, bad_windows{0}, bad_texels{0};
		std::atomic<u64> flushes{0}, mirror_blocks{0}, mirror_bad_blocks{0};
		std::atomic<u64> mark_blocks{0}, mark_stale{0}, mark_unseen{0}, mark_sweeps{0}, mark_window_blocks{0};
		std::atomic<u32> printed{0};
	};
	VerifyCounters s_vc;

	/// What the CPU readers make of the window right now: the R8 index bytes PreloadTexture would upload.
	std::vector<u8> CpuDecodeWindow(GSLocalMemory& mem, const GSOffset& off, const GSVector4i& rect)
	{
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[off.psm()];
		const GSVector4i block_rect = rect.ralign<Align_Outside>(psm.bs);
		const u32 pitch = (static_cast<u32>(block_rect.width()) + 31u) & ~31u;
		std::vector<u8> raw(static_cast<size_t>(pitch) * block_rect.height() + 64);
		u8* const aligned = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(raw.data()) + 31) & ~uintptr_t(31));
		const GIFRegTEXA texa = {};
		psm.rtxP(mem, off, block_rect, aligned, static_cast<int>(pitch), texa);

		const int w = rect.width(), h = rect.height();
		std::vector<u8> out(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++)
			std::memcpy(&out[static_cast<size_t>(y) * w],
				aligned + static_cast<size_t>(pitch) * (rect.top - block_rect.top + y) + (rect.left - block_rect.left), w);
		return out;
	}

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

GSVramMirrorVK::Source GSVramMirrorVK::SourceOf(GSLocalMemory& mem)
{
	return {mem.vm8(), mem.Id(), mem.BlockDirtyWords(), &mem};
}

void GSVramMirrorVK::SetVerify(bool enable)
{
	s_verify.store(enable);
}

bool GSVramMirrorVK::VerifyEnabled()
{
	return s_verify.load();
}

GSVramMirrorVK::VerifyReport GSVramMirrorVK::GetVerifyReport()
{
	VerifyReport r;
	r.windows = s_vc.windows;
	r.texels = s_vc.texels;
	r.bad_windows = s_vc.bad_windows;
	r.bad_texels = s_vc.bad_texels;
	r.flushes = s_vc.flushes;
	r.mirror_blocks = s_vc.mirror_blocks;
	r.mirror_bad_blocks = s_vc.mirror_bad_blocks;
	r.mark_blocks = s_vc.mark_blocks;
	r.mark_stale = s_vc.mark_stale;
	r.mark_unseen = s_vc.mark_unseen;
	r.mark_sweeps = s_vc.mark_sweeps;
	r.mark_window_blocks = s_vc.mark_window_blocks;
	return r;
}

u64 GSVramMirrorVK::DeviceFences::Completed()
{
	return GSDeviceVK::GetInstance()->GetCompletedFenceCounter();
}

u64 GSVramMirrorVK::DeviceFences::Current()
{
	return GSDeviceVK::GetInstance()->GetCurrentFenceCounter();
}

void GSVramMirrorVK::DeviceFences::Poll()
{
	GSDeviceVK::GetInstance()->ScanForCommandBufferCompletion();
}

void GSVramMirrorVK::DeviceFences::Wait(u64 counter)
{
	GSDeviceVK::GetInstance()->WaitForFenceCounter(counter);
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
		limits.maxComputeWorkGroupSize[0] < 64 || limits.maxComputeWorkGroupCount[0] < Batch::kMaxWorkgroups ||
		limits.maxStorageBufferRange < m_config.scratch_bytes || limits.maxPushConstantsSize < 16)
	{
		Console.Error("GS/Vulkan: device limits are too low for the VRAM mirror decode.");
		return false;
	}

	if (!CreateBuffers() || !CreatePipelines() || !CreateDescriptors())
	{
		Destroy();
		return false;
	}

	m_batch = std::make_unique<Batch>(m_config, m_staging_ptr, reinterpret_cast<u32*>(m_meta_ptr), &m_fences);
	m_verify = s_verify.load();
	if (m_verify)
		m_batch->EnableShadow();

	Console.WriteLn("GS/Vulkan: VRAM mirror ready (generation %llu): mirror 4 MiB, staging %u KiB%s, meta %u x %u KiB%s, "
					"scratch %u MiB%s.",
		static_cast<unsigned long long>(m_generation), m_config.staging_slots / 4, m_staging_coherent ? " coherent" : " NON-coherent",
		m_config.meta_regions, m_config.meta_region_bytes >> 10, m_meta_coherent ? " coherent" : " NON-coherent",
		m_config.scratch_bytes >> 20, m_verify ? ", verify mode" : "");
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
			VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, kQueryCount * kTimingSets, 0};
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

	// Every readback in flight is compared before the buffers go; the report outlives the mirror.
	if (m_verify)
		VerifyDrain();

	// A decode still queued belongs to a texture that outlives this object: it is not finished, and nobody
	// is going to flush it. The texture is left as it is (its image was never written); only the flag goes,
	// so its destructor does not call back into a mirror that is gone.
	if (m_batch)
	{
		for (const Batch::Job& job : m_batch->Jobs())
		{
			if (job.texture)
				static_cast<GSTextureVK*>(job.texture)->SetDecodePending(false);
		}
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
	m_batch->ForgetMemory();
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

	// The decode goes into the init command buffer, ahead of the frame's draws. A texture that a draw or a
	// copy of the command buffer being built has already used would have its update recorded into the draw
	// buffer (GSTextureVK::GetCommandBufferForUpdate), where the mirror cannot be read in order; and one
	// already queued has a decode in flight that this would race.
	if (tex->IsDecodePending() || tex->GetUseFenceCounter() == GSDeviceVK::GetInstance()->GetCurrentFenceCounter())
		return Result::Unsupported;

	if (m_verify)
		VerifyPoll(false);

	Batch::Result r = m_batch->Queue(*rq.off, layout, rq.source, m_generation, tex);
	if (r == Batch::Result::NeedFlush)
	{
		Flush();
		m_batch->CountScratchFlush();
		r = m_batch->Queue(*rq.off, layout, rq.source, m_generation, tex);
	}
	switch (r)
	{
		case Batch::Result::Queued:
			break;
		case Batch::Result::NeedSubmit:
			return Result::NeedSubmit;
		default:
			return Result::Unsupported;
	}

	// Where the image will be once the flush has run, which is where the draw buffer finds it.
	tex->OverrideImageLayout(GSTextureVK::Layout::ShaderReadOnly);
	tex->SetDecodePending(true);

	if (m_verify)
	{
		// What the CPU readers make of the window right now, to compare with the texture once it is decoded.
		// (A caller without a memory to read, like a test that models one, gets the other checks only.)
		m_verify_expected.resize(m_batch->Jobs().size());
		if (rq.source.mem)
			m_verify_expected.back() = CpuDecodeWindow(*rq.source.mem, *rq.off, rq.rect);
	}
	return Result::Queued;
}

void GSVramMirrorVK::CancelTexture(GSTextureVK* texture)
{
	if (!m_batch)
		return;
	const auto& jobs = m_batch->Jobs();
	for (size_t j = 0; j < jobs.size(); j++)
	{
		if (jobs[j].texture == texture && j < m_verify_expected.size())
			m_verify_expected[j].clear();
	}
	m_batch->Cancel(texture);
	texture->SetDecodePending(false);
}

bool GSVramMirrorVK::EnableTiming(bool enable)
{
	if (enable && m_query_pool == VK_NULL_HANDLE)
		return false;
	m_timing = enable;
	return true;
}

void GSVramMirrorVK::PollTimings()
{
	if (m_query_pool == VK_NULL_HANDLE)
		return;

	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const double ns = dev->GetDeviceProperties().limits.timestampPeriod;
	for (u32 i = 0; i < kTimingSets; i++)
	{
		TimingSet& ts = m_timing_sets[i];
		if (!ts.pending || ts.fence > dev->GetCompletedFenceCounter())
			continue;

		u64 t[kQueryCount];
		const VkResult res = vkGetQueryPoolResults(dev->GetDevice(), m_query_pool, i * kQueryCount, kQueryCount, sizeof(t), t,
			sizeof(u64), VK_QUERY_RESULT_64_BIT);
		if (res != VK_SUCCESS)
			continue;
		ts.pending = false;

		Timings tm;
		tm.decode_ms = (t[1] - t[0]) * ns * 1e-6;
		tm.barrier_ms = (t[2] - t[1]) * ns * 1e-6;
		tm.scatter_ms = (t[3] - t[2]) * ns * 1e-6;
		tm.copy_ms = (t[4] - t[3]) * ns * 1e-6;
		tm.decoded_bytes = ts.decoded;
		tm.scattered_bytes = ts.scattered;

		m_timing_totals.flushes++;
		m_timing_totals.decode_ms += tm.decode_ms;
		m_timing_totals.barrier_ms += tm.barrier_ms;
		m_timing_totals.scatter_ms += tm.scatter_ms;
		m_timing_totals.copy_ms += tm.copy_ms;
		m_timing_totals.decoded_bytes += tm.decoded_bytes;
		m_timing_totals.scattered_bytes += tm.scattered_bytes;
		if (!m_have_last_timings || ts.fence >= m_last_timings_fence)
		{
			m_last_timings = tm;
			m_last_timings_fence = ts.fence;
			m_have_last_timings = true;
		}
	}
}

bool GSVramMirrorVK::ReadTimings(Timings* out)
{
	PollTimings();
	if (!m_have_last_timings)
		return false;
	*out = m_last_timings;
	return true;
}

GSVramMirrorVK::TimingTotals GSVramMirrorVK::GetTimingTotals()
{
	PollTimings();
	return m_timing_totals;
}

void GSVramMirrorVK::Flush()
{
	if (!m_batch->HasPending())
		return;

	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	const VkCommandBuffer cb = dev->GetCurrentInitCommandBuffer();
	const Batch::FlushPlan plan = m_batch->PrepareFlush();
	const std::vector<Batch::Job>& jobs = m_batch->Jobs();

	const VmaAllocator allocator = dev->GetAllocator();
	if (!m_meta_coherent)
	{
		vmaFlushAllocation(allocator, m_meta_alloc, static_cast<VkDeviceSize>(plan.region_base_word) * 4,
			static_cast<VkDeviceSize>(plan.words_used) * 4);
	}
	if (!m_staging_coherent && plan.staged_end != plan.staged_first)
	{
		// The slots written since the last flush: one range, or two if it wrapped.
		const u64 count = plan.staged_end - plan.staged_first;
		const u32 first = static_cast<u32>(plan.staged_first) & (m_config.staging_slots - 1);
		const u32 head_part = static_cast<u32>(std::min<u64>(count, m_config.staging_slots - first));
		vmaFlushAllocation(allocator, m_staging_alloc, static_cast<VkDeviceSize>(first) * kBlockBytes,
			static_cast<VkDeviceSize>(head_part) * kBlockBytes);
		if (count > head_part)
			vmaFlushAllocation(allocator, m_staging_alloc, 0, static_cast<VkDeviceSize>(count - head_part) * kBlockBytes);
	}

	// Timestamps 0-3 bracket the compute work and use the compute stage. malisx2 (a job-manager Mali) puts a
	// timestamp on the fragment job chain unless its stage is exclusively vertex/tiler/compute, and a
	// fragment-chain stamp is written without waiting for the dispatches: with bottom-of-pipe stamps the
	// G57 timed none of the decode, with compute-stage ones the decode time agrees with the wall time. The
	// last stamp follows the copies, which are not compute work, and closes the flush. Honeykrisp separates
	// the phases with neither stage (only the copies show in its timestamps), so wall time is the measure
	// there. Each flush in flight has a query set of its own.
	bool timed = m_timing && m_query_pool != VK_NULL_HANDLE;
	u32 qbase = 0;
	if (timed)
	{
		if (m_timing_sets[m_timing_next].pending)
			PollTimings();
		if (m_timing_sets[m_timing_next].pending)
			timed = false; // every set is still in flight; skip this flush rather than overwrite one
	}
	if (timed)
	{
		qbase = m_timing_next * kQueryCount;
		vkCmdResetQueryPool(cb, m_query_pool, qbase, kQueryCount);
	}

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
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, qbase + 0);

	// One decode dispatch for every pending window.
	if (plan.job_count != 0)
	{
		const u32 push[4] = {plan.jobs_word, plan.wg_word, m_config.staging_slots - 1, 0};
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_decode_pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_set, 0, nullptr);
		vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, plan.wg_total, 1, 1);
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, qbase + 1);

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
	to_dst.reserve(jobs.size());
	to_read.reserve(jobs.size());
	for (const Batch::Job& job : jobs)
	{
		if (!job.texture)
			continue;
		const VkImage image = static_cast<GSTextureVK*>(job.texture)->GetImage();
		const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		to_dst.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			image, range});
		to_read.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range});
	}
	if (!to_dst.empty())
	{
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			nullptr, static_cast<u32>(to_dst.size()), to_dst.data());
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, qbase + 2);

	// The scatter: the newest staged version of each block into the mirror, after the decode read it. It
	// also takes the staged blocks of windows whose texture was destroyed before this flush.
	if (plan.scatter_pairs != 0)
	{
		const u32 push[4] = {plan.scatter_word, 0, m_config.staging_slots - 1, plan.scatter_pairs};
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_scatter_pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_set, 0, nullptr);
		vkCmdPushConstants(cb, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
		vkCmdDispatch(cb, (plan.scatter_pairs + 3) / 4, 1, 1);
	}
	if (timed)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool, qbase + 3);

	// The copies: today's buffer-to-image copy, from the scratch buffer. The window starts mid-block in
	// general, so the source offset is not aligned to anything; only the destination (0, 0) is.
	for (const Batch::Job& job : jobs)
	{
		if (!job.texture)
			continue;
		const WindowLayout& l = job.layout;
		const VkBufferImageCopy region = {static_cast<VkDeviceSize>(job.out_offset) + l.CopyOffset(), l.pitch, 0,
			{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
			{static_cast<u32>(l.rect.width()), static_cast<u32>(l.rect.height()), 1}};
		vkCmdCopyBufferToImage(cb, m_scratch, static_cast<GSTextureVK*>(job.texture)->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_query_pool, qbase + 4);

	const u64 fence = dev->GetCurrentFenceCounter();
	if (timed)
	{
		TimingSet& ts = m_timing_sets[m_timing_next];
		ts.pending = true;
		ts.fence = fence;
		ts.decoded = plan.decoded_bytes;
		ts.scattered = static_cast<u64>(plan.scatter_pairs) * kBlockBytes;
		m_timing_next = (m_timing_next + 1) % kTimingSets;
	}

	// The textures are final once this has run: nothing is pending any more, and the verify mode reads
	// each one back behind the flush.
	std::vector<VerifyEntry> verify_entries;
	std::vector<GSTextureVK*> verify_textures;
	for (size_t j = 0; j < jobs.size(); j++)
	{
		if (!jobs[j].texture)
			continue;
		GSTextureVK* tex = static_cast<GSTextureVK*>(jobs[j].texture);
		tex->SetDecodePending(false);
		if (m_verify && j < m_verify_expected.size() && !m_verify_expected[j].empty())
		{
			VerifyEntry e;
			e.offset = 0;
			e.width = static_cast<u32>(jobs[j].layout.rect.width());
			e.height = static_cast<u32>(jobs[j].layout.rect.height());
			e.format = static_cast<u32>(jobs[j].layout.format);
			e.expected = std::move(m_verify_expected[j]);
			verify_entries.push_back(std::move(e));
			verify_textures.push_back(tex);
		}
	}
	if (m_verify)
	{
		RecordVerifyReadback(cb, std::move(verify_entries), verify_textures, fence);
		m_verify_expected.clear();
	}

	m_last_flush_fence = fence;
	m_batch->FinishFlush(fence);
}

// ---- Verify mode ----------------------------------------------------------------------------------------

void GSVramMirrorVK::RecordVerifyReadback(
	VkCommandBuffer cb, std::vector<VerifyEntry>&& textures, const std::vector<GSTextureVK*>& live, u64 fence)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();

	VerifyRecord record;
	record.fence = fence;
	u32 total = kMirrorBytes;
	for (VerifyEntry& e : textures)
	{
		e.offset = total;
		total += (e.width * e.height + 15u) & ~15u;
	}
	record.size = total;

	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
	VmaAllocationCreateInfo aci = {};
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	VmaAllocationInfo ai;
	if (vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, &record.buffer, &record.allocation, &ai) != VK_SUCCESS)
	{
		Console.Error("GS/Vulkan: VRAM mirror verify could not allocate its readback buffer; this flush is not checked.");
		return;
	}
	record.mapped = static_cast<u8*>(ai.pMappedData);

	// What the mirror should hold after this flush, as of now.
	record.shadow.assign(m_batch->Shadow(), m_batch->Shadow() + kMirrorBytes);
	record.shadow_valid = m_batch->ShadowValid();

	// Verify is not a hot path: every barrier here is a full one.
	const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	std::vector<VkImageMemoryBarrier> to_src, to_read;
	for (GSTextureVK* tex : live)
	{
		to_src.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED, tex->GetImage(), range});
		to_read.push_back({VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_READ_BIT,
			VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, tex->GetImage(), range});
	}
	const VkMemoryBarrier all_write = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
		VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all_write, 0,
		nullptr, static_cast<u32>(to_src.size()), to_src.data());

	const VkBufferCopy mirror_copy = {0, 0, kMirrorBytes};
	vkCmdCopyBuffer(cb, m_mirror, record.buffer, 1, &mirror_copy);
	for (size_t i = 0; i < textures.size(); i++)
	{
		const VkBufferImageCopy region = {textures[i].offset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
			{textures[i].width, textures[i].height, 1}};
		vkCmdCopyImageToBuffer(cb, live[i]->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, record.buffer, 1, &region);
	}

	const VkMemoryBarrier to_host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &to_host, 0, nullptr, static_cast<u32>(to_read.size()), to_read.data());

	record.textures = std::move(textures);
	m_verify_records.push_back(std::move(record));
}

void GSVramMirrorVK::PublishChecks()
{
	// The marks are checked inside the batch as windows are queued; move what it has counted since the last
	// call into the process-wide report.
	const GSVramMirror::MarkCheck& c = m_batch->Checks();
	s_vc.mark_blocks += c.blocks - m_published_checks.blocks;
	s_vc.mark_stale += c.stale - m_published_checks.stale;
	s_vc.mark_unseen += c.unseen - m_published_checks.unseen;
	s_vc.mark_sweeps += m_batch->ChecksSweeps() - m_published_check_sweeps;
	s_vc.mark_window_blocks += m_batch->ChecksWindowBlocks() - m_published_check_window_blocks;
	m_published_checks = c;
	m_published_check_sweeps = m_batch->ChecksSweeps();
	m_published_check_window_blocks = m_batch->ChecksWindowBlocks();
}

void GSVramMirrorVK::VerifyCompare(VerifyRecord& record)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	vmaInvalidateAllocation(dev->GetAllocator(), record.allocation, 0, record.size);

	// The mirror: every block the shadow knows must hold the shadow's bytes.
	u64 blocks = 0, bad = 0;
	u32 first_bad = ~0u;
	for (u32 bn = 0; bn < kMirrorBlocks; bn++)
	{
		if (!((record.shadow_valid[bn >> 6] >> (bn & 63)) & 1))
			continue;
		blocks++;
		if (std::memcmp(record.mapped + static_cast<size_t>(bn) * kBlockBytes, record.shadow.data() + static_cast<size_t>(bn) * kBlockBytes,
				kBlockBytes) != 0)
		{
			if (first_bad == ~0u)
				first_bad = bn;
			bad++;
		}
	}
	s_vc.flushes++;
	s_vc.mirror_blocks += blocks;
	s_vc.mirror_bad_blocks += bad;
	if (bad && s_vc.printed++ < 12)
		Console.Error("VRAM-MIRROR-VERIFY: after the flush of fence %llu the mirror differs from what was staged in %llu of %llu blocks (first: block %u).",
			static_cast<unsigned long long>(record.fence), static_cast<unsigned long long>(bad), static_cast<unsigned long long>(blocks),
			first_bad);

	// The textures: byte for byte what the CPU decoded at the miss.
	for (const VerifyEntry& e : record.textures)
	{
		const size_t n = static_cast<size_t>(e.width) * e.height;
		u64 bad_texels = 0;
		size_t first = 0;
		for (size_t i = 0; i < n; i++)
		{
			if (record.mapped[e.offset + i] != e.expected[i])
			{
				if (!bad_texels)
					first = i;
				bad_texels++;
			}
		}
		s_vc.windows++;
		s_vc.texels += n;
		if (bad_texels)
		{
			s_vc.bad_windows++;
			s_vc.bad_texels += bad_texels;
			if (s_vc.printed++ < 12)
				Console.Error("VRAM-MIRROR-VERIFY: a %ux%u window (format %u) differs from the CPU decode in %llu texels (first at %zu,%zu: gpu=%u cpu=%u).",
					e.width, e.height, e.format, static_cast<unsigned long long>(bad_texels), first % e.width, first / e.width,
					record.mapped[e.offset + first], e.expected[first]);
		}
	}
}

void GSVramMirrorVK::VerifyPoll(bool wait)
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	if (!wait)
		dev->ScanForCommandBufferCompletion();
	while (!m_verify_records.empty())
	{
		VerifyRecord& record = m_verify_records.front();
		if (record.fence > dev->GetCompletedFenceCounter())
		{
			if (!wait || record.fence >= dev->GetCurrentFenceCounter())
				break;
			dev->WaitForFenceCounter(record.fence);
		}
		if (record.buffer != VK_NULL_HANDLE)
		{
			VerifyCompare(record);
			vmaDestroyBuffer(dev->GetAllocator(), record.buffer, record.allocation);
		}
		m_verify_records.pop_front();
	}
	PublishChecks();
}

void GSVramMirrorVK::VerifyDrain()
{
	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	// A readback recorded into the command buffer being built has not been submitted: do that first.
	if (!m_verify_records.empty() && m_verify_records.back().fence >= dev->GetCurrentFenceCounter())
		dev->ExecuteCommandBuffer(true);
	VerifyPoll(true);
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
