// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// FRAME.FBMSK as a per-channel colour write mask.
//
// The GS masks frame-buffer writes at BIT granularity; a raster pipeline masks at CHANNEL
// granularity. TileGpu bridges the two by dropping a channel exactly where FBMSK covers
// every bit the frame format stores in it, and handing the result to
// VkPipelineColorBlendAttachmentState::colorWriteMask.
//
// Getting this wrong is not a subtle defect. OutRun 2006 lays three full-screen sprites a
// frame over the finished world under FBMSK=0x00FFFFFF with a black fragment colour,
// meaning to rewrite the frame's alpha byte alone; with RGB unmasked they paint the world
// black, and what reaches the screen is the clear colour with the HUD and the car on it.
// Beyond Good & Evil draws silhouettes into the display buffer that is currently ON SCREEN
// under the same mask, and the alpha-only full-screen blit that ends every frame then
// copies the damage into the other buffer -- the picture ratchets to black over three
// frames. GT4, GT4 Online Public Beta and Xenosaga are the same shape.
//
// Four things are pinned here beyond the mapping itself:
//
//  - the FORMAT's stored bits decide a channel, not the raw byte. PSMCT16 keeps five bits
//    per colour channel, so FBMSK=0x000000F8 preserves the whole of red even though byte 0
//    is not 0xFF; and a PSMCT24 frame under FBMSK=0x00FFFFFF keeps every bit it stores, so
//    the draw writes nothing at all even though byte 3 reads as writable.
//  - FBMSK's ALPHA half is DEFERRED. A live draw always writes alpha; only AFAIL RGB_ONLY
//    takes it away. Preserving a target's alpha exposes unseeded pool bytes to a later
//    TCC=1 read -- measured on Ace Combat 5 as a white band across the sky, 0.021 -> 0.080
//    mean-abs against the software golden. These tests are what says the carve-out can go
//    the day targets carry a seeded alpha.
//  - the register mask and the AFAIL fold COMPOSE; neither overrides the other.
//  - the seed skip ("this sprite provably overwrites the page, do not bring the old bytes
//    in first") is gated on gsTileFrameWriteIsTotal, NOT on the channel mask being RGBA. A
//    channel the mask reports as written can still be preserving bits inside itself, and a
//    draw that preserves anything needs its page seeded.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <ios>

namespace
{
constexpr u32 kC32 = 0xFFFFFFFFu; // PSMCT32 / PSMZ32 stored bits
constexpr u32 kC24 = 0x00FFFFFFu; // PSMCT24 / PSMZ24
constexpr u32 kC16 = 0x80F8F8F8u; // PSMCT16 / PSMCT16S and the Z twins

constexpr u8 kR = 0x1, kG = 0x2, kB = 0x4, kA = 0x8;

// The composed policy the renderer actually asks for, with the alpha test defaulted off.
// The third argument is "every fragment fails this draw's alpha test" -- ATST=NEVER when
// these tests were written, and since the constant fold, also a comparison that provably
// fails (gs_tilegpu_alpha_test_fold_tests.cpp).
constexpr u8 Mask(u32 fbmsk, u32 fmsk, bool atst_all_fail = false, u32 afail = AFAIL_KEEP)
{
	return gsTileFrameColorWriteMask(fbmsk, fmsk, atst_all_fail, afail);
}
} // namespace

// ---------------------------------------------------------------------------------------
// The FBMSK -> channel mapping itself.
// ---------------------------------------------------------------------------------------

