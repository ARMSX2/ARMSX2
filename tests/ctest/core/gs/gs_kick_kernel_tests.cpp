// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Differential suite for the two-pass packed-vertex kick kernel
// (GS/GSVertexKickKernel.h) against the per-vertex kick it replaced.
//
// The kernel is not a reimplementation of the kick's arithmetic -- every quantity
// is computed by the same expression on the same inputs. What it changes is the
// ORDER of independent stores, WHERE a value is read from (side entries in
// registers instead of the mirror ring in memory; the min/max accumulate's vertex
// reloaded from the buffer instead of the parse's registers), and WHEN the
// per-draw environment snapshot is taken (once per handler call instead of once
// per leading culled prim, which is exact because nothing inside one handler call
// can change m_env). None of that is provable by reading; all of it is decidable
// by running the same register run through both arms and comparing everything the
// kick leaves behind.
//
// So that is what this does. Two probes, identical setup, one stream, one arm
// each, and then a comparison of:
//   * the vertex buffer over [0, tail) and the index buffer over [0, itail);
//   * head / tail / next / itail / xy_tail;
//   * both mirror rings (xy[] and kick_ring[]) and xyhead;
//   * the fused min/max accumulator, its watermark and its valid flag;
//   * temp_draw_rect;
//   * m_prev_env, m_dirty_gs_regs, m_backed_up_ctx and the draw-buffer
//     environment snapshot the kick takes -- the amendment that lets the kernel
//     skip repeat snapshots rests on those bytes being identical;
//   * m_v and m_q, which the piecemeal handlers and the next tag read;
//   * the number of draws the run flushed.
//
// Bytes of the vertex buffer at or past tail are deliberately NOT compared: the
// kernel parks vertices there provisionally and nothing reads them. That is the
// one place the two arms differ, and it is the reason the wider net over the dump
// corpus (-fediff) exists.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "GS/GS.h"
#include "GS/GSState.h"

namespace
{
	// ---------------------------------------------------------------- stream ---

	struct VertexSpec
	{
		u16 x, y;
		u32 z;
		u32 rgba; // R in bits 0-7, G 8-15, B 16-23, A 24-31
		float s, t, q;
		u32 fog;
		bool adc;
	};

	// One PACKED {STQ, RGBAQ, XYZ2} or {STQ, RGBAQ, XYZF2} record.
	void EncodeVertex(GIFPackedReg* r, const VertexSpec& v, bool xyzf2)
	{
		std::memset(r, 0, sizeof(GIFPackedReg) * 3);

		std::memcpy(&r[0].U32[0], &v.s, sizeof(float));
		std::memcpy(&r[0].U32[1], &v.t, sizeof(float));
		std::memcpy(&r[0].U32[2], &v.q, sizeof(float));

		r[1].U32[0] = (v.rgba >> 0) & 0xFF;
		r[1].U32[1] = (v.rgba >> 8) & 0xFF;
		r[1].U32[2] = (v.rgba >> 16) & 0xFF;
		r[1].U32[3] = (v.rgba >> 24) & 0xFF;

		r[2].U32[0] = v.x;
		r[2].U32[1] = v.y;
		if (xyzf2)
		{
			r[2].U32[2] = (v.z & 0x00FFFFFF) << 4;
			r[2].U32[3] = (v.fog & 0xFF) << 4;
		}
		else
		{
			r[2].U32[2] = v.z;
		}
		if (v.adc)
			r[2].U32[3] |= 0x8000;
	}

	std::vector<GIFPackedReg> EncodeStream(const std::vector<VertexSpec>& verts, bool xyzf2)
	{
		std::vector<GIFPackedReg> out(verts.size() * 3);
		for (size_t i = 0; i < verts.size(); i++)
			EncodeVertex(&out[i * 3], verts[i], xyzf2);
		return out;
	}

	// ----------------------------------------------------------------- setup ---

	struct KickSetup
	{
		int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 639, scissor_y1 = 447;
		u32 ofx = 0, ofy = 0;      // XYOFFSET, 12.4 fixed point
		u32 iip = 1, tme = 0, fst = 0, aa1 = 0, ctxt = 0;
		bool draw_buffering = false;
		bool recent_buffer_switch = false;
		bool empty_scissor = false; // drives m_scissor_invalid

		// Autoflush. `autoflush` installs the global level the handler tables are
		// built from; `autoflush_hit` points TEX0 at FRAME's block so
		// IsAutoFlushDraw can answer true, and clearing it points TEX0 somewhere
		// else so the predicate runs to the end and answers false -- which is the
		// case stage 3b routes into the kernel.
		GSHWAutoFlushLevel autoflush = GSHWAutoFlushLevel::Disabled;
		bool autoflush_hit = false;
	};

	// The autoflush level is a global, read by GetAutoFlushLevel through GSConfig;
	// every test that moves it restores it.
	class AutoFlushGuard
	{
	public:
		explicit AutoFlushGuard(GSHWAutoFlushLevel level)
			: m_saved(GSConfig.UserHacks_AutoFlush)
		{
			GSConfig.UserHacks_AutoFlush = level;
		}
		~AutoFlushGuard() { GSConfig.UserHacks_AutoFlush = m_saved; }

	private:
		GSHWAutoFlushLevel m_saved;
	};

	// Draw buffering is a global; every test restores it.
	class DrawBufferingGuard
	{
	public:
		explicit DrawBufferingGuard(bool on)
			: m_saved(GSConfig.UserHacks_DrawBuffering)
		{
			GSConfig.UserHacks_DrawBuffering = on;
		}
		~DrawBufferingGuard() { GSConfig.UserHacks_DrawBuffering = m_saved; }

	private:
		bool m_saved;
	};

	class KickProbe final : public GSState
	{
	public:
		KickProbe()
			: m_regs_storage(std::make_unique<GSPrivRegSet>())
		{
			// A flush walks the privileged registers looking for the display
			// buffer; nothing constructs them for a bare GSState.
			std::memset(m_regs_storage.get(), 0, sizeof(GSPrivRegSet));
			m_regs = m_regs_storage.get();
		}

		// A draw is where the environment can move under a run: FlushPrim executes
		// it, and anything the flush restores afterwards is what the rest of the
		// run must be decided against. With this set, every draw moves the scissor,
		// the offset and the shading bits, which is what a buffered-environment
		// restore does to a real title mid-tag.
		bool m_draw_moves_environment = false;

		// A draw is also the only place the autoflush predicate can move inside a
		// handler call: IsAutoFlushDraw reads TEX0 against FRAME, and a flush is
		// what restores a different one. With this set every draw flips TEX0
		// between FRAME's own block and a different one, so a run that flushes
		// crosses the predicate in both directions -- which is what the per-chunk
		// re-evaluation has to notice.
		bool m_draw_moves_autoflush = false;

		// Which of the two fused handler instantiations a KickCall drives.
		bool m_auto_flush_arm = false;

		void Draw() override
		{
			m_draws++;
			if (m_draw_moves_autoflush)
			{
				GSDrawingContext& afctx = m_env.CTXT[m_env.PRIM.CTXT];
				afctx.TEX0.TBP0 = (m_draws & 1) ? 0u : 0x400u;
				UpdateContext();
			}
			if (!m_draw_moves_environment)
				return;

			GSDrawingContext& ctx = m_env.CTXT[m_env.PRIM.CTXT];
			ctx.SCISSOR.SCAX0 = 40 + (m_draws * 13) % 100;
			ctx.SCISSOR.SCAY0 = 30 + (m_draws * 7) % 80;
			ctx.SCISSOR.SCAX1 = 300 + (m_draws * 11) % 200;
			ctx.SCISSOR.SCAY1 = 200 + (m_draws * 5) % 150;
			ctx.XYOFFSET.OFX = (m_draws & 1) ? 0u : 64u;
			ctx.XYOFFSET.OFY = (m_draws & 1) ? 32u : 0u;
			ctx.UpdateScissor();
			m_env.PRIM.IIP = (m_draws & 1) ? 0 : 1;
			m_env.PRIM.TME = (m_draws & 1) ? 1 : 0;
			UpdateContext();
		}

