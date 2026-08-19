// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu backend samples guest textures out of raw bytes: its fragment shader
// (bin/resources/shaders/vulkan/tilegpu.glsl) turns a texel coordinate into a guest
// address with the GS's own page/block/column swizzle, evaluated from the closed forms
// GSTileSwizzleForms fits at runtime. Nothing on that road can be checked by looking at
// the picture -- a wrong address produces plausible texture from the wrong place, which
// is exactly what a block-granular defect looks like on screen.
//
// So the shader's arithmetic is transcribed here, once per format it serves, and pinned
// against GSLocalMemory's own PixelAddress functions -- the addresses the software
// renderer and every CPU reader use. A disagreement here is a disagreement with the
// bytes the rest of the emulator believes in.
//
// The coordinate ranges deliberately cross page boundaries in both axes, and the layouts
// include a base that sits blocks into a page and the buffer widths the corpus actually
// uses (TBW 1 through 8, and the halved pages-per-row a paletted format addresses by).

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include <gtest/gtest.h>

using namespace GSTileSwizzleForms;

namespace
{
	// -- the shader, transcribed -------------------------------------------------------
	// One function per tilegpu.glsl texel reader. Each returns the guest address in the
	// unit the shader indexes with: words for the 32-bit road, bytes for PSMT8, nibbles
	// for PSMT4. `tbw` is what the renderer puts in the state row -- pages per texture
	// row, which for a paletted format is TBW >> 1 (GSOffset's bw >> (pageShiftX - 6)).

	u32 ShaderTexel32Word(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 5) * tbw + (u >> 6);
		const u32 blk = tbp0 + page * 32 + f.block48.Eval((u >> 3) & 7, (v >> 3) & 3);
		return blk * 64 + f.col32.Eval(u & 7, v & 7);
	}

	u32 ShaderIndex8Byte(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 6) * tbw + (u >> 7);
		const u32 blk = tbp0 + page * 32 + f.block48.Eval((u >> 4) & 7, (v >> 4) & 3);
		return blk * 256 + f.col8.Eval(u & 15, v & 15);
	}

	u32 ShaderIndex4Nibble(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 7) * tbw + (u >> 7);
		const u32 blk = tbp0 + page * 32 + f.block84.Eval((u >> 5) & 3, (v >> 4) & 7);
		return blk * 512 + f.col4.Eval(u & 31, v & 15);
	}
} // namespace

TEST(GSTileGpuTexelAddress, Direct32MatchesPixelAddress32)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			for (u32 v = 0; v < 72; v++)
			{
				for (u32 u = 0; u < static_cast<u32>(tbw) * 64 + 8; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress32(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderTexel32Word(f, u, v, tbp0, tbw), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

TEST(GSTileGpuTexelAddress, Paletted8MatchesPixelAddress8)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	// TBW is the GS register; the state row carries pages per row, TBW >> 1. TBW=1 gives
	// zero pages per row, which is GSOffset's arithmetic and not a clamp -- a 128-wide
	// 8-bit texture never leaves its first page column, so the term never contributes.
	for (const u32 tbw : {1u, 2u, 4u, 6u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			const u32 pages_per_row = tbw >> 1;
			const u32 width = std::max<u32>(pages_per_row, 1) * 128 + 16;
			for (u32 v = 0; v < 136; v++)
			{
				for (u32 u = 0; u < width; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress8(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderIndex8Byte(f, u, v, tbp0, pages_per_row), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

TEST(GSTileGpuTexelAddress, Paletted4MatchesPixelAddress4)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 6u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			const u32 pages_per_row = tbw >> 1;
			const u32 width = std::max<u32>(pages_per_row, 1) * 128 + 16;
			for (u32 v = 0; v < 264; v++)
			{
				for (u32 u = 0; u < width; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress4(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(ShaderIndex4Nibble(f, u, v, tbp0, pages_per_row), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}
