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
// TILEGPU_SRC_FMT (injected by the device) picks the address geometry and the alpha rule. Each
// value compiles its OWN path and nothing else -- these are #if, not runtime branches, because a
// build is one pipeline per format and a format's arithmetic has no business in another's program:
//   0 = PSMCT32 -- the guest word IS the texel, its top byte included.
//   1 = PSMCT24 -- the top byte of the word belongs to whatever else shares those bytes, so TEXA
//       supplies alpha and AEM makes an all-zero-RGB texel transparent. Baked in HERE, at build
//       time, which is what the CPU deswizzlers do and what the source cache's AuxKey keys on:
//       two draws differing only in TEXA are two entries, so one image never has to serve both.
//   2 = PSMT8, 3 = PSMT4 -- the window is a field of palette INDICES, and that is what this writes:
//       the index replicated into all four channels of the RGBA8 target, exactly the convention
//       ps_tile_reinterpret_index already writes and ps_tile_expand_palette already reads (it takes
//       .a). The palette is NOT applied here. It is a second, independent identity -- the source
//       cache's AuxKey is documented zero for a paletted window precisely so that one index build
//       serves every palette a game cycles through it -- so the colour arrives in a second pass,
//       ps_tile_expand_palette, keyed on (index build id x palette content id). TEXA belongs to
//       the CLUT expansion for these formats (GSClut::Read32 applies it), so `texa` is zero here
//       and the alpha rule above is not compiled in.
//   4 = PSMT8H, 5 = PSMT4HL, 6 = PSMT4HH -- the alpha-byte views. Also fields of palette indices,
//       and written the same way, but their ADDRESS geometry is PSMCT32's and not PSMT8's: 64x32
//       pages, 8x8 blocks, one word per texel, TBW pages per row. The index is the top byte of
//       that word, or a nibble of it. ⚠️ They are paletted AND 64 texels to a page, so `bw` here
//       is TBW and not TBW >> 1 -- which is why the renderer takes it from
//       GSTileSwizzleForms::PagesPerRow, off the page width, rather than off psm.pal.
//
// ⚠️ Two rules from tilegpu.glsl's header bind here too, for the same reasons:
//  - TILEGPU_STATIC_BYTE_SEL. Honeykrisp miscompiles the dynamic sub-word extract on a word loaded
//    from this SSBO -- the load returns the right word and the shift amount comes from the wrong
//    place -- so both extracts below have a constant-shift form behind the driverID gate. The
//    direct-colour formats select their bytes at constant offsets and fold either way; the PALETTED
//    formats are the ones that actually need it, which is why the define was injected at C3 before
//    anything used it.
//  - Any float mul-add on the texel path is an explicit fma() into a `precise` variable. There is
//    still none here: every format's arithmetic is integer swizzle plus one normalising multiply by
//    1/255, whose UNORM8 store round-trips to the byte it came from exactly. This image's texels are
//    compared byte-for-byte against the byte road's, so a mul-add appearing here later must be
//    written the pinned way.

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

// THE RING'S GEOMETRY, spelled once here and once in C++. Every constant below is a fact this
// shader shares with GSDeviceVK over memory the host stages and this shader indexes as raw words.
// A disagreement is invisible at runtime -- every index stays in range, every value stays plausible,
// and the frame is simply wrong -- so the device parses these out of the shader TEXT when it builds
// its TileGpu pipelines and refuses to build on a mismatch, the same thing it already does for
// StateRow (GSDeviceVK::CheckTileGpuShaderContracts).
#define TILEGPU_PAGES 512u          // GS_MAX_PAGES: the epoch page table's row stride
#define TILEGPU_PAGE_WORDS 2048u    // GS_PAGE_SIZE / 4: one page, and so one ring slot, in words
#define TILEGPU_PAGE_WORD_SHIFT 11u // log2(TILEGPU_PAGE_WORDS): guest word address -> page index
#define TILEGPU_BLOCK_WORDS 64u     // GS_BLOCK_SIZE / 4: one block, the ring's inner stride

