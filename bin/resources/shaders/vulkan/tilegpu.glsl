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
// That mechanism is TILEGPU_ROAD_* and TILEGPU_FMT_* below: the executor compiles one fragment
// module per per-pass (road mask, texel-arm mask), so a pass pays instruction size only for the
// roads AND the texel formats its own draws take -- which is how this budget is enforced, rather
// than by everything sharing one program that only grows. Specialization MANAGES the budget; it
// does not remove the cliff, so a new arm still owes the stats line.
//
// ⚠️ The budget has a gate now: gs_tilegpu_shader_budget_tests compiles every variant the planner
// can ask for and fails the suite if a single-geometry one crosses the calibrated SPIR-V word
// ceiling. It exists because this budget was breached twice by landings that never took the stats
// line, and both times nothing said so.
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

// TILEGPU_SELF_* (injected by the device, PER PASS): what this pass's draws need the in-pass
// destination read FOR -- GSTileGpuPass::self_mask, ORed over the draws the renderer admitted to the
// read. Off by default, because the overwhelming majority of passes need none of it and a pass that
// declares nothing must compile to the same program it did before this road existed.
//
// Split three ways for the reason road_mask and texel_mask are split: a pass whose only reader is
// the destination-alpha test must not carry the blend equation's integer arithmetic, and the a650
// instruction budget has no room to spare (the worst gated variant already sits within 5% of the
// ceiling).
#ifndef TILEGPU_SELF_DATE
#define TILEGPU_SELF_DATE 0
#endif

#ifndef TILEGPU_SELF_BLEND
#define TILEGPU_SELF_BLEND 0
#endif

#ifndef TILEGPU_SELF_MASK
#define TILEGPU_SELF_MASK 0
#endif

// TILEGPU_QUANT16 (injected by the device, PER PASS): this pass's frame format stores fewer bits
// than the RGBA8 target holds, so a draw whose output IS what lands has to say what the console
// would have stored. Not a self-read use -- it needs no destination -- but it lands at the same
// point in the fragment stage, so it shares the byte stage below.
#ifndef TILEGPU_QUANT16
#define TILEGPU_QUANT16 0
#endif

// TILEGPU_SPEC_* (injected by the device, PER DRAW): the per-draw GS state this program may treat
// as a COMPILE-TIME CONSTANT instead of reading it out of the state row. Every one of them is a
// field of StateRow that the fragment stage otherwise loads and branches on, and freezing one pays
// three times over on an Adreno 650: the load goes (a state-row read is a cat5 isam with its own
// address arithmetic, and there are seventeen of them because v_row is a flat per-primitive varying
// that nothing can hoist), the compare/select chain the field fed goes, and the delay slots the
// scheduler could not fill around them go with it. The register pressure that costs the textured
// roads their wave size is the same live state.
//
// -1 means "read the row", which is what an unspecialized build does and what the PASS-UNION
// fallback must always do -- a run that stands for many draws cannot freeze anything they disagree
// about. Any other value is that field's exact value for every draw the program serves; the renderer
// puts it in the draw's variant key and the executor compiles a module per distinct key, exactly the
// way the texel road and the decode arm already work.
//
// ⚠️ A specialized program must not move a pixel. That holds only while each axis SELECTS an arm and
// never rewrites one: the arithmetic on the arm that survives is the same arithmetic, character for
// character, and the mul-adds on the texel path stay explicit fma() under `precise` per the header's
// pinning rule. An axis that folded two arms into one cheaper expression would be an accuracy trade,
// which this is not.
#ifndef TILEGPU_SPEC_TEX
#define TILEGPU_SPEC_TEX -1 // StateRow::tex_enable, 0/1
#endif
#ifndef TILEGPU_SPEC_FST
#define TILEGPU_SPEC_FST -1 // StateRow::fst, 0 = STQ, 1 = UV
#endif
#ifndef TILEGPU_SPEC_LTF
#define TILEGPU_SPEC_LTF -1 // StateRow::ltf, 0 = NEAREST, 1 = LINEAR
#endif
#ifndef TILEGPU_SPEC_TFX
#define TILEGPU_SPEC_TFX -1 // StateRow::tfx, 0 MODULATE / 1 DECAL / 2 HIGHLIGHT / 3 HIGHLIGHT2
#endif
#ifndef TILEGPU_SPEC_TCC
#define TILEGPU_SPEC_TCC -1 // StateRow::tcc, 0/1
#endif
#ifndef TILEGPU_SPEC_ATST
// StateRow::atst: 0 = no test at all, else TEST.ATST + 1 (3 LESS .. 8 NOTEQUAL; the renderer folds
// NEVER and ALWAYS into the write flags, so they never reach here). The comparison itself has to be
// fixed, not merely its presence: measured on a650, a program that knows a test happens but not
// which one still holds nine registers and stays on wave64, while one that knows GEQUAL drops to
// eight and flips to wave128.
#define TILEGPU_SPEC_ATST -1
#endif
#ifndef TILEGPU_SPEC_FGE
#define TILEGPU_SPEC_FGE -1 // StateRow::fge, 0/1
#endif
#ifndef TILEGPU_SPEC_DATE
#define TILEGPU_SPEC_DATE -1 // StateRow::date, 0 off / 1 DATM 0 / 2 DATM 1
#endif
#ifndef TILEGPU_SPEC_WMS
#define TILEGPU_SPEC_WMS -1 // StateRow::wms, CLAMP.WMS 0..3
#endif
#ifndef TILEGPU_SPEC_WMT
#define TILEGPU_SPEC_WMT -1 // StateRow::wmt, CLAMP.WMT 0..3
#endif
#ifndef TILEGPU_SPEC_TEXA
#define TILEGPU_SPEC_TEXA -1 // the low two bits of StateRow::texa: bit 0 apply, bit 1 AEM
#endif

