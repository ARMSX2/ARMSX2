// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The GS blend equation as the TileGpu fragment stage evaluates it.
//
// The console computes, per colour channel and in integer,
//
//     Cv = ((A - B) * C) >> 7 + D
//
// with A, B and D selecting Cs / Cd / zero and C a plain 0..255 byte in which 0x80 is 1.0. A
// raster pipeline's blend unit cannot express that: its factors saturate at 1.0, so every C
// above 0x80 is silently clamped, destination alpha arrives as dest/255 instead of dest/128,
// COLCLAMP's wrap has no expression at all, and PABE's per-pixel gate has none either. Those
// are the draws the fragment read-modify-write road takes, and gsTileGpuBlendChannel is the
// arithmetic it takes them with.
//
// ⚠️ THE ORACLE IS THE SOFTWARE RENDERER, and this suite holds the model to it with the
// scanline's OWN vector primitives rather than to a restatement of the spec. GSDrawScanline's
// AlphaBlend block is a `sub16`, a `modulate16<1>` against `alpha << 7`, an `add16`, and then
// either a mask to eight bits (COLCLAMP wrap) or the saturating `pu16` pack. Every expectation
// below is computed by running exactly those, so a divergence is a real divergence and not a
// disagreement between two readings of the manual.
//
// Two shapes are not the closed form, and both are the scanline's:
//
//   - A == B makes the equation Cv = D and the multiply never happens. The closed form agrees,
//     which is why the renderer's expressibility classifier can gate on A != B and be right.
//   - A 24-bit destination stores no alpha, and C = Ad there is exactly 1.0 rather than
//     whatever the unstored byte holds -- the scanline skips the multiply outright under
//     `!(sel.fpsm == 1 && sel.abc == 1)`.
//
// The fragment shader mirrors gsTileGpuBlendChannel statement for statement. It cannot be run
// here, so what this suite buys is that the model it mirrors is the console's; the shader's
// fidelity to the model is a reading matter, and the road's corpus arm is what exercises it.

#include "GS/GSVector.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include "common/Pcsx2Defs.h"

#include <gtest/gtest.h>

#include <array>

namespace
{
	/// One channel of the software renderer's AlphaBlend block, run through the very vector
	/// primitives the scanline uses. `cs`/`cd` are the source and destination bytes; the selectors
	/// are the ALPHA register's own encodings.
	///
	/// The lane layout is the scanline's: an 8-bit value in a 16-bit lane, so `sub16` can go
	/// negative and `modulate16<1>` -- a shift left by two and a signed high multiply -- lands
	/// exactly `(x * C) >> 7` with an arithmetic shift.
	int ScanlineBlendChannel(
		int cs, int cd, u32 a_sel, u32 b_sel, u32 c_sel, u32 d_sel, int as, int ad, int fix, bool colclamp, bool dest24)
	{
		const GSVector4i src = GSVector4i(cs);
		const GSVector4i dst = GSVector4i(cd);
		const GSVector4i zero = GSVector4i::zero();

		GSVector4i v;
		// `if (sel.aba != sel.abb)` -- the whole subtract/modulate/add chain, or nothing.
		if (a_sel != b_sel)
		{
			switch (a_sel)
			{
				case 0: v = src; break;
				case 1: v = dst; break;
				default: v = zero; break;
			}
			switch (b_sel)
			{
				case 0: v = v.sub16(src); break;
				case 1: v = v.sub16(dst); break;
				default: break;
			}
			// `if (!(sel.fpsm == 1 && sel.abc == 1))`
			if (!(dest24 && c_sel == 1))
			{
				const int factor = (c_sel == 0) ? as : ((c_sel == 1) ? ad : fix);
				v = v.modulate16<1>(GSVector4i(factor << 7));
			}
			switch (d_sel)
			{
				case 0: v = v.add16(src); break;
				case 1: v = v.add16(dst); break;
				default: break;
			}
		}
		else
		{
			switch (d_sel)
			{
				case 0: v = src; break;
				case 1: v = dst; break;
				default: v = zero; break;
			}
		}

		const int lane = v.I16[0];
		// WriteFrame: COLCLAMP 0 masks to eight bits, COLCLAMP 1 rides the saturating pack.
		if (!colclamp)
			return lane & 0xFF;
		return (lane < 0) ? 0 : ((lane > 255) ? 255 : lane);
	}

