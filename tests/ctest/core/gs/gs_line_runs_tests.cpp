// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSLineRuns::Convert against the original conversion (gs_line_runs_reference.h), byte for byte:
// the outcome, the rectangle count, every byte of every rectangle, and the rewritten draw list.
// The rectangles are what the GPU rasterises, so byte identity here is pixel identity downstream.

#include <gtest/gtest.h>

#include "gs_line_runs_reference.h"

#include "GS/Renderers/HW/GSLineRuns.h"

#include <bit>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
	struct Draw
	{
		std::vector<GSVertex> vertices;
		std::vector<u16> indices;
		int ofx = 0;
		int ofy = 0;
		bool flat = false;
		bool fog = false;
		bool abe = false;
		bool aa1 = false;
		std::vector<size_t> groups; ///< empty means no draw list

		void Line(const GSVertex& a, const GSVertex& b)
		{
			indices.push_back(static_cast<u16>(vertices.size()));
			vertices.push_back(a);
			indices.push_back(static_cast<u16>(vertices.size()));
			vertices.push_back(b);
		}

		GSLineRuns::Input Input() const
		{
			return {.vertices = vertices.data(),
				.indices = indices.data(),
				.line_count = static_cast<u32>(indices.size() / 2),
				.ofx = ofx,
				.ofy = ofy,
				.flat = flat,
				.fog = fog,
				.abe = abe,
				.aa1 = aa1};
		}

		std::string Describe() const
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "%zu lines, of=(%d,%d) flat=%d fog=%d abe=%d aa1=%d groups=%zu",
				indices.size() / 2, ofx, ofy, flat, fog, abe, aa1, groups.size());
			return buf;
		}
	};

	GSVertex Vert(int x, int y, u32 z = 0, u32 rgba = 0x80808080u, u32 fog = 0, float s = 0.0f, float t = 0.0f,
		float q = 1.0f, u16 u = 0, u16 v = 0)
	{
		GSVertex out;
		std::memset(&out, 0, sizeof(out));
		out.XYZ.X = static_cast<u16>(x);
		out.XYZ.Y = static_cast<u16>(y);
		out.XYZ.Z = z;
		out.RGBAQ.U32[0] = rgba;
		out.RGBAQ.Q = q;
		out.ST.S = s;
		out.ST.T = t;
		out.U = u;
		out.V = v;
		out.FOG = fog;
		return out;
	}

	std::string DescribeVertex(const GSVertex& v)
	{
		char buf[192];
		std::snprintf(buf, sizeof(buf), "xy=(%u,%u) z=%08x rgba=%08x q=%08x s=%08x t=%08x uv=(%u,%u) fog=%08x", v.XYZ.X,
			v.XYZ.Y, v.XYZ.Z, v.RGBAQ.U32[0], std::bit_cast<u32>(v.RGBAQ.Q), std::bit_cast<u32>(v.ST.S),
			std::bit_cast<u32>(v.ST.T), v.U, v.V, v.FOG);
		return buf;
	}

	/// Guard vertices past the capacity Capacity() promises, to catch a write beyond it.
	constexpr u32 GUARD = 8;
	constexpr u8 FILL = 0xA5;

	/// Runs both conversions on the draw and requires identical results. Returns the outcome.
	GSLineRuns::Outcome ExpectSame(const Draw& d)
	{
		const GSLineRuns::Input in = d.Input();
		const u32 capacity = GSLineRuns::Capacity(in);

		std::vector<GSVertex> ref_dst(capacity * 4 + GUARD);
		std::vector<GSVertex> new_dst(capacity * 4 + GUARD);
		std::memset(ref_dst.data(), FILL, ref_dst.size() * sizeof(GSVertex));
		std::memset(new_dst.data(), FILL, new_dst.size() * sizeof(GSVertex));

		std::vector<size_t> ref_groups = d.groups;
		std::vector<size_t> new_groups = d.groups;

		const GSLineRuns::Result ref = GSLineRunsReference::Convert(in, ref_dst.data(), d.groups.empty() ? nullptr : &ref_groups);
		const GSLineRuns::Result got = GSLineRuns::Convert(in, new_dst.data(), d.groups.empty() ? nullptr : &new_groups);

		EXPECT_EQ(static_cast<int>(got.outcome), static_cast<int>(ref.outcome)) << d.Describe();
		EXPECT_EQ(got.rects, ref.rects) << d.Describe();
		EXPECT_EQ(new_groups, ref_groups) << d.Describe();

		if (ref.outcome == GSLineRuns::Outcome::Converted && got.outcome == ref.outcome && got.rects == ref.rects)
		{
			EXPECT_LE(ref.rects, capacity) << d.Describe();
			for (u32 i = 0; i < ref.rects * 4; i++)
			{
				if (std::memcmp(&new_dst[i], &ref_dst[i], sizeof(GSVertex)) != 0)
				{
					ADD_FAILURE() << d.Describe() << "\nvertex " << i << " of " << ref.rects * 4
								  << "\n  want " << DescribeVertex(ref_dst[i]) << "\n  got  " << DescribeVertex(new_dst[i]);
					break;
				}
			}
		}

		for (u32 i = capacity * 4; i < capacity * 4 + GUARD; i++)
		{
			const u8* p = reinterpret_cast<const u8*>(&new_dst[i]);
			for (size_t b = 0; b < sizeof(GSVertex); b++)
			{
				if (p[b] != FILL)
				{
					ADD_FAILURE() << d.Describe() << "\nwrote past the capacity of " << capacity << " rectangles";
					return got.outcome;
				}
			}
		}
		return ref.outcome;
	}

	/// Every flag combination the conversion reads.
	template <typename F>
	void ForEachFlags(F&& f)
	{
		for (int bits = 0; bits < 16; bits++)
			f((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0);
	}

	void SetFlags(Draw& d, bool flat, bool fog, bool abe, bool aa1)
	{
		d.flat = flat;
		d.fog = fog;
		d.abe = abe;
		d.aa1 = aa1;
	}

	/// A pair of vertices with every attribute different, so every interpolated field is exercised.
	void RichLine(Draw& d, int x0, int y0, int x1, int y1)
	{
		d.Line(Vert(x0, y0, 0x00001000u, 0x80FF0010u, 0x20, 0.25f, 0.75f, 1.0f, 16, 4000),
			Vert(x1, y1, 0xFFFF0000u, 0x8000FFF0u, 0xE0, 3.5f, -1.25f, 0.5f, 60000, 32));
	}
} // namespace

