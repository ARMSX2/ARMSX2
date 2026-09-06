// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for where a triangle's texture coordinate DDA lands.
//
// The gs-shade hardware capture (SCPH-30001, 2026-08-13) read the sampled
// coordinate back directly, through a palette whose every entry names its own
// texel, and found ours one sixteenth of a texel HIGH and never low -- but only
// where the exact coordinate lands on a sixteenth, and only where the walk is
// going forward:
//
//   gradient of four sixteenths per pixel, forward   3,840 of 3,840 readings high
//   the same gradient, descending                    0 of 2,048 -- exact
//   a gradient whose per-pixel step does not
//     terminate in binary                            12 of 3,072
//   a still coordinate                               exact, in every section
//
// Read together those say the hardware's coordinate trails the exact plane in the
// direction the walk is going, by less than a sixteenth. Trailing a forward walk
// puts it below a boundary, so it floors one sixteenth lower than we do; trailing
// a backward walk puts it above, where it floors where we already do; and a still
// coordinate does not trail at all, which is what makes the lag the step's rather
// than the seed's. Sprites are exact on silicon (gs-interp, 3,072 of 3,072,
// negative gradients included) and take nothing.
//
// Where the lag comes from inside the hardware's walk is unfitted -- it is one
// unit of our own 16.16 coordinate, the smallest bias that reproduces every
// reading.
//
// The suite drives the real GSSetupPrim and GSDrawScanline generators, so it pins
// the two halves together: setup deciding which axes walk forward, and the
// scanline spending that on the coordinate. It reads the sampled texel directly
// by drawing DECAL out of a texture whose every texel names its own address.
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

// One texel, in the 16.16 the coordinate is carried in.
constexpr int kTexel = 0x10000;

// The rule this suite exists to pin, written once. A coordinate is sampled where
// the exact plane puts it, less one unit on each axis whose walk goes forward --
// which can only change the texel when the exact coordinate lands on a boundary.
int SampledAddress(int u0, int du, int v0, int dv, int lane, bool sprite)
{
	int u = u0 + lane * du;
	int v = v0 + lane * dv;

	if (!sprite)
	{
		if (du > 0)
			u -= 1;
		if (dv > 0)
			v -= 1;
	}

	// 16x16, repeating on both axes; the texture names its own address.
	return (((v >> 16) & 15) * 16) + ((u >> 16) & 15);
}

class SwScanlineTcLagTest : public ::testing::Test
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

	// The setup generator takes a reduced key, exactly as GetScanlineGlobalData
	// builds it -- so this compiles the same variant a real draw would.
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

	// DECAL out of a nearest-filtered 16x16 texture into PSMCT32: the stored pixel
	// is the texel, and the texel is its own address, so every pixel reports the
	// coordinate the scanline sampled at.
	static GSScanlineSelector MakeSelector(u32 prim)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		sel.fst = 1;
		sel.ltf = 0;
		sel.tlu = 0;
		sel.tw = 1; // 1 << (tw + 3) == 16 texels wide
		sel.ababcd = 0xff;
		sel.prim = prim;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 1;
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

	// Draws four pixels of one primitive whose coordinate starts at (u0, v0) and
	// steps by (du, dv) per pixel, all in 16.16, and returns the texture address
	// each pixel sampled.
	static void RunWalk(GSScanlineSelector sel, SetupPrimPtr setup, DrawScanlinePtr draw,
		int u0, int du, int v0, int dv, int out[4])
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};
		alignas(32) u32 tex[256];
		for (int i = 0; i < 256; i++)
			tex[i] = static_cast<u32>(i) | (static_cast<u32>(i) << 8) | (static_cast<u32>(i) << 16) | (static_cast<u32>(i) << 24);

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = tex;

		// REPEAT on both axes over sixteen texels, as GSRendererSW derives it.
		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		for (int i = 0; i < 8; i++)
			global.t.min.U16[i] = 15;

		local.gd = &global;

		// Only dscan.t matters here: no depth, no fog, and DECAL with TCC on takes
		// no vertex colour, so setup runs its texture half alone.
		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		dscan.t = GSVector4(static_cast<float>(du), static_cast<float>(dv), 0.0f, 0.0f);

		setup(vertex, index, dscan, local);

		GSVertexSW scan = GSVertexSW::zero();
		scan.t = GSVector4(static_cast<float>(u0), static_cast<float>(v0), 0.0f, 0.0f);

		draw(4, 0, 0, scan, local);

		for (int x = 0; x < 4; x++)
			out[x] = static_cast<int>(vm32[PixelAddr(x, 0)] & 0xff);
	}

	// Compiled once per primitive class: the generators are deterministic, and a
	// pair per test would outrun the code buffer.
	static bool Walk(u32 prim, int u0, int du, int v0, int dv, int out[4])
	{
		const bool sprite = (prim == GS_SPRITE_CLASS);
		if (!s_setup[sprite])
		{
			const GSScanlineSelector sel = MakeSelector(prim);
			s_sel[sprite] = sel;
			s_setup[sprite] = CompileSetup(sel);
			s_draw[sprite] = CompileScanline(sel);
		}

		if (!s_setup[sprite] || !s_draw[sprite])
			return false;

		RunWalk(s_sel[sprite], s_setup[sprite], s_draw[sprite], u0, du, v0, dv, out);
		return true;
	}

	static GSScanlineSelector s_sel[2];
	static SetupPrimPtr s_setup[2];
	static DrawScanlinePtr s_draw[2];

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

