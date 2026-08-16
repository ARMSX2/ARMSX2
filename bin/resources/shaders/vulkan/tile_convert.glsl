// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Tile renderer's device-side conversions. Fragment stage only; the vertex
// stage is convert.glsl's, and the pipeline layout is the utility one (one
// combined image sampler at set 0 binding 0, 96 bytes of push constants).
//
// ps_tile_reinterpret_index: a render target read as an indexed texture.
//
// A game that samples a CT32/CT24 render target through PSMT8/PSMT4 (or the
// alpha-byte views PSMT8H/PSMT4HL/PSMT4HH) is reading the target's BYTES as
// indices -- a per-byte lookup table, which is how GT4 tone-maps its frame,
// seventy pages a frame. The bytes are on the GPU, in the owner surface's
// texture, in the CT32 arrangement; this shader writes them out in the index
// window's arrangement, one output texel per index, so the draw samples an
// ordinary index texture and never drains the target to CPU memory to have the
// swizzle readers put it straight back.
//
// The address arithmetic is GS memory's own: the index texel's absolute block
// (base + page term + the format's in-page block form) and its unit within the
// block (the column form) give the byte; the same block relative to the
// page-aligned owner, through the INVERSE forms, gives the owner texel and which
// of its four bytes. The forms are GF(2)-linear maps of the coordinate bits --
// each output bit an XOR of a fixed subset of input bits -- fitted at runtime
// from GSTables.cpp by GSTileSwizzleForms and injected here as the TILE_SWZ_*
// defines, so this shader and the CPU readers cannot disagree about a constant;
// the arithmetic itself is Locate() in GSTileSwizzleForms.cpp, which the unit
// suite pins against GSOffset::pa on every texel of ten windows.
//
// TILE_IDX_FMT (injected with the pipeline): 0 PSMT8, 1 PSMT4, 2 PSMT8H,
// 3 PSMT4HL, 4 PSMT4HH.

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(push_constant) uniform cb10
{
	uint src_bp;   // the index window's TBP0 (blocks)
	uint src_bwpg; // the window's width in pages, exactly as GSOffset computes it (bw >> (pageShiftX - 6))
	uint dst_bp;   // the owner surface's base (page-aligned blocks)
	uint dst_bwpg; // the owner's width in pages
};

// Broadcast bit b of v across mask m: m if the bit is set, 0 if not.
#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

// blockTable32 == blockTable8: (bx 0..7, by 0..3) -> block in page 0..31.
uint tile_b48(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B48_X0) ^ XB(x, 1u, TILE_SWZ_B48_X1) ^ XB(x, 2u, TILE_SWZ_B48_X2)
	     ^ XB(y, 0u, TILE_SWZ_B48_Y0) ^ XB(y, 1u, TILE_SWZ_B48_Y1);
}

// blockTable4: (bx 0..3, by 0..7) -> block in page 0..31.
uint tile_b84(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
}

// columnTable32: (x 0..7, y 0..7) -> word in block 0..63.
uint tile_c32(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C32_X0) ^ XB(x, 1u, TILE_SWZ_C32_X1) ^ XB(x, 2u, TILE_SWZ_C32_X2)
	     ^ XB(y, 0u, TILE_SWZ_C32_Y0) ^ XB(y, 1u, TILE_SWZ_C32_Y1) ^ XB(y, 2u, TILE_SWZ_C32_Y2);
}

// columnTable8: (x 0..15, y 0..15) -> byte in block 0..255.
uint tile_c8(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C8_X0) ^ XB(x, 1u, TILE_SWZ_C8_X1) ^ XB(x, 2u, TILE_SWZ_C8_X2) ^ XB(x, 3u, TILE_SWZ_C8_X3)
	     ^ XB(y, 0u, TILE_SWZ_C8_Y0) ^ XB(y, 1u, TILE_SWZ_C8_Y1) ^ XB(y, 2u, TILE_SWZ_C8_Y2) ^ XB(y, 3u, TILE_SWZ_C8_Y3);
}

