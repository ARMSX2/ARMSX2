// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu executor's wrong-fast geometry + texture shader. The vertex stage transforms a raw
// GSVertex into clip space using the per-draw screen->NDC transform (the HW tfx VertexScale/
// VertexOffset) and forwards the vertex colour and the two texture coordinates (UV and ST/Q). The
// fragment stage, when the draw is textured, samples the guest texture straight out of guest bytes
// held in a storage buffer -- addressing them with the GS's own page/block/column swizzle (the same
// GF(2)-linear forms tile_convert.glsl uses, injected as TILE_SWZ_* defines) -- and applies the
// texture function, then the fog walk. Blending is the executor's fixed-function state, fed by the
// dual-source alpha this stage emits; the alpha test arrives with the rest of the per-fragment tests.
//
// The bytes are not a whole-VRAM snapshot. The frame's ring holds one 8 KB slot per guest page the
// plan actually reads, and a page table per epoch names them: table[table_base + epoch*512 + page]
// is the word offset of that page's slot in this same buffer. A slot is either the CPU shadow's
// bytes for the page (memcpy'd by the executor) or a target's finished pixels reswizzled back by
// tilegpu_writeback.glsl, or both composed -- the renderer's page model decides, and this shader
// only ever asks the table. The state row's epoch selects which version of the frame's bytes the
// draw sees, so an upload landing between two draws that sample the same page gives each its own
// bytes without any barrier between them.
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

// Matches the executor's StateRow byte-for-byte (std430, 112 bytes). The transform and the scissor
// are read in the vertex stage; the texture fields and the tests in the fragment stage. z_write/z_test
// are pipeline state, carried for layout parity, not consumed by either.
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
	uint epoch;        // page-table epoch this draw's byte reads go through
	uint date;         // destination-alpha test: 0 off, 1 = pass where alpha bit 7 clear (DATM 0), 2 = set (DATM 1)
	int ofx, ofy;      // XYOFFSET, 12.4 fixed: the vertex-to-pixel origin the scissor is in
	int sc_x0, sc_y0;  // the GS scissor in target pixels, [x0, x1) x [y0, y1)
	int sc_x1, sc_y1;
	uint fge;          // 1 = PRIM.FGE: walk the fragment's RGB toward the fog colour by the vertex F
	uint fogcol;       // FOGCOL packed 0x00BBGGRR
	uint atst;         // 0 = no alpha test; else TEST.ATST + 1 (2 = LESS ... 8 = NOTEQUAL)
	uint aref;         // TEST.AREF
	uint pad0_, pad1_;
};

layout(std430, set = 0, binding = 0) readonly buffer StateTable
{
	StateRow state_rows[];
};

layout(push_constant) uniform cb
{
	uint base_row;   // this frame's first state row in the ring buffer (vertex stage)
	uint table_base; // this frame's first page-table word in the ring buffer (fragment stage)
	uint pal_base;   // this frame's first palette word in the ring buffer (fragment stage)
};

#ifdef VERTEX_SHADER

layout(location = 0) in uvec2 a_xy;   // raw 12.4 fixed-point screen XY
layout(location = 1) in uint a_z;     // raw integer depth (24- or 32-bit)
layout(location = 2) in vec4 a_color; // RGBA, normalised
layout(location = 3) in vec2 a_st;    // ST texture coords (float)
layout(location = 4) in float a_q;    // Q (the STQ divisor)
layout(location = 5) in uvec2 a_uv;   // UV texture coords, 12.4 fixed
layout(location = 6) in vec4 a_f;     // FOG dword; the fog factor F is its low byte

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st;      // ST forwarded; the fragment stage divides by Q
layout(location = 2) out float v_q;      // interpolated affinely (gl_Position.w == 1) -> PS2 per-pixel ST/Q
layout(location = 3) out vec2 v_uv;      // UV in texels (12.4 -> texel), for the FST path
layout(location = 4) flat out uint v_row; // this vertex's state row, so the fragment stage reads it
layout(location = 5) out float v_fog;    // fog factor F/255, interpolated across the primitive