GSScanlineSelector SwScanlineTcLagTest::s_sel[2] = {};
SetupPrimPtr SwScanlineTcLagTest::s_setup[2] = {nullptr, nullptr};
DrawScanlinePtr SwScanlineTcLagTest::s_draw[2] = {nullptr, nullptr};
u8* SwScanlineTcLagTest::s_code = nullptr;
size_t SwScanlineTcLagTest::s_code_used = 0;
GSLocalMemory* SwScanlineTcLagTest::s_mem = nullptr;

// The defect, at its trigger: a forward walk whose exact coordinate lands on a
// texel boundary at every pixel. Before the fix these sampled texels 4,5,6,7; the
// console samples the one below each.
TEST_F(SwScanlineTcLagTest, AForwardWalkOnABoundarySamplesTheTexelBelow)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 4 * kTexel, kTexel, 0, 0, got));

	// Stated as the values, so a failure shows the walk rather than an arithmetic
	// identity: this is the reading the capture took.
	EXPECT_EQ(got[0], 3);
	EXPECT_EQ(got[1], 4);
	EXPECT_EQ(got[2], 5);
	EXPECT_EQ(got[3], 6);
}

// And nowhere else. The same gradient started half a texel off a boundary keeps
// every pixel where it was: the lag is smaller than the distance to the next
// sixteenth, so it can only move a coordinate that has landed exactly on one.
TEST_F(SwScanlineTcLagTest, AForwardWalkOffABoundaryDoesNotMove)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 4 * kTexel + kTexel / 2, kTexel, 0, 0, got));

	EXPECT_EQ(got[0], 4);
	EXPECT_EQ(got[1], 5);
	EXPECT_EQ(got[2], 6);
	EXPECT_EQ(got[3], 7);
}

// A descending walk trails the other way, which floors where we already do. The
// capture's descending section is identical to the console on all 2,048 readings,
// so this direction must NOT move.
TEST_F(SwScanlineTcLagTest, ADescendingWalkDoesNotMove)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 12 * kTexel, -kTexel, 0, 0, got));

	EXPECT_EQ(got[0], 12);
	EXPECT_EQ(got[1], 11);
	EXPECT_EQ(got[2], 10);
	EXPECT_EQ(got[3], 9);
}

// A still coordinate does not trail. This is the cell that makes the whole thing
// attributable: with neither interpolator moving, every arm was already exact
// against the console, and it has to stay that way.
TEST_F(SwScanlineTcLagTest, AStillCoordinateDoesNotMove)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 4 * kTexel, 0, 0, 0, got));

	for (int x = 0; x < 4; x++)
		EXPECT_EQ(got[x], 4) << "pixel " << x;
}

