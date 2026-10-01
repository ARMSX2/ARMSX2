// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCompileStats.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"

#include "BuildVersion.h"
#include "Config.h"
#include "ShaderCacheVersion.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"
#include "shaderc/shaderc.h"

#include <cstring>
#include <memory>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#elif !defined(__ANDROID__) && !defined(ARMSX2_LINK_SHADERC)
#include <dlfcn.h>
#endif

std::unique_ptr<VKShaderCache> g_vulkan_shader_cache;

static u32 s_next_bad_shader_id = 0;

namespace
{
#pragma pack(push, 4)
	struct VK_PIPELINE_CACHE_HEADER
	{
		u32 header_length;
		u32 header_version;
		u32 vendor_id;
		u32 device_id;
		u8 uuid[VK_UUID_SIZE];
	};
#pragma pack(pop)
} // namespace

static bool ValidatePipelineCacheHeader(const VK_PIPELINE_CACHE_HEADER& header)
{
	if (header.header_length < sizeof(VK_PIPELINE_CACHE_HEADER))
	{
		Console.Error("Pipeline cache failed validation: Invalid header length");
		return false;
	}

	if (header.header_version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
	{
		Console.Error("Pipeline cache failed validation: Invalid header version");
		return false;
	}

	if (header.vendor_id != GSDeviceVK::GetInstance()->GetDeviceProperties().vendorID)
	{
		Console.Error("Pipeline cache failed validation: Incorrect vendor ID (file: 0x%X, device: 0x%X)",
			header.vendor_id, GSDeviceVK::GetInstance()->GetDeviceProperties().vendorID);
		return false;
	}

	if (header.device_id != GSDeviceVK::GetInstance()->GetDeviceProperties().deviceID)
	{
		Console.Error("Pipeline cache failed validation: Incorrect device ID (file: 0x%X, device: 0x%X)",
			header.device_id, GSDeviceVK::GetInstance()->GetDeviceProperties().deviceID);
		return false;
	}

	if (std::memcmp(header.uuid, GSDeviceVK::GetInstance()->GetDeviceProperties().pipelineCacheUUID, VK_UUID_SIZE) != 0)
	{
		Console.Error("Pipeline cache failed validation: Incorrect UUID");
		return false;
	}

	return true;
}

#if defined(__ANDROID__) || defined(ARMSX2_LINK_SHADERC)

// shaderc is linked in - Android always, and any build that found the static
// archive (the libretro core, which cannot rely on the host having one) - so
// call the functions directly instead of going looking for a library.
namespace dyn_shaderc
{
	static bool Open();
	static void Close();

	static shaderc_compiler_t s_compiler = nullptr;

	// Direct function pointers to the statically-linked shaderc.
	static constexpr auto shaderc_compiler_initialize = ::shaderc_compiler_initialize;
	static constexpr auto shaderc_compiler_release = ::shaderc_compiler_release;
	static constexpr auto shaderc_compile_options_initialize = ::shaderc_compile_options_initialize;
	static constexpr auto shaderc_compile_options_release = ::shaderc_compile_options_release;
	static constexpr auto shaderc_compile_options_set_source_language = ::shaderc_compile_options_set_source_language;
	static constexpr auto shaderc_compile_options_set_generate_debug_info = ::shaderc_compile_options_set_generate_debug_info;
	static constexpr auto shaderc_compile_options_set_optimization_level = ::shaderc_compile_options_set_optimization_level;
	static constexpr auto shaderc_compile_options_set_target_env = ::shaderc_compile_options_set_target_env;
	static constexpr auto shaderc_compile_into_spv = ::shaderc_compile_into_spv;
	static constexpr auto shaderc_result_release = ::shaderc_result_release;
	static constexpr auto shaderc_result_get_length = ::shaderc_result_get_length;
	static constexpr auto shaderc_result_get_num_warnings = ::shaderc_result_get_num_warnings;
	static constexpr auto shaderc_result_get_bytes = ::shaderc_result_get_bytes;
	static constexpr auto shaderc_result_get_error_message = ::shaderc_result_get_error_message;
	static constexpr auto shaderc_result_get_compilation_status = ::shaderc_result_get_compilation_status;
} // namespace dyn_shaderc

bool dyn_shaderc::Open()
{
	if (s_compiler)
		return true;

	s_compiler = shaderc_compiler_initialize();
	if (!s_compiler)
	{
		ERROR_LOG("shaderc_compiler_initialize() failed");
		return false;
	}

	std::atexit(&dyn_shaderc::Close);
	return true;
}

void dyn_shaderc::Close()
{
	if (s_compiler)
	{
		shaderc_compiler_release(s_compiler);
		s_compiler = nullptr;
	}
}

#else // dlopen the system's shaderc

#define SHADERC_FUNCTIONS(X) \
	X(shaderc_compiler_initialize) \
	X(shaderc_compiler_release) \
	X(shaderc_compile_options_initialize) \
	X(shaderc_compile_options_release) \
	X(shaderc_compile_options_set_source_language) \
	X(shaderc_compile_options_set_generate_debug_info) \
	X(shaderc_compile_options_set_optimization_level) \
	X(shaderc_compile_options_set_target_env) \
	X(shaderc_compile_into_spv) \
	X(shaderc_result_release) \
	X(shaderc_result_get_length) \
	X(shaderc_result_get_num_warnings) \
	X(shaderc_result_get_bytes) \
	X(shaderc_result_get_error_message) \
	X(shaderc_result_get_compilation_status)

// TODO: NOT thread safe, yet.
namespace dyn_shaderc
{
	static bool Open();
	static void Close();

	static DynamicLibrary s_library;
	static shaderc_compiler_t s_compiler = nullptr;

#define ADD_FUNC(F) static decltype(&::F) F;
	SHADERC_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

} // namespace dyn_shaderc

bool dyn_shaderc::Open()
{
	if (s_library.IsOpen())
		return true;

	Error error;

#ifdef _WIN32
	const std::string libname = DynamicLibrary::GetVersionedFilename("shaderc_shared");
#else
	// Use versioned, bundle post-processing adds it..
	const std::string libname = DynamicLibrary::GetVersionedFilename("shaderc_shared", 1);
	// Debian packages the library as libshaderc.so.1
	const std::string libname_fallback = DynamicLibrary::GetVersionedFilename("shaderc", 1);
#endif
	if (!s_library.Open(libname.c_str(), &error)
#ifndef _WIN32
		// libname_fallback only exists on non-Windows (see above); Windows has no
		// versioned/Debian .so fallback to try.
		&& !s_library.Open(libname_fallback.c_str(), &error)
#endif
	)
	{
		ERROR_LOG("Failed to load shaderc: {}", error.GetDescription());
		return false;
	}

#define LOAD_FUNC(F) \
	if (!s_library.GetSymbol(#F, &F)) \
	{ \
		ERROR_LOG("Failed to find function {}", #F); \
		Close(); \
		return false; \
	}

	SHADERC_FUNCTIONS(LOAD_FUNC)
#undef LOAD_FUNC

	s_compiler = shaderc_compiler_initialize();
	if (!s_compiler)
	{
		ERROR_LOG("shaderc_compiler_initialize() failed");
		Close();
		return false;
	}

	std::atexit(&dyn_shaderc::Close);
	return true;
}

void dyn_shaderc::Close()
{
	if (s_compiler)
	{
		shaderc_compiler_release(s_compiler);
		s_compiler = nullptr;
	}

#define UNLOAD_FUNC(F) F = nullptr;
	SHADERC_FUNCTIONS(UNLOAD_FUNC)
#undef UNLOAD_FUNC

	s_library.Close();
}

#undef SHADERC_FUNCTIONS
#undef SHADERC_INIT_FUNCTIONS

#endif // __ANDROID__ || ARMSX2_LINK_SHADERC

// Compilation itself is thread-safe on one compiler object; loading it is not.
static std::mutex s_shaderc_open_mutex;

static bool OpenShaderCompiler()
{
	std::unique_lock lock(s_shaderc_open_mutex);
	return dyn_shaderc::Open();
}

/// What produced the SPIR-V: part of the SPIR-V cache's stamp. The GLSL source is the key of every
/// entry, so a new build keeps SPIR-V it would compile identically; this covers the compiler itself.
static std::string GetShaderCompilerIdentity()
{
#if defined(__ANDROID__) || defined(ARMSX2_LINK_SHADERC)
	// The compiler is linked into this binary, so the binary stands for it.
	return fmt::format("linked {} {}", BuildVersion::GitHash, GSCacheFile::GetBuildId());
#else
	if (!OpenShaderCompiler())
		return "none";

	// The library that was loaded. Its path, size and modification time change with any update to it.
	std::string path;
	const void* const fn = reinterpret_cast<const void*>(dyn_shaderc::shaderc_compile_into_spv);
#ifdef _WIN32
	HMODULE module = nullptr;
	wchar_t wpath[MAX_PATH] = {};
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCWSTR>(fn), &module) &&
		GetModuleFileNameW(module, wpath, MAX_PATH) > 0)
	{
		path = StringUtil::WideStringToUTF8String(wpath);
	}
#else
	Dl_info info = {};
	if (dladdr(fn, &info) != 0 && info.dli_fname)
		path = info.dli_fname;
#endif
	FILESYSTEM_STAT_DATA sd = {};
	if (path.empty() || !FileSystem::StatFile(path.c_str(), &sd))
		return fmt::format("shared {} unknown", path);
	return fmt::format("shared {} {} {}", path, sd.Size, sd.ModificationTime);
#endif
}

static void DumpBadShader(std::string_view code, std::string_view errors)
{
	const std::string filename = Path::Combine(EmuFolders::Logs, fmt::format("pcsx2_bad_shader_{}.txt", ++s_next_bad_shader_id));
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb");
	if (fp)
	{
		if (!code.empty())
			std::fwrite(code.data(), code.size(), 1, fp.get());
		std::fputs("\n\n**** ERRORS ****\n", fp.get());
		if (!errors.empty())
			std::fwrite(errors.data(), errors.size(), 1, fp.get());
	}
}

static const char* compilation_status_to_string(shaderc_compilation_status status)
{
	switch (status)
	{
#define CASE(x) case shaderc_compilation_status_##x: return #x
		CASE(success);
		CASE(invalid_stage);
		CASE(compilation_error);
		CASE(internal_error);
		CASE(null_result_object);
		CASE(invalid_assembly);
		CASE(validation_error);
		CASE(transformation_error);
		CASE(configuration_error);
#undef CASE
	}
	return "unknown_error";
}

std::optional<VKShaderCache::SPIRVCodeVector> VKShaderCache::CompileShaderToSPV(u32 stage, std::string_view source, bool debug)
{
	std::optional<VKShaderCache::SPIRVCodeVector> ret;
	if (!OpenShaderCompiler())
		return ret;

	const GSShaderCompileIndicator::CompileTimer compile_timer;
	const GSCompileStats::ScopedTimer stats_timer(GSCompileStats::SpirvCompileNs);
	GSCompileStats::Add(GSCompileStats::SpirvCompiles, 1);

	shaderc_compile_options_t options = dyn_shaderc::shaderc_compile_options_initialize();
	pxAssertRel(options, "shaderc_compile_options_initialize() failed");

	dyn_shaderc::shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
	dyn_shaderc::shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, 0);
#ifdef SHADERC_PCSX2_CUSTOM
	dyn_shaderc::shaderc_compile_options_set_generate_debug_info(options, debug,
		debug && GSDeviceVK::GetInstance()->GetOptionalExtensions().vk_khr_shader_non_semantic_info);
#else
	if (debug)
		dyn_shaderc::shaderc_compile_options_set_generate_debug_info(options);
#endif
	dyn_shaderc::shaderc_compile_options_set_optimization_level(
		options, debug ? shaderc_optimization_level_zero : shaderc_optimization_level_performance);

	const shaderc_compilation_result_t result = dyn_shaderc::shaderc_compile_into_spv(
		dyn_shaderc::s_compiler, source.data(), source.length(), static_cast<shaderc_shader_kind>(stage), "source",
		"main", options);

	shaderc_compilation_status status = shaderc_compilation_status_null_result_object;
	if (!result || (status = dyn_shaderc::shaderc_result_get_compilation_status(result)) != shaderc_compilation_status_success)
	{
		const std::string_view errors(result ? dyn_shaderc::shaderc_result_get_error_message(result)
		                                     : "null result object");
		ERROR_LOG("Failed to compile shader to SPIR-V: {}\n{}", compilation_status_to_string(status), errors);
		DumpBadShader(source, errors);
	}
	else
	{
		const size_t num_warnings = dyn_shaderc::shaderc_result_get_num_warnings(result);
		if (num_warnings > 0)
			WARNING_LOG("Shader compiled with warnings:\n{}", dyn_shaderc::shaderc_result_get_error_message(result));

		const size_t spirv_size = dyn_shaderc::shaderc_result_get_length(result);
		const char* bytes = dyn_shaderc::shaderc_result_get_bytes(result);
		pxAssert(spirv_size > 0 && ((spirv_size % sizeof(u32)) == 0));
		ret = VKShaderCache::SPIRVCodeVector(reinterpret_cast<const u32*>(bytes),
			reinterpret_cast<const u32*>(bytes + spirv_size));
	}

	dyn_shaderc::shaderc_result_release(result);
	dyn_shaderc::shaderc_compile_options_release(options);
	return ret;
}


VKShaderCache::VKShaderCache() = default;

VKShaderCache::~VKShaderCache()
{
	m_spirv_store.Close();
	FlushPipelineCache(true); // teardown: never skip, this is the last chance to persist
	ClosePipelineCache();
}

void VKShaderCache::Create()
{
	pxAssert(!g_vulkan_shader_cache);
	g_vulkan_shader_cache.reset(new VKShaderCache());
	g_vulkan_shader_cache->Open();
}

void VKShaderCache::Destroy()
{
	g_vulkan_shader_cache.reset();
}

GSCacheFile::Stamp VKShaderCache::GetSPIRVStamp(bool debug)
{
	// No build identity and no driver: the key of every entry is the whole GLSL source text and
	// stage, and SPIR-V does not depend on the driver, so an entry is valid for as long as the
	// compiler and its options are the same. That is what keeps SPIR-V across a driver update.
	GSCacheFile::Stamp stamp;
	stamp.Add("shader_cache_version", SHADER_CACHE_VERSION);
	stamp.Add("compiler", GetShaderCompilerIdentity());
	stamp.Add("target", "vulkan1.0");
	stamp.Add("optimization", debug ? "zero" : "performance");
#ifdef SHADERC_PCSX2_CUSTOM
	// Debug info is non-semantic only when the device has the extension.
	stamp.Add("debug_info", debug ? (GSDeviceVK::GetInstance()->GetOptionalExtensions().vk_khr_shader_non_semantic_info ?
											 "non-semantic" :
											 "plain") :
									"none");
#else
	stamp.Add("debug_info", debug ? "plain" : "none");
#endif
	return stamp;
}

GSCacheFile::Stamp VKShaderCache::GetPipelineCacheStamp(bool debug)
{
	// Driver binaries: only valid for the driver that made them. pipelineCacheUUID alone is not
	// trusted, since drivers have shipped updates without changing it; the version and the driver's
	// own name and build string are in too. The build identity is in because what the blob holds
	// depends on this build's pipelines, and a new build starting from an empty blob is what stops
	// pipelines no build uses any more from piling up in it.
	const VkPhysicalDeviceProperties& props = GSDeviceVK::GetInstance()->GetDeviceProperties();
	const VkPhysicalDeviceDriverPropertiesKHR& drv = GSDeviceVK::GetInstance()->GetDeviceDriverProperties();
	GSCacheFile::Stamp stamp;
	stamp.Add("build", GSCacheFile::GetBuildId());
	stamp.Add("shader_cache_version", SHADER_CACHE_VERSION);
	stamp.Add("vendor", props.vendorID);
	stamp.Add("device", props.deviceID);
	stamp.Add("driver_version", props.driverVersion);
	stamp.Add("api_version", props.apiVersion);
	stamp.AddHex("pipeline_cache_uuid", props.pipelineCacheUUID, VK_UUID_SIZE);
	stamp.Add("driver_id", static_cast<u64>(drv.driverID));
	stamp.Add("driver_name", drv.driverName);
	stamp.Add("driver_info", drv.driverInfo);
	stamp.Add("debug", debug ? 1 : 0);
	return stamp;
}

void VKShaderCache::Open()
{
	if (GSConfig.DisableShaderCache)
	{
		CreateNewPipelineCache();
		return;
	}

	const bool debug = GSConfig.UseDebugDevice;
	GSCacheFile::CleanStaleTempFiles(EmuFolders::Cache);

	// Each file's name carries its stamp's digest, so builds and drivers sharing a data root keep
	// separate files; the few most recently used others stay, older ones are removed.
	static constexpr u32 KEEP_IDENTITIES = 3;
	const GSCacheFile::Stamp spirv_stamp = GetSPIRVStamp(debug);
	const std::string spirv_stem = fmt::format("vulkan_shaders{}_{}", debug ? "_debug" : "",
		GSCacheFile::ShortName(spirv_stamp.GetDigest()));
	GSCacheFile::PruneOtherIdentities(EmuFolders::Cache, "vulkan_shaders", spirv_stem, KEEP_IDENTITIES);

	m_pipeline_cache_stamp = GetPipelineCacheStamp(debug);
	const std::string pipeline_stem = fmt::format("vulkan_pipelines{}_{}", debug ? "_debug" : "",
		GSCacheFile::ShortName(m_pipeline_cache_stamp.GetDigest()));
	GSCacheFile::PruneOtherIdentities(EmuFolders::Cache, "vulkan_pipelines", pipeline_stem, KEEP_IDENTITIES);
	m_pipeline_cache_filename = Path::Combine(EmuFolders::Cache, pipeline_stem + ".bin");

	static constexpr u32 KEY_SIZE = sizeof(GSCacheFile::Digest) + 2 * sizeof(u32);
	if (!m_spirv_store.Open(Path::Combine(EmuFolders::Cache, spirv_stem), GSCacheFile::KIND_VK_SPIRV, spirv_stamp, KEY_SIZE))
		Console.Warning("Vulkan: running without a SPIR-V cache");
	else
		INFO_LOG("Vulkan: SPIR-V cache '{}' has {} entries", spirv_stem, m_spirv_store.GetEntryCount());

	if (!ReadExistingPipelineCache())
		CreateNewPipelineCache();
}

void VKShaderCache::ResetPipelineCache()
{
	// Nothing else may be using the cache: the caller has stopped the precompile workers and runs on
	// the GS thread. Pipelines already built do not refer to it.
	ClosePipelineCache();
	CreateNewPipelineCache();
}

VkPipelineCache VKShaderCache::GetPipelineCache(bool set_dirty /*= true*/)
{
	if (m_pipeline_cache == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	if (set_dirty)
		m_pipeline_cache_dirty.store(true, std::memory_order_relaxed);
	return m_pipeline_cache;
}

bool VKShaderCache::CreateNewPipelineCache()
{
	const VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
	VkResult res = vkCreatePipelineCache(GSDeviceVK::GetInstance()->GetDevice(), &ci, nullptr, &m_pipeline_cache);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreatePipelineCache() failed: ");
		return false;
	}

	m_pipeline_cache_file_hash = 0;
	m_pipeline_cache_dirty = true;
	return true;
}

bool VKShaderCache::ReadExistingPipelineCache()
{
	std::vector<u8> data;
	const GSCacheFile::ReadResult res = GSCacheFile::ReadFramedFile(
		m_pipeline_cache_filename, GSCacheFile::KIND_VK_PIPELINES, m_pipeline_cache_stamp, &data);
	if (res != GSCacheFile::ReadResult::Ok)
	{
		if (res != GSCacheFile::ReadResult::Missing)
		{
			INFO_LOG("Vulkan: discarding pipeline cache '{}': {}", m_pipeline_cache_filename,
				GSCacheFile::ReadResultString(res));
		}
		return false;
	}

	// The stamp already covers the device, but the blob's own header is what the driver will judge,
	// so it has to agree too.
	VK_PIPELINE_CACHE_HEADER header;
	if (data.size() < sizeof(header))
		return false;
	std::memcpy(&header, data.data(), sizeof(header));
	if (!ValidatePipelineCacheHeader(header))
		return false;

	const VkPipelineCacheCreateInfo ci{
		VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, data.size(), data.data()};
	VkResult vres = vkCreatePipelineCache(GSDeviceVK::GetInstance()->GetDevice(), &ci, nullptr, &m_pipeline_cache);
	if (vres != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(vres, "vkCreatePipelineCache() failed: ");
		return false;
	}

	m_pipeline_cache_file_hash = GSCacheFile::Hash64(data.data(), data.size());
	INFO_LOG("Vulkan: read {} bytes of pipeline cache", data.size());
	return true;
}

bool VKShaderCache::FlushPipelineCache(bool force)
{
	if (m_pipeline_cache == VK_NULL_HANDLE || !m_pipeline_cache_dirty || m_pipeline_cache_filename.empty())
		return false;

	// ★ Rate-limited, because this whole function is synchronous ON THE GS THREAD: it re-serialises
	// the ENTIRE cache (measured at 777 KB on a Retroid Pocket 6) and writes it to disk, and the
	// emulator is frozen for the duration. GetTFXPipeline triggers it every 256 new compiles with
	// no time bound, and fast-forward blasts through content at 4x+ — hitting many new pipeline
	// variants in a burst, crossing the threshold repeatedly, and stalling the picture for seconds
	// right as the user drops back to normal speed. That is the reported "turn FF off and it hangs
	// for a few seconds", and it is intermittent precisely because it depends on whether the burst
	// crossed the counter. Losing a flush costs nothing but recompiling those pipelines on the next
	// cold start, so throttling is strictly a win; teardown passes force=true.
	static constexpr std::chrono::seconds MIN_FLUSH_INTERVAL{120};
	const auto now = std::chrono::steady_clock::now();
	if (!force && m_last_pipeline_cache_flush.time_since_epoch().count() != 0 &&
		(now - m_last_pipeline_cache_flush) < MIN_FLUSH_INTERVAL)
	{
		return false;
	}
	m_last_pipeline_cache_flush = now;

	const GSCompileStats::ScopedTimer stats_timer(GSCompileStats::CacheFlushNs);

	// Cleared before the data is read, so a pipeline a worker adds while this runs marks it dirty
	// again instead of being lost.
	m_pipeline_cache_dirty.store(false, std::memory_order_relaxed);

	size_t data_size;
	VkResult res =
		vkGetPipelineCacheData(GSDeviceVK::GetInstance()->GetDevice(), m_pipeline_cache, &data_size, nullptr);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData() failed: ");
		m_pipeline_cache_dirty.store(true, std::memory_order_relaxed);
		return false;
	}

	std::vector<u8> data(data_size);
	res = vkGetPipelineCacheData(GSDeviceVK::GetInstance()->GetDevice(), m_pipeline_cache, &data_size, data.data());
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData() (2) failed: ");
		m_pipeline_cache_dirty.store(true, std::memory_order_relaxed);
		return false;
	}

	data.resize(data_size);

	// Compared by content: a blob can change without changing size.
	const u64 hash = GSCacheFile::Hash64(data.data(), data.size());
	if (hash == m_pipeline_cache_file_hash)
	{
		Console.WriteLn("Skipping updating pipeline cache '%s' due to no changes.", m_pipeline_cache_filename.c_str());
		return true;
	}

	// Framed with a checksum and written to a temporary file that is renamed over the old one: a
	// half-written blob that still passes the driver's header check has rendered garbage on Adreno.
	Console.WriteLn("Writing %zu bytes to '%s'", data_size, m_pipeline_cache_filename.c_str());
	if (!GSCacheFile::WriteFramedFile(m_pipeline_cache_filename, GSCacheFile::KIND_VK_PIPELINES,
			m_pipeline_cache_stamp, data.data(), data.size()))
	{
		Console.Error("Failed to write pipeline cache to '%s'", m_pipeline_cache_filename.c_str());
		m_pipeline_cache_dirty.store(true, std::memory_order_relaxed);
		return false;
	}
	m_pipeline_cache_file_hash = hash;
	return true;
}

