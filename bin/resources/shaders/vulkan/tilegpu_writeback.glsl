// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu writeback: target -> bytes. Reswizzle a resident colour target's finished pixels into the
// frame's ring, one guest page per workgroup column, at the exact words tilegpu.glsl's texel path
// reads for those texels -- so a later draw that samples pages this target holds newest finds this
// frame's render (or last frame's: targets persist) with no change on the read side. It is the
// inverse of tilegpu_unpack at the address of tilegpu_texel32; the swizzle forms are the same
// TILE_SWZ_* defines the sampler uses, so the two cannot disagree about a constant.
//
// Addressing goes through the frame's epoch page table exactly as the reader's does:
// table[table_base + epoch*512 + page] names the page's ring slot. The op's page list (staged
// beside the table) says which pages, and per page which of its 32 physical blocks the surface
// holds newest -- a page a CPU transfer has partially reclaimed keeps its CPU-newest blocks (the
// executor prefilled the slot with the CPU shadow's bytes; this pass writes only the surface's
// blocks over them). byte_mask does the same at byte granularity within a word: a PSMCT24 surface
// owns bytes 0-2 and leaves the alpha byte to whoever holds that plane.
//
// The target is a Format::Color (RGBA8) image whose pixel space is its guest layout: page-aligned
// base, width = bw * 64, page (base + row*bw + col) at pixel rect (col*64, row*pgh).
//
// TILEGPU_WB_FMT (injected by the device) picks the swizzle universe. Each value compiles its own
// path and nothing else, the way tilegpu_materialise.glsl does it -- a build is one pipeline per
// format, and a runtime branch on page geometry would be paid per invocation of every page:
//   0 = PSMCT32 / PSMCT24 -- 64x32-texel pages of 8x8 blocks, one 32-bit word per texel, so the
//       invocation grid IS the page and the guest word is the pixel.
//   1 = PSMCT16, 2 = PSMCT16S -- 64x64-texel pages of 16x8 blocks, two texels to a word. The two
//       differ only in the block table (blockTable16 is byte-identical to blockTable4, hence B84;
//       blockTable16S is its own form, B84S) and share columnTable16.
//
// ⚠️ The 16-bit arms write a WHOLE word from ONE invocation, covering the pair of texels eight
// apart that columnTable16 puts in its two halves. That is not packing for its own sake: the two
// texels sit in different 8x8 workgroups, so writing a halfword each would be two read-modify-write
// sequences on one word with no ordering between them. Pairing them makes the store a plain write
// and the whole pass race-free, exactly as the 32-bit arm already is. The pairing is unit-pinned
// (GSTileSwizzleForms::Locate16, TheSixteenBitWordPairsAreEightTexelsApart).

#ifndef TILEGPU_WB_FMT
#define TILEGPU_WB_FMT 0
#endif

layout(local_size_x = 8, local_size_y = 8) in;

// The finished colour target, sampled point (texelFetch ignores filtering). Logical RGBA regardless
// of the native byte order, matching tilegpu_unpack's logical-RGBA read.
layout(set = 0, binding = 0) uniform sampler2D src;

// The frame's ring: page slots, epoch page tables and the op's page-entry list, one word array.
// Read for the table, the entries and the read-modify-write; written for the surface's bytes.
layout(std430, set = 0, binding = 1) buffer Vram
{
	uint vram_words[];
};

layout(push_constant) uniform cb
{
	uint table_base;   // this frame's first page-table word in the ring
	uint epoch;        // which epoch's table names the destination slots
	uint bp;           // the surface's base in blocks (page-aligned)
	uint bw;           // the surface's stride in pages
	uint byte_mask;    // bytes of each word this surface owns (0xFFFFFFFF, or 0x00FFFFFF for CT24)
	uint entries_base; // ring word of the plan's page-entry array ({page, pad}, block_mask pairs)
	uint first_entry;  // this op's first entry index into that array
	uint entry_count;
};

// The swizzle forms, byte-identical to tilegpu.glsl's: block-in-page and unit-in-block column.
#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

#if TILEGPU_WB_FMT == 0

uint tile_b48(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B48_X0) ^ XB(x, 1u, TILE_SWZ_B48_X1) ^ XB(x, 2u, TILE_SWZ_B48_X2)
	     ^ XB(y, 0u, TILE_SWZ_B48_Y0) ^ XB(y, 1u, TILE_SWZ_B48_Y1);
}

uint tile_c32(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C32_X0) ^ XB(x, 1u, TILE_SWZ_C32_X1) ^ XB(x, 2u, TILE_SWZ_C32_X2)
	     ^ XB(y, 0u, TILE_SWZ_C32_Y0) ^ XB(y, 1u, TILE_SWZ_C32_Y1) ^ XB(y, 2u, TILE_SWZ_C32_Y2);
}

#else