		// The base asserts "not implemented"; the kick only reaches it for AA1
		// prims, and the answer decides whether the scalar-outcode cull applies.
		bool IsCoverageAlphaSupported() override { return true; }

		u32 m_draws = 0;
		std::unique_ptr<GSPrivRegSet> m_regs_storage;

		using GSState::m_env_buffers;
		using GSState::m_index;
		using GSState::m_q;
		using GSState::m_v;
		using GSState::m_vertex;
		using GSState::s_fused_kick_use_kernel;

		void Configure(const KickSetup& s)
		{
			GSDrawingContext& ctx = m_env.CTXT[s.ctxt];
			if (s.empty_scissor)
			{
				// Bottom-right below top-left: scissor.in comes out empty and
				// m_scissor_invalid is set.
				ctx.SCISSOR.SCAX0 = 200;
				ctx.SCISSOR.SCAY0 = 200;
				ctx.SCISSOR.SCAX1 = 100;
				ctx.SCISSOR.SCAY1 = 100;
			}
			else
			{
				ctx.SCISSOR.SCAX0 = static_cast<u32>(s.scissor_x0);
				ctx.SCISSOR.SCAY0 = static_cast<u32>(s.scissor_y0);
				ctx.SCISSOR.SCAX1 = static_cast<u32>(s.scissor_x1);
				ctx.SCISSOR.SCAY1 = static_cast<u32>(s.scissor_y1);
			}
			ctx.XYOFFSET.OFX = s.ofx;
			ctx.XYOFFSET.OFY = s.ofy;
			ctx.UpdateScissor();

			// The autoflush context. IsAutoFlushDraw wants a texture whose block
			// is (or is not) FRAME's, an unmasked FRAME, no alpha-fail rule that
			// takes FRAME out of the draw, and no mipmap range to walk.
			ctx.FRAME.FBP = 0;
			ctx.FRAME.FBW = 10;
			ctx.FRAME.PSM = PSMCT32;
			ctx.FRAME.FBMSK = 0;
			ctx.ZBUF.ZBP = 0x100;
			ctx.ZBUF.PSM = PSMZ32;
			ctx.ZBUF.ZMSK = 1;
			ctx.TEX0.TBP0 = s.autoflush_hit ? 0u : 0x400u;
			ctx.TEX0.TBW = 4;
			ctx.TEX0.PSM = PSMCT32;
			ctx.TEX0.TW = 8;
			ctx.TEX0.TH = 8;
			ctx.TEX0.TCC = 1;
			ctx.TEX0.TFX = 0;
			ctx.TEX1.MXL = 0;
			ctx.TEX1.MMIN = 0;
			ctx.TEX1.LCM = 0;
			ctx.TEST.ATE = 0;
			ctx.CLAMP.WMS = 0;
			ctx.CLAMP.WMT = 0;

			m_env.PRIM.CTXT = s.ctxt;
			m_env.PRIM.IIP = s.iip;
			m_env.PRIM.TME = s.tme;
			m_env.PRIM.FST = s.fst;
			m_env.PRIM.AA1 = s.aa1;

			m_nativeres = true;
			UpdateContext();
			UpdateVertexKick();

			m_recent_buffer_switch = s.recent_buffer_switch;

			// GSState leaves temp_draw_rect uninitialized (nothing reads it before
			// the first accepted prim writes it), so two probes would start with
			// different garbage and a run that completes no prim would compare it.
			temp_draw_rect = GSVector4i::zero();
		}

		void SetPrim(u32 prim)
		{
			m_env.PRIM.PRIM = prim;
			UpdateVertexKick();
		}

		// Kick one handler call's worth of the stream. `size` is in registers, i.e.
		// three per vertex, exactly as Transfer hands it to the fused handler.
		template <u32 prim>
		void KickCall(const GIFPackedReg* r, u32 size, bool xyzf2)
		{
			if (xyzf2)
				GIFPackedRegHandlerSTQRGBAXYZF2<prim, false>(r, size);
			else
				GIFPackedRegHandlerSTQRGBAXYZ2<prim, false>(r, size);
		}

		// The auto_flush = true instantiation of the same handler -- the one stage
		// 3b routes into the kernel when the predicate is inert.
		template <u32 prim>
		void KickCallAF(const GIFPackedReg* r, u32 size, bool xyzf2)
		{
			if (xyzf2)
				GIFPackedRegHandlerSTQRGBAXYZF2<prim, true>(r, size);
			else
				GIFPackedRegHandlerSTQRGBAXYZ2<prim, true>(r, size);
		}

		void KickCallDyn(u32 prim, const GIFPackedReg* r, u32 size, bool xyzf2)
		{
			if (m_auto_flush_arm)
			{
				switch (prim)
				{
					case GS_TRIANGLESTRIP: KickCallAF<GS_TRIANGLESTRIP>(r, size, xyzf2); break;
					case GS_TRIANGLELIST: KickCallAF<GS_TRIANGLELIST>(r, size, xyzf2); break;
					case GS_SPRITE: KickCallAF<GS_SPRITE>(r, size, xyzf2); break;
					case GS_TRIANGLEFAN: KickCallAF<GS_TRIANGLEFAN>(r, size, xyzf2); break;
					case GS_LINESTRIP: KickCallAF<GS_LINESTRIP>(r, size, xyzf2); break;
					default: FAIL() << "unhandled prim " << prim;
				}
				return;
			}
			switch (prim)
			{
				case GS_TRIANGLESTRIP: KickCall<GS_TRIANGLESTRIP>(r, size, xyzf2); break;
				case GS_TRIANGLELIST: KickCall<GS_TRIANGLELIST>(r, size, xyzf2); break;
				case GS_SPRITE: KickCall<GS_SPRITE>(r, size, xyzf2); break;
				case GS_TRIANGLEFAN: KickCall<GS_TRIANGLEFAN>(r, size, xyzf2); break;
				case GS_LINESTRIP: KickCall<GS_LINESTRIP>(r, size, xyzf2); break;
				default: FAIL() << "unhandled prim " << prim;
			}
		}

		// The routing predicate itself, so a test can assert which case it is
		// driving instead of assuming the context it built produces it.
		bool AutoFlushPredicate(u32 prim)
		{
			int tex_layer = 0;
			return IsAutoFlushDraw(prim, tex_layer);
		}
	};

	std::unique_ptr<KickProbe> MakeProbe(const KickSetup& s, u32 prim, bool use_kernel)
	{
		auto p = std::make_unique<KickProbe>();
		KickProbe::s_fused_kick_use_kernel = use_kernel;
		p->Configure(s);
		p->SetPrim(prim);
		return p;
	}

	// ------------------------------------------------------------ comparison ---

	::testing::AssertionResult SameBytes(const char* what, const void* a, const void* b, size_t n)
	{
		const u8* pa = static_cast<const u8*>(a);
		const u8* pb = static_cast<const u8*>(b);
		for (size_t i = 0; i < n; i++)
		{
			if (pa[i] != pb[i])
			{
				return ::testing::AssertionFailure()
				       << what << " differs at byte " << i << ": legacy " << static_cast<int>(pa[i])
				       << " kernel " << static_cast<int>(pb[i]);
			}
		}
		return ::testing::AssertionSuccess();
	}

