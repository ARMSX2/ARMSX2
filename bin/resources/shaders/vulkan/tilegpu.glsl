// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// ⚠️ SIZE BUDGET, Adreno 650: keep the FRAGMENT program at instrlen <= 123 units (1 unit = 128 B,
// so 15,744 B / 3936 dwords -- the number the `IR3_SHADER_DEBUG=fs` stats line reports). Crossing
// to 124 flips the whole GPU's behaviour: measured 2026-08-20, the same frame went 21.8 -> 30.7 ms
// on SotC with nothing changed but four units of DEAD, never-executed arithmetic. Turnip DMAs the
// program into a 128-unit on-chip instruction RAM and writes SP_PS_INSTR_SIZE, which the prefetcher
// reads; the cliff rides on that register, so DEAD CODE COUNTS and only the fragment stage does.
// Whatever you add here, take the stats line on an a650 before landing it. If a road cannot fit,
// the answer is a per-pass shader VARIANT that leaves the other roads out, not a bigger shader.
// That mechanism is TILEGPU_ROAD_* below: the executor compiles one fragment module per per-pass
// road mask, so a pass pays instruction size only for the roads its own draws take -- which is how
// this budget is enforced, rather than by everything sharing one program that only grows.
//
// ⚠️ THE RULE THAT COMES WITH IT, and it binds every road added here -- the materialised-source road
// below was written under it, and so must the next one be: a
// variant only ever REMOVES code, so the float arithmetic that survives must not change when its
// neighbours go. SPIR-V lets a driver fuse a mul-add and lets it pick a lowering for a built-in
// like mix(), and both choices move with the surrounding code -- so any mul-add on the texel path
// is written as an explicit fma() whose result is a `precise` variable, which the spec makes one
// fused operation nobody may re-lower. Unpinned, dropping an untaken road moved real pixels.
//
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

// TILEGPU_TEX_TARGETS (injected by the device): 1 when the device can index the per-pass
// sampled-target array by a value the shader computes (shaderSampledImageArrayDynamicIndexing).
// 0 leaves the array undeclared and rule 2's whole branch out of the program; the renderer
// independently refuses to bind targets there, so the tap only ever sees
// tex_target == 0xFFFFFFFF.

// TILEGPU_MAX_TEX_SOURCES (injected by the device): the size of the per-pass sampled-target array
// set 1 binds -- GSTileGpuPassPlan::kMaxTexSourcesPerPass. A state row's tex_target is a slot in it,
// or 0xFFFFFFFF for "decode the bytes".

// TILEGPU_TEX_SOURCES / TILEGPU_MAX_SOURCES (injected by the device): rule 3's array of MATERIALISED
// texture sources -- ordinary RGBA8 images the frame's prep ops built out of the same ring bytes the
// byte road reads. It is FRAME-scoped, not per-pass (a source belongs to a window, not to a target
// pair), so set 2 is written once per plan and a state row's tex_source indexes the whole frame's
// list. TILEGPU_TEX_SOURCES rides the same device capability the target array does -- dynamic
// indexing of a sampled-image array -- so a device without it declares neither array.

// TILEGPU_ROAD_BYTE / TILEGPU_ROAD_TARGET / TILEGPU_ROAD_SOURCE (injected by the device, PER PASS):
// the texel roads this pass's draws actually take -- GSTileGpuPass::road_mask, ORed over the pass's
// draws by the renderer at grouping. A road nobody takes is left out of the module entirely, because
// on Adreno an unexecuted instruction still costs program size (see the budget note above). The mask
// is constant across a pass, so it selects a fragment module and a pipeline exactly the way the depth
// mode and the blend key already do, and never splits an indirect run. All default to 1 -- an
// uninjected compile is the full shader, which is what every road together means.
#ifndef TILEGPU_TEX
#define TILEGPU_TEX 0
#endif

#ifndef TILEGPU_TEX_TARGETS
#define TILEGPU_TEX_TARGETS 0
#endif

#ifndef TILEGPU_TEX_SOURCES
#define TILEGPU_TEX_SOURCES 0
#endif

#ifndef TILEGPU_MAX_TEX_SOURCES
#define TILEGPU_MAX_TEX_SOURCES 8
#endif

