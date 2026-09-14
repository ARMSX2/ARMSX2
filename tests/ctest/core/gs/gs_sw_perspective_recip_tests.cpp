// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the perspective divide's reciprocal.
//
// The GS does not divide S and T by Q. It multiplies by a reciprocal taken off a
// coarse grid, and the grid TRUNCATES: across 12,288 readings of the gs-interp
// capture (SCPH-30001, 2026-09-06) 1,592 force the hardware's reciprocal strictly
// below the true value and not one forces it above.
//
// The width of that grid is what this suite pins. gs-grad's case 34 (SCPH-30001,
// 2026-09-06) is the case that decides it: a 512-pixel row whose quotient S/Q is
// the constant 8193/16384, which is a texel boundary plus a sliver. The console
// reads the same 512 sixteenths on all 512 pixels. A reciprocal held to thirteen
// mantissa bits trails far enough at the end of each of its plateaus to drop 128
// of those pixels one sixteenth low; fourteen bits clears every one of them, and
// so does anything wider. Fourteen is therefore the narrowest grid the captures
// permit -- not a fitted value -- and nothing we hold separates it from wider.
//
// Both cases below are driven through the real setup and scanline generators with
// a still coordinate, so the texture-coordinate lag (gs_sw_scanline_tclag_tests)
// does not apply and what is left is the reciprocal alone.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

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

class SwPerspectiveRecipTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 4 * kCodeSlotSize;

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

	// DECAL out of a nearest-filtered 16x16 texture whose every texel names its
	// own address, so the stored pixel reports the texel the scanline sampled.
	// fst is 0: this is the perspective path, which is the point.
	static GSScanlineSelector MakeSelector()
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		sel.fst = 0;
		sel.ltf = 0;
		sel.tlu = 0;
		sel.tw = 1; // 1 << (tw + 3) == 16 texels wide
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 1;
		sel.colclamp = 1;
		return sel;
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

	// Four pixels of one primitive holding S, T and Q still, so every pixel asks
	// the same question of the reciprocal and none of them takes the walk's lag.
	// S is in the 16.16 the sampled coordinate is carried in; the answer read back
	// is the texel address, U wrapped to sixteen.
	static bool Sample(float s, float q, int out[4])
	{
		if (!s_setup)
		{
			s_sel = MakeSelector();
			s_setup = CompileSetup(s_sel);
			s_draw = CompileScanline(s_sel);
		}
		if (!s_setup || !s_draw)
			return false;

		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};
		alignas(32) u32 tex[256];
		for (int i = 0; i < 256; i++)
			tex[i] = static_cast<u32>(i) | (static_cast<u32>(i) << 8) | (static_cast<u32>(i) << 16) | (static_cast<u32>(i) << 24);

		global.sel = s_sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = tex;

		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		for (int i = 0; i < 8; i++)
			global.t.min.U16[i] = 15;

		local.gd = &global;

		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		// A still plane: no gradient on any of S, T or Q.
		GSVertexSW dscan = GSVertexSW::zero();
		s_setup(vertex, index, dscan, local);

		GSVertexSW scan = GSVertexSW::zero();
		scan.t = GSVector4(s, 0.0f, q, 0.0f);

		s_draw(4, 0, 0, scan, local);

		for (int x = 0; x < 4; x++)
			out[x] = static_cast<int>(vm32[PixelAddr(x, 0)] & 0xff);
		return true;
	}

	static GSScanlineSelector s_sel;
	static SetupPrimPtr s_setup;
	static DrawScanlinePtr s_draw;

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

GSScanlineSelector SwPerspectiveRecipTest::s_sel = {};
SetupPrimPtr SwPerspectiveRecipTest::s_setup = nullptr;
DrawScanlinePtr SwPerspectiveRecipTest::s_draw = nullptr;
u8* SwPerspectiveRecipTest::s_code = nullptr;
size_t SwPerspectiveRecipTest::s_code_used = 0;
GSLocalMemory* SwPerspectiveRecipTest::s_mem = nullptr;

// gs-grad case 34, reduced to the one decision it turns on. Q is one part in
// 16,384 below one, so its true reciprocal is exactly 1 + 2^-14: the first bit a
// thirteen-bit grid throws away and a fourteen-bit grid keeps. S is placed so the
// exact quotient sits just inside texel 32.
//
// With thirteen bits the reciprocal collapses to 1.0, the product never reaches
// the boundary, and the sampled texel is 31 -- the 128 low readings the console
// contradicts. With fourteen it reads 32, on every pixel, as the console does.
TEST_F(SwPerspectiveRecipTest, TheGridKeepsFourteenMantissaBits)
{
	int got[4];
	ASSERT_TRUE(Sample(2097040.0f, 0.99993896484375f, got));

	for (int x = 0; x < 4; x++)
		EXPECT_EQ(got[x], 32 & 15) << "pixel " << x;
}

// And it is still a grid, not a divide. Here the true reciprocal has bits below
// the fourteenth, so truncating leaves the product one unit short of texel 32 and
// the hardware samples 31. An exact quotient would read 32 and be wrong -- this
// is the cell that keeps the truncation from being optimised away.
TEST_F(SwPerspectiveRecipTest, TheGridStillTruncates)
{
	int got[4];
	ASSERT_TRUE(Sample(1846503.0f, 0.8804812431335449f, got));

	for (int x = 0; x < 4; x++)
		EXPECT_EQ(got[x], 31 & 15) << "pixel " << x;
}
} // namespace

#endif // ARCH_ARM64