TEST(TileGpuFbmskMask, UnmaskedWritesEveryChannel)
{
	EXPECT_EQ(gsTileFrameWriteMask(0x00000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_EQ(gsTileFrameWriteMask(0x00000000u, kC16), kGSTileChannelsRGBA);
	// A 24-bit frame stores no alpha, so its alpha channel has nothing behind it and rides
	// along with the ones that do: the target's alpha is not guest memory for this format
	// (writeback byte-masks it away).
	EXPECT_EQ(gsTileFrameWriteMask(0x00000000u, kC24), kGSTileChannelsRGBA);
}

TEST(TileGpuFbmskMask, FullyMaskedWritesNothing)
{
	EXPECT_EQ(gsTileFrameWriteMask(0xFFFFFFFFu, kC32), 0);
	EXPECT_EQ(gsTileFrameWriteMask(0xFFFFFFFFu, kC24), 0);
	EXPECT_EQ(gsTileFrameWriteMask(0xFFFFFFFFu, kC16), 0);
	// A mask only has to cover the bits the FORMAT stores, not all 32.
	EXPECT_EQ(gsTileFrameWriteMask(kC16, kC16), 0);
	EXPECT_EQ(gsTileFrameWriteMask(kC24, kC24), 0);
}

TEST(TileGpuFbmskMask, WholeByteMasksNameTheirChannels)
{
	// The world-erasure idiom: RGB preserved, alpha alone lands.
	EXPECT_EQ(gsTileFrameWriteMask(0x00FFFFFFu, kC32), kA);
	// The mirror: alpha preserved, RGB lands (Classic's "(rgb only)").
	EXPECT_EQ(gsTileFrameWriteMask(0xFF000000u, kC32), kGSTileChannelsRGB);
	// FlatOut 2's single-channel exclusion.
	EXPECT_EQ(gsTileFrameWriteMask(0x00FF0000u, kC32), static_cast<u8>(kR | kG | kA));
	EXPECT_EQ(gsTileFrameWriteMask(0x0000FF00u, kC32), static_cast<u8>(kR | kB | kA));
	EXPECT_EQ(gsTileFrameWriteMask(0x000000FFu, kC32), static_cast<u8>(kG | kB | kA));
	// Every whole-byte combination, mechanically.
	for (u32 sel = 0; sel < 16; sel++)
	{
		u32 fbmsk = 0;
		for (u32 b = 0; b < 4; b++)
		{
			if (sel & (1u << b))
				fbmsk |= 0xFFu << (b * 8);
		}
		EXPECT_EQ(gsTileFrameWriteMask(fbmsk, kC32), static_cast<u8>(~sel & 0xF)) << "sel " << sel;
		EXPECT_TRUE(gsTileFrameWriteMaskIsExact(fbmsk, kC32)) << "sel " << sel;
	}
}

TEST(TileGpuFbmskMask, PartialChannelWritesTheWholeChannel)
{
	// bge's 1-LSB alpha preservation: byte 3 = 0x01. The channel lands whole.
	EXPECT_EQ(gsTileFrameWriteMask(0x01000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0x01000000u, kC32));
	// The same, over an alpha-only draw: RGB preserved exactly, alpha approximated.
	EXPECT_EQ(gsTileFrameWriteMask(0x01FFFFFFu, kC32), kA);
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0x01FFFFFFu, kC32));
	// Xenosaga keeps the alpha MSB; OutRun keeps two alpha bits.
	EXPECT_EQ(gsTileFrameWriteMask(0x80000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_EQ(gsTileFrameWriteMask(0xBFFFFFFFu, kC32), kA);
	EXPECT_EQ(gsTileFrameWriteMask(0xDFFFFFFFu, kC32), kA);
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0xBFFFFFFFu, kC32));
	// And on a 32-bit frame a dither-preserving low-bit mask writes everything.
	EXPECT_EQ(gsTileFrameWriteMask(0x00F8F8F8u, kC32), kGSTileChannelsRGBA);
	EXPECT_EQ(gsTileFrameWriteMask(0x0000000Fu, kC32), kGSTileChannelsRGBA);
}

// The one place the two parent derivations disagreed, and the reason this rule is written
// against the format's stored bits rather than against the raw byte.
TEST(TileGpuFbmskMask, SixteenBitChannelsAreDecidedByTheirFiveStoredBits)
{
	// PSMCT16 keeps five bits per colour channel. A mask covering exactly those bits
	// preserves the channel as far as guest memory is concerned, so the channel must be
	// DROPPED -- a byte-literal rule would see 0xF8 != 0xFF, write red, and put red back
	// over bits the GS keeps.
	EXPECT_EQ(gsTileFrameWriteMask(0x000000F8u, kC16), static_cast<u8>(kG | kB | kA));
	EXPECT_TRUE(gsTileFrameWriteMaskIsExact(0x000000F8u, kC16));
	EXPECT_EQ(gsTileFrameWriteMask(0x00F8F8F8u, kC16), kA);
	EXPECT_TRUE(gsTileFrameWriteMaskIsExact(0x00F8F8F8u, kC16));
	// The single stored alpha bit is the same rule.
	EXPECT_EQ(gsTileFrameWriteMask(0x80000000u, kC16), kGSTileChannelsRGB);
	EXPECT_TRUE(gsTileFrameWriteMaskIsExact(0x80000000u, kC16));
	// Masking bits the format does not store changes nothing at all.
	EXPECT_EQ(gsTileFrameWriteMask(0x00000007u, kC16), kGSTileChannelsRGBA);
	EXPECT_TRUE(gsTileFrameWriteMaskIsExact(0x00000007u, kC16));
	// A mask that covers part of a stored channel is genuinely inexact, and writes.
	EXPECT_EQ(gsTileFrameWriteMask(0x00000030u, kC16), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0x00000030u, kC16));
	// The corpus's one 16-bit oddity, OutRun's 0x00003FFF: red whole, green partial.
	EXPECT_EQ(gsTileFrameWriteMask(0x00003FFFu, kC16), static_cast<u8>(kG | kB | kA));
	EXPECT_FALSE(gsTileFrameWriteMaskIsExact(0x00003FFFu, kC16));
	// The same masks over a 32-bit frame write, because there the low bits are real.
	EXPECT_EQ(gsTileFrameWriteMask(0x80F8F8F8u, kC32), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWritesNothing(0x80F8F8F8u, kC32));
}

