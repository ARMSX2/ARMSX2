// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(ARMSX2_USE_ADRENOTOOLS)
#include <dlfcn.h>
#include <mutex>
#include "common/FileSystem.h"
#include "common/Path.h"
#include "adrenotools/driver.h"
#endif

extern "C" {

#define VULKAN_MODULE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) PFN_##name name;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) PFN_##name name;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

void Vulkan::ResetVulkanLibraryFunctionPointers()
{
#define VULKAN_MODULE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_INSTANCE_ENTRY_POINT(name, required) name = nullptr;
#define VULKAN_DEVICE_ENTRY_POINT(name, required) name = nullptr;
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT
#undef VULKAN_INSTANCE_ENTRY_POINT
#undef VULKAN_MODULE_ENTRY_POINT
}

static DynamicLibrary s_vulkan_library;

#if defined(ARMSX2_USE_ADRENOTOOLS)
namespace
{
	/// What SetCustomDriverPath was last told, read out under the lock in one go so no
	/// caller ever sees half of one configuration and half of another.
	struct CustomDriverRequest
	{
		std::string dir;
		std::string name;
		std::string redirect_dir;
		std::string hook_lib_dir;
		/// When true, failing to open this driver fails LoadVulkanLibrary instead of
		/// falling through to the system loader. See SetCustomDriverPath.
		bool required = false;

		bool IsSet() const { return !dir.empty() && !name.empty() && !hook_lib_dir.empty(); }
	};

	std::mutex s_custom_driver_mutex;
	CustomDriverRequest s_custom_driver;
} // namespace

void Vulkan::SetCustomDriverPath(const char* driver_dir, const char* driver_name,
	const char* redirect_dir, const char* hook_lib_dir, bool required)
{
	// adrenotools resolves the driver as a plain string concatenation of directory and
	// library name (driver.cpp's stat() call), so a directory without a trailing slash
	// silently becomes a sibling path that does not exist. The Kotlin side already
	// appends one; normalising here means every caller gets the same rule instead of
	// each remembering it.
	const auto with_trailing_slash = [](const char* dir) -> std::string {
		std::string out = dir ? dir : "";
		if (!out.empty() && out.back() != '/')
			out.push_back('/');
		return out;
	};

	std::lock_guard lock(s_custom_driver_mutex);
	s_custom_driver.dir          = with_trailing_slash(driver_dir);
	s_custom_driver.name         = driver_name ? driver_name : "";
	s_custom_driver.redirect_dir = with_trailing_slash(redirect_dir);
	s_custom_driver.hook_lib_dir = hook_lib_dir ? hook_lib_dir : "";
	s_custom_driver.required     = required;
}

static CustomDriverRequest GetCustomDriverRequest()
{
	std::lock_guard lock(s_custom_driver_mutex);
	return s_custom_driver;
}

/// One place decides how loudly a custom-driver failure is reported, because the two
/// callers want different volumes and the reason is the same either way.
static void ReportCustomDriverFailure(const CustomDriverRequest& request, const Error* error)
{
	const std::string description = error ? error->GetDescription() : std::string("custom driver load failed");
	if (request.required)
		Console.Error("VKLoader: %s", description.c_str());
	else
		Console.Warning("VKLoader: %s — falling back to system loader.", description.c_str());
}

static bool TryOpenAdrenotoolsDriver(DynamicLibrary& library, const CustomDriverRequest& request, Error* error)
{
	const std::string& driver_dir = request.dir;
	const std::string& driver_name = request.name;
	const std::string& redirect_dir = request.redirect_dir;
	const std::string& hook_lib_dir = request.hook_lib_dir;

	// adrenotools_open_libvulkan returns a bare null for about eight different reasons
	// and sets no dlerror for most of them, so a wrong path and a refused linker
	// namespace are indistinguishable from the outside. The three conditions worth
	// telling apart are cheap to check here, and the linker-namespace answer is the one
	// we actually want to learn on a device, so it must not be hidden behind a typo.
	const std::string driver_path = driver_dir + driver_name;
	if (!FileSystem::FileExists(driver_path.c_str()))
	{
		Error::SetStringFmt(error, "custom Vulkan driver '{}' does not exist.", driver_path);
		ReportCustomDriverFailure(request, error);
		return false;
	}
	for (const char* hook : {"libhook_impl.so", "libmain_hook.so"})
	{
		const std::string hook_path = Path::Combine(hook_lib_dir, hook);
		if (!FileSystem::FileExists(hook_path.c_str()))
		{
			Error::SetStringFmt(error, "adrenotools hook library '{}' does not exist.", hook_path);
			ReportCustomDriverFailure(request, error);
			return false;
		}
	}
	if (!redirect_dir.empty() && !FileSystem::DirectoryExists(redirect_dir.c_str()))
	{
		Error::SetStringFmt(error, "file-redirect directory '{}' does not exist.", redirect_dir);
		ReportCustomDriverFailure(request, error);
		return false;
	}

	int feature_flags = ADRENOTOOLS_DRIVER_CUSTOM;
	if (!redirect_dir.empty())
		feature_flags |= ADRENOTOOLS_DRIVER_FILE_REDIRECT;

	Console.WriteLn("VKLoader: opening custom Vulkan driver via adrenotools "
		"(dir=%s name=%s redirect=%s hook=%s)",
		driver_dir.c_str(), driver_name.c_str(),
		redirect_dir.empty() ? "<none>" : redirect_dir.c_str(),
		hook_lib_dir.c_str());

	// adrenotools_open_libvulkan takes tmpLibDir for API < 29 fallback; pass null to
	// use memfd which is fine on every modern device. driver_dir and redirect_dir carry
	// the trailing slash adrenotools' path resolution needs; SetCustomDriverPath put it
	// there.
	void* handle = adrenotools_open_libvulkan(
		RTLD_NOW, feature_flags,
		nullptr, // tmpLibDir (memfd path)
		hook_lib_dir.c_str(),
		driver_dir.c_str(),
		driver_name.c_str(),
		redirect_dir.empty() ? nullptr : redirect_dir.c_str(),
		nullptr // userMappingHandle (unused)
	);

	if (!handle)
	{
		// Everything checkable has been checked above, so reaching here means the
		// linker namespace, the hook preload or the /system/lib64/libvulkan.so reopen
		// refused -- which is the interesting failure and the one that cannot be
		// diagnosed from the return value.
		const char* err = dlerror();
		Error::SetStringFmt(error,
			"adrenotools_open_libvulkan refused {} ({}), with every path verified to exist: {}",
			driver_name, driver_dir, err ? err : "<no dlerror>");
		ReportCustomDriverFailure(request, error);
		return false;
	}

	library.Adopt(handle);
	Console.WriteLn("VKLoader: adrenotools driver handle acquired.");
	return true;
}
#endif

bool Vulkan::IsVulkanLibraryLoaded()
{
	return s_vulkan_library.IsOpen();
}

bool Vulkan::LoadVulkanLibrary(Error* error)
{
	pxAssertRel(!s_vulkan_library.IsOpen(), "Vulkan module is not loaded.");

#ifdef __APPLE__
	// Check if a path to a specific Vulkan library has been specified.
	char* libvulkan_env = getenv("LIBVULKAN_PATH");
	if (libvulkan_env)
		s_vulkan_library.Open(libvulkan_env, error);
	if (!s_vulkan_library.IsOpen() &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("MoltenVK").c_str(), error))
	{
		return false;
	}
#else
#if defined(ARMSX2_USE_ADRENOTOOLS)
	// User-picked custom driver (e.g. Mesa Turnip from K11MCH1/AdrenoToolsDrivers)
	// takes priority.
	const CustomDriverRequest custom_driver = GetCustomDriverRequest();
	if (custom_driver.IsSet() && TryOpenAdrenotoolsDriver(s_vulkan_library, custom_driver, error))
	{
		Error::Clear(error);
	}
	// A driver that was asked for and not obtained has two reasonable answers and the
	// caller picks. Interactively, falling through to the system loader means the boot
	// still proceeds and the user sees a picture. For a measurement run it means the
	// numbers describe the vendor driver while the command line says otherwise, and
	// nothing in the emulator log says so — so a required driver stops the load here
	// instead, with the adrenotools failure still in `error`.
	else if (custom_driver.IsSet() && custom_driver.required)
	{
		return false;
	}
	else
#endif
	// try versioned first, then unversioned.
	if (!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str(), error) &&
		!s_vulkan_library.Open(DynamicLibrary::GetVersionedFilename("vulkan").c_str(), error))
	{
		return false;
	}
