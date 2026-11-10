// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "x86/BaseblockEx.h" // BASEBLOCK

// Recompiler block lookup table: one BASEBLOCK per guest instruction slot, all pointing at the compile
// stub until compiled. On Linux only written pages use RAM, and Reset() just drops them.
class BlockLut
{
public:
	BlockLut() = default;
	~BlockLut();

	BlockLut(const BlockLut&) = delete;
	BlockLut& operator=(const BlockLut&) = delete;

	/// Sizes the table for `count` entries. Their contents are undefined until Reset().
	void Resize(size_t count);

	/// Sets every entry to `fnptr`.
	void Reset(uptr fnptr);

	void Free();

	BASEBLOCK* data() const { return m_data; }
	size_t size() const { return m_count; }

private:
	bool MapPattern(size_t bytes);

	BASEBLOCK* m_data = nullptr;
	size_t m_count = 0;
	size_t m_bytes = 0; // what m_data spans: the table rounded up to whole pattern blocks when mapped
	int m_fd = -1; // the memfd holding one pattern block, or -1 for the heap array
	uptr m_pattern = 0; // the value the memfd holds (a new memfd reads as zeros)
};
