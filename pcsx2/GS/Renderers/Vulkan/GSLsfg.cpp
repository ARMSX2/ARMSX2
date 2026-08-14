// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef ARMSX2_HAS_LSFG
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKSwapChain.h"

#include "armsx2_lsfg_shim.h"
#include "extract/trans.hpp"

#include <pe-parse/parse.h>

#include <android/hardware_buffer.h>
#include <dlfcn.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#endif

namespace GSLsfg
{
	namespace
	{
		std::string s_dll_path;

		// Written once from the GS thread at device creation, read from the UI thread whenever
		// the settings screen asks why the row is greyed out. Atomic rather than mutex'd because
		// the UI only needs a recent value, never a synchronised one.
		std::atomic<bool> s_caps_known{false};
		std::atomic<bool> s_is_vulkan{false};
		std::atomic<u32> s_adreno_generation{0};

		// Sticky: a device that failed to initialise once will fail the same way every frame,
		// and retrying inside the present path would turn one bad init into a per-frame stall.
		std::atomic<bool> s_init_failed{false};
	} // namespace

	void NoteRendererCapability(bool is_vulkan, u32 adreno_generation)
	{
		s_is_vulkan.store(is_vulkan, std::memory_order_relaxed);
		s_adreno_generation.store(adreno_generation, std::memory_order_relaxed);
		s_caps_known.store(true, std::memory_order_release);
	}

	void SetDllPath(std::string path)
	{
		if (s_dll_path == path)
			return;
		s_dll_path = std::move(path);
		// A new DLL deserves a fresh attempt; the previous failure may have been this file.
		s_init_failed.store(false, std::memory_order_relaxed);
	}

	const std::string& GetDllPath() { return s_dll_path; }

	bool LooksLikeLosslessDll(const std::string& path)
	{
		// Structural only: "MZ" at 0 and a PE signature where the DOS header points. The point
		// is to reject an obviously-wrong pick at import time — a .txt, a truncated download,
		// the wrong DLL — not to authenticate Lossless Scaling. A file that passes this and is
		// still not the real thing fails later with a missing-shader error, which is the
		// message the user needs anyway.
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
		if (!fp)
			return false;

		u8 dos[0x40] = {};
		if (std::fread(dos, sizeof(dos), 1, fp.get()) != 1 || dos[0] != 'M' || dos[1] != 'Z')
			return false;

		// e_lfanew at 0x3C is the offset of the PE header.
		const u32 pe_off = static_cast<u32>(dos[0x3C]) | (static_cast<u32>(dos[0x3D]) << 8) |
		                   (static_cast<u32>(dos[0x3E]) << 16) | (static_cast<u32>(dos[0x3F]) << 24);
		if (pe_off < sizeof(dos) || pe_off > (64u * 1024u * 1024u))
			return false;

		if (FileSystem::FSeek64(fp.get(), static_cast<s64>(pe_off), SEEK_SET) != 0)
			return false;
		u8 sig[4] = {};
		if (std::fread(sig, sizeof(sig), 1, fp.get()) != 1)
			return false;
		return sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
	}

	Unavailable GetUnavailableReason()
	{
#ifndef ARMSX2_HAS_LSFG
		return Unavailable::NotCompiledIn;
#else
		// Before any renderer has come up there is nothing to ask, so the two hardware gates are
		// skipped rather than guessed. Reporting GpuUnsupported from a cold start would tell a
		// perfectly capable device it is not supported, purely because no game had booted yet.
		if (s_caps_known.load(std::memory_order_acquire))
		{
			// Vulkan only: the library shares images as AHardwareBuffers imported into its own
			// VkDevice, and there is no equivalent path for the GLES backend.
			if (!s_is_vulkan.load(std::memory_order_relaxed))
				return Unavailable::NotVulkan;
			// Adreno 7xx or newer, per upstream. Asked of the resolved architecture rather than
			// a GL_RENDERER substring search, for the same reason the Mali workarounds moved
			// into the driver database: a parsed generation can say "7xx and up", a substring
			// cannot.
			if (s_adreno_generation.load(std::memory_order_relaxed) < 7)
				return Unavailable::GpuUnsupported;
		}

		if (s_dll_path.empty())
			return Unavailable::NoDll;
		if (!LooksLikeLosslessDll(s_dll_path))
			return Unavailable::DllUnreadable;
		if (s_init_failed.load(std::memory_order_relaxed))
			return Unavailable::InitFailed;
		return Unavailable::Available;
#endif
	}

