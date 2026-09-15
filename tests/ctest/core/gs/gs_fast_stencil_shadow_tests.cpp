// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the fast stencil shadow road (GS/Renderers/Common/GSFastStencilShadow.h): which devices
// take it.
//
// The device rule has no setting behind it, so these cases are the whole contract. It is on for
// Vulkan with texture barriers off and dual-source blending, and off when any one of the three is
// missing. The case that matters most is D3D11: it also runs without texture barriers, and reading
// "no barriers" as "frame reads are expensive" would put a draw the D3D11 shader cannot express on
// a backend where the read it replaces is cheap.
//
// Rides gs_vertex_tests.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSFastStencilShadow.h"

#include <gtest/gtest.h>

namespace
{
	constexpr RenderAPI kAllApis[] = {
		RenderAPI::None, RenderAPI::D3D11, RenderAPI::Metal, RenderAPI::D3D12, RenderAPI::Vulkan, RenderAPI::OpenGL};
} // namespace

// The Adreno shape: Vulkan, the RT-copy workaround has turned barriers off, and the blend unit has
// a second input.
TEST(GSFastStencilShadow, OnForVulkanWithoutBarriersWithDualSource)
{
	EXPECT_TRUE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, true));
}

TEST(GSFastStencilShadow, OffWhenTextureBarriersAreOn)
{
	// Desktop Vulkan, and OverrideTextureBarriers=1 on an Adreno part.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, true, true));
}

TEST(GSFastStencilShadow, OffWithoutDualSourceBlend)
{
	// A Mali part on the RT-copy workaround.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, false));
}

TEST(GSFastStencilShadow, OffOnD3D11WithoutBarriers)
{
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::D3D11, false, true));
}

// Every combination: on exactly when all three facts hold.
TEST(GSFastStencilShadow, OnExactlyWhenAllThreeHold)
{
	for (RenderAPI api : kAllApis)
	{
		for (bool texture_barrier : {false, true})
		{
			for (bool dual_source : {false, true})
			{
				const bool expected = api == RenderAPI::Vulkan && !texture_barrier && dual_source;
				EXPECT_EQ(GSFastStencilShadow::DeviceQualifies(api, texture_barrier, dual_source), expected)
					<< "api " << static_cast<int>(api) << " texture_barrier " << texture_barrier << " dual_source "
					<< dual_source;
			}
		}
	}
}

// Backends other than Vulkan never assign the bit, so it has to start off.
TEST(GSFastStencilShadow, FeatureBitStartsOff)
{
	const GSDevice::FeatureSupport features;
	EXPECT_FALSE(features.fast_stencil_shadow);
}

// Which draws take the road. The registers are checked by IsCounterShape and the vertex bounds by
// VerticesQualify; a draw needs both.

namespace
{
	// Jak II's shadow counter, register for register as the census logged it: a triangle fan into
	// the 0x3300 frame, textured from that frame.
	struct CounterRegs
	{
		GIFRegPRIM prim{};
		GIFRegTEX0 tex0{};
		GIFRegTEX1 tex1{};
		GIFRegTEST test{};
		GIFRegFRAME frame{};
		GIFRegZBUF zbuf{};
		GIFRegFBA fba{};

		CounterRegs()
		{
			prim.PRIM = GS_TRIANGLEFAN;
			prim.TME = 1;
			frame.FBP = 0x3300 >> 5;
			frame.PSM = PSMCT32;
			frame.FBMSK = 0x00FFFFFF;
			tex0.TBP0 = frame.Block();
			tex0.PSM = PSMCT32;
			tex0.TFX = TFX_MODULATE;
			tex0.TCC = 1;
			test.ATE = 1;
			test.ATST = ATST_ALWAYS;
			zbuf.ZMSK = 1;
		}

		bool Matches() const { return GSFastStencilShadow::IsCounterShape(prim, tex0, tex1, test, frame, zbuf, fba); }
	};
} // namespace

TEST(GSFastStencilShadow, ShapeMatchesTheCounter)
{
	CounterRegs r;
	EXPECT_TRUE(r.Matches());

	// No alpha test at all passes everything too.
	r.test.ATE = 0;
	r.test.ATST = ATST_GEQUAL;
	EXPECT_TRUE(r.Matches());
}

