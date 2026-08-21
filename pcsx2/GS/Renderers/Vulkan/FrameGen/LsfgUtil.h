// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/util.h.
// Logic unchanged; only the wrapper types are ours. See LsfgVkCompat.h.
//
// This is the LSFG-relevant subset of yuzu's present/util.h — the helpers the frame-generation
// code actually calls. The rest of that file (render passes, framebuffers, graphics pipelines,
// the blit-screen helpers) has no caller here and is deliberately not carried over.

#pragma once

#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include "FrameGenTypes.h"
#include "LsfgVkCompat.h"

namespace Vulkan {

class Device;

namespace vk {

/// PORT: stands in for yuzu's vk::DescriptorSets (a PoolAllocations<VkDescriptorSet>), which the
/// compat shim does not provide. Every pass owns one as a member, so the type has to be visible
/// from LsfgCommon.h — that header includes this one for exactly this reason.
///
/// Like yuzu's, this frees its sets back to the pool on destruction, which requires the pool to
/// carry VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT — CreateWrappedDescriptorPool sets it.
class DescriptorSets {
public:
    DescriptorSets() = default;
    DescriptorSets(VkDevice device_, VkDescriptorPool pool_, std::vector<VkDescriptorSet> sets_)
        : device{device_}, pool{pool_}, sets{std::move(sets_)} {}
    DescriptorSets(const DescriptorSets&) = delete;
    DescriptorSets& operator=(const DescriptorSets&) = delete;
    DescriptorSets(DescriptorSets&& o) noexcept
        : device{std::exchange(o.device, VK_NULL_HANDLE)},
          pool{std::exchange(o.pool, VK_NULL_HANDLE)}, sets{std::move(o.sets)} {
        o.sets.clear();
    }
    DescriptorSets& operator=(DescriptorSets&& o) noexcept {
        if (this != &o) {
            Destroy();
            device = std::exchange(o.device, VK_NULL_HANDLE);
            pool = std::exchange(o.pool, VK_NULL_HANDLE);
            sets = std::move(o.sets);
            o.sets.clear();
        }
        return *this;
    }
    ~DescriptorSets() {
        Destroy();
    }

    [[nodiscard]] const VkDescriptorSet& operator[](size_t index) const {
        return sets[index];
    }

    [[nodiscard]] size_t size() const {
        return sets.size();
    }

    [[nodiscard]] bool empty() const {
        return sets.empty();
    }

    explicit operator bool() const {
        return !sets.empty();
    }

private:
    void Destroy();

    VkDevice device{VK_NULL_HANDLE};
    VkDescriptorPool pool{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> sets;
};

} // namespace vk

vk::Buffer CreateWrappedBuffer(MemoryAllocator& allocator, VkDeviceSize size, MemoryUsage usage);

vk::ImageView CreateWrappedImageView(const Device& device, vk::Image& image, VkFormat format);

vk::ShaderModule CreateWrappedShaderModule(const Device& device, std::span<const u32> code);

vk::DescriptorPool CreateWrappedDescriptorPool(
    const Device& device, size_t max_descriptors, size_t max_sets,
    std::initializer_list<VkDescriptorType> types = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});

vk::DescriptorSetLayout CreateWrappedDescriptorSetLayout(
    const Device& device, std::initializer_list<VkDescriptorType> types,
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
vk::DescriptorSetLayout CreateWrappedDescriptorSetLayout(const Device& device,
                                                         std::span<const VkDescriptorType> types,
                                                         VkShaderStageFlags stages);

// PORT: yuzu takes vk::Span<VkDescriptorSetLayout>; std::span<const T> accepts the std::vector
// every call site already passes, so the call sites are unchanged.
vk::DescriptorSets CreateWrappedDescriptorSets(vk::DescriptorPool& pool,
                                               std::span<const VkDescriptorSetLayout> layouts);

vk::PipelineLayout CreateWrappedPipelineLayout(const Device& device,
                                               vk::DescriptorSetLayout& layout);

vk::Pipeline CreateWrappedComputePipeline(const Device& device, vk::PipelineLayout& layout,
                                          VkShaderModule shader);

} // namespace Vulkan
