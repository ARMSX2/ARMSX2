// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pins the field-shift classification used when a field render is presented directly at an integer
// upscale of 2 or more (GSFieldShiftPolicy.h). The question the classifier answers is whether a
// game draws the identical picture on both fields or moves its projection half a display line
// between them, because that decides whether the merge keeps its FFMD offset.
//
// The measurement half is exercised on synthetic field pairs -- identical, shifted by one native
// line, and unrelated noise -- with and without a merge offset already applied, since in the
// emulator the classifier always runs while the default (shift) offset is in force.

#include "GS/Renderers/Common/GSFieldShiftPolicy.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

namespace
{
	constexpr int kCols = 32;
	constexpr int kRows = 224;
	constexpr int kStep = 2; // one native line at 2x

	// A picture with real vertical structure: every row differs from its neighbours, which is what
	// makes a one-line displacement measurable at all.
	std::vector<u8> MakePicture(u32 seed)
	{
		std::mt19937 rng(seed);
		std::vector<u8> out(static_cast<size_t>(kCols) * kRows);
		for (int r = 0; r < kRows; r++)
		{
			for (int c = 0; c < kCols; c++)
				out[static_cast<size_t>(r) * kCols + c] = static_cast<u8>(rng() & 0xFF);
		}
		return out;
	}

	/// Same picture, moved down by `rows` device rows. Rows the move exposes repeat the edge, which
	/// is what the merge's clamp does.
	std::vector<u8> ShiftDown(const std::vector<u8>& src, int rows)
	{
		std::vector<u8> out(src.size());
		for (int r = 0; r < kRows; r++)
		{
			int s = r - rows;
			s = (s < 0) ? 0 : ((s >= kRows) ? kRows - 1 : s);
			for (int c = 0; c < kCols; c++)
				out[static_cast<size_t>(r) * kCols + c] = src[static_cast<size_t>(s) * kCols + c];
		}
		return out;
	}

	/// A picture with no vertical structure at all: every row identical. A vertical displacement
	/// cannot change it, so all three alignments must score the same.
	std::vector<u8> MakeRowlessPicture(u8 base)
	{
		std::vector<u8> out(static_cast<size_t>(kCols) * kRows);
		for (int r = 0; r < kRows; r++)
		{
			for (int c = 0; c < kCols; c++)
				out[static_cast<size_t>(r) * kCols + c] = static_cast<u8>(base + c * 3);
		}
		return out;
	}

	GSFieldShiftVote VoteFor(const std::vector<u8>& cur, const std::vector<u8>& prev, int applied_delta)
	{
		return GSClassifyFieldShiftPair(
			GSMeasureFieldShift(cur.data(), prev.data(), kCols, kRows, kStep, applied_delta));
	}
} // namespace

TEST(GSFieldShiftPolicy, IdenticalFieldsVoteNoShift)
{
	const std::vector<u8> picture = MakePicture(1);
	EXPECT_EQ(VoteFor(picture, picture, 0), GSFieldShiftVote::NoShift);
}

TEST(GSFieldShiftPolicy, IdenticalFieldsAreDecisive)
{
	// The margin is what lets a no-shift game correct itself on the first pair instead of waiting
	// out the whole probe budget.
	const std::vector<u8> picture = MakePicture(2);
	const GSFieldShiftSample sample =
		GSMeasureFieldShift(picture.data(), picture.data(), kCols, kRows, kStep, 0);
	EXPECT_GE(GSFieldShiftMargin(sample), GS_FIELD_SHIFT_DECISIVE_MARGIN);
}

TEST(GSFieldShiftPolicy, FieldsOneNativeLineApartVoteShift)
{
	const std::vector<u8> prev = MakePicture(3);
	EXPECT_EQ(VoteFor(ShiftDown(prev, kStep), prev, 0), GSFieldShiftVote::Shift);
	EXPECT_EQ(VoteFor(ShiftDown(prev, -kStep), prev, 0), GSFieldShiftVote::Shift);
}