out float gl_ClipDistance[4];

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
	v_fog = a_f.r; // GSVertex::FOG holds F in its low byte, so the unpacked .r is F/255
	// Point topology reads gl_PointSize; a GS point covers one pixel. Ignored for line/triangle.
	gl_PointSize = 1.0f;

	// The GS scissor as four clip planes in target pixel space: a pixel centre c is inside
	// [x0, x1) exactly when c - x0 > 0 and x1 - c > 0, and the same in y. Per draw from the state
	// row, so one indirect call carries any number of scissors -- SotC draws its full-screen
	// post sprites eight times under eight column scissors.
	vec2 pix = (vec2(a_xy) - vec2(float(sr.ofx), float(sr.ofy))) * (1.0f / 16.0f);
	gl_ClipDistance[0] = pix.x - float(sr.sc_x0);
	gl_ClipDistance[1] = float(sr.sc_x1) - pix.x;
	gl_ClipDistance[2] = pix.y - float(sr.sc_y0);
	gl_ClipDistance[3] = float(sr.sc_y1) - pix.y;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_st;
layout(location = 2) in float v_q;
layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_row;
layout(location = 5) in float v_fog;

// Two fragment outputs: the colour that lands in the target (RAW GS values -- 0x80 stays 0x80;
// the target's bytes are guest bytes, so no display scaling happens here), and the dual-source
// blend factor: the fragment alpha in the GS's 0x80 = 1.0 convention (As * 255/128, clamped),
// which the fixed-function blend takes as SRC1 when the ALPHA register selects As. The alpha
// channel of o_color is written unblended (the GS writes the fragment alpha as-is).
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_blend;

// The pass's snapshot of its own colour target, taken before the pass opened: the destination
// alpha the DATE test reads. Bound per pass (set 1); a pass without DATE draws binds a null
// texture that no fragment reads.
layout(set = 1, binding = 0) uniform sampler2D u_snapshot;

#if TILEGPU_TEX

// The frame's ring: page slots, the epoch page tables and the palettes, all in one storage buffer
// of 32-bit words. Reads go through tilegpu_ring_word below; nothing addresses guest memory flat.
layout(std430, set = 0, binding = 1) readonly buffer Vram
{
	uint vram_words[];
};

// A guest word address (absolute block * 64 + word-in-block) -> the ring word holding it, through
// this frame's page table for `epoch`. 2048 words per 8 KB page; a page the plan never staged for
// this epoch points at the executor's zero slot.
uint tilegpu_ring_word(uint gs_word, uint epoch)
{
	uint page = gs_word >> 11u;
	uint slot = vram_words[table_base + epoch * 512u + page];
	return slot + (gs_word & 2047u);
}

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
uint tilegpu_texel32(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 5u) * tbw + (u >> 6u);
	uint blk = tbp0 + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u);
	uint word_in_block = tile_c32(u & 7u, v & 7u);
	return vram_words[tilegpu_ring_word(blk * 64u + word_in_block, epoch)];
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
uint tilegpu_index8(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 6u) * tbw + (u >> 7u);
	uint blk = tbp0 + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u);
	uint byte_in_block = tile_c8(u & 15u, v & 15u);
	uint word = vram_words[tilegpu_ring_word(blk * 64u + (byte_in_block >> 2u), epoch)];
	return (word >> ((byte_in_block & 3u) * 8u)) & 0xFFu;
}

