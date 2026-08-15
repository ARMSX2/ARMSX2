// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSReplayPayload.h"

#include "pcsx2/GS/GSLocalMemory.h"
#include "pcsx2/GS/GSLzma.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"

#include "fmt/format.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

// Emission runs before any of the emulator's logging is stood up and returns before the
// thread that drains it exists, so the report goes straight to stderr. Routing it
// through Console silently produced nothing.
#define RP_LOG(...) (void)std::fprintf(stderr, "(GSReplayPayload) " __VA_ARGS__)

namespace GSReplayPayload
{
	namespace
	{
		// ---------------------------------------------------------------------
		// The freeze's layout.
		//
		// GSState::Freeze writes a fixed sequence of fixed-size fields and then all
		// four megabytes of local memory. GetSaveStateSize() computes the same total,
		// but it is a constexpr defined in GSState.cpp, so it is unusable from here --
		// hence these offsets. They are not trusted: the total is checked against the
		// size the dump itself records, so a field added upstream fails the emit
		// rather than silently sliding every register by eight bytes.
		//
		// Verified against all fifteen corpus dumps on 2026-08-13: version 9,
		// state_size 4194813, which is 433 + 4194304 + 76 exactly.
		// ---------------------------------------------------------------------
		constexpr u32 FREEZE_VERSION = 9;
		constexpr u32 OFF_PRIM = 4;
		constexpr u32 OFF_PRMODECONT = 12;
		constexpr u32 OFF_TEXCLUT = 20;
		constexpr u32 OFF_SCANMSK = 28;
		constexpr u32 OFF_TEXA = 36;
		constexpr u32 OFF_FOGCOL = 44;
		constexpr u32 OFF_DIMX = 52;
		constexpr u32 OFF_DTHE = 60;
		constexpr u32 OFF_COLCLAMP = 68;
		constexpr u32 OFF_PABE = 76;
		constexpr u32 OFF_BITBLTBUF = 84;
		constexpr u32 OFF_TRXPOS = 100;
		constexpr u32 OFF_TRXREG = 108;
		// CTXT[0] at 124, CTXT[1] at 220; twelve 64-bit registers each, in this order.
		constexpr u32 OFF_CTXT = 124;
		constexpr u32 CTXT_STRIDE = 96;
		constexpr u32 CTX_XYOFFSET = 0;
		constexpr u32 CTX_TEX0 = 8;
		constexpr u32 CTX_TEX1 = 16;
		constexpr u32 CTX_CLAMP = 24;
		constexpr u32 CTX_MIPTBP1 = 32;
		constexpr u32 CTX_MIPTBP2 = 40;
		constexpr u32 CTX_SCISSOR = 48;
		constexpr u32 CTX_ALPHA = 56;
		constexpr u32 CTX_TEST = 64;
		constexpr u32 CTX_FBA = 72;
		constexpr u32 CTX_FRAME = 80;
		constexpr u32 CTX_ZBUF = 88;
		// The current-vertex latches. ⚠️ UV and FOG are plain 32-bit fields of GSVertex,
		// not 64-bit registers -- getting that wrong puts every offset below here eight
		// bytes late, and the total still adds up, so the size check below cannot see it.
		// That is what the `write` sanity check exists for.
		constexpr u32 OFF_RGBAQ = 316;
		constexpr u32 OFF_ST = 324;
		constexpr u32 OFF_UV = 332;  // u32
		constexpr u32 OFF_FOG = 336; // u32, holding FOG.F -- bits 56..63 of the register
		// XYZ at 340, then an obsolete register at 348.
		// m_tr, from 356: x, y, w, h, then m_blit/m_pos/m_reg, a 16-byte rect at 396,
		// then total/start/end and the one-byte `write` that ends the block at 424 --
		// which is what makes local memory start at 425.
		constexpr u32 OFF_TR_TOTAL = 412;
		constexpr u32 OFF_TR_END = 420;
		constexpr u32 OFF_TR_WRITE = 424;
		constexpr u32 OFF_MEMORY = 425;
		constexpr u32 FREEZE_TRAILING = 84; // four GIF paths (tag + reg), then m_q
		constexpr u32 VM_BYTES = 4 * 1024 * 1024;

		// The whole of local memory addressed as one 32-bit surface: 64x32 pixels to a
		// page, sixteen pages to a page-row, thirty-two page-rows. 1024*1024*4 is 4 MB
		// exactly, and the walk is a bijection onto it -- which the emitter proves
		// rather than assumes.
		constexpr u32 MEM_W = 1024;
		constexpr u32 MEM_H = 1024;
		constexpr u32 MEM_BW = 16;
		constexpr u32 MEM_PSM = 0; // PSMCT32

