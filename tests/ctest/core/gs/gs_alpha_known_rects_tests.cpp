// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the per-rectangle alpha known-bits rules (GS/Renderers/HW/GSAlphaKnownRects.h).
//
// The shape these exist for: a clear that writes constant alpha over an offset rectangle of a
// target whose valid rect is larger (letterbox rows, a stray column), followed by many draws that
// hold back one alpha bit. The whole-target pair never learns the bit, because the clear does not
// cover the valid rect; the rectangle does, and the masked draws inside it can drop the mask.
//
// Rides gs_vertex_tests -- the rules are header-only, so they need no extra linkage.

#include "GS/Renderers/HW/GSAlphaKnownRects.h"
#include "GS/Renderers/HW/GSDrawAlphaMask.h"

#include <gtest/gtest.h>

using GSAlphaKnownBits::Known;
using GSAlphaKnownRects::Key;
using GSAlphaKnownRects::KnownForDraw;
using GSAlphaKnownRects::Set;
using GSAlphaKnownRects::Write;

namespace
{
	constexpr Key kKey{1, 0x1234};
	const GSVector4i kTexture(0, 0, 640, 448);
	const GSVector4i kFrame(0, 32, 640, 415);

	// A gapless constant-alpha write of every alpha bit.
	Write Cover(const GSVector4i& r, u8 alpha)
	{
		return {r, r, 0xFF, alpha, alpha, true};
	}

	// A write that may leave pixels of its rectangle alone.
	Write Partial(const GSVector4i& r, u8 written, u8 lo, u8 hi)
	{
		return {r, r, written, lo, hi, false};
	}

	Set Cleared()
	{
		Set s;
		s.AfterWrite(kKey, Cover(kFrame, 0x80), kTexture);
		return s;
	}

	// The exact rule's verdict on a draw over `r` holding back bit 0, with the whole-target pair
	// knowing nothing.
	GSDrawAlphaMask::ExactVerdict VerdictFor(const Set& s, const GSVector4i& r, u8 src)
	{
		const auto k = KnownForDraw(Known::Nothing(), s, kKey, r, 0x01);
		return GSDrawAlphaMask::DecideExact(k.known, 0x01, src, src);
	}
} // namespace

TEST(GSAlphaKnownRects, FullCoverOfAnOffsetRectEstablishesKnowledge)
{
	const Set s = Cleared();
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x80));
	EXPECT_EQ(s.Inside(kKey, GSVector4i(100, 100, 200, 200)), Known::All(0x80));
}

TEST(GSAlphaKnownRects, AWriteWithGapsEstablishesNothing)
{
	Set s;
	s.AfterWrite(kKey, Partial(kFrame, 0xFF, 0x80, 0x80), kTexture);
	EXPECT_EQ(s.Size(), 0u);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::Nothing());
}

TEST(GSAlphaKnownRects, AWriteThatMissesTheRectKeepsIt)
{
	Set s = Cleared();
	// Below the frame (BGE clears its depth through the colour pointer there), and to its side.
	s.AfterWrite(kKey, Partial(GSVector4i(0, 416, 640, 448), 0xFF, 0x00, 0xFF), kTexture);
	s.AfterWrite(kKey, Partial(GSVector4i(0, 0, 640, 32), 0xFF, 0x00, 0xFF), kTexture);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x80));
}

TEST(GSAlphaKnownRects, AMaskedWriteInsideKeepsTheHeldBits)
{
	Set s = Cleared();
	// The scene: varying alpha, bit 0 held back.
	s.AfterWrite(kKey, Partial(GSVector4i(10, 40, 300, 200), 0xFE, 0x00, 0xFF), kTexture);
	EXPECT_EQ(s.Inside(kKey, kFrame), (Known{0x01, 0x00}));
}

TEST(GSAlphaKnownRects, APartialOverlapErodesTheBitsItDisagreesOn)
{
	Set s = Cleared();
	// Alpha 0 over a strip that crosses the frame's edge: bit 7 differs, the rest agree.
	s.AfterWrite(kKey, Partial(GSVector4i(0, 0, 640, 60), 0xFF, 0x00, 0x00), kTexture);
	EXPECT_EQ(s.Inside(kKey, kFrame), (Known{0x7F, 0x00}));

	// A varying alpha over part of it leaves nothing.
	s.AfterWrite(kKey, Partial(GSVector4i(0, 0, 100, 100), 0xFF, 0x00, 0xFF), kTexture);
	EXPECT_EQ(s.Size(), 0u);
}

TEST(GSAlphaKnownRects, ACoverOfTheWholeRectRewritesIt)
{
	Set s = Cleared();
	s.AfterWrite(kKey, Cover(kFrame, 0x00), kTexture);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x00));

	// Covering it with a varying alpha but bit 0 held keeps bit 0.
	s.AfterWrite(kKey, Write{GSVector4i(0, 0, 640, 448), GSVector4i(0, 0, 640, 448), 0xFE, 0x00, 0xFF, true}, kTexture);
	EXPECT_EQ(s.Inside(kKey, kFrame), (Known{0x01, 0x00}));
}

