// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu CLUT gather: a palette loaded off a render target.
//
// The GS loads its palette from local memory when TEX0 is written -- a CSM1 32-bit load reads 256
// words (four blocks) or 16 words (one block) at CBP, in memory order, and the loader's shuffle
// decides which word becomes which entry. When those blocks were rendered by a native draw the words
// are in the owner surface's texture and nowhere on the CPU, so the load becomes a full GPU drain:
// GT4 pays it seventy times a frame, GT4-OPB twelve hundred. This shader gathers the words into an
// N x 1 palette texture instead -- texel e is entry e as GSClut::Read32 would expand it (a 32-bit
// palette expands to itself), so a draw binds it exactly like a CPU-built one and nothing waits.
//
// ⚠️ The arithmetic below is ps_tile_clut_from_target's (tile_convert.glsl) verbatim. It exists as a
// separate program for the same reason tilegpu_reinterpret.glsl and tilegpu_expand.glsl do: this
// renderer records draws and executes them at VSync, so the gather has to be one more op in the
// stream it will eventually run, recorded with raw commands -- and a device utility path binds
// descriptors through the device's own state cache, which invalidates the set bindings the
// surrounding passes established (measured at C5: 20 draw-time validation errors on one frame).
//
// Entry -> source word is the loaders' own order, fitted as a GF(2)-linear form from
// GSClut::EntryToWordCSM1_32 (TILE_SWZ_CLUT8_*/CLUT4_*); word -> owner texel is the same inverse
// arithmetic the reinterpretation uses. TILE_CLUT_ENTRIES (injected with the pipeline) is 256 for an
// eight-bit palette (CSA 0, all sixteen slots) or 16 for a four-bit one.

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

layout(set = 0, binding = 0) uniform sampler2D samp0; // the owner target's texture

layout(push_constant) uniform cb0
{
	uint src_bp;   // the load's CBP (blocks)
	uint src_bwpg; // unused here -- the layout is shared with the donor build
	uint dst_bp;   // the owner surface's base (page-aligned blocks)
	uint dst_bwpg; // the owner's width in pages
};

// Broadcast bit b of v across mask m: m if the bit is set, 0 if not.
#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

// Inverse of blockTable32: block in page 0..31 -> bx | (by << 3).
uint tile_ib48(uint v)
{
	return XB(v, 0u, TILE_SWZ_IB48_0) ^ XB(v, 1u, TILE_SWZ_IB48_1) ^ XB(v, 2u, TILE_SWZ_IB48_2)
	     ^ XB(v, 3u, TILE_SWZ_IB48_3) ^ XB(v, 4u, TILE_SWZ_IB48_4);
}

// Inverse of columnTable32: word in block 0..63 -> cx | (cy << 3).
uint tile_ic32(uint v)
{
	return XB(v, 0u, TILE_SWZ_IC32_0) ^ XB(v, 1u, TILE_SWZ_IC32_1) ^ XB(v, 2u, TILE_SWZ_IC32_2)
	     ^ XB(v, 3u, TILE_SWZ_IC32_3) ^ XB(v, 4u, TILE_SWZ_IC32_4) ^ XB(v, 5u, TILE_SWZ_IC32_5);
}

uint tile_clut8_word(uint e)
{
	return XB(e, 0u, TILE_SWZ_CLUT8_0) ^ XB(e, 1u, TILE_SWZ_CLUT8_1) ^ XB(e, 2u, TILE_SWZ_CLUT8_2) ^ XB(e, 3u, TILE_SWZ_CLUT8_3)
	     ^ XB(e, 4u, TILE_SWZ_CLUT8_4) ^ XB(e, 5u, TILE_SWZ_CLUT8_5) ^ XB(e, 6u, TILE_SWZ_CLUT8_6) ^ XB(e, 7u, TILE_SWZ_CLUT8_7);
}

uint tile_clut4_word(uint e)
{
	return XB(e, 0u, TILE_SWZ_CLUT4_0) ^ XB(e, 1u, TILE_SWZ_CLUT4_1) ^ XB(e, 2u, TILE_SWZ_CLUT4_2) ^ XB(e, 3u, TILE_SWZ_CLUT4_3);
}

void main()
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
	o_color = texelFetch(samp0, xy, 0);
}

#endif
