// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_shaders.cpp.
// Logic unchanged apart from the fp16 gate, which PCSX2 cannot express — see below.
// See FrameGenTypes.h and LsfgVkCompat.h.

#include "LosslessDll.h"
#include "LsfgShaders.h"
#include "LsfgUtil.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

LsfgShaders::LsfgShaders(const Device& device) {
    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor()) {
        return;
    }

    // PORT: Eden reads device.IsFloat16Supported() and the frame_gen_fp16 user setting here.
    // Neither exists for us: the compat shim's Device answers three queries and float16 is not
    // one of them, and PCSX2 has no fp16 toggle in GSConfig. More to the point, PCSX2's Vulkan
    // backend never asks for VK_KHR_shader_float16_int8 when it creates the logical device, so a
    // shader module declaring the Float16 capability would be invalid usage on it regardless of
    // what the physical device reports. Loading the fp32 variant is therefore the only correct
    // choice here; restore Eden's two lines if PCSX2 ever enables the feature.
    const bool allow_fp16 = false;
    const bool prefer_fp16 = false;

    VideoCore::FrameGen::ShaderModules code;
    if (VideoCore::FrameGen::LoadShaderModules(code, allow_fp16, prefer_fp16) !=
        VideoCore::FrameGen::LosslessStatus::Ok) {
        return;
    }

    for (const auto& [id, words] : code) {
        modules.emplace(id, CreateWrappedShaderModule(device, words));
    }
    valid = true;
}

VkShaderModule LsfgShaders::Get(u32 shader_id) const {
    const auto hit = modules.find(shader_id);
    return hit == modules.end() ? VK_NULL_HANDLE : *hit->second;
}

} // namespace Vulkan