#ifndef TILEGPU_MAX_SOURCES
#define TILEGPU_MAX_SOURCES 8
#endif

#ifndef TILEGPU_ROAD_BYTE
#define TILEGPU_ROAD_BYTE 1
#endif

#ifndef TILEGPU_ROAD_TARGET
#define TILEGPU_ROAD_TARGET 1
#endif

#ifndef TILEGPU_ROAD_SOURCE
#define TILEGPU_ROAD_SOURCE 1
#endif

// The gates the body actually uses. A road compiles in only where the device can serve it at all, so
// a stray mask bit can never resurrect a path TILEGPU_TEX or TILEGPU_TEX_TARGETS took out; and with
// no road at all (an untextured pass) the whole texture block goes, which is the smallest variant.
// TILEGPU_TAP_ANY is the pair of roads that go through the per-texel tap: rule 3 does not, because a
// materialised source is one hardware-filtered sample, not four wrapped and unpacked ones.
#define TILEGPU_TAP_BYTE (TILEGPU_TEX && TILEGPU_ROAD_BYTE)
#define TILEGPU_TAP_TARGET (TILEGPU_TEX && TILEGPU_TEX_TARGETS && TILEGPU_ROAD_TARGET)
#define TILEGPU_TAP_SOURCE (TILEGPU_TEX && TILEGPU_TEX_SOURCES && TILEGPU_ROAD_SOURCE)
#define TILEGPU_TAP_ANY (TILEGPU_TAP_BYTE || TILEGPU_TAP_TARGET)
#define TILEGPU_TEXTURED (TILEGPU_TAP_ANY || TILEGPU_TAP_SOURCE)

// Matches the executor's StateRow byte-for-byte (std430, 160 bytes). The transform and the scissor
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
	uint index_format; // 0 = direct 32-bit texel, else GSTileSwizzleForms::IndexFormatFor + 1:
	                   // 1 = PSMT8, 2 = PSMT4, 3 = PSMT8H, 4 = PSMT4HL, 5 = PSMT4HH;
	                   // else GSTileSwizzleForms::Direct16FormatFor + 6, a direct 16-bit texel:
	                   // 6 = PSMCT16, 7 = PSMCT16S, 8 = PSMZ16, 9 = PSMZ16S. Bit 0 of (fmt - 6)
	                   // picks the strided block table, bit 1 the depth block XOR.
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
	uint texa;         // texel alpha the format does not store: bit 0 = apply (the 24-bit road's
	                   // gate; the 16-bit road always applies), bit 1 = TEXA.AEM,
	                   // bits 8-15 = TEXA.TA0, bits 16-23 = TEXA.TA1 (16-bit only -- a 24-bit
	                   // texel has no alpha BIT to select on, so TA1 never reaches that road)
	uint region_u;     // CLAMP.MINU | (CLAMP.MAXU << 16)
	uint region_v;     // CLAMP.MINV | (CLAMP.MAXV << 16)
	uint ltf;          // 1 = bilinear: blend the four texels around the coordinate
	uint tex_target;   // slot in this pass's sampled-target array, or 0xFFFFFFFF = decode the bytes
	uint tex_source;   // slot in the frame's materialised-source array, or 0xFFFFFFFF = not rule 3.
	                   // It took the row's old explicit tail padding -- see the C++ StateRow's note
	                   // on why the padding is explicit.
	uint pal_mode;     // where this draw's palette words are: 0 = entry order at pal_offset (the CPU
	                   // road), 1 = a 256-entry palette's four copied blocks, 2 = a 16-entry one's
	                   // single copied block. The two copied modes need the CSM1 entry order applied
	                   // at fetch, because an image-to-buffer copy lands texels row-major.
	uint pal_bias;     // entry bias inside a copied palette: a four-bit draw reading slot k of a
	                   // 256-entry gathered load wants its entries 16k..16k+15.
	uint pad0_, pad1_; // explicit, for the same reason the old tail padding was: alignas(16) pads the
	                   // C++ row to 160 while std430's array stride would otherwise be 152, and the
	                   // two strides disagreeing reads every row but the first from the wrong place.
};

layout(std430, set = 0, binding = 0) readonly buffer StateTable
{
	StateRow state_rows[];
};

