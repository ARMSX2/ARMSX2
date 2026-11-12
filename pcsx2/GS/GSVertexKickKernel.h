// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSBackQueue.h"
#include "GS/GSUtil.h"
#include "GS/GSVertexKick.h"
#include "GS/Renderers/Common/GSVertex.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>

// The packed-vertex kick kernel for the fused GIF handlers.
//
// The per-vertex kick is a state machine: parse, store, mirror, decide, emit,
// accumulate, one vertex at a time, whether or not the vertex ends up in a drawn
// primitive. On the titles this was sized on, between half and four fifths of
// vertices do not: the game sets ADC on them, or the primitive they complete is
// culled. The kernel carries the three primitive types and the packed layouts
// that hold the bulk of every title's vertices, and takes a chunk of the register
// run in three passes:
//
//   decide     for every vertex, whether the primitive it completes is drawn,
//              from the packed position alone and four vertices at a time
//              (DecideVector), plus the union of the drawn primitives' boxes;
//   plan       over the drawn primitives only, with integer arithmetic alone:
//              the indices, the cursor exactly as the per-vertex kick moves it,
//              and the list of vertices that have to exist in the buffer and
//              where (ConvertJob);
//   convert    those vertices, each once, straight into its final slot, folding
//              the newly referenced ones into the fused vertex-trace bounds.
//
// Nothing here calls out to GSState. Anything that needs the rest of it -- the
// per-draw environment snapshot, a flush, a buffer growth, a draw-buffer switch
// -- is the caller's business: the handler runs the per-vertex kick for those
// vertices and re-enters, and the kernel holds nothing across such a seam.
//
// Exactness (the differential suite in tests/ctest/core/gs/gs_kick_kernel_tests.cpp
// pins all of it against the per-vertex kick): the conversion and the bounds are
// the same expressions on the same inputs; the decision is the scalar cull
// restated on the primitive's box (see DecideVector); the cursor arithmetic is
// the per-vertex kick's, applied once per drawn run instead of once per vertex.
// What the kernel does not reproduce is what nothing can read: the vertex buffer
// at or past the tail, and the two older entries of the four-deep cull rings.
namespace GSVertexKickKernel
{
	// Vertices per kernel entry: two 64-bit accept words, and at most that many
	// conversion steps on the stack.
	static constexpr u32 kChunkVertices = 128;

	// Below this many vertices in a handler call, the caller runs its own
	// per-vertex batch instead of entering the kernel. The kernel pays a fixed
	// cost per call -- pass one's setup, pass two's preheader, the ring writeback
	// and the two prologues -- that a short call cannot amortize, and the bypass
	// target is the arm that ran before the kernel existed, so it is exact for
	// free. Compile-time, not a settings key and not an env gate.
	//
	// The executed-path walker puts the break-even at 4.3 vertices on a triangle
	// strip: the kernel arm costs 314 instructions a call plus 49.25 a vertex
	// against the per-vertex batch's 111 plus 96. An M2 A/B at 0, 2, 4, 6 and 8
	// on the four titles that reach this arm (sotc, spiderman3, stuntman,
	// katamari) could not separate any two of them -- every threshold's three
	// reps overlap every other's -- so the value is the arithmetic's, rounded up
	// to cover the instantiations whose fixed cost is higher than the strip's.
	static constexpr u32 kMinKernelVertices = 6;

	// At or below this many vertices a chunk is decided one vertex at a time: the
	// four-wide pass costs about a hundred instructions to enter and leave and 85
	// a quad, against about 40 a vertex.
	static constexpr u32 kScalarDecideVertices = 5;

	// Batch invariants. Everything here is fixed for the whole of a kernel entry:
	// only a flush, a buffer switch or a register write can change any of them,
	// and none of those can happen inside the kernel.
	struct Invariants
	{
		GSVector4i xyof;                       // {ofx, ofy, ofx, ofy}
		GSVertexKernels::CullBounds bounds;    // banded at native res, raw 12.4 above it
		u64 uvfog;                             // XYZ2: {UV, FOG}; XYZF2: UV in the low word
		GSVector4i clamp_keep;                 // depth clamp, as two lane masks over m[1]:
		GSVector4i clamp_shifted;              //   m1' = (m1 & keep) | ((m1 >> 8) & shifted)
		// TME, FST and IIP in one word rather than three: they are read only on the
		// accept path, and pass two is tight enough on general-purpose registers
		// that three separate ones cost a spill.
		u32 shade;                             // bit 0 TME, bit 1 FST, bit 2 IIP
		bool clamp_enabled;
		bool sprite_q_fix;                     // sprite only: !PRIM.FST
		// Where the batch's last parsed vertex goes -- GSState::m_v, which the
		// piecemeal handlers and the next tag read. Call-invariant.
		GSVertex* last_out;
		// Appended, not interleaved: the two contiguous triple layouts never read
		// either of these, and keeping every field they DO read at the offset it
		// had is what makes their code identical to what it was.
		//
		// carry_m0 is what a layout that omits a register parses against, and it
		// is re-read at every kernel entry rather than hoisted per call -- the
		// same rule the cull bounds follow, for the same reason: a seam kick can
		// flush, and a flush can move what the rest of the run is decided against.
		GSVector4i carry_m0;
		GIFPackedLayout off;
		// The cull grid the accepted-prim bbox rounds onto. Appended for the same
		// reason carry_m0 is: the contiguous layouts never read it, and every field
		// they do read keeps the offset it had.
		GSVertexKernels::CullGrid grid;
		// Whether to accumulate the native-grid draw rect as well (draw buffering
		// on; see GSState::temp_native_draw_rect). A batch invariant, not a
		// per-prim decision: without draw buffering the heuristic that reads the
		// rect returns at its first line and the accumulation is dead work.
		bool track_native_rect;
		// Leave a zero or NaN-looking Q as the GIF wrote it (see MakePackedParseConsts).
		// Read once per chunk, when the parse constants are built.
		bool keep_gif_q;
	};

	// The buffer cursor is NOT marshalled. Every field of it -- head, tail, next,
	// xy_tail, the fused-min/max watermark and its valid flag, and the index
	// tail -- is a member of the two buffer objects the kernel is already handed,
	// so it reads them at entry and writes them at exit itself. Passing them in
	// and back out by value cost a 44-byte structure through memory each way plus
	// the caller's unpack, and bought nothing: the caller's own cursor is loaded
	// from and stored to exactly those members.
	//
	// The one thing that does come back is the deferred draw-rect accumulation,
	// which is not a buffer member: the union of the chunk's accepted prim rects,
	// returned in a vector register, plus a two-bit state saying whether it is
	// empty, unions into temp_draw_rect, or replaces it. When
	// Invariants::track_native_rect is set a second union comes back the same way,
	// the same rects asked on the native pixel grid; it shares the state, because
	// every accepted prim contributes to both.
	enum AccState : u32
	{
		kAccEmpty = 0,
		kAccUnion = 1,
		kAccReplace = 2,
	};

	// The kernel takes the two buffer objects rather than the pointers it reads
	// out of them: the vertex and index arrays, both mirror rings and the fused
	// min/max accumulator are the buffers' own members at compile-time offsets.

	// {wx, wy, wx, wy} from a mirror entry's packed position -- the shape
	// ComputeCullBBox's runion consumes, and the shape the xy ring holds.
	__forceinline_odr GSVector4i BroadcastXY(u64 xyp)
	{
#ifdef ARCH_ARM64
		return GSVector4i(vreinterpretq_s32_u64(vdupq_n_u64(xyp)));
#else
		return GSVector4i(_mm_set1_epi64x(static_cast<s64>(xyp)));
#endif
	}

	// The depth-clamp hack as two lane masks, so the clamp costs no branch and no
	// address-of on the parsed vector. m[1] is {XY, Z, UV, FOG}; only the Z lane
	// moves.
	//   Disabled        z' = z
	//   PrioritizeLower z' = z & 0x00FFFFFF
	//   PrioritizeUpper z' = ((z >> 8) & ~0xFF) | (z & 0xFF)
	__forceinline_odr void MakeDepthClampMasks(GSLimit24BitDepth mode, GSVector4i& keep, GSVector4i& shifted)
	{
		switch (mode)
		{
			case GSLimit24BitDepth::PrioritizeLower:
				keep = GSVector4i(-1, 0x00FFFFFF, -1, -1);
				shifted = GSVector4i::zero();
				break;
			case GSLimit24BitDepth::PrioritizeUpper:
				keep = GSVector4i(-1, 0x000000FF, -1, -1);
				shifted = GSVector4i(0, static_cast<int>(0xFFFFFF00u), 0, 0);
				break;
			default:
				keep = GSVector4i::xffffffff();
				shifted = GSVector4i::zero();
				break;
		}
	}

