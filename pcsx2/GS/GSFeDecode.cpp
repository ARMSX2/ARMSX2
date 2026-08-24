// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSFeDecode.h"
#include "GS/GSState.h"
#include "GS/GSXXH.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSVertex.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <cstring>
#include <vector>

namespace GSFeDecode
{
	bool g_active = false;

	namespace
	{
		enum class Mode
		{
			Off,
			Record,
			Diff,
		};

		struct State
		{
			Mode mode = Mode::Off;
			FileSystem::ManagedCFilePtr file;
			std::string path;

			// The canonical bytes of the record being built: the 16-byte header
			// prefix, then the body. Reused across records so the hot path does no
			// allocation once it has warmed up.
			std::vector<u8> canon;
			// Diff mode: the recorded record's canonical bytes, read back for
			// comparison.
			std::vector<u8> expect;

			u64 event_index = 0;
			u64 stream_digest = 0;
			u64 frame_index = 0; // Vsync records seen, for the locator

			bool diverged = false;
			std::string report;
		};

		State s_state;

		const char* const kEnvRegNames[kEnvRegWords] = {
			"PRIM", "PRMODE", "PRMODECONT", "TEXCLUT", "SCANMSK", "TEXA", "FOGCOL", "DIMX",
			"DTHE", "COLCLAMP", "PABE", "BITBLTBUF", "TRXDIR", "TRXPOS", "TRXREG"};

		const char* const kCtxRegNames[kCtxRegWords] = {
			"XYOFFSET", "TEX0", "TEX1", "CLAMP", "MIPTBP1", "MIPTBP2", "SCISSOR", "ALPHA",
			"TEST", "FBA", "FRAME", "ZBUF"};

		struct FieldSpan
		{
			u32 offset;
			u32 size;
			const char* name;
		};

#define FE_FIELD(type, member) {static_cast<u32>(offsetof(type, member)), static_cast<u32>(sizeof(type::member)), #member}

		constexpr FieldSpan kDrawFields[] = {
			FE_FIELD(DrawBody, draw_serial),
			FE_FIELD(DrawBody, draw_env),
			FE_FIELD(DrawBody, next_env),
			FE_FIELD(DrawBody, next_v),
			FE_FIELD(DrawBody, draw_rect),
			FE_FIELD(DrawBody, backed_up_ctx),
			FE_FIELD(DrawBody, dirty_gs_regs),
			FE_FIELD(DrawBody, flush_reason),
			FE_FIELD(DrawBody, channel_shuffle_finish),
			FE_FIELD(DrawBody, packed_uv_hack_flag),
			FE_FIELD(DrawBody, pad0),
			FE_FIELD(DrawBody, prim),
			FE_FIELD(DrawBody, ctxt),
			FE_FIELD(DrawBody, vertex_head),
			FE_FIELD(DrawBody, vertex_tail),
			FE_FIELD(DrawBody, vertex_next),
			FE_FIELD(DrawBody, vertex_count),
			FE_FIELD(DrawBody, index_count),
		};

		constexpr FieldSpan kTransferFields[] = {
			FE_FIELD(TransferBody, blit),
			FE_FIELD(TransferBody, env_blit),
			FE_FIELD(TransferBody, pos),
			FE_FIELD(TransferBody, reg),
			FE_FIELD(TransferBody, draw_serial),
			FE_FIELD(TransferBody, payload_digest),
			FE_FIELD(TransferBody, rect),
			FE_FIELD(TransferBody, len),
			FE_FIELD(TransferBody, stat_len),
			FE_FIELD(TransferBody, end),
			FE_FIELD(TransferBody, total),
			FE_FIELD(TransferBody, init_x),
			FE_FIELD(TransferBody, init_y),
			FE_FIELD(TransferBody, first_slice),
			FE_FIELD(TransferBody, pad0),
			FE_FIELD(TransferBody, payload_head),
		};

		constexpr FieldSpan kMoveFields[] = {
			FE_FIELD(MoveBody, blit),
			FE_FIELD(MoveBody, pos),
			FE_FIELD(MoveBody, reg),
			FE_FIELD(MoveBody, draw_serial),
		};

		constexpr FieldSpan kClutLoadFields[] = {
			FE_FIELD(ClutLoadBody, tex0),
			FE_FIELD(ClutLoadBody, texclut),
		};

