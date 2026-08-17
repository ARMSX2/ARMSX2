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
#include <limits>
#include <unordered_map>
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

	// Distinct expanded palettes seen this capture, keyed by the hash the rows carry.
	// Interned rather than stored per row because a palette is a kilobyte and a row is
	// tens of bytes, and the same palette is typically sampled by many draws.
	static std::unordered_map<u64, std::vector<u32>> s_cluts;
	// Two different palettes that hashed the same. Reported rather than merged, because
	// silently serving one palette's colours under another's hash would make the side
	// file lie about content while every count in it stayed plausible.
	static u32 s_clut_hash_collisions = 0;

	bool IsActive()
	{
		// Tracks the setting directly rather than an explicit start, so recording works
		// when DumpDrawLog is already true at GS open -- there is no config edge to
		// detect in that case.
		return GSConfig.DumpDrawLog;
	}

	bool HasOpenDraw()
	{
		return s_open_record != SIZE_MAX;
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
		s_cluts.clear();
		s_clut_hash_collisions = 0;
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

	void NoteClut(const u32* clut, u32 entries)
	{
		if (s_open_record == SIZE_MAX || !clut || entries == 0)
			return;

		// FNV-1a over the palette words. 64 bits so the interning key can be treated as
		// an identity without a content compare on every draw -- the compare happens
		// once, on insert, and only to catch the case this width makes negligible.
		u64 hash = 0xCBF29CE484222325ull;
		for (u32 i = 0; i < entries; i++)
		{
			hash ^= clut[i];
			hash *= 0x100000001B3ull;
		}
		// 0 is the row's "no palette" value, so a palette that hashes there takes 1.
		hash |= (hash == 0) ? 1 : 0;

		s_records[s_open_record].clut_hash = hash;

		const auto it = s_cluts.find(hash);
		if (it == s_cluts.end())
			s_cluts.emplace(hash, std::vector<u32>(clut, clut + entries));
		else if (it->second.size() != entries || !std::equal(it->second.begin(), it->second.end(), clut))
			s_clut_hash_collisions++;
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
		rec.expand = static_cast<u8>(static_cast<u32>(config.vs.expand) | (config.vs.point_size ? (1u << 3) : 0u) |
									(config.line_expand ? (1u << 4) : 0u));
		rec.tex_hazard = static_cast<u8>(config.tex_hazard);
		rec.destination_alpha = static_cast<u8>(config.destination_alpha);
		rec.colormask = static_cast<u8>(config.colormask.wrgba);
		rec.barrier = config.require_full_barrier ? 2 : (config.require_one_barrier ? 1 : 0);
		rec.area_x = static_cast<s16>(config.drawarea.x);
		rec.area_y = static_cast<s16>(config.drawarea.y);
		rec.area_z = static_cast<s16>(config.drawarea.z);
		rec.area_w = static_cast<s16>(config.drawarea.w);
	}

	void NoteTileDraw(bool memo_hit, u32 record_ns, u32 pass_id, TileFallback fallback, const GSVector4i& rect,
		u8 stq_guard, u8 blend_leg, u8 blend_src1, u8 carrier_refusal)
	{
		if (s_open_record == SIZE_MAX)
			return;

		Record& rec = s_records[s_open_record];
		rec.tile = 1;
		rec.memo_hit = memo_hit ? 1 : 0;
		rec.record_ns = record_ns;
		rec.pass_id = pass_id;
		rec.fallback_reason = static_cast<u8>(fallback);
		rec.stq_guard = stq_guard;
		rec.tile_blend_leg = blend_leg;
		rec.tile_blend_src1 = blend_src1;
		rec.tile_carrier_refusal = carrier_refusal;
		// The draw rect (bbox ∩ scissor) — the tile rows' equivalent of the backend
		// drawarea, and the column that localizes a wrong pixel to its draw.
		rec.area_x = static_cast<s16>(std::clamp(rect.x, -32768, 32767));
		rec.area_y = static_cast<s16>(std::clamp(rect.y, -32768, 32767));
		rec.area_z = static_cast<s16>(std::clamp(rect.z, -32768, 32767));
		rec.area_w = static_cast<s16>(std::clamp(rect.w, -32768, 32767));
	}

	void NoteTileSync(u32 pages, u32 drains, TileSyncReason reason)
	{
		if (s_open_record == SIZE_MAX || (pages == 0 && drains == 0))
			return;

		Record& rec = s_records[s_open_record];
		// Saturating: a draw that stalls more than 65535 times has a problem the exact
		// count will not help with, and wrapping would report it as a healthy draw.
		rec.stalls = static_cast<u16>(
			std::min<u32>(static_cast<u32>(rec.stalls) + drains, std::numeric_limits<u16>::max()));
		rec.sync_pages += pages;
		rec.sync_reason |= static_cast<u8>(reason);
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
			case TileFallbackFrameZPairing:
				return "FRAME_Z_PAIRING";
			case TileFallbackStrideZero:
				return "STRIDE_ZERO";
			case TileFallbackFootprintExtent:
				return "FOOTPRINT_EXTENT";
			case TileFallbackFrameZOverlap:
				return "FRAME_Z_OVERLAP";
			case TileFallbackResourceFailure:
				return "RESOURCE_FAILURE";
			case TileFallbackTextureFiltered:
				return "TEX_FILTERED";
			case TileFallbackTextureMip:
				return "TEX_MIP";
			case TileFallbackTexturePsm:
				return "TEX_PSM";
			case TileFallbackTextureFeedback:
				return "TEX_FEEDBACK";
			case TileFallbackTexturePerspective:
				return "TEX_PERSPECTIVE";
			case TileFallbackTextureStqOverflow:
				return "TEX_STQ_OVERFLOW";
			case TileFallbackDepthWalkEnvelope:
				return "ZWALK_ENVELOPE";
			case TileFallbackColClip:
				return "COLCLIP";
			case TileFallbackBlendOverlap:
				return "BLEND_OVERLAP";
			case TileFallbackBlendTexSample:
				return "BLEND_TEX_SAMPLE";
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

	// Record::expand, named. The hardware routes are reported in preference to the
	// vertex-shader class because they are mutually exclusive with it at the point the
	// draw config is built: a device that expands points in hardware never sets
	// VSExpand::Point.
	static const char* GetExpandName(u8 expand)
	{
		if (expand & (1u << 3))
			return "HW_POINT";
		if (expand & (1u << 4))
			return "HW_LINE";
		return GSGetVSExpandName(static_cast<GSHWDrawConfig::VSExpand>(expand & 7));
	}

	// Every distinct palette the capture sampled through, one row of hex words per
	// palette, keyed by the hash its draws carry. Written beside the CSV rather than
	// into it because a palette is 16 or 256 words and would swamp a row.
	static void WriteClutSidecar(const std::string& csv_path)
	{
		if (s_cluts.empty())
			return;

		const std::string path = csv_path + ".cluts.csv";
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
		if (!fp)
		{
			Console.Error(fmt::format("GSDrawLog: failed to open '{}' for writing", path));
			return;
		}

		std::fprintf(fp.get(), "clut,entries,words\n");
		for (const auto& [hash, words] : s_cluts)
		{
			std::fprintf(fp.get(), "%016llx,%zu,", static_cast<unsigned long long>(hash), words.size());
			for (size_t i = 0; i < words.size(); i++)
				std::fprintf(fp.get(), "%s%08x", i ? " " : "", words[i]);
			std::fputc('\n', fp.get());
		}

		Console.WriteLn(fmt::format("GSDrawLog: wrote {} distinct palettes to {}{}", s_cluts.size(), path,
			s_clut_hash_collisions
				? fmt::format(" (⚠ {} hash collisions -- those rows name the WRONG palette)", s_clut_hash_collisions)
				: ""));
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
			"clut,clut_addr,clut_psm,clut_csm,clut_csa,clut_cld,"
			"blend,alpha_a,alpha_b,alpha_c,alpha_d,alpha_fix,vertex_rgba,vertex_rgba_eq,"
			"atst,afail,aref,date,datm,self_read,"
			"topology,expand,barrier,fb_loop_rt,prim_overlap,tex_hazard,destination_alpha,colormask,"
			"area_x,area_y,area_w,area_h,"
			"memo_hit,record_ns,pass_id,fallback,stq_guard,blend_leg,blend_src1,carrier_refusal,stalls,sync_pages,sync_reason,"
			"mmag,mmin,mxl,tcc,tfx,fge,fst,aa1,colclamp,pabe,fba,dthe,iip\n");

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

			std::fprintf(fp.get(), "%s,%s,%u,%d,", r.tile ? "tile" : (r.sw ? "sw" : "hw"),
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

			// Palette identity, then the registers that produced it. The hash is what
			// joins a row to the side file; CBP alone does not identify a palette, since
			// a game can reload the same address hundreds of times in one frame.
			if (r.clut_hash != 0)
			{
				std::fprintf(fp.get(), "%016llx,%05x,%s,%u,%u,%u,",
					static_cast<unsigned long long>(r.clut_hash), r.tex_cbp,
					GSUtil::GetPSMName(r.tex_clut_cfg & 0xF), (r.tex_clut_cfg >> 4) & 1,
					(r.tex_clut_cfg >> 5) & 0x1F, (r.tex_clut_cfg >> 10) & 7);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,");
			}

			if (blended)
			{
				std::fprintf(fp.get(), "1,%u,%u,%u,%u,%u,", (r.alpha >> 6) & 3, (r.alpha >> 4) & 3, (r.alpha >> 2) & 3,
					r.alpha & 3, r.alpha_fix);
			}
			else
			{
				std::fprintf(fp.get(), "0,,,,,,");
			}

			// Always emitted: the vertex colour decides whether a texture function is an
			// identity or a scale, on blended and unblended draws alike.
			std::fprintf(fp.get(), "%08x,%u,", r.vertex_rgba, r.vertex_rgba_eq);

			if (r.flags & FlagAlphaTest)
				std::fprintf(fp.get(), "%u,%u,%u,", r.atst, r.afail, r.aref);
			else
				std::fprintf(fp.get(), ",,,");

			std::fprintf(fp.get(), "%d,%u,%s,", (r.flags & FlagDate) ? 1 : 0, r.datm, GetSelfReadName(r.self_read));

			if (submitted)
			{
				std::fprintf(fp.get(), "%s,%s,%u,%d,%s,%s,%s,%x,",
					GSGetTopologyName(static_cast<GSHWDrawConfig::Topology>(r.topology)), GetExpandName(r.expand),
					r.barrier, (r.flags & FlagFeedbackLoopRT) ? 1 : 0,
					GetPrimOverlapName(r.prim_overlap), GSGetTexHazardName(r.tex_hazard),
					GSGetDestinationAlphaModeName(static_cast<GSHWDrawConfig::DestinationAlphaMode>(r.destination_alpha)),
					r.colormask);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,,,");
			}

			// Tile and software rows carry the draw rect (bbox ∩ scissor) here; Classic
			// rows the backend drawarea. Same columns, same meaning: where the draw landed.
			if (submitted || r.tile || r.sw)
				std::fprintf(fp.get(), "%d,%d,%d,%d,", r.area_x, r.area_y, r.area_z - r.area_x, r.area_w - r.area_y);
			else
				std::fprintf(fp.get(), ",,,,");

			if (r.tile)
			{
				// sync_reason in hex, like expand: it is a bitfield, and naming one of
				// several set bits would throw away exactly the distinction it exists to
				// draw. A sole-reason row is a power of two, which reads at a glance.
				// blend_leg named, not numbered: the analyses over this column are
				// per-device leg distributions, read by people.
				static constexpr const char* kTileBlendLegNames[] = {
					"", "FF", "MIX", "ACCU", "HWRW", "NOREC", "READ"};
				const char* leg = (r.tile_blend_leg < std::size(kTileBlendLegNames)) ?
				                      kTileBlendLegNames[r.tile_blend_leg] :
				                      "?";
				static constexpr const char* kCarrierRefusalNames[] = {
					"", "ABUSY", "ZGT", "SAT", "SRC1", "SHAPE"};
				const char* refusal = (r.tile_carrier_refusal < std::size(kCarrierRefusalNames)) ?
				                          kCarrierRefusalNames[r.tile_carrier_refusal] :
				                          "?";
				std::fprintf(fp.get(), "%u,%u,%u,%s,%u,%s,%u,%s,%u,%u,%x,", r.memo_hit, r.record_ns, r.pass_id,
					GetTileFallbackName(r.fallback_reason), r.stq_guard, leg, r.tile_blend_src1, refusal,
					r.stalls, r.sync_pages, r.sync_reason);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,,,,,,,");
			}

			if (textured)
			{
				std::fprintf(fp.get(), "%u,%u,%u,%u,%u,", r.tex_filter & 1, (r.tex_filter >> 1) & 7,
					(r.tex_filter >> 4) & 7, (r.env >> 2) & 1, (r.env >> 3) & 3);
			}
			else
			{
				std::fprintf(fp.get(), ",,,,,");
			}

			std::fprintf(fp.get(), "%u,%u,%u,%u,%u,%u,%u,%u\n", r.env & 1, (r.env >> 1) & 1, (r.env >> 7) & 1,
				(r.env >> 5) & 1, (r.env >> 6) & 1, (r.env2 >> 2) & 1, r.env2 & 1, (r.env2 >> 1) & 1);
		}

		Console.WriteLn(fmt::format("GSDrawLog: wrote {} draws to {}{}", s_records.size(), path,
			s_truncated ? fmt::format(" (TRUNCATED at {} records -- later draws were dropped)", MAX_RECORDS) : ""));

		WriteClutSidecar(path);
		return true;
	}
} // namespace GSDrawLog
