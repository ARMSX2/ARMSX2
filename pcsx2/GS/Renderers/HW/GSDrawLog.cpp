// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/GSExtra.h"
#include "GS/GSState.h"
#include "GS/GSUtil.h"

#include "common/Console.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <algorithm>
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
	// GS-thread-side dump packet mark; see MarkPacket.
	static u32 s_packet_mark = PacketNone;

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

	void MarkPacket(u32 packet_index)
	{
		s_packet_mark = packet_index;
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
		// Stamped here rather than by each renderer's row builder, so every arm carries
		// the column and none of them can carry a different idea of what it means.
		s_records[s_open_record].packet = s_packet_mark;
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
		rec.sample_x = static_cast<s16>(std::clamp(config.samplearea.x, -32768, 32767));
		rec.sample_y = static_cast<s16>(std::clamp(config.samplearea.y, -32768, 32767));
		rec.sample_z = static_cast<s16>(std::clamp(config.samplearea.z, -32768, 32767));
		rec.sample_w = static_cast<s16>(std::clamp(config.samplearea.w, -32768, 32767));
	}

	void NoteSWDraw(const GSVector4i& rect)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.sw = 1;
		rec.area_x = static_cast<s16>(std::clamp(rect.x, -32768, 32767));
		rec.area_y = static_cast<s16>(std::clamp(rect.y, -32768, 32767));
		rec.area_z = static_cast<s16>(std::clamp(rect.z, -32768, 32767));
		rec.area_w = static_cast<s16>(std::clamp(rect.w, -32768, 32767));
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
			"frame,draw,packet,arm,prim,prim_count,submitted,"
			"fb_addr,fb_psm,fb_bw,fbmsk,"
			"z_addr,z_psm,z_test,z_mask,"
			"tex_addr,tex_psm,tex_bw,tex_w,tex_h,"
			"blend,alpha_a,alpha_b,alpha_c,alpha_d,"
			"atst,afail,date,datm,self_read,"
			"topology,barrier,fb_loop_rt,prim_overlap,tex_hazard,destination_alpha,colormask,"
			"area_x,area_y,area_w,area_h,"
			"sample_x,sample_y,sample_w,sample_h\n");

		for (const Record& r : s_records)
		{
			const bool submitted = (r.flags & FlagSubmitted) != 0;
			const bool textured = (r.flags & FlagTextured) != 0;
			const bool blended = (r.flags & FlagBlend) != 0;
			const bool ztest = (r.flags & FlagZTest) != 0;

			if (r.packet != PacketNone)
				std::fprintf(fp.get(), "%u,%u,%u,", r.frame, r.draw, r.packet);
			else
				std::fprintf(fp.get(), "%u,%u,,", r.frame, r.draw);

			std::fprintf(fp.get(), "%s,%s,%u,%d,", r.sw ? "sw" : "hw",
				GSUtil::GetPrimName(r.prim_type), r.prim_count, submitted ? 1 : 0);

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
				std::fprintf(fp.get(), "%s,%u,%d,%s,%s,%s,%x,",
					GSGetTopologyName(static_cast<GSHWDrawConfig::Topology>(r.topology)), r.barrier,
					(r.flags & FlagFeedbackLoopRT) ? 1 : 0,
					GetPrimOverlapName(r.prim_overlap), GSGetTexHazardName(r.tex_hazard),
					GSGetDestinationAlphaModeName(static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.destination_alpha)),
					r.colormask);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,,");
			}

			// Software rows carry the draw rect (bbox n scissor) here; Classic rows the
			// backend drawarea. Same columns, same meaning: where the draw landed.
			if (submitted || r.sw)
			{
				std::fprintf(fp.get(), "%d,%d,%d,%d,", r.area_x, r.area_y, r.area_z - r.area_x,
					r.area_w - r.area_y);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,");
			}

			if (submitted)
			{
				std::fprintf(fp.get(), "%d,%d,%d,%d\n", r.sample_x, r.sample_y,
					r.sample_z - r.sample_x, r.sample_w - r.sample_y);
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
