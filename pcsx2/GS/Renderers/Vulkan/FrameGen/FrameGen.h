// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/frame_gen.h.
// The orchestration is unchanged — warm-up counting, the rebuild conditions, the flow-scale
// derivation and the pacer plumbing are all Eden's. What differs is structural:
//
//  * Eden's methods take a `Frame*` out of PresentManager's pool. PCSX2 has no such pool, so the
//    three fields they read (image, storage view, extent) arrive as explicit parameters instead.
//  * Eden records through a `Scheduler`. We hold GSDeviceVK and record into its current command
//    buffer directly.
//  * Eden's debug image dump — `DumpDebugImages` plus the WritePortablePixmap / WriteGrayscalePgm
//    / WriteRaw / WriteColorPpm writers and the `dumped` flag — is not carried over. It needs
//    <filesystem>, which the GS backend does not pull in, and a buffer readback the compat shim
//    does not implement.
//
// See FrameGenTypes.h and LsfgVkCompat.h.

#pragma once

#include <array>
#include <optional>

#include "FrameGenPacer.h"
#include "FrameGenTypes.h"
#include "LsfgChain.h"
#include "LsfgShaders.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

class FrameGen {
public:
    explicit FrameGen(MemoryAllocator& memory_allocator, GSDeviceVK* device);
    ~FrameGen();

    /// Feeds the frame about to be presented into the chain.
    ///
    /// ★ The caller supplies [cmdbuf]. It is NOT taken from GSDeviceVK::GetCurrentCommandBuffer():
    /// frame generation runs from the present hook, which fires AFTER the frame's command buffer
    /// has been submitted, so that buffer is either in flight or already belongs to the next
    /// frame. Recording into it is undefined, and the visible symptom would be interpolation
    /// running a frame late rather than anything that looks like an error. GSLsfg owns its own
    /// one-shot buffers and passes them in, which is also how Eden's scheduler supplies one.
    ///
    /// `image` / `storage_view` / `extent` describe that presented image; `guest_extent` is the
    /// size the game was actually rendered at, upscale included — the flow-scale heuristic
    /// compares the two. Must be called outside a render pass.
    void Process(const Device& device, vk::CommandBuffer cmdbuf, VkImage image,
                 VkImageView storage_view, VkExtent2D extent, VkFormat format,
                 VkExtent2D guest_extent);

    [[nodiscard]] size_t WantedGenerations(size_t capacity);

    [[nodiscard]] size_t GeneratedFrameCount() const;

    /// Writes interpolated frame `generation` into `image`. Only valid while
    /// GeneratedFrameCount() is non-zero, and for `generation` below it.
    void GenerateInto(const Device& device, vk::CommandBuffer cmdbuf, VkImage image,
                      VkImageView storage_view, size_t generation);

private:
    void Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale);
    void WaitForIdle();
    [[nodiscard]] u32 TargetIndex(VkImageView view);

    MemoryAllocator& memory_allocator;
    GSDeviceVK* gs_device;

    std::optional<LsfgShaders> shaders;
    std::optional<LsfgChain> chain;
    FrameGenPacer pacer;
    FrameGenPlan plan{};
    /// PORT: stands in for Eden's `Frame::index`. LsfgGenerate keys its descriptor sets by target
    /// slot and only rewrites them when the view in that slot changes, so a destination image
    /// needs a *stable* index — Eden gets one from its frame pool, we recover it by remembering
    /// which view we handed to which slot.
    std::array<VkImageView, LSFG_MAX_TARGETS> targets{};
    size_t target_count{};
    VkExtent2D peak_guest_extent{};
    VkExtent2D built_extent{};
    VkFormat built_format{VK_FORMAT_UNDEFINED};
    f32 built_flow_scale{};
    u64 frame_count{};
    u64 last_count{};
    size_t last_generations{};
    u32 warm_streak{};
    bool generated{};
    bool unavailable{};
};

} // namespace Vulkan
