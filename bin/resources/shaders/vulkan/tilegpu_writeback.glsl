// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// TileGpu writeback: target -> bytes. Reswizzle a resident colour target's finished pixels into the
// frame's ring, one guest page per workgroup column, at the exact words tilegpu.glsl's texel path
// reads for those texels -- so a later draw that samples pages this target holds newest finds this
// frame's render (or last frame's: targets persist) with no change on the read side. It is the
// inverse of tilegpu_unpack at the address of tilegpu_texel32; the swizzle forms are the same
// TILE_SWZ_* defines the sampler uses, so the two cannot disagree about a constant.
//
// Addressing goes through the frame's epoch page table exactly as the reader's does:
// table[table_base + epoch*512 + page] names the page's ring slot. The op's page list (staged
// beside the table) says which pages, and per page which of its 32 physical blocks the surface
// holds newest -- a page a CPU transfer has partially reclaimed keeps its CPU-newest blocks (the
// executor prefilled the slot with the CPU shadow's bytes; this pass writes only the surface's
// blocks over them). byte_mask does the same at byte granularity within a word: a PSMCT24 surface
// owns bytes 0-2 and leaves the alpha byte to whoever holds that plane.
//
// A page entry's KEEP MASK is the third filter, and the only one that varies within a block. A
// host->local transfer covering part of a block splits that block's bytes -- some are the CPU's new
// ones, sitting in the slot's prefill, and the rest are still this surface's -- and neither the
// block mask nor byte_mask can express that, one being per block and the other constant over the
// op. So the entry may name a 256-word table (32 blocks x 8 words, one BIT per byte of the block's
// 256 bytes) of bytes this pass must LEAVE ALONE. 0xFFFFFFFF means it names none, which is every
// entry every road but the upload merge emits.
//
// The target is a Format::Color (RGBA8) image whose pixel space is its guest layout: page-aligned
// base, width = bw * 64, page (base + row*bw + col) at pixel rect (col*64, row*pgh).
//
// TILEGPU_WB_FMT (injected by the device) picks the swizzle universe. Each value compiles its own
// path and nothing else, the way tilegpu_materialise.glsl does it -- a build is one pipeline per
// format, and a runtime branch on page geometry would be paid per invocation of every page:
//   0 = PSMCT32 / PSMCT24 -- 64x32-texel pages of 8x8 blocks, one 32-bit word per texel, so the
//       invocation grid IS the page and the guest word is the pixel.
//   1 = PSMCT16, 2 = PSMCT16S -- 64x64-texel pages of 16x8 blocks, two texels to a word. The two
//       differ only in the block table (blockTable16 is byte-identical to blockTable4, hence B84;
//       blockTable16S is its own form, B84S) and share columnTable16.
//   3 = PSMZ32 / PSMZ24 -- arm 0's page geometry, column table and pixel-is-a-word rule under one
//       constant XOR on the block. A game that sets FRAME.PSM to a depth format and renders colour
//       into it (Ace Combat 5's occlusion probe, Beyond Good & Evil's post-process chain) makes a
//       COLOUR surface whose bytes live at depth-swizzled addresses; this is the program that puts
//       them there. The XOR is under 32 by construction, so it lands on the IN-PAGE block index and
//       the page term is arm 0's unchanged -- which is why a Z32 surface claims exactly the pages
//       its colour twin would.
//
// ⚠️ The 16-bit arms write a WHOLE word from ONE invocation, covering the pair of texels eight
// apart that columnTable16 puts in its two halves. That is not packing for its own sake: the two
// texels sit in different 8x8 workgroups, so writing a halfword each would be two read-modify-write
// sequences on one word with no ordering between them. Pairing them makes the store a plain write
// and the whole pass race-free, exactly as the 32-bit arm already is. The pairing is unit-pinned
// (GSTileSwizzleForms::Locate16, TheSixteenBitWordPairsAreEightTexelsApart).
//
// THE STORE PATH. This pass is store-bound, not fetch-bound and not swizzle-bound: the 16-bit arm
// fetches twice the texels for the same 2048 words and runs 5% FASTER, so what the machine waits on
// is the scattered four-byte stores into the ring. Two things are done about that, and both are
// re-expressions -- the invocation-to-word map below is unchanged, and so is every byte written.
//
//   THE WAVE SHAPE. The workgroup is TILEGPU_WB_WG square (injected by the device, which answers
//   the shape per part) over the page's WORD grid, not 8x8 everywhere. A 64-invocation workgroup on
//   a part whose wave is 128 leaves half of every wave idle, which is Adreno's 16; on a 16-lane part
//   256 invocations is sixteen warps under one barrier and costs more than it saves, which is Mali's
//   8. The addressing here reads gl_GlobalInvocationID, so the partition into workgroups is the only
//   thing the answer moves -- every arm below is written for any dim the device may name.
//
//   THE LDS STAGE. A block's 64 words are 64 CONSECUTIVE ring words in a permuted order, so 64
//   invocations storing one word each is 64 transactions where four-word stores would be 16. Where
//   this pass writes WHOLE words -- byte_mask covers the word and the entry names no keep mask,
//   which is every road but PSMCT24 and the upload merge -- each invocation parks its word in
//   shared memory at its permuted index, and after one barrier a quarter of the lanes each issue
//   one aligned uvec4. The permutation happens in LDS and no inverse swizzle is needed. A ring slot
//   is page-aligned and a block is 64 words, so every four-word run inside it is 16-byte aligned by
//   construction.
//
//   The read-modify-write arm (PSMCT24's alpha byte, the upload merge's keep mask) keeps the
//   per-word path: it must leave bytes it does not own alone, which a uvec4 store cannot express.
//   The two are separated on a workgroup-UNIFORM test -- byte_mask is a push constant and the keep
//   mask rides on the entry, which is gl_WorkGroupID.z -- so the barrier below is never reached by
//   part of a workgroup.
//
// ⚠️ Not done, and deliberately: hoisting the four workgroup-uniform SSBO loads to one invocation
// behind a barrier. Measured on an Adreno 650 microbenchmark it costs 30% MORE than re-issuing them
// per invocation -- the redundant loads are cache-friendly and cost ~3%, and the sync stall is ten
// times what it saves. They stay per-invocation.

