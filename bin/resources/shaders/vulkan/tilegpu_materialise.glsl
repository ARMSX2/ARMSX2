// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu materialise: bytes -> an ordinary image. Rule 3 of the VRAM model's texel road samples a
// texture window through the hardware sampler instead of decoding four swizzled loads per bilinear
// tap, and this pass is what puts the window into an image for it to sample. It is the flat road's
// tap lifted out of the geometry shader and run ONCE PER TEXEL instead of once per fragment: the
// same TILE_SWZ_* forms, the same epoch page table, the same byte extraction. So a materialised
// texel is the byte road's texel, and swapping a draw from one road to the other moves no value.
//
// A full-viewport triangle over the destination, one fragment per texel, no attributes and no
// vertex buffer. The destination is an RGBA8 render target the size of the window (tw x th), so
// fragment (x, y) IS texel (u, v) and the whole address is a function of gl_FragCoord.
//
// The bytes come from the frame's ring exactly as the draw would have read them: table[table_base +
// epoch * 512 + page] is the word offset of the page's 8 KB slot, and the slot holds whatever the
// renderer composed for that epoch -- the CPU shadow's bytes, a target's writeback, or both. The
// renderer emits this op AFTER the composition ops for the same window, so at the pass head the
// slot is finished before this reads it.
//
// TILEGPU_SRC_FMT (injected by the device) picks the address geometry and the alpha rule:
//   0 = PSMCT32 -- the guest word IS the texel, its top byte included.
//   1 = PSMCT24 -- the top byte of the word belongs to whatever else shares those bytes, so TEXA
//       supplies alpha and AEM makes an all-zero-RGB texel transparent. Baked in HERE, at build
//       time, which is what the CPU deswizzlers do and what the source cache's AuxKey keys on:
//       two draws differing only in TEXA are two entries, so one image never has to serve both.
// The paletted geometries (index output for PSMT8 / PSMT4) arrive with the paletted road.
//
// ⚠️ Two rules from tilegpu.glsl's header bind here too, for the same reasons:
//  - TILEGPU_STATIC_BYTE_SEL. Honeykrisp miscompiles the dynamic byte-extract shift on a word
//    loaded from this SSBO, and the byte extraction below is the identical expression. Today's two
//    formats select their bytes at constant offsets so both forms fold to the same code; the define
//    is injected now so the paletted arm cannot land without it.
//  - Any float mul-add on the texel path is an explicit fma() into a `precise` variable. Nothing
//    here is one yet (the arithmetic is integer swizzle plus a single normalising multiply), but a
//    format that needs one must write it that way -- this image's texels are compared byte-for-byte
//    against the byte road's.

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

// The frame's ring: page slots and the epoch page tables, one word array. Same buffer, same
// binding and same pipeline layout as the geometry pass and the seed -- this costs a pipeline bind
// and a push, no descriptor traffic.
layout(std430, set = 0, binding = 1) readonly buffer Vram
{
	uint vram_words[];
};

layout(push_constant) uniform cb
{
	uint table_base; // this frame's first page-table word in the ring
	uint epoch;      // which epoch's table names the source slots
	uint bp;         // TEX0.TBP0, the window's base in blocks
	uint bw;         // pages per texture row (the state row's convention)
	uint texa;       // 24-bit texel alpha: bit 0 = apply, bit 1 = TEXA.AEM, bits 8-15 = TEXA.TA0
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

// Byte (sel & 3) of an SSBO-loaded word, in the two forms tilegpu.glsl carries and for the reason
// its comment gives: the dynamic shift is miscompiled on Honeykrisp.
uint tilegpu_byte_sel(uint word, uint sel)
{
#if TILEGPU_STATIC_BYTE_SEL
	sel &= 3u;
	if (sel == 0u)
		return word & 0xFFu;
	if (sel == 1u)
		return (word >> 8u) & 0xFFu;
	if (sel == 2u)
		return (word >> 16u) & 0xFFu;
	return (word >> 24u) & 0xFFu;
#else
	return (word >> ((sel & 3u) * 8u)) & 0xFFu;
#endif
}

void main()
{
	const uint u = uint(gl_FragCoord.x);
	const uint v = uint(gl_FragCoord.y);

	// The CT32/CT24 address, identical to tilegpu_texel32's: a page is 64x32 texels of 8x8 blocks
	// and 8x8-texel columns, block b occupies words [b*64, b*64+64), so the absolute block times 64
	// plus the word-in-block is the linear guest word.
	const uint page = (v >> 5u) * bw + (u >> 6u);
	const uint blk = bp + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u);
	const uint gs_word = blk * 64u + tile_c32(u & 7u, v & 7u);

	// ...and the same ring indirection the byte road takes for it.
	const uint slot = vram_words[table_base + epoch * 512u + (gs_word >> 11u)];
	const uint w = vram_words[slot + (gs_word & 2047u)];

	vec4 t = vec4(float(tilegpu_byte_sel(w, 0u)), float(tilegpu_byte_sel(w, 1u)),
		float(tilegpu_byte_sel(w, 2u)), float(tilegpu_byte_sel(w, 3u))) * (1.0f / 255.0f);

#if TILEGPU_SRC_FMT == 1
	// PSMCT24: the alpha byte above is not this texture's, so TEXA replaces it. Written exactly as
	// tilegpu_texa does it on the byte road, including the AEM word compare, because a draw moved
	// from that road to this image must land on the same alpha.
	// The apply bit is the byte road's, not a formality: a caller that leaves it clear gets the raw
	// byte through, which is what tilegpu_texa does with it. (This renderer always sets it for a
	// PSMCT24 draw, so the fall-through is unreachable today and kept only because divergence from
	// the byte road here would be invisible.)
	if ((texa & 1u) != 0u)
	{
		const bool aem_zero = (texa & 2u) != 0u && (w & 0x00FFFFFFu) == 0u;
		t.a = aem_zero ? 0.0f : float((texa >> 8u) & 0xFFu) * (1.0f / 255.0f);
	}
#endif

	o_color = t;
}

#endif