// The readers. One spelling per field, so a call site cannot accidentally read the row on one road
// and the constant on another.
#if TILEGPU_SPEC_TEX < 0
#define TG_TEX(sr) ((sr).tex_enable)
#else
#define TG_TEX(sr) uint(TILEGPU_SPEC_TEX)
#endif
#if TILEGPU_SPEC_FST < 0
#define TG_FST(sr) ((sr).fst)
#else
#define TG_FST(sr) uint(TILEGPU_SPEC_FST)
#endif
#if TILEGPU_SPEC_LTF < 0
#define TG_LTF(sr) ((sr).ltf)
#else
#define TG_LTF(sr) uint(TILEGPU_SPEC_LTF)
#endif
#if TILEGPU_SPEC_TFX < 0
#define TG_TFX(sr) ((sr).tfx)
#else
#define TG_TFX(sr) uint(TILEGPU_SPEC_TFX)
#endif
#if TILEGPU_SPEC_TCC < 0
#define TG_TCC(sr) ((sr).tcc)
#else
#define TG_TCC(sr) uint(TILEGPU_SPEC_TCC)
#endif
#if TILEGPU_SPEC_ATST < 0
#define TG_ATST_ANY(sr) ((sr).atst != 0u)
#define TG_ATST_OP(sr) ((sr).atst)
#elif TILEGPU_SPEC_ATST == 0
#define TG_ATST_ANY(sr) false
#define TG_ATST_OP(sr) 0u
#else
#define TG_ATST_ANY(sr) true
#define TG_ATST_OP(sr) uint(TILEGPU_SPEC_ATST)
#endif
#if TILEGPU_SPEC_FGE < 0
#define TG_FGE(sr) ((sr).fge)
#else
#define TG_FGE(sr) uint(TILEGPU_SPEC_FGE)
#endif
#if TILEGPU_SPEC_DATE < 0
#define TG_DATE(sr) ((sr).date)
#else
#define TG_DATE(sr) uint(TILEGPU_SPEC_DATE)
#endif
#if TILEGPU_SPEC_WMS < 0
#define TG_WMS(sr) ((sr).wms)
#else
#define TG_WMS(sr) uint(TILEGPU_SPEC_WMS)
#endif
#if TILEGPU_SPEC_WMT < 0
#define TG_WMT(sr) ((sr).wmt)
#else
#define TG_WMT(sr) uint(TILEGPU_SPEC_WMT)
#endif
// TEXA's two flag bits only: TA0 and TA1 are eight-bit values that vary draw to draw within any
// sensible variant population, so they stay in the row.
#if TILEGPU_SPEC_TEXA < 0
#define TG_TEXA_APPLY(sr) (((sr).texa & 1u) != 0u)
#define TG_TEXA_AEM(sr) (((sr).texa & 2u) != 0u)
#else
#define TG_TEXA_APPLY(sr) ((uint(TILEGPU_SPEC_TEXA) & 1u) != 0u)
#define TG_TEXA_AEM(sr) ((uint(TILEGPU_SPEC_TEXA) & 2u) != 0u)
#endif

#define TILEGPU_SELF_READ (TILEGPU_SELF_DATE || TILEGPU_SELF_BLEND || TILEGPU_SELF_MASK)
// The fragment stage's integer tail: anything that has to see the fragment as the BYTES the target
// would store rather than as a normalised colour.
#define TILEGPU_BYTE_TAIL (TILEGPU_SELF_BLEND || TILEGPU_SELF_MASK || TILEGPU_QUANT16)

// TILEGPU_FMT_* (injected by the device, PER PASS): the BYTE road's texel-decode ARMS this pass's
// draws actually use -- GSTileGpuPass::texel_mask, ORed over the pass's byte-road draws exactly the
// way road_mask is ORed over its draws' roads. The byte road is not one decoder, it is five address
// geometries that share a wrapper, and tilegpu_tap is inlined FIVE times over by bilinear sampling
// -- so a pass that samples nothing but PSMT8 was paying program size for four geometries it never
// executes, times five. That is what carried the byte-carrying programs from 108-121 units (under
// the a650 cliff) to 260-271 (more than double it).
//
// Formats that share an address geometry share an arm, deliberately: the three alpha-byte views
// differ by one bitfieldExtract and the four 16-bit families by two selects, so splitting those
// further buys a handful of instructions and multiplies the variant population for nothing.
//
// Like road_mask the mask is constant across a pass by construction (it is a union, not a per-draw
// choice), so it selects a module and a pipeline and never splits an indirect run. All default to
// 1: an uninjected compile is the full shader, which is every arm together.
#ifndef TILEGPU_FMT_D32
#define TILEGPU_FMT_D32 1 // index_format 0: PSMCT32 / PSMCT24
#endif

#ifndef TILEGPU_FMT_IDX8
#define TILEGPU_FMT_IDX8 1 // index_format 1: PSMT8
#endif

#ifndef TILEGPU_FMT_IDX4
#define TILEGPU_FMT_IDX4 1 // index_format 2: PSMT4
#endif

#ifndef TILEGPU_FMT_IDXHI
#define TILEGPU_FMT_IDXHI 1 // index_format 3-5: PSMT8H / PSMT4HL / PSMT4HH
#endif

#ifndef TILEGPU_FMT_D16
#define TILEGPU_FMT_D16 1 // index_format 6-9: PSMCT16 / PSMCT16S / PSMZ16 / PSMZ16S
#endif

// The sixth arm is not an address geometry, it is the palette ORDER a GATHERED palette needs. A
// palette the CPU expanded is in entry order and one entry is one indexed load; a palette copied out
// of the target a native draw rendered it into lands texel row-major, so the CSM1 entry order has to
// be applied at fetch -- three more closed forms, eighteen XOR terms, inlined per tap like everything
// else on this road. Two of the eighteen corpus dumps ever gather a palette, and this is what keeps
// the other sixteen from carrying the machinery. It is also, precisely, the commit that pushed every
// byte-carrying program over the a650 cliff in one step.
#ifndef TILEGPU_FMT_PALGATHER
#define TILEGPU_FMT_PALGATHER 1
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

// The byte road's arms, gated the same way: an arm exists only where the byte road itself does, so a
// stray format bit on a pass that samples only resident targets can never resurrect the ring reads.
#define TILEGPU_BYTE_D32 (TILEGPU_TAP_BYTE && TILEGPU_FMT_D32)
#define TILEGPU_BYTE_IDX8 (TILEGPU_TAP_BYTE && TILEGPU_FMT_IDX8)
#define TILEGPU_BYTE_IDX4 (TILEGPU_TAP_BYTE && TILEGPU_FMT_IDX4)
#define TILEGPU_BYTE_IDXHI (TILEGPU_TAP_BYTE && TILEGPU_FMT_IDXHI)
#define TILEGPU_BYTE_D16 (TILEGPU_TAP_BYTE && TILEGPU_FMT_D16)
// What several arms need in common. PAL is the palette fetch, which every index arm ends in; W32 is
// the CT32 address geometry, which the direct 32-bit texel and the three alpha-byte views share;
// UNPACK is "this arm produces a 32-bit RGBA word", which is every arm except the 16-bit one (whose
// texel is a halfword with its own TEXA rule).
#define TILEGPU_BYTE_PAL (TILEGPU_BYTE_IDX8 || TILEGPU_BYTE_IDX4 || TILEGPU_BYTE_IDXHI)
#define TILEGPU_BYTE_PALGATHER (TILEGPU_BYTE_PAL && TILEGPU_FMT_PALGATHER)
#define TILEGPU_BYTE_W32 (TILEGPU_BYTE_D32 || TILEGPU_BYTE_IDXHI)
#define TILEGPU_BYTE_UNPACK (TILEGPU_BYTE_D32 || TILEGPU_BYTE_PAL)