#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

// The CT32 address geometry: pages 64x32 texels, blocks 8x8, columns 8x8, one word per texel. Four
// of the seven formats use it -- the two direct-colour ones and the three alpha-byte views -- so it
// gets its own gate rather than a range test, which is what let PSMT8 quietly share the 32-bit
// BLOCK form while needing its own column form.
#define TILEGPU_SRC_CT32_ADDR (TILEGPU_SRC_FMT <= 1 || TILEGPU_SRC_FMT >= 4)

// The swizzle forms, one set per address geometry -- fitted at runtime from GSTables.cpp and
// injected as TILE_SWZ_* defines, so this shader and the CPU readers cannot disagree about a
// constant. Only the forms this build's format uses are compiled: the 32-bit block form serves
// everything but PSMT4, the 4-bit block form serves PSMT4, and the column form is per geometry.
#if TILEGPU_SRC_FMT != 3
uint tile_b48(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B48_X0) ^ XB(x, 1u, TILE_SWZ_B48_X1) ^ XB(x, 2u, TILE_SWZ_B48_X2)
	     ^ XB(y, 0u, TILE_SWZ_B48_Y0) ^ XB(y, 1u, TILE_SWZ_B48_Y1);
}
#endif

#if TILEGPU_SRC_CT32_ADDR
uint tile_c32(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C32_X0) ^ XB(x, 1u, TILE_SWZ_C32_X1) ^ XB(x, 2u, TILE_SWZ_C32_X2)
	     ^ XB(y, 0u, TILE_SWZ_C32_Y0) ^ XB(y, 1u, TILE_SWZ_C32_Y1) ^ XB(y, 2u, TILE_SWZ_C32_Y2);
}
#endif

#if TILEGPU_SRC_FMT == 2
uint tile_c8(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C8_X0) ^ XB(x, 1u, TILE_SWZ_C8_X1) ^ XB(x, 2u, TILE_SWZ_C8_X2) ^ XB(x, 3u, TILE_SWZ_C8_X3)
	     ^ XB(y, 0u, TILE_SWZ_C8_Y0) ^ XB(y, 1u, TILE_SWZ_C8_Y1) ^ XB(y, 2u, TILE_SWZ_C8_Y2) ^ XB(y, 3u, TILE_SWZ_C8_Y3);
}
#endif

#if TILEGPU_SRC_FMT == 3
uint tile_b84(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
}

uint tile_c4(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C4_X0) ^ XB(x, 1u, TILE_SWZ_C4_X1) ^ XB(x, 2u, TILE_SWZ_C4_X2) ^ XB(x, 3u, TILE_SWZ_C4_X3) ^ XB(x, 4u, TILE_SWZ_C4_X4)
	     ^ XB(y, 0u, TILE_SWZ_C4_Y0) ^ XB(y, 1u, TILE_SWZ_C4_Y1) ^ XB(y, 2u, TILE_SWZ_C4_Y2) ^ XB(y, 3u, TILE_SWZ_C4_Y3);
}
#endif

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

#if TILEGPU_SRC_FMT == 3
// Nibble (sel & 1) of a byte, under the same gate and for the same reason -- a sub-word extract
// whose shift amount is computed is the shape Honeykrisp gets wrong. Both arms are pure integer
// arithmetic and produce the identical value on every input; the gate only chooses whether the
// shift amount is a constant. The static form is the one tilegpu_index4 uses on the byte road.
uint tilegpu_nibble_sel(uint byteval, uint sel)
{
#if TILEGPU_STATIC_BYTE_SEL
	return ((sel & 1u) != 0u) ? (byteval >> 4u) : (byteval & 0xFu);
#else
	return (byteval >> ((sel & 1u) * 4u)) & 0xFu;
#endif
}
#endif