		// GIF register addresses used by the environment restore.
		enum : u8
		{
			GIF_PRIM = 0x00,
			GIF_RGBAQ = 0x01,
			GIF_ST = 0x02,
			GIF_UV = 0x03,
			GIF_TEX0_1 = 0x06,
			GIF_CLAMP_1 = 0x08,
			GIF_FOG = 0x0a,
			GIF_TEX1_1 = 0x14,
			GIF_XYOFFSET_1 = 0x18,
			GIF_PRMODECONT = 0x1a,
			GIF_TEXCLUT = 0x1c,
			GIF_SCANMSK = 0x22,
			GIF_MIPTBP1_1 = 0x34,
			GIF_MIPTBP2_1 = 0x36,
			GIF_TEXA = 0x3b,
			GIF_FOGCOL = 0x3d,
			GIF_TEXFLUSH = 0x3f,
			GIF_SCISSOR_1 = 0x40,
			GIF_ALPHA_1 = 0x42,
			GIF_DIMX = 0x44,
			GIF_DTHE = 0x45,
			GIF_COLCLAMP = 0x46,
			GIF_TEST_1 = 0x47,
			GIF_PABE = 0x49,
			GIF_FBA_1 = 0x4a,
			GIF_FRAME_1 = 0x4c,
			GIF_ZBUF_1 = 0x4e,
			GIF_BITBLTBUF = 0x50,
			GIF_TRXPOS = 0x51,
			GIF_TRXREG = 0x52,
		};

#pragma pack(push, 1)
		struct Header
		{
			u32 magic; // 'GSRP'
			u32 version;
			u32 header_bytes;
			u32 flags;

			u32 mem_offset;
			u32 mem_bytes;
			u16 mem_width;
			u16 mem_height;
			u8 mem_bw;
			u8 mem_psm;
			u16 pad0;

			u32 env_offset;
			u32 env_bytes;

			u32 stream_offset;
			u32 stream_bytes;
			u32 stream_records;
			u32 checkpoints;

			u32 dump_crc;
			u32 frame_count;
		};
		// Exactly four quadwords, so the memory image that follows it starts aligned for
		// DMA. Growing the header means bumping `version`, not stealing the alignment.
		static_assert(sizeof(Header) == 64, "payload header is 64 bytes");

		struct Record
		{
			u32 kind;
			u32 bytes; // payload following this record, padded up to a qword
			u32 arg0;
			u32 arg1;
		};
		static_assert(sizeof(Record) == 16, "stream records stay qword-aligned");

		struct Checkpoint
		{
			u32 bp, bw, psm, x, y, w, h, tag;
		};
		static_assert(sizeof(Checkpoint) == 32, "checkpoint descriptor is 32 bytes");
#pragma pack(pop)

		enum : u32
		{
			REC_GIF = 0,
			REC_FIFO = 1,
			REC_VSYNC = 2,
			REC_CKPT = 3,
		};

		u64 ReadU64(const std::vector<u8>& state, u32 off)
		{
			u64 v;
			std::memcpy(&v, state.data() + off, sizeof(v));
			return v;
		}

		u32 ReadU32(const std::vector<u8>& state, u32 off)
		{
			u32 v;
			std::memcpy(&v, state.data() + off, sizeof(v));
			return v;
		}

		/// One A+D pair: the 64-bit value, then the register address.
		void PushAD(std::vector<u8>& out, u64 data, u8 addr)
		{
			const u64 hi = addr;
			const size_t at = out.size();
			out.resize(at + 16);
			std::memcpy(out.data() + at, &data, 8);
			std::memcpy(out.data() + at + 8, &hi, 8);
		}