	// ------------------------------------------------------------------------
	// The decision, scalar: one vertex at a time against the mirror ring, which is
	// what the per-vertex kick does. It decides the first vertices of a chunk that
	// begins inside a primitive -- their partners were kicked before the chunk and
	// only the ring knows where they were -- and, where there is no vector pass,
	// all of them. Sets accept bit i for each chunk vertex i that completes a
	// drawn primitive, and folds those primitives' draw rects exactly as the
	// per-vertex kick does.
	// ------------------------------------------------------------------------
	struct ScalarDecided
	{
		GSVector4i rect;        // union of PrimDrawRect over the primitives accepted here
		GSVector4i native_rect; // the same on the native grid; only when Invariants::track_native_rect
		u32 count;             // how many
	};

	template <u32 prim>
	__forceinline_odr void DecideScalar(const GIFPackedReg* RESTRICT rxyz, u32 stride, u32 end, u32 pending,
		const GSBackQueue::VertexBuff* RESTRICT vertex_buf, const Invariants& inv, int band_shift,
		u64* RESTRICT accept, ScalarDecided& out)
	{
		constexpr u32 n = (prim == GS_SPRITE) ? 2u : 3u;
		constexpr int primclass = GSUtil::GetPrimClass(prim);
		constexpr bool strip = (prim == GS_TRIANGLESTRIP);

		const int ofx = inv.xyof.I32[0];
		const int ofy = inv.xyof.I32[1];
		const u32 xyt = vertex_buf->xy_tail;

		GSVertexKernels::CullMirrorEntry e0 = vertex_buf->kick_ring[(xyt - 1) & 3];
		GSVertexKernels::CullMirrorEntry e1 = vertex_buf->kick_ring[(xyt - 2) & 3];
		GSVertexKernels::CullMirrorEntry e2 = e1;

		u32 m = pending;
		for (u32 i = 0; i < end; i++)
		{
			const GIFPackedReg* RESTRICT rv = rxyz + i * stride;
			const int wx = static_cast<int>(rv->U32[0] & 0xFFFFu) - ofx;
			const int wy = static_cast<int>(rv->U32[1] & 0xFFFFu) - ofy;

			e2 = e1;
			e1 = e0;
			e0 = (band_shift == 4) ?
					 GSVertexKernels::MakeCullMirrorEntry<true>(wx, wy, inv.bounds, 4, inv.grid.band_bias_x, inv.grid.band_bias_y) :
					 GSVertexKernels::MakeCullMirrorEntry<false>(wx, wy, inv.bounds, band_shift, inv.grid.band_bias_x, inv.grid.band_bias_y);

			if (++m < n)
				continue;
			m = strip ? (n - 1) : 0;

			if ((rv->U32[3] & 0x8000u) != 0 || GSVertexKernels::CullTestScalar<n, primclass>(e0, e1, e2) != 0)
				continue;

			accept[i >> 6] |= 1ull << (i & 63);

			const GSVector4i bbox = GSVertexKernels::ComputeCullBBox<n, primclass>(
				BroadcastXY(e0.xyp), BroadcastXY(e1.xyp), BroadcastXY(e2.xyp), inv.grid, false);
			out.rect = out.rect.runion(GSVertexKernels::PrimDrawRect(bbox));
			if (inv.track_native_rect)
				out.native_rect = out.native_rect.runion(GSVertexKernels::PrimNativeDrawRectOrNone<primclass>(bbox));
			out.count++;
		}
	}

#ifdef ARCH_ARM64
	// ------------------------------------------------------------------------
	// The decision, four vertices at a time.
	//
	// Whether the primitive a vertex completes is drawn depends on three things
	// only -- the ADC bit, and the window positions of that vertex and the one or
	// two before it -- and on nothing the previous decision left behind. So it is
	// taken for four consecutive vertices in one go, with no branch, from the X, Y
	// and ADC words of the packed position alone; the vertex is not converted.
	//
	// It is CullTestScalar restated on the primitive's bounding box instead of on
	// per-vertex outcodes and bands:
	//   * "every vertex beyond one scissor edge" is the box's far side being
	//     beyond that edge;
	//   * "every vertex in one band on an axis" is the box's two sides being in
	//     one band, because a band (and TrimKey, where the class has no grid) is
	//     non-decreasing in the coordinate;
	//   * "two positions equal" is a compare of positions.
	//
	// DecideVectorWide is that statement taken literally, one vertex at a time on
	// offset-subtracted 32-bit coordinates. It is exact for every input and is
	// what DecideVector is held to.
	//
	// DecideVector takes the same decision on the raw 16-bit coordinates as the
	// GIF wrote them, X and Y side by side in the two halves of one 32-bit lane,
	// so every box operation covers both axes at once:
	//   * the box is an unsigned 16-bit min/max of the two or three raw positions.
	//     The offset is one constant for the chunk, so the raw order is the window
	//     order;
	//   * "beyond a scissor edge" compares the raw box with the edge moved into raw
	//     space (edge + offset). An edge that lands below 0 is clamped to 0, which
	//     keeps both kinds of test exact. One that lands above 0xFFFF cannot be
	//     stated in sixteen bits, and neither can a grid step above 0x10000: those
	//     calls go to DecideVectorWide;
	//   * with d = max - min (exact, both raw) and a = min - (offset + bias) taken
	//     modulo 2^16, min and max are in one band iff d <= (step - 1) & ~a. The
	//     low bits of a are the low bits of the true biased coordinate, which is
	//     all the band's remainder needs, and d carries the distance exactly, so a
	//     box 65,521 or more units wide is not mistaken for a narrow one the way a
	//     16-bit XOR of the two sides would be;
	//   * where the class has no grid, TrimKey(min) == TrimKey(max) is
	//     d == 0 || (d == 1 && (a & 15) == 15), taken as
	//     (d -saturating- ((a & 15) == 15)) == 0;
	//   * "two positions equal" compares the raw 32-bit X:Y lanes: this vertex and
	//     the last, this and the one before, and the last two, which is the first
	//     of these one lane on;
	//   * ADC is the sign of the lane the other two unzipped words land in.
	// The drawn primitives' boxes are unioned as the pass goes, in the same packed
	// form: a rejected lane is forced to 0xFFFF for the minimum and 0 for the
	// maximum. The draw rect of a run is the rounded box of its primitives, the
	// rounding being non-decreasing in every coordinate, so no accepted primitive
	// needs its own box later.
	//
	// The vertices before the chunk are not this pass's business (DecideScalar
	// takes the primitives that need them), and it needs no mask to stay away from
	// them: the "previous" lanes start as copies of the chunk's first vertex, so
	// the primitives that would reach back are a repeated position (triangles) or
	// an empty box (sprites) and reject themselves. The lanes past the end of the
	// chunk repeat its last vertex and reject themselves the same way.
	// ------------------------------------------------------------------------
	struct DecideBounds
	{
		int ofx, ofy;     // XYOFFSET plus the band bias
		int l, t, r, b;   // in the same biased space
		int bias_x, bias_y;
		u32 step;         // 1 << the grid's shift; unused where the class has no grid
	};

	struct VectorDecided
	{
		int minx, miny, maxx, maxy; // union of the accepted primitives' raw window boxes
		bool any;
	};

	__forceinline_odr DecideBounds MakeDecideBounds(const Invariants& inv, int band_shift)
	{
		DecideBounds k;
		k.bias_x = (band_shift == 0) ? 0 : inv.grid.band_bias_x;
		k.bias_y = (band_shift == 0) ? 0 : inv.grid.band_bias_y;
		k.ofx = inv.xyof.I32[0] + k.bias_x;
		k.ofy = inv.xyof.I32[1] + k.bias_y;
		if (band_shift == 4)
		{
			k.l = inv.bounds.l << 4;
			k.t = inv.bounds.t << 4;
			k.r = inv.bounds.r << 4;
			k.b = inv.bounds.b << 4;
		}
		else
		{
			k.l = inv.bounds.l - k.bias_x;
			k.t = inv.bounds.t - k.bias_y;
			k.r = inv.bounds.r - k.bias_x;
			k.b = inv.bounds.b - k.bias_y;
		}
		k.step = 1u << band_shift;
		return k;
	}