	constexpr std::array<int, 9> kBytes = {0, 1, 2, 63, 64, 127, 128, 254, 255};
	constexpr std::array<int, 8> kFactors = {0, 1, 64, 127, 128, 129, 200, 255};
} // namespace

// The whole (A, B, C, D) x COLCLAMP space over an interesting byte lattice, against the scanline.
// 81 equations x 2 clamp modes x 9 source bytes x 9 destination bytes x 8 factors.
TEST(GSTileGpuBlend, EveryEquationMatchesTheSoftwareScanline)
{
	u32 checked = 0;
	for (u32 a = 0; a < 3; a++)
	{
		for (u32 b = 0; b < 3; b++)
		{
			for (u32 c = 0; c < 3; c++)
			{
				for (u32 d = 0; d < 3; d++)
				{
					for (int colclamp = 0; colclamp < 2; colclamp++)
					{
						for (const int cs : kBytes)
						{
							for (const int cd : kBytes)
							{
								for (const int f : kFactors)
								{
									// One factor sweep drives whichever of As / Ad / FIX the
									// equation selects; the other two are pinned somewhere
									// unhelpful so a model reading the wrong one is caught.
									const int as = (c == 0) ? f : 0x11;
									const int ad = (c == 1) ? f : 0x22;
									const int fix = (c == 2) ? f : 0x33;
									const int want = ScanlineBlendChannel(
										cs, cd, a, b, c, d, as, ad, fix, colclamp != 0, false);
									const int got = gsTileGpuBlendChannel(
										cs, cd, a, b, c, d, as, ad, fix, colclamp != 0, false);
									ASSERT_EQ(got, want)
										<< "A=" << a << " B=" << b << " C=" << c << " D=" << d
										<< " colclamp=" << colclamp << " Cs=" << cs << " Cd=" << cd
										<< " factor=" << f;
									checked++;
								}
							}
						}
					}
				}
			}
		}
	}
	EXPECT_EQ(checked, 3u * 3u * 3u * 3u * 2u * 9u * 9u * 8u);
}

// A 24-bit destination has no alpha byte, and C=Ad is exactly 1.0 there rather than whatever the
// unstored byte holds. Swept against the scanline over the same lattice, with the destination
// alpha varied so a model that read it would be caught.
TEST(GSTileGpuBlend, TwentyFourBitDestinationTakesDestinationAlphaAsOne)
{
	for (u32 a = 0; a < 3; a++)
	{
		for (u32 b = 0; b < 3; b++)
		{
			for (u32 d = 0; d < 3; d++)
			{
				for (const int cs : kBytes)
				{
					for (const int cd : kBytes)
					{
						for (const int ad : kFactors)
						{
							const int want = ScanlineBlendChannel(cs, cd, a, b, 1, d, 0x11, ad, 0x33, true, true);
							const int got = gsTileGpuBlendChannel(cs, cd, a, b, 1, d, 0x11, ad, 0x33, true, true);
							ASSERT_EQ(got, want) << "A=" << a << " B=" << b << " D=" << d << " Cs=" << cs
												 << " Cd=" << cd << " Ad=" << ad;
						}
					}
				}
			}
		}
	}
}

