// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the GS line walk the hardware renderer places lines with (GS/Renderers/HW/GSLineWalk.h).
//
// Two kinds of test. First, what the gs-prim console capture measured, stated as pixels: the
// perpendicular coordinate rounds, a far endpoint's pixel appears at half a pixel on an axis line
// and at a quarter on a diagonal swept on both axes, a line and its reverse differ by two pixels.
// Then an equivalence sweep against a float transcription of GSRasterizer::DrawEdgeLine, the form
// the software renderer ships, so the integer restatement cannot drift from it.
//
// Rides gs_vertex_tests -- the walk is header-only.

#include "GS/Renderers/HW/GSLineWalk.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace
{
	using Pixel = std::pair<int, int>;
	using Pixels = std::vector<Pixel>;

	Pixels Walk16(int x0, int y0, int x1, int y1)
	{
		Pixels out;
		const int n = GSLineWalk::Walk(x0, y0, x1, y1, [&](int x, int y) { out.emplace_back(x, y); });
		EXPECT_EQ(static_cast<size_t>(n), out.size());
		return out;
	}

	std::set<Pixel> AsSet(const Pixels& p) { return std::set<Pixel>(p.begin(), p.end()); }

	/// GSRasterizer::DrawEdgeLine, non-AA path, as the software renderer writes it: float positions
	/// in pixels, an int decision variable. Scissor and scanline ownership are left out; they filter
	/// the pixels and do not move them.
	Pixels Reference(float x0, float y0, float x1, float y1)
	{
		const float delta_x = x1 - x0;
		const float delta_y = y1 - y0;
		const bool step_x = std::abs(delta_x) >= std::abs(delta_y);
		const bool pos_x = delta_x >= 0.0f;
		const bool pos_y = delta_y >= 0.0f;
		const int dxi = pos_x ? 1 : -1;
		const int dyi = pos_y ? 1 : -1;

		float rx0 = std::floor(x0 + 0.5f);
		float ry0 = std::floor(y0 + 0.5f);
		float rx1 = std::floor(x1 + 0.5f);
		float ry1 = std::floor(y1 + 0.5f);

		const auto TestEndpoint = [&](float dx, float dy) {
			const float dist = std::abs(dx) + std::abs(dy);
			if (dist < 0.5f)
				return false;
			if (step_x)
			{
				const bool x_good = pos_x ? (dx > 0.0f) : (dx < 0.0f);
				return x_good && (dist > 0.5f || dy >= 0.0f);
			}
			const bool y_good = pos_y ? (dy > 0.0f) : (dy < 0.0f);
			return y_good && (dist > 0.5f || dx >= 0.0f);
		};

		const bool draw_first = !TestEndpoint(x0 - rx0, y0 - ry0);
		const bool draw_last = TestEndpoint(x1 - rx1, y1 - ry1);
		if (!draw_first)
		{
			rx0 += step_x ? dxi : 0.0f;
			ry0 += step_x ? 0.0f : dyi;
		}
		if (!draw_last)
		{
			rx1 -= step_x ? dxi : 0.0f;
			ry1 -= step_x ? 0.0f : dyi;
		}
		if ((step_x ? (dxi * (rx1 - rx0)) : (dyi * (ry1 - ry0))) < 0.0f)
			return {};

		const int rxi1 = static_cast<int>(rx1);
		const int ryi1 = static_cast<int>(ry1);
		const bool pos_D = step_x ? pos_y : pos_x;
		const int scaleD = static_cast<int>(2 * 16 * 16 * std::abs(step_x ? delta_x : delta_y));
		const int dD = static_cast<int>(2 * 16 * 16 * (step_x ? delta_y : delta_x));
		int D = static_cast<int>(scaleD * (step_x ? (y0 - ry0) : (x0 - rx0)));
		int xi = static_cast<int>(rx0);
		int yi = static_cast<int>(ry0);

		const auto StepDependent = [&](int sign) {
			D -= scaleD * sign;
			xi += (step_x ? 0 : 1) * sign;
			yi += (step_x ? 1 : 0) * sign;
		};

		const float prestep = step_x ? dxi * (rx0 - x0) : dyi * (ry0 - y0);
		D += static_cast<int>(dD * prestep);
		while (D >= scaleD / 2)
			StepDependent(1);
		while (D < -scaleD / 2)
			StepDependent(-1);

		Pixels out;
		while (true)
		{
			out.emplace_back(xi, yi);
			if (step_x ? (xi == rxi1) : (yi == ryi1))
				break;
			D += dD;
			xi += step_x ? dxi : 0;
			yi += step_x ? 0 : dyi;
			if (pos_D)
			{
				if (D >= scaleD / 2)
					StepDependent(1);
			}
			else
			{
				if (D < -scaleD / 2)
					StepDependent(-1);
			}
		}
		return out;
	}
} // namespace

