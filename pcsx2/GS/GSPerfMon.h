// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <ctime>
#include <string>

class GSPerfMon
{
public:
	enum counter_t
	{
		Prim,
		Draw,
		DrawCalls,
		Readbacks,
		Swizzle,
		Unswizzle,
		Fillrate,
		SyncPoint,
		Barriers,
		RenderPasses,
		TextureCopiesROV, // Overlaps with regular texture copies.
		DrawCallsROV, // Overlaps with regular draw calls.
		BarriersROV, // Overlaps with regular barriers.

		// Texture cache lookup outcomes (HW only). Appended last so the aliased
		// slots below keep their existing indices.
		TCTargetHit,
		TCTargetMiss,
		TCSourceHit,
		TCSourceMiss,
		HashCacheHit,
		HashCacheMiss,

		// Actual pipeline binds emitted to the device (post-dedupe), not requests. On
		// tiler drivers a bind re-dirties far more than the pipeline, so this is the
		// per-draw CPU proxy the mobile renderer work steers by.
		PipelineSwitches,

		// Census scaffolding for the Adreno dynamic-state rung: of the TFX pipeline switches
		// (the selector changed between consecutive TFX draws), how many differ from the
		// previous selector ONLY in blend state, only in the colour write mask, only in both,
		// and how many differ in something else. The first three are what
		// VK_EXT_extended_dynamic_state3 could absorb without a pipeline bind. Counted per
		// selector change, so their sum is at or below PipelineSwitches, which counts emitted
		// binds of every kind including utility pipelines.
		//
		// Delete these with the rung they price.
		TFXPipelineSwitches,
		TFXPipelineSwitchesBlendOnly,
		TFXPipelineSwitchesMaskOnly,
		TFXPipelineSwitchesBlendMaskOnly,
		// The same switches counted by which part moved at all, not by what moved alone.
		// Without these a zero in the three above is unreadable: "no switch is blend-only"
		// and "the blend state never moves" look identical and mean opposite things.
		TFXPipelineSwitchesBlendMoved,
		TFXPipelineSwitchesMaskMoved,
		TFXPipelineSwitchesPSMoved, ///< the fragment shader selector moved
		TFXPipelineSwitchesRestMoved, ///< vs / depth-stencil / topology / rt / ds / feedback moved

		// Host waits on GPU completion that the GS thread paid OUT OF TURN -- a readback's
		// submit-and-wait, an explicit sync. The command-buffer ring's own recycle wait is NOT
		// one: that is healthy backpressure, and counting it would hide the number this exists
		// to expose.
		//
		// It exists because ONE of these per frame serializes the whole pipeline: a frame with
		// any of them runs at cpu + gpu, a frame with none at max(cpu, gpu), and the count above
		// one does not matter. So the acceptance metric for readback work is literally "is this
		// zero in steady state", which no other counter can answer -- Readbacks counts copies
		// that reach the device, and a copy is not the same thing as a wait.
		GpuBlockingWaits,

		// Summed renderArea of the frame's render passes, in pixels. On a tiler that is the frame's
		// tile load-and-store bill: a pass pays for every pixel of its render area whether anything
		// drew there or not, so the area is what a pass costs and the count is not. Exactly the
		// population `RenderPasses` counts -- same two functions, both directions of the pair -- so
		// the mean area per pass is a valid division and a title's pass structure is readable from
		// stats.json without reconstructing it from a draw stream.
		RenderPassAreaPixels,

		// --- Per-frame byte census (SD662 tier). Bytes, not counts.
		//
		// The MQ65's GS thread costs 3.9-10x the SD865's on the same dumps, and the split
		// sorts by what the work is rather than how much of it there is. Its bandwidth floor
		// says why that could be: write-only traffic runs a flat ~3x behind the SD865 at every
		// working-set size, read-plus-write traffic slides to 5.2x at 16 MiB and 8.75x at
		// 64 MiB. That predicts two slopes -- but only if the bytes are there, and nobody had
		// counted them. These do.
		//
		// Grouped by class, because the class is the whole point: a path that reads one buffer
		// and writes another is priced on the memcpy line, a path that only writes is priced on
		// the memset line, and a path that only reads is its own thing.

		// Read+write: source read from one place, result written to another.
		BytesGifImageIn, ///< GIF image-transfer payload read out of the packet
		BytesGifImageOut, ///< ... and written into GS local memory, swizzled
		BytesTexReadVmem, ///< texture-cache source/target: swizzled bytes read out of local memory
		BytesTexExpandOut, ///< ... and the expanded linear bytes written out (4 B/px, or 1 B/px paletted)
		BytesHashReadVmem, ///< the same read, done again by the hash cache before it knows if it needs it
		BytesHashExpandOut, ///< ... and its expanded copy
		BytesUploadRing, ///< strided memcpy of an expanded texture into the host-visible upload ring
		BytesReadbackToVmem, ///< downloaded pixels written back into local memory

		// Write-only: nothing is read to produce these bytes.
		BytesVertexStream, ///< non-temporal stores into the vertex ring
		BytesIndexStream, ///< memcpy into the index ring
		BytesUniformStream, ///< VS+PS constant-buffer pushes into the uniform rings
		BytesLocalMemClear, ///< ClearGSLocalMemory's vector stores into local memory
		BytesSwSprite, ///< the CPU sprite path writing pixels straight into local memory

		BytesGifPacket, ///< the whole GIF stream handed to GSState::Transfer, decoded into vertices

		// Read-only.
		BytesHashed, ///< bytes fed to xxh3 by the hash cache

		CounterLast,

		// Reused counters for HW.
		TextureCopies = Fillrate,
		TextureUploads = SyncPoint,

		CounterLastHW = CounterLast,
		CounterLastSW = SyncPoint + 1
	};

protected:
	double m_counters[CounterLast] = {};
	double m_stats[CounterLast] = {};
	int m_frame = 0;
	clock_t m_lastframe = 0;
	int m_count = 0;
	int m_disp_fb_sprite_blits = 0;

public:
	GSPerfMon();

	void Reset();

	void SetFrame(int frame) { m_frame = frame; }
	int GetFrame() { return m_frame; }
	void EndFrame(bool frame_only);

	void Put(counter_t c, double val) { m_counters[c] += val; }
	double GetCounter(counter_t c) { return m_counters[c]; }
	double Get(counter_t c) { return m_stats[c]; }
	void Update();

	__fi void AddDisplayFramebufferSpriteBlit() { m_disp_fb_sprite_blits++; }
	__fi int GetDisplayFramebufferSpriteBlits()
	{
		const int blits = m_disp_fb_sprite_blits;
		m_disp_fb_sprite_blits = 0;
		return blits;
	}

	GSPerfMon operator-(const GSPerfMon& other);

	void Dump(const std::string& filename, bool hw);
};

extern GSPerfMon g_perfmon;
