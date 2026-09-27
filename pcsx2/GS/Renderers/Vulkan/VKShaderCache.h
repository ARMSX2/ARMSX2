// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSCacheFile.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class VKShaderCache
{
public:
	~VKShaderCache();

	static void Create();
	static void Destroy();

	/// Returns a handle to the pipeline cache. Set set_dirty to true if you are planning on writing to it externally.
	/// Callable from pipeline compile workers: the VkPipelineCache is created internally synchronized.
	VkPipelineCache GetPipelineCache(bool set_dirty = true);

	/// Writes pipeline cache to file, saving all newly compiled pipelines.
	/// Serialises the pipeline cache to disk. This is SYNCHRONOUS and runs on the GS thread, so it
	/// is rate-limited: pass force=true only where a missed flush actually loses data (teardown).
	bool FlushPipelineCache(bool force = false);

	/// Replaces the pipeline cache with an empty one, so a cleared cache is not written back. GS thread
	/// only, with no pipeline compile running on another thread.
	void ResetPipelineCache();

	/// The shader getters may run on several threads at once: the SPIR-V store locks itself, and
	/// GLSL compilation runs outside that lock.
	VkShaderModule GetVertexShader(std::string_view shader_code);
	VkShaderModule GetFragmentShader(std::string_view shader_code);
	VkShaderModule GetComputeShader(std::string_view shader_code);

private:
	// SPIR-V compiled code type
	using SPIRVCodeType = u32;
	using SPIRVCodeVector = std::vector<SPIRVCodeType>;

	VKShaderCache();

	static std::optional<VKShaderCache::SPIRVCodeVector> CompileShaderToSPV(
		u32 stage, std::string_view source, bool debug);
	static GSCacheFile::Stamp GetSPIRVStamp(bool debug);
	static GSCacheFile::Stamp GetPipelineCacheStamp(bool debug);

	void Open();

	bool CreateNewPipelineCache();
	bool ReadExistingPipelineCache();
	void ClosePipelineCache();

	std::optional<SPIRVCodeVector> GetShaderSPV(u32 type, std::string_view shader_code);
	VkShaderModule GetShaderModule(u32 type, std::string_view shader_code);

	/// SPIR-V by GLSL source. Internally locked, so callable from pipeline compile workers.
	GSCacheFile::BlobStore m_spirv_store;
	std::string m_pipeline_cache_filename;
	GSCacheFile::Stamp m_pipeline_cache_stamp;
	/// Hash of the pipeline cache data last read or written, so an unchanged cache is not rewritten.
	u64 m_pipeline_cache_file_hash = 0;

	VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
	std::atomic<bool> m_pipeline_cache_dirty{false};
	/// When the cache was last serialised, so the synchronous GS-thread write can be rate-limited.
	std::chrono::steady_clock::time_point m_last_pipeline_cache_flush{};
};

extern std::unique_ptr<VKShaderCache> g_vulkan_shader_cache;
