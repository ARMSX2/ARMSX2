// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the SW scanline's dither unit.
//
// The gs-dither hardware capture (SCPH-30001, 2026-08-11) is the first thing in
// this tree to measure the dither unit at all -- every claim we shipped about it
// before came from reading our own code. What it measured: the matrix entry is
// DM[y & 3][x & 3], added to the eight-bit colour, the same entry on red, green
// and blue, with alpha taking nothing.
//
// It also found this renderer skipping the whole thing on a flat sprite. The
// rasterizer routes a flat, untextured, unblended sprite to a bulk rectangle
// fill, selected by GSScanlineSelector::IsSolidRect() -- and the fill writes one
// constant colour, while dither is applied per pixel in the scanline the fill
// bypasses. The predicate tested primitive class, shading, texture, blend, depth
// test, alpha test, DATE and fog, and not DTHE. On silicon the same grid drawn
// as sprites and as triangles is identical to the pixel; ours differed on 1116
// of 4096, and games fill 16-bit targets with flat sprites constantly.
//
// This file guards the predicate's contract. It rides ARCH_ARM64 like its
// sibling gs_sw_scanline_date_tests.cpp, whose scanline-compiling harness it
// will share.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSScanlineEnvironment.h"

#include <gtest/gtest.h>

namespace
{
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

} // namespace

#endif // ARCH_ARM64
