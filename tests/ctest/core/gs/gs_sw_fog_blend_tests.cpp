// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the fog blend stores, and what it is handed.
//
// gs-fog (SCPH-30001, 2026-09-07, two byte-identical runs) drew a flat ladder of
// all 26 fog coefficients under a per-channel FOGCOL -- R = 0, G = 0xff, B = 0xff
// -- against vertex colours chosen so each channel isolates a different half of
// the expression. 104 fills, four channels, and exactly one rule fits every one:
//
//     stored = Cf + floor((Cv - Cf) * F / 256)   ==   (F*Cv + (256 - F)*Cf) >> 8
//
// The fog colour's weight is 256 - F, NOT the documented 255 - F, and the
// multiply floors. The documented rule mismatches 100 of 104 fills on green and
// 104 of 104 on blue. The two agree exactly wherever the fog colour is zero --
// the (256-F)*Cf term vanishes -- which is every fog environment the nine earlier
// GS captures wrote, and is why none of them could have seen it.
//
// What the ladder reads, at the integer F silicon hands the blend:
//
//     R  Cv=0xff Cf=0     ->  (F*255)>>8, which is F-1 for F >= 1 and 0 at F = 0
//     G  Cv=0    Cf=0xff  ->  255 - F exactly, at every F
//     B  Cv=0xff Cf=0xff  ->  255 at every F, because Cv = Cf and F cancels
//     A  never fogged     ->  the vertex alpha
//
// AND THE BLEND RECEIVES AN INTEGER. The fog lane walks in the colour unit, a
// 128th of a level, so its value carries a fraction between pixels; silicon
// truncates it to eight bits before the multiply. gs-fog scores the truncating
// form at 100.000% over all 115,040 fog readings and the fractional form at
// 34.6%, 73.6% and 48.4% by section, so it is not a tie the capture could not
// break. That half is written differentially below -- sweeping the fraction must
// change nothing -- so it pins the rule without restating the blend.
//
// Rides ARCH_ARM64 like its siblings gs_sw_scanline_texfunc_tests.cpp and
// gs_sw_scanline_dither_tests.cpp: all three compile the JIT scanline directly,
// and the generator is per-architecture. Both the generated scanline and the C++
// one are driven, because a shipping build takes the first and a locked-down
// platform takes the second.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/MultiISA.h"

#ifndef MULTI_ISA_SHARED_COMPILATION

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
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

/// The rule, written once, in the console's own terms.
int FogBlend(int cv, int cf, int f)
{
	const int d = cv - cf;
	// A floor, not a truncation toward zero: the product is negative whenever the
	// fog colour is above the vertex colour, and gs-fog's green channel -- which
	// is exactly that case -- reads 255 - F with no rounding anywhere.
	const int q = (d * f) >> 8;

	return cf + q;
}

class SwFogBlendTest : public ::testing::Test
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

	/// A flat, untextured, unblended, FOGGED sprite into PSMCT32 -- the ladder's
	/// own draw. A sprite so that the fog coefficient comes from local.p.f and is
	/// constant across the row, which is what makes a ladder rung one value.
	static GSScanlineSelector MakeSelector()
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_NONE;
		sel.ababcd = 0xff;
		sel.prim = GS_SPRITE_CLASS;
		sel.iip = 0;
		sel.fge = 1;
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

	/// One four-pixel row. `frac` is the sub-unit part the fog DDA would be
	/// carrying mid-gradient, in the walk's own 128ths of a level.
	static u32 RunRow(DrawScanlinePtr fn, GSScanlineSelector sel, const GSPixelOffset4* off,
		u32 cvr, u32 cvg, u32 cvb, u32 cva, u32 cfr, u32 cfg, u32 cfb, u32 f, u32 frac)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = off->row;
		global.fzbc = off->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		// FOGCOL, split the way GSRendererSW splits it: r and b in one register,
		// g and a in the other, one channel per 16-bit lane.
		global.frb = GSVector4i(static_cast<int>(cfr | (cfb << 16)));
		global.fga = GSVector4i(static_cast<int>(cfg));

		local.gd = &global;
		// TFX_NONE, so the flat colour is already the byte -- the setup's own
		// srl16<7> has happened before this point in a real draw.
		local.c.rb = GSVector4i(static_cast<int>(cvr | (cvb << 16)));
		local.c.ga = GSVector4i(static_cast<int>(cvg | (cva << 16)));
		// A sprite's fog constant, on the walk's own scale with a fraction on it.
		local.p.f = GSVector4i(static_cast<int>(((f << 7) | frac) * 0x00010001u));

		const GSVertexSW scan = GSVertexSW::zero();
		fn(4, 0, 0, scan, local);

		return vm32[PixelAddr(0, 0)];
	}

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

u8* SwFogBlendTest::s_code = nullptr;
size_t SwFogBlendTest::s_code_used = 0;
GSLocalMemory* SwFogBlendTest::s_mem = nullptr;

// The ladder's own coefficients: both ends, the two values either side of each
// end, and the midpoint.
constexpr u32 kFogValues[] = {0, 1, 2, 128, 254, 255};

// gs-fog's three channel configurations, plus an interior pair that is neither
// end and neither equal -- the one arrangement where the floor and a rounding
// multiply disagree most often.
struct Channels
{
	u32 cv, cf;
	const char* what;
};
constexpr Channels kPairs[] = {
	{0xff, 0x00, "vertex 255 over fog 0"},
	{0x00, 0xff, "vertex 0 over fog 255"},
	{0xff, 0xff, "vertex 255 over fog 255"},
	{0x00, 0x00, "both zero"},
	{0xc3, 0x2b, "an interior pair"},
};
} // namespace