layout(push_constant) uniform cb
{
	uint base_row;    // this frame's first state row in the ring buffer (vertex stage)
	uint table_base;  // this frame's first page-table word in the ring buffer (fragment stage)
	uint pal_base;    // this frame's first palette word in the ring buffer (fragment stage)
	uint epoch_count; // page tables this frame staged; the bound a state row's epoch is clamped to
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

#if TILEGPU_TEXTURED

// A guest byte, normalised. Trivial, and shared on purpose: the byte road reaches it through
// tilegpu_unpack and rule 3's point road through the re-derivation in tilegpu_source_sample, and the
// two roads have to land on the SAME float for the same byte or a draw that changes road changes
// colour. One spelling in one place is what makes that true by construction rather than by
// inspection of two.
vec4 tilegpu_norm8(vec4 b)
{
	return b * (1.0f / 255.0f);
}

#if TILEGPU_TAP_BYTE

// The frame's ring: page slots, the epoch page tables and the palettes, all in one storage buffer
// of 32-bit words. Reads go through tilegpu_ring_word below; nothing addresses guest memory flat.
// Only the byte road reads it (the palettes included), so a pass whose draws all sample resident
// targets does not declare it.
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
	// The frame stages exactly `epoch_count` tables, and the CPU builds the state rows from the same
	// counter, so an out-of-range epoch cannot happen. The clamp is here because of what happens if
	// it ever does: the read would run off the end of the table into the page entries or the palettes
	// -- live data in this same buffer -- and hand back a plausible-looking slot instead of failing.
	uint e = min(epoch, max(epoch_count, 1u) - 1u);
	uint slot = vram_words[table_base + e * 512u + page];
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

// Extract byte (sel & 3) of an SSBO-loaded word. TILEGPU_STATIC_BYTE_SEL (injected by the
// device from the driver id) selects a branchy form with constant shift amounts, because
// Honeykrisp (Mesa 25.3.6, Apple M2 Max) miscompiles the dynamic form: the load returns
// the right word, but the byte is selected by the low bits of the word INDEX instead of
// sel, so every word whose ring index is not a multiple of 4 reads 0 -- a 2x2 texel
// lattice over every paletted texture. A second, unrelated use of the sel sub-expression
// also hides it, so it is an optimiser defect, not shader semantics. Every other driver
// keeps the straight-line shift.
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

// The PSMT8 index byte at texel (u, v): 128x64 page, 16x16-texel columns; block b occupies bytes
// [b*256, b*256+256) = words [b*64, b*64+64). byte_in_block picks the byte within its word.
uint tilegpu_index8(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 6u) * tbw + (u >> 7u);
	uint blk = tbp0 + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u);
	uint byte_in_block = tile_c8(u & 15u, v & 15u);
	uint word = vram_words[tilegpu_ring_word(blk * 64u + (byte_in_block >> 2u), epoch)];
	return tilegpu_byte_sel(word, byte_in_block);
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
	uint byteval = tilegpu_byte_sel(word, byte_in_block);
	return ((nib & 1u) != 0u) ? (byteval >> 4u) : (byteval & 0xFu);
}

// The alpha-byte views: PSMT8H, PSMT4HL, PSMT4HH. Their address geometry is PSMCT32's -- 64x32
// pages, 8x8 blocks, 8x8-texel columns, TBW pages per row, one word per texel -- and the palette
// index lives in the TOP BITS of that word. So the address is tilegpu_texel32's, unchanged, and the
// only thing these formats add is which bits come out of it.
//
// ⚠️ They carry a palette but they are NOT 128 texels to a page. The state row's tbw is TBW here,
// not TBW >> 1; the renderer takes both from GSTileSwizzleForms::PagesPerRow, which reads the page
// width off the psm table rather than asking whether the format is paletted.
//
// `fmt` is the state row's index_format: 3 = PSMT8H (bits 24-31), 4 = PSMT4HL (24-27), 5 = PSMT4HH
// (28-31). Dynamically uniform per draw, so the two compares do not diverge.
//
// The extract is bitfieldExtract at a CONSTANT offset and width, then constant shifts for the
// nibble -- deliberately, not stylistically. A sub-word extract whose shift amount is computed is
// the shape Honeykrisp miscompiles on a word loaded from this SSBO (tilegpu_byte_sel's comment has
// the whole story), and these are the forms that survive it there: the same constant-shift nibble
// pick tilegpu_index4 takes under the gate, and the same bitfieldExtract tilegpu_reinterpret.glsl
// already relies on. Every other driver gets one instruction either way, so there is nothing to
// gate.
uint tilegpu_index_hi(uint u, uint v, uint tbp0, uint tbw, uint epoch, uint fmt)
{
	// The alpha byte: the whole index for PSMT8H, and the byte the two 4-bit views take a nibble of.
	uint b = bitfieldExtract(tilegpu_texel32(u, v, tbp0, tbw, epoch), 24, 8);
	if (fmt == 3u)
		return b;
	return (fmt == 4u) ? (b & 0xFu) : (b >> 4u);
}

