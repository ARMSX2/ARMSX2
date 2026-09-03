// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSLadder.h"

#include "GS/GS.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GSDumpReplayer.h"
#include "MTGS.h"

#include "common/FileSystem.h"

#include <cstdio>
#include <vector>

#define LADDER_LOG(...) (void)std::fprintf(stderr, "(GSLadder) " __VA_ARGS__)

namespace GSLadder
{
	namespace
	{
		// The console replayer's output format, byte for byte, so one decoder reads
		// both arms and no comparison has to translate between two layouts.
#pragma pack(push, 1)
		struct OutHeader
		{
			u32 magic; // 'GSRO'
			u32 version;
			u32 checkpoints;
			u32 pixel_bytes;
			u32 dump_crc;
			u32 frame_count;
			u32 stalled;
			u32 pad;
		};
		struct OutDesc
		{
			u32 bp, bw, psm, x, y, w, h, tag;
			u32 offset, bytes, status, source;
		};
#pragma pack(pop)
		static_assert(sizeof(OutHeader) == 32, "the console writes 32");
		static_assert(sizeof(OutDesc) == 48, "the console writes 48");

		constexpr u32 OUT_MAGIC = 0x4f525347; // 'GSRO'

		Options s_opts;
		bool s_armed = false;     // configured -- Finish() has a file to write
		bool s_recording = false; // still on the first pass through the stream
		u32 s_tag = 0;
		u32 s_frames = 0;
		u32 s_last_packet = 0;
		std::vector<OutDesc> s_descs;
		std::vector<u8> s_pixels;

		bool IsWordFormat(u32 psm)
		{
			// PSMCT24 shares PSMCT32's addressing and its unused byte rides along, which
			// is more information rather than less. The 16- and 4/8-bit formats would
			// need their own readers and no render target we chase uses them.
			return psm == 0x00 || psm == 0x01 || psm == 0x30 || psm == 0x31;
		}

		/// Runs on the GS thread, in order behind every packet queued before it.
		void CaptureOnGSThread(u32 packet_index, u32 tag)
		{
			GSRenderer* r = g_gs_renderer.get();
			if (!r)
				return;

			const u32 w = s_opts.w;
			const u32 h = s_opts.h;

			OutDesc d = {};
			d.bp = s_opts.bp;
			d.bw = s_opts.bw;
			d.psm = s_opts.psm;
			d.x = s_opts.x;
			d.y = s_opts.y;
			d.w = w;
			d.h = h;
			d.tag = tag;
			d.source = packet_index;
			d.offset = static_cast<u32>(s_pixels.size());
			d.bytes = w * h * 4;
			d.status = 0;

			// Retire whatever the GIF still holds, because the arm this is compared
			// against does. A rung on the console is a local-to-host transfer, and the
			// GS retires every primitive queued ahead of it before serving one; a rung
			// here is a read of the renderer's memory and retires nothing. Where a
			// packet boundary has a primitive batch still open -- which a vsync
			// routinely does, because a game kicks the next frame's first geometry
			// before the vsync packet and the batch is not flushed until something
			// forces it -- the two arms are then a whole draw apart at the same rung,
			// and the diff reads as a renderer disagreement rather than as two
			// different instants. Measured on OutRun 2006: the local rung at the vsync
			// matched the console arm's rung at the PREVIOUS one, byte for byte, and
			// the console arm's rung at the vsync matched the local arm's NEXT one.
			// DOWNLOADFIFO is the reason the GIF's own TRXDIR=1 handler uses.
			r->Flush(GSState::GSFlushReason::DOWNLOADFIFO);

			// Ask for the download a game's own readback would trigger. A no-op under
			// the software renderer, where local memory is already the result.
			GIFRegBITBLTBUF bb = {};
			bb.SBP = s_opts.bp;
			bb.SBW = s_opts.bw;
			bb.SPSM = s_opts.psm;
			r->InvalidateLocalMem(bb, GSVector4i(static_cast<int>(s_opts.x), static_cast<int>(s_opts.y),
										 static_cast<int>(s_opts.x + w), static_cast<int>(s_opts.y + h)));

			const u32* vm = r->m_mem.vm32();
			s_pixels.resize(d.offset + d.bytes);
			u32* dst = reinterpret_cast<u32*>(s_pixels.data() + d.offset);

			// Addressed the same way the emitter and the console decoder address it,
			// rather than through ReadTexture, so all three agree by construction
			// instead of by three separate readings of the same tables.
			for (u32 row = 0; row < h; row++)
			{
				for (u32 col = 0; col < w; col++)
				{
					const u32 addr = GSLocalMemory::PixelAddress32(static_cast<int>(s_opts.x + col),
						static_cast<int>(s_opts.y + row), s_opts.bp, s_opts.bw);
					dst[row * w + col] = vm[addr];
				}
			}

			s_descs.push_back(d);
		}