		constexpr FieldSpan kLocalToHostFields[] = {
			FE_FIELD(LocalToHostBody, blit),
			FE_FIELD(LocalToHostBody, pos),
			FE_FIELD(LocalToHostBody, reg),
			FE_FIELD(LocalToHostBody, draw_serial),
			FE_FIELD(LocalToHostBody, len),
			FE_FIELD(LocalToHostBody, total),
			FE_FIELD(LocalToHostBody, end),
			FE_FIELD(LocalToHostBody, pad0),
		};

		constexpr FieldSpan kVsyncFields[] = {
			FE_FIELD(VsyncBody, frame_index),
			FE_FIELD(VsyncBody, draw_serial),
			FE_FIELD(VsyncBody, field),
			FE_FIELD(VsyncBody, registers_written),
			FE_FIELD(VsyncBody, idle_frame),
			FE_FIELD(VsyncBody, pad0),
		};

#undef FE_FIELD

		// One GSVertex's 32 bytes, named. The renderer reads these by name, so a
		// divergence report that says "vertices[41].XYZ.Z" is worth far more than
		// one that says "+0x538".
		constexpr FieldSpan kVertexFields[] = {
			{0, 4, "ST.S"},
			{4, 4, "ST.T"},
			{8, 4, "RGBAQ.RGBA"},
			{12, 4, "RGBAQ.Q"},
			{16, 2, "XYZ.X"},
			{18, 2, "XYZ.Y"},
			{20, 4, "XYZ.Z"},
			{24, 2, "U"},
			{26, 2, "V"},
			{28, 4, "FOG"},
		};

		struct TypeInfo
		{
			const char* name;
			const FieldSpan* fields;
			u32 field_count;
			u32 fixed_bytes;
		};

		TypeInfo InfoFor(RecordType type)
		{
			switch (type)
			{
				case RecordType::Draw:
					return {"Draw", kDrawFields, std::size(kDrawFields), sizeof(DrawBody)};
				case RecordType::Transfer:
					return {"Transfer", kTransferFields, std::size(kTransferFields), sizeof(TransferBody)};
				case RecordType::Move:
					return {"Move", kMoveFields, std::size(kMoveFields), sizeof(MoveBody)};
				case RecordType::ClutLoad:
					return {"ClutLoad", kClutLoadFields, std::size(kClutLoadFields), sizeof(ClutLoadBody)};
				case RecordType::LocalToHost:
					return {"LocalToHost", kLocalToHostFields, std::size(kLocalToHostFields), sizeof(LocalToHostBody)};
				case RecordType::Vsync:
					return {"Vsync", kVsyncFields, std::size(kVsyncFields), sizeof(VsyncBody)};
				default:
					return {"?unknown?", nullptr, 0, 0};
			}
		}

		std::string HexWindow(const u8* data, u32 len, u32 centre, u32 radius)
		{
			if (!data || len == 0)
				return "(none)";

			const u32 begin = (centre > radius) ? (centre - radius) : 0;
			const u32 end = std::min(len, centre + radius + 1);

			std::string out = fmt::format("[+0x{:04x}] ", begin);
			for (u32 i = begin; i < end; i++)
				out += fmt::format("{}{:02x}", (i == centre) ? '>' : ' ', data[i]);
			return out;
		}

		/// Names the word inside an EnvSnapshot at `rel` bytes from its start.
		std::string DescribeEnvWord(u32 rel)
		{
			const u32 word = rel / 8;
			const u32 byte = rel % 8;
			if (word < kEnvRegWords)
				return fmt::format("{}+{}", kEnvRegNames[word], byte);

			const u32 ctx_word = word - kEnvRegWords;
			const u32 ctx = ctx_word / kCtxRegWords;
			const u32 reg = ctx_word % kCtxRegWords;
			return fmt::format("CTXT[{}].{}+{}", ctx, kCtxRegNames[reg], byte);
		}

		void Disarm()
		{
			g_active = false;
			s_state.mode = Mode::Off;
			s_state.file.reset();
		}
	} // namespace

	const char* TypeName(RecordType type)
	{
		return InfoFor(type).name;
	}

