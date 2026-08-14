// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The C++ rasteriser and the generated one, on the same span, must agree.
//
// They are two transcriptions of one design and nothing checked them against each
// other, so they drifted: the C++ setup packed the per-lane colour offsets with the
// SIGNED saturating pack while both generators used the unsigned one. The mask
// above the pack has already put every lane in 0..65535, which makes the unsigned
// pack the identity -- and makes the signed pack saturate everything from 32768 up
// to 32767. A descending gouraud gradient is exactly how a lane gets there: its
// offset is negative, the mask turns it into a large positive, and the signed pack
// flattens it. Every pixel of the group then carries that instead of its own
// colour, for the whole scanline.
//
// Upstream had already fixed this once -- "GS/SW: Mask color gradients to prevent
// incorrect clamping" is the commit that introduced the mask and the unsigned pack
// together -- and a later refactor that rewrote the same lines to change how the
// shift table is loaded retyped the tail back to the signed pack. The generators
// were not touched by that refactor, which is why only the C++ path regressed and
// why nothing noticed: shipping builds always take the generated code.
//
// The C++ path is not dead. It is the whole renderer wherever there is no code
// memory to compile into, which is a real shipping configuration on locked-down
// platforms, and it is the only path a measurement can run without a JIT.
//
// This suite exists to make that class of drift loud. It runs both paths over the
// same spans and compares what they store, so a future divergence anywhere in the
// scanline -- not only in colour -- fails here.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSState.h"
#include "common/HostSys.h"

#include <gtest/gtest.h>

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

// A gouraud span, in colour units per pixel.
struct Span
{
	float r0, g0, b0, a0;
	float dr, dg, db, da;
};

// Everything a run produces that the two paths have to agree on: what reached the
// framebuffer, and the setup state the scanline spent to get there.
struct Reading
{
	u32 px[4];
	GSVector4i d_rb[4];
	GSVector4i d_ga[4];
	GSVector4i d4c;

	// How many of the four per-lane offset vectors the scanline can reach. A span
	// that is known to start on a group boundary reads only the first, and the
	// generator declines to compute the rest where the C++ writes them anyway --
	// dead either way, so comparing them would be comparing scratch.
	int live_offsets;
};

