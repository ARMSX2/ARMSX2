// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSStreamRingFlushRange.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <deque>
#include <memory>

class VKStreamBuffer
{
public:
	VKStreamBuffer();
	VKStreamBuffer(VKStreamBuffer&& move);
	VKStreamBuffer(const VKStreamBuffer&) = delete;
	~VKStreamBuffer();

	VKStreamBuffer& operator=(VKStreamBuffer&& move);
	VKStreamBuffer& operator=(const VKStreamBuffer&) = delete;

	__fi bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
	__fi VkBuffer GetBuffer() const { return m_buffer; }
	__fi const VkBuffer* GetBufferPtr() const { return &m_buffer; }
	__fi u8* GetHostPointer() const { return m_host_pointer; }
	__fi u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
	__fi u32 GetCurrentSize() const { return m_size; }
	__fi u32 GetMaxSize() const { return m_max_size; }
	__fi u32 GetCurrentSpace() const { return m_current_space; }
	__fi u32 GetCurrentOffset() const { return m_current_offset; }

	/// `name` labels this ring in the message logged if it cannot be allocated on the road the
	/// stream-ring memory policy chose. A `max_size` above `size` lets ReserveMemory replace the
	/// buffer with a larger one instead of waiting for the GPU (GSStreamRingGrowth.h); the device
	/// is told through GSDeviceVK::OnStreamRingReplaced, because the buffer handle changes.
	bool Create(VkBufferUsageFlags usage, u32 size, const char* name, u32 max_size = 0);
	void Destroy(bool defer);

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);

	/// Cleans everything committed since the last call out of the CPU's caches, on a ring whose
	/// memory type is not coherent. A no-op on every other ring, and on a non-coherent ring that
	/// has not been written since the last flush.
	///
	/// Must be called on every ring before the queue submission that reads it, which is the only
	/// point the GPU can see any of it: command recording does not execute anything, so a draw
	/// recorded into the command buffer being built reads nothing until that buffer is submitted.
	/// GSDeviceVK::SubmitCommandBuffer does it for all six, immediately before vkQueueSubmit.
	void FlushPendingWrites();

private:
	struct ClearSpace
	{
		/// Index into m_tracked_fences of the fence to wait for.
		size_t fence_index;
		u32 offset;
		u32 space;
		u32 gpu_position;
	};

	void UpdateCurrentFencePosition();
	void UpdateGPUPosition();

	// Finds the oldest tracked fence whose completion would free num_bytes bytes.
	bool FindClearSpace(u32 num_bytes, ClearSpace* out) const;

	// Waits for the fence FindClearSpace chose and moves the ring onto the space it frees.
	void WaitForClearSpace(const ClearSpace& space);

	// Replaces the buffer with an empty one of new_size bytes. The old buffer is destroyed once the
	// command buffer being recorded retires, since draws already recorded in it still read it.
	bool Grow(u32 new_size);

	u32 m_size = 0;
	u32 m_max_size = 0;
	u32 m_current_offset = 0;
	u32 m_current_space = 0;
	u32 m_current_gpu_position = 0;

	VkBufferUsageFlags m_usage = 0;
	const char* m_name = "";

	VmaAllocation m_allocation = VK_NULL_HANDLE;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	u8* m_host_pointer = nullptr;

	// List of fences and the corresponding positions in the buffer
	std::deque<std::pair<u64, u32>> m_tracked_fences;

	/// Whether the memory type this ring landed on lacks HOST_COHERENT, in which case its writes
	/// need a real cache clean before the GPU reads them. False on every coherent ring, and then
	/// nothing below it is ever touched.
	bool m_non_coherent = false;

	/// What has been committed since the last flush, at most one range plus one more for a wrap.
	GSStreamRingFlushRanges m_pending_flush;
};