#ifndef TILEGPU_WB_FMT
#define TILEGPU_WB_FMT 0
#endif

// The workgroup's edge in WORDS of the page grid, injected by the device so the dispatch's group
// count and this cannot disagree. Must divide both page word extents (64x32 and 32x64) and be a
// multiple of 8, so that a workgroup covers whole 8x8-word blocks and never a fraction of one --
// the LDS stage below is per block. The device picks it per part (GSDeviceVK's
// TileGpuWritebackGroupDim); this default is only what a compile with nothing injected would take.
#ifndef TILEGPU_WB_WG
#define TILEGPU_WB_WG 8
#endif

// ...which makes the rest of the shape arithmetic, not a second constant to keep in step.
#define TILEGPU_WB_BLK_DIM (TILEGPU_WB_WG / 8)
#define TILEGPU_WB_BLOCKS (TILEGPU_WB_BLK_DIM * TILEGPU_WB_BLK_DIM)
// One lane per four-word store: 16 quads a block.
#define TILEGPU_WB_QUAD_LANES (TILEGPU_WB_BLOCKS * 16)

layout(local_size_x = TILEGPU_WB_WG, local_size_y = TILEGPU_WB_WG) in;

// The finished colour target, sampled point (texelFetch ignores filtering). Logical RGBA regardless
// of the native byte order, matching tilegpu_unpack's logical-RGBA read.
layout(set = 0, binding = 0) uniform sampler2D src;

// The frame's ring: page slots, epoch page tables and the op's page-entry list, one word array.
// Read for the table, the entries and the read-modify-write; written for the surface's bytes.
layout(std430, set = 0, binding = 1) buffer Vram
{
	uint vram_words[];
};