	std::string DescribeOffset(RecordType type, u32 body_offset, const void* body, u32 body_bytes)
	{
		const TypeInfo info = InfoFor(type);
		if (info.field_count == 0)
			return fmt::format("+0x{:04x}", body_offset);

		if (body_offset < info.fixed_bytes)
		{
			for (u32 i = 0; i < info.field_count; i++)
			{
				const FieldSpan& f = info.fields[i];
				if (body_offset < f.offset || body_offset >= f.offset + f.size)
					continue;

				const u32 rel = body_offset - f.offset;
				// The two environment snapshots resolve one level deeper: a bare
				// "draw_env+0x150" names 312 bytes of registers, which is not a
				// locator.
				if (type == RecordType::Draw && (f.size == sizeof(EnvSnapshot)))
					return fmt::format("{}.{}", f.name, DescribeEnvWord(rel));
				if (rel == 0 && f.size <= 8)
					return f.name;
				return fmt::format("{}+{}", f.name, rel);
			}
			return fmt::format("(unnamed)+0x{:04x}", body_offset);
		}

		// Past the fixed part: only Draw has a tail.
		if (type != RecordType::Draw || !body)
			return fmt::format("tail+0x{:04x}", body_offset - info.fixed_bytes);

		const DrawBody* draw = static_cast<const DrawBody*>(body);
		const u32 rel = body_offset - info.fixed_bytes;
		const u32 vertex_bytes = draw->vertex_count * static_cast<u32>(sizeof(GSVertex));

		if (rel < vertex_bytes)
		{
			const u32 idx = rel / static_cast<u32>(sizeof(GSVertex));
			const u32 in_vertex = rel % static_cast<u32>(sizeof(GSVertex));
			for (const FieldSpan& f : kVertexFields)
			{
				if (in_vertex >= f.offset && in_vertex < f.offset + f.size)
					return fmt::format("vertices[{}].{}+{}", idx, f.name, in_vertex - f.offset);
			}
			return fmt::format("vertices[{}]+{}", idx, in_vertex);
		}

		const u32 index_rel = rel - vertex_bytes;
		const u32 index_bytes = draw->index_count * 2;
		if (index_rel < index_bytes)
			return fmt::format("indices[{}]+{}", index_rel / 2, index_rel % 2);

		(void)body_bytes;
		return fmt::format("tail_pad+{}", index_rel - index_bytes);
	}

	// ------------------------------------------------------------------
	// Arming
	// ------------------------------------------------------------------

	bool BeginRecord(const std::string& path, const std::string& build_id)
	{
		End();

		Error error;
		FileSystem::ManagedCFilePtr file = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
		if (!file)
		{
			Console.Error(fmt::format("GSFeDecode: cannot create '{}': {}", path, error.GetDescription()));
			return false;
		}

		StreamHeader header = {};
		std::memcpy(header.magic, "ARMSFED", 8); // 7 chars + the NUL
		header.format_version = kFormatVersion;
		header.header_bytes = sizeof(StreamHeader);
		header.record_header_bytes = sizeof(RecordHeader);
		header.vertex_stride = static_cast<u32>(sizeof(GSVertex));
		header.env_reg_words = kEnvRegWords;
		header.ctx_reg_words = kCtxRegWords;
		std::strncpy(header.build, build_id.c_str(), sizeof(header.build) - 1);

		if (std::fwrite(&header, sizeof(header), 1, file.get()) != 1)
		{
			Console.Error(fmt::format("GSFeDecode: cannot write the header of '{}'", path));
			return false;
		}

		s_state.mode = Mode::Record;
		s_state.file = std::move(file);
		s_state.path = path;
		s_state.event_index = 0;
		s_state.stream_digest = 0;
		s_state.frame_index = 0;
		s_state.diverged = false;
		s_state.report.clear();
		g_active = true;

		Console.WriteLn(fmt::format("GSFeDecode: recording the front-end decode surface to {}", path));
		return true;
	}

