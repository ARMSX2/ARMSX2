// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"

#include <vector>

// The GS-on-GPU backend (GSHWRendererVariant::TileGpu): pass-planned indirect submission
// with VRAM truth on the GPU. Design: umbrella devs/bmdhacks/gs-on-gpu-design-brief-2026-08-17.
//
// Deliberately a plain GSRenderer subclass, not GSRendererSW — Tile inherits the software
// renderer solely for its byte-exact software floor, and this design has no floor. It still
// gets GSState's register/PCRTC/savestate machinery, and will override Transfer<n> when the
// flat front end replaces the shipping GIF decode. It reuses Tile's page/hazard models
// (GSVramModel and friends) as libraries; it does not share Tile's per-draw delivery road.
//
// Stage 1.3c: the pass planner is real. Each Draw() accumulates the frame's geometry and a
// per-draw record; VSync structures it into a GSTileGpuPassPlan (targets allocated, draws
// grouped into GS-semantic passes, the screen->NDC transform resolved per draw) and hands it
// to the device executor (ExecuteTileGpuPassPlan). GetOutput returns the display target the
// executor rendered into. Wrong-fast is the stage-1 contract — the pass boundaries are the
// ~99.4%-exact register-derived shortcut, not the full lowering (a 1.5 exactness item), and
// the executor's submission matures from clear-only to per-draw to indirect across 1.3c/1.4.
class GSRendererTileGpu final : public GSRenderer
{
public:
	GSRendererTileGpu();
	~GSRendererTileGpu() override;

protected:
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;

	// No draws yet, so no coverage-alpha (AA1) path — same reasoning as GSRendererNull:
	// without this override the base GSState version pxFailRel("Not implemented")s on AA1
	// games (e.g. Shadow of the Colossus). Revisited when the delivery structure draws.
	bool IsCoverageAlphaSupported() override;

	// The stream's non-draw memory events, observed into the pass model so the pass
	// structure is complete on titles that upload, read back, or move (draws-only misses
	// their breaks). The invalidate hooks are empty on the base — the actual transfer
	// already happened in GSState's transfer path — so these only observe. Move performs
	// the real guest copy through the base, which itself drives the two invalidate hooks.
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;

private:
	// The pass-structure observer: the round-2 GSTilePassSim model fed by the live GSState
	// decode, ported from the Tile renderer's PassSimObserveDraw feeder. Draws feed it from
	// Draw(); the stream's memory events (upload/cpu-read/move) feed it from the transfer
	// hooks below, so the pass structure is complete on every title, not just the gate one.
	// The plan this becomes (emitted to the executor once it can record) lands in a following
	// commit. Always on for this variant: it is the renderer's understanding of the frame,
	// not an optional arm, so it takes no config lever.
	GSTilePassSim m_pass_sim;

	// Set only for the duration of the base GSRenderer::Move() call. A GS->GS move reaches
	// the sim once as OnMove, which already accounts both halves; the base copy underneath
	// then fires InvalidateLocalMem(src) and InvalidateVideoMem(dst), whose sim forwards
	// would double-count the move (the destination write lands in the drawable branch of a
	// page OnMove has already marked written). The two hooks gate their sim forward on this
	// flag; their real work (none here — the bytes already moved) is unaffected.
	bool m_pass_sim_in_move = false;

	// The screen-space bbox of the current draw, scissor-clipped — the Tile renderer's
	// ComputeDrawRect, which reads only base state, replicated here (it is not a base method).
	GSVector4i ComputeDrawRect() const;

	// Feed one flushed primitive batch to the pass model: derive its page footprints,
	// classify its in-pass reads, and observe it. Fired once per Draw().
	void ObserveDraw();

	// Mean/p50 of the accumulated per-frame pass structure, emitted at teardown (the
	// aggregate the offline -tilepasssim arm reports, minus the Tile-only GIF-stream tail).
	void ReportPassStructure();

	// --- the pass plan the executor consumes --------------------------------------------
	// One row of the executor's indexed state table: the per-draw state a shader reads via
	// gl_InstanceIndex (the indirect draw's first_instance). Its layout is this backend's
	// contract with its own shader, opaque to the executor (which stages it by state_stride).
	// Wrong-fast holds only the screen->NDC transform — the HW tfx VertexScale/VertexOffset,
	// reused verbatim — plus z enables; it grows with the fixed-function state in 1.4.
	struct alignas(16) StateRow
	{
		float vertex_scale[2];  // maps raw 12.4 screen XY to clip, per the tfx VS
		float vertex_offset[2];
		u32 z_write;
		u32 z_test;
		u32 pad0;
		u32 pad1;
	};

	// One draw's inputs that cannot be resolved until the frame's targets are sized: the
	// FRAME/ZBUF identity (which target it lands in), the coordinate origin, and the draw
	// rect (which grows the target it needs). Resolved in BuildAndExecutePlan; color_slot /
	// z_slot are filled there. draw_index points at the matching m_plan_draws row.
	struct PendingDraw
	{
		u32 fb_bp, fb_psm, fb_fbw;
		u32 z_bp, z_psm;
		bool z_used;
		bool z_write, z_test;
		s32 ofx, ofy;         // XYOFFSET, 12.4 fixed
		GSVector4i rect;      // scissor-clipped draw bbox
		u32 draw_index;
		u32 color_slot;       // -> resolved color target
		u32 z_slot;           // -> resolved depth target (valid only if z_used)
	};

	// A resolved render-target identity for the frame: distinct FRAME (color) or ZBUF
	// (depth) base, its running max width/height over the draws that land in it, and the
	// GSTexture allocated for it (an index into m_plan_targets, the plan's target list).
	struct TargetSlot
	{
		u32 bp;
		u32 fbw;
		int height;
		u32 target_index;
	};

	// A displayable color target kept past plan submission so GetOutput can hand the frame's
	// display buffer (matched by FRAME base) to the presenter.
	struct DisplayTarget
	{
		u32 bp;
		GSTexture* texture;
	};

	// Per-frame accumulation (filled in Draw via AccumulateDraw, consumed + reset in VSync).
	std::vector<GSVertex> m_plan_vertices;
	std::vector<u16> m_plan_indices;
	std::vector<StateRow> m_plan_states;
	std::vector<GSDevice::GSTileGpuIndirectDraw> m_plan_draws;
	std::vector<PendingDraw> m_plan_pending;
	std::vector<GSDevice::GSTileGpuPass> m_plan_passes;
	std::vector<GSDevice::GSTileGpuTargetPair> m_plan_target_pairs;

	// Targets allocated for the frame; held across the frame so GetOutput can return one,
	// recycled at the head of the next plan build. m_display_targets indexes the colour ones.
	std::vector<GSTexture*> m_plan_targets;
	std::vector<DisplayTarget> m_display_targets;

	// Copy one flushed batch's geometry into the frame streams and record its indirect draw
	// + deferred PendingDraw. Fired from Draw() beside ObserveDraw().
	void AccumulateDraw();

	// Size the frame's targets, resolve the per-draw transform, group draws into passes, and
	// submit the assembled plan to the device executor. Fired from VSync before the base
	// presents (so the rendered targets are ready when GetOutput runs).
	void BuildAndExecutePlan();

	// Recycle the previous frame's targets back to the device pool and clear the per-frame
	// accumulation for the next frame.
	void RecycleTargets();
};