	// Two GSState objects hold different POINTERS in GSDrawingContext::offset --
	// the derived GSOffset tables point into each object's own GSLocalMemory -- so
	// a raw byte compare of two probes' environments is not a statement about the
	// kick. What is comparable is the RELATIONSHIP each copy has to its own live
	// m_env: which bytes the snapshot carried over and which it left behind. Equal
	// relationships mean the same copy ran (or did not run) on both arms. Outside
	// the two `offset` sub-structs the bytes are compared directly as well.
	::testing::AssertionResult SameEnvCopy(const char* what, const GSDrawingEnvironment& ca,
		const GSDrawingEnvironment& live_a, const GSDrawingEnvironment& cb, const GSDrawingEnvironment& live_b)
	{
		const u8* pa = reinterpret_cast<const u8*>(&ca);
		const u8* pb = reinterpret_cast<const u8*>(&cb);
		const u8* la = reinterpret_cast<const u8*>(&live_a);
		const u8* lb = reinterpret_cast<const u8*>(&live_b);

		// The pointer-bearing windows, located off a real object rather than with
		// offsetof (these types are not standard-layout-clean).
		const u8* env_base = reinterpret_cast<const u8*>(&live_a);
		size_t skip_lo[3], skip_hi[3];
		for (int i = 0; i < 2; i++)
		{
			skip_lo[i] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.CTXT[i].offset) - env_base);
			skip_hi[i] = skip_lo[i] + sizeof(live_a.CTXT[i].offset);
		}
		// The transfer registers and the padding that follows them, up to CTXT[0].
		// No kick path reads or writes any of it and the narrow snapshot stops
		// before it; what does reach it is the flush path, which leaves per-object
		// residue there. HarnessControlIsNotVacuous fails on this window with two
		// legacy arms, which is why it is excluded rather than compared.
		skip_lo[2] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.BITBLTBUF) - env_base);
		skip_hi[2] = static_cast<size_t>(reinterpret_cast<const u8*>(&live_a.CTXT[0]) - env_base);

		for (size_t i = 0; i < sizeof(GSDrawingEnvironment); i++)
		{
			// The pointer bytes are excluded from both tests: a pointer byte can
			// coincidentally equal the live environment's on one probe and not the
			// other, which says nothing about the kick.
			if ((i >= skip_lo[0] && i < skip_hi[0]) || (i >= skip_lo[1] && i < skip_hi[1]) ||
				(i >= skip_lo[2] && i < skip_hi[2]))
				continue;

			const bool carried_a = (pa[i] == la[i]);
			const bool carried_b = (pb[i] == lb[i]);
			if (carried_a != carried_b)
			{
				return ::testing::AssertionFailure()
				       << what << ": byte " << i << " matches the live environment on the "
				       << (carried_a ? "legacy" : "kernel") << " arm only";
			}

			if (pa[i] != pb[i])
			{
				return ::testing::AssertionFailure()
				       << what << " differs at byte " << i << ": legacy " << static_cast<int>(pa[i])
				       << " kernel " << static_cast<int>(pb[i]);
			}
		}
		return ::testing::AssertionSuccess();
	}

	void ExpectSameKickResult(KickProbe& a, KickProbe& b)
	{
		// Cursor first -- everything else is indexed by it, so a mismatch here
		// makes the rest unreadable.
		ASSERT_EQ(a.m_vertex->head, b.m_vertex->head) << "head";
		ASSERT_EQ(a.m_vertex->tail, b.m_vertex->tail) << "tail";
		ASSERT_EQ(a.m_vertex->next, b.m_vertex->next) << "next";
		ASSERT_EQ(a.m_vertex->xy_tail, b.m_vertex->xy_tail) << "xy_tail";
		ASSERT_EQ(a.m_index->tail, b.m_index->tail) << "index tail";

		EXPECT_TRUE(SameBytes("vertex buffer [0, tail)", a.m_vertex->buff, b.m_vertex->buff,
			sizeof(GSVertex) * a.m_vertex->tail));
		EXPECT_TRUE(SameBytes("index buffer [0, itail)", a.m_index->buff, b.m_index->buff,
			sizeof(u16) * a.m_index->tail));

		EXPECT_TRUE(SameBytes("xy ring", a.m_vertex->xy, b.m_vertex->xy, sizeof(a.m_vertex->xy)));
		EXPECT_TRUE(SameBytes("kick_ring", a.m_vertex->kick_ring, b.m_vertex->kick_ring,
			sizeof(a.m_vertex->kick_ring)));
		EXPECT_TRUE(SameBytes("xyhead", &a.m_vertex->xyhead, &b.m_vertex->xyhead, sizeof(GSVector4i)));

		EXPECT_EQ(a.m_vertex->fmm_valid, b.m_vertex->fmm_valid) << "fmm_valid";
		EXPECT_EQ(a.m_vertex->fmm_watermark, b.m_vertex->fmm_watermark) << "fmm_watermark";
		if (a.m_vertex->fmm_valid && b.m_vertex->fmm_valid)
		{
			EXPECT_TRUE(SameBytes("fmm_acc", &a.m_vertex->fmm_acc, &b.m_vertex->fmm_acc,
				sizeof(GSVertexKernels::FmmAcc)));
		}

		EXPECT_TRUE(SameBytes("temp_draw_rect", &a.temp_draw_rect, &b.temp_draw_rect, sizeof(GSVector4i)));

		EXPECT_TRUE(SameEnvCopy("m_prev_env", a.m_prev_env, a.m_env, b.m_prev_env, b.m_env));
		EXPECT_EQ(a.m_dirty_gs_regs, b.m_dirty_gs_regs) << "m_dirty_gs_regs";
		EXPECT_EQ(a.m_backed_up_ctx, b.m_backed_up_ctx) << "m_backed_up_ctx";

		// The environment snapshot the kick hands the draw-buffer list. This is the
		// one the "snapshot once per handler call" amendment rests on: legacy
		// rewrites it once per leading culled prim, the kernel arm once per call,
		// and the result must be the same either way.
		EXPECT_TRUE(SameEnvCopy("draw-buffer env snapshot", a.m_env_buffers[0].m_env, a.m_env,
			b.m_env_buffers[0].m_env, b.m_env));
		EXPECT_EQ(a.m_env_buffers[0].m_backed_up_ctx, b.m_env_buffers[0].m_backed_up_ctx)
			<< "draw-buffer env snapshot ctx";

		EXPECT_TRUE(SameBytes("m_v", &a.m_v, &b.m_v, sizeof(GSVertex)));
		EXPECT_EQ(a.m_q, b.m_q) << "m_q";
		EXPECT_EQ(a.m_draws, b.m_draws) << "draw count";
	}

	// Run one stream through both arms and compare. `call_sizes` is the number of
	// vertices in each handler call; a stream is split across calls exactly the way
	// a GIF tag walk splits it, and the per-call snapshot rule makes the split
	// observable, so the tests drive several.
	// Returns the number of draws the run flushed, so a test that exists to reach
	// the flush path can assert that it did.
	u32 RunAndCompare(const KickSetup& setup, u32 prim, bool xyzf2,
		const std::vector<VertexSpec>& verts, const std::vector<u32>& call_sizes, bool arm_b_kernel = true,
		bool draw_moves_environment = false, bool auto_flush_arm = false, bool draw_moves_autoflush = false,
		bool* predicate_out = nullptr)
	{
		const DrawBufferingGuard guard(setup.draw_buffering);
		const AutoFlushGuard af_guard(setup.autoflush);
		const std::vector<GIFPackedReg> stream = EncodeStream(verts, xyzf2);

		auto run = [&](bool use_kernel) {
			auto p = MakeProbe(setup, prim, use_kernel);
			p->m_draw_moves_environment = draw_moves_environment;
			p->m_draw_moves_autoflush = draw_moves_autoflush;
			p->m_auto_flush_arm = auto_flush_arm;
			if (predicate_out)
				*predicate_out = p->AutoFlushPredicate(prim);
			u32 k = 0;
			for (u32 nv : call_sizes)
			{
				if (k >= verts.size())
					break;
				const u32 take = std::min<u32>(nv, static_cast<u32>(verts.size()) - k);
				p->KickCallDyn(prim, &stream[k * 3], take * 3, xyzf2);
				k += take;
			}
			// Whatever the call list did not cover goes in one last call.
			if (k < verts.size())
			{
				p->KickCallDyn(prim, &stream[k * 3], (static_cast<u32>(verts.size()) - k) * 3, xyzf2);
			}
			return p;
		};

		auto legacy = run(false);
		auto kernel = run(arm_b_kernel);
		KickProbe::s_fused_kick_use_kernel = true;

		ExpectSameKickResult(*legacy, *kernel);
		return legacy->m_draws;
	}

	// ---------------------------------------------------------------- corpora ---

	// ADC patterns from the layout census: none, stuntman's (two set, then two or
	// three clear -- 47% of vertex qwords), katamari's (87%).
	enum class AdcPattern
	{
		None,
		Stuntman,
		Katamari,
	};

	bool AdcAt(AdcPattern p, u32 i, std::mt19937& rng)
	{
		switch (p)
		{
			case AdcPattern::None: return false;
			case AdcPattern::Stuntman:
			{
				const u32 phase = i % 5;
				return phase < 2;
			}
			case AdcPattern::Katamari: return (rng() % 100) < 87;
		}
		return false;
	}

	// Positions in three bands: comfortably inside the scissor, comfortably
	// outside it, and repeats of the previous vertex (which makes degenerate
	// triangles). The mix is chosen to produce both cull outcomes and long runs of
	// rejected prims, which is what the kernel's reject path and the strip
	// compaction need to be exercised by.
	std::vector<VertexSpec> MakeStream(u32 count, AdcPattern adc, u32 seed)
	{
		std::mt19937 rng(seed);
		std::vector<VertexSpec> v;
		v.reserve(count);

		u16 last_x = 100, last_y = 100;
		for (u32 i = 0; i < count; i++)
		{
			VertexSpec s = {};
			const u32 roll = rng() % 100;
			if (roll < 15 && i > 0)
			{
				s.x = last_x; // degenerate: same position as the previous vertex
				s.y = last_y;
			}
			else if (roll < 55)
			{
				s.x = static_cast<u16>((rng() % 640) * 16);   // inside, 12.4
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			else if (roll < 80)
			{
				s.x = static_cast<u16>(2000 * 16 + (rng() % 1000)); // right of the scissor
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			else
			{
				s.x = static_cast<u16>((rng() % 640) * 16);
				s.y = static_cast<u16>(3000 * 16 + (rng() % 500)); // below the scissor
			}
			last_x = s.x;
			last_y = s.y;

			s.z = rng();
			s.rgba = rng();
			s.fog = rng() & 0xFF;
			// S/T/Q as ordinary finite floats, plus the occasional exact zero Q so
			// the parse's Q fix-up is exercised.
			s.s = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 64.0f;
			s.t = static_cast<float>(static_cast<int>(rng() % 2000) - 1000) / 64.0f;
			s.q = ((rng() % 16) == 0) ? 0.0f : (1.0f + static_cast<float>(rng() % 100) / 32.0f);
			s.adc = AdcAt(adc, i, rng);
			v.push_back(s);
		}
		return v;
	}

	// The kernel's chunk is 128 vertices, so the run lengths that matter are the
	// ones around it and the ones shorter than a prim.
	const std::vector<u32> kRunLengths = {1, 2, 3, 5, 127, 128, 129, 1000};
} // namespace

// The kernel's side entry is a CullMirrorEntry with the ADC bit in meta's spare
// bits. If a future band or outcode change reaches those bits the cull decision
// silently changes, so pin the masks here as well as in the header's static_assert.
TEST(GsKickKernel, SideEntryAdcBitDoesNotCollide)
{
	EXPECT_EQ(GSVertexKickKernel::kCullMetaAdcBit & GSVertexKernels::kCullMetaBandXMask, 0u);
	EXPECT_EQ(GSVertexKickKernel::kCullMetaAdcBit & GSVertexKernels::kCullMetaBandYMask, 0u);
	EXPECT_EQ(GSVertexKickKernel::kCullMetaAdcBit & GSVertexKernels::kCullMetaOutcodeMask, 0u);
	EXPECT_EQ(static_cast<u64>(0x8000u) << GSVertexKickKernel::kAdcShift, GSVertexKickKernel::kCullMetaAdcBit);
}

// The three prim types the kernel carries, both packed layouts, every ADC
// pattern, every run length. This is the bulk of the suite: a mismatch anywhere in
// the contract shows up as a named field.
TEST(GsKickKernel, MatchesLegacyAcrossPrimsLayoutsAndRunLengths)
{
	const u32 prims[] = {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE};
	const AdcPattern adcs[] = {AdcPattern::None, AdcPattern::Stuntman, AdcPattern::Katamari};

	u32 seed = 1;
	for (u32 prim : prims)
	{
		for (bool xyzf2 : {false, true})
		{
			for (AdcPattern adc : adcs)
			{
				for (u32 len : kRunLengths)
				{
					SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2
					                                  << " adc=" << static_cast<int>(adc) << " len=" << len);
					KickSetup s;
					RunAndCompare(s, prim, xyzf2, MakeStream(len, adc, seed++), {len});
				}
			}
		}
	}
}

