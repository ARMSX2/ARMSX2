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
// same TILE_SWZ_* forms. Colour targets (Format::Color, page-aligned base) in every colour swizzle
// universe.
//
// TILEGPU_SEED_FMT (injected by the device) picks the universe, one pipeline per value, matching
// tilegpu_writeback.glsl's TILEGPU_WB_FMT exactly:
//   0 = PSMCT32 / PSMCT24 -- 64x32-texel pages of 8x8 blocks, the guest word IS the texel.
//   1 = PSMCT16, 2 = PSMCT16S -- 64x64-texel pages of 16x8 blocks, two texels to a word, expanded
//       from A1B5G5R5 to RGBA8. The two differ only in the block table.
//   3 = PSMZ32 / PSMZ24 -- arm 0 under the depth block XOR, for a colour surface a game made by
//       setting FRAME.PSM to a depth format. In-page constant, so the page term is arm 0's.
//
// ⚠️ The 16-bit expansion is the FRAME buffer's, not a texture read's: five bits back to the top of
// each byte, low bits zero, and the alpha bit to 0x80 or 0x00. That is GSLocalMemory::Expand16To32
// under the pool's TEXA pin (AEM = 0, TA0 = 0, TA1 = 0x80), and it is the exact inverse of the
// writeback's pack, so a cell that goes out through one comes back through the other unchanged. A
// game's TEXA belongs to the texture road and has no business here.

#ifndef TILEGPU_SEED_FMT
#define TILEGPU_SEED_FMT 0
#endif

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

// Arm 0 and arm 3 share this geometry; arm 3 adds the depth block XOR. Kept in the same two
// spellings tilegpu_writeback.glsl uses, because the seed is that pass run backwards and a
// disagreement here reads a page back from blocks the writeback never wrote.
#define TILEGPU_SEED_CT32 (TILEGPU_SEED_FMT == 0 || TILEGPU_SEED_FMT == 3)

#if TILEGPU_SEED_FMT == 3
#define TILEGPU_SEED_ZXOR TILE_SWZ_Z32XOR
#else
#define TILEGPU_SEED_ZXOR 0u
#endif

#if TILEGPU_SEED_CT32

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

uint tile_b16(uint x, uint y)
{
#if TILEGPU_SEED_FMT == 1
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
#else
	return XB(x, 0u, TILE_SWZ_B84S_X0) ^ XB(x, 1u, TILE_SWZ_B84S_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84S_Y0) ^ XB(y, 1u, TILE_SWZ_B84S_Y1) ^ XB(y, 2u, TILE_SWZ_B84S_Y2);
#endif
}

uint tile_c16(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C16_X0) ^ XB(x, 1u, TILE_SWZ_C16_X1) ^ XB(x, 2u, TILE_SWZ_C16_X2) ^ XB(x, 3u, TILE_SWZ_C16_X3)
	     ^ XB(y, 0u, TILE_SWZ_C16_Y0) ^ XB(y, 1u, TILE_SWZ_C16_Y1) ^ XB(y, 2u, TILE_SWZ_C16_Y2);
}

// Halfword (sel & 1) of an SSBO-loaded word, in the two forms tilegpu.glsl's byte extract carries
// and for the same reason: Honeykrisp miscompiles a sub-word shift whose amount is computed, taking
// the selector from the word INDEX instead. Here the selector is bit 3 of x, so the miscompile would
// blank every other 8-texel column of every seeded 16-bit page. Every other driver keeps the
// straight-line shift; both arms produce the identical value on every input.
uint tilegpu_half_sel(uint word, uint sel)
{
#if TILEGPU_STATIC_BYTE_SEL
	return ((sel & 1u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
#else
	return (word >> ((sel & 1u) * 16u)) & 0xFFFFu;
#endif
}

#endif

void main()
{
	const uint x = uint(gl_FragCoord.x);
	const uint y = uint(gl_FragCoord.y);

	// The guest page this pixel lives in under the surface layout (page-aligned base: pool page
	// (row, col) is physical page base + row*bw + col, wrapping at the end of memory). A 32-bit
	// page is 32 rows tall and a 16-bit one 64.
#if TILEGPU_SEED_CT32
	const uint rel = (y >> 5u) * bw + (x >> 6u);
#else
	const uint rel = (y >> 6u) * bw + (x >> 6u);
#endif
	const uint page = ((bp >> 5u) + rel) & 511u;
	if ((vram_words[mask_base + (page >> 5u)] & (1u << (page & 31u))) == 0u)
		discard;

	const uint slot = vram_words[table_base + epoch * 512u + page];

#if TILEGPU_SEED_CT32
	// The exact ring word tilegpu_writeback.glsl wrote (or the executor prefilled) for this texel.
	const uint bib = tile_b48((x >> 3u) & 7u, (y >> 3u) & 3u) ^ TILEGPU_SEED_ZXOR;
	const uint w = vram_words[slot + bib * 64u + tile_c32(x & 7u, y & 7u)];

	o_color = vec4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
	               float((w >> 24u) & 0xFFu)) * (1.0f / 255.0f);
#else
	const uint bib = tile_b16((x >> 4u) & 3u, (y >> 3u) & 7u);
	const uint hw = tile_c16(x & 15u, y & 7u);
	const uint c = tilegpu_half_sel(vram_words[slot + bib * 64u + (hw >> 1u)], hw);

	// A1B5G5R5 -> RGBA8888, the writeback's pack run backwards.
	o_color = vec4(float((c & 0x001Fu) << 3u), float((c & 0x03E0u) >> 2u), float((c & 0x7C00u) >> 7u),
	               ((c & 0x8000u) != 0u) ? 128.0f : 0.0f) * (1.0f / 255.0f);
#endif
}

#endif