// The CLUT gather's consumer half. A palette whose words were rendered by a native draw is not in
// the CPU's CLUT RAM at all; the executor copies its BLOCKS out of the owning target into this
// draw's reserved run of the palette stream, which lands them in texel row-major order rather than
// in entry order. So the entry order goes here, at fetch time: entry -> source word through the
// CSM1 loaders' own order (TILE_SWZ_CLUT8_*/CLUT4_*, the same forms the gather shader uses), then
// word -> its place in the copied blocks through the inverse column form. inv_col32 packs (x, y) as
// x | (y << 3), which for an 8-wide region IS the row-major offset, so nothing has to be repacked.
// Specified on the CPU as GSTileSwizzleForms::ClutEntryToCopyOffset and pinned against GSClut there.
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

// The palette word for `index`, whichever road put the words in the stream. pal_mode is per draw and
// dynamically uniform, so this does not diverge; mode 0 is byte-identical to what the palette fetch
// was before the gather existed.
uint tilegpu_palette_word(StateRow sr, uint index)
{
	if (sr.pal_mode == 0u)
		return vram_words[pal_base + sr.pal_offset + index];
	const uint e = sr.pal_bias + index;
	const uint w = (sr.pal_mode == 1u) ? tile_clut8_word(e) : tile_clut4_word(e);
	return vram_words[pal_base + sr.pal_offset + (w >> 6u) * 64u + tile_ic32(w & 63u)];
}

// The two forms the 16-bit families add: the strided block table (PSMCT16S / PSMZ16S -- the plain
// one is tile_b84 above, because blockTable16 IS blockTable4 and the fit checks it) and the
// halfword-in-block column table. A 16-bit page is 64x64 texels of 16x8-texel blocks, two texels
// to a 32-bit word.
uint tile_b84s(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B84S_X0) ^ XB(x, 1u, TILE_SWZ_B84S_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84S_Y0) ^ XB(y, 1u, TILE_SWZ_B84S_Y1) ^ XB(y, 2u, TILE_SWZ_B84S_Y2);
}

uint tile_c16(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C16_X0) ^ XB(x, 1u, TILE_SWZ_C16_X1) ^ XB(x, 2u, TILE_SWZ_C16_X2) ^ XB(x, 3u, TILE_SWZ_C16_X3)
	     ^ XB(y, 0u, TILE_SWZ_C16_Y0) ^ XB(y, 1u, TILE_SWZ_C16_Y1) ^ XB(y, 2u, TILE_SWZ_C16_Y2);
}