TEST(GSLineWalk, WholePixelLineDrawsItsFirstPixelAndNotItsLast)
{
	Pixels forward;
	for (int x = 0; x < 10; x++)
		forward.emplace_back(x, 0);
	EXPECT_EQ(Walk16(0, 0, 160, 0), forward);

	Pixels reverse;
	for (int x = 10; x > 0; x--)
		reverse.emplace_back(x, 0);
	EXPECT_EQ(Walk16(160, 0, 0, 0), reverse);
}

// gs-prim: "a horizontal line at y + 0/16 lands on row 8; at y + 8/16 it lands on row 9. Both
// directions, both axes."
TEST(GSLineWalk, PerpendicularCoordinateRoundsToNearest)
{
	for (int f = 0; f < 16; f++)
	{
		const int row = (f < 8) ? 104 : 105;
		for (const Pixel& p : Walk16(0, 104 * 16 + f, 160, 104 * 16 + f))
			EXPECT_EQ(p.second, row) << "fraction " << f;
		for (const Pixel& p : Walk16(160, 104 * 16 + f, 0, 104 * 16 + f))
			EXPECT_EQ(p.second, row) << "fraction " << f << " reversed";
		for (const Pixel& p : Walk16(104 * 16 + f, 0, 104 * 16 + f, 160))
			EXPECT_EQ(p.first, row) << "fraction " << f << " vertical";
	}
}

// gs-prim: "on a horizontal line the far end gains its eleventh pixel at fraction 8; on a 45-degree
// line swept on both axes together it gains it at fraction 4, and swept on X alone at fraction 8."
TEST(GSLineWalk, FarEndpointPixelAppearsWhereTheDiamondIsLeft)
{
	for (int f = 0; f < 16; f++)
	{
		EXPECT_EQ(Walk16(0, 0, 160 + f, 0).size(), (f < 8) ? 10u : 11u) << "horizontal, fraction " << f;
		EXPECT_EQ(Walk16(0, 0, 160 + f, 160 + f).size(), (f < 4) ? 10u : 11u) << "diagonal on both axes, fraction " << f;
		EXPECT_EQ(Walk16(0, 0, 160 + f, 160).size(), (f < 8) ? 10u : 11u) << "diagonal on X alone, fraction " << f;
	}
}

// gs-prim: "Reversing a line's endpoints changes 2 pixels, on all twelve slopes."
TEST(GSLineWalk, ReversedLineDiffersByTwoPixels)
{
	for (int dy = 0; dy <= 160; dy += 16)
	{
		const std::set<Pixel> forward = AsSet(Walk16(0, 0, 160, dy));
		const std::set<Pixel> reverse = AsSet(Walk16(160, dy, 0, 0));
		std::vector<Pixel> changed;
		std::set_symmetric_difference(forward.begin(), forward.end(), reverse.begin(), reverse.end(), std::back_inserter(changed));
		EXPECT_EQ(changed.size(), 2u) << "dy " << dy / 16;
	}
}

