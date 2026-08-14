// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for what the texture function multiplies.
//
// The gs-shade hardware capture (SCPH-30001, 2026-08-13) put a moving vertex
// colour and a moving palettised texel in series for the first time and asked
// which colour the product is formed from. It answered without a model of either
// interpolator: the same colour read back through four different multipliers
// brackets the value the hardware holds, and that bracket excludes the product of
// the eight-bit STORED colour on 0 of 24,576 readings. Ours excluded it on 17.99%
// -- because the scanline carries the colour as fixed point with seven fractional
// bits and fed all fifteen to the multiply.
//
// So: the texture function multiplies the byte, and the fraction the DDA carries
// between pixels is not part of the product. A gouraud gradient has a fraction at
// almost every pixel, which is why a flat-shaded corpus cannot see this and why
// the capture had to.
//
// The suite is written differentially first -- a fractional colour must store
// exactly what its truncation stores -- so it pins the rule without recomputing a
// format conversion or a clamp. The absolute test that follows states the product
// itself, so a failure names the cause rather than only the symptom.
//
// Rides ARCH_ARM64 like its siblings gs_sw_scanline_date_tests.cpp and
// gs_sw_scanline_dither_tests.cpp: all three compile the JIT scanline directly,
// and the generator is per-architecture.

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
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);

// What the console does, per channel: the stored eight-bit colour times the
// texel, shifted by seven, saturating.
u32 ConsoleModulate(u32 colour, u32 texel)
{
	const u32 p = (colour * texel) >> 7;
	return p > 255 ? 255 : p;
}

class SwScanlineTexFuncTest : public ::testing::Test
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

	// A flat, nearest-filtered, unblended textured sprite into PSMCT32. Every
	// axis is pinned so that only the texture function varies: the coordinate is
	// clamped to a single texel below, so filtering, wrapping and addressing
	// cannot contribute to what is stored.
	static GSScanlineSelector MakeSelector(u32 tfx, bool tcc)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = tfx;
		sel.tcc = tcc ? 1 : 0;
		sel.fst = 1;
		sel.ltf = 0;
		sel.tlu = 0;
		sel.tw = 0;
		sel.ababcd = 0xff;
		sel.prim = GS_SPRITE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 1;
		sel.colclamp = 1;
		return sel;
	}

	// FBP 0, FBW 1. The depth side is unused but the offset table needs a
	// well-formed ZBUF.
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

	// One four-pixel row with the flat colour supplied on the scanline's own
	// grid: rb and ga each hold two channels in 16-bit lanes, scaled by 128, so
	// `frac` is the sub-unit part the DDA would be carrying mid-gradient.
	//
	// Returns what landed in the first pixel; all four are the same draw.
	static u32 RunRow(DrawScanlinePtr fn, GSScanlineSelector sel, const GSPixelOffset4* off,
		u32 texel, u32 r, u32 g, u32 b, u32 a, u32 frac)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};
		alignas(32) u32 tex[4] = {texel, texel, texel, texel};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = off->row;
		global.fzbc = off->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = tex;
		// min = max = 0 with an all-zero mask is CLAMP to texel zero on both
		// axes, so every lane fetches tex[0] whatever the coordinate is.
		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.mask = GSVector4i::zero();

		local.gd = &global;
		local.c.rb = GSVector4i(static_cast<int>(((r << 7) | frac) | (((b << 7) | frac) << 16)));
		local.c.ga = GSVector4i(static_cast<int>(((g << 7) | frac) | (((a << 7) | frac) << 16)));

		const GSVertexSW scan = GSVertexSW::zero();
		fn(4, 0, 0, scan, local);

		return vm32[PixelAddr(0, 0)];
	}

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

u8* SwScanlineTexFuncTest::s_code = nullptr;
size_t SwScanlineTexFuncTest::s_code_used = 0;
GSLocalMemory* SwScanlineTexFuncTest::s_mem = nullptr;

// A spread of colours and texels, including both ends and values whose product
// straddles the shift. Kept small because every entry is crossed with every
// fraction below.
constexpr u32 kLevels[] = {0, 1, 0x1f, 0x40, 0x7f, 0x80, 0xc3, 0xfe, 0xff};

// The defect, stated as the console states it: what the multiply sees is the
// byte. Sweeping the fraction across its whole range must change nothing.
//
// This is the test that was red before the fix -- every non-zero fraction that
// carried the product over an integer boundary stored a different pixel.
TEST_F(SwScanlineTexFuncTest, ModulateIgnoresTheColourFraction)
{
	const GSScanlineSelector sel = MakeSelector(TFX_MODULATE, true);
	DrawScanlinePtr fn = Compile(sel);
	ASSERT_NE(fn, nullptr);
	const GSPixelOffset4* off = GetOffsets();

	for (u32 texel : kLevels)
	{
		const u32 t = texel | (texel << 8) | (texel << 16) | (texel << 24);

		for (u32 c : kLevels)
		{
			const u32 reference = RunRow(fn, sel, off, t, c, c, c, c, 0);

			for (u32 frac : {1u, 2u, 63u, 64u, 65u, 126u, 127u})
			{
				EXPECT_EQ(reference, RunRow(fn, sel, off, t, c, c, c, c, frac))
					<< "texel=" << texel << " colour=" << c << " fraction=" << frac;
			}
		}
	}
}