// Matches the executor's StateRow byte-for-byte (std430, 144 bytes). The transform is read in the
// vertex stage; the texture fields and the tests in the fragment stage. z_write/z_test are pipeline
// state, carried for layout parity, not consumed by either. The GS scissor is NOT here: it is a
// vkCmdSetScissor before each indirect call, which is command state and not shader state.
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
	uint index_format; // 0 = direct 32-bit texel in the CT32 block space (PSMCT32 / PSMCT24),
	                   // else GSTileSwizzleForms::IndexFormatFor + 1:
	                   // 1 = PSMT8, 2 = PSMT4, 3 = PSMT8H, 4 = PSMT4HL, 5 = PSMT4HH;
	                   // else GSTileSwizzleForms::Direct16FormatFor + 6, a direct 16-bit texel:
	                   // 6 = PSMCT16, 7 = PSMCT16S, 8 = PSMZ16, 9 = PSMZ16S. Bit 0 of (fmt - 6)
	                   // picks the strided block table, bit 1 the depth block XOR;
	                   // 10 = direct 32-bit texel in the DEPTH block space (PSMZ32 / PSMZ24), which
	                   // is 0's tables under the 32-bit depth block XOR. Not contiguous with 0 on
	                   // purpose -- 0 is what an untextured row already carries, so it stays put.
	uint pal_offset;   // word offset of this draw's palette in the frame palette stream
	uint epoch;        // page-table epoch this draw's byte reads go through
	uint date;         // destination-alpha test: 0 off, 1 = pass where alpha bit 7 clear (DATM 0), 2 = set (DATM 1)
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
	                   // single copied block, 3 = a 256-entry palette copied as ONE tile, pal_mul
	                   // and pal_shift wide. Every copied mode needs the CSM1 entry order applied at
	                   // fetch, because an image-to-buffer copy lands texels row-major.
	uint pal_bias;     // entry bias inside a copied palette: a four-bit draw reading slot k of a
	                   // 256-entry gathered load wants its entries 16k..16k+15.
	uint blend;        // the GS blend equation for a draw that reads its own destination, packed as
	                   // COEFFICIENTS, not the register's selectors (gsTileGpuPackBlend is the one
	                   // packer): bits 0-1 = coefficient of Cs in (A - B) biased +1, 2-3 = coefficient
	                   // of Cd biased +1, bit 4 = D is Cs, bit 5 = D is Cd, 6-7 = C (0 As, 1 Ad,
	                   // 2 constant), 8-15 = the constant, 16 = blend at all, 17 = COLCLAMP wraps,
	                   // 18 = PABE, 20 = quantise to the 16-bit frame's bits, 21 = AFAIL keeps the
	                   // destination alpha. A PSMCT24 destination's C=Ad arrives as the constant 0x80
	                   // (exactly 1.0); the enable bit is zero on every draw the executor blends for.
	uint fbmsk;        // FBMSK reduced to the frame format's stored bits, as a per-channel KEEP mask
	                   // on the expanded RGBA8 bytes.
	uint pal_mul;      // the merged tile's row stride S, in the two pre-chewed forms the palette arm
	uint pal_shift;    // needs it in: S/8 - 1 (1 for a 16-wide square, 7 for a 64-wide page) and
	                   // log2(S) - 4 (0 and 2). One number in two fields because deriving either
	                   // from the other costs 37 SPIR-V words in a program with a unit to spare,
	                   // and these two words were the row's tail padding. The row now ends on a
	                   // real field: that is still safe because 36 words after two vec2s is 144
	                   // bytes, a multiple of both std430's 8-byte struct alignment and the C++
	                   // side's alignas(16), so there is no implicit tail pad to disagree about --
	                   // but the NEXT field added has to check that again rather than assume it.
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

// The GS scissor is not in this stage. It is a vkCmdSetScissor before each indirect call, which is
// what the silicon does: an integer rectangle test on rasterized pixels, with the primitive's
// interpolation left alone. The clip-plane road this file used to carry did the test geometrically
// -- it re-interpolated a cut primitive from its clipped vertices, so a fragment well inside the
// rectangle could take an attribute value a ULP off the unclipped plane's, differently per GPU
// vendor. See ExecuteTileGpuPassPlan in GSDeviceVK.cpp for the call-cutting the scissor now costs.

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
}

#endif

#ifdef FRAGMENT_SHADER

// TILEGPU_SCALARIZE_VECTOR_AND (injected by the device from the driver-bug database): the Mali
// proprietary compiler miscompiles a bitwise AND whose operands are VECTORS -- including the
// broadcast form, where one side is a scalar -- so every vector AND in this file goes through here.
// Under the gate it is done one component at a time; everywhere else it is the plain operator.
//
// Same shape and the same decision as the gpu_bitwise_and the classic renderer's tfx.glsl and
// convert.glsl route their vector ANDs through: both are fed from the one ScalarizeVectorBitwiseAnd
// answer the driver database gives for this device. It is spelled locally rather than shared for two
// reasons. This shader is compiled from its own injected define block and nothing else -- the size
// gate compiles it with exactly that block -- so a shared helper would mean the gate carrying a copy
// of the thing it is supposed to be measuring. And the ivec4 arm below has no counterpart in the
// shared set, which is Classic's; widening that set would recompile every classic pipeline for a
// need only this file has.
//
// Off -- which is every driver but one -- it is a bare macro and not a function that returns the
// same thing, for the reason the shared one is: an overload costs an OpFunctionCall per call site in
// the SPIR-V, and this fragment program is held to an instruction-size budget that structure nobody
// executes still counts against.
#ifndef TILEGPU_SCALARIZE_VECTOR_AND
#define TILEGPU_SCALARIZE_VECTOR_AND 0
#endif