	bool IsAvailable() { return GetUnavailableReason() == Unavailable::Available; }

	const char* GetUnavailableReasonString()
	{
		switch (GetUnavailableReason())
		{
			case Unavailable::Available: return "available";
			case Unavailable::NotCompiledIn: return "not included in this build";
			case Unavailable::NotVulkan: return "requires the Vulkan renderer";
			case Unavailable::GpuUnsupported: return "requires an Adreno 7xx or newer GPU";
			case Unavailable::NoDll: return "no Lossless.dll selected";
			case Unavailable::DllUnreadable: return "the selected file is not a readable DLL";
			case Unavailable::InitFailed: return "frame generation failed to start on this device";
			default: return "unavailable";
		}
	}
} // namespace GSLsfg

#ifndef ARMSX2_HAS_LSFG

// Play flavour, or a build whose fetch produced no library. The state queries above still work
// (and always answer NotCompiledIn), so only the parts that would need the library are stubbed.
namespace GSLsfg
{
	bool Initialize(VKSwapChain*, u32) { return false; }
	void Shutdown() {}
	bool IsActive() { return false; }
	u32 GetMultiplier() { return 1; }
	bool PresentWithGeneration(VkQueue, VKSwapChain*, VkSemaphore) { return false; }
} // namespace GSLsfg

#else

namespace GSLsfg
{
	namespace
	{
		// --- the backend, loaded at runtime ---------------------------------------------------
		//
		// framegen lives in libarmsx2_lsfg.so and is reached only through the C entry points in
		// armsx2_lsfg_shim.h. It is NOT linked, because it carries its own volk whose 759
		// vkCreateImage-style globals would otherwise collide with (or, worse, silently alias)
		// the identically named ones in PCSX2's VKLoader. See armsx2_lsfg_shim.h for the full
		// reasoning. dlopen also means a build or a device missing that .so degrades to "frame
		// generation unavailable" instead of failing to start the emulator.

		struct Backend
		{
			void* handle = nullptr;
			pfn_armsx2_lsfg_abi_version abi_version = nullptr;
			pfn_armsx2_lsfg_initialize initialize = nullptr;
			pfn_armsx2_lsfg_create_context create_context = nullptr;
			pfn_armsx2_lsfg_present present = nullptr;
			pfn_armsx2_lsfg_wait_idle wait_idle = nullptr;
			pfn_armsx2_lsfg_delete_context delete_context = nullptr;
			pfn_armsx2_lsfg_finalize finalize = nullptr;
			pfn_armsx2_lsfg_last_error last_error = nullptr;
		};

		Backend s_backend;
		bool s_backend_tried = false;

		const char* BackendError()
		{
			const char* msg = s_backend.last_error ? s_backend.last_error() : nullptr;
			return (msg && *msg) ? msg : "no detail";
		}