TEST(TileGpuFbmskMask, TheFormatOwnsTheAllOrNothingEdge)
{
	// PSMCT24 under an RGB mask keeps every bit it stores: the draw lands nothing, even
	// though byte 3 reads as writable. This is the predicate the renderer used before
	// per-channel masks existed, and it must survive them unchanged -- otherwise a draw the
	// GS drops entirely would start writing the target's alpha.
	EXPECT_TRUE(gsTileFrameWritesNothing(0x00FFFFFFu, kC24));
	EXPECT_EQ(gsTileFrameWriteMask(0x00FFFFFFu, kC24), 0);
	// PSMZ24 is the same shape.
	EXPECT_EQ(gsTileFrameWriteMask(0xAAFFFFFFu, kC24), 0);
	// Masking a byte the format has no bits in is a no-op, and cannot drop the channel.
	EXPECT_EQ(gsTileFrameWriteMask(0xFF000000u, kC24), kGSTileChannelsRGBA);
	// A 16-bit frame whose stored bits are all masked, with the unstored ones left clear.
	EXPECT_TRUE(gsTileFrameWritesNothing(0x80F8F8F8u, kC16));
	EXPECT_EQ(gsTileFrameWriteMask(0x80F8F8F8u, kC16), 0);
}

TEST(TileGpuFbmskMask, AgreesWithTheOldAllOrNothingPredicateOnEveryMask)
{
	// The pre-existing route asked one question: does FBMSK keep every stored bit? Whatever
	// the channel mask does inside a colour-writing draw, it must still answer that question
	// the same way, or draws appear and disappear.
	for (u32 fmsk : {kC32, kC24, kC16})
	{
		for (u32 sel = 0; sel < 256; sel++)
		{
			u32 fbmsk = 0;
			for (u32 b = 0; b < 4; b++)
			{
				if (sel & (1u << b))
					fbmsk |= 0xFFu << (b * 8);
				else if (sel & (1u << (b + 4)))
					fbmsk |= 0x0Fu << (b * 8); // a partial byte in the same sweep
			}
			const bool old_color_written = (fbmsk & fmsk) != fmsk;
			EXPECT_EQ(gsTileFrameWriteMask(fbmsk, fmsk) != 0, old_color_written)
				<< "fmsk " << std::hex << fmsk << " fbmsk " << fbmsk;
		}
	}
}

// ---------------------------------------------------------------------------------------
// The deferred alpha half, and the AFAIL fold it composes with.
// ---------------------------------------------------------------------------------------