TEST(GSFieldShiftPolicy, PictureWithNoVerticalStructureIsUninformative)
{
	// Nothing a vertical displacement does can change this frame, so no alignment can win and the
	// pair must not be counted. This is the shape of a horizontal gradient, a flat sky, a fade.
	EXPECT_EQ(VoteFor(MakeRowlessPicture(20), MakeRowlessPicture(90), 0), GSFieldShiftVote::Uninformative);
}

TEST(GSFieldShiftPolicy, UnrelatedPicturesNeverVoteNoShift)
{
	// A scene change lands two unrelated frames next to each other: every alignment scores about
	// the same, and the dangerous outcome would be calling that "the fields are the same picture".
	for (u32 seed = 100; seed < 120; seed++)
		EXPECT_NE(VoteFor(MakePicture(seed), MakePicture(seed + 1000), 0), GSFieldShiftVote::NoShift);
}

TEST(GSFieldShiftPolicy, FlatFieldsAreUninformative)
{
	const std::vector<u8> black(static_cast<size_t>(kCols) * kRows, 0);
	EXPECT_EQ(VoteFor(black, black, 0), GSFieldShiftVote::Uninformative);
	EXPECT_EQ(VoteFor(black, black, kStep), GSFieldShiftVote::Uninformative);
}

TEST(GSFieldShiftPolicy, AppliedMergeOffsetIsTakenBackOut)
{
	// What the emulator actually sees while the default is in force: the merge already moved the
	// current field down one native line. A game that shifts is then aligned row-for-row, and a
	// game that does not is one line out -- and the classifier must report the game's own
	// behaviour, not the merge's.
	const std::vector<u8> prev = MakePicture(6);

	const std::vector<u8> shift_game_cur = ShiftDown(ShiftDown(prev, kStep), kStep);
	EXPECT_EQ(VoteFor(shift_game_cur, prev, kStep), GSFieldShiftVote::Shift);

	const std::vector<u8> still_game_cur = ShiftDown(prev, kStep);
	EXPECT_EQ(VoteFor(still_game_cur, prev, kStep), GSFieldShiftVote::NoShift);
}

TEST(GSFieldShiftPolicy, TwoInstancesOfTheSameFieldAreNotComparable)
{
	// The reason the rule exists: on a still screen the same field drawn twice is bit-identical,
	// which is exactly the shape of a game that draws the same picture on both fields. A dump
	// replay does not always alternate, so a shift title gets called a still one without this.
	const std::vector<u8> picture = MakePicture(8);
	EXPECT_EQ(VoteFor(picture, picture, 0), GSFieldShiftVote::NoShift);

	EXPECT_FALSE(GSFieldShiftPairIsComparable(0, 0));
	EXPECT_FALSE(GSFieldShiftPairIsComparable(1, 1));
	EXPECT_TRUE(GSFieldShiftPairIsComparable(0, 1));
	EXPECT_TRUE(GSFieldShiftPairIsComparable(1, 0));
}

TEST(GSFieldShiftPolicy, TallyDefaultsToShiftUntilNoShiftIsEarned)
{
	EXPECT_FALSE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{}));
	EXPECT_FALSE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{0, 2}));  // too few votes
	EXPECT_FALSE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{2, 3}));  // not three quarters
	EXPECT_FALSE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{8, 0}));
	EXPECT_TRUE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{0, 3}));
	EXPECT_TRUE(GSFieldShiftTallySaysNoShift(GSFieldShiftTally{1, 6}));
}

TEST(GSFieldShiftPolicy, BothShiftDirectionsAreRecognised)
{
	// Up and down are the same claim -- the game moved between fields -- and a shift game
	// alternates between them every field, so neither may be missed.
	const std::vector<u8> prev = MakePicture(7);

	const GSFieldShiftSample down = GSMeasureFieldShift(
		ShiftDown(prev, kStep).data(), prev.data(), kCols, kRows, kStep, 0);
	EXPECT_FLOAT_EQ(down.mad_down, 0.0f);
	EXPECT_GT(down.mad_aligned, 1.0f);

	const GSFieldShiftSample up = GSMeasureFieldShift(
		ShiftDown(prev, -kStep).data(), prev.data(), kCols, kRows, kStep, 0);
	EXPECT_FLOAT_EQ(up.mad_up, 0.0f);
	EXPECT_GT(up.mad_aligned, 1.0f);
}

