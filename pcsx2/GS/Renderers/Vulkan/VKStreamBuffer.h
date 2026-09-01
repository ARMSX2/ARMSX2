// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "vk_mem_alloc.h"

#include <deque>
#include <memory>

/// Defined in GSDeviceVK.h, which includes this header — so this is an opaque-enum-declaration
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

	/// `allow_wait` false means "tell me whether there is room, do not block to make room": the
	/// last-resort WaitForClearSpace is skipped and the call returns false instead. A caller that
	/// has recorded work the GPU has not seen yet can use that to hand the work over FIRST and
	/// block afterwards, which is a strictly deeper pipeline than blocking with the queue short.
	bool ReserveMemory(u32 num_bytes, u32 alignment, bool allow_wait = true);
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

	/// The same thing, but leaving behind the bytes the live reservation does not cover.
	///
	/// RetainForCurrentCommandBuffer moves the WHOLE newest range forward, and at a mid-plan cut
	/// that range is everything staged since the last cut -- most of which no longer has a reader,
	/// because the draws that read it are already recorded into the submission that is going out.
	/// Since a commit that lands on the same fence counter merges into that range rather than
	/// opening a new one, the range never subdivides: every cut slides it onto the buffer now
	/// recording, so a whole frame's staging ends up pinned to the frame's LAST submission and the
	/// ring's effective depth is measured in frames instead of submissions.
	///
	/// `keep_from_offset` is where the reservation the caller is still recording against begins.
	/// Bytes below it retire with the submission that read them; bytes at or above it ride forward,
	/// which is the part the correctness argument is actually about. Falls back to the blanket
	/// retain whenever the offset does not sit strictly inside the newest range -- a wrap, an empty
	/// live range, a caller with nothing staged -- because retaining too much is only slow and
	/// retaining too little is corruption.
	void RetainForCurrentCommandBufferFrom(u32 keep_from_offset);

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