TEST(GSLineRuns, ZeroLengthLinesLightNothing)
{
	ForEachFlags([](bool flat, bool fog, bool abe, bool aa1) {
		for (int fx = 0; fx < 16; fx++)
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = 0x8000;
			d.ofy = 0x8000;
			RichLine(d, 0x8000 + 16 * 10 + fx, 0x8000 + 16 * 10 + fx, 0x8000 + 16 * 10 + fx, 0x8000 + 16 * 10 + fx);
			EXPECT_EQ(ExpectSame(d), GSLineRuns::Outcome::NothingLit);
		}
	});
}

TEST(GSLineRuns, AxisAlignedDiagonalAndSteepLinesInEveryDirection)
{
	const int c = 0x8000 + 16 * 100;
	const int dirs[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {3, 1}, {-3, 1},
		{3, -1}, {-3, -1}, {1, 3}, {-1, 3}, {1, -3}, {-1, -3}, {7, 5}, {5, 7}, {-5, -7}, {-7, 5}};
	ForEachFlags([&](bool flat, bool fog, bool abe, bool aa1) {
		for (const auto& dir : dirs)
		{
			for (const int len : {1, 2, 3, 17, 64, 333})
			{
				Draw d;
				SetFlags(d, flat, fog, abe, aa1);
				d.ofx = 0x8000;
				d.ofy = 0x8000;
				RichLine(d, c, c, c + dir[0] * len * 16 / 3, c + dir[1] * len * 16 / 3);
				RichLine(d, c + 5, c + 11, c + 5 + dir[0] * len * 16, c + 11 + dir[1] * len * 16);
				ExpectSame(d);
			}
		}
	});
}

TEST(GSLineRuns, EveryFractionalEndpoint)
{
	const int c = 0x4000;
	ForEachFlags([&](bool flat, bool fog, bool abe, bool aa1) {
		Draw d;
		SetFlags(d, flat, fog, abe, aa1);
		d.ofx = 0x3000;
		d.ofy = 0x3000;
		for (int f0 = 0; f0 < 16; f0++)
		{
			for (int f1 = 0; f1 < 16; f1++)
			{
				RichLine(d, c + f0, c + (f1 ^ 5), c + 16 * 9 + f1, c + 16 * 4 + (f0 ^ 3));
				RichLine(d, c + f0, c + f1, c + (f1 ^ 7), c + 16 * 6 + f0);
				RichLine(d, c + 16 * 3 + f0, c + f1, c + f1, c + f0);
			}
		}
		ExpectSame(d);
	});
}

