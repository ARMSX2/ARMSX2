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
// PSMCT32/PSMCT24 only: a Format::Color (RGBA8) target whose pixel space is its guest layout
// (page-aligned base, width = bw * 64, page (base + row*bw + col) at pixel rect (col*64, row*32)).

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

// The CT32 swizzle forms, byte-identical to tilegpu.glsl: block-in-page and word-in-block column.
#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

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

void main()
{
	// One page per workgroup column: dispatch is (8, 4, entry_count) groups of 8x8 -> a 64x32 CT32 page.
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

	if (byte_mask == 0xFFFFFFFFu)
		vram_words[rw] = w;
	else
		vram_words[rw] = (vram_words[rw] & ~byte_mask) | (w & byte_mask);
}