		bool LoadBackend()
		{
			if (s_backend.handle)
				return true;
			if (s_backend_tried)
				return false; // one attempt; a missing .so will still be missing next frame
			s_backend_tried = true;

			void* handle = dlopen("libarmsx2_lsfg.so", RTLD_NOW | RTLD_LOCAL);
			if (!handle)
			{
				Console.ErrorFmt("@@ANDROID_LSFG@@ libarmsx2_lsfg.so not loadable: {}", dlerror());
				return false;
			}

			Backend b;
			b.handle = handle;
			auto sym = [handle](const char* name) { return dlsym(handle, name); };
			b.abi_version = reinterpret_cast<pfn_armsx2_lsfg_abi_version>(sym("armsx2_lsfg_abi_version"));
			b.initialize = reinterpret_cast<pfn_armsx2_lsfg_initialize>(sym("armsx2_lsfg_initialize"));
			b.create_context = reinterpret_cast<pfn_armsx2_lsfg_create_context>(sym("armsx2_lsfg_create_context"));
			b.present = reinterpret_cast<pfn_armsx2_lsfg_present>(sym("armsx2_lsfg_present"));
			b.wait_idle = reinterpret_cast<pfn_armsx2_lsfg_wait_idle>(sym("armsx2_lsfg_wait_idle"));
			b.delete_context = reinterpret_cast<pfn_armsx2_lsfg_delete_context>(sym("armsx2_lsfg_delete_context"));
			b.finalize = reinterpret_cast<pfn_armsx2_lsfg_finalize>(sym("armsx2_lsfg_finalize"));
			b.last_error = reinterpret_cast<pfn_armsx2_lsfg_last_error>(sym("armsx2_lsfg_last_error"));

			if (!b.abi_version || !b.initialize || !b.create_context || !b.present || !b.wait_idle ||
				!b.delete_context || !b.finalize || !b.last_error)
			{
				Console.Error("@@ANDROID_LSFG@@ libarmsx2_lsfg.so is missing entry points");
				dlclose(handle);
				return false;
			}
			// A stale .so left behind by an older install would otherwise be called with the
			// wrong argument layout, which is a crash with no useful backtrace.
			if (b.abi_version() != ARMSX2_LSFG_ABI_VERSION)
			{
				Console.ErrorFmt("@@ANDROID_LSFG@@ libarmsx2_lsfg.so is ABI v{}, expected v{}",
					b.abi_version(), ARMSX2_LSFG_ABI_VERSION);
				dlclose(handle);
				return false;
			}

			s_backend = b;
			return true;
		}

		// --- shader extraction ---------------------------------------------------------------
		//
		// framegen does not read Lossless.dll itself: it asks for shaders by name and wants
		// SPIR-V back, so the whole chain is ours. Upstream does this in the layer .so we do not
		// build, and its extract.cpp hunts through Steam install paths and pulls in a TOML config
		// system — neither of which means anything here, where the path comes from a SAF pick.
		// So the resource walk is reimplemented and only the DXBC->SPIR-V translation is taken
		// from upstream, where their binding-rewrite fixes live.

		std::unordered_map<u32, std::vector<u8>> s_shader_blobs;
		// Holds the SPIR-V handed back through the C callback. Valid until the next callback,
		// which is all the contract promises — framegen copies it into a VkShaderModule at once.
		std::vector<u8> s_shader_scratch;

		int OnResource(void*, const peparse::resource& res)
		{
			if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0)
				return 0;
			std::vector<u8> data(static_cast<size_t>(res.buf->bufLen));
			std::copy_n(res.buf->buf, res.buf->bufLen, data.data());
			s_shader_blobs[res.name] = std::move(data);
			return 0;
		}

		// Resource IDs, from upstream's extract.cpp — they track Lossless Scaling's own resource
		// layout. Only the names framegen asks for are listed; anything else fails the load
		// cleanly rather than feeding it a wrong shader.
		const std::unordered_map<std::string, u32>& ShaderNameTable()
		{
			static const std::unordered_map<std::string, u32> table = {
				{"mipmaps", 255},
				{"alpha[0]", 267}, {"alpha[1]", 268}, {"alpha[2]", 269}, {"alpha[3]", 270},
				{"beta[0]", 275}, {"beta[1]", 276}, {"beta[2]", 277}, {"beta[3]", 278},
				{"beta[4]", 279},
				{"gamma[0]", 257}, {"gamma[1]", 259}, {"gamma[2]", 260}, {"gamma[3]", 261},
				{"gamma[4]", 262},
				{"delta[0]", 257}, {"delta[1]", 263}, {"delta[2]", 264}, {"delta[3]", 265},
				{"delta[4]", 266}, {"delta[5]", 258}, {"delta[6]", 271}, {"delta[7]", 272},
				{"delta[8]", 273}, {"delta[9]", 274},
				{"generate", 256},
			};
			return table;
		}

		/// Pull every RCDATA resource out of the user's DLL and confirm the ones we need are
		/// there. Throws with a message the settings screen can show verbatim.
		void ExtractShaders()
		{
			if (!s_shader_blobs.empty())
				return;

			peparse::parsed_pe* dll = peparse::ParsePEFromFile(s_dll_path.c_str());
			if (!dll)
				throw std::runtime_error("could not read Lossless.dll");
			peparse::IterRsrc(dll, OnResource, nullptr);
			peparse::DestructParsedPE(dll);

			for (const auto& [name, idx] : ShaderNameTable())
			{
				if (s_shader_blobs.find(idx) == s_shader_blobs.end())
				{
					s_shader_blobs.clear();
					throw std::runtime_error(
						"Lossless.dll is missing shader '" + name + "' — is Lossless Scaling up to date?");
				}
			}
		}