		/// Builds the packet that puts the drawing environment back the way the freeze
		/// found it.
		///
		/// Two orderings here are load-bearing and both come from captures:
		///
		/// - TEX1 before TEX0 before MIPTBP. `gs-clut2` measured that the mip bases are
		///   derived at the TEX0 write when MTBA is set, and that the derived value
		///   lands in the MIPTBP register -- so writing MIPTBP first would be undone.
		/// - TEXFLUSH last. `gs-sync` measured that the GS has a texture cache nothing
		///   in our emulator models, and that a texture rewritten without a flush reads
		///   back every texel stale. Restoring local memory *is* rewriting every
		///   texture behind the sampler's back, so the flush is not optional.
		///
		/// Two registers are deliberately absent. TRXDIR *starts* a transfer, so
		/// replaying its stored value would kick one off against a rectangle nobody
		/// asked for; the emit refuses a dump with a transfer in flight instead. XYZ
		/// kicks a vertex, and the freeze's copy is the last vertex the game queued,
		/// not a piece of state anything downstream reads.
		std::vector<u8> BuildEnvironment(const std::vector<u8>& state)
		{
			std::vector<u8> regs;

			for (u32 c = 0; c < 2; c++)
			{
				const u32 base = OFF_CTXT + c * CTXT_STRIDE;
				const u8 ctx = static_cast<u8>(c); // context 2's registers sit one above context 1's

				PushAD(regs, ReadU64(state, base + CTX_TEX1), GIF_TEX1_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_TEX0), GIF_TEX0_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_MIPTBP1), GIF_MIPTBP1_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_MIPTBP2), GIF_MIPTBP2_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_CLAMP), GIF_CLAMP_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_XYOFFSET), GIF_XYOFFSET_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_SCISSOR), GIF_SCISSOR_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_ALPHA), GIF_ALPHA_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_TEST), GIF_TEST_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_FBA), GIF_FBA_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_FRAME), GIF_FRAME_1 + ctx);
				PushAD(regs, ReadU64(state, base + CTX_ZBUF), GIF_ZBUF_1 + ctx);
			}

			PushAD(regs, ReadU64(state, OFF_TEXCLUT), GIF_TEXCLUT);
			PushAD(regs, ReadU64(state, OFF_SCANMSK), GIF_SCANMSK);
			PushAD(regs, ReadU64(state, OFF_TEXA), GIF_TEXA);
			PushAD(regs, ReadU64(state, OFF_FOGCOL), GIF_FOGCOL);
			PushAD(regs, ReadU64(state, OFF_DIMX), GIF_DIMX);
			PushAD(regs, ReadU64(state, OFF_DTHE), GIF_DTHE);
			PushAD(regs, ReadU64(state, OFF_COLCLAMP), GIF_COLCLAMP);
			PushAD(regs, ReadU64(state, OFF_PABE), GIF_PABE);
			PushAD(regs, ReadU64(state, OFF_BITBLTBUF), GIF_BITBLTBUF);
			PushAD(regs, ReadU64(state, OFF_TRXPOS), GIF_TRXPOS);
			PushAD(regs, ReadU64(state, OFF_TRXREG), GIF_TRXREG);

			PushAD(regs, ReadU64(state, OFF_RGBAQ), GIF_RGBAQ);
			PushAD(regs, ReadU64(state, OFF_ST), GIF_ST);
			// Both of these are stored unpacked and have to go back the way the register
			// presents them: UV in the low 32 bits, FOG's eight bits up at 56.
			PushAD(regs, ReadU32(state, OFF_UV) & 0x3fff3fffu, GIF_UV);
			PushAD(regs, static_cast<u64>(ReadU32(state, OFF_FOG) & 0xffu) << 56, GIF_FOG);

			// PRMODECONT decides whether PRIM or PRMODE governs, so it goes in before
			// the primitive descriptor it qualifies.
			PushAD(regs, ReadU64(state, OFF_PRMODECONT), GIF_PRMODECONT);
			PushAD(regs, ReadU64(state, OFF_PRIM), GIF_PRIM);

			PushAD(regs, 0, GIF_TEXFLUSH);

			const u32 nloop = static_cast<u32>(regs.size() / 16);

			std::vector<u8> out;
			out.reserve(regs.size() + 16);

			// PACKED A+D: NLOOP register writes, EOP, NREG=1, REGS=0xE.
			const u64 tag_lo = static_cast<u64>(nloop) | (u64(1) << 15) | (u64(1) << 60);
			const u64 tag_hi = 0xEull;
			out.resize(16);
			std::memcpy(out.data(), &tag_lo, 8);
			std::memcpy(out.data() + 8, &tag_hi, 8);
			out.insert(out.end(), regs.begin(), regs.end());
			return out;
		}

		/// Reorders local memory into the byte order a host-to-local transfer of the
		/// whole of it expects.
		///
		/// The console then streams these bytes straight at the GIF and the addressing
		/// hardware un-permutes them. Both directions are the tables `gs-mem` certified
		/// against silicon across all thirteen formats, so the restore is
		/// self-certifying rather than hand-derived -- but only if the walk really is a
		/// bijection, which is checked here rather than assumed.
		bool PermuteMemory(const u8* vm8, std::vector<u8>& out)
		{
			out.resize(VM_BYTES);

			const u32* src = reinterpret_cast<const u32*>(vm8);
			u32* dst = reinterpret_cast<u32*>(out.data());

			std::vector<u8> seen(MEM_W * MEM_H, 0);
			u32 collisions = 0;

			for (u32 y = 0; y < MEM_H; y++)
			{
				for (u32 x = 0; x < MEM_W; x++)
				{
					const u32 addr = GSLocalMemory::PixelAddress32(
						static_cast<int>(x), static_cast<int>(y), 0, MEM_BW);
					if (addr >= MEM_W * MEM_H)
					{
						RP_LOG("pixel (%u,%u) addresses %u, past the end of memory.\n",
							x, y, addr);
						return false;
					}
					if (seen[addr]++)
						collisions++;
					dst[y * MEM_W + x] = src[addr];
				}
			}

			if (collisions != 0)
			{
				RP_LOG("the 32-bit walk over local memory is not a bijection: "
							  "%u pixels collide. Refusing to emit -- the restore would be incomplete.\n",
					collisions);
				return false;
			}

			return true;
		}

		void PushRecord(std::vector<u8>& out, u32 kind, u32 bytes, u32 arg0, u32 arg1)
		{
			Record r{kind, bytes, arg0, arg1};
			const size_t at = out.size();
			out.resize(at + sizeof(r));
			std::memcpy(out.data() + at, &r, sizeof(r));
		}

		void PushPadded(std::vector<u8>& out, const void* data, u32 bytes)
		{
			const size_t at = out.size();
			const u32 padded = (bytes + 15u) & ~15u;
			out.resize(at + padded, 0);
			if (bytes)
				std::memcpy(out.data() + at, data, bytes);
		}
	} // namespace

	bool Emit(const std::string& dump_path, const Options& opts)
	{
		Error error;
		std::unique_ptr<GSDumpFile> dump(GSDumpFile::OpenGSDump(dump_path.c_str(), &error));
		if (!dump || !dump->ReadFile(&error))
		{
			RP_LOG("cannot open '%s': %s\n", dump_path.c_str(), error.GetDescription().c_str());
			return false;
		}

		const std::vector<u8>& state = dump->GetStateData();

		// ---- the guards, before anything expensive -------------------------------
		if (state.size() < OFF_MEMORY + VM_BYTES)
		{
			RP_LOG("freeze is %zu bytes, too short to hold local memory.\n",
				state.size());
			return false;
		}

		const u32 version = ReadU32(state, 0);
		if (version != FREEZE_VERSION)
		{
			RP_LOG("freeze version %u, expected %u. The field offsets in "
						  "this file are pinned to version %u; re-derive them before emitting.",
				version, FREEZE_VERSION, FREEZE_VERSION);
			return false;
		}

		// The total is the check on every offset above it. If upstream inserts a field
		// the arithmetic stops matching and the emit stops, rather than sliding every
		// register by eight bytes and producing a payload that looks plausible.
		const u32 expected = OFF_MEMORY + VM_BYTES + FREEZE_TRAILING;
		if (state.size() != expected)
		{
			RP_LOG("freeze is %zu bytes, expected %u for version %u. "
						  "The layout has moved; the register offsets cannot be trusted.",
				state.size(), expected, version);
			return false;
		}

		// ⚠️ The size check above is necessary and not sufficient: it constrains the
		// total, and two wrong splits of that total add up just as well as the right
		// one. This session shipped exactly that mistake -- eight bytes late from
		// treating two 32-bit vertex latches as registers -- and the size still
		// matched. So check a field whose legal values are known. `write` is a bool;
		// anything but 0 or 1 means the offsets are wrong, whatever the total says.
		const u8 tr_write_raw = state[OFF_TR_WRITE];
		if (tr_write_raw > 1)
		{
			RP_LOG("the freeze's transfer-direction flag reads %u, and it is a boolean. "
				   "The field offsets are wrong -- re-derive them before emitting.\n",
				tr_write_raw);
			return false;
		}

		// A transfer still in flight at the snapshot means the restored state is one
		// nobody can interpret: the stream resumes mid-rectangle against a destination
		// we never re-armed, and the GS has no way to be told where the cursor was.
		// Refuse rather than measure it.
		const u32 tr_total = ReadU32(state, OFF_TR_TOTAL);
		const u32 tr_end = ReadU32(state, OFF_TR_END);
		if (tr_write_raw != 0 && tr_total != 0 && tr_end < tr_total)
		{
			RP_LOG("the dump was taken with a host-to-local transfer in flight "
				   "(%u of %u bytes delivered). Refusing to emit.\n",
				tr_end, tr_total);
			return false;
		}

		// ---- memory --------------------------------------------------------------
		std::vector<u8> memory;
		if (!PermuteMemory(state.data() + OFF_MEMORY, memory))
			return false;

		// ---- environment ---------------------------------------------------------
		const std::vector<u8> environment = BuildEnvironment(state);

		// ---- readback rectangle --------------------------------------------------
		const u64 frame0 = ReadU64(state, OFF_CTXT + CTX_FRAME);
		Checkpoint rb{};
		rb.bp = opts.rb_explicit ? opts.rb_bp : (static_cast<u32>(frame0 & 0x1ff) * 32);
		rb.bw = opts.rb_explicit ? opts.rb_bw : static_cast<u32>((frame0 >> 16) & 0x3f);
		rb.psm = opts.rb_explicit ? opts.rb_psm : static_cast<u32>((frame0 >> 24) & 0x3f);
		rb.x = opts.rb_x;
		rb.y = opts.rb_y;
		rb.w = opts.rb_explicit ? opts.rb_w : 640;
		rb.h = opts.rb_explicit ? opts.rb_h : 448;
		if (rb.bw == 0)
			rb.bw = 1;
		if (rb.w > rb.bw * 64)
			rb.w = rb.bw * 64;

		RP_LOG("readback: base block %u, buffer width %u, format %u, %ux%u at (%u,%u)%s\n",
			rb.bp, rb.bw, rb.psm, rb.w, rb.h, rb.x, rb.y,
			opts.rb_explicit ? " (given)" : " (from context-0 FRAME)");

		// ---- stream --------------------------------------------------------------
		std::vector<u8> stream;
		u32 records = 0;
		u32 checkpoints = 0;
		u32 frames = 0;
		u32 drawn_frames = 0;
		u64 gif_since_vsync = 0;
		u64 gif_bytes = 0;

		// Checkpoint zero, before a single packet runs: read the region back straight
		// after the restore.
		//
		// This is the control the probe discipline asks for in every measured cell, and
		// here it is nearly free. Its expected contents are already known -- they are
		// the memory image in this same payload, re-addressed -- so it says whether the
		// four-megabyte restore actually landed, using no emulator and no console
		// reference. A frame that differs is only interesting once this one does not.
		{
			Checkpoint ck = rb;
			ck.tag = 0;
			PushRecord(stream, REC_CKPT, sizeof(Checkpoint), checkpoints, 0);
			PushPadded(stream, &ck, sizeof(ck));
			checkpoints++;
			records++;
		}

		// Where a ladder rung may be placed at all.
		//
		// ⚠️ A rung is a GIF packet of our own, spliced into the recorded stream at a
		// packet boundary, and it starts a local-to-host transfer. If the GS is not
		// quiescent when it arrives, our registers are consumed as the game's data and the
		// game's following data as ours. It is deterministic, it is silent, and it does
		// not read as corruption -- it reads as the console rendering something the
		// emulator never did, which is the one thing the ladder exists to measure.
		//
		// Quiescent means two independent things, and checking only the first is what the
		// first version of this did:
		//
		//   * the GIF holds no path open -- the last tag it saw had EOP set, and no
		//     IMAGE payload ran past the packet's end; and
		//   * no image transfer is outstanding. A texture upload is sized once by TRXREG
		//     and then delivered as SEVERAL EOP-terminated IMAGE tags across several
		//     packets. Between them the path is closed and the transfer is still live, so
		//     an EOP check calls those boundaries safe and they are not. On GT4 the EOP
		//     check alone moved three rungs and the console still starved a readback two
		//     quadwords short -- the game's upload and our readback were sharing a
		//     transfer.
		//
		// Measured on GT4: two rung-free console runs are byte-identical, and a run with
		// rungs every 50 packets disagreed with them on 57% of the window by the end of
		// frame one.
		bool path_open = false;
		u32 image_carry = 0; // qwords of IMAGE payload owed by the previous packet
		u64 transfer_left = 0; // bytes of image data the live transfer still expects
		bool readback_pending = false; // a local-to-host kick the stream has not drained yet
		u32 trxreg_w = 0, trxreg_h = 0, bitbltbuf_psm = 0;
		u32 rungs_moved = 0;
		u32 frames_on_open_path = 0;
		bool ladder_deferred = false;

		// Bits per pixel of a transfer format, for sizing an image transfer. Only the
		// formats a transfer can name; anything else leaves the size unknown, which is
		// treated as "not quiescent" rather than "zero".
		const auto TransferBits = [](u32 psm) -> u32 {
			switch (psm)
			{
				case 0x00: case 0x01: return 32; // PSMCT32, PSMCT24 (24 pads to 32 in transfer)
				case 0x02: case 0x0A: return 16; // PSMCT16, PSMCT16S
				case 0x13: case 0x1B: return 8;  // PSMT8, PSMT8H
				case 0x14: case 0x24: case 0x2C: return 4; // PSMT4, PSMT4HL, PSMT4HH
				case 0x30: case 0x31: return 32; // PSMZ32, PSMZ24
				case 0x32: case 0x3A: return 16; // PSMZ16, PSMZ16S
				default: return 0;
			}
		};

		const auto TrackGifPath = [&](const u8* data, size_t length) {
			const u8* p = data;
			const u8* end = data + length;
			if (image_carry)
			{
				const size_t skip = std::min<size_t>(static_cast<size_t>(image_carry) * 16,
					static_cast<size_t>(end - p));
				p += skip;
				image_carry -= static_cast<u32>(skip / 16);
				transfer_left -= std::min<u64>(transfer_left, skip);
			}
			while (p + 16 <= end)
			{
				const u8* const tag = p;
				u64 lo, hi;
				std::memcpy(&lo, p, sizeof(lo));
				std::memcpy(&hi, p + 8, sizeof(hi));
				p += 16;
				const u32 nloop = static_cast<u32>(lo & 0x7FFF);
				const u32 flg = static_cast<u32>((lo >> 58) & 3);
				u32 nreg = static_cast<u32>((lo >> 60) & 0xF);
				if (nreg == 0)
					nreg = 16;
				path_open = ((lo >> 15) & 1) == 0;

				size_t payload = 0;
				if (flg == 0) // PACKED
					payload = static_cast<size_t>(nloop) * nreg * 16;
				else if (flg == 1) // REGLIST, padded to a quadword
					payload = ((static_cast<size_t>(nloop) * nreg * 8) + 15) & ~size_t(15);
				else if (flg == 2) // IMAGE
					payload = static_cast<size_t>(nloop) * 16;

				// The transfer registers, so an outstanding upload can be sized. Only
				// PACKED A+D carries them; a game that writes them through REGLIST would
				// leave transfer_left stale, which errs toward "not quiescent".
				if (flg == 0 && payload <= static_cast<size_t>(end - p))
				{
					const u8* q = p;
					for (u32 i = 0; i < nloop; i++)
					{
						for (u32 j = 0; j < nreg; j++, q += 16)
						{
							if (((hi >> (4 * j)) & 0xF) != 0xE) // A+D
								continue;
							u64 value, addr;
							std::memcpy(&value, q, sizeof(value));
							std::memcpy(&addr, q + 8, sizeof(addr));
							switch (addr & 0xFF)
							{
								case 0x50: // BITBLTBUF
									bitbltbuf_psm = static_cast<u32>((value >> 56) & 0x3F);
									break;
								case 0x52: // TRXREG
									trxreg_w = static_cast<u32>(value & 0xFFF);
									trxreg_h = static_cast<u32>((value >> 32) & 0xFFF);
									break;
								case 0x53: // TRXDIR -- the kick
								{
									const u32 dir = static_cast<u32>(value & 3);
									if (dir == 0 || dir == 2) // host-to-local, local-to-local
									{
										const u32 bits = TransferBits(bitbltbuf_psm);
										transfer_left = bits ? (static_cast<u64>(trxreg_w) * trxreg_h * bits + 7) / 8
															 : ~0ull; // unknown size: never quiescent
										readback_pending = false;
									}
									else if (dir == 1) // local-to-host
									{
										// The GS now holds data for the EE, and nothing in
										// the GIF stream collects it -- the dump records
										// the collection separately, as ReadFIFO2, many
										// packets later. Every boundary in between has the
										// GS already in local-to-host mode, so our own
										// readback does not start a new transfer there: it
										// finishes the game's. That is what starves it a
										// few quadwords short of what we asked for, and it
										// is the acute form of the same collision that
										// once deadlocked this replayer outright.
										readback_pending = true;
										transfer_left = 0;
									}
									else
									{
										transfer_left = 0;
										readback_pending = false;
									}
									break;
								}
								default:
									break;
							}
						}
					}
				}

				const size_t have = static_cast<size_t>(end - p);
				if (payload > have)
				{
					// Only IMAGE legitimately spans packets; anything else running past
					// the end means the walk lost sync, and a rung must not be placed on
					// a boundary this cannot vouch for.
					image_carry = (flg == 2) ? static_cast<u32>((payload - have) / 16) : 0;
					if (flg != 2)
						path_open = true;
					if (flg == 2)
						transfer_left -= std::min<u64>(transfer_left, have);
					p = end;
				}
				else
				{
					if (flg == 2)
						transfer_left -= std::min<u64>(transfer_left, payload);
					p += payload;
				}
				(void)tag;
			}
			if (image_carry)
				path_open = true;
		};

		u32 packet_index = 0;
		for (const GSDumpFile::GSData& packet : dump->GetPackets())
		{
			const u32 this_packet = packet_index++;

			switch (packet.id)
			{
				case GSDumpTypes::GSType::Transfer:
				{
					// Path is recorded but not reproduced. `gs-path` established that
					// arbitration only decides who talks when two units want the GIF at
					// once; a serialised replay has no contention, so path collapses to
					// ordering, and the stream already carries that. Kept in arg0 so a
					// later design can drive the real paths without re-emitting.
					if (packet.length == 0)
						break;

					// A wrapped PATH1 packet is recorded as the tail of VU1 memory; the
					// replayer reads it from the offset the length implies, and so do we.
					const u8* data = packet.data;
					size_t length = packet.length;
					if (packet.path == GSDumpTypes::GSTransferPath::Path1Old)
					{
						if (length > 16384)
							length = 16384;
						data = packet.data + (16384 - length);
					}

					TrackGifPath(data, length);

					PushRecord(stream, REC_GIF, static_cast<u32>(length),
						static_cast<u32>(packet.path), 0);
					PushPadded(stream, data, static_cast<u32>(length));
					gif_bytes += length;
					gif_since_vsync += length;
					records++;
					break;
				}

				case GSDumpTypes::GSType::ReadFIFO2:
				{
					// The transfer that started this is already in the GIF stream; what
					// the dump records here is how many quadwords the EE pulled out. The
					// replayer has to pull the same number or the GS is left holding a
					// transfer that the next writes would be folded into.
					u32 qwc = 0;
					if (packet.length >= sizeof(u32))
						std::memcpy(&qwc, packet.data, sizeof(u32));
					PushRecord(stream, REC_FIFO, 0, qwc, 0);
					records++;
					// The collection the local-to-host kick was waiting for. The GS is
					// free again from here.
					readback_pending = false;
					break;
				}

				case GSDumpTypes::GSType::VSync:
				{
					// Counted the way GSDumpReplayer counts, on every vsync whether or
					// not anything drew before it, so a checkpoint tag names the same
					// frame the runner's own frame dumps do. Dumps routinely open with a
					// vsync that closes a frame drawn before the recording started, so
					// the first checkpoint is often over an untouched buffer -- reported
					// below rather than quietly skipped.
					frames++;
					if (gif_since_vsync > 0)
						drawn_frames++;
					gif_since_vsync = 0;

					PushRecord(stream, REC_VSYNC, 0, frames, 0);
					records++;

					// A frame checkpoint cannot be moved -- "end of frame" is the whole
					// point of it -- so an open path here is reported instead. It has
					// the same consequence as it does for a rung, and a frame capture
					// taken across one is not an oracle.
					if (path_open || transfer_left != 0 || readback_pending)
						frames_on_open_path++;

					Checkpoint ck = rb;
					ck.tag = frames;
					PushRecord(stream, REC_CKPT, sizeof(Checkpoint), checkpoints, this_packet);
					PushPadded(stream, &ck, sizeof(ck));
					checkpoints++;
					records++;
					break;
				}

				case GSDumpTypes::GSType::Registers:
					// Privileged registers: the CRTC's business, not the rasterizer's.
					// We read the render target back directly rather than through the
					// merge circuit, so nothing here changes a drawn pixel.
					break;

				default:
					break;
			}

			// The ladder. A rung is named by the packet it follows, which is the one
			// identity the console arm and a local gsrunner run can both compute
			// without reading each other's file -- gsrunner counts the same packets in
			// the same order off the same dump.
			//
			// A vsync has already emitted one, so this does not double it.
			// A rung is due on the cadence, but it may only be placed where the GIF has
			// closed every path it opened; otherwise it is deferred to the first boundary
			// that qualifies. Deferring rather than dropping keeps the rung count roughly
			// on cadence, and the rung still names the packet it actually follows, so the
			// two arms join on a real index instead of an intended one.
			const bool rung_due = (opts.ladder_every != 0 && packet.id != GSDumpTypes::GSType::VSync &&
								   ((this_packet + 1) % opts.ladder_every) == 0);
			const bool quiescent = !path_open && transfer_left == 0 && !readback_pending;
			if (rung_due && !quiescent)
			{
				ladder_deferred = true;
				rungs_moved++;
			}
			if ((rung_due || ladder_deferred) && quiescent &&
				packet.id != GSDumpTypes::GSType::VSync)
			{
				ladder_deferred = false;
				Checkpoint ck = rb;
				ck.tag = 0x10000u + this_packet; // ladder rungs live above the frame tags
				PushRecord(stream, REC_CKPT, sizeof(Checkpoint), checkpoints, this_packet);
				PushPadded(stream, &ck, sizeof(ck));
				checkpoints++;
				records++;
			}

			if (opts.frame_limit != 0 && frames >= opts.frame_limit)
				break;
		}

		if (frames == 0)
		{
			RP_LOG("the stream contains no vsync, so nothing bounds a frame. "
				   "Refusing to emit.\n");
			return false;
		}

		// ---- write it out --------------------------------------------------------
		Header hdr{};
		hdr.magic = 0x50525347u; // 'GSRP'
		hdr.version = 1;
		hdr.header_bytes = sizeof(Header);
		hdr.flags = 1;
		hdr.mem_offset = sizeof(Header);
		hdr.mem_bytes = VM_BYTES;
		hdr.mem_width = MEM_W;
		hdr.mem_height = MEM_H;
		hdr.mem_bw = MEM_BW;
		hdr.mem_psm = MEM_PSM;
		hdr.env_offset = hdr.mem_offset + hdr.mem_bytes;
		hdr.env_bytes = static_cast<u32>(environment.size());
		hdr.stream_offset = hdr.env_offset + hdr.env_bytes;
		hdr.stream_bytes = static_cast<u32>(stream.size());
		hdr.stream_records = records;
		hdr.checkpoints = checkpoints;
		hdr.dump_crc = dump->GetCRC();
		hdr.frame_count = frames;

		auto fp = FileSystem::OpenManagedCFile(opts.output_path.c_str(), "wb", &error);
		if (!fp)
		{
			RP_LOG("cannot write '%s': %s\n", opts.output_path.c_str(), error.GetDescription().c_str());
			return false;
		}

		const bool ok =
			std::fwrite(&hdr, sizeof(hdr), 1, fp.get()) == 1 &&
			std::fwrite(memory.data(), memory.size(), 1, fp.get()) == 1 &&
			std::fwrite(environment.data(), environment.size(), 1, fp.get()) == 1 &&
			std::fwrite(stream.data(), stream.size(), 1, fp.get()) == 1;

		if (!ok)
		{
			RP_LOG("short write. The payload is incomplete; delete it.");
			return false;
		}

		const u64 total = sizeof(hdr) + memory.size() + environment.size() + stream.size();
		RP_LOG("%s: %u frames (%u with drawing), %u records, %u checkpoints "
			   "(tag 0 is the post-restore control), %llu MiB of GIF stream, %llu MiB total.\n",
			opts.output_path.c_str(), frames, drawn_frames, records, checkpoints,
			static_cast<unsigned long long>(gif_bytes >> 20),
			static_cast<unsigned long long>(total >> 20));
		RP_LOG("environment restore: %zu register writes.\n",
			(environment.size() / 16) - 1);
		if (rungs_moved)
		{
			RP_LOG("ladder: %u rungs moved off a packet that left a GIF path open. "
				   "Reported rather than silent -- a rung placed there splices our "
				   "registers into the game's transfer and the console then draws "
				   "something the emulator never did.\n",
				rungs_moved);
		}
		if (ladder_deferred)
			RP_LOG("ladder: the stream ended with a rung still owed; it was dropped.\n");
		if (frames_on_open_path)
		{
			RP_LOG("⚠️  %u of %u frame checkpoints sit on a packet that left a GIF path "
				   "open. Those frames are not oracles -- the readback is spliced into a "
				   "live transfer.\n",
				frames_on_open_path, frames);
		}

		return true;
	}
} // namespace GSReplayPayload
