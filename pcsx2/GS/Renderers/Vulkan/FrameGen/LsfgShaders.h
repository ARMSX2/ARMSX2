// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_shaders.h.
// Logic unchanged; only the scalar aliases and the Vulkan wrapper types are ours.
// See FrameGenTypes.h and LsfgVkCompat.h.

#pragma once

#include <map>

#include "FrameGenTypes.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

class Device;

class LsfgShaders {
public:
    explicit LsfgShaders(const Device& device);

    [[nodiscard]] bool IsValid() const {
        return valid;
    }

    [[nodiscard]] VkShaderModule Get(u32 shader_id) const;

private:
    std::map<u32, vk::ShaderModule> modules;
    bool valid{};
};

} // namespace Vulkan