// The PSMT4 index nibble at texel (u, v): 128x128 page, 32x16-texel columns. col4 gives the
// nibble index within the 512-nibble (256-byte) block; the low bit selects the nibble in its byte.
uint tilegpu_index4(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 7u) * tbw + (u >> 7u);
	uint blk = tbp0 + page * 32u + tile_b84((u >> 5u) & 3u, (v >> 4u) & 7u);
	uint nib = tile_c4(u & 31u, v & 15u);
	uint byte_in_block = nib >> 1u;
	uint word = vram_words[tilegpu_ring_word(blk * 64u + (byte_in_block >> 2u), epoch)];
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

	// Destination alpha test: the GS passes a pixel when the destination alpha's bit 7 equals
	// DATM. The destination is the pass snapshot (pre-pass bytes; the planner keeps that exact).
	if (sr.date != 0u)
	{
		const float da = texelFetch(u_snapshot, ivec2(gl_FragCoord.xy), 0).a;
		const bool msb = da >= (128.0f / 255.0f);
		if (msb != (sr.date == 2u))
			discard;
	}

	// The fragment colour before blending, in raw GS units (0..255 -> 0..1). Cf is the vertex
	// colour as the GS holds it (0x80 = 1.0 for modulation); the texture function combines it with
	// the texel by the GS formulas: MODULATE Cv = Ct*Cf*2 (>>7 in integer), DECAL Cv = Ct,
	// HIGHLIGHT Cv = Ct*Cf*2 + Af, HIGHLIGHT2 likewise; alpha follows TCC (texture carries alpha
	// or the fragment keeps Af).
	vec4 cf = v_color;
	vec4 cv = cf;

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
			w = tilegpu_texel32(uint(iu), uint(iv), sr.tbp0, sr.tbw, sr.epoch);
		else if (sr.index_format == 1u)
			w = vram_words[pal_base + sr.pal_offset + tilegpu_index8(uint(iu), uint(iv), sr.tbp0, sr.tbw, sr.epoch)];
		else
			w = vram_words[pal_base + sr.pal_offset + tilegpu_index4(uint(iu), uint(iv), sr.tbp0, sr.tbw, sr.epoch)];
		vec4 ct = tilegpu_unpack(w);

		const float k = 255.0f / 128.0f;
		if (sr.tfx == 1u) // DECAL
		{
			cv.rgb = ct.rgb;
			cv.a = (sr.tcc != 0u) ? ct.a : cf.a;
		}
		else if (sr.tfx == 0u) // MODULATE
		{
			cv.rgb = min(ct.rgb * cf.rgb * k, vec3(1.0f));
			cv.a = (sr.tcc != 0u) ? min(ct.a * cf.a * k, 1.0f) : cf.a;
		}
		else // HIGHLIGHT (2) / HIGHLIGHT2 (3)
		{
			cv.rgb = min(ct.rgb * cf.rgb * k + vec3(cf.a), vec3(1.0f));
			cv.a = (sr.tcc != 0u) ? ((sr.tfx == 2u) ? min(ct.a + cf.a, 1.0f) : ct.a) : cf.a;
		}
	}
#endif

	// The alpha test. The GS compares the fragment's alpha byte -- after the texture function, so
	// this must sit below it -- against AREF, and AFAIL says what a failing fragment still writes.
	// Here a failure always discards, which is exact for AFAIL=KEEP and an approximation for the
	// three modes that keep writing something (the renderer only asks for a test when the mode
	// changes what lands, and folds the ATST=NEVER cases into the write flags instead).
	if (sr.atst != 0u)
	{
		const uint a = uint(cv.a * 255.0f);
		bool pass;
		switch (sr.atst - 1u)
		{
			case 2u:  pass = a <  sr.aref; break; // LESS
			case 3u:  pass = a <= sr.aref; break; // LEQUAL
			case 4u:  pass = a == sr.aref; break; // EQUAL
			case 5u:  pass = a >= sr.aref; break; // GEQUAL
			case 6u:  pass = a >  sr.aref; break; // GREATER
			case 7u:  pass = a != sr.aref; break; // NOTEQUAL
			default:  pass = true; break;         // NEVER/ALWAYS never reach the fragment stage
		}
		if (!pass)
			discard;
	}

	// Fog. The console rule (gs-interp capture, SCPH-30001, and the Tile floor's walk) is
	// (C*F + Cfog*(256 - F)) >> 8 on RGB with alpha passed through untouched, written here the way
	// the floor writes it: Cfog plus the difference scaled by F at fifteen fractional bits, all in
	// integer arithmetic, because a float mix rounds where the hardware truncates. Our colour is in
	// normalised guest units, so it scales to 0..255 for the walk and back afterwards.
	if (sr.fge != 0u)
	{
		const ivec3 cfog = ivec3(uvec3(sr.fogcol, sr.fogcol >> 8u, sr.fogcol >> 16u) & 0xFFu);
		const int f15 = int(v_fog * (255.0f * 128.0f));
		cv.rgb = vec3(cfog + (((ivec3(cv.rgb * 255.0f) - cfog) * f15) >> 15)) * (1.0f / 255.0f);
	}

	o_color = cv;
	o_blend = vec4(min(cv.a * (255.0f / 128.0f), 1.0f));
}

#endif
