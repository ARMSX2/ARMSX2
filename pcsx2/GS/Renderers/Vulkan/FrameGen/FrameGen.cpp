// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/frame_gen.cpp.
// The orchestration is verbatim; see FrameGen.h for the three structural differences and for why
// the debug image dump is gone. Only the PORT-marked spots below deviate.

#include <algorithm>
#include <array>
#include <cmath>

#include "FrameGen.h"
#include "LsfgChain.h"
#include "LsfgCommon.h"
#include "LsfgShaders.h"
#include "LsfgVkCompat.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"

namespace Vulkan {

namespace {

constexpr u64 LSFG_REQUIRED_FRAMES = 2;
constexpr u32 LSFG_RECURRENCE_FRAMES = 2;

[[nodiscard]] f32 ManualFlowScale() {
    // The clamp is not Eden's: their setting type enforces 25..100 on the way in, ours is a plain
    // u8 that a hand-edited INI can hold anything in, and LsfgResources feeds this straight into
    // 1.0f / flow_scale. GSLsfg.cpp clamps the same way for the same reason.
    return static_cast<f32>(std::clamp<u8>(GSConfig.LsfgFlowScale, 25, 100)) / 100.0f;
}

[[nodiscard]] f32 ConfiguredFlowScale(VkExtent2D guest_extent, VkExtent2D presented_extent) {
    // PORT: Eden gates the automatic path on frame_gen_flow_scale_auto, a toggle that defaults on
    // and that PCSX2 has no equivalent for. 100% — our default, and the top of the 25..100 range
    // the UI offers — reads as "do not reduce the flow resolution", and the automatic result is
    // clamped to 1.0 anyway, so treating it as Eden's auto mode preserves both projects' default
    // behaviour. Any explicit value below 100 pins the scale exactly where the user put it.
    if (GSConfig.LsfgFlowScale < 100) {
        return ManualFlowScale();
    }
    if (guest_extent.width == 0 || presented_extent.width == 0) {
        return 1.0f;
    }

    // PORT: Eden scales by resolution_info.up_factor because its guest_extent is the console's own
    // resolution. Ours is the size the game was really rendered at, upscale already applied.
    const f32 rendered_width = static_cast<f32>(guest_extent.width);
    const f32 ratio = rendered_width / static_cast<f32>(presented_extent.width);

    constexpr f32 FLOW_SCALE_STEPS = 20.0f;
    const f32 stepped = std::ceil(ratio * FLOW_SCALE_STEPS) / FLOW_SCALE_STEPS;
    return std::clamp(stepped, 0.25f, 1.0f);
}

VkImageMemoryBarrier MakeTransitionBarrier(VkImage image, VkAccessFlags src_access,
                                           VkAccessFlags dst_access, VkImageLayout old_layout,
                                           VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
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

VkImageCopy MakeCopyRegion(VkExtent2D extent) {
    return VkImageCopy{
        .srcSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = {},
        .dstSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = {},
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
}

void CopyPresentedFrame(vk::CommandBuffer cmdbuf, VkImage source, LsfgImage& destination,
                        VkExtent2D extent) {
    const auto make_barrier = MakeTransitionBarrier;

    const std::array before{
        make_barrier(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        make_barrier(destination.Handle(), VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     destination.Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    // PORT: yuzu's vk::Span takes a lone VkImageCopy; the shim's std::span needs a range.
    const std::array regions{MakeCopyRegion(extent)};
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.Handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions);

    const std::array after{
        make_barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        make_barrier(destination.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, {}, {}, after);

    destination.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
}

} // Anonymous namespace

FrameGen::FrameGen(MemoryAllocator& memory_allocator_, GSDeviceVK* device_)
    : memory_allocator{memory_allocator_}, gs_device{device_} {}

FrameGen::~FrameGen() {
    if (chain) {
        // ★ Device idle before teardown — see WaitForIdle. The chain is about to be destroyed by
        // the member destructor, and its images and pipelines go with it immediately.
        WaitForIdle();
    }
}

void FrameGen::Process(const Device& device, vk::CommandBuffer cmdbuf, VkImage image,
                       VkImageView storage_view, VkExtent2D extent, VkFormat format,
                       VkExtent2D guest_extent) {
    generated = false;

    if (unavailable || !GSConfig.LsfgEnabled) {
        if (chain) {
            // ★ Device idle before teardown — see WaitForIdle. Stands in for Eden's
            // scheduler.Finish() ahead of the same chain.reset().
            WaitForIdle();
            chain.reset();
        }
        warm_streak = 0;
        return;
    }

    if (storage_view == VK_NULL_HANDLE) {
        unavailable = true;
        return;
    }

    if (!shaders) {
        shaders.emplace(device);
        if (!shaders->IsValid()) {
            unavailable = true;
            return;
        }
    }

    peak_guest_extent.width = std::max(peak_guest_extent.width, guest_extent.width);
    peak_guest_extent.height = std::max(peak_guest_extent.height, guest_extent.height);

    const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, extent);
    if (!chain || built_extent.width != extent.width || built_extent.height != extent.height ||
        built_format != format || built_flow_scale != flow_scale) {
        Rebuild(device, extent, format, flow_scale);
    }

    const u64 count = frame_count++;
    last_count = count;
    last_generations = plan.generations;

    const bool warm = plan.warm && count + 1 >= LSFG_REQUIRED_FRAMES;
    warm_streak = warm ? warm_streak + 1 : 0;
    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;

    // PORT: Eden asks the scheduler for an outside-render-pass context and defers the recording
    // into a callback. Here the caller is already outside any render pass and hands us the buffer
    // to record into — see the note on Process() in the header for why it must not be the frame's.
    CopyPresentedFrame(cmdbuf, image, chain->Input(count), extent);
    if (warm) {
        chain->DispatchShared(cmdbuf, count);
    }
}

size_t FrameGen::WantedGenerations(size_t capacity) {
    if (unavailable) {
        plan = {};
        return 0;
    }
    plan = pacer.Plan(capacity);
    return plan.generations;
}

size_t FrameGen::GeneratedFrameCount() const {
    return generated ? last_generations : 0;
}

void FrameGen::GenerateInto(const Device& device, vk::CommandBuffer cmdbuf, VkImage image,
                            VkImageView storage_view, size_t generation) {
    const u32 target = TargetIndex(storage_view);
    chain->SetTarget(device, last_generations, generation, target, storage_view);

    // PORT: Eden reads the destination Frame's own size. Everything we generate into is a
    // presented image, so it is the extent the chain was built for.
    const VkExtent2D extent = built_extent;

    // PORT: recorded into the caller's buffer, as in Process.
    chain->DispatchGeneration(cmdbuf, last_count, last_generations, generation, target, image,
                              extent);
}

void FrameGen::Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale) {
    // ★ Device idle before teardown — see WaitForIdle. This is Eden's scheduler.Finish().
    WaitForIdle();
    chain.reset();

    // PORT: the slot table keys on VkImageView handles, and a rebuild is exactly when the
    // presented images are recreated. Dropping the stale handles keeps a recycled one from
    // matching an entry that belongs to a view that no longer exists.
    targets.fill(VK_NULL_HANDLE);
    target_count = 0;

    built_flow_scale = flow_scale;

    chain.emplace(device, memory_allocator, *shaders, extent, format, built_flow_scale);
    built_extent = extent;
    built_format = format;
    frame_count = 0;
    warm_streak = 0;
    generated = false;
}

void FrameGen::WaitForIdle() {
    // ★ LOAD-BEARING, and the reason every teardown path calls it first.
    //
    // LsfgVkCompat's wrappers call vkDestroy* the moment they go out of scope instead of routing
    // through GSDeviceVK's deferred-destruction queue. Releasing the chain while a submitted
    // command buffer still references its images, pipelines or descriptor pool is therefore a
    // use-after-free, and one that surfaces as a random GPU fault rather than as an obvious bug.
    // Eden gets the guarantee from Scheduler::Finish(); ours has to be explicit.
    //
    // vkDeviceWaitIdle covers everything submitted, which is sufficient here because every site
    // that calls this runs before that frame's chain work is recorded, and the previous frame's
    // command buffer was submitted by the present that ended it. Note it is deliberately not
    // GSDeviceVK::ExecuteCommandBuffer: that submits the frame's command buffer, which is the
    // wrong thing to do halfway through a present.
    gs_device->WaitForGPUIdle();
}

u32 FrameGen::TargetIndex(VkImageView view) {
    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i] == view) {
            return static_cast<u32>(i);
        }
    }

    // Unseen view: claim the next slot. Wrapping is not expected — a present path draws from a
    // handful of images and there are LSFG_MAX_TARGETS of these — and costs only a descriptor
    // rewrite if it ever happens.
    const u32 index = static_cast<u32>(target_count++ % targets.size());
    targets[index] = view;
    return index;
}

} // namespace Vulkan