// -------------------------------------------------------------------------------------------
// The top band. The shift moves what the merge READS, so the rows it exposes at the top of a
// circuit's rect are filled by the sampler's clamp at the TEXTURE's edge. That is the rect's own
// first row only when the rect starts at the texture top; otherwise the merge has to draw the
// band itself.

namespace
{
// The geometry the emulator is in at 2x on a 480i field-mode title: a 224-line field render into
// a 1280x896 target, magnified over 894 destination rows, shifted by one native line (2 device
// rows) on the odd field.
constexpr int kTexHeight = 896;
constexpr float kDstTop = 0.0f;
constexpr float kDstBottom = 894.0f;
constexpr float kShiftRows = 2.0f;

// Which texel row a destination row samples, for a nearest-filtered draw of [src_top_v, src_bot_v]
// over [dst_top, dst_bottom).
int SampledTexelRow(float src_top_v, float src_bot_v, float dst_top, float dst_bottom, int dst_row)
{
	const float src_top = src_top_v * static_cast<float>(kTexHeight);
	const float src_bot = src_bot_v * static_cast<float>(kTexHeight);
	const float per_row = (src_bot - src_top) / (dst_bottom - dst_top);
	const float v = src_top + (static_cast<float>(dst_row) + 0.5f - dst_top) * per_row;
	return static_cast<int>(std::floor(v));
}
} // namespace

TEST(GSFieldShiftTopBand, RectAtTheTextureTopNeedsNoBand)
{
	// The whole corpus is here: every field-mode title that shifts has DISPFB.DBY = 0, so the
	// clamp already returns the rect's first row and the common path must not gain a draw.
	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(0, 0.0f, kDstTop, kShiftRows, kTexHeight);
	EXPECT_FALSE(band.enabled);
}

TEST(GSFieldShiftTopBand, NoShiftMeansNoBand)
{
	// The unshifted field reads its own rect and exposes nothing.
	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(1, 2.0f / kTexHeight, kDstTop, 0.0f, kTexHeight);
	EXPECT_FALSE(band.enabled);
}

TEST(GSFieldShiftTopBand, RectBelowTheTextureTopGetsABandOnItsOwnFirstRow)
{
	// DISPFB.DBY = 1 at 2x: the rect starts at texel row 2, so the clamp at texel row 0 would
	// hand back two rows this circuit does not own.
	const float src_top_v = 2.0f / kTexHeight;
	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(1, src_top_v, kDstTop, kShiftRows, kTexHeight);
	ASSERT_TRUE(band.enabled);

	// Every row of the band samples the centre of texel row 2 -- the rect's first row.
	EXPECT_FLOAT_EQ(band.src_v * static_cast<float>(kTexHeight), 2.5f);
	EXPECT_FLOAT_EQ(band.dst_top, kDstTop);
	EXPECT_FLOAT_EQ(band.dst_bottom, kDstTop + kShiftRows);
}

TEST(GSFieldShiftTopBand, TheBandCoversExactlyTheRowsTheShiftExposed)
{
	const float src_top_v = 2.0f / kTexHeight;
	const float src_bot_v = (2.0f + 448.0f) / kTexHeight;
	const float per_dst_row = (src_bot_v - src_top_v) / (kDstBottom - kDstTop);
	const float shift_v = kShiftRows * per_dst_row;

	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(1, src_top_v, kDstTop, kShiftRows, kTexHeight);
	ASSERT_TRUE(band.enabled);

	// A destination row belongs to the band when the shifted main draw would sample above the
	// rect. Below the band the main draw is inside the rect and must be left alone.
	for (int row = 0; row < 8; row++)
	{
		const int sampled = SampledTexelRow(
			src_top_v - shift_v, src_bot_v - shift_v, kDstTop, kDstBottom, row);
		const bool in_band = static_cast<float>(row) + 0.5f < band.dst_bottom;
		EXPECT_EQ(in_band, sampled < 2) << "destination row " << row;
	}
}

