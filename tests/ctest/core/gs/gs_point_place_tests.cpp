// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins where the hardware renderer puts a point (GS/Renderers/HW/GSPointPlace.h).
//
// Two kinds of test. First the rule itself, as the gs-prim console capture read it off a phase
// sweep: fractions 0..7 stay on the pixel, 8..15 move to the next one, on both axes and on negative
// coordinates too.
//
// Then the part that only matters above 1x, and that no amount of staring at SetupIA settles: given
// the snapped vertex, the vertex offset the branch chose and the figure the backend draws, which
// device pixels actually come out. The model below is the vertex transform from tfx.glsl -- subtract
// 0.05 GS units, scale, offset, viewport -- followed by the ordinary rasterisation rule that a pixel
// is covered when its centre falls inside the figure. It has to agree with the device block of the
// pixel the point lights: columns ceil(n * S) .. ceil((n + 1) * S) - 1, which is the same partition
// a one-pixel sprite and a pixel-run rectangle land on.
//
// Rides gs_vertex_tests -- the rules are header-only.

#include "GS/Renderers/HW/GSPointPlace.h"

#include <gtest/gtest.h>

#include <cmath>
#include <utility>
#include <vector>

namespace
{
	// ------------------------------------------------------------------ the rule

	TEST(GSPointPlace, RoundsToNearestOnThePhaseSweep)
	{
		// The console table for one pixel: local pixel 8 for fractions 0..7, 9 for 8..15.
		for (int f = 0; f < 16; f++)
		{
			const int expected = (f < 8) ? 8 : 9;
			EXPECT_EQ(GSPointPlace::Pixel(8 * 16 + f), expected) << "fraction " << f;
		}
	}

	TEST(GSPointPlace, RoundsNegativeCoordinatesTheSameWay)
	{
		// -8.5 pixels has to land on -8, not on -9: the boundary is at half a pixel wherever the
		// coordinate sits, which a truncating divide would get wrong on the negative side.
		EXPECT_EQ(GSPointPlace::Pixel(-8 * 16 + 8), -7);
		EXPECT_EQ(GSPointPlace::Pixel(-8 * 16 + 7), -8);
		EXPECT_EQ(GSPointPlace::Pixel(-8 * 16), -8);
		EXPECT_EQ(GSPointPlace::Pixel(-8 * 16 - 8), -8);
		EXPECT_EQ(GSPointPlace::Pixel(-8 * 16 - 9), -9);
	}

	TEST(GSPointPlace, SnapLandsOnThePixelBoundaryAndNeverMovesMoreThanHalfAPixel)
	{
		for (int v = -64 * 16; v <= 64 * 16; v++)
		{
			const int snapped = GSPointPlace::SnapToPixel(v);
			EXPECT_EQ(snapped % 16, 0) << "v " << v;
			EXPECT_EQ(snapped / 16, GSPointPlace::Pixel(v)) << "v " << v;
			EXPECT_LE(std::abs(snapped - v), 8) << "v " << v;
			// Snapping is idempotent: a point already on a boundary does not move.
			EXPECT_EQ(GSPointPlace::SnapToPixel(snapped), snapped) << "v " << v;
		}
	}

	// ------------------------------------------------- the device pixels that come out

	/// One axis of the vertex shader's position transform, ending in device pixels.
	/// `coord` is the vertex's own 1/16 coordinate including XYOFFSET, as the buffer holds it.
	double ToDevicePixels(int coord, int offset16, int native_size, float target_scale, float vertex_offset_term)
	{
		const float scale = 2.0f / static_cast<float>(native_size << 4);
		const float ox = static_cast<float>(offset16);
		const float vertex_offset = ox * scale - vertex_offset_term + 1.0f;
		const double ndc = (static_cast<double>(coord) - 0.05) * scale - vertex_offset;
		const double device_size = static_cast<double>(native_size) * target_scale;
		return (ndc + 1.0) * device_size * 0.5;
	}

	/// The device pixels a figure spanning [lo, hi) device pixels covers: centre-in-figure.
	std::vector<int> Covered(double lo, double hi)
	{
		std::vector<int> out;
		for (int j = -8; j < 512; j++)
		{
			const double centre = j + 0.5;
			if (centre >= lo && centre < hi)
				out.push_back(j);
		}
		return out;
	}

	/// The device pixels native pixel n owns, the partition sprites and pixel runs land on.
	std::vector<int> Block(int n, float target_scale)
	{
		std::vector<int> out;
		const int lo = static_cast<int>(std::ceil(n * static_cast<double>(target_scale)));
		const int hi = static_cast<int>(std::ceil((n + 1) * static_cast<double>(target_scale)));
		for (int j = lo; j < hi; j++)
			out.push_back(j);
		return out;
	}

	constexpr int kNativeSize = 256;
	constexpr int kOffset16 = 2048; // a typical XYOFFSET, 128 pixels