class SwScanlineCPathTest : public ::testing::Test
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

	// A gouraud triangle with no texture into PSMCT32: the stored pixel IS the
	// interpolated colour, so nothing between the DDA and the readback can hide a
	// difference.
	static GSScanlineSelector MakeSelector(bool notest)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_NONE;
		sel.tcc = 0;
		sel.fst = 1;
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

	// Runs one span through one pair of functions. `left` chooses the lane the span
	// starts on, which is what selects between the four per-lane offset vectors.
	static void Run(GSScanlineSelector sel, SetupPrimPtr setup, DrawScanlinePtr draw,
		const Span& span, int left, int pixels, Reading& out)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0u;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));

		local.gd = &global;

		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		dscan.c = GSVector4(span.dr, span.dg, span.db, span.da) * kColorScale;

		setup(vertex, index, dscan, local);

		for (int i = 0; i < 4; i++)
		{
			out.d_rb[i] = local.d[i].rb;
			out.d_ga[i] = local.d[i].ga;
		}
		out.d4c = local.d4.c;
		out.live_offsets = sel.notest ? 1 : 4;

		GSVertexSW scan = GSVertexSW::zero();
		scan.c = GSVector4(span.r0, span.g0, span.b0, span.a0) * kColorScale;

		draw(pixels, left, 0, scan, local);

		for (int x = 0; x < 4; x++)
			out.px[x] = vm32[PixelAddr(x, 0)];
	}

	// Compiled once per selector variant: the generators are deterministic, and a
	// pair per test would outrun the code buffer.
	static bool RunJit(bool notest, const Span& span, int left, int pixels, Reading& out)
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

		Run(s_sel[slot], s_setup[slot], s_draw[slot], span, left, pixels, out);
		return true;
	}

	static void RunCpp(bool notest, const Span& span, int left, int pixels, Reading& out)
	{
		Run(MakeSelector(notest), &isa_native::GSDrawScanline::CSetupPrim,
			static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline),
			span, left, pixels, out);
	}

	// Compares the two, naming the lane and the field, so a failure says where the
	// paths parted rather than only that they did.
	static void ExpectSame(const Reading& c, const Reading& jit, const char* what)
	{
		for (int i = 0; i < c.live_offsets; i++)
		{
			for (int lane = 0; lane < 4; lane++)
			{
				EXPECT_EQ(c.d_rb[i].U32[lane], jit.d_rb[i].U32[lane])
					<< what << ": setup offset d[" << i << "].rb lane " << lane;
				EXPECT_EQ(c.d_ga[i].U32[lane], jit.d_ga[i].U32[lane])
					<< what << ": setup offset d[" << i << "].ga lane " << lane;
			}
		}

		for (int lane = 0; lane < 4; lane++)
			EXPECT_EQ(c.d4c.U32[lane], jit.d4c.U32[lane]) << what << ": step d4.c lane " << lane;

		for (int x = 0; x < 4; x++)
			EXPECT_EQ(c.px[x], jit.px[x]) << what << ": pixel " << x;
	}

	static u32 Pixel(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

	static GSScanlineSelector s_sel[2];
	static SetupPrimPtr s_setup[2];
	static DrawScanlinePtr s_draw[2];

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

GSScanlineSelector SwScanlineCPathTest::s_sel[2] = {};
SetupPrimPtr SwScanlineCPathTest::s_setup[2] = {nullptr, nullptr};
DrawScanlinePtr SwScanlineCPathTest::s_draw[2] = {nullptr, nullptr};
u8* SwScanlineCPathTest::s_code = nullptr;
size_t SwScanlineCPathTest::s_code_used = 0;
GSLocalMemory* SwScanlineCPathTest::s_mem = nullptr;

// The defect. Red and alpha descend, so their per-lane offsets are negative and the
// signed pack flattened all three of them to the same saturated value.
TEST_F(SwScanlineCPathTest, ADescendingGouraudSpanMatches)
{
	const Span span = {200.0f, 10.0f, 100.0f, 250.0f, -8.0f, 10.0f, 0.0f, -5.0f};

	Reading jit, cpp;
	ASSERT_TRUE(RunJit(true, span, 0, 4, jit));
	RunCpp(true, span, 0, 4, cpp);

	ExpectSame(cpp, jit, "descending");
}

// And the answer both paths give is the ramp, not merely the same thing twice. The
// colours are whole bytes on the scanline's own seven-bit grid, so this is exact.
TEST_F(SwScanlineCPathTest, ADescendingGouraudSpanIsTheRamp)
{
	const Span span = {200.0f, 10.0f, 100.0f, 250.0f, -8.0f, 10.0f, 0.0f, -5.0f};

	Reading cpp;
	RunCpp(true, span, 0, 4, cpp);

	EXPECT_EQ(cpp.px[0], Pixel(200, 10, 100, 250));
	EXPECT_EQ(cpp.px[1], Pixel(192, 20, 100, 245));
	EXPECT_EQ(cpp.px[2], Pixel(184, 30, 100, 240));
	EXPECT_EQ(cpp.px[3], Pixel(176, 40, 100, 235));
}

// The control: with every channel climbing and the span starting on a group
// boundary, no lane offset is ever negative, so nothing reaches the pack's
// saturating range. This agreed before the fix and has to keep agreeing -- it is
// what says the fix did not simply move the disagreement somewhere else.
TEST_F(SwScanlineCPathTest, AnAscendingGouraudSpanMatches)
{
	const Span span = {10.0f, 40.0f, 0.0f, 5.0f, 9.0f, 10.0f, 21.0f, 17.0f};

	Reading jit, cpp;
	ASSERT_TRUE(RunJit(true, span, 0, 4, jit));
	RunCpp(true, span, 0, 4, cpp);

	ExpectSame(cpp, jit, "ascending");
}

// A flat span is the other control: no gradient, no offsets, nothing to pack.
TEST_F(SwScanlineCPathTest, AFlatGouraudSpanMatches)
{
	const Span span = {60.0f, 70.0f, 80.0f, 90.0f, 0.0f, 0.0f, 0.0f, 0.0f};

	Reading jit, cpp;
	ASSERT_TRUE(RunJit(true, span, 0, 4, jit));
	RunCpp(true, span, 0, 4, cpp);

	ExpectSame(cpp, jit, "flat");
}

// Every channel decides on its own gradient, so a span that descends in two of them
// and climbs in the other two has to match lane for lane.
TEST_F(SwScanlineCPathTest, EachChannelIsPackedOnItsOwnGradient)
{
	const Span span = {250.0f, 0.0f, 250.0f, 0.0f, -30.0f, 30.0f, -1.0f, 1.0f};

	Reading jit, cpp;
	ASSERT_TRUE(RunJit(true, span, 0, 4, jit));
	RunCpp(true, span, 0, 4, cpp);

	ExpectSame(cpp, jit, "mixed");
}

// The four offset vectors differ only in which lane sits at zero, and a span that
// does not start on a group boundary selects a different one. This walks all four,
// which is also the only way the negative entries of the shift table are reached.
TEST_F(SwScanlineCPathTest, EveryStartingLaneMatches)
{
	const Span span = {240.0f, 12.0f, 200.0f, 255.0f, -11.0f, 7.0f, -3.0f, -13.0f};

	for (int left = 0; left < 4; left++)
	{
		Reading jit, cpp;
		ASSERT_TRUE(RunJit(false, span, left, 4 - left, jit));
		RunCpp(false, span, left, 4 - left, cpp);

		ExpectSame(cpp, jit, "starting lane");
	}
}

// A sweep, so the pin is the rule and not the handful of gradients above. Steep
// gradients are included deliberately: they are what pushes an offset furthest past
// the point the signed pack saturates at.
TEST_F(SwScanlineCPathTest, TheTwoPathsAgreeAcrossAGradientSweep)
{
	static const float kSteps[] = {-85.0f, -60.0f, -17.0f, -1.0f, 0.0f, 1.0f, 17.0f, 60.0f, 85.0f};

	for (float dr : kSteps)
	{
		for (float da : kSteps)
		{
			const Span span = {128.0f, 128.0f, 128.0f, 128.0f, dr, -dr, da, -da};

			Reading jit, cpp;
			ASSERT_TRUE(RunJit(true, span, 0, 4, jit));
			RunCpp(true, span, 0, 4, cpp);

			ExpectSame(cpp, jit, "sweep");
		}
	}
}

} // namespace

#endif // ARCH_ARM64
