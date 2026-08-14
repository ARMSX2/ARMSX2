// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>
#include <sys/mman.h>

// RAII executable buffer, allocated exactly like PCSX2's macOS JIT regions
// (SharedMemoryMappingArea::Create with jit=true): MAP_JIT so that
// pthread_jit_write_protect_np can flip it between writable and executable.
//
// Emit into it through the recompilers' own lifecycle — armSetAsmPtr, then
// armStartBlock/armEndBlock, which toggle the write protection and flush the
// I-cache — and call the returned pointer.
class JitBuffer
{
public:
	explicit JitBuffer(size_t size)
		: m_size(size)
	{
		int flags = MAP_ANONYMOUS | MAP_PRIVATE;
#ifdef __APPLE__
		flags |= MAP_JIT;
#endif
		void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
		m_ptr = (ptr == MAP_FAILED) ? nullptr : ptr;
	}

	~JitBuffer()
	{
		if (m_ptr)
			munmap(m_ptr, m_size);
	}

	JitBuffer(const JitBuffer&) = delete;
	JitBuffer& operator=(const JitBuffer&) = delete;

	void* ptr() const { return m_ptr; }
	size_t size() const { return m_size; }

private:
	void* m_ptr = nullptr;
	size_t m_size;
};