TEST(GSFieldShiftTopBand, TheBandRepeatsTheRowTheShiftedDrawShowsBelowIt)
{
	// The band and the shifted main draw abut. If they disagreed about which texel row sits at
	// the seam the fix would trade a wrong band for a visible step.
	const float src_top_v = 2.0f / kTexHeight;
	const float src_bot_v = (2.0f + 448.0f) / kTexHeight;
	const float per_dst_row = (src_bot_v - src_top_v) / (kDstBottom - kDstTop);
	const float shift_v = kShiftRows * per_dst_row;

	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(1, src_top_v, kDstTop, kShiftRows, kTexHeight);
	ASSERT_TRUE(band.enabled);

	const int band_row = static_cast<int>(std::floor(band.src_v * static_cast<float>(kTexHeight)));
	const int first_row_below = static_cast<int>(std::ceil(band.dst_bottom - 0.5f));
	EXPECT_EQ(band_row, SampledTexelRow(src_top_v - shift_v, src_bot_v - shift_v, kDstTop,
							 kDstBottom, first_row_below));
}

TEST(GSFieldShiftTopBand, TheBandTracksTheRectWhereverItSits)
{
	// The point of the change: the fill follows the rect, not the texture.
	for (const int dby : {1, 2, 7, 64})
	{
		const float src_top_v = static_cast<float>(dby * 2) / kTexHeight;
		const GSFieldShiftTopBand band =
			GSComputeFieldShiftTopBand(dby, src_top_v, kDstTop, kShiftRows, kTexHeight);
		ASSERT_TRUE(band.enabled) << "DBY " << dby;
		EXPECT_FLOAT_EQ(band.src_v * static_cast<float>(kTexHeight), static_cast<float>(dby * 2) + 0.5f)
			<< "DBY " << dby;
	}
}

TEST(GSFieldShiftTopBand, TheMainDrawStopsWhereTheBandStarts)
{
	// Circuit 1 is BLENDED over circuit 2. If the shifted main draw still covered the band's rows
	// and the band then drew over them, those rows would take circuit 1 twice -- once from the
	// wrong rows above the rect and once from the band -- whenever the merge alpha is below one.
	const float src_top_v = 2.0f / kTexHeight;
	const float src_bot_v = (2.0f + 448.0f) / kTexHeight;
	const float per_dst_row = (src_bot_v - src_top_v) / (kDstBottom - kDstTop);
	const float shift_v = kShiftRows * per_dst_row;

	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(1, src_top_v, kDstTop, kShiftRows, kTexHeight);
	ASSERT_TRUE(band.enabled);

	const GSFieldShiftMainTop main = GSFieldShiftMainDrawBelowBand(band, kDstTop, src_top_v - shift_v, shift_v);
	EXPECT_FLOAT_EQ(main.dst_top, band.dst_bottom);

	// Below the band every row samples what the untrimmed shifted draw sampled there.
	for (int row = 0; row < 16; row++)
	{
		if (static_cast<float>(row) + 0.5f < main.dst_top)
			continue;
		EXPECT_EQ(SampledTexelRow(main.src_top_v, src_bot_v - shift_v, main.dst_top, kDstBottom, row),
			SampledTexelRow(src_top_v - shift_v, src_bot_v - shift_v, kDstTop, kDstBottom, row))
			<< "destination row " << row;
	}
}

TEST(GSFieldShiftTopBand, WithNoBandTheMainDrawIsUntouched)
{
	const GSFieldShiftTopBand band =
		GSComputeFieldShiftTopBand(0, 0.0f, kDstTop, kShiftRows, kTexHeight);
	ASSERT_FALSE(band.enabled);

	const GSFieldShiftMainTop main = GSFieldShiftMainDrawBelowBand(band, kDstTop, -0.01f, 0.01f);
	EXPECT_FLOAT_EQ(main.dst_top, kDstTop);
	EXPECT_FLOAT_EQ(main.src_top_v, -0.01f);
}
