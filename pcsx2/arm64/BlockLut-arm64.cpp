// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "arm64/BlockLut-arm64.h"

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/BitUtils.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

// How much one private mapping of the pattern covers.
static constexpr size_t PATTERN_BYTES = 1u << 20;

BlockLut::~BlockLut()
{
	Free();
}

void BlockLut::Free()
{
#if defined(__linux__)
	if (m_fd >= 0)
	{
		if (m_data)
			munmap(m_data, m_bytes);
		close(m_fd);
	}
	else
#endif
	{
		_aligned_free(m_data);
	}

	m_data = nullptr;
	m_count = 0;
	m_bytes = 0;
	m_fd = -1;
	m_pattern = 0;
}

void BlockLut::Resize(size_t count)
{
	if (m_data && count == m_count)
		return;

	Free();
	const size_t bytes = count * sizeof(BASEBLOCK);
	if (!MapPattern(bytes))
	{
		m_bytes = bytes;
		m_data = static_cast<BASEBLOCK*>(_aligned_malloc(bytes, 4096));
		pxAssertRel(m_data, "Failed to allocate the block lookup table.");
	}
	m_count = count;
}

bool BlockLut::MapPattern(size_t bytes)
{
#if defined(__linux__)
	// memfd_create through syscall(): bionic has no wrapper below API 30.
	const int fd = static_cast<int>(syscall(__NR_memfd_create, "pcsx2_block_lut", MFD_CLOEXEC));
	if (fd < 0)
		return false;
	if (ftruncate(fd, PATTERN_BYTES) != 0)
	{
		close(fd);
		return false;
	}

	// Reserve the span, then lay private mappings of the one pattern block over all of it.
	const size_t mapped = Common::AlignUpPow2(bytes, PATTERN_BYTES);
	u8* const base = static_cast<u8*>(
		mmap(nullptr, mapped, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
	if (base == MAP_FAILED)
	{
		close(fd);
		return false;
	}
	for (size_t offset = 0; offset < mapped; offset += PATTERN_BYTES)
	{
		if (mmap(base + offset, PATTERN_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED)
		{
			munmap(base, mapped);
			close(fd);
			return false;
		}
	}

	m_data = reinterpret_cast<BASEBLOCK*>(base);
	m_bytes = mapped;
	m_fd = fd;
	m_pattern = 0;
	return true;
#else
	return false;
#endif
}

void BlockLut::Reset(uptr fnptr)
{
#if defined(__linux__)
	if (m_fd >= 0)
	{
		if (fnptr != m_pattern)
		{
			// Through a shared view of the memfd: every private page nobody has written reads it.
			void* const view = mmap(nullptr, PATTERN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
			pxAssertRel(view != MAP_FAILED, "Failed to map the block lookup table's pattern.");
			BASEBLOCK* const blocks = static_cast<BASEBLOCK*>(view);
			for (size_t i = 0; i < PATTERN_BYTES / sizeof(BASEBLOCK); i++)
				blocks[i].SetFnptr(fnptr);
			munmap(view, PATTERN_BYTES);
			m_pattern = fnptr;
		}

		// The pages written since the last reset go back to the host and read as the pattern again.
		// If that fails, the fill below still resets the table: stale entries would jump into a wiped code cache.
		if (madvise(m_data, m_bytes, MADV_DONTNEED) == 0)
			return;
	}
#endif

	for (size_t i = 0; i < m_count; i++)
		m_data[i].SetFnptr(fnptr);
}