	alignas(16) inline constexpr u32 kDecideLaneMask[16][4] = {
		{0, 0, 0, 0}, {~0u, 0, 0, 0}, {0, ~0u, 0, 0}, {~0u, ~0u, 0, 0},
		{0, 0, ~0u, 0}, {~0u, 0, ~0u, 0}, {0, ~0u, ~0u, 0}, {~0u, ~0u, ~0u, 0},
		{0, 0, 0, ~0u}, {~0u, 0, 0, ~0u}, {0, ~0u, 0, ~0u}, {~0u, ~0u, 0, ~0u},
		{0, 0, ~0u, ~0u}, {~0u, 0, ~0u, ~0u}, {0, ~0u, ~0u, ~0u}, {~0u, ~0u, ~0u, ~0u}};

	// `n` is the primitive's vertex count (3: triangle strip and list, 2: sprite);
	// `keyed` says the class has no cull grid; `every` says every vertex completes
	// a primitive (a strip), and otherwise `period` has a bit for each of twelve
	// consecutive vertices that does, a pattern both a list's and a sprite's
	// repeat with. Not a template on the layout: every layout reads the position
	// the same way, at `step_bytes` from one record to the next. Writes
	// accept[0..1] whole.
	template <u32 n, bool keyed, bool every>
	__noinline void DecideVectorWide(const u8* RESTRICT pxyz, size_t step_bytes, u32 count, u32 period,
		const DecideBounds& kb, u64* RESTRICT accept, VectorDecided& out)
	{
		const auto word = [&](u32 i, u32 w) __attribute__((always_inline))
		{
			return reinterpret_cast<const u32*>(pxyz + i * step_bytes)[w];
		};
		const auto key = [](int v) __attribute__((always_inline)) { return v + (((v & 15) == 15) ? 1 : 0); };

		// The two vertices before the chunk's first stand at the first.
		int x1 = static_cast<int>(word(0, 0) & 0xFFFFu) - kb.ofx;
		int y1 = static_cast<int>(word(0, 1) & 0xFFFFu) - kb.ofy;
		int x2 = x1, y2 = y1;
		bool last_two_equal = false;
		int amnx = std::numeric_limits<int>::max(), amny = amnx;
		int amxx = std::numeric_limits<int>::min(), amxy = amxx;

		accept[0] = 0;
		accept[1] = 0;
		for (u32 i = 0; i < count; i++)
		{
			const int x = static_cast<int>(word(i, 0) & 0xFFFFu) - kb.ofx;
			const int y = static_cast<int>(word(i, 1) & 0xFFFFu) - kb.ofy;
			bool rej = (word(i, 3) & 0x8000u) != 0;

			int mnx = std::min(x, x1), mxx = std::max(x, x1);
			int mny = std::min(y, y1), mxy = std::max(y, y1);
			if constexpr (n == 3)
			{
				mnx = std::min(mnx, x2);
				mxx = std::max(mxx, x2);
				mny = std::min(mny, y2);
				mxy = std::max(mxy, y2);

				const bool eq1 = (x == x1) && (y == y1);
				const bool eq2 = (x == x2) && (y == y2);
				rej = rej || eq1 || eq2 || last_two_equal;
				last_two_equal = eq1;
			}

			rej = rej || mxx < kb.l || mnx >= kb.r || mxy < kb.t || mny >= kb.b;
			if constexpr (keyed)
				rej = rej || key(mnx) == key(mxx) || key(mny) == key(mxy);
			else
				rej = rej || static_cast<u32>(mnx ^ mxx) < kb.step || static_cast<u32>(mny ^ mxy) < kb.step;
			if constexpr (!every)
				rej = rej || ((period >> (i % 12)) & 1u) == 0;

			if (!rej)
			{
				accept[i >> 6] |= 1ull << (i & 63);
				amnx = std::min(amnx, mnx);
				amny = std::min(amny, mny);
				amxx = std::max(amxx, mxx);
				amxy = std::max(amxy, mxy);
			}

			x2 = x1;
			y2 = y1;
			x1 = x;
			y1 = y;
		}

		out.any = (accept[0] | accept[1]) != 0;
		if (out.any)
		{
			out.minx = amnx + kb.bias_x;
			out.miny = amny + kb.bias_y;
			out.maxx = amxx + kb.bias_x;
			out.maxy = amxy + kb.bias_y;
		}
	}

