// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu executor's wrong-fast geometry + texture shader. The vertex stage transforms a raw
// GSVertex into clip space using the per-draw screen->NDC transform (the HW tfx VertexScale/
// VertexOffset) and forwards the vertex colour and the two texture coordinates (UV and ST/Q). The
// fragment stage, when the draw is textured, samples the guest texture straight out of a snapshot
// of GS local memory held in a storage buffer -- addressing it with the GS's own page/block/column
// swizzle (the same GF(2)-linear forms tile_convert.glsl uses, injected as TILE_SWZ_* defines) --
// and applies the texture function (MODULATE/DECAL). No blending or fog yet; those arrive with the
// rest of the fixed-function state.
//
// Per-draw state does NOT ride in push constants: the executor submits with
// vkCmdDrawIndexedIndirect, so there is no per-draw command to push against. Every draw's state row
// lives in a storage buffer (set 0 binding 0), and the indirect draw's first_instance selects it --
// first_instance arrives as gl_InstanceIndex (instanceCount is 1, so gl_InstanceID is 0). A push
// constant base_row rebases into this frame's slice of the state ring; the vertex stage resolves the
// row and forwards its index (v_row, flat) so the fragment stage reads the same row without needing
// the instance index, which is a vertex-stage input only.
//
// TILEGPU_TEX (injected by the device): 1 when the page-swizzle forms fitted closed forms and the
// VRAM sampling path is compiled in, 0 when they did not (the whole draw then falls back to the
// vertex-colour path whatever a state row's tex_enable says).

#ifndef TILEGPU_TEX
#define TILEGPU_TEX 0
#endif

// Matches the executor's StateRow byte-for-byte (std430, 80 bytes). The transform is read in the
// vertex stage; the texture fields in the fragment stage. z_write/z_test are pipeline state, carried
// for layout parity, not consumed by either.
struct StateRow
{
	vec2 vertex_scale;
	vec2 vertex_offset;
	uint z_write;
	uint z_test;
	uint tex_enable; // 1 = sample the VRAM texture, 0 = vertex-colour only
	uint fst;        // 1 = FST/UV coords, 0 = STQ coords
	uint tbp0;       // TEX0.TBP0, texture base in blocks
	uint tbw;        // texture buffer width in pages (max(TBW, 1))
	uint tw;         // texture width  in texels (1 << TW)
	uint th;         // texture height in texels (1 << TH)
	uint tfx;        // TEX0.TFX texture function
	uint tcc;        // TEX0.TCC: 1 = texture carries alpha, 0 = alpha from vertex
	uint wms;        // CLAMP.WMS horizontal wrap mode
	uint wmt;        // CLAMP.WMT vertical wrap mode
	uint index_format; // 0 = direct 32-bit texel, 1 = PSMT8 index, 2 = PSMT4 index
	uint pal_offset;   // word offset of this draw's palette in the frame palette stream
	uint pad0_;
	uint pad1_;
};

layout(std430, set = 0, binding = 0) readonly buffer StateTable
{
	StateRow state_rows[];
};

layout(push_constant) uniform cb
{
	uint base_row;  // this frame's first state row in the ring buffer (vertex stage)
	uint vram_base; // this frame's first VRAM word in the ring buffer (fragment stage)
	uint pal_base;  // this frame's first palette word in the ring buffer (fragment stage)
};

#ifdef VERTEX_SHADER

layout(location = 0) in uvec2 a_xy;   // raw 12.4 fixed-point screen XY
layout(location = 1) in uint a_z;     // raw integer depth (24- or 32-bit)
layout(location = 2) in vec4 a_color; // RGBA, normalised
layout(location = 3) in vec2 a_st;    // ST texture coords (float)
layout(location = 4) in float a_q;    // Q (the STQ divisor)
layout(location = 5) in uvec2 a_uv;   // UV texture coords, 12.4 fixed

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st;      // ST forwarded; the fragment stage divides by Q
layout(location = 2) out float v_q;      // interpolated affinely (gl_Position.w == 1) -> PS2 per-pixel ST/Q
layout(location = 3) out vec2 v_uv;      // UV in texels (12.4 -> texel), for the FST path
layout(location = 4) flat out uint v_row; // this vertex's state row, so the fragment stage reads it

