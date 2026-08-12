// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the SW scanline's dither unit.
//
// The gs-dither hardware capture (SCPH-30001, 2026-08-11) is the first thing in
// this tree to measure the dither unit at all -- every claim we shipped about it
// before came from reading our own code. What it measured: the matrix entry is
// DM[y & 3][x & 3], added to the eight-bit colour, the same entry on red, green
// and blue, with alpha taking nothing, applied before the colour clamp and after
// the blend. It found two defects, one pinned by each suite below.
//
// 1. A flat sprite was never dithered. The rasterizer routes a flat, untextured,
//    unblended sprite to a bulk rectangle fill selected by IsSolidRect(), and the
//    fill writes one constant colour while dither is added per pixel in the
//    scanline the fill bypasses. The predicate tested primitive class, shading,
//    texture, blend, depth test, alpha test, DATE and fog -- not DTHE. On silicon
//    the same grid drawn as sprites and as triangles is identical to the pixel;
//    ours differed on 1116 of 4096.
//
// 2. Dither was gated on a 16-bit destination. Silicon dithers 32-bit and 24-bit
//    destinations too, same matrix, same indexing: 4080 of 4096 pixels move on
//    each, and we scored 50% on both by getting right only the pixels whose
//    matrix entry happens to be zero.
//
// Rides ARCH_ARM64 like its sibling gs_sw_scanline_date_tests.cpp: both compile
// the JIT scanline directly, and the generator is per-architecture.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
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
// ---------------------------------------------------------------------------
// The fast-path predicate
// ---------------------------------------------------------------------------

// The selector GSRendererSW::GetScanlineGlobalData derives for the draw the bulk
// fill exists to serve: a flat, untextured, unblended sprite with no depth test,
// no alpha test, no DATE and no fog.
GSScanlineSelector MakeSolidRectSelector()
{
	GSScanlineSelector sel;
	sel.key = 0;
	sel.prim = GS_SPRITE_CLASS;
	sel.iip = 0;
	sel.tfx = TFX_NONE;
	sel.abe = 0;
	sel.ztst = 0;
	sel.atst = ATST_ALWAYS;
	sel.date = 0;
	sel.fge = 0;
	sel.dthe = 0;
	sel.fwrite = 1;
	sel.colclamp = 1;
	return sel;
}

TEST(SwScanlineSolidRect, TheBaseCaseTakesTheFastPath)
{
	EXPECT_TRUE(MakeSolidRectSelector().IsSolidRect());
}

// The defect: a dithered draw took the fill, which cannot dither.
TEST(SwScanlineSolidRect, DitherDefeatsTheFastPath)
{
	GSScanlineSelector sel = MakeSolidRectSelector();
	sel.dthe = 1;
	EXPECT_FALSE(sel.IsSolidRect());
}

// Every other condition the fill cannot reproduce, pinned alongside it so the
// predicate's contract is stated in one place rather than inferred from the
// expression. Each is flipped on its own from the base case.
TEST(SwScanlineSolidRect, EachUnreproducibleConditionDefeatsTheFastPath)
{
	struct Case
	{
		const char* what;
		void (*apply)(GSScanlineSelector&);
	};

	static const Case cases[] = {
		{"non-sprite primitive", [](GSScanlineSelector& s) { s.prim = GS_TRIANGLE_CLASS; }},
		{"gouraud shading", [](GSScanlineSelector& s) { s.iip = 1; }},
		{"textured", [](GSScanlineSelector& s) { s.tfx = TFX_MODULATE; }},
		{"blending", [](GSScanlineSelector& s) { s.abe = 1; }},
		{"depth test", [](GSScanlineSelector& s) { s.ztst = 2; }},
		{"alpha test", [](GSScanlineSelector& s) { s.atst = ATST_GEQUAL; }},
		{"destination alpha test", [](GSScanlineSelector& s) { s.date = 1; }},
		{"fog", [](GSScanlineSelector& s) { s.fge = 1; }},
		{"dither", [](GSScanlineSelector& s) { s.dthe = 1; }},
	};

	for (const Case& c : cases)
	{
		GSScanlineSelector sel = MakeSolidRectSelector();
		c.apply(sel);
		EXPECT_FALSE(sel.IsSolidRect()) << c.what;
	}
}

// ---------------------------------------------------------------------------
// The dither unit itself
// ---------------------------------------------------------------------------

using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);