// Ratchet & Clank: Up Your Arsenal's effect counter, as the renderer sees it. Its alpha test never
// passes and fails to the frame only, so it cannot change what is written, and the renderer's cached
// registers have already dropped it (ATE cleared, ATST and AFAIL left as written). The renderer checks
// the shape on that copy, so the counter takes the road, accepting a hidden counter that can drift a
// few levels low from 128.
TEST(GSFastStencilShadow, ShapeMatchesRatchetsCounterAfterTheAlphaTestIsDropped)
{
	CounterRegs r;
	r.test.ATE = 0;
	r.test.ATST = ATST_NEVER;
	r.test.AFAIL = AFAIL_FB_ONLY;
	EXPECT_TRUE(r.Matches());
}

// One register away from the counter is not the counter. Each of these changes what the draw writes
// or what it reads, so the blend equation would no longer be the draw.
TEST(GSFastStencilShadow, ShapeRejectsEveryOneRegisterChange)
{
	struct Change
	{
		const char* name;
		void (*apply)(CounterRegs&);
	};
	const Change changes[] = {
		{"untextured", [](CounterRegs& r) { r.prim.TME = 0; }},
		{"gouraud", [](CounterRegs& r) { r.prim.IIP = 1; }},
		{"fog", [](CounterRegs& r) { r.prim.FGE = 1; }},
		{"aa1", [](CounterRegs& r) { r.prim.AA1 = 1; }},
		{"24-bit frame", [](CounterRegs& r) { r.frame.PSM = PSMCT24; }},
		{"colour written", [](CounterRegs& r) { r.frame.FBMSK = 0; }},
		{"alpha masked too", [](CounterRegs& r) { r.frame.FBMSK = 0xFFFFFFFF; }},
		{"texture is another buffer", [](CounterRegs& r) { r.tex0.TBP0 = 0; }},
		{"16-bit texture", [](CounterRegs& r) { r.tex0.PSM = PSMCT16; }},
		{"decal", [](CounterRegs& r) { r.tex0.TFX = TFX_DECAL; }},
		{"texture alpha off", [](CounterRegs& r) { r.tex0.TCC = 0; }},
		{"linear magnification", [](CounterRegs& r) { r.tex1.MMAG = 1; }},
		{"linear minification", [](CounterRegs& r) { r.tex1.MMIN = 1; }},
		{"destination alpha test", [](CounterRegs& r) { r.test.DATE = 1; }},
		{"alpha test that can fail", [](CounterRegs& r) { r.test.ATST = ATST_GEQUAL; }},
		// Ratchet & Clank's counter with its alpha test as written. The auto-flush predicate sees these
		// registers, so it keeps splitting that counter; the renderer sees the copy with the test dropped
		// and takes it (ShapeMatchesRatchetsCounterAfterTheAlphaTestIsDropped).
		{"alpha test never passes, fails to the frame only",
			[](CounterRegs& r) {
				r.test.ATST = ATST_NEVER;
				r.test.AFAIL = AFAIL_FB_ONLY;
			}},
		{"depth written", [](CounterRegs& r) { r.zbuf.ZMSK = 0; }},
		{"fba", [](CounterRegs& r) { r.fba.FBA = 1; }},
	};

	for (const Change& c : changes)
	{
		CounterRegs r;
		c.apply(r);
		EXPECT_FALSE(r.Matches()) << c.name;
	}
}

TEST(GSFastStencilShadow, VerticesQualifyOnlyOnTheCounterSteps)
{
	const GSVector4 p0(10.5f, 20.5f, 0.0f, 0.0f);
	const GSVector4 p1(200.5f, 180.5f, 0.0f, 0.0f);

	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0, p1));
	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(130, 130, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(126, 130, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 131, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(64, 64, p0, p1, p0, p1));
}

TEST(GSFastStencilShadow, VerticesQualifyOnlyWhenSamplingTheirOwnPixel)
{
	const GSVector4 p0(10.5f, 20.5f, 0.0f, 0.0f);
	const GSVector4 p1(200.5f, 180.5f, 0.0f, 0.0f);
	const GSVector4 half(0.5f, 0.5f, 0.0f, 0.0f);
	const GSVector4 one_x(1.0f, 0.0f, 0.0f, 0.0f);
	const GSVector4 one_y(0.0f, 1.0f, 0.0f, 0.0f);

	// Texel centres half a pixel off the pixel positions still read the same pixel.
	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 - half, p1 - half));

	// A whole pixel off on either axis, at either corner, reads a neighbour.
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 + one_x, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0, p1 - one_y));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 - one_x, p1 - one_x));
}
