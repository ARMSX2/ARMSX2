// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_shaders.cpp.
// Logic unchanged apart from the fp16 gate — see below.
// See FrameGenTypes.h and LsfgVkCompat.h.

#include <algorithm>

#include "common/Console.h"

#include "GS/GS.h"

#include "LosslessDll.h"
#include "LsfgShaders.h"
#include "LsfgUtil.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

namespace {

/// Whether a module asks the device for nothing but what the fp32 family is known to work with
/// here (Shader, StorageImageExtendedFormats, ImageQuery, StorageImageWriteWithoutFormat) plus
/// Float16, the one thing the half-precision family adds: read out of Lossless.dll on 2026-09-29.
/// A later DLL whose fp16 shaders wanted more would be invalid usage on our device, so it gets
/// the fp32 family instead of a device-lost.
[[nodiscard]] bool OnlyKnownCapabilities(const std::vector<u32>& words) {
    constexpr u32 OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11, OP_MEMORY_MODEL = 14, OP_CAPABILITY = 17;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        const u32 op = words[i] & 0xffff;
        if (count == 0 || i + count > words.size()) {
            return false;
        }
        if (op == OP_CAPABILITY) {
            switch (words[i + 1]) {
            case 1:  // Shader
            case 9:  // Float16
            case 49: // StorageImageExtendedFormats
            case 50: // ImageQuery
            case 56: // StorageImageWriteWithoutFormat
                break;
            default:
                return false;
            }
        } else if (op == OP_EXTENSION) {
            return false; // neither family declares one today
        } else if (op != OP_EXT_INST_IMPORT && op != OP_MEMORY_MODEL) {
            break; // past the capabilities, which come first
        }
        i += count;
    }
    return true;
}

} // Anonymous namespace

LsfgShaders::LsfgShaders(const Device& device) {
    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor()) {
        failure = "device lacks the Vulkan memory model or nullDescriptor";
        return;
    }

    // Eden's two lines: the half-precision family when the user asks for it and the device has
    // shaderFloat16, which GSDeviceVK enables only while GSConfig.LsfgFp16 is on. Without the
    // feature a module declaring Float16 is invalid usage, whatever the GPU could do.
    bool allow_fp16 = GSConfig.LsfgFp16 && device.IsFloat16Supported();
    bool prefer_fp16 = allow_fp16;

    VideoCore::FrameGen::ShaderModules code;
    VideoCore::FrameGen::LosslessStatus status = VideoCore::FrameGen::LoadShaderModules(code, allow_fp16, prefer_fp16);
    if (allow_fp16 && status == VideoCore::FrameGen::LosslessStatus::Ok &&
        !std::ranges::all_of(code, [](const auto& module) { return OnlyKnownCapabilities(module.second); })) {
        Console.Warning("LSFG: this Lossless.dll's half-precision shaders need more than the device has; using full precision.");
        allow_fp16 = prefer_fp16 = false;
        code.clear();
        status = VideoCore::FrameGen::LoadShaderModules(code, allow_fp16, prefer_fp16);
    }
    switch (status) {
    case VideoCore::FrameGen::LosslessStatus::Ok:
        break;
    case VideoCore::FrameGen::LosslessStatus::NotInstalled:
        failure = "no Lossless.dll";
        return;
    case VideoCore::FrameGen::LosslessStatus::UnreadableFile:
        failure = "Lossless.dll unreadable";
        return;
    case VideoCore::FrameGen::LosslessStatus::NotPortableExecutable:
        failure = "not a DLL";
        return;
    case VideoCore::FrameGen::LosslessStatus::MissingShaders:
        failure = "no usable shaders in this Lossless.dll";
        return;
    case VideoCore::FrameGen::LosslessStatus::TranslationFailed:
        failure = "shader translation failed";
        return;
    default:
        failure = "shader cache unusable";
        return;
    }

    for (const auto& [id, words] : code) {
        modules.emplace(id, CreateWrappedShaderModule(device, words));
    }
    // prefer_fp16 takes the half-precision family whenever the DLL has it, and the loader falls
    // back to fp32 when it doesn't, so say which one actually came in.
    fp16 = prefer_fp16 && std::ranges::any_of(code, [](const auto& module) {
        const std::vector<u32>& words = module.second;
        for (size_t i = 5; i < words.size();) {
            const u32 count = words[i] >> 16;
            if (count == 0 || (words[i] & 0xffff) != 17) {
                break; // past the capabilities, or malformed
            }
            if (i + 1 < words.size() && words[i + 1] == 9) {
                return true; // Float16
            }
            i += count;
        }
        return false;
    });
    if (GSConfig.LsfgFp16) {
        Console.WriteLn(fp16 ? "LSFG: half-precision shaders." :
                               "LSFG: half precision asked for, but the device or the DLL can't; full precision.");
    }
    valid = true;
}

VkShaderModule LsfgShaders::Get(u32 shader_id) const {
    const auto hit = modules.find(shader_id);
    return hit == modules.end() ? VK_NULL_HANDLE : *hit->second;
}

} // namespace Vulkan