TEST(GSAlphaKnownRects, ASmallCoverInsideStartsNoRectOfItsOwn)
{
	Set s = Cleared();
	// Letterbox bars: alpha 0, inside the frame. The frame keeps every bit but 7.
	s.AfterWrite(kKey, Cover(GSVector4i(0, 32, 640, 60), 0x00), kTexture);
	s.AfterWrite(kKey, Cover(GSVector4i(0, 387, 640, 415), 0x00), kTexture);
	EXPECT_EQ(s.Size(), 1u);
	EXPECT_EQ(s.Inside(kKey, GSVector4i(0, 100, 640, 300)), (Known{0x7F, 0x00}));
}

TEST(GSAlphaKnownRects, TwoRectsAndTheOldestIsEvicted)
{
	Set s = Cleared();
	const GSVector4i below(0, 416, 640, 448);
	s.AfterWrite(kKey, Cover(below, 0x00), kTexture);
	EXPECT_EQ(s.Size(), 2u);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x80));
	EXPECT_EQ(s.Inside(kKey, below), Known::All(0x00));

	// Renewing the frame makes the other one the oldest.
	s.AfterWrite(kKey, Cover(kFrame, 0x80), kTexture);
	s.AfterWrite(kKey, Cover(GSVector4i(0, 0, 640, 16), 0x40), kTexture);
	EXPECT_EQ(s.Size(), 2u);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x80));
	EXPECT_EQ(s.Inside(kKey, below), Known::Nothing());
}

TEST(GSAlphaKnownRects, ADrawOutsideTheKnownRectGetsNoDrop)
{
	const Set s = Cleared();
	EXPECT_EQ(VerdictFor(s, GSVector4i(100, 100, 200, 200), 0x80), GSDrawAlphaMask::ExactVerdict::Drop);
	EXPECT_EQ(VerdictFor(s, GSVector4i(100, 100, 200, 200), 0x81), GSDrawAlphaMask::ExactVerdict::Substitute);
	// Straddling the edge, or wholly outside.
	EXPECT_EQ(VerdictFor(s, GSVector4i(100, 20, 200, 200), 0x80), GSDrawAlphaMask::ExactVerdict::TargetUnknown);
	EXPECT_EQ(VerdictFor(s, GSVector4i(100, 0, 200, 20), 0x80), GSDrawAlphaMask::ExactVerdict::TargetUnknown);
}

TEST(GSAlphaKnownRects, TheWholeTargetPairAnswersAloneWhenItKnows)
{
	// The rectangles never override a pair that knows the masked bits, even where they disagree.
	const Set s = Cleared();
	const auto k = KnownForDraw(Known{0x01, 0x01}, s, kKey, kFrame, 0x01);
	EXPECT_FALSE(k.from_rects);
	EXPECT_EQ(k.known, (Known{0x01, 0x01}));

	// Where it does not, the two combine, and a bit they disagree on is unknown.
	const auto c = KnownForDraw(Known{0x81, 0x01}, s, kKey, kFrame, 0x02);
	EXPECT_TRUE(c.from_rects);
	EXPECT_EQ(c.known, (Known{0x7E, 0x00}));
}

TEST(GSAlphaKnownRects, UploadsErode)
{
	Set s = Cleared();
	s.AfterUpload(GSVector4i(0, 0, 64, 64), 0x80, 0x80);
	EXPECT_EQ(s.Inside(kKey, kFrame), Known::All(0x80));
	s.AfterUpload(GSVector4i(0, 0, 64, 64), 0x00, 0x00);
	EXPECT_EQ(s.Inside(kKey, kFrame), (Known{0x7F, 0x00}));
	s.AfterUpload(GSVector4i(0, 400, 64, 448), 0x00, 0xFF);
	EXPECT_EQ(s.Size(), 0u);
	// One that misses changes nothing.
	Set t = Cleared();
	t.AfterUpload(GSVector4i(0, 0, 64, 32), 0x00, 0xFF);
	EXPECT_EQ(t.Inside(kKey, kFrame), Known::All(0x80));
}

TEST(GSAlphaKnownRects, ANewTextureOrLayoutForgets)
{
	const Set s = Cleared();
	EXPECT_EQ(s.Inside(Key{2, kKey.layout}, kFrame), Known::Nothing());
	EXPECT_EQ(s.Inside(Key{kKey.texture, 0x4321}, kFrame), Known::Nothing());

	// A write under the new key starts from nothing.
	Set t = Cleared();
	t.AfterWrite(Key{2, kKey.layout}, Partial(GSVector4i(0, 0, 8, 8), 0xFF, 0x00, 0x00), kTexture);
	EXPECT_EQ(t.Size(), 0u);
	EXPECT_EQ(t.Inside(kKey, kFrame), Known::Nothing());
}