// The same streams split across several handler calls. The split is observable:
// the per-draw environment snapshot is taken once per call on the kernel arm, and
// the kernel re-enters from scratch at every call boundary.
TEST(GsKickKernel, MatchesLegacyAcrossCallSplits)
{
	const std::vector<std::vector<u32>> splits = {
		{1, 1, 1, 1, 1, 1, 1, 1},
		{2, 3, 5, 7, 11, 13},
		{127, 1, 128, 1},
		{64, 64, 64, 64},
		{300},
	};

	u32 seed = 100;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (const auto& split : splits)
		{
			SCOPED_TRACE(::testing::Message() << "prim=" << prim << " split[0]=" << split[0]);
			KickSetup s;
			RunAndCompare(s, prim, false, MakeStream(400, AdcPattern::Stuntman, seed++), split);
		}
	}
}

// A stream whose leading completed prims are all culled: legacy takes the
// environment snapshot at every one of them, the kernel takes it once. The bytes
// must match, which is the whole of the amendment's exactness argument.
TEST(GsKickKernel, LeadingCulledPrimsTakeTheSameSnapshot)
{
	std::mt19937 rng(7);
	std::vector<VertexSpec> v;
	// Forty vertices entirely off the right edge -- every completed prim is culled,
	// so the index buffer stays empty and the snapshot condition stays true.
	for (u32 i = 0; i < 40; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(4000 * 16 + i);
		s.y = static_cast<u16>(100 * 16 + i);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}
	// Then a run that actually draws, so itail leaves zero and the accumulate and
	// draw-rect paths run too.
	for (u32 i = 0; i < 40; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>((50 + (i * 7) % 400) * 16);
		s.y = static_cast<u16>((40 + (i * 13) % 300) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}

	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		KickSetup s;
		RunAndCompare(s, prim, false, v, {80});
		RunAndCompare(s, prim, false, v, {3, 3, 3, 71});
	}
}

// Every prim rejected before the cull test even runs.
TEST(GsKickKernel, InvalidScissorRejectsEverythingIdentically)
{
	KickSetup s;
	s.empty_scissor = true;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::None, 200 + prim), {300});
	}
}