// ⚠️ The carve-out. FBMSK's alpha byte is NOT honoured: nothing seeds a TileGpu target's
// alpha independently of its colour, so preserving it exposes stale pool bytes to a later
// TCC=1 read (Ace Combat 5, 248 such draws a frame, 0.021 -> 0.080 mean-abs when it is
// honoured). The day the seeding lands, THIS is the test that says the carve-out can go.
TEST(TileGpuFbmskMask, AlphaHalfOfFbmskIsDeferred)
{
	// The honest derivation knows the alpha byte is masked...
	EXPECT_EQ(gsTileFrameWriteMask(0xFF000000u, kC32), kGSTileChannelsRGB);
	// ...and the composed policy writes it anyway.
	EXPECT_EQ(Mask(0xFF000000u, kC32), kGSTileChannelsRGBA);
	// A partial alpha mask is the same answer, from the other direction.
	EXPECT_EQ(Mask(0x80000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_EQ(Mask(0x7F000000u, kC32), kGSTileChannelsRGBA);
	// A 16-bit frame's single stored alpha bit, likewise.
	EXPECT_EQ(gsTileFrameWriteMask(0x80000000u, kC16), kGSTileChannelsRGB);
	EXPECT_EQ(Mask(0x80000000u, kC16), kGSTileChannelsRGBA);
	// The deferral never resurrects a draw that lands nothing.
	EXPECT_EQ(Mask(0xFFFFFFFFu, kC32), 0);
	EXPECT_EQ(Mask(0x00FFFFFFu, kC24), 0);
	// Nor does it undo the AFAIL fold, which is the one thing that still removes alpha.
	EXPECT_EQ(Mask(0xFF000000u, kC32, true, AFAIL_RGB_ONLY), kGSTileChannelsRGB);
}

// One colour channel at a time; alpha rides along with each of them (the deferred half).
TEST(TileGpuFbmskMask, OneChannelAtATime)
{
	EXPECT_EQ(Mask(0xFFFFFF00u, kC32), static_cast<u8>(kR | kA));
	EXPECT_EQ(Mask(0xFFFF00FFu, kC32), static_cast<u8>(kG | kA));
	EXPECT_EQ(Mask(0xFF00FFFFu, kC32), static_cast<u8>(kB | kA));
	// The world-erasure draws: alpha alone, and this one FBMSK really does mask RGB.
	EXPECT_EQ(Mask(0x00FFFFFFu, kC32), kA);
}

// ATST=NEVER never reaches the fragment stage: every pixel fails, so AFAIL alone says what
// a failing pixel still lands.
TEST(TileGpuFbmskMask, AlphaTestNeverFoldsIntoTheMask)
{
	EXPECT_EQ(Mask(0x00000000u, kC32, true, AFAIL_KEEP), 0);
	EXPECT_EQ(Mask(0x00000000u, kC32, true, AFAIL_ZB_ONLY), 0);
	EXPECT_EQ(Mask(0x00000000u, kC32, true, AFAIL_FB_ONLY), kGSTileChannelsRGBA);
	EXPECT_EQ(Mask(0x00000000u, kC32, true, AFAIL_RGB_ONLY), kGSTileChannelsRGB);
	// With the test not selected, AFAIL says nothing at all.
	EXPECT_EQ(Mask(0x00000000u, kC32, false, AFAIL_KEEP), kGSTileChannelsRGBA);
	EXPECT_EQ(Mask(0x00000000u, kC32, false, AFAIL_ZB_ONLY), kGSTileChannelsRGBA);
}

// The two derivations compose: neither may override the other. OutRun's alpha-only draws
// carry ATST=NEVER + AFAIL=FB_ONLY, which is "write the frame buffer" -- and FBMSK then
// says which part of it. Reading only one of the two is exactly what erased the world.
TEST(TileGpuFbmskMask, RegisterMaskAndAfailCompose)
{
	EXPECT_EQ(Mask(0x00FFFFFFu, kC32, true, AFAIL_FB_ONLY), kA);
	EXPECT_EQ(Mask(0xFF000000u, kC32, true, AFAIL_FB_ONLY), kGSTileChannelsRGBA); // alpha half deferred
	// RGB_ONLY drops alpha on top of whatever FBMSK left.
	EXPECT_EQ(Mask(0xFF000000u, kC32, true, AFAIL_RGB_ONLY), kGSTileChannelsRGB);
	EXPECT_EQ(Mask(0x00FF0000u, kC32, true, AFAIL_RGB_ONLY), static_cast<u8>(kR | kG));
	// An alpha-only draw whose failing pixels write RGB only lands nothing at all.
	EXPECT_EQ(Mask(0x00FFFFFFu, kC32, true, AFAIL_RGB_ONLY), 0);
	// KEEP and ZB_ONLY write no colour whatever FBMSK says.
	EXPECT_EQ(Mask(0x00FFFFFFu, kC32, true, AFAIL_KEEP), 0);
	EXPECT_EQ(Mask(0xFF000000u, kC32, true, AFAIL_ZB_ONLY), 0);
}

// The invariant every call site rests on: "this draw writes colour" is exactly "some
// channel is in the mask", so nobody has to ask FBMSK a second, differently-phrased
// question. This is the pre-existing predicate, and it must not move.
TEST(TileGpuFbmskMask, EmptyMaskIsExactlyWritesNoColour)
{
	for (u32 fmsk : {kC32, kC24, kC16})
	{
		for (u32 afail = 0; afail < 4; afail++)
		{
			for (bool never : {false, true})
			{
				// A mask covering every writable bit is the only FBMSK that can write nothing
				// on its own.
				EXPECT_EQ(Mask(fmsk, fmsk, never, afail), 0);
				const u8 m = Mask(0, fmsk, never, afail);
				const bool writes = !(never && (afail == AFAIL_KEEP || afail == AFAIL_ZB_ONLY));
				EXPECT_EQ(m != 0, writes) << "fmsk " << std::hex << fmsk << " afail " << afail << " never " << never;
			}
		}
	}
}

// ---------------------------------------------------------------------------------------
// The seed skip's gate.
// ---------------------------------------------------------------------------------------

TEST(TileGpuFbmskMask, SeedSkipNeedsATotalWriteNotAFullChannelMask)
{
	// Only a mask that keeps nothing lets a sprite skip the seed.
	EXPECT_TRUE(gsTileFrameWriteIsTotal(0x00000000u, kC32));
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0x00FFFFFFu, kC32));
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0xFF000000u, kC32));

	// The trap the gate exists for: a mask that preserves bits INSIDE a channel still
	// reports every channel as written, so a channel-mask test would wrongly skip the seed
	// and the preserved bits would come back as whatever the texture happened to hold.
	EXPECT_EQ(gsTileFrameWriteMask(0x01000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0x01000000u, kC32));
	EXPECT_EQ(gsTileFrameWriteMask(0x80000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0x80000000u, kC32));

	// And the deferred alpha half makes the trap worse, not better: the composed mask is
	// RGBA for every FBMSK that touches alpha alone, total write or not.
	EXPECT_EQ(Mask(0xFF000000u, kC32), kGSTileChannelsRGBA);
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0xFF000000u, kC32));

	// A 16-bit frame masking only bits it does not store overwrites its cells in full, so it
	// keeps the skip -- the gate asks about STORED bits, which is the pre-existing predicate.
	EXPECT_TRUE(gsTileFrameWriteIsTotal(0x00000007u, kC16));
	EXPECT_FALSE(gsTileFrameWriteIsTotal(0x00000008u, kC16));
}