void VKShaderCache::ClosePipelineCache()
{
	if (m_pipeline_cache == VK_NULL_HANDLE)
		return;

	vkDestroyPipelineCache(GSDeviceVK::GetInstance()->GetDevice(), m_pipeline_cache, nullptr);
	m_pipeline_cache = VK_NULL_HANDLE;
}

std::optional<VKShaderCache::SPIRVCodeVector> VKShaderCache::GetShaderSPV(u32 type, std::string_view shader_code)
{
	// The key: a 128-bit hash of the whole source text, its length and the stage.
	u8 key[sizeof(GSCacheFile::Digest) + 2 * sizeof(u32)];
	const GSCacheFile::Digest digest = GSCacheFile::Hash128(shader_code.data(), shader_code.size());
	const u32 length = static_cast<u32>(shader_code.size());
	std::memcpy(&key[0], digest.bytes, sizeof(digest.bytes));
	std::memcpy(&key[16], &length, sizeof(length));
	std::memcpy(&key[20], &type, sizeof(type));

	std::vector<u8> data;
	if (m_spirv_store.Lookup(key, &data))
	{
		// The checksum already passed; a SPIR-V module is whole words starting with the magic number.
		static constexpr u32 SPIRV_MAGIC = 0x07230203;
		u32 magic = 0;
		if (data.size() >= sizeof(u32) && (data.size() % sizeof(u32)) == 0 &&
			(std::memcpy(&magic, data.data(), sizeof(magic)), magic == SPIRV_MAGIC))
		{
			GSCompileStats::Add(GSCompileStats::SpirvCacheHits, 1);
			SPIRVCodeVector spv(data.size() / sizeof(u32));
			std::memcpy(spv.data(), data.data(), data.size());
			return spv;
		}
		Console.Error("Vulkan: cached SPIR-V is not SPIR-V, recompiling");
	}

	std::optional<SPIRVCodeVector> spv = CompileShaderToSPV(type, shader_code, GSConfig.UseDebugDevice);
	if (spv.has_value() && m_spirv_store.IsWritable())
		m_spirv_store.Insert(key, spv->data(), spv->size() * sizeof(SPIRVCodeType));
	return spv;
}

