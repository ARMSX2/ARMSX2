// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// See LsfgVkCompat.h for why this layer exists.

#include "LsfgVkCompat.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "common/Console.h"

namespace Vulkan
{
	namespace vk
	{
		// Destroy() is specialised per handle type rather than carrying a function pointer: the
		// vkDestroy* entry points are PCSX2's loader globals, so a template specialisation costs
		// nothing at runtime and keeps Handle<> a two-pointer object.
		template <>
		void Handle<VkImageView>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyImageView(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkSampler>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroySampler(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkShaderModule>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyShaderModule(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkDescriptorSetLayout>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyDescriptorSetLayout(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkPipelineLayout>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyPipelineLayout(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkPipeline>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}
		template <>
		void Handle<VkDescriptorPool>::Destroy()
		{
			if (m_handle != VK_NULL_HANDLE)
				vkDestroyDescriptorPool(m_device, m_handle, nullptr);
			m_handle = VK_NULL_HANDLE;
		}

		// --- Image -------------------------------------------------------------------------

		Image::Image(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
			: m_allocator(allocator)
			, m_image(image)
			, m_allocation(allocation)
		{
		}
		Image::Image(Image&& o) noexcept
			: m_allocator(std::exchange(o.m_allocator, VK_NULL_HANDLE))
			, m_image(std::exchange(o.m_image, VK_NULL_HANDLE))
			, m_allocation(std::exchange(o.m_allocation, VK_NULL_HANDLE))
		{
		}
		Image& Image::operator=(Image&& o) noexcept
		{
			if (this != &o)
			{
				Destroy();
				m_allocator = std::exchange(o.m_allocator, VK_NULL_HANDLE);
				m_image = std::exchange(o.m_image, VK_NULL_HANDLE);
				m_allocation = std::exchange(o.m_allocation, VK_NULL_HANDLE);
			}
			return *this;
		}
		Image::~Image() { Destroy(); }
		void Image::Destroy()
		{
			if (m_image != VK_NULL_HANDLE)
				vmaDestroyImage(m_allocator, m_image, m_allocation);
			m_image = VK_NULL_HANDLE;
			m_allocation = VK_NULL_HANDLE;
		}

		// --- Buffer ------------------------------------------------------------------------

		Buffer::Buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mapped, VkDeviceSize size)
			: m_allocator(allocator)
			, m_buffer(buffer)
			, m_allocation(allocation)
			, m_mapped(mapped)
			, m_size(size)
		{
		}
		Buffer::Buffer(Buffer&& o) noexcept
			: m_allocator(std::exchange(o.m_allocator, VK_NULL_HANDLE))
			, m_buffer(std::exchange(o.m_buffer, VK_NULL_HANDLE))
			, m_allocation(std::exchange(o.m_allocation, VK_NULL_HANDLE))
			, m_mapped(std::exchange(o.m_mapped, nullptr))
			, m_size(std::exchange(o.m_size, 0))
		{
		}
		Buffer& Buffer::operator=(Buffer&& o) noexcept
		{
			if (this != &o)
			{
				Destroy();
				m_allocator = std::exchange(o.m_allocator, VK_NULL_HANDLE);
				m_buffer = std::exchange(o.m_buffer, VK_NULL_HANDLE);
				m_allocation = std::exchange(o.m_allocation, VK_NULL_HANDLE);
				m_mapped = std::exchange(o.m_mapped, nullptr);
				m_size = std::exchange(o.m_size, 0);
			}
			return *this;
		}
		Buffer::~Buffer() { Destroy(); }
		void Buffer::Flush() const
		{
			if (m_allocation != VK_NULL_HANDLE)
				vmaFlushAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
		}
		void Buffer::Destroy()
		{
			if (m_buffer != VK_NULL_HANDLE)
				vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
			m_buffer = VK_NULL_HANDLE;
			m_allocation = VK_NULL_HANDLE;
			m_mapped = nullptr;
		}

		// --- LogicalDevice factories -------------------------------------------------------
		//
		// yuzu's wrapper throws on failure; PCSX2 is built with -fno-exceptions, so these log and
		// return an empty handle instead. Every caller stores the result in a member that is
		// checked before use, and FrameGen's init bails when any of them come back null — so a
		// failure here disables frame generation rather than taking the renderer down with it.

		ImageView LogicalDevice::CreateImageView(const VkImageViewCreateInfo& ci) const
		{
			VkImageView handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateImageView(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateImageView() failed: %d", static_cast<int>(res));
				return {};
			}
			return ImageView(m_device, handle);
		}

		Sampler LogicalDevice::CreateSampler(const VkSamplerCreateInfo& ci) const
		{
			VkSampler handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateSampler(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateSampler() failed: %d", static_cast<int>(res));
				return {};
			}
			return Sampler(m_device, handle);
		}

		ShaderModule LogicalDevice::CreateShaderModule(const VkShaderModuleCreateInfo& ci) const
		{
			VkShaderModule handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateShaderModule(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateShaderModule() failed: %d", static_cast<int>(res));
				return {};
			}
			return ShaderModule(m_device, handle);
		}

		DescriptorSetLayout LogicalDevice::CreateDescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo& ci) const
		{
			VkDescriptorSetLayout handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateDescriptorSetLayout() failed: %d", static_cast<int>(res));
				return {};
			}
			return DescriptorSetLayout(m_device, handle);
		}

		PipelineLayout LogicalDevice::CreatePipelineLayout(const VkPipelineLayoutCreateInfo& ci) const
		{
			VkPipelineLayout handle = VK_NULL_HANDLE;
			const VkResult res = vkCreatePipelineLayout(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreatePipelineLayout() failed: %d", static_cast<int>(res));
				return {};
			}
			return PipelineLayout(m_device, handle);
		}

		Pipeline LogicalDevice::CreateComputePipeline(const VkComputePipelineCreateInfo& ci) const
		{
			VkPipeline handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateComputePipelines() failed: %d", static_cast<int>(res));
				return {};
			}
			return Pipeline(m_device, handle);
		}

		DescriptorPool LogicalDevice::CreateDescriptorPool(const VkDescriptorPoolCreateInfo& ci) const
		{
			VkDescriptorPool handle = VK_NULL_HANDLE;
			const VkResult res = vkCreateDescriptorPool(m_device, &ci, nullptr, &handle);
			if (res != VK_SUCCESS)
			{
				Console.Error("LSFG: vkCreateDescriptorPool() failed: %d", static_cast<int>(res));
				return {};
			}
			return DescriptorPool(m_device, handle);
		}

		void LogicalDevice::UpdateDescriptorSets(std::span<const VkWriteDescriptorSet> writes,
			std::span<const VkCopyDescriptorSet> copies) const
		{
			vkUpdateDescriptorSets(m_device, static_cast<u32>(writes.size()),
				writes.empty() ? nullptr : writes.data(), static_cast<u32>(copies.size()),
				copies.empty() ? nullptr : copies.data());
		}
	} // namespace vk

