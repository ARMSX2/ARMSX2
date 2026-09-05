// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The block a TEXTURED draw interpolates is eight pixels wide, like every other
// draw's, and on a four-lane host that is two vectors.
//
// gs_sw_scanline_cpath_tests.cpp already pins the eight-wide walk for an
// UNTEXTURED draw. This suite is the textured half, and it exists because the two
// were different for a year: the width was read as eight without texture mapping
// and four with it, and only the untextured half was ever measured.
//
// What measured it: with the colour gradient formed the way silicon forms it, the
// gs-walk2 capture's width section splits by TME into an untextured arm that
// agrees with the console on 93.0% and a textured arm on 92.6%, and the same model
// scored at eight puts the textured arm at 96.9%. The console prefers eight on both
// arms. The four we shipped came from a document rather than a measurement, and
// the two errors -- the wrong gradient and the wrong width -- were partly
// cancelling on textured draws, so correcting the gradient is what exposed it.
//
// The draw here is MODULATE against a single texel of 0x80808080, which is the
// texture function's identity: the stored pixel is the interpolated vertex colour
// byte and nothing else, so the block walk is read directly out of the
// framebuffer. The coordinate is held still, so the colour walk is the only thing
// moving.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSBlockWalk.h"
#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSState.h"
#include "common/HostSys.h"

#include <gtest/gtest.h>

#include <cstring>

#ifndef _WIN32
#include <sys/mman.h>
#else
#include "common/RedtapeWindows.h"
#endif

namespace
{
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
using SetupPrimPtr = void (*)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);

// The scanline carries a colour multiplied by 128: seven fractional bits under the
// byte the GS stores.
constexpr float kColorScale = 128.0f;

// MODULATE's identity texel. (c * 0x80) >> 7 == c for every c in 0..255.
constexpr u32 kIdentityTexel = 0x80808080u;

constexpr int kRowPixels = 64;

struct Span
{
	float r0, g0, b0, a0;
	float dr, dg, db, da;
};

struct Reading
{
	u32 px[kRowPixels];
	GSVector4i d4c;
	GSVector4 d4stq;
	GSScanlineLocalData::blockstep dw[8][2];
};

// The rule, stated once and independently of either transcription of it. The value
// at a pixel is the truncated seed, plus one truncated whole-block step for every
// block boundary between it and the seed's block, plus the truncated gradient at
// its own column inside the block measured from the seed's column. Nothing in it
// mentions the host's vector.
int BlockWalkModel(float c0, float dc, int x0, int x, int w)
{
	const int xb0 = x0 & ~(w - 1);
	const int s = x0 - xb0;
	const int m = (x - xb0) % w;
	const int b = (x - xb0) / w;

	const int seed = static_cast<int>(c0 * kColorScale);
	const int block = static_cast<int>(dc * kColorScale * static_cast<float>(w));
	const int lane = static_cast<int>(dc * kColorScale * static_cast<float>(m - s));

	return (seed + b * block + lane) >> 7;
}

class SwTexturedBlockWidthTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 8 * kCodeSlotSize;

	static void SetUpTestSuite()
	{
#ifdef _WIN32
		s_code = static_cast<u8*>(VirtualAlloc(nullptr, kCodeBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#elif defined(__APPLE__)
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#else
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#endif
		s_code_used = 0;
		s_mem = new GSLocalMemory();
	}

	static void TearDownTestSuite()
	{
		delete s_mem;
		s_mem = nullptr;

		if (s_code)
		{
#ifdef _WIN32
			VirtualFree(s_code, 0, MEM_RELEASE);
#else
			munmap(s_code, kCodeBufferSize);
#endif
			s_code = nullptr;
		}
	}

	static u8* Slot()
	{
		if (!s_code || s_code_used + kCodeSlotSize > kCodeBufferSize)
			return nullptr;

		u8* slot = s_code + s_code_used;
		s_code_used += kCodeSlotSize;
		return slot;
	}

	static DrawScanlinePtr CompileScanline(GSScanlineSelector sel)
	{
		u8* slot = Slot();
		if (!slot)
			return nullptr;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSDrawScanlineCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<DrawScanlinePtr>(const_cast<u8*>(cg.GetCode()));
	}

	// The reduced key GetScanlineGlobalData builds, so this is the same variant a
	// real draw would compile.
	static SetupPrimPtr CompileSetup(GSScanlineSelector full)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.iip = full.iip;
		sel.tfx = full.tfx;
		sel.tcc = full.tcc;
		sel.fst = full.fst;
		sel.fge = full.fge;
		sel.prim = full.prim;
		sel.fb = full.fb;
		sel.zb = full.zb;
		sel.zoverflow = full.zoverflow;
		sel.zequal = full.zequal;
		sel.notest = full.notest;

		u8* slot = Slot();
		if (!slot)
			return nullptr;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSSetupPrimCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<SetupPrimPtr>(const_cast<u8*>(cg.GetCode()));
	}

	// A gouraud, MODULATE-textured triangle into PSMCT32, nearest-filtered and
	// unblended, with the coordinate clamped to one texel so nothing about
	// addressing or filtering reaches the framebuffer.
	static GSScanlineSelector MakeSelector(bool notest)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_MODULATE;
		sel.tcc = 1;
		sel.fst = 1;
		sel.ltf = 0;
		sel.tlu = 0;
		sel.tw = 0;
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 1;
		sel.fwrite = 1;
		sel.notest = notest ? 1 : 0;
		sel.colclamp = 1;
		return sel;
	}

	static const GSPixelOffset4* GetOffsets()
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 1;
		frame.PSM = PSMCT32;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 256;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(int x, int y)
	{
		return GSLocalMemory::m_psm[PSMCT32].info.pa(x, y, 0, 1);
	}

	static void Run(GSScanlineSelector sel, SetupPrimPtr setup, DrawScanlinePtr draw,
		const Span& span, const GSVector4& dt, int left, int pixels, Reading& out)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < kRowPixels; x++)
			vm32[PixelAddr(x, 0)] = 0u;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};
		alignas(32) u32 tex[4] = {kIdentityTexel, kIdentityTexel, kIdentityTexel, kIdentityTexel};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = tex;
		// min = max = 0 with an all-zero mask is CLAMP to texel zero on both axes,
		// so every lane fetches tex[0] whatever the coordinate is.
		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.mask = GSVector4i::zero();

		local.gd = &global;

		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		dscan.c = GSVector4(span.dr, span.dg, span.db, span.da) * kColorScale;
		dscan.t = dt;

		setup(vertex, index, dscan, local);

		out.d4c = local.d4.c;
		out.d4stq = local.d4.stq;
		std::memcpy(out.dw, local.dw, sizeof(out.dw));

		GSVertexSW scan = GSVertexSW::zero();
		scan.c = GSVector4(span.r0, span.g0, span.b0, span.a0) * kColorScale;

		draw(pixels, left, 0, scan, local);

		for (int x = 0; x < kRowPixels; x++)
			out.px[x] = vm32[PixelAddr(x, 0)];
	}

	static bool RunJit(bool notest, const Span& span, const GSVector4& dt, int left, int pixels, Reading& out)
	{
		const int slot = notest ? 1 : 0;
		if (!s_setup[slot])
		{
			s_sel[slot] = MakeSelector(notest);
			s_setup[slot] = CompileSetup(s_sel[slot]);
			s_draw[slot] = CompileScanline(s_sel[slot]);
		}

		if (!s_setup[slot] || !s_draw[slot])
			return false;

		Run(s_sel[slot], s_setup[slot], s_draw[slot], span, dt, left, pixels, out);
		return true;
	}

	static void RunCpp(bool notest, const Span& span, const GSVector4& dt, int left, int pixels, Reading& out)
	{
		Run(MakeSelector(notest), &isa_native::GSDrawScanline::CSetupPrim,
			static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline),
			span, dt, left, pixels, out);
	}

	static GSScanlineSelector s_sel[2];
	static SetupPrimPtr s_setup[2];
	static DrawScanlinePtr s_draw[2];

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

GSScanlineSelector SwTexturedBlockWidthTest::s_sel[2] = {};
SetupPrimPtr SwTexturedBlockWidthTest::s_setup[2] = {nullptr, nullptr};
DrawScanlinePtr SwTexturedBlockWidthTest::s_draw[2] = {nullptr, nullptr};
u8* SwTexturedBlockWidthTest::s_code = nullptr;
size_t SwTexturedBlockWidthTest::s_code_used = 0;
GSLocalMemory* SwTexturedBlockWidthTest::s_mem = nullptr;