// Mid-range so that adding any entry of a signed three-bit matrix (-4..3) stays
// inside 0..255 and no channel saturates. Saturation is deliberately out of
// scope: the capture pinned the add as happening before the clamp, the code's
// position relative to the clamp is unchanged here, and the differential method
// below needs the shifted reference colour to be representable.
constexpr int kBaseR = 0x40, kBaseG = 0x80, kBaseB = 0xC0, kBaseA = 0x55;

// Asymmetric on purpose: DM[0][1] is -3 where DM[1][0] is 0, so an implementation
// indexing DM[x & 3][y & 3] fails here. That transposition is exactly what the
// capture excluded on console with its one-hot pair.
constexpr int kMatrix[4][4] = {
	{-4, -3, -2, -1},
	{ 0,  1,  2,  3},
	{ 3,  2,  1,  0},
	{-1, -2, -3, -4},
};

class SwScanlineDitherTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 16 * kCodeSlotSize;

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

		// Also populates the static m_psm swizzle tables on first construction.
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

	static DrawScanlinePtr Compile(GSScanlineSelector sel)
	{
		if (!s_code || s_code_used + kCodeSlotSize > kCodeBufferSize)
			return nullptr;

		u8* slot = s_code + s_code_used;
		s_code_used += kCodeSlotSize;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSDrawScanlineCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<DrawScanlinePtr>(const_cast<u8*>(cg.GetCode()));
	}

	// Mirrors GSRendererSW's derivation for a flat untextured unblended sprite
	// with no tests -- precisely the draw class the bulk fill used to swallow.
	// notest holds because the sprite is 4-aligned and nothing tests.
	static GSScanlineSelector MakeDitherSelector(u32 fpsm, bool dthe)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = fpsm;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_NONE;
		sel.ababcd = 0xff;
		sel.prim = GS_SPRITE_CLASS;
		sel.fwrite = 1;
		sel.notest = 1;
		sel.colclamp = 1;
		sel.dthe = dthe ? 1 : 0;
		return sel;
	}

	// FBP 0, FBW 1 (64 pixels). The depth side is unused but the offset table
	// needs a well-formed ZBUF.
	static const GSPixelOffset4* GetOffsets(u32 frame_psm)
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 1;
		frame.PSM = frame_psm;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 256;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(u32 psm, int x, int y)
	{
		return GSLocalMemory::m_psm[psm].info.pa(x, y, 0, 1);
	}

	// kMatrix as a DIMX register. Going through the real register and the real
	// expansion means this pins the whole path from a game's DIMX write to the
	// stored pixel, rather than the scanline's half of it.
	static GIFRegDIMX MakeDimxRegister()
	{
		GIFRegDIMX dimx;
		dimx.U64 = 0;
		dimx.DM00 = kMatrix[0][0]; dimx.DM01 = kMatrix[0][1]; dimx.DM02 = kMatrix[0][2]; dimx.DM03 = kMatrix[0][3];
		dimx.DM10 = kMatrix[1][0]; dimx.DM11 = kMatrix[1][1]; dimx.DM12 = kMatrix[1][2]; dimx.DM13 = kMatrix[1][3];
		dimx.DM20 = kMatrix[2][0]; dimx.DM21 = kMatrix[2][1]; dimx.DM22 = kMatrix[2][2]; dimx.DM23 = kMatrix[2][3];
		dimx.DM30 = kMatrix[3][0]; dimx.DM31 = kMatrix[3][1]; dimx.DM32 = kMatrix[3][2]; dimx.DM33 = kMatrix[3][3];
		return dimx;
	}

	// Runs one 4-pixel scanline at (0..3, top) with a flat colour, over a frame
	// seeded to a known value first, so a format that preserves bits it does not
	// write (24-bit keeps its top byte) compares like for like across runs.
	static void RunRow(DrawScanlinePtr fn, GSScanlineSelector sel, const GSPixelOffset4* off,
		u32 fm_value, u32 frame_psm, int top, int r, int g, int b, int a, GSVector4i* dimx)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(frame_psm, x, top)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = off->row;
		global.fzbc = off->col;
		global.fm = GSVector4i(static_cast<int>(fm_value));
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.dimx = dimx;

		local.gd = &global;
		// Flat colour as CSetupPrim leaves it for TFX_NONE: r|b and g|a pairs.
		local.c.rb = GSVector4i(r | (b << 16));
		local.c.ga = GSVector4i(g | (a << 16));

		const GSVertexSW scan = GSVertexSW::zero();
		fn(4, 0, top, scan, local);
	}

	// The whole point of this suite, and the reason it is written differentially.
	//
	// For every phase, drawing colour C with dither on must store exactly what
	// drawing colour C + DM[y & 3][x & 3] stores with dither off. Stated that
	// way, the test pins the dither semantics -- which entry, on which channels,
	// at which pixel -- without ever computing a format conversion, a swizzled
	// address or a clamp itself. Everything it would otherwise have had to
	// reimplement is supplied by the renderer on both sides of the comparison,
	// so a bug in any of it cannot make this pass.
	static void CheckDitherMatchesShiftedColour(u32 frame_psm, u32 fpsm, u32 fm_value)
	{
		const GSPixelOffset4* off = GetOffsets(frame_psm);
		u32* vm32 = s_mem->vm32();

		alignas(32) GSVector4i dimx[8];
		GSState::ExpandDIMX(dimx, MakeDimxRegister());

		const GSScanlineSelector dithered_sel = MakeDitherSelector(fpsm, true);
		const GSScanlineSelector plain_sel = MakeDitherSelector(fpsm, false);

		DrawScanlinePtr dithered_fn = Compile(dithered_sel);
		DrawScanlinePtr plain_fn = Compile(plain_sel);
		ASSERT_NE(dithered_fn, nullptr);
		ASSERT_NE(plain_fn, nullptr);

		// Dithered pass: four rows of the flat base colour.
		u32 dithered[4][4];
		for (int y = 0; y < 4; y++)
		{
			RunRow(dithered_fn, dithered_sel, off, fm_value, frame_psm, y,
				kBaseR, kBaseG, kBaseB, kBaseA, dimx);
			for (int x = 0; x < 4; x++)
				dithered[y][x] = vm32[PixelAddr(frame_psm, x, y)];
		}

		// Reference pass: dither off, colour pre-shifted by the entry that pixel
		// should have received.
		for (int y = 0; y < 4; y++)
		{
			for (int x = 0; x < 4; x++)
			{
				const int d = kMatrix[y][x];
				RunRow(plain_fn, plain_sel, off, fm_value, frame_psm, y,
					kBaseR + d, kBaseG + d, kBaseB + d, kBaseA, nullptr);

				EXPECT_EQ(dithered[y][x], vm32[PixelAddr(frame_psm, x, y)])
					<< "x=" << x << " y=" << y << " entry=" << d;
			}
		}
	}

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