		void OnPacket(u32 packet_index, bool is_vsync)
		{
			if (!s_recording)
				return;

			// One pass only. The replayer wraps to packet zero and runs again when a
			// loop count is set, and a second pass would silently double every rung
			// under the same index -- a ladder that looks twice as dense and whose
			// second half compares a frame against a different frame's state.
			if (packet_index < s_last_packet)
			{
				LADDER_LOG("stream wrapped at packet %u; recording the first pass only\n", packet_index);
				s_recording = false;
				return;
			}
			s_last_packet = packet_index;

			if (is_vsync)
				s_frames++;

			const bool rung = is_vsync || (s_opts.every != 0 && ((packet_index + 1) % s_opts.every) == 0);
			if (!rung)
				return;

			const u32 tag = ++s_tag;
			MTGS::RunOnGSThread([packet_index, tag]() { CaptureOnGSThread(packet_index, tag); });
		}
	} // namespace

	bool Begin(const Options& opts)
	{
		if (opts.output_path.empty())
			return true;

		if (opts.w == 0 || opts.h == 0)
		{
			LADDER_LOG("refusing: the readback rectangle is empty\n");
			return false;
		}
		if (!IsWordFormat(opts.psm))
		{
			LADDER_LOG("refusing: format %u is not one this reads (32- and 24-bit only)\n", opts.psm);
			return false;
		}

		s_opts = opts;
		s_armed = true;
		s_recording = true;
		s_tag = 0;
		s_frames = 0;
		s_last_packet = 0;
		s_descs.clear();
		s_pixels.clear();

		// Rung zero: the same post-restore control the console takes, and for the same
		// reason. Here it says whether the dump's freeze landed; there it says whether
		// the four-megabyte restore did. A rung that differs is only interesting once
		// this one does not.
		//
		// ⚠️ It has to be taken from the initial-state hook, not from here. Arming
		// happens before the VM runs, and the freeze is applied on the first CPU step --
		// so a capture queued here reads memory that has had *nothing* put in it, and
		// then disagrees with the console at every rung for a reason that has nothing to
		// do with either renderer. That is exactly what it did on the first run.
		GSDumpReplayer::SetInitialStateHook(
			[]() { MTGS::RunOnGSThread([]() { CaptureOnGSThread(0, 0); }); });
		GSDumpReplayer::SetPacketHook(&OnPacket);

		LADDER_LOG("armed: base block %u, buffer width %u, format %u, %ux%u at (%u,%u), every %u packets\n",
			opts.bp, opts.bw, opts.psm, opts.w, opts.h, opts.x, opts.y, opts.every);
		return true;
	}

	void Finish()
	{
		if (!s_armed)
			return;

		s_armed = false;
		s_recording = false;
		GSDumpReplayer::SetPacketHook(nullptr);
		GSDumpReplayer::SetInitialStateHook(nullptr);

		// Every rung is queued behind the packets it follows, so the last of them may
		// still be in flight; draining is what makes the file complete rather than
		// nearly complete.
		MTGS::WaitGS(false, false, false);

		std::FILE* fp = FileSystem::OpenCFile(s_opts.output_path.c_str(), "wb");
		if (!fp)
		{
			LADDER_LOG("cannot open %s for writing\n", s_opts.output_path.c_str());
			return;
		}

		OutHeader oh = {};
		oh.magic = OUT_MAGIC;
		oh.version = 1;
		oh.checkpoints = static_cast<u32>(s_descs.size());
		oh.pixel_bytes = static_cast<u32>(s_pixels.size());
		// The console arm stamps this from the payload and the decoder refuses to compare
		// mismatched crcs. Writing zero here opted the local arm out of that check, so a
		// local ladder could be compared against a console run of a different dump and
		// nothing would say so -- an hour went into chasing content differences that
		// would have been one line of output.
		oh.dump_crc = GSDumpReplayer::GetDumpCRC();
		oh.frame_count = s_frames;
		oh.stalled = 0;

		std::fwrite(&oh, sizeof(oh), 1, fp);
		if (!s_descs.empty())
			std::fwrite(s_descs.data(), sizeof(OutDesc), s_descs.size(), fp);
		if (!s_pixels.empty())
			std::fwrite(s_pixels.data(), 1, s_pixels.size(), fp);
		std::fclose(fp);

		LADDER_LOG("%s: %zu rungs, %zu KiB\n", s_opts.output_path.c_str(), s_descs.size(),
			s_pixels.size() >> 10);

		s_descs.clear();
		s_pixels.clear();
	}
} // namespace GSLadder