TEST(GSLineRuns, ColourAndFogExtremes)
{
	const u32 colours[] = {0x00000000u, 0xFFFFFFFFu, 0x80000000u, 0x80FFFFFFu, 0x00FF00FFu, 0xFF00FF00u, 0x7F7F7F7Fu,
		0x01010101u, 0xFEFEFEFEu, 0x80808080u};
	const u32 fogs[] = {0x00u, 0xFFu, 0x80u, 0x01u, 0xFFFFFFFFu, 0xFF00FF00u};
	const int c = 0x8000 + 16 * 50;
	ForEachFlags([&](bool flat, bool fog, bool abe, bool aa1) {
		for (const u32 ca : colours)
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = 0x8000;
			d.ofy = 0x8000;
			for (const u32 cb : colours)
			{
				for (const u32 fa : fogs)
				{
					const u32 fb = fogs[(fa * 7 + cb) % std::size(fogs)];
					for (const int len : {1, 5, 255, 256, 700})
					{
						d.Line(Vert(c + 3, c + 9, 0, ca, fa), Vert(c + 3 + len * 16 + 7, c + 9 + len * 5, 0, cb, fb));
						d.Line(Vert(c + len * 16, c + 1, 0, ca, fa), Vert(c + 2, c + len * 3, 0, cb, fb));
					}
				}
			}
			ExpectSame(d);
		}
	});
}

TEST(GSLineRuns, LongLinesAndTheSixteenBitEdges)
{
	ForEachFlags([](bool flat, bool fog, bool abe, bool aa1) {
		for (const int of : {0, 8, 0x7F8, 0x8000, 0xFFF0, 0xFFFF})
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = of;
			d.ofy = 0xFFFF - of;
			RichLine(d, 0, 0, 0xFFFF, 0xFFFF);
			RichLine(d, 0xFFFF, 3, 0, 0x7FFF);
			RichLine(d, 7, 0xFFFF, 0xFFF8, 0);
			RichLine(d, 0x8000, 0, 0x8008, 0xFFFF);
			RichLine(d, of, of, of + 16 * 4000, of + 16 * 1);
			ExpectSame(d);
		}
	});
}

TEST(GSLineRuns, InterpolatedAttributeExtremes)
{
	const float floats[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-30f, -1e30f, 3.0e38f, std::numeric_limits<float>::max(),
		std::numeric_limits<float>::denorm_min(), 0.333333f, 4096.5f, -7.75f};
	const u32 zs[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 12345678u};
	const u16 uvs[] = {0, 1, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF, 1234};
	const int c = 0x8000;
	ForEachFlags([&](bool flat, bool fog, bool abe, bool aa1) {
		Draw d;
		SetFlags(d, flat, fog, abe, aa1);
		d.ofx = 0x7000;
		d.ofy = 0x7000;
		int n = 0;
		for (const float fa : floats)
		{
			for (const float fb : floats)
			{
				const u32 za = zs[n % std::size(zs)];
				const u32 zb = zs[(n / 3) % std::size(zs)];
				const u16 ua = uvs[n % std::size(uvs)];
				const u16 ub = uvs[(n / 5) % std::size(uvs)];
				const int len = 1 + (n % 37) * 3;
				d.Line(Vert(c + (n & 15), c + n, za, 0x80102030u, 0, fa, fb, fb, ua, ub),
					Vert(c + len * 16 + (n % 11), c + n + len * 7, zb, 0x80302010u, 0, fb, fa, fa, ub, ua));
				d.Line(Vert(c + n, c + len * 16, zb, 0x80102030u, 0, fb, fa, fa, ub, ua),
					Vert(c + n + 3, c, za, 0x80302010u, 0, fa, fb, fb, ua, ub));
				n++;
			}
		}
		ExpectSame(d);
	});
}

TEST(GSLineRuns, DrawListGroups)
{
	const int c = 0x8000 + 16 * 30;
	ForEachFlags([&](bool flat, bool fog, bool abe, bool aa1) {
		// Every group lights something, and the list covers every line.
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = d.ofy = 0x8000;
			for (int i = 0; i < 12; i++)
				RichLine(d, c + i * 16, c, c + i * 16 + 16 * (i + 2), c + 16 * 3 * i);
			d.groups = {3, 1, 5, 3};
			EXPECT_EQ(ExpectSame(d), GSLineRuns::Outcome::Converted);
		}
		// The list stops short of the last lines.
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = d.ofy = 0x8000;
			for (int i = 0; i < 8; i++)
				RichLine(d, c, c + i * 16, c + 16 * 20, c + i * 16 + 16 * i);
			d.groups = {2, 2};
			EXPECT_EQ(ExpectSame(d), GSLineRuns::Outcome::Converted);
		}
		// A middle group of lines that light nothing.
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = d.ofy = 0x8000;
			RichLine(d, c, c, c + 16 * 10, c + 16 * 2);
			RichLine(d, c + 3, c + 3, c + 3, c + 3);
			RichLine(d, c + 1, c + 1, c + 4, c + 2);
			RichLine(d, c, c + 16, c + 16 * 7, c + 16 * 9);
			d.groups = {1, 2, 1};
			EXPECT_EQ(ExpectSame(d), GSLineRuns::Outcome::GroupEmpty);
		}
		// Nothing lit anywhere, with a list: the empty group decides first.
		{
			Draw d;
			SetFlags(d, flat, fog, abe, aa1);
			d.ofx = d.ofy = 0x8000;
			RichLine(d, c + 3, c + 3, c + 3, c + 3);
			d.groups = {1};
			EXPECT_EQ(ExpectSame(d), GSLineRuns::Outcome::GroupEmpty);
		}
	});
}

