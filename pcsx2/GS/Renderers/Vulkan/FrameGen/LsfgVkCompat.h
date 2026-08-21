// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vulkan compatibility layer for the frame-generation code ported from Eden (eden-emu PR #4263).
//
// WHY THIS EXISTS
//
// The ported passes are ~2000 lines of ordinary Vulkan compute work, but they are written against
// yuzu's RAII wrapper (`vk::Image`, `vk::CommandBuffer`, ...) and its `Device` / `MemoryAllocator`
// classes. PCSX2 uses raw handles plus VMA directly, so the two do not meet.
//
// Reimplementing yuzu's small API on top of PCSX2's is far cheaper and far safer than rewriting
// every call site: the surface the frame-generation code actually touches is tiny — five command
// buffer methods, three Device queries, two allocator entry points and a handful of factory calls.
// Keeping the pass code verbatim also means upstream fixes stay a readable diff instead of a
// merge puzzle.
//
// LICENSING: Eden is GPL-3.0-or-later, PCSX2 is GPL-3.0+, so the two combine. This is NOT true of
// RPCS3 (GPL-2.0-only) — these files must not be carried into ARMSX3.

#pragma once

#include "FrameGenTypes.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <span>
#include <utility>
#include <vector>

class GSDeviceVK;

namespace Vulkan
{
	/// Where a buffer's memory should live. yuzu's enum, narrowed to the two values the frame
	/// generator uses.
	enum class MemoryUsage
	{
		Upload,  ///< host-visible, written every frame by the CPU
		Download ///< host-visible, read back by the CPU
	};

	namespace vk
	{
		/// Owns a device-child handle and destroys it on scope exit.
		///
		/// ★ Destruction is IMMEDIATE, not deferred. PCSX2 normally routes teardown through
		/// GSDeviceVK::DeferImageDestruction so a resource still referenced by an in-flight
		/// command buffer outlives it. Everything here is created once when frame generation
		/// starts and released only when it stops — and the stop path idles the device first —
		/// so immediate destruction is safe. If any of these ever become per-frame, they must
		/// move to the deferred path instead.
		template <typename T>
		class Handle
		{
		public:
			Handle() = default;
			Handle(VkDevice device, T handle)
				: m_device(device)
				, m_handle(handle)
			{
			}
			Handle(const Handle&) = delete;
			Handle& operator=(const Handle&) = delete;
			Handle(Handle&& o) noexcept
				: m_device(std::exchange(o.m_device, VK_NULL_HANDLE))
				, m_handle(std::exchange(o.m_handle, VK_NULL_HANDLE))
			{
			}
			Handle& operator=(Handle&& o) noexcept
			{
				if (this != &o)
				{
					Destroy();
					m_device = std::exchange(o.m_device, VK_NULL_HANDLE);
					m_handle = std::exchange(o.m_handle, VK_NULL_HANDLE);
				}
				return *this;
			}
			~Handle() { Destroy(); }

			T operator*() const { return m_handle; }
			explicit operator bool() const { return m_handle != VK_NULL_HANDLE; }

		private:
			void Destroy();

			VkDevice m_device = VK_NULL_HANDLE;
			T m_handle = VK_NULL_HANDLE;
		};

		using ImageView = Handle<VkImageView>;
		using Sampler = Handle<VkSampler>;
		using ShaderModule = Handle<VkShaderModule>;
		using DescriptorSetLayout = Handle<VkDescriptorSetLayout>;
		using PipelineLayout = Handle<VkPipelineLayout>;
		using Pipeline = Handle<VkPipeline>;
		using DescriptorPool = Handle<VkDescriptorPool>;

		/// VMA-backed image. Separate from Handle because it carries an allocation.
		class Image
		{
		public:
			Image() = default;
			Image(VmaAllocator allocator, VkImage image, VmaAllocation allocation);
			Image(const Image&) = delete;
			Image& operator=(const Image&) = delete;
			Image(Image&& o) noexcept;
			Image& operator=(Image&& o) noexcept;
			~Image();

			VkImage operator*() const { return m_image; }
			explicit operator bool() const { return m_image != VK_NULL_HANDLE; }

		private:
			void Destroy();

			VmaAllocator m_allocator = VK_NULL_HANDLE;
			VkImage m_image = VK_NULL_HANDLE;
			VmaAllocation m_allocation = VK_NULL_HANDLE;
		};

		/// VMA-backed buffer. Persistently mapped when created Upload/Download, which is what
		/// Mapped() hands back — the frame generator rewrites its uniform every frame and must
		/// not pay for a map/unmap each time.
		class Buffer
		{
		public:
			Buffer() = default;
			Buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mapped, VkDeviceSize size);
			Buffer(const Buffer&) = delete;
			Buffer& operator=(const Buffer&) = delete;
			Buffer(Buffer&& o) noexcept;
			Buffer& operator=(Buffer&& o) noexcept;
			~Buffer();

			VkBuffer operator*() const { return m_buffer; }
			explicit operator bool() const { return m_buffer != VK_NULL_HANDLE; }
			std::span<u8> Mapped() const { return {static_cast<u8*>(m_mapped), static_cast<size_t>(m_size)}; }

			/// Make a CPU write visible to the device.
			///
			/// A no-op on HOST_COHERENT memory, which is what CPU_TO_GPU resolves to on every
			/// device we currently ship to — but VMA is free to hand back non-coherent memory,
			/// and the write this guards is the shader's entire uniform block. The failure mode
			/// without it is not a crash: it is the interpolation silently reading stale
			/// constants, which looks like a subtle motion artefact rather than a bug.
			void Flush() const;