// The SAME buffer at the same offset, seen as quads -- the device binds one VkBuffer to both. GLSL
// gives no other way to spell a 16-byte store into an array declared as words, and declaring the
// ring as quads instead would make the read-modify-write arm's four-byte store unspellable. Only
// the whole-word arm stores through this view; nothing reads it.
layout(std430, set = 0, binding = 2) buffer VramQuads
{
	uvec4 vram_quads[];
};

layout(push_constant) uniform cb
{
	uint table_base;   // this frame's first page-table word in the ring
	uint epoch;        // which epoch's table names the destination slots
	uint bp;           // the surface's base in blocks (page-aligned)
	uint bw;           // the surface's stride in pages
	uint byte_mask;    // bytes of each word this surface owns (0xFFFFFFFF, or 0x00FFFFFF for CT24)
	uint entries_base; // ring word of the plan's page-entry array (3 words per entry, below)
	uint first_entry;  // this op's first entry index into that array
	uint entry_count;
	uint keep_base;    // ring word of the plan's keep-mask tables, indexed by the entry's third word
};

// One page entry is three words: {page | pad<<16}, block_mask, keep_mask_words.
#define TILEGPU_WB_ENTRY_WORDS 3u

// The swizzle forms, byte-identical to tilegpu.glsl's: block-in-page and unit-in-block column.
#define XB(v, b, m) ((0u - (((v) >> (b)) & 1u)) & (m))

// Arm 0 and arm 3 share this geometry; arm 3 adds the depth block XOR below.
#define TILEGPU_WB_CT32 (TILEGPU_WB_FMT == 0 || TILEGPU_WB_FMT == 3)

// The in-page block XOR that makes arm 3 the depth twin of arm 0. Zero everywhere else, so the
// expression below is one spelling for both arms and cannot drift between them.
#if TILEGPU_WB_FMT == 3
#define TILEGPU_WB_ZXOR TILE_SWZ_Z32XOR
#else
#define TILEGPU_WB_ZXOR 0u
#endif

#if TILEGPU_WB_CT32

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

// The 16-bit block form: B84 for PSMCT16 (blockTable16 == blockTable4, checked at fit time),
// B84S for PSMCT16S.
uint tile_b16(uint x, uint y)
{
#if TILEGPU_WB_FMT == 1
	return XB(x, 0u, TILE_SWZ_B84_X0) ^ XB(x, 1u, TILE_SWZ_B84_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84_Y0) ^ XB(y, 1u, TILE_SWZ_B84_Y1) ^ XB(y, 2u, TILE_SWZ_B84_Y2);
#else
	return XB(x, 0u, TILE_SWZ_B84S_X0) ^ XB(x, 1u, TILE_SWZ_B84S_X1)
	     ^ XB(y, 0u, TILE_SWZ_B84S_Y0) ^ XB(y, 1u, TILE_SWZ_B84S_Y1) ^ XB(y, 2u, TILE_SWZ_B84S_Y2);
#endif
}

// columnTable16: (x 0..15, y 0..7) -> the halfword within the block's 128. Its low bit is bit 3 of
// x, which is what puts texel x and texel x+8 in the two halves of one word.
uint tile_c16(uint x, uint y)
{
	return XB(x, 0u, TILE_SWZ_C16_X0) ^ XB(x, 1u, TILE_SWZ_C16_X1) ^ XB(x, 2u, TILE_SWZ_C16_X2) ^ XB(x, 3u, TILE_SWZ_C16_X3)
	     ^ XB(y, 0u, TILE_SWZ_C16_Y0) ^ XB(y, 1u, TILE_SWZ_C16_Y1) ^ XB(y, 2u, TILE_SWZ_C16_Y2);
}