	template <u32 n, bool keyed, bool every>
	__noinline void DecideVector(const u8* RESTRICT pxyz, size_t step_bytes, u32 count, u32 period,
		const DecideBounds& kb, u64* RESTRICT accept, VectorDecided& out)
	{
		// The scissor in raw space; an edge below zero is the same test at zero.
		const int el = std::max(kb.l + kb.ofx, 0);
		const int et = std::max(kb.t + kb.ofy, 0);
		const int er = std::max(kb.r + kb.ofx, 0);
		const int eb = std::max(kb.b + kb.ofy, 0);
		const u32 low = keyed ? 15u : (kb.step - 1);
		if (((static_cast<u32>(el | et | er | eb) | low) >> 16) != 0) [[unlikely]]
		{
			DecideVectorWide<n, keyed, every>(pxyz, step_bytes, count, period, kb, accept, out);
			return;
		}

		const auto both = [](u32 x, u32 y) __attribute__((always_inline))
		{
			return vreinterpretq_u16_u32(vdupq_n_u32(x | (y << 16)));
		};
		const uint16x8_t near_edge = both(static_cast<u32>(el), static_cast<u32>(et));
		const uint16x8_t far_edge = both(static_cast<u32>(er), static_cast<u32>(eb));
		const uint16x8_t offset = both(static_cast<u32>(kb.ofx) & 0xFFFFu, static_cast<u32>(kb.ofy) & 0xFFFFu);
		const uint16x8_t lowbits = vdupq_n_u16(static_cast<u16>(low)); // step - 1, or the key's fifteen
		alignas(16) static constexpr u32 lane_bits[4] = {1, 2, 4, 8};
		const uint32x4_t bits = vld1q_u32(lane_bits);

		// The validity of the three quads a list's pattern cycles through.
		uint32x4_t valid0 = vdupq_n_u32(0), valid1 = valid0, valid2 = valid0;
		if constexpr (!every)
		{
			valid0 = vld1q_u32(kDecideLaneMask[period & 15]);
			valid1 = vld1q_u32(kDecideLaneMask[(period >> 4) & 15]);
			valid2 = vld1q_u32(kDecideLaneMask[(period >> 8) & 15]);
		}

		const auto ld = [](const u8* p) __attribute__((always_inline)) { return vld1q_u32(reinterpret_cast<const u32*>(p)); };

		// The "previous" lanes start as copies of the chunk's first vertex. peq
		// needs no particular start: it reaches only the chunk's first lane, which
		// the copy beside it has already rejected.
		const uint16x8_t f = vreinterpretq_u16_u32(ld(pxyz));
		const uint32x4_t fh = vreinterpretq_u32_u16(vuzp1q_u16(f, f));
		uint32x4_t prev = vuzp1q_u32(fh, fh);
		uint32x4_t peq = vdupq_n_u32(0);
		uint16x8_t amn = vdupq_n_u16(0xFFFFu), amx = vdupq_n_u16(0);

		// Returns the quad's four accept bits.
		const auto quad = [&](uint32x4_t v0, uint32x4_t v1, uint32x4_t v2, uint32x4_t v3) __attribute__((always_inline))
		{
			// The even half-words of a packed position are X, Y, the low half of Z
			// and the half of the last word that carries ADC in its top bit. Two
			// unzips leave X:Y in each lane of one vector and that ADC half on top
			// of each lane of the other, so ADC is the lane's sign.
			const uint32x4_t t01 = vreinterpretq_u32_u16(vuzp1q_u16(vreinterpretq_u16_u32(v0), vreinterpretq_u16_u32(v1)));
			const uint32x4_t t23 = vreinterpretq_u32_u16(vuzp1q_u16(vreinterpretq_u16_u32(v2), vreinterpretq_u16_u32(v3)));
			const uint32x4_t xy = vuzp1q_u32(t01, t23);
			const uint32x4_t za = vuzp2q_u32(t01, t23);

			// The vertex before each lane's, and for a triangle the one before that.
			const uint32x4_t xy1 = vextq_u32(prev, xy, 3);
			uint16x8_t mn = vminq_u16(vreinterpretq_u16_u32(xy), vreinterpretq_u16_u32(xy1));
			uint16x8_t mx = vmaxq_u16(vreinterpretq_u16_u32(xy), vreinterpretq_u16_u32(xy1));

			uint32x4_t rej32 = vcltzq_s32(vreinterpretq_s32_u32(za));

			if constexpr (n == 3)
			{
				const uint32x4_t xy2 = vextq_u32(prev, xy, 2);
				mn = vminq_u16(mn, vreinterpretq_u16_u32(xy2));
				mx = vmaxq_u16(mx, vreinterpretq_u16_u32(xy2));

				const uint32x4_t eq1 = vceqq_u32(xy, xy1);
				const uint32x4_t eq2 = vceqq_u32(xy, xy2);
				rej32 = vorrq_u32(vorrq_u32(rej32, eq1), vorrq_u32(eq2, vextq_u32(peq, eq1, 3)));
				peq = eq1;
			}

			// The scissor: far side before the near edge, near side at or past the far edge.
			uint16x8_t rej16 = vorrq_u16(vcgtq_u16(near_edge, mx), vcgeq_u16(mn, far_edge));

			// No sample point between the two sides.
			const uint16x8_t d = vsubq_u16(mx, mn);
			const uint16x8_t a = vsubq_u16(mn, offset);
			if constexpr (keyed)
			{
				const uint16x8_t one = vshrq_n_u16(vceqq_u16(vandq_u16(a, lowbits), lowbits), 15);
				rej16 = vorrq_u16(rej16, vceqzq_u16(vqsubq_u16(d, one)));
			}
			else
			{
				rej16 = vorrq_u16(rej16, vcgeq_u16(vbicq_u16(lowbits, a), d));
			}

			uint32x4_t any = vorrq_u32(vreinterpretq_u32_u16(rej16), rej32);
			if constexpr (!every)
			{
				any = vornq_u32(any, valid0);
				const uint32x4_t t = valid0;
				valid0 = valid1;
				valid1 = valid2;
				valid2 = t;
			}
			// Any bit of either half rejects the lane.
			const uint32x4_t rej = vtstq_u32(any, any);

			amn = vminq_u16(amn, vorrq_u16(mn, vreinterpretq_u16_u32(rej)));
			amx = vmaxq_u16(amx, vbicq_u16(mx, vreinterpretq_u16_u32(rej)));

			prev = xy;
			return static_cast<u64>(vaddvq_u32(vbicq_u32(bits, rej)));
		};

		// Sixty-four vertices to an accept word, so the word being built stays in
		// a register.
		const u8* RESTRICT p = pxyz;
		u32 left = count;
		for (u32 w = 0; w < 2; w++)
		{
			u64 word = 0;
			u32 block = std::min<u32>(left, 64);
			left -= block;
			u32 shift = 0;
			for (; block >= 4; block -= 4, shift += 4, p += 4 * step_bytes)
				word |= quad(ld(p), ld(p + step_bytes), ld(p + 2 * step_bytes), ld(p + 3 * step_bytes)) << shift;
			if (block != 0)
			{
				// The last one to three vertices of the chunk; nothing may be read
				// past the packet.
				const u8* const p1 = p + ((block > 1) ? step_bytes : 0);
				const u8* const p2 = p1 + ((block > 2) ? step_bytes : 0);
				word |= quad(ld(p), ld(p1), ld(p2), ld(p2)) << shift;
			}
			accept[w] = word;
		}

		out.any = (accept[0] | accept[1]) != 0;
		if (out.any)
		{
			// Raw to window: minus the offset. kb.of* hold offset plus bias.
			const uint32x4_t mn32 = vreinterpretq_u32_u16(amn);
			const uint32x4_t mx32 = vreinterpretq_u32_u16(amx);
			out.minx = static_cast<int>(vminv_u16(vmovn_u32(mn32))) - kb.ofx + kb.bias_x;
			out.miny = static_cast<int>(vminv_u16(vshrn_n_u32(mn32, 16))) - kb.ofy + kb.bias_y;
			out.maxx = static_cast<int>(vmaxv_u16(vmovn_u32(mx32))) - kb.ofx + kb.bias_x;
			out.maxy = static_cast<int>(vmaxv_u16(vshrn_n_u32(mx32, 16))) - kb.ofy + kb.bias_y;
		}
	}
#endif // ARCH_ARM64

	// One step of the conversion: chunk vertices [from, to) go to consecutive
	// buffer slots from `slot`; the ones at or above `acc_slot` are folded into the
	// fused vertex-trace bounds as they pass. A step is a run of drawn primitives
	// with nothing rejected between them, or the vertices left after the last.
	struct ConvertJob
	{
		u32 from, to, slot;
		u32 acc_slot;    // ~0 for none
		u32 colour_slot; // with flat shading only the provoking vertices' colours count: the slots from here on
	};

	// Pass three's whole interface: the boundary a conversion pass emitted at run
	// time would stand behind, with the same differential suite in front of it.
	struct ConvertArgs
	{
		const GIFPackedReg* rin;        // the chunk's first record
		GSVertex* vbuff;
		const ConvertJob* jobs;
		const ConvertJob* jobs_end;
		GSVertexKernels::FmmAcc* acc;   // triangle classes: read at entry, written at exit
		const Invariants* inv;
	};

#ifdef ARCH_ARM64
	// A value the optimiser is to take as given. ConvertParse's zero-Q select is
	// one operation as written; knowing that its mask is "the lane is zero", the
	// compiler rewrites it as an AND and an OR.
	__forceinline_odr uint32x4_t Unfolded(uint32x4_t v)
	{
		__asm__("" : "+w"(v));
		return v;
	}

	// One record of a layout as the two vectors of a GSVertex, for the conversion
	// loops. The bytes are GSVertexKernels::ParsePackedRecord_Neon's, which the
	// per-vertex paths keep; what differs is that everything a chunk fixes is a
	// vector made once, so a vertex is loads, table lookups and the fix-ups and
	// nothing crosses from a general register:
	//
	//   * a carried UV or {UV, FOG} is OR-ed into the lanes the table lookup leaves
	//     zero, where the per-vertex parse inserts it;
	//   * a UV the record carries is gathered by the same lookup that gathers the
	//     position and masked to its fourteen bits a half, where the per-vertex parse
	//     builds it in a general register;
	//   * Q == +0.0 becomes FLT_MIN by selecting on "the lane is zero" over all four
	//     lanes against a vector that is zero in the other three, so a zero S, T or
	//     colour is replaced by zero;
	//   * the NOP-padded triple's second fix-up, a NaN Q to FLT_MAX, is the compare,
	//     the lane mask and the select.
	// Under keep_gif_q both replacement masks are zero and both selects pass Q through.
	template <GSVertexKernels::PackedLayout layout, bool clamp>
	struct ConvertParse
	{
		uint8x16_t pat_m0;
		uint8x16_t pat_m1;
		uint32x4_t q_min;      // {0, 0, 0, FLT_MIN}
		uint32x4_t q_nan_lane; // lane 3
		uint32x4_t q_max;      // FLT_MAX
		uint32x4_t carry;      // the carried m[0], for a layout that omits part of it
		uint32x4_t m1_or;      // the carried UV and fog
		uint32x4_t m1_and;     // a record's own UV keeps fourteen bits a half
		uint32x4_t clamp_keep;
		uint32x4_t clamp_shifted;
		u32 off_a, off_c, off_xyz; // descriptors from the record's start