#if TILEGPU_SCALARIZE_VECTOR_AND
uvec3 tilegpu_and(uvec3 a, uvec3 b)
{
	return uvec3(a.x & b.x, a.y & b.y, a.z & b.z);
}

ivec3 tilegpu_and(ivec3 a, ivec3 b)
{
	return ivec3(a.x & b.x, a.y & b.y, a.z & b.z);
}

ivec4 tilegpu_and(ivec4 a, ivec4 b)
{
	return ivec4(a.x & b.x, a.y & b.y, a.z & b.z, a.w & b.w);
}
#else
#define tilegpu_and(a, b) ((a) & (b))
#endif

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_st;
layout(location = 2) in float v_q;
layout(location = 3) in vec2 v_uv;
layout(location = 4) flat in uint v_row;
layout(location = 5) in float v_fog;

// The colour that lands in the target: RAW GS values -- 0x80 stays 0x80; the target's bytes are
// guest bytes, so no display scaling happens here. The alpha channel is written unblended (the GS
// writes the fragment alpha as-is), except on the road below that borrows it.
layout(location = 0, index = 0) out vec4 o_color;

#if TILEGPU_DUAL_SRC
// ...and the dual-source blend factor: the fragment alpha in the GS's 0x80 = 1.0 convention
// (As * 255/128, clamped), which the fixed-function blend takes as SRC1 when the ALPHA register
// selects As. Declared only where the device has dualSrcBlend -- a module that names an index-1
// output is a module a driver without the feature may refuse, so zeroing it would not have been
// enough. Where it is absent the epilogue at the end of main() carries the same factor instead.
layout(location = 0, index = 1) out vec4 o_blend;
#endif

// The pass's snapshot of its own colour target, taken before the pass opened: the destination
// alpha the DATE test reads. Bound per pass (set 1); a pass without DATE draws binds a null
// texture that no fragment reads.
layout(set = 1, binding = 0) uniform sampler2D u_snapshot;

#if TILEGPU_SELF_READ
// The pass's own colour attachment, read back at the fragment's own pixel in rasterization order.
// The pass declares it as an input attachment as well as a colour attachment, keeps it in GENERAL
// for the pass's whole life, and carries the rasterization-order subpass flag -- so this returns
// what every earlier fragment of this pass wrote to this pixel, with no barrier and no pass break.
// Declared only in a variant whose pass actually reads: a pass that does not is not built against
// that render pass and has no such descriptor bound.
layout(input_attachment_index = 0, set = 3, binding = 0) uniform subpassInput u_dest;

// The destination pixel as the guest's own bytes. A UNORM8 fetch hands back k/255 correctly rounded,
// which is not the same float as k * (1/255) -- so recover the integer and do the arithmetic there,
// exactly as the materialised-source road recovers a texel byte. Everything downstream of here is
// integer, because the GS blend is integer.
ivec4 tilegpu_dest_bytes()
{
	const vec4 raw = subpassLoad(u_dest);
	return ivec4(floor(fma(raw, vec4(255.0f), vec4(0.5f))));
}
#endif

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

#if TILEGPU_BYTE_W32 || TILEGPU_BYTE_IDX8
uint tile_b48(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B48_X0) ^ XB(x, 1u, TILE_SWZ_B48_X1) ^ XB(x, 2u, TILE_SWZ_B48_X2)
	     ^ XB(y, 0u, TILE_SWZ_B48_Y0) ^ XB(y, 1u, TILE_SWZ_B48_Y1);
}
#endif

#if TILEGPU_BYTE_W32
uint tile_c32(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C32_X0) ^ XB(x, 1u, TILE_SWZ_C32_X1) ^ XB(x, 2u, TILE_SWZ_C32_X2)
	     ^ XB(y, 0u, TILE_SWZ_C32_Y0) ^ XB(y, 1u, TILE_SWZ_C32_Y1) ^ XB(y, 2u, TILE_SWZ_C32_Y2);
}

// The CT32/CT24 texel at integer coords (u, v) of the texture at tbp0/tbw, as a raw RGBA8888 word.
// A CT32 page is 64x32 texels, 8x8 blocks, 8x8-texel columns; block b of guest memory occupies words
// [b*64, b*64+64), so the absolute block times 64 plus the word-in-block is the linear word address.
//
// `zxor` turns the same arithmetic into PSMZ32/PSMZ24's: the depth pair is the colour pair's tables
// under one constant XOR on the ABSOLUTE block, which is where GSOffset::bn() applies it -- and where
// it has to stay, because distributing it into the in-page index would be right only for a
// page-aligned TBP0 and a texture window's often is not. Zero on every colour read, so the depth road
// costs the caller a select the compiler hoists out of the four bilinear taps.
//
// & 16383 is bn()'s own `% GS_MAX_BLOCKS`, a power of two, and it matters for the same reason it does
// on the 16-bit arm: a window based near the top of memory wraps, and an unwrapped block would send
// tilegpu_ring_word past the end of the page table into the entries and the palettes -- live data in
// the same buffer -- and hand back a plausible slot.
uint tilegpu_texel32(uint u, uint v, uint tbp0, uint tbw, uint epoch, uint zxor)
{
	uint page = (v >> 5u) * tbw + (u >> 6u);
	uint blk = ((tbp0 + page * 32u + tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u)) ^ zxor) & 16383u;
	uint word_in_block = tile_c32(u & 7u, v & 7u);
	return vram_words[tilegpu_ring_word(blk * 64u + word_in_block, epoch)];
}
#endif // TILEGPU_BYTE_W32

// The extra swizzle forms the paletted index reads need, copied from tfx.glsl: the 4-bit block
// form and the 8-/4-bit column forms (the 32-bit block form tile_b48 is shared with CT32). The
// 4-bit block form is also the 16-bit families' unstrided one -- blockTable16 IS blockTable4 --
// so it survives into a module that carries only the 16-bit arm.
#if TILEGPU_BYTE_IDX4 || TILEGPU_BYTE_D16
uint tile_b84(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
}
#endif

#if TILEGPU_BYTE_IDX8
uint tile_c8(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C8_X0) ^ XB(x, 1u, TILE_SWZ_C8_X1) ^ XB(x, 2u, TILE_SWZ_C8_X2) ^ XB(x, 3u, TILE_SWZ_C8_X3)
	     ^ XB(y, 0u, TILE_SWZ_C8_Y0) ^ XB(y, 1u, TILE_SWZ_C8_Y1) ^ XB(y, 2u, TILE_SWZ_C8_Y2) ^ XB(y, 3u, TILE_SWZ_C8_Y3);
}
#endif

