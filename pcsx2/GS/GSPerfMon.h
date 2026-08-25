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

		// Bytes of Tile walk payload built and streamed to the device per frame
		// (BuildTilePayload's output — depth/texture/colour blocks together). The
		// operation counters cannot see a payload shrinking, so this is the column
		// that attributes a fast-profile CPU move to the payload machinery or
		// exonerates it.
		TilePayloadBytes,

		// Host waits on GPU completion that the GS thread paid OUT OF TURN — a readback's
		// submit-and-wait, an out-of-band copy's fence, an explicit sync. The command-buffer
		// ring's own recycle wait is NOT one: that is healthy backpressure, and counting it
		// would hide the number this exists to expose.
		//
		// It exists because ONE of these per frame serializes the whole pipeline: a frame with
		// any of them runs at cpu + gpu, a frame with none at max(cpu, gpu), and the count above
		// one does not matter. So the acceptance metric for the readback work is literally
		// "is this zero in steady state", which no other counter can answer — Readbacks counts
		// copies that reach the device, and a copy is not the same thing as a wait.
		GpuBlockingWaits,

		// The TileGpu per-frame depth-pass predictor (EmuCore/GS/TileGpuAdaptiveDepthPasses). All
		// three are zero unless it is engaged, which is what makes a run's arm readable from
		// stats.json without parsing an emulog: MergedFrames is how many frames were planned with the
		// depth mode OUT of the pass key, PolicySwitches how many times it changed hands (the churn
		// column -- a policy that oscillates is re-cutting every pass in the frame for nothing), and
		// PassesSaved the metric's numerator, summed per frame. Its denominator is `Draw`, already
		// counted, so the metric the decision is made on is recoverable from the pair.
		TileGpuDepthMergedFrames,
		TileGpuDepthPolicySwitches,
		TileGpuDepthPassesSaved,

		// The TileGpu GS scissor, priced. It is a vkCmdSetScissor before each indirect call.
		// ScissorDraws is the denominator, ScissorCuts the draws the scissor actually rejects part
		// of, and ScissorExtraCalls the whole price -- the boundaries where the scissor changes and
		// neither the pipeline run nor the sampled-binding key already cut.
		TileGpuScissorDraws,
		TileGpuScissorCuts,
		TileGpuScissorExtraCalls,
		TileGpuDualSrcDraws,
		TileGpuDualSrcRestore,
		TileGpuDualSrcCompanions,

		// Summed renderArea of the frame's render passes, in pixels. On a tiler that is the frame's
		// tile load-and-store bill: a pass pays for every pixel of its render area whether anything
		// drew there or not, so the area is what a pass costs and the count is not. Exactly the
		// population `RenderPasses` counts -- same two functions, both directions of the pair -- so
		// the mean area per pass is a valid division and a title's pass structure is readable from
		// stats.json without reconstructing it from a draw stream.
		RenderPassAreaPixels,

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