// A nonzero XYOFFSET moves every window position and therefore every outcode and
// band; the kernel derives them scalar-side from the raw 12.4 coordinates while
// the ring holds the vector form, so the two must agree lane for lane.
TEST(GsKickKernel, NonZeroXYOffsetMatches)
{
	KickSetup s;
	s.ofx = 1728 * 16; // the usual PS2 centre offset
	s.ofy = 1808 * 16;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		RunAndCompare(s, prim, false, MakeStream(500, AdcPattern::Stuntman, 300 + prim), {500});
	}
}

// Flat shading takes only the provoking vertex's colour into the min/max
// accumulate; gouraud takes every referenced vertex. The kernel reloads those
// vertices from the buffer instead of using the parse's registers, so both rules
// need driving.
TEST(GsKickKernel, ShadingAndTextureModesMatch)
{
	for (u32 iip : {0u, 1u})
	{
		for (u32 tme : {0u, 1u})
		{
			for (u32 fst : {0u, 1u})
			{
				if (!tme && fst)
					continue;
				SCOPED_TRACE(::testing::Message() << "iip=" << iip << " tme=" << tme << " fst=" << fst);
				KickSetup s;
				s.iip = iip;
				s.tme = tme;
				s.fst = fst;
				RunAndCompare(s, GS_TRIANGLESTRIP, false, MakeStream(400, AdcPattern::Stuntman, 400 + iip * 4 + tme * 2 + fst), {400});
				RunAndCompare(s, GS_TRIANGLELIST, true, MakeStream(400, AdcPattern::None, 500 + iip * 4 + tme * 2 + fst), {400});
			}
		}
	}
}

// AA1 with coverage alpha turns off the scalar-outcode cull, so the handler must
// fall back to the per-vertex path -- which makes both arms literally the same
// code. The test is here to catch the fallback going missing.
TEST(GsKickKernel, Aa1FallsBackToTheLegacyPath)
{
	KickSetup s;
	s.aa1 = 1;
	RunAndCompare(s, GS_TRIANGLESTRIP, false, MakeStream(300, AdcPattern::Stuntman, 600), {300});
}

// Prim types the kernel does not carry keep the per-vertex path.
TEST(GsKickKernel, UncarriedPrimsAreUnchanged)
{
	KickSetup s;
	for (u32 prim : {GS_TRIANGLEFAN, GS_LINESTRIP})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::Stuntman, 700 + prim), {300});
	}
}

// A run long enough to cross the initial 10,000-vertex allocation, so
// GrowVertexBuffer fires. The kernel reserves a whole chunk plus a prim before
// entering; legacy grows one vertex at a time. Growth timing is not in the
// contract, the resulting buffer is.
TEST(GsKickKernel, CrossesVertexBufferGrowth)
{
	// Everything on-screen and non-degenerate so the strip actually accumulates
	// vertices instead of compacting them away.
	std::vector<VertexSpec> v;
	std::mt19937 rng(11);
	for (u32 i = 0; i < 12000; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
		s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}
	KickSetup s;
	RunAndCompare(s, GS_TRIANGLESTRIP, false, v, {1000});
}


// Draw buffering with a recent buffer switch: every vertex must go through the
// per-vertex path while the flag is up, because the overlap check reads the whole
// buffer and can flush. The flag clears itself once a window fills.
TEST(GsKickKernel, DrawBufferingOverlapPrologue)
{
	KickSetup s;
	s.draw_buffering = true;
	s.recent_buffer_switch = true;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::Stuntman, 800 + prim), {300});
		RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::None, 810 + prim), {7, 7, 286});
	}
}

// A run long enough that an accept crosses MaxVerticesForPrim (65,532 for the
// triangle classes) and the kick flushes on vertex count. The kernel stops short
// of that vertex so the per-vertex path performs the flush.
TEST(GsKickKernel, CrossesMaxVerticesForPrim)
{
	std::vector<VertexSpec> v;
	std::mt19937 rng(23);
	for (u32 i = 0; i < 70000; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
		s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}
	KickSetup s;
	EXPECT_GT(RunAndCompare(s, GS_TRIANGLESTRIP, false, v, {5000}), 0u)
		<< "the run never crossed MaxVerticesForPrim, so the flush seam was not exercised";
}

// The comparison's own control: run the SAME streams with the legacy arm on both
// sides. Anything this reports is a property of the harness, not of the kernel --
// which is how the environment's transfer-register window came to be excluded from
// SameEnvCopy. If this ever fails, fix the harness before reading any other
// failure in this file.
TEST(GsKickKernel, HarnessControlIsNotVacuous)
{
	u32 seed = 900;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (bool xyzf2 : {false, true})
		{
			SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2);
			KickSetup s;
			RunAndCompare(s, prim, xyzf2, MakeStream(400, AdcPattern::Stuntman, seed++), {400}, false);
		}
	}
}

