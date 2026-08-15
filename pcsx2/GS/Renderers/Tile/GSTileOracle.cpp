// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileOracle.h"

#include "GS/GS.h"
#include "GS/GSUtil.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace GSTileOracle
{
	// A native-heavy frame runs ~1100 draws and the oracle is far too slow to leave on
	// for a long session, so this is generous rather than tuned. At sizeof(Row) it is
	// single-digit megabytes.
	static constexpr size_t MAX_ROWS = 16 * 4096;

	// The detail file is a microscope, not a second ledger: dumping every pixel of every
	// divergent draw in a bad frame produces hundreds of megabytes nobody opens. The
	// first handful of divergent draws is what attribution actually reads, because the
	// lockstep property means row order IS causal order.
	static constexpr u32 MAX_DETAIL_DRAWS = 16;
	static constexpr u32 MAX_DETAIL_PIXELS = 256;

	struct PixelRow
	{
		u32 frame;
		u32 draw;
		s32 x, y;
		u32 pre, sw, gpu;
		u8 depth;
	};

	static std::vector<Row> s_rows;
	static std::vector<PixelRow> s_pixels;
	static bool s_truncated = false;
	static size_t s_divergent = 0;
	static u32 s_detail_draws = 0;
	static u32 s_detail_left = 0;

	namespace
	{
		u32 LaneDelta8(u32 a, u32 b)
		{
			u32 worst = 0;
			for (u32 i = 0; i < 4; i++)
			{
				const int av = static_cast<int>((a >> (i * 8)) & 0xFFu);
				const int bv = static_cast<int>((b >> (i * 8)) & 0xFFu);
				worst = std::max(worst, static_cast<u32>(std::abs(av - bv)));
			}
			return worst;
		}

		// Scaled up to 8-bit levels so a 16-bit destination's number means the same
		// thing as a 32-bit one's -- and the same thing as the frame scorer's threshold,
		// which is what anyone reading the ledger will compare it against.
		u32 LaneDelta5551(u32 a, u32 b)
		{
			static constexpr u32 shift[4] = {0, 5, 10, 15};
			static constexpr u32 mask[4] = {31, 31, 31, 1};
			u32 worst = 0;
			for (u32 i = 0; i < 4; i++)
			{
				const int av = static_cast<int>((a >> shift[i]) & mask[i]);
				const int bv = static_cast<int>((b >> shift[i]) & mask[i]);
				worst = std::max(worst, static_cast<u32>(std::abs(av - bv)) * (255u / mask[i]));
			}
			return worst;
		}
	} // namespace

	Metric MetricForPsm(u32 psm)
	{
		switch (psm)
		{
			case PSMCT32:
			case PSMCT24:
			case PSGPU24:
				return Metric::Rgba8;
			case PSMCT16:
			case PSMCT16S:
				return Metric::Rgba5551;
			default:
				return Metric::Raw;
		}
	}

	Tally Compare(const u32* pre, const u32* sw, const u32* gpu, int w, int h, int x0, int y0, Metric metric)
	{
		Tally t;
		if (w <= 0 || h <= 0)
			return t;

		t.compared = static_cast<u32>(w) * static_cast<u32>(h);

		for (int y = 0; y < h; y++)
		{
			const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
			for (int x = 0; x < w; x++)
			{
				const size_t i = row + static_cast<size_t>(x);
				const u32 s = sw[i];
				const u32 g = gpu[i];
				if (s == g)
					continue;

				// The three-way split. `pre` is what both arms started from, so which
				// arm moved the pixel is a fact rather than an inference -- and it is
				// the difference between a shading bug and a coverage or claim bug.
				const u32 p = pre[i];
				if (s == p)
					t.gpu_only++;
				else if (g == p)
					t.sw_only++;
				else
					t.both++;

				u32 delta;
				switch (metric)
				{
					case Metric::Rgba8:
						delta = LaneDelta8(s, g);
						break;
					case Metric::Rgba5551:
						delta = LaneDelta5551(s, g);
						break;
					default:
						delta = (s > g) ? (s - g) : (g - s);
						break;
				}
				t.max_delta = std::max(t.max_delta, delta);
				if (delta == 1)
					t.d1++;
				else if (delta == 2)
					t.d2++;
				else if (delta < 16)
					t.d3_15++;
				else if (delta < 128)
					t.d16_127++;
				else
					t.d128p++;

				if (t.first_x < 0)
				{
					t.first_x = x0 + x;
					t.first_y = y0 + y;
					t.first_pre = p;
					t.first_sw = s;
					t.first_gpu = g;
				}
			}
		}

		return t;
	}

	bool IsActive()
	{
		// The setting directly, like GSDrawLog: recording must work when the bit is
		// already true at GS open, where there is no config edge to catch.
		return GSConfig.TileDrawOracle;
	}

	void AddRow(const Row& row)
	{
		if (s_rows.size() >= MAX_ROWS)
		{
			s_truncated = true;
			return;
		}
		if (s_rows.empty())
			s_rows.reserve(MAX_ROWS / 16);
		s_rows.push_back(row);
		if (row.diverged())
			s_divergent++;
	}

	u32 OpenPixelDetail()
	{
		if (s_detail_draws >= MAX_DETAIL_DRAWS)
			return 0;
		s_detail_draws++;
		s_detail_left = MAX_DETAIL_PIXELS;
		return s_detail_left;
	}

	void AddPixel(u32 frame, u32 draw, bool depth, s32 x, s32 y, u32 pre, u32 sw, u32 gpu)
	{
		if (s_detail_left == 0)
			return;
		s_detail_left--;
		s_pixels.push_back(PixelRow{frame, draw, x, y, pre, sw, gpu, static_cast<u8>(depth ? 1 : 0)});
	}

	size_t GetRowCount()
	{
		return s_rows.size();
	}

	size_t GetDivergentCount()
	{
		return s_divergent;
	}

	bool WasTruncated()
	{
		return s_truncated;
	}

	void Reset()
	{
		s_rows.clear();
		s_rows.shrink_to_fit();
		s_pixels.clear();
		s_pixels.shrink_to_fit();
		s_truncated = false;
		s_divergent = 0;
		s_detail_draws = 0;
		s_detail_left = 0;
	}

	// Fifteen columns, no trailing separator -- the caller places it.
	static void WriteTally(std::FILE* fp, const Tally& t)
	{
		std::fprintf(fp, "%u,%u,%u,%u,%u,", t.compared, t.sw_only, t.gpu_only, t.both, t.max_delta);
		std::fprintf(fp, "%u,%u,%u,%u,%u,", t.d1, t.d2, t.d3_15, t.d16_127, t.d128p);
		if (t.first_x >= 0)
			std::fprintf(fp, "%d,%d,%08x,%08x,%08x", t.first_x, t.first_y, t.first_pre, t.first_sw, t.first_gpu);
		else
			std::fprintf(fp, ",,,,");
	}

	static bool WritePixelCSV(const std::string& path)
	{
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
		if (!fp)
		{
			Console.Error(fmt::format("GSTileOracle: failed to open '{}' for writing", path));
			return false;
		}

		std::fprintf(fp.get(), "frame,draw,plane,x,y,pre,sw,gpu,moved_by\n");
		for (const PixelRow& p : s_pixels)
		{
			// Which arm moved the pixel, spelled out so the detail file can be read
			// without re-deriving the three-way rule per line.
			const char* moved = (p.sw == p.pre) ? "gpu_only" : ((p.gpu == p.pre) ? "sw_only" : "both");
			std::fprintf(fp.get(), "%u,%u,%s,%d,%d,%08x,%08x,%08x,%s\n", p.frame, p.draw, p.depth ? "z" : "c",
				p.x, p.y, p.pre, p.sw, p.gpu, moved);
		}
		return std::fflush(fp.get()) == 0;
	}

	bool WriteCSV(const std::string& path)
	{
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
		if (!fp)
		{
			Console.Error(fmt::format("GSTileOracle: failed to open '{}' for writing", path));
			return false;
		}

		std::fprintf(fp.get(),
			"frame,draw,prim,prim_count,rect_x,rect_y,rect_w,rect_h,"
			"fb_addr,fb_psm,fb_bw,fbmsk,colormask,"
			"z_addr,z_psm,z_write,z_test,"
			"tme,iip,abe,fge,mip,fst,ate,date,tex_psm,"
			"sync_pages,fp_pages,diff_pages,diff_bytes,rb_bytes,rb_diff_bytes,"
			"c_compared,c_sw_only,c_gpu_only,c_both,c_max_level,c_d1,c_d2,c_d3_15,c_d16_127,c_d128p,"
			"c_x,c_y,c_pre,c_sw,c_gpu,"
			"z_compared,z_sw_only,z_gpu_only,z_both,z_max_delta,z_d1,z_d2,z_d3_15,z_d16_127,z_d128p,"
			"z_x,z_y,z_pre,z_sw,z_gpu\n");

		for (const Row& r : s_rows)
		{
			std::fprintf(fp.get(), "%u,%u,%s,%u,%d,%d,%d,%d,", r.frame, r.draw, GSUtil::GetPrimName(r.prim_type),
				r.prim_count, r.rect_x, r.rect_y, r.rect_z - r.rect_x, r.rect_w - r.rect_y);

			std::fprintf(fp.get(), "%05x,%s,%u,%08x,%u,", r.fb_bp, GSUtil::GetPSMName(r.fb_psm), r.fb_bw, r.fbmsk,
				r.colormask);

			std::fprintf(fp.get(), "%05x,%s,%u,%u,", r.z_bp, GSUtil::GetPSMName(r.z_psm), r.z_write, r.ztst);

			for (u32 bit = 0; bit < 8; bit++)
				std::fprintf(fp.get(), "%u,", (r.shape >> bit) & 1u);
			std::fprintf(fp.get(), "%s,", (r.shape & 1u) ? GSUtil::GetPSMName(r.tex_psm) : "");

			std::fprintf(fp.get(), "%u,%u,%u,%u,%u,%u,", r.sync_pages, r.fp_pages, r.diff_pages, r.diff_bytes,
				r.rb_bytes, r.rb_diff_bytes);

			WriteTally(fp.get(), r.colour);
			std::fprintf(fp.get(), ",");
			WriteTally(fp.get(), r.depth);
			std::fprintf(fp.get(), "\n");
		}

		if (std::fflush(fp.get()) != 0)
			return false;

		return WritePixelCSV(path + ".pixels.csv");
	}
} // namespace GSTileOracle