// columnTable4: (x 0..31, y 0..15) -> nibble in block 0..511.
uint tile_c4(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C4_X0) ^ XB(x, 1u, TILE_SWZ_C4_X1) ^ XB(x, 2u, TILE_SWZ_C4_X2) ^ XB(x, 3u, TILE_SWZ_C4_X3) ^ XB(x, 4u, TILE_SWZ_C4_X4)
	     ^ XB(y, 0u, TILE_SWZ_C4_Y0) ^ XB(y, 1u, TILE_SWZ_C4_Y1) ^ XB(y, 2u, TILE_SWZ_C4_Y2) ^ XB(y, 3u, TILE_SWZ_C4_Y3);
}

// Inverse of tile_b48: block in page 0..31 -> bx | (by << 3).
uint tile_ib48(uint v)
{
	return XB(v, 0u, TILE_SWZ_IB48_0) ^ XB(v, 1u, TILE_SWZ_IB48_1) ^ XB(v, 2u, TILE_SWZ_IB48_2)
	     ^ XB(v, 3u, TILE_SWZ_IB48_3) ^ XB(v, 4u, TILE_SWZ_IB48_4);
}

// Inverse of tile_c32: word in block 0..63 -> cx | (cy << 3).
uint tile_ic32(uint v)
{
	return XB(v, 0u, TILE_SWZ_IC32_0) ^ XB(v, 1u, TILE_SWZ_IC32_1) ^ XB(v, 2u, TILE_SWZ_IC32_2)
	     ^ XB(v, 3u, TILE_SWZ_IC32_3) ^ XB(v, 4u, TILE_SWZ_IC32_4) ^ XB(v, 5u, TILE_SWZ_IC32_5);
}

void ps_tile_reinterpret_index()
{
	uvec2 uv = uvec2(gl_FragCoord.xy);
	uint u = uv.x;
	uint v = uv.y;

	uint blk;
	uint byte_in_block;
	uint nibble;
#if TILE_IDX_FMT == 0
	// PSMT8: 128x64 pages, 16x16 blocks, one byte per texel.
	uint page = (v >> 6u) * src_bwpg + (u >> 7u);
	blk = src_bp + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u);
	byte_in_block = tile_c8(u & 15u, v & 15u);
	nibble = 0u;
#elif TILE_IDX_FMT == 1
	// PSMT4: 128x128 pages, 32x16 blocks, one nibble per texel (even = low).
	uint page = (v >> 7u) * src_bwpg + (u >> 7u);
	blk = src_bp + page * 32u + tile_b84((u >> 5u) & 3u, (v >> 4u) & 7u);
	uint nib = tile_c4(u & 31u, v & 15u);
	byte_in_block = nib >> 1u;
	nibble = nib & 1u;
#else
	// The alpha-byte views over the CT32 layout: 64x32 pages, 8x8 blocks, one
	// word per texel, the index in byte 3.
	uint page = (v >> 5u) * src_bwpg + (u >> 6u);
	blk = src_bp + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u);
	byte_in_block = tile_c32(u & 7u, v & 7u) * 4u + 3u;
	nibble = (TILE_IDX_FMT == 4) ? 1u : 0u;
#endif

	// Owner side. The caller proved the whole window lies inside the owner (its
	// pages are the owner's, at whole-page granularity), so blk >= dst_bp; the
	// clamp below only keeps a texelFetch in bounds if that proof were ever wrong.
	uint rel = blk - dst_bp;
	uint pg = rel >> 5u;
	uint bip = rel & 31u;
	uint bpk = tile_ib48(bip);
	uint word = byte_in_block >> 2u;
	uint ch = byte_in_block & 3u;
	uint cpk = tile_ic32(word);
	uint bwpg = max(dst_bwpg, 1u);
	ivec2 xy = ivec2(int((pg % bwpg) * 64u + (bpk & 7u) * 8u + (cpk & 7u)),
	                 int((pg / bwpg) * 32u + (bpk >> 3u) * 8u + (cpk >> 3u)));
	xy = clamp(xy, ivec2(0), textureSize(samp0, 0) - 1);

	// UNORM8 back to the byte, exactly: k/255 * 255 lands within an ulp of k.
	uvec4 bytes = uvec4(texelFetch(samp0, xy, 0) * 255.0f + 0.5f);
	uint b = (ch == 0u) ? bytes.r : (ch == 1u) ? bytes.g : (ch == 2u) ? bytes.b : bytes.a;
