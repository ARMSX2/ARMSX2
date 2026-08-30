// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/lsfg_generate.cpp.
// The pass is unchanged: the descriptor layouts, the tile shift and the barrier ordering are the
// shader contract, not style, so this stays verbatim and diffable against upstream. Only the
// includes are remapped onto the PCSX2 shim. See LsfgVkCompat.h.
//
// This is the one pass that writes into a swapchain-owned image rather than a private LsfgImage,
// so it builds its own barriers by hand instead of going through LsfgBarriers.

#include <vector>

#include "LosslessDll.h"
#include "LsfgGenerate.h"
#include "LsfgShaders.h"
#include "LsfgUtil.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

namespace {

constexpr u32 DISPATCH_TILE_SHIFT = 4;

[[nodiscard]] u32 GroupCount(u32 size) {
    return (size + (1u << DISPATCH_TILE_SHIFT) - 1) >> DISPATCH_TILE_SHIFT;
}

VkImageMemoryBarrier MakeTargetBarrier(VkImage image, VkAccessFlags src_access,
                                       VkAccessFlags dst_access, VkImageLayout old_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

} // Anonymous namespace

LsfgGenerate::LsfgGenerate(const Device& device, const LsfgShaders& shaders,
                           LsfgResources& resources, vk::DescriptorPool& descriptor_pool,
                           LsfgImagePair& frames_, LsfgImage& motion_, LsfgImage& detail1_,
                           LsfgImage& detail2_)
    : frames{&frames_}, motion{&motion_}, detail1{&detail1_}, detail2{&detail2_} {
    using namespace VideoCore::FrameGen::PerformanceShader;

    pass = LsfgPass(device, shaders, GENERATE,
                    {{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                     {2, VK_DESCRIPTOR_TYPE_SAMPLER},
                     {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
                     {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});

    sampler = resources.GetSampler();
    edge_sampler =
        resources.GetSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false);

    const std::vector<VkDescriptorSetLayout> layouts(
        LSFG_GENERATION_SLOTS * LSFG_MAX_TARGETS * 2, pass.SetLayout());
    owned_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);

    size_t next = 0;
    for (size_t slot = 0; slot < LSFG_GENERATION_SLOTS; ++slot) {
        Generation& target = generations[slot];
        target.buffer = resources.GetBuffer(LsfgSlotTimestamp(slot));

        for (auto& entry : target.targets) {
            for (auto& set : entry.descriptor_sets) {
                set = owned_sets[next++];
            }
        }
    }
}

void LsfgGenerate::SetTarget(const Device& device, size_t slot, u32 target, VkImageView view) {
    Target& entry = generations[slot].targets[target];
    if (entry.view == view) {
        return;
    }
    entry.view = view;

    for (size_t i = 0; i < entry.descriptor_sets.size(); ++i) {
        LsfgDescriptorWriter(entry.descriptor_sets[i])
            .AddUniformBuffer(generations[slot].buffer, LsfgResources::BufferSize())
            .AddSampler(sampler)
            .AddSampler(edge_sampler)
            .AddSampledImage((*frames)[1 - i])
            .AddSampledImage((*frames)[i])
            .AddSampledImage(*motion)
            .AddSampledImage(*detail1)
            .AddSampledImage(*detail2)
            .AddStorageView(view)
            .Build(device);
    }
}

void LsfgGenerate::Dispatch(vk::CommandBuffer cmdbuf, u64 frame_count, size_t slot, u32 target,
                            VkImage image, VkExtent2D extent) {
    const Target& entry = generations[slot].targets[target];

    LsfgBarriers(cmdbuf)
        .WriteToReadAll(*frames)
        .WriteToRead(*motion)
        .WriteToRead(*detail1)
        .WriteToRead(*detail2)
        .DiscardToWrite(image)
        .Build();

    pass.Bind(cmdbuf, entry.descriptor_sets[frame_count % entry.descriptor_sets.size()]);
    cmdbuf.Dispatch(GroupCount(extent.width), GroupCount(extent.height), 1);

    const std::array after{MakeTargetBarrier(
        image, VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL)};
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, after);
}

} // namespace Vulkan
