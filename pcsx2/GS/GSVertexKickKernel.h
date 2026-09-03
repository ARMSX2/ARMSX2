// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSUtil.h"
#include "GS/GSVertexKick.h"
#include "GS/Renderers/Common/GSVertex.h"

#include <algorithm>

// Two-pass packed-vertex kick kernel for the fused GIF handlers.
//
// The legacy kick is a per-vertex state machine: parse, store, mirror, decide,
// emit, accumulate, with the buffer cursor and the whole of GSState reachable at
// every step. This replaces it, for the three prim types and two packed layouts
// that carry the bulk of every title's vertices, with two loops over a chunk of
// the register run:
//
//   pass one   parses each vertex into the buffer at a PROVISIONAL position
//              (tail at chunk entry, plus the vertex's index in the chunk) and
//              writes one 16-byte side entry: the window position, the outcode,
//              the two pixel bands, and the ADC bit. No state machine, no
//              branches but the loop.
//   pass two   walks the side table with head/tail/next/itail as locals and the
//              three most recent side entries rotating in registers, rejects
//              the majority with bit arithmetic alone, and runs the emission
//              path only for accepted prims -- copying the prim's vertices from
//              their provisional slots down to the position the legacy
//              arithmetic puts them in.
//
// Nothing here calls out to GSState. Anything that needs the rest of it -- the
// per-draw environment snapshot, a flush, a buffer growth, a draw-buffer switch
// -- is the caller's business: the handler runs the legacy kick for those
// vertices and re-enters, and the kernel holds nothing across such a seam.
//
// Exactness (the differential suite in tests/ctest/core/gs/gs_kick_kernel_tests.cpp
// pins all of it): every quantity is computed by the same expression on the same
// inputs as the legacy kick. What changes is the order of independent stores and
// where a value is read from -- the cull decision reads side entries in registers
// instead of the mirror ring in memory, and the min/max accumulate reloads the
// prim's vertices from the (L1-hot) buffer instead of using the parse's
// registers. Provisional slots for rejected vertices always sit at or above the
// eventual tail, which is outside the contract.
namespace GSVertexKickKernel
{
	// Vertices per kernel entry. The side table is 16 bytes a vertex, so this is
	// a 2 KB working set. Bigger amortizes the entry/exit cost further; smaller
	// wastes less parse work when the handler has to break out to a legacy kick.
	static constexpr u32 kChunkVertices = 128;

	// The ADC (skip) bit rides in CullMirrorEntry::meta's spare bits, so the side
	// entry IS a mirror entry: the ring can be filled by copying and
	// CullTestScalar runs on side entries unchanged. Bits 0-27 are the X band,
	// 28-55 the Y band, 56-59 the outcode; 60-63 are unused by the mirror.
	static constexpr u64 kCullMetaAdcBit = 1ull << 60;

	static_assert((kCullMetaAdcBit & (GSVertexKernels::kCullMetaBandXMask |
										 GSVertexKernels::kCullMetaBandYMask |
										 GSVertexKernels::kCullMetaOutcodeMask)) == 0,
		"the ADC bit must not collide with the mirror entry's band or outcode fields");

	// GIFPacked XYZ2/XYZF2 both carry ADC in bit 15 of the fourth word (Skip()),
	// so the bit lands in place with one shift.
	static constexpr u32 kAdcShift = 45;
	static_assert((static_cast<u64>(0x8000u) << kAdcShift) == kCullMetaAdcBit);

	// Batch invariants. Everything here is fixed for the whole of a kernel entry:
	// only a flush, a buffer switch or a register write can change any of them,
	// and none of those can happen inside the kernel.
	struct Invariants
	{
		GSVector4i xyof;                       // {ofx, ofy, ofx, ofy}
		GSVertexKernels::CullBounds bounds;    // banded, i.e. triangle/sprite native res
		u64 uvfog;                             // XYZ2: {UV, FOG}; XYZF2: UV in the low word
		GSVector4i clamp_keep;                 // depth clamp, as two lane masks over m[1]:
		GSVector4i clamp_shifted;              //   m1' = (m1 & keep) | ((m1 >> 8) & shifted)
		u32 scissor_invalid;                   // 0 or 1, OR'd into every prim's skip
		bool clamp_enabled;
		bool nativeres;
		bool tme, fst, iip;
		bool sprite_q_fix;                     // sprite only: !PRIM.FST
	};