// Halfword (sel & 1) of an SSBO-loaded word, under the same driver gate tilegpu_byte_sel carries
// and for the same reason: Honeykrisp takes the selector of a computed sub-word shift from the word
// INDEX. Here the selector is bit 3 of u, so the miscompile would blank every other 8-texel column
// of every 16-bit texture.
uint tilegpu_half_sel(uint word, uint sel)
{
#if TILEGPU_STATIC_BYTE_SEL
	return ((sel & 1u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
#else
	return (word >> ((sel & 1u) * 16u)) & 0xFFFFu;
#endif
}

// A direct 16-bit texel at (u, v), expanded to normalised RGBA. `fmt` is the state row's
// index_format minus six: bit 0 selects the strided block table, bit 1 the depth block XOR.
//
// The XOR lands on the ABSOLUTE block, after the base and the page term, because that is where
// GSOffset::bn() applies it -- distributing it into the in-page index would be right only for a
// page-aligned TBP0, and a texture window's is often not.
//
// The alpha rule is the GAME's TEXA, not the frame-buffer pin the seed and writeback use: bit 15
// selects TA1, and with AEM set an all-zero cell is transparent instead of taking TA0. That is
// GSLocalMemory::Expand16To32 exactly, including its test being on the whole cell.
vec4 tilegpu_texel16(StateRow sr, uint u, uint v, uint fmt)
{
	const uint page = (v >> 6u) * sr.tbw + (u >> 6u);
	const uint b = ((fmt & 1u) != 0u) ? tile_b84s((u >> 4u) & 3u, (v >> 3u) & 7u)
	                                  : tile_b84((u >> 4u) & 3u, (v >> 3u) & 7u);
	// & 16383 is bn()'s own `% GS_MAX_BLOCKS`, a power of two. It matters here and not on the
	// 32-bit arm: a window based near the top of memory wraps, and an unwrapped block would send
	// tilegpu_ring_word past the end of the page table into the entries and the palettes -- live
	// data in the same buffer -- and hand back a plausible slot.
	const uint blk = ((sr.tbp0 + page * 32u + b) ^ (((fmt & 2u) != 0u) ? TILE_SWZ_Z16XOR : 0u)) & 16383u;
	const uint hw = tile_c16(u & 15u, v & 7u);
	const uint c = tilegpu_half_sel(vram_words[tilegpu_ring_word(blk * 64u + (hw >> 1u), sr.epoch)], hw);

	const uint a = ((c & 0x8000u) != 0u) ? ((sr.texa >> 16u) & 0xFFu)
	             : ((((sr.texa & 2u) != 0u) && c == 0u) ? 0u : ((sr.texa >> 8u) & 0xFFu));
	return tilegpu_norm8(vec4(float((c & 0x001Fu) << 3u), float((c & 0x03E0u) >> 2u),
		float((c & 0x7C00u) >> 7u), float(a)));
}

// Unpack a raw RGBA8888 word (a CT32 texel or an expanded CLUT entry) to normalised RGBA.
vec4 tilegpu_unpack(uint w)
{
	return tilegpu_norm8(vec4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
		float((w >> 24u) & 0xFFu)));
}

#endif // TILEGPU_TAP_BYTE

#if TILEGPU_TAP_ANY

// TEXA: a 24-bit texel stores no alpha byte, so the GS gives it TEXA.TA0, and AEM makes a texel
// whose RGB is entirely zero transparent instead. (A paletted texture takes TEXA in the CLUT
// expansion on the CPU, so only the direct roads need this; rule 3's image has it baked in at build
// time.) `rgb_zero` is the AEM test, which the two roads phrase differently -- a word compare on the
// byte road, a channel compare on the image.
vec4 tilegpu_texa(StateRow sr, vec4 t, bool rgb_zero)
{
	if ((sr.texa & 1u) != 0u)
	{
		const bool aem_zero = (sr.texa & 2u) != 0u && rgb_zero;
		t.a = aem_zero ? 0.0f : float((sr.texa >> 8u) & 0xFFu) * (1.0f / 255.0f);
	}
	return t;
}

#endif // TILEGPU_TAP_ANY

// Rule 2 of the VRAM model's texel road: this pass's resident sampled targets. A target's pixel
// space IS the guest layout it renders (pool page (row, col) = base page + row*bw + col), and the
// renderer only names one here when the page model proves that target is the sole owner of the whole
// read window at the same base, stride and format. So target pixel (u, v) holds guest texel (u, v),
// and the fetch below returns exactly what tilegpu_texel32 would -- out of the LIVE pixels, instead
// of out of bytes some earlier writeback had to compose first.
//
// ONE fetch site, indexed dynamically. This used to be a chain of eight literal compares, which
// needs no device feature -- and cost 12 instrlen units, carrying the program from 117 to 129 and
// straight over the a650 cliff the header warns about (measured 2026-08-20: sotc 21.4 -> 32.1 ms
// GPU). The chain was inlined four times over by bilinear sampling, so it paid for eight fetch
// sites times four taps whatever a draw actually did.
//
// ⚠️ `slot` MUST be dynamically uniform, and what makes it so is on the executor's side: one
// vkCmdDrawIndexedIndirect covers many draws, so the executor splits its indirect runs wherever
// the slot changes and every call it issues carries a single slot. Undecorated is deliberate --
// nonuniformEXT() compiles to the same instruction count but switches ir3 from
// isam.s2en.uniform (one descriptor per wave) to isam.s2en.nonuniform (one per lane), measured
// 2026-08-20 at +1.20 ms GPU on SotC, +5.6%, and free on the five dumps that barely use rule 2.
// If the run splitting is ever dropped, this needs nonuniformEXT and its feature back.
#if TILEGPU_TAP_TARGET
layout(set = 1, binding = 1) uniform sampler2D u_targets[TILEGPU_MAX_TEX_SOURCES];

vec4 tilegpu_target_texel(uint slot, ivec2 c)
{
	return texelFetch(u_targets[slot], c, 0);
}
#endif

// Apply the wrap mode to one axis. REPEAT masks (dims are powers of two); CLAMP clamps to the
// texture; the two REGION modes work off the CLAMP register's MIN/MAX pair for the axis, which
// arrives packed as MIN | (MAX << 16) -- REGION_CLAMP clamps between them, REGION_REPEAT reads them
// as a mask and an or-value, which is how a game addresses a sub-rect of a shared texture page.
int tilegpu_wrap(int c, uint dim, uint mode, uint region)
{
	const int rmin = int(region & 0xFFFFu);
	const int rmax = int(region >> 16u);
	if (mode == 0u) // REPEAT
		return int(uint(c) & (dim - 1u));
	if (mode == 2u) // REGION_CLAMP
		return clamp(c, rmin, rmax);
	if (mode == 3u) // REGION_REPEAT
		return int((uint(c) & uint(rmin)) | uint(rmax));
	return clamp(c, 0, int(dim) - 1); // CLAMP
}

#if TILEGPU_TAP_ANY

// One texel of the draw's texture at a raw (pre-wrap) coordinate: wrapped per axis, read from the
// bytes by whichever address geometry the format uses, palette-expanded if it is an index, and given
// its TEXA alpha if it is a 24-bit texel. This is the whole per-tap road, so a bilinear draw gets the
// same treatment on each of its four corners -- which is what the GS does: it looks the four texels
// up individually, expands each through the CLUT, and blends the results, never the indices.
vec4 tilegpu_tap(StateRow sr, int cu, int cv)
{
	const uint iu = uint(tilegpu_wrap(cu, sr.tw, sr.wms, sr.region_u));
	const uint iv = uint(tilegpu_wrap(cv, sr.th, sr.wmt, sr.region_v));

	// Rule 2: the texel comes out of a resident target instead of the bytes. Same wrap, same
	// coordinate, same TEXA -- only the fetch differs, so the two roads agree wherever the bytes
	// are current and the image road wins wherever they are not. The min is a guard, not a
	// semantic: every wrap mode lands inside tw x th (GetSizeFixedTEX0 grows TW/TH to cover a
	// REGION window), and the renderer refuses the bind unless tw x th fits the image, so this
	// only stops a fetch running out of bounds if one of those ever stops holding.
#if TILEGPU_TAP_TARGET
	if (sr.tex_target != 0xFFFFFFFFu)
	{
		const vec4 tt = tilegpu_target_texel(sr.tex_target,
			ivec2(int(min(iu, sr.tw - 1u)), int(min(iv, sr.th - 1u))));
		return tilegpu_texa(sr, tt, all(equal(tt.rgb, vec3(0.0f))));
	}
#endif

#if TILEGPU_TAP_BYTE
	// The 16-bit families first, because their alpha rule is their own: a 16-bit texel has an
	// alpha BIT, so TEXA selects between TA1 and TA0 rather than always supplying TA0, and the
	// AEM test is on the whole cell rather than on an RGB word. tilegpu_texa below cannot express
	// that, so this arm returns rather than falling through to it.
	if (sr.index_format >= 6u)
		return tilegpu_texel16(sr, iu, iv, sr.index_format - 6u);

	// index_format is dynamically uniform per draw, so this does not diverge.
	uint w;
	if (sr.index_format == 0u)
		w = tilegpu_texel32(iu, iv, sr.tbp0, sr.tbw, sr.epoch);
	else if (sr.index_format == 1u)
		w = tilegpu_palette_word(sr, tilegpu_index8(iu, iv, sr.tbp0, sr.tbw, sr.epoch));
	else if (sr.index_format == 2u)
		w = tilegpu_palette_word(sr, tilegpu_index4(iu, iv, sr.tbp0, sr.tbw, sr.epoch));
	else
		w = tilegpu_palette_word(sr, tilegpu_index_hi(iu, iv, sr.tbp0, sr.tbw, sr.epoch, sr.index_format));

	return tilegpu_texa(sr, tilegpu_unpack(w), (w & 0x00FFFFFFu) == 0u);
#else
	// Target road only: every textured draw in this pass names a target, so the branch above took
	// every fragment. GLSL still needs the fall-through to return something.
	return vec4(0.0f);
#endif
}

#endif // TILEGPU_TAP_ANY

// Rule 3 of the VRAM model's texel road: a MATERIALISED source. The frame's prep ops deswizzled this
// window out of the same ring bytes the byte road reads into an ordinary RGBA8 image the size of the
// window (tilegpu_materialise.glsl), TEXA already baked into its alpha, so the whole per-tap road --
// the swizzle arithmetic, the dependent palette fetch, the per-tap TEXA, and under LINEAR all four
// of them -- collapses to one fetch of a normal texture.
//
// ⚠️ `slot` MUST be dynamically uniform, for the reason spelled out over tilegpu_target_texel: the
// executor splits its indirect runs wherever a draw's sampled binding changes, and this array shares
// that split key with the target array.
#if TILEGPU_TAP_SOURCE
layout(set = 2, binding = 0) uniform sampler2D u_sources[TILEGPU_MAX_SOURCES];

vec4 tilegpu_source_sample(StateRow sr, vec2 uv)
{
	if (sr.ltf == 0u)
	{
		// NEAREST, and this arm has to be BIT-IDENTICAL to the byte road's, because a draw moving
		// between the two roads must not move a pixel. Two things buy that.
		//
		// The coordinate is integer: the same tilegpu_wrap the byte road applies, then texelFetch --
		// not a normalised sample, whose coordinate would go through the sampler's own fixed-point
		// quantisation to reach the same texel.
		//
		// And the VALUE is re-derived rather than taken. The image holds the guest byte, but what a
		// fetch hands back is the texture unit's unorm8->float conversion, and that rounding is the
		// hardware's business: `k/255` correctly rounded and `k * (1/255)` differ by one ULP for 126
		// of the 256 bytes, and nothing in the shader says which the device does. The byte road says
		// tilegpu_norm8, so this road recovers the byte -- round-tripping through *255 is exact from
		// either conversion, since both land within an ULP of k -- and says tilegpu_norm8 too.
		const int iu = tilegpu_wrap(int(floor(uv.x)), sr.tw, sr.wms, sr.region_u);
		const int iv = tilegpu_wrap(int(floor(uv.y)), sr.th, sr.wmt, sr.region_v);
		const vec4 raw = texelFetch(u_sources[sr.tex_source], ivec2(iu, iv), 0);
		precise vec4 b = floor(fma(raw, vec4(255.0f), vec4(0.5f)));
		return tilegpu_norm8(b);
	}
	// LINEAR: the hardware's bilinear, which is the whole point of the road. The weights are the
	// sampler's fixed-point ones rather than the byte road's fp32 fma chain, so a LINEAR draw's
	// pixels move when it changes road -- a deliberate accuracy trade, rowed in the deferred-accuracy
	// ledger, and the reason the identity above is stated for NEAREST alone.
	//
	// tw/th are powers of two, so the reciprocal is exact and so is the scale: the normalised
	// coordinate is uv/dim with no rounding of its own, which puts the sampler's texel index and
	// fraction on exactly the coordinate the byte road splits. textureLod pins level 0 in the shader
	// as well as in the sampler (rule 3 is level 0 only -- GS LOD selection is M4), which also spares
	// the fragment stage the derivative.
	const vec2 n = uv * vec2(1.0f / float(sr.tw), 1.0f / float(sr.th));
	return textureLod(u_sources[sr.tex_source], n, 0.0f);
}
#endif

// The draw's texture at a texel coordinate, filtered the way TEX1 asked. NEAREST truncates the
// coordinate; LINEAR blends the four texels around it, sampling half a texel back so the weights are
// zero at a texel centre -- the GS subtracts the same half texel from its fixed-point coordinate
// before splitting it into an index and a fraction.
//
// ⚠️ The three lerps are explicit fma() under `precise`, per the pinning rule in the header, and it
// is load-bearing rather than stylistic. Written as mix() the blend is a built-in the driver lowers
// to whichever of `a*(1-t) + b*t` or `a + t*(b-a)` suits the surrounding code -- and the surrounding
// code is exactly what a variant changes, so the same texel came out one LSB apart depending on
// which roads the pass compiled. Measured before it was pinned: ten of eighteen corpus dumps moved,
// worst case a few hundred pixels, and wherever the LSB landed on an alpha test the whole fragment
// flipped. `precise` on the result makes each fma a single fused rounding that nobody may re-lower,
// so every variant computes the same texel -- and one rounding per lerp instead of two is not a
// concession, it is the more accurate form.
vec4 tilegpu_sample(StateRow sr, vec2 uv)
{
	// Rule 3 first: it is a whole-sample road, not a tap road, so it replaces the filter as well as
	// the fetch. The renderer names a source only where the cache proved it holds this window's
	// current bytes; everything else arrives here with tex_source unset and falls through.
#if TILEGPU_TAP_SOURCE
	if (sr.tex_source != 0xFFFFFFFFu)
		return tilegpu_source_sample(sr, uv);
#endif

#if TILEGPU_TAP_ANY
	if (sr.ltf == 0u)
		return tilegpu_tap(sr, int(floor(uv.x)), int(floor(uv.y)));

	const vec2 c = uv - 0.5f;
	const vec2 b = floor(c);
	const vec2 f = c - b;
	const int x0 = int(b.x), y0 = int(b.y);
	const vec4 t00 = tilegpu_tap(sr, x0, y0);
	const vec4 t10 = tilegpu_tap(sr, x0 + 1, y0);
	const vec4 t01 = tilegpu_tap(sr, x0, y0 + 1);
	const vec4 t11 = tilegpu_tap(sr, x0 + 1, y0 + 1);
	precise vec4 top = fma(vec4(f.x), t10 - t00, t00);
	precise vec4 bot = fma(vec4(f.x), t11 - t01, t01);
	precise vec4 t = fma(vec4(f.y), bot - top, top);
	return t;
#else
	// Source road only: every textured draw in this pass names a materialised source, so the branch
	// above took every fragment. GLSL still needs the fall-through to return something.
	return vec4(0.0f);
#endif
}

#endif // TILEGPU_TEXTURED

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
	// precise, same rule: HIGHLIGHT's `Ct*Cf*k + Af` is a mul-add the driver is free to fuse or not,
	// and the alpha test below reads this value -- so a fusion that happens in one variant and not
	// another moves a fragment across ATST and takes the whole pixel with it.
	precise vec4 cv = cf;

#if TILEGPU_TEXTURED
	if (sr.tex_enable != 0u)
	{
		// The texel coordinate: FST is a direct texel (12.4 already unpacked in the VS); STQ divides
		// the interpolated S,T by the interpolated Q and scales by the texture dimensions. The affine
		// interpolation (gl_Position.w == 1) plus this per-pixel divide reproduces the PS2's own
		// affine-rasteriser-with-per-pixel-divide texturing.
		const vec2 uv = (sr.fst != 0u) ? v_uv : (vec2(v_st.x, v_st.y) / v_q) * vec2(float(sr.tw), float(sr.th));
		const vec4 ct = tilegpu_sample(sr, uv);

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
		// The alpha byte the test sees is the one the target would store, so it rounds -- the same
		// value the UNORM write produces. Truncating instead costs a level wherever the texture
		// function lands a hair under an integer, which an ATST=EQUAL draw reads as "no pixel at
		// all": Shadow of the Colossus tests EQUAL against 255 for one of its two sky layers.
		precise float av = fma(cv.a, 255.0f, 0.5f);
		const uint a = uint(av);
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
