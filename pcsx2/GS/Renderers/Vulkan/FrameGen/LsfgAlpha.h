// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_alpha.h.
// Descriptor layouts, dispatch maths and barrier ordering are verbatim; only the includes are
// remapped onto the PCSX2 shim. See LsfgVkCompat.h.

#pragma once

#include <array>

#include "FrameGenTypes.h"
#include "LsfgCommon.h"

namespace Vulkan {

class Device;
class LsfgShaders;

constexpr size_t LSFG_ALPHA_STAGES = 4;

class LsfgAlphaPasses {
public:
    LsfgAlphaPasses() = default;
    LsfgAlphaPasses(const Device& device, const LsfgShaders& shaders);

    [[nodiscard]] const LsfgPass& Get(size_t stage) const {
        return passes[stage];
    }

private:
    std::array<LsfgPass, LSFG_ALPHA_STAGES> passes;
};

class LsfgAlpha {
public:
    LsfgAlpha() = default;
    LsfgAlpha(const Device& device, MemoryAllocator& memory_allocator,
              const LsfgAlphaPasses& passes_, LsfgResources& resources,
              vk::DescriptorPool& descriptor_pool, LsfgImage& input);

    void PushBarriers(LsfgBarriers& barriers, u64 frame_count, size_t stage);
    void DispatchStage(vk::CommandBuffer cmdbuf, u64 frame_count, size_t stage);

    [[nodiscard]] LsfgImageHistory& Outputs() {
        return out_images;
    }

private:
    const LsfgAlphaPasses* passes{};
    LsfgImage* input{};

    std::array<VkDescriptorSet, LSFG_ALPHA_STAGES - 1> descriptor_sets{};
    std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> last_descriptor_sets{};
    vk::DescriptorSets owned_sets;

    LsfgImage temp1;
    LsfgImage temp2;
    LsfgImagePair temp3;
    LsfgImageHistory out_images;
};

} // namespace Vulkan