	bool BeginDiff(const std::string& path)
	{
		End();

		Error error;
		FileSystem::ManagedCFilePtr file = FileSystem::OpenManagedCFile(path.c_str(), "rb", &error);
		if (!file)
		{
			Console.Error(fmt::format("GSFeDecode: cannot open '{}': {}", path, error.GetDescription()));
			return false;
		}

		StreamHeader header = {};
		if (std::fread(&header, sizeof(header), 1, file.get()) != 1)
		{
			Console.Error(fmt::format("GSFeDecode: '{}' is shorter than its header", path));
			return false;
		}

		// Refuse rather than reinterpret. A stream read under the wrong layout
		// diffs green on garbage, which is the one failure this instrument must
		// never have.
		if (std::memcmp(header.magic, "ARMSFED", 8) != 0)
		{
			Console.Error(fmt::format("GSFeDecode: '{}' is not a front-end decode stream", path));
			return false;
		}
		if (header.format_version != kFormatVersion || header.header_bytes != sizeof(StreamHeader) ||
			header.record_header_bytes != sizeof(RecordHeader) ||
			header.vertex_stride != static_cast<u32>(sizeof(GSVertex)) || header.env_reg_words != kEnvRegWords ||
			header.ctx_reg_words != kCtxRegWords)
		{
			Console.Error(fmt::format("GSFeDecode: '{}' was written by a different format "
									  "(version {} header {} rechdr {} vertex {} env {} ctx {}); this build reads "
									  "version {} header {} rechdr {} vertex {} env {} ctx {}",
				path, header.format_version, header.header_bytes, header.record_header_bytes, header.vertex_stride,
				header.env_reg_words, header.ctx_reg_words, kFormatVersion, sizeof(StreamHeader), sizeof(RecordHeader),
				sizeof(GSVertex), kEnvRegWords, kCtxRegWords));
			return false;
		}

		s_state.mode = Mode::Diff;
		s_state.file = std::move(file);
		s_state.path = path;
		s_state.event_index = 0;
		s_state.stream_digest = 0;
		s_state.frame_index = 0;
		s_state.diverged = false;
		s_state.report.clear();
		g_active = true;

		char build[sizeof(header.build) + 1] = {};
		std::memcpy(build, header.build, sizeof(header.build));
		Console.WriteLn(fmt::format("GSFeDecode: diffing against {} (recorded by build '{}')", path, build));
		return true;
	}

	void End()
	{
		if (s_state.mode == Mode::Record)
		{
			std::fflush(s_state.file.get());
			Console.WriteLn(fmt::format("GSFeDecode: recorded {} events to {} (stream digest {:016x})",
				s_state.event_index, s_state.path, s_state.stream_digest));
		}
		else if (s_state.mode == Mode::Diff)
		{
			// A recording that outlives the live stream is a divergence too: the
			// replay stopped producing records the recording still has.
			if (!s_state.diverged)
			{
				RecordHeader leftover = {};
				if (std::fread(&leftover, sizeof(leftover), 1, s_state.file.get()) == 1)
				{
					s_state.diverged = true;
					s_state.report = fmt::format(
						"GSFeDecode: FIRST DIVERGENCE (stream ended early)\n"
						"  the replay produced {} events; the recording still holds event {} ({})\n",
						s_state.event_index, leftover.event_index, TypeName(static_cast<RecordType>(leftover.type)));
					Console.Error(s_state.report);
				}
				else
				{
					Console.WriteLn(fmt::format("GSFeDecode: {} events compared, IDENTICAL (stream digest {:016x})",
						s_state.event_index, s_state.stream_digest));
				}
			}
		}

		Disarm();
	}

	bool Diverged()
	{
		return s_state.diverged;
	}

	const std::string& DivergenceReport()
	{
		return s_state.report;
	}

	u64 EventCount()
	{
		return s_state.event_index;
	}

	// ------------------------------------------------------------------
	// Submission
	// ------------------------------------------------------------------

