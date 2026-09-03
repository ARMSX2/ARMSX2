// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/Renderers/HW/GSAlphaKnownBits.h"
#include "GS/GSExtra.h"
#include "GS/GSState.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSRenderer.h"
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
	// Serial handed to each TFX-call row, so calls stay orderable without relying on the
	// row order surviving a sort by frame and draw serial.
	static u32 s_tfx_call_serial = 0;

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
		s_tfx_call_serial = 0;
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

	void NoteAlphaRanges(int src_min, int src_max, int rt_min, int rt_max)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.flags2 |= Flags2AlphaRanges;
		rec.src_alpha_min = static_cast<s16>(src_min);
		rec.src_alpha_max = static_cast<s16>(src_max);
		rec.rt_alpha_min = static_cast<s16>(rt_min);
		rec.rt_alpha_max = static_cast<s16>(rt_max);
	}

	void NoteDATEModes(u8 adreno, u8 stencil, u8 selfcheck)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.flags2 |= Flags2DATEModes;
		rec.date_mode_adreno = adreno;
		rec.date_mode_stencil = stencil;
		rec.date_mode_selfcheck = selfcheck;
	}

	void NoteRTAlpha(u32 target_id, u32 tbp0, u8 alpha_flags, u8 fbmask_a, u8 alpha_fmt_mask)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.flags2 |= Flags2RTAlpha;
		rec.rt_id = target_id;
		rec.rt_tbp0 = tbp0;
		rec.rt_alpha_flags = alpha_flags;
		rec.rt_fbmask_a = fbmask_a;
		rec.rt_alpha_fmt_mask = alpha_fmt_mask;
	}

	void NoteExactAlphaDrop(u8 decision, u8 known_bits, u8 known_value, u8 known_reason)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.exact_alpha_drop = decision;
		rec.exact_alpha_known_bits = known_bits;
		rec.exact_alpha_known_value = known_value;
		rec.exact_alpha_known_reason = known_reason;
	}

	void NoteRTAlphaCommitted()
	{
		if (s_open_record == SIZE_MAX)
			return;

		s_records[s_open_record].rt_alpha_flags |= RTAlphaCommitted;
	}

	void NoteTargetEvent(u8 kind, u32 target_id, u32 tbp0, int payload_min, int payload_max,
		int rt_min, int rt_max, const GSVector4i& valid)
	{
		if (!IsActive())
			return;

		if (s_records.capacity() < MAX_RECORDS) [[unlikely]]
			s_records.reserve(MAX_RECORDS);

		if (s_records.size() >= MAX_RECORDS)
		{
			s_truncated = true;
			return;
		}

		// An event row is not a draw, so it does not disturb the row a draw has open: it is
		// appended past it and s_open_record is left where it was. Ordering against the
		// draws is what the row is for, so it carries the same frame and draw serial the
		// next draw will -- an event between draws N and N+1 sorts as N+1 with no
		// FlagSubmitted, which reads correctly in stream order.
		Record rec = {};
		rec.frame = static_cast<u32>(g_perfmon.GetFrame());
		rec.draw = g_gs_renderer ? static_cast<u32>(g_gs_renderer->s_n) : 0u;
		rec.packet = s_packet_mark;
		rec.flags2 = Flags2Event | Flags2AlphaRanges;
		rec.evt_kind = kind;
		rec.rt_id = target_id;
		rec.rt_tbp0 = tbp0;
		rec.src_alpha_min = static_cast<s16>(payload_min);
		rec.src_alpha_max = static_cast<s16>(payload_max);
		rec.rt_alpha_min = static_cast<s16>(rt_min);
		rec.rt_alpha_max = static_cast<s16>(rt_max);
		rec.area_x = static_cast<s16>(std::clamp(valid.x, -32768, 32767));
		rec.area_y = static_cast<s16>(std::clamp(valid.y, -32768, 32767));
		rec.area_z = static_cast<s16>(std::clamp(valid.z, -32768, 32767));
		rec.area_w = static_cast<s16>(std::clamp(valid.w, -32768, 32767));
		s_records.push_back(rec);
	}

	void NoteTFXCall(const TFXCall& call)
	{
		if (!IsActive())
			return;

		if (s_records.capacity() < MAX_RECORDS) [[unlikely]]
			s_records.reserve(MAX_RECORDS);

		if (s_records.size() >= MAX_RECORDS)
		{
			s_truncated = true;
			return;
		}

		// Same discipline as NoteTargetEvent: appended past the open draw row, s_open_record
		// left alone. The frame and draw serial are the join key back to that row, which is
		// where the PS2 view and the barrier decision live.
		Record rec = {};
		rec.frame = static_cast<u32>(g_perfmon.GetFrame());
		rec.draw = g_gs_renderer ? static_cast<u32>(g_gs_renderer->s_n) : 0u;
		rec.packet = s_packet_mark;
		rec.flags2 = Flags2TFXCall;
		rec.tfx_call = s_tfx_call_serial++;
		rec.tfx_pipe_hash = call.pipe_hash;
		rec.tfx_ps_key_lo = call.ps_key_lo;
		rec.tfx_ps_key_hi = call.ps_key_hi;
		rec.tfx_pass = call.pass_serial;
		rec.tfx_pipe_key = call.pipe_key;
		rec.tfx_bs_key = call.bs_key;
		rec.tfx_rt_obj = call.rt_obj;
		rec.tfx_ds_obj = call.ds_obj;
		rec.tfx_tex_obj = call.tex_obj;
		rec.tfx_pal_obj = call.pal_obj;
		rec.tfx_sc_x = static_cast<s16>(std::clamp(call.scissor_x, -32768, 32767));
		rec.tfx_sc_y = static_cast<s16>(std::clamp(call.scissor_y, -32768, 32767));
		rec.tfx_sc_z = static_cast<s16>(std::clamp(call.scissor_z, -32768, 32767));
		rec.tfx_sc_w = static_cast<s16>(std::clamp(call.scissor_w, -32768, 32767));
		rec.tfx_vs_key = call.vs_key;
		rec.tfx_dss_key = call.dss_key;
		rec.tfx_cms_key = call.cms_key;
		rec.tfx_samp_sel = call.samp_sel;
		rec.tfx_kind = call.kind;
		rec.tfx_pass_end = call.pass_end;
		s_records.push_back(rec);
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
		rec.sw_road = SWRoadRenderer;
		rec.area_x = static_cast<s16>(std::clamp(rect.x, -32768, 32767));
		rec.area_y = static_cast<s16>(std::clamp(rect.y, -32768, 32767));
		rec.area_z = static_cast<s16>(std::clamp(rect.z, -32768, 32767));
		rec.area_w = static_cast<s16>(std::clamp(rect.w, -32768, 32767));
	}

	void NoteSWRoad(u8 road)
	{
		if (s_open_record == SIZE_MAX)
			return;

		s_records[s_open_record].sw_road = road;
	}

	void NoteSWPrimRender(const GSVector4i& rect, u32 bp_start, u32 bp_end, u8 tex_is_target,
		u32 tex_clear_bytes, u32 tex_blocks)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.sw = 1;
		// The road was named at the decision site; a row that reaches here without one took
		// an entry point nothing tagged, and saying so beats guessing.
		if (rec.sw_road == SWRoadNone)
			rec.sw_road = SWRoadHack;
		rec.sw_tex_is_target = tex_is_target;
		rec.sw_bp_start = bp_start;
		rec.sw_bp_end = bp_end;
		rec.sw_tex_clear_bytes = tex_clear_bytes;
		rec.sw_tex_blocks = tex_blocks;
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
			case SelfReadShuffleOffset:
				return "SHUFFLE_OFFSET";
			default:
				return "";
		}
	}

	static const char* GetSWRoadName(u8 road)
	{
		switch (road)
		{
			case SWRoadSprite:
				return "SPRITE";
			case SWRoadCLUT:
				return "CLUT";
			case SWRoadHack:
				return "HACK";
			case SWRoadRenderer:
				return "RENDERER";
			default:
				return "";
		}
	}

	static const char* GetTargetEventName(u8 kind)
	{
		switch (kind)
		{
			case TargetEventCreate:
				return "CREATE";
			case TargetEventDestroy:
				return "DESTROY";
			case TargetEventClear:
				return "CLEAR";
			case TargetEventUploadFull:
				return "UPLOAD_FULL";
			case TargetEventUploadPartial:
				return "UPLOAD_PARTIAL";
			case TargetEventUploadNoAlpha:
				return "UPLOAD_NO_ALPHA";
			case TargetEventInherit:
				return "INHERIT";
			case TargetEventClobber:
				return "CLOBBER";
			case TargetEventValidGrow:
				return "VALID_GROW";
			default:
				return "";
		}
	}

	static const char* GetExactAlphaDropName(u8 decision)
	{
		switch (decision)
		{
			case ExactAlphaDropTaken:
				return "TAKEN";
			case ExactAlphaDropIneligible:
				return "INELIGIBLE";
			case ExactAlphaDropTargetUnknown:
				return "TARGET_UNKNOWN";
			case ExactAlphaDropSourceNotConstant:
				return "SRC_NOT_CONST";
			case ExactAlphaDropLoadBearing:
				return "LOAD_BEARING";
			default:
				return "";
		}
	}

	static const char* GetAlphaKnownReasonName(u8 reason)
	{
		switch (static_cast<GSAlphaKnownBits::Reason>(reason))
		{
			case GSAlphaKnownBits::Reason::NeverEstablished:
				return "NEVER";
			case GSAlphaKnownBits::Reason::CreateCleared:
				return "CREATE_CLEARED";
			case GSAlphaKnownBits::Reason::Clear:
				return "CLEAR";
			case GSAlphaKnownBits::Reason::DrawFullCover:
				return "DRAW_FULL";
			case GSAlphaKnownBits::Reason::DrawPartialCover:
				return "DRAW_PARTIAL";
			case GSAlphaKnownBits::Reason::ChannelShuffle:
				return "SHUFFLE";
			case GSAlphaKnownBits::Reason::Upload:
				return "UPLOAD";
			case GSAlphaKnownBits::Reason::Grow:
				return "GROW";
			case GSAlphaKnownBits::Reason::Inherit:
				return "INHERIT";
			case GSAlphaKnownBits::Reason::Move:
				return "MOVE";
			case GSAlphaKnownBits::Reason::Clobber:
				return "CLOBBER";
			case GSAlphaKnownBits::Reason::HwHack:
				return "HW_HACK";
			default:
				return "";
		}
	}

	static const char* GetTFXCallKindName(u8 kind)
	{
		switch (kind)
		{
			case TFXCallMain:
				return "MAIN";
			case TFXCallBlendMultiPass:
				return "BLEND_MULTI";
			case TFXCallAlphaSecondPass:
				return "ALPHA_SECOND";
			case TFXCallPrimIDPrepass:
				return "PRIMID_PREPASS";
			default:
				return "";
		}
	}

	static const char* GetPassEndReasonName(u8 reason)
	{
		switch (reason)
		{
			case PassEndOther:
				return "OTHER";
			case PassEndTargetSwitch:
				return "TARGET_SWITCH";
			case PassEndResourceTransition:
				return "RESOURCE_TRANSITION";
			case PassEndTextureUnbound:
				return "TEXTURE_UNBOUND";
			case PassEndTextureUpload:
				return "TEXTURE_UPLOAD";
			case PassEndClear:
				return "CLEAR";
			case PassEndCopy:
				return "COPY";
			case PassEndCloneCopy:
				return "CLONE_COPY";
			case PassEndDATE:
				return "DATE";
			case PassEndColClip:
				return "COLCLIP";
			case PassEndSubmit:
				return "SUBMIT";
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
			"sample_x,sample_y,sample_w,sample_h,"
			"src_alpha_min,src_alpha_max,rt_alpha_min,rt_alpha_max,"
			"date_mode_adreno,date_mode_stencil,date_mode_selfcheck,"
			"event,rt_id,rt_tbp0,rt_alpha_written,rt_alpha_shuffle,rt_alpha_full_cover,"
			"rt_alpha_committed,rt_alpha_range_was_set,rt_covers_valid,rt_no_gaps,rt_tests_pass,"
			"rt_fbmask_a,rt_alpha_fmt_mask,exact_alpha_drop,"
			"rt_alpha_known_bits,rt_alpha_known_value,rt_alpha_known_why,"
			"tfx_call,tfx_kind,tfx_pass,tfx_pass_end,tfx_pipe_hash,"
			"tfx_ps_lo,tfx_ps_hi,tfx_vs,tfx_dss,tfx_cms,tfx_bs,tfx_pipe_key,"
			"tfx_rt,tfx_ds,tfx_tex,tfx_pal,tfx_samp,"
			"tfx_sc_x,tfx_sc_y,tfx_sc_w,tfx_sc_h,"
			"sw_road,sw_tex_target,sw_bp_start,sw_bp_end,sw_tex_clear_bytes,sw_tex_blocks\n");

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
				std::fprintf(fp.get(), "%d,%d,%d,%d,", r.sample_x, r.sample_y,
					r.sample_z - r.sample_x, r.sample_w - r.sample_y);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,");
			}

			if (r.flags2 & Flags2AlphaRanges)
			{
				std::fprintf(fp.get(), "%d,%d,%d,%d,", r.src_alpha_min, r.src_alpha_max, r.rt_alpha_min,
					r.rt_alpha_max);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,");
			}

			if (r.flags2 & Flags2DATEModes)
			{
				std::fprintf(fp.get(), "%s,%s,%s,",
					GSGetDestinationAlphaModeName(
						static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.date_mode_adreno)),
					GSGetDestinationAlphaModeName(
						static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.date_mode_stencil)),
					GSGetDestinationAlphaModeName(
						static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.date_mode_selfcheck)));
			}
			else
			{
				std::fprintf(fp.get(), ",,,");
			}

			std::fprintf(fp.get(), "%s,", GetTargetEventName(r.evt_kind));

			if (r.flags2 & (Flags2RTAlpha | Flags2Event))
			{
				std::fprintf(fp.get(), "%u,%05x,", r.rt_id, r.rt_tbp0);
			}
			else
			{
				std::fprintf(fp.get(), ",,");
			}

			if (r.flags2 & Flags2RTAlpha)
			{
				std::fprintf(fp.get(), "%d,%d,%d,%d,%d,%d,%d,%d,%02x,%02x,%s,",
					(r.rt_alpha_flags & RTAlphaWritten) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaShuffle) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaFullCover) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaCommitted) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaRangeWasSet) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaCoversValid) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaNoGaps) ? 1 : 0,
					(r.rt_alpha_flags & RTAlphaTestsPass) ? 1 : 0,
					r.rt_fbmask_a, r.rt_alpha_fmt_mask, GetExactAlphaDropName(r.exact_alpha_drop));
			}
			else
			{
				// The drop decision is taken before the alpha-range calculation, so a draw that
				// returned in between still has one worth printing.
				std::fprintf(fp.get(), ",,,,,,,,,,%s,", GetExactAlphaDropName(r.exact_alpha_drop));
			}

			if (r.exact_alpha_drop != ExactAlphaDropNotConsidered)
			{
				std::fprintf(fp.get(), "%02x,%02x,%s,", r.exact_alpha_known_bits, r.exact_alpha_known_value,
					GetAlphaKnownReasonName(r.exact_alpha_known_reason));
			}
			else
			{
				std::fprintf(fp.get(), ",,,");
			}

			if (r.flags2 & Flags2TFXCall)
			{
				std::fprintf(fp.get(),
					"%u,%s,%u,%s,%016llx,%016llx,%016llx,%02x,%02x,%02x,%08x,%08x,%08x,%08x,%08x,%08x,%02x,"
					"%d,%d,%d,%d",
					r.tfx_call, GetTFXCallKindName(r.tfx_kind), r.tfx_pass,
					GetPassEndReasonName(r.tfx_pass_end),
					static_cast<unsigned long long>(r.tfx_pipe_hash),
					static_cast<unsigned long long>(r.tfx_ps_key_lo),
					static_cast<unsigned long long>(r.tfx_ps_key_hi),
					r.tfx_vs_key, r.tfx_dss_key, r.tfx_cms_key, r.tfx_bs_key, r.tfx_pipe_key,
					r.tfx_rt_obj, r.tfx_ds_obj, r.tfx_tex_obj, r.tfx_pal_obj, r.tfx_samp_sel,
					r.tfx_sc_x, r.tfx_sc_y, r.tfx_sc_z - r.tfx_sc_x, r.tfx_sc_w - r.tfx_sc_y);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,,,,,,,,,,,,,,,");
			}

			if (r.sw)
			{
				std::fprintf(fp.get(), ",%s,%d,%05x,%05x,%u,%u\n", GetSWRoadName(r.sw_road),
					r.sw_tex_is_target ? 1 : 0, r.sw_bp_start, r.sw_bp_end, r.sw_tex_clear_bytes,
					r.sw_tex_blocks);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,\n");
			}
		}

		Console.WriteLn(fmt::format("GSDrawLog: wrote {} draws to {}{}", s_records.size(), path,
			s_truncated ? fmt::format(" (TRUNCATED at {} records -- later draws were dropped)", MAX_RECORDS) : ""));
		return true;
	}
} // namespace GSDrawLog