TEST(GSLineRuns, PastTheIndexCeiling)
{
	ForEachFlags([](bool flat, bool fog, bool abe, bool aa1) {
		// Gouraud lines with a colour step every pixel: one rectangle per pixel.
		Draw d;
		SetFlags(d, flat, fog, abe, aa1);
		d.ofx = d.ofy = 0;
		for (int i = 0; i < 12; i++)
			d.Line(Vert(8, 16 * (2 * i) + 8, 0, 0x00000000u, 0), Vert(16 * 1500 + 8, 16 * (2 * i + 1) + 8, 0, 0xFFFFFFFFu, 0xFF));
		ExpectSame(d);

		// An empty group after the ceiling is crossed still refuses as an empty group.
		Draw g = d;
		g.Line(Vert(40, 40), Vert(40, 40));
		g.Line(Vert(8, 8, 0, 0, 0), Vert(16 * 20 + 8, 8, 0, 0xFFFFFFFFu, 0xFF));
		g.groups = {12, 1, 1};
		ExpectSame(g);
	});
}

TEST(GSLineRuns, RandomDraws)
{
	std::mt19937 rng(0x6C696E65u);
	const auto pick = [&](u32 n) { return static_cast<u32>(rng() % n); };

	for (int iter = 0; iter < 6000; iter++)
	{
		Draw d;
		SetFlags(d, pick(2), pick(2), pick(2), pick(4) == 0);
		const int shape = pick(4);
		if (shape == 0)
		{
			d.ofx = pick(0x10000);
			d.ofy = pick(0x10000);
		}
		else
		{
			d.ofx = 0x8000 - 16 * 2048 + pick(64);
			d.ofy = 0x8000 - 16 * 2048 + pick(64);
		}

		const u32 lines = 1 + pick(iter % 10 == 0 ? 200 : 12);
		const u32 shared_colour = rng();
		for (u32 i = 0; i < lines; i++)
		{
			GSVertex v[2];
			for (GSVertex& v_ : v)
			{
				int x, y;
				if (shape == 0)
				{
					x = pick(0x10000);
					y = pick(0x10000);
				}
				else
				{
					x = 0x8000 - 16 * 32 + pick(16 * 700);
					y = 0x8000 - 16 * 32 + pick(16 * 500);
				}
				u32 rgba;
				switch (pick(4))
				{
					case 0: rgba = shared_colour; break;
					case 1: rgba = (rng() & 0x00FFFFFFu) | 0x80000000u; break;
					case 2: rgba = pick(2) ? 0xFFFFFFFFu : 0u; break;
					default: rgba = rng(); break;
				}
				const u32 fog = pick(3) == 0 ? 0xFFu : (pick(8) == 0 ? rng() : pick(256));
				const u32 z = pick(3) == 0 ? rng() : pick(0x10000);
				const float s = std::ldexp(static_cast<float>(static_cast<int>(rng())), -static_cast<int>(pick(40)));
				const float t = std::ldexp(static_cast<float>(static_cast<int>(rng())), -static_cast<int>(pick(40)));
				const float q = pick(4) == 0 ? 1.0f : std::ldexp(static_cast<float>(rng() >> 8), -24);
				v_ = Vert(x, y, z, rgba, fog, s, t, q, static_cast<u16>(rng()), static_cast<u16>(rng()));
			}
			if (pick(8) == 0)
			{
				// Axis-aligned, which is where runs are longest.
				if (pick(2))
					v[1].XYZ.Y = v[0].XYZ.Y;
				else
					v[1].XYZ.X = v[0].XYZ.X;
			}
			if (pick(16) == 0)
				v[1] = v[0];
			if (pick(8) == 0)
			{
				// Short: a pixel or two, or inside one pixel's diamond.
				v[1].XYZ.X = static_cast<u16>(v[0].XYZ.X + pick(40) - 20);
				v[1].XYZ.Y = static_cast<u16>(v[0].XYZ.Y + pick(40) - 20);
			}
			d.Line(v[0], v[1]);
		}

		if (pick(4) == 0)
		{
			u32 left = lines - (pick(3) == 0 ? pick(lines) : 0);
			while (left > 0)
			{
				const u32 n = 1 + pick(std::min<u32>(left, 5));
				d.groups.push_back(n);
				left -= n;
			}
		}

		ExpectSame(d);
		if (::testing::Test::HasFailure())
		{
			ADD_FAILURE() << "random draw " << iter;
			return;
		}
	}
}