void main()
{
	uint row = base_row + uint(gl_InstanceIndex);
	StateRow sr = state_rows[row];

	vec2 p = vec2(a_xy);
	gl_Position = vec4(p, float(a_z), 1.0f) - vec4(0.05f, 0.05f, 0.0f, 0.0f);
	gl_Position.xy = gl_Position.xy * vec2(sr.vertex_scale.x, -sr.vertex_scale.y) - vec2(sr.vertex_offset.x, -sr.vertex_offset.y);
	gl_Position.z *= exp2(-32.0f); // integer depth -> float, monotonic; D32_SFLOAT keeps precision
	gl_Position.y = -gl_Position.y;
	if (gl_Position.z == gl_Position.w)
		gl_Position.z *= 0.999999f;

	v_color = a_color;
	v_st = a_st;
	v_q = a_q;
	v_uv = vec2(a_uv) * (1.0f / 16.0f); // 12.4 fixed -> texel
	v_row = row;
	// Point topology reads gl_PointSize; a GS point covers one pixel. Ignored for line/triangle.
	gl_PointSize = 1.0f;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_st;
layout(location = 2) in float v_q;
layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_row;

layout(location = 0) out vec4 o_color;

#if TILEGPU_TEX

// The GS VRAM snapshot the fragment stage samples: guest local memory as a flat array of 32-bit
// words, uploaded once per frame. vram_base rebases into this frame's slice of the ring.
layout(std430, set = 0, binding = 1) readonly buffer Vram
{
	uint vram_words[];
};

// The two swizzle forms a 32-bit (CT32/CT24) texture needs, copied from tile_convert.glsl: the
// block-in-page form and the word-in-block column form. Both are GF(2)-linear maps of the coordinate
// bits fitted at runtime from GSTables.cpp (GSTileSwizzleForms) and injected as TILE_SWZ_* defines,
// so this shader and the CPU readers cannot disagree about a constant.
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

// The CT32/CT24 texel at integer coords (u, v) of the texture at tbp0/tbw, as a raw RGBA8888 word.
// A CT32 page is 64x32 texels, 8x8 blocks, 8x8-texel columns; block b of guest memory occupies words
// [b*64, b*64+64), so the absolute block times 64 plus the word-in-block is the linear word address.
uint tilegpu_texel32(uint u, uint v, uint tbp0, uint tbw)
{
	uint page = (v >> 5u) * tbw + (u >> 6u);
	uint blk = tbp0 + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u);
	uint word_in_block = tile_c32(u & 7u, v & 7u);
	return vram_words[vram_base + blk * 64u + word_in_block];
}

// The extra swizzle forms the paletted index reads need, copied from tfx.glsl: the 4-bit block
// form and the 8-/4-bit column forms (the 32-bit block form tile_b48 is shared with CT32).
uint tile_b84(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
}

uint tile_c8(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C8_X0) ^ XB(x, 1u, TILE_SWZ_C8_X1) ^ XB(x, 2u, TILE_SWZ_C8_X2) ^ XB(x, 3u, TILE_SWZ_C8_X3)
	     ^ XB(y, 0u, TILE_SWZ_C8_Y0) ^ XB(y, 1u, TILE_SWZ_C8_Y1) ^ XB(y, 2u, TILE_SWZ_C8_Y2) ^ XB(y, 3u, TILE_SWZ_C8_Y3);
}

uint tile_c4(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C4_X0) ^ XB(x, 1u, TILE_SWZ_C4_X1) ^ XB(x, 2u, TILE_SWZ_C4_X2) ^ XB(x, 3u, TILE_SWZ_C4_X3) ^ XB(x, 4u, TILE_SWZ_C4_X4)
	     ^ XB(y, 0u, TILE_SWZ_C4_Y0) ^ XB(y, 1u, TILE_SWZ_C4_Y1) ^ XB(y, 2u, TILE_SWZ_C4_Y2) ^ XB(y, 3u, TILE_SWZ_C4_Y3);
}

// The PSMT8 index byte at texel (u, v): 128x64 page, 16x16-texel columns; block b occupies bytes
// [b*256, b*256+256) = words [b*64, b*64+64). byte_in_block picks the byte within its word.
uint tilegpu_index8(uint u, uint v, uint tbp0, uint tbw)
{
	uint page = (v >> 6u) * tbw + (u >> 7u);
	uint blk = tbp0 + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u);
	uint byte_in_block = tile_c8(u & 15u, v & 15u);
	uint word = vram_words[vram_base + blk * 64u + (byte_in_block >> 2u)];
	return (word >> ((byte_in_block & 3u) * 8u)) & 0xFFu;
}