// The two things a fixed-function factor cannot do, stated as values rather than as a sweep, so
// the suite says WHY this road exists and not only that it agrees with the scanline.
TEST(GSTileGpuBlend, CoefficientsAboveOneReachAlmostTwo)
{
	// Cs * (255/128) over black: the classic "double the source" additive, which a saturating
	// factor clamps to 1.0 and loses half of.
	EXPECT_EQ(gsTileGpuBlendChannel(100, 0, /*A=*/0, /*B=*/2, /*C=*/2, /*D=*/2, 0, 0, 255, true, false), 199);
	// ...and at 0x80 the same expression is exactly 1.0, which is where the two roads agree.
	EXPECT_EQ(gsTileGpuBlendChannel(100, 0, 0, 2, 2, 2, 0, 0, 128, true, false), 100);
	// Destination alpha is scaled by 128, not by 255: Ad = 0xFF is 1.99, not 1.0.
	EXPECT_EQ(gsTileGpuBlendChannel(0, 100, 1, 2, 1, 2, 0, 255, 0, true, false), 199);
}

TEST(GSTileGpuBlend, ColclampWrapsWhereTheClampWouldSaturate)
{
	// (200 - 0) * 255 >> 7 = 398 -- FIX 0xFF is 1.99, not 2.0, and the difference is visible here.
	// Clamped: 255. Wrapped: 398 & 0xFF = 142.
	EXPECT_EQ(gsTileGpuBlendChannel(200, 0, 0, 2, 2, 2, 0, 0, 255, true, false), 255);
	EXPECT_EQ(gsTileGpuBlendChannel(200, 0, 0, 2, 2, 2, 0, 0, 255, false, false), 398 & 0xFF);
	// ...and underflow the same way: (0 - 40) * 1.0 = -40. Clamped: 0. Wrapped: 216.
	EXPECT_EQ(gsTileGpuBlendChannel(0, 40, 0, 1, 2, 2, 0, 0, 128, true, false), 0);
	EXPECT_EQ(gsTileGpuBlendChannel(0, 40, 0, 1, 2, 2, 0, 0, 128, false, false), 216);
}

TEST(GSTileGpuBlend, EqualAAndBDegenerateToD)
{
	// (X - X) * C + D is D whatever C is -- which is what lets the expressibility classifier
	// gate on A != B before it looks at anything else.
	for (u32 s = 0; s < 3; s++)
	{
		for (const int f : kFactors)
		{
			EXPECT_EQ(gsTileGpuBlendChannel(70, 90, s, s, 0, 0, f, f, f, true, false), 70);
			EXPECT_EQ(gsTileGpuBlendChannel(70, 90, s, s, 0, 1, f, f, f, true, false), 90);
			EXPECT_EQ(gsTileGpuBlendChannel(70, 90, s, s, 0, 2, f, f, f, true, false), 0);
		}
	}
}

