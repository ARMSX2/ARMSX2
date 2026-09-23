// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "GS/GSVector.h"

#include <cfloat>
#include <limits>

// Pure kernels backing the fused GIF packed vertex handlers and the per-prim
// accept/cull decision in GSState::VertexKick. Factored out of GSState.cpp so the
// gs_vertex_tests oracle suite can drive them standalone. (During the GV campaign
// the optimized kernels were additionally crosschecked against the legacy ones
// per vertex/prim over live replays — that plumbing is gone; recover the
// GS_VERTEX_CROSSCHECK machinery from git history if a divergence hunt ever
// needs it again.)
//
// Anything here must stay a pure function of its arguments: no GSState members,
// no config reads. Behavioral contract for the legacy kernels is pinned by
// tests/ctest/core/gs/gs_vertex_tests.cpp against an independent scalar model.
namespace GSVertexKernels
{
	// Parse one packed {STQ, RGBAQ, XYZF2} record (r[0..2]) into GSVertex m[0]/m[1].
	// uv = the latched UV register value (packed XYZF2 does not write UV). Q == +0.0
	// (integer compare, so -0.0 passes through) is rewritten to FLT_MIN to avoid
	// divides by zero downstream — matches GIFPackedRegHandlerSTQ.
	__forceinline_odr void ParsePackedSTQRGBAXYZF2(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
		const GSVector4i st = GSVector4i::loadl(&r[0].U64[0]);
		GSVector4i q = GSVector4i::loadl(&r[0].U64[1]);
		const GSVector4i rgba = (GSVector4i::load<false>(&r[1]) & GSVector4i::x000000ff()).ps32().pu16();

		q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

		m0 = st.upl64(rgba.upl32(q));

		GSVector4i xy = GSVector4i::loadl(&r[2].U64[0]);
		GSVector4i zf = GSVector4i::loadl(&r[2].U64[1]);
		xy = xy.upl16(xy.srl<4>()).upl32(GSVector4i::load((int)uv));
		zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());

		m1 = xy.upl32(zf);
	}

	// Parse one packed {STQ, RGBAQ, XYZ2} record. Z is the full 32 bits; UV and FOG
	// are both preserved from the current vertex state (passed packed as {UV, FOG}).
	__forceinline_odr void ParsePackedSTQRGBAXYZ2(const GIFPackedReg* RESTRICT r, u64 uvfog, GSVector4i& m0, GSVector4i& m1)
	{
		const GSVector4i st = GSVector4i::loadl(&r[0].U64[0]);
		GSVector4i q = GSVector4i::loadl(&r[0].U64[1]);
		const GSVector4i rgba = (GSVector4i::load<false>(&r[1]) & GSVector4i::x000000ff()).ps32().pu16();

		q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

		m0 = st.upl64(rgba.upl32(q));

		const GSVector4i xy = GSVector4i::loadl(&r[2].U64[0]);
		const GSVector4i z = GSVector4i::loadl(&r[2].U64[1]);
		const GSVector4i xyz = xy.upl16(xy.srl<4>()).upl32(z);

		m1 = xyz.upl64(GSVector4i::loadl(&uvfog));
	}

