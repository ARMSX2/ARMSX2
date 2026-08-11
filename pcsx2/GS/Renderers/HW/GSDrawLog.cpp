// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/GSExtra.h"
#include "GS/GSState.h"
#include "GS/GSUtil.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <cstdio>
#include <vector>

namespace GSDrawLog
{
	// Bounded capture: ~64 frames of a heavy 2048-draw frame. At sizeof(Record) this is
	// a few MB, which is cheap next to keeping the GS thread free of I/O, and it stops a
	// long live session producing a file nobody can open.
	static constexpr size_t MAX_RECORDS = 64 * 2048;

	static std::vector<Record> s_records;
	static bool s_active = false;
	static bool s_truncated = false;
	// Index of the row opened by BeginDraw, or SIZE_MAX when no row is open.
	static size_t s_open_record = SIZE_MAX;

	bool IsActive()
	{
		// Tracks the setting directly rather than an explicit start, so recording works
		// when DumpDrawLog is already true at GS open -- there is no config edge to
		// detect in that case.
		return GSConfig.DumpDrawLog;
	}

	void Start()
	{
		if (s_active)
			return;

		// Reserved up front so no draw ever pays for a reallocation.
		s_records.reserve(MAX_RECORDS);
		s_active = true;
		s_open_record = SIZE_MAX;
	}

	void Stop()
	{
		s_active = false;
		s_open_record = SIZE_MAX;
	}

	void Reset()
	{
		s_records.clear();
		s_records.shrink_to_fit();
		s_truncated = false;
		s_open_record = SIZE_MAX;
	}

	size_t GetRecordCount()
	{
		return s_records.size();
	}

	bool WasTruncated()
	{
		return s_truncated;
	}

	void BeginDraw(const Record& ps2_state)
	{
		if (!IsActive())
			return;

		// Lazily reserved on the first recorded draw, so enabling the setting at any
		// point still avoids per-draw reallocation.
		if (s_records.capacity() < MAX_RECORDS) [[unlikely]]
			s_records.reserve(MAX_RECORDS);

		if (s_records.size() >= MAX_RECORDS)
		{
			// Stop recording rather than evicting: a contiguous prefix is easier to
			// reason about than a ring whose frame numbering wraps mid-file.
			s_truncated = true;
			return;
		}

		s_records.push_back(ps2_state);
		s_open_record = s_records.size() - 1;
	}

	void NoteSelfRead(SelfRead resolution)
	{
		if (s_open_record == SIZE_MAX)
			return;

		s_records[s_open_record].self_read = static_cast<u8>(resolution);
	}