	void Submit(RecordType type, const void* fixed, u32 fixed_bytes, const void* tail, u32 tail_bytes)
	{
		if (s_state.mode == Mode::Off)
			return;

		// Body length always lands on 8 so the next record header is aligned and
		// the padding is part of the compared bytes (a decoder that leaves junk
		// there is a decoder that has an uninitialized buffer).
		const u32 unpadded = fixed_bytes + tail_bytes;
		const u32 body_bytes = (unpadded + 7u) & ~7u;
		const u32 canon_bytes = kCanonicalPrefixBytes + body_bytes;

		s_state.canon.assign(canon_bytes, 0);
		u8* canon = s_state.canon.data();

		const u32 type_word = static_cast<u32>(type);
		std::memcpy(canon + 0, &type_word, sizeof(u32));
		std::memcpy(canon + 4, &body_bytes, sizeof(u32));
		std::memcpy(canon + 8, &s_state.event_index, sizeof(u64));
		std::memcpy(canon + kCanonicalPrefixBytes, fixed, fixed_bytes);
		if (tail_bytes != 0)
			std::memcpy(canon + kCanonicalPrefixBytes + fixed_bytes, tail, tail_bytes);

		const u64 record_digest = GSXXH3_64bits(canon, canon_bytes);
		const u64 chain[2] = {s_state.stream_digest, record_digest};
		const u64 stream_digest = GSXXH3_64bits(chain, sizeof(chain));

		if (type == RecordType::Vsync)
			s_state.frame_index++;

		if (s_state.mode == Mode::Record)
		{
			RecordHeader header;
			header.type = type_word;
			header.body_bytes = body_bytes;
			header.event_index = s_state.event_index;
			header.record_digest = record_digest;
			header.stream_digest = stream_digest;

			std::fwrite(&header, sizeof(header), 1, s_state.file.get());
			std::fwrite(canon + kCanonicalPrefixBytes, 1, body_bytes, s_state.file.get());

			s_state.event_index++;
			s_state.stream_digest = stream_digest;
			return;
		}

		// Diff mode. Once diverged we keep the run alive but stop comparing: the
		// first divergence is the answer, and every record after it is noise.
		if (s_state.diverged)
			return;

		RecordHeader expect_header = {};
		if (std::fread(&expect_header, sizeof(expect_header), 1, s_state.file.get()) != 1)
		{
			s_state.diverged = true;
			s_state.report = fmt::format("GSFeDecode: FIRST DIVERGENCE (recording ran out)\n"
										 "  event index  : {}\n"
										 "  record type  : {}\n"
										 "  the replay produced a record the recording does not have\n",
				s_state.event_index, TypeName(type));
			Console.Error(s_state.report);
			return;
		}

		const bool type_ok = (expect_header.type == type_word);
		const bool size_ok = (expect_header.body_bytes == body_bytes);

		s_state.expect.assign(kCanonicalPrefixBytes + expect_header.body_bytes, 0);
		std::memcpy(s_state.expect.data() + 0, &expect_header.type, sizeof(u32));
		std::memcpy(s_state.expect.data() + 4, &expect_header.body_bytes, sizeof(u32));
		std::memcpy(s_state.expect.data() + 8, &expect_header.event_index, sizeof(u64));
		if (expect_header.body_bytes != 0 &&
			std::fread(s_state.expect.data() + kCanonicalPrefixBytes, 1, expect_header.body_bytes,
				s_state.file.get()) != expect_header.body_bytes)
		{
			s_state.diverged = true;
			s_state.report = fmt::format("GSFeDecode: FIRST DIVERGENCE (recording is truncated)\n"
										 "  event index  : {}\n"
										 "  record type  : {}\n"
										 "  the recording's body is shorter than its header claims ({} bytes)\n",
				s_state.event_index, TypeName(type), expect_header.body_bytes);
			Console.Error(s_state.report);
			return;
		}

		if (type_ok && size_ok && record_digest == expect_header.record_digest &&
			std::memcmp(s_state.expect.data(), canon, canon_bytes) == 0)
		{
			s_state.event_index++;
			s_state.stream_digest = stream_digest;
			return;
		}

		// Diverged. Build the locator.
		const u8* expect_body = s_state.expect.data() + kCanonicalPrefixBytes;
		const u8* actual_body = canon + kCanonicalPrefixBytes;

		std::string detail;
		if (!type_ok)
		{
			detail = fmt::format("  record type  : expected {} ({}), got {} ({})\n",
				TypeName(static_cast<RecordType>(expect_header.type)), expect_header.type, TypeName(type), type_word);
		}
		else if (!size_ok)
		{
			detail = fmt::format("  body bytes   : expected {}, got {}\n", expect_header.body_bytes, body_bytes);
		}
		else
		{
			u32 first = body_bytes;
			for (u32 i = 0; i < body_bytes; i++)
			{
				if (expect_body[i] != actual_body[i])
				{
					first = i;
					break;
				}
			}

			if (first == body_bytes)
			{
				// Bodies match byte for byte, so the header prefix is what moved
				// (event index desync).
				detail = fmt::format("  event index  : recording says {}, replay says {}\n", expect_header.event_index,
					s_state.event_index);
			}
			else
			{
				detail = fmt::format("  first diff at: body offset {} (0x{:04x})\n"
									 "  field        : {}\n"
									 "  expected     : {}\n"
									 "  actual       : {}\n",
					first, first, DescribeOffset(type, first, actual_body, body_bytes),
					HexWindow(expect_body, body_bytes, first, 12), HexWindow(actual_body, body_bytes, first, 12));
			}
		}

		u64 draw_serial = 0;
		if (fixed_bytes >= sizeof(u64))
		{
			switch (type)
			{
				case RecordType::Draw:
					draw_serial = static_cast<const DrawBody*>(fixed)->draw_serial;
					break;
				case RecordType::Transfer:
					draw_serial = static_cast<const TransferBody*>(fixed)->draw_serial;
					break;
				case RecordType::Move:
					draw_serial = static_cast<const MoveBody*>(fixed)->draw_serial;
					break;
				case RecordType::LocalToHost:
					draw_serial = static_cast<const LocalToHostBody*>(fixed)->draw_serial;
					break;
				case RecordType::Vsync:
					draw_serial = static_cast<const VsyncBody*>(fixed)->draw_serial;
					break;
				default:
					break;
			}
		}

		s_state.diverged = true;
		s_state.report = fmt::format("GSFeDecode: FIRST DIVERGENCE\n"
									 "  recording    : {}\n"
									 "  event index  : {}\n"
									 "  frame        : {}\n"
									 "  record type  : {}\n"
									 "  draw serial  : {}\n"
									 "{}",
			s_state.path, s_state.event_index, s_state.frame_index, TypeName(type), draw_serial, detail);
		Console.Error(s_state.report);
	}
} // namespace GSFeDecode