// The PSMT4 index nibble at texel (u, v): 128x128 page, 32x16-texel columns. col4 gives the
// nibble index within the 512-nibble (256-byte) block; the low bit selects the nibble in its byte.
uint tilegpu_index4(uint u, uint v, uint tbp0, uint tbw)
{
	uint page = (v >> 7u) * tbw + (u >> 7u);
	uint blk = tbp0 + page * 32u + tile_b84((u >> 5u) & 3u, (v >> 4u) & 7u);
	uint nib = tile_c4(u & 31u, v & 15u);
	uint byte_in_block = nib >> 1u;
	uint word = vram_words[vram_base + blk * 64u + (byte_in_block >> 2u)];
	uint byteval = (word >> ((byte_in_block & 3u) * 8u)) & 0xFFu;
	return ((nib & 1u) != 0u) ? (byteval >> 4u) : (byteval & 0xFu);
}

// Unpack a raw RGBA8888 word (a CT32 texel or an expanded CLUT entry) to normalised RGBA.
vec4 tilegpu_unpack(uint w)
{
	return vec4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
	            float((w >> 24u) & 0xFFu)) * (1.0f / 255.0f);
}

// Apply the wrap mode to one axis. REPEAT masks (dims are powers of two); everything else clamps --
// REGION_CLAMP/REGION_REPEAT are approximated by CLAMP until the region bounds are carried (wrong-fast).
int tilegpu_wrap(int c, uint dim, uint mode)
{
	if (mode == 0u) // REPEAT
		return int(uint(c) & (dim - 1u));
	return clamp(c, 0, int(dim) - 1); // CLAMP and the region modes
}

#endif // TILEGPU_TEX

void main()
{
	StateRow sr = state_rows[v_row];

#if TILEGPU_TEX
	if (sr.tex_enable != 0u)
	{
		// The texel coordinate: FST is a direct texel (12.4 already unpacked in the VS); STQ divides
		// the interpolated S,T by the interpolated Q and scales by the texture dimensions. The affine
		// interpolation (gl_Position.w == 1) plus this per-pixel divide reproduces the PS2's own
		// affine-rasteriser-with-per-pixel-divide texturing.
		vec2 uv = (sr.fst != 0u) ? v_uv : (vec2(v_st.x, v_st.y) / v_q) * vec2(float(sr.tw), float(sr.th));
		int iu = tilegpu_wrap(int(floor(uv.x)), sr.tw, sr.wms);
		int iv = tilegpu_wrap(int(floor(uv.y)), sr.th, sr.wmt);

		// The texel: direct 32-bit words, or a paletted index (PSMT8/PSMT4) looked up in the frame's
		// expanded CLUT stream. index_format is dynamically uniform per draw, so this does not diverge.
		uint w;
		if (sr.index_format == 0u)
			w = tilegpu_texel32(uint(iu), uint(iv), sr.tbp0, sr.tbw);
		else if (sr.index_format == 1u)
			w = vram_words[pal_base + sr.pal_offset + tilegpu_index8(uint(iu), uint(iv), sr.tbp0, sr.tbw)];
		else
			w = vram_words[pal_base + sr.pal_offset + tilegpu_index4(uint(iu), uint(iv), sr.tbp0, sr.tbw)];
		vec4 ct = tilegpu_unpack(w);

		// Cf is the vertex colour; its 128 == 1.0 modulation reference is the *2 (255/128, rounded).
		vec3 cf = v_color.rgb;
		float af = v_color.a;
		vec3 rgb;
		float a;
		if (sr.tfx == 1u) // DECAL: the texel replaces the fragment
		{
			rgb = ct.rgb;
			a = (sr.tcc != 0u) ? ct.a : af * (255.0f / 128.0f);
		}
		else // MODULATE (and HIGHLIGHT/HIGHLIGHT2 approximated as MODULATE for now)
		{
			rgb = min(ct.rgb * cf * (255.0f / 128.0f), vec3(1.0f));
			a = (sr.tcc != 0u) ? min(ct.a * af * (255.0f / 128.0f), 1.0f) : min(af * (255.0f / 128.0f), 1.0f);
		}
		o_color = vec4(rgb, a);
		return;
	}
#endif

	// Untextured: PS2 vertex colour is 8-bit with 128 as the 1.0 reference; scale into display range
	// and force opaque (no blending yet, so alpha would not be consumed).
	o_color = vec4(v_color.rgb * 2.0f, 1.0f);
}

#endif