	void EndDraw(const GSHWDrawConfig& config, u8 prim_overlap)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.flags |= FlagSubmitted;
		rec.prim_overlap = prim_overlap;
		if (config.IsFeedbackLoopRT(config.ps) || config.IsFeedbackLoopRT(config.alpha_second_pass.ps))
			rec.flags |= FlagFeedbackLoopRT;
		rec.topology = static_cast<u8>(config.topology);
		rec.tex_hazard = static_cast<u8>(config.tex_hazard);
		rec.destination_alpha = static_cast<u8>(config.destination_alpha);
		rec.colormask = static_cast<u8>(config.colormask.wrgba);
		rec.barrier = config.require_full_barrier ? 2 : (config.require_one_barrier ? 1 : 0);
		rec.area_x = static_cast<s16>(config.drawarea.x);
		rec.area_y = static_cast<s16>(config.drawarea.y);
		rec.area_z = static_cast<s16>(config.drawarea.z);
		rec.area_w = static_cast<s16>(config.drawarea.w);
	}

	void NoteTileDraw(bool memo_hit, u32 record_ns, u32 pass_id, TileFallback fallback)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.tile = 1;
		rec.memo_hit = memo_hit ? 1 : 0;
		rec.record_ns = record_ns;
		rec.pass_id = pass_id;
		rec.fallback_reason = static_cast<u8>(fallback);
	}

	void FinishDraw()
	{
		s_open_record = SIZE_MAX;
	}

	static const char* GetSelfReadName(u8 resolution)
	{
		switch (resolution)
		{
			case SelfReadTexIsFb:
				return "TEX_IS_FB";
			case SelfReadBarrier:
				return "BARRIER";
			case SelfReadDepthDirect:
				return "DEPTH_DIRECT";
			case SelfReadCopy:
				return "COPY";
			default:
				return "";
		}
	}

	static const char* GetTileFallbackName(u8 reason)
	{
		switch (reason)
		{
			case TileFallbackFloor:
				return "FLOOR";
			case TileFallbackPrimClass:
				return "PRIM_CLASS";
			case TileFallbackSpriteExpand:
				return "SPRITE_EXPAND";
			case TileFallbackTextured:
				return "TEXTURED";
			case TileFallbackBlend:
				return "BLEND";
			case TileFallbackCoverageAA1:
				return "AA1";
			case TileFallbackFog:
				return "FOG";
			case TileFallbackFba:
				return "FBA";
			case TileFallbackScanMask:
				return "SCANMSK";
			case TileFallbackAlphaTest:
				return "ALPHA_TEST";
			case TileFallbackDateTest:
				return "DATE";
			case TileFallbackZTestNever:
				return "ZTST_NEVER";
			case TileFallbackFrameMaskPartial:
				return "FBMSK_PARTIAL";
			case TileFallbackNothingWritten:
				return "NOTHING_WRITTEN";
			case TileFallbackFramePsm:
				return "FRAME_PSM";
			case TileFallbackZbufPsm:
				return "ZBUF_PSM";
			case TileFallbackStrideZero:
				return "STRIDE_ZERO";
			case TileFallbackFootprintExtent:
				return "FOOTPRINT_EXTENT";
			case TileFallbackFrameZOverlap:
				return "FRAME_Z_OVERLAP";
			case TileFallbackResourceFailure:
				return "RESOURCE_FAILURE";
			default:
				return "";
		}
	}

	static const char* GetPrimOverlapName(u8 overlap)
	{
		switch (overlap)
		{
			case GSState::PRIM_OVERLAP_YES:
				return "YES";
			case GSState::PRIM_OVERLAP_NO:
				return "NO";
			default:
				return "UNKNOWN";
		}
	}

	bool WriteCSV(const std::string& path)
	{
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
		if (!fp)
		{
			Console.Error(fmt::format("GSDrawLog: failed to open '{}' for writing", path));
			return false;
		}

		std::fprintf(fp.get(),
			"frame,draw,prim,prim_count,submitted,"
			"fb_addr,fb_psm,fb_bw,fbmsk,"
			"z_addr,z_psm,z_test,z_mask,"
			"tex_addr,tex_psm,tex_bw,tex_w,tex_h,"
			"blend,alpha_a,alpha_b,alpha_c,alpha_d,"
			"atst,afail,date,datm,self_read,"
			"topology,barrier,fb_loop_rt,prim_overlap,tex_hazard,destination_alpha,colormask,"
			"area_x,area_y,area_w,area_h,"
			"memo_hit,record_ns,pass_id,fallback\n");

		for (const Record& r : s_records)
		{
			const bool submitted = (r.flags & FlagSubmitted) != 0;
			const bool textured = (r.flags & FlagTextured) != 0;
			const bool blended = (r.flags & FlagBlend) != 0;
			const bool ztest = (r.flags & FlagZTest) != 0;

			std::fprintf(fp.get(), "%u,%u,%s,%u,%d,", r.frame, r.draw, GSUtil::GetPrimName(r.prim_type),
				r.prim_count, submitted ? 1 : 0);

			std::fprintf(fp.get(), "%05x,%s,%u,%08x,", r.frame_block, GSUtil::GetPSMName(r.frame_psm),
				r.frame_fbw, r.frame_fbmsk);

			if (ztest)
			{
				std::fprintf(fp.get(), "%05x,%s,%u,%d,", r.z_block, GSUtil::GetPSMName(r.z_psm), r.z_ztst,
					(r.flags & FlagZMask) ? 1 : 0);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,");
			}

			if (textured)
			{
				std::fprintf(fp.get(), "%05x,%s,%u,%u,%u,", r.tex_tbp0, GSUtil::GetPSMName(r.tex_psm), r.tex_tbw,
					1u << r.tex_tw, 1u << r.tex_th);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,");
			}

			if (blended)
			{
				std::fprintf(fp.get(), "1,%u,%u,%u,%u,", (r.alpha >> 6) & 3, (r.alpha >> 4) & 3, (r.alpha >> 2) & 3,
					r.alpha & 3);
			}
			else
			{
				std::fprintf(fp.get(), "0,,,,,");
			}

			if (r.flags & FlagAlphaTest)
				std::fprintf(fp.get(), "%u,%u,", r.atst, r.afail);
			else
				std::fprintf(fp.get(), ",,");

			std::fprintf(fp.get(), "%d,%u,%s,", (r.flags & FlagDate) ? 1 : 0, r.datm, GetSelfReadName(r.self_read));

			if (submitted)
			{
				std::fprintf(fp.get(), "%s,%u,%d,%s,%s,%s,%x,%d,%d,%d,%d,",
					GSGetTopologyName(static_cast<GSHWDrawConfig::Topology>(r.topology)), r.barrier,
					(r.flags & FlagFeedbackLoopRT) ? 1 : 0,
					GetPrimOverlapName(r.prim_overlap), GSGetTexHazardName(r.tex_hazard),
					GSGetDestinationAlphaModeName(static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.destination_alpha)),
					r.colormask, r.area_x, r.area_y, r.area_z - r.area_x, r.area_w - r.area_y);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,,,,,,");
			}

			if (r.tile)
			{
				std::fprintf(fp.get(), "%u,%u,%u,%s\n", r.memo_hit, r.record_ns, r.pass_id,
					GetTileFallbackName(r.fallback_reason));
			}
			else
			{
				std::fprintf(fp.get(), ",,,\n");
			}
		}

		Console.WriteLn(fmt::format("GSDrawLog: wrote {} draws to {}{}", s_records.size(), path,
			s_truncated ? fmt::format(" (TRUNCATED at {} records -- later draws were dropped)", MAX_RECORDS) : ""));
		return true;
	}
} // namespace GSDrawLog