// ======================================================================
// The taps. These are GSState/GSRenderer members so the gather can read the
// protected decode state directly; they live here so GSState.cpp carries only
// the one-line guarded call at each seam.
// ======================================================================

namespace
{
	/// Copies one environment's register words in declaration order. Nothing else
	/// from the class: GSDrawingContext::scissor is derived and ::offset holds
	/// live pointers, so a sizeof() copy would make the stream unreproducible.
	void SnapshotEnv(GSFeDecode::EnvSnapshot& out, const GSDrawingEnvironment& env)
	{
		u64* w = out.w;
		*w++ = env.PRIM.U64;
		*w++ = env.PRMODE.U64;
		*w++ = env.PRMODECONT.U64;
		*w++ = env.TEXCLUT.U64;
		*w++ = env.SCANMSK.U64;
		*w++ = env.TEXA.U64;
		*w++ = env.FOGCOL.U64;
		*w++ = env.DIMX.U64;
		*w++ = env.DTHE.U64;
		*w++ = env.COLCLAMP.U64;
		*w++ = env.PABE.U64;
		*w++ = env.BITBLTBUF.U64;
		*w++ = env.TRXDIR.U64;
		*w++ = env.TRXPOS.U64;
		*w++ = env.TRXREG.U64;

		for (int i = 0; i < 2; i++)
		{
			const GSDrawingContext& c = env.CTXT[i];
			*w++ = c.XYOFFSET.U64;
			*w++ = c.TEX0.U64;
			*w++ = c.TEX1.U64;
			*w++ = c.CLAMP.U64;
			*w++ = c.MIPTBP1.U64;
			*w++ = c.MIPTBP2.U64;
			*w++ = c.SCISSOR.U64;
			*w++ = c.ALPHA.U64;
			*w++ = c.TEST.U64;
			*w++ = c.FBA.U64;
			*w++ = c.FRAME.U64;
			*w++ = c.ZBUF.U64;
		}

		pxAssert(w == out.w + GSFeDecode::kEnvSnapshotWords);
	}
} // namespace

