// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The CPU half of the GPU texture decode: which blocks of GS local memory a paletted texture window
// reads, in what order, and where its decoded rows land in the scratch buffer. The decode shader
// (bin/resources/shaders/vulkan/vram_mirror.glsl) only does the swizzle INSIDE a block; every block
// number comes from here, so TBW, page tables, block tables and the wrap at 16384 blocks are the
// same arithmetic ReadTexture uses (GSOffset::bnMulti), not a second copy of it.
//
// Header only and free of Vulkan, so the host tests can pin it without a device.

#include "GS/GSLocalMemory.h"

#include <vector>

namespace GSVramMirror
{
	constexpr u32 kBlockBytes = 256;
	constexpr u32 kMirrorBlocks = GS_MAX_BLOCKS; // 16384 blocks, 4 MiB
	constexpr u32 kMirrorBytes = kMirrorBlocks * kBlockBytes;

	/// A block-table entry is a 256-byte block index plus this bit, which selects the staging
	/// buffer over the mirror. Without it the entry indexes the mirror.
	constexpr u32 kTableStagedBit = 0x80000000u;

	/// The shader switches on this value (job record word 0). Keep in step with vram_mirror.glsl.
	enum class Format : u32
	{
		T8 = 0,
		T4 = 1,
		T8H = 2,
		T4HL = 3,
		T4HH = 4,
	};

	constexpr u32 kFormatCount = 5;

	inline bool FormatFromPSM(u32 psm, Format* out)
	{
		switch (psm)
		{
			case PSMT8: *out = Format::T8; return true;
			case PSMT4: *out = Format::T4; return true;
			case PSMT8H: *out = Format::T8H; return true;
			case PSMT4HL: *out = Format::T4HL; return true;
			case PSMT4HH: *out = Format::T4HH; return true;
			default: return false;
		}
	}

	/// Where a window sits in the blocks it reads and in the scratch buffer.
	struct WindowLayout
	{
		Format format;
		GSVector4i rect;       ///< the texels asked for
		GSVector4i block_rect; ///< rect grown to whole blocks, as PreloadTexture does
		u32 blocks_wide;
		u32 blocks_high;
		u32 block_texels_w; ///< texels per block row; one byte per texel in scratch
		u32 block_texels_h;
		u32 pitch;         ///< bytes per scratch row, a multiple of 16
		u32 scratch_bytes; ///< pitch * rows
		u32 crop_x;        ///< rect.left - block_rect.left
		u32 crop_y;

		u32 BlockCount() const { return blocks_wide * blocks_high; }
		/// One unit of shader work is one 64-byte column of one block.
		u32 UnitCount() const { return BlockCount() * 4; }
		/// Where rect's first texel is, relative to the window's first scratch byte.
		u32 CopyOffset() const { return crop_y * pitch + crop_x; }
	};

	/// Fails on a format the shader does not decode or an empty rect. `rect` is in the texel space of
	/// the surface `off` addresses.
	inline bool ComputeLayout(const GSOffset& off, const GSVector4i& rect, WindowLayout* out)
	{
		Format fmt;
		if (!FormatFromPSM(off.psm(), &fmt) || rect.width() <= 0 || rect.height() <= 0 || rect.left < 0 || rect.top < 0)
			return false;

		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[off.psm()];
		WindowLayout w;
		w.format = fmt;
		w.rect = rect;
		w.block_rect = rect.ralign<Align_Outside>(psm.bs);
		w.block_texels_w = static_cast<u32>(psm.bs.x);
		w.block_texels_h = static_cast<u32>(psm.bs.y);
		w.blocks_wide = static_cast<u32>(w.block_rect.width()) / w.block_texels_w;
		w.blocks_high = static_cast<u32>(w.block_rect.height()) / w.block_texels_h;
		w.pitch = (w.blocks_wide * w.block_texels_w + 15u) & ~15u;
		w.scratch_bytes = w.pitch * w.blocks_high * w.block_texels_h;
		w.crop_x = static_cast<u32>(rect.left - w.block_rect.left);
		w.crop_y = static_cast<u32>(rect.top - w.block_rect.top);
		*out = w;
		return true;
	}

	/// The block number of every block of the window, in the order ReadTexture visits them (rows of
	/// blocks, left to right), already wrapped to 16384. This is GSLocalMemoryFunctions::foreachBlock.
	inline void BuildBlockNumbers(const GSOffset& off, const WindowLayout& w, std::vector<u32>* out)
	{
		out->clear();
		out->reserve(w.BlockCount());
		GSOffset::BNHelper bn = off.bnMulti(w.block_rect.left, w.block_rect.top);
		const int right = w.block_rect.right >> off.blockShiftX();
		const int bottom = w.block_rect.bottom >> off.blockShiftY();
		for (; bn.blkY() < bottom; bn.nextBlockY())
			for (; bn.blkX() < right; bn.nextBlockX())
				out->push_back(bn.value());
	}
} // namespace GSVramMirror