// Gradients deliberately off any binade, so a whole-block step is not accidentally
// eight lane steps and the truncation has something to lose. This is the span the
// two widths disagree on.
constexpr Span kSeparatingSpan = {40.0f, 90.0f, 60.0f, 120.0f, 1.3f, -0.7f, 2.1f, 0.55f};

// Gradients that are whole numbers on the scanline's own 2^-7 grid. There the
// truncation is the identity, trunc(g*8) is exactly two trunc(g*4)s, and the two
// widths are the same walk.
constexpr Span kAgreeingSpan = {40.0f, 90.0f, 60.0f, 120.0f, 1.0f, -2.0f, 3.0f, 0.5f};

// The separation, checked before anything is scored against it: a test that cannot
// tell four from eight proves nothing when it passes. This is a property of the
// model alone, so it holds whatever the renderer does.
TEST_F(SwTexturedBlockWidthTest, TheSeparatingSpanReallySeparatesFourFromEight)
{
	int differing = 0;

	for (int left = 0; left < 8; left++)
	{
		for (int x = left; x < 40; x++)
		{
			if (BlockWalkModel(kSeparatingSpan.r0, kSeparatingSpan.dr, left, x, 4)
				!= BlockWalkModel(kSeparatingSpan.r0, kSeparatingSpan.dr, left, x, 8))
			{
				differing++;
			}
		}
	}

	EXPECT_GT(differing, 0) << "the separating span does not separate the two widths";
}

// The rule. A textured draw's block is eight pixels wide, and on this host that is
// two vectors of the alternating pair.
TEST_F(SwTexturedBlockWidthTest, ATexturedGouraudSpanWalksInEightPixelBlocks)
{
	for (int left = 0; left < 8; left++)
	{
		Reading jit;
		ASSERT_TRUE(RunJit(false, kSeparatingSpan, GSVector4::zero(), left, 40 - left, jit));

		for (int x = left; x < 40; x++)
		{
			SCOPED_TRACE(testing::Message() << "left " << left << " pixel " << x);

			EXPECT_EQ(static_cast<int>(jit.px[x] & 0xff),
				BlockWalkModel(kSeparatingSpan.r0, kSeparatingSpan.dr, left, x, 8)) << "red";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 8) & 0xff),
				BlockWalkModel(kSeparatingSpan.g0, kSeparatingSpan.dg, left, x, 8)) << "green";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 16) & 0xff),
				BlockWalkModel(kSeparatingSpan.b0, kSeparatingSpan.db, left, x, 8)) << "blue";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 24) & 0xff),
				BlockWalkModel(kSeparatingSpan.a0, kSeparatingSpan.da, left, x, 8)) << "alpha";
		}
	}
}

// The control that has to hold on both sides of the change: where the truncation is
// the identity the two widths are one walk, so this span answers the same whatever
// the width is. If it ever moves, something other than the width did.
TEST_F(SwTexturedBlockWidthTest, AGradientOnTheGridIsTheSameWalkAtEitherWidth)
{
	for (int left = 0; left < 8; left++)
	{
		Reading jit;
		ASSERT_TRUE(RunJit(false, kAgreeingSpan, GSVector4::zero(), left, 40 - left, jit));

		for (int x = left; x < 40; x++)
		{
			SCOPED_TRACE(testing::Message() << "left " << left << " pixel " << x);

			ASSERT_EQ(BlockWalkModel(kAgreeingSpan.r0, kAgreeingSpan.dr, left, x, 4),
				BlockWalkModel(kAgreeingSpan.r0, kAgreeingSpan.dr, left, x, 8));

			EXPECT_EQ(static_cast<int>(jit.px[x] & 0xff),
				BlockWalkModel(kAgreeingSpan.r0, kAgreeingSpan.dr, left, x, 8)) << "red";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 8) & 0xff),
				BlockWalkModel(kAgreeingSpan.g0, kAgreeingSpan.dg, left, x, 8)) << "green";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 16) & 0xff),
				BlockWalkModel(kAgreeingSpan.b0, kAgreeingSpan.db, left, x, 8)) << "blue";
			EXPECT_EQ(static_cast<int>((jit.px[x] >> 24) & 0xff),
				BlockWalkModel(kAgreeingSpan.a0, kAgreeingSpan.da, left, x, 8)) << "alpha";
		}
	}
}