		__forceinline explicit ConvertParse(const Invariants& inv)
		{
			using namespace GSVertexKernels;
			alignas(16) static constexpr u8 k_m0[16] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 20, 24, 28, 8, 9, 10, 11};
			// {X | Y << 16, Z, UV, F} from {the record, the record >> 4 a lane} and,
			// in the second row, the UV record as a third table.
			alignas(16) static constexpr u8 k_m1_xyzf2[2][16] = {
				{0, 1, 4, 5, 24, 25, 26, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 28, 0xFF, 0xFF, 0xFF},
				{0, 1, 4, 5, 24, 25, 26, 0xFF, 32, 33, 36, 37, 28, 0xFF, 0xFF, 0xFF}};
			// {X | Y << 16, Z, UV, FOG} from the record and, in the second row, the
			// UV record as a second table.
			alignas(16) static constexpr u8 k_m1_xyz2[2][16] = {
				{0, 1, 4, 5, 8, 9, 10, 11, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
				{0, 1, 4, 5, 8, 9, 10, 11, 16, 17, 20, 21, 0xFF, 0xFF, 0xFF, 0xFF}};
			constexpr bool xyzf2 = LayoutIsXYZF2(layout);
			constexpr bool has_uv = LayoutHasUV(layout);

			pat_m0 = vld1q_u8(k_m0);
			pat_m1 = vld1q_u8(xyzf2 ? k_m1_xyzf2[has_uv] : k_m1_xyz2[has_uv]);

			const uint32x4_t zero = vdupq_n_u32(0);
			const u32 fix = inv.keep_gif_q ? 0u : ~0u;
			q_min = vsetq_lane_u32(0x00800000u & fix, zero, 3);
			q_nan_lane = vsetq_lane_u32(fix, zero, 3);
			q_max = vreinterpretq_u32_f32(vdupq_n_f32(FLT_MAX));

			carry = zero;
			if constexpr (LayoutCarriesM0(layout))
				carry = vreinterpretq_u32_s32(inv.carry_m0.v4s);

			const u64 uvfog = inv.uvfog;
			m1_and = vsetq_lane_u32(0x3FFF3FFFu, vdupq_n_u32(~0u), 2);
			if constexpr (has_uv)
				m1_or = vsetq_lane_u32(xyzf2 ? 0u : static_cast<u32>(uvfog >> 32), zero, 3);
			else if constexpr (xyzf2)
				m1_or = vsetq_lane_u32(static_cast<u32>(uvfog), zero, 2);
			else
				m1_or = vreinterpretq_u32_u64(vsetq_lane_u64(uvfog, vdupq_n_u64(0), 1));

			clamp_keep = clamp ? vreinterpretq_u32_s32(inv.clamp_keep.v4s) : zero;
			clamp_shifted = clamp ? vreinterpretq_u32_s32(inv.clamp_shifted.v4s) : zero;

			off_a = LayoutOffA<layout>(inv.off);
			off_xyz = LayoutOffXyz<layout>(inv.off);
			off_c = 0;
			if constexpr (LayoutIsTriple(layout))
				off_c = LayoutOffRgba<layout>(inv.off);
			else if constexpr (LayoutTakesColourOverCarry(layout))
				off_c = LayoutOffColour<layout>(inv.off);
		}

		// Every load is one whole descriptor of the record at `rv`, so nothing is
		// read outside the record.
		__forceinline void operator()(const GIFPackedReg* RESTRICT rv, uint32x4_t& m0, uint32x4_t& m1) const
		{
			using namespace GSVertexKernels;
			const auto ld = [rv](u32 off) __attribute__((always_inline)) { return vld1q_u8(reinterpret_cast<const u8*>(rv + off)); };

			if constexpr (LayoutIsTriple(layout))
			{
				const uint8x16x2_t st_rgba = {ld(off_a), ld(off_c)};
				uint32x4_t v0 = vreinterpretq_u32_u8(vqtbl2q_u8(st_rgba, pat_m0));
				v0 = vbslq_u32(Unfolded(vceqzq_u32(v0)), q_min, v0);
				if constexpr (layout == PackedLayout::NopTripleXYZF2)
				{
					// FCMEQ of a lane with itself is false only for a NaN.
					const float32x4_t f = vreinterpretq_f32_u32(v0);
					v0 = vbslq_u32(vbicq_u32(q_nan_lane, vceqq_f32(f, f)), q_max, v0);
				}
				m0 = v0;
			}
			else if constexpr (layout == PackedLayout::PairSTQXYZ2)
			{
				m0 = vreinterpretq_u32_u64(vcombine_u64(vld1_u64(reinterpret_cast<const u64*>(rv + off_a)),
					vget_high_u64(vreinterpretq_u64_u32(carry))));
			}
			else if constexpr (LayoutTakesColourOverCarry(layout))
			{
				// The carry's lane 2 is the latched Q (MakeLayoutCarry), which pat_m0 puts last.
				const uint8x16x2_t carry_rgba = {vreinterpretq_u8_u32(carry), ld(off_c)};
				m0 = vreinterpretq_u32_u8(vqtbl2q_u8(carry_rgba, pat_m0));
			}
			else
			{
				m0 = carry;
			}

			const uint8x16_t xyz = ld(off_xyz);
			uint32x4_t v1;
			if constexpr (LayoutIsXYZF2(layout))
			{
				const uint8x16_t shifted = vreinterpretq_u8_u32(vshrq_n_u32(vreinterpretq_u32_u8(xyz), 4));
				if constexpr (LayoutHasUV(layout))
				{
					const uint8x16x3_t t = {xyz, shifted, ld(off_a)};
					v1 = vandq_u32(vreinterpretq_u32_u8(vqtbl3q_u8(t, pat_m1)), m1_and);
				}
				else
				{
					const uint8x16x2_t t = {xyz, shifted};
					v1 = vorrq_u32(vreinterpretq_u32_u8(vqtbl2q_u8(t, pat_m1)), m1_or);
				}
			}
			else if constexpr (LayoutHasUV(layout))
			{
				const uint8x16x2_t t = {xyz, ld(off_a)};
				v1 = vorrq_u32(vandq_u32(vreinterpretq_u32_u8(vqtbl2q_u8(t, pat_m1)), m1_and), m1_or);
			}
			else
			{
				v1 = vorrq_u32(vreinterpretq_u32_u8(vqtbl1q_u8(xyz, pat_m1)), m1_or);
			}
			if constexpr (clamp)
				v1 = vorrq_u32(vandq_u32(v1, clamp_keep), vandq_u32(vshrq_n_u32(v1, 8), clamp_shifted));
			m1 = v1;
		}
	};
#endif // ARCH_ARM64

