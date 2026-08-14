// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// C ABI for libarmsx2_lsfg.so — the frame-generation backend, isolated in its own shared
// object and reached by dlopen/dlsym from GSLsfg.cpp.
//
// WHY A SEPARATE .so, AND WHY C
//
// LSFG's framegen links volk, which DEFINES 759 globals named vkCreateImage, vkQueueSubmit,
// and so on — exactly the names PCSX2's own VKLoader.cpp defines. Linking them into one
// library is a duplicate-symbol error at best. At worst the linker merges them and
// framegen's volkLoadDevice() call, which it makes against ITS OWN VkDevice, silently
// repoints every entry point the GS renderer uses. Every subsequent vkCmdDraw would go to
// the wrong device, and it would present as a driver crash with nothing pointing back here.
// A separate shared object gives volk its own copy of those globals, which is the only
// arrangement where both loaders can coexist.
//
// The interface is C because the app builds with ANDROID_STL=c++_static: each .so carries
// its own libc++, so an std::vector or std::function crossing this boundary would be two
// unrelated types that happen to share a name. Nothing here is richer than a pointer, an
// integer, or a function pointer.
//
// Errors come back as codes, never exceptions. framegen throws (LSFG::vulkan_error) and so
// does the DXBC translator; the shim catches everything and stores the message for
// armsx2_lsfg_last_error(). An exception must never reach the unwinder in the caller's
// shared object.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AHardwareBuffer;

#define ARMSX2_LSFG_ABI_VERSION 1

/// Fill `out_data`/`out_size` with SPIR-V for the shader framegen asked for by `name`.
/// The buffer must stay valid until the next call to this callback or until initialise
/// returns, whichever comes first — framegen copies it into a VkShaderModule immediately.
/// Return 0 on success, non-zero to fail the initialise.
typedef int (*armsx2_lsfg_shader_fn)(
	void* user, const char* name, const uint8_t** out_data, uint32_t* out_size);

/// ABI version this .so was built against; mismatch means do not use it.
typedef uint32_t (*pfn_armsx2_lsfg_abi_version)(void);

/// Bring the library up. `generation_count` is the number of frames to interpolate between
/// each pair of real ones (multiplier - 1). Returns 0 on success.
typedef int (*pfn_armsx2_lsfg_initialize)(
	uint64_t device_uuid, uint32_t generation_count, armsx2_lsfg_shader_fn loader, void* user);

/// Create the interpolation context over caller-owned AHardwareBuffers. `format` is a
/// VkFormat. Returns the context id, or -1 on failure.
typedef int32_t (*pfn_armsx2_lsfg_create_context)(struct AHardwareBuffer* in0,
	struct AHardwareBuffer* in1, struct AHardwareBuffer* const* outs, uint32_t out_count,
	uint32_t width, uint32_t height, uint32_t format);

/// Run generation for one frame. Returns 0 on success.
typedef int (*pfn_armsx2_lsfg_present)(int32_t context);

/// Block until the library's own device is idle. On Android this is the only barrier that
/// exists between its device and ours — there is no exportable cross-device semaphore.
typedef void (*pfn_armsx2_lsfg_wait_idle)(void);

typedef void (*pfn_armsx2_lsfg_delete_context)(int32_t context);
typedef void (*pfn_armsx2_lsfg_finalize)(void);

/// Message from the most recent failure on this thread, or "" if there was none. Valid until
/// the next failing call on the same thread.
typedef const char* (*pfn_armsx2_lsfg_last_error)(void);

#ifdef __cplusplus
} // extern "C"
#endif
