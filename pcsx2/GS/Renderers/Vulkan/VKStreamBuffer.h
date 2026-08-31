// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <deque>
#include <memory>

class VKStreamBuffer
{
public:
	/// Which HOST attributes to ask the allocator for. It is deliberately not a choice of HEAP:
	/// on every target this campaign runs -- Adreno 650, the Mali-G615 tier and Apple/Honeykrisp
	/// -- every non-lazy memory type is DEVICE_LOCAL *and* HOST_VISIBLE, so "put this buffer in
	/// device memory" is not a thing the hardware offers. The only axis that exists is the CPU
	/// cache attribute of the type, and on Adreno 650 the shipped preference below lands the ring
	/// on type 0: DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT, i.e. an UNCACHED / write-combining
	/// mapping. Everything but the TileGpu ring passes Default and keeps the shipped allocation.
	enum class MemoryClass : u8
	{
		/// Prefer HOST_COHERENT. What every stream buffer has always allocated.
		Default = 0,
		/// Prefer HOST_CACHED. On Adreno 650 this moves the buffer off the write-combining type 0
		/// onto type 1 (DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED) -- cached, and
		/// still coherent, so nothing about flushing changes.
		HostCached = 1,
		/// Require HOST_CACHED and refuse every type carrying HOST_COHERENT. On Adreno 650 that is
		/// type 2 (DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED): cached and NOT snooped, which is the
		/// only rung that gets the GPU a plain write-back cacheable mapping. Correctness then rests
		/// entirely on CommitMemory's flush, which is unconditional and always has been.
		///
		/// Falls back to HostCached where the device has no such type -- Apple/Honeykrisp offers
		/// only 0x0f and 0x07, both coherent -- rather than failing to allocate.
		HostCachedIncoherent = 2,
		/// The MIRROR of HostCached: refuse every type carrying HOST_CACHED, i.e. ask for the
		/// uncached / write-combining one. It is not a candidate for shipping -- it is the arm that
		/// makes the other two testable.
		///
		/// On Adreno 650 it is the SHIPPED type (0x07 is already what class 0 selects there), so a
		/// device A/B that shows class 3 differing from class 0 in anything but noise has a harness
		/// fault, not a finding. On Apple/Honeykrisp it is the only rung that MOVES: the ring goes
		/// from type 0 (0x0f, cached) to type 1 (0x07, uncached), which is the same axis the Adreno
		/// arm walks, just with the two ends available in the opposite order. That is what lets a
		/// machine with no incoherent memory type still prove this ladder selects a real second type
		/// and still renders the same bytes from it.
		HostUncached = 3,
	};

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

	bool Create(VkBufferUsageFlags usage, u32 size, MemoryClass mem_class = MemoryClass::Default);
	void Destroy(bool defer);

	/// What the allocator actually GRANTED, not what was asked for. A memory-class arm that moves
	/// nothing on a device is uninterpretable without this -- "the type did not change" and "the
	/// type changed and cost nothing" are different findings and only these say which happened.
	__fi VkMemoryPropertyFlags GetMemoryProperties() const { return m_memory_properties; }
	__fi u32 GetMemoryTypeIndex() const { return m_memory_type_index; }
	__fi bool IsHostCoherent() const
	{
		return (m_memory_properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	}
	__fi bool IsHostCached() const { return (m_memory_properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0; }

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);

	/// Hold the newest committed range until the command buffer being recorded NOW retires, not
	/// merely until the one it was committed under does.
	///
	/// The ring's whole model is "a range is free once the command buffer that was recording when
	/// it was committed has completed", and that holds as long as the only reader is that command
	/// buffer. A caller that stages once and then keeps recording across a SUBMISSION breaks it,
	/// which is what every mid-plan flush in the TileGpu executor does: the range is tracked
	/// against the first submission, which retires first, and the ring then hands the same bytes
	/// out again while the later submissions are still reading them. Worse than a wrap-round
	/// overwrite, because UpdateGPUPosition's "the GPU caught up" case resets the whole ring to
	/// offset zero the moment the tracked position equals the write position, so the very next
	/// staging lands on top of the live data.
	///
	/// Calling this at each submission boundary walks the range's tracked fence forward with the
	/// recording, so it ends up on the last submission that actually reads it. It only ever moves
	/// a fence FORWARD, so it can delay a reuse and never permit one.
	void RetainForCurrentCommandBuffer();

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
	VkMemoryPropertyFlags m_memory_properties = 0;
	u32 m_memory_type_index = 0;

	// List of fences and the corresponding positions in the buffer
	std::deque<std::pair<u64, u32>> m_tracked_fences;
};