		private:
			void Destroy();

			VmaAllocator m_allocator = VK_NULL_HANDLE;
			VkBuffer m_buffer = VK_NULL_HANDLE;
			VmaAllocation m_allocation = VK_NULL_HANDLE;
			void* m_mapped = nullptr;
			VkDeviceSize m_size = 0;
		};

		/// Non-owning command buffer with the five recording calls the passes make.
		class CommandBuffer
		{
		public:
			CommandBuffer() = default;
			CommandBuffer(VkCommandBuffer cmdbuf)
				: m_cmdbuf(cmdbuf)
			{
			}

			VkCommandBuffer operator*() const { return m_cmdbuf; }

			void Dispatch(u32 x, u32 y, u32 z) const { vkCmdDispatch(m_cmdbuf, x, y, z); }

			void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline) const
			{
				vkCmdBindPipeline(m_cmdbuf, bind_point, pipeline);
			}

			void BindDescriptorSets(VkPipelineBindPoint bind_point, VkPipelineLayout layout, u32 first,
				std::span<const VkDescriptorSet> sets, std::span<const u32> dynamic_offsets) const
			{
				vkCmdBindDescriptorSets(m_cmdbuf, bind_point, layout, first, static_cast<u32>(sets.size()),
					sets.data(), static_cast<u32>(dynamic_offsets.size()),
					dynamic_offsets.empty() ? nullptr : dynamic_offsets.data());
			}

			void CopyImage(VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
				std::span<const VkImageCopy> regions) const
			{
				vkCmdCopyImage(m_cmdbuf, src, src_layout, dst, dst_layout, static_cast<u32>(regions.size()),
					regions.data());
			}

			void PipelineBarrier(VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
				VkDependencyFlags dependencies, std::span<const VkMemoryBarrier> memory,
				std::span<const VkBufferMemoryBarrier> buffers,
				std::span<const VkImageMemoryBarrier> images) const
			{
				vkCmdPipelineBarrier(m_cmdbuf, src_stage, dst_stage, dependencies,
					static_cast<u32>(memory.size()), memory.empty() ? nullptr : memory.data(),
					static_cast<u32>(buffers.size()), buffers.empty() ? nullptr : buffers.data(),
					static_cast<u32>(images.size()), images.empty() ? nullptr : images.data());
			}

		private:
			VkCommandBuffer m_cmdbuf = VK_NULL_HANDLE;
		};

		/// The logical device, as a factory. Mirrors the yuzu calls the ported code makes.
		class LogicalDevice
		{
		public:
			LogicalDevice() = default;
			explicit LogicalDevice(VkDevice device)
				: m_device(device)
			{
			}

			VkDevice operator*() const { return m_device; }

			[[nodiscard]] ImageView CreateImageView(const VkImageViewCreateInfo& ci) const;
			[[nodiscard]] Sampler CreateSampler(const VkSamplerCreateInfo& ci) const;
			[[nodiscard]] ShaderModule CreateShaderModule(const VkShaderModuleCreateInfo& ci) const;
			[[nodiscard]] DescriptorSetLayout CreateDescriptorSetLayout(const VkDescriptorSetLayoutCreateInfo& ci) const;
			[[nodiscard]] PipelineLayout CreatePipelineLayout(const VkPipelineLayoutCreateInfo& ci) const;
			[[nodiscard]] Pipeline CreateComputePipeline(const VkComputePipelineCreateInfo& ci) const;
			[[nodiscard]] DescriptorPool CreateDescriptorPool(const VkDescriptorPoolCreateInfo& ci) const;

			void UpdateDescriptorSets(std::span<const VkWriteDescriptorSet> writes,
				std::span<const VkCopyDescriptorSet> copies) const;

		private:
			VkDevice m_device = VK_NULL_HANDLE;
		};
	} // namespace vk

	/// Adapter over PCSX2's GSDeviceVK presenting the three queries the ported code asks for.
	class Device
	{
	public:
		explicit Device(GSDeviceVK* dev);

		[[nodiscard]] const vk::LogicalDevice& GetLogical() const { return m_logical; }

		/// Both are hard requirements of the LSFG shaders, checked once before anything is built.
		[[nodiscard]] bool IsVulkanMemoryModelSupported() const { return m_vulkan_memory_model; }
		[[nodiscard]] bool HasNullDescriptor() const { return m_null_descriptor; }

		[[nodiscard]] GSDeviceVK* Raw() const { return m_device; }

	private:
		GSDeviceVK* m_device = nullptr;
		vk::LogicalDevice m_logical;
		bool m_vulkan_memory_model = false;
		bool m_null_descriptor = false;
	};

	/// Adapter over PCSX2's VMA allocator.
	class MemoryAllocator
	{
	public:
		explicit MemoryAllocator(VmaAllocator allocator)
			: m_allocator(allocator)
		{
		}

		[[nodiscard]] vk::Image CreateImage(const VkImageCreateInfo& ci) const;
		[[nodiscard]] vk::Buffer CreateBuffer(const VkBufferCreateInfo& ci, MemoryUsage usage) const;

	private:
		VmaAllocator m_allocator = VK_NULL_HANDLE;
	};
} // namespace Vulkan
