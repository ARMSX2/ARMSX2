// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_beta.h.
// Descriptor layouts, dispatch maths and barrier ordering are verbatim; only the includes are
// remapped onto the PCSX2 shim. See LsfgVkCompat.h.

#pragma once

#include <array>

#include "FrameGenTypes.h"
#include "LsfgCommon.h"

namespace Vulkan {

class Device;
class LsfgShaders;

constexpr size_t LSFG_BETA_STAGES = 5;
constexpr size_t LSFG_BETA_OUTPUTS = 6;

class LsfgBeta {
public:
    LsfgBeta() = default;
    LsfgBeta(const Device& device, MemoryAllocator& memory_allocator, const LsfgShaders& shaders,
             LsfgResources& resources, vk::DescriptorPool& descriptor_pool,
             LsfgImageHistory& inputs);

    void Dispatch(vk::CommandBuffer cmdbuf, u64 frame_count);

    [[nodiscard]] LsfgImage& Output(size_t level) {
        return out_images[level];
    }

private:
    LsfgImageHistory* inputs{};

    std::array<LsfgPass, LSFG_BETA_STAGES> passes;
    std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> first_descriptor_sets{};
    std::array<VkDescriptorSet, LSFG_BETA_STAGES - 1> descriptor_sets{};
    vk::DescriptorSets owned_sets;

    LsfgImagePair temp1;
    LsfgImagePair temp2;
    std::array<LsfgImage, LSFG_BETA_OUTPUTS> out_images;
};

} // namespace Vulkan