VkShaderModule VKShaderCache::GetShaderModule(u32 type, std::string_view shader_code)
{
	std::optional<SPIRVCodeVector> spv = GetShaderSPV(type, shader_code);
	if (!spv.has_value())
		return VK_NULL_HANDLE;

	const VkShaderModuleCreateInfo ci{
		VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv->size() * sizeof(SPIRVCodeType), spv->data()};

	const GSCompileStats::ScopedTimer stats_timer(GSCompileStats::ModuleCreateNs);
	VkShaderModule mod;
	VkResult res = vkCreateShaderModule(GSDeviceVK::GetInstance()->GetDevice(), &ci, nullptr, &mod);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateShaderModule() failed: ");
		return VK_NULL_HANDLE;
	}

	return mod;
}

VkShaderModule VKShaderCache::GetVertexShader(std::string_view shader_code)
{
	return GetShaderModule(shaderc_glsl_vertex_shader, std::move(shader_code));
}

VkShaderModule VKShaderCache::GetFragmentShader(std::string_view shader_code)
{
	return GetShaderModule(shaderc_glsl_fragment_shader, std::move(shader_code));
}

VkShaderModule VKShaderCache::GetComputeShader(std::string_view shader_code)
{
	return GetShaderModule(shaderc_glsl_compute_shader, std::move(shader_code));
}
