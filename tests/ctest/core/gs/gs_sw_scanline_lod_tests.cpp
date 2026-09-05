// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The three texture rules the other scanline suites do not reach: the truncated
// perspective reciprocal, the per-pixel choice between MMAG and MMIN, and the
// four-bit trilinear weight.
//
// Each of them is implemented twice -- once in the C++ reference scanline and once
// in the ARM64 generator -- and the two are what ShouldUseCDrawScanline picks
// between at runtime, so every test here runs the same span through both and
// requires the same pixels out. That catches a rule transcribed into one path and
// not the other, which is the failure this suite exists for.
//
// Where a rule has an observable that does not need a model, the test also states
// it directly, so reverting the rule in BOTH paths still fails rather than quietly
// agreeing on the wrong answer.
//
// It reads what the scanline sampled by drawing DECAL out of a texture whose every
// texel names its own address, the same trick the tclag suite uses.
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

// One texel, in the 16.16 the coordinate is carried in.
constexpr int kTexel = 0x10000;

// What one span of four pixels stored, per pixel.
struct Row
{
	u32 px[4];

	bool operator==(const Row& o) const
	{
		return px[0] == o.px[0] && px[1] == o.px[1] && px[2] == o.px[2] && px[3] == o.px[3];
	}
};

// Everything a span needs that is not in the selector. The two texture levels are
// filled by the caller; level 1 is only read when mmin is on.
struct Span
{
	float s0, t0, q0;
	float ds, dt, dq;
	float ltfx_q = 0.0f;
	u32 lod_i = 0;
	GSVector4i lod_f = GSVector4i::zero();
};

// The reciprocal the scanline multiplies by, in scalar, so a test can say what it
// expects rather than only that the two paths agree. Same rule as
// GSPerspectiveRecip: a float32 reciprocal with its low ten mantissa bits cleared.
float TruncRecip(float q)
{
	float r = 1.0f / q;
	u32 bits;

	std::memcpy(&bits, &r, sizeof(bits));
	bits &= 0xfffffc00u;
	std::memcpy(&r, &bits, sizeof(bits));

	return r;
}

class SwScanlineLodTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 64 * kCodeSlotSize;

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

	// The reduced key GetScanlineGlobalData builds, so this compiles the same setup
	// variant a real draw would.
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

	// DECAL out of a 16x16 texture into PSMCT32: the stored pixel IS the sampled
	// texel, with nothing between the sampler and the readback.
	static GSScanlineSelector BaseSelector()
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
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
		const Span& span, const u32* tex0, const u32* tex1, Row& out)
	{
		u32* vm32 = s_mem->vm32();
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(x, 0)] = 0x0Du;

		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = GetOffsets()->row;
		global.fzbc = GetOffsets()->col;
		global.fm = GSVector4i(0);
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		global.tex[0] = tex0;
		global.tex[1] = tex1;

		// REPEAT on both axes over sixteen texels, as GSRendererSW derives it.
		global.t.min = GSVector4i::zero();
		global.t.max = GSVector4i::zero();
		global.t.minmax = GSVector4i::zero();
		global.t.mask = GSVector4i(static_cast<int>(0xffffffffu));
		for (int i = 0; i < 8; i++)
			global.t.min.U16[i] = 15;
		global.t.minmax.U16[0] = 15; // min u
		global.t.minmax.U16[1] = 15; // min v

		global.ltfx_q = GSVector4(span.ltfx_q);
		global.lod.i = GSVector4i(static_cast<int>(span.lod_i));
		global.lod.f = span.lod_f;

		local.gd = &global;

		// GSDrawScanline::BeginDraw does this for a constant-LOD draw; the harness
		// calls the two halves of the pipeline directly, so it does it here.
		if (sel.mmin && sel.lcm)
		{
			GSVector4i v = global.t.minmax.srl16(global.lod.i.extract32<0>());
			v = v.upl16(v);
			local.temp.uv_minmax[0] = v.upl32(v);
			local.temp.uv_minmax[1] = v.uph32(v);
		}

		// DECAL with TCC on takes no vertex colour, and there is no depth or fog, so
		// the setup runs its texture half alone.
		GSVertexSW vertex[3];
		u16 index[3] = {0, 1, 2};
		for (int i = 0; i < 3; i++)
			vertex[i] = GSVertexSW::zero();

		GSVertexSW dscan = GSVertexSW::zero();
		dscan.t = GSVector4(span.ds, span.dt, span.dq, 0.0f);

		setup(vertex, index, dscan, local);

		GSVertexSW scan = GSVertexSW::zero();
		scan.t = GSVector4(span.s0, span.t0, span.q0, 0.0f);

		draw(4, 0, 0, scan, local);

		for (int x = 0; x < 4; x++)
			out.px[x] = vm32[PixelAddr(x, 0)];
	}

	// One span through both implementations. They must agree; the caller then makes
	// its claim about the value they agreed on.
	static Row Both(GSScanlineSelector sel, const Span& span, const u32* tex0, const u32* tex1, const char* what)
	{
		Row cpp{}, jit{};

		Run(sel, &isa_native::GSDrawScanline::CSetupPrim,
			static_cast<DrawScanlinePtr>(&isa_native::GSDrawScanline::CDrawScanline),
			span, tex0, tex1, cpp);

		SetupPrimPtr setup = CompileSetup(sel);
		DrawScanlinePtr draw = CompileScanline(sel);
		EXPECT_TRUE(setup && draw) << what << ": the code buffer ran out";
		if (!setup || !draw)
			return cpp;

		Run(sel, setup, draw, span, tex0, tex1, jit);

		for (int i = 0; i < 4; i++)
			EXPECT_EQ(cpp.px[i], jit.px[i]) << what << ": the C++ scanline and the generated one "
											   "disagree at pixel " << i;
		return cpp;
	}

	// A texture whose every texel names its own address, in all four channels.
	static const u32* AddressTexture()
	{
		static u32 tex[256];
		for (int i = 0; i < 256; i++)
		{
			const u32 b = static_cast<u32>(i);
			tex[i] = b | (b << 8) | (b << 16) | (b << 24);
		}
		return tex;
	}

	// The two mip levels the trilinear tests blend: level 0 black, level 1 a mid
	// value in every channel, so a blend is visible in every channel.
	static const u32* Level0Texture()
	{
		static u32 tex[256] = {};
		return tex;
	}

	static const u32* Level1Texture()
	{
		static u32 tex[256];
		for (int i = 0; i < 256; i++)
			tex[i] = 0x40404040u;
		return tex;
	}

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

u8* SwScanlineLodTest::s_code = nullptr;
size_t SwScanlineLodTest::s_code_used = 0;
GSLocalMemory* SwScanlineLodTest::s_mem = nullptr;

// ---- the truncated perspective reciprocal -------------------------------------

// The GS multiplies by a reciprocal truncated to thirteen mantissa bits rather than
// dividing, so a perspective coordinate lands a little short of the true quotient --
// which changes the sampled texel wherever the exact quotient lands on a boundary.
TEST_F(SwScanlineLodTest, PerspectiveSamplesTheTruncatedReciprocal)
{
	GSScanlineSelector sel = BaseSelector();
	sel.fst = 0; // perspective
	sel.ltf = 0; // nearest, so the stored pixel is one texel and not a blend

	// s and q are constant across the span, so nothing here depends on the DDA: the
	// only thing between s/q and the texel is the reciprocal. q = 3 is a value whose
	// float32 reciprocal has mantissa bits below the truncation, and s is three
	// texels' worth of it, so the exact quotient lands exactly on texel 2.
	const float q = 3.0f;
	const float s = 3.0f * 2.0f * static_cast<float>(kTexel);

	Span span{};
	span.s0 = s;
	span.t0 = 0.0f;
	span.q0 = q;
	span.ds = 0.0f;
	span.dt = 0.0f;
	span.dq = 0.0f;

	const int exact = static_cast<int>(s / q) >> 16;
	const int truncated = static_cast<int>(s * TruncRecip(q)) >> 16;
	ASSERT_EQ(exact, 2);
	ASSERT_EQ(truncated, 1) << "the shape of this test is wrong: the two models must differ here";

	const Row got = Both(sel, span, AddressTexture(), nullptr, "perspective recip");

	for (int i = 0; i < 4; i++)
	{
		EXPECT_EQ(got.px[i] & 0xff, static_cast<u32>(truncated))
			<< "pixel " << i << ": the coordinate came out of an exact divide, not the "
							   "hardware's truncated reciprocal";
	}
}