// A finished pixel packed to A1B5G5R5: the top five bits of each colour byte and the MSB of alpha,
// which is GSLocalMemory::WriteFrame16's arithmetic and the pool's readback store's. Integer
// throughout after the one rounding multiply -- the same round-to-nearest the 32-bit arm uses, so
// a byte written by one arm and read by the other is the same byte.
uint tilegpu_pack5551(vec4 c)
{
	const uint r = uint(clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint g = uint(clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint b = uint(clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
	const uint a = uint(clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
	return (r >> 3u) | ((g >> 3u) << 5u) | ((b >> 3u) << 10u) | ((a >> 7u) << 15u);
}

#endif

// The workgroup's blocks, each staged at its PERMUTED word index so the gather below can read four
// consecutive ring words as four consecutive shared ones. Sized off the workgroup shape, so the
// device's answer for TILEGPU_WB_WG carries it -- 4 blocks and 1024 bytes at 16, 1 and 256 at 8.
shared uint tile_words[TILEGPU_WB_WG * TILEGPU_WB_WG];
// Each staged block's physical in-page index, or ~0 for a block this surface does not hold newest
// -- which is how the gather skips it without re-deriving the block mask.
shared uint tile_blocks[TILEGPU_WB_BLOCKS];

#define TILEGPU_WB_NO_BLOCK 0xFFFFFFFFu

void main()
{
	// One page per workgroup column: dispatch is (grid, entry_count) groups of TILEGPU_WB_WG
	// square, where the grid is the page's shape in WORDS -- 64x32 for a CT32 page, 32x64 for a
	// 16-bit one whose words each hold two texels. Either way 2048 invocations, one guest word
	// each, and the map from invocation to word is the global id, not the partition.
	const uint entry_index = gl_WorkGroupID.z;
	if (entry_index >= entry_count)
		return;
	const uint eword = entries_base + (first_entry + entry_index) * TILEGPU_WB_ENTRY_WORDS;
	const uint page = vram_words[eword] & 0xFFFFu;
	const uint block_mask = vram_words[eword + 1u];
	const uint keep_words = vram_words[eword + 2u];

	// The page's pixel rect in the target: pool row/column from its offset off the base page.
	const uint rel = (page + 512u - (bp >> 5u)) & 511u;
	const uint col = rel % bw;
	const uint row = rel / bw;

	// The page's ring slot. Read before the block test rather than after it, because the gather
	// lanes below need it whether or not their own block travels; it is one workgroup-uniform load
	// on a shader whose four of them measured at ~3% of its body.
	const uint slot = vram_words[table_base + epoch * 512u + page];

#if TILEGPU_WB_CT32
	const uint u = col * 64u + gl_GlobalInvocationID.x;
	const uint v = row * 32u + gl_GlobalInvocationID.y;

	// Block within the page; only the surface's blocks travel. The block_mask is over PHYSICAL
	// in-page blocks, so the XOR has to be applied before the mask test as well as before the word.
	const uint bib = tile_b48((u >> 3u) & 7u, (v >> 3u) & 3u) ^ TILEGPU_WB_ZXOR;
	// The exact ring word tilegpu_texel32(u, v, bp, bw, epoch) will read for this texel.
	const uint wib = tile_c32(u & 7u, v & 7u);
	const bool mine = (block_mask & (1u << bib)) != 0u;

	// Pack the finished pixel to a raw RGBA8888 word: the inverse of tilegpu_unpack, round to nearest.
	uint w = 0u;
	if (mine)
	{
		const vec4 c = texelFetch(src, ivec2(int(u), int(v)), 0);
		w = (uint(clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f))
		  | (uint(clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f) << 8u)
		  | (uint(clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f) << 16u)
		  | (uint(clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24u);
	}
#else
	// gx walks the 32 word-columns of a 16-bit page row, gy its 64 pixel rows. Word-column gx owns
	// the LOW half of one 16-texel group -- texel (gx>>3)*16 + (gx&7) -- and the high half is that
	// texel plus eight.
	const uint gx = gl_GlobalInvocationID.x;
	const uint gy = gl_GlobalInvocationID.y;
	const uint u = col * 64u + (gx >> 3u) * 16u + (gx & 7u);
	const uint v = row * 64u + gy;

	// Block within the page: 16x8-texel blocks, four across and eight down. u and u+8 share it,
	// because they differ only in bit 3.
	const uint bib = tile_b16((u >> 4u) & 3u, (v >> 3u) & 7u);
	// The word both halves live in: the low texel's halfword is even, the high texel's is that + 1.
	const uint wib = tile_c16(u & 15u, v & 7u) >> 1u;
	const bool mine = (block_mask & (1u << bib)) != 0u;

	uint w = 0u;
	if (mine)
	{
		w = tilegpu_pack5551(texelFetch(src, ivec2(int(u), int(v)), 0))
		  | (tilegpu_pack5551(texelFetch(src, ivec2(int(u + 8u), int(v)), 0)) << 16u);
	}
#endif

	// THE WHOLE-WORD ARM. Both tests are workgroup-uniform -- byte_mask is a push constant, and the
	// entry the keep mask rides on is this workgroup's z -- so every invocation of the workgroup
	// reaches the barrier or none of them does. A block's 64 words are covered by exactly the 64
	// invocations of one 8x8 sub-tile, each with a distinct wib, so the quads below write the same
	// 64 words with the same values the per-word store did.
	if (byte_mask == 0xFFFFFFFFu && keep_words == 0xFFFFFFFFu)
	{
		const uint lblk =
			(gl_LocalInvocationID.y >> 3u) * uint(TILEGPU_WB_BLK_DIM) + (gl_LocalInvocationID.x >> 3u);
		tile_words[lblk * 64u + wib] = w;
		// wib is a bijection over the sub-tile, so exactly one invocation of each block has wib 0.
		if (wib == 0u)
			tile_blocks[lblk] = mine ? bib : TILEGPU_WB_NO_BLOCK;

		barrier();

		if (gl_LocalInvocationIndex < uint(TILEGPU_WB_QUAD_LANES))
		{
			const uint b = gl_LocalInvocationIndex >> 4u;
			const uint quad = gl_LocalInvocationIndex & 15u;
			const uint gbib = tile_blocks[b];
			if (gbib != TILEGPU_WB_NO_BLOCK)
			{
				const uint base = b * 64u + quad * 4u;
				// slot is page-aligned and the block is 64 words, so this is 16-byte aligned.
				vram_quads[(slot + gbib * 64u + quad * 4u) >> 2u] = uvec4(
					tile_words[base], tile_words[base + 1u], tile_words[base + 2u], tile_words[base + 3u]);
			}
		}
		return;
	}

	// THE READ-MODIFY-WRITE ARM: PSMCT24's alpha byte and the upload merge's keep mask, per word.
	if (!mine)
		return;
	const uint rw = slot + bib * 64u + wib;

	// The bytes of this word that are actually ours to write: the surface's own byte window, minus
	// anything the entry's keep mask claims for the CPU. Word `wib` of the block owns nibble
	// (wib & 7) of keep word (wib >> 3), one bit per byte, low byte first -- which is the guest byte
	// order the slot's memcpy from the shadow put them in.
	uint bytes = byte_mask;
	if (keep_words != 0xFFFFFFFFu)
	{
		const uint nib = (vram_words[keep_base + keep_words + bib * 8u + (wib >> 3u)] >> ((wib & 7u) * 4u)) & 0xFu;
		bytes &= ~(((nib & 1u) != 0u ? 0x000000FFu : 0u) | ((nib & 2u) != 0u ? 0x0000FF00u : 0u) |
				   ((nib & 4u) != 0u ? 0x00FF0000u : 0u) | ((nib & 8u) != 0u ? 0xFF000000u : 0u));
	}

	if (bytes == 0xFFFFFFFFu)
		vram_words[rw] = w;
	else if (bytes != 0u)
		vram_words[rw] = (vram_words[rw] & ~bytes) | (w & bytes);
}