		/// The C callback framegen drives during initialise. Everything it can throw is caught
		/// here: an exception must not unwind through the shim's shared object.
		int ShaderCallback(void*, const char* name, const uint8_t** out_data, uint32_t* out_size)
		{
			try
			{
				const auto hit = ShaderNameTable().find(name);
				if (hit == ShaderNameTable().end())
					return -1;
				const auto blob = s_shader_blobs.find(hit->second);
				if (blob == s_shader_blobs.end())
					return -1;
				s_shader_scratch = Extract::translateShader(blob->second);
				if (s_shader_scratch.empty())
					return -1;
				*out_data = s_shader_scratch.data();
				*out_size = static_cast<uint32_t>(s_shader_scratch.size());
				return 0;
			}
			catch (const std::exception& ex)
			{
				Console.ErrorFmt("@@ANDROID_LSFG@@ shader '{}' failed to translate: {}", name, ex.what());
				return -1;
			}
			catch (...)
			{
				return -1;
			}
		}

		// --- AHardwareBuffer-backed images ----------------------------------------------------
		//
		// The interpolator runs on its own VkDevice, so the only images both sides can touch are
		// ones backed by an AHardwareBuffer: we allocate the AHB, wrap it in a VkImage on OUR
		// device, and hand the raw AHB across, where it is wrapped again on theirs.

		struct AhbImage
		{
			AHardwareBuffer* ahb = nullptr;
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
		};

		VkDevice s_device = VK_NULL_HANDLE;
		VkPhysicalDevice s_physical_device = VK_NULL_HANDLE;
		VkQueue s_queue = VK_NULL_HANDLE;
		VkCommandPool s_cmd_pool = VK_NULL_HANDLE;

		AhbImage s_frame[2];               // previous and current real frames
		std::vector<AhbImage> s_generated; // multiplier - 1 interpolated outputs

		s32 s_context_id = -1;
		bool s_active = false;
		u32 s_multiplier = 1;
		VkExtent2D s_extent = {};
		VkFormat s_format = VK_FORMAT_UNDEFINED;
		u64 s_frame_index = 0;

		// One command buffer + one semaphore per generated frame, plus one of each for the
		// pre-copy. Recycled across frames rather than pooled: the Android path is fully
		// synchronous (framegen has no exportable cross-device semaphore, so an idle wait is the
		// only barrier available), which means last frame's work is provably finished before
		// this frame records anything.
		VkCommandBuffer s_pre_copy_cmd = VK_NULL_HANDLE;
		VkSemaphore s_pre_copy_sem = VK_NULL_HANDLE;
		std::vector<VkCommandBuffer> s_post_copy_cmds;
		std::vector<VkSemaphore> s_post_copy_sems;
		std::vector<VkSemaphore> s_acquire_sems;

		void DestroyAhbImage(AhbImage& img)
		{
			if (img.image != VK_NULL_HANDLE)
				vkDestroyImage(s_device, img.image, nullptr);
			if (img.memory != VK_NULL_HANDLE)
				vkFreeMemory(s_device, img.memory, nullptr);
			if (img.ahb)
				AHardwareBuffer_release(img.ahb);
			img = {};
		}

