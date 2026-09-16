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
