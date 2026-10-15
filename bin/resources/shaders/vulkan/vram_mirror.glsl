#version 450

// GPU copy of GS local memory: texture decode and block scatter.
//
// One source, two compute entry points. The host (GSVramMirrorVK) compiles it twice, with
// VRAM_MIRROR_DECODE or VRAM_MIRROR_SCATTER defined on the line after #version, and replaces the
// #include line below with the contents of vram_mirror_swizzle.glsl (GLSL has no include without an
// extension, and the shader cache keys on the final text).
//
// DECODE   Turns paletted texture windows (PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH) into one-byte-per-
//          texel index rows in a linear scratch buffer. The CPU computes every block number
//          (GSVramMirrorLayout.h, the same walk ReadTexture does), so TBW, page tables and the wrap at
//          16384 blocks are not repeated here; the shader only swizzles INSIDE a block.
//          One invocation is one 64-byte column of one block: it loads four uvec4, extracts the
//          texels with the constant shifts of vram_mirror_swizzle.glsl, and stores whole rows.
// SCATTER  Copies the blocks a batch staged into the mirror, after the decode has read the mirror.
//
// Buffers (set 0), all storage buffers:
//   0  mirror   4 MiB, 16384 blocks of 256 bytes, device local
//   1  staging  blocks the CPU copied at miss time (slot = 256-byte index), host coherent
//   2  meta     block tables, job records, workgroup list, scatter pairs, host coherent
//   3  scratch  decoded rows, written as uvec4 (PSMT8, PSMT4)
//   4  scratch  the same buffer as uvec2, for the 8-texel blocks of the H formats. Two adjacent blocks
//               share each 16-byte scratch word, so a uvec4 store there would race between invocations.
//               There is no 8-bit or 16-bit storage on malisx2, so uvec2 is the smallest safe store.
//
// A block-table entry is a block index, with bit 31 set when it names a staging slot. Both are masked
// to their buffer here as well as on the CPU: a bad entry must not read outside the buffer.

#if defined(VRAM_MIRROR_DECODE)
#define MIRROR_ACCESS readonly
#else
#define MIRROR_ACCESS writeonly
#endif

layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) MIRROR_ACCESS buffer MirrorBuffer { uvec4 vm[]; };
layout(std430, set = 0, binding = 1) readonly buffer StagingBuffer { uvec4 stg[]; };
layout(std430, set = 0, binding = 2) readonly buffer MetaBuffer { uint meta[]; };
#if defined(VRAM_MIRROR_DECODE)
layout(std430, set = 0, binding = 3) writeonly buffer ScratchBuffer { uvec4 o[]; };
layout(std430, set = 0, binding = 4) writeonly buffer ScratchBufferH { uvec2 oh[]; };
#endif

layout(push_constant) uniform PushConstants
{
	uint jobs_word;  // DECODE: word offset of the job records. SCATTER: of the (block, slot) pairs.
	uint wg_word;    // DECODE: word offset of the workgroup -> job index list.
	uint stage_mask; // staging slots - 1 (a power of two)
	uint count;      // SCATTER: number of pairs
} pc;

#if defined(VRAM_MIRROR_DECODE)

#include "vram_mirror_swizzle.glsl"