void GSState::FeDecodeOnDraw()
{
	GSFeDecode::DrawBody body = {};

	body.draw_serial = s_n;
	SnapshotEnv(body.draw_env, *m_draw_env);
	SnapshotEnv(body.next_env, m_env);
	std::memcpy(body.next_v, &m_v, sizeof(body.next_v));
	GSVector4i::store<false>(body.draw_rect, temp_draw_rect);
	body.backed_up_ctx = m_backed_up_ctx;
	body.dirty_gs_regs = m_dirty_gs_regs;
	body.flush_reason = static_cast<s32>(m_state_flush_reason);
	body.channel_shuffle_finish = m_channel_shuffle_finish ? 1 : 0;
	body.packed_uv_hack_flag = m_isPackedUV_HackFlag ? 1 : 0;
	body.prim = PRIM->U64;
	body.ctxt = PRIM->CTXT;
	body.vertex_head = m_vertex->head;
	body.vertex_tail = m_vertex->tail;
	body.vertex_next = m_vertex->next;
	// Renderers read both counts: the SW/HW conversion walks [0, next) while the
	// index buffer references [0, tail). Serialize the union so neither reader
	// can see a byte the stream does not carry.
	body.vertex_count = std::max(m_vertex->tail, m_vertex->next);
	body.index_count = m_index->tail;

	// The tail is two arrays; build it once so Submit sees contiguous bytes.
	const u32 vertex_bytes = body.vertex_count * static_cast<u32>(sizeof(GSVertex));
	const u32 index_bytes = body.index_count * 2;
	m_fe_decode_tail.resize(vertex_bytes + index_bytes);
	if (vertex_bytes != 0)
		std::memcpy(m_fe_decode_tail.data(), m_vertex->buff, vertex_bytes);
	if (index_bytes != 0)
		std::memcpy(m_fe_decode_tail.data() + vertex_bytes, m_index->buff, index_bytes);

	GSFeDecode::Submit(GSFeDecode::RecordType::Draw, &body, sizeof(body), m_fe_decode_tail.data(),
		static_cast<u32>(m_fe_decode_tail.size()));
}

void GSState::FeDecodeOnTransfer(const GSBackQueue::TransferRecord& rec)
{
	GSFeDecode::TransferBody body = {};

	body.blit = rec.blit.U64;
	body.env_blit = rec.env_blit.U64;
	body.pos = rec.pos.U64;
	body.reg = rec.reg.U64;
	body.draw_serial = rec.draw_serial;
	GSVector4i::store<false>(body.rect, rec.rect);
	body.len = rec.len;
	body.stat_len = rec.stat_len;
	body.end = rec.end;
	body.total = rec.total;
	body.init_x = rec.init_x;
	body.init_y = rec.init_y;
	body.first_slice = rec.first_slice ? 1 : 0;

	if (rec.payload && rec.len > 0)
	{
		const size_t len = static_cast<size_t>(rec.len);
		body.payload_digest = GSXXH3_64bits(rec.payload, len);
		std::memcpy(body.payload_head, rec.payload, std::min<size_t>(len, GSFeDecode::kPayloadHeadBytes));
	}

	GSFeDecode::SubmitFixed(GSFeDecode::RecordType::Transfer, body);
}

void GSState::FeDecodeOnMove(const GSBackQueue::MoveRecord& rec)
{
	GSFeDecode::MoveBody body = {};
	body.blit = rec.blit.U64;
	body.pos = rec.pos.U64;
	body.reg = rec.reg.U64;
	body.draw_serial = rec.draw_serial;
	GSFeDecode::SubmitFixed(GSFeDecode::RecordType::Move, body);
}

void GSState::FeDecodeOnClutLoad(const GSBackQueue::ClutLoadRecord& rec)
{
	GSFeDecode::ClutLoadBody body = {};
	body.tex0 = rec.TEX0.U64;
	body.texclut = rec.TEXCLUT.U64;
	GSFeDecode::SubmitFixed(GSFeDecode::RecordType::ClutLoad, body);
}

void GSState::FeDecodeOnLocalToHost(int len)
{
	GSFeDecode::LocalToHostBody body = {};
	body.blit = m_env.BITBLTBUF.U64;
	body.pos = m_env.TRXPOS.U64;
	body.reg = m_env.TRXREG.U64;
	body.draw_serial = s_n;
	body.len = len;
	body.total = m_tr.total;
	body.end = m_tr.end;
	GSFeDecode::SubmitFixed(GSFeDecode::RecordType::LocalToHost, body);
}

void GSState::FeDecodeOnVsync(u32 field, bool registers_written, bool idle_frame)
{
	GSFeDecode::VsyncBody body = {};
	body.frame_index = static_cast<u64>(g_perfmon.GetFrame());
	body.draw_serial = s_n;
	body.field = field;
	body.registers_written = registers_written ? 1 : 0;
	body.idle_frame = idle_frame ? 1 : 0;
	GSFeDecode::SubmitFixed(GSFeDecode::RecordType::Vsync, body);
}