#ifdef ARCH_ARM64
	// aarch64-native parse: the whole vertex build is byte movement, so one TBL
	// gathers each qword (the legacy path spends ~11 NEON ops on the RGBA
	// pack chain alone). Out-of-range TBL indices read as zero, which provides the
	// 24-bit Z and 8-bit F masks for free. Bit-identical to the legacy kernels —
	// pinned by gs_vertex_tests.
	//
	// The TBL patterns and the Q fix-up constant are lifted into a struct a caller
	// can hoist out of a batch loop. As function-local statics inside a
	// force-inlined parse, clang materializes them in the caller's frame and
	// reloads all three every iteration (measured: `ldur q0, [x29,#-112]` and two
	// siblings in the fused handler's loop). A batch loop passes them in once.
	struct PackedParseConsts
	{
		uint8x16_t pat_m0;   // {S, T, RGBA, Q}, shared by both layouts
		uint8x16_t pat_m1;   // XYZF2: {X|Y<<16, Z, -, F}
		uint8x16_t pat_xyz;  // XYZ2:  {X|Y<<16, Z32} into the low half
		uint32x4_t q_fixup;  // Q == +0.0 rewrites to FLT_MIN
	};

	__forceinline_odr PackedParseConsts MakePackedParseConsts()
	{
		alignas(16) static constexpr u8 pat_m0[16] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 20, 24, 28, 8, 9, 10, 11};
		alignas(16) static constexpr u8 pat_m1[16] = {0, 1, 4, 5, 24, 25, 26, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 28, 0xFF, 0xFF, 0xFF};
		alignas(16) static constexpr u8 pat_xyz[16] = {0, 1, 4, 5, 8, 9, 10, 11, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		alignas(16) static constexpr u32 q_fixup[4] = {0, 0, 0, 0x00800000};

		PackedParseConsts k;
		k.pat_m0 = vld1q_u8(pat_m0);
		k.pat_m1 = vld1q_u8(pat_m1);
		k.pat_xyz = vld1q_u8(pat_xyz);
		k.q_fixup = vld1q_u32(q_fixup);
		return k;
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Neon(const GIFPackedReg* RESTRICT r, u32 uv,
		const PackedParseConsts& k, GSVector4i& m0, GSVector4i& m1)
	{
		const uint8x16x2_t st_rgba = {vld1q_u8(reinterpret_cast<const u8*>(r + 0)), vld1q_u8(reinterpret_cast<const u8*>(r + 1))};
		uint32x4_t v0 = vreinterpretq_u32_u8(vqtbl2q_u8(st_rgba, k.pat_m0));
		v0 = vorrq_u32(v0, vandq_u32(vceqzq_u32(v0), k.q_fixup));

		const uint8x16_t xyzf = vld1q_u8(reinterpret_cast<const u8*>(r + 2));
		const uint8x16x2_t xyzf_pair = {xyzf, vreinterpretq_u8_u32(vshrq_n_u32(vreinterpretq_u32_u8(xyzf), 4))};
		uint32x4_t v1 = vreinterpretq_u32_u8(vqtbl2q_u8(xyzf_pair, k.pat_m1));
		v1 = vsetq_lane_u32(uv, v1, 2);

		m0 = GSVector4i(vreinterpretq_s32_u32(v0));
		m1 = GSVector4i(vreinterpretq_s32_u32(v1));
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZ2_Neon(const GIFPackedReg* RESTRICT r, u64 uvfog,
		const PackedParseConsts& k, GSVector4i& m0, GSVector4i& m1)
	{
		const uint8x16x2_t st_rgba = {vld1q_u8(reinterpret_cast<const u8*>(r + 0)), vld1q_u8(reinterpret_cast<const u8*>(r + 1))};
		uint32x4_t v0 = vreinterpretq_u32_u8(vqtbl2q_u8(st_rgba, k.pat_m0));
		v0 = vorrq_u32(v0, vandq_u32(vceqzq_u32(v0), k.q_fixup));

		const uint8x16_t xyz = vqtbl1q_u8(vld1q_u8(reinterpret_cast<const u8*>(r + 2)), k.pat_xyz);
		const uint64x2_t v1 = vsetq_lane_u64(uvfog, vreinterpretq_u64_u8(xyz), 1);

		m0 = GSVector4i(vreinterpretq_s32_u32(v0));
		m1 = GSVector4i(vreinterpretq_s32_u64(v1));
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Neon(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
		ParsePackedSTQRGBAXYZF2_Neon(r, uv, MakePackedParseConsts(), m0, m1);
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZ2_Neon(const GIFPackedReg* RESTRICT r, u64 uvfog, GSVector4i& m0, GSVector4i& m1)
	{
		ParsePackedSTQRGBAXYZ2_Neon(r, uvfog, MakePackedParseConsts(), m0, m1);
	}
#endif // ARCH_ARM64

	// Dispatchers the fused handlers call: aarch64 takes the TBL kernels, x86
	// keeps the legacy path.
	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Fast(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
#ifdef ARCH_ARM64
		ParsePackedSTQRGBAXYZF2_Neon(r, uv, m0, m1);
#else
		ParsePackedSTQRGBAXYZF2(r, uv, m0, m1);
#endif
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZ2_Fast(const GIFPackedReg* RESTRICT r, u64 uvfog, GSVector4i& m0, GSVector4i& m1)
	{
#ifdef ARCH_ARM64
		ParsePackedSTQRGBAXYZ2_Neon(r, uvfog, m0, m1);
#else
		ParsePackedSTQRGBAXYZ2(r, uvfog, m0, m1);
#endif
	}

	// ------------------------------------------------------------------------
	// The packed layouts the fused handlers carry (stage 3c).
	//
	// Every one of them is the same vertex build from the same three sources --
	// {S, T}, the colour, and the position -- differing only in where those
	// sources sit in the record and in which of them the tag omits. An omitted
	// source is not a partial vertex: the field keeps the value the previous
	// write latched, which is what the per-qword path leaves in m_v, so the
	// parse takes it from a carried vector instead of from the stream.
	//
	// The two contiguous triples are the shipped layouts and their stride and
	// offsets are compile-time, so their code is what it was; the rest read a
	// GIFPackedLayout the tag classified once.
	// ------------------------------------------------------------------------
	enum class PackedLayout : u32
	{
		TripleXYZF2,    // {ST, RGBAQ, XYZF2}, contiguous
		TripleXYZ2,     // {ST, RGBAQ, XYZ2}, contiguous
		NopTripleXYZF2, // the same three with NOP descriptors between them
		PairSTQXYZ2,    // {ST, XYZ2}: colour and Q carried
		PairUVXYZ2,     // {UV, XYZ2}: ST, colour and Q carried
		PairRGBAQXYZ2,  // {RGBAQ, XYZ2}: ST carried, Q from the latch
	};

	constexpr bool LayoutIsXYZF2(PackedLayout l)
	{
		return l == PackedLayout::TripleXYZF2 || l == PackedLayout::NopTripleXYZF2;
	}
	constexpr bool LayoutIsContiguousTriple(PackedLayout l)
	{
		return l == PackedLayout::TripleXYZF2 || l == PackedLayout::TripleXYZ2;
	}
	constexpr bool LayoutIsTriple(PackedLayout l)
	{
		return LayoutIsContiguousTriple(l) || l == PackedLayout::NopTripleXYZF2;
	}
	// Whether the record carries an ST descriptor, and therefore whether the tag
	// moves the latched Q that the next RGBAQ write will read.
	constexpr bool LayoutLatchesQ(PackedLayout l)
	{
		return LayoutIsTriple(l) || l == PackedLayout::PairSTQXYZ2;
	}
	// Whether the parse needs the carried m[0].
	constexpr bool LayoutCarriesM0(PackedLayout l) { return !LayoutIsTriple(l); }

	template <PackedLayout L>
	__forceinline_odr u32 LayoutStride(const GIFPackedLayout& o)
	{
		if constexpr (LayoutIsContiguousTriple(L))
			return 3;
		else
			return o.stride;
	}
	template <PackedLayout L>
	__forceinline_odr u32 LayoutOffA(const GIFPackedLayout& o)
	{
		if constexpr (LayoutIsContiguousTriple(L))
			return 0;
		else
			return o.off_a;
	}
	template <PackedLayout L>
	__forceinline_odr u32 LayoutOffRgba(const GIFPackedLayout& o)
	{
		if constexpr (LayoutIsContiguousTriple(L))
			return 1;
		else
			return o.off_rgba;
	}
	template <PackedLayout L>
	__forceinline_odr u32 LayoutOffXyz(const GIFPackedLayout& o)
	{
		if constexpr (LayoutIsContiguousTriple(L))
			return 2;
		else
			return o.off_xyz;
	}

	// The carried m[0] a pair layout parses against.
	//
	//   PairSTQ / PairUV  m_v.m[0] as it stands: lanes 2 and 3 are the colour and
	//                     the vertex's own Q, neither of which such a tag writes.
	//   PairRGBAQ         m_v.m[0] with LANE 2 SET TO THE LATCHED Q. That is not
	//                     cosmetic: GIFPackedRegHandlerRGBA writes RGBAQ.Q = m_q,
	//                     and putting m_q there lets the parse reuse the shipped
	//                     {S, T, RGBA, Q} table pattern, whose last lane comes
	//                     from the carry's lane 2.
	__forceinline_odr GSVector4i MakeLayoutCarry(PackedLayout l, const GSVector4i& v_m0, float q)
	{
		GSVector4i c = v_m0;
		if (l == PackedLayout::PairRGBAQXYZ2)
			std::memcpy(&c.U32[2], &q, sizeof(q));
		return c;
	}

	// {UV, FOG} for one record: the carried pair, with the UV half replaced by the
	// record's own when the layout carries a UV descriptor. Same masking and pack
	// as GIFPackedRegHandlerUV.
	template <PackedLayout L>
	__forceinline_odr u64 LayoutUVFog(const GIFPackedReg* RESTRICT rv, u32 off_a, u64 uvfog)
	{
		if constexpr (L == PackedLayout::PairUVXYZ2)
		{
			u64 w;
			std::memcpy(&w, &rv[off_a], sizeof(w));
			return (uvfog & 0xFFFFFFFF00000000ull) | (w & 0x3fffull) | ((w >> 16) & 0x3fff0000ull);
		}
		else
		{
			(void)rv;
			(void)off_a;
			return uvfog;
		}
	}

	// One record, portable. `carry` is MakeLayoutCarry's result; `uvfog` the
	// carried {UV, FOG}.
	template <PackedLayout L>
	__forceinline_odr void ParsePackedRecord(const GIFPackedReg* RESTRICT rv, const GIFPackedLayout& off,
		u64 uvfog, const GSVector4i& carry, GSVector4i& m0, GSVector4i& m1)
	{
		if constexpr (LayoutIsContiguousTriple(L))
		{
			if constexpr (L == PackedLayout::TripleXYZF2)
				ParsePackedSTQRGBAXYZF2(rv, static_cast<u32>(uvfog), m0, m1);
			else
				ParsePackedSTQRGBAXYZ2(rv, uvfog, m0, m1);
			return;
		}
		else
		{
			const u32 off_a = LayoutOffA<L>(off);
			const u32 off_xyz = LayoutOffXyz<L>(off);

			if constexpr (LayoutIsTriple(L))
			{
				static_assert(L == PackedLayout::NopTripleXYZF2, "the contiguous triples took the branch above");

				const GSVector4i st = GSVector4i::loadl(&rv[off_a].U64[0]);
				GSVector4i q = GSVector4i::loadl(&rv[off_a].U64[1]);
				const GSVector4i rgba =
					(GSVector4i::load<false>(&rv[LayoutOffRgba<L>(off)]) & GSVector4i::x000000ff()).ps32().pu16();
				q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());
				// GIFPackedRegHandlerSTQ applies a second fix-up, NaN to FLT_MAX,
				// and the next RGBAQ write copies the fixed-up latch into the
				// vertex. So the per-qword path this layout replaces puts the
				// replaced value in the vertex, and so must we. (The two
				// contiguous triples above skip it; that divergence is inherited
				// from upstream and is pinned as it stands.)
				q = GSVector4i::cast(GSVector4::cast(q).replace_nan(GSVector4::m_max));
				m0 = st.upl64(rgba.upl32(q));
			}
			else
			{
				GSVector4i v = carry;
				if constexpr (L == PackedLayout::PairSTQXYZ2)
				{
					v.U64[0] = rv[off_a].U64[0];
				}
				else if constexpr (L == PackedLayout::PairRGBAQXYZ2)
				{
					const GSVector4i mask = GSVector4i::load(0x0c080400);
					const GSVector4i rgba = GSVector4i::load<false>(&rv[off_a]).shuffle8(mask);
					v.U32[3] = carry.U32[2]; // the latch, where the carry parks it
					v.U32[2] = static_cast<u32>(GSVector4i::store(rgba));
				}
				m0 = v;
			}

			const GSVector4i xy = GSVector4i::loadl(&rv[off_xyz].U64[0]);
			if constexpr (LayoutIsXYZF2(L))
			{
				GSVector4i zf = GSVector4i::loadl(&rv[off_xyz].U64[1]);
				const GSVector4i xyuv =
					xy.upl16(xy.srl<4>()).upl32(GSVector4i::load(static_cast<int>(uvfog)));
				zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());
				m1 = xyuv.upl32(zf);
			}
			else
			{
				const u64 uvf = LayoutUVFog<L>(rv, off_a, uvfog);
				const GSVector4i z = GSVector4i::loadl(&rv[off_xyz].U64[1]);
				const GSVector4i xyz = xy.upl16(xy.srl<4>()).upl32(z);
				m1 = xyz.upl64(GSVector4i::loadl(&uvf));
			}
		}
	}

#ifdef ARCH_ARM64
	// The same, with the table patterns hoisted -- what pass one and the batch
	// loops call. Byte-identical to the portable build above; gs_vertex_tests and
	// the differential suite pin both against the per-qword handlers.
	template <PackedLayout L>
	__forceinline_odr void ParsePackedRecord_Neon(const GIFPackedReg* RESTRICT rv, const GIFPackedLayout& off,
		u64 uvfog, const GSVector4i& carry, const PackedParseConsts& k, GSVector4i& m0, GSVector4i& m1)
	{
		if constexpr (L == PackedLayout::TripleXYZF2)
		{
			ParsePackedSTQRGBAXYZF2_Neon(rv, static_cast<u32>(uvfog), k, m0, m1);
		}
		else if constexpr (L == PackedLayout::TripleXYZ2)
		{
			ParsePackedSTQRGBAXYZ2_Neon(rv, uvfog, k, m0, m1);
		}
		else
		{
			const u32 off_a = LayoutOffA<L>(off);
			const u32 off_xyz = LayoutOffXyz<L>(off);

			// m[0].
			if constexpr (LayoutIsTriple(L))
			{
				static_assert(L == PackedLayout::NopTripleXYZF2, "the contiguous triples took the branches above");

				const uint8x16x2_t st_rgba = {vld1q_u8(reinterpret_cast<const u8*>(rv + off_a)),
					vld1q_u8(reinterpret_cast<const u8*>(rv + LayoutOffRgba<L>(off)))};
				uint32x4_t v0 = vreinterpretq_u32_u8(vqtbl2q_u8(st_rgba, k.pat_m0));
				v0 = vorrq_u32(v0, vandq_u32(vceqzq_u32(v0), k.q_fixup));

				// The second STQ fix-up: a NaN Q becomes FLT_MAX. pat_m0 puts Q in
				// lane 3, so the compare's other three lanes -- S, T and the packed
				// colour, any of which can carry a NaN bit pattern -- are masked
				// out. FCMEQ is false for a NaN operand, so BIC of the lane mask
				// with it leaves ones only in a lane 3 that is NaN, and BSL takes
				// FLT_MAX there and the parsed bits everywhere else.
				// See the portable branch above for why this layout needs it and
				// the contiguous triples do not.
				const float32x4_t f = vreinterpretq_f32_u32(v0);
				const uint32x4_t q_lane = vsetq_lane_u32(0xFFFFFFFFu, vdupq_n_u32(0), 3);
				const uint32x4_t q_is_nan = vbicq_u32(q_lane, vceqq_f32(f, f));
				v0 = vbslq_u32(q_is_nan, vreinterpretq_u32_f32(vdupq_n_f32(FLT_MAX)), v0);

				m0 = GSVector4i(vreinterpretq_s32_u32(v0));
			}
			else if constexpr (L == PackedLayout::PairSTQXYZ2)
			{
				// {S, T} over the carry's colour and Q: one load and one insert.
				u64 st;
				std::memcpy(&st, &rv[off_a], sizeof(st));
				m0 = GSVector4i(vreinterpretq_s32_u64(
					vsetq_lane_u64(st, vreinterpretq_u64_s32(carry.v4s), 0)));
			}
			else if constexpr (L == PackedLayout::PairRGBAQXYZ2)
			{
				// pat_m0 is {S, T} from the first vector, the colour packed out of
				// the second's four low bytes, and the first's lane 2 last -- which
				// is where MakeLayoutCarry parks the latched Q. So the shipped
				// pattern builds this layout's vertex unchanged, with no fix-up:
				// the carried Q has already had one applied by whoever latched it.
				const uint8x16x2_t carry_rgba = {vreinterpretq_u8_s32(carry.v4s),
					vld1q_u8(reinterpret_cast<const u8*>(rv + off_a))};
				m0 = GSVector4i(vreinterpretq_s32_u8(vqtbl2q_u8(carry_rgba, k.pat_m0)));
			}
			else
			{
				static_assert(L == PackedLayout::PairUVXYZ2);
				m0 = carry;
			}

			// m[1].
			if constexpr (LayoutIsXYZF2(L))
			{
				const uint8x16_t xyzf = vld1q_u8(reinterpret_cast<const u8*>(rv + off_xyz));
				const uint8x16x2_t xyzf_pair = {
					xyzf, vreinterpretq_u8_u32(vshrq_n_u32(vreinterpretq_u32_u8(xyzf), 4))};
				uint32x4_t v1 = vreinterpretq_u32_u8(vqtbl2q_u8(xyzf_pair, k.pat_m1));
				v1 = vsetq_lane_u32(static_cast<u32>(uvfog), v1, 2);
				m1 = GSVector4i(vreinterpretq_s32_u32(v1));
			}
			else
			{
				const uint8x16_t xyz =
					vqtbl1q_u8(vld1q_u8(reinterpret_cast<const u8*>(rv + off_xyz)), k.pat_xyz);
				const uint64x2_t v1 =
					vsetq_lane_u64(LayoutUVFog<L>(rv, off_a, uvfog), vreinterpretq_u64_u8(xyz), 1);
				m1 = GSVector4i(vreinterpretq_s32_u64(v1));
			}
		}
	}
#endif // ARCH_ARM64

	// The dispatcher the per-vertex paths call.
	template <PackedLayout L>
	__forceinline_odr void ParsePackedRecord_Fast(const GIFPackedReg* RESTRICT rv, const GIFPackedLayout& off,
		u64 uvfog, const GSVector4i& carry, GSVector4i& m0, GSVector4i& m1)
	{
#ifdef ARCH_ARM64
		ParsePackedRecord_Neon<L>(rv, off, uvfog, carry, MakePackedParseConsts(), m0, m1);
#else
		ParsePackedRecord<L>(rv, off, uvfog, carry, m0, m1);
#endif
	}

	// ------------------------------------------------------------------------
	// The cull grid: the sub-texel spacing of the device sample points a prim has
	// to span to paint anything.
	//
	// A prim whose bounding box holds no sample point covers no device pixel, so
	// it can be dropped before it becomes part of a draw -- and a draw all of
	// whose prims go that way never happens at all. The grid is a power-of-two
	// step in 12.4 sub-texels, carried as the two vectors the interior rounding
	// needs plus its log2:
	//
	//   4  the native pixel centres, which are the device sample points at scale
	//      1. This is what the shipped code selected with a `nativeres` bool.
	//   3  2x.  2  4x.  1  8x. Exact at each of those, because the sample points
	//      really are the multiples of 16 >> shift there -- see
	//      GSState::ConfigCullGrid for the two conditions that have to hold.
	//   0  no grid: keep every prim with any sub-texel extent, which is what every
	//      non-native scale used to get and what a scale or a half-pixel-offset
	//      mode whose sample points are not a power-of-two grid still gets. It is
	//      spelled out as its own branch rather than falling out of the general
	//      formula at step 1: the two disagree about a bottom/right edge sitting
	//      exactly on a pixel boundary, and that is behaviour, not rounding.
	//
	// The sprite class carries its own shift because sprite geometry moves between
	// the cull and the raster: at any upscale CorrectSpriteCoverageForUpscale
	// pushes a sprite's far edge out to the next whole pixel, so a grid decision
	// taken here would be taken on coordinates nothing rasterises. At scale 1 that
	// pass returns early and sprites keep the pixel-centre grid.
	//
	// Invariant: sprite_shift is either equal to shift or 0, so the round vectors
	// are valid for whichever class ends up using them.
	//
	// The grid also carries a per-axis PHASE, in sub-texels: the sample points are
	// `phase + k*step`, not `k*step`. Every half-pixel-offset mode whose window
	// constant is half a device pixel has phase 0, and HalfPixelOffset::Native --
	// whose align-to-native branch puts the constant at S/2 -- has phase step/2,
	// the odd multiples of half the device step (sub-texel 8k+4 at 2x, 4k+2 at 4x).
	// See GSState::CullGridFor for the derivation from the mode's ox2.
	//
	// Invariant: phase is zero at shift 4 (native, where 1x byte identity is
	// structural) and at shift 0 (no grid), and 0 <= phase < step otherwise. The
	// sprite class therefore needs no phase of its own: it only ever carries the
	// native grid.
	struct CullGrid
	{
		GSVector4i round_add;  // (step - 1, step - 1, -1, -1)
		GSVector4i round_mask; // ~(step - 1)
		GSVector4i phase;      // (phase_x, phase_y, phase_x, phase_y)
		int shift;             // triangle class
		int sprite_shift;      // sprite class
		int band_bias_x;       // phase_x + 1, the constant the band expression subtracts
		int band_bias_y;       // phase_y + 1

		template <int primclass>
		__forceinline_odr int ShiftFor() const
		{
			return (primclass == GS_SPRITE_CLASS) ? sprite_shift : shift;
		}
	};

	// The device sample grid at an exact power-of-two upscale, as a log2 sub-texel
	// step. 0 means the scale's sample points are not a sub-texel grid at all and
	// nothing can be culled on them -- see GSState::ConfigCullGrid.
	__forceinline_odr int DeviceCullGridShift(float scale)
	{
		if (scale == 1.0f)
			return 4;
		if (scale == 2.0f)
			return 3;
		if (scale == 4.0f)
			return 2;
		if (scale == 8.0f)
			return 1;
		return 0;
	}

	__forceinline_odr CullGrid MakeCullGrid(int shift, int sprite_shift, int phase_x = 0, int phase_y = 0)
	{
		const int step = 1 << shift;
		return {GSVector4i(step - 1, step - 1, -1, -1), GSVector4i(~(step - 1)),
			GSVector4i(phase_x, phase_y, phase_x, phase_y), shift, sprite_shift, phase_x + 1, phase_y + 1};
	}

	// Raw bounding box of one completed prim's window entries, no rounding.
	template <u32 n>
	__forceinline_odr GSVector4i CullPrimBounds(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2)
	{
		if constexpr (n == 1)
			return v0;
		else if constexpr (n == 2)
			return v0.runion(v1);
		else
		{
			static_assert(n == 3);
			return v0.runion(v1).runion(v2);
		}
	}

	// Snap a bbox inwards onto the grid: top/left up to the first sample point at
	// or past the edge, bottom/right down to the last one strictly inside, then +1
	// on bottom/right so rempty() reads "spans no sample point".
	__forceinline_odr GSVector4i RoundToCullGrid(const GSVector4i& bbox, const CullGrid& grid)
	{
		const GSVector4i interior = (bbox + grid.round_add) & grid.round_mask;
		return interior + GSVector4i(0, 0, 1, 1);
	}

	// The rounded bbox the accepted-prim draw_rect update and the scissor test
	// consume. **This is the shipped rounding at every scale** -- the pixel-centre
	// snap at native, the bottom/right sub-texel trim at any upscale -- and the
	// finer cull grid deliberately does not reach it.
	//
	// The grid could round this box too, and that is the tighter and more honest
	// rect. It is also a different change: the rect builds temp_draw_rect, which
	// reaches the texture cache as valid rects, invalidations and page ranges, and
	// at 2x letting the grid round it moved pixels on five of the forty-seven dumps
	// in the test corpus -- the last two rows of a Splashdown frame, the last two
	// columns of a Call of Duty 3 frame, a patch of Armored Core 3. So the grid
	// answers one question only: does this prim paint anything at all.
	template <int primclass>
	__forceinline_odr GSVector4i RoundCullRect(GSVector4i bbox, const CullGrid& grid, bool aa1_expand)
	{
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			if (grid.ShiftFor<primclass>() == 4)
			{
				// Native: the grid IS the pixel centres, so this is the shipped
				// interior-pixel-centre rounding.
				bbox = RoundToCullGrid(bbox, grid);
			}
			else
			{
				// For upscaling, remove bottom/right subtexels.
				bbox -= ((bbox & GSVector4i(0xF)) == GSVector4i(0)) & GSVector4i(0, 0, 1, 1);
			}

			// For AA1 triangles and lines, expand the bounds by 1 pixel on all sides.
			if (aa1_expand)
			{
				bbox += GSVector4i(-0x10, -0x10, 0x10, 0x10);
			}
		}

		return bbox;
	}

	// The pixel rect one accepted prim contributes to temp_draw_rect: the class
	// rounding is already in bbox, this is only the sub-pixel shift and the
	// exclusive bottom/right endpoint.
	__forceinline_odr GSVector4i PrimDrawRect(const GSVector4i& bbox)
	{
		return bbox.sra32<4>() + GSVector4i(0, 0, 1, 1);
	}

	// The same rect asked on the NATIVE pixel grid, whatever grid bbox was rounded
	// on -- what the draw-buffering overlap heuristic compares its raw incoming
	// prim box against (GSState::CheckOverlapVertsSlow). Only that heuristic reads
	// it; temp_draw_rect keeps the shipped rounding, because it reaches the texture
	// cache and rounding it moved pixels on five of forty-seven dumps at 2x.
	//
	// Why the one expression works at every grid. RoundCullRect leaves the
	// bottom/right edge on the native grid already: its upscale arm trims the one
	// sub-texel that separates floor(z/16)+1 from floor((z-1)/16)+1, and its native
	// arm computes the latter outright. So the only edge that moves with the grid is
	// top/left, floor above native against ceil at it, and +15 before the shift is
	// that ceil:
	//   * at the native grid bbox.x is already a multiple of 16, so +15 cannot reach
	//     the next one and this is bit-identical to PrimDrawRect -- 1x is a
	//     structural no-op, not a configuration branch;
	//   * above it, (raw.x + 15) >> 4 is ceil(raw.x / 16), the native answer.
	// The AA1 expansion is a whole pixel on every edge and is applied after the
	// rounding in both arms, so it commutes with this.
	//
	// The point and line classes are excluded because RoundCullRect does not round
	// them either: their contribution is the raw box at every scale, already
	// scale-invariant, and putting the ceil on it would change native output.
	template <int primclass>
	__forceinline_odr GSVector4i PrimNativeDrawRect(const GSVector4i& bbox)
	{
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
			return (bbox + GSVector4i(15, 15, -1, -1)).sra32<4>() + GSVector4i(0, 0, 1, 1);
		else
			return PrimDrawRect(bbox);
	}

	// The identity element of runion, for a prim whose native rect is empty.
	//
	// A prim that spans no native sample point paints no native pixel, and at the
	// native grid it is not merely thin -- CullTest rejects it on bbox.rempty() and
	// it never reaches the accumulation. So the native rect must not take it either,
	// and runion is a plain min/max with no notion of an empty operand: unioning
	// such a box pulls the rect out to that prim's pixel, which is the 2x behaviour
	// this is here to remove.
	//
	// It survives the fold's rintersect(scissor) as the scissor's own corners
	// inverted -- still empty, and still an identity, because clamping the union
	// gives the same answer as clamping the real operand alone.
	__forceinline_odr GSVector4i NativeDrawRectNone()
	{
		return GSVector4i(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
			std::numeric_limits<int>::min(), std::numeric_limits<int>::min());
	}

	// PrimNativeDrawRect, with an empty result replaced by the union identity.
	// Branchless: the emptiness is a data-dependent predicate (69% of Stuntman's
	// submitted prims are sub-native at 2x), so a branch here mispredicts per prim.
	template <int primclass>
	__forceinline_odr GSVector4i PrimNativeDrawRectOrNone(const GSVector4i& bbox)
	{
		const GSVector4i rect = PrimNativeDrawRect<primclass>(bbox);
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			// lt lanes: (x < z, y < w, false, false) -- the same test rempty() runs,
			// kept in vector registers. ANDed with itself lane-swapped and then
			// broadcast, so every lane carries "not empty".
			const GSVector4i lt = rect.lt32(rect.zwzw());
			const GSVector4i keep = (lt & lt.yxwz()).xyxy();
			return NativeDrawRectNone().blend8(rect, keep);
		}
		else
		{
			return rect;
		}
	}

	// Whether the prim spans no point of the cull grid, and so paints nothing.
	//
	// Only asked where the grid is finer than the rect rounding, i.e. shift 1..3:
	// at shift 4 the rect IS the grid and its own rempty() already says this, and
	// at shift 0 there is no grid. Note the rect's rempty() is never weaker than
	// this one -- a bbox the trim empties spans at most one sub-texel, which cannot
	// straddle two grid points -- so the two are OR'd, not swapped.
	template <int primclass>
	__forceinline_odr u32 CullGridEmpty(const GSVector4i& bbox, const CullGrid& grid, bool aa1_expand)
	{
		const int shift = grid.ShiftFor<primclass>();
		if (shift == 0 || shift == 4)
			return 0;

		// The phase shifts the sample set, so snap in the grid's own frame: subtract
		// it, snap, and leave it subtracted. rempty() compares the two edges against
		// each other, so the translation cancels and adding the phase back would be
		// dead work. The AA1 expansion is +/-16 sub-texels, a multiple of every step
		// the grid can take, so it commutes with the snap either way round.
		GSVector4i snapped = RoundToCullGrid(bbox - grid.phase, grid);
		if (aa1_expand)
			snapped += GSVector4i(-0x10, -0x10, 0x10, 0x10);

		return static_cast<u32>(snapped.rempty());
	}

	// Bounding box of one completed prim's window entries with the class rounding
	// applied — the bbox half of the legacy CullTest, shared by the scalar-outcode
	// fast path (which only needs it for accepted prims).
	template <u32 n, int primclass>
	__forceinline_odr GSVector4i ComputeCullBBox(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		const CullGrid& grid, bool aa1_expand)
	{
		return RoundCullRect<primclass>(CullPrimBounds<n>(v0, v1, v2), grid, aa1_expand);
	}

	// Accept/cull test for one completed prim. v0/v1/v2 are the window entries for
	// the prim's vertices ({x, y, x, y} offset-subtracted 12.4 fixed-point, v0 most
	// recent), scissor_cull = context scissor in cull form. grid selects the
	// sample-point rounding (see CullGrid); aa1_expand is the caller-evaluated
	// "PRIM->AA1 && IsCoverageAlphaSupported()" (only for triangle/sprite classes).
	// Returns nonzero to skip the prim; bbox receives the rounded bounding box the
	// accepted-prim draw_rect update consumes.
	template <u32 n, int primclass>
	__forceinline_odr u32 CullTest(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		const GSVector4i& scissor_cull, const CullGrid& grid, bool aa1_expand, GSVector4i& bbox)
	{
		const GSVector4i raw = CullPrimBounds<n>(v0, v1, v2);
		bbox = RoundCullRect<primclass>(raw, grid, aa1_expand);

		// Do scissor test. The box is the shipped one, and so is the half-pixel slop
		// scissor_cull carries (GSDrawingContext::UpdateScissor): a prim in that
		// margin still covers device pixels inside the scissor once it is upscaled,
		// and the grid has no opinion about that.
		const GSVector4i bbox_ex = bbox + GSVector4i(0, 0, 1, 1); // Exclusive coords for the scissor test.
		u32 test = static_cast<u32>(!bbox_ex.rintersects(scissor_cull));

		// Test for empty bbox, and for spanning no point of the cull grid.
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			test |= static_cast<u32>(bbox.rempty());
			test |= CullGridEmpty<primclass>(raw, grid, aa1_expand);
		}

		// Test for degenerate triangle.
		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			test |= static_cast<u32>(v0.eq(v1)) | static_cast<u32>(v1.eq(v2)) | static_cast<u32>(v0.eq(v2));
		}

		return test;
	}

	// ------------------------------------------------------------------------
	// Scalar-outcode cull: exact scalar reformulation of CullTest for the cases
	// the fused handlers hit hottest (point/line always; triangle strips/lists
	// and sprites at native res without AA1 expansion). The NEON formulation
	// pays 3x umaxv + 2x uminp + 5x NEON->GPR moves of exposed latency per prim
	// on in-order cores; this one is a handful of ALU ops on per-vertex
	// precomputed metadata.
	//
	// Derivation (bit-exact, pinned by gs_vertex_tests):
	// - The prim bbox is the min/max of the vertices, so "bbox beyond scissor
	//   edge" == "every vertex beyond that edge": an AND of per-vertex 4-bit
	//   outcodes replaces the bbox build + saturate + empty chain.
	// - For triangle/sprite at native res, the ceil16/floor16 interior rounding
	//   and the cull rect's +/-8 fold into pixel-band bounds: with band(v) =
	//   (v-1)>>4, reject-left <=> all band(x) < (cull.x+14)>>4, reject-right <=>
	//   all band(x) >= (cull.z-1)>>4 (same for y). Point/line compare raw 12.4
	//   coords against cull directly (no rounding, no empty test).
	// - Interior-empty: since (v+15)>>4 == ((v-1)>>4)+1 identically,
	//   ceil16(min) > floor16strict(max) <=> all vertices share one band on
	//   that axis — a pure equality test on the packed bands.
	// - Degenerate triangle: the legacy 128-bit eq on {x,y,x,y} entries is xy
	//   equality — one u64 compare on the packed position.
	//
	// Window coords are offset-subtracted s32 (the offset subtract uses the full
	// 32-bit XYOFFSET lane, pad bits included, to match the NEON ring exactly), so
	// bands are stored as 28-bit fields — exact for any s32 coord, no truncation
	// aliasing.
	// ------------------------------------------------------------------------

	constexpr u64 kCullMetaBandXMask = 0xFFFFFFFull;
	constexpr u64 kCullMetaBandYMask = 0xFFFFFFFull << 28;
	constexpr u64 kCullMetaOutcodeMask = 0xFull << 56;

	// One mirror-ring slot: packed window position + derived cull metadata.
	// xyp = (u64)(u32)wy << 32 | (u32)wx; meta = bandx:28 | bandy:28 | outcode:4.
	struct CullMirrorEntry
	{
		u64 xyp;
		u64 meta;
	};

	// Pre-adjusted per-class scissor bounds, derived from scissor.cull whenever the
	// scissor changes. Outcode bits: 1 = out-left (< l), 2 = out-right (>= r),
	// 4 = out-top (< t), 8 = out-bottom (>= b).
	struct CullBounds
	{
		int l, t, r, b;
	};

	// Band-space bounds for triangle/sprite at native res (rounding folded in).
	__forceinline_odr CullBounds MakeBandedCullBounds(const GSVector4i& cull)
	{
		return {(cull.x + 14) >> 4, (cull.y + 14) >> 4, (cull.z - 1) >> 4, (cull.w - 1) >> 4};
	}

	// Raw 12.4 bounds for point/line, and for triangle/sprite wherever the cull
	// grid is finer than native (no rounding, exclusive bbox test folded in).
	//
	// Why the raw bounds serve a grid class too: away from native the rounded
	// classes' scissor test runs on the shipped trim box, whose only difference
	// from the raw box is one sub-texel off a bottom/right edge that sits exactly
	// on a pixel boundary. That difference is invisible to this test, because the
	// trimmed edge only changes the answer when it meets the cull rect exactly,
	// and the cull rect's edges are SCAX*16 -/+ 8 (GSDrawingContext::UpdateScissor)
	// -- never a multiple of 16. GsVertexCull.ScalarTriangleSweep pins it.
	__forceinline_odr CullBounds MakeRawCullBounds(const GSVector4i& cull)
	{
		return {cull.x, cull.y, cull.z, cull.w};
	}

	// Build one mirror entry from a vertex's window position.
	//
	// `banded` selects which coordinate space the outcode compares in: bands at
	// native res, where the scissor test runs on the pixel-centre-rounded box and
	// the rounding folds into the bounds, and raw 12.4 everywhere else.
	// `band_shift` is the cull grid's log2 sub-texel step, which sets how wide a
	// band is -- 4 at native, 3 at 2x and so on. Bands are packed whatever the
	// outcode space, so the entry shape is uniform; the band fields are read only
	// as differences between vertices of one prim, which share an XYOFFSET, so a
	// narrower band cannot overflow the 28-bit field either.
	// `band_bias_*` is the grid's phase plus one, so a band boundary sits on a
	// sample point: the band test asks "do all the prim's vertices fall between the
	// same two sample points", and with a phase those points are phase + k*step.
	// It is 1 at native and wherever the phase is zero, which is every mode but
	// HalfPixelOffset::Native.
	template <bool banded>
	__forceinline_odr CullMirrorEntry MakeCullMirrorEntry(
		int wx, int wy, const CullBounds& bounds, int band_shift, int band_bias_x = 1, int band_bias_y = 1)
	{
		const int bx = (wx - band_bias_x) >> band_shift;
		const int by = (wy - band_bias_y) >> band_shift;
		const int cx = banded ? bx : wx;
		const int cy = banded ? by : wy;

		u32 oc = 0;
		oc |= (cx < bounds.l) ? 1u : 0u;
		oc |= (cx >= bounds.r) ? 2u : 0u;
		oc |= (cy < bounds.t) ? 4u : 0u;
		oc |= (cy >= bounds.b) ? 8u : 0u;

		CullMirrorEntry e;
		e.xyp = static_cast<u64>(static_cast<u32>(wx)) | (static_cast<u64>(static_cast<u32>(wy)) << 32);
		e.meta = (static_cast<u64>(static_cast<u32>(bx)) & kCullMetaBandXMask) |
		         ((static_cast<u64>(static_cast<u32>(by)) << 28) & kCullMetaBandYMask) |
		         (static_cast<u64>(oc) << 56);
		return e;
	}

	// The scalar decision. e0 is the most recent vertex. Unused entries (n < 3)
	// may alias e0. Bit-equivalent to CullTest's return under the fast-path gate
	// (point/line always; triangle non-fan / sprite whenever the class has a cull
	// grid and there is no AA1 expansion).
	//
	// The band-equality test is "every vertex in one band on some axis", which is
	// exactly "the prim spans no grid point" at whatever step the bands were built
	// with -- so it serves as the native interior-empty test and as the upscale
	// grid test without changing shape. It also subsumes the shipped trim box's
	// own empty test: a box the trim empties spans at most one sub-texel, which
	// cannot straddle two grid points.
	template <u32 n, int primclass>
	__forceinline_odr u32 CullTestScalar(const CullMirrorEntry& e0, const CullMirrorEntry& e1, const CullMirrorEntry& e2)
	{
		u64 all_out = e0.meta;
		if constexpr (n >= 2)
			all_out &= e1.meta;
		if constexpr (n == 3)
			all_out &= e2.meta;

		if ((all_out & kCullMetaOutcodeMask) != 0)
			return 1;

		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			// Interior-empty: all vertices in one pixel band on either axis.
			u64 diff = e0.meta ^ e1.meta;
			if constexpr (n == 3)
				diff |= e0.meta ^ e2.meta;

			if ((diff & kCullMetaBandXMask) == 0 || (diff & kCullMetaBandYMask) == 0)
				return 1;
		}

		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			if (e0.xyp == e1.xyp || e1.xyp == e2.xyp || e0.xyp == e2.xyp)
				return 1;
		}

		return 0;
	}

	// ------------------------------------------------------------------------
	// Fused vertex-trace bounds: accumulate GSVertexTraceFMM::FindMinMax's
	// min/max at index-emission time over each newly-referenced vertex (the data
	// is register/L1-hot in the kick), so the flush doesn't re-walk the index
	// list (strip vertices up to 3x redundant) with a non-pipelined FDIV per
	// vertex pair. The raw material is accumulated env-blind; the finish step
	// reproduces the legacy tail bit-exactly or declines (caller then runs the
	// legacy FindMinMax).
	//
	// Exactness notes (pinned by gs_vertex_tests):
	// - Each accumulate step performs the exact per-vertex op sequence the
	//   legacy walk performs for that vertex (same lane values through the same
	//   IEEE ops — the legacy pairs two vertices per 4-lane op, but per-lane
	//   the scalars are identical). min/max (and the NaN-blend-masked min/max
	//   of the STQ path, where a masked lane is an identity step) are
	//   idempotent, associative and commutative, so accumulating the referenced
	//   vertex SET once (dedup'd by the caller's watermark) equals the legacy
	//   walk over the index list with its duplicates, in any order.
	// - STQ (!FST) divides per new vertex at accumulate time — one 4-lane FDIV
	//   per unique vertex vs the legacy walk's one per index-list pair (strips
	//   reference vertices up to 3x) — and reproduces the legacy per-lane NaN
	//   masking and tnan reporting verbatim, so there are no decline cases.
	// ------------------------------------------------------------------------

	struct FmmAcc
	{
		GSVector4i pmin, pmax; // u32 min/max of {x, y, z, fog-word} (the legacy p vectors)
		GSVector4i tmin, tmax; // FST: u16 min/max of raw m[1] (elements 4/5 = U/V).
		                       // !FST: the legacy NaN-blend-masked float min/max chains
		                       //       over per-vertex {S/Q, T/Q, Q, Q}.
		GSVector4i tnan;       // !FST: accumulated per-lane NaN masks (legacy tnan)
		GSVector4i cmin, cmax; // u8 min/max of m[0] (bytes 8-11 = RGBA); flat shading
		                       // accumulates provoking vertices only.
	};

	// {x, y, z, fog-word} exactly as the legacy kernel builds its p vectors.
	__forceinline_odr GSVector4i FmmPos(const GSVector4i& m1)
	{
		return m1.upl16().blend32<0xc>(m1.ywyw());
	}

	__forceinline_odr void FmmAccReset(FmmAcc& a, bool tme, bool fst)
	{
		a.pmin = GSVector4i::xffffffff();
		a.pmax = GSVector4i::zero();
		if (tme && !fst)
		{
			a.tmin = GSVector4i::cast(GSVector4(FLT_MAX));
			a.tmax = GSVector4i::cast(GSVector4(-FLT_MAX));
		}
		else
		{
			a.tmin = GSVector4i::xffffffff();
			a.tmax = GSVector4i::zero();
		}
		a.tnan = GSVector4i::zero();
		a.cmin = GSVector4i::xffffffff();
		a.cmax = GSVector4i::zero();
	}

	// accumulate_color = iip || provoking, evaluated by the caller (flat shading
	// only takes the provoking vertex's color; the provoking vertex is always the
	// last-emitted index of the prim).
	__forceinline_odr void FmmAccumVertex(FmmAcc& a, const GSVector4i& m0, const GSVector4i& m1,
		bool tme, bool fst, bool accumulate_color)
	{
		const GSVector4i p = FmmPos(m1);
		a.pmin = a.pmin.min_u32(p);
		a.pmax = a.pmax.max_u32(p);

		if (tme)
		{
			if (fst)
			{
				a.tmin = a.tmin.min_u16(m1);
				a.tmax = a.tmax.max_u16(m1);
			}
			else
			{
				// Single-vertex transcription of the legacy STQ step: build
				// {S/Q, T/Q, Q, Q}, mask NaN lanes out of the min/max chains,
				// record them in tnan.
				const GSVector4 stq_raw = GSVector4::cast(m0);
				const GSVector4 stq = (stq_raw / stq_raw.wwww()).xyww(stq_raw);

				const GSVector4i nan = GSVector4i::cast(stq != stq);
				const GSVector4 keep = GSVector4::cast(~nan);

				GSVector4 tmin = GSVector4::cast(a.tmin);
				GSVector4 tmax = GSVector4::cast(a.tmax);
				a.tmin = GSVector4i::cast(tmin.blend32(tmin.min(stq), keep));
				a.tmax = GSVector4i::cast(tmax.blend32(tmax.max(stq), keep));
				a.tnan |= nan;
			}
		}

		if (accumulate_color)
		{
			a.cmin = a.cmin.min_u8(m0);
			a.cmax = a.cmax.max_u8(m0);
		}
	}

	struct FmmResult
	{
		GSVector4 min_p, max_p, min_t, max_t;
		GSVector4i min_c, max_c;
		u32 nan_value;  // only meaningful when write_nan
		bool write_nan; // legacy leaves vt.nan untouched for TME && FST draws
	};

	// Reproduce the legacy FindMinMax tail from the accumulators. tw/th are the
	// draw context's TEX0.TW/TH.
	__forceinline_odr void FmmFinish(const FmmAcc& a, bool tme, bool fst, bool color,
		const GIFRegXYOFFSET& ofs, u32 tw, u32 th, FmmResult& out)
	{
		out.write_nan = !(tme && fst);
		out.nan_value = 0;

		const GSVector4 o(ofs);
		const GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

		out.min_p = (GSVector4(a.pmin) - o) * s;
		out.max_p = (GSVector4(a.pmax) - o) * s;

		// Fix signed int conversion of the Z lane, as the legacy tail does.
		out.min_p = out.min_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(a.pmin.extract32<2>()))));
		out.max_p = out.max_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(a.pmax.extract32<2>()))));

		if (tme)
		{
			if (fst)
			{
				// Legacy converts each vertex's {U, V} u16s to float and min/maxes
				// against FLT_MAX sentinels; u16 -> float is monotone and exact and
				// the sentinels never survive, so min-in-u16-then-convert matches.
				const GSVector4i uvmin(a.tmin.U16[4], a.tmin.U16[5], a.tmin.U16[4], a.tmin.U16[5]);
				const GSVector4i uvmax(a.tmax.U16[4], a.tmax.U16[5], a.tmax.U16[4], a.tmax.U16[5]);
				const GSVector4 sc = GSVector4(1.0f / 16, 1.0f).xxyy();
				out.min_t = GSVector4(uvmin) * sc;
				out.max_t = GSVector4(uvmax) * sc;
			}
			else
			{
				const GSVector4 sc = GSVector4(1 << static_cast<int>(tw), 1 << static_cast<int>(th), 1, 1);
				out.min_t = GSVector4::cast(a.tmin) * sc;
				out.max_t = GSVector4::cast(a.tmax) * sc;
				out.nan_value = static_cast<u32>(a.tnan.mask()) & ~4u;
			}
		}
		else
		{
			out.min_t = GSVector4::zero();
			out.max_t = GSVector4::zero();
		}

		if (color)
		{
			out.min_c = a.cmin.zzzz().u8to32();
			out.max_c = a.cmax.zzzz().u8to32();
		}
		else
		{
			out.min_c = GSVector4i::zero();
			out.max_c = GSVector4i::zero();
		}
	}
} // namespace GSVertexKernels