#if TILEGPU_BYTE_IDX4
uint tile_c4(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C4_X0) ^ XB(x, 1u, TILE_SWZ_C4_X1) ^ XB(x, 2u, TILE_SWZ_C4_X2) ^ XB(x, 3u, TILE_SWZ_C4_X3) ^ XB(x, 4u, TILE_SWZ_C4_X4)
	     ^ XB(y, 0u, TILE_SWZ_C4_Y0) ^ XB(y, 1u, TILE_SWZ_C4_Y1) ^ XB(y, 2u, TILE_SWZ_C4_Y2) ^ XB(y, 3u, TILE_SWZ_C4_Y3);
}
#endif

// Extract byte (sel & 3) of an SSBO-loaded word. TILEGPU_STATIC_BYTE_SEL (injected by the
// device from the driver id) selects a branchy form with constant shift amounts, because
// Honeykrisp (Mesa 25.3.6, Apple M2 Max) miscompiles the dynamic form: the load returns
// the right word, but the byte is selected by the low bits of the word INDEX instead of
// sel, so every word whose ring index is not a multiple of 4 reads 0 -- a 2x2 texel
// lattice over every paletted texture. A second, unrelated use of the sel sub-expression
// also hides it, so it is an optimiser defect, not shader semantics. Every other driver
// keeps the straight-line shift.
#if TILEGPU_BYTE_IDX8 || TILEGPU_BYTE_IDX4
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
#endif // TILEGPU_BYTE_IDX8 || TILEGPU_BYTE_IDX4

#if TILEGPU_BYTE_IDX8
// The PSMT8 index byte at texel (u, v): 128x64 page, 16x16-texel columns; block b occupies bytes
// [b*256, b*256+256) = words [b*64, b*64+64). byte_in_block picks the byte within its word.
uint tilegpu_index8(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 6u) * tbw + (u >> 7u);
	// & 16383 is bn()'s own `% GS_MAX_BLOCKS`, for the reason spelled out on the 32-bit arm.
	uint blk = (tbp0 + page * 32u + tile_b48((u >> 4u) & 7u, (v >> 4u) & 3u)) & 16383u;
	uint byte_in_block = tile_c8(u & 15u, v & 15u);
	uint word = vram_words[tilegpu_ring_word(blk * 64u + (byte_in_block >> 2u), epoch)];
	return tilegpu_byte_sel(word, byte_in_block);
}
#endif // TILEGPU_BYTE_IDX8

#if TILEGPU_BYTE_IDX4
// The PSMT4 index nibble at texel (u, v): 128x128 page, 32x16-texel columns. col4 gives the
// nibble index within the 512-nibble (256-byte) block; the low bit selects the nibble in its byte.
uint tilegpu_index4(uint u, uint v, uint tbp0, uint tbw, uint epoch)
{
	uint page = (v >> 7u) * tbw + (u >> 7u);
	// & 16383 is bn()'s own `% GS_MAX_BLOCKS`, for the reason spelled out on the 32-bit arm.
	uint blk = (tbp0 + page * 32u + tile_b84((u >> 5u) & 3u, (v >> 4u) & 7u)) & 16383u;
	uint nib = tile_c4(u & 31u, v & 15u);
	uint byte_in_block = nib >> 1u;
	uint word = vram_words[tilegpu_ring_word(blk * 64u + (byte_in_block >> 2u), epoch)];
	uint byteval = tilegpu_byte_sel(word, byte_in_block);
	return ((nib & 1u) != 0u) ? (byteval >> 4u) : (byteval & 0xFu);
}
#endif // TILEGPU_BYTE_IDX4

#if TILEGPU_BYTE_IDXHI
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
	// No block XOR: the three alpha-byte views live in the CT32 swizzle universe and there is no
	// depth spelling of any of them.
	uint b = bitfieldExtract(tilegpu_texel32(u, v, tbp0, tbw, epoch, 0u), 24, 8);
	if (fmt == 3u)
		return b;
	return (fmt == 4u) ? (b & 0xFu) : (b >> 4u);
}
#endif // TILEGPU_BYTE_IDXHI

#if TILEGPU_BYTE_PALGATHER
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
#endif // TILEGPU_BYTE_PALGATHER