// The fused min/max accumulator is the subtlest part of the contract: it is fed
// per emitted prim over a watermark-deduplicated vertex set, and a strip
// compaction moves vertices below the watermark so they have to be folded in
// again. Long random streams are a poor probe for it -- a running min/max
// saturates within a few dozen vertices and then stops noticing missed vertices.
// Short streams with a bimodal value spread do notice: every vertex is either
// near the extreme or near the middle, so a vertex that should have been
// accumulated and was not moves the result.
TEST(GsKickKernel, ShortStreamsWithExtremeValuesPinTheAccumulator)
{
	for (u32 seed = 0; seed < 400; seed++)
	{
		std::mt19937 rng(seed * 2654435761u + 1);
		std::vector<VertexSpec> v;
		const u32 len = 6 + (seed % 24);
		for (u32 i = 0; i < len; i++)
		{
			VertexSpec s = {};
			const bool extreme = (rng() % 3) == 0;
			const bool offscreen = (rng() % 3) == 0;
			if (offscreen)
			{
				s.x = static_cast<u16>(3000 * 16 + (rng() % 64));
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			else
			{
				s.x = static_cast<u16>((rng() % 640) * 16);
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			s.z = extreme ? ((rng() & 1) ? 0xFFFFFFFFu : 0u) : rng();
			s.rgba = extreme ? ((rng() & 1) ? 0xFFFFFFFFu : 0u) : rng();
			s.fog = extreme ? ((rng() & 1) ? 0xFFu : 0u) : (rng() & 0xFF);
			s.s = extreme ? ((rng() & 1) ? 1.0e18f : -1.0e18f) : static_cast<float>(static_cast<int>(rng() % 200) - 100);
			s.t = extreme ? ((rng() & 1) ? 1.0e18f : -1.0e18f) : static_cast<float>(static_cast<int>(rng() % 200) - 100);
			s.q = ((rng() % 8) == 0) ? 0.0f : (0.25f + static_cast<float>(rng() % 8));
			s.adc = (rng() % 5) < 2;
			v.push_back(s);
		}

		for (u32 iip : {0u, 1u})
		{
			for (u32 tme : {0u, 1u})
			{
				for (u32 fst : {0u, 1u})
				{
					if (!tme && fst)
						continue;
					SCOPED_TRACE(::testing::Message() << "seed=" << seed << " len=" << len
					                                  << " iip=" << iip << " tme=" << tme << " fst=" << fst);
					KickSetup s;
					s.iip = iip;
					s.tme = tme;
					s.fst = fst;
					RunAndCompare(s, GS_TRIANGLESTRIP, false, v, {len});
					RunAndCompare(s, GS_TRIANGLELIST, false, v, {len});
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The mirror entry, at the boundaries.
//
// Pass one builds four entries at a time in NEON (GSVertexKickKernel::
// BuildMirrorQuad). That is a second implementation of MakeCullMirrorEntry's
// arithmetic, and the place a second implementation goes wrong is the edges: the
// band shift is arithmetic on a value that can be negative, the two bound tests
// are strict on one side and not the other, and the pack splits a 28-bit field
// across two words. The differential suite would catch a wrong entry only if
// some stream happened to place a vertex on the edge; this places every vertex
// on an edge.
//
// Two things are pinned. First, both scalar builders (banded and raw) against a
// model written from the documented formula rather than from the code. Second,
// the NEON quad against the banded scalar builder, byte for byte.
// ---------------------------------------------------------------------------
namespace
{
	// The formula as GSVertexKick.h's header comment states it, written out.
	GSVertexKernels::CullMirrorEntry ModelEntry(int wx, int wy, const GSVertexKernels::CullBounds& b, bool banded)
	{
		const int bx = (wx - 1) >> 4;
		const int by = (wy - 1) >> 4;
		const int cx = banded ? bx : wx;
		const int cy = banded ? by : wy;

		u64 oc = 0;
		if (cx < b.l) oc |= 1;
		if (cx >= b.r) oc |= 2;
		if (cy < b.t) oc |= 4;
		if (cy >= b.b) oc |= 8;

		GSVertexKernels::CullMirrorEntry e;
		e.xyp = (static_cast<u64>(static_cast<u32>(wy)) << 32) | static_cast<u32>(wx);
		e.meta = (static_cast<u64>(static_cast<u32>(bx)) & 0xFFFFFFFull) |
		         ((static_cast<u64>(static_cast<u32>(by)) & 0xFFFFFFFull) << 28) |
		         (oc << 56);
		return e;
	}

	// Every window coordinate whose band sits on or beside a bound, plus both ends
	// of the band itself (wx = bx*16 + 1 is the first coordinate in band bx and
	// bx*16 + 16 the last), plus a coordinate whose band is negative.
	std::vector<int> BoundaryCoords(int lo_band, int hi_band)
	{
		std::vector<int> out;
		for (int band : {lo_band - 2, lo_band - 1, lo_band, lo_band + 1,
			     hi_band - 2, hi_band - 1, hi_band, hi_band + 1, 0, -1, -2})
		{
			out.push_back(band * 16 + 1);  // first coordinate in the band
			out.push_back(band * 16 + 8);  // middle
			out.push_back(band * 16 + 16); // last
		}
		return out;
	}
} // namespace

TEST(GsKickKernel, ScalarMirrorEntryMatchesTheFormulaAtTheBounds)
{
	const GSVertexKernels::CullBounds bounds = {13, 7, 41, 29};
	const std::vector<int> xs = BoundaryCoords(bounds.l, bounds.r);
	const std::vector<int> ys = BoundaryCoords(bounds.t, bounds.b);

	for (int wx : xs)
	{
		for (int wy : ys)
		{
			SCOPED_TRACE(::testing::Message() << "wx=" << wx << " wy=" << wy);

			const auto banded = GSVertexKernels::MakeCullMirrorEntry<true>(wx, wy, bounds);
			const auto banded_ref = ModelEntry(wx, wy, bounds, true);
			EXPECT_EQ(banded.xyp, banded_ref.xyp) << "banded xyp";
			EXPECT_EQ(banded.meta, banded_ref.meta) << "banded meta";

			const auto raw = GSVertexKernels::MakeCullMirrorEntry<false>(wx, wy, bounds);
			const auto raw_ref = ModelEntry(wx, wy, bounds, false);
			EXPECT_EQ(raw.xyp, raw_ref.xyp) << "raw xyp";
			EXPECT_EQ(raw.meta, raw_ref.meta) << "raw meta";
		}
	}
}

#ifdef ARCH_ARM64
TEST(GsKickKernel, NeonMirrorQuadMatchesTheScalarBuilder)
{
	// The offsets are subtracted from the raw 12.4 coordinate, so a target window
	// coordinate wx is reached as X = wx + ofx with X inside a u16. Both offsets
	// are exercised: zero, and the usual PS2 screen-centre value.
	for (int ofx : {0, 1728 * 16})
	{
		for (int ofy : {0, 1808 * 16})
		{
			const GSVertexKernels::CullBounds bounds = {13, 7, 41, 29};
			const GSVector4i xyof(ofx, ofy, ofx, ofy);
			const auto mb = GSVertexKickKernel::MakeMirrorBounds(xyof, bounds);

			std::vector<int> xs, ys;
			for (int v : BoundaryCoords(bounds.l, bounds.r))
			{
				if (v + ofx >= 0 && v + ofx <= 0xFFFF)
					xs.push_back(v);
			}
			for (int v : BoundaryCoords(bounds.t, bounds.b))
			{
				if (v + ofy >= 0 && v + ofy <= 0xFFFF)
					ys.push_back(v);
			}
			ASSERT_FALSE(xs.empty());
			ASSERT_FALSE(ys.empty());

			// Four vertices per call, so the sweep is walked four at a time with
			// every lane carrying a different boundary and a different ADC bit.
			std::vector<std::pair<int, int>> pts;
			for (int wx : xs)
			{
				for (int wy : ys)
					pts.emplace_back(wx, wy);
			}
			while (pts.size() % 4 != 0)
				pts.push_back(pts.back());

			for (size_t base = 0; base < pts.size(); base += 4)
			{
				alignas(16) u32 raw[4][4] = {};
				GSVertexKernels::CullMirrorEntry expect[4];
				for (int k = 0; k < 4; k++)
				{
					const int wx = pts[base + k].first;
					const int wy = pts[base + k].second;
					raw[k][0] = static_cast<u32>(wx + ofx) & 0xFFFFu;
					raw[k][1] = static_cast<u32>(wy + ofy) & 0xFFFFu;
					raw[k][2] = 0x12345678u;
					// ADC on two lanes of every quad, so a stuck lane shows up.
					raw[k][3] = (k & 1) ? 0x8000u : 0u;

					expect[k] = GSVertexKernels::MakeCullMirrorEntry<true>(wx, wy, bounds);
					if (k & 1)
						expect[k].meta |= GSVertexKickKernel::kCullMetaAdcBit;
				}

				alignas(16) u64 got_xyp[4] = {};
				alignas(16) u64 got_meta[4] = {};
				GSVertexKickKernel::BuildMirrorQuad(vld1q_u32(raw[0]), vld1q_u32(raw[1]),
					vld1q_u32(raw[2]), vld1q_u32(raw[3]), mb, got_xyp, got_meta);

				for (int k = 0; k < 4; k++)
				{
					SCOPED_TRACE(::testing::Message() << "ofx=" << ofx << " ofy=" << ofy << " lane=" << k
					                                  << " wx=" << pts[base + k].first
					                                  << " wy=" << pts[base + k].second);
					EXPECT_EQ(got_xyp[k], expect[k].xyp) << "xyp";
					EXPECT_EQ(got_meta[k], expect[k].meta) << "meta";
				}
			}
		}
	}
}
#endif // ARCH_ARM64

// The environment moving under a run in progress.
//
// The two-pass kernel reads the offset, the cull bounds and PRIM's shading bits
// once per chunk and then decides a whole chunk against them. The per-vertex kick
// reads all of them out of memory on EVERY vertex. Between those two positions
// sits a flush: a legacy kick inside the handler's loop can flush on vertex count
// or on a draw-buffer overlap, and what a flush restores afterwards is a
// different environment. A kernel still holding the copy it took before that seam
// decides the rest of the run against an environment that no longer exists.
//
// This drives it directly: the probe's Draw() -- which FlushPrim calls -- moves
// the scissor, the offset and the shading bits, so the vertices after the flush
// must be culled, offset and shaded by the new ones. Both arms see the same
// mutation, so any difference is the kernel reading a stale copy.
TEST(GsKickKernel, EnvironmentMovingUnderTheRunIsPickedUp)
{
	std::vector<VertexSpec> v;
	std::mt19937 rng(31);
	for (u32 i = 0; i < 70000; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
		s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}

	KickSetup s;
	EXPECT_GT(RunAndCompare(s, GS_TRIANGLESTRIP, false, v, {5000}, true, true), 0u)
		<< "the run never flushed, so the environment never moved";
}

// ---------------------------------------------------------------------------
// Stage 3b: the auto_flush = true instantiations.
//
// The whole autoflush test is a no-op for a run of vertices whenever
// IsAutoFlushDraw is false -- every store and every flush HandleAutoFlush can
// perform is inside that predicate's body, and the predicate reads only draw
// state, which nothing inside a kernel chunk can change. So the chunk is routed:
// predicate false takes the two-pass kernel, predicate true keeps the staged
// per-vertex loop that reads the incoming vertex out of m_v.
//
// These tests drive the SAME differential comparison as the rest of the file
// through the auto_flush = true handler, in both predicate states, and across a
// flush that moves the predicate under the run.
// ---------------------------------------------------------------------------

namespace
{
	// A setup whose IsAutoFlushDraw answers false with the whole predicate having
	// run: autoflush enabled, texture mapping on, and TEX0 pointing at a block
	// that is neither FRAME's nor ZBUF's. This is the case 3b routes.
	KickSetup AutoFlushInertSetup()
	{
		KickSetup s;
		s.autoflush = GSHWAutoFlushLevel::Enabled;
		s.autoflush_hit = false;
		s.tme = 1;
		return s;
	}

	// The same, with TEX0 on FRAME's own block, so the predicate answers true and
	// the run must stay on the staged loop.
	KickSetup AutoFlushLiveSetup()
	{
		KickSetup s = AutoFlushInertSetup();
		s.autoflush_hit = true;
		return s;
	}
} // namespace

// The setups do what they claim. Everything below reads as a routing test only
// because of this, so it is asserted rather than assumed.
TEST(GsKickKernel, AutoFlushSetupsProduceTheIntendedPredicate)
{
	const AutoFlushGuard guard(GSHWAutoFlushLevel::Enabled);
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		SCOPED_TRACE(::testing::Message() << "prim=" << prim);
		auto inert = MakeProbe(AutoFlushInertSetup(), prim, true);
		EXPECT_FALSE(inert->AutoFlushPredicate(prim)) << "the inert setup must answer false";

		auto live = MakeProbe(AutoFlushLiveSetup(), prim, true);
		EXPECT_TRUE(live->AutoFlushPredicate(prim)) << "the live setup must answer true";
	}
	KickProbe::s_fused_kick_use_kernel = true;
}

// The whole matrix -- three prims, both packed layouts, three ADC patterns, every
// run length -- through the auto_flush = true handler with the predicate inert.
// This is the case the routing sends to the kernel, so it is the case where the
// kernel arm and the staged arm are different code and the comparison has content.
TEST(GsKickKernel, AutoFlushInertMatchesStagedAcrossPrimsLayoutsAndRunLengths)
{
	const u32 prims[] = {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE};
	const AdcPattern adcs[] = {AdcPattern::None, AdcPattern::Stuntman, AdcPattern::Katamari};

	u32 seed = 5000;
	for (u32 prim : prims)
	{
		for (bool xyzf2 : {false, true})
		{
			for (AdcPattern adc : adcs)
			{
				for (u32 len : kRunLengths)
				{
					SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2
					                                  << " adc=" << static_cast<int>(adc) << " len=" << len);
					bool predicate = true;
					RunAndCompare(AutoFlushInertSetup(), prim, xyzf2, MakeStream(len, adc, seed++), {len},
						true, false, true, false, &predicate);
					EXPECT_FALSE(predicate) << "this stream was supposed to run with the predicate inert";
				}
			}
		}
	}
}

// The same streams split across handler calls, which is where the per-call
// snapshot rule and the per-chunk re-evaluation both become observable.
TEST(GsKickKernel, AutoFlushInertMatchesStagedAcrossCallSplits)
{
	const std::vector<std::vector<u32>> splits = {
		{1, 1, 1, 1, 1, 1, 1, 1},
		{2, 3, 5, 7, 11, 13},
		{127, 1, 128, 1},
		{64, 64, 64, 64},
		{300},
	};

	u32 seed = 5300;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (const auto& split : splits)
		{
			SCOPED_TRACE(::testing::Message() << "prim=" << prim << " split[0]=" << split[0]);
			RunAndCompare(AutoFlushInertSetup(), prim, false, MakeStream(400, AdcPattern::Stuntman, seed++),
				split, true, false, true);
		}
	}
}

// The predicate live. Both arms must stay on the staged loop, so the two sides of
// this comparison are the same code and it can only fail if the routing sent a
// live chunk to the kernel.
TEST(GsKickKernel, AutoFlushLiveStaysStaged)
{
	u32 seed = 5600;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (bool xyzf2 : {false, true})
		{
			for (u32 len : {5u, 127u, 128u, 129u, 400u})
			{
				SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2 << " len=" << len);
				bool predicate = false;
				RunAndCompare(AutoFlushLiveSetup(), prim, xyzf2, MakeStream(len, AdcPattern::Stuntman, seed++),
					{len}, true, false, true, false, &predicate);
				EXPECT_TRUE(predicate) << "this stream was supposed to run with the predicate live";
			}
		}
	}
}

// Sprites are not routed at all in 3b: EarlyDetectShuffle reads the incoming
// vertex out of m_v, so the predicate is per-prim state for the sprite class and
// not chunk-invariant. Drive the sprite handler in both predicate states with a
// texture-mapped 16-bit FRAME and TEX0, which is the shape EarlyDetectShuffle
// actually inspects, so a routing decision that ignored the prim class would show.
TEST(GsKickKernel, AutoFlushSpritesStayStagedUnderShuffleShapedState)
{
	for (bool hit : {false, true})
	{
		KickSetup s = hit ? AutoFlushLiveSetup() : AutoFlushInertSetup();
		s.fst = 1;
		SCOPED_TRACE(::testing::Message() << "autoflush_hit=" << hit);
		RunAndCompare(s, GS_SPRITE, false, MakeStream(300, AdcPattern::Stuntman, 5900 + hit), {300},
			true, false, true);
		RunAndCompare(s, GS_SPRITE, true, MakeStream(300, AdcPattern::None, 5910 + hit), {7, 7, 286},
			true, false, true);
	}
}

// The predicate moving under a run in progress.
//
// The routing decision is taken once per chunk, and what can move it is a flush:
// a flush restores a buffered environment, which is a different TEX0 against the
// same FRAME. A driver that took the decision once per handler call instead of
// once per chunk would run the rest of a call on the wrong arm. This drives it:
// the probe's Draw() flips TEX0 between FRAME's block and another one on every
// draw, and the stream is long enough that the vertex-count flush fires inside a
// call, so the predicate crosses in both directions mid-call.
TEST(GsKickKernel, AutoFlushPredicateMovingUnderTheRunIsPickedUp)
{
	std::vector<VertexSpec> v;
	std::mt19937 rng(41);
	for (u32 i = 0; i < 70000; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
		s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}

	EXPECT_GT(RunAndCompare(AutoFlushInertSetup(), GS_TRIANGLESTRIP, false, v, {5000},
				  true, false, true, true),
		0u)
		<< "the run never flushed, so the predicate never moved";
	EXPECT_GT(RunAndCompare(AutoFlushLiveSetup(), GS_TRIANGLESTRIP, false, v, {5000},
				  true, false, true, true),
		0u)
		<< "the run never flushed, so the predicate never moved";
}

// The environment moving under a run, on the autoflush arm. Same shape as
// EnvironmentMovingUnderTheRunIsPickedUp: the scissor, the offset and the shading
// bits all move at every draw, and the vertices after a flush must be decided
// against the new ones. On this arm the invariants are rebuilt at a chunk
// boundary that is also a routing decision, so both have to be re-read together.
TEST(GsKickKernel, AutoFlushEnvironmentMovingUnderTheRunIsPickedUp)
{
	std::vector<VertexSpec> v;
	std::mt19937 rng(43);
	for (u32 i = 0; i < 70000; i++)
	{
		VertexSpec s = {};
		s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
		s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
		s.z = rng();
		s.rgba = rng();
		s.q = 1.0f;
		v.push_back(s);
	}

	EXPECT_GT(RunAndCompare(AutoFlushInertSetup(), GS_TRIANGLESTRIP, false, v, {5000}, true, true, true), 0u)
		<< "the run never flushed, so the environment never moved";
}

// The remaining shapes from the direct arm, repeated on the autoflush arm: an
// invalid scissor (every prim rejected before the cull), a nonzero XYOFFSET, AA1
// (the kernel does not apply, so the handler must fall back), the prim types the
// kernel does not carry, draw buffering's overlap prologue, and a run that grows
// the vertex buffer.
TEST(GsKickKernel, AutoFlushInertCoversTheSeamShapes)
{
	{
		KickSetup s = AutoFlushInertSetup();
		s.empty_scissor = true;
		for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
		{
			SCOPED_TRACE(::testing::Message() << "invalid scissor, prim=" << prim);
			RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::None, 6000 + prim), {300}, true, false, true);
		}
	}
	{
		KickSetup s = AutoFlushInertSetup();
		s.ofx = 1728 * 16;
		s.ofy = 1808 * 16;
		for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
		{
			SCOPED_TRACE(::testing::Message() << "xyoffset, prim=" << prim);
			RunAndCompare(s, prim, false, MakeStream(500, AdcPattern::Stuntman, 6100 + prim), {500}, true, false, true);
		}
	}
	{
		KickSetup s = AutoFlushInertSetup();
		s.aa1 = 1;
		SCOPED_TRACE("aa1");
		RunAndCompare(s, GS_TRIANGLESTRIP, false, MakeStream(300, AdcPattern::Stuntman, 6200), {300}, true, false, true);
	}
	{
		KickSetup s = AutoFlushInertSetup();
		for (u32 prim : {GS_TRIANGLEFAN, GS_LINESTRIP})
		{
			SCOPED_TRACE(::testing::Message() << "uncarried prim=" << prim);
			RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::Stuntman, 6300 + prim), {300}, true, false, true);
		}
	}
	{
		KickSetup s = AutoFlushInertSetup();
		s.draw_buffering = true;
		s.recent_buffer_switch = true;
		for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
		{
			SCOPED_TRACE(::testing::Message() << "overlap prologue, prim=" << prim);
			RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::Stuntman, 6400 + prim), {300}, true, false, true);
			RunAndCompare(s, prim, false, MakeStream(300, AdcPattern::None, 6410 + prim), {7, 7, 286}, true, false, true);
		}
	}
	{
		std::vector<VertexSpec> v;
		std::mt19937 rng(47);
		for (u32 i = 0; i < 12000; i++)
		{
			VertexSpec s = {};
			s.x = static_cast<u16>(((i * 37) % 600 + 10) * 16);
			s.y = static_cast<u16>(((i * 53) % 400 + 10) * 16);
			s.z = rng();
			s.rgba = rng();
			s.q = 1.0f;
			v.push_back(s);
		}
		SCOPED_TRACE("buffer growth");
		RunAndCompare(AutoFlushInertSetup(), GS_TRIANGLESTRIP, false, v, {1000}, true, false, true);
	}
}