	// `triangle`: the class keeps fused vertex-trace bounds (strips and lists).
	// `clamp`: the depth clamp, a compile-time arm of the loop rather than a test
	// inside it -- no title in the corpus turns it on.
	//
	// A job's vertices fall, in slot order, into at most three stretches: below
	// acc_slot they are converted and nothing else; from there to colour_slot their
	// position and texture coordinates are folded into the bounds; from colour_slot
	// on their colour is too. Gouraud shading folds every colour, so there the
	// second stretch is empty, and each of the two that remain is a loop with no
	// test in it: the convert loop, and one fold loop per texture mode, chosen per
	// job. Flat shading is rare in volume and takes one loop that tests each vertex.
	//
	// The fold is GSVertexKernels::FmmAccumVertex's operations in its order, with
	// the position gathered by one table lookup. The STQ divide and the masked IEEE
	// min and max are that function's own: FMINNM and FMAXNM would not do, because
	// the two Q lanes are the GIF's bits and can hold a signalling NaN.
	template <GSVertexKernels::PackedLayout layout, bool triangle, bool clamp>
	__forceinline_odr void ConvertJobs(const ConvertArgs& args)
	{
		const Invariants& inv = *args.inv;
		const u32 stride = GSVertexKernels::LayoutStride<layout>(inv.off);
		GSVertex* RESTRICT vbuff = args.vbuff;
#ifdef ARCH_ARM64
		const ConvertParse<layout, clamp> parse(inv);
		const GIFPackedReg* RESTRICT rin = args.rin;

		[[maybe_unused]] const u32 shade = inv.shade;
		[[maybe_unused]] const bool tme = (shade & 1u) != 0;
		[[maybe_unused]] const bool fst = (shade & 2u) != 0;
		[[maybe_unused]] const bool iip = (shade & 4u) != 0;

		// {x, y, z, fog word} out of m[1]: GSVertexKernels::FmmPos.
		alignas(16) static constexpr u8 k_pos[16] = {0, 1, 0xFF, 0xFF, 2, 3, 0xFF, 0xFF, 4, 5, 6, 7, 12, 13, 14, 15};
		[[maybe_unused]] const uint8x16_t pat_pos = vld1q_u8(k_pos);
		uint32x4_t pmin = vdupq_n_u32(0), pmax = pmin, tmin = pmin, tmax = pmin, tnan = pmin;
		uint8x16_t cmin = vdupq_n_u8(0), cmax = cmin;
		if constexpr (triangle)
		{
			const GSVertexKernels::FmmAcc& a = *args.acc;
			pmin = vreinterpretq_u32_s32(a.pmin.v4s);
			pmax = vreinterpretq_u32_s32(a.pmax.v4s);
			tmin = vreinterpretq_u32_s32(a.tmin.v4s);
			tmax = vreinterpretq_u32_s32(a.tmax.v4s);
			tnan = vreinterpretq_u32_s32(a.tnan.v4s);
			cmin = vreinterpretq_u8_s32(a.cmin.v4s);
			cmax = vreinterpretq_u8_s32(a.cmax.v4s);
		}

		const auto put = [](GSVertex* RESTRICT out, uint32x4_t m0, uint32x4_t m1) __attribute__((always_inline))
		{
			out->m[0] = vreinterpretq_s32_u32(m0);
			out->m[1] = vreinterpretq_s32_u32(m1);
		};
		const auto fold_position = [&](uint32x4_t m1) __attribute__((always_inline))
		{
			const uint32x4_t pos = vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(m1), pat_pos));
			pmin = vminq_u32(pmin, pos);
			pmax = vmaxq_u32(pmax, pos);
		};
		const auto fold_uv = [&](uint32x4_t m1) __attribute__((always_inline))
		{
			tmin = vreinterpretq_u32_u16(vminq_u16(vreinterpretq_u16_u32(tmin), vreinterpretq_u16_u32(m1)));
			tmax = vreinterpretq_u32_u16(vmaxq_u16(vreinterpretq_u16_u32(tmax), vreinterpretq_u16_u32(m1)));
		};
		const auto fold_stq = [&](uint32x4_t m0) __attribute__((always_inline))
		{
			// {S/Q, T/Q, Q, Q}; a NaN lane leaves the bound as it was and is recorded.
			const float32x4_t raw = vreinterpretq_f32_u32(m0);
			const float32x2_t q2 = vdup_laneq_f32(raw, 3);
			const float32x4_t stq = vcombine_f32(vdiv_f32(vget_low_f32(raw), q2), q2);
			const uint32x4_t number = vceqq_f32(stq, stq);
			const float32x4_t lo = vreinterpretq_f32_u32(tmin);
			const float32x4_t hi = vreinterpretq_f32_u32(tmax);
			tmin = vbslq_u32(number, vreinterpretq_u32_f32(vminq_f32(lo, stq)), tmin);
			tmax = vbslq_u32(number, vreinterpretq_u32_f32(vmaxq_f32(hi, stq)), tmax);
			tnan = vornq_u32(tnan, number);
		};
		const auto fold_colour = [&](uint32x4_t m0) __attribute__((always_inline))
		{
			cmin = vminq_u8(cmin, vreinterpretq_u8_u32(m0));
			cmax = vmaxq_u8(cmax, vreinterpretq_u8_u32(m0));
		};

		uint32x4_t m0, m1;
		for (const ConvertJob* RESTRICT q = args.jobs; q != args.jobs_end; q++)
		{
			const GIFPackedReg* RESTRICT rv = rin + q->from * stride;
			GSVertex* RESTRICT out = vbuff + q->slot;
			const u32 n = q->to - q->from;
			u32 only = n;
			if constexpr (triangle)
				only = std::min(n, q->acc_slot - std::min(q->acc_slot, q->slot));

			for (GSVertex* const end = out + only; out != end; out++, rv += stride)
			{
				parse(rv, m0, m1);
				put(out, m0, m1);
			}

			if constexpr (triangle)
			{
				if (only == n)
					continue;
				GSVertex* const end = out + (n - only);
				if (!iip) [[unlikely]]
				{
					u32 s = q->slot + only;
					const u32 colour_slot = q->colour_slot;
					do
					{
						parse(rv, m0, m1);
						put(out, m0, m1);
						fold_position(m1);
						if (tme)
						{
							if (fst)
								fold_uv(m1);
							else
								fold_stq(m0);
						}
						if (s >= colour_slot)
							fold_colour(m0);
						s++;
						rv += stride;
					} while (++out != end);
				}
				else if (!tme)
				{
					do
					{
						parse(rv, m0, m1);
						put(out, m0, m1);
						fold_position(m1);
						fold_colour(m0);
						rv += stride;
					} while (++out != end);
				}
				else if (fst)
				{
					do
					{
						parse(rv, m0, m1);
						put(out, m0, m1);
						fold_position(m1);
						fold_uv(m1);
						fold_colour(m0);
						rv += stride;
					} while (++out != end);
				}
				else
				{
					do
					{
						parse(rv, m0, m1);
						put(out, m0, m1);
						fold_position(m1);
						fold_stq(m0);
						fold_colour(m0);
						rv += stride;
					} while (++out != end);
				}
			}
		}

		if constexpr (triangle)
		{
			GSVertexKernels::FmmAcc& a = *args.acc;
			a.pmin = GSVector4i(vreinterpretq_s32_u32(pmin));
			a.pmax = GSVector4i(vreinterpretq_s32_u32(pmax));
			a.tmin = GSVector4i(vreinterpretq_s32_u32(tmin));
			a.tmax = GSVector4i(vreinterpretq_s32_u32(tmax));
			a.tnan = GSVector4i(vreinterpretq_s32_u32(tnan));
			a.cmin = GSVector4i(vreinterpretq_s32_u8(cmin));
			a.cmax = GSVector4i(vreinterpretq_s32_u8(cmax));
		}
#else
		// No vector parse and no fused bounds here: the portable parse, a vertex at a time.
		const GIFPackedLayout off = inv.off;
		const GIFPackedReg* RESTRICT rin = args.rin;
		GSVector4i carry = GSVector4i::zero();
		if constexpr (GSVertexKernels::LayoutCarriesM0(layout))
			carry = inv.carry_m0;
		const u64 uvfog = inv.uvfog;
		[[maybe_unused]] const GSVector4i clamp_keep = clamp ? inv.clamp_keep : GSVector4i::zero();
		[[maybe_unused]] const GSVector4i clamp_shifted = clamp ? inv.clamp_shifted : GSVector4i::zero();
		const GSVertexKernels::PackedParseConsts kc = GSVertexKernels::MakePackedParseConsts(inv.keep_gif_q);

		GSVector4i m0, m1;
		for (const ConvertJob* RESTRICT q = args.jobs; q != args.jobs_end; q++)
		{
			const GIFPackedReg* RESTRICT rv = rin + q->from * stride;
			GSVertex* RESTRICT out = vbuff + q->slot;
			u32 left = q->to - q->from;
			do
			{
				GSVertexKernels::ParsePackedRecord_Fast<layout>(rv, off, uvfog, carry, kc, m0, m1);
				if constexpr (clamp)
					m1 = (m1 & clamp_keep) | (m1.srl32<8>() & clamp_shifted);

				out->m[0] = m0;
				out->m[1] = m1;

				rv += stride;
				out++;
			} while (--left != 0);
		}
