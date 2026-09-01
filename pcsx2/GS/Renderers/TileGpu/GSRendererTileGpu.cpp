// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSRendererTileGpu.h"

#include "GS/Renderers/TileGpu/GSTileGpuShaderVariant.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSUtil.h"
#include "GS/GS.h"

#include "common/Console.h"
#include "common/Assertions.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
// Color/depth kind from the swizzle family. The Tile renderer's KindForPsm is a file-local
// helper (anonymous namespace) there, not exported — replicated rather than linked.
GSTileSurfaceKind KindForPsm(u32 psm)
{
	switch (gsTileSwizzleFamily(psm))
	{
		case GSTileSwizzleFamily::Z32:
		case GSTileSwizzleFamily::Z16:
		case GSTileSwizzleFamily::Z16S:
			return GSTileSurfaceKind::Depth;
		default:
			return GSTileSurfaceKind::Color;
	}
}

// A zero stride degenerates every row of the page derivation; claim the whole page space
// rather than under-claim (the same conservative choice the Tile renderer's wrapper makes).
GSPageBitmap PagesForTargetRect(const GSTileSurfaceLayout& layout, const GSVector4i& r)
{
	if (layout.bw == 0)
	{
		GSPageBitmap all;
		for (u32 i = 0; i < GS_MAX_PAGES; i++)
			all.set(i);
		return all;
	}
	return GSVramModel::PagesForRect(layout, r);
}

// The bytes of a 32-bit cell one plane's data occupies under one layout — what a readback of
// that plane may write into CPU local memory (the Tile renderer's PlaneByteMask, replicated).
u32 PlaneByteMask(u32 plane_idx, const GSTileSurfaceLayout& layout)
{
	const u32 fmt_bytes = (layout.psm == PSMCT24 || layout.psm == PSMZ24) ? 0x00FFFFFFu : 0xFFFFFFFFu;
	u32 plane_bytes = 0;
	switch (1u << plane_idx)
	{
		case GSTilePlaneRGB:
			plane_bytes = 0x00FFFFFFu;
			break;
		case GSTilePlaneAlphaLow:
			plane_bytes = 0x0F000000u;
			break;
		case GSTilePlaneAlphaHigh:
			plane_bytes = 0xF0000000u;
			break;
		case GSTilePlaneZ:
			plane_bytes = 0xFFFFFFFFu;
			break;
		default:
			ASSUME(0);
	}
	return fmt_bytes & plane_bytes;
}

// Whether the target pool can back a layout: every pool format has 64-pixel-wide pages
// (the 32/16-bit colour and depth families); the 8/4-bit index formats do not, and a zero
// stride has no page geometry at all.
bool PoolSupports(const GSTileSurfaceLayout& layout)
{
	return layout.bw != 0 && gsTileStorageBpp(layout.psm) >= 16;
}

// Whether a surface can travel the byte road (writeback / seed): a colour surface in one of the
// three colour swizzle universes with a page-aligned base -- the two shaders carry a program per
// universe, and pool page (row, col) == physical page base + row*bw + col only for a page-aligned
// base. DEPTH surfaces exist in the model and have no road at all: truth that moves through one is
// counted lossy.
bool HasByteRoad(const GSTileSurfaceLayout& layout)
{
	return gsTileSurfaceHasByteRoad(layout);
}

// The narrower question the roads that REINTERPRET an owner's texture through CT32's block and
// column forms ask -- the donor build and rule 2's direct bind. They must go on refusing a 16-bit
// owner even though the byte road carries one.
//
// ⚠️ This is NOT the CLUT gather's question any more, and widening it to make it so is the single
// largest correctness trap in the 16-bit gather: the callers above would then read a 16-bit owner's
// texels out of the wrong bytes with nothing anywhere to say so. The gather asks the predicate below.
bool HasCt32PixelSpace(const GSTileSurfaceLayout& layout)
{
	return gsTileSurfaceHasCt32PixelSpace(layout);
}

// ...and the CLUT gather's own, which is the one above plus the two 16-bit colour formats. It is a
// wider question because the gather has a second copy geometry and a second entry order for them
// (LocateClutBlocks16 / ClutEntryToCt16Offset) where the roads above have neither.
bool HasClutGatherPixelSpace(const GSTileSurfaceLayout& layout)
{
	return gsTileSurfaceHasClutGatherPixelSpace(layout);
}

// Pages per texture ROW for a sampled window, in the one spelling every road has to share: the
// state row the fragment stage reads, the materialise op that builds a source out of the same
// bytes, and the donor op that reinterprets them out of a target. Two roads disagreeing here means
// one window addressing two different sets of bytes, which looks like plausible texture from the
// wrong place.
//
// GSOffset's own term, bw >> (pageShiftX - 6), which GSTileSwizzleForms::PagesPerRow reads off the
// format's page WIDTH. ⚠️ Width, never "is it paletted": PSMT8H/PSMT4HL/PSMT4HH have a palette and
// CT32's 64-texel pages, so the TBW >> 1 that PSMT8 needs would put every row but the first in the
// wrong page for them.
//
// Plus the direct-colour road's floor at one page, which is not GSOffset's. It predates this
// helper, it only ever applied to PSMCT32/PSMCT24, and a TBW=0 texture is degenerate either way --
// so it stays exactly where it was rather than being tidied into a byte-moving change.
u32 TextureWindowBwPg(u32 psm, u32 tbw)
{
	// ...and their depth twins, which are the identical 64-texel page geometry one swizzle universe
	// over. A road that differed from its colour twin only at TBW = 0 is a difference nobody would
	// ever go looking for.
	if (psm == PSMCT32 || psm == PSMCT24 || psm == PSMZ32 || psm == PSMZ24)
		return std::max<u32>(tbw, 1);
	return GSTileSwizzleForms::PagesPerRow(psm, tbw);
}
} // namespace

// Stage-1 pin: the back-record/back-thread machinery stays off for this variant whatever
// GSBackThreadMode says — its own front end is the threading story (revisited only if a
// measurement asks).
GSRendererTileGpu::GSRendererTileGpu()
	: GSRenderer(/*allow_back_records=*/false)
{
	// The pass sim is a cross-check, not a planner input: nothing the executor consumes is
	// derived from it. So it is a lever, read once here like the Tile renderer reads its own --
	// a mid-run flip needs a renderer restart.
	m_pass_sim.SetActive(GSConfig.TileGpuPassSim);

	// Asked once, before any draw: rule 2 is served by work this renderer DOES NOT DO, so a device
	// that cannot bind targets has to be known before the first read window is (not) composed.
	m_bindless_targets = g_gs_device && g_gs_device->TileGpuBindlessTargets();

	// Asked once for the same reason: it decides pass boundaries, and a boundary that moved
	// mid-frame would leave a draw's prep ops hoisted into a pass the draw is not in.
	m_depth_uniform_passes = g_gs_device && g_gs_device->TileGpuPrefersDepthUniformPasses();
	// ...and the A/B override, so a three-arm device round runs off one binary. Both keys off is the
	// shipped arm and says nothing; a contradiction is refused back to the device's own answer rather
	// than resolved by precedence, because a run whose polarity you have to derive from a rule is a
	// run whose polarity nobody will believe. Every non-default arm names itself in the emulog, which
	// is what an archived log has to carry for the numbers taken under it to be attributable.
	{
		const bool device_wants = m_depth_uniform_passes;
		const bool force_uniform = GSConfig.TileGpuForceDepthUniformPasses;
		const bool force_merged = GSConfig.TileGpuForceDepthMergedPasses;
		if (force_uniform && force_merged)
		{
			Console.Error("TileGpu: TileGpuForceDepthUniformPasses and TileGpuForceDepthMergedPasses are BOTH set; "
						  "neither is applied. Depth-pass policy stays the device's answer (%s).",
				device_wants ? "uniform" : "merged");
		}
		else if (force_uniform || force_merged)
		{
			m_depth_uniform_passes = force_uniform;
			Console.WriteLn("TileGpu: depth-pass policy FORCED to %s by settings (the device asked for %s).",
				force_uniform ? "uniform" : "merged", device_wants ? "uniform" : "merged");
		}
		else
		{
			Console.WriteLn("TileGpu: depth-pass policy is the device's answer: %s.",
				device_wants ? "uniform" : "merged");
		}
		// ...and above the device's answer but BELOW the force keys, the per-frame predictor.
		//
		// Two bits, not one, because observing and deciding are different rights. The census counts
		// both groupings of every frame and reports what the predictor WOULD have done; it moves no
		// boundary and no pixel, so it is allowed anywhere the key is on -- including on a forced arm
		// and on a device that already merges, which is the only way its thresholds can be checked
		// against a corpus off the one device they were calibrated on.
		//
		// DECIDING is scoped to where the device asked for uniform, because that is the only place a
		// corpus found the answer to be scene-dependent, and to runs with no force key set at all, a
		// contradiction included -- so a forced arm is one polarity for the whole run and nothing has
		// to be derived from a precedence rule to know which.
		m_depth_predictor_census = GSConfig.TileGpuAdaptiveDepthPasses;
		m_depth_predictor = m_depth_predictor_census && device_wants && !force_uniform && !force_merged;
		if (m_depth_predictor_census)
		{
			Console.WriteLn("TileGpu: per-frame depth-pass predictor %s (device asked for %s%s).",
				m_depth_predictor ? "DECIDING, starting uniform" : "OBSERVING only (dry run)",
				device_wants ? "uniform" : "merged",
				(force_uniform || force_merged) ? ", and a force key outranks it" : "");
		}
	}
	// ...and the pass-LENGTH policy, asked once for the same reason and independent of the polarity
	// above: the most draws one pass may hold. A pass that reaches it closes and another with the same
	// key opens -- same draws, same order, same attachments -- so this moves frame time and no pixel.
	//
	// The INI key overrides the device three ways rather than two, because "ask the device" and "force
	// uncapped" are different instructions and only a three-state key can give both on a device that
	// answers a cap. Every run names its effective cap and where it came from, which is what an
	// archived log has to carry for a four-arm crossing's numbers to be attributable to their arm.
	{
		const u32 device_answer = g_gs_device ? g_gs_device->TileGpuMaxPassDraws() : 0u;
		m_max_pass_draws = gsTileGpuMaxPassDraws(GSConfig.TileGpuMaxPassDraws, device_answer);
		// Both numbers, always, and the word that says which one won. A log line carrying only the
		// effective cap cannot distinguish "the device asked for 64" from "somebody forced 64", and
		// those are different runs.
		Console.WriteLn("TileGpu: max draws per pass %u (0 = uncapped), %s -- device answered %u, "
						"TileGpuMaxPassDraws = %d.",
			m_max_pass_draws, (GSConfig.TileGpuMaxPassDraws != 0) ? "FORCED by settings" : "the device's answer",
			device_answer, GSConfig.TileGpuMaxPassDraws);
	}
	// ...and the SPECIALIZATION bind budget, asked once beside them. Not a pass boundary -- it moves
	// no draw between passes -- but the same hazard class: it changes which fragment program a draw
	// compiles, which is worth several milliseconds on the device and nothing at all in the frame. So
	// it is named the same way, with both numbers and the word that says which one won.
	{
		const u32 device_answer = g_gs_device ? g_gs_device->TileGpuMaxSpecializationBinds() : 0u;
		m_max_spec_binds = gsTileGpuMaxSpecializationBinds(GSConfig.TileGpuMaxSpecializationBinds, device_answer);
		Console.WriteLn("TileGpu: a plan may add %u pipeline binds for fragment specialization (0 = no "
						"guard), %s -- device answered %u, TileGpuMaxSpecializationBinds = %d.",
			m_max_spec_binds,
			(GSConfig.TileGpuMaxSpecializationBinds != 0) ? "FORCED by settings" : "the device's answer",
			device_answer, GSConfig.TileGpuMaxSpecializationBinds);
	}
	// ...and the second pass-boundary policy, asked once beside it: whether a pass that declares the
	// in-pass destination read may carry draws that do not need it.
	m_segregate_self_read = g_gs_device && g_gs_device->TileGpuSegregatesSelfRead();
	// ...and the startup polarity of the density budget that bit also gates. Frame one runs before
	// any measurement exists, so it is a bet, and it is taken toward not hanging the GPU.
	m_declaring_budget.Start(m_segregate_self_read);

	// Asked once because the answer changes how a colour target is CREATED -- the read is an input
	// attachment and that usage cannot be added to an image later. Without it no draw is admitted to
	// the read, no pass declares one, and the destination-alpha test keeps its snapshot copy.
	m_self_read = g_gs_device && g_gs_device->TileGpuSelfRead();
	m_target_pool.SetColorSelfReadable(m_self_read);

	// Asked once for the same class of reason: the device compiled its fragment module at creation,
	// either with an index-1 output or without one, and a plan built under the wrong answer would
	// hand a factor to an output that is not there. Without it every draw whose blend row names As
	// takes one of the feature-free roads instead -- see GSDevice::gsTileGpuDualSrcRoad.
	m_dual_source = !g_gs_device || g_gs_device->TileGpuDualSourceBlend();
	Console.WriteLn("TileGpu: the As blend factor rides %s.",
		m_dual_source ? "the fragment stage's second colour output" : "the alpha carrier (no dual-source blending)");

	// Validation scaffolding, read once like every other lever here: admit everything the classifier
	// can name rather than only the classes whose repairs have landed. It is what lets the declared
	// read be exercised over the whole corpus before anything depends on it.
	m_force_self_read = m_self_read && GSConfig.TileGpuForceSelfRead;

	// ...and the instrument pointing the other way, read once beside it because it moves the same
	// boundaries: the classes a dev probe refuses outright, above the budget and above the forced
	// admission over it. Named in the log on the ON arm only -- a refused class changes what the
	// frame is exact about, so a number taken under this key has to be attributable to it.
	m_probe_refused_classes = gsTileGpuProbeRefusedClasses(GSConfig.TileGpuRefuseExoticBlendClass);
	if (m_probe_refused_classes != 0)
	{
		Console.WriteLn("TileGpu: PROBE -- the exotic-blend admission class is refused outright "
						"(TileGpuRefuseExoticBlendClass on). Its draws take the RT-copy blend road, so this "
						"run's frames are a measurement and not a hash gate.");
	}

	// The colour write mask served by the fragment stage instead of the pipeline, read once for the
	// reason the pass-boundary levers below are: it decides whether a draw declares, and so which pass
	// it keys into. ANDed with the device having the read road, because there is nothing to serve the
	// destination read from without one -- and said out loud, because a lever whose effect is entirely
	// in the run structure leaves no other trace in a log.
	m_shader_write_mask = m_self_read && GSConfig.TileGpuShaderWriteMask;
	if (GSConfig.TileGpuShaderWriteMask)
	{
		Console.WriteLn("TileGpu: the colour write mask rides the FRAGMENT stage%s -- draws differing only in "
						"FRAME.FBMSK share a pipeline and merge into one indirect call.",
			m_shader_write_mask ? "" : " (REFUSED: this device has no in-pass destination read)");
	}

	// The sound varies road, read once for the same reason: the split's second draw carries its own
	// depth variant, which is a pass boundary on a device that asked for depth-uniform passes. Needs
	// nothing from the device, so nothing narrows it here.
	m_afail_split = GSConfig.TileGpuAfailSplit;
	// ...and whether that second draw's depth variant is allowed to be a pass boundary at all. The
	// run key carries it either way, so this decides only where the cut is made, and it is read here
	// rather than per draw for the reason the polarity below is: both grouping sites ask it and a
	// mid-frame answer would have them disagree.
	m_split_shares_pass_key = GSConfig.TileGpuSplitSharesPassKey;
	// ...and the one class the split is refused on, read here for the same reason the two above are:
	// it decides how many plan entries a draw becomes, and the pass cap and the plan build's (j - i)
	// both count those.
	m_split_refuse_fb_only = GSConfig.TileGpuSplitRefuseFbOnly;

	// The merge's seed batch, said out loud on BOTH positions for the reason the write mask above
	// is: its whole effect is how many render passes carry the same bytes, and a pass the executor
	// opens at a pass head leaves no trace in the plan-pass census. The seed-render-pass line the
	// merge census now prints is the only place either arm can be read, and a number with no arm
	// beside it in the log is a number nobody can attribute.
	m_merge_seed_batch = GSConfig.TileGpuMergeSeedBatch;
	Console.WriteLn("TileGpu: the upload merge's seed repairs %s (TileGpuMergeSeedBatch %s) -- the blocks it may "
					"write ride %s.",
		m_merge_seed_batch ? "a BATCH of pages in one render pass" : "ONE PAGE per render pass",
		m_merge_seed_batch ? "on" : "off", m_merge_seed_batch ? "in each page's entry" : "in the op");

	// The DATE snapshot's rect, said out loud on BOTH positions for the reason the seed batch above
	// is: its whole effect is how many bytes a copy moves, and a copy leaves no trace in the plan
	// pass census. The snapshot line's page columns are the only place either arm can be read, and a
	// number with no arm beside it in the log is a number nobody can attribute.
	m_narrow_date_snapshot = GSConfig.TileGpuNarrowDateSnapshot;
	Console.WriteLn("TileGpu: a DATE pass snapshots %s (TileGpuNarrowDateSnapshot %s).",
		m_narrow_date_snapshot ? "the pages its DATE draws READ, rounded out to the page grid" :
								 "its WHOLE colour target",
		m_narrow_date_snapshot ? "on" : "off");

	// The palette-cycle substitute (EmuCore/GS/TileGpuPaletteCycleHle), and its first-release
	// GameDB gate. The gate asks the database the same question Classic asks -- does this title name
	// GSC_PolyphonyDigitalGames as its skip-count function -- BY NAME rather than by index, because
	// the index is a position in a table upstream reorders freely.
	//
	// Said out loud on every position, and with the reason for the position, because a lever whose
	// effect is that a thousand draws DON'T happen leaves no other trace in a log: a run with the
	// detector gated off and a run on a title the idiom never appears in produce identical censuses.
	{
		const s16 poly = GSLookupGetSkipCountFunctionId("GSC_PolyphonyDigitalGames");
		m_palette_cycle_gated = (poly >= 0 && GSConfig.GetSkipCountFunctionId == poly);
		m_palette_cycle_hle = GSConfig.TileGpuPaletteCycleHle && m_palette_cycle_gated;
		Console.WriteLn("TileGpu: the palette-cycle substitute is %s (TileGpuPaletteCycleHle %s, GameDB gate %s).",
			m_palette_cycle_hle ? "ARMED" : "off",
			GSConfig.TileGpuPaletteCycleHle ? "on" : "off",
			m_palette_cycle_gated ? "matched" : "not matched");
	}

	// The sixteen-bit CLUT gather, said out loud for the same reason the write mask above is: a
	// lever whose whole effect is that a readback DOESN'T happen leaves no other trace in a log, and
	// the population it serves is one title of twenty-one.
	if (GSConfig.TileGpuClut16Gather)
	{
		Console.WriteLn("TileGpu: a palette rendered into a SIXTEEN-BIT target is gathered rather than pulled "
						"(TileGpuClut16Gather) -- two 5551 cells to a word, packed at fetch.");
	}

	// ...and the CLUT gather's owner question in BLOCKS rather than in pages, said out loud for the
	// same reason: what it does is stop a readback happening, which leaves no other trace in a log.
	m_clut_block_gather = GSConfig.TileGpuClutBlockGather;
	if (m_clut_block_gather)
	{
		Console.WriteLn("TileGpu: a palette is gathered off a target that holds its OWN BLOCKS, not the whole "
						"page (TileGpuClutBlockGather) -- and the block GSState invalidates but the loader "
						"never reads is not pulled.");
	}

	// ...and the held second call, on its own key because it is not an admission question: it drops a
	// readback of guest memory the CLUT loader never reads, which is worth having wherever the gather
	// itself stands. Said out loud for the reason the others are -- what it does is stop a device
	// round trip happening, and that leaves no other trace in a log.
	m_clut_hold_second_call = GSConfig.TileGpuClutHoldSecondCall;
	if (m_clut_hold_second_call)
	{
		Console.WriteLn("TileGpu: the second block GSState invalidates for a four-bit-index CLUT load is not "
						"pulled -- the loader never reads it (TileGpuClutHoldSecondCall).");
	}

	// The upload merge's CPU-read window, read once so that every page of a session is judged by one
	// rule and a run's window can be read off its log. Said out loud on every position including the
	// default, because this is a WAIT-COUNT heuristic whose whole effect is which of two byte-
	// identical roads a spill takes -- it leaves no other trace in a log, and its two extreme
	// positions are the control arms of an A/B whose numbers are worthless if the arm is a guess.
	m_merge_cpu_read_window = GSConfig.TileGpuMergeCpuReadWindow;
	if (m_merge_cpu_read_window <= 0)
	{
		Console.WriteLn("TileGpu: a CPU read keeps a page off the upload merge for NO TIME AT ALL -- the clause "
						"never refuses (TileGpuMergeCpuReadWindow = %d).",
			GSConfig.TileGpuMergeCpuReadWindow);
	}
	else if (m_merge_cpu_read_window >= 65535)
	{
		Console.WriteLn("TileGpu: a CPU read keeps a page off the upload merge for the REST OF THE SESSION -- the "
						"mark never expires (TileGpuMergeCpuReadWindow = %d).",
			GSConfig.TileGpuMergeCpuReadWindow);
	}
	else
	{
		Console.WriteLn("TileGpu: a CPU read keeps a page off the upload merge for %d frame(s) "
						"(TileGpuMergeCpuReadWindow).",
			m_merge_cpu_read_window);
	}

	// Surface-identity containment, read once for the strongest version of the reason the others
	// are: it decides which surface a view's draws land in, and a flip mid-run would leave views
	// already folded with no surface to fall back to and views already created with no container to
	// fold into. Off, the placement still runs and still counts -- the census the fold grew out of.
	m_contain_surfaces = GSConfig.TileGpuContainSurfaces;
	if (m_contain_surfaces)
	{
		Console.WriteLn("TileGpu: surface-identity containment is ON -- one surface may hold two views of the same "
						"base%s, page budget %u (TileGpuContainPageBudget = %d).",
			kGSTileContainDisplacedViews ? " and views displaced inside it" : " (zero offset only)",
			gsTileGpuContainPageBudget(GSConfig.TileGpuContainPageBudget), GSConfig.TileGpuContainPageBudget);
	}

	// Draw reordering, read once for the reason every pass-boundary policy here is: it decides which
	// pass a draw lands in, and a value that moved mid-run would leave one frame's draws scheduled two
	// ways. Negative is the census -- the model runs and reports and moves nothing.
	//
	// The shipped setting is AUTO, so the level usually comes from the game database's
	// coalesceRenderPasses bit rather than from the INI, on the same terms as the palette-cycle
	// substitute above. Said out loud on EVERY position, and with the reason for the position,
	// because the whole effect of this lever is that a thousand render passes DON'T get opened: a
	// title the gate misses and a title with no alternation to collect produce the same census, and
	// a number with no arm beside it in the log is a number nobody can attribute.
	m_reorder_gated = GSConfig.CoalesceRenderPasses;
	m_reorder_setting = gsTileGpuReorderRuns(GSConfig.TileGpuReorderRuns, m_reorder_gated);
	m_reorder_census = m_reorder_setting < 0;
	m_reorder_active = m_reorder_setting > 0;
	m_reorder_max_runs =
		std::min(static_cast<u32>(std::abs(m_reorder_setting)), GSTileGpuReorderScheduler::kMaxRuns);
	m_reorder.Reset(m_reorder_max_runs);
	Console.WriteLn("TileGpu: draw reordering is %s (%u concurrent runs; TileGpuReorderRuns = %d, %s; "
					"GameDB gate %s).",
		m_reorder_active ? "ON" : (m_reorder_census ? "COUNTED ONLY -- nothing moves" : "off"),
		m_reorder_max_runs, GSConfig.TileGpuReorderRuns,
		(GSConfig.TileGpuReorderRuns == Pcsx2Config::GSOptions::TileGpuReorderRunsAuto) ?
			"AUTO -- the database decides" :
			"forced -- the database gets no say",
		m_reorder_gated ? "matched" : "not matched");

	// The campaign censuses this renderer owns (EmuCore/GS/TileGpuCensus), read once here for the
	// reason every pass-structure policy above is read once: the pass-break census asserts it counted
	// the same passes the plan build did, and a bit that changed between the walk and the assert
	// would fail a run that is perfectly correct.
	//
	// Announced only when something is armed. A census decides nothing and moves no pixel, so a line
	// on the OFF position would be noise in every shipped log -- but a run whose numbers were taken
	// with an instrument running has to say so, because the instrument is what this key exists to
	// price.
	m_census_pass_breaks =
		(GSConfig.TileGpuCensus & Pcsx2Config::GSOptions::TileGpuCensusPassBreaks) != 0;
	m_census_date_cover = (GSConfig.TileGpuCensus & Pcsx2Config::GSOptions::TileGpuCensusDateCover) != 0;
	m_census_palette = (GSConfig.TileGpuCensus & Pcsx2Config::GSOptions::TileGpuCensusPalette) != 0;
	if (GSConfig.TileGpuCensus != 0)
	{
		Console.WriteLn("TileGpu: censuses ARMED (TileGpuCensus = %d): pass breaks %s, DATE cover %s, "
						"variant fields %s. Instrumentation only -- nothing here decides a pixel or a pass.",
			GSConfig.TileGpuCensus, m_census_pass_breaks ? "on" : "off", m_census_date_cover ? "on" : "off",
			(GSConfig.TileGpuCensus & Pcsx2Config::GSOptions::TileGpuCensusVariantFields) != 0 ? "on" : "off");
	}

	// Arm the pin discipline. Until a cache is given a non-zero frame it behaves exactly as it does
	// for the Tile renderer, which draws immediately and needs none of this; this renderer records
	// draws and issues them at the plan, so a texture one of them names must survive to the end of
	// that plan.
	AdvanceSourcePinFrame();
}

void GSRendererTileGpu::AdvanceSourcePinFrame()
{
	m_source_frame++;
	m_tex_source.SetFrame(m_source_frame);
	m_palette_cache.SetFrame(m_source_frame);
	m_expand_cache.SetFrame(m_source_frame);
}

GSRendererTileGpu::~GSRendererTileGpu()
{
	ReportPassStructure();
}

void GSRendererTileGpu::Destroy()
{
	// Textures must go back to the device pool while the device is alive; the pool's
	// destructor only checks that they did. The source caches own device textures on the same
	// terms (none yet -- nothing builds into them at this chunk -- but the teardown order is
	// theirs whether they hold anything or not).
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	ReleaseGpuPalettes();
	// The palette-cycle substitute's source copy, on the same terms as everything above: a device
	// texture goes back to the pool while the device is alive.
	if (m_pcyc_scratch)
	{
		g_gs_device->Recycle(m_pcyc_scratch);
		m_pcyc_scratch = nullptr;
	}
	m_target_pool.ReleaseAll();
	GSRenderer::Destroy();
}

void GSRendererTileGpu::Reset(bool hardware_reset)
{
	GSRenderer::Reset(hardware_reset);
	// Everything falls back to CPU-newest (the reset rewrote local memory); surface textures
	// die with it, and so does the frame in progress. Device palettes go with the textures they
	// were gathered out of -- the CLUT RAM the reset leaves behind is the CPU's again.
	ReleaseGpuPalettes();
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
	m_surface_version.clear();
	// Containment's containers ARE those surfaces, so its table names ids that no longer mean
	// anything. The veto set goes with it: it names layouts, and the reasons it holds them for --
	// a display base, a view a depth draw named -- are facts about a draw stream the reset ended.
	m_contain_alias.clear();
	m_contain_no_offset.clear();
	m_contain_rows.clear();
	m_contain_view = kGSTileNoSurface;
	m_contain_prev_valid = false;
	// The source caches key on the model's write generations, which the reset just rewound, and
	// the probe's window record on the same. Both die with it or a replay reaching equal counts
	// under different bytes would look current.
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	m_probe_windows.clear();
	m_probe_prev.clear();
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_palettes.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_scissors.clear();
	m_plan_blend_keys.clear();
	m_plan_bind_keys.clear();
	m_plan_variant_keys.clear();
	m_plan_depth_modes.clear();
	m_plan_pending.clear();
	m_plan_write_pages.clear();
	m_plan_read_pages.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_writeback_keep_masks.clear();
	m_plan_targets.clear();
	m_plan_prep_textures.clear();
	m_plan_sources.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	m_merge_groups.clear();
	m_merge_bytes.clear();
	m_merge_words.clear();
	m_merge_emitting = false;
	m_cpu_read_frame.fill(0);
	m_frame_index = 0;
	// The pass-break census's run-level histograms. They outlive a plan -- the census's denominator
	// is the run's drawn frame count -- but not a model reset, because the surfaces whose alternation
	// the masks describe die with it.
	m_break_masks.clear();
	m_break_adreno_masks.clear();
	AdvanceSourcePinFrame();
	// The surfaces the open pass named died with the model, so its ids would alias whatever takes
	// them next.
	BreakOpenPass();
}

void GSRendererTileGpu::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Close the frame's open pass and push its stats before the base presents (the order the
	// Tile renderer's observer uses — the model is independent of presentation, but the frame
	// boundary is here).
	if (m_pass_sim.IsActive()) [[unlikely]]
		m_pass_sim.OnVSync();

	// Structure the frame's accumulated draws into a pass plan and render it through the
	// device executor, before the base presents — so the targets the executor drew into are
	// ready when the base's present path calls GetOutput. The frame boundary itself syncs
	// nothing: targets persist, and the ring the plan build consumed is gone (every synced
	// claim with it -- BuildAndExecutePlan drops them).
	MaterialiseDisplayBuffers();
	BuildAndExecutePlan();

	// The device's own frame boundary, and the reason it is a call rather than something the
	// executor works out for itself: the executor runs once per PLAN, and a mid-frame flush ends a
	// plan without ending a frame, so nothing below this line is visible from in there. The kick
	// predictor is what needs it -- it decides the next frame's submission cadence from the frame
	// that just finished, and observing it per plan ran its decay and its confirmation at the plan
	// rate. Here for the same reason the depth polarity below is here: every plan of the frame has
	// been executed and the next frame's first draw has not been accumulated.
	g_gs_device->TileGpuFrameBoundary();

	// The depth polarity for the NEXT frame, from the one that just finished. Here and nowhere else:
	// the plan is built and executed, BuildAndExecutePlan's tail has broken the open pass, and the
	// next frame's first draw has not been accumulated -- so both grouping sites will see one value
	// for every draw of a frame, which is the whole constraint on this member.
	// A frame that planned no draws is skipped entirely rather than fed as a null observation: the
	// picker treats it as neutral anyway, and counting it would put frames that rendered nothing into
	// a merged-frame share meant to say what the frames with pixels in them ran under.
	if (m_depth_predictor_census && m_frame.planned_draws != 0)
	{
		// Counted against the frame that just RAN, not the one about to: a run's merged-frame share
		// has to name frames whose pixels were produced that way. Under a dry run the picker's state
		// is what it WOULD have run, which is the number a threshold check wants.
		g_perfmon.Put(GSPerfMon::TileGpuDepthMergedFrames, m_depth_picker.merged ? 1 : 0);
		g_perfmon.Put(GSPerfMon::TileGpuDepthPassesSaved,
			static_cast<double>((m_frame.passes_uniform_key > m_frame.passes_merged_key) ?
									(m_frame.passes_uniform_key - m_frame.passes_merged_key) :
									0u));
		const u32 switches_before = m_depth_picker.switches;
		const bool merged = m_depth_picker.Observe(
			m_frame.passes_uniform_key, m_frame.passes_merged_key, m_frame.planned_draws);
		g_perfmon.Put(GSPerfMon::TileGpuDepthPolicySwitches, m_depth_picker.switches - switches_before);
		if (m_depth_predictor)
			m_depth_uniform_passes = !merged;
	}

	// The source probe's frame boundary. This frame's window record becomes what the next frame
	// compares its content stamps against, and the expanded cache's admission clock ticks -- a
	// pair first seen this frame is admitted no earlier than the next one, which is the filter
	// doing its job rather than a refusal. m_frame.src_distinct already holds the number of
	// source slots the frame handed out, counted as they were handed out.
	m_probe_prev.swap(m_probe_windows);
	m_probe_windows.clear();
	m_expand_cache.NextFrame();

	// The scissor census, out to stats.json so a run's scissor bill is readable without an emulog.
	// Unconditional: it says what the road NOT taken would have cost, so it has to be counted on
	// the road that was.
	g_perfmon.Put(GSPerfMon::TileGpuScissorDraws, static_cast<double>(m_frame.scissor_draws));
	g_perfmon.Put(GSPerfMon::TileGpuScissorCuts, static_cast<double>(m_frame.scissor_cuts));
	g_perfmon.Put(GSPerfMon::TileGpuScissorExtraCalls, static_cast<double>(m_frame.scissor_extra_calls));
	g_perfmon.Put(GSPerfMon::TileGpuDualSrcDraws, static_cast<double>(m_frame.dualsrc_draws));
	g_perfmon.Put(GSPerfMon::TileGpuDualSrcRestore, static_cast<double>(m_frame.dualsrc_restore));
	g_perfmon.Put(GSPerfMon::TileGpuDualSrcCompanions, static_cast<double>(m_frame.dualsrc_needs_comp));

	m_frame.surfaces_live = m_vram_model.LiveSurfaces();
	for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
	{
		// The EFFECTIVE verdict, probe included. On a device that does not tax declaring the budget
		// admits everything unconditionally, so without this term a probe arm's census would read
		// "refused in 0% of frames" for the class it is refusing.
		m_frame.class_refused[c] =
			((m_declaring_budget.admitted & ~m_probe_refused_classes) & (1u << c)) ? 0u : 1u;
		m_frame.class_peak[c] = m_declaring_budget.peak[c];
	}
	m_model_frames.push_back(m_frame);
	m_frame = ModelFrame{};

	// The palette census compares each CPU-road draw against the previous one of the SAME frame,
	// and counts the frame's distinct ids. Both reset here for the reason the stream itself does:
	// m_plan_palettes was handed to the executor and cleared, so nothing the next frame appends can
	// be a duplicate of anything counted above.
	m_census_pal_prev_valid = false;
	m_census_pal_ids.clear();

	// The palette-cycle run latch. A run cannot span a frame: the plan its draws would be elided out
	// of does not, and the substitute that stands for them is a plan entry.
	m_palette_cycle.Reset();

	// The containment census counts key changes WITHIN a frame, so the first draw of the next one
	// follows nothing. The alias table and the container rows deliberately do NOT reset with it:
	// the fold is sticky across frames, which is what keeps a view in one container.
	m_contain_prev_valid = false;

	// The density budget's frame boundary, taken here so the verdict is constant for every draw of a
	// frame and is decided from the frame that just finished. The counters it read reset with it.
	m_declaring_budget.Roll();
	m_budget_group = GSTileGpuPassKey{};
	m_budget_prev_wanted = 0;

	GSRenderer::VSync(field, registers_written, idle_frame);

	// Nothing consumes the transfer log yet; clear it so it cannot grow without bound.
	m_draw_transfers.clear();

	// The video-frame clock the upload merge's CPU-read marks are stamped against. Last thing in
	// VSync, so every stamp taken during a frame carries that frame's index and an age of zero
	// means "read since the last present".
	m_frame_index++;
}

void GSRendererTileGpu::Draw()
{
	if (m_pass_sim.IsActive()) [[unlikely]]
		ObserveDraw();
	AccumulateDraw();
}

// Every enabled display buffer's texture must hold the display rect before the frontend reads it.
// Two ways it can fail to: a page nothing has ever drawn into this surface (the texture rectangle
// is allocated whole, its untouched rows hold whatever the allocator left), and a page some other
// surface has taken byte truth of since (the texels here are stale even though the bytes are
// perfectly accounted for). Both are invisible to the memory model, which is about bytes, and the
// present is the one road that reads texels instead.
//
// The repair is an ordinary seed, appended to the frame's plan as a pending draw with no geometry:
// it carries the prep ops and nothing else -- the writebacks that compose the pages' bytes into the
// ring, then the seed that reads them into the texture -- so the pass runs those and draws zero
// indices. It also gives a display buffer that only the CPU ever writes -- an FMV, a menu blitted in
// by transfers -- its first surface, which is why the surface is created here rather than looked up.
//
// Fired from VSync after every draw of the frame has been accumulated and before the plan is built,
// so the pseudo-draw is always last and always breaks its pass: the writebacks below are hoisted
// over nothing but the seed they feed, and the collision test the mid-frame road needs
// (WritebackHoistCollides) has nothing to decide here.
void GSRendererTileGpu::MaterialiseDisplayBuffers()
{
	for (int i = 0; i < 2; i++)
	{
		const auto& fb = PCRTCDisplays.PCRTCDisplays[i];
		if (!fb.enabled || fb.FBW == 0)
			continue;
		const GSVector2i size = PCRTCDisplays.GetFramebufferSize(i);
		if (size.x <= 0 || size.y <= 0)
			continue;

		const GSTileSurfaceLayout disp_l{static_cast<u32>(fb.Block()), static_cast<u8>(fb.FBW),
			static_cast<u8>(fb.PSM), GSTileSurfaceKind::Color};
		if (!PoolSupports(disp_l) || !HasByteRoad(disp_l))
			continue;
		const GSVector4i rect(0, 0, size.x, size.y);
		const GSPageBitmap pages = GSVramModel::PagesForRect(disp_l, rect);
		// Design rule 9: the display base is never DISPLACED inside a container. This call takes
		// EnsureSurface's default veto, which also unfolds a view displaced before the PCRTC pointed
		// at it -- the seed below is what repairs the pages either way. GetOutput hands the frontend
		// a texture and a y_offset with no column offset beside it, and the prep-only draw below
		// carries `rect` untranslated, so a displaced display view could not be presented or seeded
		// correctly; growing those two roads for a case no corpus dump has is not worth it. A
		// zero-offset fold moves nothing and is left alone -- it is the same "a CT32 render scanned
		// out as CT24 is the same bytes" road GetOutput's base scan already takes.
		const GSTileSurfaceId id = EnsureSurface(disp_l, rect, pages);
		if (id == kGSTileNoSurface)
			continue;

		// Pages the texture spans but does not hold: never filled, or filled and since superseded.
		GSPageBitmap need = PagesNeedingSeed(id, pages, kGSTilePlanesColor) | Texels(id).filled.MissingFrom(pages);
		need &= Texels(id).pages;
		if (need.empty())
			continue;

		// The op range opens BEFORE the compose, not between it and the seed. The executor runs
		// exactly the ops each pass names -- [first_prep_op, +prep_op_count) of the pass, which
		// BuildAndExecutePlan takes from its draws -- so an op emitted before this pseudo-draw's
		// range starts is inside no pass's range at all, and is silently never run. That is what
		// the writebacks ComposeRingPages emits here were: dropped, leaving the seed below to read
		// ring slots holding nothing but their S prefill while the model counted the bytes
		// composed. Opening the range first puts the compose and the seed in one range, in the
		// order they must run.
		const u32 first_op = static_cast<u32>(m_plan_prep_ops.size());
		ComposeRingPages(need);
		EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, id, need);
		if (!AppendPrepOnlyDraw(id, rect, first_op, pages))
			continue; // nothing was emitted at all, so this bail strands nothing
		Texels(id).filled.MarkWhole(need);
		m_frame.seed_breaks++;
	}
}

// See the declaration. Both callers need the same five per-draw plan arrays appended to and the
// same pass break; the executor indexes every one of them by draw, and a short one reads the next
// draw's state -- which is exactly the trap the pass-grouping assert exists to catch.
bool GSRendererTileGpu::AppendPrepOnlyDraw(GSTileSurfaceId color, const GSVector4i& rect, u32 first_prep_op,
	const GSPageBitmap& write_pages)
{
	const u32 count = static_cast<u32>(m_plan_prep_ops.size()) - first_prep_op;
	if (count == 0)
		return false;

	// It renders nothing and exists to be a sequence point, so it is one for the reorder too: the
	// backlog goes out and it gets a run of its own. Done BEFORE the entry is pushed, for the reason
	// AccumulateDraw chooses its run early -- the ops are already emitted and they belong to this
	// pseudo-draw's pass.
	if (m_reorder_active)
	{
		m_frame.reorder_draws++;
		m_frame.reorder_ref_prep++;
		u32 emitted = 0;
		const u32 joined = m_reorder.OpenBarrierRun(GSTileGpuPassKey{}, &emitted);
		m_frame.reorder_runs += emitted;
		CarryOpenPassAcrossFlush(emitted);
		m_open_run = joined;
	}

	PendingDraw pd = {};
	pd.tex_source = kGSTileNoSurface;
	pd.tex_slot = GSDevice::GSTileGpuPassPlan::kNoTexSlot;
	pd.src_slot = kNoSourceSlot;
	pd.first_prep_op = first_prep_op;
	pd.prep_op_count = count;
	pd.color_surface = color;
	pd.z_surface = kGSTileNoSurface;
	pd.break_before = true; // the ops must land after every draw recorded so far
	pd.break_reasons = gsTileGpuBreakBit(GSTileGpuBreakCause::PrepOnly);
	pd.rect = rect;
	pd.scissor = rect;
	pd.epoch = m_epoch;

	GSDevice::GSTileGpuIndirectDraw draw = {};
	draw.instance_count = 1;
	draw.index_count = 0; // the pass exists for its prep ops
	draw.first_index = static_cast<u32>(m_plan_indices.size());
	draw.vertex_offset = static_cast<s32>(m_plan_vertices.size());
	draw.state_index = static_cast<u32>(m_plan_draws.size());
	pd.draw_index = draw.state_index;
	m_plan_draws.push_back(draw);
	m_plan_topologies.push_back(GSDevice::GSTileGpuTopology::Triangle);
	m_plan_blend_keys.push_back(0);
	// No depth attachment: this pseudo-draw exists for its prep ops and renders nothing.
	m_plan_depth_modes.push_back(GSDevice::GSTileGpuDepthMode::None);
	m_plan_pending.push_back(pd);
	// Its ops write the surface's pages and read nothing. Pushed here rather than at the two call
	// sites for the reason the five arrays above are: an append site that forgets one leaves every
	// later draw's entry shifted by one, silently.
	m_plan_write_pages.push_back(write_pages);
	m_plan_read_pages.emplace_back();
	StageDraw(draw.state_index, write_pages, GSPageBitmap());

	// This pseudo-draw broke the pass, so the tracking that answers "what has the open pass
	// already done" must reset with it, exactly as AccumulateDraw's breaks do.
	BreakOpenPass();
	return true;
}

GSTexture* GSRendererTileGpu::GetOutput(int i, float& scale, int& y_offset)
{
	// The display buffer is whichever colour surface holds the PCRTC display base's pages: the
	// exact display layout first (the composite draws it under the same FBW/PSM it is scanned
	// out with), else any live colour surface at that base (a CT32 render scanned out as CT24
	// is the same bytes). Wrong-fast: scale 1, no y offset (no upscale, no wrapped display
	// buffer yet). No surface -> nothing to present: a display buffer written only by the CPU
	// (uploads, moves) needs the byte-road present, not built yet.
	const int index = i >= 0 ? i : 1;
	const auto& fb = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i size = PCRTCDisplays.GetFramebufferSize(i);
	if (fb.framebufferRect.rempty() || fb.FBW == 0 || size.x < 0 || size.y < 0)
		return nullptr;

	const GSTileSurfaceLayout disp_l{static_cast<u32>(fb.Block()), static_cast<u8>(fb.FBW), static_cast<u8>(fb.PSM),
		GSTileSurfaceKind::Color};
	int contained_y = 0;
	GSTileSurfaceId id = m_vram_model.FindExact(disp_l);
	if (id == kGSTileNoSurface)
	{
		// ...or the surface that CONTAINS this view, which has no layout of its own to find. Rule 9
		// keeps the display base out of containment, so this is the case rule 9 cannot reach: a view
		// folded before the PCRTC pointed at it, presented in the same frame the materialise
		// unfolds it. Serviceable exactly when the fold has no column offset, which is what
		// y_offset is -- the same number Classic derives from its own page delta. A column offset
		// has nowhere to go, so it falls through to the base scan below and then to a black frame,
		// which is what this returned before it consulted the table at all.
		//
		// While kGSTileContainDisplacedViews is false every fold is at (0, 0), so this road can only
		// ever hand back the container at y_offset 0 -- the same bytes under a different PSM, which
		// is what the base scan below would have found anyway. It is the displaced road's arm and it
		// stays, because it is the half of the present that would be wrong without it.
		const auto alias = m_contain_alias.find(disp_l.pack());
		if (alias != m_contain_alias.end() && alias->second.x_off == 0 &&
			m_vram_model.Get(alias->second.id).alive)
		{
			id = alias->second.id;
			contained_y = alias->second.y_off;
		}
	}
	if (id == kGSTileNoSurface)
	{
		for (u32 k = 0; k < m_vram_model.SurfaceSlots(); k++)
		{
			const GSVramModel::Surface& s = m_vram_model.Get(static_cast<GSTileSurfaceId>(k));
			if (s.alive && s.layout.kind == GSTileSurfaceKind::Color && s.layout.bp == disp_l.bp)
			{
				id = static_cast<GSTileSurfaceId>(k);
				break;
			}
		}
	}
	if (id == kGSTileNoSurface)
		return nullptr;

	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	if (!surf.pool_handle)
		return nullptr;
	scale = 1.0f;
	y_offset = contained_y;
	GSTexture* t = m_target_pool.GetTexture(surf.pool_handle);

	// -dump f, the same hook Classic and the software renderer carry, in the same call at the same
	// point of it: the display buffer as the presenter is about to receive it. Without this the
	// flag was a silent no-op on this renderer, so a device A/B had no pixels to compare.
	//
	// Named off the PCRTC display's own BP and PSM, not off the surface actually found -- a
	// container, or a CT32 render scanned out as CT24, is still the same display unit, and the
	// comparison only works if both renderers write the same filename for the same frame. The
	// .bmp is Classic's too and is not a mistake: GSTexture::Save rewrites the extension and
	// writes PNG. Changing it here would break the parity this exists for.
	//
	// GSTexture::Save reads the image back, which stalls the device. It is reached only with
	// dumping on; with it off this is one bool test, GSConfig.SaveFrame, on a call that runs
	// twice a frame. The pixels are finished by the time it runs -- VSync builds and executes
	// this frame's plan before the base's present path calls GetOutput.
	if (GSConfig.SaveFrame && GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		t->Save(GetDrawDumpPath("%05lld_f%05lld_fr%d_%05x_%s.bmp", s_n, g_perfmon.GetFrame(), i,
			static_cast<int>(fb.Block()), GSUtil::GetPSMName(fb.PSM)));
	}

	return t;
}

bool GSRendererTileGpu::IsCoverageAlphaSupported()
{
	return false;
}

// A host->local transfer's destination. Fires BEFORE GSState writes the shadow, so: (1) any
// live ring slot for the pages captures the page's current bytes as a version copy and closes,
// keeping every already-planned reader on the bytes it saw (the "version" upload mechanism);
// (2) blocks only a target holds newest that this write only PARTIALLY overwrites are reconciled
// first -- on the GPU where the merge can be planned, by draining the target to the shadow where
// it cannot (the whole-block/whole-page common case shrinks or clears the target's claim for
// free); (3) the model records the CPU write, except on the pages the merge already accounted
// for. Order matters: closing the slots first also drops their synced claims, so the spill
// question is asked against truth the CPU shadow does NOT hold (a claim the ring vouched for is
// not one S can shrink against). Footprints are exact through GSOffset, DBW=0 included (the
// address math folds every row onto one page column, which is what the hardware does with it).
void GSRendererTileGpu::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.DBP, static_cast<u8>(BITBLTBUF.DBW),
		static_cast<u8>(BITBLTBUF.DPSM), KindForPsm(BITBLTBUF.DPSM)};
	const u8 planes = gsTilePlanesInvalidatedByWrite(BITBLTBUF.DPSM);
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(layout, r, fp);
	SupersedeRingSlots(fp.pages);
	GSPageBitmap merged;
	if (!m_vram_model.SpillBeforeCpuWrite(fp, planes).empty())
	{
		// This write partially overwrites blocks only a target holds, and what has to be reconciled
		// is the spill set a FLUSH would leave, not the one standing here: a flush retires every
		// synced claim the ring vouched for (those slots are spent, and their bytes were never in
		// S), and retiring a claim can only ADD pages. Answer with the ordinary question on this
		// side and the pages whose claim the flush retired go unreconciled, after which OnCpuWrite
		// below clears truth the CPU shadow never received -- silent in Release, its synced assert
		// in Devel.
		//
		// The synced-IGNORED question IS that far-side set, asked from here: it is exactly what the
		// ordinary form returns after a flush (FlushPendingPlan's tail calls ClearAllSynced, so
		// there are no claims left to subtract over there) and a superset of what it returns now.
		// So plan the merge against it. If the merge takes every page of it, nothing is left for
		// the readback -- and with the readback empty the flush has no consumer at all: it never
		// widens the set it is asked for (0 of 1,386 merge-road entries on Spider-Man 3), it feeds
		// a readback that pulls nothing, and it orders nothing, because EmitUploadMerge's ops go to
		// AppendPrepOnlyDraw, whose break_before already cuts the pass behind every recorded draw
		// whether the plan ends here or not. Emit on the plan as it stands and skip it
		// (EmuCore/GS/TileGpuFlushGateUploadMerge). Anything left over takes today's road below,
		// unchanged.
		//
		// The ENTRY condition above stays the NARROW question deliberately. The synced-ignored set
		// is a superset, so entering on it would admit writes today's road does not touch -- they
		// are the same set on this corpus, and keeping the narrow test is what makes that a
		// measurement rather than an assumption.
		//
		// Decided BEFORE the readback, EMITTED after it, on both roads. The readback's own plan
		// flush must not carry merge ops: their ring slot is prefilled from the CPU shadow AT
		// PLAN-BUILD TIME, and GSState has not written this transfer into it yet, so a plan built
		// between here and the write would compose the page as it stands BEFORE the transfer and
		// seed that into the target -- losing the upload entirely.
		//
		// ...and the elision is BOUNDED BY SIZE. Skipping the flush is free per merge and priced per
		// PLAN, in the executor: the whole staging block is ONE reservation against the 32 MiB vram
		// stream buffer, so a plan's ring pages ARE its reservation size, and its epoch count is
		// both the rest of that reservation and all of the table-fill work. Past either budget the
		// merge takes the road below instead, which makes that flush a plan BOUNDARY rather than a
		// reconciliation -- it reconciles nothing the elision needed, it just ends the plan. See
		// kGSTileGpuMergeElisionMaxEpochs / kGSTileGpuMergeElisionMaxRingPages for the measurements
		// the two numbers come from. A boundary can never cost more flushes than the pre-lever road:
		// both budgets are only ever asked here, on a write that road would have flushed anyway.
		//
		// ...and the probe is not planned where its answer is already known. PlanUploadMerge settles
		// three questions before it looks at a page -- the merge road's own config key, whether
		// GSState handed us the whole rect, and whether the footprint kept its block masks -- and all
		// three are properties of this WRITE, so they can be asked here for free. Where any of them
		// refuses, the probe is planned, refused whole, thrown away and planned again below, and the
		// road it takes is the fallback either way.
		//
		// SKIPPING IS A PROOF, NOT A HEURISTIC, and the proof is one line: the ignore-synced spill is
		// a SUPERSET of the ordinary one (same per-page test, one fewer set subtracted from the
		// candidates), the entry condition above just proved the ordinary one non-empty, so the wide
		// set is non-empty -- and `wide.andnot(empty).empty()` on a non-empty set is false. The probe
		// cannot be accepted. Nothing downstream can tell the difference: the counters it would have
		// touched are restored on that road anyway, and PlanUploadMerge clears the same three scratch
		// vectors at its head when the fallback calls it.
		//
		// Priced, because a gate that saves nothing is worth knowing about: the corpus makes 218.37
		// probes a drawn frame and throws 11.00 of them away, 9.00 of those for the whole-rect
		// question this skips (OutRun 2006 A 5.00, B 4.00; the other 2.00 are Yu-Gi-Oh's, per-page
		// answers no cheap test can predict). A thrown-away probe costs 54 ns on the M2 measured
		// amplified 64x against a 14 ns clock floor -- so this is 0.3 us a drawn frame on OutRun,
		// and it is NOT the 0.91 ms of gs_cpu that title loses to the elision on the SD865. The
		// probe was priced looking for that regression and is three orders of magnitude too small
		// to be it.
		const bool over_epochs = m_epoch >= kGSTileGpuMergeElisionMaxEpochs;
		const bool over_pages = m_ring_entries.size() >= kGSTileGpuMergeElisionMaxRingPages;
		bool gated = false;
		if (GSConfig.TileGpuFlushGateUploadMerge && !over_epochs && !over_pages &&
			UploadMergeServesThisWrite(fp))
		{
			// The probe's refusals are provisional. PlanUploadMerge counts as it decides, and a
			// probe this branch discards is planned again below over the same pages, so put the
			// counters back and let the road that actually runs be the one that records.
			const ModelFrame::MergeRefusals refusals = m_frame.merge_ref;
			const GSPageBitmap wide = m_vram_model.SpillBeforeCpuWrite(fp, planes, /*ignore_synced=*/true);
			const GSPageBitmap probe = PlanUploadMerge(layout, r, fp, wide);
			gated = wide.andnot(probe).empty();
			if (gated)
			{
				merged = probe;
				EmitUploadMerge();
			}
			else
			{
				m_frame.merge_ref = refusals;
			}
		}
		if (!gated)
		{
			// Today's road, and everything is asked again on the far side of the flush: the probe
			// above is only trustworthy where it is accepted whole, because PlanUploadMerge reads
			// the source-pin clock, the texel records and the pool handles, and the flush's tail
			// moves all three.
			if (GSConfig.TileGpuFlushGateUploadMerge)
			{
				m_frame.merge_bound_epochs += over_epochs ? 1u : 0u;
				m_frame.merge_bound_pages += over_pages ? 1u : 0u;
			}
			FlushPendingPlan();
			const GSPageBitmap spill = m_vram_model.SpillBeforeCpuWrite(fp, planes);
			merged = PlanUploadMerge(layout, r, fp, spill);
			ReadbackToShadow(spill.andnot(merged), StallSite::UploadSubBlock);
			EmitUploadMerge();
		}
	}
	// The merged pages' write is already accounted for: the ring carried its bytes into the owner's
	// texture and the owner still holds the page's newest bytes, so nothing about truth moves. Every
	// other page takes the ordinary clear-or-shrink.
	GSVramModel::RectFootprint cpu_fp = fp;
	cpu_fp.pages = fp.pages.andnot(merged);
	m_vram_model.OnCpuWrite(cpu_fp, planes);
	m_vram_model.OnCpuWriteServedOnGpu(merged);
	if (m_pass_sim.IsActive() && !m_pass_sim_in_move) [[unlikely]]
		m_pass_sim.OnUpload(PagesForTargetRect(layout, r));
}

// A local->host readback's source (or a CLUT load's): a CPU consumer of the pages. Anything a
// target holds newest comes down first -- the genuine crossing, and on this road a stall. The
// clut flag splits the on-GPU palette-gather road from a genuine readback in the pass model.
//
// A CLUT load's blocks get one more question first: can the palette be read off the GPU instead?
// Where it can, the readback -- and the plan flush in front of it -- does not happen at all, which
// is the whole prize (the flush count and the CLUT stall count are numerically the same population
// on the two GT4 dumps). PreClutLoad reads the deferred blocks back after all if the load's shape
// turns out to be one the gather cannot serve.
void GSRendererTileGpu::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	const GSPageBitmap pages = GSVramModel::PagesForRect(layout, r);
	if (!clut || !ClutLoadDefer(layout, r, pages))
	{
		// Bracketed on the clut road so the census counts the same population `stalls[Clut]` does.
		// "Did a target hold these pages" is the question the gather has to ask (it is the only one
		// the plan flush cannot change the answer to), but it is not the same question as "did a
		// readback happen": a page whose bytes the ring already vouches for answers yes to the first
		// and no to the second, and counting those as refusals would inflate the refused column by
		// twenty a frame on GT4 for loads that cost nothing.
		const u32 before = m_frame.stalls[static_cast<u32>(StallSite::Clut)];
		ReadbackToShadow(layout, r, pages, clut ? StallSite::Clut : StallSite::LocalRead);
		if (clut && m_frame.stalls[static_cast<u32>(StallSite::Clut)] != before) [[unlikely]]
			m_clut_pending.readback = true;
	}
	if (m_pass_sim.IsActive() && !m_pass_sim_in_move) [[unlikely]]
		m_pass_sim.OnCpuRead(PagesForTargetRect(layout, r), clut);
}

// -- the CLUT gather: a palette loaded off a render target ------------------------------------

void GSRendererTileGpu::ClutLoadSlots(const GIFRegTEX0& TEX0, u32& first, u32& count)
{
	const u32 csa = TEX0.CSA & 15;
	first = csa;
	count = (GSLocalMemory::m_psm[TEX0.PSM].pal == 256) ? (16 - csa) : 1;
}

bool GSRendererTileGpu::ClutGatherServesLoad(const GIFRegTEX0& TEX0)
{
	const u32 pal = GSLocalMemory::m_psm[TEX0.PSM].pal;
	if (TEX0.CSM != 0 || (TEX0.CPSM & 0x2) != 0 || pal == 0)
		return false;
	return pal == 16 || TEX0.CSA == 0;
}

bool GSRendererTileGpu::ClutGatherServes()
{
	if (!m_clut_gather_probed)
	{
		m_clut_gather_probed = true;
		bool clut_ok = false;
		// The entry -> word order is a fitted form with its own validity bit: a device whose CLUT
		// forms did not fit must not gather, even where the block forms did. The renderer's own fit
		// is the same function over the same tables -- it exists because the block copy's source
		// rects are a CPU-side answer no shader computes.
		const bool forms_ok = g_gs_device && g_gs_device->TileSwizzleFormsFit(clut_ok);
		m_clut_forms = GSTileSwizzleForms::Fit();
		m_clut_gather_serves = forms_ok && clut_ok && m_clut_forms.valid && m_clut_forms.clut_valid;
		// The merged copy's consumer is a MODULE arm, not just a state-row mode: the session's
		// fragment modules were assembled once, from the setting as it stood then. So the question
		// is "does the shader that will read this have the arm", not "is the setting on now" --
		// a mode-3 row in front of a module without the arm would read the palette in the wrong
		// order. False also means the lever is simply off, which is the default.
		m_clut_merge_serves = g_gs_device && g_gs_device->TileGpuClutMergeCompiled();
		// ...and the page merge needs one more arm on top of that -- the per-draw stride. A module
		// with the square arm and not the stride would read a page-merged palette at 16 instead of
		// 64, which is a plausible-looking palette made of the wrong words.
		m_clut_merge_pages_serves = m_clut_merge_serves && g_gs_device->TileGpuClutMergePagesCompiled();
		// ...and the SIXTEEN-BIT owner's road. Not a module question, unlike the two above: its
		// fetch arm rides the per-draw variant key (kGSTileGpuTexelPalGather16), which this renderer
		// sets itself on exactly the draws it gives a mode-4/5 row, so the row and the program cannot
		// disagree. What it does need is its own fitted inverses -- the CT16 block tables, which are
		// a different fit from the CT32 ones and carry their own validity bit, so a loader change
		// that stopped them fitting turns off this road alone.
		m_clut16_serves = m_clut_gather_serves && GSConfig.TileGpuClut16Gather && m_clut_forms.clut16_valid;
		// Deliberately NOT gated on bindless targets, unlike every other road that reads one: the
		// consumer that carries 88% of the population is an image-to-buffer COPY into the frame's
		// palette stream, which asks the device for nothing. Rule 3's N x 1 gather does need the
		// source array, and it is already unreachable without it (ProbeSourceRoad is the gate).
	}
	return m_clut_gather_serves;
}

// One block of a CLUT load, asked the donor road's own owner questions about the pages it is about
// to pull down. The clause bundle is the design probe's, which is DonorForTextureRead's asked of a
// palette's source instead of a texture window: one live owner for every plane of every page,
// holding a pool texture, a page-aligned CT32/CT24 colour layout, resident, with texels actually
// written there, and the palette at or after the owner's base.
//
// ⚠️ The first question is the cheap one and it has to stay that way. sotc runs ~1789 CLUT loads a
// frame and not one of them needs anything; the whole road must cost those loads a single member
// check. It also has to be a question the plan flush cannot change its answer to -- "does any target
// hold these pages newest" is exactly that, where "does a readback need to happen" is not (a synced
// claim the ring vouches for dies with the flush, which is why the design probe had to measure from
// inside ReadbackToShadow and this does not).
// One block of a CLUT load, asked the SAME owner questions in BLOCK units, and folded into the
// pending load's running answer. The whole point is the unit: a palette lives in four blocks (or
// one, or two) of a page whose other twenty-eight the CPU may well own, and the page-granular
// question then says "drain the page" about bytes the load never reads.
//
// Two things it does that the page-granular road does not. It runs for EVERY block of the load,
// including after that road has latched a refusal -- the answer is over the whole block set, and a
// load half of which is one owner's and half another's is a mixed-owner refusal here just as it is
// there. And the texels test is narrowed the same way as the truth test, because the two are read
// against each other: "the rest of the page is allocator leftovers" is not an answer about the
// blocks this load reads.
void GSRendererTileGpu::ProbeClutBlockOwner(const GSVramModel::RectFootprint& fp, const GSPageBitmap& pages)
{
	ClutPendingLoad& p = m_clut_pending;
	const u32 j = p.calls - 1; // p.calls was bumped on entry, and call j covers block CBP + j
	if (j >= ClutPendingLoad::kMaxClutCalls)
		return;

	GSVramModel::SoleOwnerRefusal why = GSVramModel::SoleOwnerRefusal::kServed;
	const GSTileSurfaceId owner = m_vram_model.SoleGpuOwnerOfBlocks(fp, kGSTilePlanesAll, why);

	u32 next;
	if (owner == kGSTileNoSurface)
	{
		next = kClutRefMulti;
		p.call_cause[j] = static_cast<u8>(why);
	}
	else
	{
		// ⚠️ A VERBATIM copy of the clauses below the owner question in ClutLoadDefer, narrowed to
		// blocks where the unit differs, and they have to stay one. What it answers is "what would
		// this call meet if the owner question were asked in blocks", which is only worth asking
		// while the two spellings agree. The MIXED-OWNER clause is not here: whether two calls
		// disagree is a question about the load, and which calls are part of the load is not settled
		// until TEX0 arrives.
		const GSVramModel::Surface& s = m_vram_model.Get(owner);
		if (!s.alive || !s.pool_handle)
			next = kClutRefDead;
		else if (m_clut16_serves ? !HasClutGatherPixelSpace(s.layout) : !HasCt32PixelSpace(s.layout))
			next = kClutRefLayout;
		else if (!s.residency.contains(pages))
			next = kClutRefResidency;
		else if (m_surface_texels.size() <= owner || !m_surface_texels[owner].filled.HoldsBlocks(fp))
			next = kClutRefTexels;
		else
			next = kClutRefNone;
		p.call_owner[j] = owner;
		p.call_psm[j] = static_cast<u16>(s.layout.psm);
	}
	p.call_ref[j] = static_cast<u8>(next);
}

u32 GSRendererTileGpu::ResolveClutBlockGather(const ClutPendingLoad& p, u32 blocks, u32& owner_psm, u32& cause)
{
	// ClutPendingLoad's per-call arrays are brace-initialised with the numbers, because a member
	// array cannot name an enumerator declared after it. These pin the numbers.
	static_assert(kClutRefMulti == 1, "ClutPendingLoad::call_ref's default must stay kClutRefMulti");
	static_assert(static_cast<u32>(GSVramModel::SoleOwnerRefusal::kNoOwner) == 2,
		"ClutPendingLoad::call_cause's default must stay kNoOwner");

	owner_psm = 0;
	cause = static_cast<u32>(GSVramModel::SoleOwnerRefusal::kServed);
	if (blocks == 0 || blocks > p.calls || blocks > ClutPendingLoad::kMaxClutCalls)
		return kClutRefMulti; // a block somebody reads that no call ever classified

	GSTileSurfaceId sole = kGSTileNoSurface;
	for (u32 j = 0; j < blocks; j++)
	{
		if (p.call_ref[j] != kClutRefNone)
		{
			cause = p.call_cause[j];
			return p.call_ref[j];
		}
		if (sole == kGSTileNoSurface)
		{
			sole = p.call_owner[j];
			owner_psm = p.call_psm[j];
		}
		else if (p.call_owner[j] != sole)
		{
			// Two of the load's own blocks off different targets. The copy road names one owner per
			// record, so this is the multi-owner serve and not this one.
			owner_psm = 0;
			return kClutRefMixedOwner;
		}
	}
	return kClutRefNone;
}

// The page-granular owner clauses: the road as it stood before TileGpuClutBlockGather, kept whole
// so the lever's off arm is the same code and not a re-derivation of it. Returns kClutRefNone when
// every clause holds, and fills `owner` with the surface that would serve the load.
u32 GSRendererTileGpu::ClutOwnerRefusalForPages(const GSPageBitmap& pages, GSTileSurfaceId& owner)
{
	ClutPendingLoad& p = m_clut_pending;
	GSVramModel::SoleOwnerRefusal why = GSVramModel::SoleOwnerRefusal::kServed;
	owner = m_vram_model.SoleGpuOwner(pages, kGSTilePlanesAll, why);
	if (owner == kGSTileNoSurface)
	{
		p.multi_cause = static_cast<u32>(why);
		return kClutRefMulti;
	}

	const GSVramModel::Surface& s = m_vram_model.Get(owner);
	if (!s.alive || !s.pool_handle)
		return kClutRefDead;
	if (m_clut16_serves ? !HasClutGatherPixelSpace(s.layout) : !HasCt32PixelSpace(s.layout))
	{
		// Which of the three tests refused, carried to PreClutLoad where the total is counted.
		//
		// ⚠️ Always the NARROW predicate's clause, whichever predicate decided above. Its contract is
		// that it returns None exactly when the narrow predicate is true, and the census is
		// denominated in it -- so on the wide road the psm bucket becomes the population this lever
		// SERVED rather than the one it refused, and the reading of the counter changes with the
		// lever rather than the counter's own meaning drifting.
		p.layout_clause = gsTileSurfaceCt32PixelSpaceClause(s.layout);
		pxAssert(p.layout_clause != kGSTileCt32ClauseNone);
		// On the PSM test -- a page-aligned colour owner in a 16-bit format, the one population a
		// wider gather could take -- keep going and record which of the clauses below would have
		// refused next. Census only: the refusal is already settled and none of this changes it.
		//
		// ⚠️ The three tests below are a VERBATIM copy of the three that follow this if, and have to
		// stay one. The question they answer is "what would this load meet if the layout clause
		// admitted it", which is only worth asking while the two spellings agree.
		if (p.layout_clause == kGSTileCt32ClausePsm)
		{
			if (!s.residency.contains(pages))
				p.psm_next_clause = kClutRefResidency;
			else if (m_surface_texels.size() <= owner || !m_surface_texels[owner].filled.HoldsWhole(pages))
				p.psm_next_clause = kClutRefTexels;
			else if (p.deferred_blocks != 0 && p.owner != owner)
				p.psm_next_clause = kClutRefMixedOwner;
			else
				p.psm_next_clause = kClutRefNone;
		}
		return kClutRefLayout;
	}
	if (!s.residency.contains(pages))
		return kClutRefResidency;
	if (m_surface_texels.size() <= owner || !m_surface_texels[owner].filled.HoldsWhole(pages))
		return kClutRefTexels;
	if (p.deferred_blocks != 0 && p.owner != owner)
		return kClutRefMixedOwner;
	return kClutRefNone;
}

bool GSRendererTileGpu::ClutLoadDefer(const GSTileSurfaceLayout& layout, const GSVector4i& r, const GSPageBitmap& pages)
{
	ClutPendingLoad& p = m_clut_pending;
	p.calls++;
	if (pages.empty() || (pages & m_vram_model.TruthAny()).empty())
		return false; // the CPU shadow is authoritative here; today's road early-outs on the same test
	p.stalling = true;
	if (!ClutGatherServes())
		return false;
	// GSState makes at most four calls for one load (GSState.cpp's `blocks`). More would mean the
	// per-call verdicts no longer describe the load, so the whole thing falls to today's road.
	const u32 j = p.calls - 1;
	if (j >= ClutPendingLoad::kMaxClutCalls) [[unlikely]]
	{
		p.refused = true;
		p.refusal = kClutRefMulti;
		return false;
	}

	// The BLOCK-granular question, and the census that sizes it. Below the cheap early-out because
	// building a footprint is not free and sotc's ~1789 loads a frame must keep costing one bitmap
	// AND; above the refusal latch because the answer is over the load's WHOLE block set and the
	// page-granular road stops asking after its first refusal.
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(layout, r, fp);
	ProbeClutBlockOwner(fp, pages);

	// THE SECOND CALL IS HELD, and most of the lever's prize is here. GSState invalidates two blocks
	// for a four-bit-index CSM1 palette and the loader reads sixteen entries -- 64 bytes -- out of
	// the FIRST one, so the second is over-invalidation nothing reads. TEX0 has not arrived yet and
	// the load could equally be an eight-bit one whose four calls are all the palette's, so the
	// verdict is recorded and nothing is decided: no refusal, and no readback taken. Deferring a
	// readback is always safe -- truth does not move, so any later reader of those bytes still pulls
	// them. SettleHeldClutCall finishes it once TEX0 says which.
	if (m_clut_hold_second_call && j == 1)
	{
		p.call1_held = true;
		p.call1_layout = layout;
		p.call1_rect = r;
		return true;
	}

	if (p.refused)
		return false;

	u32 refusal;
	GSTileSurfaceId owner;
	if (m_clut_block_gather)
	{
		// The owner question in BLOCKS. ProbeClutBlockOwner asked it and every clause below it just
		// above, so this reads that verdict rather than spelling the clauses a second time.
		refusal = p.call_ref[j];
		owner = p.call_owner[j];
		if (refusal == kClutRefMulti)
			p.multi_cause = p.call_cause[j];
		else if (refusal == kClutRefNone && p.deferred_blocks != 0 && p.owner != owner)
			refusal = kClutRefMixedOwner;
	}
	else
	{
		refusal = ClutOwnerRefusalForPages(pages, owner);
	}
	if (refusal != kClutRefNone)
	{
		p.refused = true;
		p.refusal = refusal;
		return false;
	}

	if (p.deferred_blocks == 0)
	{
		const GSVramModel::Surface& s = m_vram_model.Get(owner);
		p.owner = owner;
		p.owner_bp = s.layout.bp;
		p.owner_bwpg = s.layout.bw;
		// Taken here, with the rest of the owner's shape, because the surface may be replaced before
		// the copy is emitted and the format is what says which of the two copy geometries the record
		// takes. A mixed-owner load never gets this far (the clause above refuses it), so one psm
		// describes the whole load.
		p.owner_psm = s.layout.psm;
	}
	p.pages |= pages;
	p.deferred_blocks++;
	return true;
}

// The held second call, settled. TEX0 finally says how many of the blocks GSState invalidated the
// loader will read, and that is the only thing the call was waiting for: a four-bit-index CSM1 load
// reads sixteen entries out of the block at CBP and nothing else, so the block at CBP+1 that GSState
// named alongside it is read by nobody. It is dropped -- no refusal, and no readback, which is where
// GT4 Online Public Beta's whole CLUT stall count lives. See gsTileGpuClutBlocksRead.
//
// Any other shape does include it: an eight-bit index reads all four of its blocks, and a load the
// gather does not serve at all reads whatever GSState said it would. So the call rejoins the load,
// its verdict decides with the rest, and its readback is taken here instead of at the call.
//
// ⚠️ WHOSE VERDICT depends on which question the rest of the load was asked. The two levers are
// independent -- the hold ships on and the block gather ships off -- and a held call admitted on the
// block question inside a page-question load would be the block gather leaking in by the back door,
// which is exactly the thing a lever that ships off is supposed to prevent.
void GSRendererTileGpu::SettleHeldClutCall(const GIFRegTEX0& TEX0)
{
	ClutPendingLoad& p = m_clut_pending;
	if (!p.call1_held)
		return;
	p.call1_held = false;
	const u32 read_blocks = gsTileGpuClutBlocksRead(
		TEX0.CSM, GSLocalMemory::m_psm[TEX0.CPSM].trbpp, GSLocalMemory::m_psm[TEX0.PSM].trbpp);
	if (read_blocks < 2)
		return; // nothing reads it

	const GSPageBitmap pages = GSVramModel::PagesForRect(p.call1_layout, p.call1_rect);
	if (!p.refused)
	{
		GSTileSurfaceId owner = p.call_owner[1];
		u32 refusal;
		if (m_clut_block_gather)
		{
			refusal = p.call_ref[1];
			if (refusal == kClutRefMulti)
				p.multi_cause = p.call_cause[1];
		}
		else
		{
			refusal = ClutOwnerRefusalForPages(pages, owner);
		}

		if (refusal != kClutRefNone)
		{
			p.refused = true;
			p.refusal = refusal;
		}
		else if (p.deferred_blocks != 0 && p.owner != owner)
		{
			p.refused = true;
			p.refusal = kClutRefMixedOwner;
		}
		else
		{
			if (p.deferred_blocks == 0)
			{
				const GSVramModel::Surface& s = m_vram_model.Get(owner);
				p.owner = owner;
				p.owner_bp = s.layout.bp;
				p.owner_bwpg = s.layout.bw;
				p.owner_psm = s.layout.psm;
			}
			p.pages |= pages;
			p.deferred_blocks++;
		}
	}
	if (p.refused)
	{
		// The readback ClutLoadDefer did not take. Block-refined, exactly as the call would have
		// taken it: this is the same road, one step later.
		const u32 before = m_frame.stalls[static_cast<u32>(StallSite::Clut)];
		ReadbackToShadow(p.call1_layout, p.call1_rect, pages, StallSite::Clut);
		if (m_frame.stalls[static_cast<u32>(StallSite::Clut)] != before)
			p.readback = true;
	}
}

// The load is about to run against the CPU's local memory. Two things have to be settled first: a
// load of a shape the sixteen-slot mirror does not model rewrites the CLUT RAM in units it cannot
// name, so the RAM is made whole before it; and blocks whose readback was deferred come back after
// all when the whole load is not one the gather serves, because then the loader below needs them.
void GSRendererTileGpu::PreClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	ClutPendingLoad& p = m_clut_pending;
	m_frame.clut_loads++;

	// Before anything else that can pull, so the held call's readback keeps the place in the order it
	// would have had at the call.
	SettleHeldClutCall(TEX0);

	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;
	if (!csm1_32 && m_clut_mirror.AnyGpu()) [[unlikely]]
		SyncClutToCpu();

	if (p.deferred_blocks != 0 && (p.refused || !ClutGatherServesLoad(TEX0)))
	{
		// Part of the load is deferrable and part is not, or the shape is not one the gather can
		// serve: the whole load takes the CPU route and the deferred blocks come down now.
		const u32 before = m_frame.stalls[static_cast<u32>(StallSite::Clut)];
		ReadbackToShadow(p.pages, StallSite::Clut);
		if (m_frame.stalls[static_cast<u32>(StallSite::Clut)] != before)
			p.readback = true;
		p.deferred_blocks = 0;
	}
	if (p.deferred_blocks != 0)
		return; // PostClutLoad turns it into a record, and counts it there

	// The census, over the population `stalls[Clut]` counts: a load that cost nothing is not a
	// refusal of anything, and on the titles the gather never serves it is every load there is.
	if (!p.readback)
		m_frame.clut_no_stall++;
	else if (p.refusal != kClutRefNone)
	{
		m_frame.clut_ref_owner++;
		m_frame.clut_ro[p.refusal < kClutRefCount ? p.refusal : 0u]++;
		// The multi/partial bucket, split into its three causes and priced against the block-granular
		// question. One counter over three populations is what made this bucket unsizeable: only one
		// of the three is a granularity question, and it is the only one anything cheap can serve.
		if (p.refusal == kClutRefMulti)
		{
			m_frame.clut_multi_cause[p.multi_cause < 4 ? p.multi_cause : 0u]++;

			// Asked over the blocks GSState INVALIDATED, which is the unit the road speaks in today.
			u32 psm = 0, cause = 0;
			const u32 inval = ResolveClutBlockGather(p, p.calls, psm, cause);
			m_frame.clut_multi_next[inval < kClutRefCount ? inval : 0u]++;
			if (inval == kClutRefMulti)
				m_frame.clut_block_cause[cause < 4 ? cause : 0u]++;
			if (inval == kClutRefNone && (psm == PSMCT16 || psm == PSMCT16S))
				m_frame.clut_multi_ct16++;

			// ...and over the blocks the LOADER READS. A CSM1 32-bit load reads one block for sixteen
			// entries (64 of its 256 bytes) and four for 256; GSState invalidates two and four. So the
			// two columns differ by one block on four-bit-index loads, and that block is the whole
			// difference. Only asked of a shape the gather serves at all -- for the others the block
			// set is not CBP..CBP+n and the answer would be meaningless.
			if (!ClutGatherServesLoad(TEX0))
			{
				m_frame.clut_loader_shape++;
			}
			else
			{
				const u32 read_blocks = (GSLocalMemory::m_psm[TEX0.PSM].pal == 256) ? 4u : 1u;
				const u32 loader = ResolveClutBlockGather(p, read_blocks, psm, cause);
				m_frame.clut_loader_next[loader < kClutRefCount ? loader : 0u]++;
				if (loader == kClutRefNone && (psm == PSMCT16 || psm == PSMCT16S))
					m_frame.clut_loader_ct16++;
			}
		}
		// The layout clause's own split, a strict partition of clut_ro[kClutRefLayout].
		if (p.refusal == kClutRefLayout && p.layout_clause < kGSTileCt32ClauseCount)
		{
			m_frame.clut_rol[p.layout_clause]++;
			// ...and the PSM bucket split again by the load's own shape. TEX0 is the first place the
			// entry count and CBP are known -- ClutLoadDefer sees blocks, not loads -- which is why
			// this sits here rather than beside the clause that produced it.
			if (p.layout_clause == kGSTileCt32ClausePsm)
			{
				const u32 entries = GSLocalMemory::m_psm[TEX0.PSM].pal;
				m_frame.clut_rol16[(entries == 256) ? 1u : 0u][TEX0.CBP & 3u]++;
				m_frame.clut_rol16_next[p.psm_next_clause < kClutRefCount ? p.psm_next_clause : 0u]++;
			}
		}
	}
	else
	{
		// The load's own shape is not one the gather serves (CSM2, a 16-bit palette, an eight-bit
		// index at a non-zero CSA) -- or the device does not serve the road at all.
		m_frame.clut_ref_shape++;
	}
}

// The load has happened. TEX0 names its shape and the mirror slots it wrote, so this is where the
// pending classification becomes either a device palette or an ordinary CPU one.
void GSRendererTileGpu::PostClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	const ClutPendingLoad p = m_clut_pending;
	m_clut_pending = ClutPendingLoad{};

	// Nothing on the device and nothing deferred: the mirror is all-CPU and stays that way, so the
	// whole road costs this load one branch. That is the case on every title the gather never serves
	// -- sotc's 1789 loads a frame, xenosaga's 1321 -- and it has to stay the case.
	if (m_gpu_palettes.empty() && p.deferred_blocks == 0)
		return;

	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;
	if (!csm1_32)
	{
		// PreClutLoad made the RAM whole; the load then rewrote it from CPU bytes, in units the
		// mirror cannot name, so every slot is the CPU's again.
		m_clut_mirror.OnCpuLoad(0, GSTileClutMirror::kSlots);
		RefreshClutLive();
		return;
	}

	u32 first = 0, count = 0;
	ClutLoadSlots(TEX0, first, count);
	if (p.deferred_blocks == 0)
	{
		m_clut_mirror.OnCpuLoad(first, count);
		RefreshClutLive();
		return;
	}

	// The palette lives on the device from here: the CPU's CLUT RAM holds whatever the loader just
	// read out of a stale shadow for these slots, and the mirror says so. Fresh record per load, no
	// lookup structure -- measured, not assumed: on both prize titles every eligible load's identity
	// tuple is new, so a cache could not hit.
	GpuPalette gp;
	gp.handle = m_gpu_palette_next++;
	gp.owner = p.owner;
	gp.cbp = TEX0.CBP;
	gp.entries = GSLocalMemory::m_psm[TEX0.PSM].pal;
	gp.first_slot = first;
	gp.owner_bp = p.owner_bp;
	gp.owner_bwpg = p.owner_bwpg;
	gp.owner_psm = p.owner_psm;
	gp.pages = p.pages;
	gp.tex0 = TEX0;
	gp.texclut = TEXCLUT;
	// The identity: (owner surface x owner version x CBP x entries), namespace-tagged. The version
	// is the same counter the donor road's target token keys on, which is what makes "this palette's
	// words" mean the same thing to both roads.
	u64 h = 0xCBF29CE484222325ull;
	for (const u64 part : {static_cast<u64>(p.owner), SurfaceVersion(p.owner), static_cast<u64>(gp.cbp),
			 static_cast<u64>(gp.entries)})
	{
		h ^= part;
		h *= 0x100000001B3ull;
	}
	gp.pal_id = GSTilePaletteCache::ForeignPalId(h);

	m_gpu_palettes.push_back(gp);
	m_clut_mirror.OnGpuLoad(first, count, gp.handle);
	RefreshClutLive();
	m_frame.clut_gathered++;
	// ...and the sixteen-bit owners on their own, which is TileGpuClut16Gather's whole population.
	// Zero on every title that never renders a palette into a 16-bit target, which on this corpus is
	// every one but Spider-Man 3 and (at a quarter of a load a frame) legosw.
	if (gp.owner_psm == PSMCT16 || gp.owner_psm == PSMCT16S)
		m_frame.clut16_gathered++;
}

GSRendererTileGpu::GpuPalette* GSRendererTileGpu::FindGpuPalette(u32 handle)
{
	// Handles only ever increase and pruning preserves order, so the vector is sorted by handle.
	const auto it = std::lower_bound(m_gpu_palettes.begin(), m_gpu_palettes.end(), handle,
		[](const GpuPalette& gp, u32 h) { return gp.handle < h; });
	return (it != m_gpu_palettes.end() && it->handle == handle) ? &*it : nullptr;
}

void GSRendererTileGpu::RefreshClutLive()
{
	m_clut_live.clear();
	if (m_gpu_palettes.empty())
		return;
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		const u32 handle = m_clut_mirror.SlotHandle(s);
		if (handle == GSTileClutMirror::kCpu)
			continue;
		const GpuPalette* gp = FindGpuPalette(handle);
		if (!gp)
			continue;
		const u32 idx = static_cast<u32>(gp - m_gpu_palettes.data());
		if (std::find(m_clut_live.begin(), m_clut_live.end(), idx) == m_clut_live.end())
			m_clut_live.push_back(idx);
	}
}

// A pool texture's pages are ABOUT TO BE written -- by a draw claiming them, or by a seed queued
// just after this. Every live palette gathered out of those pages is losing the only copy of its
// words, so it is CAPTURED first: its blocks are copied into the frame's palette stream now, from
// the op position ahead of the write, and the rest of the plan reads them from there.
//
// It has to be eager rather than at the next consumer, and the corpus said so before the design
// did: GT4 renders its palettes into the frame buffer it then draws over, so a record loses its
// source mid-plan while draws that still read it are yet to come. The capture is a transfer at a
// pass head, not a pass, so paying it per record is affordable in a way a gather pass is not.
//
// No hoist test here, and none is needed: the FIRST write to a record's pages is what runs this, so
// no draw of the open pass can already have written them -- if one had, this would have run then.
// The same question NoteClutSourceWritten asks, asked without deciding anything: which live palette
// record's source pages does this draw's write footprint meet? It is the first conjunct of the
// palette-cycle signature -- "this draw overwrites the memory its own palette came from".
//
// The owner clause is a PREFERENCE here and a requirement there, deliberately. NoteClutSourceWritten
// destroys a record and must only destroy one the writing surface actually owns; this one is
// answering a census question as well as a decision, and "a record's pages were written by a draw
// into a DIFFERENT surface" is a diagnosis worth being able to read off the funnel rather than one
// worth hiding behind a null.
const GSRendererTileGpu::GpuPalette* GSRendererTileGpu::FindClutSourceWrittenBy(
	GSTileSurfaceId id, const GSPageBitmap& pages) const
{
	const GpuPalette* other = nullptr;
	for (const u32 idx : m_clut_live)
	{
		const GpuPalette& gp = m_gpu_palettes[idx];
		if (gp.source_lost || !gp.pages.intersects(pages))
			continue;
		if (gp.owner == id)
			return &gp;
		if (!other)
			other = &gp;
	}
	return other;
}

// The palette cycle's substitute: one device pass standing in for the whole run the elision drops,
// semantically matched to Classic's GSC_PolyphonyDigitalGames.
//
// WHAT CLASSIC DOES, and this is the part that is easy to get backwards. Its HLE draw has the SAME
// texture as render target and as source -- `src`, the target its TEX0 names -- and applies a
// per-channel palette lookup to it IN PLACE. It does not write the shuffle's FRAME at all; the
// shuffle's writes are the thing being skipped. So the substitute's subject is the image the run
// READS, and the ramp is that image's brightness curve. Measured: eliding gt4opb's run with nothing
// in its place takes the frame from mean 49.9 to 36.4 against Classic's 86.5, and gt4 -- where
// un-elided TileGpu already matches Classic to within 4 levels -- from 105.6 to 91.5. The run is a
// BRIGHTENING pass, and dropping it darkens the picture by about what the ramp adds.
//
// It is a prep op and not a plan draw. It needs no state row, no vertex stream and no fragment
// variant, and GSTileGpuPassPlan's variant key has no bits left for one -- while the prep stream is
// exactly the place TileGpu already puts "a device operation on a surface". Three ops go out
// together, in the order the executor runs them: the CLUT gather that builds the palette as an N x 1
// image, the channel fetch itself, and the prep-only pseudo-draw that gives the pair a home in the
// plan (an op emitted into no draw's range is silently never executed).
//
// ⚠️ The claims are HERE and not in the elision, because this is the draw that writes. The rule the
// elision states is that the ledger is told what actually gets written; the substitute writes the
// source surface's rect, so the source surface claims it.
bool GSRendererTileGpu::IssuePaletteCycleSubstitute(
	const PendingDraw& pd, GSTileSurfaceId fb_id, const GSVector4i& rect, const GIFRegTEX0& tex0)
{
	(void)rect;
	// Classic picks its variant off FBMSK: 0x00FFFFFF is the alpha-destination shuffle, which
	// extracts each channel into its own page-sized target, and everything else is the in-place
	// brightness ramp. Only the ramp is served here -- the three-target variant needs three
	// destinations this road does not name. Asked FIRST, before anything is allocated or queued: a
	// refusal below this line would strand the ops it had already emitted.
	if (gsTilePaletteCycleIsAlphaSplit(m_palette_cycle.fbmsk()))
		return false;
	if (pd.pal_record == 0)
		return false;
	GpuPalette* const gp = FindGpuPalette(pd.pal_record);
	if (!gp)
		return false;

	// The subject: the live target that owns the whole window this draw READS. Classic's
	// LookupDrawTarget(RTEX0) in the terms this renderer states ownership in -- and asked with the
	// same all-planes GPU-truth question the donor road asks, so a window the byte store owns, or one
	// split across two targets, is refused rather than half-served.
	const GSTileSurfaceLayout tex_l{tex0.TBP0, static_cast<u8>(std::max<u32>(tex0.TBW, 1)),
		static_cast<u8>(tex0.PSM), KindForPsm(tex0.PSM)};
	const u32 tw = 1u << std::min<u32>(tex0.TW, 10);
	const u32 th = 1u << std::min<u32>(tex0.TH, 10);
	const GSPageBitmap win =
		GSVramModel::PagesForRect(tex_l, GSVector4i(0, 0, static_cast<int>(tw), static_cast<int>(th)));
	const GSTileSurfaceId src = m_vram_model.SoleGpuOwner(win, kGSTilePlanesAll);
	if (src == kGSTileNoSurface || src == fb_id)
	{
		// No single target holds the run's source, or it is the very surface the run writes -- and
		// then the substitute would be reading what the elision refused to produce.
		return false;
	}
	const GSVramModel::Surface& surf = m_vram_model.Get(src);
	if (!surf.alive || !surf.pool_handle)
		return false;
	GSTexture* const src_tex = m_target_pool.GetTexture(surf.pool_handle);
	if (!src_tex)
		return false;
	const GSVector2i size = src_tex->GetSize();
	const GSVector4i sub_rect = GSVector4i::loadh(size);
	if (sub_rect.rempty())
		return false;

	// The palette, as the N x 1 image rule 3 already knows how to build. It reads the owner's texture
	// at the pass head, and it is queued BEFORE the fetch, so the gather sees the palette's texels
	// whatever the fetch goes on to overwrite.
	PendingDraw sub_pd = pd;
	GSTexture* const pal_tex = ClutGatherTexture(*gp, sub_pd);
	if (!pal_tex)
		return false;

	// The scratch the executor copies the destination into before sampling it: a render pass may not
	// sample the image it writes. One per renderer, grown to the largest subject a session has seen,
	// because a substitute is once or twice a frame and an allocation per substitute would churn the
	// pool for nothing.
	if (m_pcyc_scratch && (m_pcyc_scratch->GetWidth() < size.x || m_pcyc_scratch->GetHeight() < size.y))
	{
		g_gs_device->Recycle(m_pcyc_scratch);
		m_pcyc_scratch = nullptr;
	}
	if (!m_pcyc_scratch)
		m_pcyc_scratch = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false, true);
	if (!m_pcyc_scratch)
		return false;

	const u32 first_op = sub_pd.first_prep_op;
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::ChannelFetch;
	op.target = PlanTargetIndex(src);
	op.index_texture = PrepTextureIndex(m_pcyc_scratch);
	op.palette_texture = PrepTextureIndex(pal_tex);
	op.chan_mode = 0; // RGB (the FBMSK variant was decided at the top, before anything was queued)
	op.copy_x[0] = static_cast<u32>(sub_rect.x);
	op.copy_y[0] = static_cast<u32>(sub_rect.y);
	op.copy_w = static_cast<u32>(sub_rect.width());
	op.copy_h = static_cast<u32>(sub_rect.height());
	m_plan_prep_ops.push_back(op);

	// The pages the substitute writes, in the subject's own layout. Its residency is what the surface
	// covers; the write is the whole of it, which is what Classic's `src->GetUnscaledRect()` is.
	const GSPageBitmap sub_pages = GSVramModel::PagesForRect(surf.layout, sub_rect) & surf.residency;
	if (!AppendPrepOnlyDraw(src, sub_rect, first_op, sub_pages))
		return false;

	// The claims: this pass really did write those pages, out of the subject's own pixels, so the
	// ledger is told exactly that. NoteClutSourceWritten last, for the reason the ordinary claim
	// block runs it last -- a palette gathered off these very pages is now gone, and its words were
	// captured by the gather queued above.
	m_vram_model.OnNativeDraw(src, sub_pages, kGSTilePlanesAll);
	BumpSurfaceVersion(src);
	NoteClutSourceWritten(src, sub_pages);
	m_frame.pcyc_subs++;
	m_frame.pcyc_sub_pages += sub_pages.count();
	return true;
}

void GSRendererTileGpu::NoteClutSourceWritten(GSTileSurfaceId id, const GSPageBitmap& pages)
{
	if (m_clut_live.empty()) [[likely]]
		return;
	for (const u32 idx : m_clut_live)
	{
		GpuPalette& gp = m_gpu_palettes[idx];
		if (gp.source_lost || gp.owner != id || !gp.pages.intersects(pages))
			continue;
		CaptureClutRecordWords(gp);
		gp.source_lost = true;
	}
}

// TileGpuClutMergePages. See the declaration for why this is a plan-build pass and not a capture-side
// one -- the short version is that the capture site can only ever see one 256-entry record at a time.
//
// What it looks for is a CONSECUTIVE run of merged-square copy ops that: pull from one owner, sit in
// one guest page, reserved a contiguous run of 256-word slots, and cover whole rows of that page's
// 4x2 grid of squares. Such a run becomes one 64x16 or 64x32 region writing exactly the words the run
// reserved, and every draw that read one of those palettes is re-pointed at its square's origin
// inside the region, at stride 64.
//
// Consecutive is load-bearing three times over. It is what makes the reservations contiguous, so the
// region's write lands inside the run and nowhere else. It is what lets the run be replaced in place,
// so no other op's index moves and no draw's prep-op range is disturbed. And it is what the executor
// already relies on to batch the run into one command, so nothing about the ordering argument is new.
// A frame whose reorder lever split the run simply does not merge it, which is a miss and not a bug.
//
// The in-bounds proof is the one the per-block copy already has: ClutLoadDefer proved every palette's
// pages resident, residency is page-granular ("pages this surface's texture materializes") and the
// target pool allocates whole page rows of whole pages -- so a copy that stays inside one resident
// page is inside the image by the same clause the four-block copy passes. No new proof term, which
// matters, because the executor DROPS an op whose rect leaves the image and a dropped page op would
// zero every palette in the run at once.
void GSRendererTileGpu::MergeClutCopiesIntoPages()
{
	if (!m_clut_merge_pages_serves || !GSConfig.TileGpuClutMergePages || m_plan_prep_ops.empty())
		return;

	// ⚠️ The stride test is not redundant with the extents: it says the op's destination is a RUN
	// and not a placed tile, which is what the band's in-bounds argument below assumes (n squares
	// reserved n*256 consecutive words). No 16-bit copy is 16x16 either, so today it never fires --
	// but the extents are a shape and this is the contract.
	const auto is_square = [](const GSDevice::GSTileGpuPrepOp& op) {
		return op.kind == GSDevice::GSTileGpuPrepKind::ClutBlockCopy && op.copy_count == 1 && op.copy_w == 16 &&
			   op.copy_h == 16 && op.copy_stride == 0;
	};

	// Every palette this pass moves, as (old stream offset -> new one). Collected across the WHOLE
	// op array and applied to the draws once at the end, sorted, rather than re-walking the draw list
	// per run: gt4opb merges 139 runs in a frame of thousands of draws, and the per-run shape is a
	// multiply where this is an add.
	m_clut_page_remap.clear();
	std::array<u32, kClutPageSquares> rel_x{}, rel_y{};

	const u32 op_count = static_cast<u32>(m_plan_prep_ops.size());
	for (u32 i = 0; i < op_count;)
	{
		if (!is_square(m_plan_prep_ops[i]))
		{
			i++;
			continue;
		}
		const GSDevice::GSTileGpuPrepOp& head = m_plan_prep_ops[i];
		const u32 px = head.copy_x[0] & ~63u;
		const u32 py = head.copy_y[0] & ~31u;
		// How far the run of same-owner, same-page, contiguously-reserved squares reaches.
		u32 n = 1;
		while (i + n < op_count && n < kClutPageSquares)
		{
			const GSDevice::GSTileGpuPrepOp& op = m_plan_prep_ops[i + n];
			if (!is_square(op) || op.donor_target != head.donor_target || (op.copy_x[0] & ~63u) != px ||
				(op.copy_y[0] & ~31u) != py || op.bp != head.bp + n * 256)
			{
				break;
			}
			n++;
		}

		// Eight whole slots, or the four of one row. Nothing else tiles a band, and nothing else may
		// be written -- see gsTileGpuClutPageBand.
		u32 take = 0, band_y = 0, band_h = 0;
		for (const u32 len : {kClutPageSquares, 4u})
		{
			if (len > n)
				continue;
			for (u32 k = 0; k < len; k++)
			{
				rel_x[k] = m_plan_prep_ops[i + k].copy_x[0] - px;
				rel_y[k] = m_plan_prep_ops[i + k].copy_y[0] - py;
			}
			if (gsTileGpuClutPageBand(rel_x.data(), rel_y.data(), len, band_y, band_h))
			{
				take = len;
				break;
			}
		}
		if (take == 0)
		{
			i++;
			continue;
		}

		// The run's reservation is [head.bp, head.bp + take * 256), and the region writes
		// 64 * band_h words from head.bp. The predicate above is what says those are the same range.
		const u32 base = head.bp;
		for (u32 k = 0; k < take; k++)
			m_clut_page_remap.push_back({m_plan_prep_ops[i + k].bp, base + (rel_y[k] - band_y) * 64 + rel_x[k]});

		GSDevice::GSTileGpuPrepOp& page = m_plan_prep_ops[i];
		page.copy_count = 1;
		page.copy_w = 64;
		page.copy_h = band_h;
		page.copy_x[0] = px;
		page.copy_y[0] = py + band_y;
		page.bp = base;
		// The rest of the run stays in the array so that no op index moves -- a draw's prep range is
		// a slice of it -- and is neutered instead. copy_count 0 is the executor's own "drop this op"
		// condition, taken before it records anything, so a neutered op does not even open a copy run.
		for (u32 k = 1; k < take; k++)
			m_plan_prep_ops[i + k].copy_count = 0;
		m_frame.clut_copies -= (take - 1);
		m_frame.clut_copy_regions -= (take - 1);
		m_frame.clut_page_merges++;
		i += take;
	}

	if (m_clut_page_remap.empty())
		return;

	// The draws. pal_offset is the record's stream offset, handed to the draw when it was accumulated;
	// nothing else in the plan names a palette by its offset, so this is the whole fix-up. The stride
	// goes to 64, in the two forms the fragment arm folds into bit positions.
	std::sort(m_clut_page_remap.begin(), m_clut_page_remap.end(),
		[](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) { return a.first < b.first; });
	for (PendingDraw& pd : m_plan_pending)
	{
		if (pd.pal_mode != 3u) [[likely]]
			continue;
		const auto at = std::lower_bound(m_clut_page_remap.begin(), m_clut_page_remap.end(), pd.pal_offset,
			[](const std::pair<u32, u32>& e, u32 v) { return e.first < v; });
		if (at == m_clut_page_remap.end() || at->first != pd.pal_offset)
			continue; // a square this pass did not merge: it still reads its own 16-wide tile
		pd.pal_offset = at->second;
		pd.pal_mul = 7u;
		pd.pal_shift = 2u;
	}
}

// The surface id has been handed to a NEW surface, so whatever texture a palette on it named holds
// somebody else's pixels now, on every page.
void GSRendererTileGpu::NoteClutSurfaceReplaced(GSTileSurfaceId id)
{
	if (m_clut_live.empty()) [[likely]]
		return;
	for (const u32 idx : m_clut_live)
	{
		GpuPalette& gp = m_gpu_palettes[idx];
		if (gp.owner == id)
			gp.source_lost = true;
	}
}

// A prep op that reads an owner target cannot be hoisted over a draw of the open pass that wrote
// that target's pixels -- the same hazard a donor build has, and the same answer: the draw opens its
// own pass, where the op runs after everything that came before it.
void GSRendererTileGpu::ClutHoistBreak(const GpuPalette& gp, PendingDraw& pd)
{
	if (!DonorHoistCollides(gp.owner, gp.pages))
		return;
	pd.break_before = true;
	pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::ClutHoist);
	m_frame.clut_breaks++;
	BreakOpenPass();
}

bool GSRendererTileGpu::CaptureClutRecordWords(GpuPalette& gp)
{
	if (gp.stream_plan == m_source_frame)
		return true; // already in this plan's stream; one copy serves every consumer of the record
	if (gp.source_lost)
		return false;

	// Which geometry the palette's words have in the owner's pixel space. A CT32/CT24 owner stores
	// one guest word per texel, so copying the palette's blocks IS copying its words; a CT16/CT16S
	// one stores two cells to a word, so the copy goes out twice as wide and the consumer packs the
	// pairs back. Two functions and not one argument, because every number differs -- the region
	// extents, the block table, the page height, the destination offsets and the stride.
	const bool ct16 = (gp.owner_psm == PSMCT16 || gp.owner_psm == PSMCT16S);
	GSTileSwizzleForms::ClutBlockCopy blocks;
	const bool located =
		ct16 ? GSTileSwizzleForms::LocateClutBlocks16(
				   m_clut_forms, gp.owner_psm, gp.cbp, gp.owner_bp, gp.owner_bwpg, gp.entries, blocks) :
			   GSTileSwizzleForms::LocateClutBlocks(
				   m_clut_forms, gp.cbp, gp.owner_bp, gp.owner_bwpg, gp.entries, m_clut_merge_serves, blocks);
	if (!located)
		return false;

	// A reservation, not an append: the words are zero until the copy fills them, and no CPU palette
	// ever crosses into the stream for a draw on this record. Its size is what the REGIONS tile,
	// which the executor also assumes -- it puts region b at b * copy_w * copy_h words into the
	// reservation, so a reservation smaller than that would overwrite the next record's words.
	//
	// The 16-bit road reserves the same expression and it is the same number twice over: its regions
	// tile a stride x h tile exactly (pinned as APlacedCopyTilesItsReservationExactly), so 512 words
	// for a 256-entry palette whether it went out as one 32x16 region or four placed 16x8 ones, and
	// 32 for a 16-entry one. Twice the 32-bit road's, because a palette word is two cells.
	gp.stream_offset = ReserveClutStreamWords(blocks.region_count * blocks.w * blocks.h);
	gp.stream_plan = m_source_frame;
	gp.stream_stride = blocks.stride;
	gp.stream_ct16 = ct16;

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::ClutBlockCopy;
	op.donor_target = PlanTargetIndex(gp.owner);
	op.bp = gp.stream_offset;
	op.copy_count = blocks.region_count;
	op.copy_w = blocks.w;
	op.copy_h = blocks.h;
	for (u32 b = 0; b < blocks.region_count; b++)
	{
		op.copy_x[b] = blocks.x[b];
		op.copy_y[b] = blocks.y[b];
	}
	if (ct16)
	{
		// The destination is a TILE the regions are PLACED in rather than a run they follow one
		// another down: a 256-entry palette at a CBP that is not 4-aligned is four scattered blocks
		// that still have to arrive as the four quadrants of one 32x16 tile, because the fetch has
		// one word order for both alignments. The 32-bit road leaves both fields zero and the
		// executor's arithmetic there is what it always was.
		op.copy_stride = blocks.stride;
		for (u32 b = 0; b < blocks.region_count; b++)
			op.copy_off[b] = blocks.copy_off[b];
	}
	m_plan_prep_ops.push_back(op);
	m_frame.clut_copies++;
	m_frame.clut_copy_regions += blocks.region_count;
	return true;
}

u32 GSRendererTileGpu::ReserveClutStreamWords(u32 words)
{
	const u32 at = static_cast<u32>(m_plan_palettes.size());
	m_plan_palettes.resize(m_plan_palettes.size() + words, 0);
	return at;
}

bool GSRendererTileGpu::EnsureClutStreamWords(GpuPalette& gp, PendingDraw& pd)
{
	const bool fresh = (gp.stream_plan != m_source_frame);
	if (!CaptureClutRecordWords(gp))
		return false;
	// Only a copy emitted for THIS draw can be hoisted over the open pass; one an earlier draw's
	// capture already emitted has run its test where it was queued.
	if (fresh)
		ClutHoistBreak(gp, pd);
	return true;
}

GSTexture* GSRendererTileGpu::ClutGatherTexture(GpuPalette& gp, PendingDraw& pd)
{
	if (gp.gather_tex)
		return gp.gather_tex;
	if (gp.source_lost)
		return nullptr;
	// A SIXTEEN-BIT owner is refused here rather than gathered out of the wrong bytes.
	// tilegpu_clutgather.glsl reads its source through the CT32 block and column forms and packs one
	// texel per entry, which for a 16-bit owner is a plausible palette made of the wrong halfwords --
	// silently, since nothing downstream can tell. The byte road has the second geometry and the
	// second entry order; this one does not, and giving it them is its own piece of work.
	//
	// The refusal costs the draw nothing but the materialise: the caller counts it, returns false,
	// and the draw takes the byte road, which serves the same palette through pal_mode 4/5.
	if (gp.stream_ct16 || gp.owner_psm == PSMCT16 || gp.owner_psm == PSMCT16S)
	{
		m_frame.clut16_r3_refused++;
		return nullptr;
	}
	GSTexture* const tex =
		g_gs_device->CreateRenderTarget(static_cast<int>(gp.entries), 1, GSTexture::Format::Color, false, true);
	if (!tex)
		return nullptr;

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::ClutGather;
	op.target = PrepTextureIndex(tex);
	op.donor_target = PlanTargetIndex(gp.owner);
	op.bp = gp.cbp;
	op.owner_bp = gp.owner_bp;
	op.owner_bwpg = gp.owner_bwpg;
	m_plan_prep_ops.push_back(op);
	m_frame.clut_gathers++;
	gp.gather_tex = tex;
	ClutHoistBreak(gp, pd);
	return tex;
}

// Which device palette a draw's slots resolve to. A four-bit index reads slot CSA; an eight-bit one
// at CSA 0 reads all sixteen; an eight-bit one at a non-zero CSA reads a window the sixteen-slot
// mirror cannot name, so it takes the always-correct road -- the CPU words, made whole first.
GSRendererTileGpu::GpuPalette* GSRendererTileGpu::ResolveDrawPalette(const GIFRegTEX0& tex0, u32 pal_entries,
	u32& first_texel, bool& synced)
{
	first_texel = 0;
	synced = false;
	if (m_gpu_palettes.empty() || !m_clut_mirror.AnyGpu()) [[likely]]
		return nullptr;

	u32 first, count;
	if (pal_entries == 16)
	{
		first = tex0.CSA & 15;
		count = 1;
	}
	else if (tex0.CSA == 0)
	{
		first = 0;
		count = GSTileClutMirror::kSlots;
	}
	else
	{
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}

	const GSTileClutMirror::Resolution r = m_clut_mirror.Resolve(first, count);
	if (r.verdict == GSTileClutMirror::Verdict::Cpu)
		return nullptr;
	if (r.verdict == GSTileClutMirror::Verdict::Mixed)
	{
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}

	GpuPalette* const gp = FindGpuPalette(r.handle);
	if (!gp)
	{
		// The mirror names a record nothing holds. Cannot happen (the mirror and the record list are
		// retired together), and if it ever does the CPU road is the safe answer.
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}
	if (gp->source_lost && gp->stream_plan != m_source_frame)
	{
		// The owner's texels on the palette's pages were overwritten after the load, and its words
		// were not copied anywhere first: they are gone. Nothing can recover them -- a readback of
		// those pages now would hand back whatever was rendered over them -- so this is counted and
		// said out loud rather than served silently, and the record is retired so it costs the stale
		// CPU words at most once. Expected to be zero: the window between a palette's source being
		// overwritten and its slots being displaced by the next load carries no draw that reads it.
		RetireLostClutRecord(*gp);
		return nullptr;
	}
	first_texel = r.first_texel;
	m_frame.clut_draws++;
	return gp;
}

void GSRendererTileGpu::RetireLostClutRecord(GpuPalette& gp)
{
	m_frame.clut_lost++;
	if (!m_warned_clut_lost)
	{
		m_warned_clut_lost = true;
		Console.Warning("TileGpu: a palette gathered off a render target was read after its source pixels had been "
						"overwritten -- its words are unrecoverable and the CPU's stale CLUT is used instead. "
						"Counted per frame as lost CLUT records.");
	}
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		if (m_clut_mirror.SlotHandle(s) == gp.handle)
			m_clut_mirror.OnCpuLoad(s, 1);
	}
	RefreshClutLive();
}

// Every device palette's words back into the CPU's CLUT RAM. The seam every genuine CPU consumer of
// the palette goes through -- a savestate, a renderer switch, a load of a shape the mirror cannot
// model -- and exact-Tile's SyncClutToCpu contract: make the words whole, then hand the slots back.
void GSRendererTileGpu::SyncClutToCpu()
{
	// Driven off the MIRROR rather than off a snapshot of the record list, and that is not
	// stylistic: syncing a record reads pages back, a readback flushes the pending plan, and the
	// plan's tail PRUNES the record list -- so any index or pointer taken before the loop is stale
	// inside it. Each pass hands one record's slots back, so at most sixteen passes run.
	for (u32 guard = 0; guard <= GSTileClutMirror::kSlots; guard++)
	{
		u32 handle = GSTileClutMirror::kCpu;
		for (u32 s = 0; s < GSTileClutMirror::kSlots && handle == GSTileClutMirror::kCpu; s++)
			handle = m_clut_mirror.SlotHandle(s);
		if (handle == GSTileClutMirror::kCpu)
			break;
		GpuPalette* const gp = FindGpuPalette(handle);
		if (!gp)
		{
			// The mirror names a record nothing holds -- it cannot, the two are retired together,
			// and if it ever does the CPU road is the safe answer for those slots.
			for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
			{
				if (m_clut_mirror.SlotHandle(s) == handle)
					m_clut_mirror.OnCpuLoad(s, 1);
			}
			continue;
		}
		SyncClutRecordToCpu(*gp);
	}
	RefreshClutLive();
}

void GSRendererTileGpu::SyncClutRecordToCpu(GpuPalette& gp)
{
	if (gp.source_lost)
	{
		RetireLostClutRecord(gp);
		return;
	}
	m_frame.clut_syncs++;
	// ⚠️ Everything this needs is taken BEFORE the readback: the readback flushes the plan, whose
	// tail may reallocate the record list, and `gp` does not survive that.
	const u32 handle = gp.handle;
	const u32 cbp = gp.cbp;
	const u32 entries = gp.entries;
	const u32 first_slot = gp.first_slot;
	const GSPageBitmap pages = gp.pages;

	// The owner still holds the load's words, so a readback of its pages puts them back in the CPU
	// shadow exactly as the loader read them -- the same bytes today's un-gathered road loads from.
	ReadbackToShadow(pages, StallSite::Clut);

	if (!m_clut_word_map_taken)
	{
		m_clut_word_map_taken = true;
		GSClut::EntryToWordCSM1_32(m_clut_word_i8.data(), m_clut_word_i4.data());
	}
	const u32* const src = reinterpret_cast<const u32*>(m_mem.BlockPtr32(0, 0, cbp, 1));
	std::array<u32, 256> words{};
	for (u32 e = 0; e < entries; e++)
		words[e] = src[(entries == 256) ? m_clut_word_i8[e] : m_clut_word_i4[e]];

	// Only the slots this record still owns: a later load may have taken some of them, and writing
	// those back would clobber a palette that is current.
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		if (m_clut_mirror.SlotHandle(s) != handle)
			continue;
		const u32 texel = (s - first_slot) * GSTileClutMirror::kEntriesPerSlot;
		m_mem.m_clut.SetEntries32(s * GSTileClutMirror::kEntriesPerSlot, GSTileClutMirror::kEntriesPerSlot,
			words.data() + texel);
		m_clut_mirror.OnCpuLoad(s, 1);
	}
	RefreshClutLive();
}

// Records the mirror no longer names are recycled here and NOWHERE else. Tile prunes at the load,
// and that is unsound under deferred consumption: a draw recorded into the open plan may still name
// a palette whose slots a later load has already displaced, and its gather texture would be handed
// back to the device under it. At the plan tail every recorded op has issued and there are no
// pending draws, so Tile's own predicate becomes correct as written.
void GSRendererTileGpu::PruneGpuPalettes()
{
	if (m_gpu_palettes.empty())
		return;
	const size_t before = m_gpu_palettes.size();
	std::vector<GpuPalette> kept;
	kept.reserve(m_gpu_palettes.size());
	for (GpuPalette& gp : m_gpu_palettes)
	{
		if (m_clut_mirror.References(gp.handle))
		{
			kept.push_back(gp);
			continue;
		}
		if (gp.gather_tex)
			g_gs_device->Recycle(gp.gather_tex);
	}
	m_gpu_palettes.swap(kept);
	m_frame.clut_pruned += static_cast<u32>(before - m_gpu_palettes.size());
	RefreshClutLive();
}

void GSRendererTileGpu::ReleaseGpuPalettes()
{
	for (const GpuPalette& gp : m_gpu_palettes)
	{
		if (gp.gather_tex)
			g_gs_device->Recycle(gp.gather_tex);
	}
	m_gpu_palettes.clear();
	m_clut_live.clear();
	m_clut_mirror.Reset();
	m_clut_pending = ClutPendingLoad{};
}

// A local->local move. Observed as OnMove first — matching the Tile renderer's order, where
// OnMove sees the pass state before the move's own read/write halves touch it — then the
// base performs the real guest-memory copy. That base Move calls InvalidateLocalMem(src) and
// InvalidateVideoMem(dst) in turn, which the memory model needs (the source comes down if a
// target holds it, the destination write shrinks or clears claims); OnMove has already
// accounted both halves for the pass sim, so m_pass_sim_in_move suppresses only the sim
// forwards for the copy's duration.
void GSRendererTileGpu::Move()
{
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GSVector4i src_r(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);
	const GSVector4i dst_r(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);

	const GSTileSurfaceLayout src_l{m_env.BITBLTBUF.SBP, static_cast<u8>(m_env.BITBLTBUF.SBW),
		static_cast<u8>(m_env.BITBLTBUF.SPSM), KindForPsm(m_env.BITBLTBUF.SPSM)};
	const GSTileSurfaceLayout dst_l{m_env.BITBLTBUF.DBP, static_cast<u8>(m_env.BITBLTBUF.DBW),
		static_cast<u8>(m_env.BITBLTBUF.DPSM), KindForPsm(m_env.BITBLTBUF.DPSM)};

	if (m_pass_sim.IsActive()) [[unlikely]]
		m_pass_sim.OnMove(PagesForTargetRect(src_l, src_r), PagesForTargetRect(dst_l, dst_r));

	m_pass_sim_in_move = true;
	GSRenderer::Move();
	m_pass_sim_in_move = false;
}

// The close/switch seam Classic reads its texture cache back on: everything a target holds
// newest comes down. A savestate taken with UserHacks_ReadTCOnClose lands here too.
void GSRendererTileGpu::ReadbackTextureCache()
{
	SyncAllTruthToCpu();
}

// A purge drops the resident targets (device loss, a config change that rebuilds them). Truth
// comes down first so nothing is lost; the model then forgets every surface.
void GSRendererTileGpu::PurgeTextureCache(bool sources, bool targets, bool hash_cache)
{
	if (sources)
	{
		// Materialised sources are a cache of guest bytes, so dropping them costs rebuilds and
		// nothing else. Inert at this chunk (nothing builds into them yet), wired now because a
		// half-connected lifetime is the kind of thing that surfaces as a stale texture later.
		m_tex_source.Clear();
		m_palette_cache.Clear();
		m_expand_cache.Clear();
		m_probe_windows.clear();
		m_probe_prev.clear();
	}
	if (!targets)
		return;
	// The palettes come down with everything else (SyncAllTruthToCpu), and then the textures they
	// were gathered into go back to the device with the targets.
	SyncAllTruthToCpu();
	ReleaseGpuPalettes();
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
	m_surface_version.clear();
}

GSVector4i GSRendererTileGpu::ComputeDrawBBox() const
{
	GSVector4i bbox;
	if (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_POINT_CLASS)
		bbox = GSVector4i((m_vt.m_min.p + GSVector4(0.5f)).floor().upld((m_vt.m_max.p + GSVector4(0.5f)).floor()));
	else
		bbox = GSVector4i(m_vt.m_min.p.ceil().upld(m_vt.m_max.p.floor()));
	if (PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS))
		bbox += GSVector4i(-1, -1, 1, 1);
	bbox += GSVector4i(0, 0, 1, 1);

	return bbox;
}

GSVector4i GSRendererTileGpu::ComputeDrawRect() const
{
	return ComputeDrawBBox().rintersect(m_context->scissor.in);
}

// The Tile renderer's PassSimObserveDraw, ported to the plain GSRenderer base. Every input
// reads from base state; the only substitutions are (a) the draw rect and page/kind helpers,
// replicated above, and (b) the z-write/z-test booleans, derived here from the registers using
// the SW-floor semantics (ZTE gates both) rather than pulled from a full draw lowering — the
// lowering's later z-write refinements are a stage-1.5 concern, when the decode-compare harness
// makes the whole vertex/state input byte-identical to the shipping decode.
// -- the in-pass reader classifier ------------------------------------------------------------
// Which draws need the raster-order read of their own pixel, beyond pixel-identity feedback (the
// pass sim classifies that itself): blend equations and tests fixed-function cannot express. Alpha
// range comes from the vertex trace; when it is not valid the classifier assumes the full range, so
// it leans conservative. The dual-source class is kept apart because Adreno has dual-source blending
// and Mali-G615 does not — that flag is where the two tiers' reader populations diverge. DATE on
// 24-bit targets is skipped: destination alpha reads back as a constant there, so no read is needed.
//
// ⚠️ ONE reader for two consumers, deliberately. The pass-structure census (ObserveDraw, which only
// runs under -tilepasssim) has counted these since the crossover study, and admission to the actual
// read now decides from the SAME facts. If the two ever answered differently, the census would be
// describing a renderer nobody runs.
//
// `color_written` is the caller's own notion of "this draw lands colour": the census asks it as
// "FBMSK does not mask everything", the plan asks it as the channel mask the AFAIL fold leaves. The
// two differ only where a draw is being dropped anyway.
u32 GSRendererTileGpu::ReaderFlags(bool color_written)
{
	const GSDrawingContext* ctx = m_context;
	u32 reader_flags = 0;
	if (!color_written)
		return reader_flags;

	if (ctx->TEST.DATE && GSLocalMemory::m_psm[ctx->FRAME.PSM].trbpp != 24)
		reader_flags |= GSTilePassSim::ReaderDate;
	if (PRIM->ABE)
	{
		const GIFRegALPHA& al = ctx->ALPHA;
		const bool uses_cd = (al.A == 1) || (al.B == 1) || (al.D == 1) || (al.C == 1);
		if (uses_cd && al.A != al.B)
		{
			// Cv = (A - B) * C >> 7 + D with the destination involved. A == B degenerates
			// to Cv = D (source-only or destination passthrough), which fixed-function
			// always expresses — hence the gate above.
			if (m_draw_env->COLCLAMP.CLAMP == 0)
				reader_flags |= GSTilePassSim::ReaderWrap;
			if (m_draw_env->PABE.PABE)
				reader_flags |= GSTilePassSim::ReaderPabe;
			if (al.D == al.A)
				reader_flags |= GSTilePassSim::ReaderCoeffGt1; // accumulation: a coefficient is 1 + C
			if (al.C == 1)
				reader_flags |= GSTilePassSim::ReaderAdFactor;
			else if (al.C == 2)
			{
				if (al.FIX > 0x80)
					reader_flags |= GSTilePassSim::ReaderFacGt1;
				// FIX <= 0x80 maps onto the constant blend factor: expressible.
			}
			else if (al.C == 0)
			{
				// C=As: the blend factor is the draw's POST-texture-function source alpha,
				// so the range must fold TFX/TEXA/CLUT — GetAlphaMinMax does exactly that
				// (the base trace's Update only zeroes m_alpha.valid; the renderer computes
				// the range lazily, which the plain-GSRenderer base does not do for us). A
				// palettised draw needs the CLUT's decoded read buffer current first or the
				// scan reads the previous draw's palette.
				//
				// ⚠️ And "current" is a question about the DRAW'S OWN SLOTS, not about the
				// CLUT RAM's age. The gathers leave a palette the game rendered on the device
				// and the RAM stale for its slots, so the scan would fold the wrong palette's
				// alpha in — and this road is reached on every blended draw of a title that
				// gathers. Where the mirror says the slots are not the CPU's, the scan is
				// skipped outright (it would cache a wrong answer for the fold further down
				// as well) and the bound widens to the whole range.
				//
				// Widening is what Classic does with the same fact, and it is sound HERE
				// because the conservative branch PAINTS: ReaderFacGt1 admits the draw to a
				// destination read it may not need, which costs a read and never a pixel.
				// That asymmetry is the whole lesson of the alpha-test fold's own guard,
				// where the conservative branch DISCARDS instead.
				const u32 pal_entries = PRIM->TME ? GSLocalMemory::m_psm[ctx->TEX0.PSM].pal : 0;
				const bool palettised = pal_entries > 0;
				const bool clut_cpu_current = m_clut_mirror.DrawSlotsCpuCurrent(pal_entries, ctx->TEX0.CSA);
				u32 scanned_max = 0;
				if (clut_cpu_current)
				{
					if (palettised)
						m_mem.m_clut.Read32(ctx->TEX0, m_draw_env->TEXA);
					scanned_max = static_cast<u32>(std::clamp(GetAlphaMinMax().max, 0, 255));
				}
				const u32 amax = gsTileGpuReaderSourceAlphaMax(palettised, clut_cpu_current, scanned_max);
				if (amax > 0x80)
					reader_flags |= GSTilePassSim::ReaderFacGt1;
				else
					reader_flags |= GSTilePassSim::ReaderAsDualSource;
			}
		}
	}
	return reader_flags;
}

// Which classes this draw is admitted for. Everything, unless the device charges for declaring and
// the budget has priced a class out -- and everything again under the forced-admission instrument,
// which exists to exercise the road and so may not be the thing that stops it. A forced run on a
// device that taxes declaring is a measurement, not a shipping shape.
//
// The dev probe's refusal is applied LAST and to all three of those roads alike, so a key naming one
// class means the same thing on a device that taxes declaring, on one that does not, and under the
// forced admission. Zero on every shipping run, and this is then the expression it always was.
u32 GSRendererTileGpu::AdmittedClasses(u32 wanted_classes) const
{
	const u32 budget_admitted =
		(m_force_self_read || !m_segregate_self_read) ? kGSTileGpuClassAll : m_declaring_budget.admitted;
	return gsTileGpuAdmittedClasses(wanted_classes, budget_admitted, m_probe_refused_classes);
}

// Charge one draw against the budget, and record the per-class census.
//
// The run this draw might be continuing is a run within its ATTACHMENT group, not within the pass it
// finally lands in. That distinction is the whole reason the budget converges: the passes depend on
// which classes were admitted, so measuring the classes against the passes would measure the
// budget's own output, and a refused class -- declaring nothing, measuring zero -- would re-admit
// itself every second frame forever. Against the attachment group the answer is what the class WOULD
// cost, which is the same whether or not it was admitted.
void GSRendererTileGpu::ChargeDeclaringBudget(u32 wanted_classes, const GSTileGpuPassKey& group)
{
	if (group != m_budget_group)
	{
		m_budget_group = group;
		m_budget_prev_wanted = 0;
	}
	const u32 opening = wanted_classes & ~m_budget_prev_wanted;
	m_declaring_budget.Charge(wanted_classes, opening);
	for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
	{
		const u32 bit = 1u << c;
		if ((wanted_classes & bit) == 0)
			continue;
		m_frame.class_wanted[c]++;
		if (opening & bit)
			m_frame.class_runs[c]++;
		else
			m_frame.class_taxed[c]++;
	}
	m_budget_prev_wanted = wanted_classes;
}

void GSRendererTileGpu::ObserveDraw()
{
	const GSVector4i r = ComputeDrawRect();
	if (r.rempty())
		return;

	const GSDrawingContext* ctx = m_context;

	const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	const GSPageBitmap fb_pages = PagesForTargetRect(fb_l, r);
	GSPageBitmap fb_written;
	if (ctx->FRAME.FBMSK != 0xFFFFFFFFu)
		fb_written = fb_pages;

	// SW-floor z semantics: ZTE=0 turns off the test AND the write.
	const bool z_write = ctx->TEST.ZTE && !ctx->ZBUF.ZMSK;
	const bool z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;

	GSPageBitmap z_pages;
	GSPageBitmap z_written;
	const bool z_used = z_write || z_test;
	if (z_used)
	{
		const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};
		z_pages = PagesForTargetRect(z_l, r);
		if (z_write)
			z_written = z_pages;
	}

	GSPageBitmap tex_pages;
	GSPageBitmap core_pages;
	bool has_core = false;
	bool identity_feedback = false;
	if (PRIM->TME)
	{
		const bool mip = IsMipMapActive();
		const GIFRegTEX0 fixed_tex0 =
			ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		// The in-pass-read shape: the sampling maps pixel-to-pixel — the UV bbox lands within
		// one texel of the draw rect. An input-attachment read serves same-pixel reads of ANY
		// attachment bound to the pass (it has no coordinate argument), so the sampled window's
		// base does NOT have to be this draw's own target: the ping-pong post chain (write A,
		// draw into B sampling A at 1:1) is exactly this shape.
		if (!mip && PRIM->FST)
		{
			const int u0 = static_cast<int>(std::floor(m_vt.m_min.t.x));
			const int v0 = static_cast<int>(std::floor(m_vt.m_min.t.y));
			const int u1 = static_cast<int>(std::ceil(m_vt.m_max.t.x));
			const int v1 = static_cast<int>(std::ceil(m_vt.m_max.t.y));
			identity_feedback = std::abs(u0 - r.x) <= 1 && std::abs(v0 - r.y) <= 1 &&
								std::abs(u1 - r.z) <= 1 && std::abs(v1 - r.w) <= 1;
		}
		const u32 mip_levels = mip ? (std::min<u32>(ctx->TEX1.MXL, 6) + 1) : 1u;
		for (u32 i = 0; i < mip_levels; i++)
		{
			const GIFRegTEX0 lvl = (i == 0) ? fixed_tex0 : GetTex0Layer(i);
			const GSTileSurfaceLayout tex_l{
				lvl.TBP0, static_cast<u8>(lvl.TBW), static_cast<u8>(lvl.PSM), KindForPsm(lvl.PSM)};
			const int tw = 1 << std::min<u32>(lvl.TW, 10);
			const int th = 1 << std::min<u32>(lvl.TH, 10);
			tex_pages |= PagesForTargetRect(tex_l, GSVector4i(0, 0, tw, th));
		}
		if (mip_levels == 1 && ctx->CLAMP.WMS <= CLAMP_CLAMP && ctx->CLAMP.WMT <= CLAMP_CLAMP)
		{
			const int tw = 1 << std::min<u32>(fixed_tex0.TW, 10);
			const int th = 1 << std::min<u32>(fixed_tex0.TH, 10);
			int cx0 = static_cast<int>(std::floor(m_vt.m_min.t.x)) + 1;
			int cy0 = static_cast<int>(std::floor(m_vt.m_min.t.y)) + 1;
			int cx1 = static_cast<int>(std::floor(m_vt.m_max.t.x));
			int cy1 = static_cast<int>(std::floor(m_vt.m_max.t.y));
			const bool u_ok = (ctx->CLAMP.WMS == CLAMP_CLAMP) || (cx0 >= 0 && cx1 <= tw);
			const bool v_ok = (ctx->CLAMP.WMT == CLAMP_CLAMP) || (cy0 >= 0 && cy1 <= th);
			cx0 = std::max(cx0, 0);
			cy0 = std::max(cy0, 0);
			cx1 = std::min(cx1, tw);
			cy1 = std::min(cy1, th);
			if (u_ok && v_ok && cx0 < cx1 && cy0 < cy1)
			{
				const GSTileSurfaceLayout tex_l{fixed_tex0.TBP0, static_cast<u8>(fixed_tex0.TBW),
					static_cast<u8>(fixed_tex0.PSM), KindForPsm(fixed_tex0.PSM)};
				core_pages = PagesForTargetRect(tex_l, GSVector4i(cx0, cy0, cx1, cy1));
				has_core = true;
			}
		}
	}

	const u32 reader_flags = ReaderFlags(!fb_written.empty());

	// Does this draw land any alpha in memory? Only the alpha bits the target FORMAT keeps
	// count — PSMCT24 stores none at all, PSMCT16 keeps one — and FBMSK masks them off from
	// there. A draw that stores no alpha can take the doubled-alpha trick whatever else
	// touches the surface, because the doubled value never outlives the blend.
	const u32 fmt_alpha = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk & 0xFF000000u;
	const bool stores_alpha = !fb_written.empty() && (fmt_alpha & ~ctx->FRAME.FBMSK) != 0;

	m_pass_sim.OnDraw(fb_written, z_written, fb_pages | z_pages, tex_pages, core_pages, has_core,
		identity_feedback, PRIM->TME && PRIM->FST, reader_flags, stores_alpha, ctx->FRAME.Block(),
		ctx->FRAME.PSM, ctx->ZBUF.Block(), ctx->ZBUF.PSM, z_used,
		static_cast<u32>(std::max(m_vertex->tail, m_vertex->next)));
}

// The accumulated per-frame pass structure, mean / p50 at teardown -- the offline
// -tilepasssim arm's aggregate (minus the Tile-only GIF-stream tail), then the memory model's
// own traffic. These are the structural counters the stage-1 gate reads against the design
// values, over draws AND the stream's memory events, so every break reason is measured rather
// than zero by construction.
void GSRendererTileGpu::ReportPassStructure()
{
	ReportModelTraffic();
	ReportVariantCensus();
	ReportPassShapeCensus();
	if (!m_pass_sim.IsActive())
	{
		// Say so rather than print nothing: a missing section reads as "the sim found nothing",
		// and the whole point of the numbers above is that they are compared against these.
		if (!m_model_frames.empty())
			Console.WriteLn("  (pass-structure cross-check off -- EmuCore/GS/TileGpuPassSim, gsrunner -tilepasssim)");
		return;
	}
	const std::vector<GSTilePassSim::FrameStats>& frames = m_pass_sim.Frames();
	if (frames.empty())
		return;

	const double n = static_cast<double>(frames.size());
	const auto stat = [&frames, n](auto get) {
		std::vector<u32> v(frames.size());
		double sum = 0.0;
		for (size_t i = 0; i < frames.size(); i++)
		{
			v[i] = get(frames[i]);
			sum += v[i];
		}
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		struct
		{
			double mean;
			u32 p50;
		} r{sum / n, v[v.size() / 2]};
		return r;
	};
	using FS = GSTilePassSim::FrameStats;
	const auto brk = [&stat](GSTilePassSim::BreakReason why) {
		return stat([why](const FS& f) { return f.breaks[static_cast<u32>(why)]; });
	};
	const auto tier = [&stat](auto get, u32 t) {
		return stat([get, t](const FS& f) { return get(f, t); });
	};

	const auto draws = stat([](const FS& f) { return f.draws; });
	const auto verts = stat([](const FS& f) { return f.verts; });
	const auto passes = stat([](const FS& f) { return f.passes; });
	const auto snaps = stat([](const FS& f) { return f.snapshots; });
	const auto snap_pages = stat([](const FS& f) { return f.snapshot_pages; });
	const auto biggest = stat([](const FS& f) { return f.draws_in_biggest_pass; });
	const auto inpass = stat([](const FS& f) { return f.feedback_inpass; });
	const auto identity = stat([](const FS& f) { return f.feedback_identity; });
	const auto fruns = stat([](const FS& f) { return f.feedback_runs; });
	const auto upfree = stat([](const FS& f) { return f.uploads_free; });
	const auto upver = stat([](const FS& f) { return f.uploads_versionable; });
	const auto updraw = stat([](const FS& f) { return f.uploads_drawable; });
	const auto palgather = stat([](const FS& f) { return f.palette_gathers; });
	const auto cpupages = stat([](const FS& f) { return f.cpu_read_pages; });
	const auto b_feedback = brk(GSTilePassSim::BreakReason::Feedback);
	const auto b_cap = brk(GSTilePassSim::BreakReason::TargetCap);
	const auto b_upload = brk(GSTilePassSim::BreakReason::Upload);
	const auto b_move = brk(GSTilePassSim::BreakReason::MoveHazard);
	const auto b_cpu = brk(GSTilePassSim::BreakReason::CpuRead);
	const auto rdrsA = tier([](const FS& f, u32 t) { return f.readers[t]; }, 0);
	const auto rdrsB = tier([](const FS& f, u32 t) { return f.readers[t]; }, 1);
	const auto dvertsB = tier([](const FS& f, u32 t) { return f.declared_verts[t]; }, 1);
	const auto rvertsB = tier([](const FS& f, u32 t) { return f.reader_verts[t]; }, 1);
	const auto tas = stat([](const FS& f) { return f.trick_as_draws; });
	const auto tlive = stat([](const FS& f) { return f.trick_as_on_live; });
	const auto tresc = stat([](const FS& f) { return f.trick_as_mask_rescued; });
	const double eligible = tas.mean - tlive.mean + tresc.mean;

	Console.WriteLn("TileGpu pass structure over %u frames (GS-semantic MINIMUM, %u pairs/pass; "
					"draws + stream memory events; mean / p50 per frame):",
		static_cast<u32>(frames.size()), GSTilePassSim::kMaxTargetPairs);
	Console.WriteLn("  draws %9.2f / %-6u  verts %9.2f / %-8u  biggest pass %6.2f / %u draws", draws.mean,
		draws.p50, verts.mean, verts.p50, biggest.mean, biggest.p50);
	Console.WriteLn("  passes %8.2f / %-6u  snapshots %5.2f / %-4u (%.2f / %u pages)", passes.mean,
		passes.p50, snaps.mean, snaps.p50, snap_pages.mean, snap_pages.p50);
	Console.WriteLn("  breaks: feedback %.2f/%u  target-cap %.2f/%u  upload %.2f/%u  move %.2f/%u  "
					"cpu-read %.2f/%u",
		b_feedback.mean, b_feedback.p50, b_cap.mean, b_cap.p50, b_upload.mean, b_upload.p50, b_move.mean,
		b_move.p50, b_cpu.mean, b_cpu.p50);
	Console.WriteLn("  feedback served in-pass: core proof %.2f / %u   pixel-identity %.2f / %u   "
					"offset residue %.2f runs",
		inpass.mean, inpass.p50, identity.mean, identity.p50, fruns.mean);
	Console.WriteLn("  uploads (never break): free %.2f / %u   versionable %.2f / %u   draw-realized "
					"%.2f / %u   palette gathers %.2f / %u   cpu-read pages %.2f / %u",
		upfree.mean, upfree.p50, upver.mean, upver.p50, updraw.mean, updraw.p50, palgather.mean,
		palgather.p50, cpupages.mean, cpupages.p50);
	Console.WriteLn("  in-pass readers/frame: tierA(dual-src) %.2f / %u   tierB(Mali) %.2f / %u   "
					"tierB declared %.2f verts, reader %.2f verts",
		rdrsA.mean, rdrsA.p50, rdrsB.mean, rdrsB.p50, dvertsB.mean, rvertsB.mean);
	Console.WriteLn("  doubled-alpha trick (tierB): As-class %.2f draws/frame; disqualified %.2f, "
					"rescued %.2f -> eligible %.2f",
		tas.mean, tlive.mean, tresc.mean, eligible);
}

// The fragment-variant draw census: how many draws of the whole run compiled each variant, and how
// many would have compiled each PASS-UNION variant instead.
//
// Two histograms off one run, deliberately. The question the decoupling has to answer is "what does
// the average draw's program cost now, against what it cost before", and answering it with two runs
// would compare two scenes as well as two arms. Joined against the device's own per-variant
// "... N SPIR-V words" lines in the same log, these two columns ARE the draw-weighted program size,
// before and after.
// The CPU palette road's re-work, counted at the one place that appends to the frame's palette
// stream. Behind TileGpuCensusPalette: one integer compare and one set insert per CPU-road draw.
//
// The question is narrow and the test for it is exact rather than heuristic. GSClut::Read32 bumps
// the CLUT's read generation only when it actually re-expands (GSClut::Read32's IsDirty gate), so
// two consecutive CPU-road draws that share a generation and an entry count read byte-identical
// words out of m_clut. For that population the insert above copied bytes the stream already holds
// and the ContentId above hashed words it had just hashed -- both are removable without changing a
// single byte the executor sees, which is what makes this a size rather than an opinion.
void GSRendererTileGpu::CensusCpuPalette(u32 read_gen, u32 entries, u64 content_id)
{
	m_frame.clut_cpu_draws++;
	m_frame.clut_cpu_words += entries;
	if (m_census_pal_prev_valid && read_gen == m_census_pal_prev_gen && entries == m_census_pal_prev_entries)
	{
		m_frame.clut_cpu_readgen_held++;
		m_frame.clut_cpu_words_held += entries;
	}
	m_census_pal_prev_valid = true;
	m_census_pal_prev_gen = read_gen;
	m_census_pal_prev_entries = entries;
	m_census_pal_ids.insert(content_id);
	m_frame.clut_cpu_distinct = static_cast<u32>(m_census_pal_ids.size());
}

void GSRendererTileGpu::ReportVariantCensus()
{
	if (m_variant_census_own.empty())
		return;
	const auto report = [](const char* what, std::vector<std::pair<u32, u64>>& hist) {
		std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		u64 total = 0;
		for (const auto& [key, n] : hist)
			total += n;
		Console.WriteLn("  %s (%zu variants over %llu draws):", what, hist.size(), static_cast<unsigned long long>(total));
		for (const auto& [key, n] : hist)
		{
			// The raw key rides in front of the decoded name because the frozen-state half has no
			// spelling in VariantName and an offline compile needs all thirty-one bits to reproduce
			// the program. One hex word does that and reads back through VariantSpec.
			Console.WriteLn("    key=0x%08X  %10llu  %5.1f%%  road=%u texel=%u self=%u q16=%u  %s%s", key,
				static_cast<unsigned long long>(n), (total > 0) ? (static_cast<double>(n) * 100.0 / static_cast<double>(total)) : 0.0,
				GSDevice::GSTileGpuPassPlan::VariantRoadMask(key), GSDevice::GSTileGpuPassPlan::VariantTexelMask(key),
				GSDevice::GSTileGpuPassPlan::VariantSelfMask(key),
				GSDevice::GSTileGpuPassPlan::VariantQuantises(key) ? 1u : 0u,
				GSTileGpuShaderVariant::VariantName(GSDevice::GSTileGpuPassPlan::VariantRoadMask(key),
					GSDevice::GSTileGpuPassPlan::VariantTexelMask(key),
					GSDevice::GSTileGpuPassPlan::VariantSelfMask(key),
					GSDevice::GSTileGpuPassPlan::VariantQuantises(key))
					.c_str(),
				GSTileGpuShaderVariant::SpecName(GSDevice::GSTileGpuPassPlan::VariantSpec(key)).c_str());
		}
	};
	Console.WriteLn("TileGpu fragment-variant draw census:");
	report("draws by the variant they COMPILE (per run)", m_variant_census_own);
	report("draws by their PASS's UNION (what they compiled before)", m_variant_census_union);
}

// One pass's distinct fragment programs, folded into the shape histogram. `keys` is consumed
// (sorted in place) because the caller has no use for it afterwards and a pass is on the plan
// build's hot path.
void GSRendererTileGpu::PassShapeAdd(std::vector<u32>& keys, u64 draws, u64 runs, u64 runs_despec)
{
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	for (PassShape& s : m_pass_shape_census)
	{
		if (s.keys != keys)
			continue;
		s.passes++;
		s.draws += draws;
		s.runs += runs;
		s.runs_despec += runs_despec;
		return;
	}
	// A frame's passes come in a handful of shapes -- 39 on the Ratchet gameplay scene, 79 on Gran
	// Turismo 4 -- but a pathological title could invent one per pass and turn this scan into a
	// quadratic walk over thousands of entries. Past the cap the pass is COUNTED and not recorded.
	// Deliberately not folded into some other shape's row: a shape's numbers have to mean that shape,
	// and a census that quietly averages unrelated passes together is worse than one that says it
	// stopped.
	static constexpr size_t kMaxShapes = 512;
	if (m_pass_shape_census.size() >= kMaxShapes)
	{
		m_pass_shapes_dropped++;
		return;
	}
	PassShape s;
	s.keys = keys;
	s.passes = 1;
	s.draws = draws;
	s.runs = runs;
	s.runs_despec = runs_despec;
	m_pass_shape_census.push_back(std::move(s));
}

// What each pass ALTERNATES among, which is the question a program-DMA cost asks and the draw
// census above cannot answer. A title binding thirty-five programs a frame is cheap if each pass
// uses two of them and expensive if every pass cycles through all thirty-five, and those two
// frames have the same frame-wide census.
//
// Printed with the raw keys so an offline a650 compile can turn the set into a working set in
// instrlen units and compare it against the instruction cache (127 units on an a650). Nothing in
// the renderer knows a program's size -- that is a driver fact, not a planner one -- so the join
// happens outside and this line carries what the join needs.
void GSRendererTileGpu::ReportPassShapeCensus()
{
	if (m_pass_shape_census.empty())
		return;
	std::sort(m_pass_shape_census.begin(), m_pass_shape_census.end(),
		[](const PassShape& a, const PassShape& b) { return a.draws > b.draws; });
	u64 total_passes = 0, total_draws = 0;
	for (const PassShape& s : m_pass_shape_census)
	{
		total_passes += s.passes;
		total_draws += s.draws;
	}
	// The biggest shapes by draws and no more: the tail is a long list of one-pass shapes that says
	// nothing the head does not, and this runs at teardown into the same log everything else uses.
	static constexpr size_t kPrintShapes = 48;
	Console.WriteLn("TileGpu pass working-set census (%zu shapes over %llu passes, %llu draws; the %zu "
					"biggest by draws; %llu passes past the shape cap, unrecorded):",
		m_pass_shape_census.size(), static_cast<unsigned long long>(total_passes),
		static_cast<unsigned long long>(total_draws), std::min(m_pass_shape_census.size(), kPrintShapes),
		static_cast<unsigned long long>(m_pass_shapes_dropped));
	size_t printed = 0;
	for (const PassShape& s : m_pass_shape_census)
	{
		if (printed++ >= kPrintShapes)
			break;
		std::string keys;
		for (const u32 k : s.keys)
			keys += fmt::format("{}0x{:08X}", keys.empty() ? "" : " ", k);
		Console.WriteLn("    shape progs=%2zu passes=%8llu draws=%10llu runs=%10llu runs_despec=%10llu : %s",
			s.keys.size(), static_cast<unsigned long long>(s.passes), static_cast<unsigned long long>(s.draws),
			static_cast<unsigned long long>(s.runs), static_cast<unsigned long long>(s.runs_despec), keys.c_str());
	}
}

// The memory model's per-frame traffic: what the byte road actually moved, and every crossing
// the design says should be rare. Mean / p50 per DRAWN frame.
void GSRendererTileGpu::ReportModelTraffic()
{
	if (m_model_frames.empty())
		return;
	using MF = ModelFrame;

	// ⚠️ PER DRAWN FRAME, and until 2026-08-31 this divided by every PRESENTED frame while every
	// reader -- and every record quoting it -- took it for the drawn one. A model frame is pushed at
	// every VSync, and six of the twenty-two corpus titles draw into every other one (Stuntman,
	// FlatOut 2, both Katamari dumps, MGS3, Yu-Gi-Oh!, Xenosaga), so on those the whole census read
	// exactly 2x low: Stuntman's 1712.16 passes a drawn frame printed as 856.12. The containment
	// block below has always divided by the drawn count and said so, which is what made the
	// discrepancy visible; now there is one denominator for the whole census and it is that one.
	//
	// The SUM is over every frame and only the DIVISOR is the drawn count, so a present-only frame's
	// work is not lost -- it is charged to the frames that did the drawing, which is the arithmetic
	// the off-line counts this census is compared against use. Multiply any mean by the drawn count
	// in the header to recover the run total.
	//
	// `contain_draws` is the test because it counts PS2 draws as they are accumulated: it is what
	// the runner's stats.json uses for `drawn_frames`, and it does not see the display-buffer
	// materialise, which happens on every presented frame and would call every frame drawn.
	//
	// The p50 is the median of the DRAWN frames for the same reason -- a median over a population
	// half of which is structurally zero is a statistic about the presentation cadence, not about
	// the frames with pixels in them. The max stays over every frame: a peak is a peak wherever it
	// happened, and a present-only frame that spikes is exactly what a max is for.
	u32 drawn = 0;
	for (const MF& f : m_model_frames)
		drawn += (f.contain_draws != 0) ? 1u : 0u;
	const double n = static_cast<double>(std::max(drawn, 1u));
	//
	// ⚠️ TWO STATISTICS, because two kinds of column live in this census and the same divisor is
	// right for only one of them.
	//
	//   A FLOW is counted as the frame runs -- ring pages, writebacks, pass breaks, stalls, draws.
	//     A present-only frame's flow is real work and belongs in the bill, so the sum is over every
	//     frame and only the divisor is the drawn count.
	//   A LEVEL is a state read at the frame boundary, or the frame's own maximum -- live surfaces,
	//     the biggest plan's epoch count, the longest pass built, the declaring budget's peak, the
	//     0/1 "was this class refused". It does not accumulate, so summing it over the run and
	//     dividing by the drawn count DOUBLES it on a title that draws every other frame -- and the
	//     refusal column, which is a share of frames, would print over 100%. A level is averaged
	//     over the drawn frames' own values instead.
	const bool any_drawn = drawn != 0;
	const auto tally = [this, n, any_drawn](auto get, bool level) {
		std::vector<u32> v;
		v.reserve(m_model_frames.size());
		double sum = 0.0;
		u32 worst = 0;
		for (const MF& f : m_model_frames)
		{
			const u32 x = get(f);
			worst = std::max(worst, x);
			// A run that drew in no frame at all has no drawn population to take a median over, so
			// it falls back to every frame rather than reporting nothing.
			const bool counted = !any_drawn || f.contain_draws != 0;
			if (counted)
				v.push_back(x);
			if (counted || !level)
				sum += x;
		}
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		struct
		{
			double mean;
			u32 p50;
			u32 max;
		} r{sum / n, v[v.size() / 2], worst};
		return r;
	};
	const auto stat = [&tally](auto get) { return tally(get, false); };
	const auto lvl = [&tally](auto get) { return tally(get, true); };
	const auto surf = lvl([](const MF& f) { return f.surfaces_live; });
	const auto passes = stat([](const MF& f) { return f.passes; });
	const auto ring = stat([](const MF& f) { return f.ring_pages; });
	const auto prefill = stat([](const MF& f) { return f.ring_prefill; });
	const auto versions = stat([](const MF& f) { return f.ring_versions; });
	const auto epochs = lvl([](const MF& f) { return f.epochs; });
	const auto wbo = stat([](const MF& f) { return f.writeback_ops; });
	const auto wbp = stat([](const MF& f) { return f.writeback_pages; });
	const auto wbd = stat([](const MF& f) { return f.writeback_dispatches; });
	const auto wbr = stat([](const MF& f) { return f.writeback_runs; });
	const auto wbh = stat([](const MF& f) { return f.writeback_run_hazards; });
	const auto sdo = stat([](const MF& f) { return f.seed_ops; });
	const auto sdp = stat([](const MF& f) { return f.seed_pages; });
	const auto sdzo = stat([](const MF& f) { return f.seed_ops_depth; });
	const auto sdzp = stat([](const MF& f) { return f.seed_pages_depth; });
	const auto wbrk = stat([](const MF& f) { return f.writeback_breaks; });
	const auto sbrk = stat([](const MF& f) { return f.seed_breaks; });
	const auto mrg = stat([](const MF& f) { return f.merge_ops; });
	const auto mrgp = stat([](const MF& f) { return f.merge_pages; });
	const auto mref_o = stat([](const MF& f) { return f.merge_ref.owner; });
	const auto mref_c = stat([](const MF& f) { return f.merge_ref.cpu; });
	const auto mref_b = stat([](const MF& f) { return f.merge_ref.bytes; });
	const auto mref_s = stat([](const MF& f) { return f.merge_ref.slice; });
	const auto mref_f = stat([](const MF& f) { return f.merge_ref.overflow; });
	const auto mnext = [&stat](u32 c) { return stat([c](const MF& f) { return f.merge_ref.cpu_next[c]; }); };
	const auto mage = [&stat](u32 b) { return stat([b](const MF& f) { return f.merge_ref.cpu_age[b]; }); };
	const auto dbrk = stat([](const MF& f) { return f.date_breaks; });
	const auto ddraw = stat([](const MF& f) { return f.date_draws; });
	const auto dskip = stat([](const MF& f) { return f.date_breaks_cover_would_skip; });
	const auto uncomp = stat([](const MF& f) { return f.prefill_uncomposed; });
	const auto snaps = stat([](const MF& f) { return f.snapshots; });
	const auto snapp = stat([](const MF& f) { return f.snapshot_pages; });
	const auto snapf = stat([](const MF& f) { return f.snapshot_full_pages; });
	const auto snapn = stat([](const MF& f) { return f.snapshot_narrowed; });
	const auto snaph = [&stat](u32 b) { return stat([b](const MF& f) { return f.snapshot_page_hist[b]; }); };
	const auto self = stat([](const MF& f) { return f.self_reads; });
	const auto binds = stat([](const MF& f) { return f.tex_binds; });
	const auto bindbrk = stat([](const MF& f) { return f.tex_bind_breaks; });
	const auto alias = stat([](const MF& f) { return f.alias_steal_pages; });
	const auto lossy = stat([](const MF& f) { return f.lossy_pages; });
	// The depth half is the standing gap, so it is reported beside the total rather than folded
	// into it -- the COLOUR half is the number a byte-road change is answerable for. What is left in
	// the depth half is the WRITEBACK direction: the seed direction exists now, and the pages it
	// cannot serve (another depth surface's, or this one's own partial claim) are what remain here.
	const auto lossyz = stat([](const MF& f) { return f.lossy_pages_depth; });
	const auto skipped = stat([](const MF& f) { return f.skipped_draws; });
	const auto flushes = stat([](const MF& f) { return f.flushes; });
	const auto afold_f = stat([](const MF& f) { return f.atst_fold_fail; });
	const auto afold_p = stat([](const MF& f) { return f.atst_fold_pass; });
	const auto afv = stat([](const MF& f) { return f.afail_varies; });
	const auto afvr = stat([](const MF& f) { return f.afail_varies_rgb_only; });
	const auto afvz = stat([](const MF& f) { return f.afail_varies_depth; });
	const auto afss = stat([](const MF& f) { return f.afail_split_served; });
	const auto afsr = stat([](const MF& f) { return f.afail_split_reordered; });
	const auto afsd = stat([](const MF& f) { return f.afail_split_discard; });
	const auto afsn = stat([](const MF& f) { return f.afail_split_refused_no_ztest; });
	const auto afsf = stat([](const MF& f) { return f.afail_split_refused_fb_only; });
	const auto afsx = stat([](const MF& f) { return f.afail_split_draws; });
	const auto afso = stat([](const MF& f) { return f.afail_split_overlap_asked; });
	const auto st = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.stalls[static_cast<u32>(s)]; });
	};
	const auto stp = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.stall_pages[static_cast<u32>(s)]; });
	};
	const auto srd = stat([](const MF& f) { return f.self_read_draws; });
	const auto srp = stat([](const MF& f) { return f.self_read_passes; });
	const auto srpd = stat([](const MF& f) { return f.self_read_pass_draws; });
	const auto txp = stat([](const MF& f) { return f.texel_passes; });
	const auto txm = stat([](const MF& f) { return f.texel_mixed_passes; });
	const auto txmd = stat([](const MF& f) { return f.texel_mixed_draws; });
	const auto vruns = stat([](const MF& f) { return f.variant_runs; });
	const auto vcalls = stat([](const MF& f) { return f.variant_extra_calls; });
	const auto vnarrow = stat([](const MF& f) { return f.variant_narrowed_draws; });
	const auto vfree = stat([](const MF& f) { return f.variant_byte_freeloaders; });
	const auto vrunsd = stat([](const MF& f) { return f.variant_runs_despec; });
	const auto vrunsu = stat([](const MF& f) { return f.variant_runs_unguarded; });
	const auto vprog = stat([](const MF& f) { return f.variant_pass_programs; });
	const auto vprogd = stat([](const MF& f) { return f.variant_pass_programs_despec; });
	const auto sgp = stat([](const MF& f) { return f.specguard_passes; });
	const auto sgd = stat([](const MF& f) { return f.specguard_draws; });
	const auto bigpass = lvl([](const MF& f) { return f.biggest_pass_draws; });
	const auto capped = stat([](const MF& f) { return f.capped_passes; });
	const auto road_oob = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.pull_oob[static_cast<u32>(s)]; });
	};
	const auto road_drain = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.pull_drain[static_cast<u32>(s)]; });
	};
	const auto road_idle = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.pull_idle[static_cast<u32>(s)]; });
	};
	const auto s_up = st(StallSite::UploadSubBlock), s_rd = st(StallSite::LocalRead), s_cl = st(StallSite::Clut),
			   s_all = st(StallSite::SyncAll);
	const auto ro_up = road_oob(StallSite::UploadSubBlock), ro_rd = road_oob(StallSite::LocalRead),
			   ro_cl = road_oob(StallSite::Clut), ro_all = road_oob(StallSite::SyncAll);
	const auto rd_up = road_drain(StallSite::UploadSubBlock), rd_rd = road_drain(StallSite::LocalRead),
			   rd_cl = road_drain(StallSite::Clut), rd_all = road_drain(StallSite::SyncAll);
	const auto ri_up = road_idle(StallSite::UploadSubBlock), ri_rd = road_idle(StallSite::LocalRead),
			   ri_cl = road_idle(StallSite::Clut), ri_all = road_idle(StallSite::SyncAll);
	const auto p_up = stp(StallSite::UploadSubBlock), p_rd = stp(StallSite::LocalRead), p_cl = stp(StallSite::Clut),
			   p_all = stp(StallSite::SyncAll);

	Console.WriteLn("TileGpu memory model over %u frames, %u of them DRAWN (mean / p50 per DRAWN frame; the mean is "
					"the run total over the drawn count, so mean x %u is the total):",
		static_cast<u32>(m_model_frames.size()), drawn, drawn);
	Console.WriteLn("  surfaces live %6.2f / %-4u  passes %7.2f / %-5u  ring pages %7.2f / %-5u (prefilled %.2f / %u, "
					"version copies %.2f / %u, epochs %.2f / %u, prefilled for uncomposed bytes %.2f / %u)",
		surf.mean, surf.p50, passes.mean, passes.p50, ring.mean, ring.p50, prefill.mean, prefill.p50, versions.mean,
		versions.p50, epochs.mean, epochs.p50, uncomp.mean, uncomp.p50);
	// The pass-length distribution the cap acts on. Printed whether or not a cap is set, because the
	// uncapped biggest-pass number is the thing a cap is chosen against.
	Console.WriteLn("  max draws per pass %u (0 = uncapped)  biggest pass built %.2f / %-5u draws  "
					"passes the cap ended %.2f / %u",
		m_max_pass_draws, bigpass.mean, bigpass.p50, capped.mean, capped.p50);
	// The pass-break cause census. Two numbers per cause: the boundaries it was PRESENT at, and in
	// brackets those where it was the SOLE cause -- the ones a merge removing that cause would
	// actually collect. Printed whenever the census RAN: with the key off nothing counted a boundary,
	// and a table of zeroes reads as a plan with no pass boundaries rather than as a census nobody
	// asked for.
	if (m_census_pass_breaks)
	{
		const auto brk_plans = stat([](const MF& f) { return f.break_plans; });
		const auto brk_bnd = stat([](const MF& f) { return f.break_boundaries; });
		const auto brk_multi = stat([](const MF& f) { return f.break_multi; });
		const auto brk_adr = stat([](const MF& f) { return f.break_adreno_passes; });
		const auto cause = [&stat](u32 c) { return stat([c](const MF& f) { return f.break_cause[c]; }); };
		const auto sole = [&stat](u32 c) { return stat([c](const MF& f) { return f.break_cause_sole[c]; }); };
		const auto acause = [&stat](u32 c) { return stat([c](const MF& f) { return f.break_adreno_cause[c]; }); };
		const auto asole = [&stat](u32 c) {
			return stat([c](const MF& f) { return f.break_adreno_cause_sole[c]; });
		};
		Console.WriteLn("  pass breaks: %.2f passes = %.2f plan starts + %.2f boundaries, of which %.2f carry more "
						"than one cause   [x(y) below = present at x boundaries, SOLE cause at y]",
			brk_plans.mean + brk_bnd.mean, brk_plans.mean, brk_bnd.mean, brk_multi.mean);
		std::string key_line, ops_line, adr_line;
		for (u32 c = 0; c < kGSTileGpuBreakCauses; c++)
		{
			const auto cc = cause(c);
			const auto ss = sole(c);
			const auto ac = acause(c);
			const auto as = asole(c);
			const GSTileGpuBreakCause id = static_cast<GSTileGpuBreakCause>(c);
			std::string& line = (c <= static_cast<u32>(GSTileGpuBreakCause::Cap)) ? key_line : ops_line;
			line += StringUtil::StdStringFromFormat(
				"  %s %.2f(%.2f)", gsTileGpuBreakCauseName(id), cc.mean, ss.mean);
			if (ac.mean > 0.0)
				adr_line += StringUtil::StdStringFromFormat(
					"  %s %.2f(%.2f)", gsTileGpuBreakCauseName(id), ac.mean, as.mean);
		}
		Console.WriteLn("    key + cap:%s", key_line.c_str());
		Console.WriteLn("    ops:      %s", ops_line.c_str());
		// The same plan recut under the Adreno pass policy (depth-uniform, readers segregated, 64-draw
		// cap). On a non-Adreno device the three policy causes above are structurally zero, so this is
		// the only line here that says what the primary device's pass structure is made of -- exact in
		// its key and cap halves, an estimate in its ops half (each draw's break_before was decided
		// under the policy that ran). Causes with no population are left out to keep the line readable.
		Console.WriteLn("    as Adreno would cut it: %.2f passes%s", brk_adr.mean, adr_line.c_str());
		// The exact cause-SET distribution, which is what prices a TIER rather than a single merge.
		// Descending by population, and every mask with a whole boundary a frame in it -- the tail
		// below that cannot decide anything and would bury the head.
		const auto dump_masks = [&](const char* what, const std::unordered_map<u32, u32>& hist) {
			std::vector<std::pair<u32, u32>> v(hist.begin(), hist.end());
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			u32 shown = 0;
			for (const auto& [mask, hits] : v)
			{
				const double per = static_cast<double>(hits) / n;
				if (per < 1.0 || shown >= 12)
					break;
				std::string names;
				for (u32 c = 0; c < kGSTileGpuBreakCauses; c++)
				{
					if ((mask & (1u << c)) == 0)
						continue;
					if (!names.empty())
						names += "+";
					names += gsTileGpuBreakCauseName(static_cast<GSTileGpuBreakCause>(c));
				}
				Console.WriteLn("      %s %8.2f  %s", what, per, names.c_str());
				shown++;
			}
		};
		Console.WriteLn("    cause SETS, descending (a tier removing a set of causes collects exactly the "
						"boundaries whose whole set is inside it):");
		dump_masks("as cut ", m_break_masks);
		dump_masks("adreno ", m_break_adreno_masks);
	}
	// The predictor's own line, and only when it ran -- a census of a policy that was not deciding
	// anything reads as a policy that decided nothing, which is a different run. Both counterfactual
	// pass counts are printed beside the metric, because the metric is a ratio and a ratio hides
	// whether it came from a 6000-pass collapse or a 3-pass one.
	if (m_depth_predictor_census)
	{
		const auto pu = stat([](const MF& f) { return f.passes_uniform_key; });
		const auto pm = stat([](const MF& f) { return f.passes_merged_key; });
		Console.WriteLn("  per-frame depth-pass predictor (%s): %u/%u frames merged, %u switches, "
						"metric %.4f saved passes/draw (threshold 1/%u on, 1/%u off) -- "
						"grouping would cut %.2f uniform / %.2f merged passes a frame",
			m_depth_predictor ? "deciding" : "dry run", m_depth_picker.frames_merged, m_depth_picker.frames,
			m_depth_picker.switches,
			m_depth_picker.MeanMetric(), kGSTileGpuDepthMergeOnReciprocal, kGSTileGpuDepthMergeOffReciprocal,
			pu.mean, pm.mean);
		// What the polarity in force COST, which the pair above cannot say: it names the two
		// groupings, not which of them ran. Positive means the polarity that ran is the more
		// expensive one and the other would have cut the frame into fewer passes. This is the line a
		// pass-structure policy is answerable to -- without it a vendor gate can charge a title a
		// third of its pass count with nothing in any log naming the bill.
		const auto pa = stat([](const MF& f) { return f.passes_active_key; });
		const auto po = stat([](const MF& f) { return f.passes_other_key; });
		Console.WriteLn("    the depth polarity in force costs %+.2f passes a frame: %.2f under the "
						"polarity that ran, %.2f under the other one",
			pa.mean - po.mean, pa.mean, po.mean);
	}
	Console.WriteLn("  writebacks %6.2f / %-4u ops, %8.2f / %-5u pages, %.2f / %u pass breaks   seeds %6.2f / %-4u ops, "
					"%8.2f / %-5u pages, %.2f / %u pass breaks (of the seeds, depth %.2f / %u ops, %.2f / %u pages)",
		wbo.mean, wbo.p50, wbp.mean, wbp.p50, wbrk.mean, wbrk.p50, sdo.mean, sdo.p50, sdp.mean, sdp.p50, sbrk.mean,
		sbrk.p50, sdzo.mean, sdzo.p50, sdzp.mean, sdzp.p50);
	// ...and what the executor records for them, which is the number the 3.67 us per-dispatch term
	// is charged against. Pages are unchanged by batching, by construction; if these two ever equal
	// the op count the batch found nothing to merge and the barrier bracket is the whole saving.
	Console.WriteLn("    writeback dispatches %6.2f / %-4u (from %.2f ops), barrier runs %6.2f / %-4u "
					"(of which %.2f / %u forced by a page collision)",
		wbd.mean, wbd.p50, wbo.mean, wbr.mean, wbr.p50, wbh.mean, wbh.p50);
	Console.WriteLn("  target binds (rule 2: the read came off a resident target, no bytes composed) %.2f / %u draws, "
					"%.2f / %u pass breaks",
		binds.mean, binds.p50, bindbrk.mean, bindbrk.p50);
	Console.WriteLn("  self-reads (snapshot semantics) %.2f / %u   DATE: %.2f / %u pass breaks, %.2f / %u snapshots   "
					"alias-steal pages %.2f / %u   lossy pages (no byte road) %.2f / %u (of which depth %.2f / %u)   "
					"skipped draws %.2f / %u   mid-frame flushes %.2f / %u",
		self.mean, self.p50, dbrk.mean, dbrk.p50, snaps.mean, snaps.p50, alias.mean, alias.p50, lossy.mean,
		lossy.p50, lossyz.mean, lossyz.p50, skipped.mean, skipped.p50, flushes.mean, flushes.p50);
	// What the DATE staleness test costs for being a bounding UNION. Counted when the census is armed
	// and taken by nothing even then: a rect set covers everything the union covers, so every break it
	// skips is one the union made over pixels nobody wrote. The pair is the lever's whole size.
	if (m_census_date_cover && dbrk.mean > 0.0)
	{
		Console.WriteLn("    DATE staleness: %.2f / %u draws asked, %.2f / %u broke   a %u-rect cover would skip "
						"%.2f / %u of the breaks (%.1f%%) and the snapshots with them",
			ddraw.mean, ddraw.p50, dbrk.mean, dbrk.p50, GSTileGpuDateCover::kMaxRects, dskip.mean, dskip.p50,
			100.0 * dskip.mean / std::max(dbrk.mean, 1e-9));
	}
	// What those snapshots MOVE, which the count above cannot say: a copy is 8 KB a page, and the
	// road's whole cost is bytes. Both bills printed side by side off the one run -- what the copies
	// cover and what copying the whole target would have -- because the lever's value is the
	// difference and a difference read across two runs is two scenes, not one change. The megabytes
	// are the same numbers in the units the DRAM bandwidth argument is made in.
	if (snaps.mean > 0.0)
	{
		Console.WriteLn("  DATE snapshot copies: %.2f / %u pages (%.2f MB/f) against %.2f / %u full-target "
						"(%.2f MB/f), %.2f / %u narrowed of %.2f / %u (TileGpuNarrowDateSnapshot %s)",
			snapp.mean, snapp.p50, snapp.mean * 8192.0 / 1048576.0, snapf.mean, snapf.p50,
			snapf.mean * 8192.0 / 1048576.0, snapn.mean, snapn.p50, snaps.mean, snaps.p50,
			m_narrow_date_snapshot ? "on" : "off");
		// The distribution, because a mean over "one page" and "the whole target" is a number no
		// copy has. Doubling bands, the last one open; every band printed whether or not it has a
		// population, so two runs' lines sit under each other column for column.
		Console.WriteLn("    pages per copy: 1 %.2f/%u  2 %.2f/%u  3-4 %.2f/%u  5-8 %.2f/%u  9-16 %.2f/%u  "
						"17-32 %.2f/%u  33-64 %.2f/%u  65+ %.2f/%u",
			snaph(0).mean, snaph(0).p50, snaph(1).mean, snaph(1).p50, snaph(2).mean, snaph(2).p50,
			snaph(3).mean, snaph(3).p50, snaph(4).mean, snaph(4).p50, snaph(5).mean, snaph(5).p50,
			snaph(6).mean, snaph(6).p50, snaph(7).mean, snaph(7).p50);
	}
	// Surface-identity containment, counted and not taken. The two break columns are the same draw
	// stream keyed twice -- on the surfaces the draws take, and on the containers they would take --
	// so the difference is the passes the fold would remove and nothing else. Colour first because
	// that is the column containment acts on; the full key adds the depth surface and its presence,
	// which containment leaves alone, so the gap between the two columns is depth's own contribution.
	//
	// ⚠️ Per DRAWN frame -- like every other line here since 2026-08-31, where it used to be the only
	// one -- and the p50 is dropped for a max. A title that presents a frame it drew nothing into --
	// Yu-Gi-Oh! presents two for every one it draws -- would otherwise report half the breaks it
	// has, and this census exists to be compared against an off-line count over the same draw
	// stream. Getting the denominator wrong there reads as the fold working twice as well as it
	// does. The count is the block-wide `drawn` above; this block is where its definition came from.
	const auto ctstat = [this, n](auto get) {
		double sum = 0.0;
		u32 worst = 0;
		for (const MF& f : m_model_frames)
		{
			const u32 v = get(f);
			sum += v;
			worst = std::max(worst, v);
		}
		struct
		{
			double mean;
			u32 max;
		} r{sum / n, worst};
		return r;
	};
	const auto ct_brk = ctstat([](const MF& f) { return f.contain_breaks; });
	const auto ct_brkf = ctstat([](const MF& f) { return f.contain_breaks_folded; });
	const auto ct_key = ctstat([](const MF& f) { return f.contain_full_breaks; });
	const auto ct_keyf = ctstat([](const MF& f) { return f.contain_full_breaks_folded; });
	const auto ct_fold = ctstat([](const MF& f) { return f.contain_folds; });
	const auto ct_fdraw = ctstat([](const MF& f) { return f.contain_folded_draws; });
	const auto ct_first = ctstat([](const MF& f) { return f.contain_first_sight; });
	const auto ct_grow = ctstat([](const MF& f) { return f.contain_growth_pages; });
	const auto ct_big = ctstat([](const MF& f) { return f.contain_max_pages; });
	const auto ct_regrow = ctstat([](const MF& f) { return f.contain_regrow_refused; });
	const auto ct_none = ctstat([](const MF& f) { return f.contain_ref_none; });
	const auto ct_fam = ctstat([](const MF& f) { return f.contain_ref_family; });
	const auto ct_str = ctstat([](const MF& f) { return f.contain_ref_stride; });
	const auto ct_aln = ctstat([](const MF& f) { return f.contain_ref_align; });
	const auto ct_dlt = ctstat([](const MF& f) { return f.contain_ref_delta; });
	const auto ct_col = ctstat([](const MF& f) { return f.contain_ref_column; });
	const auto ct_ext = ctstat([](const MF& f) { return f.contain_ref_extent; });
	const auto ct_wrp = ctstat([](const MF& f) { return f.contain_ref_wrap; });
	const auto ct_disp = ctstat([](const MF& f) { return f.contain_ref_displaced; });
	const auto ct_unf = ctstat([](const MF& f) { return f.contain_unfolded; });
	const auto ct_bind = ctstat([](const MF& f) { return f.contain_bind_refused; });
	Console.WriteLn("  containment (%s; mean per DRAWN frame, %u of %u): colour-key breaks "
					"%8.2f unfolded -> %8.2f folded   full key %8.2f -> %8.2f",
		m_contain_surfaces ? "TAKEN" : "probed, NOT taken", drawn, static_cast<u32>(m_model_frames.size()),
		ct_brk.mean, ct_brkf.mean, ct_key.mean, ct_keyf.mean);
	Console.WriteLn("    folds %.2f of %.2f first sights (%.2f draws land in a container), growth %.2f pages, "
					"biggest container %u pages, outgrown %.2f, unfolded %.2f",
		ct_fold.mean, ct_first.mean, ct_fdraw.mean, ct_grow.mean, ct_big.max, ct_regrow.mean, ct_unf.mean);
	Console.WriteLn("    refused: no container %.2f  family %.2f  stride %.2f  align %.2f  delta %.2f  column %.2f  "
					"budget %.2f  wrap %.2f  legal but DISPLACED (only the zero-offset merge ships) %.2f",
		ct_none.mean, ct_fam.mean, ct_str.mean, ct_aln.mean, ct_dlt.mean, ct_col.mean, ct_ext.mean, ct_wrp.mean,
		ct_disp.mean);
	// Q5: what the missing per-slot offset bind costs. Under ~20 a frame on the GT4 pair and the
	// design defers building it; above, it is the next rung's first job.
	Console.WriteLn("    rule-2 binds refused for the fold's base alone (the offset bind is not built) %.2f / %u draws",
		ct_bind.mean, ct_bind.max);
	// Draw reordering, and only where the lever asked for it -- a census line printed on a run that
	// modelled nothing reads as a model that found nothing. Per DRAWN frame like the containment
	// census above and for the same reason: a frame the game presents without drawing into halves
	// every mean here.
	if (m_reorder_setting != 0)
	{
		u32 ro_drawn = 0;
		for (const MF& f : m_model_frames)
			ro_drawn += (f.reorder_draws != 0) ? 1u : 0u;
		const double ro_n = static_cast<double>(std::max(ro_drawn, 1u));
		const auto rostat = [this, ro_n](auto get) {
			double sum = 0.0;
			u32 worst = 0;
			for (const MF& f : m_model_frames)
			{
				const u32 v = get(f);
				sum += v;
				worst = std::max(worst, v);
			}
			struct
			{
				double mean;
				u32 max;
			} r{sum / ro_n, worst};
			return r;
		};
		const auto ro_draws = rostat([](const MF& f) { return f.reorder_draws; });
		const auto ro_ok = rostat([](const MF& f) { return f.reorder_admitted; });
		const auto ro_fb = rostat([](const MF& f) { return f.reorder_ref_feedback; });
		const auto ro_date = rostat([](const MF& f) { return f.reorder_ref_date; });
		const auto ro_roaa = rostat([](const MF& f) { return f.reorder_ref_roaa; });
		const auto ro_prep = rostat([](const MF& f) { return f.reorder_ref_prep; });
		const auto ro_raw = rostat([](const MF& f) { return f.reorder_haz_raw; });
		const auto ro_war = rostat([](const MF& f) { return f.reorder_haz_war; });
		const auto ro_waw = rostat([](const MF& f) { return f.reorder_haz_waw; });
		const auto ro_rar = rostat([](const MF& f) { return f.reorder_haz_rar; });
		const auto ro_runs = rostat([](const MF& f) { return f.reorder_runs; });
		const auto ro_pass = rostat([](const MF& f) { return f.reorder_passes; });
		const auto ro_moved = rostat([](const MF& f) { return f.reorder_passes_moved; });
		const auto ro_peakd = rostat([](const MF& f) { return f.reorder_peak_draws; });
		const auto ro_peakr = rostat([](const MF& f) { return f.reorder_peak_runs; });
		Console.WriteLn("  reorder (%s; %u runs; mean per DRAWN frame, %u of %u): reorderable %.2f "
						"of %.2f draws, refused %.2f (feedback %.2f, DATE %.2f, ROAA %.2f, prep-only %.2f)",
			m_reorder_census ? "modelled, NOT taken" : "TAKEN", m_reorder_max_runs, ro_drawn,
			static_cast<u32>(m_model_frames.size()), ro_ok.mean, ro_draws.mean, ro_draws.mean - ro_ok.mean,
			ro_fb.mean, ro_date.mean, ro_roaa.mean, ro_prep.mean);
		Console.WriteLn("    hazard rejects %.2f (RAW %.2f, WAR %.2f, WAW %.2f, RAR %.2f)   runs emitted %.2f   "
						"backlog peak %u draws / %u runs",
			ro_raw.mean + ro_war.mean + ro_waw.mean + ro_rar.mean, ro_raw.mean, ro_war.mean, ro_waw.mean,
			ro_rar.mean, ro_runs.mean, ro_peakd.max, ro_peakr.max);
		// The model's own before-and-after. Only the count-only arm has it: on the taken arm the plan
		// IS the reordered one, and its pass count is the `passes` column above.
		//
		// ⚠️ The projection is optimistic against what the taken arm then measures, by design and not
		// by accident. It counts passes over the emission order using each draw's own break_before,
		// while the road forces an extra break at every flush -- accumulation's pass and the plan
		// build's have to agree about where a pass starts, and after a flush the draw follows the last
		// emitted run rather than anything accumulation still holds.
		if (ro_pass.mean > 0.0)
		{
			const double drop = 100.0 * (ro_pass.mean - ro_moved.mean) / ro_pass.mean;
			Console.WriteLn("    passes as planned %8.2f -> reordered %8.2f (%+.1f%%), flush breaks not counted",
				ro_pass.mean, ro_moved.mean, -drop);
		}
	}
	Console.WriteLn("  alpha test folded at plan time (fragment alpha interval): all-fail %.2f / %u   "
					"all-pass %.2f / %u",
		afold_f.mean, afold_f.p50, afold_p.mean, afold_p.p50);
	Console.WriteLn("  alpha test that VARIES under a non-KEEP AFAIL: %.2f / %u draws   of which RGB_ONLY "
					"%.2f / %u   of which depth-writing %.2f / %u",
		afv.mean, afv.p50, afvr.mean, afvr.p50, afvz.mean, afvz.p50);
	// ...and of those, the ones whose failing fragments the DISCARD would drop a write for, which is
	// the population the draw split exists for. The discard line is what the road still owes: every
	// draw on it loses something the console writes. Counted on both arms -- an OFF run reports the
	// same population an ON run does, or the two are not comparable.
	Console.WriteLn("    of which the discard would drop a write: split exactly %.2f / %u   split with the "
					"fragments reordered %.2f / %u   still discarding %.2f / %u (no depth test %.2f, "
					"FB_ONLY refused %.2f / %u)   extra draws %.2f / %u, overlap asked %.2f "
					"(TileGpuAfailSplit %s, TileGpuSplitRefuseFbOnly %s)",
		afss.mean, afss.p50, afsr.mean, afsr.p50, afsd.mean, afsd.p50, afsn.mean, afsf.mean, afsf.p50,
		afsx.mean, afsx.p50, afso.mean, m_afail_split ? "on" : "off", m_split_refuse_fb_only ? "on" : "off");
	Console.WriteLn("  in-pass destination read: %.2f / %u draws admitted, %.2f / %u passes declared, "
					"%.2f / %u draws inside a declaring pass",
		srd.mean, srd.p50, srp.mean, srp.p50, srpd.mean, srpd.p50);
	// ...and the same population broken out by the reason each draw asked, which is the only view that
	// distinguishes a title whose readers are one dense class from one whose readers are spread thin.
	// A total cannot: that is how Xenosaga came to be recorded as a 16-bit-saturated title when it
	// renders to 32-bit frames and its bulk is a single-bit alpha FBMSK.
	{
		static constexpr const char* kClassNames[kGSTileGpuAdmissionClasses] = {
			"DATE", "exotic blend", "16-bit blend", "partial FBMSK", "AFAIL alpha keep"};
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
		{
			const auto want = stat([c](const MF& f) { return f.class_wanted[c]; });
			if (want.mean == 0.0)
				continue;
			const auto taxd = stat([c](const MF& f) { return f.class_taxed[c]; });
			const auto runs = stat([c](const MF& f) { return f.class_runs[c]; });
			const auto peak = lvl([c](const MF& f) { return f.class_peak[c]; });
			const auto refu = lvl([c](const MF& f) { return f.class_refused[c]; });
			// Who refused it, not just how often: on a probe arm the refusal is the KEY's and the cost
			// peak beside it is still the budget's own opinion, which is the number the arm is pricing.
			// ⚠️ The peak is MICROSECONDS OF GPU A FRAME since the 2026-08-31 re-fit, and the column
			// says so: it used to be a dimensionless score against a line of 256 and the numbers are
			// now three orders of magnitude bigger. A log read against the old wording compares
			// nothing.
			Console.WriteLn("    class %-17s wanted %.2f / %u draws (taxed %.2f / %u + runs %.2f / %u)   "
							"refusing it would buy %.2f / %u us   %s refused it in %.0f%% of frames",
				kClassNames[c], want.mean, want.p50, taxd.mean, taxd.p50, runs.mean, runs.p50, peak.mean,
				peak.p50, (m_probe_refused_classes & (1u << c)) ? "the PROBE" : "budget", refu.mean * 100.0);
		}
	}
	// The colour write mask as a run key, and it is a funnel rather than three independent numbers:
	// carried a mask -> the fragment stage could serve it exactly -> it did. The first two are counted
	// on both arms on purpose, so an OFF run answers "is TileGpuShaderWriteMask worth a device round
	// on this title" without a second build. The gap between the first two is the refusals -- a draw
	// the executor's blend unit still has to touch, or one the shader's byte tail does not already
	// own -- and those keep the pipeline mask whatever the lever says.
	{
		const auto mp = stat([](const MF& f) { return f.mask_partial_draws; });
		if (mp.mean > 0.0)
		{
			const auto ms = stat([](const MF& f) { return f.mask_servable_draws; });
			const auto mm = stat([](const MF& f) { return f.mask_shader_draws; });
			Console.WriteLn("  colour write mask: %.2f / %u draws carry a partial one   the fragment stage could "
							"serve %.2f / %u of them   it served %.2f / %u (TileGpuShaderWriteMask %s)",
				mp.mean, mp.p50, ms.mean, ms.p50, mm.mean, mm.p50,
				GSConfig.TileGpuShaderWriteMask ? (m_shader_write_mask ? "on" : "on, refused by the device") : "off");
		}
	}
	Console.WriteLn("  byte-road passes %.2f / %u   of which mixed texel arms %.2f / %u (%.1f%%), carrying %.2f / %u "
					"byte-road draws",
		txp.mean, txp.p50, txm.mean, txm.p50, (txp.mean > 0.0) ? (txm.mean * 100.0 / txp.mean) : 0.0, txmd.mean,
		txmd.p50);
	Console.WriteLn("  fragment variant as a run key: %.2f / %u runs, of which %.2f / %u cut by the variant alone "
					"(the COST, ~237 ns each)   narrowed draws %.2f / %u   off-byte-road draws inside a "
					"byte-compiled pass %.2f / %u (the contamination the decoupling ends)",
		vruns.mean, vruns.p50, vcalls.mean, vcalls.p50, vnarrow.mean, vnarrow.p50, vfree.mean, vfree.p50);
	// The SPECIALIZATION half of that key, priced on its own, and what the pass guard did about it.
	// `despec` is the same frame counted with the frozen state withheld -- the unspecialized arm's
	// number, off this run rather than a second one -- so the shatter ratio between the two is the
	// bind bill the frozen state adds, and the programs-per-pass pair is the working set behind it.
	Console.WriteLn("  ...specialization's own share: runs %.2f / %u submitted, %.2f unguarded, %.2f / %u "
					"de-specialized -- ADDED BINDS %.2f (budget %u)   programs per frame's passes %.2f vs "
					"%.2f   guard de-specialized %.2f / %u passes carrying %.2f / %u draws",
		vruns.mean, vruns.p50, vrunsu.mean, vrunsd.mean, vrunsd.p50, vrunsu.mean - vrunsd.mean, m_max_spec_binds,
		vprog.mean, vprogd.mean, sgp.mean, sgp.p50, sgd.mean, sgd.p50);
	// The GS scissor priced as a device question: what the clip-plane road buys, and what the
	// per-call vkCmdSetScissor road that needs no device feature would cost instead. Runs on both
	// roads, so the arms are directly comparable.
	{
		const auto scdraws = stat([](const MF& f) { return f.scissor_draws; });
		const auto scsub = stat([](const MF& f) { return f.scissor_subrect; });
		const auto sccut = stat([](const MF& f) { return f.scissor_cuts; });
		const auto scdist = stat([](const MF& f) { return f.scissor_distinct; });
		const auto scmax = lvl([](const MF& f) { return f.scissor_pass_distinct_max; });
		const auto scex = stat([](const MF& f) { return f.scissor_extra_calls; });
		Console.WriteLn("  scissor: draws %.2f / %u   narrower than target %.2f / %u (%.2f%%)   cuts the draw "
						"%.2f / %u (%.2f%%)   distinct rects %.2f / %u summed over passes, most in one pass "
						"%.2f / %u / %u worst   PER-CALL-SCISSOR extra calls %.2f / %u / %u worst",
			scdraws.mean, scdraws.p50, scsub.mean, scsub.p50,
			(scdraws.mean > 0.0) ? (scsub.mean * 100.0 / scdraws.mean) : 0.0, sccut.mean, sccut.p50,
			(scdraws.mean > 0.0) ? (sccut.mean * 100.0 / scdraws.mean) : 0.0, scdist.mean, scdist.p50, scmax.mean,
			scmax.p50, scmax.max, scex.mean, scex.p50, scex.max);
	}
	// ...and the As blend factor priced the same way: what the dual-source feature carries, and how
	// much of that the two feature-free roads take between them.
	{
		// scissor_draws is the plan's own draw count -- every draw the state-row loop walks -- so it
		// is what the dual-source population is a fraction OF.
		const auto pld = stat([](const MF& f) { return f.scissor_draws; });
		const auto dsd = stat([](const MF& f) { return f.dualsrc_draws; });
		const auto dss = stat([](const MF& f) { return f.dualsrc_src_only; });
		const auto dsa = stat([](const MF& f) { return f.dualsrc_carrier; });
		const auto dsv = stat([](const MF& f) { return f.dualsrc_restore; });
		const auto dsn = stat([](const MF& f) { return f.dualsrc_needs_comp; });
		const auto dsk = stat([](const MF& f) { return f.dualsrc_companions; });
		Console.WriteLn("  dual-src: draws %.2f / %u / %u worst (%.2f%% of plan draws)   As on the source alone "
						"%.2f / %u   ROADS alpha free %.2f / %u, alpha restored %.2f / %u, companion needed "
						"%.2f / %u / %u worst   companions ISSUED %.2f / %u",
			dsd.mean, dsd.p50, dsd.max, (pld.mean > 0.0) ? (dsd.mean * 100.0 / pld.mean) : 0.0, dss.mean, dss.p50,
			dsa.mean, dsa.p50, dsv.mean, dsv.p50, dsn.mean, dsn.p50, dsn.max, dsk.mean, dsk.p50);
	}
	Console.WriteLn("  stalls: upload sub-block %.2f/%u (%.2f pages)  local-read %.2f/%u (%.2f)  clut %.2f/%u (%.2f)  "
					"sync-all %.2f/%u (%.2f)",
		s_up.mean, s_up.p50, p_up.mean, s_rd.mean, s_rd.p50, p_rd.mean, s_cl.mean, s_cl.p50, p_cl.mean, s_all.mean,
		s_all.p50, p_all.mean);
	// ...and what those stalls' pool calls actually COST, which the counts above cannot show. A drain
	// submits the frame's command buffer and waits for the whole recorded plan; an out-of-band call
	// submits its own tiny buffer and waits only behind what was already queued. Measured ~1.07 ms
	// against ~37 us on the SD865, so a site with a few drains can outweigh one with a hundred
	// out-of-band trips. `idle` reached the device for neither and is free.
	Console.WriteLn("  stall pool calls by road (oob / drain / idle): upload sub-block %.2f/%.2f/%.2f  "
					"local-read %.2f/%.2f/%.2f  clut %.2f/%.2f/%.2f  sync-all %.2f/%.2f/%.2f",
		ro_up.mean, rd_up.mean, ri_up.mean, ro_rd.mean, rd_rd.mean, ri_rd.mean, ro_cl.mean, rd_cl.mean, ri_cl.mean,
		ro_all.mean, rd_all.mean, ri_all.mean);
	// The other half of the upload-sub-block population: the spills served on the device instead.
	// Read it beside the stall line above -- the two add up to the spills the frame takes, and the
	// refusals say which clause sent the rest down the blocking road.
	Console.WriteLn("  upload merge: %.2f / %-4u served on the GPU, %.2f / %-4u pages   refused: no sole byte-road "
					"owner %.2f, a CPU read already wanted it %.2f, half a byte %.2f, rect is a claim %.2f, "
					"footprint overflow %.2f",
		mrg.mean, mrg.p50, mrgp.mean, mrgp.p50, mref_o.mean, mref_c.mean, mref_b.mean, mref_s.mean, mref_f.mean);
	// ...and the elision's two size budgets, which say why a merge-road flush that survives the gate
	// survived it. Zero on both with the gate off (nothing is ever elided, so nothing is bounded);
	// with it on, these are the boundary flushes the mid-frame flush line above is now made of.
	const auto mbe = stat([](const MF& f) { return f.merge_bound_epochs; });
	const auto mbp = stat([](const MF& f) { return f.merge_bound_pages; });
	Console.WriteLn("  merge elision bounds (%s, %u epochs / %u ring pages): epoch budget %.2f / %-4u, "
					"ring-page budget %.2f / %-4u",
		GSConfig.TileGpuFlushGateUploadMerge ? "on" : "off", kGSTileGpuMergeElisionMaxEpochs,
		kGSTileGpuMergeElisionMaxRingPages, mbe.mean, mbe.p50, mbp.mean, mbp.p50);
	// ...and what those merges COST in render passes, which no other number here can show: the pass
	// count above it is the PLAN's, and a seed runs at a pass head inside no plan pass. Counted in
	// the executor at the BeginRenderPass itself. Read the merge's column against merge_ops above --
	// batched they are equal, per-page it is one pass per merged PAGE -- and read the total beside
	// the plan-pass line, which has never included any of these.
	Console.WriteLn("  seed render passes (TileGpuMergeSeedBatch %s): %.2f /drawn frame, of which the upload "
					"merge's %.2f /drawn frame",
		m_merge_seed_batch ? "on" : "off",
		static_cast<double>(g_gs_device->GetTileGpuSeedRenderPasses()) / n,
		static_cast<double>(g_gs_device->GetTileGpuMergeSeedRenderPasses()) / n);
	if (mref_c.mean > 0.0)
	{
		// The CPU-read refusals, priced. The clause fires first and returns, so "no sole byte-road
		// owner 0.00" above describes the pages that got PAST it, not the ones it refused -- and how
		// much of a lifted clause would actually convert is otherwise unknown. `served` is the
		// fraction that converts.
		Console.WriteLn("  cpu-read refusals, would refuse next: none (served) %.2f  no sole owner %.2f  "
						"no byte road / no texels %.2f",
			mnext(kMergeRefNone).mean, mnext(kMergeRefOwner).mean, mnext(kMergeRefRoad).mean);
		// ...and how old the marks that refused them are, which is what a window has to cover. A
		// title that re-reads its palettes every frame refuses on FRESH marks and any window keeps
		// its protection; a title refused by the accumulated union of every read since Reset refuses
		// on old ones, and a window collects them.
		Console.WriteLn("  cpu-read mark age (frames): this %.2f  1 %.2f  2-3 %.2f  4-15 %.2f  older %.2f",
			mage(0).mean, mage(1).mean, mage(2).mean, mage(3).mean, mage(4).mean);
	}

	// The wait bill, which the stall census above cannot show: a stall is one CONSUMER asking, and
	// what costs the frame is the number of times the GS thread blocked on the GPU to answer it.
	// One such wait per frame serializes the whole pipeline — frame becomes cpu + gpu instead of
	// max(cpu, gpu), and the count above one does not matter — so the number to read here is
	// whether the total is ZERO, not whether it fell.
	//
	// Both roads block. The drain road submits the frame's buffer and waits for it; the
	// out-of-band road submits its own tiny buffer and waits for that, which still waits for
	// everything already queued because queue submissions retire in order. TileGpu reported
	// NEITHER until now (only exact-Tile printed the out-of-band line), which is how a regression
	// built entirely of out-of-band waits once hid behind falling drain counters.
	//
	// ⚠️ Basis: DRAWN frames, matching every other line in this census and matching the runner's
	// stats.json, which has always divided by them. It used to be model frames -- one per VSync,
	// present-only frames included -- so on a title that draws every other frame this block read
	// half of what stats.json read of the same counters, and neither number said which it was.
	const double mframes = n;
	const auto pcalls = stat([](const MF& f) { return f.pull_calls; });
	Console.WriteLn("  pool calls the stalls issued %.2f / %u  (one per owner x block mask x byte window, over the "
					"whole page set it needs -- the unit the device round trip is charged in)",
		pcalls.mean, pcalls.p50);
	// ...and the same calls by VOLUME, which the line above cannot show. A round trip is priced per
	// call, but what a call moves over the bus is a rectangle -- the union bounding box of the runs
	// the pool collects -- so a scattered page set fetches texels its pages do not hold, and a call
	// whose pages the CPU shadow already holds fetches nothing while still being a call. Asked pages
	// beside fetched pages so the gap between them IS the elision, with no arithmetic off two lines.
	//
	// ⚠️ Fetched pages are summed per CALL, so a page two calls name (two owners, or two block masks
	// on one page) counts twice: it costs two downloads and the number is what the device moved, not
	// how much guest memory is distinct. The asked figure has no such double count.
	const auto dlp = stat([](const MF& f) { return f.pull_dl_pages; });
	const auto dlt = stat([](const MF& f) { return f.pull_dl_texels; });
	Console.WriteLn("  ...and what they FETCHED %.2f pages / %-5u  %.1f KB / %-6u  (asked %.2f pages; KB is the "
					"download rectangle at four bytes a texel, not the guest bytes it stores)",
		dlp.mean, dlp.p50, dlt.mean * 4.0 / 1024.0, static_cast<u32>(static_cast<u64>(dlt.p50) * 4 / 1024),
		p_up.mean + p_rd.mean + p_cl.mean + p_all.mean);
	//
	// ⚠️ Itemised off the DEVICE's own counters, not off the pool's drain count, since 2026-08-24.
	// The old line summed drains + out-of-band and called that the bill, which made it structurally
	// incapable of seeing a wait belonging to neither -- and one does: the source-descriptor ring
	// wrapping onto a set an in-flight submission still reads. On the SD865 that class was the
	// entire residual while this line read a flat zero, and it took an elimination argument rather
	// than a reading to name it. The pool's drains stay, as the named SUB-cause of the sync class,
	// so a sync figure larger than the drain figure is itself the report that something else is in
	// there. Total = the GpuBlockingWaits population exactly; ring backpressure is the one class
	// deliberately outside it.
	const double drains = static_cast<double>(m_target_pool.Drains());
	const double sync = static_cast<double>(g_gs_device->GetSyncWaitCalls());
	const double oob = static_cast<double>(g_gs_device->GetOobWaitCalls());
	const double srcset = static_cast<double>(g_gs_device->GetSourceSetWaitCalls());
	Console.WriteLn("  BLOCKING GPU WAITS %8.2f /drawn frame  =  sync %.2f (%.2f ms/drawn frame, of which pool "
					"drains %.2f at %.2f ms)  +  out-of-band %.2f (%.2f ms/drawn frame)  +  source-set wrap %.2f "
					"(%.2f ms/drawn frame)   [ring backpressure, not a drain: %.2f /drawn frame, %.2f ms]",
		(sync + oob + srcset) / mframes, sync / mframes,
		static_cast<double>(g_gs_device->GetSyncWaitNs()) / mframes / 1e6, drains / mframes,
		static_cast<double>(m_target_pool.DrainWallNs()) / mframes / 1e6, oob / mframes,
		static_cast<double>(g_gs_device->GetOobWaitNs()) / mframes / 1e6, srcset / mframes,
		static_cast<double>(g_gs_device->GetSourceSetWaitNs()) / mframes / 1e6,
		static_cast<double>(g_gs_device->GetRingWaitCalls()) / mframes,
		static_cast<double>(g_gs_device->GetRingWaitNs()) / mframes / 1e6);
	// The mid-frame kick's own volume, printed on BOTH arms. A lever whose whole effect is
	// submission timing leaves no other trace, and "offered 0 / taken 0" with the lever ON is a
	// different report from the lever being off -- one says the gate never opened, the other says
	// nothing was asked. Offered-minus-taken is the fence gate declining because the next command
	// buffer is still executing, which is the pipeline being full and is the expected majority.
	Console.WriteLn("  mid-frame kick (TileGpuKickReadbackFrames %s, pass cadence %u): offered %.2f /drawn frame, "
					"taken %.2f /drawn frame",
		GSConfig.TileGpuKickReadbackFrames ? "ON" : "off",
		gsTileGpuKickPassCadence(GSConfig.TileGpuKickPassCadence),
		static_cast<double>(g_gs_device->GetTileGpuKicksOffered()) / mframes,
		static_cast<double>(g_gs_device->GetTileGpuKicksTaken()) / mframes);
	// ...and the PREDICTOR's own line, on the one arm it runs (see GSTileGpuKickPolicyPicker: it has
	// no dry run, because its verdict changes the quantity it reads). Four numbers, and the last one
	// is the point: a device that charges nothing for a mid-frame submit backs off nowhere, and one
	// that charges 0.1 ms backs off wherever the wait it buys is smaller. That single number is why
	// the same title decides opposite ways on two devices with no vendor gate anywhere in this.
	if (GSConfig.TileGpuAdaptiveKick)
	{
		const double pframes = static_cast<double>(g_gs_device->GetTileGpuKickPredictorFrames());
		Console.WriteLn("  mid-frame kick predictor (EmuCore/GS/TileGpuAdaptiveKick): cadence ON in %llu/%llu "
						"frames (%.1f%%), %llu switches, %llu cadence submits -- bubble %.3f ms latched against "
						"%.3f us a render pass measured on this device (the bubble is a DIFFERENCE: %.3f ms of "
						"wait peak with the cadence off, less %.3f ms with it on)",
			g_gs_device->GetTileGpuKickPredictorFramesOn(), g_gs_device->GetTileGpuKickPredictorFrames(),
			pframes ? (100.0 * static_cast<double>(g_gs_device->GetTileGpuKickPredictorFramesOn()) / pframes) : 0.0,
			g_gs_device->GetTileGpuKickPredictorSwitches(), g_gs_device->GetTileGpuKickPredictorSubmits(),
			static_cast<double>(g_gs_device->GetTileGpuKickPredictorBubbleNs()) / 1e6,
			static_cast<double>(g_gs_device->GetTileGpuKickPredictorTaxNs()) / 1e3,
			static_cast<double>(g_gs_device->GetTileGpuKickPredictorWaitOffNs()) / 1e6,
			static_cast<double>(g_gs_device->GetTileGpuKickPredictorWaitOnNs()) / 1e6);
	}

	// Rule 3 as probed. Nothing in this block changed a pixel: it says what a cache of
	// materialised sources WOULD have served, and for the rest, exactly which clause refused.
	const auto tdraws = stat([](const MF& f) { return f.tex_draws; });
	const auto tunsup = stat([](const MF& f) { return f.tex_unsupported; });
	const auto thi = stat([](const MF& f) { return f.tex_hi_draws; });
	const auto elig = stat([](const MF& f) { return f.src_eligible; });
	const auto shit = stat([](const MF& f) { return f.src_hit; });
	const auto sreb = stat([](const MF& f) { return f.src_rebuild; });
	const auto sfresh = stat([](const MF& f) { return f.src_fresh; });
	const auto rregion = stat([](const MF& f) { return f.src_ref_region; });
	const auto rsfmt = stat([](const MF& f) { return f.src_ref_srcfmt; });
	const auto smip = stat([](const MF& f) { return f.src_mip_draws; });
	const auto smipl = stat([](const MF& f) { return f.src_mip_live; });
	const auto rcap = stat([](const MF& f) { return f.src_ref_capacity; });
	const auto rpal = stat([](const MF& f) { return f.src_ref_palette; });
	const auto rpin = stat([](const MF& f) { return f.src_ref_pin; });
	const auto sdist = stat([](const MF& f) { return f.src_distinct; });
	const auto scalls = stat([](const MF& f) { return f.src_extra_calls; });
	const auto skept = stat([](const MF& f) { return f.src_stamp_kept; });
	const auto smoved = stat([](const MF& f) { return f.src_stamp_moved; });
	const auto srescued = stat([](const MF& f) { return f.src_stamp_rescued; });
	const auto sopaque = stat([](const MF& f) { return f.src_stamp_opaque; });
	// Per frame, not p50-minus-p50: rescued is a subset of moved within one frame, so the
	// difference is a real per-frame quantity and its median is the one worth printing.
	const auto sredo = stat([](const MF& f) { return f.src_stamp_moved - f.src_stamp_rescued; });
	// Served share of textured draws, the checkpoint's headline: rule 2's binds plus rule 3's
	// eligible draws over the draws in a format either road samples.
	const double served = (tdraws.mean > 0.0) ? (binds.mean + elig.mean) * 100.0 / tdraws.mean : 0.0;
	const double stamp_total = skept.mean + smoved.mean;

	// Rule 3 as BUILT and SERVED, over the whole eligible population -- direct colour materialises to
	// the image a draw samples, paletted to an index image the palette expansion turns into one.
	const auto sbuild = stat([](const MF& f) { return f.src_builds; });
	const auto schit = stat([](const MF& f) { return f.src_cache_hits; });
	const auto scresc = stat([](const MF& f) { return f.src_cache_rescued; });
	const auto scpin = stat([](const MF& f) { return f.src_pin_refusals; });
	const auto sccap = stat([](const MF& f) { return f.src_cap_refusals; });
	const auto scfail = stat([](const MF& f) { return f.src_build_failed; });
	const auto sserved = stat([](const MF& f) { return f.src_served; });
	const auto sbind = stat([](const MF& f) { return f.src_bind_slots; });
	const auto rbind = stat([](const MF& f) { return f.src_ref_bind; });
	// The share of textured draws whose fragment stage did NOT decode ring bytes: rule 2's binds plus
	// the rule-3 draws that were actually served. `src_eligible` above is the admission question's
	// answer, taken before the build could refuse anything -- so this is the honest one.
	const double sampled = (tdraws.mean > 0.0) ? (binds.mean + sserved.mean) * 100.0 / tdraws.mean : 0.0;

	// The donor road: rule 3's build taken off the owning target instead of off the ring. Its served
	// draws are inside src_served above -- the ROAD a draw takes is SOURCE either way -- so these
	// columns are about what did not have to happen for them, not about a different road.
	const auto delig = stat([](const MF& f) { return f.src_donor_eligible; });
	const auto dserved = stat([](const MF& f) { return f.src_donor_served; });
	const auto dbuild = stat([](const MF& f) { return f.src_donor_builds; });
	const auto dhit = stat([](const MF& f) { return f.src_donor_hits; });
	const auto dresc = stat([](const MF& f) { return f.src_donor_rescued; });
	const auto dref = stat([](const MF& f) { return f.src_donor_refused; });
	const auto dbreak = stat([](const MF& f) { return f.src_donor_breaks; });
	const auto ddirect = stat([](const MF& f) { return f.src_donor_direct; });

	const auto pdraws = stat([](const MF& f) { return f.src_pal_draws; });
	const auto pmiss = stat([](const MF& f) { return f.src_pal_missing; });
	const auto ehit = stat([](const MF& f) { return f.src_expand_hits; });
	const auto ebuild = stat([](const MF& f) { return f.src_expand_builds; });
	const auto edefer = stat([](const MF& f) { return f.src_expand_defer; });
	const auto ecap = stat([](const MF& f) { return f.src_expand_cap; });
	const auto efail = stat([](const MF& f) { return f.src_expand_failed; });

	Console.WriteLn("TileGpu source road (rule 3: direct colour and paletted, BUILT and SAMPLED):");
	Console.WriteLn("  textured draws %.2f / %-5u (alpha-byte views %.2f / %u)  rule 2 served %.2f / %-5u  "
					"rule 3 eligible %.2f / %-5u  => %.1f%% of textured draws served",
		tdraws.mean, tdraws.p50, thi.mean, thi.p50, binds.mean, binds.p50, elig.mean, elig.p50, served);
	Console.WriteLn("  eligible by cost: hit %.2f / %-5u  rebuild %.2f / %-5u  first build %.2f / %u",
		shit.mean, shit.p50, sreb.mean, sreb.p50, sfresh.mean, sfresh.p50);
	Console.WriteLn("  refused: unsampleable format %.2f/%u  no materialise arm (16-bit) %.2f/%u  "
					"region wrap %.2f/%u  palette first sight %.2f/%u  pin %.2f/%u  capacity %.2f/%u",
		tunsup.mean, tunsup.p50, rsfmt.mean, rsfmt.p50, rregion.mean, rregion.p50, rpal.mean, rpal.p50, rpin.mean,
		rpin.p50, rcap.mean, rcap.p50);
	Console.WriteLn("  mipmapped draws (eligible, served at level 0 as every road serves them today) %.2f/%u, "
					"mipmapping live on %.2f/%u of them",
		smip.mean, smip.p50, smipl.mean, smipl.p50);
	Console.WriteLn("  distinct source windows %.2f / %-4u  bound (image x sampler) slots %.2f / %-4u "
					"(array holds %u, bind-table refusals %.2f/%u)   extra indirect calls from the split %.2f / %u",
		sdist.mean, sdist.p50, sbind.mean, sbind.p50, kMaxSourceSlots, rbind.mean, rbind.p50, scalls.mean,
		scalls.p50);
	Console.WriteLn("  windows vs the frame before: gen stamp held %.2f / %-4u  moved %.2f / %-4u  (%.1f%% held)",
		skept.mean, skept.p50, smoved.mean, smoved.p50, stamp_total > 0.0 ? skept.mean * 100.0 / stamp_total : 0.0);
	Console.WriteLn("  of the moved: same bytes after all %.2f / %-4u  rebuilt %.2f / %-4u  (of those, %.2f/%u "
					"unprovable: GPU-side truth)  => %.1f%% effectively stable",
		srescued.mean, srescued.p50, sredo.mean, sredo.p50, sopaque.mean, sopaque.p50,
		stamp_total > 0.0 ? (skept.mean + srescued.mean) * 100.0 / stamp_total : 0.0);
	Console.WriteLn("  materialised: builds %.2f / %-4u  cache hits %.2f / %-4u  same-bytes rescues %.2f / %u",
		sbuild.mean, sbuild.p50, schit.mean, schit.p50, scresc.mean, scresc.p50);
	Console.WriteLn("  donor (built off the owning target, no compose): eligible %.2f / %-4u  served %.2f / %-4u  "
					"builds %.2f / %-4u  hits %.2f / %-4u  target-identity rescues %.2f / %-4u  refused %.2f/%u  "
					"pass breaks %.2f/%u   (direct-colour windows a copy leg would serve %.2f/%u)",
		delig.mean, delig.p50, dserved.mean, dserved.p50, dbuild.mean, dbuild.p50, dhit.mean, dhit.p50, dresc.mean,
		dresc.p50, dref.mean, dref.p50, dbreak.mean, dbreak.p50, ddirect.mean, ddirect.p50);
	Console.WriteLn("  paletted second stage: draws %.2f / %-4u  expansion hits %.2f / %-4u  passes %.2f / %-4u  "
					"deferred at build %.2f / %-4u  (palette missing %.2f/%u, capacity %.2f/%u, failed %.2f/%u)",
		pdraws.mean, pdraws.p50, ehit.mean, ehit.p50, ebuild.mean, ebuild.p50, edefer.mean, edefer.p50, pmiss.mean,
		pmiss.p50, ecap.mean, ecap.p50, efail.mean, efail.p50);
	Console.WriteLn("  SERVED by rule 3 (the draw sampled the image) %.2f / %-5u  => %.1f%% of textured draws "
					"sampled an image rather than decoding bytes",
		sserved.mean, sserved.p50, sampled);
	Console.WriteLn("  cache refusals: pinned by a recorded draw %.2f / %-4u  every entry pinned %.2f / %-4u  "
					"build failed %.2f / %u   (cache totals: %llu pin, %llu capacity; palette %llu, expand %llu)",
		scpin.mean, scpin.p50, sccap.mean, sccap.p50, scfail.mean, scfail.p50,
		static_cast<unsigned long long>(m_tex_source.PinRefusals()),
		static_cast<unsigned long long>(m_tex_source.CapacityRefusals()),
		static_cast<unsigned long long>(m_palette_cache.CapacityRefusals()),
		static_cast<unsigned long long>(m_expand_cache.CapacityRefusals()));

	// The CLUT gather. A load whose source words were rendered by a native draw stops being read
	// back at all; its words reach the byte road as a block copy into the frame's palette stream and
	// rule 3 as an N x 1 gather. What this block has to show is (a) that the machinery is invisible
	// where it serves nothing -- no-stall is the whole population on sotc and xenosaga -- and (b)
	// that clut stalls above have collapsed to the refused population and nothing else.
	const auto cl = stat([](const MF& f) { return f.clut_loads; });
	const auto cnos = stat([](const MF& f) { return f.clut_no_stall; });
	const auto cgat = stat([](const MF& f) { return f.clut_gathered; });
	const auto crs = stat([](const MF& f) { return f.clut_ref_shape; });
	const auto cro = stat([](const MF& f) { return f.clut_ref_owner; });
	const auto cmulti = stat([](const MF& f) { return f.clut_ro[kClutRefMulti]; });
	const auto cdead = stat([](const MF& f) { return f.clut_ro[kClutRefDead]; });
	const auto clay = stat([](const MF& f) { return f.clut_ro[kClutRefLayout]; });
	// The layout clause, split three ways: `kind` is a depth owner, `base` a colour owner whose base
	// is not page-aligned, `psm` a page-aligned colour owner in a format the CT32 forms cannot read
	// -- the 16-bit population, and the only one of the three a wider gather could ever take.
	const auto clk = stat([](const MF& f) { return f.clut_rol[kGSTileCt32ClauseKind]; });
	const auto clb = stat([](const MF& f) { return f.clut_rol[kGSTileCt32ClauseBase]; });
	const auto clp = stat([](const MF& f) { return f.clut_rol[kGSTileCt32ClausePsm]; });
	// The psm bucket by (entry count, CBP & 3), which is the shape a 16-bit-owner gather would have to
	// copy: 256 entries at a 4-aligned CBP are one square, 256 elsewhere are four scattered blocks,
	// and 16 are two texel rows wherever they sit.
	const auto p16 = [&](u32 e256, u32 al) { return stat([e256, al](const MF& f) { return f.clut_rol16[e256][al]; }); };
	const auto pn = [&](u32 c) { return stat([c](const MF& f) { return f.clut_rol16_next[c]; }); };
	// The multi/partial bucket's three causes, and what the same loads would meet if the owner
	// question were asked in BLOCKS. `bnext[kClutRefNone]` is the population a block-granular gather
	// serves; it is a subset of the block-subset cause by construction.
	const auto mcause = [&](GSVramModel::SoleOwnerRefusal c) {
		const u32 i = static_cast<u32>(c);
		return stat([i](const MF& f) { return f.clut_multi_cause[i]; });
	};
	const auto bnext = [&](u32 c) { return stat([c](const MF& f) { return f.clut_multi_next[c]; }); };
	const auto mct16 = stat([](const MF& f) { return f.clut_multi_ct16; });
	const auto lnext = [&](u32 c) { return stat([c](const MF& f) { return f.clut_loader_next[c]; }); };
	const auto lct16 = stat([](const MF& f) { return f.clut_loader_ct16; });
	const auto lshape = stat([](const MF& f) { return f.clut_loader_shape; });
	const auto bcause = [&](GSVramModel::SoleOwnerRefusal c) {
		const u32 i = static_cast<u32>(c);
		return stat([i](const MF& f) { return f.clut_block_cause[i]; });
	};
	const auto cres = stat([](const MF& f) { return f.clut_ro[kClutRefResidency]; });
	const auto ctex = stat([](const MF& f) { return f.clut_ro[kClutRefTexels]; });
	const auto cmix = stat([](const MF& f) { return f.clut_ro[kClutRefMixedOwner]; });
	const auto cd = stat([](const MF& f) { return f.clut_draws; });
	const auto cd3 = stat([](const MF& f) { return f.clut_draws_r3; });
	const auto cdb = stat([](const MF& f) { return f.clut_draws_byte; });
	const auto cds = stat([](const MF& f) { return f.clut_draws_sync; });
	const auto cr3r = stat([](const MF& f) { return f.clut_r3_refused; });
	const auto c16g = stat([](const MF& f) { return f.clut16_gathered; });
	const auto c16r3 = stat([](const MF& f) { return f.clut16_r3_refused; });
	const auto c16b = stat([](const MF& f) { return f.clut16_arm_breaks; });
	const auto csw = stat([](const MF& f) { return f.clut_stream_words; });
	const auto ccp = stat([](const MF& f) { return f.clut_copies; });
	const auto ccr = stat([](const MF& f) { return f.clut_copy_regions; });
	const auto cpm = stat([](const MF& f) { return f.clut_page_merges; });
	const auto cgp = stat([](const MF& f) { return f.clut_gathers; });
	const auto cbrk = stat([](const MF& f) { return f.clut_breaks; });
	const auto csy = stat([](const MF& f) { return f.clut_syncs; });
	const auto clost = stat([](const MF& f) { return f.clut_lost; });
	const auto cpr = stat([](const MF& f) { return f.clut_pruned; });
	if (cl.mean > 0.0)
	{
		Console.WriteLn("TileGpu CLUT gather (a palette loaded off a render target):");
		Console.WriteLn("  loads %.2f / %-5u  no-stall %.2f / %-5u  gathered %.2f / %-5u  refused: shape %.2f/%u  "
						"owner %.2f/%u",
			cl.mean, cl.p50, cnos.mean, cnos.p50, cgat.mean, cgat.p50, crs.mean, crs.p50, cro.mean, cro.p50);
		// TileGpuClut16Gather's own population, and the two things it can cost. A frame with no
		// 16-bit palette owner prints zeros on all four, which is every corpus dump but Spider-Man 3
		// and legosw -- and the arm breaks staying at zero is the lever's standing claim, since a
		// pass split is the price of keeping two entry orders out of one fragment program.
		Console.WriteLn("  16-bit owners: gathered %.2f / %-5u  rule-3 refused %.2f / %-4u  arm breaks %.2f / %-4u  "
						"stream words %.2f / %u",
			c16g.mean, c16g.p50, c16r3.mean, c16r3.p50, c16b.mean, c16b.p50, csw.mean, csw.p50);
		Console.WriteLn("  owner refusals: multi/partial %.2f  dead %.2f  layout %.2f (kind %.2f, base %.2f, "
						"psm %.2f)  residency %.2f  texels %.2f  mixed-owner %.2f",
			cmulti.mean, cdead.mean, clay.mean, clk.mean, clb.mean, clp.mean, cres.mean, ctex.mean, cmix.mean);
		if (cmulti.mean > 0.0)
		{
			// The one bucket that carries three populations, split. Only the block-subset cause is a
			// granularity question: the other two need a stitch across GPU and CPU bytes, or a
			// palette record naming two owners, and GSVramModel refuses the first on purpose.
			Console.WriteLn("  multi/partial by cause: block subset %.2f  no owner %.2f  two owners %.2f",
				mcause(GSVramModel::SoleOwnerRefusal::kBlockSubset).mean,
				mcause(GSVramModel::SoleOwnerRefusal::kNoOwner).mean,
				mcause(GSVramModel::SoleOwnerRefusal::kTwoOwners).mean);
			// ...and the same loads asked in blocks. "served" is what TileGpuClutBlockGather takes;
			// "own blocks split" is a load whose OWN blocks are not one owner's GPU truth either, so
			// the narrower question does not help it.
			Console.WriteLn("  ...asked in blocks: served %.2f (of which a 16-bit owner %.2f)  own blocks split "
							"%.2f  dead %.2f  layout %.2f  residency %.2f  texels %.2f  mixed-owner %.2f",
				bnext(kClutRefNone).mean, mct16.mean, bnext(kClutRefMulti).mean, bnext(kClutRefDead).mean,
				bnext(kClutRefLayout).mean, bnext(kClutRefResidency).mean, bnext(kClutRefTexels).mean,
				bnext(kClutRefMixedOwner).mean);
			// ...and the block question's own refusals split the same three ways. kBlockSubset here is
			// the load whose OWN blocks are partly the CPU's, which is the one shape no narrowing of
			// the unit reaches -- so it is the number that says whether the block serve is worth it.
			Console.WriteLn("  ...and those splits by cause: own blocks not all GPU %.2f  no owner %.2f  "
							"two owners %.2f",
				bcause(GSVramModel::SoleOwnerRefusal::kBlockSubset).mean,
				bcause(GSVramModel::SoleOwnerRefusal::kNoOwner).mean,
				bcause(GSVramModel::SoleOwnerRefusal::kTwoOwners).mean);
			// ...and the same question over the blocks the LOADER READS. The gap between this line
			// and the one two above is GSState's second invalidated block on a four-bit index, which
			// the loader never touches.
			Console.WriteLn("  ...asked over the READ blocks: served %.2f (of which a 16-bit owner %.2f)  own "
							"blocks split %.2f  dead %.2f  layout %.2f  residency %.2f  texels %.2f  mixed-owner "
							"%.2f  (shape the gather never serves %.2f)",
				lnext(kClutRefNone).mean, lct16.mean, lnext(kClutRefMulti).mean, lnext(kClutRefDead).mean,
				lnext(kClutRefLayout).mean, lnext(kClutRefResidency).mean, lnext(kClutRefTexels).mean,
				lnext(kClutRefMixedOwner).mean, lshape.mean);
		}
		if (clp.mean > 0.0)
		{
			// The 16-bit owners, split the two ways that decide whether a gather off one is worth
			// building: what it would have to copy, and what it would meet after the layout clause.
			Console.WriteLn("  psm clause by (entries, cbp&3): 16 @0 %.2f  @1 %.2f  @2 %.2f  @3 %.2f   "
							"256 @0 %.2f  @1 %.2f  @2 %.2f  @3 %.2f",
				p16(0, 0).mean, p16(0, 1).mean, p16(0, 2).mean, p16(0, 3).mean, p16(1, 0).mean, p16(1, 1).mean,
				p16(1, 2).mean, p16(1, 3).mean);
			// Zero by construction in every counter above, because the layout clause fires before all
			// three: this is the only place they are actually asked of the 16-bit population.
			Console.WriteLn("  psm clause, would refuse next: none (served) %.2f  residency %.2f  texels %.2f  "
							"mixed-owner %.2f",
				pn(kClutRefNone).mean, pn(kClutRefResidency).mean, pn(kClutRefTexels).mean,
				pn(kClutRefMixedOwner).mean);
		}
		Console.WriteLn("  gathered draws %.2f / %-5u  (rule 3 %.2f / %-4u, byte road %.2f / %-5u, rule-3 refused "
						"%.2f/%u)   mirror could not serve %.2f / %u draws",
			cd.mean, cd.p50, cd3.mean, cd3.p50, cdb.mean, cdb.p50, cr3r.mean, cr3r.p50, cds.mean, cds.p50);
		// The copy's REGIONS beside its ops, because a region is the unit the device charges: per-region
		// copy cost on Adreno 650 is flat at ~2.8 us whatever the region's size, so the merge levers move
		// this number and leave the op count almost where it was.
		Console.WriteLn("  block copies %.2f / %-5u  copy regions %.2f / %-5u  (page bands %.2f / %-4u)  "
						"gather passes %.2f / %-4u  pass breaks %.2f / %-4u  cpu syncs %.2f / %-4u  "
						"lost records %.2f / %-4u  pruned %.2f / %u",
			ccp.mean, ccp.p50, ccr.mean, ccr.p50, cpm.mean, cpm.p50, cgp.mean, cgp.p50, cbrk.mean, cbrk.p50, csy.mean,
			csy.p50, clost.mean, clost.p50, cpr.mean, cpr.p50);
	}

	// The CPU palette road's re-work (TileGpuCensusPalette). Its own block rather than a line inside
	// the gather report above, because that block is gated on the frame having had a gather LOAD and
	// this population is the draws no gather served -- the two are near-complements, and a title with
	// no gather at all is exactly the one whose CPU road is worth counting.
	const auto pcd = stat([](const MF& f) { return f.clut_cpu_draws; });
	const auto pcw = stat([](const MF& f) { return f.clut_cpu_words; });
	const auto pch = stat([](const MF& f) { return f.clut_cpu_readgen_held; });
	const auto pcwh = stat([](const MF& f) { return f.clut_cpu_words_held; });
	const auto pcdi = stat([](const MF& f) { return f.clut_cpu_distinct; });
	if (pcd.mean > 0.0)
	{
		// held% is what a read-generation memo would remove outright: the words were byte-identical
		// to the previous append and the content id was the previous id. distinct is the separate,
		// larger question a full stream dedup would reach -- an upper bound on the same road, since
		// two draws can share a palette without being adjacent.
		Console.WriteLn("TileGpu CPU palette road (draws the device could not serve, so the CLUT is expanded, "
						"copied into the plan's stream and hashed):");
		Console.WriteLn("  draws %.2f / %-5u  words %.2f / %-6u  read-gen held %.2f / %-5u (%.1f%% of draws, "
						"%.1f%% of words)  distinct ids %.2f / %u",
			pcd.mean, pcd.p50, pcw.mean, pcw.p50, pch.mean, pch.p50,
			pcd.mean > 0.0 ? 100.0 * pch.mean / pcd.mean : 0.0,
			pcw.mean > 0.0 ? 100.0 * pcwh.mean / pcw.mean : 0.0, pcdi.mean, pcdi.p50);
	}

	// The palette cycle. Printed whenever ANY draw reached the first conjunct's first half, on both
	// arms of the lever -- the funnel is what says which conjunct refused a population, and a run
	// with the lever off has to print the same columns or the two arms cannot be compared.
	const auto pc1p = stat([](const MF& f) { return f.pcyc_pal; });
	const auto pc1 = stat([](const MF& f) { return f.pcyc_loop; });
	const auto pc2 = stat([](const MF& f) { return f.pcyc_shape; });
	const auto pcff = stat([](const MF& f) { return f.pcyc_fail_fmt; });
	const auto pcfo = stat([](const MF& f) { return f.pcyc_fail_prim; });
	const auto pcr = stat([](const MF& f) { return f.pcyc_runs; });
	const auto pcra = stat([](const MF& f) { return f.pcyc_runs_alpha; });
	const auto pcb = stat([](const MF& f) { return f.pcyc_building; });
	const auto pce = stat([](const MF& f) { return f.pcyc_elided; });
	const auto pcs = stat([](const MF& f) { return f.pcyc_subs; });
	const auto pccp = stat([](const MF& f) { return f.pcyc_elided_pages; });
	const auto pcsp = stat([](const MF& f) { return f.pcyc_sub_pages; });
	const auto pcx = stat([](const MF& f) { return f.pcyc_refused; });
	if (pc1p.mean > 0.0)
	{
		Console.WriteLn("TileGpu palette cycle (channel shuffle), %s:",
			m_palette_cycle_hle ? "ARMED" :
								  (GSConfig.TileGpuPaletteCycleHle ? "detector only (GameDB gate not matched)" :
																	 "detector only (TileGpuPaletteCycleHle off)"));
		Console.WriteLn("  device palette %.2f / %-5u  ...closing the loop %.2f / %-5u   shape %.2f / %-5u  "
						"refused: format %.2f / %-5u  primclass %.2f / %u",
			pc1p.mean, pc1p.p50, pc1.mean, pc1.p50, pc2.mean, pc2.p50, pcff.mean, pcff.p50, pcfo.mean, pcfo.p50);
		Console.WriteLn("  runs %.2f / %-4u  (alpha-destination %.2f / %-4u)  below threshold, drawn %.2f / %-4u  "
						"elided %.2f / %-5u  pages they would have written %.2f / %-5u",
			pcr.mean, pcr.p50, pcra.mean, pcra.p50, pcb.mean, pcb.p50, pce.mean, pce.p50, pccp.mean, pccp.p50);
		Console.WriteLn("  substitutes %.2f / %-4u  their pages %.2f / %-5u  runs with no substitute %.2f / %u",
			pcs.mean, pcs.p50, pcsp.mean, pcsp.p50, pcx.mean, pcx.p50);
	}
}


// -- the memory model's per-draw road ---------------------------------------------------------

GSTileSurfaceId GSRendererTileGpu::EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect,
	const GSPageBitmap& pages, GSVector2i* out_offset, bool may_offset)
{
	if (out_offset)
		*out_offset = GSVector2i(0, 0);
	if (!PoolSupports(layout))
		return kGSTileNoSurface;

	GSTileSurfaceId id = m_vram_model.FindExact(layout);
	const bool first_sight = (id == kGSTileNoSurface);
	// Where this view belongs. Asked before the create, because a fold IS the create not happening.
	// Off the lever the answer is counted and thrown away, and every road below is the one it was.
	const GSTileContainedView placed = PlaceContainedView(layout, rect, first_sight, may_offset);
	const bool folded = m_contain_surfaces && placed.id != kGSTileNoSurface;
	if (folded)
	{
		// The view has no surface of its own and never will: it renders into the container, at a
		// rectangle offset the caller puts its geometry at. Everything below is the growth road the
		// container's own view takes -- the container is the surface, and it has to hold this
		// view's pages, which sit further down its pixel space than they would down the view's.
		id = placed.id;
		if (out_offset)
			*out_offset = GSVector2i(placed.x_off, placed.y_off);
	}
	else if (first_sight)
	{
		const int height = GSTileTargetPool::HeightForPages(layout, pages);
		const u32 handle = m_target_pool.Allocate(layout, height);
		if (handle == 0)
			return kGSTileNoSurface;
		id = m_vram_model.Create(layout, rect, handle);
		m_vram_model.GrowResidency(id, pages);
		Texels(id) = {}; // a fresh texture holds nothing; see the member's note
		// A fresh allocation holds whatever the pool left there, and this id may be a slot some
		// earlier surface used: a donor source cached under the old surface's version must not be
		// served for the new one, and neither must a palette gathered off it.
		BumpSurfaceVersion(id);
		NoteClutSurfaceReplaced(id);
		NoteTextureGeometry(id, height);
		NoteSurfaceRecycled(id);
		// Off the lever a view the placement folded still takes a surface of its own, and the
		// placement has already put it in a container -- so only a view it left unplaced is one.
		if (placed.id == kGSTileNoSurface)
			NoteOwnContainer(id, layout, rect);
		return id;
	}
	else if (placed.id == kGSTileNoSurface)
	{
		NoteOwnContainer(id, layout, rect);
	}

	m_vram_model.GrowResidency(id, pages);
	// The height is asked of the surface the pages land IN, which for a folded view is the
	// container: the same guest pages sit at a lower page row of the view's own pixel space than
	// they do of the container's, and it is the container's texture that has to reach them.
	const int height = GSTileTargetPool::HeightForPages(m_vram_model.Get(id).layout, pages);
	// Growth copies the texture's content into a taller one on the device now -- before any of
	// this frame's passes are recorded (the plan is built at VSync), so it moves last frame's
	// pixels and every draw of this frame lands in the grown texture.
	const int old_height = Texels(id).height;
	if (!m_target_pool.EnsureHeight(m_vram_model.Get(id).pool_handle, height))
		return kGSTileNoSurface;
	if (old_height != height)
		BumpSurfaceVersion(id); // a re-allocated texture is a different image, whatever it copied
	NoteTextureGeometry(id, height);
	return id;
}

bool GSRendererTileGpu::IsDisplayBase(u32 bp) const
{
	std::array<u32, 2> bases{kGSTileNoDisplayBase, kGSTileNoDisplayBase};
	for (int i = 0; i < 2; i++)
	{
		const auto& fb = PCRTCDisplays.PCRTCDisplays[i];
		if (fb.enabled && fb.FBW != 0)
			bases[i] = static_cast<u32>(fb.Block());
	}
	return gsTileContainIsDisplayBase(bp, bases);
}

void GSRendererTileGpu::NoteContainerRows(GSTileSurfaceId id, u32 end_row)
{
	if (m_contain_rows.size() <= id)
		m_contain_rows.resize(id + 1, 0);
	if (end_row > m_contain_rows[id])
		m_contain_rows[id] = end_row;
	const u32 pages = m_contain_rows[id] * m_vram_model.Get(id).layout.bw;
	if (pages > m_frame.contain_max_pages)
		m_frame.contain_max_pages = pages;
}

// See the declaration.
//
// The container-choice policy is the composite one, and it is not a detail: a view that is legal
// inside more than one live container has to pick the one whose alternation it removes rather than
// one that merely relocates it. Measured over the corpus draw streams, "nearest containing base"
// leaves Gran Turismo 4 at 107.75 colour-key breaks a frame where preferring the container the
// PREVIOUS draw rendered into gets it to 20.38 -- five-sixths of the win is in the choice. So:
// a container that already covers the view first (no growth, nearest base among those), else the
// container the previous draw took if it is legal and within the budget, else a new surface.
//
// ⚠️ A no-growth-only rule buys nothing for the DISPLACED road: every displaced fold on this corpus
// needs the container to grow, so growth is that feature and not an optimisation of it. The
// zero-offset folds this actually takes are the other way round -- a view that IS its container's
// pages needs no growth at all, and the corpus grows a container by zero pages as it ships.
GSTileContainedView GSRendererTileGpu::PlaceContainedView(const GSTileSurfaceLayout& layout,
	const GSVector4i& rect, bool first_sight, bool may_offset)
{
	if (layout.kind != GSTileSurfaceKind::Color)
		return GSTileContainedView{};

	// The view's footprint is the extent its draws reach from the surface's own origin -- what the
	// container must hold, not what this draw covers.
	const u32 cols = gsTileContainPageCols(static_cast<u32>(std::max(rect.z, 0)), layout.bw);
	const u32 rows = gsTileContainPageRows(static_cast<u32>(std::max(rect.w, 0)), layout.psm);
	const u32 budget = gsTileGpuContainPageBudget(GSConfig.TileGpuContainPageBudget);
	const u32 key = layout.pack();

	// The caller's veto, and design rule 9's. Both hold the view to a ZERO offset rather than out of
	// containment altogether -- a fold that displaces nothing is invisible to everything outside
	// this function, and the cross-PSM same-base merge it keeps is most of gt4opb's win. Both are
	// permanent for the layout, because both are reasons that can arrive AFTER the fold decision --
	// a view first drawn without depth that later carries one, a base the PCRTC starts scanning out
	// -- and a fold decided at first sight has no other way to be taken back.
	//
	// ⚠️ The STANDING rule (kGSTileContainDisplacedViews) holds every other view to the same thing,
	// so what ships is the merge and nothing else: the two views of one base stop stealing each
	// other's pages, and nothing moves in anyone's pixel space. Its two reasons are written out at
	// the constant -- the container's writeback byte mask, and a displaced view's reads no longer
	// matching the container -- and both are measured, not feared. This set and the unfold below
	// are the per-view half of the same refusal, kept live for the rung that turns displacement
	// back on; while it is off they decide nothing this function would not decide anyway.
	if (!may_offset || IsDisplayBase(layout.bp))
		m_contain_no_offset.insert(key);
	const bool no_offset = m_contain_no_offset.count(key) != 0;
	// A view already displaced when the refusal first arrives is unfolded, which costs it a surface
	// create and a re-seed of its pages out of the container's writeback. That is the ordinary road
	// for a page changing owner, so it is correct; it is counted because a count that does not
	// settle to zero is a policy oscillating. Unreachable while displacement is off -- no fold is
	// ever taken at a nonzero offset for this to undo.
	const auto stale = m_contain_alias.find(key);
	if (stale != m_contain_alias.end() &&
		!gsTileContainMayPlaceAt(stale->second.x_off, stale->second.y_off, no_offset))
	{
		m_contain_alias.erase(stale);
		m_frame.contain_unfolded++;
	}

	// Already folded, and the fold is sticky. It has to be: under containment no surface was ever
	// created for this view, so the exact-match road must not take it back. It is also what keeps a
	// view from drifting between containers as the draw order shifts, which would cost a surface
	// create and a re-seed every frame.
	const auto hit = m_contain_alias.find(key);
	if (hit != m_contain_alias.end())
	{
		const GSTileSurfaceId cid = hit->second.id;
		if (m_vram_model.Get(cid).alive)
		{
			GSTileContainOffset off;
			// Re-asked for THIS extent: the view may have grown since the draw that admitted it, and
			// a growth the predicate would refuse is a fact about the admission, not about the view.
			if (gsTileContainView(m_vram_model.Get(cid).layout, layout, cols, rows, budget, off) ==
				GSTileContainRefusal::Admitted)
				NoteContainerRows(cid, off.end_row);
			else
				m_frame.contain_regrow_refused++;
			m_contain_view = cid;
			m_frame.contain_folded_draws++;
			return hit->second;
		}
		m_contain_alias.erase(hit);
	}

	// A surface that already exists is its own container, and the caller notes its rows once it has
	// the id. There is no retro-folding: a view that was not folded when it was created is not
	// folded later, because moving it would mean moving page ownership and bumping two surface
	// versions mid-frame.
	if (!first_sight)
		return GSTileContainedView{};

	m_frame.contain_first_sight++;
	const GSTileSurfaceId open = m_contain_prev_valid ? m_contain_prev_folded : kGSTileNoSurface;
	GSTileSurfaceId fit_id = kGSTileNoSurface, open_id = kGSTileNoSurface;
	GSTileContainOffset fit_off, open_off;
	u32 fit_base = 0;
	u32 candidates = 0;
	bool displaced_any = false;
	GSTileContainRefusal furthest = GSTileContainRefusal::Admitted;
	for (u32 slot = 0; slot < m_vram_model.SurfaceSlots(); slot++)
	{
		const GSTileSurfaceId cid = static_cast<GSTileSurfaceId>(slot);
		const GSVramModel::Surface& surf = m_vram_model.Get(cid);
		if (!surf.alive || surf.layout.kind != GSTileSurfaceKind::Color)
			continue;
		// A view that is itself folded is not a container. Under containment it never had a surface
		// at all, so admitting it here would offer a candidate that does not exist.
		if (m_contain_alias.count(surf.layout.pack()) != 0)
			continue;
		candidates++;

		GSTileContainOffset off;
		const GSTileContainRefusal why = gsTileContainView(surf.layout, layout, cols, rows, budget, off);
		if (why != GSTileContainRefusal::Admitted)
		{
			if (static_cast<u8>(why) > static_cast<u8>(furthest))
				furthest = why;
			continue;
		}
		// Legal, and this container would displace the view -- which is refused, by the standing rule
		// and by the caller's own veto alike (see above). Counted apart from the predicate's clauses:
		// nothing about the geometry is wrong, and this is the census the displaced rung sizes its
		// work from.
		if (!gsTileContainMayPlaceAt(off.x, off.y, no_offset))
		{
			displaced_any = true;
			continue;
		}
		const u32 held = (m_contain_rows.size() > cid) ? m_contain_rows[cid] : 0;
		if (off.end_row <= held)
		{
			// Covers the view already. Nearest base among these, which is the tightest fit.
			if (fit_id == kGSTileNoSurface || surf.layout.bp > fit_base)
			{
				fit_id = cid;
				fit_off = off;
				fit_base = surf.layout.bp;
			}
		}
		else if (cid == open)
		{
			open_id = cid;
			open_off = off;
		}
	}

	const GSTileSurfaceId picked = (fit_id != kGSTileNoSurface) ? fit_id : open_id;
	if (picked != kGSTileNoSurface)
	{
		const GSTileContainOffset& off = (fit_id != kGSTileNoSurface) ? fit_off : open_off;
		const GSTileContainedView view{picked, off.x, off.y};
		m_contain_alias[key] = view;
		m_frame.contain_folds++;
		m_frame.contain_folded_draws++;
		const u32 held = (m_contain_rows.size() > picked) ? m_contain_rows[picked] : 0;
		if (off.end_row > held)
			m_frame.contain_growth_pages += (off.end_row - held) * m_vram_model.Get(picked).layout.bw;
		NoteContainerRows(picked, off.end_row);
		m_contain_view = picked;
		return view;
	}

	if (candidates == 0)
		m_frame.contain_ref_none++;
	else if (displaced_any)
		m_frame.contain_ref_displaced++;
	else
	{
		switch (furthest)
		{
			case GSTileContainRefusal::Family:
				m_frame.contain_ref_family++;
				break;
			case GSTileContainRefusal::Stride:
				m_frame.contain_ref_stride++;
				break;
			case GSTileContainRefusal::Alignment:
				m_frame.contain_ref_align++;
				break;
			case GSTileContainRefusal::Delta:
				m_frame.contain_ref_delta++;
				break;
			case GSTileContainRefusal::Column:
				m_frame.contain_ref_column++;
				break;
			case GSTileContainRefusal::Extent:
				m_frame.contain_ref_extent++;
				break;
			case GSTileContainRefusal::Wrap:
				m_frame.contain_ref_wrap++;
				break;
			default:
				break;
		}
	}
	return GSTileContainedView{};
}

// See the declaration. The rows a surface holds in its own right, noted once EnsureSurface knows
// which id the view took.
void GSRendererTileGpu::NoteOwnContainer(GSTileSurfaceId id, const GSTileSurfaceLayout& layout,
	const GSVector4i& rect)
{
	if (layout.kind != GSTileSurfaceKind::Color)
		return;
	NoteContainerRows(id, gsTileContainPageRows(static_cast<u32>(std::max(rect.w, 0)), layout.psm));
	m_contain_view = id;
}

// A freshly created surface may be sitting in a slot an earlier one used, and that earlier surface
// may have been a container: its rows go, and so does every view folded into it. Ids come back off
// the model's free list, so this is not hypothetical -- though nothing in this renderer destroys a
// surface today, and the free list only ever fills at a Reset that clears both tables anyway.
void GSRendererTileGpu::NoteSurfaceRecycled(GSTileSurfaceId id)
{
	if (m_contain_rows.size() <= id)
		m_contain_rows.resize(id + 1, 0);
	m_contain_rows[id] = 0;
	for (auto it = m_contain_alias.begin(); it != m_contain_alias.end();)
	{
		if (it->second.id == id)
			it = m_contain_alias.erase(it);
		else
			++it;
	}
}

void GSRendererTileGpu::CountContainmentBreaks(GSTileSurfaceId fb_id, GSTileSurfaceId z_id, bool z_used)
{
	// A colour surface the census declined to answer for (a display materialise ran last, or the
	// probe never saw this layout) falls back to the surface the draw actually takes, which counts
	// as no fold rather than as a wrong one.
	const GSTileSurfaceId folded = (m_contain_view != kGSTileNoSurface) ? m_contain_view : fb_id;
	m_frame.contain_draws++;
	if (m_contain_prev_valid)
	{
		// The depth half of the key is unchanged by containment -- depth is deferred -- so it is
		// the same term on both sides and only decides whether a colour-identical pair still breaks.
		const bool depth_same =
			z_used == m_contain_prev_depth_used && (!z_used || z_id == m_contain_prev_depth);
		const bool color_moved = fb_id != m_contain_prev_color;
		const bool folded_moved = folded != m_contain_prev_folded;
		m_frame.contain_breaks += color_moved ? 1 : 0;
		m_frame.contain_breaks_folded += folded_moved ? 1 : 0;
		m_frame.contain_full_breaks += (color_moved || !depth_same) ? 1 : 0;
		m_frame.contain_full_breaks_folded += (folded_moved || !depth_same) ? 1 : 0;
	}
	m_contain_prev_color = fb_id;
	m_contain_prev_folded = folded;
	m_contain_prev_depth = z_id;
	m_contain_prev_depth_used = z_used;
	m_contain_prev_valid = true;
}

// "This surface's texels changed." The donor road's whole content identity rests on this counter,
// so what bumps it is the specification: every write of a pool texture and nothing else. Those are
// a draw rendering into the surface, a seed filling it out of the ring, and the pool allocating or
// growing it -- three call sites, all in this file. Globally monotonic rather than per-surface, so
// a surface id coming back out of the model's free list cannot reproduce a version its predecessor
// already filed under the same window key.
void GSRendererTileGpu::BumpSurfaceVersion(GSTileSurfaceId id)
{
	if (id == kGSTileNoSurface)
		return;
	if (m_surface_version.size() <= id)
		m_surface_version.resize(id + 1, 0);
	m_surface_version[id] = ++m_surface_version_counter;
}

u64 GSRendererTileGpu::SurfaceVersion(GSTileSurfaceId id) const
{
	return (id != kGSTileNoSurface && m_surface_version.size() > id) ? m_surface_version[id] : 0;
}

// The page set the surface's texture rectangle spans, refreshed when the pool grows it. Growth
// keeps the pixels it had, so `filled` survives; the new rows are simply not in it yet.
void GSRendererTileGpu::NoteTextureGeometry(GSTileSurfaceId id, int height)
{
	SurfaceTexels& t = Texels(id);
	if (t.height == height)
		return;
	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	const GSVector2i dim = m_target_pool.GetTexture(surf.pool_handle)->GetSize();
	t.pages = GSVramModel::PagesForRect(surf.layout, GSVector4i(0, 0, dim.x, dim.y));
	t.height = height;
}

void GSTileBlockFill::Mark(u32 page, u32 mask)
{
	if (mask == 0 || m_whole.test(page))
		return;
	if (mask == GSVramModel::kFullBlockMask)
	{
		m_whole.set(page);
		if (m_partial.test(page))
		{
			m_partial.unset(page);
			m_masks.erase(static_cast<u16>(page));
		}
		return;
	}
	u32& have = m_masks[static_cast<u16>(page)];
	m_partial.set(page);
	have |= mask;
	if (have == GSVramModel::kFullBlockMask)
	{
		// The blocks add up: promote, so the common query (HoldsWhole) stays one bitmap test.
		m_whole.set(page);
		m_partial.unset(page);
		m_masks.erase(static_cast<u16>(page));
	}
}

void GSTileBlockFill::MarkWhole(const GSPageBitmap& pages)
{
	// The partial side is small (only pages some draw covered in part and no seed has filled),
	// so clean it out of the map rather than leaving dead entries behind a `whole` bit.
	(pages & m_partial).forEachSetPage([this](u32 page) { m_masks.erase(static_cast<u16>(page)); });
	m_partial = m_partial.andnot(pages);
	m_whole |= pages;
}

u32 GSTileBlockFill::BlocksOf(u32 page) const
{
	if (m_whole.test(page))
		return GSVramModel::kFullBlockMask;
	const auto it = m_masks.find(static_cast<u16>(page));
	return it == m_masks.end() ? 0u : it->second;
}

bool GSTileBlockFill::HoldsBlocks(const GSVramModel::RectFootprint& fp) const
{
	if (fp.overflowed)
		return HoldsWhole(fp.pages);
	bool holds = true;
	fp.pages.forEachSetPage([&](u32 page) {
		if (!holds || m_whole.test(page))
			return;
		const GSVramModel::RectFootprint::Edge* e = fp.FindEdge(page);
		const u32 required = e ? e->blocks : GSVramModel::kFullBlockMask;
		if ((BlocksOf(page) & required) != required)
			holds = false;
	});
	return holds;
}

GSRendererTileGpu::SurfaceTexels& GSRendererTileGpu::Texels(GSTileSurfaceId id)
{
	if (m_surface_texels.size() <= id)
		m_surface_texels.resize(id + 1);
	return m_surface_texels[id];
}

// Tile's PagesNeedingUpload: pages whose texture bytes are not already current for every
// relevant plane -- another owner, no owner (CPU-newest), or a shrunk block mask.
GSPageBitmap GSRendererTileGpu::PagesNeedingSeed(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const
{
	GSPageBitmap need;
	pages.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(planes & (1u << pi)))
				continue;
			if (m_vram_model.OwnerOf(page, pi) != id || m_vram_model.TruthMask(page, pi) != GSVramModel::kFullBlockMask)
			{
				need.set(page);
				return;
			}
		}
	});
	return need;
}

// Rule 2 of the design record's texel road: "sole owner is a live surface, same format, 1:1 -> bind
// the surface". Every clause below is a proof the two roads would read the same texel, so that the
// only difference the bind makes is FRESHNESS -- the target's live pixels instead of bytes composed
// from it earlier. Anything that cannot be proved takes the byte road, which is slower and never
// wrong; that is the whole reason the image road can be built one case at a time.
GSTileSurfaceId GSRendererTileGpu::TargetForTextureRead(const GSTileSurfaceLayout& tex_l, u32 tw, u32 th,
	const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id, GSTileSurfaceId z_id)
{
	if (!m_bindless_targets || tex_pages.empty())
		return kGSTileNoSurface;

	// All-or-nothing by construction: one live surface holds whole-page truth of every colour
	// plane on every page of the window. A window split across owners, or part CPU-newest, or with
	// a block mask shrunk by an upload, has no single authoritative texture and is refused here.
	const GSTileSurfaceId src = m_vram_model.SoleGpuOwner(tex_pages, kGSTilePlanesColor);
	if (src == kGSTileNoSurface || src == fb_id || src == z_id)
		return kGSTileNoSurface;

	const GSVramModel::Surface& surf = m_vram_model.Get(src);
	if (!surf.alive || !surf.pool_handle)
		return kGSTileNoSurface;

	// The pool's pixel space is the guest layout only for a page-aligned CT32/CT24 colour surface,
	// and it is the READ's pixel space only when base and stride agree exactly. Then target pixel
	// (u, v) is guest texel (u, v) -- no offset, no restride, no region arithmetic.
	//
	// The narrow question, not HasByteRoad's: a 16-bit target read as 16-bit would be just as
	// bindable, but nothing can ask for one -- the texture block above admits only direct-32 and
	// index formats -- so widening this would be an untested road nothing takes. It goes with the
	// 16-bit texture READ arm, which is where a caller for it first exists.
	if (!HasCt32PixelSpace(surf.layout) || surf.layout.bp != tex_l.bp || surf.layout.bw != tex_l.bw)
	{
		// Q5 of the design's open questions, and the number that decides whether rule 2's per-slot
		// offset bind gets built at all. Surface-identity containment can break exactly one clause
		// of this rule while every other one holds: the window's pages are owned by the container
		// the read's own view is folded into, so the BASE differs and nothing else does. The bind
		// would be legal at the fold's own offset; there is no offset bind, so the draw takes the
		// byte road -- correct, three to four times the fragment instructions, and invisible
		// without this. The PSM clause below is asked here too, so a window this rule would have
		// refused anyway is not counted against the fold.
		//
		// Zero while displacement is off, and that is the point rather than an omission: a
		// same-base merge leaves the container's base equal to the read's, so the clause this
		// counts never fires and rule 2 serves the folded view's reads exactly as it did before
		// the fold. This is what the displaced road costs, waiting for the road to come back.
		if (m_contain_surfaces && HasCt32PixelSpace(surf.layout) && surf.layout.bw == tex_l.bw &&
			(surf.layout.psm == tex_l.psm || (surf.layout.psm == PSMCT32 && tex_l.psm == PSMCT24)))
		{
			const auto it = m_contain_alias.find(tex_l.pack());
			if (it != m_contain_alias.end() && it->second.id == src)
				m_frame.contain_bind_refused++;
		}
		return kGSTileNoSurface;
	}
	// Byte-compatible formats. Equal PSM is the whole story except for a CT32 target read as CT24:
	// same bytes, and the draw's TEXA already supplies the alpha the read must not take from the
	// target's own alpha channel. A CT24 target read as CT32 is refused -- its alpha bytes belong
	// to whoever else writes that page, and the target's texture does not hold them.
	if (!(surf.layout.psm == tex_l.psm || (surf.layout.psm == PSMCT32 && tex_l.psm == PSMCT24)))
		return kGSTileNoSurface;

	// The model tracks BYTES; the sampler reads TEXELS. A page whose bytes the surface owns but
	// whose texture rows nothing has written yet would sample allocator leftovers, which is exactly
	// what SurfaceTexels exists to know about.
	if (m_surface_texels.size() <= src || !m_surface_texels[src].filled.HoldsWhole(tex_pages))
		return kGSTileNoSurface;

	// ...and the window has to be inside the image. The pool only ever grows a target, so a fit
	// proved here still holds when the pass runs.
	const GSTexture* tex = m_target_pool.GetTexture(surf.pool_handle);
	if (!tex)
		return kGSTileNoSurface;
	const GSVector2i dim = tex->GetSize();
	if (static_cast<int>(tw) > dim.x || static_cast<int>(th) > dim.y)
		return kGSTileNoSurface;

	return src;
}

// Rule 3's DONOR build road: can one live target serve this window whole, by reinterpretation?
//
// This is the class rule 2 refuses by construction. Rule 2 binds a target only where the READ's own
// layout is the target's, and a palettised read of a colour surface never is -- the game is reading
// the target's BYTES as indices, at a different page geometry, usually at an offset into it. The
// byte road serves it by writing the whole target back into the frame's ring and unswizzling those
// bytes into an index image: two GPU passes and a megabyte of ring slots for bytes that were on the
// GPU the entire time. The device can rearrange them in place instead (GSTileSwizzleForms, the same
// arithmetic ps_tile_reinterpret_index performs), and then the window also has an IDENTITY -- its
// owner's -- where a page fingerprint had nothing to say about it.
//
// Every clause is a proof, because a wrong donor is silent wrong output:
//
//  - every page GPU-newest and NOT SYNCED, which is the clause the corpus had to teach and the one
//    the block below explains at length.
//  - ONE owner for EVERY plane of every page of the window, at whole-page granularity. All planes,
//    not just colour: the reinterpretation reads any byte of the owner's RGBA cell, so the alpha
//    byte has to be the owner's too.
//  - not the draw's own colour or depth target -- that is the feedback road, and this stage has none.
//  - a page-aligned CT32/CT24 colour layout (HasCt32PixelSpace), which is what makes the owner's
//    pixel space the guest layout AND what makes it the CT32-shaped one the reinterpretation reads:
//    pool textures are linear from the base page. A 16-bit owner has a byte road and is still
//    refused here, because Locate's owner side is 64x32 pages of 8x8 blocks and a 16-bit target's
//    is not.
//  - the owner's texture actually holding texels on those pages. The model tracks BYTES, and a page
//    whose bytes the surface owns but whose rows nothing has written yet is in perfect order by the
//    model and allocator leftovers to a sampler. Rule 2's own clause, for the same reason.
//  - the window starting at or after the owner's base. The shader computes `blk - dst_bp` unsigned;
//    the page proof implies this, and it is asserted here rather than trusted.
//  - a format the swizzle forms express as an index: PSMT8/PSMT4 and the three alpha-byte views.
bool GSRendererTileGpu::DonorForTextureRead(const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
	GSTileSurfaceId fb_id, GSTileSurfaceId z_id, DonorPlan& out)
{
	out = DonorPlan{};
	if (tex_pages.empty())
		return false;

	// ⚠️ Every page has to be truth the owner's TEXTURE actually holds -- GPU-newest and NOT synced.
	// A synced page is one the model says the byte store already has an equal copy of, and on two
	// roads it says that without having moved a byte (the alias steal and the road-less steal, both
	// rowed in the deferred-accuracy ledger). On those pages the target's pixels and the ring's bytes
	// legitimately disagree, so a donor build and the byte road would produce different texels for
	// the same window -- measured on R&C UYA, where it moved both dumps away from the software
	// golden. The Tile renderer's own donor asks the identical question (its `sync_tex` is
	// ReadbackNeeded over the window, and it only reaches the donor when that set is non-empty).
	const GSPageBitmap need = m_vram_model.ReadbackNeeded(tex_pages, kGSTilePlanesAll);
	if (!tex_pages.andnot(need).empty())
		return false;

	const GSTileSurfaceId owner = m_vram_model.SoleGpuOwner(tex_pages, kGSTilePlanesAll);
	if (owner == kGSTileNoSurface || owner == fb_id || owner == z_id)
		return false;

	const GSVramModel::Surface& surf = m_vram_model.Get(owner);
	if (!surf.alive || !surf.pool_handle || !HasCt32PixelSpace(surf.layout))
		return false;
	if (!surf.residency.contains(tex_pages))
		return false;
	if (m_surface_texels.size() <= owner || !m_surface_texels[owner].filled.HoldsWhole(tex_pages))
		return false;
	if (tex0.TBP0 < surf.layout.bp)
		return false;

	const int idx_fmt = GSTileSwizzleForms::IndexFormatFor(tex0.PSM);
	if (idx_fmt < 0)
	{
		// A direct-colour window one target owns whole that rule 2 did NOT take -- an offset into
		// the target, or a different stride, or a CT32 read of a CT24 owner. Serving it needs a copy
		// leg this road does not have (the reinterpretation only produces index formats), so it
		// keeps the byte road. Counted so the size of that population is a measurement rather than a
		// guess when someone asks whether the leg is worth building.
		m_frame.src_donor_direct++;
		return false;
	}

	out.owner = owner;
	out.src_bp = tex0.TBP0;
	// Pages per texture row, through the one spelling the byte road and the materialise also take.
	// The two roads have to agree here or the same window addresses two different sets of bytes --
	// and this used to be a bare TBW >> 1, correct for PSMT8/PSMT4 and wrong for the alpha-byte
	// views, whose pages are 64 texels wide.
	out.src_bwpg = TextureWindowBwPg(tex0.PSM, tex0.TBW);
	out.owner_bp = surf.layout.bp;
	out.owner_bwpg = surf.layout.bw;
	out.psm = tex0.PSM;
	return true;
}

// A donor build reads an IMAGE, so unlike a materialise it is not free to hoist: it runs at the head
// of its draw's pass, ahead of every draw already in that pass, and one of those may be what wrote
// the pixels it is about to read. The writeback's test, narrowed -- a donor rewrites no ring slot,
// so the slot half of WritebackHoistCollides has nothing to say about it.
bool GSRendererTileGpu::DonorHoistCollides(GSTileSurfaceId owner, const GSPageBitmap& pages) const
{
	const OpenRun& run = CurrentRun();
	if (owner == run.pass.color && pages.intersects(run.color_written))
		return true;
	if (run.pass.depth_used && owner == run.pass.depth && pages.intersects(run.z_written))
		return true;
	return false;
}

// Rule 3 of the same texel road -- the cache of materialised texture sources -- asked and
// deliberately not taken. The road itself is not built yet; what is built here is the question it
// will ask, run against the live stream so the answer is measured instead of assumed: for every
// textured draw rule 2 declined, would a source cache serve this window, and if not, which clause
// refused it. Nothing is built, nothing is bound, and the caller goes on to compose the byte road
// exactly as it did before this existed.
//
// Every refusal lands in its own counter. A silent fallthrough would report the road as unusable
// without saying why, and "the road is wrong" and "the road is not being taken" are the two
// answers the checkpoint has to tell apart.
//
// The probe models BOTH tiers of the cache's invalidation predicate, because the first tier alone
// answers the wrong question. A gen stamp says a byte on the window's pages was written; the
// fingerprint says whether any of them changed. On this corpus those are not the same number
// anywhere it matters, and a probe that stopped at the stamp reported churn the cache would not
// have had -- churn that then propagated into every identity derived from it, which is what made
// the palette-admission column read as a wall rather than as a first frame.
u32 GSRendererTileGpu::ProbeSourceRoad(const PendingDraw& pd, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
	bool paletted, bool mip_active, const GSTileTextureSource::ContentToken& donor_token)
{
	const GSDrawingContext* ctx = m_context;

	// A format the materialise has no arm for. The 16-bit families are sampled straight out of the
	// bytes and nothing builds them into an image, so admitting one here would hand the draw a
	// source slot whose build op the executor then skips -- an image nobody wrote, sampled as
	// texture. The direct-32 DEPTH pair is in the same position: tilegpu_materialise.glsl's CT32 arm
	// has no block XOR, so it would build the window out of the colour twin's blocks. Asked FIRST,
	// before the shape questions, because both are facts about the road rather than about this
	// window.
	if (GSTileSwizzleForms::Direct16FormatFor(tex0.PSM) >= 0 ||
		GSTileSwizzleForms::Direct32FormatFor(tex0.PSM) == static_cast<int>(GSTileSwizzleForms::Direct32Format::Z32))
	{
		m_frame.src_ref_srcfmt++;
		return kNoSourceSlot;
	}

	// The wrap modes a sampler expresses exactly at tw x th. The two REGION modes address a
	// sub-rect of a shared texture page and have no sampler spelling, so they keep the byte road:
	// a performance trade, not an accuracy one, and it is in the bill either way.
	if (ctx->CLAMP.WMS >= CLAMP_REGION_CLAMP || ctx->CLAMP.WMT >= CLAMP_REGION_CLAMP)
	{
		m_frame.src_ref_region++;
		return kNoSourceSlot;
	}
	// Level 0, and a draw declaring a pyramid is served at level 0 like every other one. This was
	// a refusal and is not any more, because it refused a third of the corpus (1595 of
	// rcuya-gameplay's 1608 textured draws, 582 on the SotC gate scene) to buy nothing: NO TileGpu
	// road mipmaps today -- the byte road samples level 0 for every draw whatever TEX1 says -- and
	// rule 3 with maxLod pinned to 0 samples the identical level. Refusing here would have moved
	// those draws from one level-0 road to another level-0 road.
	//
	// What is deferred is therefore not mipmapping-versus-not; it is correct GS LOD selection,
	// which nothing has today and which arrives in M4 as a shader-variant axis (the GS's K/L and
	// MMIN modes are not the hardware's derivative-based LOD). Counted in its own two columns
	// because the second one is the population M4 has to serve: a draw whose pyramid is declared
	// but not active already samples level 0 correctly.
	if (ctx->TEX1.MXL != 0)
	{
		m_frame.src_mip_draws++;
		if (mip_active)
			m_frame.src_mip_live++;
	}

	// Ask the cache. It holds nothing until a later chunk builds into it, so today it answers "no
	// entry" for every window and the frame-to-frame record below is what classifies the draw;
	// from the chunk that builds, this is the authority and the record only adds what the cache
	// cannot know -- which windows a draw already recorded THIS frame has named. The probe hands
	// back the content stamp it compared against, which is the whole invalidation predicate: the
	// sum of the per-page write generations over the window, monotone under both a CPU write and
	// a native draw, so stamp inequality is exactly "some byte under this window moved". Walking
	// those pages is the bulk of what this probe adds to the GS thread, and it happens once.
	const GSTileTextureSource::WindowKey key = GSTileTextureSource::KeyFor(tex0, m_env.TEXA);
	const GSTileTextureSource::ProbeResult pr = m_tex_source.Probe(m_vram_model, tex0, m_env.TEXA, tex_pages);
	const u64 stamp = pr.gen_stamp;

	// The second tier of the same predicate, and on this corpus the load-bearing one. A moved gen
	// stamp says some byte on the window's PAGES was written, not that any texel changed, and
	// games re-upload identical texture data every frame: measured on 11 of the 18 dumps, the
	// stamp holds for exactly nothing, so a road that believed it would rebuild every source every
	// frame, and every identity derived from it -- the palette pairs the expanded cache admits on
	// -- would churn with it and never admit. So when the stamp has moved, ask the pages what
	// their bytes actually are. Lazily, and at most once per draw: only a moved stamp needs the
	// fingerprint, and the library memoises per page off the write generations, so a page
	// rewritten with the bytes it already held is hashed once per frame however many windows read
	// it. This is the tier Lookup runs as RebuildsSamePages (97.7% of GT4's rebuilds), modelled
	// here rather than assumed away.
	//
	// ...unless a DONOR serves the window, in which case the fingerprint is not the tier at all. The
	// bytes under a window whose truth is a rendered target are not in the CPU shadow, so the
	// fingerprint declines and returns nothing, having first hashed nothing useful; what identifies
	// that window is the owning target and its version, and the caller has already worked that out.
	// Taking it here rather than falling back to the declining tier is what retires the whole
	// "unprovable rebuild" class -- 18.4 of SotC's 24.5 believed rebuilds a frame, and all of
	// GT4-OPB's -- and it removes an 8 KB-per-moved-page hash on the way.
	GSTileTextureSource::ContentToken content = donor_token;
	bool content_taken = donor_token.Known();
	const auto ContentStamp = [&]() -> GSTileTextureSource::ContentToken {
		if (!content_taken)
		{
			const u64 h = m_tex_source.ProbePageContent(m_mem, m_vram_model, tex_pages);
			content = GSTileTextureSource::ContentToken{
				h != 0 ? GSTileTextureSource::ContentToken::Source::Pages :
						 GSTileTextureSource::ContentToken::Source::None,
				h};
			content_taken = true;
		}
		return content;
	};
	// Does the window still hold the content this record was taken under? An unknown token on either
	// side is "cannot say" -- the fingerprint declined and no donor answers for it -- and cannot-say
	// is never the same content, which ContentToken's own equality already says.
	const auto SameBytes = [&](const ProbedWindow& p) { return ContentStamp() == p.content; };

	// This frame's record of the window, if some earlier draw already probed it.
	u32 wi = 0;
	while (wi < m_probe_windows.size() && !(m_probe_windows[wi].key == key))
		wi++;

	// Deferred execution against an immediate cache. A draw already recorded this frame names
	// this window's entry, and a rebuild would land IN PLACE, in the same entry and the same
	// texture -- so at VSync that earlier draw would sample bytes it never asked for. The pin
	// discipline refuses instead: this draw takes the byte road and the rebuild happens next
	// frame naturally. Fail-closed by construction, and counted rather than assumed rare.
	//
	// A stamp that moved without the bytes moving is not a rebuild at all: Lookup serves the entry
	// it already has and nothing lands in place, so there is nothing for an earlier draw to be
	// protected from. The pin closes on a window whose bytes really did change under it.
	if (wi < m_probe_windows.size() && m_probe_windows[wi].admitted && m_probe_windows[wi].stamp != stamp &&
		!SameBytes(m_probe_windows[wi]))
	{
		m_frame.src_ref_pin++;
		return kNoSourceSlot;
	}

	// Whether an earlier draw of this frame already had a source built for this window -- which
	// makes this draw a hit whatever the cache's own state was at the top of the frame.
	const bool built_this_frame = (wi < m_probe_windows.size() && m_probe_windows[wi].admitted);

	if (wi == m_probe_windows.size())
	{
		// First sight this frame. Was the window here last frame, and does it still hold the same
		// bytes? That is the whole stability question: a title that genuinely re-writes its
		// sampled pages every frame rebuilds every source every frame, and then the expanded cache
		// never admits and rule 3 serves almost nothing. Answered in two tiers, counted in two
		// columns, because the gap between them is the entire result on this corpus.
		ProbedWindow w;
		w.key = key;
		w.stamp = stamp;
		w.build_id = ++m_probe_build_counter;
		const ProbedWindow* prev = nullptr;
		for (const ProbedWindow& p : m_probe_prev)
		{
			if (p.key == key)
			{
				prev = &p;
				break;
			}
		}
		if (!prev)
		{
			// Nothing to compare against yet. Take the fingerprint anyway: a first CPU build takes
			// one too (it is what the NEXT frame's rebuild is checked against), so a probe that
			// skipped it would model a cheaper cache than the one being built.
			w.content = ContentStamp();
		}
		else
		{
			w.prev_seen = true;
			if (prev->stamp == stamp)
			{
				m_frame.src_stamp_kept++;
				w.prev_current = true;
				w.build_id = prev->build_id;
				// The stamp held, so no byte under the window moved and the fingerprint taken
				// under it still stands. Carried rather than recomputed -- that is the whole
				// point of the cheap tier being first.
				w.content = prev->content;
			}
			else
			{
				m_frame.src_stamp_moved++;
				w.content = ContentStamp();
				if (!w.content.Known())
				{
					// GPU-side truth under the window: the CPU bytes here are not the bytes
					// anything would build from, so the move stands as a rebuild -- believed, not
					// proved, and counted apart so a low rescue rate cannot be read as "the bytes
					// changed" when it means "we could not look".
					m_frame.src_stamp_opaque++;
				}
				else if (w.content == prev->content)
				{
					// The pages were rewritten with the bytes they already held. No rebuild, and
					// -- the part that matters downstream -- the SAME identity, so the palette
					// pairs keyed on it survive the frame boundary and the expanded cache admits.
					m_frame.src_stamp_rescued++;
					w.prev_current = true;
					w.build_id = prev->build_id;
				}
			}
		}
		m_probe_windows.push_back(w);
	}
	else if (m_probe_windows[wi].stamp != stamp)
	{
		// Seen this frame and the stamp has moved since -- either a window nothing has named yet
		// (something refused it) or one whose bytes the pin check above just found unchanged.
		// Same two tiers; not counted in the stamp columns, which ask about the frame before, not
		// about mid-frame churn.
		ProbedWindow& pw = m_probe_windows[wi];
		const bool same = SameBytes(pw);
		pw.stamp = stamp; // either way this is the stamp the entry now carries
		if (!same)
		{
			// Nothing is pinned on it, so the entry is rebuilt under the new bytes and takes a new
			// identity with them.
			pw.content = ContentStamp();
			pw.build_id = ++m_probe_build_counter;
			pw.prev_current = false;
		}
	}
	ProbedWindow& w = m_probe_windows[wi];

	if (paletted)
	{
		// The palette is a second, independent identity: a paletted source holds raw indices and
		// the fused RGBA8 is keyed on (index build id x palette content id), so a palette change
		// re-expands without re-deswizzling. The pair is admitted on SECOND SIGHT across a frame
		// boundary -- a fresh pair defers and this draw takes the byte road for one frame, which
		// is what stops a per-frame-volatile window paying an expansion pass for a draw that runs
		// once. High deferrals beside high hits is the healthy shape, not a fault.
		// The palette's identity, taken where the draw's palette was resolved: a hash of the CPU
		// words for an ordinary palette, and the load's (owner x version x CBP x entries) tuple for
		// one the CLUT gather put on the device, where there are no CPU words to hash at all.
		const u64 pal_id = pd.pal_id;
		// ...except for a window a DONOR serves, where the wait cannot ever end and is measuring the
		// wrong alternative anyway: the index image is the owner target's own pixels, which change
		// every frame by definition, so no pair over it survives a frame boundary -- and the other
		// road is not "expand in the shader", it is "write the whole target back into the ring and
		// unswizzle it again, then expand in the shader". Those draws admit on first sight. The probe
		// and the build must agree, so the same answer goes to both.
		//
		// A GATHERED PALETTE joins them, for the same reason on the other axis: its identity carries
		// the owner's version, which moves every time the game re-renders the palette, so the pair
		// can literally never recur (measured -- gt4 631 of 631 loads new, gt4opb 1259 of 1259) and
		// the alternative road is not the cheap one either.
		const GSTileExpandedCache::AdmitWhen when = (donor_token.Known() || pd.pal_record != 0) ?
														GSTileExpandedCache::AdmitWhen::FirstSight :
														GSTileExpandedCache::AdmitWhen::SecondSight;
		if (m_expand_cache.ProbeAdmit(w.build_id, pal_id, when) == GSTileExpandedCache::Admission::Deferred)
		{
			m_frame.src_ref_palette++;
			return kNoSourceSlot;
		}
	}

	// The frame's working set against the array it has to fit in. Capacity is a hard bound rather
	// than a hope: a frame whose distinct windows outrun the array refuses the overflow to the byte
	// road. This is the WINDOW census -- the bind table below counts (image, sampler) pairs, which is
	// the same population unless one window is sampled through two filters -- and it is checked here
	// so a refusal costs nothing but the probe.
	if (!w.admitted)
	{
		if (m_frame.src_distinct >= kMaxSourceSlots)
		{
			m_frame.src_ref_capacity++;
			return kNoSourceSlot;
		}
		w.admitted = true;
		m_frame.src_distinct++;
	}

	// Served. What serving it would COST is the second column: a hit needs no work at all, a
	// rebuild a deswizzle into the entry it already has, a fresh window a first build. Classified
	// off two frames of record, so the hit column is a lower bound -- a real 512-entry cache also
	// hits on a window last drawn several frames ago.
	m_frame.src_eligible++;
	if (pr.hit || built_this_frame || w.prev_current)
		m_frame.src_hit++;
	else if (pr.would_rebuild || w.prev_seen)
		m_frame.src_rebuild++;
	else
		m_frame.src_fresh++;
	return wi;
}

// The sampler the draw's TEX1/CLAMP asks for. Only the eight combinations rule 3 admits can arrive:
// the probe refuses REGION_CLAMP and REGION_REPEAT because a sampler cannot express them, and
// mipmapping is off (triln 0), which is what pins max LOD to level 0 in the device's sampler cache.
u8 GSRendererTileGpu::SourceSamplerKey(u32 wms, u32 wmt, bool ltf)
{
	GSHWDrawConfig::SamplerSelector ss;
	ss.tau = (wms == CLAMP_REPEAT) ? 1 : 0;
	ss.tav = (wmt == CLAMP_REPEAT) ? 1 : 0;
	ss.biln = ltf ? 1 : 0;
	return ss.key;
}

// This frame's slot for one (image, sampler) pair. The pair rather than the image alone because the
// filtering and the wrap ride in the descriptor: the shader has one index and one fetch site, so two
// draws sampling the same window through different TEX1/CLAMP need two descriptors. Rare and cheap;
// a linear scan is the right shape at this size, and the frame's table is a hard bound.
u32 GSRendererTileGpu::SourceSlotFor(GSTexture* tex, u8 sampler)
{
	for (u32 i = 0; i < m_plan_sources.size(); i++)
	{
		if (m_plan_sources[i].texture == tex && m_plan_sources[i].sampler == sampler)
			return i;
	}
	if (m_plan_sources.size() >= kMaxSourceSlots)
	{
		m_frame.src_ref_bind++;
		return kNoSourceSlot;
	}
	m_plan_sources.push_back(GSDevice::GSTileGpuPassPlan::SourceBind{tex, sampler});
	m_frame.src_bind_slots++;
	return static_cast<u32>(m_plan_sources.size() - 1);
}

// Rule 3's build, and the point where the draw changes road. The cache decides whether this window
// already has an image and whether that image is still current, and on a build it hands the texture
// to the builder, which queues the pass that fills it. With an image in hand the draw takes a bind
// slot, its state row names the source, and its fragment stage samples that image instead of
// decoding ring bytes.
//
// TWO build roads reach here and the difference is where the texels come from. Without a donor the
// window's bytes have already been composed into the frame's ring and the source is unswizzled out
// of them. WITH one, the window's pages belong to a live target and the source is reinterpreted
// straight out of that target's texture -- and the caller has NOT composed anything, because not
// composing is the entire point of that road. The cache is told neither: what it sees is a builder
// and a content token, and the token is what keeps the two identities from meeting.
//
// Every refusal leaves the draw exactly where the caller put it -- on the byte road -- so a refusal
// costs at most a wasted compose. Never a dropped draw. ⚠️ On the donor road the caller must compose
// the window itself when this returns false, since it skipped the compose to get here.
bool GSRendererTileGpu::MaterialiseSourceRoad(PendingDraw& pd, u32 window, const GIFRegTEX0& tex0,
	const GSPageBitmap& tex_pages, u32 epoch, const DonorPlan* donor)
{
	const ProbedWindow& w = m_probe_windows[window];

	// The content identity the cache decides rebuilds by. The probe took it BEFORE the window was
	// composed, which is the only moment a page fingerprint is honest -- after the compose those
	// pages carry bytes a writeback has superseded -- and for a donor window it is the owner's
	// identity, which no compose can disturb. Unknown never matches anything, so a window nothing
	// can vouch for rebuilds rather than serving stale texels.
	const GSTileTextureSource::ContentToken token = w.content;

	SourceMaterialiser builder;
	builder.r = this;
	builder.epoch = epoch;
	builder.donor = donor;

	GSTileTextureSource::BuiltOutcome outcome = GSTileTextureSource::BuiltOutcome::Failed;
	u64 build_id = 0;
	GSTexture* tex =
		m_tex_source.LookupBuilt(m_vram_model, tex0, m_env.TEXA, tex_pages, builder, token, &build_id, &outcome);
	switch (outcome)
	{
		case GSTileTextureSource::BuiltOutcome::Hit:
			(donor ? m_frame.src_donor_hits : m_frame.src_cache_hits)++;
			break;
		case GSTileTextureSource::BuiltOutcome::Rescued:
			(donor ? m_frame.src_donor_rescued : m_frame.src_cache_rescued)++;
			break;
		case GSTileTextureSource::BuiltOutcome::Built: break; // counted by the emitter
		case GSTileTextureSource::BuiltOutcome::RefusedPinned: m_frame.src_pin_refusals++; break;
		case GSTileTextureSource::BuiltOutcome::RefusedCapacity: m_frame.src_cap_refusals++; break;
		case GSTileTextureSource::BuiltOutcome::Failed: m_frame.src_build_failed++; break;
	}
	if (!tex)
	{
		if (donor)
			m_frame.src_donor_refused++;
		return false;
	}

	// Stage two, for a paletted window. What the cache just handed back is an INDEX image -- the
	// palette is not in its key, which is the whole reason one build serves every palette the game
	// cycles through it -- so the colour a draw samples comes from fusing that image with THIS draw's
	// palette. Both halves are content-keyed: the palette on its CLUT words, the fusion on (index
	// build id x palette content id), so a palette change re-expands without re-deswizzling and a
	// window re-uploaded with the bytes it already had keeps both.
	const u32 pal_entries = GSLocalMemory::m_psm[tex0.PSM].pal;
	if (pal_entries > 0)
	{
		m_frame.src_pal_draws++;
		u64 pal_id = 0;
		GSTexture* pal = nullptr;
		if (pd.pal_record != 0)
		{
			// A GATHERED palette has no CPU words to build from -- that is the entire point -- so
			// rule 3's palette texture is the device gather itself, an N x 1 image the executor fills
			// off the owner target. A four-bit draw reading one SLOT of a 256-entry load wants a
			// sixteen-texel view of that image, which this road has no copy leg for: it keeps the
			// byte road, whose fetch addresses the entry bias directly.
			GpuPalette* const gp = FindGpuPalette(pd.pal_record);
			if (gp && pd.pal_bias == 0)
				pal = ClutGatherTexture(*gp, pd);
			pal_id = pd.pal_id;
			if (!pal)
				m_frame.clut_r3_refused++;
		}
		else
		{
			// The same words the byte road reads, from the same place: AccumulateDraw already ran
			// Read32 (CSA, CPSM and TEXA applied there) and appended the expansion to the frame's
			// palette stream. Taking them from there rather than re-reading the CLUT is what makes
			// the two roads' colours the same bytes by construction rather than by coincidence.
			const u32* const clut = m_plan_palettes.data() + pd.pal_offset;
			pal = m_palette_cache.Lookup(clut, pal_entries, m_mem.m_clut.GetReadGeneration(), &pal_id);
			// The namespace the gathered ids are kept out of: the expanded cache fuses
			// (index build id x palette id) pairs, and two ids from different worlds compared equal
			// would fuse two unrelated pairs with nothing to catch it.
			pal_id = ClutCpuPalId(pal_id);
		}
		if (!pal || pal_id == 0 || build_id == 0)
		{
			m_frame.src_pal_missing++;
			if (donor)
				m_frame.src_donor_refused++;
			return false;
		}

		SourceExpander expander;
		expander.r = this;
		GSTileExpandedCache::BuiltOutcome eo = GSTileExpandedCache::BuiltOutcome::Failed;
		// Keyed on the TOKEN, not on whether a donor was handed in: the probe asked the same question
		// off the same token, and the two must give the same answer or a pair the probe admitted
		// defers at the build and the draw is refused for nothing. They differ exactly when a donor
		// was offered and the build then fell back to the ring.
		GSTexture* const expanded = m_expand_cache.LookupBuilt(tex, build_id, pal, pal_id, expander,
			(w.content.source == GSTileTextureSource::ContentToken::Source::Target || pd.pal_record != 0) ?
				GSTileExpandedCache::AdmitWhen::FirstSight :
				GSTileExpandedCache::AdmitWhen::SecondSight,
			&eo);
		switch (eo)
		{
			case GSTileExpandedCache::BuiltOutcome::Hit: m_frame.src_expand_hits++; break;
			case GSTileExpandedCache::BuiltOutcome::Built: break; // counted by EmitExpandOp
			case GSTileExpandedCache::BuiltOutcome::Deferred: m_frame.src_expand_defer++; break;
			case GSTileExpandedCache::BuiltOutcome::RefusedCapacity: m_frame.src_expand_cap++; break;
			case GSTileExpandedCache::BuiltOutcome::Failed: m_frame.src_expand_failed++; break;
		}
		if (!expanded)
		{
			if (donor)
				m_frame.src_donor_refused++;
			return false;
		}
		// From here the draw samples an ordinary RGBA8 image and the road knows nothing about palettes.
		tex = expanded;
	}

	const u32 slot = SourceSlotFor(tex, SourceSamplerKey(pd.wms, pd.wmt, pd.ltf));
	if (slot == kNoSourceSlot)
	{
		if (donor)
			m_frame.src_donor_refused++;
		return false;
	}

	// On the road. The byte road's bit comes off: this draw's fragment stage reads no ring word, so a
	// pass made only of draws like it compiles no byte road at all -- which is where the instruction
	// budget the variants bought is actually spent.
	pd.src_slot = slot;
	pd.road_mask = GSDevice::kGSTileGpuRoadSource;
	m_frame.src_served++;
	if (donor)
		m_frame.src_donor_served++;
	return true;
}

bool GSRendererTileGpu::SourceMaterialiser::BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA)
{
	// The donor road reads the owner target's texture and no ring word at all, so it goes through
	// neither the epoch nor the page table -- which is also why its op survives a frame that composed
	// nothing (the executor's byte-road gate is a union for exactly that reason).
	if (donor)
		return r->EmitDonorOp(tex, TEX0, *donor);
	return r->EmitMaterialiseOp(tex, TEX0, TEXA, epoch);
}

bool GSRendererTileGpu::SourceExpander::BuildTileExpansion(GSTexture* index, GSTexture* palette, GSTexture* dst)
{
	return r->EmitExpandOp(dst, index, palette);
}

// A prep op names its textures by index into the plan's prep-texture list, so every texture one
// touches has to have a slot in it. Deduped, unlike a materialise destination (which is fresh by
// construction): one palette serves every window drawn through it this frame, and one index image
// serves every palette cycled over it.
u32 GSRendererTileGpu::PrepTextureIndex(GSTexture* tex)
{
	for (u32 i = 0; i < m_plan_prep_textures.size(); i++)
	{
		if (m_plan_prep_textures[i] == tex)
			return i;
	}
	m_plan_prep_textures.push_back(tex);
	return static_cast<u32>(m_plan_prep_textures.size() - 1);
}

// The paletted road's second pass. Queued from inside the expanded cache's build, which
// MaterialiseSourceRoad runs straight after the index build -- so if that build emitted a
// materialise, this op sits behind it in the array, and array order IS execution order at the pass
// head. That is the whole ordering argument: nothing here has to look at what came before.
bool GSRendererTileGpu::EmitExpandOp(GSTexture* dst, GSTexture* index, GSTexture* palette)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Expand;
	op.target = PrepTextureIndex(dst);
	op.index_texture = PrepTextureIndex(index);
	op.palette_texture = PrepTextureIndex(palette);
	m_plan_prep_ops.push_back(op);
	m_frame.src_expand_builds++;
	return true;
}

bool GSRendererTileGpu::EmitMaterialiseOp(GSTexture* tex, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 epoch)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Materialise;
	// A source is not a target: the op names it through the plan's prep_textures list, which is
	// this vector, and the target list stays exactly what it was.
	op.target = PrepTextureIndex(tex);
	op.bp = tex0.TBP0;
	// Pages per texture row, through the one spelling every road shares, so the materialise addresses
	// the window exactly the way the byte road would have.
	op.bw = TextureWindowBwPg(tex0.PSM, tex0.TBW);
	op.psm = tex0.PSM;
	op.epoch = epoch;
	// A 24-bit window's alpha byte belongs to whatever else shares those bytes, so TEXA's expansion
	// is baked into the image here, at build time -- which is what the CPU deswizzlers do and what
	// the cache's key already accounts for.
	op.texa = (tex0.PSM == PSMCT24) ? (1u | (texa.AEM ? 2u : 0u) | (static_cast<u32>(texa.TA0) << 8)) : 0u;
	m_plan_prep_ops.push_back(op);
	m_frame.src_builds++;
	return true;
}

// The donor build's op. It names TWO lists -- its destination in prep_textures (a source is nothing's
// render target) and its owner in the frame's target list -- which is why the op carries both indices
// and why PlanTargetIndex is called here: an owner no draw of this frame rendered into would
// otherwise not be in the target list at all, and the executor resolves the op through it.
//
// No epoch, no page entries, no TEXA. The reinterpretation reads the owner's texels and writes
// indices; a paletted window's TEXA rides in the CLUT expansion, exactly as it does for a
// materialised index image.
bool GSRendererTileGpu::EmitDonorOp(GSTexture* tex, const GIFRegTEX0& tex0, const DonorPlan& donor)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Donor;
	op.target = PrepTextureIndex(tex);
	op.donor_target = PlanTargetIndex(donor.owner);
	op.bp = donor.src_bp;
	op.bw = donor.src_bwpg;
	op.psm = donor.psm;
	op.owner_bp = donor.owner_bp;
	op.owner_bwpg = donor.owner_bwpg;
	m_plan_prep_ops.push_back(op);
	m_frame.src_donor_builds++;
	return true;
}

u32 GSRendererTileGpu::PlanTargetIndex(GSTileSurfaceId id)
{
	if (m_plan_target_of_surface.size() <= id)
		m_plan_target_of_surface.resize(id + 1, GSDevice::GSTileGpuPassPlan::kNoTarget);
	if (m_plan_target_of_surface[id] == GSDevice::GSTileGpuPassPlan::kNoTarget)
	{
		m_plan_target_of_surface[id] = static_cast<u32>(m_plan_target_surfaces.size());
		m_plan_target_surfaces.push_back(id);
	}
	return m_plan_target_of_surface[id];
}

// EmitPrepOp's writeback rule, stated as a pure function so the prefill decision below and the
// op it predicts cannot drift apart. See the declaration for what it means.
void gsTileComposableBlocksPerByte(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount], u32 (&out)[4])
{
	out[0] = out[1] = out[2] = out[3] = 0;

	// Who writes back: an owner holding unsynced truth on some plane, with a road to carry it.
	GSTileSurfaceId writers[kGSTilePlaneCount];
	u32 writer_bytes[kGSTilePlaneCount];
	u32 writer_count = 0;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const GSTileRingPlaneState& p = planes[pi];
		if (p.owner == kGSTileNoSurface || p.truth_mask == 0 || p.synced || !p.HasByteRoad())
			continue;
		u32 w = 0;
		while (w < writer_count && writers[w] != p.owner)
			w++;
		if (w == writer_count)
		{
			writers[writer_count] = p.owner;
			writer_bytes[writer_count] = p.write_bytes;
			writer_count++;
		}
	}

	// What each of them writes: the union over every plane it owns here, synced or not -- that is
	// the mask EmitPrepOp puts on the page entry, and the shader honours no other filter -- landed
	// only in the byte lanes its format stores.
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		for (u32 w = 0; w < writer_count; w++)
		{
			if (writers[w] != planes[pi].owner)
				continue;
			for (u32 b = 0; b < 4; b++)
			{
				if (writer_bytes[w] & (0xFFu << (b * 8)))
					out[b] |= planes[pi].truth_mask;
			}
			break;
		}
	}
}

u32 gsTileComposableBlocks(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount])
{
	u32 per_byte[4];
	gsTileComposableBlocksPerByte(planes, per_byte);
	return per_byte[0] | per_byte[1] | per_byte[2] | per_byte[3];
}

bool gsTileComposeCoversWholePage(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount])
{
	u32 per_byte[4];
	gsTileComposableBlocksPerByte(planes, per_byte);
	for (u32 b = 0; b < 4; b++)
	{
		if (per_byte[b] != GSVramModel::kFullBlockMask)
			return false;
	}
	return true;
}

// See the declaration. GSTileComposeBuckets::Gather's lossy test, as a pure function of the same
// four plane states, so the readers that must decide before the compose and the counter that runs
// inside it cannot come apart.
bool gsTilePageByteTruthReachable(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount])
{
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const GSTileRingPlaneState& p = planes[pi];
		// No truth here (the CPU shadow is newest, and the prefill carries it), or the ring already
		// holds it. Either way nothing has to move.
		if (p.truth_mask == 0 || p.synced)
			continue;
		if (!p.HasByteRoad())
			return false;
	}
	return true;
}

void GSRendererTileGpu::RingPlaneStateFor(u32 page, GSTileRingPlaneState (&planes)[kGSTilePlaneCount]) const
{
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
		planes[pi].owner = owner;
		planes[pi].truth_mask = m_vram_model.TruthMask(page, pi);
		planes[pi].synced = m_vram_model.SyncedPages(pi).test(page);
		const bool road = owner != kGSTileNoSurface && HasByteRoad(m_vram_model.Get(owner).layout);
		planes[pi].write_bytes = road ? gsTileWritebackByteMask(m_vram_model.Get(owner).layout.psm) : 0;
	}
}

GSPageBitmap GSRendererTileGpu::PagesDepthSeedable(const GSPageBitmap& pages, const GSTileSurfaceLayout& z_layout) const
{
	GSPageBitmap out;
	pages.forEachSetPage([&](u32 page) {
		GSTileRingPlaneState planes[kGSTilePlaneCount];
		RingPlaneStateFor(page, planes);
		if (!gsTilePageByteTruthReachable(planes))
			return;
		// ...and every plane that holds truth here holds it through this Z buffer's own pixel space.
		// A plane with no owner is the CPU shadow, which is authoritative by construction: no GPU
		// truth was dropped to get there. See gsTileDepthSeedSourceMatches for why a foreign view of
		// the same pages is refused rather than trusted.
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			const GSTileSurfaceId owner = planes[pi].owner;
			if (owner == kGSTileNoSurface || planes[pi].truth_mask == 0)
				continue;
			if (!gsTileDepthSeedSourceMatches(m_vram_model.Get(owner).layout, z_layout))
				return;
		}
		out.set(page);
	});
	return out;
}

// A ring slot for `page` in the current epoch. Prefill from S unless the page is, in every
// plane, whole-page unsynced truth of a surface with a byte road -- exactly the case where a
// writeback this epoch composes every byte (ComposeRingPages emits it right after asking here).
// A page synced already (either its bytes are in S after a stall readback, or an earlier
// writeback this epoch composed its live slot) prefills too: over the same live slot the memcpy
// runs first and the writeback's blocks land over it, so the extra copy is harmless.
//
// ⚠️ The per-plane test above is a PROXY for the question that actually matters -- "will the
// writebacks this compose emits reach every BYTE of the page?" -- and a proxy is only as good as
// the rule it stands in for. Nothing in the two functions makes them move together: the decision
// is per-plane and whole-page, the writeback's reach is per-block, per-OWNER and per-BYTE, and a
// byte that is neither prefilled nor inside some writeback's reach keeps whatever the previous
// tenant of this ring OFFSET left there. Only the zero slot is ever cleared, so that is not a zero
// and not a stale guest byte either: on a desktop allocator it is some other page's bytes from an
// earlier frame, and on an allocator that recycles foreign pages (Android Mali page pools) it is
// arbitrary content. The model counts the page composed either way.
//
// So the coverage is computed too and both must agree: a slot goes unprefilled only where the
// planned writebacks provably reach every byte of every block. Strictly more prefill than the
// proxy alone, never less.
//
// The byte half of that is not hypothetical padding. A PSMCT24 surface stores nothing in the alpha
// byte, so its writeback masks that byte off -- while the model still hands it the alpha PLANES,
// because a colour draw claims every colour plane of the pages it covers whatever its frame format
// stores in them (the draw itself no longer WRITES that channel). Every plane then
// passes the proxy, the block coverage is whole, and byte 3 of all 2048 words of the page is
// written by nobody. Eight of the sixteen corpus titles shipped pages like that every frame (MGS3,
// Ratchet & Clank UYA both scenes, Gran Turismo 4, Ace Combat 5, Dirge of Cerberus, both Katamari
// scenes); the allocation-poison lever caught two of them consuming the bytes on this box, and what
// they consumed was another page's leftovers rather than anything guest-shaped.
u32 GSRendererTileGpu::EnsureRingSlot(u32 page, bool force_prefill)
{
	GSTileRingPlaneState planes[kGSTilePlaneCount];
	RingPlaneStateFor(page, planes);
	bool needs_prefill = force_prefill;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (planes[pi].owner == kGSTileNoSurface || planes[pi].truth_mask != GSVramModel::kFullBlockMask ||
			planes[pi].synced || !planes[pi].HasByteRoad())
			needs_prefill = true;
	}
	if (!force_prefill && !needs_prefill && !gsTileComposeCoversWholePage(planes))
	{
		needs_prefill = true;
		m_frame.prefill_uncomposed++;
	}

	if (m_ring_live[page] != 0)
	{
		RingEntry& e = m_ring_entries[m_ring_live[page] - 1];
		e.prefill_s |= needs_prefill;
		return m_ring_live[page] - 1;
	}
	RingEntry e = {};
	e.page = static_cast<u16>(page);
	e.epoch_first = static_cast<u16>(m_epoch);
	e.epoch_last = 0xFFFF; // open
	e.prefill_s = needs_prefill;
	m_ring_entries.push_back(e);
	m_ring_live[page] = static_cast<u16>(m_ring_entries.size());
	m_frame.ring_pages++;
	return static_cast<u32>(m_ring_entries.size() - 1);
}

void GSRendererTileGpu::SupersedeRingSlots(const GSPageBitmap& pages)
{
	bool cut = false;
	GSPageBitmap closed;
	pages.forEachSetPage([&](u32 page) {
		if (m_ring_live[page] == 0)
			return;
		RingEntry& e = m_ring_entries[m_ring_live[page] - 1];
		e.epoch_last = static_cast<u16>(m_epoch);
		if (e.prefill_s)
		{
			// The bytes an already-planned reader must see are about to be overwritten in S:
			// keep them. (A slot the GPU composes entirely needs nothing -- its writebacks read
			// targets, not S.)
			e.versioned = true;
			e.version_offset = static_cast<u32>(m_ring_versions.size());
			m_ring_versions.resize(m_ring_versions.size() + GS_PAGE_SIZE);
			std::memcpy(m_ring_versions.data() + e.version_offset, m_mem.vm8() + page * GS_PAGE_SIZE, GS_PAGE_SIZE);
			m_frame.ring_versions++;
		}
		m_ring_live[page] = 0;
		closed.set(page);
		cut = true;
	});
	if (cut)
	{
		// The closed slots' synced claims die with them: the next reader of these pages this
		// frame composes a fresh slot (its writebacks re-run into it).
		m_vram_model.ClearSynced(closed);
		m_epoch++;
	}
}

void GSRendererTileGpu::EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages,
	u32 seed_blocks, const u32* seed_page_blocks)
{
	if (pages.empty())
		return;
	// A seed overwrites the surface's texels, so a palette gathered out of those pages has to be
	// captured into the frame's stream BEFORE the op that destroys it is queued: array order is
	// execution order at the pass head, and a copy queued after the seed would read what the seed
	// wrote. (A writeback only reads, so it asks nothing.) Both seed kinds ask, though only the
	// colour one can find anything: a palette's owner is always a colour surface.
	const bool is_seed = kind == GSDevice::GSTileGpuPrepKind::Seed ||
						 kind == GSDevice::GSTileGpuPrepKind::SeedDepth;
	if (is_seed)
		NoteClutSourceWritten(id, pages);
	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = kind;
	op.target = PlanTargetIndex(id);
	op.bp = surf.layout.bp;
	op.bw = surf.layout.bw;
	op.psm = surf.layout.psm;
	op.byte_mask = gsTileWritebackByteMask(surf.layout.psm);
	op.seed_blocks = seed_blocks;
	op.seed_blocks_per_page = (is_seed && seed_page_blocks) ? 1u : 0u;
	op.seed_from_merge = (is_seed && m_merge_seeding) ? 1u : 0u;
	op.epoch = m_epoch;
	op.first_page_entry = static_cast<u32>(m_plan_page_entries.size());
	pages.forEachSetPage([&](u32 page) {
		u32 mask = GSVramModel::kFullBlockMask;
		u32 keep = GSDevice::kGSTileGpuNoKeepMask;
		if (op.seed_blocks_per_page != 0)
		{
			// The batched merge seed: the blocks are this page's, not the op's. A page the owner
			// holds nothing of is dropped rather than carried with a zero mask, so the op's page set
			// is exactly the per-page road's set of ops.
			mask = seed_page_blocks[page];
			if (mask == 0)
				return;
		}
		else if (kind == GSDevice::GSTileGpuPrepKind::Writeback)
		{
			// The blocks this surface holds newest on the page, over the planes it owns there
			// (they agree except after a CPU write that shrank one plane's claim differently --
			// then the union over-writes bytes of the other plane, a bounded imprecision noted
			// in the design record).
			mask = 0;
			for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
			{
				if (m_vram_model.OwnerOf(page, pi) == id)
					mask |= m_vram_model.TruthMask(page, pi);
			}
			// The upload merge's exception, and ONLY inside the merge's own compose: an upload is
			// about to land on this page, its bytes are what the slot's prefill carries, and the
			// model still says the surface holds the blocks it is landing in. So the blocks the
			// upload covers whole come out of the writeback altogether, and the ones it covers in
			// part carry a keep mask naming its bytes. (A later reader composing the same page
			// gets neither: by then the model accounts for the transfer, and a keep mask there
			// would leave the CPU's bytes standing over bytes the surface has since made its own.)
			if (m_merge_emitting)
			{
				for (const UploadPageBytes& ub : m_merge_bytes)
				{
					if (ub.page != page)
						continue;
					mask &= ~ub.blocks_whole;
					keep = m_merge_keep_base + ub.first_word;
					break;
				}
			}
			if (mask == 0)
				return;
		}
		// The page's column and row in the surface's pixel space, which is the divide the writeback
		// shader used to run in every one of a page's 2048 invocations for one workgroup-uniform
		// answer -- run here instead, once, where the hardware has a divider. GSTileGpuPageEntry
		// carries the argument and the packing. `op.bw` is the surface's own stride and cannot be
		// zero: the target pool divides by the same number to size the texture this reads.
		pxAssert(op.bw != 0);
		const u32 rel = (page + GS_MAX_PAGES - (op.bp >> 5)) & (GS_MAX_PAGES - 1);
		const u32 pgcol = rel % op.bw;
		const u32 pgrow = rel / op.bw;
		pxAssert(pgcol < (1u << GSDevice::kGSTileGpuPageEntryRowShift) && pgrow < GS_MAX_PAGES);
		const u16 rowcol = static_cast<u16>((pgrow << GSDevice::kGSTileGpuPageEntryRowShift) | pgcol);
		// The trailing zero is the ring slot, which the EXECUTOR fills once it knows where the ring
		// landed -- see GSTileGpuPageEntry. Spelled rather than left to aggregate init so the word
		// that reaches GPU memory is a value this line chose.
		m_plan_page_entries.push_back(
			GSDevice::GSTileGpuPageEntry{static_cast<u16>(page), rowcol, mask, keep, 0});
	});
	op.page_entry_count = static_cast<u32>(m_plan_page_entries.size()) - op.first_page_entry;
	if (op.page_entry_count == 0)
		return;
	m_plan_prep_ops.push_back(op);
	if (kind == GSDevice::GSTileGpuPrepKind::Writeback)
	{
		m_frame.writeback_ops++;
		m_frame.writeback_pages += op.page_entry_count;
	}
	else
	{
		m_frame.seed_ops++;
		m_frame.seed_pages += op.page_entry_count;
		if (kind == GSDevice::GSTileGpuPrepKind::SeedDepth)
		{
			m_frame.seed_ops_depth++;
			m_frame.seed_pages_depth += op.page_entry_count;
		}
		// A seed writes the surface's texture, so anything cached under its identity is stale. (The
		// palettes gathered out of those pages were captured at the top of this function, before the
		// op was queued.)
		BumpSurfaceVersion(id);
	}
}

// See the declaration for why this is sized by the model's id space and not by a constant.
//
// Two vectors indexed by surface id, plus the list of ids this gather touched. The stamp is what
// makes the reuse cheap: an entry belongs to this gather only when its stamp is this gather's
// serial, so a gather clears the page bitmaps it actually uses and nothing else, and the arrays
// stop growing once the biggest id the session ever allocates has been seen.
void GSTileComposeBuckets::Gather(const GSVramModel& model, const GSPageBitmap& need)
{
	const u32 slots = model.SurfaceSlots();
	if (m_stamp.size() < slots)
	{
		m_stamp.resize(slots, 0);
		m_pages.resize(slots);
	}
	m_order.clear();
	m_lossy = 0;
	m_lossy_depth = 0;

	const u64 serial = ++m_serial;
	need.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!model.Truth(pi).test(page) || model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface && owner < m_stamp.size());
			const GSTileSurfaceLayout& owner_l = model.Get(owner).layout;
			if (!gsTileSurfaceHasByteRoad(owner_l))
			{
				m_lossy++;
				m_lossy_depth += (owner_l.kind == GSTileSurfaceKind::Depth) ? 1 : 0;
				continue;
			}
			if (m_stamp[owner] != serial)
			{
				m_stamp[owner] = serial;
				m_pages[owner].clear();
				m_order.push_back(owner);
			}
			m_pages[owner].set(page);
		}
	});
}

// Every byte of `pages` reachable through the ring for the current epoch: a slot per page,
// prefilled from S where any byte is CPU-newest, and a writeback of every surface holding
// unsynced truth on them (in the ring's read-modify-write order, one op per owner). Marks what
// the ring now holds synced. Owners without a byte road contribute nothing (the slot keeps its
// prefill: stale where they held newest) -- counted, and warned once, never silent.
void GSRendererTileGpu::ComposeRingPages(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	// Under the upload merge every page here is one the merge staged, and its slot MUST take the S
	// prefill whatever the model says about who holds the page -- see EnsureRingSlot.
	pages.forEachSetPage([&](u32 page) { EnsureRingSlot(page, m_merge_emitting); });

	const GSPageBitmap need = m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll);
	if (need.empty())
		return;

	// Bucket by owner: one writeback op each, in first page-ascending appearance order (array
	// order is execution order at the pass head). Nothing here re-enters ComposeRingPages, so the
	// scratch stays this gather's for the whole emit loop -- EmitPrepOp only reads the model, the
	// plan arrays and the CLUT/version bookkeeping.
	m_compose_buckets.Gather(m_vram_model, need);
	for (u32 b = 0, n = m_compose_buckets.Count(); b < n; b++)
		EmitPrepOp(GSDevice::GSTileGpuPrepKind::Writeback, m_compose_buckets.IdAt(b), m_compose_buckets.PagesAt(b));

	if (const u32 lossy = m_compose_buckets.LossyPages())
	{
		m_frame.lossy_pages += lossy;
		m_frame.lossy_pages_depth += m_compose_buckets.LossyDepthPages();
		if (!m_warned_lossy)
		{
			m_warned_lossy = true;
			Console.Warning("TileGpu: page truth held by a surface without a byte road (depth, or a base that is not "
							"page-aligned) was read or stolen -- the bytes are stale there. Counted per frame as "
							"lossy pages.");
		}
	}

	// What the ring now holds (or, for the lossy part, what we will treat as held): synced.
	m_vram_model.OnReadback(need);
}

// Truth on `pages` is about to become unreachable and no byte road can carry it: count it and say
// so once. Two roads reach here -- a target whose own layout has no writeback shader, and the depth
// plane, which has none at all.
void GSRendererTileGpu::NoteLossyPages(const GSPageBitmap& pages, GSTileSurfaceKind kind)
{
	if (pages.empty())
		return;
	m_frame.lossy_pages += pages.count();
	if (kind == GSTileSurfaceKind::Depth)
		m_frame.lossy_pages_depth += pages.count();
	if (!m_warned_lossy)
	{
		m_warned_lossy = true;
		Console.Warning("TileGpu: page truth moved through a surface without a byte road (depth, or a base that is "
						"not page-aligned) -- the bytes are stale there. Counted per frame as lossy pages.");
	}
}

// A surface without a byte road is about to take `pages`: the model's steal invariant needs the
// previous owners' truth marked synced, but no bytes actually move -- the previous owners' pixels
// are lost to the byte store, and the taker's texture keeps whatever it held.
void GSRendererTileGpu::LossySteal(const GSPageBitmap& pages, GSTileSurfaceKind kind)
{
	if (pages.empty())
		return;
	NoteLossyPages(pages, kind);
	m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll));
}

// Compose `pages` into the ring for the draw being accumulated, and open a new pass if the
// writebacks that took cannot run at the head of the open one.
//
// A writeback the plan build leaves inside the open pass runs at that pass's head, ahead of every
// draw already in it. Where it collides with one of them -- reading a surface they have rendered
// into, or rewriting a ring slot they have sampled -- the hoist composes stale bytes and hands them
// backwards, and the draw opens its own pass instead, exactly as a seed does. Where it collides with
// none of them the hoist is provably invisible and the pass stays whole: pass count is the currency
// of this design.
void GSRendererTileGpu::ComposeForPendingDraw(const GSPageBitmap& pages, PendingDraw& pd)
{
	const u32 ops_before = static_cast<u32>(m_plan_prep_ops.size());
	ComposeRingPages(pages);
	if (m_plan_prep_ops.size() != ops_before && WritebackHoistCollides(ops_before))
	{
		pd.break_before = true;
		pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::Writeback);
		m_frame.writeback_breaks++;
		BreakOpenPass();
	}
}

// One draw's colour and depth surfaces claim the same pages, so only one of them can be the
// holder and the other's byte truth on them is dropped. Same model move as a road-less steal --
// synced without moving bytes -- but a different accuracy story, so a counter and a warning of
// its own: this one is about GS memory layout (how tightly the game packs FRAME against ZBUF),
// not about which surfaces this stage can write back yet.
void GSRendererTileGpu::AliasSteal(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	m_frame.alias_steal_pages += pages.count();
	if (!m_warned_alias)
	{
		m_warned_alias = true;
		Console.Warning("TileGpu: a draw's colour and depth footprints share pages -- the game packs FRAME and ZBUF "
						"close enough that one draw writes both through the same pages. Both surfaces claim them and "
						"only one can hold them, so the colour surface's byte truth there is dropped. Counted per "
						"frame as alias-steal pages.");
	}
	m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll));
}

void GSRendererTileGpu::BreakOpenPass()
{
	OpenRun& run = CurrentRun();
	run.pass = GSTileGpuPassKey{};
	run.color_written.clear();
	run.z_written.clear();
	run.read.clear();
	run.tex_count = 0;
	// The cap's count resets with everything else the open pass tracked, so it stays in step with the
	// plan build's own (j - i).
	run.draw_count = 0;
	// ...and what the pass's fragment program would have had to carry. A new pass unions nothing.
	run.gather_arm = 0;
	run.target_road = false;
	// date_surface / date_written deliberately survive: the DATE snapshot's run spans ordinary pass
	// breaks and is reset only where the snapshot is genuinely retaken.
}

// See the declaration. All three families are asked, not just the one the walk stopped on.
void GSRendererTileGpu::CensusPassBreak(const GSTileGpuPassKey& prev_key, u32 prev_draws, u32 d)
{
	const PendingDraw& pd = m_plan_pending[d];
	u32 mask = pd.break_reasons;
	mask |= gsTileGpuBreakKeyFields(prev_key,
		gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode, m_depth_uniform_passes,
			pd.self_mask != 0, m_segregate_self_read));
	// gsTileGpuPassEnd tests the cap BEFORE it looks at the draw, so a pass that reached the cap ended
	// there whatever the draw would have said. The draw's own causes are still recorded beside it: the
	// question a merge asks is whether removing a cause would join the two passes, and where the cap
	// also fired the answer is no.
	if (m_max_pass_draws != 0 && prev_draws >= m_max_pass_draws)
		mask |= gsTileGpuBreakBit(GSTileGpuBreakCause::Cap);
	// A boundary that names no cause is a break site that forgot to record itself. It would read as a
	// merge opportunity nobody can collect, which is worse than no census at all.
	pxAssertMsg(mask != 0, "TileGpu ended a render pass and no break site recorded why");
	m_frame.break_boundaries++;
	gsTileGpuTallyBreak(mask, m_frame.break_cause, m_frame.break_cause_sole, &m_frame.break_multi);
	m_break_masks[mask]++;
}

// See the declaration and ModelFrame::break_adreno_passes. The policy triple is spelled here rather
// than asked of the device because the whole point is to answer it on a device that says no to all
// three -- GSDeviceVK::TileGpuPrefersDepthUniformPasses, ::TileGpuSegregatesSelfRead and
// ::TileGpuMaxPassDraws are Adreno-only, so an M2 census reads a structural zero for exactly the
// three causes the primary device pays most for. On an Adreno the two arms coincide, which is a
// free self-check rather than a duplicate.
void GSRendererTileGpu::CensusAdrenoPasses()
{
	const u32 count = static_cast<u32>(m_plan_draws.size());
	if (count == 0)
		return;
	constexpr u32 kAdrenoPassDrawCap = 64; // GSDeviceVK::TileGpuMaxPassDraws
	const auto key_of = [this](u32 d) {
		const PendingDraw& pd = m_plan_pending[d];
		return gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode, true,
			pd.self_mask != 0, true);
	};
	const auto breaks_at = [this](u32 d) { return m_plan_pending[d].break_before; };
	GSTileGpuPassKey prev_key;
	u32 prev_draws = 0;
	bool have_prev = false;
	for (u32 i = 0; i < count;)
	{
		if (have_prev)
		{
			u32 mask = m_plan_pending[i].break_reasons | gsTileGpuBreakKeyFields(prev_key, key_of(i));
			if (prev_draws >= kAdrenoPassDrawCap)
				mask |= gsTileGpuBreakBit(GSTileGpuBreakCause::Cap);
			pxAssertMsg(mask != 0, "TileGpu's Adreno pass recut ended a pass and no cause names it");
			gsTileGpuTallyBreak(mask, m_frame.break_adreno_cause, m_frame.break_adreno_cause_sole, nullptr);
			m_break_adreno_masks[mask]++;
		}
		const u32 j = gsTileGpuPassEnd(i, count, kAdrenoPassDrawCap, key_of, breaks_at);
		m_frame.break_adreno_passes++;
		prev_key = key_of(i);
		prev_draws = j - i;
		have_prev = true;
		i = j;
	}
}

// The one piece of bookkeeping draw reordering adds to the pass model, and the one that is silent
// when it is wrong.
//
// Accumulation's idea of "the open pass" and the plan build's cut have to name the same boundaries,
// because a draw's rule-2 slot and its writeback hoist verdict are decided against the first and
// checked against the second. The plan build cuts over the EMISSION order, so what a draw follows
// there is the question -- and after a flush, what it follows is the LAST emitted run's last draw,
// not anything the runs still hold.
//
// So the last emitted run's open pass carries into the run this draw joins (run zero, since the
// flush emptied the backlog) and every other slot resets. The alternative -- reset everything and
// force the draw to break -- is also consistent, and costs a pass at every flush: +44% on Shadow of
// the Colossus, +39% on Armored Core 3, measured.
void GSRendererTileGpu::CarryOpenPassAcrossFlush(u32 runs_emitted)
{
	if (runs_emitted == 0)
		return; // nothing went out, so nothing the runs hold is stale
	const OpenRun tail = m_open_runs[runs_emitted - 1];
	ResetOpenRuns();
	m_open_runs[0] = tail;
}

void GSRendererTileGpu::ResetOpenRuns()
{
	for (m_open_run = 0; m_open_run < GSTileGpuReorderScheduler::kMaxRuns; m_open_run++)
	{
		BreakOpenPass();
		CurrentRun().date_surface = kGSTileNoSurface;
		CurrentRun().date_written = GSVector4i::zero();
		CurrentRun().date_cover.clear();
	}
	m_open_run = 0;
}

// A writeback the plan build leaves inside the open pass runs at that pass's head, ahead of every
// draw already in it. Two things make that wrong, and nothing else does:
//
//  - the writeback reads the target's pixels, so a draw of the open pass that renders into THAT
//    surface on a page the writeback takes would have its pixels composed before it drew them;
//  - the writeback rewrites the page's live ring slot in place, so a draw of the open pass that
//    samples that page would retroactively read the new bytes.
//
// The slot half is compared by SLOT, and it has to be. An epoch comparison proves nothing --
// OnNativeDraw drops a page's synced bit without advancing the epoch, so the page is re-composed
// into the same live slot at the same epoch, which is the case the unconditional break was
// written for. A page comparison is sound but pessimistic the other way: an upload between the
// earlier read and this writeback closes the page's slot and bumps the epoch, so the writeback
// composes a NEW slot and the earlier read keeps the bytes it saw. So the read set remembers the
// slot each page was read through and this compares it against the live one, which is the slot
// EnsureRingSlot just handed the writeback.
//
// One conservatism left: the read set is the whole composed read window (the size-fixed TEX0
// footprint), not the pages the draw's coordinates actually reach inside it.
bool GSRendererTileGpu::WritebackHoistCollides(u32 first_op) const
{
	// ⚠️ The run the ops will land in, which is the run whose pass they would be hoisted to the head
	// of -- not "whichever run is newest". They are the same thing only because the draw's run is
	// chosen before any of its prep ops is emitted; see m_open_run.
	const OpenRun& run = CurrentRun();
	for (u32 k = first_op; k < m_plan_prep_ops.size(); k++)
	{
		const GSDevice::GSTileGpuPrepOp& op = m_plan_prep_ops[k];
		// Writebacks only, and stated rather than assumed: this reads `op.target` as an index into
		// the plan's TARGET list, and a materialise names its destination in prep_textures instead.
		// The ranges this is asked about today hold nothing else (ComposeRingPages emits writebacks
		// and the materialise is queued after it returns), so this is the contract in code rather
		// than a fix -- but the two index spaces are the kind of thing a later road walks into.
		if (op.kind != GSDevice::GSTileGpuPrepKind::Writeback)
			continue;
		GSPageBitmap p;
		for (u32 e = op.first_page_entry; e < op.first_page_entry + op.page_entry_count; e++)
			p.set(m_plan_page_entries[e].page);

		bool hit = false;
		(p & run.read).forEachSetPage([&](u32 page) { hit |= (run.read_slot[page] == m_ring_live[page]); });
		if (hit)
			return true;

		const GSTileSurfaceId src = m_plan_target_surfaces[op.target];
		if (src == run.pass.color && p.intersects(run.color_written))
			return true;
		if (run.pass.depth_used && src == run.pass.depth && p.intersects(run.z_written))
			return true;
	}
	return false;
}

// The pass plan's geometry and the memory model's traffic, one flushed batch at a time. Copies
// this draw's vertices and indices into the frame streams, drives the model for its texture
// read and its target footprints, and records an indexed indirect draw plus the PendingDraw
// (its surfaces, transform inputs and prep-op range, resolved at the plan build). Skips
// empty-rect draws exactly as ObserveDraw does, so the plan and the pass model count the same
// draws. Indices are 0-based within the batch; the draw's vertex_offset rebases them into the
// frame vertex stream.
void GSRendererTileGpu::AccumulateDraw()
{
	const GSVector4i bbox = ComputeDrawBBox();
	const GSVector4i r = bbox.rintersect(m_context->scissor.in);
	if (r.rempty())
		return;

	// Wrong-fast geometry. Triangles, lines and points are drawn at their native footprint: the
	// index buffer is already a topology list (strips and fans expanded at decode), so it copies
	// straight in and selects the matching executor pipeline. Sprites are two opposite corners we
	// synthesise into a quad (four corners, two triangles), so they ride the triangle pipeline.
	// The pass model still sees every draw through ObserveDraw, so accounting is unaffected --
	// only the submitted geometry is shaped here.
	GSDevice::GSTileGpuTopology topology;
	switch (m_vt.m_primclass)
	{
		case GS_TRIANGLE_CLASS:
		case GS_SPRITE_CLASS:
			topology = GSDevice::GSTileGpuTopology::Triangle;
			break;
		case GS_LINE_CLASS:
			topology = GSDevice::GSTileGpuTopology::Line;
			break;
		case GS_POINT_CLASS:
			topology = GSDevice::GSTileGpuTopology::Point;
			break;
		default:
			return;
	}
	const bool is_sprite = (m_vt.m_primclass == GS_SPRITE_CLASS);

	const u32 vcount = m_vertex->tail;
	const u32 icount = m_index->tail;
	if (vcount == 0 || icount == 0)
		return;

	const GSDrawingContext* ctx = m_context;

	// -- the model: where this draw lands and what it reads ---------------------------------
	const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};

	// What this draw actually writes. An alpha test every fragment fails still lands whatever
	// AFAIL keeps alive -- Classic's zm/fm derivation, and the idiom SotC uses for its
	// full-screen fades and post sprites: ATE=1 ATST=NEVER AFAIL=FB_ONLY is "colour only, leave
	// depth alone" whatever ZTE/ZMSK say. Missing it deposits those sprites' Z=max into the
	// persistent depth buffer and the whole 3D pass then fails GEQUAL.
	//
	// The same intent also arrives as a COMPARISON that cannot come out either way, and it has to
	// be recognised there too. Katamari Damacy stamps a lower Z into the King's head's rectangle
	// with an untextured sprite carrying vertex alpha 0x00 under ATST=NOTEQUAL AREF=0 -- "0 != 0"
	// is false on every fragment -- plus AFAIL=ZB_ONLY, "a failing fragment still writes depth".
	// Read as a live per-fragment test the fragment stage discards the whole sprite, the stamp
	// never lands, and the head draw behind it (ZTST=GEQUAL at the stamped Z) then fails over
	// every pixel the star covers. So the fold asks the alpha, not just the register.
	//
	// It asks for the alpha's INTERVAL, not for a single value, because the draws that most need
	// deciding are textured. R&C UYA's exhaust flare is one additively-blended paletted strip
	// under ATST=GEQUAL AREF=128 AFAIL=RGB_ONLY whose palette tops out at alpha 128 and whose
	// vertex alpha tops out at 64: (128 * 64) >> 7 = 64, so nothing it draws can reach the
	// reference. Decidable, and the whole flare depends on it -- RGB_ONLY paints the colour and
	// drops the alpha and depth writes, while the live test discards every fragment instead.
	// GetAlphaMinMax is the software renderer's own bound and folds TFX, TCC, TEXA/AEM and the
	// CLUT's alpha range in, which is what makes a textured draw answerable at all.
	//
	// Only asked where the answer can matter: ATE off and the two self-deciding ATSTs need no
	// alpha, and the scan is the one part of this that costs anything on a palettised draw.
	//
	// Two cases refuse the bound and take the whole range instead:
	//   * AA1. GetAlphaMinMax answers for a renderer that either supports coverage alpha or
	//     assumes a fixed 128 for it; this road applies no coverage alpha at all, so neither
	//     answer describes its fragments.
	//   * A palette the device holds and the CPU has not synced. The scan reads the CPU's CLUT
	//     RAM, which is stale for those slots, and a verdict off the wrong palette is unsound --
	//     the same failure GSRendererTile::BuildLoweringInput guards with its cpu_current check.
	//     Asked of the slot mirror about THIS DRAW'S OWN SLOTS, the same window ResolveDrawPalette
	//     resolves further down. A readback would cost far more than the verdict is worth.
	//
	// ⚠️ WIDENING IS NOT THE CONSERVATIVE ANSWER HERE, and the comment that used to stand in this
	// place said it was ("conservative can only cost a fold, unsound moves pixels"). Both halves
	// were wrong. Asking about every slot rather than the draw's window made ONE gathered palette
	// un-bound every later palettised alpha test in the frame; and a fold that answers Varies is a
	// SECOND APPROXIMATION rather than the safe one whenever AFAIL is not KEEP, because the
	// fragment stage answers a live test by DISCARDING, which drops writes the console makes. That
	// is what cost LEGO Star Wars both of its floor-reflection draws. What makes the widening safe
	// is not the widening: it is gsTileGpuPlanAlphaSplit below, which realizes a varying test
	// exactly. The contrast with ReaderFlags, where widening IS free, is that its conservative
	// branch paints and this one discards.
	u32 alpha_min = 0, alpha_max = 255;
	if (ctx->TEST.ATE && ctx->TEST.ATST != ATST_ALWAYS && ctx->TEST.ATST != ATST_NEVER && !PRIM->AA1)
	{
		const u32 tex_pal = PRIM->TME ? GSLocalMemory::m_psm[ctx->TEX0.PSM].pal : 0;
		const bool palettised = tex_pal > 0;
		if (m_clut_mirror.DrawSlotsCpuCurrent(tex_pal, ctx->TEX0.CSA))
		{
			if (palettised)
				m_mem.m_clut.Read32(ctx->TEX0, m_draw_env->TEXA);
			const GSVertexTrace::VertexAlpha& av = GetAlphaMinMax();
			alpha_min = static_cast<u32>(std::clamp(av.min, 0, 255));
			alpha_max = static_cast<u32>(std::clamp(av.max, 0, 255));
		}
	}
	const GSTileAlphaTestFold atst_fold = gsTileFoldAlphaTest(ctx->TEST.ATE, ctx->TEST.ATST,
		ctx->TEST.AREF, alpha_min, alpha_max);
	const bool atst_all_fail = (atst_fold == GSTileAlphaTestFold::AllFail);
	if (ctx->TEST.ATE && ctx->TEST.ATST != ATST_NEVER && ctx->TEST.ATST != ATST_ALWAYS &&
		atst_fold != GSTileAlphaTestFold::Varies)
	{
		// The population the fold moves off the fragment stage, per frame. NEVER and ALWAYS are
		// excluded: they never reached it in the first place.
		if (atst_all_fail)
			m_frame.atst_fold_fail++;
		else
			m_frame.atst_fold_pass++;
	}
	const u32 afail = ctx->TEST.GetAFAIL(ctx->FRAME.PSM);
	const bool z_write = gsTileDepthWriteSurvives(ctx->TEST.ZTE, ctx->ZBUF.ZMSK, atst_all_fail, afail);
	const bool z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;
	const bool z_used = z_write || z_test;
	// The exact-AFAIL population, counted where the fold is known. A draw whose test folds one way
	// for every fragment is already exact (the fold moved it into the write flags); what is left is
	// the draws that genuinely straddle AREF under an AFAIL that keeps something, where the fragment
	// stage's discard drops what the console still writes.
	// ...and the sliver of it the DESTINATION READ can serve exactly: RGB_ONLY on a draw that writes
	// no depth, where landing the failing fragment's colour costs nothing at the depth stage. It is
	// the road that predates the draw split below, and where the split serves the same draw the
	// split wins -- it needs no read, so it declares nothing and taxes no pass. Settled once the
	// split is planned, a few dozen lines down.
	const bool afail_keep_alpha_road = m_self_read && gsTileGpuAfailKeepsAlpha(atst_fold, afail, z_write);
	if (atst_fold == GSTileAlphaTestFold::Varies && ctx->TEST.ATE && afail != AFAIL_KEEP)
	{
		m_frame.afail_varies++;
		if (afail == AFAIL_RGB_ONLY)
			m_frame.afail_varies_rgb_only++;
		if (ctx->TEST.ZTE && !ctx->ZBUF.ZMSK)
			m_frame.afail_varies_depth++;
	}
	// FBMSK per channel, not all-or-nothing. Games use the frame buffer as scratch for one
	// channel at a time -- OutRun 2006 lays three full-screen sprites a frame over the finished
	// world under FBMSK=0x00FFFFFF with a black fragment colour, meaning to rewrite the frame's
	// alpha byte alone, and Beyond Good & Evil draws silhouettes into the buffer that is ON
	// SCREEN under the same mask. Writing the RGB of those draws paints black over the world in
	// the first case and the silhouettes over the picture in the second, and bge's alpha-only
	// full-screen blit at the end of each frame then carries the damage into the other buffer --
	// a ratchet to black over three frames. gsTileFrameColorWriteMask folds the AFAIL modes in
	// on top, alpha honoured like the other channels; a partly masked channel lands whole (the
	// approximation, ledgered).
	const u32 fb_fmsk = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk;
	const u8 color_mask = gsTileFrameColorWriteMask(ctx->FRAME.FBMSK, fb_fmsk, atst_all_fail, afail);
	const bool color_written = color_mask != 0;
	if (!color_written && !z_write)
		return; // nothing lands anywhere: a no-op draw

	if (!PoolSupports(fb_l) || (z_used && !PoolSupports(z_l)))
	{
		// An 8/4-bit or zero-stride target: no surface can back it on this road yet. The draw
		// is dropped (its bytes never land) -- counted, and the pass model still saw it.
		m_frame.skipped_draws++;
		return;
	}

	// The destination-alpha test. On a 24-bit target there is no alpha to test and the GS passes
	// nothing (Classic's finding, tested on hardware) -- the draw is a no-op. Otherwise the draw
	// reads its own target's alpha before writing: from the live pixel where the device serves the
	// in-pass read, and from a pass snapshot where it does not -- and the snapshot road is what the
	// staleness break further down exists for.
	const GSTileDateFold date_fold =
		gsTileFoldDate(ctx->TEST.DATE, ctx->TEST.DATM, GSLocalMemory::m_psm[ctx->FRAME.PSM].trbpp);
	if (date_fold == GSTileDateFold::DropDraw)
		return;
	const u32 date = gsTileGpuDateRow(date_fold);

	// -- the sound varies road -----------------------------------------------------------------
	//
	// A live alpha test is answered by the fragment stage DISCARDING the fragments that fail it,
	// which is exact under AFAIL=KEEP and drops writes the console makes under the other three.
	// gsTileGpuPlanAlphaSplit realizes it by splitting the draw instead -- Classic's
	// `split_rgb_only` and `PASS_THEN_FAIL`, and the shared Tile lowering's `atst_split`, none of
	// which needs anything from the device. What it needs from HERE is the order proof, which LABELS
	// the result rather than gating it: the split happens either way, because every alternative was
	// measured and this is the least wrong of them.
	//
	//  - DEPTH. The exact channel split's colour pass writes no depth, so a fragment it lands is
	//    tested against the depth the draw FOUND rather than against what an earlier primitive of
	//    the same draw wrote.
	//  - COLOUR. The alpha byte moves to the other pass, so anything reading DESTINATION ALPHA in
	//    the first -- a C=Ad blend factor, and on this renderer DATE, which can read the live pixel
	//    where the device serves the in-pass read -- reads what the draw found rather than what an
	//    earlier primitive of the same draw wrote. Classic guards the blend half; the DATE half is
	//    ours, and Classic's own answer to it (swap the stencil DATE for a PrimID one) has no
	//    analogue here.
	//
	// Both clauses are about primitives of this draw OVERLAPPING, so the overlap test settles
	// either of them. It walks the vertex list, so it is asked only where the cheap clauses have
	// already failed -- the same discipline GSRendererTile::BuildLoweringInput applies to the same
	// question. What it buys is the census's exact-versus-reordered line, which is how much of this
	// road's population it serves with no approximation at all, and the fact a per-draw rule for
	// the reordered remainder would hang off.
	//
	// ⚠️ GetPrimitiveOverlapDrawlist, NOT PrimitiveOverlap. The latter answers UNKNOW for every
	// triangle draw that is not a single quad, by POLICY rather than by cost: it exists for
	// Classic's barrier decisions, where UNKNOW is the safe answer. Every draw whose cheap proof
	// fails over the whole corpus is a triangle draw, so the cheap question would call the entire
	// population reordered -- and the real answer proves twice as many exact (R&C UYA effects
	// 60 -> 120 a frame, SotC 41 -> 64, dirge 10 -> 25).
	const bool blend_reads_dest_alpha = PRIM->ABE && ctx->ALPHA.C == 1;
	const bool cheap_z = !z_test || (ctx->TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z);
	const bool cheap_colour = !blend_reads_dest_alpha && date == 0;
	GSTileAlphaSplit split = gsTileGpuPlanAlphaSplit(atst_fold, ctx->TEST.ATST, ctx->TEST.AREF, afail,
		color_mask, z_write, z_test, cheap_z, cheap_colour, m_afail_split, m_split_refuse_fb_only);
	if (split.reorders_fragments)
	{
		m_frame.afail_split_overlap_asked++;
		if (GetPrimitiveOverlapDrawlist(false) == PRIM_OVERLAP_NO)
		{
			split = gsTileGpuPlanAlphaSplit(atst_fold, ctx->TEST.ATST, ctx->TEST.AREF, afail, color_mask,
				z_write, z_test, true, true, m_afail_split, m_split_refuse_fb_only);
		}
	}
	// The census of what became of the varying non-KEEP population. The number the design owes is
	// the DISCARD line -- draws still losing a write the console makes -- and the reorder line
	// beside it is the population served with the failing fragments arriving after the passing
	// ones, which is an approximation but not a lost write.
	if (split.discards_a_write)
	{
		if (split.refusal == GSTileAlphaSplitRefusal::NoDepthTest)
		{
			m_frame.afail_split_refused_no_ztest++;
			m_frame.afail_split_discard++;
		}
		else if (split.refusal == GSTileAlphaSplitRefusal::RefusedFbOnly)
		{
			// The policy refusal, counted apart from the structural one above and from the whole-lever
			// OFF arm below, because it is the number that scopes the key: a title whose hash moves
			// with EmuCore/GS/TileGpuSplitRefuseFbOnly on is a title with a non-zero count here, and
			// nothing else may move.
			m_frame.afail_split_refused_fb_only++;
			m_frame.afail_split_discard++;
		}
		else if (!m_afail_split)
			m_frame.afail_split_discard++;
		else if (split.reorders_fragments)
			m_frame.afail_split_reordered++;
		else
			m_frame.afail_split_served++;
	}

	// Where the split serves the draw it supersedes the destination read's alpha keep, which is the
	// same repair bought with a declaration this does not need.
	const bool afail_keep_alpha = afail_keep_alpha_road && split.pass_count == 1;
	// What the PRINCIPAL draw -- the one the rest of this function builds -- writes and tests. The
	// unsplit `color_mask` and `z_write` above stay the draw's own and go on driving every piece of
	// page and seed bookkeeping below, because those describe what the draw lands in GS memory and
	// the split's two passes union back to exactly that.
	const u8 draw_color_mask = split.pass[0].color_mask;
	const bool draw_z_write = split.pass[0].z_write;

	// Admission to the in-pass destination read. The classifier is asked only where it can change the
	// answer: without the road there is nothing to admit to, and its blend arm costs a GetAlphaMinMax
	// scan on every blended draw, which is real CPU work on a road that is CPU-bound. So the gate
	// names the reasons that are live rather than asking unconditionally.
	const bool fbmsk_exact = gsTileFrameWriteMaskIsExact(ctx->FRAME.FBMSK, fb_fmsk);
	// Whether the blend unit has anything to do for this draw, spelt exactly as the blend key spells
	// it further down -- ABE with a colour to write, and not the (X - X) * C + Cs identity. One
	// definition, because two decisions read it and they must not disagree: whether the draw is
	// admitted to the shader blend, and whether its output is what lands (and so gets quantised).
	const bool blend_active =
		PRIM->ABE && color_written && !(ctx->ALPHA.A == ctx->ALPHA.B && ctx->ALPHA.D == 0);
	// A frame that stores fewer bits than the target holds quantises the blend's RESULT, which the
	// executor's blend unit cannot do -- it runs after the fragment stage. So a blended draw on such
	// a frame goes to the shader whatever its equation is.
	const bool blend_needs_quantised_result =
		blend_active && gsTileGpuFrameQuantises(GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt);
	// What this draw would be admitted FOR, before any device has an opinion. Asked only where it can
	// be non-zero: the classifier's blend arm costs a GetAlphaMinMax scan on every blended draw, which
	// is real CPU work on a road that is CPU-bound, so the gate names the reasons that are live rather
	// than asking unconditionally.
	u32 wanted_classes = 0;
	u32 reader_flags = 0;
	if (date != 0 || PRIM->ABE || !fbmsk_exact || afail_keep_alpha)
	{
		reader_flags = ReaderFlags(color_written);
		wanted_classes = gsTileGpuWantedClasses(
			reader_flags, date != 0, fbmsk_exact, blend_needs_quantised_result, afail_keep_alpha);
	}
	const u32 admitted_classes = AdmittedClasses(wanted_classes);
	// The classifier's answer WITHOUT the alpha keep, which is what the shipped reader counter has
	// meant since the crossover study and must go on meaning, or the device runner's before-and-after
	// numbers stop being comparable.
	const u32 self_mask = gsTileGpuSelfReadUses(admitted_classes & ~kGSTileGpuClassAfailKeep,
		kGSTileGpuClassAll, m_self_read);
	// What this draw's self mask finally is: the classifier's answer plus the alpha keep, which is a
	// per-fragment write mask and so rides the arm that already does one. Folded HERE, rather than
	// where the mask is stored on the PendingDraw further down, because the pass key below asks
	// whether this draw reads and the two have to be the same question. A draw keyed as a non-reader
	// whose mask then unions into its pass would make that pass declare anyway, which is exactly
	// what segregation exists to prevent.
	//
	// The keep IS gated on the road -- afail_keep_alpha carries m_self_read from its computation,
	// because the shader serves the keep out of the destination read and has nothing to serve it
	// from on a device without one. The !m_self_read disjunct below is therefore never the deciding
	// term; it is there so this expression stays correct if that upstream gate ever moves. Where
	// there IS a road, the keep is one of the budget's classes like any other.
	const bool keep_alpha_admitted =
		afail_keep_alpha && (!m_self_read || (admitted_classes & kGSTileGpuClassAfailKeep) != 0);
	// ...and the colour write mask, where the lever moved it off the pipeline. Folded in HERE for the
	// same reason the keep is: the pass key two dozen lines down asks whether this draw reads, and
	// that has to be the same question the mask itself is answered by.
	//
	// NOT a per-class budget charge, and the reason is the road's own admission rule rather than an
	// exemption: gsTileGpuMasksInShader takes only draws whose blend or whose FBMSK the fragment stage
	// was ALREADY evaluating, so every draw it moves was already a declared reader and the budget has
	// nothing new to price. That is what keeps the budget out of it. It also matters that it could not
	// price this one if it were asked: the four classes it does price buy EXACTNESS, which it cannot
	// measure, so it prices only their tax -- while this buys SPEED, whose size scales with the same
	// draw count the tax does, and a line fitted to the first question would refuse exactly the
	// population the second pays best on (Spider-Man 3's masked draws are ~3,400 in the budget's unit
	// against a line of 256, on the one title the lever exists for).
	//
	// The last two arguments are spelt exactly as the state row will spell them further down -- the
	// row's blend-enable bit and its fbmsk word -- because they decide whether this draw's colour
	// already goes through the shader's byte tail. Two definitions of that would disagree eventually,
	// and the disagreement is ±1 in a channel on a handful of pixels: the kind nothing notices for
	// months. `self_mask` and not `draw_self_mask`, which is not merely the pre-keep value being
	// convenient: the keep is a per-FRAGMENT mask, so a draw carrying only that one still leaves its
	// passing fragments outside the tail, and they are the ones the write mask covers.
	const bool shader_blend = (self_mask & GSDevice::kGSTileGpuSelfBlend) != 0;
	const bool shader_fbmsk =
		(self_mask & GSDevice::kGSTileGpuSelfMask) != 0 && gsTileGpuFrameKeepMask(ctx->FRAME.FBMSK, fb_fmsk) != 0;
	// The PRINCIPAL's mask, not the draw's: this decides what the pipeline binds for the draw the
	// rest of this function builds, and the split's second draw answers it for itself.
	const bool mask_in_shader = gsTileGpuMasksInShader(
		m_shader_write_mask, draw_color_mask, blend_active, shader_blend, shader_fbmsk);
	const u32 draw_self_mask = self_mask | (keep_alpha_admitted ? GSDevice::kGSTileGpuSelfMask : 0u) |
							   (mask_in_shader ? GSDevice::kGSTileGpuSelfMask : 0u);
	const GSPageBitmap fb_pages = GSVramModel::PagesForRect(fb_l, r);
	GSPageBitmap z_pages;
	if (z_used)
		z_pages = GSVramModel::PagesForRect(z_l, r);

	// Targets: the surfaces this draw renders into (created or grown here; nothing moves yet).
	// Only the 32/16-bit colour and depth families have pool geometry: an 8/4-bit or
	// zero-stride target has no surface on this road, and the draw is dropped before it can
	// touch the model -- counted; the pass model still saw it.
	//
	// The colour surface is not always this view's own: under surface-identity containment it can
	// be a container holding the view at a rectangle offset, and then everything below that speaks
	// in the surface's pixel space is translated by `fb_off`. A draw carrying a depth attachment is
	// refused a DISPLACED placement -- one vertex transform serves both attachments, so displacing
	// the colour view would displace the depth rows with it, into guest Z belonging to other
	// pixels, and depth containment is deferred. It keeps the zero-offset merge, which moves
	// nothing and is where most of the corpus's win is.
	GSVector2i fb_off(0, 0);
	const GSTileSurfaceId fb_id = EnsureSurface(fb_l, r, fb_pages, &fb_off, !z_used);
	if (fb_id == kGSTileNoSurface)
	{
		m_frame.skipped_draws++;
		return;
	}
	GSTileSurfaceId z_id = kGSTileNoSurface;
	if (z_used)
	{
		z_id = EnsureSurface(z_l, r, z_pages);
		if (z_id == kGSTileNoSurface)
		{
			m_frame.skipped_draws++;
			return;
		}
	}
	// This draw's rect in the surface's own pixel space, which is the view's own only while the
	// view is its own surface. The guest page footprints above are NOT translated and must not be:
	// they name pages of GS memory, which is the one space containment does not move anything in.
	const GSVector4i off4(fb_off.x, fb_off.y, fb_off.x, fb_off.y);
	const GSVector4i sr = r + off4;

	// The containment census's own boundary, counted here because it is a change BETWEEN draws.
	// Placed after both EnsureSurface calls and before anything reads the pass key, so it sees the
	// same pair of surfaces the pass structure below is built from -- and it decides nothing.
	CountContainmentBreaks(fb_id, z_id, z_used);

	// The read window this draw would sample, resolved before its pass is chosen.
	//
	// It is a pure function of the draw's own registers and its vertex trace -- the size-fixed TEX0,
	// which of the three address geometries the format belongs to, and the guest pages the whole
	// window covers -- so asking it early costs nothing and loses nothing. What it buys is that
	// "which GS pages does this draw READ" is answerable before any decision that depends on the
	// pass, which is what a draw's run has to be chosen from. The texture block far below takes these
	// rather than deriving them a second time.
	bool tex_mip = false;
	GIFRegTEX0 tex0 = {};
	u32 tex_psm = 0;
	int d32_fmt = -1, idx_fmt = -1, d16_fmt = -1;
	bool direct32 = false, direct32_depth = false, paletted = false, direct16 = false;
	bool tex_sampleable = false; ///< TME on, and the format is one of the three this stage samples
	u32 win_tw = 0, win_th = 0;
	GSPageBitmap tex_pages;
	if (PRIM->TME)
	{
		tex_mip = IsMipMapActive();
		tex0 = ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), tex_mip);
		tex_psm = tex0.PSM;
		// The direct 32-bit families: the CT32 pair, and their depth twins PSMZ32/PSMZ24, which are
		// the same tables under one constant block XOR. Its own list beside the other two, and all
		// three feed the single index_format numbering the fragment shader switches on.
		d32_fmt = GSTileSwizzleForms::Direct32FormatFor(tex_psm);
		direct32 = (d32_fmt >= 0);
		direct32_depth = (d32_fmt == static_cast<int>(GSTileSwizzleForms::Direct32Format::Z32));
		// Every format whose texel is a palette index: PSMT8/PSMT4 and the three alpha-byte views.
		// IndexFormatFor is the one list -- the donor road's reinterpretation already keyed off it,
		// and a second list here is how the two would come to disagree about what is sampleable.
		idx_fmt = GSTileSwizzleForms::IndexFormatFor(tex_psm);
		paletted = (idx_fmt >= 0);
		// ...and the direct 16-bit families, whose texel is one halfword expanded by TEXA. Its own
		// list beside that one, and both feed the single index_format numbering the fragment shader
		// switches on.
		d16_fmt = GSTileSwizzleForms::Direct16FormatFor(tex_psm);
		direct16 = (d16_fmt >= 0);
		tex_sampleable = direct32 || paletted || direct16;
		if (tex_sampleable)
		{
			win_tw = 1u << std::min<u32>(tex0.TW, 10);
			win_th = 1u << std::min<u32>(tex0.TH, 10);
			// The whole (size-fixed) texture under its own layout, page-granular. Deliberately the
			// window and not the texels the coordinates reach: the window is what the sampler is free
			// to fetch.
			const GSTileSurfaceLayout tex_l{tex0.TBP0, static_cast<u8>(tex0.TBW), static_cast<u8>(tex_psm),
				KindForPsm(tex_psm)};
			tex_pages = GSVramModel::PagesForRect(
				tex_l, GSVector4i(0, 0, static_cast<int>(win_tw), static_cast<int>(win_th)));
		}
	}

	// Which pass is open for this draw? A draw whose key differs from the open pass's starts a new
	// pass, and nothing the earlier draws did constrains its prep ops. Same key type the plan build
	// groups on (gsTileGpuPassKeyFor), which is what keeps the two in step.
	// `z_used` is the DRAW's, so the depth ATTACHMENT is present for both halves of a split -- a
	// render pass cannot gain or lose one -- while the write-enable is the principal's alone. The
	// three depth-carrying variants are per indirect run, so the two halves differing there cuts a
	// run. Under EmuCore/GS/TileGpuSplitSharesPassKey that is ALL it cuts; with the key off it also
	// cuts a pass wherever the device asked for depth-uniform passes.
	const GSDevice::GSTileGpuDepthMode depth_mode = gsTileGpuDepthModeFor(z_used, z_test, draw_z_write);
	// ...and the variant this draw's PASS is keyed on, which is the same one unless the split took
	// the depth write off the principal. Then the pass keys on the DRAW's own write and the two
	// halves come out equal, so the split cuts a run and not a pass. See gsTileGpuPassDepthModeFor.
	const GSDevice::GSTileGpuDepthMode pass_depth_mode =
		gsTileGpuPassDepthModeFor(z_used, z_test, z_write, draw_z_write, m_split_shares_pass_key);
	// The budget is charged HERE rather than where the classes were worked out, because a draw the
	// surface pool refused above never becomes a pass and must not be priced as one. The group it is
	// charged within is this same key with the reader dimension left out -- that dimension is what the
	// budget decides, so measuring inside it would measure the budget's own output.
	ChargeDeclaringBudget(wanted_classes,
		gsTileGpuPassKeyFor(fb_id, z_id, pass_depth_mode, m_depth_uniform_passes, false, m_segregate_self_read));
	const GSTileGpuPassKey draw_key = gsTileGpuPassKeyFor(
		fb_id, z_id, pass_depth_mode, m_depth_uniform_passes, draw_self_mask != 0, m_segregate_self_read);

	// This draw's guest-page footprints. WRITE is what lands in GS memory: the colour footprint where
	// any channel survives the masks and the folds, the depth footprint where the depth write
	// survives them. READ is what it may fetch: the depth footprint where it TESTS depth, the whole
	// composed texture window, and the pages a device-held palette was loaded from.
	//
	// Built HERE and not at the plan entry it is stored on, because the reorder's admission test is
	// asked of both sets and it has to be asked before any prep op of this draw exists.
	GSPageBitmap draw_write_pages, draw_read_pages;
	if (color_written)
		draw_write_pages |= fb_pages;
	if (z_write)
		draw_write_pages |= z_pages;
	if (z_test)
		draw_read_pages |= z_pages;
	if (tex_sampleable)
		draw_read_pages |= tex_pages;
	if (m_reorder_setting != 0)
		draw_read_pages |= LiveClutPages();
	// Feedback, asked here because this is the one place the two sets are still apart. Against the
	// WRITE set and not against the read set: a draw that tests depth and writes it has z_pages on
	// both sides and is not sampling anything.
	const bool tex_reads_own_target = tex_sampleable && tex_pages.intersects(draw_write_pages);

	// Which RUN this draw joins, and it is settled here for the same reason the pass key is: below
	// this point the draw emits prep ops, takes a rule-2 slot, and has its writeback and donor hoists
	// tested -- all against the pass it lands in. A run picked after any of that would test them
	// against the wrong one, and a wrong hoist is stale bytes rather than a fault.
	//
	// A draw the class test refuses gets a barrier run: the backlog goes out and it opens a run of its
	// own, so nothing crosses it in either direction. Three of the four refusals are conservatism with
	// the proof already written -- a DATE draw's snapshot difference is unobservable under its own
	// rect, and declaring is pixel-inert for a pass's non-readers -- but they cost one pass apiece on
	// this corpus and the first shipping rung should not rest on the proofs.
	if (m_reorder_active)
	{
		m_frame.reorder_draws++;
		u32 emitted = 0;
		u32 joined = 0;
		if (draw_self_mask != 0 || date != 0 || tex_reads_own_target)
		{
			if (draw_self_mask != 0)
				m_frame.reorder_ref_roaa++;
			else if (date != 0)
				m_frame.reorder_ref_date++;
			else
				m_frame.reorder_ref_feedback++;
			joined = m_reorder.OpenBarrierRun(draw_key, &emitted);
		}
		else
		{
			const GSTileGpuReorderScheduler::Verdict v =
				m_reorder.Admit(draw_key, draw_write_pages, draw_read_pages);
			m_frame.reorder_haz_raw += (v.hazards & GSTileGpuReorderScheduler::kHazardRaw) ? 1u : 0u;
			m_frame.reorder_haz_war += (v.hazards & GSTileGpuReorderScheduler::kHazardWar) ? 1u : 0u;
			m_frame.reorder_haz_waw += (v.hazards & GSTileGpuReorderScheduler::kHazardWaw) ? 1u : 0u;
			m_frame.reorder_haz_rar += (v.hazards & GSTileGpuReorderScheduler::kHazardRar) ? 1u : 0u;
			m_frame.reorder_admitted++;
			joined = v.run;
			emitted = v.runs_emitted;
		}
		m_frame.reorder_runs += emitted;
		CarryOpenPassAcrossFlush(emitted);
		m_open_run = joined;
	}

	// The cap sits in the same test as the key because it answers the same question -- this draw does
	// not join the open pass -- and it is placed HERE, before any prep op of this draw is emitted, for
	// the reason a key mismatch is: everything below reads the open pass to decide what may be hoisted
	// into it and which rule-2 slot this draw takes, and a break decided after those reads would leave
	// the plan build's pass and accumulation's pass disagreeing about both. Mirrored in
	// BuildAndExecutePlan through gsTileGpuPassEnd; the two count the same draws.
	if (draw_key != CurrentRun().pass || !gsTileGpuPassAdmitsMore(CurrentRun().draw_count, m_max_pass_draws))
		BreakOpenPass();

	PendingDraw pd = {};
	pd.tex_reads_own_target = tex_reads_own_target;
	pd.tex_source = kGSTileNoSurface;
	pd.tex_slot = GSDevice::GSTileGpuPassPlan::kNoTexSlot;
	pd.src_slot = kNoSourceSlot;
	pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());

	// Fog. The GS walks the fragment's RGB toward FOGCOL by the per-vertex fog factor F (RGB only,
	// alpha untouched); the vertex stream already carries F, so the draw needs only the enable and
	// the colour. Sprites take F from the second vertex like everything else flat about them.
	pd.fge = PRIM->FGE;
	pd.fogcol = static_cast<u32>(m_env.FOGCOL.FCR) | (static_cast<u32>(m_env.FOGCOL.FCG) << 8) |
				(static_cast<u32>(m_env.FOGCOL.FCB) << 16);

	// The alpha test, as the plan resolved it above. A test the fold decided never reaches the
	// fragment stage: every fragment takes the same road, so the write flags already carry it (an
	// all-fail draw's AFAIL decides what it lands; an all-pass one writes everything). A test that
	// varies is emitted only where the two sides of it land differently -- AFAIL=FB_ONLY keeps the
	// whole colour and drops only the depth write, so a draw that writes no depth is unaffected by
	// its own test, which is most of a game's blended geometry -- and where they DO land
	// differently, gsTileGpuPlanAlphaSplit has already decided whether this draw is the whole of
	// itself or the first half of a channel or fragment split. Its comparison rides the row as
	// ATST + 1, with zero meaning no test at all.
	if (split.pass[0].atst != ATST_ALWAYS)
	{
		pd.atst = static_cast<u32>(split.pass[0].atst) + 1;
		pd.aref = split.pass[0].aref;
	}
	pd.color_mask = draw_color_mask;

	// Texture inputs. Three address geometries are sampled at this stage: the CT32 one (PSMCT32 and
	// PSMCT24 as direct colour, their depth twins PSMZ32/PSMZ24 under one block XOR, and the
	// alpha-byte views PSMT8H/PSMT4HL/PSMT4HH as indices in the top bits of the same words), the
	// PSMT8/PSMT4 one (128-texel pages, a swizzled index into an expanded CLUT), and the 16-bit one
	// (64x64-texel pages of 16x8 blocks, two texels to a word -- PSMCT16/PSMCT16S and their depth
	// twins PSMZ16/PSMZ16S, which are the same tables under one block XOR). That is EVERY format the
	// GS can name in TEX0.PSM. The fixed TEX0 folds TW/TH to the
	// used ST range, exactly as ObserveDraw derives its footprint, so the dimensions the shader
	// scales by match the draw.
	//
	// ⚠️ Admitting a depth format here is a statement about the ADDRESS, not about whether the bytes
	// under it are current. A window a live DEPTH surface owns has no writeback road, so the ring
	// hands this road the CPU shadow's bytes for those pages and the read is stale -- counted lossy
	// by the model, and rowed in the deferred-accuracy ledger. Sampling the right bytes staleley
	// beats sampling nothing, which is what the vertex-colour fallback did.
	pd.tex_enable = false;
	pd.index_format = 0;
	pd.pal_offset = 0;
	// Did this draw's read window get composed into the ring? Rule 2 and the donor road both leave it
	// uncomposed -- they read an image, not bytes -- and the open pass's read set has to say so, or a
	// later writeback would test itself against a slot nothing read.
	bool composed_tex = false;
	if (PRIM->TME)
	{
		// tex0, the format arms and the window's pages were all resolved above, before the pass was
		// chosen. Everything from here on is what the read COSTS, which is a question about the pass.
		const u32 psm = tex_psm;
		if (tex_sampleable)
		{
			m_frame.tex_draws++;
			if (paletted && GSLocalMemory::m_psm[psm].pgs.x == GSLocalMemory::m_psm[PSMCT32].pgs.x)
				m_frame.tex_hi_draws++;
			pd.tex_enable = true;
			pd.fst = PRIM->FST;
			pd.tbp0 = tex0.TBP0;
			// Pages per texture row, GSOffset's bw >> (pageShiftX - 6): a 64-texel-wide page (the
			// CT32 family, which the alpha-byte views share) takes TBW, a 128-wide one (PSMT8/PSMT4)
			// halves it. ⚠️ Decided by page WIDTH, never by "is it paletted" -- PSMT8H is both
			// paletted and 64 wide, and halving its TBW would address the wrong page in every row
			// but the first.
			pd.tbw = TextureWindowBwPg(psm, tex0.TBW);
			pd.tw = win_tw;
			pd.th = win_th;
			pd.tfx = tex0.TFX;
			pd.tcc = tex0.TCC;
			pd.wms = ctx->CLAMP.WMS;
			pd.wmt = ctx->CLAMP.WMT;
			// The REGION wrap modes address a sub-rect of a shared texture page; the pair means
			// min/max under REGION_CLAMP and mask/or under REGION_REPEAT. GetSizeFixedTEX0 has
			// already extended TW/TH to cover whatever the region reaches.
			pd.region_u = static_cast<u32>(ctx->CLAMP.MINU) | (static_cast<u32>(ctx->CLAMP.MAXU) << 16);
			pd.region_v = static_cast<u32>(ctx->CLAMP.MINV) | (static_cast<u32>(ctx->CLAMP.MAXV) << 16);
			// LINEAR or NEAREST, from the same per-draw decision the software floor and the HW
			// renderer take: the vertex trace resolves TEX1's MMAG/MMIN against the LOD range the
			// primitive actually spans, and applies the user's filtering override. A crossover
			// primitive (the two filters differ and the LOD crossing falls inside it) takes one
			// filter for the whole draw here, which is what IsLinear already picked.
			pd.ltf = m_vt.IsLinear();
			// The state row's index_format: 0 for a direct 32-bit texel in the CT32 block space, 10
			// for one in the DEPTH block space, else the shader's format number plus one (1 = PSMT8,
			// 2 = PSMT4, 3 = PSMT8H, 4 = PSMT4HL, 5 = PSMT4HH) or plus six for a 16-bit halfword. The
			// numberings are pinned together in the gs suite -- nothing links this file to the GLSL,
			// and a renumbering that moved one and not the other would sample PSMT4HL's nibble out of
			// a PSMT8H texture.
			pd.index_format = direct32 ? (direct32_depth ? 10u : 0u) :
									     (paletted ? static_cast<u32>(idx_fmt) + 1u :
													 static_cast<u32>(d16_fmt) + 6u);
			// A 24-bit texture's texels carry no alpha byte: TEXA supplies it (and AEM makes an
			// all-zero RGB texel transparent). BOTH 24-bit formats take that rule, PSMZ24 exactly
			// as PSMCT24 -- it is about the word's missing top byte, and the depth twin is missing
			// the same one. A 16-bit texel has an alpha BIT, so TEXA selects between TA1 and TA0
			// by it, and AEM makes an all-zero CELL transparent -- a different rule, which is why
			// the fragment stage gives that road its own arm rather than sharing tilegpu_texa. A
			// paletted texture takes TEXA in the CLUT expansion.
			const bool bits24 = (psm == PSMCT24 || psm == PSMZ24);
			const u32 texa_common = 1u | (m_env.TEXA.AEM ? 2u : 0u) | (static_cast<u32>(m_env.TEXA.TA0) << 8);
			pd.texa = bits24 ? texa_common :
					  direct16 ? (texa_common | (static_cast<u32>(m_env.TEXA.TA1) << 16)) :
								 0u;
			if (paletted)
			{
				// Where this draw's palette words are. The mirror answers for the CLUT slots the draw
				// reads: the CPU's RAM as always, or ONE device palette a gathered load put there --
				// and then no CPU words cross into the frame's stream at all, and the road decision
				// below is what says how the device serves them instead.
				//
				// ⚠️ A resolution the mirror cannot serve whole SYNCS, which reads pages back and so
				// flushes the pending plan -- and that clears the prep-op array this draw's range is
				// anchored in. Nothing has been emitted for the draw yet (the compose is below), so
				// re-anchoring is the whole repair, but it has to happen.
				const u32 pal_entries = GSLocalMemory::m_psm[psm].pal;
				bool clut_synced = false;
				u32 first_texel = 0;
				GpuPalette* const gp = ResolveDrawPalette(tex0, pal_entries, first_texel, clut_synced);
				if (clut_synced) [[unlikely]]
				{
					pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());
					// ...and the plan the flush built took every staged run with it, so the run this
					// draw was admitted to is gone. It re-joins an empty scheduler, which is run zero,
					// and it is the first draw of the new plan either way.
					if (m_reorder_active)
						m_open_run = m_reorder.OpenRun(draw_key);
				}
				if (gp)
				{
					pd.pal_record = gp->handle;
					pd.pal_bias = first_texel;
					pd.pal_id = gp->pal_id;
				}
				else
				{
					// Expand this draw's CLUT to 32-bit RGBA (CSA/CPSM/TEXA applied by Read32) and
					// append it to the frame's palette stream; pal_offset is the word index of entry 0.
					// No dedup yet (wrong-fast): a palette is 16 or 256 words, a full SotC frame well
					// under the ring.
					m_mem.m_clut.Read32(tex0, m_env.TEXA);
					const u32* clut = m_mem.m_clut;
					pd.pal_offset = static_cast<u32>(m_plan_palettes.size());
					m_plan_palettes.insert(m_plan_palettes.end(), clut, clut + pal_entries);
					const u64 pal_content = GSTilePaletteCache::ContentId(clut, pal_entries);
					pd.pal_id = ClutCpuPalId(pal_content);
					if (m_census_palette) [[unlikely]]
						CensusCpuPalette(m_mem.m_clut.GetReadGeneration(), pal_entries, pal_content);
				}
			}

			// -- the palette cycle -------------------------------------------------------------
			//
			// Everything the signature is stated over is now in hand: the palette this draw samples
			// is resolved, and its write footprint and the live records' source pages were built
			// above. Asked HERE, before the compose and before any claim, because this is the last
			// point at which the draw has cost nothing -- one line further down and the read window
			// is written back into the ring, which is most of what the elision exists to avoid.
			//
			// Counted on both arms of the lever and latched on both, so a run's census can be read
			// off a build with the lever off; only the ELISION below is gated. A funnel and not a
			// single number: "the detector did not fire" is several different diagnoses and a
			// draw-call count cannot tell them apart -- which is exactly how the first shape of
			// this signature was found to be refusing its whole population.
			GSTilePaletteCycleDraw pcyc = {};
			pcyc.pal_on_device = (pd.pal_record != 0);
			pcyc.paletted_index = (tex_psm == PSMT8 || tex_psm == PSMT4);
			pcyc.sprite = is_sprite;
			pcyc.fbmsk = ctx->FRAME.FBMSK;
			if (pcyc.pal_on_device)
			{
				m_frame.pcyc_pal++;
				if (const GpuPalette* const rec = FindGpuPalette(pd.pal_record))
					pcyc.pal_owner = rec->owner;
				// S1, asked with NoteClutSourceWritten's own predicate so the two cannot disagree
				// about what "this draw destroys that palette" means -- this is the same draw that
				// will run it a few hundred lines below.
				pcyc.destroys_pal_source = (FindClutSourceWrittenBy(fb_id, fb_pages) != nullptr);
				if (pcyc.destroys_pal_source)
					m_frame.pcyc_loop++;
				if (pcyc.paletted_index && pcyc.sprite)
					m_frame.pcyc_shape++;
				else if (!pcyc.paletted_index)
					m_frame.pcyc_fail_fmt++;
				else
					m_frame.pcyc_fail_prim++;
			}
			const GSTilePaletteCycleVerdict pcyc_verdict = m_palette_cycle.Observe(pcyc);
			switch (pcyc_verdict)
			{
				case GSTilePaletteCycleVerdict::Building:
					m_frame.pcyc_building++;
					break;
				case GSTilePaletteCycleVerdict::Arm:
					m_frame.pcyc_runs++;
					if (gsTilePaletteCycleIsAlphaSplit(m_palette_cycle.fbmsk()))
						m_frame.pcyc_runs_alpha++;
					break;
				default:
					break;
			}

			// -- the elision -------------------------------------------------------------------
			//
			// ⚠️ THE LOAD-BEARING PART, and the one hard difference from Classic. Classic's skip is
			// free: the draw never reaches the texture cache and the damage, if the detector is
			// wrong, is confined to a render target. TileGpu's page model is a BYTE-TRUTH LEDGER --
			// it names, per page and per plane, which surface holds the newest bytes -- and the
			// writeback road pushes those bytes into guest VRAM, where the EE reads them. Getting
			// the ledger wrong here does not move a pixel, it moves GUEST MEMORY.
			//
			// THE RULE IS: THE LEDGER IS TOLD WHAT ACTUALLY GETS WRITTEN. An elided draw with no
			// substitute behind it writes nothing, so it claims nothing, bumps nothing and destroys
			// no palette -- which is not a shortcut, it is the only sound answer. "The draw did not
			// happen" is exactly what the model then says, and the pages keep the holder whose
			// bytes really are the newest.
			//
			// ⚠️ THE DESIGN SHEET ASKED FOR THE OPPOSITE (claim the pages, bump the version, run
			// NoteClutSourceWritten) and it is wrong, in a way that is worth writing down because
			// the reasoning looks right. Those three are what the RUN would have done to memory --
			// but only if something replaces its pixels. Claiming pages nothing wrote makes this
			// surface the model's answer for bytes another surface genuinely holds, which is a
			// LOSS of truth and cannot be recovered; and NoteClutSourceWritten marks a palette
			// record destroyed by a draw that destroyed nothing, which reserves ring words and
			// emits a block copy nothing owns. That last one was measured: 141 orphaned copy ops a
			// frame on gt4opb, emitted into no draw's op range and so never executed, against a
			// reservation the fragment stage would have read as zeros.
			//
			// So the claims move WITH the substitute. When it lands it claims what it writes, in
			// its own plan entry, and this block stays as it is.
			// ⚠️ A RUN IS ONLY ELIDED IF ITS SUBSTITUTE WAS BUILT. Dropping one with nothing in its
			// place is the arrangement that loses a lot of picture silently -- measured, before the
			// substitute existed: gt4opb 13.5 levels of mean difference against Classic and gt4 9.9,
			// both away. So a refusal latches the run into "drawn" and this draw goes on down the
			// ordinary road exactly as it would have.
			bool pcyc_elide = m_palette_cycle_hle && pcyc_verdict == GSTilePaletteCycleVerdict::Elide;
			if (m_palette_cycle_hle && pcyc_verdict == GSTilePaletteCycleVerdict::Arm)
			{
				pcyc_elide = IssuePaletteCycleSubstitute(pd, fb_id, r, tex0);
				if (!pcyc_elide)
				{
					m_palette_cycle.Refuse();
					m_frame.pcyc_refused++;
				}
			}
			if (pcyc_elide)
			{
				m_frame.pcyc_elided++;
				m_frame.pcyc_elided_pages += fb_pages.count();
				return;
			}

			// The read window (resolved above) is composed into the ring for this epoch -- prefilled
			// from S where CPU-newest, written back from any target holding it newest -- BEFORE this
			// draw's own claims move anything, because a draw sampling pages it also renders reads
			// the pre-pass bytes: snapshot semantics.
			const GSTileSurfaceLayout tex_l{tex0.TBP0, static_cast<u8>(tex0.TBW), static_cast<u8>(psm), KindForPsm(psm)};

			// Rule 2 first. When one resident target owns the whole window at this exact layout the
			// fragment stage samples it directly, and the byte road is not asked for anything: no
			// ring slot, no writeback, no epoch. That is where the structural saving is -- a
			// composite reading last frame's 3D buffer stops reswizzling a megabyte per frame into
			// bytes only it will read -- and it is also where the staleness goes, because the
			// target IS the newest copy while a composed slot is only as new as its writeback.
			pd.tex_source = TargetForTextureRead(tex_l, pd.tw, pd.th, tex_pages, fb_id, z_id);
			if (pd.tex_source != kGSTileNoSurface)
			{
				PlanTargetIndex(pd.tex_source); // the executor resolves the bind through the target list
				m_frame.tex_binds++;
				// The road is decided here, so record it here: the pass's shader variant is the OR of
				// its draws' roads, and a road no draw takes is never compiled.
				pd.road_mask = GSDevice::kGSTileGpuRoadTarget;
			}
			else
			{
				pd.road_mask = GSDevice::kGSTileGpuRoadByte;

				// Rule 3, and the DONOR question comes first because it changes what the rest of this
				// block does. A window whose pages one live target owns whole can be reinterpreted
				// straight out of that target's texture, and then nothing has to be composed for it
				// at all -- no writeback of a megabyte of pixels into ring slots only this read would
				// have consumed, and no ring slots. It also supplies the window's content identity,
				// which a page fingerprint cannot: those bytes are not in the CPU shadow.
				DonorPlan donor;
				const bool donor_ok =
					m_bindless_targets && DonorForTextureRead(tex0, tex_pages, fb_id, z_id, donor);
				const GSTileTextureSource::ContentToken donor_token =
					donor_ok ? GSTileTextureSource::TargetToken(donor.owner, SurfaceVersion(donor.owner)) :
							   GSTileTextureSource::ContentToken{};
				// Counted here rather than after the admission question, deliberately: this is the
				// population ONE live target could serve whole, and every later refusal (region wrap,
				// an unadmitted palette pair, the pin) is a separate fact about the same draw. Folding
				// them together would report the road as unavailable when it is merely not reached.
				if (donor_ok)
					m_frame.src_donor_eligible++;

				// The admission question, asked BEFORE the compose and answered against the bytes as
				// they stand -- which is the only moment a page fingerprint is honest, since composing
				// supersedes exactly the bytes it would hash.
				const u32 src_window = m_bindless_targets ?
										   ProbeSourceRoad(pd, tex0, tex_pages, paletted, tex_mip, donor_token) :
										   kNoSourceSlot;

				// The donor build, tried before the compose because skipping the compose IS the win.
				// It reads the owner's image rather than the ring, so unlike a materialise it is not
				// free to hoist to the pass head: a draw already in the open pass may be what wrote
				// the pixels it is about to read, and then this draw opens its own pass -- the
				// writeback's own test and the same precedent.
				const u32 ops_before = static_cast<u32>(m_plan_prep_ops.size());
				const bool served_by_donor = donor_ok && src_window != kNoSourceSlot &&
											 MaterialiseSourceRoad(pd, src_window, tex0, tex_pages, m_epoch, &donor);
				if (served_by_donor)
				{
					pd.epoch = m_epoch;
					if (m_plan_prep_ops.size() != ops_before && DonorHoistCollides(donor.owner, tex_pages))
					{
						pd.break_before = true;
						pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::Donor);
						m_frame.src_donor_breaks++;
						BreakOpenPass();
					}
				}
				else
				{
					// The byte road, and rule 3 off the ring on top of it. The materialise reads the
					// ring at this draw's epoch, so the window's slots have to exist and any writeback
					// composing them has to be queued first: prep ops run at the pass head in array
					// order, and the compose below emits its writebacks before the build. A donor that
					// was offered and then refused downstream lands here too, which is why the compose
					// is on this side of the branch and not above it.
					ComposeForPendingDraw(tex_pages, pd);
					composed_tex = true;
					pd.epoch = m_epoch;
					// A paletted window goes the same way in two stages -- index image, then the
					// palette expansion on top of it -- and both ops are queued from in there, in that
					// order.
					if (src_window != kNoSourceSlot)
						MaterialiseSourceRoad(pd, src_window, tex0, tex_pages, pd.epoch);
				}
			}

			// The gathered palette's words, once the road is known. A rule-3 draw samples the N x 1
			// gather through the expansion and needs nothing here; a BYTE-road one reads its palette out
			// of the frame's stream, and gets it from one image-to-buffer copy of the palette's blocks --
			// not from a gather pass, because at six hundred to twelve hundred CLUT loads a frame a pass
			// each would double the pass count this whole design rests on. A copy lands texels row-major,
			// so the state row carries the mode the fragment stage applies the CSM1 entry order in.
			if (pd.pal_record != 0) [[unlikely]]
			{
				if (pd.road_mask == GSDevice::kGSTileGpuRoadSource)
				{
					m_frame.clut_draws_r3++;
				}
				else
				{
					GpuPalette* const pal_rec = FindGpuPalette(pd.pal_record);
					if (pal_rec && EnsureClutStreamWords(*pal_rec, pd))
					{
						pd.pal_offset = pal_rec->stream_offset;
						// The copy's own layout decides the fetch mode and the two stride forms the
						// arm folds into bit positions -- a merged 32-bit copy landed the words as one
						// tile, an unmerged one as a run of row-major blocks, and a SIXTEEN-BIT copy
						// as a tile of cells that is a different entry order again. All three in one
						// function (gsTileGpuClutPalRow) because the shader has one switch over them.
						const GSTileGpuPalRow row =
							gsTileGpuClutPalRow(pal_rec->stream_ct16, pal_rec->entries, pal_rec->stream_stride);
						pd.pal_mode = row.mode;
						pd.pal_mul = row.mul;
						pd.pal_shift = row.shift;
						m_frame.clut_draws_byte++;
					}
					else
					{
						// The device cannot serve the words after all -- the source died between the resolve
						// and here, or the forms stopped fitting. Retire the record and take the CPU road,
						// which is where every un-gathered draw already is.
						if (pal_rec)
							RetireLostClutRecord(*pal_rec);
						pd.pal_record = 0;
						pd.pal_bias = 0;
						pd.pal_mode = 0;
						pd.pal_mul = 0;
						pd.pal_shift = 0;
						m_mem.m_clut.Read32(tex0, m_env.TEXA);
						const u32* const clut = m_mem.m_clut;
						pd.pal_offset = static_cast<u32>(m_plan_palettes.size());
						m_plan_palettes.insert(m_plan_palettes.end(), clut, clut + GSLocalMemory::m_psm[psm].pal);
					}
				}
			}
		}
		else
		{
			// Nothing is left: every PSM the GS can name as a texture is now one of direct-32,
			// paletted, or direct-16. Kept as a counter rather than an assert because TEX0.PSM is
			// six raw bits and a game can write a reserved one, and the answer to that is a
			// vertex-colour draw and a number, not a crash.
			m_frame.tex_unsupported++;
		}
	}

	// Seeds: pages the target textures do not hold newest are filled out of the ring before this
	// draw's pass, so the draw opens a new pass. Only colour surfaces have a road; a depth
	// surface's missing pages stay whatever its texture held (lossy, counted).
	// A colour draw claims every colour plane and the Z plane on its pages (Tile's rule: the
	// written bytes invalidate any depth reading, and the surface holds the newest bytes of the
	// whole cell once seeded across the span). The seed gate uses the colour span -- minus the
	// pages this draw provably overwrites in full: a single sprite that writes every fragment
	// (no test can reject one, no blend reads the destination, every channel lands) covers each
	// page inside its rect entirely, so those pages need no bytes brought in first. This is what
	// keeps SotC's page-column clears (and every game's frame clear) from re-seeding what they
	// are about to overwrite.
	//
	// ⚠️ "Every channel lands" is gsTileFrameWriteIsTotal, NOT color_mask == RGBA. A channel the
	// mask reports as written can still be preserving bits inside its byte, and a masked draw
	// leaves the rest of the cell holding whatever the texture had -- which is exactly what the
	// seed is for. Only a draw that masks nothing at all may skip it.
	//
	// "No test can reject one" is gsTileColorLandsOnEveryFragment, which reads the SAME fold the
	// write flags did: no alpha test, one that provably passes, or one that provably fails in an
	// AFAIL mode that still writes the frame buffer. Both halves of the fold only strengthen this
	// -- an all-pass draw is as total as ATST=ALWAYS, and an all-fail one is total exactly where
	// ATST=NEVER already was.
	const u8 fb_claims = kGSTilePlanesAll;
	if (color_written)
	{
		GSPageBitmap seed = PagesNeedingSeed(fb_id, fb_pages, kGSTilePlanesColor);
		// Plus whatever of this surface's RESIDENCY has never been filled. The byte model would say
		// those pages are fine -- their bytes live in the CPU shadow -- but the present reads the
		// texture, so an unfilled page reaches the screen as allocator leftovers. Paid once per
		// surface: after this the whole residency is materialised.
		//
		// Residency, not the texture rectangle, and they are not the same set. The pool texture is
		// always bw pages wide and as tall as the highest page row any rect has reached, while the
		// residency is the union of the rects themselves -- so a surface whose views are narrower
		// than its stride, or whose tall view is a thin strip, carries a gap between the two. No
		// view ever claimed a page in that gap, so nothing owns it, nothing samples it, nothing
		// reads it back and nothing presents it: the display's own rect is claimed by
		// MaterialiseDisplayBuffers, which grows residency over it and seeds it explicitly before
		// the plan is built. Seeding the gap costs a writeback and a seed page apiece for pages
		// nobody is going to look at.
		//
		// It matters more than it did. Surface-identity containment makes the gap the size of the
		// container: fold a 65x33 palette buffer into the frame buffer 17 page-rows below it and
		// the texture rectangle grows by every page between the two bases while the residency grows
		// by two. Seeding the rectangle would put a full-image seed and hundreds of writebacks on
		// the frame each container grows.
		seed |= Texels(fb_id).filled.MissingFrom(Texels(fb_id).pages & m_vram_model.Get(fb_id).residency);
		GSPageBitmap covered;
		if (!seed.empty() && m_vt.m_primclass == GS_SPRITE_CLASS && icount == 2 && !PRIM->ABE && date == 0 && !z_test &&
			gsTileFrameWriteIsTotal(ctx->FRAME.FBMSK, fb_fmsk) &&
			gsTileColorLandsOnEveryFragment(atst_fold, afail))
		{
			GSVramModel::RectFootprint fp;
			GSVramModel::FootprintForRect(fb_l, r, fp);
			if (!fp.overflowed)
			{
				covered = fp.pages;
				for (u32 e = 0; e < fp.edge_count; e++)
				{
					if (fp.edges[e].full != GSVramModel::kFullBlockMask)
						covered.unset(fp.edges[e].page);
				}
				// Whatever another surface held on a fully covered page is dead bytes: mark it
				// synced without moving anything, so the claim below can take the page.
				m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(seed & covered, kGSTilePlanesAll));
				seed = seed.andnot(covered);
				// The same proof, per block, for the pages this draw covers only in PART. A page
				// still needs a whole-page seed before it can be skipped -- the seed shader has no
				// block mask -- but its texels are as real in the blocks the rect covers in full
				// as they are on a page it covers entirely, and recording them is what keeps the
				// texture bookkeeping in the model's own units. Edge blocks (`blocks & ~full`) are
				// NOT recorded: the rect enters them, so the fragments outside it keep whatever
				// the texture had.
				for (u32 e = 0; e < fp.edge_count; e++)
					Texels(fb_id).filled.Mark(fp.edges[e].page, fp.edges[e].full);
			}
		}
		if (!seed.empty())
		{
			if (HasByteRoad(fb_l))
			{
				// Whatever holds those pages' bytes now composes into the ring first (other
				// targets' writebacks, S prefill) and is marked synced -- which is also what lets
				// the claim below steal them losslessly. Then the seed reads the composed slots
				// into the target.
				ComposeRingPages(seed);
				EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, fb_id, seed);
				pd.break_before = true;
				pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::SeedColor);
				m_frame.seed_breaks++;
				BreakOpenPass();
				Texels(fb_id).filled.MarkWhole(seed);
			}
			else
			{
				LossySteal(seed, GSTileSurfaceKind::Color);
			}
		}
		// A page this draw covers in full ends up holding texels whether it was seeded or not.
		Texels(fb_id).filled.MarkWhole(covered);
	}
	u8 z_claims = 0;
	if (z_used)
	{
		// The depth texture should hold this footprint's newest Z for a test or a write, and the
		// pages it does not hold have to be brought in from the bytes -- the same seed the colour
		// branch above runs, into the depth attachment. Whatever holds those pages' bytes composes
		// into the ring first (other targets' writebacks, the S prefill), which is also the spill a
		// road-having surface has always owed the depth claim below.
		//
		// ⚠️ This used to mark the whole set synced and move nothing, which is a LIE the model then
		// tells every later reader of those pages: "the ring has these bytes", and the reader
		// samples whatever the CPU shadow's prefill left. It stayed invisible only while some other
		// read of the same pages happened to write them back first; removing one such read (the
		// resident-target bind) is how FlatOut 2 surfaced it.
		//
		// ⚠️ And it then composed the bytes and CONSUMED NOTHING, because a depth surface had no seed
		// program. Since targets became persistent that is not a stale page, it is an ACCUMULATING
		// one: the image keeps every earlier frame's GEQUAL high-water mark, and the scene fails the
		// depth test against it. Beyond Good & Evil clears its Z buffer by drawing zero-coloured
		// pixels over the Z buffer's own address as a PSMCT32 FRAME, so the Z plane of those pages is
		// invalidated, the bytes are right there, and 18,054 pixels of frame 3 came out black.
		//
		// The split is the whole correctness story, and it is what separates this from the blanket
		// "clear the depth image whenever any page is unservable" that healed BGE and broke OutRun.
		// `z_seed` names the pages the IMAGE does not hold newest; of those, PagesDepthSeedable takes
		// only the ones whose bytes a compose really can serve AND whose truth is held through this Z
		// buffer's own pixel space. Everything it refuses keeps what the image has, which is exactly
		// what every depth page got before this road existed, so a refusal can only ever be neutral:
		//
		//   - a page another DEPTH surface holds: no writeback shader, so the ring would carry the
		//     CPU shadow's stale bytes;
		//   - a page THIS surface holds only in part: same, and the blocks it does hold would be
		//     overwritten by stale ones;
		//   - a page whose truth a colour surface holds through a FOREIGN view -- OutRun 2006 draws
		//     16-bit colour over its own Z buffer's address, so the model would hand the depth road a
		//     page of colour and the player's car would fail GEQUAL against it.
		//
		// Asked BEFORE the compose, which marks even lossy pages synced and would answer differently
		// after.
		const GSPageBitmap z_seed = PagesNeedingSeed(z_id, z_pages, GSTilePlaneZ);
		const GSPageBitmap z_fill =
			gsTileSurfaceHasDepthSeedRoad(z_l) ? PagesDepthSeedable(z_seed, z_l) : GSPageBitmap();
		// What is left is the depth gap as it now stands: pages whose Z the byte store cannot serve,
		// or serves only through a foreign view of the same memory. They keep whatever the depth image
		// holds -- exactly as every depth page did before this road existed.
		NoteLossyPages(z_seed.andnot(z_fill), GSTileSurfaceKind::Depth);
		ComposeForPendingDraw(z_seed, pd);
		if (!z_fill.empty())
		{
			EmitPrepOp(GSDevice::GSTileGpuPrepKind::SeedDepth, z_id, z_fill);
			pd.break_before = true;
			pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::SeedDepth);
			m_frame.seed_breaks++;
			BreakOpenPass();
		}
		if (z_write)
			z_claims = gsTilePlanesInvalidatedByWrite(ctx->ZBUF.PSM);
	}

	// Self-read accounting: the read window intersects this draw's own colour target's pages.
	if (pd.tex_enable && !(tex_pages & fb_pages).empty())
		m_frame.self_reads++;

	// The claims: this draw's target textures now hold the newest bytes of its footprint. A
	// depth-only draw writes no colour and claims none.
	if (color_written)
	{
		m_vram_model.OnNativeDraw(fb_id, fb_pages, fb_claims);
		// This draw's pixels land in the surface's texture, so a donor source cached off it is
		// stale from here. Deliberately whole-surface rather than per-page: the identity names the
		// image, and an over-eager bump costs a rebuild where an under-eager one costs a wrong texel.
		BumpSurfaceVersion(fb_id);
		// A palette gathered off these very pages is a different matter and is asked PER PAGE: its
		// words are not a cache that can be rebuilt, they are the only copy, and there is no cheaper
		// road to fall back to -- so the question has to be the exact one. (Only a colour surface can
		// own a palette, which is why the depth claim below asks nothing.)
		NoteClutSourceWritten(fb_id, fb_pages);
	}
	if (z_write)
	{
		// The depth claim is asked what it steals HERE, between the two claims, and not with the
		// seeds above -- because the colour claim that just ran is what creates the steal. Games
		// pack FRAME and ZBUF back to back, so the two page footprints of ONE draw routinely share
		// the boundary page (Dirge of Cerberus: FRAME pages 224-280, ZBUF starting at 280, one page
		// shared per draw), and a few alias them outright (FlatOut 2 puts a 16-page FRAME and its
		// ZBUF at the same base; GT4's online beta offsets ZBUF one page into a 126-page FRAME, so
		// 112 pages are shared). The colour claim has just made itself the holder of those pages,
		// and the depth claim takes them straight back.
		//
		// Nothing that pre-dates the draw is lost -- both claims describe this same draw's own
		// output -- but the model names one holder per page per plane, so the colour surface's
		// version of those bytes is. Marked synced without moving any (the depth surface has no
		// byte road to move them onto in any case) and counted, never silent.
		//
		// The colour claim needs no such question of its own, and the depth claim needed none
		// before the colour claim ran: every claim set that contains a colour plane also contains
		// Z and vice versa (fb_claims is all four, z_claims is gsTilePlanesInvalidatedByWrite),
		// and so does every set a CPU write clears, so no surface can come to hold one of a page's
		// planes while another holds the rest -- which is the only way the seed gates above could
		// miss a steal. Measured: the pre-claim query is empty on all 18 corpus dumps. If it ever
		// stops being, the model's synced assert says so, and the answer is a ComposeRingPages of
		// the pages it names, before the claims.
		AliasSteal(m_vram_model.SpillBeforeNativeDraw(z_id, z_pages, z_claims));
		m_vram_model.OnNativeDraw(z_id, z_pages, z_claims);
		BumpSurfaceVersion(z_id);
	}

	// A DATE draw reads the pass snapshot; if the run of draws into this surface since the last
	// break has written pixels under it, the snapshot would be stale for those, so it opens a new
	// pass. Disjoint column sprites (SotC's depth-of-field composite) share one snapshot.
	if (CurrentRun().date_surface != fb_id)
	{
		CurrentRun().date_surface = fb_id;
		CurrentRun().date_written = GSVector4i::zero();
		CurrentRun().date_cover.clear();
	}
	// ...and only on the snapshot road. A DATE draw served by the in-pass read sees the live pixel,
	// including whatever this same pass has already written under it, so there is nothing for a break
	// to make exact -- it would cost a pass and buy nothing.
	//
	// In the SURFACE's coordinates, not the view's: two views contained in one image share
	// date_surface, so the run's written rect has to be a rect of the image they share. Disjoint
	// views then simply never intersect, which is the right answer -- the snapshot the second one
	// reads genuinely does hold the first one's pixels.
	const bool date_reads_live = (self_mask & GSDevice::kGSTileGpuSelfDate) != 0;
	if (m_census_date_cover && date != 0 && !date_reads_live)
		m_frame.date_draws++;
	if (date != 0 && !date_reads_live && !CurrentRun().date_written.rempty() &&
		!CurrentRun().date_written.rintersect(sr).rempty())
	{
		// The counterfactual, before the break clears the history it reads. The rect set covers
		// everything the union covers, so it can only ever say "clear" where the union said "stale"
		// -- never the other way -- and this counts exactly the breaks the finer test would not make.
		if (m_census_date_cover && !CurrentRun().date_cover.intersects(sr))
			m_frame.date_breaks_cover_would_skip++;
		pd.break_before = true;
		pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::Date);
		CurrentRun().date_written = GSVector4i::zero();
		CurrentRun().date_cover.clear();
		m_frame.date_breaks++;
		BreakOpenPass();
	}
	if (pd.break_before)
	{
		CurrentRun().date_written = GSVector4i::zero();
		CurrentRun().date_cover.clear();
	}
	CurrentRun().date_written = CurrentRun().date_written.rempty() ? sr : CurrentRun().date_written.runion(sr);
	// The counterfactual's own history, beside the union and under the census gate. This runs on
	// EVERY draw, and a set of eight rects is not a rect: with the census off it is the one piece of
	// this instrument a frame would actually feel.
	if (m_census_date_cover)
		CurrentRun().date_cover.add(sr);
	pd.date = date;
	// The alpha keep is a per-fragment write mask, so it joins the arm that already does one.
	pd.afail_keep_alpha = afail_keep_alpha;
	pd.self_mask = draw_self_mask;
	// The equation and the mask the fragment stage would need, whether or not this draw was admitted:
	// two shifts and an AND, and a field that only sometimes carries a meaning is a field that gets
	// read in the wrong state eventually.
	// The draw's own alpha never passes 0x80, so the As blend factor -- min(As * 255/128, 1) -- does
	// not reach its clamp anywhere in this draw. That is the classifier's ReaderAsDualSource
	// condition and nothing else, kept here because the dual-source-free road needs the same fact:
	// a factor that does not clamp is a factor an alpha channel can carry and give back.
	pd.as_unclamped = (reader_flags & GSTilePassSim::ReaderAsDualSource) != 0;
	pd.self_blend = gsTileGpuPackBlend(PRIM->ABE && color_written, ctx->ALPHA.A, ctx->ALPHA.B, ctx->ALPHA.C,
		ctx->ALPHA.D, ctx->ALPHA.FIX, m_draw_env->COLCLAMP.CLAMP != 0, m_draw_env->PABE.PABE != 0,
		GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt);
	pd.self_fbmsk = gsTileGpuFrameKeepMask(ctx->FRAME.FBMSK, fb_fmsk);
	// ...plus, where the lever moved the channel mask off the pipeline, the channels the pipeline was
	// dropping -- as WHOLE keep bytes, because that is what it was preserving. Unioned rather than
	// substituted: a channel the register masks in part is still written and still owes those bits
	// back. The AFAIL fold is inside color_mask, so an all-fail RGB_ONLY draw's alpha is kept here too.
	pd.mask_in_shader = mask_in_shader;
	if (mask_in_shader)
		pd.self_fbmsk |= gsTileGpuChannelKeepMask(draw_color_mask);
	// What a 16-bit frame stores, truncated in the fragment stage wherever the fragment stage's
	// output is what lands (gsTileGpuQuantisesOnWrite). A draw whose blend the executor's blend unit
	// still has to run is quantised by the console after that blend, so it takes nothing here --
	// which is where the refused 16-bit blend class lands on a device that taxes declaring.
	pd.quantise_5551 = gsTileGpuQuantisesOnWrite(gsTileGpuFrameQuantises(GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt),
		blend_active, shader_blend);
	m_frame.self_read_draws += (self_mask != 0) ? 1u : 0u;
	// The write mask's census, counted on BOTH arms -- the population is what says whether this title
	// is worth a device round for the lever, and a counter that exists only in the arm that moved
	// cannot answer that. `servable` asks the same predicate with the lever forced on, so the three
	// read as one funnel: carried a mask -> the shader could serve it -> it did.
	if (draw_color_mask != 0 && draw_color_mask != 0xF)
	{
		m_frame.mask_partial_draws++;
		if (gsTileGpuMasksInShader(true, draw_color_mask, blend_active, shader_blend, shader_fbmsk))
			m_frame.mask_servable_draws++;
	}
	m_frame.mask_shader_draws += mask_in_shader ? 1u : 0u;

	// The pass's fragment program, asked as a pass question rather than a draw one: the program a
	// pass compiles in the union arm is the OR of its draws' arms, and two of those unions are past
	// the Adreno 650's 123-unit instruction cliff.
	//
	//  - BOTH palette entry orders in one program is 145 units (151 with the destination read). The
	//    two orders are alternatives -- a palette's words came off a 32-bit owner or a 16-bit one --
	//    so no pass has any reason to carry both, and gs_tilegpu_shader_budget_tests refuses the
	//    shape outright rather than pricing it.
	//  - The SIXTEEN-BIT order in a pass that also samples a resident target directly: 118 units at
	//    byte+target[IDX4] and 123 at byte+target+source[IDX4], past both of that gate's ceilings.
	//    The 32-bit order in the same shapes is 109 and 114 and stays.
	//
	// The answer to both is a pass SPLIT, and it has to be: by the time a draw is accumulated its
	// palette was gathered rather than read back, so there are no CPU words to fall back to and
	// "refuse the gather for this draw" is not an available move. A break costs a pass and never a
	// pixel. It goes here for the same reason the rule-2 bind below does -- this is the last point
	// in the function where the pass can still be broken -- and it runs first, so the bind's own
	// test sees the pass this draw actually lands in.
	//
	// Never fires on Spider-Man 3, the one title the 16-bit road serves on this corpus: 16 of its
	// 43,747 draws a frame take the target road, and it gathers off one owner format at a time.
	{
		OpenRun& run = CurrentRun();
		const u32 want_arm = gsTileGpuClutGatherArm(pd.pal_mode);
		const bool want_target = (pd.road_mask & GSDevice::kGSTileGpuRoadTarget) != 0;
		if (gsTileGpuClutArmBreaks(run.gather_arm, run.target_road, want_arm, want_target))
		{
			pd.break_before = true;
			pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::Clut16Arm);
			m_frame.clut16_arm_breaks++;
			// The DATE snapshot's written rect starts again at this draw, exactly as it does at the
			// bind break below and at every other break in this function.
			run.date_written = sr;
			run.date_cover.clear();
			if (m_census_date_cover)
				run.date_cover.add(sr);
			BreakOpenPass();
		}
		// ...and this draw is now in a pass -- its own, if the break above made one -- so what it
		// makes that pass carry is recorded against it. BreakOpenPass cleared both fields, so the
		// same reference is the new pass's.
		if (want_arm != 0)
			run.gather_arm = want_arm;
		run.target_road = run.target_road || want_target;
	}

	// The rule-2 bind takes its slot HERE, not where the source was chosen, because the slot
	// numbering belongs to the pass and everything above may still have broken the pass. Two things
	// can stop the bind joining the open pass, and both are answered by breaking it: the open pass
	// renders into the very surface this draw wants to sample (a pass cannot have one image as
	// attachment and texture at once), or its bind table is already full. A break is always legal --
	// it costs a pass, never correctness -- and after it the table is empty and the attachments are
	// this draw's own, which the eligibility test already excluded.
	if (pd.tex_source != kGSTileNoSurface)
	{
		OpenRun& run = CurrentRun();
		bool need_break = run.pass.color != kGSTileNoSurface &&
						  (pd.tex_source == run.pass.color ||
							  (run.pass.depth_used && pd.tex_source == run.pass.depth));
		if (!need_break)
		{
			u32 s = 0;
			while (s < run.tex_count && run.tex_src[s] != pd.tex_source)
				s++;
			need_break = (s == run.tex_count && run.tex_count == run.tex_src.size());
		}
		if (need_break)
		{
			pd.break_before = true;
			pd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::TexBind);
			m_frame.tex_bind_breaks++;
			run.date_written = sr;
			run.date_cover.clear();
			if (m_census_date_cover)
				run.date_cover.add(sr);
			BreakOpenPass();
		}
		u32 slot = 0;
		while (slot < run.tex_count && run.tex_src[slot] != pd.tex_source)
			slot++;
		if (slot == run.tex_count)
			run.tex_src[run.tex_count++] = pd.tex_source;
		pd.tex_slot = slot;
	}

	// This draw is now part of the open pass (its own, if anything above broke the previous one):
	// remember the pages it renders into and the ring pages it samples, so a later draw's
	// writeback can be tested against it. Last word in the function on pass structure -- nothing
	// below here can still break the pass.
	OpenRun& run = CurrentRun();
	run.pass = draw_key;
	// This draw is in the pass, so the cap counts it. No return stands between here and the
	// m_plan_pending push at the end of the function, so one increment is exactly one plan entry --
	// which is what makes this count and the plan build's (j - i) the same number.
	run.draw_count++;
	if (color_written)
		run.color_written |= fb_pages;
	if (z_write)
		run.z_written |= z_pages;
	if (pd.tex_enable && composed_tex)
	{
		// ComposeRingPages gave every page of the read window a live slot, and nothing between
		// there and here closes one (only an upload does, and uploads do not land mid-draw), so
		// m_ring_live still names the slot this draw's shader will read. A rule-2 draw and a
		// donor-served one read no slot at all, so they constrain no later writeback.
		run.read |= tex_pages;
		tex_pages.forEachSetPage([&](u32 page) { run.read_slot[page] = m_ring_live[page]; });
	}

	pd.color_surface = fb_id;
	pd.z_surface = z_id;
	pd.z_used = z_used;
	pd.z_write = draw_z_write;
	pd.z_test = z_test;
	pd.depth_mode = depth_mode;
	pd.pass_depth_mode = pass_depth_mode;
	// The three things that speak in the colour surface's pixel space, all translated by the
	// containment offset and all of them (0, 0) worth of translation for a view that is its own
	// surface. Doing the vertex half through XYOFFSET rather than by rewriting the vertex stream is
	// Classic's `v[i].XYZ.X += horizontal_offset << 4` one level up: cheaper, and it leaves the
	// vertices byte-identical to what the guest handed us.
	pd.ofx = gsTileContainVertexOffset(static_cast<s32>(ctx->XYOFFSET.OFX), fb_off.x);
	pd.ofy = gsTileContainVertexOffset(static_cast<s32>(ctx->XYOFFSET.OFY), fb_off.y);
	pd.rect = sr;
	pd.scissor = ctx->scissor.in + off4;
	// Whether the scissor rejects anything of THIS draw, which is not the same question as whether
	// it is narrower than the target: the bbox is what the geometry covers, r is what survives.
	// Both untranslated, because the question is about the two of them and not about where they sit.
	pd.scissor_cuts = !bbox.eq(r);
	pd.prep_op_count = static_cast<u32>(m_plan_prep_ops.size()) - pd.first_prep_op;
	// Make sure the surfaces are in the plan's target list even when no prep op named them.
	PlanTargetIndex(fb_id);
	if (z_used)
		PlanTargetIndex(z_id);

	// -- the geometry --------------------------------------------------------------------------
	GSDevice::GSTileGpuIndirectDraw draw = {};
	draw.instance_count = 1;
	draw.first_index = static_cast<u32>(m_plan_indices.size());
	draw.vertex_offset = static_cast<s32>(m_plan_vertices.size());
	draw.state_index = static_cast<u32>(m_plan_draws.size());

	if (!is_sprite)
	{
		// Triangle, line or point: the index list is already the topology the pipeline consumes
		// (3 indices per triangle, 2 per line, 1 per point). Copy vertices and indices straight.
		draw.index_count = icount;
		m_plan_vertices.insert(m_plan_vertices.end(), m_vertex->buff, m_vertex->buff + vcount);
		m_plan_indices.insert(m_plan_indices.end(), m_index->buff, m_index->buff + icount);
	}
	else
	{
		// Sprite corner synthesis. Each sprite is a pair of opposite corners (index[0], index[1]);
		// the GS draws it flat, taking colour, Q, Z and fog from the second vertex, while the
		// texture coordinates interpolate between the two corners (Classic's Lines2Sprites). Build
		// four corners off the second vertex, giving each its own XY and the matching UV / ST
		// pair — u from the corner's x side, v from its y side — and emit two triangles. Indices
		// are batch-local (corner s*4..s*4+3); the draw's vertex_offset rebases them into the
		// frame stream. No culling in the executor pipeline, so corner winding is free.
		const u16* RESTRICT index = m_index->buff;
		const GSVertex* RESTRICT verts = m_vertex->buff;
		const u32 sprite_count = icount / 2;
		draw.index_count = sprite_count * 6;
		m_plan_vertices.reserve(m_plan_vertices.size() + sprite_count * 4);
		m_plan_indices.reserve(m_plan_indices.size() + sprite_count * 6);
		for (u32 s = 0; s < sprite_count; s++)
		{
			const GSVertex& v0 = verts[index[s * 2 + 0]];
			const GSVertex& v1 = verts[index[s * 2 + 1]];

			GSVertex c[4] = {v1, v1, v1, v1};
			c[0].XYZ.X = v0.XYZ.X; c[0].XYZ.Y = v0.XYZ.Y; // top-left
			c[1].XYZ.X = v1.XYZ.X; c[1].XYZ.Y = v0.XYZ.Y; // top-right
			c[2].XYZ.X = v0.XYZ.X; c[2].XYZ.Y = v1.XYZ.Y; // bottom-left
			c[3].XYZ.X = v1.XYZ.X; c[3].XYZ.Y = v1.XYZ.Y; // bottom-right
			c[0].U = v0.U; c[0].V = v0.V; c[0].ST.S = v0.ST.S; c[0].ST.T = v0.ST.T;
			c[1].U = v1.U; c[1].V = v0.V; c[1].ST.S = v1.ST.S; c[1].ST.T = v0.ST.T;
			c[2].U = v0.U; c[2].V = v1.V; c[2].ST.S = v0.ST.S; c[2].ST.T = v1.ST.T;
			m_plan_vertices.push_back(c[0]);
			m_plan_vertices.push_back(c[1]);
			m_plan_vertices.push_back(c[2]);
			m_plan_vertices.push_back(c[3]);

			const u16 b = static_cast<u16>(s * 4);
			m_plan_indices.push_back(b + 0);
			m_plan_indices.push_back(b + 1);
			m_plan_indices.push_back(b + 2);
			m_plan_indices.push_back(b + 2);
			m_plan_indices.push_back(b + 1);
			m_plan_indices.push_back(b + 3);
		}
	}
	m_plan_draws.push_back(draw);
	m_plan_topologies.push_back(topology);
	// The blend: the GS ALPHA equation Cv = (A - B) * C + D as a fixed-function key (the executor
	// maps it through GSDevice::m_blendMap; As is the shader's dual-source alpha, FIX the blend
	// constant, Ad the target's alpha). ABE off, or A == B with D == Cs, is no blend at all.
	u32 blend_key = 0;
	bool fixed_function_blend = false;
	if (PRIM->ABE && color_written)
	{
		const GIFRegALPHA& al = ctx->ALPHA;
		const bool identity = (al.A == al.B) && (al.D == 0); // (X - X)*C + Cs == Cs
		if (!identity)
		{
			blend_key = GSDevice::GSTileGpuPassPlan::kBlendEnable | (al.A * 27u + al.B * 9u + al.C * 3u + al.D) |
						((al.C == 2) ? (static_cast<u32>(al.FIX) << 8) : 0u);
			// ...unless the fragment stage evaluates the whole equation, in which case the blend unit
			// is switched off for this draw and names no factors at all.
			fixed_function_blend = (pd.self_mask & GSDevice::kGSTileGpuSelfBlend) == 0;
		}
	}

	// The As blend factor's road, asked as though the device had no second colour output -- always,
	// on every device. That is what makes the census comparable across the two arms: a run on a
	// device that HAS dual-source still reports what the carrier roads would have cost it. Which
	// road is then APPLIED is the device's question, one gate below.
	u32 write_mask = pd.color_mask;
	bool alpha_companion = false;
	pd.dualsrc_terms = fixed_function_blend ? GSDevice::gsTileGpuDualSrcTerms(blend_key & 0x7Fu) : 0u;
	if (pd.dualsrc_terms != 0)
	{
		// The two ways the fragment stage owns the alpha byte it writes: a per-fragment AFAIL keep,
		// and an FBMSK that masks alpha in PART so the shader merges the destination's bits. Both
		// put a value in o_color.a that the carrier would overwrite.
		const bool alpha_is_the_shaders =
			pd.afail_keep_alpha || ((pd.self_mask & GSDevice::kGSTileGpuSelfMask) != 0 &&
									   (pd.self_fbmsk & 0xFF000000u) != 0);
		pd.dualsrc_road = static_cast<u8>(GSDevice::gsTileGpuDualSrcRoad(
			pd.dualsrc_terms, false, (pd.color_mask & 0x8u) != 0, pd.as_unclamped, alpha_is_the_shaders));
	}
	if (pd.dualsrc_terms != 0 && !m_dual_source)
	{
		switch (static_cast<GSDevice::GSTileGpuDualSrcRoad>(pd.dualsrc_road))
		{
			case GSDevice::GSTileGpuDualSrcRoad::Carrier:
				blend_key |= GSDevice::GSTileGpuPassPlan::kDualSrcCarrier;
				pd.dualsrc_bits = kGSTileBlendAlphaCarrier;
				break;
			case GSDevice::GSTileGpuDualSrcRoad::CarrierRestore:
				blend_key |= GSDevice::GSTileGpuPassPlan::kDualSrcCarrierRestore;
				pd.dualsrc_bits = kGSTileBlendAlphaCarrier;
				break;
			case GSDevice::GSTileGpuDualSrcRoad::CarrierCompanion:
				// The carrier takes o_color.a and the alpha channel comes off this draw's write mask,
				// so a second draw over the same indices -- same geometry, same state, carrier bit
				// off, alpha alone -- writes what the alpha byte was going to be. Immediately after,
				// in the same pass, so nothing can read the target between the two.
				blend_key |= GSDevice::GSTileGpuPassPlan::kDualSrcCarrier;
				pd.dualsrc_bits = kGSTileBlendAlphaCarrier;
				write_mask &= ~0x8u;
				alpha_companion = true;
				break;
			default:
				break;
		}
	}

	// The colour write mask rides the blend key, so the executor's per-draw pipeline pick carries
	// it and a draw whose mask differs only splits the indirect run -- never breaks the pass. A
	// depth-only draw (ATST NEVER + AFAIL ZB_ONLY, or an FBMSK that keeps every stored bit) masks
	// all four channels and still tests and writes depth in its pass.
	//
	// ...unless the fragment stage took the mask over, and then it must NOT also ride here: the whole
	// point of the move is that the key stops carrying it, so consecutive draws differing only in
	// FBMSK land in one run and one indirect call. The pipeline writes all four channels and
	// StateRow::fbmsk keeps the dropped ones out. The carrier road's own use of the mask (dropping
	// alpha for the companion draw) cannot collide with this: it needs a fixed-function blend, which
	// gsTileGpuMasksInShader refuses.
	pxAssert(!pd.mask_in_shader || write_mask == pd.color_mask);
	blend_key |= GSDevice::GSTileGpuPassPlan::PackNoWrite(pd.mask_in_shader ? 0xFu : write_mask);
	// ...and so does the in-pass read, for the same reason: the executor's pipeline pick has to turn
	// the fixed-function blend OFF for a draw whose equation the fragment stage evaluates instead,
	// and the run cut has to fall where admission changes. One bit does both.
	if (pd.self_mask != 0)
		blend_key |= GSDevice::GSTileGpuPassPlan::kSelfRead;
	if (pd.self_mask & GSDevice::kGSTileGpuSelfBlend)
		blend_key |= GSDevice::GSTileGpuPassPlan::kSelfBlend;
	m_plan_blend_keys.push_back(blend_key);
	// The depth variant rides beside them: pipeline state, so it cuts the executor's indirect run
	// and never the pass. Under depth-uniform passes every draw of a pass carries the same one and
	// the cut fires nowhere.
	m_plan_depth_modes.push_back(pd.depth_mode);
	pd.draw_index = draw.state_index;
	// The footprints were built above, where the reorder's admission test had to ask about them. They
	// are kept on the plan rather than dropped with the locals because the count-only model asks the
	// same question of a plan nothing staged.
	m_plan_pending.push_back(pd);
	m_plan_write_pages.push_back(draw_write_pages);
	m_plan_read_pages.push_back(draw_read_pages);
	StageDraw(draw.state_index, draw_write_pages, draw_read_pages);

	if (alpha_companion)
	{
		// The alpha byte the draw above gave up to the carrier, written by a second draw over the
		// SAME indices -- no vertices are copied and no index is, only one more indirect command
		// pointing at the range that is already there. Alpha channel alone, no blend (the GS stores
		// the fragment's alpha as-is), carrier bit off so the fragment stage writes the alpha it
		// would have written, byte tail and all.
		//
		// It carries the same depth mode on purpose. A depth-writing draw writes the same values
		// again, and where two of its own fragments overlap the second pass keeps the same winner
		// the first did, because it tests against what the first left. Making it depth-less instead
		// would let a fragment the depth test rejected land its alpha.
		GSDevice::GSTileGpuIndirectDraw alpha_draw = draw;
		alpha_draw.state_index = static_cast<u32>(m_plan_draws.size());
		m_plan_draws.push_back(alpha_draw);
		m_plan_topologies.push_back(topology);
		u32 alpha_key = GSDevice::GSTileGpuPassPlan::PackNoWrite(pd.color_mask & 0x8u);
		if (pd.self_mask != 0)
			alpha_key |= GSDevice::GSTileGpuPassPlan::kSelfRead;
		m_plan_blend_keys.push_back(alpha_key);
		m_plan_depth_modes.push_back(pd.depth_mode);
		PendingDraw apd = pd;
		apd.dualsrc_bits = 0;
		// It blends nothing and names no factor, so it is not part of the population the census
		// counts -- leaving the copy's fields set would have it asking for a companion of its own.
		apd.dualsrc_terms = 0;
		apd.dualsrc_road = static_cast<u8>(GSDevice::GSTileGpuDualSrcRoad::None);
		apd.color_mask = static_cast<u8>(pd.color_mask & 0x8u);
		apd.draw_index = alpha_draw.state_index;
		// The prep ops belong to the draw above and have already run; this one names an empty range
		// so the pass grouping cannot hoist them a second time.
		apd.first_prep_op = pd.first_prep_op + pd.prep_op_count;
		apd.prep_op_count = 0;
		apd.break_before = false;
		// ...and the causes with it. The copy carries the principal's, and a companion that
		// inherited them would have the census attribute the principal's seed or bind break to a
		// second boundary that never happened.
		apd.break_reasons = 0;
		m_plan_pending.push_back(apd);
		// Same geometry, same state, same target: the companion's footprint is its principal's. Its
		// write mask is narrower (the alpha byte alone), which cannot reach a page the principal did
		// not, so copying the pair is exact and not merely conservative.
		m_plan_write_pages.push_back(draw_write_pages);
		m_plan_read_pages.push_back(draw_read_pages);
		StageDraw(alpha_draw.state_index, draw_write_pages, draw_read_pages);
		// One increment per plan entry, or the pass cap and the plan build's (j - i) stop agreeing.
		CurrentRun().draw_count++;
		m_frame.dualsrc_companions++;
	}

	if (split.pass_count == 2)
	{
		// THE SECOND HALF OF THE ALPHA-TEST SPLIT: what only a fragment on one side of the test may
		// write. One more indirect command over the SAME index range -- no vertex is copied and no
		// index is -- exactly as the carrier's alpha companion above is, and immediately after the
		// principal so nothing can read the target between the two.
		//
		// Under RGB_ONLY and FB_ONLY the principal wrote colour for EVERY fragment with no test at
		// all, and this one adds the alpha and the depth the passing fragments own. Under ZB_ONLY
		// the split is by fragment instead -- the principal kept the real test and this one carries
		// the inverted comparison and the depth write alone, because a channel split would have the
		// first pass's depth be what the second tests against.
		const GSTileAlphaSplitPass& p1 = split.pass[1];
		const GSDevice::GSTileGpuDepthMode p1_depth =
			gsTileGpuDepthModeFor(z_used, z_test, p1.z_write);
		// The PASS's depth variant, which is the principal's under
		// EmuCore/GS/TileGpuSplitSharesPassKey and this half's own with the key off. Asked with the
		// draw's own write, the same way the principal asked, so the two answers are equal by
		// construction rather than by a comparison that could be got wrong here.
		const GSDevice::GSTileGpuDepthMode p1_pass_depth =
			gsTileGpuPassDepthModeFor(z_used, z_test, z_write, p1.z_write, m_split_shares_pass_key);
		PendingDraw spd = pd;
		spd.color_mask = p1.color_mask;
		spd.z_write = p1.z_write;
		spd.depth_mode = p1_depth;
		spd.pass_depth_mode = p1_pass_depth;
		spd.atst = (p1.atst != ATST_ALWAYS) ? (static_cast<u32>(p1.atst) + 1u) : 0u;
		spd.aref = (p1.atst != ATST_ALWAYS) ? p1.aref : 0u;
		// It writes the alpha byte the console stores, which is the fragment's own and never a
		// blended value -- so no carrier may ride in it and the write mask stays on the pipeline
		// where this draw's own channels are named.
		spd.dualsrc_bits = 0;
		spd.dualsrc_terms = 0;
		spd.dualsrc_road = static_cast<u8>(GSDevice::GSTileGpuDualSrcRoad::None);
		spd.mask_in_shader = false;
		spd.self_fbmsk = gsTileGpuFrameKeepMask(ctx->FRAME.FBMSK, fb_fmsk);
		// The prep ops belong to the principal and have already run; this names an empty range so
		// the pass grouping cannot hoist them a second time.
		spd.first_prep_op = pd.first_prep_op + pd.prep_op_count;
		spd.prep_op_count = 0;
		spd.break_before = false;
		// ...and the causes with it. The copy carries the principal's, and a companion that
		// inherited them would have the census attribute the principal's seed or bind break to a
		// second boundary that never happened.
		spd.break_reasons = 0;

		// The depth WRITE-ENABLE differs between the two halves under the channel splits, and with
		// EmuCore/GS/TileGpuSplitSharesPassKey off, on a device that asked for depth-uniform passes,
		// that was a pass BOUNDARY rather than a run cut. Under the key p1_key equals draw_key and
		// the block below is dead for this path -- the pass cap is then the only thing that can
		// break here, which is why the test is kept rather than removed.
		//
		// Taken here in accumulation's own idiom, and with all of the open pass's bookkeeping, so
		// that its notion of where a pass ends and the plan build's cut go on naming the same
		// boundaries: they are compared, and a disagreement puts a draw's prep ops in a pass the
		// draw is not in. The pass cap is asked for the same reason -- this is one more plan entry.
		const GSTileGpuPassKey p1_key = gsTileGpuPassKeyFor(
			fb_id, z_id, p1_pass_depth, m_depth_uniform_passes, spd.self_mask != 0, m_segregate_self_read);
		if (p1_key != draw_key || !gsTileGpuPassAdmitsMore(CurrentRun().draw_count, m_max_pass_draws))
		{
			spd.break_before = true;
			spd.break_reasons |= gsTileGpuBreakBit(GSTileGpuBreakCause::AfailSplit);
			BreakOpenPass();
			OpenRun& p1_run = CurrentRun();
			// Everything BreakOpenPass cleared that this draw itself establishes in the new pass.
			// The rule-2 bind first: a slot number belongs to the pass, so the principal's is not
			// this one's, and an empty table makes it slot zero.
			p1_run.date_written = sr;
			p1_run.date_cover.clear();
			if (m_census_date_cover)
				p1_run.date_cover.add(sr);
			if (spd.tex_source != kGSTileNoSurface)
			{
				p1_run.tex_src[0] = spd.tex_source;
				p1_run.tex_count = 1;
				spd.tex_slot = 0;
			}
			const u32 p1_arm = gsTileGpuClutGatherArm(spd.pal_mode);
			if (p1_arm != 0)
				p1_run.gather_arm = p1_arm;
			p1_run.target_road = (spd.road_mask & GSDevice::kGSTileGpuRoadTarget) != 0;
			if (spd.tex_enable && composed_tex)
			{
				p1_run.read |= tex_pages;
				tex_pages.forEachSetPage([&](u32 page) { p1_run.read_slot[page] = m_ring_live[page]; });
			}
		}
		CurrentRun().pass = p1_key;

		GSDevice::GSTileGpuIndirectDraw split_draw = draw;
		split_draw.state_index = static_cast<u32>(m_plan_draws.size());
		m_plan_draws.push_back(split_draw);
		m_plan_topologies.push_back(topology);
		u32 split_key = GSDevice::GSTileGpuPassPlan::PackNoWrite(p1.color_mask);
		if (spd.self_mask != 0)
			split_key |= GSDevice::GSTileGpuPassPlan::kSelfRead;
		m_plan_blend_keys.push_back(split_key);
		m_plan_depth_modes.push_back(p1_depth);
		spd.draw_index = split_draw.state_index;
		m_plan_pending.push_back(spd);
		// Same geometry, same target: this half's footprint is the principal's. Its write mask is
		// narrower and its depth write is a subset of the draw's own, so copying the pair is exact
		// rather than merely conservative -- neither half can reach a page the draw did not.
		m_plan_write_pages.push_back(draw_write_pages);
		m_plan_read_pages.push_back(draw_read_pages);
		StageDraw(split_draw.state_index, draw_write_pages, draw_read_pages);
		CurrentRun().draw_count++;
		if (p1.z_write)
			CurrentRun().z_written |= z_pages;
		if (p1.color_mask != 0)
			CurrentRun().color_written |= fb_pages;
		m_frame.afail_split_draws++;
	}
}

// The reordering admission model, run over the finished plan and taken by nothing. See the
// declaration.
//
// Running it HERE rather than inside AccumulateDraw costs nothing and settles one thing: the plan is
// exactly the reorder window. Every road that can observe a target's bytes -- a readback, a transfer
// crossing a live surface, a CLUT load off GPU-newest pages -- goes through FlushPendingPlan first,
// so a plan boundary is already the point past which nothing may be deferred. The model therefore
// sees the same draw list, with the same bounds, that a scheduler wired into accumulation would.
//
// What it does NOT model is the seed set. `break_before` was decided in guest order against the pass
// that was open then, and this replays those verdicts unchanged -- which is the load-bearing
// assumption of the whole design (the same draws seed under either order) rather than a shortcut. If
// it is wrong, the projected pass count here is optimistic and the on-arm's real count will say so.
void GSRendererTileGpu::ReorderCensus()
{
	const u32 n = static_cast<u32>(m_plan_pending.size());
	if (n == 0)
		return;

	const auto key_of = [this](const PendingDraw& pd) {
		return gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode,
			m_depth_uniform_passes, pd.self_mask != 0, m_segregate_self_read);
	};

	// The same scheduler the live road stages with. It is reset here rather than carried, because a
	// plan IS the reorder window and the model must not remember the previous one's runs.
	m_reorder.Reset(m_reorder_max_runs);
	for (u32 d = 0; d < n; d++)
	{
		const PendingDraw& pd = m_plan_pending[d];
		const GSPageBitmap& w = m_plan_write_pages[d];
		const GSPageBitmap& rd = m_plan_read_pages[d];
		m_frame.reorder_draws++;

		// The class test: is this draw reorderable at all. Three of the four refusals are conservatism
		// with the proof already written -- a DATE draw's snapshot difference is unobservable under its
		// own rect, and declaring is pixel-inert for a pass's non-readers -- but refusing costs one pass
		// apiece on this corpus and the proofs are not what the first rung should rest on.
		u32* refusal = nullptr;
		if (m_plan_draws[d].index_count == 0)
			refusal = &m_frame.reorder_ref_prep; // a prep-only pseudo-draw: it IS a sequence point
		else if (pd.self_mask != 0)
			refusal = &m_frame.reorder_ref_roaa;
		else if (pd.date != 0)
			refusal = &m_frame.reorder_ref_date;
		else if (pd.tex_reads_own_target)
			refusal = &m_frame.reorder_ref_feedback;
		if (refusal)
		{
			(*refusal)++;
			u32 emitted = 0;
			const u32 run = m_reorder.OpenBarrierRun(key_of(pd), &emitted);
			m_frame.reorder_runs += emitted;
			m_reorder.Stage(d, run, w, rd);
			continue;
		}

		const GSTileGpuReorderScheduler::Verdict v = m_reorder.Admit(key_of(pd), w, rd);
		m_frame.reorder_haz_raw += (v.hazards & GSTileGpuReorderScheduler::kHazardRaw) ? 1u : 0u;
		m_frame.reorder_haz_war += (v.hazards & GSTileGpuReorderScheduler::kHazardWar) ? 1u : 0u;
		m_frame.reorder_haz_waw += (v.hazards & GSTileGpuReorderScheduler::kHazardWaw) ? 1u : 0u;
		m_frame.reorder_haz_rar += (v.hazards & GSTileGpuReorderScheduler::kHazardRar) ? 1u : 0u;
		m_frame.reorder_runs += v.runs_emitted;
		m_reorder.Stage(d, v.run, w, rd);
		m_frame.reorder_admitted++;
		m_frame.reorder_peak_draws = std::max(m_frame.reorder_peak_draws, m_reorder.StagedDraws());
		m_frame.reorder_peak_runs = std::max(m_frame.reorder_peak_runs, m_reorder.OpenRuns());
	}
	m_frame.reorder_runs += m_reorder.FlushAll();
	const std::vector<u32>& order = m_reorder.Order();
	pxAssertMsg(order.size() == n, "TileGpu's reorder model lost or duplicated a draw");

	// Both counts off the same cut function and the same per-draw verdicts, so the pair is one
	// arrangement against another and not two different questions.
	const auto breaks_at = [this](u32 d) { return m_plan_pending[d].break_before; };
	m_frame.reorder_passes += gsTileGpuCountPasses(n, m_max_pass_draws,
		[&](u32 d) { return key_of(m_plan_pending[d]); }, breaks_at);
	m_frame.reorder_passes_moved += gsTileGpuCountPasses(n, m_max_pass_draws,
		[&](u32 i) { return key_of(m_plan_pending[order[i]]); },
		[&](u32 i) { return m_plan_pending[order[i]].break_before; });
	m_reorder.ClearOrder();
}

GSPageBitmap GSRendererTileGpu::LiveClutPages() const
{
	GSPageBitmap p;
	for (const u32 idx : m_clut_live)
		p |= m_gpu_palettes[idx].pages;
	return p;
}

void GSRendererTileGpu::StageDraw(u32 index, const GSPageBitmap& written, const GSPageBitmap& read)
{
	if (!m_reorder_active)
		return;
	// The run was chosen where the draw's pass was, which is before anything it emits. Staging is the
	// bookkeeping half and happens here, at the plan entry, so an index is never staged for a draw
	// that turned out not to be pushed.
	pxAssert(m_open_run < m_reorder.OpenRuns());
	m_reorder.Stage(index, m_open_run, written, read);
	m_frame.reorder_peak_draws = std::max(m_frame.reorder_peak_draws, m_reorder.StagedDraws());
	m_frame.reorder_peak_runs = std::max(m_frame.reorder_peak_runs, m_reorder.OpenRuns());
}

// The splice: the plan's per-draw arrays, gathered into the order the runs came out in.
//
// Three things happen and one deliberately does not.
//
//  - The per-draw arrays move, through the index permutation. Every array the plan build's own
//    assert names that is FILLED AT ACCUMULATION is here; the ones it resizes and fills afterwards
//    (the state rows, the scissors, the bind keys, the variant keys) are not, because they are
//    written against the spliced order in the first place.
//  - The prep ops are rebuilt slice by slice, in the new order, and each draw's first_prep_op is
//    renumbered to where its slice landed. The slices tile the array -- every op belongs to exactly
//    one draw -- which is the same contiguity the plan build's pass range arithmetic already needs,
//    so the rebuild comes out monotone and the ranges stay intact.
//  - state_index and draw_index become the draw's new position, which is what the executor indexes
//    the state table by. The dual-source companion is pushed immediately after its principal and
//    joins the same run, so their adjacency survives and its zero-length op slice still lands
//    exactly at the end of its principal's.
//
// ⚠️ And what does NOT move: the page entries, the target list, the source and prep-texture tables,
// the vertex and index streams. An op record carries indices into all of those and none of them were
// touched, so the record still names what it named. This is the whole reason reordering is cheap --
// it moves RECORDS, never geometry and never a side array.
void GSRendererTileGpu::SpliceRuns()
{
	if (!m_reorder_active)
		return;
	const u32 n = static_cast<u32>(m_plan_pending.size());
	m_reorder.FlushAll();
	const std::vector<u32>& order = m_reorder.Order();
	pxAssertMsg(order.size() == n, "TileGpu staged a different number of draws than the plan holds");
	if (order.size() != n)
	{
		m_reorder.ClearOrder();
		return;
	}

	// The op slices first, because renumbering first_prep_op is what the gathered PendingDraw carries.
	gsTileGpuSplicePrepOps(
		m_plan_prep_ops, order, [this](u32 d) { return m_plan_pending[d].first_prep_op; },
		[this](u32 d) { return m_plan_pending[d].prep_op_count; },
		[this](u32 d, u32 base) { m_plan_pending[d].first_prep_op = base; }, m_splice_prep_ops);
	pxAssertMsg(m_splice_prep_ops.size() == m_plan_prep_ops.size(),
		"TileGpu's prep-op slices do not tile the op array: the splice would drop an op");

	gsTileGpuGatherByOrder(m_plan_pending, order, m_splice_pending);
	gsTileGpuGatherByOrder(m_plan_draws, order, m_splice_draws);
	gsTileGpuGatherByOrder(m_plan_topologies, order, m_splice_topologies);
	gsTileGpuGatherByOrder(m_plan_blend_keys, order, m_splice_blend_keys);
	gsTileGpuGatherByOrder(m_plan_depth_modes, order, m_splice_depth_modes);
	gsTileGpuGatherByOrder(m_plan_write_pages, order, m_splice_write_pages);
	gsTileGpuGatherByOrder(m_plan_read_pages, order, m_splice_read_pages);
	// The two indices that name a draw's own POSITION rather than anything it owns. Renumbered after
	// the gather, from the position and not from the old index -- taking the old one is the mistake
	// this is spelt out to avoid, and it reads another draw's state row rather than failing.
	for (u32 pos = 0; pos < n; pos++)
	{
		m_splice_pending[pos].draw_index = pos;
		m_splice_draws[pos].state_index = pos;
	}

	m_plan_pending.swap(m_splice_pending);
	m_plan_draws.swap(m_splice_draws);
	m_plan_topologies.swap(m_splice_topologies);
	m_plan_blend_keys.swap(m_splice_blend_keys);
	m_plan_depth_modes.swap(m_splice_depth_modes);
	m_plan_write_pages.swap(m_splice_write_pages);
	m_plan_read_pages.swap(m_splice_read_pages);
	m_plan_prep_ops.swap(m_splice_prep_ops);
	m_reorder.ClearOrder();
}

// Every draw's fragment variant, and the guard that may withhold half of it.
//
// The variant has two halves and they are decided differently. The ROAD half -- which texel road the
// draw takes, which decode arm, what it needs the destination read for, whether a 16-bit frame
// stores its output -- is a fact about the draw and its pass, and narrowing it is free: a draw stops
// executing a decoder it never enters. The SPECIALIZATION half freezes the draw's own GS state into
// the program, which makes the program much smaller (247 of the corpus's 251 distinct variants at
// eight registers or fewer on an Adreno 650, so wave128, against two of seventeen) and makes the
// program part of the indirect-run key, so two consecutive draws differing only in their alpha
// comparison stop being one run.
//
// On an SD865 that second half is not uniformly a win. It takes Shadow of the Colossus -20.5% and
// Gran Turismo 4 -7.6% with GPU time falling, and both Ratchet & Clank scenes +9.2% and +8.4% with
// GPU time RISING. The separator is the number of pipeline binds the freezing ADDS over what the
// road half already required -- 141 and 150 a frame on the winners, 525 and 859 on the losers, with
// the rest of the eighteen-dump corpus at 72 or below. Over the device's budget, the passes that add
// those binds give the frozen state back.
//
// ⚠️ Two things this function's shape depends on, both established rather than assumed:
//
//  - It is a PLAN question, not a pass question. Shadow of the Colossus builds passes that are
//    structurally identical to Ratchet's -- 64 draws, one road, fifty-odd binds -- and wins; it
//    simply builds two of them a frame where Ratchet builds fifteen. So no per-pass number separates
//    the two, and the guard is a budget over the whole submission with the passes that spend it
//    picked afterwards. That is why the variant fill moved out of the grouping walk.
//  - The withheld form is the DE-SPECIALIZED key, never the pass UNION. Both are pixel-inert, but
//    the union also widens the road half back to the pass's, which is the contamination the per-draw
//    variant was landed to end -- 1,499 draws a frame put back on a byte decoder they never enter on
//    the Ratchet gameplay scene, and its draw-weighted program from 2,684 SPIR-V words to 11,000. The
//    de-specialized key is bit-for-bit what EmuCore/GS/TileGpuUnspecializedFragmentVariant writes,
//    which is the arm gated byte-identical over the corpus AND the arm that measured faster on the
//    device for both losing scenes. The guard picks a proven program, and the better of the two.
void GSRendererTileGpu::PlanFragmentVariants()
{
	const u32 count = static_cast<u32>(m_plan_draws.size());
	m_spec_passes.clear();
	if (count == 0)
		return;
	// The SAME cut the grouping walk below takes, off the same function, for the reason accumulation
	// and the plan build share theirs: two spellings of "where does this pass end" would give a draw a
	// program built against a pass it is not in. The grouping walk asserts the keys it finds here.
	const auto key_of = [this](const PendingDraw& pd) {
		return gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode,
			m_depth_uniform_passes, pd.self_mask != 0, m_segregate_self_read);
	};
	// What cuts an indirect run for a reason that predates the fragment variant. Subtracting these
	// is what makes the guard's number the SPECIALIZATION's bill rather than the submission's.
	const auto other_cut = [this](u32 d) {
		return m_plan_topologies[d] != m_plan_topologies[d - 1] ||
			   m_plan_blend_keys[d] != m_plan_blend_keys[d - 1] ||
			   m_plan_depth_modes[d] != m_plan_depth_modes[d - 1];
	};

	u32 added = 0, unguarded_runs = 0;
	for (u32 i = 0; i < count;)
	{
		const u32 j = gsTileGpuPassEnd(i, count, m_max_pass_draws,
			[&](u32 d) { return key_of(m_plan_pending[d]); },
			[&](u32 d) { return m_plan_pending[d].break_before; });
		// The pass's destination-read union, because two of the variant's axes are promoted to it --
		// a DATE draw in a pass that declares for DATE has no snapshot to read, and the AFAIL alpha
		// keep's row bit is set for every draw the fold names. Same union the grouping walk computes.
		u32 self_union = 0;
		for (u32 d = i; d < j; d++)
			self_union |= m_plan_pending[d].self_mask;
		for (u32 d = i; d < j; d++)
			m_plan_variant_keys[d] = PlanVariantKeyAt(d, self_union);

		const u32 runs = gsTileGpuRunCount(
			i, j, [this](u32 d) { return m_plan_variant_keys[d]; }, other_cut);
		const u32 runs_despec = gsTileGpuRunCount(
			i, j, [this](u32 d) { return gsTileGpuDespecializeVariantKey(m_plan_variant_keys[d]); },
			other_cut);
		m_spec_passes.push_back(SpecPass{i, j, runs, runs_despec});
		added += runs - runs_despec;
		unguarded_runs += runs;
		i = j;
	}
	m_frame.variant_runs_unguarded += unguarded_runs;

	if (!gsTileGpuGuardsSpecialization(added, m_max_spec_binds))
		return;
	u32 guarded_passes = 0, guarded_draws = 0;
	for (const SpecPass& p : m_spec_passes)
	{
		// A pass whose frozen state cut no run it would not otherwise have cut keeps it: that program
		// is smaller for free, and taking it away would give up the wave size and save nothing. It
		// also makes the guarded plan's bind total exactly the unspecialized arm's, since every pass
		// then contributes its de-specialized run count whichever branch it took.
		if (!gsTileGpuDespecializesPass(p.runs, p.runs_despec))
			continue;
		for (u32 d = p.first; d < p.end; d++)
			m_plan_variant_keys[d] = gsTileGpuDespecializeVariantKey(m_plan_variant_keys[d]);
		guarded_passes++;
		guarded_draws += p.end - p.first;
	}
	m_frame.specguard_passes += guarded_passes;
	m_frame.specguard_draws += guarded_draws;
	if (!m_specguard_announced)
	{
		m_specguard_announced = true;
		Console.WriteLn("TileGpu: fragment specialization added %u pipeline binds to a plan against a "
						"budget of %u, so %u of %u passes (%u draws) give the frozen state back.",
			added, m_max_spec_binds, guarded_passes, static_cast<u32>(m_spec_passes.size()), guarded_draws);
	}
}

// Group the accumulated draws into passes, resolve targets and state rows, build the ring, and
// submit. One FRAME/ZBUF pair per pass (break on target change, depth mode change, or a prep op);
// scale 1. Consumes everything: the plan streams, the ring, and the model's synced claims (the
// ring the claims vouched for is gone once submitted).
void GSRendererTileGpu::BuildAndExecutePlan()
{
	if (m_plan_pending.empty())
	{
		// No draws to issue, so nothing can be naming a device palette: the prune's precondition
		// holds here too, and a stretch of nothing but CLUT loads must not accumulate records.
		PruneGpuPalettes();
		return;
	}

	// The runs come out here, in open order, and the plan's per-draw arrays go into that order. Ahead
	// of everything below, because everything below is written against the order the executor will
	// see. A no-op with the lever off.
	SpliceRuns();

	// ...and after it, because it reads the FINAL op array: a run of copies the splice pulled apart
	// is a run this cannot merge, and asking before the splice would merge one the executor never
	// sees consecutively. A no-op with the lever off.
	MergeClutCopiesIntoPages();

	if (g_gs_device->TileGpuExecutorAvailable())
	{
		// 1. Resolve the plan's target list: pool textures as they are NOW (any growth this
		//    frame has already happened, so these are the textures every draw lands in).
		m_plan_targets.resize(m_plan_target_surfaces.size());
		for (u32 k = 0; k < m_plan_target_surfaces.size(); k++)
			m_plan_targets[k] = m_target_pool.GetTexture(m_vram_model.Get(m_plan_target_surfaces[k]).pool_handle);

		// 2. The per-draw state row: the HW tfx VertexScale/VertexOffset transform against the
		//    resolved colour target's size (scale 1), plus the z enables and the texture block.
		//    Row i serves draw i.
		m_plan_states.resize(m_plan_pending.size());
		// The same two slots, lifted out of the state row for the executor: it splits its indirect
		// runs on the pair, and the state table's layout is not part of that contract
		// (GSTileGpuPassPlan::bind_keys).
		m_plan_bind_keys.resize(m_plan_pending.size());
		// ...and the per-draw GS scissor, which is a command rather than shader state.
		m_plan_scissors.resize(m_plan_pending.size());
		// ...and the per-draw fragment variant, sized here and filled by the grouping below, which is
		// where a draw's pass -- and so the DATE road its pass provided -- is finally known.
		m_plan_variant_keys.assign(m_plan_pending.size(), 0u);
		for (u32 i = 0; i < m_plan_pending.size(); i++)
		{
			const PendingDraw& pd = m_plan_pending[i];
			const GSVector2i dim = m_plan_targets[m_plan_target_of_surface[pd.color_surface]]->GetSize();
			const float sx = 2.0f / static_cast<float>(dim.x << 4);
			const float sy = 2.0f / static_cast<float>(dim.y << 4);
			const float ox2 = -1.0f / static_cast<float>(dim.x);
			const float oy2 = -1.0f / static_cast<float>(dim.y);
			StateRow& sr = m_plan_states[i];
			sr.vertex_scale[0] = sx;
			sr.vertex_scale[1] = sy;
			sr.vertex_offset[0] = static_cast<float>(pd.ofx) * sx + ox2 + 1.0f;
			sr.vertex_offset[1] = static_cast<float>(pd.ofy) * sy + oy2 + 1.0f;
			sr.z_write = pd.z_write ? 1u : 0u;
			sr.z_test = pd.z_test ? 1u : 0u;
			sr.tex_enable = pd.tex_enable ? 1u : 0u;
			sr.fst = pd.fst ? 1u : 0u;
			sr.tbp0 = pd.tbp0;
			sr.tbw = pd.tbw;
			sr.tw = pd.tw;
			sr.th = pd.th;
			sr.tfx = pd.tfx;
			sr.tcc = pd.tcc;
			sr.wms = pd.wms;
			sr.wmt = pd.wmt;
			sr.index_format = pd.index_format;
			sr.pal_offset = pd.pal_offset;
			sr.epoch = pd.epoch;
			sr.date = pd.date;
			sr.fge = pd.fge ? 1u : 0u;
			sr.fogcol = pd.fogcol;
			sr.atst = pd.atst;
			sr.aref = pd.aref;
			sr.texa = pd.texa;
			sr.region_u = pd.region_u;
			sr.region_v = pd.region_v;
			sr.ltf = pd.ltf ? 1u : 0u;
			sr.tex_target = pd.tex_slot;
			sr.tex_source = pd.src_slot;
			sr.pal_mode = pd.pal_mode;
			sr.pal_bias = pd.pal_bias;
			// The row says what the fragment stage must DO with this draw, not what the registers said.
			// A pass declares the read for the draws that need it and carries the ones that do not, so
			// the two bits below are what tells them apart -- and spending the admission that way costs
			// the shader nothing to unpack, which the Adreno instruction budget notices.
			sr.blend = (pd.self_mask & GSDevice::kGSTileGpuSelfBlend) ? pd.self_blend :
																		(pd.self_blend & ~kGSTileBlendEnable);
			sr.fbmsk = (pd.self_mask & GSDevice::kGSTileGpuSelfMask) ? pd.self_fbmsk : 0u;
			if (pd.quantise_5551)
				sr.blend |= kGSTileBlendQuant16;
			if (pd.afail_keep_alpha)
				sr.blend |= kGSTileBlendAfailKeepAlpha;
			// ...and what to do with the As blend factor where there is no second output to put it
			// in. Zero on every draw on a device that has one, so the arm is not even compiled there.
			sr.blend |= pd.dualsrc_bits;
			// The row's last two words, which used to be its tail padding. Written on every row even
			// though only pal_mode 3 reads them, for the reason the padding was written: the rows are
			// reused frame to frame, and a state buffer whose unread words differ between two runs of
			// the same dump is a capture diff nobody can dismiss at a glance.
			sr.pal_mul = pd.pal_mul;
			sr.pal_shift = pd.pal_shift;
			m_plan_bind_keys[i] = GSDevice::GSTileGpuPassPlan::PackBindKey(pd.tex_slot, pd.src_slot);
			// The draw's GS scissor, handed to the executor as its own stream: it is a Vulkan
			// scissor and a cut in the indirect run, neither of which the state table can express.
			m_plan_scissors[i] = pd.scissor;

			// The scissor census's per-draw half. `dim` is this draw's colour target, so a scissor
			// that swallows it whole rejects nothing whatever the geometry does; `scissor_cuts` is
			// the narrower population that actually loses fragments to it.
			m_frame.scissor_draws++;
			const GSVector4i target_rect(0, 0, dim.x, dim.y);
			if (!pd.scissor.rintersect(target_rect).eq(target_rect))
				m_frame.scissor_subrect++;
			if (pd.scissor_cuts)
				m_frame.scissor_cuts++;

			// The dual-source census. The road was decided at accumulation as though no device had
			// a second colour output, so these columns say what the carrier roads cost on BOTH arms
			// -- and dualsrc_companions, counted where the companions are really issued, says what
			// this arm actually paid.
			if (pd.dualsrc_terms != 0)
			{
				m_frame.dualsrc_draws++;
				// Informational: the population the deleted fold road served. Kept because it is the
				// one number that would price bringing it back on a driver that rounds a shader
				// multiply and a blend-unit multiply the same way.
				if ((pd.dualsrc_terms & GSDevice::kGSTileGpuDualSrcDest) == 0)
					m_frame.dualsrc_src_only++;
				switch (static_cast<GSDevice::GSTileGpuDualSrcRoad>(pd.dualsrc_road))
				{
					case GSDevice::GSTileGpuDualSrcRoad::Carrier: m_frame.dualsrc_carrier++; break;
					case GSDevice::GSTileGpuDualSrcRoad::CarrierRestore: m_frame.dualsrc_restore++; break;
					case GSDevice::GSTileGpuDualSrcRoad::CarrierCompanion: m_frame.dualsrc_needs_comp++; break;
					default: break;
				}
			}
		}

		// 3. Group contiguous draws sharing a colour+depth surface pair and depth mode into one
		//    pass. The depth pipeline is per-pass fixed function, so z-write and z-test must be
		//    uniform across the pass (a ZTST=ALWAYS write sprite and a ZTST=GEQUAL 3D draw share
		//    a depth buffer in SotC). Each pass carries the prep ops its draws emitted, in order
		//    (each draw's ops are contiguous and appended in draw order, so the range is [first
		//    draw's first, last draw's last]) and the executor runs the whole range BEFORE the
		//    pass opens -- so an op belonging to any draw but the first is hoisted over the draws
		//    ahead of it. Accumulation decides whether that is allowed, per op: a seed always
		//    breaks (it is a fragment pass over the target that has to land between the draws),
		//    and a writeback breaks only where it collides with what the open pass has already
		//    written or sampled (WritebackHoistCollides). A non-breaking writeback therefore
		//    belongs to a later draw and is hoisted BY DESIGN, having been proved invisible.
		//
		//    ...and, where the device asked for one, a LENGTH cap: a pass stops at m_max_pass_draws
		//    draws and the next one opens with the same key. That is a pure split, and the two
		//    per-pass mechanisms it touches narrow with it rather than change meaning. Each split
		//    pass's union masks are computed over ITS draws, so the same pixels come out of a
		//    narrower program. And a DATE run split in two takes a second snapshot, which sees the
		//    first half's output -- still exact, because a DATE draw on the snapshot road already
		//    breaks the pass whenever anything since the run started wrote under it (OpenRun::date_written,
		//    in AccumulateDraw). So no draw of the first half ever wrote a pixel the second half's
		//    DATE test reads, and the two snapshots agree everywhere either of them is looked at.
		//
		// Every per-draw array the plan hands the executor is indexed by draw. Two places append a
		// draw -- AccumulateDraw and the display-seed pseudo-draw above -- so a new array added to
		// one and not the other reads the next draw's state, silently and only on the dumps that
		// take the second road.
		pxAssert(m_plan_topologies.size() == m_plan_draws.size() &&
				 m_plan_blend_keys.size() == m_plan_draws.size() &&
				 m_plan_scissors.size() == m_plan_draws.size() &&
				 m_plan_depth_modes.size() == m_plan_draws.size() &&
				 m_plan_bind_keys.size() == m_plan_draws.size() &&
				 m_plan_variant_keys.size() == m_plan_draws.size() &&
				 m_plan_write_pages.size() == m_plan_draws.size() &&
				 m_plan_read_pages.size() == m_plan_draws.size() &&
				 m_plan_pending.size() == m_plan_draws.size());

		// Every draw's fragment variant, decided before the grouping walk below rather than inside
		// it, because the SPECIALIZATION half is answered per PLAN and not per pass: the guard's
		// question is how many pipeline binds the frozen state adds to the whole submission, and no
		// pass knows that while it is being built. See PlanFragmentVariants.
		PlanFragmentVariants();

		// Hoisted out of the walk below so the pass-break census can ask it about the PREVIOUS pass
		// too. One reader for the key, as everywhere else that groups draws.
		const auto key_of = [this](const PendingDraw& pd) {
			return gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode,
				m_depth_uniform_passes, pd.self_mask != 0, m_segregate_self_read);
		};

		u32 i = 0;
		// The pass the walk is about to leave, for the census: its key, and how many draws it held.
		// Held here rather than recomputed because the cut compared against the FIRST draw of that
		// pass, not against the draw before this one, and attributing a boundary to a comparison the
		// walk did not make is how a census and the thing it counts drift apart.
		GSTileGpuPassKey prev_key;
		u32 prev_draws = 0;
		bool have_prev = false;
		while (i < m_plan_draws.size())
		{
			const PendingDraw& first = m_plan_pending[i];
			if (m_census_pass_breaks)
			{
				if (have_prev)
					CensusPassBreak(prev_key, prev_draws, i);
				else
					m_frame.break_plans++;
			}
			const u32 j = gsTileGpuPassEnd(i, static_cast<u32>(m_plan_draws.size()), m_max_pass_draws,
				[&](u32 d) { return key_of(m_plan_pending[d]); },
				[&](u32 d) { return m_plan_pending[d].break_before; });
			const PendingDraw& last = m_plan_pending[j - 1];

			GSDevice::GSTileGpuTargetPair tp = {};
			tp.frame_target = m_plan_target_of_surface[first.color_surface];
			tp.zbuf_target = first.z_used ? m_plan_target_of_surface[first.z_surface] : GSDevice::GSTileGpuPassPlan::kNoTarget;

			GSDevice::GSTileGpuPass pass = {};
			pass.first_draw = i;
			pass.draw_count = j - i;
			pass.first_target_pair = static_cast<u32>(m_plan_target_pairs.size());
			pass.target_pair_count = 1;
			pass.first_prep_op = first.first_prep_op;
			pass.prep_op_count = (last.first_prep_op + last.prep_op_count) - first.first_prep_op;
			// The in-pass destination read, as a union over the draws THIS GROUPING already put in
			// the pass. Computed here rather than at accumulation on purpose: declaring is a property
			// of a pass, never a reason to open one, so the read can only ever remove pass breaks.
			pass.self_mask = 0;
			u32 readers = 0;
			for (u32 d = i; d < j; d++)
			{
				pass.self_mask |= m_plan_pending[d].self_mask;
				readers += (m_plan_pending[d].self_mask != 0) ? 1u : 0u;
			}
			pass.declares_self_read = pass.self_mask != 0;
			// Under segregation a declaring pass's readers ARE the pass: the key put every
			// non-reader in some other pass, so nothing rides along paying for the declaration.
			// Asserted rather than assumed, because the key and this union disagreeing is invisible
			// in the frame and is the entire failure mode the policy exists to prevent.
			pxAssertMsg(!m_segregate_self_read || readers == 0 || readers == pass.draw_count,
				"TileGpu segregated the readers but a declaring pass still carries draws that do not read");
			// A pass has ONE colour surface and therefore one frame format, so this is a property of
			// the pass even though the decision that uses it is per draw.
			pass.quantises_frame = false;
			for (u32 d = i; d < j; d++)
				pass.quantises_frame = pass.quantises_frame || m_plan_pending[d].quantise_5551;
			m_frame.self_read_passes += pass.declares_self_read ? 1u : 0u;
			// ...and the draws SITTING IN those passes, which is the number a device that charges
			// for declaring actually pays. Under segregation it converges on the reader count;
			// merged it is however many draws the readers happened to be standing among.
			m_frame.self_read_pass_draws += pass.declares_self_read ? pass.draw_count : 0u;

			// The pass's rule-2 bind table, rebuilt by walking its draws in order and appending each
			// source the first time it appears. That is exactly how accumulation numbered the slots
			// (the run's tex_src list resets on the same breaks this grouping does), so the slot a
			// state row carries has to come out the same -- asserted, because a silent disagreement
			// here would sample the wrong target rather than fail.
			pass.first_tex_source = static_cast<u32>(m_plan_tex_sources.size());
			pass.tex_source_count = 0;
			pass.road_mask = 0;
			pass.texel_mask = 0;
			u32 byte_draws = 0;
			for (u32 d = i; d < j; d++)
			{
				const PendingDraw& pd = m_plan_pending[d];
				// The pass's shader variant: the union of the roads its draws take, and -- for the draws
				// that decode ring bytes -- the union of the texel arms they decode through. A pass of
				// nothing but untextured draws lands at zero on both and gets the smallest program there
				// is.
				//
				// ⚠️ This is no longer what a draw COMPILES. The union is the pass's descriptor story and
				// the size gate's census; the program a draw runs comes off its own variant key, filled
				// below, which is part of the executor's run key. Keeping the union is not redundancy --
				// it is the fallback for a plan that carries no variant stream, and the number the
				// contamination census is expressed in.
				pass.road_mask |= pd.road_mask;
				if (pd.tex_enable && (pd.road_mask & GSDevice::kGSTileGpuRoadByte) != 0)
				{
					pass.texel_mask |= GSDevice::GSTileGpuTexelArm(pd.index_format);
					// The sixth arm: a palette the gather copied out of a target is in texel order, so the
					// entry order has to be applied at fetch. Only the two Gran Turismo dumps in the corpus
					// ever ask for it, and every other pass gets the machinery left out.
					//
					// ...and the seventh, which is the same job against a SIXTEEN-BIT owner: a different
					// entry order, so it replaces the sixth rather than joining it. A pass carrying both
					// is 145 units against a 123-unit cliff, which is why accumulation splits the pass
					// instead (see the guard in AccumulateDraw) and the size gate refuses to price the
					// union at all.
					if (pd.pal_mode >= 4u)
						pass.texel_mask |= GSDevice::kGSTileGpuTexelPalGather16;
					else if (pd.pal_mode != 0)
						pass.texel_mask |= GSDevice::kGSTileGpuTexelPalGather;
					byte_draws++;
				}
				if (pd.tex_source == kGSTileNoSurface)
					continue;
				const u32 ti = m_plan_target_of_surface[pd.tex_source];
				u32 slot = 0;
				while (slot < pass.tex_source_count && m_plan_tex_sources[pass.first_tex_source + slot] != ti)
					slot++;
				if (slot == pass.tex_source_count)
				{
					m_plan_tex_sources.push_back(ti);
					pass.tex_source_count++;
				}
				pxAssertMsg(slot == pd.tex_slot, "TileGpu rule-2 slot disagrees with the pass's bind table");
			}
			// What the texel-arm axis costs, watched rather than assumed: a pass whose byte-road draws
			// span two arms compiles a program carrying both, which is the one case the shatter does not
			// shrink all the way. The corpus says 1.6% of byte-road passes, so this number staying small
			// is a standing claim -- if a title moves it, the answer is a wider census, not a shrug.
			if (byte_draws != 0)
			{
				m_frame.texel_passes++;
				// Mixed means two address GEOMETRIES; the palette-order arm rides along with whichever
				// paletted geometry asked for it and is not a format the pass mixes.
				const u32 geom = pass.texel_mask & GSDevice::kGSTileGpuTexelGeometryMask;
				if ((geom & (geom - 1)) != 0)
				{
					m_frame.texel_mixed_passes++;
					m_frame.texel_mixed_draws += byte_draws;
				}
			}
			// The per-draw fragment variant, already resolved by PlanFragmentVariants -- which walks
			// the same passes off the same cut and so hands this loop the same self_mask union. Checked
			// rather than trusted, because the two walks disagreeing would put a draw on a program its
			// pass does not describe, and that is a wrong pixel rather than a slow one. A guarded draw
			// legitimately carries the de-specialized form of the same key and nothing else.
			for (u32 d = i; d < j; d++)
			{
				pxAssertMsg(m_plan_variant_keys[d] == PlanVariantKeyAt(d, pass.self_mask) ||
								m_plan_variant_keys[d] ==
									gsTileGpuDespecializeVariantKey(PlanVariantKeyAt(d, pass.self_mask)),
					"TileGpu's two pass walks disagree about a draw's fragment variant");
				const PendingDraw& pd = m_plan_pending[d];
				const u32 v = m_plan_variant_keys[d];
				// What this draw stops paying for: the union program is a superset of its own on every
				// axis, so "narrower on any axis" is the whole population the decoupling serves.
				if (GSDevice::GSTileGpuPassPlan::VariantRoadMask(v) != pass.road_mask ||
					GSDevice::GSTileGpuPassPlan::VariantTexelMask(v) != pass.texel_mask ||
					GSDevice::GSTileGpuPassPlan::VariantSelfMask(v) != pass.self_mask ||
					GSDevice::GSTileGpuPassPlan::VariantQuantises(v) != pass.quantises_frame)
					m_frame.variant_narrowed_draws++;
				// ...and the census's own column, in the units the pass-cause table used: a draw off the
				// byte road inside a pass that compiles it. Those are the expensive ones -- the byte
				// roads are 1039-1748 instructions against the materialised source road's 390.
				if ((pd.road_mask & GSDevice::kGSTileGpuRoadByte) == 0 &&
					(pass.road_mask & GSDevice::kGSTileGpuRoadByte) != 0)
					m_frame.variant_byte_freeloaders++;
				// The two histograms: what this draw compiles now, and what it would have compiled as a
				// member of its pass. Same run, both arms, so the before-and-after cannot be a different
				// scene.
				CensusAdd(m_variant_census_own, v, 1);
				CensusAdd(m_variant_census_union,
					GSDevice::GSTileGpuPassPlan::PackVariantKey(
						pass.road_mask, pass.texel_mask, pass.self_mask, pass.quantises_frame),
					1);
			}
			// The runs the executor will submit inside this pass, and what the variant axis added to
			// them. Counted here rather than inferred from the device's call total because the device
			// also cuts on the sampled-binding key, and the two costs have to stay separable.
			for (u32 d = i; d < j; d++)
			{
				if (d == i || PlanRunKeyAt(d) != PlanRunKeyAt(d - 1))
					m_frame.variant_runs++;
			}
			// ...and the same count with the SPECIALIZATION half withheld, which is what the
			// unspecialized arm submits. Both off one run, so the difference is the frozen state's own
			// bind bill rather than two scenes compared. A bind is an instruction-RAM DMA on Adreno,
			// so this difference -- not the run total -- is what specialization is charged for.
			//
			// Alongside it, the two PROGRAM counts: how many distinct fragment programs this pass
			// alternates among, with and without the frozen state. That is the working set an
			// instruction cache holds or does not, while the run counts are how often it is cycled.
			{
				u32 runs_here = 0, runs_despec = 0;
				m_shape_keys.clear();
				m_shape_keys_despec.clear();
				for (u32 d = i; d < j; d++)
				{
					const u32 own = m_plan_variant_keys[d];
					const u32 despec = own & ~GSDevice::GSTileGpuPassPlan::kVariantSpecMask;
					if (d == i || PlanRunKeyAt(d) != PlanRunKeyAt(d - 1))
						runs_here++;
					if (d == i || m_plan_topologies[d] != m_plan_topologies[d - 1] ||
						m_plan_blend_keys[d] != m_plan_blend_keys[d - 1] ||
						m_plan_depth_modes[d] != m_plan_depth_modes[d - 1] ||
						despec != (m_plan_variant_keys[d - 1] & ~GSDevice::GSTileGpuPassPlan::kVariantSpecMask))
						runs_despec++;
					m_shape_keys.push_back(own);
					m_shape_keys_despec.push_back(despec);
				}
				m_frame.variant_runs_despec += runs_despec;
				m_frame.variant_pass_programs += gsTileGpuSortUniqueCount(m_shape_keys);
				m_frame.variant_pass_programs_despec += gsTileGpuSortUniqueCount(m_shape_keys_despec);
				// Both scratch vectors come back sorted and uniqued, so the shipped-key one is already
				// the shape the census wants.
				PassShapeAdd(m_shape_keys, pass.draw_count, runs_here, runs_despec);
			}

			// What the rule-3 half of the sampled-binding split COSTS, in calls -- the same
			// arithmetic that priced it before it was taken, now measuring the real thing. The
			// executor cuts a call at a pipeline change (topology, blend key, depth mode or fragment
			// variant) and at a bind-key change, because a descriptor array index has to be
			// dynamically uniform across the call. Folding the source into that key cuts one more
			// call wherever the source changes and nothing else does -- so only those boundaries are
			// counted, and only inside a pass, which is the one place pass membership is known.
			// Priced at the measured ~237 ns per indirect call, this is what says whether the
			// slot-splitting shape stays affordable.
			for (u32 d = i + 1; d < j; d++)
			{
				if (PlanRunKeyAt(d) != PlanRunKeyAt(d - 1))
					continue; // a new pipeline run, so already a new call
				if (m_plan_pending[d].tex_slot != m_plan_pending[d - 1].tex_slot)
					continue; // already a new call, for the rule-2 slot
				if (m_plan_pending[d].src_slot != m_plan_pending[d - 1].src_slot)
					m_frame.src_extra_calls++;
			}
			// And the same arithmetic for the variant axis, against the run key WITHOUT it: a cut
			// nothing else would have made. This is the number the census predicted at 30 a frame
			// worst case in the corpus, and the one that says whether the decoupling stays free.
			for (u32 d = i + 1; d < j; d++)
			{
				if (m_plan_topologies[d] != m_plan_topologies[d - 1] ||
					m_plan_blend_keys[d] != m_plan_blend_keys[d - 1] ||
					m_plan_depth_modes[d] != m_plan_depth_modes[d - 1])
					continue; // a new pipeline run for a reason that predates this axis
				if (m_plan_pending[d].tex_slot != m_plan_pending[d - 1].tex_slot ||
					m_plan_pending[d].src_slot != m_plan_pending[d - 1].src_slot)
					continue; // already a new call, for the sampled-binding key
				if (m_plan_variant_keys[d] != m_plan_variant_keys[d - 1])
					m_frame.variant_extra_calls++;
			}

			// ...and the same arithmetic for the SCISSOR, which is a cut on every device: the
			// executor sets it per indirect call, so it ends a call the way the sampled-binding key
			// does. This is what that costs -- the boundaries where the scissor changes and the
			// pipeline run and the sampled-binding key both do not.
			m_scissor_keys.clear();
			for (u32 d = i; d < j; d++)
			{
				const GSVector4i& sc = m_plan_pending[d].scissor;
				m_scissor_keys.push_back(gsTileGpuScissorKey(sc.x, sc.y, sc.z, sc.w));
			}
			const u32 scissors_here = gsTileGpuSortUniqueCount(m_scissor_keys);
			m_frame.scissor_distinct += scissors_here;
			m_frame.scissor_pass_distinct_max = std::max(m_frame.scissor_pass_distinct_max, scissors_here);
			for (u32 d = i + 1; d < j; d++)
			{
				if (PlanRunKeyAt(d) != PlanRunKeyAt(d - 1))
					continue; // a new pipeline run, so already a new call
				if (m_plan_bind_keys[d] != m_plan_bind_keys[d - 1])
					continue; // already a new call, for the sampled-binding key
				if (!m_plan_pending[d].scissor.eq(m_plan_pending[d - 1].scissor))
					m_frame.scissor_extra_calls++;
			}

			// A pass with a DATE draw snapshots the pixels those draws read, before opening.
			pass.first_snapshot = static_cast<u32>(m_plan_snapshots.size());
			pass.snapshot_count = 0;
			// ...and takes none when the read serves DATE: the live pixel is the destination, exactly.
			//
			// The rect is the union of the DATE draws' own scissor-clipped boxes and not the whole
			// target, which is what it used to be. A snapshot has ONE consumer -- the texelFetch at
			// `gl_FragCoord.xy` in tilegpu.glsl, under the draw's own `date != 0` -- so a DATE draw
			// reads exactly the pixels it rasterizes and the union covers every read the pass can
			// make of it. Gathered here rather than folded in the loop so the rule itself is a pure
			// function the tests can break on purpose (gsTileGpuSnapshotRect).
			//
			// Every draw in the pass shares the colour surface -- it is in the pass key -- so these
			// rectangles and the target are in one pixel space, which is also the space the copy
			// lands in and the space gl_FragCoord counts in.
			m_snapshot_reads.clear();
			for (u32 d = i; d < j; d++)
			{
				if (m_plan_pending[d].date != 0 && (pass.self_mask & GSDevice::kGSTileGpuSelfDate) == 0)
					m_snapshot_reads.push_back(m_plan_pending[d].rect);
			}
			if (!m_snapshot_reads.empty())
			{
				const GSVector2i tsz = m_plan_targets[tp.frame_target]->GetSize();
				const GSVector4i full(0, 0, tsz.x, tsz.y);
				const GSVector4i rect = m_narrow_date_snapshot ?
											GSDevice::gsTileGpuSnapshotRect(m_snapshot_reads, tsz.x, tsz.y) :
											full;
				m_plan_snapshots.push_back(GSDevice::GSTileGpuSnapshotCopy{tp.frame_target, rect});
				pass.snapshot_count = 1;
				m_frame.snapshots++;
				// Both roads' bills, off the one run: what this copy costs and what the whole-target
				// copy it replaces would have. Counted on BOTH arms, so the census is answerable for
				// the change without a second scene to compare against.
				const u32 pages = GSDevice::gsTileGpuSnapshotPages(rect);
				m_frame.snapshot_pages += pages;
				m_frame.snapshot_full_pages += GSDevice::gsTileGpuSnapshotPages(full);
				m_frame.snapshot_narrowed += rect.eq(full) ? 0u : 1u;
				m_frame.snapshot_page_hist[GSDevice::gsTileGpuSnapshotPageBucket(pages)]++;
			}

			// The cap, checked rather than trusted. Mechanical because the failure it guards against
			// is not visible in a frame: accumulation and the plan build counting the same draws
			// differently produces a pass one side thinks is shorter than the other, which surfaces
			// as a hoisted prep op or a mis-numbered rule-2 slot, not as a wrong pixel.
			pxAssertMsg(gsTileGpuPassAdmitsMore(pass.draw_count - 1, m_max_pass_draws),
				"TileGpu built a pass longer than the max-pass-draws cap");
			m_frame.biggest_pass_draws = std::max(m_frame.biggest_pass_draws, pass.draw_count);
			// A pass the CAP ended, as against one that reached the cap and would have stopped there
			// anyway. The distinction is the whole value of the counter: it is what says the cap
			// engaged on this dump rather than merely being set.
			if (m_max_pass_draws != 0 && pass.draw_count == m_max_pass_draws && j < m_plan_draws.size() &&
				!m_plan_pending[j].break_before && key_of(m_plan_pending[j]) == key_of(first))
				m_frame.capped_passes++;

			m_plan_target_pairs.push_back(tp);
			m_plan_passes.push_back(pass);
			// The census's memory of the pass the walk is leaving. Under the gate with the census
			// itself: the key it builds is the walk's own comparison repeated, and nothing else reads
			// either of these.
			if (m_census_pass_breaks)
			{
				prev_key = key_of(first);
				prev_draws = pass.draw_count;
				have_prev = true;
			}
			i = j;
		}
		m_frame.passes += static_cast<u32>(m_plan_passes.size());
		if (m_census_pass_breaks)
		{
			// The census counts the passes the walk above built, or it is a census of something else.
			// Inside the gate with the census: with it off nothing counted a boundary, so the identity
			// would fail on a run that is perfectly correct.
			pxAssertMsg(m_frame.break_plans + m_frame.break_boundaries == m_frame.passes,
				"TileGpu's pass-break census counted a different number of passes than the plan build");
			// ...and the recut, which is a SECOND grouping walk over every draw of the plan. It is the
			// most expensive thing this key gates and the reason the key is a bitmask.
			CensusAdrenoPasses();
		}

		// What draw reordering would have done to this plan, taken by nothing. Gated, so the shipped
		// default pays not one bitmap intersection for it.
		if (m_reorder_census)
		{
			const u32 before = m_frame.reorder_passes;
			ReorderCensus();
			// The model's "as it stands" arm must reproduce the cut the grouping walk above just made,
			// or the pair it reports is one arrangement against a different question.
			pxAssertMsg(m_frame.reorder_passes - before == static_cast<u32>(m_plan_passes.size()),
				"TileGpu's reorder census counted the plan's own passes differently from the plan");
		}

		// The depth predictor's observation of this plan: how many passes THIS draw list cuts into
		// under each depth polarity, with everything else -- the cap, the reader segregation, every
		// draw's own break_before -- held exactly as it stands. One of the two is the cut that just
		// ran, and it is recounted rather than read off m_plan_passes.size() so both numbers come out
		// of the same loop over the same list and cannot differ by a bookkeeping accident.
		//
		// ⚠️ The counterfactual side is an ESTIMATE in one respect and it cannot be otherwise: a
		// draw's break_before was decided during accumulation, against the pass that was open under
		// the polarity actually in force. The other polarity would have had a different pass open and
		// so, for a handful of draws a frame, a different break_before. Measured on the M2 corpus the
		// residual is small enough to leave the metric's verdict unchanged on all 19 dumps; it is
		// named here because a reader deriving the metric from first principles would expect exactness.
		if (m_depth_predictor_census)
		{
			const u32 count = static_cast<u32>(m_plan_draws.size());
			const auto breaks_at = [this](u32 d) { return m_plan_pending[d].break_before; };
			// ⚠️ The PASS-key depth mode, not the pipeline one. Both counterfactuals have to see the
			// boundaries the grouping walk above actually made, or the predictor answers a question
			// about a cut nobody takes -- and the assert below, which recounts the active polarity
			// against the plan, is what would catch it. Before
			// EmuCore/GS/TileGpuSplitSharesPassKey the two were the same field.
			const auto key_under = [this](u32 d, bool depth_uniform) {
				const PendingDraw& pd = m_plan_pending[d];
				return gsTileGpuPassKeyFor(pd.color_surface, pd.z_surface, pd.pass_depth_mode,
					depth_uniform, pd.self_mask != 0, m_segregate_self_read);
			};
			const u32 under_uniform = gsTileGpuCountPasses(count, m_max_pass_draws,
				[&](u32 d) { return key_under(d, true); }, breaks_at);
			const u32 under_merged = gsTileGpuCountPasses(count, m_max_pass_draws,
				[&](u32 d) { return key_under(d, false); }, breaks_at);
			m_frame.passes_uniform_key += under_uniform;
			m_frame.passes_merged_key += under_merged;
			// ...and the same two sorted by which one RAN, off the polarity this plan was cut under.
			// No third walk: it is the pair above, read the other way round.
			m_frame.passes_active_key += m_depth_uniform_passes ? under_uniform : under_merged;
			m_frame.passes_other_key += m_depth_uniform_passes ? under_merged : under_uniform;
			m_frame.planned_draws += count;
			// The recount of the polarity that DID run must reproduce the plan, or the metric is
			// being read off a cut the executor never made.
			pxAssertMsg(static_cast<u32>(m_plan_passes.size()) ==
							(m_depth_uniform_passes ? under_uniform : under_merged),
				"TileGpu depth predictor recounted the active grouping differently from the plan");
		}

		// 3b. Backstop for the framebuffer-fits-its-attachments clamp. The executor renders each
		//     pass into min(colour, depth) of the pair (a framebuffer may not be bigger than the
		//     attachment it carries), so a draw whose bottom falls below that minimum height would
		//     be silently clipped. It cannot: both surfaces grew to cover every draw's footprint
		//     (EnsureSurface), so min(colour,depth) height >= every draw's bottom by construction,
		//     and this loop never fires. It is a guard against a future grouping change breaking
		//     that invariant. (Width is the layout's stride, shared across a pair by FBW.)
		for (const GSDevice::GSTileGpuPass& pass : m_plan_passes)
		{
			const GSDevice::GSTileGpuTargetPair& tp = m_plan_target_pairs[pass.first_target_pair];
			if (tp.zbuf_target == GSDevice::GSTileGpuPassPlan::kNoTarget)
				continue; // no depth attachment, no clamp
			const int lim_h = std::min(m_plan_targets[tp.frame_target]->GetSize().y,
				m_plan_targets[tp.zbuf_target]->GetSize().y);
			for (u32 d = pass.first_draw; d < pass.first_draw + pass.draw_count; d++)
			{
				if (m_plan_pending[d].rect.w > lim_h)
				{
					Console.Error("TileGpu: draw %u bottom %d exceeds pass target height %d -- clamp would clip it",
						d, m_plan_pending[d].rect.w, lim_h);
					pxAssertMsg(false, "TileGpu pass draw exceeds min(colour,depth) target height");
				}
			}
		}

		// 3c. What the executor will RECORD for the writeback ops emitted above, walked with the
		//     class it decides with. The op count is what the compose road emitted; this is what the
		//     command stream costs, which is the term a batching change moves and the op count does
		//     not. Census only -- the walk reads the finished plan and changes nothing in it.
		for (const GSDevice::GSTileGpuPass& pass : m_plan_passes)
		{
			const u32 first = std::min<u32>(pass.first_prep_op, static_cast<u32>(m_plan_prep_ops.size()));
			const u32 count = std::min<u32>(pass.prep_op_count, static_cast<u32>(m_plan_prep_ops.size()) - first);
			m_frame.writeback_dispatches += GSDevice::GSTileGpuWritebackBatch::CountDispatches(
				m_plan_prep_ops.data() + first, count, m_plan_page_entries.data());
			m_frame.writeback_runs += GSDevice::GSTileGpuWritebackBatch::CountRuns(
				m_plan_prep_ops.data() + first, count, m_plan_page_entries.data(),
				&m_frame.writeback_run_hazards);
		}

		// 4. The ring: one entry per (page, epoch range), its source bytes resolved now -- the
		//    CPU shadow as it stands at the end of the frame for a live slot, or the version copy
		//    taken when an upload superseded it. Slots nobody prefills are the GPU's to compose.
		std::vector<GSDevice::GSTileGpuRingPage> ring;
		ring.reserve(m_ring_entries.size());
		for (const RingEntry& e : m_ring_entries)
		{
			GSDevice::GSTileGpuRingPage rp = {};
			rp.page = e.page;
			rp.epoch_first = e.epoch_first;
			rp.epoch_last = (e.epoch_last == 0xFFFF) ? static_cast<u16>(m_epoch) : e.epoch_last;
			rp.src = !e.prefill_s ? nullptr
					 : e.versioned ? static_cast<const void*>(m_ring_versions.data() + e.version_offset)
								   : static_cast<const void*>(m_mem.vm8() + static_cast<u32>(e.page) * GS_PAGE_SIZE);
			if (rp.src)
				m_frame.ring_prefill++;
			ring.push_back(rp);
		}
		m_frame.epochs = std::max(m_frame.epochs, m_epoch + 1);

		// 5. Assemble the plan over the frame's streams and submit. The spans are CPU-side
		//    views alive until the clear below; the executor stages what it needs during the
		//    synchronous call.
		GSDevice::GSTileGpuPassPlan plan;
		plan.passes = m_plan_passes;
		plan.draws = m_plan_draws;
		plan.topologies = m_plan_topologies;
		plan.scissors = m_plan_scissors;
		plan.blend_keys = m_plan_blend_keys;
		plan.bind_keys = m_plan_bind_keys;
		// Withholding the stream is what the forced-vs-narrowed gate does: the executor's fallback is
		// each pass's UNION masks, which is exactly the program every draw used to run. Same plan, same
		// draws, same order -- only the fragment program each one compiles differs, so the two arms
		// have to come out byte-identical and a difference is a shader bug rather than a planner one.
		if (!GSConfig.TileGpuUnionFragmentVariant)
			plan.variant_keys = m_plan_variant_keys;
		plan.depth_modes = m_plan_depth_modes;
		plan.target_pairs = m_plan_target_pairs;
		plan.snapshots = m_plan_snapshots;
		plan.prep_ops = m_plan_prep_ops;
		plan.page_entries = m_plan_page_entries;
		plan.writeback_keep_masks = m_plan_writeback_keep_masks;
		plan.state_table = m_plan_states.data();
		plan.state_stride = sizeof(StateRow);
		plan.state_count = static_cast<u32>(m_plan_states.size());
		plan.vertices = std::span<const u8>(reinterpret_cast<const u8*>(m_plan_vertices.data()),
			m_plan_vertices.size() * sizeof(GSVertex));
		plan.vertex_stride = sizeof(GSVertex);
		plan.indices = m_plan_indices;
		plan.targets = m_plan_targets;
		plan.prep_textures = m_plan_prep_textures;
		plan.sources = m_plan_sources;
		plan.tex_sources = m_plan_tex_sources;
		plan.ring_pages = ring;
		plan.epoch_count = m_epoch + 1;
		plan.palettes = m_plan_palettes;
		// The frame's palette words, counted where the plan finally names them. A 16-bit gather
		// reserves twice what a 32-bit one does, so this is the column that says how close the
		// reservation is to the ring stream buffer it shares (32 MiB, with the ring slots, the epoch
		// tables, the page entries and the two mask tables, reserved as one block per plan).
		m_frame.clut_stream_words += static_cast<u32>(m_plan_palettes.size());

		g_gs_device->ExecuteTileGpuPassPlan(plan);
	}
	else if (!m_warned_no_executor)
	{
		// The device failed the TileGpu contract at construction, but the renderer was selected
		// and built anyway -- which now only happens under EmuCore/GS/TileGpuIgnoreDeviceContract,
		// because the shipped answer is to resolve to Classic instead (GSTileSelectionPolicy.h
		// carries the decision record). So this is a bring-up path, not the ordinary one. From here
		// every frame's plan is silently discarded: without this, that is a black frame and a
		// clean exit code, indistinguishable at every level above this one from working GS
		// output. Say it once, loudly, and name the log line that has the actual missing feature
		// (GSDeviceVK.cpp, device creation).
		m_warned_no_executor = true;
		Console.Error("TileGpu: device contract absent -- every draw is being DISCARDED, output "
					  "will be black. See the \"VK: TileGpu device contract absent (...)\" line "
					  "logged at device creation for which feature is missing.");
	}

	// The plan is submitted (or the device does not serve it); reset the per-frame streams and
	// the ring. Every synced claim referred to slots that are now spent.
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_palettes.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_scissors.clear();
	m_plan_blend_keys.clear();
	m_plan_bind_keys.clear();
	m_plan_variant_keys.clear();
	m_plan_depth_modes.clear();
	m_plan_pending.clear();
	m_plan_write_pages.clear();
	m_plan_read_pages.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_writeback_keep_masks.clear();
	m_plan_targets.clear();
	m_plan_prep_textures.clear();
	m_plan_sources.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	// The plan has been handed to the executor, so every cache entry its draws named is free
	// again: advance the pin clock, which is what releases them. This is the only place that may
	// do it -- a mid-frame flush ends a plan just as VSync does, and the pins have to clear with
	// the plan and not with the video frame.
	AdvanceSourcePinFrame();
	ResetOpenRuns();
	m_vram_model.ClearAllSynced();
	// The one place a device palette may be recycled. Here, and only here, both conditions Tile's
	// own predicate silently assumes are true by construction: there are no pending draws and every
	// recorded op has issued, so a palette the mirror no longer names is a palette nothing can still
	// be about to sample.
	PruneGpuPalettes();
}

// -- the stall road ------------------------------------------------------------------------

void GSRendererTileGpu::FlushPendingPlan()
{
	if (m_plan_pending.empty())
		return;
	m_frame.flushes++;
	BuildAndExecutePlan();
}

// Pull whatever of `pages` a target holds newest down into the CPU shadow. The targets must
// hold every draw so far first, so the pending plan is flushed (a mid-frame submission,
// counted) -- which also drops every ring-synced claim, so the question "what does S not
// have" is asked afterwards. Then the pool's synchronous readback per owner surface, masked to
// the planes and blocks each holds. The one road on which the CPU waits for the GPU.
void GSRendererTileGpu::ReadbackToShadow(const GSPageBitmap& pages, StallSite site)
{
	// Nothing any target holds newest: no crossing (a CLUT load or a local read of plain CPU
	// memory, the common case). Asked against truth rather than the unsynced subset, because a
	// ring-synced claim is not one S can serve.
	if (pages.empty() || (pages & m_vram_model.TruthAny()).empty())
		return;

	FlushPendingPlan();
	PullToShadow(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll), site);
}

// The pull itself, shared by both overloads: the plan is already flushed and the page set is
// already settled. The pool's synchronous readback per owner surface, masked to the planes and
// blocks each holds.
void GSRendererTileGpu::PullToShadow(const GSPageBitmap& need, StallSite site)
{
	if (need.empty())
		return;
	m_frame.stalls[static_cast<u32>(site)]++;
	m_frame.stall_pages[static_cast<u32>(site)] += need.count();

	// A CPU READ had to pull these pages: remember WHEN, so the upload merge stops keeping them on
	// the GPU while the mark is fresh. See m_cpu_read_frame -- the upload road is excluded because
	// that is the merge's own road and marking it would switch the merge off the first time it
	// declined a page, and the savestate sync is excluded because it pulls everything and means
	// nothing about what the game reads.
	if (site == StallSite::LocalRead || site == StallSite::Clut)
	{
		const u32 stamp = m_frame_index + 1;
		need.forEachSetPage([&](u32 page) { m_cpu_read_frame[page] = stamp; });
	}

	// One pool call per (owner surface, truth block mask) -- NOT one per plane, and NOT one per
	// page.
	//
	// A readback's price is the device round trip, not the bytes: every call ends in a
	// submit-and-wait, either a drain of the frame's command buffer or an out-of-band submit
	// plus its own fence, and the out-of-band one is a whole vkQueueSubmit and fence wait for
	// what is often a single 8 KB page. A colour target owns three planes and a colour/depth pair
	// owns four, so asking plane by plane downloads the SAME rect of the SAME image three or four
	// times, differing only in which bytes of the 32-bit cell the store is allowed to write; and
	// asking page by page pays a fresh round trip for every page of a set the pool would have
	// collected into runs of one image in one copy. Beyond Good & Evil's one local read a frame is
	// 30 pages and was 30 round trips; Armored Core 3 pulls 14.5 single pages a frame off one Z
	// buffer.
	//
	// The pool has always taken a page BITMAP and derived its runs from it -- exact-Tile's
	// ReadbackModelPages has bucketed whole page sets per owner since it was written. TileGpu
	// kept a per-page loop through both the plane merge and this, which is transcription drift,
	// not a decision.
	//
	// The merge is byte-identical by construction, on four legs:
	//   - same owner => the same source texture, and the same rect per page, since the block mask
	//     is part of the key and the rect is derived from it;
	//   - the write mask "selects the bytes of each 32-bit cell to write", so the union mask
	//     writes exactly the union of what the separate calls wrote, out of the same source;
	//   - on a 16-bit colour surface the pool narrows that mask to the 5551 cell, and the
	//     narrowing distributes over OR (GSTileTargetPool::NarrowWriteMaskTo16, pinned);
	//   - a wider page set only widens the DOWNLOAD rect (the union bounding box of the runs);
	//     the store still walks the same runs and writes the same guest bytes.
	//
	// ⚠️ ORDER is why the page merge is not unconditional. Two groups on ONE page must be issued in
	// plane order, because their byte windows can OVERLAP: a Z plane's PlaneByteMask is all four
	// bytes and an RGB plane's is the low three, so a page aliased by a colour owner and a depth
	// owner has the depth store covering the colour one. Bucketing such a page across pages can
	// float its second group ahead of its first (page P contributes only the depth group, page Q
	// both -- then the depth bucket exists first and Q's colour write lands after it).
	//
	// So a page whose pull is a SINGLE group joins the cross-page bucket for that group, and a
	// page with two or more keeps the per-page path, issued in plane order, exactly as before.
	// Pages are disjoint guest memory, so the two halves cannot interact with each other whatever
	// order they run in. Aliased pages are rare -- the alias-steal counter is zero on most of the
	// corpus -- and the coalescible ones are the whole population that matters.
	// Which ROAD each call took, attributed to the SITE that asked for it. The pool knows which road
	// it took and the site does not, so the two are joined here, the one place both are in scope --
	// by delta rather than by a return value, because the roads are internal to ReadbackPages and a
	// call that falls off the out-of-band road onto the drain must be counted as the drain it became.
	// A call takes at most one road, so the three buckets partition the calls exactly.

	// What must actually COME DOWN, which after the flush above is not the whole ask. A page the
	// CPU shadow already holds -- claimed by an earlier pull of this same road, and
	// written by neither side since -- is bytes we have; fetching them again buys nothing and costs
	// a full pipeline wait, because the readback's submit-and-wait waits on everything already
	// submitted. The ask itself stays whole on purpose: the stall census above, the
	// m_cpu_read_frame stamp and OnReadback below are all spent on `need`, and that stamp is the
	// upload merge's input. See EmuCore/GS/TileGpuShadowSurvivesFlush.
	const GSPageBitmap fetch = GSConfig.TileGpuShadowSurvivesFlush ? m_vram_model.ShadowMissing(need) : need;
	PlanPull(m_vram_model, fetch, m_pull_calls);
	const u32 site_index = static_cast<u32>(site);
	for (const PullCall& c : m_pull_calls)
	{
		const GSVramModel::Surface& surf = m_vram_model.Get(c.owner);
		const u32 oob_before = m_target_pool.OutOfBandCopies();
		const u32 drains_before = m_target_pool.Drains();
		// The pull's WIDTH, taken the same way its ROAD is: by delta off the pool's counters, because
		// the rectangle a call downloads is the union bounding box of runs the pool derives inside
		// ReadbackPages and nothing out here can name it. A call that reaches the device for nothing
		// moves neither counter and is charged neither pages nor texels.
		const u64 dl_pages_before = m_target_pool.ReadbackPageCount();
		const u64 dl_texels_before = m_target_pool.ReadbackTexels();
		m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, c.pages, c.write_mask, c.block_mask);
		m_frame.pull_dl_pages += static_cast<u32>(m_target_pool.ReadbackPageCount() - dl_pages_before);
		m_frame.pull_dl_texels += static_cast<u32>(m_target_pool.ReadbackTexels() - dl_texels_before);
		if (m_target_pool.OutOfBandCopies() != oob_before)
			m_frame.pull_oob[site_index]++;
		else if (m_target_pool.Drains() != drains_before)
			m_frame.pull_drain[site_index]++;
		else
			m_frame.pull_idle[site_index]++;
	}
	m_frame.pull_calls += static_cast<u32>(m_pull_calls.size());
	m_vram_model.OnReadback(need);
	// The pull's own receipt, and the one claim in the model that means the CPU really has the
	// bytes rather than that some fiction was needed to satisfy the steal invariant. Recorded for
	// the WHOLE ask, not for what was fetched: the pages the claim already served are in the shadow
	// too. Maintained on both arms of the knob -- only whether it is HONOURED above is gated -- so
	// the bitmap and its invariant get the same exercise either way.
	m_vram_model.OnShadowFilled(need);
}

void GSRendererTileGpu::PlanPull(const GSVramModel& model, const GSPageBitmap& need, std::vector<PullCall>& out)
{
	out.clear();

	struct PlaneGroup
	{
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 block_mask = 0;
		u32 write_mask = 0;
	};
	// Where the coalescible pages of each key are accumulating, as an index into `out`. Bounded by
	// the distinct keys the page set actually produces rather than by a constant, for the reason
	// GSTileComposeBuckets carries: MGS3 gathers 49 owners at once elsewhere, and a fixed table
	// there dropped the ninth silently and served every later reader stale memory.
	std::vector<size_t> buckets;

	need.forEachSetPage([&](u32 page) {
		std::array<PlaneGroup, kGSTilePlaneCount> groups;
		u32 group_count = 0;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!model.Truth(pi).test(page) || model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface);
			const GSVramModel::Surface& surf = model.Get(owner);
			if (!surf.pool_handle)
				continue;
			const u32 block_mask = model.TruthMask(page, pi);
			u32 g = 0;
			for (; g < group_count; g++)
			{
				if (groups[g].owner == owner && groups[g].block_mask == block_mask)
					break;
			}
			if (g == group_count)
			{
				groups[g].owner = owner;
				groups[g].block_mask = block_mask;
				groups[g].write_mask = 0;
				group_count++;
			}
			groups[g].write_mask |= PlaneByteMask(pi, surf.layout);
		}

		if (group_count == 1)
		{
			for (const size_t bi : buckets)
			{
				if (out[bi].owner == groups[0].owner && out[bi].block_mask == groups[0].block_mask &&
					out[bi].write_mask == groups[0].write_mask)
				{
					out[bi].pages.set(page);
					return;
				}
			}
			buckets.push_back(out.size());
			out.push_back(PullCall{groups[0].owner, groups[0].block_mask, groups[0].write_mask, GSPageBitmap()});
			out.back().pages.set(page);
			return;
		}

		// Two or more calls on one page: they stay this page's own, in plane order, and join no
		// bucket. Their byte windows can overlap, so the later one is meant to land on top.
		for (u32 g = 0; g < group_count; g++)
		{
			out.push_back(PullCall{groups[g].owner, groups[g].block_mask, groups[g].write_mask, GSPageBitmap()});
			out.back().pages.set(page);
		}
	});
}

// The same road, for a caller whose ask is a RECT -- a local->host transfer, a move's source,
// a CLUT load. Only the question about WHAT to pull is sharper.
//
// The page-granular overload cannot see which blocks of a page the read wants, and that costs
// whole readbacks. A CLUT load reads 256 bytes, and what games do with a palette is park it in
// the blocks past the bottom row of a render target's last page -- the unused tail of the
// framebuffer. The page belongs to a target; not one block of the read does. Asking
// page-granularly there flushes the plan and pulls every GPU-newest block of the page down so
// that a loader can read bytes the target never wrote: on this corpus that is EVERY CLUT
// readback that costs anything.
//
// `GSVramModel::ReadbackNeeded`'s rect overload already refines through the block sidecar and
// counts what it refutes -- exact-Tile has taken that road since it was written. This one
// simply had no way to hand it a rect.
void GSRendererTileGpu::ReadbackToShadow(const GSTileSurfaceLayout& layout, const GSVector4i& r,
	const GSPageBitmap& pages, StallSite site)
{
	// The cheap question first, so a read of plain CPU memory still costs one bitmap AND and
	// builds no footprint: sotc runs ~1789 CLUT loads a frame that need nothing at all, and
	// that has to stay free.
	if (pages.empty() || (pages & m_vram_model.TruthAny()).empty())
		return;

	// The block question, asked BEFORE the flush as a pure "nothing here whatever the flush does"
	// gate (EmuCore/GS/TileGpuFlushGateBlockAsk). FootprintForRect is a pure function of the layout
	// and the rect, so building it on this side of the flush changes nothing but the order.
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(layout, r, fp);
	if (GSConfig.TileGpuFlushGateBlockAsk &&
		m_vram_model.ReadbackNeeded(fp, kGSTilePlanesAll, /*ignore_synced=*/true).empty())
	{
		return;
	}

	FlushPendingPlan();
	PullToShadow(m_vram_model.ReadbackNeeded(fp, kGSTilePlanesAll), site);
}

// Whole-of-truth: every page any target holds newest comes down. Synced claims are dropped
// first because on this road "synced" may mean the ring holds the bytes, and the ring is not
// the CPU shadow.
//
// The CLUT is the one piece of truth this does NOT reach on its own. A palette the gather served
// lives in a target's texture and in the sixteen-slot mirror, and the CPU's CLUT RAM holds whatever
// the loader read out of a stale shadow for those slots -- so pulling every page down leaves the
// palette itself wrong. SyncClutToCpu is what makes those words whole (exact-Tile's own contract),
// and it runs first, because it reads pages back through the same road.
void GSRendererTileGpu::SyncAllTruthToCpu()
{
	SyncClutToCpu();
	m_vram_model.ClearAllSynced();
	ReadbackToShadow(m_vram_model.TruthAny(), StallSite::SyncAll);
}

// -- the CPU->GPU upload merge --------------------------------------------------------------------
//
// The road that used to be the ONLY reason five corpus dumps ever blocked on the device. An upload
// covering part of a block a target holds newest leaves that block's bytes split -- some the CPU's
// new ones, the rest still the target's -- and GS truth is tracked per block, so somebody has to
// merge them. Doing it on the CPU means the target's half has to come down first, which is a submit
// and a fence wait per spill: 2.88 a frame on Shadow of the Colossus, and under the serialization
// law one such wait costs the whole frame's overlap however few there are.
//
// So the merge goes the other way, and needs no wait, because the ring already moves bytes in both
// directions:
//
//   1. the page takes a ring slot prefilled from the CPU shadow. The prefill is a memcpy the
//      EXECUTOR does when the plan is built, and GSState writes this transfer into the shadow
//      immediately after InvalidateVideoMem returns -- so by then the slot's source already holds
//      the CPU's new bytes;
//   2. a WRITEBACK of the owner composes the rest of the page out of its texture, under a per-block
//      keep mask naming the bytes the transfer wrote, so the CPU's half survives the read-modify-
//      write instead of being overwritten by a byte the target no longer owns;
//   3. a SEED reads the merged bytes back into the owner's texture -- only the blocks it holds, so
//      no texel of it changes that the transfer had nothing to do with;
//   4. truth does not move AT ALL. The owner still holds exactly the blocks it held, whose newest
//      bytes are now in its texture; the shadow still holds exactly the rest. Nothing was pulled
//      down and the model records only the write generation.
//
// The ops are one range on a draw-less pending draw (AppendPrepOnlyDraw), which puts them at a pass
// head behind every draw recorded so far and ahead of every draw after. That IS the transfer's
// sequence point, and it is why the merge does not need to know anything about the frame's pass
// structure.
//
// ⚠️ What it CANNOT keep is the favour the old road did on the side: draining a page also handed
// truth back, so every later CPU read of it was free. See m_cpu_read_pages for what that cost and
// what the merge does about it.

namespace
{
/// Where writing one texel of `psm` lands inside the unit its pixel address names, as a BIT range:
/// the unit's width, the first bit of it the write touches, and how many. This is GSLocalMemory's
/// own WritePixel family read as coverage rather than as a value -- WritePixel24 keeps 0xff000000,
/// WritePixel8H writes bits 24-31, WritePixel4HL bits 24-27 -- and the 4-bit case needs no special
/// handling here because its pixel address is already a nibble address.
struct UploadUnitBits
{
	u32 unit_bits;
	u32 bit_offset;
	u32 bit_count;
};

bool UploadUnitBitsFor(u32 psm, UploadUnitBits& out)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMZ32: out = {32, 0, 32}; return true;
		case PSMCT24:
		case PSMZ24: out = {32, 0, 24}; return true;
		case PSMT8H: out = {32, 24, 8}; return true;
		case PSMT4HL: out = {32, 24, 4}; return true;
		case PSMT4HH: out = {32, 28, 4}; return true;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S: out = {16, 0, 16}; return true;
		case PSMT8: out = {8, 0, 8}; return true;
		case PSMT4: out = {4, 0, 4}; return true;
		default: return false;
	}
}
} // namespace

// See the declaration.
bool GSRendererTileGpu::BuildUploadByteMask(const GSTileSurfaceLayout& layout, const GSVector4i& r,
	const GSPageBitmap& pages, std::vector<UploadPageBytes>& out, std::vector<u32>& words)
{
	out.clear();
	words.clear();
	UploadUnitBits unit;
	if (r.rempty() || pages.empty() || !UploadUnitBitsFor(layout.psm, unit))
		return false;

	// What one WORD of a fully written block looks like, as a 4-bit byte mask. Every format packs a
	// whole number of units into a 32-bit word, so this is the same for every word of every block:
	// 0xF for the formats that write whole units, 0x7 for the 24-bit ones (the alpha byte is not
	// theirs) and 0x8 for PSMT8H.
	u32 full_bits = 0;
	for (u32 u = 0; u < 32 / unit.unit_bits; u++)
	{
		const u32 span = (unit.bit_count >= 32) ? ~0u : ((1u << unit.bit_count) - 1u);
		full_bits |= span << (u * unit.unit_bits + unit.bit_offset);
	}
	u32 full_byte_bits = 0;
	for (u32 b = 0; b < 4; b++)
	{
		const u32 byte = (full_bits >> (b * 8)) & 0xFFu;
		// A format whose texel owns half a byte can never be merged, whatever the rect: the
		// writeback masks at byte granularity, so a byte the CPU owns one nibble of has no right
		// answer -- writing it loses the CPU's nibble, skipping it loses the target's. PSMT4HL and
		// PSMT4HH are exactly that, always. (PSMT4 is not: its texels pair into whole bytes, and
		// whether a given rect's do is the per-block question below.)
		if (byte != 0 && byte != 0xFFu)
			return false;
		full_byte_bits |= (byte != 0) ? (1u << b) : 0u;
	}
	const u32 full_word = full_byte_bits * 0x11111111u;

	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	const GSVector2i bs(1 << off.blockShiftX(), 1 << off.blockShiftY());
	const int bx0 = (r.left / bs.x) * bs.x;
	const int by0 = (r.top / bs.y) * bs.y;

	// Two bits a byte: which halves of this block's 256 bytes the write covers. Only the 4-bit
	// formats can ever set one and not the other, and that is what makes a rect unmergeable.
	std::array<u8, 256> half{};

	for (int by = by0; by < r.bottom; by += bs.y)
	{
		for (int bx = bx0; bx < r.right; bx += bs.x)
		{
			const u32 bn = off.bn(bx, by);
			const u32 page = bn >> 5;
			if (!pages.test(page))
				continue;
			const u32 bib = bn & 31;

			UploadPageBytes* pb = nullptr;
			for (UploadPageBytes& e : out)
			{
				if (e.page == page)
				{
					pb = &e;
					break;
				}
			}
			if (!pb)
			{
				// A whole page's table, not just its touched blocks: it costs a kilobyte on a spill
				// that happens a couple of times a frame, and it makes the block index the shader
				// already has the whole lookup -- no ranking, no popcount, nothing to get wrong.
				out.push_back(UploadPageBytes{static_cast<u16>(page), 0, 0, static_cast<u32>(words.size())});
				words.resize(words.size() + GSDevice::kGSTileGpuKeepMaskWordsPerPage, 0u);
				pb = &out.back();
			}
			u32* const blk = words.data() + pb->first_word + bib * 8;
			pb->blocks |= 1u << bib;

			// A footprint big enough to wrap GS memory revisits blocks, so everything here ORs.
			if (bx >= r.left && (bx + bs.x) <= r.right && by >= r.top && (by + bs.y) <= r.bottom)
			{
				for (u32 w = 0; w < 8; w++)
					blk[w] |= full_word;
				continue;
			}

			// The block the rect only enters: texel by texel, so it claims the bytes it lands on
			// and not one more. Bounded by the block, which is 512 texels at its largest.
			half.fill(0);
			const int x0 = std::max(bx, r.left), x1 = std::min(bx + bs.x, r.right);
			const int y0 = std::max(by, r.top), y1 = std::min(by + bs.y, r.bottom);
			for (int y = y0; y < y1; y++)
			{
				const GSOffset::PAHelper pa = off.paMulti(0, y);
				for (int x = x0; x < x1; x++)
				{
					const u32 bit0 = pa.value(x) * unit.unit_bits + unit.bit_offset;
					pxAssert((((bit0 >> 3) >> 8) & 31) == bib);
					for (u32 b = 0; b < unit.bit_count; b += 4)
						half[((bit0 + b) >> 3) & 255] |= static_cast<u8>(1u << (((bit0 + b) >> 2) & 1));
				}
			}
			for (u32 byte = 0; byte < 256; byte++)
			{
				if (half[byte] == 0)
					continue;
				if (half[byte] != 3)
				{
					// Half a byte, so no byte mask expresses it: see the format check above. Leaving
					// a part-built table behind would be a mask a caller could still read.
					out.clear();
					words.clear();
					return false;
				}
				blk[byte >> 5] |= 1u << ((((byte >> 2) & 7) * 4) + (byte & 3));
			}
		}
	}

	for (UploadPageBytes& e : out)
	{
		const u32* const table = words.data() + e.first_word;
		for (u32 b = 0; b < 32; b++)
		{
			if (!(e.blocks & (1u << b)))
				continue;
			bool whole = true;
			for (u32 w = 0; w < 8; w++)
				whole = whole && table[b * 8 + w] == 0xFFFFFFFFu;
			if (whole)
				e.blocks_whole |= 1u << b;
		}
	}
	return true;
}

// See the declaration. Every clause is a proof that the ring slot this merge composes will hold the
// page's real bytes, because the seed writes that slot straight into the owner's texture and the
// model goes on saying the owner holds those blocks -- so a slot that is wrong anywhere is a wrong
// pixel no later read can correct.
GSPageBitmap GSRendererTileGpu::PlanUploadMerge(const GSTileSurfaceLayout& layout, const GSVector4i& r,
	const GSVramModel::RectFootprint& fp, const GSPageBitmap& spill)
{
	m_merge_groups.clear();
	m_merge_bytes.clear();
	m_merge_words.clear();
	if (spill.empty() || GSConfig.TileGpuUploadSpillReadback)
		return GSPageBitmap();
	if (!m_upload_writes_whole_rect)
	{
		// The rect is a CLAIM (a sliced transfer hands the whole image to every slice; a truncated
		// one hands a guess), and the keep mask has to be a RECORD -- a byte it names that the
		// shadow does not actually receive is a byte the writeback skips and nobody writes.
		m_frame.merge_ref.slice++;
		return GSPageBitmap();
	}
	if (fp.overflowed)
	{
		// No block masks, so there is no telling which blocks the write covers whole.
		m_frame.merge_ref.overflow++;
		return GSPageBitmap();
	}
	// Past all three, so the hoisted form of them must agree. The two directions are not
	// symmetric and only one of them is dangerous: a refusal ADDED above and not to the hoist
	// leaves the hoist a superset, which costs a probe that gets thrown away -- today's price. A
	// refusal REMOVED from above and not from the hoist makes the gate skip a probe that would
	// now be accepted, which is a silently lost elision and shows up only as a slower frame.
	// That is this direction, and this is where it is caught.
	pxAssert(UploadMergeServesThisWrite(fp));

	GSPageBitmap take;
	const u32 now = m_frame_index + 1;
	spill.forEachSetPage([&](u32 page) {
		// THE OWNER CLAUSES FIRST, for every page, and only then the CPU-read one -- which reads
		// backwards and is deliberate. The CPU-read test decides nothing the owner clauses can undo,
		// so the order it is ASKED in is free, and asking it last means a page it refuses can still
		// say what it would have met. That is the only way to price lifting it: today the clause
		// returns, so MergeRefusals::owner is zero over the refused population by construction rather
		// than by measurement.
		//
		// ONE live surface must hold every plane of the page that has truth at all. Not a
		// simplification: the compose can only carry a plane whose owner has a writeback shader, and
		// it marks the page composed either way -- so a page whose Z a depth buffer holds would get
		// the stale shadow's bytes for the Z half, the seed would write that into the colour target's
		// cells, and the model would go on saying the ring has the page. Every plane one owner, or
		// the blocking road, which moves all of it.
		u32 next = kMergeRefNone;
		GSTileSurfaceId owner = kGSTileNoSurface;
		bool split = false;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			const GSTileSurfaceId o = m_vram_model.OwnerOf(page, pi);
			if (o == kGSTileNoSurface)
				continue;
			split = split || (owner != kGSTileNoSurface && o != owner);
			owner = o;
		}
		if (split || owner == kGSTileNoSurface)
		{
			next = kMergeRefOwner;
		}
		else
		{
			const GSVramModel::Surface& surf = m_vram_model.Get(owner);
			// A byte road both ways (the writeback that composes the page and the seed that reads it
			// back), a pool texture to run them against, and a texture rectangle that actually spans
			// the page -- the seed's scissor comes off the page's row and column in that rectangle.
			// The texel record is read WITHOUT Texels()'s grow, so a page whose owner has no record
			// yet answers the same "no texels" it would have, and asking about a refused page costs
			// nothing.
			if (!surf.pool_handle || !HasByteRoad(surf.layout) || owner >= m_surface_texels.size() ||
				!m_surface_texels[owner].pages.test(page))
			{
				next = kMergeRefRoad;
			}
		}

		// A page a CPU read has recently had to pull goes back down the blocking road: see
		// m_cpu_read_frame for the measurement that put this clause here, and
		// TileGpuMergeCpuReadWindow for how long "recently" is.
		//
		// ⚠️ So the two counters this branch feeds -- the "would refuse next" split and the mark-age
		// histogram -- describe what the window STILL refuses, not the whole marked population. The
		// monotone arm (window >= 65535) is what reports the latter, which is what makes the pair an
		// A/B rather than two unrelated numbers.
		const u32 mark = m_cpu_read_frame[page];
		if (gsTileGpuMergeRefusesForCpuRead(mark, now, m_merge_cpu_read_window))
		{
			m_frame.merge_ref.cpu++;
			m_frame.merge_ref.cpu_next[next]++;
			m_frame.merge_ref.cpu_age[gsTileGpuMergeCpuAgeBucket(now - mark)]++;
			return;
		}
		if (next != kMergeRefNone)
		{
			m_frame.merge_ref.owner++;
			return;
		}
		take.set(page);
		for (UploadMergeGroup& g : m_merge_groups)
		{
			if (g.owner == owner)
			{
				g.pages.set(page);
				return;
			}
		}
		m_merge_groups.push_back(UploadMergeGroup{owner, GSPageBitmap()});
		m_merge_groups.back().pages.set(page);
	});
	if (take.empty())
		return take;

	if (!BuildUploadByteMask(layout, r, take, m_merge_bytes, m_merge_words))
	{
		m_frame.merge_ref.bytes++;
		m_merge_groups.clear();
		m_merge_bytes.clear();
		m_merge_words.clear();
		return GSPageBitmap();
	}
	return take;
}

// See the declaration.
void GSRendererTileGpu::EmitUploadMerge()
{
	if (m_merge_groups.empty())
		return;

	// The keep tables move into the plan here, once, and the entries below name them by their
	// position in it.
	m_merge_keep_base = static_cast<u32>(m_plan_writeback_keep_masks.size());
	m_plan_writeback_keep_masks.insert(
		m_plan_writeback_keep_masks.end(), m_merge_words.begin(), m_merge_words.end());

	for (const UploadMergeGroup& g : m_merge_groups)
	{
		const u32 first_op = static_cast<u32>(m_plan_prep_ops.size());
		// The compose stages the pages from the shadow and writes the owner's remaining bytes over
		// them. Every page here is unsynced truth of `g.owner` (PlanUploadMerge proved the owner and
		// the spill question proved the truth), so the writeback is always emitted -- a page that
		// composed nothing would be seeded straight from the prefill, which is the shadow, which is
		// stale exactly where the owner holds it.
		pxAssert(!m_vram_model.ReadbackNeeded(g.pages, kGSTilePlanesAll).empty());
		m_merge_emitting = true;
		ComposeRingPages(g.pages);
		m_merge_emitting = false;
		// The seed, narrowed to the blocks the owner holds on each page. Not a whole-page seed:
		// the rest of the page is the CPU's, its bytes are already right in the byte store, and
		// writing them over this surface's texels would change texels nothing asked about --
		// measured as the whole of the difference this road made to the frame before it was
		// narrowed (6132 bytes of one SotC page, all of them blocks the target does not hold).
		//
		// TileGpuMergeSeedBatch decides whether that is ONE render pass for the group or one per
		// page. The bytes are the same either way -- see MergeSeedMaskFor, which is the two roads'
		// only difference and is where the equality is tested.
		m_merge_seeding = true;
		if (m_merge_seed_batch)
		{
			// The pages whose blocks are the owner's, and what those blocks are. Gathered first
			// because the op names them all at once; a page the owner holds nothing of is left out
			// of the bitmap rather than handed over with a zero mask.
			GSPageBitmap seed_pages;
			g.pages.forEachSetPage([&](u32 page) {
				u32 own_blocks = 0;
				for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
				{
					if (m_vram_model.OwnerOf(page, pi) == g.owner)
						own_blocks |= m_vram_model.TruthMask(page, pi);
				}
				if (own_blocks == 0)
					return;
				m_merge_seed_blocks[page] = MergeSeedMaskFor(true, own_blocks).entry_blocks;
				seed_pages.set(page);
			});
			if (!seed_pages.empty())
			{
				EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, g.owner, seed_pages,
					MergeSeedMaskFor(true, 0).op_blocks, m_merge_seed_blocks.data());
				seed_pages.forEachSetPage(
					[&](u32 page) { Texels(g.owner).filled.Mark(page, m_merge_seed_blocks[page]); });
			}
		}
		else
		{
			g.pages.forEachSetPage([&](u32 page) {
				u32 own_blocks = 0;
				for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
				{
					if (m_vram_model.OwnerOf(page, pi) == g.owner)
						own_blocks |= m_vram_model.TruthMask(page, pi);
				}
				if (own_blocks == 0)
					return;
				GSPageBitmap one;
				one.set(page);
				EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, g.owner, one,
					MergeSeedMaskFor(false, own_blocks).op_blocks);
				Texels(g.owner).filled.Mark(page, own_blocks);
			});
		}
		m_merge_seeding = false;
		// The compose always emits at least the owner's writeback (the assert above), so the range is
		// never empty -- and if it ever were, the ops would be inside no pass and would silently not
		// run while the model below recorded the reconciliation as done.
		const bool carried = AppendPrepOnlyDraw(g.owner, GSVector4i::zero(), first_op, g.pages);
		pxAssertMsg(carried, "TileGpu upload merge emitted no prep op to carry");
		if (!carried)
			continue;

		// ⚠️ And the model move is NOTHING, which is the whole point and is worth stating plainly.
		// The owner held blocks T of this page and the CPU held the rest. The transfer's bytes have
		// just gone into the owner's texture for the blocks of T it lands in, so the owner still
		// holds the newest bytes of exactly T; the blocks outside T got their new bytes in the
		// shadow, which still holds the newest bytes of exactly those. Truth is unchanged, and
		// InvalidateVideoMem's OnCpuWrite skips these pages for that reason.
		//
		// Claiming the WHOLE page for the owner instead would also be true -- the seed writes every
		// texel of it -- but it is a bigger move than the transfer justifies, and it is not free:
		// road selection is a function of the model, so handing the owner blocks it did not hold
		// makes later reads eligible for target and donor roads they were not eligible for before.
		m_frame.merge_ops++;
		m_frame.merge_pages += g.pages.count();
		m_frame.seed_breaks++;
	}

	m_merge_groups.clear();
	m_merge_bytes.clear();
	m_merge_words.clear();
}