#endif

	bool required_functions_missing = false;
#define VULKAN_MODULE_ENTRY_POINT(name, required) \
	if (!s_vulkan_library.GetSymbol(#name, &name)) \
	{ \
		ERROR_LOG("Vulkan: Failed to load required module function {}", #name); \
		required_functions_missing = true; \
	}

#include "VKEntryPoints.inl"
#undef VULKAN_MODULE_ENTRY_POINT

	if (required_functions_missing)
	{
		ResetVulkanLibraryFunctionPointers();
		s_vulkan_library.Close();
		return false;
	}

	return true;
}

void Vulkan::UnloadVulkanLibrary()
{
	ResetVulkanLibraryFunctionPointers();
	s_vulkan_library.Close();
}

bool Vulkan::LoadVulkanInstanceFunctions(VkInstance instance)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, instance](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetInstanceProcAddr(instance, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required instance function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_INSTANCE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_INSTANCE_ENTRY_POINT

	return !required_functions_missing;
}

bool Vulkan::LoadVulkanDeviceFunctions(VkDevice device)
{
	bool required_functions_missing = false;
	auto LoadFunction = [&required_functions_missing, device](PFN_vkVoidFunction* func_ptr, const char* name, bool is_required) {
		*func_ptr = vkGetDeviceProcAddr(device, name);
		if (!(*func_ptr) && is_required)
		{
			std::fprintf(stderr, "Vulkan: Failed to load required device function %s\n", name);
			required_functions_missing = true;
		}
	};

#define VULKAN_DEVICE_ENTRY_POINT(name, required) \
	LoadFunction(reinterpret_cast<PFN_vkVoidFunction*>(&name), #name, required);
#include "VKEntryPoints.inl"
#undef VULKAN_DEVICE_ENTRY_POINT

	return !required_functions_missing;
}