// ---- per-pixel MMAG / MMIN ----------------------------------------------------

namespace
{
// A perspective span whose q crosses ltfx_q between pixel 1 and pixel 2, with s and
// t proportional to q so the sampled coordinate sits a quarter of a texel into texel
// (5, 5) on every pixel. Nearest reads texel (5,5) = 85; linear straddles the pair
// below it and reads a blend, which is a different number.
Span CrossoverSpan()
{
	constexpr float kCoord = 5.0f * static_cast<float>(kTexel) + 0x4000;

	Span span{};
	span.q0 = 0.5f;
	span.dq = 0.25f; // q = 0.5, 0.75, 1.0, 1.25
	span.s0 = kCoord * span.q0;
	span.t0 = kCoord * span.q0;
	span.ds = kCoord * span.dq;
	span.dt = kCoord * span.dq;
	span.ltfx_q = 0.875f; // pixels 0 and 1 minify, pixels 2 and 3 do not
	return span;
}
} // namespace

TEST_F(SwScanlineLodTest, LtfxFiltersOnlyThePixelsBelowTheCrossing)
{
	const Span span = CrossoverSpan();
	const u32* tex = AddressTexture();

	GSScanlineSelector all_linear = BaseSelector();
	all_linear.fst = 0;
	all_linear.ltf = 1;

	GSScanlineSelector per_pixel = all_linear;
	per_pixel.ltfx = 1;
	per_pixel.ltfx_ge = 0;

	const Row linear = Both(all_linear, span, tex, nullptr, "ltf everywhere");
	const Row mixed = Both(per_pixel, span, tex, nullptr, "ltfx, linear below the crossing");

	// The whole point of the rule: one span, two filters, chosen per pixel.
	EXPECT_EQ(mixed.px[0], linear.px[0]);
	EXPECT_EQ(mixed.px[1], linear.px[1]);
	EXPECT_NE(mixed.px[2], linear.px[2]);
	EXPECT_NE(mixed.px[3], linear.px[3]);

	// And the nearest side reads the texel the coordinate is actually in, which is
	// what withholding both the half-texel bias and the weight buys.
	EXPECT_EQ(mixed.px[2] & 0xff, 5u * 16u + 5u);
	EXPECT_EQ(mixed.px[3] & 0xff, 5u * 16u + 5u);
}

TEST_F(SwScanlineLodTest, LtfxGeFlipsWhichSideFilters)
{
	const Span span = CrossoverSpan();
	const u32* tex = AddressTexture();

	GSScanlineSelector all_linear = BaseSelector();
	all_linear.fst = 0;
	all_linear.ltf = 1;

	GSScanlineSelector flipped = all_linear;
	flipped.ltfx = 1;
	flipped.ltfx_ge = 1;

	const Row linear = Both(all_linear, span, tex, nullptr, "ltf everywhere");
	const Row mixed = Both(flipped, span, tex, nullptr, "ltfx, linear above the crossing");

	// Exactly the other two pixels.
	EXPECT_NE(mixed.px[0], linear.px[0]);
	EXPECT_NE(mixed.px[1], linear.px[1]);
	EXPECT_EQ(mixed.px[2], linear.px[2]);
	EXPECT_EQ(mixed.px[3], linear.px[3]);

	EXPECT_EQ(mixed.px[0] & 0xff, 5u * 16u + 5u);
	EXPECT_EQ(mixed.px[1] & 0xff, 5u * 16u + 5u);
}

// ---- the trilinear weight -----------------------------------------------------

namespace
{
// An affine span sitting on texel (0,0), so the only thing that varies between the
// tests below is the level blend.
Span FlatSpan()
{
	Span span{};
	span.s0 = 0.0f;
	span.t0 = 0.0f;
	span.q0 = 1.0f;
	span.ds = 0.0f;
	span.dt = 0.0f;
	span.dq = 0.0f;
	return span;
}
} // namespace

