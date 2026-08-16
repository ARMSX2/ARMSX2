// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include "Config.h"
#include "GS/GS.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

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
#include <map>
#include <optional>
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

		// The structural PE check reads the file, and GetUnavailableReason() runs once per frame
		// from EndPresent while the feature is on — so without this the GS thread did an
		// fopen/fread/fseek/fread/fclose on the present path every single frame. The verdict can
		// only change when the path does, which is exactly when SetDllPath() clears it.
		std::atomic<bool> s_dll_checked{false};
		std::atomic<bool> s_dll_ok{false};

		// What the overlay reports. Written from the GS thread in the present path, read from
		// whichever thread draws the OSD, so both are atomic rather than mutex'd — a recent
		// value is all a status line needs.
		//
		// s_no_shaders separates "your DLL has no usable shader family" from every other way
		// initialisation can fail. Both are InitFailed to the settings screen, but they are
		// different problems: one is fixed by updating Lossless Scaling, the other is not.
		std::atomic<float> s_display_fps{0.0f};
		std::atomic<bool> s_no_shaders{false};
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
		s_dll_checked.store(false, std::memory_order_relaxed);
		s_no_shaders.store(false, std::memory_order_relaxed);
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
		if (!s_dll_checked.load(std::memory_order_acquire))
		{
			s_dll_ok.store(LooksLikeLosslessDll(s_dll_path), std::memory_order_relaxed);
			s_dll_checked.store(true, std::memory_order_release);
		}
		if (!s_dll_ok.load(std::memory_order_relaxed))
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

	float GetDisplayFPS() { return s_display_fps.load(std::memory_order_relaxed); }

	std::string GetStatusText()
	{
		// Nothing at all when the user has not asked for frame generation — an overlay line for a
		// feature nobody switched on is just clutter. Every OTHER state says something, including
		// the ones where nothing is wrong yet, because "on but silent" is indistinguishable from
		// "on and broken" and that is precisely the failure this exists to prevent.
		if (!GSConfig.LsfgEnabled)
			return {};

		switch (GetUnavailableReason())
		{
			case Unavailable::Available:
				break;
			case Unavailable::InitFailed:
				// Split out because the two have different fixes: "no shaders" means update
				// Lossless Scaling, "failed" means this device or driver refused.
				return s_no_shaders.load(std::memory_order_relaxed) ? "LSFG: no shaders" : "LSFG: failed";
			default:
				return "LSFG: unavailable";
		}

		// Available but no window has closed yet: bring-up, or the first second of a session.
		const float fps = s_display_fps.load(std::memory_order_relaxed);
		if (fps <= 0.0f)
			return "LSFG: starting";
		return fmt::format("LSFG: {:.2f}", fps);
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
	bool PresentWithGeneration(VkQueue, VKSwapChain*, VkSemaphore, bool) { return false; }
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
			// v2 signature: is_hdr / flow_scale / performance joined the argument list, which is
			// exactly why the ABI check below exists — the old layout would misread every one.
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

		// name -> SPIR-V, translated once and then held. The DXBC below is scratch: it exists
		// only while a DLL is being read and is dropped the moment every shader is translated.
		//
		// This used to hold DXBC instead, with the translation done inside ShaderCallback — so
		// all 26 (now 52) DXBC->SPIR-V compiles ran on the GS thread, inside EndPresent, on the
		// first frame after every enable, resolution change and multiplier change.
		std::map<std::string, std::vector<u8>> s_shader_spirv;
		std::unordered_map<u32, std::vector<u8>> s_shader_blobs;
		// Which DLL s_shader_spirv was built from. Without it, picking a different Lossless.dll
		// kept serving the previous file's shaders for the rest of the session.
		std::string s_shader_source;
		// Which families that DLL turned out to carry. A given Lossless Scaling version ships
		// one or the other, so this is what lets the 3.1p request fall back instead of failing.
		bool s_have_standard = false;
		bool s_have_performance = false;

		int OnResource(void*, const peparse::resource& res)
		{
			if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0)
				return 0;
			std::vector<u8> data(static_cast<size_t>(res.buf->bufLen));
			std::copy_n(res.buf->buf, res.buf->bufLen, data.data());
			s_shader_blobs[res.name] = std::move(data);
			return 0;
		}

		/// True for the LSFG 3.1p (performance) family. The p_ prefix is upstream's own naming.
		bool IsPerformanceShader(const std::string& name) { return name.compare(0, 2, "p_") == 0; }

		// Resource IDs, from upstream's extract.cpp — they track Lossless Scaling's own resource
		// layout. Only the names framegen asks for are listed; anything else fails the load
		// cleanly rather than feeding it a wrong shader.
		//
		// Two families: the plain names are LSFG 3.1 and the p_ prefixed ones are 3.1p, the
		// lighter pipeline. p_mipmaps and p_generate deliberately share 255/256 with 3.1 — those
		// two resources are common to both, so they are extracted twice under the two names
		// framegen asks for rather than special-cased.
		const std::map<std::string, u32>& ShaderNameTable()
		{
			static const std::map<std::string, u32> table = {
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
				{"p_mipmaps", 255},
				{"p_alpha[0]", 290}, {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293},
				{"p_beta[0]", 298}, {"p_beta[1]", 299}, {"p_beta[2]", 300}, {"p_beta[3]", 301},
				{"p_beta[4]", 302},
				{"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284},
				{"p_gamma[4]", 285},
				{"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287}, {"p_delta[3]", 288},
				{"p_delta[4]", 289}, {"p_delta[5]", 281}, {"p_delta[6]", 294}, {"p_delta[7]", 295},
				{"p_delta[8]", 296}, {"p_delta[9]", 297},
				{"p_generate", 256},
			};
			return table;
		}

		// --- the SPIR-V cache ------------------------------------------------------------------
		//
		// Translated SPIR-V only, never the DLL — that file is the user's own property and stays
		// where they put it. Reading it back skips both the PE walk and 52 DXBC compiles.

		constexpr u32 k_cache_magic = 0x4746534Cu; // "LSFG"
		constexpr u32 k_cache_version = 1;
		// Bounds on what the file may claim, so a truncated or garbage cache stops cleanly at the
		// first bad field instead of trying to allocate whatever the bytes happened to say.
		constexpr u32 k_max_name_len = 64;
		constexpr u32 k_max_shader_size = 4u * 1024u * 1024u;
		constexpr u32 k_max_shader_count = 256;

		std::string ShaderCachePath() { return Path::Combine(EmuFolders::Cache, "lsfg_shaders.bin"); }

		void AppendU32(std::vector<u8>& out, u32 value)
		{
			out.insert(out.end(), reinterpret_cast<const u8*>(&value), reinterpret_cast<const u8*>(&value) + 4);
		}

		void AppendU64(std::vector<u8>& out, u64 value)
		{
			out.insert(out.end(), reinterpret_cast<const u8*>(&value), reinterpret_cast<const u8*>(&value) + 8);
		}

		/// Size and mtime of the file the cache was built from. Upstream's own equivalent has no
		/// invalidation at all: update Lossless Scaling and the stale shaders are used forever,
		/// silently. We keep the DLL, so we can just ask.
		bool StatSourceDll(u64* size, u64* mtime)
		{
			FILESYSTEM_STAT_DATA sd = {};
			if (!FileSystem::StatFile(s_dll_path.c_str(), &sd))
				return false;
			*size = static_cast<u64>(sd.Size);
			*mtime = static_cast<u64>(sd.ModificationTime);
			return true;
		}

		void SaveShaderCache()
		{
			u64 dll_size = 0, dll_mtime = 0;
			if (!StatSourceDll(&dll_size, &dll_mtime))
				return; // no way to invalidate it later, so do not write one

			std::vector<u8> out;
			AppendU32(out, k_cache_magic);
			AppendU32(out, k_cache_version);
			AppendU64(out, dll_size);
			AppendU64(out, dll_mtime);
			AppendU32(out, static_cast<u32>(s_shader_spirv.size()));
			for (const auto& [name, spirv] : s_shader_spirv)
			{
				AppendU32(out, static_cast<u32>(name.size()));
				out.insert(out.end(), name.begin(), name.end());
				AppendU32(out, static_cast<u32>(spirv.size()));
				out.insert(out.end(), spirv.begin(), spirv.end());
			}

			const std::string path = ShaderCachePath();
			if (!FileSystem::WriteBinaryFile(path.c_str(), out.data(), out.size()))
			{
				Console.WarningFmt("@@ANDROID_LSFG@@ could not write {} — shaders will be translated again next time", path);
				return;
			}
			Console.WriteLnFmt("@@ANDROID_LSFG@@ cached {} translated shaders", s_shader_spirv.size());
		}

		/// Fills s_shader_spirv from disk. False for every ordinary reason a cache is not usable
		/// — absent, from another build, or from a DLL the user has since replaced — none of
		/// which is an error, they just mean "extract".
		bool LoadShaderCache()
		{
			u64 dll_size = 0, dll_mtime = 0;
			if (!StatSourceDll(&dll_size, &dll_mtime))
				return false;

			const std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(ShaderCachePath().c_str());
			if (!data.has_value())
				return false;

			const u8* p = data->data();
			size_t left = data->size();
			const auto read_u32 = [&p, &left](u32* value) {
				if (left < 4)
					return false;
				std::memcpy(value, p, 4);
				p += 4;
				left -= 4;
				return true;
			};
			const auto read_u64 = [&p, &left](u64* value) {
				if (left < 8)
					return false;
				std::memcpy(value, p, 8);
				p += 8;
				left -= 8;
				return true;
			};

			u32 magic = 0, version = 0, count = 0;
			u64 cached_size = 0, cached_mtime = 0;
			if (!read_u32(&magic) || !read_u32(&version) || !read_u64(&cached_size) ||
				!read_u64(&cached_mtime) || !read_u32(&count))
				return false;
			if (magic != k_cache_magic || version != k_cache_version)
				return false;
			if (cached_size != dll_size || cached_mtime != dll_mtime)
			{
				Console.WriteLn("@@ANDROID_LSFG@@ Lossless.dll changed since the shader cache was written");
				return false;
			}
			if (count == 0 || count > k_max_shader_count)
				return false;

			std::map<std::string, std::vector<u8>> loaded;
			for (u32 i = 0; i < count; i++)
			{
				u32 name_len = 0, size = 0;
				if (!read_u32(&name_len) || name_len == 0 || name_len > k_max_name_len || left < name_len)
					break;
				std::string name(reinterpret_cast<const char*>(p), name_len);
				p += name_len;
				left -= name_len;

				if (!read_u32(&size) || size == 0 || size > k_max_shader_size || left < size)
					break;
				loaded[std::move(name)].assign(p, p + size);
				p += size;
				left -= size;
			}

			if (loaded.size() != count)
			{
				Console.Warning("@@ANDROID_LSFG@@ shader cache is truncated — translating again");
				return false;
			}

			s_shader_spirv = std::move(loaded);
			Console.WriteLnFmt("@@ANDROID_LSFG@@ restored {} shaders from the cache", s_shader_spirv.size());
			return true;
		}

		/// Note which families the loaded SPIR-V actually covers, and reject a set that covers
		/// neither. "Every listed name present" was right when only 3.1 existed and is wrong now:
		/// a DLL legitimately ships one family, so requiring both would reject every one of them.
		/// A HALF-present family must still fail here, though, rather than inside framegen's
		/// initialise where the only message is "Shader hash not found".
		void ClassifyShaderFamilies()
		{
			s_have_standard = true;
			s_have_performance = true;
			for (const auto& [name, idx] : ShaderNameTable())
			{
				if (s_shader_spirv.find(name) != s_shader_spirv.end())
					continue;
				if (IsPerformanceShader(name))
					s_have_performance = false;
				else
					s_have_standard = false;
			}
		}

		/// Pull every RCDATA resource out of the user's DLL, translate the ones framegen asks for,
		/// and keep only the SPIR-V. Throws with a message the settings screen can show verbatim.
		void ExtractShaders()
		{
			// A different pick invalidates what is held, and holding it anyway is how the old
			// code served the previous DLL's shaders after the user replaced the file.
			if (s_shader_source != s_dll_path)
				s_shader_spirv.clear();
			if (!s_shader_spirv.empty())
				return;

			s_shader_source = s_dll_path;
			const bool from_cache = LoadShaderCache();
			if (!from_cache)
			{
				// A previous attempt that threw before the clear at the bottom would otherwise
				// leave its resources here to be merged with this DLL's.
				s_shader_blobs.clear();
				peparse::parsed_pe* dll = peparse::ParsePEFromFile(s_dll_path.c_str());
				if (!dll)
					throw std::runtime_error("could not read Lossless.dll");
				peparse::IterRsrc(dll, OnResource, nullptr);
				peparse::DestructParsedPE(dll);

				// Eagerly, and here rather than in the callback: this is the one point in the
				// feature's life where a multi-hundred-millisecond stall is acceptable, and the
				// callback runs inside a present.
				for (const auto& [name, idx] : ShaderNameTable())
				{
					const auto blob = s_shader_blobs.find(idx);
					if (blob == s_shader_blobs.end())
						continue; // the other family; ClassifyShaderFamilies decides if that matters
					// Individually guarded because a resource id shared between the families can
					// be present while its sibling shaders are not, and one bad translation must
					// not lose the family that did translate.
					try
					{
						std::vector<u8> spirv = Extract::translateShader(blob->second);
						if (!spirv.empty())
							s_shader_spirv[name] = std::move(spirv);
					}
					catch (const std::exception& ex)
					{
						Console.ErrorFmt("@@ANDROID_LSFG@@ shader '{}' failed to translate: {}", name, ex.what());
					}
				}
				// The DXBC has done its job. It is several megabytes and nothing reads it again.
				s_shader_blobs.clear();
			}

			ClassifyShaderFamilies();
			if (!s_have_standard && !s_have_performance)
			{
				s_shader_spirv.clear();
				s_shader_source.clear();
				s_no_shaders.store(true, std::memory_order_relaxed);
				throw std::runtime_error(
					"Lossless.dll has no complete shader set — is Lossless Scaling up to date?");
			}
			s_no_shaders.store(false, std::memory_order_relaxed);
			if (!from_cache)
				SaveShaderCache();
		}

		/// The C callback framegen drives during initialise. A lookup and nothing else — the
		/// translation happened in ExtractShaders. The returned pointer is into s_shader_spirv,
		/// which outlives the whole initialise, so it comfortably satisfies the shim's
		/// valid-until-the-next-call contract. Still guarded: an exception must not unwind
		/// through the shim's shared object, whatever the reason for it.
		int ShaderCallback(void*, const char* name, const uint8_t** out_data, uint32_t* out_size)
		{
			try
			{
				const auto hit = s_shader_spirv.find(name);
				if (hit == s_shader_spirv.end() || hit->second.empty())
				{
					Console.ErrorFmt(
						"@@ANDROID_LSFG@@ framegen asked for shader '{}', which this DLL does not have", name);
					return -1;
				}
				*out_data = hit->second.data();
				*out_size = static_cast<uint32_t>(hit->second.size());
				return 0;
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
		// What the SETTING said at the last successful bring-up, not the family that ended up
		// running — the two differ when a DLL ships only one, and comparing the resolved value
		// against the setting would tear the whole thing down and rebuild it every frame.
		bool s_performance_requested = false;
		u8 s_flow_scale_percent = 100;
		VkExtent2D s_extent = {};
		VkFormat s_format = VK_FORMAT_UNDEFINED;
		u64 s_frame_index = 0;

		// The one-second display-rate window. Reset with everything else in Shutdown so a stale
		// number cannot outlive the session it came from.
		u64 s_fps_window_start = 0;
		u32 s_fps_real = 0;
		u32 s_fps_generated = 0;

		/// Book frames as they reach the presentation engine and republish the rate once a
		/// second. Called on the declined paths too, with nothing generated: a real frame still
		/// went out, and a counter that stops updating whenever generation is skipped would sit
		/// on its last value through an entire pause menu.
		void NoteFramesDisplayed(u32 real, u32 generated)
		{
			s_fps_real += real;
			s_fps_generated += generated;

			const u64 now = Common::Timer::GetCurrentValue();
			if (s_fps_window_start == 0)
			{
				s_fps_window_start = now;
				return;
			}
			const double secs = Common::Timer::ConvertValueToSeconds(now - s_fps_window_start);
			if (secs < 1.0)
				return;

			s_display_fps.store(static_cast<float>((s_fps_real + s_fps_generated) / secs), std::memory_order_relaxed);
			s_fps_window_start = now;
			s_fps_real = 0;
			s_fps_generated = 0;
		}

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
		s_performance_requested = false;
		s_flow_scale_percent = 100;
		s_extent = {};
		s_format = VK_FORMAT_UNDEFINED;
		s_frame_index = 0;
		s_device = VK_NULL_HANDLE;
		s_physical_device = VK_NULL_HANDLE;
		s_queue = VK_NULL_HANDLE;

		s_display_fps.store(0.0f, std::memory_order_relaxed);
		s_fps_window_start = 0;
		s_fps_real = 0;
		s_fps_generated = 0;
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
		const u8 flow_scale_percent = std::clamp<u8>(GSConfig.LsfgFlowScale, 25, 100);
		const VkExtent2D extent = {swap_chain->GetWidth(), swap_chain->GetHeight()};
		const VkFormat format = swap_chain->GetTextureFormat();

		if (s_active && extent.width == s_extent.width && extent.height == s_extent.height &&
			format == s_format && multiplier == s_multiplier &&
			GSConfig.LsfgPerformance == s_performance_requested && flow_scale_percent == s_flow_scale_percent)
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

		// 3.1p when the user asked for it AND their DLL carries it. A version that predates the
		// performance family would otherwise fail initialise with "Shader hash not found", which
		// reads like a corrupt file and is not — it just means this Lossless Scaling is older.
		bool use_performance = GSConfig.LsfgPerformance;
		if (use_performance && !s_have_performance)
		{
			Console.WriteLn("@@ANDROID_LSFG@@ this Lossless.dll has no 3.1p shaders — using 3.1");
			use_performance = false;
		}
		else if (!use_performance && !s_have_standard)
		{
			Console.WriteLn("@@ANDROID_LSFG@@ this Lossless.dll has only 3.1p shaders — using 3.1p");
			use_performance = true;
		}

		// ★ flowScale is a DIVISOR, not a multiplier: framegen sizes the optical-flow pyramid as
		// `inputExtent / flowScale`, and upstream's own layer reaches it by passing
		// `1.0 / conf.flowScale` from a [0.25, 1.0] fraction. So the percentage the UI shows has
		// to be INVERTED here. Handing it 0.25 for "25%" would make the pyramid four times larger
		// per axis — sixteen times the pixels — which is the exact opposite of what a user
		// dragging that slider down is asking for.
		const float flow_scale = std::clamp(100.0f / static_cast<float>(flow_scale_percent), 1.0f, 4.0f);

		const VkPhysicalDeviceProperties& props = dev->GetDeviceProperties();
		const u64 device_uuid = (static_cast<u64>(props.vendorID) << 32) | props.deviceID;
		// is_hdr is false and stays false: it tells framegen its images carry HDR primaries, and
		// there is no HDR output path in ARMSX2 for that to be true against — the swapchain is
		// the 8-bit UNORM or 16-bit float surface CreateAhbImage already restricts us to.
		if (s_backend.initialize(device_uuid, /*is_hdr*/ 0, flow_scale, multiplier - 1,
				use_performance ? 1 : 0, ShaderCallback, nullptr) != 0)
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
		s_performance_requested = GSConfig.LsfgPerformance;
		s_flow_scale_percent = flow_scale_percent;
		s_frame_index = 0;
		s_active = true;
		Console.WriteLnFmt("@@ANDROID_LSFG@@ active: {}x{} x{} frames, {}, flow {}%", extent.width, extent.height,
			multiplier, use_performance ? "3.1p" : "3.1", flow_scale_percent);
		return true;
	}

	bool PresentWithGeneration(
		VkQueue present_queue, VKSwapChain* swap_chain, VkSemaphore render_finished, bool frame_has_new_content)
	{
		if (!s_active || !swap_chain)
			return false;

		// Nothing new to interpolate between. Pause menus, boot screens before the GS has any
		// output, and the blank frames a fade produces all land here — inventing motion across
		// them is wrong AND costs a full generation pass per frame to do it. The history goes
		// with them: keeping it would stitch the frame before the gap to the frame after it and
		// produce one bogus in-between frame on the way back.
		if (!frame_has_new_content)
		{
			s_frame_index = 0;
			NoteFramesDisplayed(1, 0);
			return false;
		}

		// A resize between Initialize and here would have us copying between mismatched extents.
		// Decline the frame; the caller presents normally and the next Initialize picks up the
		// new size.
		if (swap_chain->GetWidth() != s_extent.width || swap_chain->GetHeight() != s_extent.height)
		{
			NoteFramesDisplayed(1, 0);
			return false;
		}

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
			NoteFramesDisplayed(1, 0);
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
		//    between the previous real frame and this one, so they display first. Presents issued
		//    on one queue are processed in call order, which is what puts them on screen in that
		//    order without any semaphore between them.
		//
		// ★ EVERY binary semaphore below is signalled exactly once and waited exactly once, and
		// that is a correctness requirement, not tidiness. This loop used to reassign the real
		// present's wait to the last post-copy semaphore, which left s_pre_copy_sem signalled and
		// never waited — so the next frame signalled an already-signalled binary semaphore — while
		// the last post-copy semaphore was waited twice, by its own present and by the real one.
		//
		// The real present waits on s_pre_copy_sem and nothing else, for a concrete reason: the
		// pre-copy READS the real image as TRANSFER_SRC and puts it back in PRESENT_SRC, so
		// presenting it before that lands would present an image still being read. The generated
		// presents are independent — different swapchain images, each gated by its own post-copy.
		//
		// Counted rather than assumed to be s_multiplier - 1: every break below drops a frame that
		// was generated but never displayed, and the overlay is supposed to report what reached the
		// screen, not what we hoped would.
		u32 presented_generated = 0;
		if (generated)
		{
			for (u32 i = 0; i < s_multiplier - 1; i++)
			{
				u32 image_index = 0;
				// Zero timeout, deliberately. An interpolated frame is a bonus, so if the
				// presentation engine has nothing free right now the right move is to drop it and
				// get the real frame out — waiting costs more than the frame is worth. The old
				// UINT64_MAX was actively dangerous here on two counts: it bypassed the bounded
				// ACQUIRE_TIMEOUT_NS that VKSwapChain::AcquireNextImage adopted so a surface
				// destroyed under the GS thread (background/rotate/fold) does not wedge every
				// thread at 0% CPU with no log; and with a 2- or 3-image swapchain this loop
				// holds the real image while asking for multiplier-1 more, so at x3/x4 it
				// serialised on a vblank per generated frame and ate the very time the feature
				// exists to spend. VK_NOT_READY/VK_TIMEOUT leave the semaphore unsignalled, so
				// s_acquire_sems[i] stays clean for the next frame.
				const VkResult acq = vkAcquireNextImageKHR(s_device, swap_chain->GetSwapChain(), 0,
					s_acquire_sems[i], VK_NULL_HANDLE, &image_index);
				if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
					break; // nothing free, out of date, or lost — still present the real frame

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
				presented_generated++;
			}
		}

		// 4. The real frame goes out last, after whatever generated frames made it.
		const VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &s_pre_copy_sem, 1,
			swap_chain->GetSwapChainPtr(), &real_index, nullptr};
		swap_chain->ResetImageAcquireResult();
		vkQueuePresentKHR(present_queue, &present);
		NoteFramesDisplayed(1, presented_generated);
		return true;
	}
} // namespace GSLsfg

#endif // ARMSX2_HAS_LSFG
