// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Implementation of the C ABI declared in armsx2_lsfg_shim.h. Compiled ONLY into
// libarmsx2_lsfg.so, alongside framegen and its private copy of volk. See the header for why
// this boundary exists at all.

#include "armsx2_lsfg_shim.h"

#include "lsfg_3_1.hpp"

#include <exception>
#include <string>
#include <vector>

#define LSFG_EXPORT extern "C" __attribute__((visibility("default")))

namespace
{
	// Thread-local so a failure on the GS thread cannot be overwritten by one elsewhere, and so
	// the returned pointer stays valid without any locking on the reader's side.
	thread_local std::string t_last_error;

	void ClearError() { t_last_error.clear(); }
	void SetError(const char* what) { t_last_error = what ? what : "unknown error"; }
} // namespace

LSFG_EXPORT uint32_t armsx2_lsfg_abi_version(void)
{
	return ARMSX2_LSFG_ABI_VERSION;
}

LSFG_EXPORT const char* armsx2_lsfg_last_error(void)
{
	return t_last_error.c_str();
}

LSFG_EXPORT int armsx2_lsfg_initialize(
	uint64_t device_uuid, uint32_t generation_count, armsx2_lsfg_shader_fn loader, void* user)
{
	ClearError();
	if (!loader)
	{
		SetError("no shader loader supplied");
		return -1;
	}
	try
	{
		// The std::function wrapper is constructed HERE, on this side of the boundary, so the
		// std::vector it returns is allocated and freed by the same libc++ that framegen uses.
		// Building it on the caller's side would hand framegen a vector from a different
		// allocator — the exact hazard this shim exists to prevent.
		LSFG_3_1::initialize(device_uuid, /*isHdr*/ false, /*flowScale*/ 1.0f, generation_count,
			[loader, user](const std::string& name) -> std::vector<uint8_t> {
				const uint8_t* data = nullptr;
				uint32_t size = 0;
				if (loader(user, name.c_str(), &data, &size) != 0 || !data || size == 0)
					throw std::runtime_error("shader '" + name + "' could not be loaded");
				return std::vector<uint8_t>(data, data + size);
			});
		return 0;
	}
	catch (const std::exception& ex)
	{
		SetError(ex.what());
		return -1;
	}
	catch (...)
	{
		SetError("unknown exception during initialise");
		return -1;
	}
}

LSFG_EXPORT int32_t armsx2_lsfg_create_context(AHardwareBuffer* in0, AHardwareBuffer* in1,
	AHardwareBuffer* const* outs, uint32_t out_count, uint32_t width, uint32_t height, uint32_t format)
{
	ClearError();
	try
	{
		std::vector<AHardwareBuffer*> out_vec(outs, outs + out_count);
		const VkExtent2D extent = {width, height};
		return LSFG_3_1::createContextFromAHB(in0, in1, out_vec, extent, static_cast<VkFormat>(format));
	}
	catch (const std::exception& ex)
	{
		SetError(ex.what());
		return -1;
	}
	catch (...)
	{
		SetError("unknown exception creating the context");
		return -1;
	}
}

LSFG_EXPORT int armsx2_lsfg_present(int32_t context)
{
	ClearError();
	try
	{
		// No semaphores: on Android framegen has no exportable cross-device semaphore (Turnip
		// rejects OPAQUE_FD on AHB-imported memory), so the caller brackets this with idle waits
		// instead. Passing -1 and an empty list is upstream's own Android path.
		LSFG_3_1::presentContext(context, -1, {});
		return 0;
	}
	catch (const std::exception& ex)
	{
		SetError(ex.what());
		return -1;
	}
	catch (...)
	{
		SetError("unknown exception during generation");
		return -1;
	}
}

LSFG_EXPORT void armsx2_lsfg_wait_idle(void)
{
	try
	{
		LSFG_3_1::waitIdle();
	}
	catch (...)
	{
		// A failed idle leaves nothing for the caller to do differently, and this is on the
		// teardown path where throwing would be worse than continuing.
	}
}

LSFG_EXPORT void armsx2_lsfg_delete_context(int32_t context)
{
	try
	{
		LSFG_3_1::deleteContext(context);
	}
	catch (...)
	{
	}
}

LSFG_EXPORT void armsx2_lsfg_finalize(void)
{
	try
	{
		LSFG_3_1::finalize();
	}
	catch (...)
	{
	}
}