TEST(TileGpuFbmskMask, TotalWriteImpliesEveryStoredChannelAndExactness)
{
	// A total write leaves nothing to preserve, so every channel the format actually stores
	// must come back as written and the derivation must be exact. (A channel with no stored
	// bits -- PSMCT24's alpha -- is exempt: nothing in guest memory depends on it, and
	// writeback masks that byte away.)
	for (u32 fmsk : {kC32, kC24, kC16})
	{
		for (u32 sel = 0; sel < 4096; sel++)
		{
			// Spread the sweep over whole bytes, single bits, and the format's own bit
			// patterns, so "total" is reached by more than just fbmsk == 0.
			const u32 fbmsk = ((sel & 0xF) * 0x11111111u) & ((sel >> 4) * 0x00010101u | 0xFF000000u);
			if (!gsTileFrameWriteIsTotal(fbmsk, fmsk))
				continue;
			const u8 m = gsTileFrameWriteMask(fbmsk, fmsk);
			EXPECT_EQ(m, kGSTileChannelsRGBA) << std::hex << "fmsk " << fmsk << " fbmsk " << fbmsk;
			EXPECT_TRUE(gsTileFrameWriteMaskIsExact(fbmsk, fmsk)) << std::hex << fbmsk;
		}
	}
}

// ---------------------------------------------------------------------------------------
// The device contract: the mask survives the trip through the blend key.
// ---------------------------------------------------------------------------------------

TEST(TileGpuFbmskMask, BlendKeyCarriesTheMaskInThePreserveSense)
{
	using Plan = GSDevice::GSTileGpuPassPlan;
	for (u32 m = 0; m < 16; m++)
	{
		const u32 key = Plan::PackNoWrite(m);
		EXPECT_EQ(key & ~Plan::kNoWriteMask, 0u) << "mask " << m << " spilled out of its field";
		const u32 round_trip = (~(key >> Plan::kNoWriteShift)) & 0xFu;
		EXPECT_EQ(round_trip, m);
	}
	// Zero key = write all four, which is what a plan carrying no blend keys falls back to.
	EXPECT_EQ((~(0u >> Plan::kNoWriteShift)) & 0xFu, 0xFu);
	// The field is disjoint from everything else the key carries: the blend index (bits 0-6),
	// ALPHA.FIX (8-15) and the enable bit (31).
	EXPECT_EQ(Plan::kNoWriteMask & Plan::kBlendEnable, 0u);
	EXPECT_EQ(Plan::kNoWriteMask & 0x7Fu, 0u);
	EXPECT_EQ(Plan::kNoWriteMask & 0xFF00u, 0u);
}