// The two halves of a split block advance by exactly one whole block step between
// them. If this fails the walk drifts a little every block, which is the error that
// only shows up hundreds of pixels along a span.
TEST_F(SwTexturedBlockWidthTest, TheTwoPhasesOfATexturedStepSumToTheBlockStep)
{
	static const float kSteps[] = {-85.0f, -17.0f, -1.3f, 0.0f, 0.55f, 17.0f, 85.0f};

	for (float dr : kSteps)
	{
		const Span span = {128.0f, 128.0f, 128.0f, 128.0f, dr, -dr, dr * 0.5f, -dr * 0.25f};

		Reading jit;
		ASSERT_TRUE(RunJit(false, span, GSVector4::zero(), 0, 32, jit));

		for (int s = 0; s < 8; s++)
		{
			for (int lane = 0; lane < 4; lane++)
			{
				// Two 16-bit channels share each word, so each is summed on its own
				// rather than letting one carry into its neighbour.
				const u32 rb0 = jit.dw[s][0].rb.U32[lane], rb1 = jit.dw[s][1].rb.U32[lane];
				const u32 ga0 = jit.dw[s][0].ga.U32[lane], ga1 = jit.dw[s][1].ga.U32[lane];

				SCOPED_TRACE(testing::Message() << "dr " << dr << " s " << s << " lane " << lane);

				EXPECT_EQ((rb0 + rb1) & 0xffff, jit.d4c.U32[0] & 0xffff) << "red";
				EXPECT_EQ(((rb0 >> 16) + (rb1 >> 16)) & 0xffff, (jit.d4c.U32[0] >> 16) & 0xffff) << "blue";
				EXPECT_EQ((ga0 + ga1) & 0xffff, jit.d4c.U32[1] & 0xffff) << "green";
				EXPECT_EQ(((ga0 >> 16) + (ga1 >> 16)) & 0xffff, (jit.d4c.U32[1] >> 16) & 0xffff) << "alpha";
			}
		}
	}
}

// The coordinate is not the colour. Its step is what the scanline adds once per
// VECTOR, and widening the colour's block must not widen that -- a coordinate
// stepping a whole block every four pixels would sample the texture twice as fast
// as the draw walks. Pinned on the setup's own output because it is the quantity
// the mistake would land in.
TEST_F(SwTexturedBlockWidthTest, TheCoordinateStepStaysOneVector)
{
	const Span span = {40.0f, 90.0f, 60.0f, 120.0f, 1.3f, -0.7f, 2.1f, 0.55f};
	const GSVector4 dt = GSVector4(2000.0f, -1500.0f, 0.0f, 0.0f);

	Reading jit;
	ASSERT_TRUE(RunJit(false, span, dt, 0, 32, jit));

	// The host vector this build walks with, in lanes.
	constexpr int vlen = static_cast<int>(sizeof(GSVector4) / sizeof(float));
	const GSVector4i step = GSVector4i::cast(jit.d4stq);

	EXPECT_EQ(step.I32[0], static_cast<int>(dt.x) * vlen) << "u";
	EXPECT_EQ(step.I32[1], static_cast<int>(dt.y) * vlen) << "v";
}

// The C++ reference and the generated scanline are two transcriptions of one
// design, and this project has had them drift. Both walk the same textured span.
TEST_F(SwTexturedBlockWidthTest, TheTwoPathsAgreeOnEveryTexturedBlockStart)
{
	for (int left = 0; left < 8; left++)
	{
		Reading jit, cpp;
		ASSERT_TRUE(RunJit(false, kSeparatingSpan, GSVector4::zero(), left, 32, jit));
		RunCpp(false, kSeparatingSpan, GSVector4::zero(), left, 32, cpp);

		for (int x = 0; x < kRowPixels; x++)
			EXPECT_EQ(cpp.px[x], jit.px[x]) << "block start " << left << ": pixel " << x;

		for (int s = 0; s < 8; s++)
		{
			for (int ph = 0; ph < 2; ph++)
			{
				for (int lane = 0; lane < 4; lane++)
				{
					EXPECT_EQ(cpp.dw[s][ph].rb.U32[lane], jit.dw[s][ph].rb.U32[lane])
						<< "block step dw[" << s << "][" << ph << "].rb lane " << lane;
					EXPECT_EQ(cpp.dw[s][ph].ga.U32[lane], jit.dw[s][ph].ga.U32[lane])
						<< "block step dw[" << s << "][" << ph << "].ga lane " << lane;
				}
			}
		}
	}
}

} // namespace

#endif // ARCH_ARM64