// And what it stores is the console's product of that byte. Stated absolutely so
// a regression that truncates the colour but then rounds, or shifts by the wrong
// amount, cannot pass the differential test above on its own.
TEST_F(SwScanlineTexFuncTest, ModulateStoresTheProductOfTheStoredByte)
{
	const GSScanlineSelector sel = MakeSelector(TFX_MODULATE, true);
	DrawScanlinePtr fn = Compile(sel);
	ASSERT_NE(fn, nullptr);
	const GSPixelOffset4* off = GetOffsets();

	for (u32 texel : kLevels)
	{
		const u32 t = texel | (texel << 8) | (texel << 16) | (texel << 24);

		for (u32 c : kLevels)
		{
			for (u32 frac : {0u, 99u, 127u})
			{
				const u32 stored = RunRow(fn, sel, off, t, c, c, c, c, frac);
				const u32 want = ConsoleModulate(c, texel);

				EXPECT_EQ(stored & 0xff, want) << "R texel=" << texel << " colour=" << c;
				EXPECT_EQ((stored >> 8) & 0xff, want) << "G texel=" << texel << " colour=" << c;
				EXPECT_EQ((stored >> 16) & 0xff, want) << "B texel=" << texel << " colour=" << c;
				EXPECT_EQ((stored >> 24) & 0xff, want) << "A texel=" << texel << " colour=" << c;
			}
		}
	}
}

// With TCC off the alpha is the vertex's own byte and no product at all -- the
// capture measured that at 512/512, with the texture's alpha refuted on every
// separating reading. It shares the truncation with the colour path, so it
// belongs in the same sweep.
TEST_F(SwScanlineTexFuncTest, ModulateWithoutTCCStoresTheVertexAlphaByte)
{
	const GSScanlineSelector sel = MakeSelector(TFX_MODULATE, false);
	DrawScanlinePtr fn = Compile(sel);
	ASSERT_NE(fn, nullptr);
	const GSPixelOffset4* off = GetOffsets();

	for (u32 a : kLevels)
	{
		for (u32 frac : {0u, 64u, 127u})
		{
			const u32 stored = RunRow(fn, sel, off, 0x80808080u, 0x40, 0x40, 0x40, a, frac);
			EXPECT_EQ(stored >> 24, a) << "alpha=" << a << " fraction=" << frac;
		}
	}
}

// HIGHLIGHT and HIGHLIGHT2 modulate the same way and then add the vertex alpha
// into RGB, saturating. The capture scored both at 2,048/2,048 against that rule
// with a stepped colour -- which nothing had exercised before it -- so the same
// fraction sweep has to hold through them.
TEST_F(SwScanlineTexFuncTest, HighlightModesIgnoreTheColourFraction)
{
	const GSPixelOffset4* off = GetOffsets();

	for (u32 tfx : {static_cast<u32>(TFX_HIGHLIGHT), static_cast<u32>(TFX_HIGHLIGHT2)})
	{
		const GSScanlineSelector sel = MakeSelector(tfx, true);
		DrawScanlinePtr fn = Compile(sel);
		ASSERT_NE(fn, nullptr);

		for (u32 texel : kLevels)
		{
			const u32 t = texel | (texel << 8) | (texel << 16) | (texel << 24);

			for (u32 c : kLevels)
			{
				const u32 reference = RunRow(fn, sel, off, t, c, c, c, c, 0);

				for (u32 frac : {1u, 64u, 127u})
				{
					EXPECT_EQ(reference, RunRow(fn, sel, off, t, c, c, c, c, frac))
						<< "tfx=" << tfx << " texel=" << texel << " colour=" << c
						<< " fraction=" << frac;
				}
			}
		}
	}
}

// DECAL takes the texel and nothing else, which the capture confirmed at
// 1,024/1,024 by stepping the colour and watching nothing move. It is the control
// on the whole colour path: if a future change leaks the vertex colour into a
// product it should not form, this is where it shows.
TEST_F(SwScanlineTexFuncTest, DecalIgnoresTheVertexColourEntirely)
{
	const GSScanlineSelector sel = MakeSelector(TFX_DECAL, true);
	DrawScanlinePtr fn = Compile(sel);
	ASSERT_NE(fn, nullptr);
	const GSPixelOffset4* off = GetOffsets();

	for (u32 texel : kLevels)
	{
		const u32 t = texel | (texel << 8) | (texel << 16) | (texel << 24);
		const u32 reference = RunRow(fn, sel, off, t, 0, 0, 0, 0, 0);

		for (u32 c : kLevels)
		{
			for (u32 frac : {0u, 127u})
			{
				EXPECT_EQ(reference, RunRow(fn, sel, off, t, c, c, c, c, frac))
					<< "texel=" << texel << " colour=" << c << " fraction=" << frac;
			}
		}
		EXPECT_EQ(reference & 0xff, texel) << "texel=" << texel;
	}
}

} // namespace

#endif // ARCH_ARM64