u8* SwScanlineDitherTest::s_code = nullptr;
size_t SwScanlineDitherTest::s_code_used = 0;
GSLocalMemory* SwScanlineDitherTest::s_mem = nullptr;

// Console-measured: 4080 of 4096 pixels move on a 32-bit destination. We applied
// no matrix at all there and scored 50%, right only where the entry is zero.
TEST_F(SwScanlineDitherTest, Dithers32BitDestinations)
{
	CheckDitherMatchesShiftedColour(PSMCT32, 0, 0);
}

// Same on 24-bit, where the frame mask preserves the byte the format does not
// own -- so this also pins that dither does not leak into it.
TEST_F(SwScanlineDitherTest, Dithers24BitDestinations)
{
	CheckDitherMatchesShiftedColour(PSMCT24, 1, 0xff000000);
}

// The one destination we already dithered. It stays right.
TEST_F(SwScanlineDitherTest, Dithers16BitDestinations)
{
	CheckDitherMatchesShiftedColour(PSMCT16, 2, 0xffff0000);
}

// Alpha takes nothing, on the only colour format wide enough to carry a byte of
// it. Measured at eight-bit precision on console, where "alpha takes the
// colour's entry" scored 16 of 4096 against 4096 of 4096 for "alpha takes
// nothing". The differential test above would catch this too; stating it
// directly means a failure names the cause.
TEST_F(SwScanlineDitherTest, LeavesAlphaAlone)
{
	const GSPixelOffset4* off = GetOffsets(PSMCT32);
	u32* vm32 = s_mem->vm32();

	alignas(32) GSVector4i dimx[8];
	GSState::ExpandDIMX(dimx, MakeDimxRegister());

	const GSScanlineSelector sel = MakeDitherSelector(0, true);
	DrawScanlinePtr fn = Compile(sel);
	ASSERT_NE(fn, nullptr);

	for (int y = 0; y < 4; y++)
	{
		RunRow(fn, sel, off, 0, PSMCT32, y, kBaseR, kBaseG, kBaseB, kBaseA, dimx);
		for (int x = 0; x < 4; x++)
		{
			const u32 stored = vm32[PixelAddr(PSMCT32, x, y)];
			EXPECT_EQ(stored >> 24, static_cast<u32>(kBaseA))
				<< "x=" << x << " y=" << y << " entry=" << kMatrix[y][x];
		}
	}
}

} // namespace

#endif // ARCH_ARM64