		bool CreateAhbImage(AhbImage& out, VkExtent2D extent, VkFormat format)
		{
			u32 ahb_format = 0;
			switch (format)
			{
				case VK_FORMAT_R8G8B8A8_UNORM: ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; break;
				case VK_FORMAT_R16G16B16A16_SFLOAT: ahb_format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT; break;
				default:
					Console.ErrorFmt("@@ANDROID_LSFG@@ unsupported swapchain format {}", static_cast<u32>(format));
					return false;
			}

			const AHardwareBuffer_Desc desc = {
				extent.width, extent.height, 1, ahb_format,
				AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
				0, 0, 0};
			if (AHardwareBuffer_allocate(&desc, &out.ahb) != 0 || !out.ahb)
			{
				Console.Error("@@ANDROID_LSFG@@ AHardwareBuffer_allocate failed");
				return false;
			}

			VkExternalMemoryImageCreateInfo ext_info = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
				nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
			VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext_info, 0, VK_IMAGE_TYPE_2D,
				format, {extent.width, extent.height, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
					VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
			if (vkCreateImage(s_device, &image_info, nullptr, &out.image) != VK_SUCCESS)
			{
				Console.Error("@@ANDROID_LSFG@@ vkCreateImage failed for the shared image");
				DestroyAhbImage(out);
				return false;
			}

			// Upstream deliberately skips vkGetAndroidHardwareBufferPropertiesANDROID here and
			// takes the requirements off the image instead, because the wrapper ICDs some hosts
			// use do not forward that entry point. The image's own requirements are correct
			// either way, so this follows them.
			VkMemoryRequirements reqs = {};
			vkGetImageMemoryRequirements(s_device, out.image, &reqs);

			VkPhysicalDeviceMemoryProperties mem_props = {};
			vkGetPhysicalDeviceMemoryProperties(s_physical_device, &mem_props);

			u32 type_index = UINT32_MAX;
			for (u32 i = 0; i < mem_props.memoryTypeCount; i++)
			{
				if ((reqs.memoryTypeBits & (1u << i)) &&
					(mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				{
					type_index = i;
					break;
				}
			}
			if (type_index == UINT32_MAX)
			{
				for (u32 i = 0; i < mem_props.memoryTypeCount; i++)
				{
					if (reqs.memoryTypeBits & (1u << i))
					{
						type_index = i;
						break;
					}
				}
			}
			if (type_index == UINT32_MAX)
			{
				Console.Error("@@ANDROID_LSFG@@ no memory type accepts the shared image");
				DestroyAhbImage(out);
				return false;
			}

			VkMemoryDedicatedAllocateInfo dedicated = {
				VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, out.image, VK_NULL_HANDLE};
			VkImportAndroidHardwareBufferInfoANDROID import_info = {
				VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &dedicated, out.ahb};
			VkMemoryAllocateInfo alloc_info = {
				VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import_info, reqs.size, type_index};
			if (vkAllocateMemory(s_device, &alloc_info, nullptr, &out.memory) != VK_SUCCESS)
			{
				Console.Error("@@ANDROID_LSFG@@ could not import the AHardwareBuffer");
				DestroyAhbImage(out);
				return false;
			}
			if (vkBindImageMemory(s_device, out.image, out.memory, 0) != VK_SUCCESS)
			{
				Console.Error("@@ANDROID_LSFG@@ vkBindImageMemory failed for the shared image");
				DestroyAhbImage(out);
				return false;
			}
			return true;
		}

		// --- copy helpers ----------------------------------------------------------------------

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
			VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
			VkPipelineStageFlags dst_stage)
		{
			const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src_access,
				dst_access, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		/// Full-surface copy between two images of identical extent and format. `src_layout` is
		/// where the source is on entry and must be left; `dst_layout` likewise for the target.
		void RecordCopy(VkCommandBuffer cmd, VkImage src, VkImageLayout src_layout, VkImage dst,
			VkImageLayout dst_layout, VkExtent2D extent)
		{
			ImageBarrier(cmd, src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
				VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			// The destination is fully overwritten, so its previous contents are worthless and
			// UNDEFINED is the cheaper source layout — it lets a tiler skip the load.
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

			const VkImageCopy region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {extent.width, extent.height, 1}};
			vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			ImageBarrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout, VK_ACCESS_TRANSFER_READ_BIT,
				0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_layout, VK_ACCESS_TRANSFER_WRITE_BIT,
				0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		}

		bool SubmitOneShot(VkCommandBuffer cmd, VkSemaphore wait, VkSemaphore signal)
		{
			const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			submit.waitSemaphoreCount = (wait != VK_NULL_HANDLE) ? 1u : 0u;
			submit.pWaitSemaphores = (wait != VK_NULL_HANDLE) ? &wait : nullptr;
			submit.pWaitDstStageMask = (wait != VK_NULL_HANDLE) ? &wait_stage : nullptr;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &cmd;
			submit.signalSemaphoreCount = (signal != VK_NULL_HANDLE) ? 1u : 0u;
			submit.pSignalSemaphores = (signal != VK_NULL_HANDLE) ? &signal : nullptr;
			return vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
		}

		void DestroyResources()
		{
			if (s_device == VK_NULL_HANDLE)
				return;

			for (VkSemaphore s : s_post_copy_sems)
				if (s != VK_NULL_HANDLE)
					vkDestroySemaphore(s_device, s, nullptr);
			for (VkSemaphore s : s_acquire_sems)
				if (s != VK_NULL_HANDLE)
					vkDestroySemaphore(s_device, s, nullptr);
			if (s_pre_copy_sem != VK_NULL_HANDLE)
				vkDestroySemaphore(s_device, s_pre_copy_sem, nullptr);
			s_post_copy_sems.clear();
			s_acquire_sems.clear();
			s_pre_copy_sem = VK_NULL_HANDLE;

			// The pool owns the buffers; freeing it frees them.
			if (s_cmd_pool != VK_NULL_HANDLE)
				vkDestroyCommandPool(s_device, s_cmd_pool, nullptr);
			s_cmd_pool = VK_NULL_HANDLE;
			s_pre_copy_cmd = VK_NULL_HANDLE;
			s_post_copy_cmds.clear();

			for (AhbImage& img : s_generated)
				DestroyAhbImage(img);
			s_generated.clear();
			DestroyAhbImage(s_frame[0]);
			DestroyAhbImage(s_frame[1]);
		}

		/// Everything Initialize() allocates, released in one place so its several failure exits
		/// cannot each forget a different piece.
		bool FailInitialize(const char* why)
		{
			Console.ErrorFmt("@@ANDROID_LSFG@@ {} — frame generation off", why);
			s_init_failed.store(true, std::memory_order_relaxed);
			if (s_context_id >= 0 && s_backend.delete_context)
				s_backend.delete_context(s_context_id);
			s_context_id = -1;
			if (s_backend.finalize)
				s_backend.finalize();
			DestroyResources();
			s_device = VK_NULL_HANDLE;
			s_physical_device = VK_NULL_HANDLE;
			s_queue = VK_NULL_HANDLE;
			return false;
		}
	} // namespace

	bool IsActive() { return s_active; }

	u32 GetMultiplier() { return s_active ? s_multiplier : 1u; }

	void Shutdown()
	{
		if (!s_active && s_context_id < 0 && s_cmd_pool == VK_NULL_HANDLE)
			return;

		// The backend's own device is idled first: it reads our AHBs and we hold no fence on it,
		// so on Android this is the only barrier that exists between the two devices. Then our
		// own, because our copies touch the same storage.
		if (s_context_id >= 0)
		{
			s_backend.wait_idle();
			s_backend.delete_context(s_context_id);
		}
		s_backend.finalize();
		s_context_id = -1;

		if (s_device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(s_device);
		DestroyResources();

		s_active = false;
		s_multiplier = 1;
		s_extent = {};
		s_format = VK_FORMAT_UNDEFINED;
		s_frame_index = 0;
		s_device = VK_NULL_HANDLE;
		s_physical_device = VK_NULL_HANDLE;
		s_queue = VK_NULL_HANDLE;
	}

	bool Initialize(VKSwapChain* swap_chain, u32 multiplier)
	{
		if (!swap_chain || !g_gs_device || !IsAvailable())
			return false;
		if (!LoadBackend())
		{
			s_init_failed.store(true, std::memory_order_relaxed);
			return false;
		}

		multiplier = std::clamp<u32>(multiplier, 2, 4);
		const VkExtent2D extent = {swap_chain->GetWidth(), swap_chain->GetHeight()};
		const VkFormat format = swap_chain->GetTextureFormat();

		if (s_active && extent.width == s_extent.width && extent.height == s_extent.height &&
			format == s_format && multiplier == s_multiplier)
		{
			return true; // idempotent; nothing changed
		}
		if (s_active || s_cmd_pool != VK_NULL_HANDLE)
			Shutdown();

		if (extent.width == 0 || extent.height == 0)
			return false;

		GSDeviceVK* dev = GSDeviceVK::GetInstance();
		s_device = dev->GetDevice();
		s_physical_device = dev->GetPhysicalDevice();
		s_queue = dev->GetGraphicsQueue();

		if (!CreateAhbImage(s_frame[0], extent, format) || !CreateAhbImage(s_frame[1], extent, format))
			return FailInitialize("could not allocate the shared frame images");
		s_generated.resize(multiplier - 1);
		for (u32 i = 0; i < multiplier - 1; i++)
		{
			if (!CreateAhbImage(s_generated[i], extent, format))
				return FailInitialize("could not allocate the interpolated frame images");
		}

		const VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
			VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, dev->GetGraphicsQueueFamilyIndex()};
		if (vkCreateCommandPool(s_device, &pool_info, nullptr, &s_cmd_pool) != VK_SUCCESS)
			return FailInitialize("vkCreateCommandPool failed");

		{
			// One pre-copy plus one post-copy per interpolated frame.
			std::vector<VkCommandBuffer> buffers(multiplier);
			const VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
				s_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, multiplier};
			if (vkAllocateCommandBuffers(s_device, &alloc, buffers.data()) != VK_SUCCESS)
				return FailInitialize("vkAllocateCommandBuffers failed");
			s_pre_copy_cmd = buffers[0];
			s_post_copy_cmds.assign(buffers.begin() + 1, buffers.end());
		}

		{
			const VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			bool ok = vkCreateSemaphore(s_device, &sem_info, nullptr, &s_pre_copy_sem) == VK_SUCCESS;
			s_post_copy_sems.assign(multiplier - 1, VK_NULL_HANDLE);
			s_acquire_sems.assign(multiplier - 1, VK_NULL_HANDLE);
			for (u32 i = 0; ok && i < multiplier - 1; i++)
			{
				ok = vkCreateSemaphore(s_device, &sem_info, nullptr, &s_post_copy_sems[i]) == VK_SUCCESS &&
					 vkCreateSemaphore(s_device, &sem_info, nullptr, &s_acquire_sems[i]) == VK_SUCCESS;
			}
			if (!ok)
				return FailInitialize("vkCreateSemaphore failed");
		}

		// The extractor throws — pe-parse failures, a DLL missing shaders, a malformed DXBC. It
		// is the one part of bring-up that runs user-supplied data, so it is also the part most
		// likely to fail, and it must degrade to "feature off" rather than take the GS thread.
		try
		{
			ExtractShaders();
		}
		catch (const std::exception& ex)
		{
			return FailInitialize(ex.what());
		}
		catch (...)
		{
			return FailInitialize("Lossless.dll could not be read");
		}

		const VkPhysicalDeviceProperties& props = dev->GetDeviceProperties();
		const u64 device_uuid = (static_cast<u64>(props.vendorID) << 32) | props.deviceID;
		if (s_backend.initialize(device_uuid, multiplier - 1, ShaderCallback, nullptr) != 0)
			return FailInitialize(BackendError());

		std::vector<AHardwareBuffer*> outputs;
		outputs.reserve(s_generated.size());
		for (const AhbImage& img : s_generated)
			outputs.push_back(img.ahb);

		s_context_id = s_backend.create_context(s_frame[0].ahb, s_frame[1].ahb, outputs.data(),
			static_cast<u32>(outputs.size()), extent.width, extent.height, static_cast<u32>(format));
		if (s_context_id < 0)
			return FailInitialize(BackendError());

		s_extent = extent;
		s_format = format;
		s_multiplier = multiplier;
		s_frame_index = 0;
		s_active = true;
		Console.WriteLnFmt("@@ANDROID_LSFG@@ active: {}x{} x{} frames", extent.width, extent.height, multiplier);
		return true;
	}

	bool PresentWithGeneration(VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished)
	{
		if (!s_active || !swap_chain)
			return false;

		// A resize between Initialize and here would have us copying between mismatched extents.
		// Decline the frame; the caller presents normally and the next Initialize picks up the
		// new size.
		if (swap_chain->GetWidth() != s_extent.width || swap_chain->GetHeight() != s_extent.height)
			return false;

		const u32 real_index = swap_chain->GetCurrentImageIndex();
		VkImage real_image = swap_chain->GetCurrentTexture()->GetImage();

		// The first frame has no predecessor to interpolate from: seed one slot and present it
		// plainly. Generation starts on the frame after.
		const bool have_previous = s_frame_index > 0;
		AhbImage& target = s_frame[s_frame_index % 2];

		// 1. Copy the frame the caller just rendered into our shared storage. It is in
		//    PRESENT_SRC because EndPresent transitioned it there, and it has to stay that way
		//    for the final present below.
		const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
		vkResetCommandBuffer(s_pre_copy_cmd, 0);
		if (vkBeginCommandBuffer(s_pre_copy_cmd, &begin) != VK_SUCCESS)
			return false;
		RecordCopy(s_pre_copy_cmd, real_image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, target.image,
			VK_IMAGE_LAYOUT_GENERAL, s_extent);
		if (vkEndCommandBuffer(s_pre_copy_cmd) != VK_SUCCESS)
			return false;

		// Past this point the caller's semaphore is consumed and there is no way back to its own
		// present path, so every later failure must still present something.
		if (!SubmitOneShot(s_pre_copy_cmd, render_finished, s_pre_copy_sem))
			return false;

		s_frame_index++;

		if (!have_previous)
		{
			// Nothing to interpolate from yet — present the real frame, waiting on the copy so
			// the shared image is complete before the next frame reads it.
			const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &s_pre_copy_sem, 1,
				swap_chain->GetSwapChainPtr(), &real_index, nullptr};
			swap_chain->ResetImageAcquireResult();
			vkQueuePresentKHR(present_queue, &present);
			return true;
		}

		// 2. Hand both frames to the interpolator, then block until it is done. There is no
		//    cross-device semaphore on Android — framegen runs on its own VkDevice and Turnip
		//    rejects OPAQUE_FD export on AHB-imported memory — so a device idle is the only
		//    barrier that exists. This is why frame generation costs latency here rather than
		//    being free.
		vkQueueWaitIdle(s_queue); // our pre-copy must land before framegen reads that image
		const bool generated = (s_backend.present(s_context_id) == 0);
		if (!generated)
			Console.ErrorFmt("@@ANDROID_LSFG@@ generation failed: {}", BackendError());
		else
			s_backend.wait_idle();

		// 3. Present each interpolated frame, then the real one last — the generated frames sit
		//    between the previous real frame and this one, so they display first.
		VkSemaphore wait_for_real = s_pre_copy_sem;
		if (generated)
		{
			for (u32 i = 0; i < s_multiplier - 1; i++)
			{
				u32 image_index = 0;
				const VkResult acq = vkAcquireNextImageKHR(s_device, swap_chain->GetSwapChain(), UINT64_MAX,
					s_acquire_sems[i], VK_NULL_HANDLE, &image_index);
				if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
					break; // out of date or lost — drop the generated frames, still present the real one

				vkResetCommandBuffer(s_post_copy_cmds[i], 0);
				if (vkBeginCommandBuffer(s_post_copy_cmds[i], &begin) != VK_SUCCESS)
					break;
				RecordCopy(s_post_copy_cmds[i], s_generated[i].image, VK_IMAGE_LAYOUT_GENERAL,
					swap_chain->GetImage(image_index), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, s_extent);
				if (vkEndCommandBuffer(s_post_copy_cmds[i]) != VK_SUCCESS)
					break;

				if (!SubmitOneShot(s_post_copy_cmds[i], s_acquire_sems[i], s_post_copy_sems[i]))
					break;

				const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1,
					&s_post_copy_sems[i], 1, swap_chain->GetSwapChainPtr(), &image_index, nullptr};
				if (vkQueuePresentKHR(present_queue, &present) != VK_SUCCESS)
					break;
				wait_for_real = s_post_copy_sems[i];
			}
		}

		// 4. The real frame goes out last, after whatever generated frames made it.
		const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &wait_for_real, 1,
			swap_chain->GetSwapChainPtr(), &real_index, nullptr};
		swap_chain->ResetImageAcquireResult();
		vkQueuePresentKHR(present_queue, &present);
		return true;
	}
} // namespace GSLsfg

#endif // ARMSX2_HAS_LSFG