#if TILEGPU_BYTE_PAL
// The palette word for `index`, whichever road put the words in the stream. pal_mode is per draw and
// dynamically uniform, so this does not diverge; mode 0 is byte-identical to what the palette fetch
// was before the gather existed -- and in a module without the gather arm it is all that is left,
// which is the same statement one preprocessor level up.
uint tilegpu_palette_word(StateRow sr, uint index)
{
#if TILEGPU_BYTE_PALGATHER
	if (sr.pal_mode != 0u)
	{
		const uint e = sr.pal_bias + index;
#if TILEGPU_CLUT_MERGE
		// Mode 3, the MERGED copy: the palette's four blocks came out of the owner as ONE region, so
		// the words are a tile `S` wide and the word's block and its position inside that block
		// compose as texel coordinates instead of concatenating. Block b sits at square-relative
		// texel (8*(b&1), 8*((b>>1)&1)) -- x bit 0 is block bit 0, y bit 0 is block bit 1 -- so with
		// b = w >> 6 and (cx, cy) = (c & 7, c >> 3):
		//
		//     off = (((b & 2) << 2) + (c >> 3)) * S + ((b & 1) << 3) + (c & 7)
		//
		// which is GSTileSwizzleForms::ClutEntryToMergedOffset, spelled there and pinned against
		// GSClut's own loader at both strides. Only a 256-entry palette ever merges, so this arm
		// reads the eight-bit entry form and nothing else.
		//
		// The lines below are that expression with the multiply folded into bit positions, because
		// every one of its pieces lands in a DISJOINT bit range: b1 at bit 3 + log2(S), cy at
		// log2(S)..log2(S)+2, b0 at bit 3, cx at bits 0-2. So the adds are ORs, and
		// `c + (S/8 - 1) * (c & 0x38)` carries cy and cx together -- ((cy << 3) | cx) plus
		// (S/8 - 1) copies of (cy << 3) is (cy * S) | cx.
		//
		// ⚠️ Behind a MODULE define, not just behind its per-draw mode, and that is the whole reason
		// the lever has one. DEAD CODE COUNTS on this hardware -- the size gate exists because four
		// units of never-executed arithmetic once moved Shadow of the Colossus from 21.8 to 30.7 ms
		// with byte-identical frames -- and a lever that ships OFF must not enlarge the program every
		// device runs. With TILEGPU_CLUT_MERGE 0 this function is the one that shipped before the
		// merge existed, character for character. The same budget is why the spelling above is worth
		// the paragraph it takes: in the widest paletted variant three operations of it are a whole
		// unit of Adreno 650 instruction length.
		//
		// Mode 3 is always a 256-entry palette, so the eight-bit entry form serves modes 1 AND 3 and
		// only mode 2 is the four-bit one -- which is why the select below reads the other way round
		// from the one in the #else. The two arms SHARE w and its column split: computing them twice
		// costs 2,183 words here, six times what the merged offset itself does, because the entry
		// form is fourteen XOR terms and it is inlined at every texel of a bilinear sample. One load
		// rather than one per arm is worth another 120 for the same reason.
		const uint w = (sr.pal_mode == 2u) ? tile_clut4_word(e) : tile_clut8_word(e);
		const uint c = tile_ic32(w & 63u);
		uint off = (w >> 6u) * 64u + c;
		if (sr.pal_mode == 3u)
#if TILEGPU_CLUT_MERGE_PAGES
			// S is per draw here: 16 for a palette copied as its own square, 64 for one read out of
			// a whole copied 64x32 owner page, where pal_offset carries its square's origin inside
			// that page. The stride arrives pre-chewed in two fields -- see the state row -- because
			// deriving one from the other costs 37 words and this program has a unit to spare.
			off = ((w & 0x80u) << sr.pal_shift) | (c + sr.pal_mul * (c & 0x38u)) | ((w & 0x40u) >> 3u);
#else
			// S is 16: without the page merge a palette merges into its own 16x16 square and into
			// nothing else, so the stride is a constant and the two state words go unread. That is
			// worth 72 SPIR-V words, which is why the page merge has its own define rather than
			// riding this one.
			off = (w & 0x80u) | (c + (c & 0x38u)) | ((w & 0x40u) >> 3u);
#endif
		return vram_words[pal_base + sr.pal_offset + off];
#else
		const uint w = (sr.pal_mode == 1u) ? tile_clut8_word(e) : tile_clut4_word(e);
		return vram_words[pal_base + sr.pal_offset + (w >> 6u) * 64u + tile_ic32(w & 63u)];
#endif
	}
#endif
	return vram_words[pal_base + sr.pal_offset + index];
}
#endif // TILEGPU_BYTE_PAL

#if TILEGPU_BYTE_D16
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
	             : ((TG_TEXA_AEM(sr) && c == 0u) ? 0u : ((sr.texa >> 8u) & 0xFFu));
	return tilegpu_norm8(vec4(float((c & 0x001Fu) << 3u), float((c & 0x03E0u) >> 2u),
		float((c & 0x7C00u) >> 7u), float(a)));
}
#endif // TILEGPU_BYTE_D16

#if TILEGPU_BYTE_UNPACK
// Unpack a raw RGBA8888 word (a CT32 texel or an expanded CLUT entry) to normalised RGBA.
vec4 tilegpu_unpack(uint w)
{
	return tilegpu_norm8(vec4(float(w & 0xFFu), float((w >> 8u) & 0xFFu), float((w >> 16u) & 0xFFu),
		float((w >> 24u) & 0xFFu)));
}
#endif

#endif // TILEGPU_TAP_BYTE

#if TILEGPU_TAP_ANY