TEST_F(SwScanlineLodTest, EveryLaneOfAConstantLodFractionReachesTheBlend)
{
	// Level 0 is black and level 1 is 0x40 in every channel, so a blend shows up in
	// all four channels of all four pixels -- and a lane that never reaches the blend
	// leaves its channel at level 0's value.
	const u32* lo = Level0Texture();
	const u32* hi = Level1Texture();

	GSScanlineSelector sel = BaseSelector();
	sel.fst = 1;
	sel.ltf = 0;
	sel.mmin = 2; // trilinear
	sel.lcm = 1;  // constant level, so lod.i and lod.f come straight from the global

	Span span = FlatSpan();
	span.lod_i = 0;
	span.lod_f = GSVector4i(0x8000).xxxxlh();

	const Row got = Both(sel, span, lo, hi, "constant lod fraction");

	// Uniform weight, so a uniform result -- in every channel of every pixel.
	for (int i = 0; i < 4; i++)
	{
		EXPECT_EQ(got.px[i], got.px[0]) << "pixel " << i << " blended on a different weight";
		for (int ch = 0; ch < 4; ch++)
		{
			const u32 v = (got.px[i] >> (ch * 8)) & 0xff;
			EXPECT_GT(v, 0u) << "pixel " << i << " channel " << ch << " never left level 0";
			EXPECT_LT(v, 0x40u) << "pixel " << i << " channel " << ch << " jumped to level 1";
		}
	}

	// The shape of the bug this pins: the fraction used to be broadcast with
	// xxxxl().xxzz(), which leaves lanes 5 and 7 zero -- the blue and alpha of pixels
	// 2 and 3. Feed the scanline exactly that and those two pixels come out different
	// from the other two, which is what a real draw was doing.
	Span holed = span;
	holed.lod_f = GSVector4i(0x8000).xxxxlh();
	holed.lod_f.U16[5] = 0;
	holed.lod_f.U16[7] = 0;

	const Row bad = Both(sel, holed, lo, hi, "lod fraction with lanes 5 and 7 zeroed");
	EXPECT_EQ(bad.px[0], got.px[0]);
	EXPECT_EQ(bad.px[1], got.px[1]);
	EXPECT_NE(bad.px[2], got.px[2]);
	EXPECT_NE(bad.px[3], got.px[3]);
}

TEST_F(SwScanlineLodTest, TheTrilinearWeightIsFourBits)
{
	const u32* lo = Level0Texture();
	const u32* hi = Level1Texture();

	GSScanlineSelector sel = BaseSelector();
	sel.fst = 1;
	sel.ltf = 0;
	sel.mmin = 2;
	sel.lcm = 1;

	Span a = FlatSpan();
	a.lod_f = GSVector4i(0x1000).xxxxlh();
	Span b = FlatSpan();
	b.lod_f = GSVector4i(0x1fff).xxxxlh();
	Span c = FlatSpan();
	c.lod_f = GSVector4i(0x2000).xxxxlh();

	const Row ra = Both(sel, a, lo, hi, "weight 0x1000");
	const Row rb = Both(sel, b, lo, hi, "weight 0x1fff");
	const Row rc = Both(sel, c, lo, hi, "weight 0x2000");

	// Everything below the top four bits is thrown away, and the next step up is a
	// different colour: sixteen steps across the level, not two hundred and fifty.
	EXPECT_EQ(ra.px[0], rb.px[0]);
	EXPECT_NE(ra.px[0], rc.px[0]);
}

// The renderer builds the constant fraction with xxxxlh(), which is what puts it in
// all eight 16-bit lanes. The previous xxxxl().xxzz() left lanes 5 and 7 zero, and
// the test above shows what the scanline then does with it.
TEST(SwScanlineLodFraction, TheConstantFractionBroadcastFillsEveryLane)
{
	const GSVector4i f = GSVector4i(0x1234).xxxxlh();

	for (int i = 0; i < 8; i++)
		EXPECT_EQ(f.U16[i], 0x1234) << "lane " << i;
}

} // namespace

#endif // ARCH_ARM64