// ---------------------------------------------------------------------------
// The rule itself.
// ---------------------------------------------------------------------------

TEST_F(SwFogBlendTest, TheBlendWeightsTheFogColourBy256MinusF)
{
	const GSScanlineSelector sel = MakeSelector();
	DrawScanlinePtr jit = Compile(sel);
	ASSERT_NE(jit, nullptr) << "no code memory";

	const GSPixelOffset4* off = GetOffsets();

	for (const Channels& p : kPairs)
	{
		for (u32 f : kFogValues)
		{
			SCOPED_TRACE(testing::Message() << p.what << ", F " << f);

			// Red and blue carry the pair, green carries gs-fog's own G column,
			// alpha carries a value fog must not touch.
			const u32 got = RunRow(jit, sel, off, p.cv, 0x00, p.cv, 0x5a, p.cf, 0xff, p.cf, f, 0);

			EXPECT_EQ(static_cast<int>(got & 0xff), FogBlend(p.cv, p.cf, f)) << "red";
			EXPECT_EQ(static_cast<int>((got >> 8) & 0xff), FogBlend(0x00, 0xff, f)) << "green";
			EXPECT_EQ(static_cast<int>((got >> 16) & 0xff), FogBlend(p.cv, p.cf, f)) << "blue";
			EXPECT_EQ(static_cast<int>((got >> 24) & 0xff), 0x5a) << "alpha is never fogged";
		}
	}
}

// The three columns gs-fog printed, stated as the closed forms it stated them in,
// so a failure says which half of the expression moved rather than only that a
// byte did.
TEST_F(SwFogBlendTest, TheLaddersOwnColumns)
{
	const GSScanlineSelector sel = MakeSelector();
	DrawScanlinePtr jit = Compile(sel);
	ASSERT_NE(jit, nullptr) << "no code memory";

	const GSPixelOffset4* off = GetOffsets();

	for (u32 f : kFogValues)
	{
		SCOPED_TRACE(testing::Message() << "F " << f);

		// R: vertex 255 over fog 0. G: vertex 0 over fog 255. B: vertex 255 over
		// fog 255 -- gs-fog's own arrangement.
		const u32 got = RunRow(jit, sel, off, 0xff, 0x00, 0xff, 0x11, 0x00, 0xff, 0xff, f, 0);

		EXPECT_EQ(static_cast<int>(got & 0xff), static_cast<int>((f * 255) >> 8)) << "red";
		EXPECT_EQ(static_cast<int>((got >> 8) & 0xff), static_cast<int>(255 - f)) << "green";
		EXPECT_EQ(static_cast<int>((got >> 16) & 0xff), 255) << "blue";
	}

	// The documented weight would put green at 255 - ((255*f) >> 8), which is one
	// level high at every F the two disagree on. If this ever stops separating
	// them the case above has stopped deciding anything.
	int separating = 0;
	for (u32 f : kFogValues)
	{
		if (static_cast<int>(255 - f) != 255 - static_cast<int>((255 * f) >> 8))
			separating++;
	}
	EXPECT_GT(separating, 3) << "the ladder no longer tells the two weights apart";
}

// ---------------------------------------------------------------------------
// And what the blend is handed: the byte, never the walk's fraction.
// ---------------------------------------------------------------------------

TEST_F(SwFogBlendTest, TheBlendIgnoresTheFogFraction)
{
	const GSScanlineSelector sel = MakeSelector();
	DrawScanlinePtr jit = Compile(sel);
	ASSERT_NE(jit, nullptr) << "no code memory";

	const GSPixelOffset4* off = GetOffsets();

	for (const Channels& p : kPairs)
	{
		for (u32 f : kFogValues)
		{
			const u32 base = RunRow(jit, sel, off, p.cv, 0x00, p.cv, 0x5a, p.cf, 0xff, p.cf, f, 0);

			for (u32 frac : {1u, 63u, 64u, 100u, 127u})
			{
				SCOPED_TRACE(testing::Message() << p.what << ", F " << f << " + " << frac << "/128");

				EXPECT_EQ(RunRow(jit, sel, off, p.cv, 0x00, p.cv, 0x5a, p.cf, 0xff, p.cf, f, frac), base);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The C++ scanline is the whole renderer wherever there is no code memory to
// compile into, so it answers the same or it is a bug nobody would see in a
// shipping build.
// ---------------------------------------------------------------------------

TEST_F(SwFogBlendTest, TheCppScanlineAgrees)
{
	const GSScanlineSelector sel = MakeSelector();
	DrawScanlinePtr jit = Compile(sel);
	ASSERT_NE(jit, nullptr) << "no code memory";

	DrawScanlinePtr cpp = static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline);
	const GSPixelOffset4* off = GetOffsets();

	for (const Channels& p : kPairs)
	{
		for (u32 f : kFogValues)
		{
			for (u32 frac : {0u, 63u, 127u})
			{
				SCOPED_TRACE(testing::Message() << p.what << ", F " << f << " + " << frac << "/128");

				const u32 a = RunRow(jit, sel, off, p.cv, 0x00, p.cv, 0x5a, p.cf, 0xff, p.cf, f, frac);
				const u32 b = RunRow(cpp, sel, off, p.cv, 0x00, p.cv, 0x5a, p.cf, 0xff, p.cf, f, frac);

				EXPECT_EQ(a, b);
				EXPECT_EQ(static_cast<int>(b & 0xff), FogBlend(p.cv, p.cf, f)) << "red";
			}
		}
	}
}

#endif // MULTI_ISA_SHARED_COMPILATION
#endif // ARCH_ARM64
