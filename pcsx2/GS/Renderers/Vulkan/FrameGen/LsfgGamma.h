// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_gamma.h.
// Descriptor layouts, dispatch maths and barrier ordering are verbatim; only the includes are
// remapped onto the PCSX2 shim. See LsfgVkCompat.h.

#pragma once

#include <array>

#include "FrameGenTypes.h"
#include "LsfgCommon.h"

namespace Vulkan {

class Device;
class LsfgShaders;

constexpr size_t LSFG_GAMMA_STAGES = 5;
constexpr size_t LSFG_GAMMA_TEMPS = 3;

class LsfgGamma {
public:
    LsfgGamma() = default;
    LsfgGamma(const Device& device, MemoryAllocator& memory_allocator, const LsfgShaders& shaders,
              LsfgResources& resources, vk::DescriptorPool& descriptor_pool,
              LsfgImageHistory& inputs, LsfgImage& flow_input, LsfgImage* previous);

    void Dispatch(vk::CommandBuffer cmdbuf, u64 frame_count, size_t slot);

    [[nodiscard]] LsfgImage& Output() {
        return out_image;
    }

private:
    struct Generation {
        std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> first_descriptor_sets{};
        std::array<VkDescriptorSet, LSFG_GAMMA_STAGES - 1> descriptor_sets{};
    };

    LsfgImageHistory* inputs{};
    LsfgImage* flow_input{};
    LsfgImage* previous{};

    std::array<LsfgPass, LSFG_GAMMA_STAGES> passes;
    std::array<Generation, LSFG_GENERATION_SLOTS> generations{};
    vk::DescriptorSets owned_sets;

    std::array<LsfgImage, LSFG_GAMMA_TEMPS> temp1;
    LsfgImagePair temp2;
    LsfgImage out_image;
};

} // namespace Vulkan