// The ring indirection, the same one the byte road takes: table[table_base + epoch*512 + page] is
// the word offset of that page's 8 KB slot in this same buffer.
uint mat_ring_word(uint gs_word)
{
	const uint slot = vram_words[table_base + epoch * TILEGPU_PAGES + (gs_word >> TILEGPU_PAGE_WORD_SHIFT)];
	return vram_words[slot + (gs_word & (TILEGPU_PAGE_WORDS - 1u))];
}

void main()
{
	const uint u = uint(gl_FragCoord.x);
	const uint v = uint(gl_FragCoord.y);

#if TILEGPU_SRC_CT32_ADDR
	// The CT32/CT24 address, identical to tilegpu_texel32's: a page is 64x32 texels of 8x8 blocks
	// and 8x8-texel columns, block b occupies words [b*64, b*64+64), so the absolute block times 64
	// plus the word-in-block is the linear guest word. The three alpha-byte views read the same
	// word; they differ only in which of its bits are the index.
	const uint page = (v >> 5u) * bw + (u >> 6u);
	const uint blk = (bp + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u)) & 16383u;
	const uint w = mat_ring_word(blk * TILEGPU_BLOCK_WORDS + tile_c32(u & 7u, v & 7u));
#endif

#if TILEGPU_SRC_FMT <= 1
	vec4 t = vec4(float(tilegpu_byte_sel(w, 0u)), float(tilegpu_byte_sel(w, 1u)),
		float(tilegpu_byte_sel(w, 2u)), float(tilegpu_byte_sel(w, 3u))) * (1.0f / 255.0f);
#elif TILEGPU_SRC_FMT == 2
	// PSMT8, identical to tilegpu_index8's: a page is 128x64 texels of 16x16 blocks and 16x16-texel
	// columns, so a block is 256 bytes = 64 words and the column form gives the byte within it.
	const uint page = (v >> 6u) * bw + (u >> 7u);
	const uint blk = (bp + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u)) & 16383u;
	const uint byte_in_block = tile_c8(u & 15u, v & 15u);
	const uint idx = tilegpu_byte_sel(mat_ring_word(blk * TILEGPU_BLOCK_WORDS + (byte_in_block >> 2u)), byte_in_block);
	// The index, replicated -- ps_tile_expand_palette reads .a, and an R8 view of a one-byte source
	// would have replicated it anyway, so this target holds what that pass already expects.
	vec4 t = vec4(float(idx)) * (1.0f / 255.0f);
#elif TILEGPU_SRC_FMT == 3
	// PSMT4, identical to tilegpu_index4's: a page is 128x128 texels of 32x16 blocks; the column
	// form gives the NIBBLE within the block's 512, its low bit choosing the half of its byte.
	const uint page = (v >> 7u) * bw + (u >> 7u);
	const uint blk = (bp + page * 32u + tile_b84((u >> 5u) & 3u, (v >> 4u) & 7u)) & 16383u;
	const uint nib = tile_c4(u & 31u, v & 15u);
	const uint byte_in_block = nib >> 1u;
	const uint byteval = tilegpu_byte_sel(mat_ring_word(blk * TILEGPU_BLOCK_WORDS + (byte_in_block >> 2u)), byte_in_block);
	vec4 t = vec4(float(tilegpu_nibble_sel(byteval, nib))) * (1.0f / 255.0f);
#else
	// The alpha-byte views, identical to tilegpu_index_hi's: the index is the top byte of the CT32
	// word read above, or a nibble of that byte. Constant offsets throughout -- a computed shift
	// amount on a word out of this SSBO is what Honeykrisp gets wrong.
	const uint hb = bitfieldExtract(w, 24, 8);
#if TILEGPU_SRC_FMT == 4 // PSMT8H
	const uint idx = hb;
#elif TILEGPU_SRC_FMT == 5 // PSMT4HL
	const uint idx = hb & 0xFu;
#else // PSMT4HH
	const uint idx = hb >> 4u;
#endif
	// Replicated, exactly as the PSMT8/PSMT4 arms do it -- ps_tile_expand_palette reads .a.
	vec4 t = vec4(float(idx)) * (1.0f / 255.0f);
#endif

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