// TEXA: a 24-bit texel stores no alpha byte, so the GS gives it TEXA.TA0, and AEM makes a texel
// whose RGB is entirely zero transparent instead. (A paletted texture takes TEXA in the CLUT
// expansion on the CPU, so only the direct roads need this; rule 3's image has it baked in at build
// time.) `rgb_zero` is the AEM test, which the two roads phrase differently -- a word compare on the
// byte road, a channel compare on the image.
vec4 tilegpu_texa(StateRow sr, vec4 t, bool rgb_zero)
{
	if (TG_TEXA_APPLY(sr))
	{
		const bool aem_zero = TG_TEXA_AEM(sr) && rgb_zero;
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
	const uint iu = uint(tilegpu_wrap(cu, sr.tw, TG_WMS(sr), sr.region_u));
	const uint iv = uint(tilegpu_wrap(cv, sr.th, TG_WMT(sr), sr.region_v));

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

#if TILEGPU_BYTE_D16
	// The 16-bit families first, because their alpha rule is their own: a 16-bit texel has an
	// alpha BIT, so TEXA selects between TA1 and TA0 rather than always supplying TA0, and the
	// AEM test is on the whole cell rather than on an RGB word. tilegpu_texa below cannot express
	// that, so this arm returns rather than falling through to it.
	if (sr.index_format >= 6u)
		return tilegpu_texel16(sr, iu, iv, sr.index_format - 6u);
#endif

#if TILEGPU_BYTE_UNPACK
	// index_format is dynamically uniform per draw, so this does not diverge. Each arm is here only
	// if this pass's draws take it (TILEGPU_FMT_*, the header's note): an arm nobody takes is not in
	// the module, and when the mask names one arm all that is left of the switch is its own guard.
	//
	// ⚠️ Each guard tests its OWN range rather than leaning on the arms above it having returned.
	// That costs one compare in the alpha-byte arm and buys the property the whole variant scheme
	// rests on: removing an arm removes exactly that arm. A `>= 3u` that was only correct because
	// the 16-bit arm returned first would silently decode a 16-bit texture as an alpha-byte view in
	// the module that leaves the 16-bit arm out.
	uint w = 0u;
#if TILEGPU_BYTE_D32
	// TWO values land on this arm: 0 is the CT32 pair (PSMCT32 / PSMCT24) and 10 the DEPTH pair
	// (PSMZ32 / PSMZ24), which is the same address geometry under one constant block XOR -- exactly
	// the relation 8 and 9 have to 6 and 7 on the 16-bit arm. The 24-bit member of either pair is
	// told from the 32-bit one by TEXA below, not here, because that is a fact about the word's top
	// byte and not about where the word is.
	if (sr.index_format == 0u || sr.index_format == 10u)
		w = tilegpu_texel32(iu, iv, sr.tbp0, sr.tbw, sr.epoch,
			(sr.index_format != 0u) ? TILE_SWZ_Z32XOR : 0u);
#endif
#if TILEGPU_BYTE_IDX8
	if (sr.index_format == 1u)
		w = tilegpu_palette_word(sr, tilegpu_index8(iu, iv, sr.tbp0, sr.tbw, sr.epoch));
#endif
#if TILEGPU_BYTE_IDX4
	if (sr.index_format == 2u)
		w = tilegpu_palette_word(sr, tilegpu_index4(iu, iv, sr.tbp0, sr.tbw, sr.epoch));
#endif
#if TILEGPU_BYTE_IDXHI
	if (sr.index_format >= 3u && sr.index_format < 6u)
		w = tilegpu_palette_word(sr, tilegpu_index_hi(iu, iv, sr.tbp0, sr.tbw, sr.epoch, sr.index_format));
#endif

	return tilegpu_texa(sr, tilegpu_unpack(w), (w & 0x00FFFFFFu) == 0u);
#else
	// Nothing in this module produces a 32-bit word: the pass reads only resident targets, or only
	// 16-bit textures off the byte road, so every fragment returned above. GLSL still needs the
	// fall-through to return something.
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
	if (TG_LTF(sr) == 0u)
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
		const int iu = tilegpu_wrap(int(floor(uv.x)), sr.tw, TG_WMS(sr), sr.region_u);
		const int iv = tilegpu_wrap(int(floor(uv.y)), sr.th, TG_WMT(sr), sr.region_v);
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
	if (TG_LTF(sr) == 0u)
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

	// Destination alpha test: the GS passes a pixel when the destination alpha's bit 7 equals DATM.
	if (TG_DATE(sr) != 0u)
	{
#if TILEGPU_SELF_DATE
		// The live pixel, in rasterization order: exact by construction, including against what this
		// same pass has already written under it. The pass therefore takes no snapshot copy and the
		// planner breaks it for nothing.
		const bool msb = tilegpu_dest_bytes().a >= 128;
#else
		// The pass snapshot (pre-pass bytes). Exact only because the planner opens a new pass
		// whenever a DATE draw's rect meets what the open pass already wrote.
		const float da = texelFetch(u_snapshot, ivec2(gl_FragCoord.xy), 0).a;
		const bool msb = da >= (128.0f / 255.0f);
#endif
		if (msb != (TG_DATE(sr) == 2u))
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
	if (TG_TEX(sr) != 0u)
	{
		// The texel coordinate: FST is a direct texel (12.4 already unpacked in the VS); STQ divides
		// the interpolated S,T by the interpolated Q and scales by the texture dimensions. The affine
		// interpolation (gl_Position.w == 1) plus this per-pixel divide reproduces the PS2's own
		// affine-rasteriser-with-per-pixel-divide texturing.
		const vec2 uv = (TG_FST(sr) != 0u) ? v_uv : (vec2(v_st.x, v_st.y) / v_q) * vec2(float(sr.tw), float(sr.th));
		const vec4 ct = tilegpu_sample(sr, uv);

		const float k = 255.0f / 128.0f;
		if (TG_TFX(sr) == 1u) // DECAL
		{
			cv.rgb = ct.rgb;
			cv.a = (TG_TCC(sr) != 0u) ? ct.a : cf.a;
		}
		else if (TG_TFX(sr) == 0u) // MODULATE
		{
			cv.rgb = min(ct.rgb * cf.rgb * k, vec3(1.0f));
			cv.a = (TG_TCC(sr) != 0u) ? min(ct.a * cf.a * k, 1.0f) : cf.a;
		}
		else // HIGHLIGHT (2) / HIGHLIGHT2 (3)
		{
			cv.rgb = min(ct.rgb * cf.rgb * k + vec3(cf.a), vec3(1.0f));
			cv.a = (TG_TCC(sr) != 0u) ? ((TG_TFX(sr) == 2u) ? min(ct.a + cf.a, 1.0f) : ct.a) : cf.a;
		}
	}
#endif

	// Set by the alpha test below for a fragment that FAILS on a draw whose AFAIL keeps its colour
	// alive; read by the byte tail, which is where the per-fragment alpha keep happens.
	bool afail_keep_alpha = false;
	// The alpha test. The GS compares the fragment's alpha byte -- after the texture function, so
	// this must sit below it -- against AREF, and AFAIL says what a failing fragment still writes.
	// Here a failure always discards, which is exact for AFAIL=KEEP and an approximation for the
	// three modes that keep writing something (the renderer only asks for a test when the mode
	// changes what lands, and folds the ATST=NEVER cases into the write flags instead).
	if (TG_ATST_ANY(sr))
	{
		// The alpha byte the test sees is the one the target would store, so it rounds -- the same
		// value the UNORM write produces. Truncating instead costs a level wherever the texture
		// function lands a hair under an integer, which an ATST=EQUAL draw reads as "no pixel at
		// all": Shadow of the Colossus tests EQUAL against 255 for one of its two sky layers.
		precise float av = fma(cv.a, 255.0f, 0.5f);
		const uint a = uint(av);
		bool pass;
		switch (TG_ATST_OP(sr) - 1u)
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
		{
#if TILEGPU_SELF_MASK
			// AFAIL=RGB_ONLY on a draw that writes no depth: the console lands this fragment's RGB and
			// keeps the destination's alpha. Discarding gets the alpha and the depth right by accident
			// and loses the colour, so the fragment stays alive and the byte tail below takes its alpha
			// from the destination instead.
			if ((sr.blend & 0x00200000u) != 0u)
				afail_keep_alpha = true;
			else
				discard;
#else
			discard;
#endif
		}
	}

	// Fog. The console rule (gs-interp capture, SCPH-30001, and the Tile floor's walk) is
	// (C*F + Cfog*(256 - F)) >> 8 on RGB with alpha passed through untouched, written here the way
	// the floor writes it: Cfog plus the difference scaled by F at fifteen fractional bits, all in
	// integer arithmetic, because a float mix rounds where the hardware truncates. Our colour is in
	// normalised guest units, so it scales to 0..255 for the walk and back afterwards.
	if (TG_FGE(sr) != 0u)
	{
		const ivec3 cfog = ivec3(tilegpu_and(uvec3(sr.fogcol, sr.fogcol >> 8u, sr.fogcol >> 16u), uvec3(0xFFu)));
		const int f15 = int(v_fog * (255.0f * 128.0f));
		cv.rgb = vec3(cfog + (((ivec3(cv.rgb * 255.0f) - cfog) * f15) >> 15)) * (1.0f / 255.0f);
	}

	// The GS's As blend factor: the fragment alpha read in the 0x80 = 1.0 convention. Computed here,
	// off the fragment's own alpha and above the byte tail, because that is the alpha the console's
	// blend unit takes -- the tail's write-mask merge happens at the write, after the blend.
	const float as_factor = min(cv.a * (255.0f / 128.0f), 1.0f);

	o_color = cv;
#if TILEGPU_DUAL_SRC
	o_blend = vec4(as_factor);
#endif

#if TILEGPU_BYTE_TAIL
	// The fragment stage's integer tail: the blend the executor's fixed-function state cannot
	// express, the write mask it cannot express, and the precision a 16-bit frame does not keep.
	// It runs LAST, on the finished fragment colour, because that is where the console puts all
	// three: the texture function, the alpha test and the fog walk happen before the blend unit,
	// and the frame format's own quantisation happens at the write after it.
	//
	// A pass compiles the arms its draws need and carries the draws that need none: every branch
	// here is per DRAW, out of its state row. The row says what this stage must DO rather than what
	// the registers said -- the enable bit is set only for a draw whose blend the shader owns, the
	// keep mask is non-zero only for one whose write mask it owns, and the quantise bit only for one
	// whose output is what lands -- so the gate is three tests on one word already loaded.
	if ((sr.blend & 0x00110000u) != 0u || sr.fbmsk != 0u || afail_keep_alpha)
	{
		// The bytes the target would have stored for this fragment. Same rounding as the alpha test's,
		// which is the rounding a UNORM8 write performs -- so the tail's arithmetic is done on the
		// value that would otherwise have been written, not on a neighbour of it.
		ivec4 outc = ivec4(floor(fma(cv, vec4(255.0f), vec4(0.5f))));
#if TILEGPU_SELF_BLEND || TILEGPU_SELF_MASK
		const ivec4 dst = tilegpu_dest_bytes();
		const ivec4 src = outc;
#endif

#if TILEGPU_SELF_BLEND
		// Cv = ((A - B) * C) >> 7 + D per channel, in integer, with C a 0..255 byte in which 0x80 is
		// 1.0 -- the reason a fixed-function factor cannot express it, since 0xFF reaches 1.99. The
		// shift is arithmetic, matching the software renderer's signed high multiply. Alpha is never
		// blended: the console writes the fragment's own alpha byte.
		//
		// The row carries the COEFFICIENTS the register's selectors add up to, not the selectors, so
		// there is not a select chain in sight: A - B is linear in Cs and Cd with coefficients in
		// {-1, 0, +1}, and D is one of the two or zero. Spelt as selectors this arm cost 828 SPIR-V
		// words against 556 of headroom, which on the Adreno 650 is the difference between having
		// this road and not. gsTileGpuPackBlend builds it; the unit suite holds it to the equation
		// and the equation to the software renderer's own scanline.
		if ((sr.blend & 0x00010000u) != 0u)
		{
			// Four coefficients out of one vector shift: ka and kb are the {-1, 0, +1} the register's
			// A and B add up to, ds and dd pick D. No selector chain anywhere.
			const ivec4 k = tilegpu_and(ivec4(sr.blend) >> ivec4(0, 2, 4, 5), ivec4(3, 3, 1, 1));
			const uint c_mode = (sr.blend >> 6u) & 3u;
			const int C = (c_mode == 0u) ? src.a : ((c_mode == 1u) ? dst.a : int((sr.blend >> 8u) & 0xFFu));
			ivec3 v = ((((k.x - 1) * src.rgb + (k.y - 1) * dst.rgb) * C) >> 7) + k.z * src.rgb + k.w * dst.rgb;
			// COLCLAMP, as one clamp rather than two paths: clamping to 0..255 makes the mask below a
			// no-op, and widening the clamp past the equation's own range makes the mask the wrap.
			const int lo = ((sr.blend & 0x00020000u) != 0u) ? -1024 : 0;
			v = tilegpu_and(clamp(v, ivec3(lo), ivec3(255 - lo)), ivec3(0xFF));
			// PABE gates the whole blend on the SOURCE alpha's bit 7, per pixel. The alpha byte is
			// the fragment's either way, so only RGB is taken back.
			if ((sr.blend & 0x00040000u) != 0u && src.a < 128)
				v = src.rgb;
			outc.rgb = v;
		}
#endif

#if TILEGPU_QUANT16
		// What a 16-bit frame stores: five bits per colour channel and one of alpha, TRUNCATED --
		// the console drops the low bits rather than rounding to the nearest representable value.
		// After the blend and before the write mask, which is the console's own order, and which is
		// what makes the mask's low bits (the ones a 16-bit FBMSK cannot name) come out zero.
		if ((sr.blend & 0x00100000u) != 0u)
			outc = tilegpu_and(outc, ivec4(0xF8, 0xF8, 0xF8, 0x80));
#endif

#if TILEGPU_SELF_MASK
		// FBMSK at BIT granularity: keep the destination's bits where the register says so, take the
		// computed ones everywhere else. The channel-granular half of the same mask is already the
		// pipeline's colour write mask, and the two agree by construction -- a channel the pipeline
		// drops has every stored bit set here too.
		if (sr.fbmsk != 0u || afail_keep_alpha)
		{
			// One vector shift by a CONSTANT vector, not four scalar extracts: the amounts are
			// literals, so this is not the computed sub-word shift the Honeykrisp workaround exists
			// for, and it is a good deal smaller.
			ivec4 keep = tilegpu_and(ivec4(sr.fbmsk) >> ivec4(0, 8, 16, 24), ivec4(0xFF));
			// ...and the per-FRAGMENT half: a failing fragment under AFAIL=RGB_ONLY keeps the whole
			// destination alpha byte, which is the same operation over a different mask.
			if (afail_keep_alpha)
				keep.a = 0xFF;
			outc = tilegpu_and(outc, ~keep) | tilegpu_and(dst, keep);
		}
#endif

		o_color = vec4(outc) * (1.0f / 255.0f);
	}
#endif

#if !TILEGPU_DUAL_SRC
	// The As factor with no second output to put it in: it goes in the alpha channel, where the
	// pipeline reads it as SRC_ALPHA. Below the byte tail, on the FINISHED colour, because that is
	// where the blend unit would have taken the factor from.
	//
	// The alpha this displaces is the plan's problem, not the shader's, and it has three answers:
	// the pipeline masks the channel off, the alpha blend equation multiplies the carrier back down
	// by 128/255, or a companion draw over the same geometry writes the byte with this bit clear.
	// GSDevice::gsTileGpuDualSrcRoad picks one for every draw that sets this.
	if ((sr.blend & 0x00400000u) != 0u)
		o_color.a = as_factor;
#endif
}

#endif