TEST(GSLineWalk, ZeroLengthLineDrawsNothing)
{
	for (int f = 0; f < 16; f++)
		EXPECT_TRUE(Walk16(f, 16 - f, f, 16 - f).empty()) << "fraction " << f;
}

// Sly 3 and Sly Cooper draw their particles as one short line per draw, and 60 to 216 of them per
// capture light no pixel at all: the segment starts outside its first pixel's diamond and ends
// inside its last one, so the walk's first pixel is past its last and there is no span. These are
// the coordinates the games send, relative to XYOFFSET, in 1/16 pixel. GSRendererHW draws nothing
// for such a draw -- neither an expanded stripe nor a GPU line -- which is what this pins.
TEST(GSLineWalk, ShortSegmentThatNeverOwnsAPixelDrawsNothing)
{
	const int segments[6][4] = {
		{4887, 2066, 4901, 2081}, // Sly Cooper, draw 2585: 14/16 x 15/16, major Y
		{4916, 2106, 4924, 2092}, // Sly Cooper, draw 2653: the same, heading up
		{5254, 1995, 5261, 1979}, // Sly Cooper, draw 2851: a whole pixel on Y
		{4427, 2652, 4420, 2643}, // Sly 3, draw 3375: 9/16 on the major axis
		{4482, 2249, 4498, 2253}, // Sly 3, draw 3429: a whole pixel on X
		{4324, 2372, 4331, 2386}, // Sly Cooper, draw 2907
	};
	for (const auto& s : segments)
		EXPECT_TRUE(Walk16(s[0], s[1], s[2], s[3]).empty())
			<< "(" << s[0] << "," << s[1] << ") -> (" << s[2] << "," << s[3] << ")";
}

// Ace Combat 5's GUN box, as the game sends it (SLUS-20851 dump 20260812132022, frame 0): a line
// strip whose left side runs upward one row past the top edge. The row past is the dropped last
// pixel, so the box closes and nothing is lit above the top edge.
TEST(GSLineWalk, AceCombatBoxClosesWithNothingAboveTheTopEdge)
{
	const int sides[4][4] = {{390, 291, 435, 291}, {435, 291, 435, 306}, {435, 306, 390, 306}, {390, 306, 390, 290}};
	std::set<Pixel> lit;
	for (const auto& s : sides)
		for (const Pixel& p : Walk16(s[0] * 16, s[1] * 16, s[2] * 16, s[3] * 16))
			lit.insert(p);

	std::set<Pixel> box;
	for (int x = 390; x <= 435; x++)
	{
		box.emplace(x, 291);
		box.emplace(x, 306);
	}
	for (int y = 291; y <= 306; y++)
	{
		box.emplace(390, y);
		box.emplace(435, y);
	}
	EXPECT_EQ(lit, box);
}

// The integer walk against the float code it restates, over every sub-pixel phase of the first
// endpoint on X, a spread of phases on Y, both signs of both deltas, every octant, and negative
// coordinates.
TEST(GSLineWalk, MatchesTheSoftwareRendererWalk)
{
	int lines = 0;
	for (int base : {-40 * 16, 3 * 16, 700 * 16})
	{
		for (int fx = 0; fx < 16; fx++)
		{
			for (int fy = 0; fy < 16; fy += 3)
			{
				for (int dx = -75; dx <= 75; dx += 5)
				{
					for (int dy = -75; dy <= 75; dy += 5)
					{
						const int x0 = base + fx;
						const int y0 = base + 7 * 16 + fy;
						const int x1 = x0 + dx + (dx & 3);
						const int y1 = y0 + dy - (dy & 5);
						const Pixels expected = Reference(x0 / 16.0f, y0 / 16.0f, x1 / 16.0f, y1 / 16.0f);
						ASSERT_EQ(Walk16(x0, y0, x1, y1), expected) << "(" << x0 << "," << y0 << ") -> (" << x1 << "," << y1 << ")";
						lines++;
					}
				}
			}
		}
	}
	EXPECT_GT(lines, 100000);
}
