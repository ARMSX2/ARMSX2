// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <deque>
#include <memory>

/// Defined in GSDeviceVK.h, which includes this header -- so this is an opaque-enum-declaration
/// (legal, and it makes the type complete) rather than an include the other way round. A stream
/// buffer stores one so that when it blocks for room, the wait is charged to THIS buffer and not to
/// the undifferentiated pile every stream ring used to share.
enum class GpuWaitSite : u8;

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
	__fi u32 GetCurrentSpace() const { return m_current_space; }
	__fi u32 GetCurrentOffset() const { return m_current_offset; }

	/// `wait_site` is who gets charged when this buffer runs out of room and blocks. Passed at
	/// creation rather than set afterwards because a buffer that serves traffic before anybody
	/// names it books its waits somewhere useless, and there is no way to notice.
	bool Create(VkBufferUsageFlags usage, u32 size, GpuWaitSite wait_site);
	void Destroy(bool defer);

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);

private:
	bool AllocateBuffer(VkBufferUsageFlags usage, u32 size);
	void UpdateCurrentFencePosition();
	void UpdateGPUPosition();

	// Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
	bool WaitForClearSpace(u32 num_bytes);

	u32 m_size = 0;
	u32 m_current_offset = 0;
	u32 m_current_space = 0;
	u32 m_current_gpu_position = 0;

	VmaAllocation m_allocation = VK_NULL_HANDLE;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	u8* m_host_pointer = nullptr;
	// Set to GpuWaitSite::StreamUnnamed by the constructor, which is out of line in the .cpp
	// because only there is the enum's definition visible.
	GpuWaitSite m_wait_site;

	// List of fences and the corresponding positions in the buffer
	std::deque<std::pair<u64, u32>> m_tracked_fences;
};