	/// The quad VSExpand::Point builds: one native pixel right and down from the position, so its
	/// corners are pixel boundaries and it takes half a device pixel.
	std::vector<int> BoundaryFigureCoverage(int coord, float target_scale)
	{
		const float scale = 2.0f / static_cast<float>(kNativeSize << 4);
		const float term = GSPointPlace::BoundaryFigureOffset(scale, target_scale);
		const double lo = ToDevicePixels(coord, kOffset16, kNativeSize, target_scale, term);
		const double hi = ToDevicePixels(coord + 16, kOffset16, kNativeSize, target_scale, term);
		return Covered(lo, hi);
	}

	/// A hardware point sprite: target_scale device pixels across, centred on the position, so it
	/// takes half a native pixel.
	std::vector<int> CentredFigureCoverage(int coord, float target_scale)
	{
		const float scale = 2.0f / static_cast<float>(kNativeSize << 4);
		const float term = GSPointPlace::CentredFigureOffset(scale);
		const double centre = ToDevicePixels(coord, kOffset16, kNativeSize, target_scale, term);
		const double half = target_scale * 0.5;
		return Covered(centre - half, centre + half);
	}

	TEST(GSPointPlace, SnappedPointCoversItsBlockOnTheExpandedQuad)
	{
		for (const float s : {1.0f, 2.0f, 3.0f, 4.0f, 1.5f, 2.5f})
		{
			for (int pixel = 16; pixel <= 40; pixel++)
			{
				for (int f = 0; f < 16; f++)
				{
					const int coord = kOffset16 + pixel * 16 + f;
					const int snapped = kOffset16 + GSPointPlace::SnapToPixel(coord - kOffset16);
					const int n = GSPointPlace::Pixel(coord - kOffset16);
					EXPECT_EQ(BoundaryFigureCoverage(snapped, s), Block(n, s))
						<< "scale " << s << " pixel " << pixel << " fraction " << f;
				}
			}
		}
	}

	TEST(GSPointPlace, SnappedPointCoversItsBlockOnAHardwarePointSprite)
	{
		// Integer scales only: a centred figure of width S cannot reproduce a partition whose
		// boundaries are not at half-integer device positions, which is what a fractional scale
		// gives. The expanded quad above is the path that stays exact there.
		for (const float s : {1.0f, 2.0f, 3.0f, 4.0f, 8.0f})
		{
			for (int pixel = 16; pixel <= 40; pixel++)
			{
				for (int f = 0; f < 16; f++)
				{
					const int coord = kOffset16 + pixel * 16 + f;
					const int snapped = kOffset16 + GSPointPlace::SnapToPixel(coord - kOffset16);
					const int n = GSPointPlace::Pixel(coord - kOffset16);
					EXPECT_EQ(CentredFigureCoverage(snapped, s), Block(n, s))
						<< "scale " << s << " pixel " << pixel << " fraction " << f;
				}
			}
		}
	}

	TEST(GSPointPlace, TheTwoOffsetsAgreeAtNativeResolutionAndDivergeAbove)
	{
		const float scale = 2.0f / static_cast<float>(kNativeSize << 4);
		EXPECT_FLOAT_EQ(GSPointPlace::BoundaryFigureOffset(scale, 1.0f), GSPointPlace::CentredFigureOffset(scale));
		EXPECT_GT(GSPointPlace::CentredFigureOffset(scale), GSPointPlace::BoundaryFigureOffset(scale, 2.0f));

		// Half a native pixel is what "align to native" already means: one over the native width,
		// in the same clip-space units. This is why the 1x paths need no new value.
		EXPECT_FLOAT_EQ(GSPointPlace::CentredFigureOffset(scale), 1.0f / static_cast<float>(kNativeSize));
	}

	TEST(GSPointPlace, NoConstantOffsetCanRoundForTheWholeSweepAboveNative)
	{
		// The reason the vertex has to be snapped at all. Sweep every offset a tenth of a device
		// pixel apart over a wide range and show that none of them puts all sixteen phases of an
		// unsnapped point on the right block at 2x.
		constexpr float s = 2.0f;
		const float device_px = 2.0f / (static_cast<float>(kNativeSize) * s);

		bool any_offset_works = false;
		for (int step = -40; step <= 40; step++)
		{
			const float term = static_cast<float>(step) * 0.1f * device_px;
			bool all_phases = true;
			for (int f = 0; f < 16 && all_phases; f++)
			{
				const int coord = kOffset16 + 24 * 16 + f;
				const double lo = ToDevicePixels(coord, kOffset16, kNativeSize, s, term);
				const double hi = ToDevicePixels(coord + 16, kOffset16, kNativeSize, s, term);
				all_phases = (Covered(lo, hi) == Block(GSPointPlace::Pixel(coord - kOffset16), s));
			}
			any_offset_works |= all_phases;
		}
		EXPECT_FALSE(any_offset_works);
	}
} // namespace
