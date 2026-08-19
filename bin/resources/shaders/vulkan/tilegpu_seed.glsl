// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu seed: bytes -> target. Before a draw renders into guest pages its colour target does not
// yet hold newest (a fresh target, pages a CPU upload reclaimed, pages another target owned under a
// different layout), unswizzle those pages out of the frame's ring into the target's pixel space, so
// the draw lands on the same bytes the GS would have found. The ring slot for a page is whatever the
// renderer composed for this epoch -- the CPU shadow's bytes, another target's writeback, or both --
// and this pass only asks the epoch page table where it is, exactly as the texel path does.
//
// A full-target triangle; fragments outside the op's page set discard, so one draw serves any page
// subset. Reads are the inverse of tilegpu_writeback.glsl's writes at the same address, through the
// same TILE_SWZ_* forms. PSMCT32/PSMCT24 targets (Format::Color, page-aligned base).

#ifdef VERTEX_SHADER

void main()
{
	// Full-viewport triangle from the vertex index; no attributes.
	const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0f - 1.0f, 0.0f, 1.0f);
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) out vec4 o_color;

// The frame's ring: page slots, epoch page tables and the op's 512-bit page mask, one word array.
layout(std430, set = 0, binding = 1) readonly buffer Vram
{
	uint vram_words[];
};

layout(push_constant) uniform cb
{
	uint table_base; // this frame's first page-table word in the ring
	uint epoch;      // which epoch's table names the source slots
	uint bp;         // the surface's base in blocks (page-aligned)
	uint bw;         // the surface's stride in pages
	uint mask_base;  // ring word of this op's 16-word page mask (bit page&31 of word page>>5)
};

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
	const uint x = uint(gl_FragCoord.x);
	const uint y = uint(gl_FragCoord.y);

	// The guest page this pixel lives in under the surface layout (page-aligned base: pool page
	// (row, col) is physical page base + row*bw + col, wrapping at the end of memory).
	const uint rel = (y >> 5u) * bw + (x >> 6u);
	const uint page = ((bp >> 5u) + rel) & 511u;
	if ((vram_words[mask_base + (page >> 5u)] & (1u << (page & 31u))) == 0u)
		discard;

	// The exact ring word tilegpu_writeback.glsl wrote (or the executor prefilled) for this texel.
	const uint bib = tile_b48((x >> 3u) & 7u, (y >> 3u) & 3u);
	const uint slot = vram_words[table_base + epoch * 512u + page];
	const uint w = vram_words[slot + bib * 64u + tile_c32(x & 7u, y & 7u)];

	o_color = vec4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
	               float((w >> 24u) & 0xFFu)) * (1.0f / 255.0f);
}

#endif