// The packed form the fragment stage actually evaluates, against the semantic model above -- the
// second link of the chain from the software renderer to the shader. It is the interesting one:
// the row carries COEFFICIENTS rather than the register's selectors, because spelt as selectors
// the arm cost 828 SPIR-V words against 556 of headroom on the Adreno 650.
TEST(GSTileGpuBlend, PackedCoefficientFormReproducesTheEquation)
{
	for (u32 a = 0; a < 3; a++)
	{
		for (u32 b = 0; b < 3; b++)
		{
			for (u32 c = 0; c < 3; c++)
			{
				for (u32 d = 0; d < 3; d++)
				{
					for (int colclamp = 0; colclamp < 2; colclamp++)
					{
						for (u32 fmt = 0; fmt < 3; fmt++)
						{
							for (const int cs : kBytes)
							{
								for (const int cd : kBytes)
								{
									for (const int f : kFactors)
									{
										const int as = (c == 0) ? f : 0x11;
										const int ad = (c == 1) ? f : 0x22;
										const int fix = (c == 2) ? f : 0x33;
										const u32 packed =
											gsTileGpuPackBlend(true, a, b, c, d, fix, colclamp != 0, false, fmt);
										const int want = gsTileGpuBlendChannel(
											cs, cd, a, b, c, d, as, ad, fix, colclamp != 0, fmt == 1);
										const int got = gsTileGpuBlendChannelPacked(cs, cd, packed, as, ad);
										ASSERT_EQ(got, want)
											<< "A=" << a << " B=" << b << " C=" << c << " D=" << d << " fmt=" << fmt
											<< " colclamp=" << colclamp << " Cs=" << cs << " Cd=" << cd
											<< " factor=" << f;
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

// The state row's bit layout, which the fragment shader unpacks by hand.
TEST(GSTileGpuBlend, PackedBlendCarriesTheEquationAndItsModifiers)
{
	// A = Cd, B = zero, C = As, D = Cd: (Cd - 0) * As + Cd, so ka = 0, kb = +1, D is Cd.
	const u32 v = gsTileGpuPackBlend(/*abe=*/true, /*a=*/1, /*b=*/2, /*c=*/0, /*d=*/1, /*fix=*/0xA5,
		/*colclamp=*/false, /*pabe=*/true, /*fmt=*/0);
	EXPECT_EQ((v >> kGSTileBlendKaShift) & 3u, 1u); // 0, biased
	EXPECT_EQ((v >> kGSTileBlendKbShift) & 3u, 2u); // +1, biased
	EXPECT_FALSE(v & kGSTileBlendDsBit);
	EXPECT_TRUE(v & kGSTileBlendDdBit);
	EXPECT_EQ((v >> kGSTileBlendCShift) & 3u, 0u);
	EXPECT_TRUE(v & kGSTileBlendEnable);
	EXPECT_TRUE(v & kGSTileBlendWrap); // COLCLAMP 0 is the wrap
	EXPECT_TRUE(v & kGSTileBlendPabe);
	EXPECT_EQ(v & 0xFF000000u, 0u); // nothing rides above the equation

	// C = Ad on a 24-bit destination becomes the constant 0x80, which is 1.0 exactly.
	const u32 d24 = gsTileGpuPackBlend(true, 0, 2, 1, 2, 0x40, true, false, 1);
	EXPECT_EQ((d24 >> kGSTileBlendCShift) & 3u, 2u);
	EXPECT_EQ((d24 >> kGSTileBlendFixShift) & 0xFFu, 0x80u);
	// ...and on a 32-bit one it stays the destination alpha.
	const u32 d32 = gsTileGpuPackBlend(true, 0, 2, 1, 2, 0x40, true, false, 0);
	EXPECT_EQ((d32 >> kGSTileBlendCShift) & 3u, 1u);
}

// FBMSK reduced to the frame format's stored bits is the whole of the exact write mask, for
// every format -- including the 16-bit one, which looks like it should need work and does not:
// the console packs FBMSK by the same 5551 packing it packs the colour with, so bit k of a
// stored channel is bit k of the byte an expanded RGBA8 target holds.
TEST(GSTileGpuBlend, KeepMaskIsTheFormatsStoredBitsOfFbmsk)
{
	// PSMCT32: every bit is stored, so the mask is the register.
	EXPECT_EQ(gsTileGpuFrameKeepMask(0x7F000000u, 0xFFFFFFFFu), 0x7F000000u);
	// Katamari's punctuation glyph store: alpha bits 24-30 kept, bit 31 writable.
	EXPECT_EQ(gsTileGpuFrameKeepMask(0x7F000000u, 0xFFFFFFFFu) >> 24, 0x7Fu);
	// PSMCT24 stores no alpha, so nothing there can be kept.
	EXPECT_EQ(gsTileGpuFrameKeepMask(0xFF000000u, 0x00FFFFFFu), 0u);
	// PSMCT16 stores five bits per colour channel and one of alpha. A mask over the low three
	// bits of a byte keeps nothing, because the format stores nothing there.
	EXPECT_EQ(gsTileGpuFrameKeepMask(0x00000007u, 0x80F8F8F8u), 0u);
	EXPECT_EQ(gsTileGpuFrameKeepMask(0x000000F8u, 0x80F8F8F8u), 0x000000F8u);
	// OutRun's sub-channel green mask, the corpus's one non-alpha partial case.
	EXPECT_EQ(gsTileGpuFrameKeepMask(0x00003FFFu, 0x80F8F8F8u), 0x000038F8u);
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0x00003FFFu, 0x80F8F8F8u));
}