	// --- Device ----------------------------------------------------------------------------

	Device::Device(GSDeviceVK* dev)
		: m_device(dev)
	{
		if (!dev)
			return;

		m_logical = vk::LogicalDevice(dev->GetDevice());

		// Both features are queried straight from the driver rather than trusted from PCSX2's
		// own feature struct: the LSFG shaders need them specifically, and PCSX2 does not enable
		// or track either for its own rendering, so there is nothing cached to read.
		// Value-init then assign, rather than a braced initialiser: these structs carry more
		// members than the two being set, and naming only the first leaves the rest to
		// -Wmissing-field-initializers. Same idiom as the rest of the Vulkan backend.
		VkPhysicalDeviceRobustness2FeaturesEXT robustness = {};
		robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;

		VkPhysicalDeviceVulkanMemoryModelFeatures memory_model = {};
		memory_model.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
		memory_model.pNext = &robustness;

		VkPhysicalDeviceFeatures2 features = {};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &memory_model;
		vkGetPhysicalDeviceFeatures2(dev->GetPhysicalDevice(), &features);

		m_vulkan_memory_model = (memory_model.vulkanMemoryModel == VK_TRUE);
		m_null_descriptor = (robustness.nullDescriptor == VK_TRUE);
	}

	// --- MemoryAllocator -------------------------------------------------------------------

	vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo& ci) const
	{
		// Same idiom as GSTextureVK: value-init then assign, rather than a positional aggregate
		// whose meaning silently changes if VMA reorders the struct.
		VmaAllocationCreateInfo aci = {};
		aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		aci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
		aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		const VkResult res = vmaCreateImage(m_allocator, &ci, &aci, &image, &allocation, nullptr);
		if (res != VK_SUCCESS)
		{
			Console.Error("LSFG: vmaCreateImage() failed: %d", static_cast<int>(res));
			return {};
		}
		return vk::Image(m_allocator, image, allocation);
	}

	vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo& ci, MemoryUsage usage) const
	{
		// Both usages are CPU-visible and touched every frame, so they are persistently mapped
		// (VMA_ALLOCATION_CREATE_MAPPED_BIT) — the uniform is rewritten per generated frame and
		// must not pay a map/unmap each time. Using the older CPU_TO_GPU / GPU_TO_CPU spelling
		// rather than AUTO + HOST_ACCESS_* to match how the rest of the Vulkan backend asks.
		VmaAllocationCreateInfo aci = {};
		aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		aci.usage = (usage == MemoryUsage::Upload) ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_GPU_TO_CPU;

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo ai = {};
		const VkResult res = vmaCreateBuffer(m_allocator, &ci, &aci, &buffer, &allocation, &ai);
		if (res != VK_SUCCESS)
		{
			Console.Error("LSFG: vmaCreateBuffer() failed: %d", static_cast<int>(res));
			return {};
		}
		return vk::Buffer(m_allocator, buffer, allocation, ai.pMappedData, ci.size);
	}
} // namespace Vulkan