// Job record, 8 words at pc.jobs_word + 8 * job:
//   0 format (0 PSMT8, 1 PSMT4, 2 PSMT8H, 3 PSMT4HL, 4 PSMT4HH)   1 word offset of the block table
//   2 blocks wide    3 blocks high    4 scratch byte offset of the first row    5 scratch row pitch, bytes
//   6 index of the job's first workgroup    7 units (4 per block)
void main()
{
	const uint wg = gl_WorkGroupID.x;
	const uint jb = pc.jobs_word + meta[pc.wg_word + wg] * 8u;
	const uint unit = (wg - meta[jb + 6u]) * 64u + gl_LocalInvocationID.x;
	if (unit >= meta[jb + 7u])
		return;

	const uint fmt = meta[jb];
	const uint bw = meta[jb + 2u];
	const uint out_off = meta[jb + 4u];
	const uint pitch = meta[jb + 5u];

	const uint b = unit >> 2;
	const uint col = unit & 3u;
	const uint by = b / bw;
	const uint bx = b - by * bw;

	const uint entry = meta[meta[jb + 1u] + b];
	uvec4 q0, q1, q2, q3;
	if ((entry & 0x80000000u) != 0u)
	{
		const uint base = (entry & pc.stage_mask) * 16u + col * 4u;
		q0 = stg[base];
		q1 = stg[base + 1u];
		q2 = stg[base + 2u];
		q3 = stg[base + 3u];
	}
	else
	{
		const uint base = (entry & 16383u) * 16u + col * 4u;
		q0 = vm[base];
		q1 = vm[base + 1u];
		q2 = vm[base + 2u];
		q3 = vm[base + 3u];
	}

	uint s[16];
	s[0] = q0.x; s[1] = q0.y; s[2] = q0.z; s[3] = q0.w;
	s[4] = q1.x; s[5] = q1.y; s[6] = q1.z; s[7] = q1.w;
	s[8] = q2.x; s[9] = q2.y; s[10] = q2.z; s[11] = q2.w;
	s[12] = q3.x; s[13] = q3.y; s[14] = q3.z; s[15] = q3.w;

	if (fmt == 0u)
	{
		// PSMT8: a block is 16x16 texels, a column 4 rows of 16 bytes.
		uvec4 row[4];
		swz_t8(col, s, row);
		const uint step4 = pitch >> 4;
		const uint at = (out_off + (by * 16u + col * 4u) * pitch + bx * 16u) >> 4;
		o[at] = row[0];
		o[at + step4] = row[1];
		o[at + 2u * step4] = row[2];
		o[at + 3u * step4] = row[3];
	}
	else if (fmt == 1u)
	{
		// PSMT4: a block is 32x16 texels, a column 4 rows of 32 bytes (two uvec4 each).
		uvec4 row[8];
		swz_t4(col, s, row);
		const uint step4 = pitch >> 4;
		const uint at = (out_off + (by * 16u + col * 4u) * pitch + bx * 32u) >> 4;
		o[at] = row[0];
		o[at + 1u] = row[1];
		o[at + step4] = row[2];
		o[at + step4 + 1u] = row[3];
		o[at + 2u * step4] = row[4];
		o[at + 2u * step4 + 1u] = row[5];
		o[at + 3u * step4] = row[6];
		o[at + 3u * step4 + 1u] = row[7];
	}
	else
	{
		// The H formats: a block is 8x8 texels, a column 2 rows of 8 bytes.
		uvec2 row[2];
		if (fmt == 2u)
			swz_t8h(col, s, row);
		else if (fmt == 3u)
			swz_t4hl(col, s, row);
		else
			swz_t4hh(col, s, row);
		const uint step2 = pitch >> 3;
		const uint at = (out_off + (by * 8u + col * 2u) * pitch + bx * 8u) >> 3;
		oh[at] = row[0];
		oh[at + step2] = row[1];
	}
}

#elif defined(VRAM_MIRROR_SCATTER)

// One block per 16 invocations, one uvec4 each, so a workgroup copies four whole blocks.
void main()
{
	const uint gid = gl_GlobalInvocationID.x;
	const uint entry = gid >> 4;
	if (entry >= pc.count)
		return;
	const uint lane = gid & 15u;
	const uint block = meta[pc.jobs_word + entry * 2u] & 16383u;
	const uint slot = meta[pc.jobs_word + entry * 2u + 1u] & pc.stage_mask;
	vm[block * 16u + lane] = stg[slot * 16u + lane];
}

#else
#error "define VRAM_MIRROR_DECODE or VRAM_MIRROR_SCATTER"
#endif
