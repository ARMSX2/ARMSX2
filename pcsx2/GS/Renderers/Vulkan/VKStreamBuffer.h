// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSStreamRingFlushRange.h"
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

	/// Cleans everything committed since the last call out of the CPU's caches, on a ring whose
	/// memory type is not coherent. A no-op on every other ring, and on a non-coherent ring that
	/// has not been written since the last flush.
	///
	/// Must be called on every ring before the queue submission that reads it, which is the only
	/// point the GPU can see any of it: command recording does not execute anything, so a draw
	/// recorded into the command buffer being built reads nothing until that buffer is submitted.
	/// GSDeviceVK::SubmitCommandBuffer does it for all six, immediately before vkQueueSubmit.
	void FlushPendingWrites();

	/// The cache maintenance this ring has paid so far: cleans issued, committed regions those
	/// cleans covered, bytes cleaned. Zero for the whole run on a coherent ring.
	__fi u64 GetFlushCalls() const { return m_flush_calls; }
	__fi u64 GetFlushCommits() const { return m_flush_commits; }
	__fi u64 GetFlushBytes() const { return m_flush_bytes; }

	/// Hold the newest committed range until the command buffer being recorded NOW retires, not
	/// merely until the one it was committed under does.
	///
	/// The ring's whole model is "a range is free once the command buffer that was recording when
	/// it was committed has completed", and that holds as long as the only reader is that command
	/// buffer. A caller that stages once and then keeps recording across a SUBMISSION breaks it:
	/// the range is tracked against the first submission, which retires first, and the ring then
	/// hands the same bytes out again while the later submissions are still reading them. Worse
	/// than a wrap-round overwrite, because UpdateGPUPosition's "the GPU caught up" case resets
	/// the whole ring to offset zero the moment the tracked position equals the write position, so
	/// the very next staging lands on top of the live data.
	///
	/// Calling this at each submission boundary walks the range's tracked fence forward with the
	/// recording, so it ends up on the last submission that actually reads it. It only ever moves
	/// a fence FORWARD, so it can delay a reuse and never permit one.
	///
	/// Nothing on this branch stages across a submission yet -- the classic renderer stages a
	/// draw's vertices and records the draw with no submission in between, which is exactly the
	/// shape the ring's own rule assumes. A pass planner that stages a whole frame before opening
	/// its first pass does not, and has to call this at every cut.
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
	// Set to GpuWaitSite::StreamUnnamed by the constructor, which is out of line in the .cpp
	// because only there is the enum's definition visible.
	GpuWaitSite m_wait_site;

	// List of fences and the corresponding positions in the buffer
	std::deque<std::pair<u64, u32>> m_tracked_fences;

	/// Whether the memory type this ring landed on lacks HOST_COHERENT, in which case its writes
	/// need a real cache clean before the GPU reads them. False on every coherent ring, and then
	/// nothing below it is ever touched.
	bool m_non_coherent = false;

	/// What has been committed since the last flush, at most one range plus one more for a wrap.
	GSStreamRingFlushRanges m_pending_flush;

	/// The price of the cached non-coherent road, as a run has to be able to state it: how many
	/// cache cleans were issued, how many committed regions they covered -- the number of cleans
	/// the per-commit road would have issued for the same bytes -- and how many bytes were cleaned.
	u64 m_flush_calls = 0;
	u64 m_flush_commits = 0;
	u64 m_flush_bytes = 0;
};