// Inside one primitive, a quarter-texel gradient crosses a boundary on one pixel
// in four -- so one pixel moves and three do not. The capture measured exactly
// this shape, at four sixteenths per pixel.
TEST_F(SwScanlineTcLagTest, OnlyThePixelsOnABoundaryMove)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 4 * kTexel, kTexel / 4, 0, 0, got));

	EXPECT_EQ(got[0], 3); // exactly on texel 4, so it takes the one below
	EXPECT_EQ(got[1], 4);
	EXPECT_EQ(got[2], 4);
	EXPECT_EQ(got[3], 4);
}

// The V axis carries the same rule and is decided separately: here U is still and
// V walks forward, so only V moves.
TEST_F(SwScanlineTcLagTest, EachAxisIsDecidedOnItsOwnStep)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 4 * kTexel, 0, 4 * kTexel, kTexel, got));

	// U stays at texel 4 while V takes the row below each exact one.
	EXPECT_EQ(got[0], 3 * 16 + 4);
	EXPECT_EQ(got[1], 4 * 16 + 4);
	EXPECT_EQ(got[2], 5 * 16 + 4);
	EXPECT_EQ(got[3], 6 * 16 + 4);
}

// Sprites take nothing. Silicon's sprite coordinate is exact and so is ours, and
// sprites are how every two-dimensional game on the machine draws -- a lag here
// would shift blits by a whole texel.
TEST_F(SwScanlineTcLagTest, ASpriteTakesNoLag)
{
	int got[4];
	ASSERT_TRUE(Walk(GS_SPRITE_CLASS, 4 * kTexel, kTexel, 0, 0, got));

	EXPECT_EQ(got[0], 4);
	EXPECT_EQ(got[1], 5);
	EXPECT_EQ(got[2], 6);
	EXPECT_EQ(got[3], 7);
}

// The rule over a spread of walks, so the cases above are a sample of it rather
// than the whole of it: both directions on both axes, sub-texel and multi-texel
// steps, and a step that does not terminate in binary.
TEST_F(SwScanlineTcLagTest, TheRuleHoldsAcrossAWalkSweep)
{
	static const int kStarts[] = {0, 4 * kTexel, 4 * kTexel + kTexel / 2, 15 * kTexel, 4 * kTexel + 7};
	static const int kSteps[] = {0, 1, -1, kTexel / 16, -kTexel / 16, kTexel / 4, kTexel, 2 * kTexel,
		-2 * kTexel, 11117};

	for (u32 prim : {static_cast<u32>(GS_TRIANGLE_CLASS), static_cast<u32>(GS_SPRITE_CLASS)})
	{
		const bool sprite = (prim == GS_SPRITE_CLASS);

		for (int u0 : kStarts)
		{
			for (int du : kSteps)
			{
				int got[4];
				// V still: a sprite's V does not step in the scanline at all, so
				// sweeping it would compare against a rule that path never applies.
				ASSERT_TRUE(Walk(prim, u0, du, 3 * kTexel, 0, got));

				for (int x = 0; x < 4; x++)
				{
					EXPECT_EQ(got[x], SampledAddress(u0, du, 3 * kTexel, 0, x, sprite))
						<< (sprite ? "sprite" : "triangle") << " u0=" << u0
						<< " du=" << du << " pixel " << x;
				}
			}
		}
	}

	// And the V axis, on the primitive class whose V walks.
	for (int v0 : kStarts)
	{
		for (int dv : kSteps)
		{
			int got[4];
			ASSERT_TRUE(Walk(GS_TRIANGLE_CLASS, 5 * kTexel, 0, v0, dv, got));

			for (int x = 0; x < 4; x++)
			{
				EXPECT_EQ(got[x], SampledAddress(5 * kTexel, 0, v0, dv, x, false))
					<< "triangle v0=" << v0 << " dv=" << dv << " pixel " << x;
			}
		}
	}
}

} // namespace

#endif // ARCH_ARM64