	// The buffer cursor, in and out by value.
	struct Cursor
	{
		u32 head, tail, next, itail, xy_tail;
		u32 fmm_watermark;
		u32 acc_state;
		bool fmm_valid;
		GSVector4i acc_rect;
	};

	struct Buffers
	{
		GSVertex* vbuff;
		u16* ibuff;
		GSVertexKernels::CullMirrorEntry* side;     // kChunkVertices entries
		GSVector4i* xy_ring;                        // [4]
		GSVertexKernels::CullMirrorEntry* kick_ring; // [4]
		GSVertexKernels::FmmAcc* fmm_acc;
	};

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
	// Pass one: parse, store, side entry. `clamp` is a template parameter rather
	// than a per-vertex test because the disabled case is the default and must
	// carry no cost at all.
	// ------------------------------------------------------------------------
	template <bool xyzf2, bool clamp>
	__forceinline_odr void PassOne(const GIFPackedReg* RESTRICT r, u32 count,
		GSVertex* RESTRICT out, GSVertexKernels::CullMirrorEntry* RESTRICT side, const Invariants& inv)
	{
		const u64 uvfog = inv.uvfog;
		const int ofx = inv.xyof.I32[0];
		const int ofy = inv.xyof.I32[1];
		const int bl = inv.bounds.l, bt = inv.bounds.t, br = inv.bounds.r, bb = inv.bounds.b;
		const GSVector4i keep = inv.clamp_keep;
		const GSVector4i shifted = inv.clamp_shifted;
#ifdef ARCH_ARM64
		// Hoisted out of the loop on purpose: left inside the parse they are
		// function-local statics that clang rematerializes from the frame every
		// iteration.
		const GSVertexKernels::PackedParseConsts kc = GSVertexKernels::MakePackedParseConsts();
#endif

		for (u32 i = 0; i < count; i++)
		{
			const GIFPackedReg* RESTRICT rv = r + i * 3;

			GSVector4i m0, m1;
#ifdef ARCH_ARM64
			if constexpr (xyzf2)
				GSVertexKernels::ParsePackedSTQRGBAXYZF2_Neon(rv, static_cast<u32>(uvfog), kc, m0, m1);
			else
				GSVertexKernels::ParsePackedSTQRGBAXYZ2_Neon(rv, uvfog, kc, m0, m1);
#else
			if constexpr (xyzf2)
				GSVertexKernels::ParsePackedSTQRGBAXYZF2(rv, static_cast<u32>(uvfog), m0, m1);
			else
				GSVertexKernels::ParsePackedSTQRGBAXYZ2(rv, uvfog, m0, m1);
#endif

			if constexpr (clamp)
				m1 = (m1 & keep) | (m1.srl32<8>() & shifted);

			out[i].m[0] = m0;
			out[i].m[1] = m1;

			// The window position and its cull metadata, scalar (it dual-issues
			// against the NEON parse). Same expressions as MakeCullMirrorEntry,
			// with the ADC bit folded into the spare meta bits.
			const u32 raw = rv[2].U32[0];
			const u32 raw_y = rv[2].U32[1];
			const int wx = static_cast<int>(raw & 0xFFFFu) - ofx;
			const int wy = static_cast<int>(raw_y & 0xFFFFu) - ofy;
			const int bx = (wx - 1) >> 4;
			const int by = (wy - 1) >> 4;

			u32 oc = 0;
			oc |= (bx < bl) ? 1u : 0u;
			oc |= (bx >= br) ? 2u : 0u;
			oc |= (by < bt) ? 4u : 0u;
			oc |= (by >= bb) ? 8u : 0u;

			side[i].xyp = static_cast<u64>(static_cast<u32>(wx)) | (static_cast<u64>(static_cast<u32>(wy)) << 32);
			side[i].meta = (static_cast<u64>(static_cast<u32>(bx)) & GSVertexKernels::kCullMetaBandXMask) |
			               ((static_cast<u64>(static_cast<u32>(by)) << 28) & GSVertexKernels::kCullMetaBandYMask) |
			               (static_cast<u64>(oc) << 56) |
			               (static_cast<u64>(rv[2].U32[3] & 0x8000u) << kAdcShift);
		}
	}