// The 16-bit block form: B84 for PSMCT16 (blockTable16 == blockTable4, checked at fit time),
// B84S for PSMCT16S.
uint tile_b16(uint x, uint y)
{
#if TILEGPU_WB_FMT == 1
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
#else
	return XB(x, 0u, TILE_SWZ_B84S_X0) ^ XB(x, 1u, TILE_SWZ_B84S_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84S_Y0) ^ XB(y, 1u, TILE_SWZ_B84S_Y1) ^ XB(y, 2u, TILE_SWZ_B84S_Y2);
#endif
}

// columnTable16: (x 0..15, y 0..7) -> the halfword within the block's 128. Its low bit is bit 3 of
// x, which is what puts texel x and texel x+8 in the two halves of one word.
uint tile_c16(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C16_X0) ^ XB(x, 1u, TILE_SWZ_C16_X1) ^ XB(x, 2u, TILE_SWZ_C16_X2) ^ XB(x, 3u, TILE_SWZ_C16_X3)
	     ^ XB(y, 0u, TILE_SWZ_C16_Y0) ^ XB(y, 1u, TILE_SWZ_C16_Y1) ^ XB(y, 2u, TILE_SWZ_C16_Y2);
}

// A finished pixel packed to A1B5G5R5: the top five bits of each colour byte and the MSB of alpha,
// which is GSLocalMemory::WriteFrame16's arithmetic and the pool's readback store's. Integer
// throughout after the one rounding multiply -- the same round-to-nearest the 32-bit arm uses, so
// a byte written by one arm and read by the other is the same byte.
uint tilegpu_pack5551(vec4 c)
{
	const uint r = uint(clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint g = uint(clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint b = uint(clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint a = uint(clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
	return (r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) | ((a >> 7u) << 15u);
}

#endif

void main()
{
	// One page per workgroup column: dispatch is (grid, entry_count) groups of 8x8, where the grid
	// is the page's shape in WORDS -- (8, 4) for a 64x32 CT32 page, (4, 8) for a 64x64 16-bit one
	// whose words each hold two texels. Either way 2048 invocations, one guest word each.
	const uint entry_index = gl_WorkGroupID.z;
	if (entry_index >= entry_count)
		return;
	const uint eword = entries_base + (first_entry + entry_index) * 2u;
	const uint page = vram_words[eword] & 0xFFFFu;
	const uint block_mask = vram_words[eword + 1u];

	// The page's pixel rect in the target: pool row/column from its offset off the base page.
	const uint rel = (page + 512u - (bp >> 5u)) & 511u;
	const uint col = rel % bw;
	const uint row = rel / bw;

#if TILEGPU_WB_FMT == 0
	const uint u = col * 64u + gl_WorkGroupID.x * 8u + gl_LocalInvocationID.x;
	const uint v = row * 32u + gl_WorkGroupID.y * 8u + gl_LocalInvocationID.y;

	// Block within the page; only the surface's blocks travel.
	const uint bib = tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u);
	if ((block_mask & (1u << bib)) == 0u)
		return;

	// The exact ring word tilegpu_texel32(u, v, bp, bw, epoch) will read for this texel.
	const uint slot = vram_words[table_base + epoch * 512u + page];
	const uint rw = slot + bib * 64u + tile_c32(u & 7u, v & 7u);

	// Pack the finished pixel to a raw RGBA8888 word: the inverse of tilegpu_unpack, round to nearest.
	const vec4 c = texelFetch(src, ivec2(int(u), int(v)), 0);
	const uint w = (uint(clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f))
	             | (uint(clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f) << 8u)
	             | (uint(clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f) << 16u)
	             | (uint(clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24u);
#else
	// gx walks the 32 word-columns of a 16-bit page row, gy its 64 pixel rows. Word-column gx owns
	// the LOW half of one 16-texel group -- texel (gx>>3)*16 + (gx&7) -- and the high half is that
	// texel plus eight.
	const uint gx = gl_WorkGroupID.x * 8u + gl_LocalInvocationID.x;
	const uint gy = gl_WorkGroupID.y * 8u + gl_LocalInvocationID.y;
	const uint u = col * 64u + (gx >> 3u) * 16u + (gx & 7u);
	const uint v = row * 64u + gy;

	// Block within the page: 16x8-texel blocks, four across and eight down. u and u+8 share it,
	// because they differ only in bit 3.
	const uint bib = tile_b16((u >> 4u) & 3u, (v >> 3u) & 7u);
	if ((block_mask & (1u << bib)) == 0u)
		return;

	// The word both halves live in: the low texel's halfword is even, the high texel's is that + 1.
	const uint slot = vram_words[table_base + epoch * 512u + page];
	const uint rw = slot + bib * 64u + (tile_c16(u & 15u, v & 7u) >> 1u);

	const uint w = tilegpu_pack5551(texelFetch(src, ivec2(int(u), int(v)), 0))
	             | (tilegpu_pack5551(texelFetch(src, ivec2(int(u + 8u), int(v)), 0)) << 16u);
#endif

	if (byte_mask == 0xFFFFFFFFu)
		vram_words[rw] = w;
	else
		vram_words[rw] = (vram_words[rw] & ~byte_mask) | (w & byte_mask);
}