#if TILE_IDX_FMT == 0 || TILE_IDX_FMT == 2
	uint idx = b;
#else
	// bitfieldExtract, not shift-and-mask: the latter is shape-dependently
	// miscompiled by honeykrisp (M2/Asahi) on sub-word extracts.
	uint idx = bitfieldExtract(b, int(nibble * 4u), 4);
#endif
	o_col0 = vec4(float(idx) / 255.0f);
}

// ps_tile_clut_from_target: a palette loaded off a render target.
//
// The GS loads its palette from local memory at TEX0-write time — a CSM1 32-bit
// load reads 256 words (four blocks) or 16 words (one block) at CBP, in memory
// order, and the loader's shuffle decides which word becomes which entry. When
// those blocks were rendered by a native draw, the words are in the owner
// surface's texture and nowhere on the CPU; this shader gathers them into an
// N×1 palette texture — texel e is entry e as Read32 would expand it (a 32-bit
// palette expands to itself), so the draw binds it exactly like a CPU-built one.
// Entry → source word is the loaders' own order, fitted as a GF(2)-linear form
// from GSClut::EntryToWordCSM1_32 (TILE_SWZ_CLUT8_*/CLUT4_*); word → owner texel
// is the same inverse arithmetic the reinterpretation above uses.
//
// TILE_CLUT_ENTRIES (injected with the pipeline): 256 for an eight-bit palette
// (CSA 0, all sixteen slots), 16 for a four-bit one (one slot).

uint tile_clut8_word(uint e)
{
	return XB(e, 0u, TILE_SWZ_CLUT8_0) ^ XB(e, 1u, TILE_SWZ_CLUT8_1) ^ XB(e, 2u, TILE_SWZ_CLUT8_2) ^ XB(e, 3u, TILE_SWZ_CLUT8_3)
	     ^ XB(e, 4u, TILE_SWZ_CLUT8_4) ^ XB(e, 5u, TILE_SWZ_CLUT8_5) ^ XB(e, 6u, TILE_SWZ_CLUT8_6) ^ XB(e, 7u, TILE_SWZ_CLUT8_7);
}

uint tile_clut4_word(uint e)
{
	return XB(e, 0u, TILE_SWZ_CLUT4_0) ^ XB(e, 1u, TILE_SWZ_CLUT4_1) ^ XB(e, 2u, TILE_SWZ_CLUT4_2) ^ XB(e, 3u, TILE_SWZ_CLUT4_3);
}

void ps_tile_clut_from_target()
{
	uint e = uint(gl_FragCoord.x);
#if TILE_CLUT_ENTRIES == 256
	uint w = tile_clut8_word(e);
#else
	uint w = tile_clut4_word(e);
#endif
	// src_bp is CBP (blocks); a block is 64 words, contiguous in memory.
	uint blk = src_bp + (w >> 6u);
	uint word_in_block = w & 63u;

	uint rel = blk - dst_bp;
	uint pg = rel >> 5u;
	uint bip = rel & 31u;
	uint bpk = tile_ib48(bip);
	uint cpk = tile_ic32(word_in_block);
	uint bwpg = max(dst_bwpg, 1u);
	ivec2 xy = ivec2(int((pg % bwpg) * 64u + (bpk & 7u) * 8u + (cpk & 7u)),
	                 int((pg / bwpg) * 32u + (bpk >> 3u) * 8u + (cpk >> 3u)));
	xy = clamp(xy, ivec2(0), textureSize(samp0, 0) - 1);
	o_col0 = texelFetch(samp0, xy, 0);
}