TEST(GSAlphaKnownRects, AMoveInPlaceKeepsWhatWasCopied)
{
	Set s = Cleared();
	const Key moved{2, kKey.layout};
	s.Moved(kKey, moved, GSVector4i(0, 0, 640, 300));
	EXPECT_EQ(s.Inside(kKey, GSVector4i(0, 32, 640, 300)), Known::Nothing());
	EXPECT_EQ(s.Inside(moved, GSVector4i(0, 32, 640, 300)), Known::All(0x80));
	EXPECT_EQ(s.Inside(moved, kFrame), Known::Nothing());

	// From a key the set does not hold, nothing is carried.
	Set t = Cleared();
	t.Moved(Key{9, 9}, moved, kTexture);
	EXPECT_EQ(t.Size(), 0u);
}

TEST(GSAlphaKnownRects, NoClaimOutsideTheTexture)
{
	Set s;
	s.AfterWrite(kKey, Cover(GSVector4i(0, 416, 640, 800), 0x00), kTexture);
	EXPECT_EQ(s.Inside(kKey, GSVector4i(0, 416, 640, 448)), Known::All(0x00));
	EXPECT_EQ(s.Inside(kKey, GSVector4i(0, 416, 640, 500)), Known::Nothing());
}

TEST(GSAlphaKnownRects, FormatKeepsOnlyTheBitsItStores)
{
	Set s = Cleared();
	s.KeepBits(0x80);
	EXPECT_EQ(s.Inside(kKey, kFrame), (Known{0x80, 0x80}));
	s.KeepBits(0x00);
	EXPECT_EQ(s.Size(), 0u);
}

TEST(GSAlphaKnownRects, AWriteReachingPastItsRectErodesTheNeighbour)
{
	Set s;
	const GSVector4i right(320, 32, 640, 415);
	s.AfterWrite(kKey, Cover(right, 0x80), kTexture);
	const GSVector4i left(0, 32, 320, 415);
	s.AfterWrite(kKey, Write{left, GSAlphaKnownRects::Reach(left, true), 0xFF, 0x81, 0x81, false}, kTexture);
	EXPECT_EQ(s.Inside(kKey, right), (Known{0xFE, 0x80}));

	Set t;
	t.AfterWrite(kKey, Cover(right, 0x80), kTexture);
	t.AfterWrite(kKey, Write{left, GSAlphaKnownRects::Reach(left, false), 0xFF, 0x81, 0x81, false}, kTexture);
	EXPECT_EQ(t.Inside(kKey, right), Known::All(0x80));
}

TEST(GSAlphaKnownRects, ReachGrowsOnlyUpscaled)
{
	EXPECT_TRUE(GSAlphaKnownRects::Reach(kFrame, false).eq(kFrame));
	EXPECT_TRUE(GSAlphaKnownRects::Reach(kFrame, true).eq(GSVector4i(-1, 31, 641, 416)));
}

namespace
{
	GSVector4i Fixed(int x0, int y0, int x1, int y1)
	{
		return GSVector4i(x0 * 16, y0 * 16, x1 * 16, y1 * 16);
	}
} // namespace

TEST(GSAlphaKnownRects, SpritesWriteEveryPixelOnlyOnWholePixelsWithNoHole)
{
	using GSAlphaKnownRects::SpritesWriteEveryPixel;

	// One sprite, at an offset.
	const GSVector4i clear[] = {Fixed(0, 32, 640, 415)};
	EXPECT_TRUE(SpritesWriteEveryPixel(clear, 1, kFrame));

	// Two coincident sprites, and two that tile.
	const GSVector4i twice[] = {Fixed(0, 32, 640, 415), Fixed(0, 32, 640, 415)};
	EXPECT_TRUE(SpritesWriteEveryPixel(twice, 2, kFrame));
	const GSVector4i halves[] = {Fixed(0, 32, 640, 200), Fixed(0, 200, 640, 415)};
	EXPECT_TRUE(SpritesWriteEveryPixel(halves, 2, kFrame));

	// The second sprite covers 8 columns of its band.
	const GSVector4i hole[] = {Fixed(0, 240, 640, 250), Fixed(0, 250, 8, 260)};
	EXPECT_FALSE(SpritesWriteEveryPixel(hole, 2, GSVector4i(0, 240, 640, 260)));

	// A far edge at 300.25: rounding decides whether row 300 is written.
	const GSVector4i frac[] = {GSVector4i(0, 32 * 16, 640 * 16, 300 * 16 + 4)};
	EXPECT_FALSE(SpritesWriteEveryPixel(frac, 1, GSVector4i(0, 32, 640, 301)));
	EXPECT_FALSE(SpritesWriteEveryPixel(frac, 1, GSVector4i(0, 32, 640, 300)));

	// A rect wider than the sprites.
	EXPECT_FALSE(SpritesWriteEveryPixel(clear, 1, GSVector4i(0, 32, 641, 415)));
}