// Short streams with a bimodal value spread, on the autoflush arm: the fused
// min/max accumulator is the subtlest part of the contract and the arm that
// reaches it through the routing is a different driver.
TEST(GsKickKernel, AutoFlushInertShortStreamsPinTheAccumulator)
{
	for (u32 seed = 0; seed < 120; seed++)
	{
		std::mt19937 rng(seed * 2246822519u + 7);
		std::vector<VertexSpec> v;
		const u32 len = 6 + (seed % 24);
		for (u32 i = 0; i < len; i++)
		{
			VertexSpec s = {};
			const bool extreme = (rng() % 3) == 0;
			const bool offscreen = (rng() % 3) == 0;
			if (offscreen)
			{
				s.x = static_cast<u16>(3000 * 16 + (rng() % 64));
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			else
			{
				s.x = static_cast<u16>((rng() % 640) * 16);
				s.y = static_cast<u16>((rng() % 448) * 16);
			}
			s.z = extreme ? ((rng() & 1) ? 0xFFFFFFFFu : 0u) : rng();
			s.rgba = extreme ? ((rng() & 1) ? 0xFFFFFFFFu : 0u) : rng();
			s.fog = extreme ? ((rng() & 1) ? 0xFFu : 0u) : (rng() & 0xFF);
			s.s = extreme ? ((rng() & 1) ? 1.0e18f : -1.0e18f) : static_cast<float>(static_cast<int>(rng() % 200) - 100);
			s.t = extreme ? ((rng() & 1) ? 1.0e18f : -1.0e18f) : static_cast<float>(static_cast<int>(rng() % 200) - 100);
			s.q = ((rng() % 8) == 0) ? 0.0f : (0.25f + static_cast<float>(rng() % 8));
			s.adc = (rng() % 5) < 2;
			v.push_back(s);
		}

		for (u32 iip : {0u, 1u})
		{
			SCOPED_TRACE(::testing::Message() << "seed=" << seed << " len=" << len << " iip=" << iip);
			KickSetup s = AutoFlushInertSetup();
			s.iip = iip;
			RunAndCompare(s, GS_TRIANGLESTRIP, false, v, {len}, true, false, true);
			RunAndCompare(s, GS_TRIANGLELIST, false, v, {len}, true, false, true);
		}
	}
}

// The autoflush arm's own harness control: the same streams with the STAGED arm
// on both sides. Anything this reports is a property of the harness -- the
// autoflush context it builds, the flushes that context causes, the predicate
// toggling -- and not of the routing.
TEST(GsKickKernel, AutoFlushHarnessControlIsNotVacuous)
{
	u32 seed = 6600;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (bool xyzf2 : {false, true})
		{
			for (bool hit : {false, true})
			{
				SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2 << " hit=" << hit);
				const KickSetup s = hit ? AutoFlushLiveSetup() : AutoFlushInertSetup();
				RunAndCompare(s, prim, xyzf2, MakeStream(400, AdcPattern::Stuntman, seed++), {400}, false, false, true);
			}
		}
	}
}

// The small-count bypass. Below the compile-time threshold the handler runs the
// per-vertex batch for its own arm rather than entering the kernel; every count
// on both sides of the threshold has to land on the same state, and the counts
// that matter are the ones below a prim's worth.
TEST(GsKickKernel, SmallCountsMatchOnBothArms)
{
	u32 seed = 6900;
	for (u32 prim : {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_SPRITE})
	{
		for (bool xyzf2 : {false, true})
		{
			for (u32 len = 1; len <= 16; len++)
			{
				SCOPED_TRACE(::testing::Message() << "prim=" << prim << " xyzf2=" << xyzf2 << " len=" << len);
				RunAndCompare(KickSetup{}, prim, xyzf2, MakeStream(len, AdcPattern::Stuntman, seed++), {len});
				RunAndCompare(AutoFlushInertSetup(), prim, xyzf2, MakeStream(len, AdcPattern::None, seed++), {len},
					true, false, true);
				// One vertex per handler call, which is the shape a bypass gets
				// wrong by carrying state between calls it must not carry.
				RunAndCompare(KickSetup{}, prim, xyzf2, MakeStream(len, AdcPattern::Stuntman, seed++),
					std::vector<u32>(len, 1u));
			}
		}
	}
}
