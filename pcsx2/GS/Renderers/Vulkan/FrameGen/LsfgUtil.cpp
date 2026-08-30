// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Eden (eden-emu PR #4263), src/video_core/renderer_vulkan/present/util.cpp.
// Logic unchanged; only the wrapper types are ours. See LsfgVkCompat.h.

#include <vector>

#include "LsfgUtil.h"

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "common/Console.h"

namespace Vulkan {

namespace vk {

void DescriptorSets::Destroy() {
    if (!sets.empty() && device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, pool, static_cast<u32>(sets.size()), sets.data());
    }
    sets.clear();
    device = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
}

} // namespace vk

vk::Buffer CreateWrappedBuffer(MemoryAllocator& allocator, VkDeviceSize size, MemoryUsage usage) {
    const VkBufferCreateInfo dst_buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    return allocator.CreateBuffer(dst_buffer_info, usage);
}

vk::ImageView CreateWrappedImageView(const Device& device, vk::Image& image, VkFormat format) {
    return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
}

vk::ShaderModule CreateWrappedShaderModule(const Device& device, std::span<const u32> code) {
    return device.GetLogical().CreateShaderModule(VkShaderModuleCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    });
}

vk::DescriptorPool CreateWrappedDescriptorPool(const Device& device, size_t max_descriptors,
                                               size_t max_sets,
                                               std::initializer_list<VkDescriptorType> types) {
    std::vector<VkDescriptorPoolSize> pool_sizes(types.size());
    for (size_t i = 0; i < types.size(); i++) {
        pool_sizes[i] = VkDescriptorPoolSize{
            .type = std::data(types)[i],
            .descriptorCount = static_cast<u32>(max_descriptors),
        };
    }

    return device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = static_cast<u32>(max_sets),
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

vk::DescriptorSetLayout CreateWrappedDescriptorSetLayout(const Device& device,
                                                         std::span<const VkDescriptorType> types,
                                                         VkShaderStageFlags stages) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (size_t i = 0; i < types.size(); i++) {
        bindings[i] = {
            .binding = static_cast<u32>(i),
            .descriptorType = types[i],
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr,
        };
    }

    return device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
}

vk::DescriptorSetLayout CreateWrappedDescriptorSetLayout(
    const Device& device, std::initializer_list<VkDescriptorType> types,
    VkShaderStageFlags stages) {
    return CreateWrappedDescriptorSetLayout(
        device, std::span<const VkDescriptorType>{std::data(types), types.size()}, stages);
}

vk::DescriptorSets CreateWrappedDescriptorSets(vk::DescriptorPool& pool,
                                               std::span<const VkDescriptorSetLayout> layouts) {
    // PORT: yuzu calls pool.Allocate(), and its pool handle carries the VkDevice that the
    // allocation needs. The shim's DescriptorPool exposes only operator*, so the device is taken
    // from the one live GSDeviceVK instead — frame generation is built and torn down inside that
    // device's lifetime, and PCSX2 never has a second one. Keeping the device out of the
    // signature is deliberate: it leaves all six call sites in the pass files identical to Eden's.
    GSDeviceVK* const gs_device = GSDeviceVK::GetInstance();
    if (!gs_device) {
        return {};
    }
    const VkDevice device = gs_device->GetDevice();
    std::vector<VkDescriptorSet> sets(layouts.size());

    const VkDescriptorSetAllocateInfo ai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = *pool,
        .descriptorSetCount = static_cast<u32>(layouts.size()),
        .pSetLayouts = layouts.data(),
    };

    const VkResult res = vkAllocateDescriptorSets(device, &ai, sets.data());
    if (res != VK_SUCCESS) {
        Console.Error("LSFG: vkAllocateDescriptorSets() failed: %d", static_cast<int>(res));
        return {};
    }
    return vk::DescriptorSets(device, *pool, std::move(sets));
}

vk::PipelineLayout CreateWrappedPipelineLayout(const Device& device,
                                               vk::DescriptorSetLayout& layout) {
    // PORT: yuzu's handles expose address(); the shim's expose only operator*, so the handle is
    // copied into a local whose address can be taken.
    const VkDescriptorSetLayout set_layout = *layout;
    return device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    });
}

vk::Pipeline CreateWrappedComputePipeline(const Device& device, vk::PipelineLayout& layout,
                                          VkShaderModule shader) {
    return device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        .layout = *layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });
}

} // namespace Vulkan