	// ------------------------------------------------------------------------
	// The kernel. `prim` is one of GS_TRIANGLESTRIP / GS_TRIANGLELIST /
	// GS_SPRITE; `xyzf2` selects the packed layout. The caller guarantees:
	//   * itail != 0, so the per-draw environment snapshot cannot fire inside;
	//   * m_recent_buffer_switch is clear or draw buffering is off;
	//   * the scalar-outcode cull applies (native res, no AA1 expansion);
	//   * tail + count + 3 <= maxcount, so no growth can be needed;
	//   * tail + count < MaxVerticesForPrim, so no VERTEXCOUNT flush can be
	//     needed.
	// Every vertex of the chunk is consumed; the caller advances by `count`.
	// ------------------------------------------------------------------------
	template <u32 prim, bool xyzf2>
	__noinline Cursor RunChunk(const GIFPackedReg* RESTRICT rin, u32 count,
		const Buffers& bufs, const Invariants& inv, Cursor cur)
	{
		constexpr u32 n = (prim == GS_SPRITE) ? 2u : 3u;
		constexpr int primclass = GSUtil::GetPrimClass(prim);
		constexpr bool strip = (prim == GS_TRIANGLESTRIP);
		static_assert(prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLELIST || prim == GS_SPRITE);

		GSVertex* RESTRICT vbuff = bufs.vbuff;
		u16* RESTRICT ibuff = bufs.ibuff;
		GSVertexKernels::CullMirrorEntry* RESTRICT side = bufs.side;

		const u32 tail0 = cur.tail;

		if (inv.clamp_enabled)
			PassOne<xyzf2, true>(rin, count, vbuff + tail0, side, inv);
		else
			PassOne<xyzf2, false>(rin, count, vbuff + tail0, side, inv);

		// ---- pass two -------------------------------------------------------
		u32 head = cur.head;
		u32 tail = cur.tail;
		u32 next = cur.next;
		u32 itail = cur.itail;
		u32 watermark = cur.fmm_watermark;
		bool fmm_valid = cur.fmm_valid;
		u32 acc_state = cur.acc_state;
		GSVector4i acc_rect = cur.acc_rect;

		const u32 scissor_invalid = inv.scissor_invalid;
		const bool nativeres = inv.nativeres;
		const bool tme = inv.tme;
		const bool fst = inv.fst;
		const bool iip = inv.iip;

		GSVertexKernels::FmmAcc acc;
		bool fmm_dirty = false;
#ifdef ARCH_ARM64
		if (primclass == GS_TRIANGLE_CLASS && fmm_valid)
		{
			// Only meaningful while fmm_valid; the reset at the first emission of
			// a draw initializes it for real.
			acc = *bufs.fmm_acc;
		}
		else
#endif
		{
			GSVertexKernels::FmmAccReset(acc, false, false);
		}

		// The three most recent mirror entries, most recent first. Seeded from the
		// ring because a chunk can begin mid-prim; after that they rotate in
		// registers and the ring is not read again.
		GSVertexKernels::CullMirrorEntry e0 = bufs.kick_ring[(cur.xy_tail - 1) & 3];
		GSVertexKernels::CullMirrorEntry e1 = bufs.kick_ring[(cur.xy_tail - 2) & 3];
		GSVertexKernels::CullMirrorEntry e2 = bufs.kick_ring[(cur.xy_tail - 3) & 3];

		for (u32 i = 0; i < count; i++)
		{
			e2 = e1;
			e1 = e0;
			e0 = side[i];

			// Move the vertex from its provisional slot to the live tail, so the
			// buffer below tail is what the per-vertex kick would have left there
			// -- including the slots a rejected strip vertex passes through, which
			// nothing indexes but the front-end instrument records. The live tail
			// never runs ahead of the provisional cursor, so the destination is at
			// or below the source and a later vertex's source is never written
			// over; when they coincide (an unbroken run of accepts, or any chunk
			// with no compaction in it) this is a self-copy the store buffer eats.
			vbuff[tail] = vbuff[tail0 + i];

			tail++;
			if ((tail - head) < n)
				continue;

			u32 skip = scissor_invalid | static_cast<u32>((e0.meta >> 60) & 1u);
			if (skip == 0)
				skip = GSVertexKernels::CullTestScalar<n, primclass>(e0, e1, e2);

			if (skip != 0)
			{
				// A rejected strip vertex stays in the buffer until the next
				// accept compacts over it; list classes rewind outright.
				if constexpr (strip)
					head = head + 1;
				else
					tail = head;
				continue;
			}

			// The strip compaction, exactly as the per-vertex kick does it: the
			// window slides down over the vertices the rejections left behind.
			u32 dst = head;
			if constexpr (strip)
			{
				if (next < head)
				{
					vbuff[next + 0] = vbuff[head + 0];
					vbuff[next + 1] = vbuff[head + 1];
					vbuff[next + 2] = vbuff[head + 2];
					dst = next;
#ifdef ARCH_ARM64
					// Vertices moved below the fused-FMM watermark must re-accumulate.
					watermark = std::min(watermark, next);
#endif
				}
			}

			const GSVector4i bbox = GSVertexKernels::ComputeCullBBox<n, primclass>(
				BroadcastXY(e0.xyp), BroadcastXY(e1.xyp), BroadcastXY(e2.xyp), nativeres, false);

			u16* RESTRICT ib = ibuff + itail;
			if constexpr (prim == GS_TRIANGLESTRIP)
			{
				ib[0] = static_cast<u16>(dst + 0);
				ib[1] = static_cast<u16>(dst + 1);
				ib[2] = static_cast<u16>(dst + 2);
				head = dst + 1;
				next = dst + 3;
				tail = dst + 3;
				itail += 3;
			}
			else if constexpr (prim == GS_TRIANGLELIST)
			{
				ib[0] = static_cast<u16>(dst + 0);
				ib[1] = static_cast<u16>(dst + 1);
				ib[2] = static_cast<u16>(dst + 2);
				head = dst + 3;
				next = dst + 3;
				tail = dst + 3;
				itail += 3;
			}
			else
			{
				ib[0] = static_cast<u16>(dst + 0);
				ib[1] = static_cast<u16>(dst + 1);
				if (inv.sprite_q_fix)
					vbuff[dst + 0].RGBAQ.Q = vbuff[dst + 1].RGBAQ.Q;
				head = dst + 2;
				next = dst + 2;
				tail = dst + 2;
				itail += 2;
			}

#ifdef ARCH_ARM64
			if constexpr (primclass == GS_TRIANGLE_CLASS)
			{
				const u32 last = tail - 1;
				if (itail == n)
				{
					GSVertexKernels::FmmAccReset(acc, tme, fst);
					fmm_valid = true;
					watermark = last - 2;
				}

				if (fmm_valid)
				{
					for (u32 j = std::max(watermark, last - 2); j < last; j++)
					{
						GSVertexKernels::FmmAccumVertex(acc, GSVector4i(vbuff[j].m[0]),
							GSVector4i(vbuff[j].m[1]), tme, fst, iip);
					}
					GSVertexKernels::FmmAccumVertex(acc, GSVector4i(vbuff[last].m[0]),
						GSVector4i(vbuff[last].m[1]), tme, fst, true);
					watermark = last + 1;
					fmm_dirty = true;
				}
			}
#endif

			const GSVector4i draw_rect = bbox.sra32<4>() + GSVector4i(0, 0, 1, 1);
			if (acc_state != 0)
			{
				acc_rect = acc_rect.runion(draw_rect);
			}
			else
			{
				acc_rect = draw_rect;
				acc_state = (itail == n) ? 2 : 1;
			}
		}

		// ---- exit -----------------------------------------------------------
		// The rings hold the last four kicked vertices. Nothing reads them
		// mid-chunk: every reader runs inside a flush, and a flush only happens
		// inside a legacy kick.
		{
			const u32 first = (count > 4) ? (count - 4) : 0;
			for (u32 j = first; j < count; j++)
			{
				const u32 slot = (cur.xy_tail + j) & 3;
				bufs.xy_ring[slot] = BroadcastXY(side[j].xyp);
				// The ring's entries carry no ADC bit -- the legacy kick's do not
				// and the differential test compares the bytes.
				bufs.kick_ring[slot].xyp = side[j].xyp;
				bufs.kick_ring[slot].meta = side[j].meta & ~kCullMetaAdcBit;
			}
		}

#ifdef ARCH_ARM64
		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			if (fmm_dirty)
				*bufs.fmm_acc = acc;
		}
#else
		(void)fmm_dirty;
#endif

		Cursor out;
		out.head = head;
		out.tail = tail;
		out.next = next;
		out.itail = itail;
		out.xy_tail = cur.xy_tail + count;
		out.fmm_watermark = watermark;
		out.acc_state = acc_state;
		out.fmm_valid = fmm_valid;
		out.acc_rect = acc_rect;
		return out;
	}
} // namespace GSVertexKickKernel