#endif
	}

	// ------------------------------------------------------------------------
	// The kernel. `prim` is one of GS_TRIANGLESTRIP / GS_TRIANGLELIST /
	// GS_SPRITE; `layout` says where the record's descriptors sit and which of
	// them the tag omits -- the only per-layout part of the kernel is the parse
	// and the offset the position is read at. The caller guarantees:
	//   * the per-draw environment snapshot cannot fire inside (it has been taken
	//     for this call, or the index buffer is not empty);
	//   * m_recent_buffer_switch is clear or draw buffering is off;
	//   * the scissor is valid, so the ADC bit is the whole pre-cull rejection;
	//   * the scalar-outcode cull applies (no AA1 expansion), so the decision is
	//     the band/outcode pair and the accepted-prim bbox takes whichever
	//     rounding inv.grid names;
	//   * XYOFFSET's two lanes are within 0..0xFFFF;
	//   * tail - head is less than the primitive's vertex count;
	//   * tail + count + 3 <= maxcount, so no growth can be needed;
	//   * tail + count < MaxVerticesForPrim, so no VERTEXCOUNT flush can be
	//     needed.
	// Every vertex of the chunk is consumed; the caller advances by `count`.
	//
	// Three passes. The first decides, for every vertex, whether the primitive it
	// completes is drawn (DecideVector, with DecideScalar for the primitives that
	// began before the chunk). The second walks the drawn primitives with integer
	// arithmetic alone: it writes their indices, moves the cursor exactly as the
	// per-vertex kick does, and lists which vertices have to exist in the buffer
	// and where. The third converts those vertices, each once, straight into its
	// final slot.
	//
	// What is left is what the per-vertex kick leaves: the vertex buffer over
	// [0, tail), the indices, head / tail / next, both rings, the fused min/max
	// state and the last vertex. A vertex is converted only if it is below the
	// tail at exit: the vertices of drawn primitives, and the ones kicked after
	// the last drawn primitive, which the buffer keeps (a strip's whole rejected
	// tail, a list's or a sprite's unfinished primitive). The rejected vertices
	// between two drawn primitives are never converted; the per-vertex kick wrote
	// them and then wrote over them or rewound past them.
	// ------------------------------------------------------------------------
	template <u32 prim, GSVertexKernels::PackedLayout layout>
	__noinline GSVector4i RunChunk(const GIFPackedReg* RESTRICT rin, u32 count,
		GSBackQueue::VertexBuff* RESTRICT vertex_buf, GSBackQueue::IndexBuff* RESTRICT index_buf,
		const Invariants& inv, u32* RESTRICT acc_state_out, GSVector4i* RESTRICT native_acc_out)
	{
		constexpr u32 n = (prim == GS_SPRITE) ? 2u : 3u;
		constexpr int primclass = GSUtil::GetPrimClass(prim);
		constexpr bool strip = (prim == GS_TRIANGLESTRIP);
		static_assert(prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLELIST || prim == GS_SPRITE);
		static_assert(kChunkVertices == 128, "the accept mask is two words");

		GSVertex* RESTRICT vbuff = vertex_buf->buff;
		u16* RESTRICT ibuff = index_buf->buff;

		const GIFPackedLayout off = inv.off;
		const u32 stride = GSVertexKernels::LayoutStride<layout>(off);
		const u32 off_xyz = GSVertexKernels::LayoutOffXyz<layout>(off);
		const GIFPackedReg* RESTRICT rxyz = rin + off_xyz;

		const u32 head0 = vertex_buf->head;
		const u32 tail0 = vertex_buf->tail;
		const u32 xy_tail0 = vertex_buf->xy_tail;
		const u32 itail0 = index_buf->tail;
		const u32 pending = tail0 - head0;

		// The class's own grid: a sprite has one only at native, a triangle wherever the scale is a power of two.
		const int band_shift = inv.grid.ShiftFor<primclass>();

		// ---- pass one: the decision -------------------------------------------
		u64 accept[2] = {0, 0};
		ScalarDecided sd;
		sd.rect = GSVertexKernels::NativeDrawRectNone();
		sd.native_rect = GSVertexKernels::NativeDrawRectNone();
		sd.count = 0;
		GSVector4i acc_rect = GSVertexKernels::NativeDrawRectNone();
		u32 scalar_decided = 0;

#ifdef ARCH_ARM64
		if (count <= kScalarDecideVertices)
		{
			// Too few vertices to pay for the four-wide pass's entry and exit.
			DecideScalar<prim>(rxyz, stride, count, pending, vertex_buf, inv, band_shift, accept, sd);
			scalar_decided = sd.count;
		}
		else
		{
			// Which of twelve consecutive vertices complete a primitive: for a
			// list, where (pending + i + 1) is a multiple of three.
			u32 period = 0;
			if constexpr (prim == GS_TRIANGLELIST)
				period = 0x249u << ((5 - pending) % 3);
			else if constexpr (prim == GS_SPRITE)
				period = pending ? 0x555u : 0xAAAu;

			const DecideBounds kb = MakeDecideBounds(inv, band_shift);
			const u8* const pxyz = reinterpret_cast<const u8*>(rxyz);
			const size_t step_bytes = stride * sizeof(GIFPackedReg);
			VectorDecided vd;
			if (band_shift == 0)
				DecideVector<n, true, strip>(pxyz, step_bytes, count, period, kb, accept, vd);
			else
				DecideVector<n, false, strip>(pxyz, step_bytes, count, period, kb, accept, vd);
			if (vd.any)
			{
				const GSVector4i bbox = GSVertexKernels::RoundCullRect<primclass>(
					GSVector4i(vd.minx, vd.miny, vd.maxx, vd.maxy), inv.grid, false);
				acc_rect = GSVertexKernels::PrimDrawRect(bbox);
			}

			// The first n - 1 vertices have a partner from before the chunk, if any
			// is pending. The vector pass found nothing there.
			if (pending != 0)
				DecideScalar<prim>(rxyz, stride, n - 1, pending, vertex_buf, inv, band_shift, accept, sd);
			scalar_decided = sd.count;
		}
#else
		DecideScalar<prim>(rxyz, stride, count, pending, vertex_buf, inv, band_shift, accept, sd);
		scalar_decided = sd.count;
#endif
		if (sd.count != 0)
			acc_rect = acc_rect.runion(sd.rect);

		// ---- pass two: the cursor, the indices, and what to convert ------------
		const u32 shade = inv.shade;
		const bool tme = (shade & 1u) != 0;
		const bool fst = (shade & 2u) != 0;
		const bool iip = (shade & 4u) != 0;

		u32 head = head0;
		u32 next = vertex_buf->next;
		u32 itail = itail0;
		u16* RESTRICT ib = ibuff + itail;
		u32 watermark = vertex_buf->fmm_watermark;
		bool fmm_valid = vertex_buf->fmm_valid;
		bool fmm_dirty = false;

		GSVertexKernels::FmmAcc acc;
#ifdef ARCH_ARM64
		if (primclass == GS_TRIANGLE_CLASS && fmm_valid)
		{
			// Only meaningful while fmm_valid; the reset at the first emission of
			// a draw initializes it for real.
			acc = vertex_buf->fmm_acc;
		}
		else
#endif
		{
			GSVertexKernels::FmmAccReset(acc, false, false);
		}

		ConvertJob jobs[kChunkVertices + 1];
		ConvertJob* RESTRICT job = jobs;

		// Chunk vertices [0, done) are accounted for; `base` is the slot chunk
		// vertex `done` takes when it is written where the per-vertex kick's tail
		// stood, which is every case but a strip's compaction.
		u32 done = 0;
		u32 base = tail0;
		u32 tail = tail0;
		bool last_in_buffer = true;

		u32 word = 0;
		u64 bitsleft = accept[0];
		for (;;)
		{
			if (bitsleft == 0 && word == 0)
			{
				word = 1;
				bitsleft = accept[1];
			}
			// The next drawn primitive, and how many more follow it with nothing
			// rejected between: the next vertex's for a strip, the next pair's for a
			// sprite. A list's are taken one at a time (its flat-shaded colour bound
			// wants every third vertex, and no title in the census sends lists in
			// volume). A run stops at the accept word's end.
			const bool drawn = (bitsleft != 0);
			u32 i = count, run = 0;
			if (drawn)
			{
				const u32 b = static_cast<u32>(std::countr_zero(bitsleft));
				i = word * 64 + b;
				if constexpr (prim == GS_TRIANGLELIST)
				{
					run = 1;
					bitsleft &= bitsleft - 1;
				}
				else
				{
					constexpr u32 spacing = strip ? 1u : 2u;
					constexpr u64 positions = strip ? ~0ull : 0x5555555555555555ull;
					const u64 gap = ~(bitsleft >> b) & positions;
					run = (gap != 0) ? (static_cast<u32>(std::countr_zero(gap)) / spacing) : ((64 - b + spacing - 1) / spacing);
					const u32 end = b + run * spacing;
					bitsleft = (end >= 64) ? 0 : ((bitsleft >> end) << end);
				}
			}

			// Which chunk vertices this step converts, [from, to), and the slot the
			// first of them goes to.
			u32 from, to, slot, dst = 0;
			if constexpr (strip)
			{
				from = done;
				slot = base;
				to = drawn ? (i + run) : count;
				if (drawn)
				{
					// The per-vertex kick's head when this vertex completes its
					// window: two below the slot the vertex itself takes.
					const u32 window = base + (i - done) - 2;
					dst = window;
					if (next < window)
					{
						// The window slides down over the vertices the rejections
						// left behind. Its three vertices are the last three kicked:
						// the ones from before the chunk are in the buffer and move,
						// the rest are converted straight into place.
						dst = next;
						const u32 first = (i >= done + 2) ? (i - 2) : done;
						const u32 moved = first + 2 - i;
						for (u32 t = 0; t < moved; t++)
							vbuff[dst + t] = vbuff[window + t];
						from = first;
						slot = dst + moved;
#ifdef ARCH_ARM64
						// Vertices moved below the fused-FMM watermark must re-accumulate.
						watermark = std::min(watermark, dst);
#endif
					}
				}
				else
				{
					tail = base + (count - done);
					if (tail > head + 2)
						head = tail - 2;
				}
			}
			else
			{
				if (drawn)
				{
					dst = head;
					from = (i + 1 >= n) ? (i + 1 - n) : 0;
					slot = dst + (n - 1) - (i - from);
					to = i + 1 + (run - 1) * n;
				}
				else
				{
					// The unfinished primitive at the end of the chunk.
					const u32 unfinished = (pending + count) % n;
					const u32 fresh = std::min(unfinished, count - done);
					from = count - fresh;
					slot = head + unfinished - fresh;
					to = count;
					tail = head + unfinished;
					last_in_buffer = (unfinished != 0) || (done == count);
				}
			}

			u32 acc_slot = ~0u;
			u32 colour_slot = ~0u;
			if (drawn)
			{
				const bool first_of_draw = (itail == 0);
				if constexpr (prim == GS_SPRITE)
				{
					for (u32 t = 0; t < run; t++, ib += 2)
					{
						ib[0] = static_cast<u16>(dst + 2 * t + 0);
						ib[1] = static_cast<u16>(dst + 2 * t + 1);
					}
					head = dst + 2 * run;
					next = head;
					base = head;
					itail += 2 * run;
					done = to;
				}
				else if constexpr (strip)
				{
					for (u32 t = 0; t < run; t++, ib += 3)
					{
						ib[0] = static_cast<u16>(dst + t + 0);
						ib[1] = static_cast<u16>(dst + t + 1);
						ib[2] = static_cast<u16>(dst + t + 2);
					}
					head = dst + run;
					next = dst + run + 2;
					base = next;
					itail += 3 * run;
					done = to;
				}
				else
				{
					ib[0] = static_cast<u16>(dst + 0);
					ib[1] = static_cast<u16>(dst + 1);
					ib[2] = static_cast<u16>(dst + 2);
					ib += 3;
					head = dst + 3;
					next = dst + 3;
					base = next;
					itail += 3;
					done = to;
				}
				tail = base;

#ifdef ARCH_ARM64
				if constexpr (primclass == GS_TRIANGLE_CLASS)
				{
					// The run's first primitive, as the per-vertex kick takes it.
					// Each later one adds one vertex above the watermark, its own
					// provoking vertex.
					const u32 last = dst + 2;
					if (first_of_draw)
					{
						GSVertexKernels::FmmAccReset(acc, tme, fst);
						fmm_valid = true;
						watermark = last - 2;
					}

					if (fmm_valid)
					{
						// The primitive's vertices that were in the buffer before
						// this step are folded in from there; the ones this step
						// converts are folded in as they are converted.
						const u32 lo = std::max(watermark, last - 2);
						for (u32 j = lo; j < std::min(slot, last); j++)
						{
							GSVertexKernels::FmmAccumVertex(acc, GSVector4i(vbuff[j].m[0]),
								GSVector4i(vbuff[j].m[1]), tme, fst, iip);
						}
						acc_slot = std::max(lo, slot);
						colour_slot = last;
						watermark = next;
						fmm_dirty = true;
					}
				}
#else
				(void)first_of_draw;
#endif
			}

			if (from != to)
				*job++ = {from, to, slot, acc_slot, colour_slot};

			if (!drawn)
				break;
		}

		// ---- pass three: conversion ---------------------------------------------
		if (job != jobs)
		{
			const ConvertArgs args = {rin, vbuff, jobs, job, &acc, &inv};
			if (inv.clamp_enabled)
				ConvertJobs<layout, primclass == GS_TRIANGLE_CLASS, true>(args);
			else
				ConvertJobs<layout, primclass == GS_TRIANGLE_CLASS, false>(args);
		}
#ifdef ARCH_ARM64
		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			if (fmm_dirty)
				vertex_buf->fmm_acc = acc;
		}
#endif

		// m_v carries the last parsed vertex out of the batch. It is the buffer's
		// last vertex unless the chunk ended on a rejected list primitive or
		// sprite, which leaves nothing of itself behind.
		if (last_in_buffer)
		{
			*inv.last_out = vbuff[tail - 1];
		}
		else
		{
			GSVector4i m0, m1;
			GSVector4i carry = GSVector4i::zero();
			if constexpr (GSVertexKernels::LayoutCarriesM0(layout))
				carry = inv.carry_m0;
			GSVertexKernels::ParsePackedRecord_Fast<layout>(rin + (count - 1) * stride, off, inv.uvfog, carry,
				GSVertexKernels::MakePackedParseConsts(inv.keep_gif_q), m0, m1);
			if (inv.clamp_enabled)
				m1 = (m1 & inv.clamp_keep) | (m1.srl32<8>() & inv.clamp_shifted);
			inv.last_out->m[0] = m0;
			inv.last_out->m[1] = m1;
		}

		// The sprite's first vertex takes the second's Q, and the native-grid rect
		// has no monotone shortcut -- a primitive that spans no native sample point
		// contributes nothing -- so it is asked per primitive. Both read the
		// primitives' vertices now that they are in place; the second runs only with
		// draw buffering on above native, and the primitives DecideScalar took have
		// theirs already.
		GSVector4i native_acc_rect = sd.native_rect;
		if constexpr (prim == GS_SPRITE)
		{
			if (inv.sprite_q_fix)
			{
				for (u32 t = itail0; t < itail; t += 2)
				{
					const u32 dst = ibuff[t];
					vbuff[dst + 0].RGBAQ.Q = vbuff[dst + 1].RGBAQ.Q;
				}
			}
		}
		if (inv.track_native_rect)
		{
			const GSVector4i xyof = inv.xyof;
			for (u32 t = itail0 + scalar_decided * n; t < itail; t += n)
			{
				const u32 dst = ibuff[t];
				const GSVector4i v0 = GSVector4i(vbuff[dst + n - 1].m[1]).xxxx().u16to32().sub32(xyof);
				const GSVector4i v1 = GSVector4i(vbuff[dst + n - 2].m[1]).xxxx().u16to32().sub32(xyof);
				const GSVector4i v2 = (n == 3) ? GSVector4i(vbuff[dst].m[1]).xxxx().u16to32().sub32(xyof) : v0;
				const GSVector4i bbox = GSVertexKernels::ComputeCullBBox<n, primclass>(v0, v1, v2, inv.grid, false);
				native_acc_rect = native_acc_rect.runion(GSVertexKernels::PrimNativeDrawRectOrNone<primclass>(bbox));
			}
		}

		// ---- exit -----------------------------------------------------------
		// The two rings, for the chunk's last two vertices. A window never holds
		// more than two vertices waiting for a third, so the two most recent
		// entries are the only ones any decision can read before they are written
		// again; the per-vertex kick keeps four because a mask of three is cheaper
		// than a modulus, not because the older two are read.
		{
			GSVector4i* RESTRICT xy_ring = vertex_buf->xy;
			GSVertexKernels::CullMirrorEntry* RESTRICT kick_ring = vertex_buf->kick_ring;
			const int ofx = inv.xyof.I32[0];
			const int ofy = inv.xyof.I32[1];
			for (u32 j = (count > 2) ? (count - 2) : 0; j < count; j++)
			{
				const GIFPackedReg* RESTRICT rv = rxyz + j * stride;
				const int wx = static_cast<int>(rv->U32[0] & 0xFFFFu) - ofx;
				const int wy = static_cast<int>(rv->U32[1] & 0xFFFFu) - ofy;
				const GSVertexKernels::CullMirrorEntry e = (band_shift == 4) ?
					GSVertexKernels::MakeCullMirrorEntry<true>(wx, wy, inv.bounds, 4, inv.grid.band_bias_x, inv.grid.band_bias_y) :
					GSVertexKernels::MakeCullMirrorEntry<false>(wx, wy, inv.bounds, band_shift, inv.grid.band_bias_x, inv.grid.band_bias_y);
				const u32 slot = (xy_tail0 + j) & 3;
				xy_ring[slot] = BroadcastXY(e.xyp);
				kick_ring[slot] = e;
			}
		}

#ifndef ARCH_ARM64
		(void)fmm_dirty;
		(void)tme;
		(void)fst;
		(void)iip;
#endif

		vertex_buf->head = head;
		vertex_buf->tail = tail;
		vertex_buf->next = next;
		vertex_buf->xy_tail = xy_tail0 + count;
		vertex_buf->fmm_watermark = watermark;
		vertex_buf->fmm_valid = fmm_valid;
		index_buf->tail = itail;

		*acc_state_out = (itail == itail0) ? kAccEmpty : ((itail0 == 0) ? kAccReplace : kAccUnion);
		*native_acc_out = native_acc_rect;
		return acc_rect;
	}
} // namespace GSVertexKickKernel
