// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileClutMirror.h"
#include "GS/Renderers/Tile/GSTileExpandedCache.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"
#include "GS/Renderers/Tile/GSTilePassSim.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTargetPool.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The GS-on-GPU backend (GSHWRendererVariant::TileGpu): pass-planned indirect submission
// with VRAM truth on the GPU. Design: umbrella devs/bmdhacks/gs-on-gpu-design-brief-2026-08-17
// and gs-on-gpu-vram-model-2026-08-18 (the memory model).
//
// Deliberately a plain GSRenderer subclass, not GSRendererSW — Tile inherits the software
// renderer solely for its byte-exact software floor, and this design has no floor. It still
// gets GSState's register/PCRTC/savestate machinery, and will override Transfer<n> when the
// flat front end replaces the shipping GIF decode. It reuses Tile's page/hazard models
// (GSVramModel and friends) as libraries; it does not share Tile's per-draw delivery road.
//
// The memory model, in one paragraph. Guest bytes have three homes: the CPU shadow S (GSState's
// local memory, CPU-write-only), the frame's ring B (8 KB slots the GPU composes and reads,
// GPU-write-only past the executor's prefill), and the resident targets I (the target pool's
// textures, keyed by layout, PERSISTING across frames -- the frame boundary syncs nothing).
// GSVramModel says, per page and plane, which of S or some target holds the newest bytes; here
// "synced" means the frame's ring holds a page's byte truth, so a reader can take it from there.
// Every draw asks the model: pages its target does not hold newest are SEEDED into the target
// out of the ring first (bytes -> image); pages a texture read needs that some target holds
// newest are WRITTEN BACK into the ring first (image -> bytes); the ring slot for a page is
// prefilled from S when any of its bytes are CPU-newest. Both are GPU dispatches the plan
// carries to the executor in stream order. Uploads shrink or clear a target's claim (Tile's
// block sidecar), or, when they would overwrite bytes only a target holds at sub-block
// granularity, flush the plan and pull the page down first (the one stall road, counted).
// The GPU never reads S; the CPU never reads B; program order plus the ring is the whole
// synchronisation story.
//
// One plane's state on one page, as the ring's compose sees it: who holds the newest bytes,
// which of the page's 32 blocks they hold, whether the ring already carries them, and which bytes
// of a cell that owner's writeback shader actually writes.
struct GSTileRingPlaneState
{
	GSTileSurfaceId owner = kGSTileNoSurface;
	u32 truth_mask = 0; ///< GSVramModel::TruthMask for this page/plane (0 = no truth)
	bool synced = false; ///< the ring already holds these bytes
	/// gsTileWritebackByteMask for the owner's layout, or 0 when the owner has no writeback shader
	/// at all. One field rather than a "has a road" bool beside a mask, because the two would be one
	/// fact spelled twice and free to disagree.
	u32 write_bytes = 0;

	bool HasByteRoad() const { return write_bytes != 0; }
};

/// The blocks of a page that the writebacks ComposeRingPages is about to emit will actually
/// compose, PER BYTE LANE of the 32-bit cell: `out[b]` is the union of the block masks of the
/// writers whose writeback writes byte `b`.
///
/// This is EmitPrepOp's rule, stated once so the two cannot drift: an owner joins the compose
/// when it holds UNSYNCED truth on some plane and has a byte road, and each owner that joins
/// writes back the union of the truth masks of EVERY plane it owns on the page -- masked, within
/// each cell, to the bytes its format stores.
///
/// Per lane and not per block, because a PSMCT24 surface composes a page's 32 blocks whole and
/// still writes only three bytes in four. A (block, byte) pair outside every writer's reach is one
/// the slot must carry from its prefill, or the draw reads whatever the previous tenant of that
/// ring offset left there -- guest bytes from another page on a desktop allocator, arbitrary
/// content on one that recycles foreign pages.
void gsTileComposableBlocksPerByte(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount], u32 (&out)[4]);

/// The blocks ANY of those writebacks touches: the fold of the above over the four lanes. A
/// separate spelling only so the two cannot drift -- callers that care about block reach and
/// callers that care about byte reach read one computation.
u32 gsTileComposableBlocks(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount]);

/// Whether those writebacks reach EVERY byte of the page: every lane, every block. The exact
/// condition under which a ring slot may go unprefilled.
bool gsTileComposeCoversWholePage(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount]);

/// Whether a compose of this page will put the page's REAL guest bytes in its ring slot.
///
/// GSTileComposeBuckets::Gather's lossy test, stated once for the readers who have to decide
/// something before the compose runs rather than count afterwards. A plane with no unsynced truth
/// costs nothing -- the slot's prefill from the CPU shadow already carries it -- and a plane whose
/// unsynced truth an owner WITH a byte road holds is composed by that owner's writeback. What is
/// left is an owner with no writeback shader (a depth surface, or a base that is not page-aligned):
/// its bytes never reach the slot, the prefill's stale ones stay, and the model marks the page
/// synced anyway. That page is `lossy`, and a reader that would OVERWRITE something with what it
/// finds there must not read it -- which is the difference between the depth seed being page-precise
/// and it being the blanket clear that healed BGE and broke OutRun.
///
/// ⚠️ Ask BEFORE ComposeRingPages. It marks everything it was given synced, lossy pages included, so
/// afterwards the question answers `true` for pages it just answered `false` for.
bool gsTilePageByteTruthReachable(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount]);

/// The depth pipeline variant a draw takes, from the three depth facts the planner derives out of
/// ZTE/ZTST/ZMSK and the alpha test's AFAIL fold. GS depth grows towards the viewer, so a real test
/// is GEQUAL and a write-only draw is ALWAYS, and the write follows ZMSK independently of the test.
/// z_used == (z_write || z_test) at every call site.
///
/// One function rather than an expression repeated per call site: the derivation is asked for by
/// the pass key, by the per-draw plan array the executor picks its pipeline from, and by anything
/// that wants to name a draw's depth variant, and two of those disagreeing would bind a pipeline
/// that tests depth for a draw that must not.
constexpr GSDevice::GSTileGpuDepthMode gsTileGpuDepthModeFor(bool z_used, bool z_test, bool z_write)
{
	if (!z_used)
		return GSDevice::GSTileGpuDepthMode::None;
	if (!z_test)
		return GSDevice::GSTileGpuDepthMode::WriteAlways;
	return z_write ? GSDevice::GSTileGpuDepthMode::TestWrite : GSDevice::GSTileGpuDepthMode::TestNoWrite;
}

/// What a draw must share with the pass ahead of it to join it rather than open a new one.
///
/// Two places group draws into passes -- accumulation, which needs to know the open pass while it
/// is still deciding whether a prep op may be hoisted, and the plan build, which cuts the finished
/// draw list into GSTileGpuPass ranges. They must produce the same boundaries or a draw's prep ops
/// land in a different pass than the draw, so both ask this one type instead of each spelling the
/// predicate out.
struct GSTileGpuPassKey
{
	GSTileSurfaceId color = kGSTileNoSurface;
	GSTileSurfaceId depth = kGSTileNoSurface; ///< compared only when depth_used
	bool depth_used = false;
	/// The depth pipeline variant, but only where the device asked for depth-uniform passes --
	/// None everywhere else, so the field falls out of the comparison. See gsTileGpuPassKeyFor.
	GSDevice::GSTileGpuDepthMode keyed_depth_mode = GSDevice::GSTileGpuDepthMode::None;
	/// Whether this draw reads its own destination, but only where the device asked for the readers
	/// to be segregated -- false everywhere else, so the field falls out of the comparison. An
	/// independent dimension from the depth one: Adreno keys both. See gsTileGpuPassKeyFor.
	bool keyed_self_read = false;

	constexpr bool operator==(const GSTileGpuPassKey& o) const
	{
		return color == o.color && depth_used == o.depth_used && (!depth_used || depth == o.depth) &&
			   keyed_depth_mode == o.keyed_depth_mode && keyed_self_read == o.keyed_self_read;
	}
	constexpr bool operator!=(const GSTileGpuPassKey& o) const { return !(*this == o); }
};

/// The pass key of a draw rendering into `color` and `depth` with depth variant `mode`.
///
/// The depth attachment's PRESENCE is `mode != None` rather than a separate argument: the two are
/// the same fact. gsTileGpuDepthModeFor returns None exactly when the draw neither tests nor writes
/// depth, which is exactly when the pass needs no depth attachment. That half of the key is not
/// negotiable -- a Vulkan render pass cannot gain or lose an attachment without ending.
///
/// `depth_uniform_passes` is the negotiable half, and it is the device's call rather than the
/// planner's, because the two costs it trades sit on opposite sides of the hardware:
///
///  - Keeping the depth WRITE-ENABLE and COMPARE-ENABLE out of the key merges runs of draws that
///    differ only in those two bits into one pass, and the executor rebinds the depth pipeline per
///    indirect run instead. On a tiler a pass boundary is a tile-buffer resolve and reload -- ~50 us
///    on an M2 -- and the merge costs no indirect calls at all, because a pass boundary already
///    ended a run and this only moves where the cut is made (measured: identical calls/frame on all
///    five dumps). So the merge is worth 3-7x on scenes where the alpha test's all-fail fold drops
///    hundreds of depth writes a frame -- Shadow of the Colossus, 574.95 passes a frame merged back
///    down to 76.95 and 34.7 ms down to 10.5.
///  - Keeping them IN the key costs those passes but leaves every pass depth-uniform, and some
///    hardware measures faster that way. On Adreno 650 the merge is a net LOSS on every dump tried,
///    while the pass-count deltas match the model to the pass -- the passes really are saved and
///    something else more than eats them. No mechanism is claimed; the trade is measured.
///
/// So the planner asks the device (GSDevice::TileGpuPrefersDepthUniformPasses) and takes the answer
/// through here. Both grouping sites read the one bit, and the executor's run cut is unconditional:
/// under depth-uniform passes every draw in a pass carries the same mode, so cutting on it cuts
/// nowhere and the submission is byte-identical to not cutting at all.
///
/// `segregate_self_read` is the second negotiable half, and the same shape of question. A pass
/// declares the in-pass destination read when ANY of its draws needs it, so a reader can otherwise
/// join whatever pass is open and let the pass declare on its behalf -- the non-readers just do not
/// read. Where declaring is free that is the cheapest arrangement, and where declaring changes how
/// the whole pass rasterizes it makes every draw in the pass pay for the readers. Under this bit a
/// reader never joins a non-reader's pass, at the price of a pass boundary each way. See
/// GSDevice::TileGpuSegregatesSelfRead.
///
/// The two dimensions are keyed independently, because Adreno asks for both and a key that merged
/// them would break passes where neither policy asked for a break.
constexpr GSTileGpuPassKey gsTileGpuPassKeyFor(GSTileSurfaceId color, GSTileSurfaceId depth,
	GSDevice::GSTileGpuDepthMode mode, bool depth_uniform_passes, bool reads_self, bool segregate_self_read)
{
	return GSTileGpuPassKey{color, depth, mode != GSDevice::GSTileGpuDepthMode::None,
		depth_uniform_passes ? mode : GSDevice::GSTileGpuDepthMode::None, segregate_self_read && reads_self};
}

/// The effective max-pass-draws cap: what EmuCore/GS/TileGpuMaxPassDraws says, or the device's own
/// answer where it says nothing. Zero out means NO CAP.
///
/// Three states rather than two, because "ask the device" and "force uncapped" are different
/// questions and a two-state key cannot ask the second on a device that answers 64. Zero is the
/// shipped arm and asks; a negative forces no cap; a positive pins that cap on any device.
constexpr u32 gsTileGpuMaxPassDraws(int setting, u32 device_answer)
{
	if (setting > 0)
		return static_cast<u32>(setting);
	if (setting < 0)
		return 0;
	return device_answer;
}

/// Whether a pass already holding `open_draws` draws may take another. `max_pass_draws` is 0 for no
/// cap.
///
/// The third clause of "does this draw join the open pass", beside the key and the draw's own
/// break_before -- and it lives here for the same reason the key does: two grouping sites ask, and a
/// pass length they spell differently is a draw whose prep ops land in a pass the draw is not in.
constexpr bool gsTileGpuPassAdmitsMore(u32 open_draws, u32 max_pass_draws)
{
	return max_pass_draws == 0 || open_draws < max_pass_draws;
}

/// Where the pass starting at draw `first` ends: the index of the first draw that refuses to join
/// it, or `count`.
///
/// `key_at(d)` is draw d's pass key; `breaks_at(d)` is whether the draw forces a break of its own
/// (a prep op that could not be hoisted over the open pass, a stale DATE snapshot, a full bind
/// table). This IS the plan build's cut -- it is a function so a test can drive it with a draw list
/// instead of a device, not a model of one.
///
/// The cap is tested BEFORE the draw is looked at, so a pass ends at exactly `max_pass_draws` draws
/// and never at one more. A draw that both hits the cap and carries break_before produces one
/// boundary, not two: both clauses cut in the same place.
template <typename KeyAt, typename BreaksAt>
u32 gsTileGpuPassEnd(u32 first, u32 count, u32 max_pass_draws, KeyAt key_at, BreaksAt breaks_at)
{
	const GSTileGpuPassKey first_key = key_at(first);
	u32 j = first + 1;
	while (j < count && gsTileGpuPassAdmitsMore(j - first, max_pass_draws))
	{
		if (breaks_at(j) || key_at(j) != first_key)
			break;
		j++;
	}
	return j;
}

// -- draw reordering (EmuCore/GS/TileGpuReorderRuns) -------------------------------------------
//
// A pass is its attachments, so a game that alternates between two render targets ends a render
// pass on every switch. Where the two targets share no GS page the alternation is a scheduling
// accident and not a data dependency: the draws into each can be grouped and the passes collapse.
// Dirge of Cerberus plans about a thousand passes a frame that way and would plan under sixty.
//
// This is the bookkeeping half -- which draws are staged in which RUN, and what order the runs come
// out in. It holds no renderer state and does no page arithmetic beyond intersect, so the whole
// thing is drivable from a test with a synthetic draw list (gs_tilegpu_reorder_tests.cpp).
//
// The standing invariant, which is the entire correctness argument:
//
//   Every pair of open runs is page-independent, because admission tested every draw against every
//   other run at the moment it joined.
//
// That is what makes any flush legal -- the whole backlog, in open order -- because the only pairs
// the concatenation can invert are cross-run pairs, and those were proved disjoint.
class GSTileGpuReorderScheduler
{
public:
	/// The ceiling on concurrent runs. Classic's own scheduler holds four; the corpus never wants
	/// more than three.
	static constexpr u32 kMaxRuns = 8;
	static constexpr u32 kNoRun = ~0u;

	/// Which way a draw collided with the runs it was not joining.
	enum : u32
	{
		kHazardRaw = 1u << 0, ///< it reads pages a queued run writes
		kHazardWar = 1u << 1, ///< it writes pages a queued run reads
		kHazardWaw = 1u << 2, ///< it writes pages a queued run writes
		/// ...and it READS pages a queued run reads, which is a hazard here where it would not be in
		/// a renderer that only reads memory. Reading a page in this design can BUILD something: the
		/// first reader of a page in guest order is the one whose prep ops compose its ring slot,
		/// materialise its source image, or gather its palette, and every reader after it is a cache
		/// hit that emits nothing. Invert two readers and the hit runs before the build.
		kHazardRar = 1u << 3,
	};

	/// Where a draw may go, and what making room for it cost.
	struct Verdict
	{
		u32 run = 0;           ///< the run to stage it in
		u32 hazards = 0;       ///< the channels that forced the backlog out first
		u32 runs_emitted = 0;  ///< runs that flush emitted (0 when nothing had to be flushed)
	};

	void Reset(u32 max_runs)
	{
		m_max_runs = std::min(std::max(max_runs, 1u), kMaxRuns);
		m_open = 0;
		m_order.clear();
		for (Run& r : m_runs)
		{
			r.draws.clear();
			r.written.clear();
			r.read.clear();
		}
	}

	u32 MaxRuns() const { return m_max_runs; }
	u32 OpenRuns() const { return m_open; }
	u32 StagedDraws() const
	{
		u32 n = 0;
		for (u32 r = 0; r < m_open; r++)
			n += static_cast<u32>(m_runs[r].draws.size());
		return n;
	}

	/// Which run will take a draw with this key and these footprints. Flushes the backlog where it
	/// has to -- a hazard against another run, or a new key with no room -- so this MUTATES, and the
	/// caller must act on the verdict before asking again.
	///
	/// The test is asked of every run but the draw's own: order inside a run is preserved, so a draw
	/// may freely overlap the pages of the run it is joining. That is where a page bitmap beats a
	/// texture-pointer comparison, which cannot tell which run a queued reader lives in.
	///
	/// ⚠️ All four channels, read-against-read included. That last one is not the usual hazard
	/// analysis and it is not conservatism -- see kHazardRar: a read here can be what BUILDS the
	/// thing every later read of the same page hits in a cache.
	Verdict Admit(const GSTileGpuPassKey& key, const GSPageBitmap& written, const GSPageBitmap& read)
	{
		Verdict v;
		u32 mine = kNoRun;
		for (u32 r = 0; r < m_open; r++)
		{
			if (m_runs[r].key == key)
			{
				mine = r;
				break;
			}
		}
		for (u32 r = 0; r < m_open; r++)
		{
			if (r == mine)
				continue;
			v.hazards |= written.intersects(m_runs[r].written) ? kHazardWaw : 0u;
			v.hazards |= written.intersects(m_runs[r].read) ? kHazardWar : 0u;
			v.hazards |= read.intersects(m_runs[r].written) ? kHazardRaw : 0u;
			v.hazards |= read.intersects(m_runs[r].read) ? kHazardRar : 0u;
		}
		if (v.hazards != 0)
		{
			v.runs_emitted = FlushAll();
			mine = kNoRun;
		}
		if (mine == kNoRun)
			mine = OpenRun(key, &v.runs_emitted); // the cap flushes here, if there is no room
		v.run = mine;
		return v;
	}

	/// Open a run with this key and return its index, emitting the backlog first if the cap leaves no
	/// room. `emitted` takes however many runs that cost.
	u32 OpenRun(const GSTileGpuPassKey& key, u32* emitted = nullptr)
	{
		if (m_open == m_max_runs)
		{
			const u32 n = FlushAll();
			if (emitted)
				*emitted += n;
		}
		m_runs[m_open].key = key;
		return m_open++;
	}

	/// Put draw `index` in run `run` and grow that run's footprints.
	void Stage(u32 index, u32 run, const GSPageBitmap& written, const GSPageBitmap& read)
	{
		pxAssert(run < m_open);
		Run& r = m_runs[run];
		r.draws.push_back(index);
		r.written |= written;
		r.read |= read;
	}

	/// A draw nothing has proved independent of the backlog: the backlog goes out, and the draw gets a
	/// run of its own. That makes it a barrier both ways -- nothing before it can be deferred past it,
	/// because the flush emitted all of it; and nothing after it can be floated ahead of it, because
	/// every run opened later is emitted later. Returns the run to stage it in; `emitted` takes the
	/// runs the flush put out.
	u32 OpenBarrierRun(const GSTileGpuPassKey& key, u32* emitted = nullptr)
	{
		const u32 n = FlushAll();
		if (emitted)
			*emitted += n;
		return OpenRun(key);
	}

	/// Emit every open run, in open order, and start over. Returns how many emitted.
	u32 FlushAll()
	{
		const u32 emitted = m_open;
		for (u32 r = 0; r < m_open; r++)
		{
			Run& run = m_runs[r];
			m_order.insert(m_order.end(), run.draws.begin(), run.draws.end());
			run.draws.clear();
			run.written.clear();
			run.read.clear();
		}
		m_open = 0;
		return emitted;
	}

	/// The emission order committed so far: a permutation of the draw indices handed in.
	const std::vector<u32>& Order() const { return m_order; }
	void ClearOrder() { m_order.clear(); }

private:
	struct Run
	{
		GSTileGpuPassKey key;
		GSPageBitmap written;
		GSPageBitmap read;
		std::vector<u32> draws;
	};

	std::array<Run, kMaxRuns> m_runs;
	u32 m_max_runs = 1;
	u32 m_open = 0;
	std::vector<u32> m_order;
};

/// Gather one per-draw array into the emission order: out[i] = src[order[i]]. Through a scratch
/// vector the caller owns, so a frame's splice allocates nothing after the first.
template <typename T>
void gsTileGpuGatherByOrder(const std::vector<T>& src, const std::vector<u32>& order, std::vector<T>& out)
{
	out.clear();
	out.reserve(order.size());
	for (const u32 d : order)
		out.push_back(src[d]);
}

/// Rebuild a prep-op array in `order`, moving whole op RECORDS and renumbering only the base each
/// draw's slice starts at.
///
/// ⚠️ The side arrays an op names are NOT reindexed and must not be: `first_page_entry` addresses
/// the plan's page entries, `target` the plan's target list, and a materialise names its
/// destination in prep_textures. Nothing in those spaces moved, so an op record that carries its
/// indices unchanged still names what it named.
///
/// `first_at(d)` / `count_at(d)` name draw d's slice in `src`; `set_first(d, base)` records where it
/// landed. The slices must tile `src` -- every op belongs to exactly one draw, which is what the
/// plan build's own contiguous-range arithmetic already requires.
template <typename Op, typename FirstAt, typename CountAt, typename SetFirst>
void gsTileGpuSplicePrepOps(const std::vector<Op>& src, const std::vector<u32>& order, FirstAt first_at,
	CountAt count_at, SetFirst set_first, std::vector<Op>& out)
{
	out.clear();
	out.reserve(src.size());
	for (const u32 d : order)
	{
		const u32 first = first_at(d);
		const u32 count = count_at(d);
		set_first(d, static_cast<u32>(out.size()));
		out.insert(out.end(), src.begin() + first, src.begin() + first + count);
	}
}

// -- surface-identity containment (EmuCore/GS/TileGpuContainSurfaces) --------------------------
//
// The pass key above is what makes this worth doing. A pass is its attachments, so two views of GS
// memory that get two surfaces get two pass keys, and a game that alternates between them ends a
// render pass every time it switches -- 140 times a frame on Gran Turismo 4, where the alternation
// is a 65x33 palette buffer and the frame buffer it is drawn beside. Neither view moves; what
// changes is which image holds them. Give the small one a rectangle inside the big one's texture
// and the two share a surface id, the key stops changing, and the passes merge.
//
// Two rules, and they are worthless apart. Cross-PSM identity within a swizzle family (0x01a40 as
// PSMCT24 and as PSMCT32 are one surface) removes no break on either GT4 dump on its own; nor does
// a nonzero page offset with PSM equality still required. Together they remove 87% and 91% of the
// colour-key breaks. Simulated over the corpus's own PS2 draw streams; the arithmetic is below.
//
// ⚠️ That paragraph is the DESIGN. What ships is its zero-offset half only -- see
// kGSTileContainDisplacedViews below, which is the rule the placement actually applies and the
// reasons the other half is not taken yet. The colour-key metric the two-rule claim was selected on
// also mis-attributes where the saving is: measured in passes, the same-base merge alone is worth
// -117.00 render passes a drawn frame on the Online Public Beta dump and the displaced fold beside
// it is worth -13.00, because what the merge really removes is two views of one base stealing each
// other's pages -- -130 seed ops and -130 writeback ops a frame, each seed carrying a pass break.

/// Where a contained view's origin lands inside its container, in the container's page grid and in
/// its pixels. `end_row` is the page-row count the container must hold to admit the view -- what
/// the growth budget is charged against, and what the pool must have allocated before a draw of
/// this view is recorded.
struct GSTileContainOffset
{
	u32 col = 0;
	u32 row = 0;
	u32 end_row = 0;
	s32 x = 0;
	s32 y = 0;
};

/// Why a container may not hold a view. In rule order, so the numerically largest refusal a
/// candidate produced is the clause it got furthest before failing -- which is what a census of
/// refusals wants to report.
enum class GSTileContainRefusal : u8
{
	Admitted = 0,
	Kind, ///< different attachment types, or a depth view (deferred: worth ~1 break/frame corpus-wide)
	Family, ///< different page geometry or intra-page block order, so no rectangle expresses it
	Stride, ///< a differing stride is a per-page-row remap (GSTileAliasTier::PageRemap), not an offset
	Alignment, ///< a block-offset base starts mid-page, where the intra-page swizzle is unexpressible
	Delta, ///< the view starts BEFORE the container; every road computes `blk - base` unsigned
	Column, ///< the view's rows would wrap into the container's next page row
	Extent, ///< the container cannot grow that far without leaving GS memory or the page budget
	Wrap, ///< ...or without running off the end of GS memory from where the container itself starts
};

/// Whether `container` may hold `view` at a rectangle offset, and where.
///
/// `view_page_cols` / `view_page_rows` are the view's own footprint in pages, from the extent its
/// draws actually reach (gsTileContainPageCols / gsTileContainPageRows). `page_budget` is the
/// largest page footprint the container may reach, 0 for the whole of GS memory.
///
/// ⚠️ Legality only. It does not say the fold is a good idea -- which of several legal containers a
/// view should take is a policy question the caller owns, and the wrong answer there costs GT4
/// five-sixths of the win (a "nearest containing base" rule leaves it at 107.75 breaks a frame
/// against the composite policy's 20.38).
///
/// ⚠️ Rule 8 of the design's predicate -- a PSMCT24 view under a PSMCT32 container writes back an
/// alpha byte its own format does not store -- is NOT here, because it is not a legality question.
/// Half of it was answered by making a 24-bit frame stop writing the image's alpha at all, which
/// moves pixels with or without containment and landed on its own. The other half -- the WRITEBACK
/// still carries the container's byte mask -- is open, and is one of the two reasons a displaced
/// placement is refused (kGSTileContainDisplacedViews).
constexpr GSTileContainRefusal gsTileContainView(const GSTileSurfaceLayout& container,
	const GSTileSurfaceLayout& view, u32 view_page_cols, u32 view_page_rows, u32 page_budget,
	GSTileContainOffset& out)
{
	out = GSTileContainOffset{};
	if (container.kind != view.kind || view.kind != GSTileSurfaceKind::Color)
		return GSTileContainRefusal::Kind;
	const GSTileSwizzleFamily family = gsTileSwizzleFamily(view.psm);
	if (family == GSTileSwizzleFamily::Invalid || gsTileSwizzleFamily(container.psm) != family)
		return GSTileContainRefusal::Family;
	if (container.bw == 0 || container.bw != view.bw)
		return GSTileContainRefusal::Stride;
	if (((container.bp | view.bp) & 31) != 0)
		return GSTileContainRefusal::Alignment;
	if (view.bp < container.bp)
		return GSTileContainRefusal::Delta;

	const u32 bw = container.bw;
	const u32 delta = (view.bp - container.bp) / 32;
	const u32 col = delta % bw;
	const u32 row = delta / bw;
	// A zero-extent view still occupies the page its base names.
	const u32 cols = (view_page_cols == 0) ? 1 : view_page_cols;
	const u32 rows = (view_page_rows == 0) ? 1 : view_page_rows;
	if (col + cols > bw)
		return GSTileContainRefusal::Column;

	const u32 end_row = row + rows;
	const u32 budget = (page_budget == 0 || page_budget > GS_MAX_PAGES) ? GS_MAX_PAGES : page_budget;
	// Divided rather than multiplied so the row count cannot overflow the product on the way to
	// being refused for being too large.
	if (end_row > budget / bw)
		return GSTileContainRefusal::Extent;
	if (container.bp / 32 + end_row * bw > GS_MAX_PAGES)
		return GSTileContainRefusal::Wrap;

	out.col = col;
	out.row = row;
	out.end_row = end_row;
	out.x = static_cast<s32>(col * 64);
	out.y = static_cast<s32>(row * gsTilePageHeight(container.psm));
	return GSTileContainRefusal::Admitted;
}

/// Whether a view may be placed inside a container at a NONZERO offset. The predicate above is
/// geometry; this is policy, and it is the one thing that decides what containment ships as.
///
/// False: a view is admitted only where it lands at (0, 0) -- the cross-PSM same-base merge, where
/// the two views name exactly the same guest pages -- and every displaced placement is refused
/// however legal the geometry. Measured over all 21 corpus dumps, the zero-offset rule is
/// byte-identical to containment being off, and displacement is not: it breaks five titles, by two
/// mechanisms that are both a displaced view and its container disagreeing about guest BYTES.
///
///   * The WRITEBACK carries the container's byte mask. EmitPrepOp stamps a writeback with the
///     owning surface's layout and gsTileWritebackByteMask(psm), and under containment the owner is
///     the container -- so a PSMCT24 view inside a PSMCT32 container has its pages written back with
///     all four bytes, putting the image's alpha into guest memory the view's own surface would have
///     masked off. (God of War II reads exactly that byte back as PSMT8H.) A zero-offset merge is
///     exempt: both views cover the same pages, so no page is written under a mask that is not its
///     own owner's. The fix is to bucket the writeback by byte mask, which is a rung of its own.
///   * The READS stop matching. TargetForTextureRead wants the owner's layout to be the read's
///     exactly, and DonorForTextureRead refuses when the owner is the drawing pass's own attachment.
///     Displace a view and both start refusing reads they used to serve, and those draws fall to the
///     byte road -- which decodes guest bytes a writeback assembled, where rule 2 and the donor
///     sample the live image. Wherever the writeback is lossy those are different texels. On the
///     Online Public Beta dump this collapses the donor road outright (468.50 eligible windows a
///     frame to 1.00), because folding the palette buffer into the frame buffer makes the frame
///     buffer the owner of the palette's pages and the frame buffer is what those draws render into.
///     A same-base merge changes no read's layout match that was not already refused. The fix is the
///     per-slot offset bind plus a page-granular donor clause, which is the other rung.
///
/// Flipping this to true is the arm that work measures against. It must not ship true until both
/// fixes are in, and its gate is the 21-dump byte-identity comparison plus "the read-road counters
/// did not move".
constexpr bool kGSTileContainDisplacedViews = false;

/// Whether a view may actually SIT at this pixel offset inside its container -- asked of a placement
/// the predicate admitted, and of a fold already in the alias table. `view_held_to_zero` is the
/// caller's own veto for this one view (EnsureSurface's `may_offset`, design rule 9); the standing
/// rule above holds every view to the same thing while displacement is off.
constexpr bool gsTileContainMayPlaceAt(s32 x_off, s32 y_off, bool view_held_to_zero)
{
	if (x_off == 0 && y_off == 0)
		return true; // the same-base merge: nothing moves, so there is nothing to refuse
	return kGSTileContainDisplacedViews && !view_held_to_zero;
}

/// A view's page footprint from the extent its draws reach, measured from the surface's own origin
/// (a draw at x=600 of width 40 makes the view 640 wide, not 40).
///
/// Every format the target pool backs has 64-pixel-wide pages, so the column count is the same
/// arithmetic for all of them; the row count is not, and takes the page height of the view's own
/// PSM. Clamped to the stride, because a view wider than its own stride wraps rather than
/// overflowing -- the column clause below is about where it STARTS, not about how wide it is.
constexpr u32 gsTileContainPageCols(u32 width, u32 bw)
{
	if (bw == 0)
		return 1;
	const u32 cols = (width + 63) / 64;
	return (cols == 0) ? 1 : ((cols > bw) ? bw : cols);
}

constexpr u32 gsTileContainPageRows(u32 height, u32 psm)
{
	const u32 page_height = gsTilePageHeight(psm);
	if (page_height == 0)
		return 1;
	const u32 rows = (height + page_height - 1) / page_height;
	return (rows == 0) ? 1 : rows;
}

/// The effective containment page budget: what EmuCore/GS/TileGpuContainPageBudget says, clamped to
/// what the geometry allows. Zero -- the default -- is the whole of GS memory.
constexpr u32 gsTileGpuContainPageBudget(int setting)
{
	if (setting <= 0 || static_cast<u32>(setting) > GS_MAX_PAGES)
		return GS_MAX_PAGES;
	return static_cast<u32>(setting);
}

/// The XYOFFSET a contained view's draws are transformed by, for one axis.
///
/// The vertex transform SUBTRACTS this from the guest vertex position, so putting the draw
/// `pixels` further right (or further down) means taking those pixels OFF the offset. XYOFFSET is
/// the register's own 4.4 fixed point and the pixel offset is not, which is the whole reason this
/// is a named function and not a subtraction at the call site -- getting the shift wrong displaces
/// the view by a sixteenth of where it belongs, which on the GT4 pair is a palette buffer landing
/// inside the frame it should sit below.
constexpr s32 gsTileContainVertexOffset(s32 xyoffset, s32 pixels)
{
	return xyoffset - (pixels << 4);
}

/// A circuit that is scanning nothing out. No view can carry it, because a base is 5 bits of BP
/// shifted up and cannot reach the top of the word.
constexpr u32 kGSTileNoDisplayBase = 0xFFFFFFFFu;

/// Design rule 9: a base the PCRTC scans out is never a containee, whatever the geometry allows.
///
/// GetOutput hands the frontend a texture and a y_offset, with no column offset beside it, so a
/// display view folded at a nonzero page column could not be presented at all -- and the display
/// buffer is never the small view in any corpus dump, so refusing costs nothing measurable.
///
/// Asked of the BASE alone rather than the whole layout: the PCRTC scans a base out under its own
/// FBW and PSM, and a view that shares only the base is the same bytes seen differently. Folding
/// that view would move the pixels the present reads just as surely.
constexpr bool gsTileContainIsDisplayBase(u32 view_bp, const std::array<u32, 2>& display_bp)
{
	return view_bp == display_bp[0] || view_bp == display_bp[1];
}

/// A view folded into a container: the surface that holds it, and where its origin sits in that
/// surface's pixel space. The offset is what the draw's XYOFFSET, scissor and rect are translated
/// by; (0, 0) is a view that IS its container.
struct GSTileContainedView
{
	GSTileSurfaceId id = kGSTileNoSurface;
	s32 x_off = 0;
	s32 y_off = 0;
};

/// layout.pack() -> the container that holds that layout's view.
///
/// Renderer-side rather than a second index in GSVramModel: the model's layout map is a bijection
/// and its Destroy erases exactly one key, and making it a multimap would put the whole invariant
/// suite at risk for a lookup only this road performs. An entry outlives the frame that installed
/// it -- the fold has to be STICKY or a view drifts between containers with the draw order, which
/// costs a surface create and a re-seed every frame -- and dies with its container.
using GSTileViewAliasTable = std::unordered_map<u32, GSTileContainedView>;

/// The effective specialization bind budget: what EmuCore/GS/TileGpuMaxSpecializationBinds says, or
/// the device's own answer where it says nothing. Zero out means NO GUARD.
///
/// Three states for the same reason the pass cap has three: "ask the device" and "force the guard
/// off" are different instructions, and only the second gives an Adreno the un-guarded arm the
/// deciding A/B needs. Zero asks; a negative forces off; a positive pins that budget anywhere.
constexpr u32 gsTileGpuMaxSpecializationBinds(int setting, u32 device_answer)
{
	if (setting > 0)
		return static_cast<u32>(setting);
	if (setting < 0)
		return 0;
	return device_answer;
}

/// A GS scissor as one comparable word, for counting distinct rects. SCAX0/SCAY0/SCAX1/SCAY1 are
/// eleven-bit register fields and scissor.in adds one to the exclusive edges, so every edge is in
/// [0, 2048] and the four pack into a u64 without loss.
constexpr u64 gsTileGpuScissorKey(s32 x0, s32 y0, s32 x1, s32 y1)
{
	return static_cast<u64>(static_cast<u16>(x0)) | (static_cast<u64>(static_cast<u16>(y0)) << 16) |
		   (static_cast<u64>(static_cast<u16>(x1)) << 32) | (static_cast<u64>(static_cast<u16>(y1)) << 48);
}

/// The same variant key with the frozen per-draw GS state withheld -- every axis back on the state
/// row, the road/texel/self/quantise half untouched.
///
/// This is the ONE transform the guard performs, and it is pixel-inert by construction rather than
/// by argument: it produces bit-for-bit the key PlanVariantKeyAt writes when
/// EmuCore/GS/TileGpuUnspecializedFragmentVariant is set, which is the control arm the specialization
/// landing gated byte-identical over the whole corpus. The guard picks a program that was already
/// proved to render the same pixels; it does not invent one.
///
/// ⚠️ Deliberately NOT the pass UNION key, which would also be inert and is a much worse trade: the
/// union widens the road, texel and self masks back to the pass's, which is exactly the contamination
/// the per-draw variant was landed to end. On the Ratchet gameplay scene that is 1,499 draws a frame
/// put back on a byte decoder they never enter, and the draw-weighted program goes 2,684 SPIR-V words
/// to 11,000. De-specializing keeps the road narrowing and drops only the axis that was measured to
/// cost.
constexpr u32 gsTileGpuDespecializeVariantKey(u32 key)
{
	return key & ~GSDevice::GSTileGpuPassPlan::kVariantSpecMask;
}

/// Whether a plan paying `added_binds` extra pipeline binds for specialization is over `budget`.
/// Budget 0 is no guard, and no number of binds crosses it.
constexpr bool gsTileGpuGuardsSpecialization(u32 added_binds, u32 budget)
{
	return budget != 0 && added_binds > budget;
}

/// ...and, once a plan is over budget, whether THIS pass is one the guard acts on: only a pass whose
/// frozen state actually cut a run it would not otherwise have cut.
///
/// A pass that adds no bind is left specialized, and that is not a micro-optimisation -- it is what
/// keeps the guard from being the coarse whole-frame switch the config key already provides. Those
/// passes get the smaller program for free; taking it away would give up the wave size and save
/// nothing. It also makes the guarded plan's bind total exactly the unspecialized arm's, since every
/// pass then contributes its de-specialized run count whichever branch it took.
constexpr bool gsTileGpuDespecializesPass(u32 pass_runs, u32 pass_runs_despec)
{
	return pass_runs > pass_runs_despec;
}

/// How many indirect runs a stretch of draws makes: a run starts at `first`, wherever the rest of the
/// run key cut anyway (`other_cut_at` -- topology, blend, depth mode), and wherever the fragment
/// variant changes.
///
/// A function so the planner can ask it twice off one walk -- once with the shipped keys and once
/// with the de-specialized ones -- and so a test can drive it with a key list instead of a device.
template <typename VariantAt, typename OtherCutAt>
u32 gsTileGpuRunCount(u32 first, u32 end, VariantAt variant_at, OtherCutAt other_cut_at)
{
	u32 runs = 0;
	for (u32 d = first; d < end; d++)
	{
		if (d == first || other_cut_at(d) || variant_at(d) != variant_at(d - 1))
			runs++;
	}
	return runs;
}

/// How many passes a draw list of `count` draws cuts into under `key_at`.
///
/// This IS gsTileGpuPassEnd in a loop, which is the point: the depth predictor below has to count a
/// grouping the frame did NOT take, and a grouping counted by a model of the cut rather than by the
/// cut itself is a number that can drift away from the plan build without anything failing.
template <typename KeyAt, typename BreaksAt>
u32 gsTileGpuCountPasses(u32 count, u32 max_pass_draws, KeyAt key_at, BreaksAt breaks_at)
{
	u32 passes = 0;
	for (u32 i = 0; i < count; passes++)
		i = gsTileGpuPassEnd(i, count, max_pass_draws, key_at, breaks_at);
	return passes;
}

/// The depth-pass predictor's constants.
///
/// The thresholds are RECIPROCALS of the metric, so the whole decision is integer arithmetic on
/// counts. A frame's verdict must not depend on how a float rounded on one architecture.
enum : u32
{
	/// Go merged at (uniform_passes - merged_passes) / draws >= 1/16.
	kGSTileGpuDepthMergeOnReciprocal = 16,
	/// ...and fall back to uniform only below 1/32. The 2x band is the hysteresis: the metric is a
	/// per-frame quantity and a scene sitting on the threshold would otherwise re-key every pass in
	/// the frame, every frame.
	kGSTileGpuDepthMergeOffReciprocal = 32,
	/// Consecutive frames that must ask for the other polarity before it is taken. Two, not more:
	/// the band above is what suppresses oscillation, and this only removes a single anomalous
	/// frame. A longer window costs a real share of a 20-frame corpus replay and buys nothing the
	/// band does not already buy.
	kGSTileGpuDepthPolicyConfirmFrames = 2,
};

/// Whether the frame just planned asks for MERGED depth passes.
///
/// The metric is `(uniform_passes - merged_passes) / draws`: the fraction of the frame's draws that
/// STOP OPENING A PASS when the depth mode leaves the pass key. Every pass is opened by exactly one
/// draw, so the numerator is a count of draws promoted out of pass-opening and the ratio is
/// dimensionless -- which is what makes one threshold serve a 52-draw menu and a 6738-draw battle.
///
/// It is the only one of the three candidates measured that separates the SD865 corpus round
/// (19 dumps, both arms, `-loop 10`, structural counters off the device's own stats.json). Ranked by
/// this metric the seven titles merging wins on are the top seven of nineteen, and no title merging
/// loses on reaches the eighth place: the winners run 0.084 (Beyond Good & Evil) to 0.841
/// (Xenosaga), the losers 0.002 (Dirge of Cerberus) to 0.033 (Armored Core 3). 1/16 sits in that
/// gap, nearer the loser side on purpose -- misclassifying a loser costs frame time, misclassifying
/// a winner costs only the win.
///
/// The two candidates that FAIL, recorded so they are not re-proposed: the collapse RATIO
/// (uniform/merged) puts BG&E at 1.10 below Baldur's Gate 2's 1.20, and the ABSOLUTE saving puts
/// BG&E's 8 passes below Armored Core 3's 53. Merging's win is not how much of the pass structure
/// collapses, nor how many passes go away; it is how many passes go away PER DRAW THAT PAID FOR THEM.
constexpr bool gsTileGpuWantsMergedDepthPasses(u32 uniform_passes, u32 merged_passes, u32 draws, bool merged_now)
{
	// A frame with no draws says nothing about either polarity, so it must not vote.
	if (draws == 0)
		return merged_now;
	const u64 saved = (uniform_passes > merged_passes) ? static_cast<u64>(uniform_passes - merged_passes) : 0;
	const u32 reciprocal = merged_now ? kGSTileGpuDepthMergeOffReciprocal : kGSTileGpuDepthMergeOnReciprocal;
	return saved * reciprocal >= draws;
}

/// The sticky per-frame depth-pass polarity, and its census.
///
/// Sticky because the polarity is a PASS KEY input: flipping it re-cuts every pass in the frame, so
/// a scene oscillating around the threshold would re-plan its whole frame structure twice a frame
/// for no gain. Two guards, and they answer different failure modes -- the hysteresis band answers a
/// metric sitting on the threshold, the confirmation count answers one anomalous frame (a load
/// screen, a full-screen wipe) in an otherwise steady scene.
///
/// Starts UNIFORM whatever the scene, because uniform is what ships on the device this engages on:
/// the predictor may only ever move a frame off the shipped arrangement after it has seen evidence,
/// never before.
struct GSTileGpuDepthPolicyPicker
{
	bool merged = false;   ///< the polarity in force
	u32 confirming = 0;    ///< consecutive frames that have asked for the other one
	u32 switches = 0;      ///< times the polarity actually changed -- the churn column
	u32 frames = 0;        ///< frames observed
	u32 frames_merged = 0; ///< ...of which ran merged
	u64 saved_passes = 0;  ///< sum of the metric's numerator over those frames
	u64 draws = 0;         ///< ...and of its denominator

	/// Feed one finished frame; returns the polarity the NEXT frame runs under.
	bool Observe(u32 uniform_passes, u32 merged_passes, u32 frame_draws)
	{
		// ⚠️ A frame that planned no draws is NEUTRAL. Not a vote for the polarity in force, and in
		// particular not agreement -- agreement clears the confirmation count, and a title that
		// alternates a drawn frame with an empty one would then never reach two consecutive votes.
		// Xenosaga and MGS3 are exactly that shape (6750 draws, 0, 6725, 0, ...), so treating an
		// empty frame as agreement pinned the two biggest wins in the corpus to uniform for ever.
		// Measured: both read 0/8 frames merged with a mean metric of 0.84 and 0.51.
		if (frame_draws == 0)
			return merged;
		frames++;
		frames_merged += merged ? 1u : 0u;
		saved_passes += (uniform_passes > merged_passes) ? (uniform_passes - merged_passes) : 0u;
		draws += frame_draws;

		const bool want = gsTileGpuWantsMergedDepthPasses(uniform_passes, merged_passes, frame_draws, merged);
		if (want == merged)
		{
			confirming = 0;
			return merged;
		}
		if (++confirming < kGSTileGpuDepthPolicyConfirmFrames)
			return merged;
		merged = want;
		confirming = 0;
		switches++;
		return merged;
	}

	/// The metric over every frame observed. Reported, never decided on -- the decision is per frame.
	double MeanMetric() const
	{
		return draws ? (static_cast<double>(saved_passes) / static_cast<double>(draws)) : 0.0;
	}
};

/// Whether the fragment stage truncates this draw's output to what a 16-bit frame stores.
///
/// A TileGpu colour target is an RGBA8 image whatever the guest format is, and every road that puts
/// bytes into one already agrees the 16-bit families live there expanded -- the writeback packs
/// `byte >> 3`, the seed unpacks `bits << 3`. A draw that writes eight bits a channel leaves
/// precision in the image the console never stored, and everything that reads the target back sees
/// it.
///
/// ⚠️ Only where the fragment stage's output IS what lands. A draw the executor's blend unit still
/// has to touch is quantised by the console AFTER that blend, so truncating the shader's output
/// would be a different picture rather than a more accurate one. Hence the question is whether the
/// draw took the SHADER blend, not merely whether it blends.
constexpr bool gsTileGpuQuantisesOnWrite(bool frame_quantises, bool blend_active, bool shader_blend)
{
	return frame_quantises && (shader_blend || !blend_active);
}

/// Whether this draw's COLOUR WRITE MASK leaves the pipeline and is served by the fragment stage
/// instead (EmuCore/GS/TileGpuShaderWriteMask).
///
/// The mask is in the blend key, and the blend key is the run key -- so two otherwise identical
/// draws with different FBMSK channels cost two pipelines and two indirect calls. A game that builds
/// a buffer one channel group at a time therefore gets one call per draw: Spider-Man 3 cuts 2,841 of
/// its 2,992 adjacent in-pass draw pairs on the mask alone, and taking it out of the key turns 3,624
/// calls into 785.
///
/// Bit-exact either way, and the refusals below are what makes that true rather than nearly true:
/// the shader's FBMSK arm already merges the destination at BIT granularity for the partial-mask
/// road, and a mask covering whole channels is a strict subset of what that arm does -- but only for
/// a draw the arm's own tail already owns end to end. What this decides is which of two exact
/// realizations a draw takes; a draw it refuses keeps the pipeline mask and nothing about it moves.
///
/// The three refusals, all of which keep the pipeline mask for that one draw:
///
///   nothing to move   a draw writing all four channels has no mask; a draw writing none has no
///                     colour for the merge to buy anything on, and would read its destination
///                     only to write the same bytes back.
///   the blend unit    ⚠️ the write mask applies AFTER blending. A shader that merged the
///                     destination in first would then have the blend unit blend the destination
///                     with itself, which is a different picture. So this is only for draws whose
///                     FRAGMENT output is what lands -- no blend, or a blend the shader owns.
///   the byte tail    ⚠️ and the one that is not obvious. The fragment stage's mask arm lives at the
///                    end of a tail that first converts the colour to bytes at
///                    floor(cv * 255 + 0.5), and a Vulkan UNORM8 attachment write rounds ties its
///                    own way. For a draw whose colour ALREADY goes through that tail the two round
///                    identically and the merge is bit-exact; for one that does not, moving the mask
///                    would also move the rounding of the channels the mask does not even cover.
///                    Measured, not feared: 17 pixels of Ace Combat 5, ±1 in one channel each, on 12
///                    unblended 32-bit draws. So the last argument, and it is spelt as "this draw
///                    already evaluates its own blend or its own FBMSK" rather than as the tail's
///                    full entry condition on purpose -- see below.
///
/// ⚠️ THE INVARIANT, and it is what makes this lever cheap enough to reason about: a draw the road
/// takes was ALREADY a declared in-pass reader. So the lever adds no reader, no declaring pass, no
/// per-class budget charge and no reorder barrier run -- it can only stop the write mask cutting a
/// run that was going to be cut anyway. Provable from the argument list rather than measured, which
/// is why the third way into the tail (a 16-bit frame's quantise, which a draw reaches WITHOUT
/// reading anything) is deliberately not here: admitting it would be exact, and would be the one
/// shape that turns a non-reader into a reader. Zero draws on the 21-dump corpus, so it costs
/// nothing today and it can come back behind a measurement when a title wants it.
constexpr bool gsTileGpuMasksInShader(
	bool lever, u8 color_mask, bool blend_active, bool shader_blend, bool shader_fbmsk)
{
	if (!lever || color_mask == 0 || color_mask == 0xF)
		return false;
	if (blend_active && !shader_blend)
		return false;
	return shader_blend || shader_fbmsk;
}

/// The channels a 4-bit colour write mask DROPS, as whole-byte keep bits on the expanded RGBA8
/// target -- exactly what the pipeline's colour write mask was preserving before the fragment stage
/// took the job over.
///
/// Whole bytes, including the bits a 16-bit frame does not store: the pipeline preserved the byte
/// entire, so keeping only `fmsk`'s bits here would let the fragment's low bits land where the
/// pipeline dropped them. It unions with gsTileGpuFrameKeepMask rather than replacing it, because a
/// channel the register masks in PART is still written and still owes those bits to the destination.
constexpr u32 gsTileGpuChannelKeepMask(u8 color_mask)
{
	u32 keep = 0;
	for (u32 b = 0; b < 4; b++)
	{
		if ((color_mask & (1u << b)) == 0)
			keep |= 0xFFu << (b * 8);
	}
	return keep;
}

/// The ADMISSION CLASSES: the five distinct reasons a draw asks for the in-pass destination read.
///
/// Finer than the three kGSTileGpuSelf* USES, and it has to be. Two classes can want the same use
/// for entirely different reasons and cost entirely different amounts: the exotic-blend class and
/// the 16-bit-quantised-blend class both want kGSTileGpuSelfBlend, and on FlatOut 2 the first is 448
/// draws a frame while the second is zero; the partial-FBMSK class and the AFAIL alpha keep both
/// want kGSTileGpuSelfMask, and on Xenosaga the first is 2,409 draws a frame while the second is
/// none. A budget kept at USE granularity would price those together and get both wrong.
///
/// ⚠️ It took a per-class census to find that out, and the shipped counters did not have one: every
/// title's readers looked alike, so Xenosaga was recorded for weeks as a 16-bit-saturated title when
/// it renders to 32-bit frames and its bulk is a single-bit alpha FBMSK. That is why these are
/// permanent counters and not a debugging scratch.
enum : u32
{
	kGSTileGpuClassDate = 1u << 0,           ///< TEST.DATE reads the live pixel, not a pass snapshot
	kGSTileGpuClassExoticBlend = 1u << 1,    ///< the five blend equations fixed function cannot express
	kGSTileGpuClassQuantisedBlend = 1u << 2, ///< a 16-bit frame quantises the blend's RESULT
	kGSTileGpuClassPartialMask = 1u << 3,    ///< FBMSK masks a channel in PART
	kGSTileGpuClassAfailKeep = 1u << 4,      ///< AFAIL keeps the destination alpha
};
static constexpr u32 kGSTileGpuAdmissionClasses = 5;
static constexpr u32 kGSTileGpuClassAll = (1u << kGSTileGpuAdmissionClasses) - 1;

/// What this draw would be admitted FOR, whatever any device thinks of the price.
///
/// Deliberately free of policy: the budget below needs to know what a class WOULD cost even in a
/// frame where it was refused, and a classifier that answered "nothing, it was refused" would make
/// the budget measure its own output. That is not a subtlety -- it is the difference between a
/// budget and a two-frame oscillator.
constexpr u32 gsTileGpuWantedClasses(u32 reader_flags, bool date, bool fbmsk_exact,
	bool blend_needs_quantised_result, bool afail_keep_alpha)
{
	u32 wanted = 0;
	// The destination-alpha test. The live pixel is exact, where the pre-pass snapshot is only exact
	// because the planner spends a pass break and a full-target copy making it so.
	//
	// ⚠️ Asked of the draw's own DATE fold rather than of the classifier's ReaderDate bit. The two
	// differ in one case and the difference matters: the classifier answers for the pass-structure
	// census, which only counts a draw that lands colour, and a DATE draw that lands none still reads
	// destination alpha to decide its DEPTH write. Widening the census would change what it has
	// counted since the crossover study; widening here costs nothing and admits the draw.
	if (date)
		wanted |= kGSTileGpuClassDate;

	// The blend equations the executor's fixed-function state cannot express:
	//
	//   ReaderAdFactor  C=Ad. Fixed-function DST_ALPHA is dest/255; the console's is dest/128.
	//   ReaderFacGt1    C=As with a fragment alpha that can pass 0x80, or C=FIX above 0x80. Both
	//                   saturate at 1.0 in a blend factor and reach 1.99 on the console.
	//   ReaderCoeffGt1  the D == A accumulation shapes, where a coefficient is 1 + C.
	//   ReaderWrap      COLCLAMP = 0, which a blend unit cannot do at all -- it clamps.
	//   ReaderPabe      PABE's per-pixel gate on the source alpha's MSB, likewise.
	//
	// ⚠️ ReaderAsDualSource is deliberately NOT here. That class is exactly the one dual-source
	// blending expresses exactly, and it is most of the corpus's blended draws -- admitting it would
	// move the bulk of every blended frame onto the read for no accuracy at all.
	if (reader_flags & (GSTilePassSim::ReaderWrap | GSTilePassSim::ReaderPabe | GSTilePassSim::ReaderCoeffGt1 |
						   GSTilePassSim::ReaderAdFactor | GSTilePassSim::ReaderFacGt1))
		wanted |= kGSTileGpuClassExoticBlend;

	// ...and a blend whose equation IS expressible but whose RESULT the frame format quantises. The
	// console quantises after the blend; the executor's blend unit runs after the fragment stage, so
	// there is no point at which it could. The whole blend has to move to the shader, which can.
	if (blend_needs_quantised_result)
		wanted |= kGSTileGpuClassQuantisedBlend;

	// A write mask the channel-granular pipeline mask cannot reproduce bit for bit. The pipeline still
	// drops the channels FBMSK covers whole -- that half is exact and cheaper -- and the shader owns
	// only the bits inside a channel the register masks in part.
	if (!fbmsk_exact)
		wanted |= kGSTileGpuClassPartialMask;

	// The alpha keep is a per-fragment write mask, so it rides the arm that already does one.
	if (afail_keep_alpha)
		wanted |= kGSTileGpuClassAfailKeep;

	return wanted;
}

/// The kGSTileGpuSelf* uses a set of admitted classes needs out of the fragment stage.
constexpr u32 gsTileGpuClassUses(u32 classes)
{
	u32 uses = 0;
	if (classes & kGSTileGpuClassDate)
		uses |= GSDevice::kGSTileGpuSelfDate;
	if (classes & (kGSTileGpuClassExoticBlend | kGSTileGpuClassQuantisedBlend))
		uses |= GSDevice::kGSTileGpuSelfBlend;
	if (classes & (kGSTileGpuClassPartialMask | kGSTileGpuClassAfailKeep))
		uses |= GSDevice::kGSTileGpuSelfMask;
	return uses;
}

/// What a draw USES the in-pass destination read for: what it wanted, less what the budget refused.
/// Zero for the overwhelming majority of draws and for every draw on a device without the road, and
/// zero is what keeps that draw's pipeline, its indirect run and its pass exactly what they were.
constexpr u32 gsTileGpuSelfReadUses(u32 wanted_classes, u32 admitted_classes, bool self_read)
{
	return self_read ? gsTileGpuClassUses(wanted_classes & admitted_classes) : 0u;
}

/// A class's cost, in the unit the device actually charges in.
///
/// TWO terms, because the SD865 round (20260824, e4d6eddce3) said the one-term form was half a
/// model. The tax has two halves and a class pays both:
///
///   taxed draws     a draw sharing a declaring pass with an earlier draw of the same class pays a
///                   render-backend flush, because on sysmem a declaring pass rasterizes under
///                   GRAS_SC_CNTL.single_prim_mode = FLUSH_PER_OVERLAP_AND_OVERWRITE.
///   declared runs   ...and a declaring pass costs of its OWN, whatever it holds -- transitions,
///                   load forcing, the input-attachment bind. The first cost model scored a run of
///                   one draw as FREE, and the device refuted it in the loudest available way:
///                   FlatOut 2's exotic blends are 896 draws in 896 solo declared passes, scored
///                   ZERO, admitted, and the frame came back at 122.04 ms -- +120% against the arm
///                   before the budget existed and +383% against the arm before the read road
///                   existed, with the pass count going 235 -> 1,100.
///
/// So cost = taxed + alpha * runs, and alpha = 8/5.
///
/// ⚠️ alpha WAS 1, on an argument that was principled and that the device then overruled. The
/// argument: at alpha = 1 the cost is arithmetically taxed + runs = the class's draw count, which is
/// invariant to how those draws fall into passes -- and that invariance is worth having, because the
/// run counter is a number this planner cannot measure properly. It counts runs broken by the
/// attachment group changing and cannot see the pass breaks a prep op or a texture bind makes later
/// in the same draw; on Ratchet & Clank's effects it counts SEVEN runs where the device declared
/// 357 passes.
///
/// What overruled it: MGS3. Its exotic blends are 162 draws in 162 SOLO declared passes -- no
/// overlap anywhere for the flush to charge -- and the alpha probe (manager scaffold 1dd0ed3edd,
/// force-refusing that one class) took MGS3 from 29.1 ms to 19.734-19.745, which is the 19.725
/// pre-regression floor to within 0.05%, with the device pass count dropping by exactly the class's
/// own count, 633 -> 471. Its whole +48% residual was that class. At alpha = 1 it costs 162 and this
/// line admits it; the run term is simply real, and about 59 us a pass by that arithmetic, which
/// agrees with the ~77 us FlatOut 2's 896 declared passes implied.
///
/// The run-counter caveat survives the change, with its DIRECTION now stated: undercounting runs
/// UNDERPRICES a class and never the reverse, so the failure mode is admitting something that should
/// have been refused, and it is bounded by however badly the count is wrong. On the one class where
/// the undercount is known to be enormous -- Ratchet's 7 counted against 357 declared -- the taxed
/// term alone is 357 and already refuses it, so nothing there rides on the runs at all.
///
/// ⚠️ alpha is bounded on BOTH sides and 8/5 is chosen inside the window, not for tidiness:
///
///   a > 1.336   or MGS3 (162 runs) and Katamari's punctuation (195 taxed + 16 runs) cannot be
///               separated by any line at all, whatever it is set to.
///   a > 1.581   to put MGS3 past the refuse line where the device says it belongs, with the line
///               left where the previous round fitted it.
///   a <= 1.8125 or Katamari's 220 crosses the RE-ADMIT line, and a class that must be admitted
///               stops being able to come back after a transient spike refuses it. alpha = 2 puts
///               it at 227 and re-creates exactly the latch-off bug a 192 re-admit line would have
///               caused at alpha = 1.
///
/// 8/5 is the fraction in that window that leaves BOTH lines exactly where they were; 3/2 would
/// have needed the refuse line moved as well.
constexpr u32 kGSTileGpuRunCostNumerator = 8;
constexpr u32 kGSTileGpuRunCostDenominator = 5;

constexpr u32 gsTileGpuClassCost(u32 taxed_draws, u32 declared_runs)
{
	return taxed_draws + declared_runs * kGSTileGpuRunCostNumerator / kGSTileGpuRunCostDenominator;
}

/// The line, and it is a fit to a constraint set rather than a round number.
///
/// Per-class costs over the 18-dump corpus under the Adreno pass shape, at alpha = 8/5, with the
/// device's verdict on each where it ran:
///
///   must refuse   Xenosaga's alpha-MSB FBMSK 7118, FlatOut 2's exotic blends 1433, Baldur's Gate's
///                 16-bit blends 481, Ratchet & Clank's effect blends 368, MGS3's exotic blends 259
///   must admit    Katamari's punctuation FBMSK 220, OutRun's exotic blends 170 and 151, Beyond
///                 Good & Evil's FBMSK 74, Shadow of the Colossus' exotic blends 56, Yu-Gi-Oh's
///                 16-bit blend 56, GT4 Online Beta's 46, and twenty-odd classes under 40
///
/// MGS3's 259 and Katamari's 220 are the two that bind now, and they are what fixed ALPHA rather
/// than this line -- see gsTileGpuClassCost. The line itself has not moved since it was fitted
/// against Katamari and Ratchet, which is the point of having chosen 8/5 over 3/2.
constexpr u32 kGSTileGpuDeclaringRefuseAbove = 256;
/// ...and the re-admit line, the near edge of the hysteresis band.
///
/// A class hovering at one line would otherwise change what the frame is exact about every frame;
/// both renderings are valid, so the flicker is subtle rather than broken. The band has to satisfy
/// one property beyond being narrow, and the obvious 25% band does not: a class that MUST be
/// admitted has to be able to come BACK after any transient spike refuses it, so the re-admit line
/// must sit above the largest must-admit cost. That is Katamari's 211, so 192 would latch its
/// punctuation off for good after one bad frame. 224 clears it, and the band it leaves (225..256)
/// contains no measured class on the corpus at all.
constexpr u32 kGSTileGpuDeclaringReadmitAtOrUnder = 224;

/// The per-class declaring budget: which admission classes are worth their tax this frame, decided
/// from what each of them has recently cost.
///
/// A static per-class rule cannot work and the corpus says so three times over. Refusing the
/// partial-FBMSK class buys Xenosaga its whole toll for two changed pixels and costs Katamari its
/// punctuation; refusing the quantised-blend class buys Baldur's Gate Dark Alliance II its toll and
/// costs Katamari the quantisation of a full-screen composite. Two titles, opposite right answers,
/// one class. The variable that separates them is never the class -- it is the DENSITY, and the
/// renderer is the only thing that knows it.
///
/// One frame stale for a class that has been priced, deliberately. Targets persist across the frame
/// boundary and content is stable frame to frame, so last frame's density is this frame's density;
/// this is the same Classic-grade-staleness bet the rest of the design already makes. A verdict that
/// moved mid-frame for a priced class would key two draws of one class into different passes for no
/// reason the measurement asked for.
///
/// ⚠️ An UNPRICED class is the exception, and it is the whole of the startup story. See Charge.
struct GSTileGpuDeclaringBudget
{
	/// This frame, per class: draws standing behind another draw of the same class, and the number
	/// of consecutive runs of the class. The two terms of the cost.
	std::array<u32, kGSTileGpuAdmissionClasses> taxed{};
	std::array<u32, kGSTileGpuAdmissionClasses> runs{};
	/// ...and what the class has cost RECENTLY: a running peak of gsTileGpuClassCost that decays a
	/// quarter each frame, which is the number the verdict is actually made on.
	///
	/// ⚠️ Not the last frame's cost, and this is not a refinement -- a budget reading one frame is
	/// wrong on half the corpus. A title presenting at 30 Hz over a 60 Hz vsync draws nothing at all
	/// in every second frame, so a one-frame-lagged verdict is decided by the EMPTY frame and the
	/// heavy frame is admitted, every time: Xenosaga's FBMSK alternates 4805 and 0, and read one
	/// frame at a time it was refused in only 62% of frames while the frames it was refused in were
	/// the cheap ones. The peak asks "has this class been expensive lately", which is the question,
	/// and a quarter a frame means about eight frames of genuine quiet before a class that has
	/// calmed down is re-admitted.
	std::array<u32, kGSTileGpuAdmissionClasses> peak{};
	/// The verdict in force. Constant for the frame for a priced class; see Charge for the other.
	u32 admitted = kGSTileGpuClassAll;
	/// Classes that have completed a frame in which they were seen, and so have a peak worth
	/// believing. A class outside this set is UNPRICED.
	u32 priced = 0;
	/// Whether this device charges for declaring at all (GSDevice::TileGpuSegregatesSelfRead).
	/// Latched once, here, rather than passed to each call: it cannot change without a renderer
	/// restart, and two copies of one answer is two chances to disagree.
	bool taxes = false;

	constexpr void Start(bool declaring_taxes_the_pass)
	{
		taxes = declaring_taxes_the_pass;
		admitted = kGSTileGpuClassAll;
		priced = 0;
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
		{
			taxed[c] = 0;
			runs[c] = 0;
			peak[c] = 0;
		}
	}

	/// Charge one draw. `wanted` is every class it would be admitted for; `opening` is the subset of
	/// those whose consecutive run this draw STARTS.
	///
	/// ⚠️ This is also where an UNPRICED class is refused, mid-frame, the moment its own running cost
	/// crosses the line -- the one deliberate exception to the frame-constant verdict, and it exists
	/// because the alternative was measured and is user-visible.
	///
	/// A class with no peak cannot be judged, so something has to be assumed about the first frame it
	/// appears in. Assuming the worst -- start the bulk-capable classes refused -- costs one frame of
	/// approximation, and one frame of approximation is NOT transient: targets persist by design, so
	/// anything drawn once into a target later frames do not redraw keeps whatever frame one wrote.
	/// On the device that was Katamari's punctuation, missing from the screen, permanently, because
	/// the glyph store is written once. Assuming the best -- start everything admitted -- costs one
	/// whole admitted frame, which on Xenosaga was 303 ms and a kernel-level GPU fault.
	///
	/// So: start admitted, and stop the moment the class proves expensive. Exposure is bounded by the
	/// line rather than by the frame -- about 256 draws' worth, not 2,409 -- and a class whose whole
	/// frame fits under the line is never refused at all, which is exactly the case the permanent
	/// damage was in. Exactness is MONOTONE within a frame: admitted to refused, never back.
	constexpr void Charge(u32 wanted, u32 opening)
	{
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
		{
			const u32 bit = 1u << c;
			if ((wanted & bit) == 0)
				continue;
			if ((opening & bit) != 0)
				runs[c]++;
			else
				taxed[c]++;
			if (taxes && (priced & bit) == 0 && (admitted & bit) != 0 &&
				gsTileGpuClassCost(taxed[c], runs[c]) > kGSTileGpuDeclaringRefuseAbove)
				admitted &= ~bit;
		}
	}

	/// End of frame: this frame's cost joins the peak, the peak decides the next frame's verdict, and
	/// the per-frame counters reset.
	///
	/// A device that does not charge for declaring re-admits everything unconditionally rather than
	/// being left at whatever the last verdict was -- the budget must be inert there, not merely idle.
	constexpr void Roll()
	{
		// ⚠️ AN EMPTY FRAME IS NOT EVIDENCE. A title presenting at 30 Hz over a 60 Hz vsync draws
		// nothing at all in every second frame, and letting those frames decay the peak says "the
		// class got cheaper" when what happened is that nothing was drawn. It is the same trap the
		// peak itself exists to avoid, one level down, and it is not hypothetical: MGS3's exotic
		// blends cost 259 against a 256 line, one decay step takes that to 195, 195 is under the
		// re-admit line, so the class came back on every off-frame -- and being re-admitted it was
		// unpriced too, so the bootstrap then let 161 of its 162 draws through before the guard
		// fired. Measured that way it was "refused in 100% of frames" and still paying 161 of its
		// 162 declared passes.
		//
		// So a frame in which nothing was charged at all changes nothing: not the peak, not the
		// verdict, not `priced`. A frame that DID draw but not this class still decays it, which is
		// the case the decay is for.
		bool charged = false;
		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
			charged = charged || taxed[c] != 0 || runs[c] != 0;
		if (!charged)
			return;

		for (u32 c = 0; c < kGSTileGpuAdmissionClasses; c++)
		{
			const u32 bit = 1u << c;
			const u32 cost = gsTileGpuClassCost(taxed[c], runs[c]);
			const u32 decayed = peak[c] - peak[c] / 4;
			peak[c] = cost > decayed ? cost : decayed;
			// A class seen for a whole frame now has a peak worth believing, so it stops being
			// bootstrapped and takes the frame-constant verdict from here on.
			if (taxed[c] != 0 || runs[c] != 0)
				priced |= bit;
			if (!taxes)
				admitted |= bit;
			else if ((admitted & bit) != 0)
			{
				if (peak[c] > kGSTileGpuDeclaringRefuseAbove)
					admitted &= ~bit;
			}
			else if (peak[c] <= kGSTileGpuDeclaringReadmitAtOrUnder)
			{
				// Re-admitting a class the budget once refused is a GUESS again -- the peak that
				// justifies it was measured while the class was refused and the scene has moved on.
				// So it goes back to being unpriced, and the frame that tries it gets Charge's
				// mid-frame guard rather than a whole frame at whatever the class now costs.
				admitted |= bit;
				priced &= ~bit;
			}
			taxed[c] = 0;
			runs[c] = 0;
		}
	}
};

/// Which BLOCKS of which GS pages a surface's pool texture actually holds texels for.
///
/// The memory model tracks guest bytes at block granularity and this has to answer in the same
/// units, because the two are read against each other: the model says a page's bytes are the
/// surface's, and the seed gate then asks whether the surface's TEXTURE has them. A page bitmap
/// standing in for a 32-bit block mask can only answer that question by rounding, and the
/// rounding that costs nothing is the one that over-claims -- a page counted held whose texels
/// are half allocator leftovers, sampled by the present or written back into the ring as if it
/// were guest data.
///
/// Nothing may be marked without a proof that those blocks were written. Two roads have one: a
/// seed, which fills whole pages out of the ring, and a draw the planner has already proved
/// writes every fragment it covers, which fills the blocks its (scissor-clipped) rect covers in
/// full. Everything else stays unheld, and unheld only ever costs a seed.
class GSTileBlockFill
{
public:
	void Clear()
	{
		m_whole.clear();
		m_partial.clear();
		m_masks.clear();
	}

	/// `mask`'s blocks of `page` now hold texels.
	void Mark(u32 page, u32 mask);
	/// Every block of every page in `pages` now holds texels.
	void MarkWhole(const GSPageBitmap& pages);

	/// The blocks of `page` that hold texels.
	u32 BlocksOf(u32 page) const;
	/// Does every page of `pages` hold texels in all 32 blocks?
	bool HoldsWhole(const GSPageBitmap& pages) const { return pages.andnot(m_whole).empty(); }
	/// The pages of `pages` that do not: what a seed has to bring in.
	GSPageBitmap MissingFrom(const GSPageBitmap& pages) const { return pages.andnot(m_whole); }

private:
	GSPageBitmap m_whole; ///< all 32 blocks held
	GSPageBitmap m_partial; ///< some blocks held; the mask lives in m_masks
	std::unordered_map<u16, u32> m_masks;
};

/// ComposeRingPages' owner grouping: which surfaces must write back, over which pages, in the
/// order the ops have to be emitted.
///
/// Every page/plane of a read window carrying unsynced truth joins its owner's bucket, and each
/// bucket becomes one writeback op. This was a fixed eight-entry array, on the stated belief that
/// a read window sees one or two owners. A game that tiles one buffer by walking FRAME.FBP breaks
/// that belief outright -- one surface per tile, so owners-per-gather scales with the tile count:
/// MGS3's post-process scratch is 55 one-page targets and its full-screen composite gathers 49
/// owners at once. The ninth and later were dropped silently: no writeback emitted, the ring slot
/// force-prefilled from the CPU shadow, the page marked synced anyway, and every later reader
/// served stale guest memory (on screen, blocky garbage blended over the whole frame).
///
/// So the scratch is sized by the model's surface-id space rather than by a constant -- ids are
/// dense u16s and the whole thing is reused across gathers, so it allocates once and clears only
/// the entries it touched. A constant here is a silent cliff whatever its value; this one was
/// blown by six times.
class GSTileComposeBuckets
{
public:
	/// Group `need`'s unsynced page truth by owning surface. An owner without a byte road is not
	/// grouped -- nothing can carry its bytes -- and its page/plane hits are counted lossy instead.
	void Gather(const GSVramModel& model, const GSPageBitmap& need);

	u32 Count() const { return static_cast<u32>(m_order.size()); }
	GSTileSurfaceId IdAt(u32 i) const { return m_order[i]; }
	const GSPageBitmap& PagesAt(u32 i) const { return m_pages[m_order[i]]; }

	u32 LossyPages() const { return m_lossy; }
	u32 LossyDepthPages() const { return m_lossy_depth; }

private:
	std::vector<GSPageBitmap> m_pages; ///< by surface id; only the ids in m_order are this gather's
	std::vector<u64> m_stamp; ///< by surface id: the gather serial that last touched it
	std::vector<GSTileSurfaceId> m_order; ///< emission order == first page-ascending appearance
	u64 m_serial = 0;
	u32 m_lossy = 0;
	u32 m_lossy_depth = 0;
};

// Stage-1 contract: wrong-fast where the executor is (no blending, no per-draw write masks);
// EXACT where the model is -- every page's truth is named, every crossing is counted.
class GSRendererTileGpu final : public GSRenderer
{
public:
	GSRendererTileGpu();
	~GSRendererTileGpu() override;

protected:
	void Destroy() override;
	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
	void Draw() override;
	GSTexture* GetOutput(int i, float& scale, int& y_offset) override;

	// No coverage-alpha (AA1) path — same reasoning as GSRendererNull: without this override
	// the base GSState version pxFailRel("Not implemented")s on AA1 games (e.g. Shadow of the
	// Colossus). Revisited with blending.
	bool IsCoverageAlphaSupported() override;

	// The stream's non-draw memory events. Each drives the memory model (the truth of the
	// affected pages moves or is pulled) AND the pass-structure observer. The base hooks are
	// empty -- the actual transfer already happened in GSState's transfer path -- but the
	// upload hook fires BEFORE the shadow write lands, which is what lets an in-flight
	// version of the page's old bytes be captured (see SupersedeRingSlots). Move performs the real
	// guest copy through the base, which itself drives the two hooks.
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override;
	void Move() override;

	// The whole-of-truth seams: every page a target holds newest comes down to the CPU shadow
	// (savestate, renderer switch, close), and a purge drops the targets after that.
	void ReadbackTextureCache() override;
	void PurgeTextureCache(bool sources, bool targets, bool hash_cache) override;

private:
	// The pass-structure observer: the round-2 GSTilePassSim model fed by the live GSState
	// decode, ported from the Tile renderer's PassSimObserveDraw feeder. It is the reference
	// accounting for the GS-semantic minimum pass structure; the plan below is the actual one.
	//
	// A CROSS-CHECK, and nothing downstream of it: the planner's inputs are AccumulateDraw and
	// the memory model, and no field of this object is ever read back into a plan. So it is a
	// lever -- EmuCore/GS/TileGpuPassSim, gsrunner -tilepasssim -- off by default, because
	// feeding it costs roughly 0.4 ms/frame and a timed run must not pay that. Every call site
	// is guarded by IsActive(), the Tile renderer's own idiom; the flag is read once, at
	// construction, so flipping it mid-run needs a renderer restart.
	GSTilePassSim m_pass_sim;

	// Set only for the duration of the base GSRenderer::Move() call. A GS->GS move reaches
	// the sim once as OnMove, which already accounts both halves; the base copy underneath
	// then fires InvalidateLocalMem(src) and InvalidateVideoMem(dst), whose sim forwards
	// would double-count the move. The two hooks gate their sim forward on this flag; the
	// memory model still sees both halves through them (a move IS a CPU read then a CPU write
	// as far as truth is concerned).
	bool m_pass_sim_in_move = false;

	// --- the memory model ------------------------------------------------------------------
	GSVramModel m_vram_model;
	GSTileTargetPool m_target_pool;

	// The screen-space bbox of the current draw, BEFORE the scissor. Split out of ComputeDrawRect
	// so a caller can tell a scissor that rejects part of this draw from one that does not.
	GSVector4i ComputeDrawBBox() const;

	// The screen-space bbox of the current draw, scissor-clipped — the Tile renderer's
	// ComputeDrawRect, which reads only base state, replicated here (it is not a base method).
	GSVector4i ComputeDrawRect() const;

	// Feed one flushed primitive batch to the pass model: derive its page footprints,
	// classify its in-pass reads, and observe it. Fired once per Draw().
	void ObserveDraw();
	/// Why this draw would need to read its own destination pixel, as GSTilePassSim::ReaderFlag bits.
	/// One reader for the pass-structure census and for admission to the actual read, so the two
	/// cannot come to describe different renderers.
	u32 ReaderFlags(bool color_written);
	/// Which of `wanted_classes` this draw is actually admitted for: the budget's verdict for this
	/// frame, or everything where the device does not tax declaring or -tilermw is forcing.
	u32 AdmittedClasses(u32 wanted_classes) const;
	/// Charge one draw against the budget, and count it into the frame's per-class census. `group` is
	/// the draw's attachment key -- colour surface, depth surface, depth mode -- which is what a run
	/// of same-class draws is counted within.
	void ChargeDeclaringBudget(u32 wanted_classes, const GSTileGpuPassKey& group);

	// Mean/p50 of the accumulated per-frame pass structure and of the memory model's traffic,
	// emitted at teardown.
	void ReportPassStructure();
	void ReportModelTraffic();

	// --- the pass plan the executor consumes --------------------------------------------
	// One row of the executor's indexed state table: the per-draw state a shader reads via
	// gl_InstanceIndex (the indirect draw's first_instance). Its layout is this backend's
	// contract with its own shader (tilegpu.glsl's StateRow, std430, 144 bytes), opaque to the
	// executor (which stages it by state_stride). The transform is the HW tfx VertexScale/
	// VertexOffset reused verbatim; the texture block is wrong-fast — direct 32-bit and
	// paletted, MODULATE/DECAL, nearest — and grows to the rest of the fixed-function state.
	struct alignas(16) StateRow
	{
		float vertex_scale[2];  // maps raw 12.4 screen XY to clip, per the tfx VS
		float vertex_offset[2];
		u32 z_write;
		u32 z_test;
		u32 tex_enable;         // 1 = sample the guest texture, 0 = vertex-colour only
		u32 fst;                // 1 = FST/UV coords, 0 = STQ coords
		u32 tbp0;               // TEX0.TBP0, texture base in blocks
		u32 tbw;                // pages per texture row (TBW, or TBW>>1 for paletted); AccumulateDraw
		u32 tw;                 // texture width  in texels (1 << TW)
		u32 th;                 // texture height in texels (1 << TH)
		u32 tfx;                // TEX0.TFX texture function
		u32 tcc;                // TEX0.TCC: 1 = texture carries alpha, 0 = alpha from vertex
		u32 wms;                // CLAMP.WMS horizontal wrap mode
		u32 wmt;                // CLAMP.WMT vertical wrap mode
		u32 index_format;       // 0 = direct 32-bit texel, 1 = PSMT8 index, 2 = PSMT4 index
		u32 pal_offset;         // word offset of this draw's palette in the frame stream
		u32 epoch;              // page-table epoch this draw's byte reads go through
		u32 date;               // destination-alpha test: 0 off, 1 pass on alpha bit 7 clear, 2 on set
		u32 fge;                // 1 = PRIM.FGE, the fragment's colour walks toward the fog colour
		u32 fogcol;             // FOGCOL packed 0x00BBGGRR
		u32 atst;               // 0 = no alpha test; else TEST.ATST + 1 (2 = LESS ... 8 = NOTEQUAL)
		u32 aref;               // TEST.AREF, the value the fragment alpha is compared against
		u32 texa;               // 24-bit texel alpha: bit 0 = apply, bit 1 = TEXA.AEM, bits 8-15 = TEXA.TA0
		u32 region_u;           // CLAMP.MINU | (CLAMP.MAXU << 16), for the two REGION wrap modes
		u32 region_v;           // CLAMP.MINV | (CLAMP.MAXV << 16)
		u32 ltf;                // 1 = bilinear: the fragment blends the four texels around its coordinate
		u32 tex_target;         // slot in this pass's sampled-target array, or kNoTexSlot = decode bytes
		// Rule 3: slot in the FRAME's materialised-source array, or kNoSourceSlot. It took the row's
		// old explicit tail padding, which was there because alignas(16) pads the C++ side to 144
		// anyway while the shader's std430 array stride is a multiple of 8 -- an implicit tail would
		// have put the two sides on different strides and read every row but the first from the wrong
		// place. Spending it on a real field keeps the strides in step.
		u32 tex_source;
		// Where this draw's palette words are. 0 = entry order at pal_offset, which is the CPU road
		// and what every draw carried before the CLUT gather existed; 1 = a 256-entry palette's four
		// copied blocks; 2 = a 16-entry one's single copied block. The gather's byte-road consumer
		// gets its words by an image-to-buffer COPY of the palette's blocks out of the owning target
		// (no gather pass, at six hundred to twelve hundred loads a frame), and a copy lands texels
		// row-major rather than in entry order -- so the CSM1 entry order is applied at fetch instead.
		u32 pal_mode;
		// The entry bias inside a copied palette: a four-bit draw reading mirror slot k of a
		// 256-entry gathered load wants that palette's entries 16k..16k+15.
		u32 pal_bias;
		// The GS blend equation, for a draw the fixed-function state cannot express and that
		// therefore reads its own destination -- in gsTileGpuPackBlend's encoding, which packs the
		// COEFFICIENTS the register's selectors add up to, not the selectors (see GSTileTypes.h for
		// why that is a size decision): bits 0-1 = coefficient of Cs in (A - B) biased +1, 2-3 =
		// coefficient of Cd biased +1, bit 4 = D is Cs, bit 5 = D is Cd, 6-7 = C (0 As, 1 Ad,
		// 2 constant), 8-15 = the constant, bit 16 = blend at all, 17 = COLCLAMP is WRAP rather than
		// clamp, 18 = PABE, 20 = quantise to the 16-bit frame's bits, 21 = AFAIL keeps the
		// destination alpha. A PSMCT24 destination's C=Ad is packed as the constant 0x80 (exactly
		// 1.0). The enable bit is zero on every draw the executor blends for.
		u32 blend;
		// FRAME.FBMSK reduced to the bits the frame FORMAT actually stores (FBMSK & fmsk), as a
		// per-channel KEEP mask on the target's expanded RGBA8 bytes. A 16-bit frame's mask lands
		// here unchanged because the console packs FBMSK by the same 5551 packing as the colour, so
		// bit k of a stored channel is bit k of the expanded byte. Read only by a draw that reads
		// its destination; the channel-granular half of the same mask stays in the pipeline.
		u32 fbmsk;
		// Explicit tail padding, and it has to be explicit: alignas(16) rounds the C++ row up to
		// 144 while std430's array stride over the fields above is 136, and two sides on different
		// strides read every row but the first from the wrong place. The asserts below are what
		// say the row still ends where the shader thinks it does. A new field goes HERE, spending
		// a padding word rather than appending past it -- which is what the scissor's four words
		// and the pixel origin's two did in reverse when the clip-plane road was deleted.
		u32 pad0, pad1;
	};
	static_assert(sizeof(StateRow) == GSDevice::GSTileGpuPassPlan::kStateRowWords * sizeof(u32),
		"TileGpu StateRow must be kStateRowWords words to match tilegpu.glsl std430");
	static_assert(offsetof(StateRow, pad0) == (GSDevice::GSTileGpuPassPlan::kStateRowWords - 2) * sizeof(u32),
		"TileGpu StateRow must end at its declared size with no implicit tail padding -- std430's stride would differ");

	// One draw's inputs the plan build resolves once the frame is complete: which surfaces it
	// renders into (model ids -> pool textures), the coordinate origin, the draw rect, and the
	// range of reconciliation ops the model emitted for it. draw_index points at the matching
	// m_plan_draws row.
	struct PendingDraw
	{
		GSTileSurfaceId color_surface;
		GSTileSurfaceId z_surface; // valid only if z_used
		bool z_used;
		bool z_write, z_test;
		// The pipeline variant z_used/z_test/z_write add up to, derived ONCE at accumulation
		// (gsTileGpuDepthModeFor) and read by everything downstream. The three bools above stay
		// because the draw's state row carries them to the shader.
		GSDevice::GSTileGpuDepthMode depth_mode;
		bool break_before;    // this draw's prep ops cannot be hoisted over the open pass: it opens a new one
		s32 ofx, ofy;         // XYOFFSET, 12.4 fixed
		GSVector4i rect;      // scissor-clipped draw bbox
		GSVector4i scissor;   // the GS scissor (SCISSOR register), exclusive right/bottom
		// The scissor rejects part of THIS draw, as against merely being narrower than the target.
		// The two are different populations: a draw wholly inside a narrow scissor loses nothing.
		bool scissor_cuts;
		u32 draw_index;
		u32 first_prep_op;    // reconciliation ops that must run before this draw's pass
		u32 prep_op_count;

		// Texture inputs, resolved at accumulation. tex_enable is set for the formats this stage
		// samples (direct 32-bit and paletted); every other textured draw falls back to the
		// vertex-colour path. See AccumulateDraw.
		bool tex_enable;
		/// This draw's composed read window covers a page it also renders into -- it samples its own
		/// target. Not the same question as `self_mask`, which is the in-pass DESTINATION read (the
		/// pixel under the fragment); this is the ordinary feedback shape, a game rendering into a
		/// scratch buffer and reading it back through TEX0. Recorded because it is the one thing a
		/// draw's write and read page sets cannot be asked apart: a draw that tests depth and writes
		/// it has z_pages on both sides for a reason that is not feedback at all.
		bool tex_reads_own_target;
		bool fst;
		u32 tbp0, tbw, tw, th, tfx, tcc, wms, wmt;
		u32 index_format, pal_offset;
		u32 epoch;
		u32 date;             // 0 off, 1 = DATM 0, 2 = DATM 1; served by the in-pass read where the
		                      // device has it, and by a pass snapshot where it does not
		// What this draw needs the in-pass destination read FOR (GSDevice::kGSTileGpuSelf*), zero for
		// a draw the fixed-function state expresses exactly. The pass's self_mask is the OR over its
		// draws, so this is what decides how much shader a declaring pass compiles -- and non-zero is
		// what puts GSTileGpuPassPlan::kSelfRead in the draw's blend key.
		u32 self_mask;
		// The GS blend equation and the bit-granular write mask the fragment stage would need to do
		// this draw itself, in the state row's own encoding (see StateRow::blend / StateRow::fbmsk).
		// Filled for every draw, not just admitted ones: the encoding is two shifts and a mask, and a
		// row that only sometimes means something is how a field comes to be read in the wrong state.
		u32 self_blend;
		u32 self_fbmsk;
		/// This draw's OUTPUT is what lands in a frame format that stores fewer bits than the target's
		/// RGBA8 image holds, so the fragment stage has to say what the console would have stored. Not
		/// set for a draw the fixed-function blend unit still has to touch: the console quantises the
		/// blend's result, not its source, and the blend happens after the fragment stage.
		bool quantise_5551;
		/// No fragment of this draw carries an alpha byte above 0x80, so the As blend factor
		/// min(As * 255/128, 1) does not clamp anywhere in it. Set only for the draws the classifier
		/// scans (a blend with the destination in it and C = As); false everywhere else, including
		/// where the answer was simply never asked.
		bool as_unclamped;
		/// What the fragment stage must do with the As blend factor when there is no second colour
		/// output to hand it to (kGSTileBlendAlphaCarrier: put it in o_color.a). Zero on every draw
		/// on a device with dual-source blending, on every draw whose row names no As factor, and on
		/// the companion draw that writes the alpha byte the carrier displaced.
		u32 dualsrc_bits;
		/// Which of the blend row's two factors the GS's own source alpha supplies
		/// (GSDevice::kGSTileGpuDualSrc*), and which road that draw's factor WOULD take on a device
		/// with no second colour output (GSDevice::GSTileGpuDualSrcRoad). Both filled on every
		/// device, because the census has to say what the carrier roads cost even on an arm that
		/// does not take them; only the road's application is gated on the device.
		u32 dualsrc_terms;
		u8 dualsrc_road;
		/// This draw's failing fragments land their RGB and keep the destination's alpha instead of
		/// being discarded (gsTileGpuAfailKeepsAlpha). A per-fragment write mask, so it rides the same
		/// fragment arm the bit-granular FBMSK merge does.
		bool afail_keep_alpha;
		bool fge;             // PRIM.FGE: this draw's fragments are fogged
		u32 fogcol;           // FOGCOL packed 0x00BBGGRR
		u32 atst;             // 0 = no per-fragment alpha test; else TEST.ATST + 1
		u32 aref;
		u8 color_mask;        // rgba channels this draw lands (GSTileTypes.h's
		                      // gsTileFrameColorWriteMask: FBMSK per channel, alpha included,
		                      // with the all-fail AFAIL fold on top); the pipeline's colour
		                      // write mask. Zero is a depth-only draw.
		/// ...unless the FRAGMENT stage is serving that mask instead (gsTileGpuMasksInShader), in
		/// which case the pipeline writes all four channels and self_fbmsk carries the dropped ones
		/// as whole keep bytes. The point of the move is that the mask then stops cutting the run.
		bool mask_in_shader;
		u32 texa;             // 24-bit texel alpha: bit 0 apply, bit 1 AEM, bits 8-15 TA0
		u32 region_u, region_v; // CLAMP MIN | (MAX << 16) per axis, for the REGION wrap modes
		bool ltf;             // TEX1 asks for LINEAR on the side of the LOD this draw sits on

		// Rule 2 of the VRAM model's texel road: the resident target that holds this draw's whole
		// read window, or kGSTileNoSurface for the byte road. When set, nothing was composed into
		// the ring for this read -- the fragment stage samples the target's live pixels.
		GSTileSurfaceId tex_source;
		u32 tex_slot;         // its slot in the pass's bind table, resolved once the pass is known

		// Which texel road this draw takes (GSDevice::kGSTileGpuRoad*), zero when it is untextured.
		// The pass's variant is the OR over its draws, so this is what decides how much shader the
		// pass compiles -- see GSDevice::GSTileGpuPass::road_mask.
		u32 road_mask;

		// Rule 3 of the same road: this draw's slot in the FRAME's materialised-source bind table
		// (m_plan_sources), or kNoSourceSlot where rule 3 refused and the draw kept the byte road.
		// Frame-wide rather than per-pass, because a source belongs to a texture window and serves
		// draws in any number of passes. Set only after the cache has actually handed over an image.
		u32 src_slot;

		// The CLUT gather's per-draw link. `pal_record` is the handle of the device palette every
		// mirror slot this draw reads was stamped by, or 0 when the draw's palette words came off the
		// CPU as they always did. `pal_bias` is where inside that palette the draw's entries start
		// (non-zero only for a four-bit draw on a slot of a 256-entry load), `pal_mode` the state
		// row's fetch mode, and `pal_id` the palette's content identity for the expanded cache --
		// derived from the load's tuple rather than from words nobody has.
		u32 pal_record;
		u32 pal_bias;
		u32 pal_mode;
		u64 pal_id;
	};

	// Per-frame accumulation (filled in Draw via AccumulateDraw, consumed + reset in the plan
	// build).
	std::vector<GSVertex> m_plan_vertices;
	std::vector<u16> m_plan_indices;
	std::vector<StateRow> m_plan_states;
	std::vector<u32> m_plan_palettes; // expanded CLUTs (GSClut::Read32), concatenated per frame
	std::vector<GSDevice::GSTileGpuIndirectDraw> m_plan_draws;
	std::vector<GSDevice::GSTileGpuTopology> m_plan_topologies; // one per m_plan_draws entry
	// One per m_plan_draws entry: the draw's GS scissor (GSTileGpuPassPlan::scissors). Filled from
	// m_plan_pending at the plan build rather than appended at accumulation, so it cannot come out
	// short on the road neither append site remembered to touch. Read only by a device on the
	// per-call scissor road; carried on both, because a stream that exists on one device and not on
	// another is a difference no test on this box can see.
	std::vector<GSVector4i> m_plan_scissors;
	std::vector<u32> m_plan_blend_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::blend_keys)
	std::vector<u32> m_plan_bind_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::bind_keys)
	// One per m_plan_draws entry (GSTileGpuPassPlan::variant_keys): the draw's own fragment variant,
	// packed. Resolved with the bind keys rather than at accumulation, because the DATE axis of it is
	// a property of the pass the draw lands in (whether that pass took a snapshot copy) and pass
	// membership is not known until the grouping below has run.
	std::vector<u32> m_plan_variant_keys;
	// One per m_plan_draws entry (GSTileGpuPassPlan::depth_modes): the draw's depth pipeline
	// variant. Pipeline state like the topology and the blend key, so it cuts the indirect run and
	// never the pass.
	std::vector<GSDevice::GSTileGpuDepthMode> m_plan_depth_modes;
	// Rule 3's frame-wide bind table: the (image, sampler) pairs this frame's draws sample, in the
	// slot order a state row's tex_source names. Deduped on the pair -- one window sampled through two
	// different TEX1/CLAMP settings is two slots, because the filtering and the wrap ride in the
	// descriptor and not in the shader.
	std::vector<GSDevice::GSTileGpuPassPlan::SourceBind> m_plan_sources;
	std::vector<PendingDraw> m_plan_pending;
	// The GS pages each pending draw WRITES and READS -- one entry per m_plan_pending entry, in the
	// same order, so index d names the same draw in all three.
	//
	// Accumulation already builds every page set these are made of (fb_pages, z_pages, tex_pages,
	// and the gathered palette's own load pages) and throws them away at the end of the draw. What
	// the pair is FOR is a question those locals cannot answer once the draw is over: does this draw
	// touch any GS page that some OTHER draw of this frame touches? That is the whole admission test
	// draw reordering rests on -- two draws whose footprints are disjoint can be emitted in either
	// order, and page disjointness is exact where a texture-pointer comparison is not.
	//
	// The read side is deliberately the WHOLE composed read window (the size-fixed TEX0 footprint,
	// every mip), not the texels the draw's coordinates actually reach -- the same pessimism
	// OpenRun::read already carries, and for the same reason: the window is what the sampler is free
	// to fetch.
	//
	// 64 bytes each, so 128 bytes a draw -- about 220 KB a frame on the corpus's longest draw list.
	std::vector<GSPageBitmap> m_plan_write_pages;
	std::vector<GSPageBitmap> m_plan_read_pages;
	std::vector<GSDevice::GSTileGpuPass> m_plan_passes;
	std::vector<GSDevice::GSTileGpuTargetPair> m_plan_target_pairs;
	std::vector<GSDevice::GSTileGpuSnapshotCopy> m_plan_snapshots;
	std::vector<GSDevice::GSTileGpuPrepOp> m_plan_prep_ops;
	std::vector<GSDevice::GSTileGpuPageEntry> m_plan_page_entries;
	/// The keep tables a writeback page entry's `keep_mask_words` indexes. Empty on every frame
	/// that plans no upload merge, which is most of them.
	std::vector<u32> m_plan_writeback_keep_masks;
	std::vector<u32> m_plan_tex_sources; // per pass: the rule-2 targets its draws sample, as target indices
	// The images this frame's Materialise prep ops build into (GSTileGpuPassPlan::prep_textures).
	// Deliberately NOT the target list: a materialised source is nothing's render target, no target
	// pair names it, and the page model does not own it -- sharing the index space would put a
	// non-surface into every place a target index is read as a surface.
	std::vector<GSTexture*> m_plan_prep_textures;
	std::vector<GSTileSurfaceId> m_plan_target_surfaces; // the plan's target list, as surfaces
	std::vector<GSTexture*> m_plan_targets; // ...resolved to pool textures at the plan build
	std::vector<u32> m_plan_target_of_surface; // surface id -> index into the target list (kNoTarget = absent)

	// --- the frame's byte ring -------------------------------------------------------------
	// One entry per (page, epoch range) the frame's plan reads or reconciles; the executor gives
	// each an 8 KB slot. `src` names the bytes it starts from: the CPU shadow's page (read at
	// plan build), a version copy in m_ring_versions (the page's bytes as they were before an
	// upload superseded them mid-frame), or nothing (the GPU composes every byte).
	struct RingEntry
	{
		u16 page;
		u16 epoch_first;
		u16 epoch_last;   // closed when a later CPU write supersedes the slot; else the last epoch
		bool prefill_s;   // start from S (or the version copy) -- some byte of the page is CPU-newest
		bool versioned;   // src is m_ring_versions + version_offset, not S
		u32 version_offset;
	};
	std::vector<RingEntry> m_ring_entries;
	std::vector<u8> m_ring_versions; // version copies, 8 KB each, appended as uploads supersede live slots
	std::array<u16, GS_MAX_PAGES> m_ring_live{}; // page -> index+1 of its live entry this frame, 0 = none
	u32 m_epoch = 0; // current page-table epoch; bumps when a CPU write supersedes a live slot

	// Give `page` a ring slot for the current epoch (or find its live one), and mark the S
	// prefill if any of its bytes are CPU-newest right now. Returns the entry index.
	//
	// `force_prefill` is the upload merge's, and it is a correctness requirement rather than a
	// hint: the merge's whole mechanism is that the slot CARRIES the transfer's bytes out of the
	// shadow, and the model cannot see that yet -- it still says the surface holds the page, which
	// on a whole-page claim reads as "the GPU composes every byte of this, prefill nothing".
	u32 EnsureRingSlot(u32 page, bool force_prefill = false);

	// A CPU write is about to land on `pages`: every live slot among them captures the page's
	// current S bytes as a version copy and closes at this epoch, so draws already planned keep
	// reading the bytes they saw; the epoch advances if anything closed. Called from the upload
	// hook BEFORE GSState writes the shadow (that is why the copy is exact).
	void SupersedeRingSlots(const GSPageBitmap& pages);

	// Compose `pages`' byte truth into ring slots for the current epoch: writebacks for every
	// colour surface holding unsynced truth on them (appended as prep ops on the pending draw),
	// S prefill for the rest, and the model's synced bit set for what the ring now holds. Depth
	// and unsupported-format owners have no writeback road yet: their bytes stay whatever the
	// slot was prefilled with, counted as lossy.
	void ComposeRingPages(const GSPageBitmap& pages);

	// ComposeRingPages' scratch, a member so the per-surface-id arrays are allocated once and
	// reused rather than sized by a constant that a game can walk past.
	GSTileComposeBuckets m_compose_buckets;

	// -- the CPU->GPU upload merge ---------------------------------------------------------------
	//
	// An upload covering only PART of a block a target holds newest used to be the only road on
	// which the GS thread waited for the device, because the block's bytes end up split between the
	// two sides and the merge has to happen where both halves are. This puts it on the GPU: the ring
	// slot carries the CPU's half (its prefill is the shadow, which holds the transfer's bytes by
	// the time the plan is built), a byte-masked writeback fills in the target's half, and a seed
	// reads the merged page back into the target. See EmitUploadMerge.

	/// The pages of `spill` the merge can take, with its groups and keep tables staged in the
	/// scratch below. Asked BEFORE the residual readback, because the readback's own plan flush must
	/// not carry merge ops (their ring slot would then be prefilled from a shadow the transfer has
	/// not been written into yet).
	GSPageBitmap PlanUploadMerge(const GSTileSurfaceLayout& layout, const GSVector4i& r,
		const GSVramModel::RectFootprint& fp, const GSPageBitmap& spill);
	/// Emit what PlanUploadMerge staged: per owner, a draw-less pass carrying its writebacks and
	/// the seeds that read the merged pages back into its texture. Moves no truth -- see the body.
	void EmitUploadMerge();

	// Public only so the oracle suite can reach the mask builder with no device and no
	// renderer, exactly as PlanPull is -- see gs_tilegpu_upload_merge_tests.cpp. The members
	// below use the type, so it has to be declared here rather than beside PlanPull.
public:
	/// One guest page of a host->local transfer, described at the byte granularity the merge needs.
	struct UploadPageBytes
	{
		u16 page;
		u32 blocks;       ///< blocks of the page the write touches at all
		u32 blocks_whole; ///< ...of those, the ones every byte of which it writes
		/// This page's keep table: kGSTileGpuKeepMaskWordsPerPage words, block-major, 8 words a
		/// block, one BIT per byte of the block's 256. Indexes the words vector built beside it.
		u32 first_word;
	};

	/// The bytes a host->local transfer of `r` under `layout` writes, per guest page of `pages`.
	///
	/// Returns FALSE when the write is not byte-exact, in which case no merge may be planned off
	/// it at all: a byte the transfer owns only HALF of (PSMT4HL and PSMT4HH always, PSMT4 where the
	/// rect's nibbles do not pair up) cannot be split by a byte mask, so neither answer is right --
	/// writing it loses the CPU's nibble and skipping it loses the target's.
	///
	/// Static and model-free: the address arithmetic is the load-bearing part and it is pinned
	/// against GSLocalMemory's own WritePixel family with no device and no renderer.
	static bool BuildUploadByteMask(const GSTileSurfaceLayout& layout, const GSVector4i& r,
		const GSPageBitmap& pages, std::vector<UploadPageBytes>& out, std::vector<u32>& words);

private:
	/// One owner's share of a merge: every page whose planes it alone holds truth of.
	struct UploadMergeGroup
	{
		GSTileSurfaceId owner;
		GSPageBitmap pages;
	};
	/// Pages a CPU READ has already had to pull off the GPU (a local read or a CLUT load), from the
	/// start of the session. The merge refuses them, and that refusal is what keeps it from trading
	/// one wait for several.
	///
	/// The road it replaces did the CPU a favour on the side: draining a page for an upload spill
	/// also handed truth back, so every later CPU read of that page was free. Keeping truth on the
	/// GPU takes that away, and on a title whose palettes live in the tail blocks of a framebuffer
	/// page -- Dirge of Cerberus does exactly this -- the CLUT loads then pay for it over and over.
	/// Measured, before this bitmap existed: dirge's upload stalls fell 4.62 -> 0.88 a frame and its
	/// CLUT stalls rose 2.00 -> 10.25, a net LOSS of 4.5 waits a frame.
	///
	/// So the merge does not guess which pages the CPU wants: it waits to be told. The first read
	/// that has to pull a page marks it, and from then on uploads to it take the blocking road and
	/// leave it where the CPU can reach it. Monotonic, so it cannot oscillate, and every entry in it
	/// was paid for by a wait that already happened.
	GSPageBitmap m_cpu_read_pages;
	std::vector<UploadMergeGroup> m_merge_groups;
	std::vector<UploadPageBytes> m_merge_bytes; ///< one entry per page of this merge
	std::vector<u32> m_merge_words;             ///< their keep tables, before the plan rebases them
	u32 m_merge_keep_base = 0;                  ///< where m_merge_words landed in the plan's array
	/// EmitPrepOp applies the keep tables ONLY inside the merge's own compose. A later reader of the
	/// same page composes it again from a model that already accounts for the transfer, and giving
	/// that writeback the keep mask would leave the CPU's bytes standing over the target's own.
	bool m_merge_emitting = false;

	// Ensure a surface for `layout` covering `pages` exists in the model and the pool (grown as
	// needed). Returns kGSTileNoSurface on allocation failure.
	//
	// The returned surface is not always the view's own. Under TileGpuContainSurfaces a view whose
	// pages sit a whole number of pages inside a live container renders into the CONTAINER, at the
	// rectangle offset written to `out_offset` -- so the caller has to put its draw there: every
	// coordinate that speaks in the surface's pixel space (the XYOFFSET the vertex transform
	// subtracts, the hardware scissor, the draw rect) is translated by it. (0, 0) is a view that is
	// its own surface, which is every view while the lever is off.
	//
	// ⚠️ While kGSTileContainDisplacedViews is false NO view is displaced, so `out_offset` is always
	// (0, 0) and the fold is always the cross-PSM same-base merge. `may_offset` is the per-view half
	// of the same refusal and is what the displaced road is gated on when it comes back; it decides
	// nothing today, and it is kept live rather than deleted because deleting it would take the
	// reasons with it.
	//
	// `may_offset` false says this caller's view may not be DISPLACED inside a container, whatever
	// the predicate thinks, and says it permanently. Two callers say it, for the same underlying
	// reason -- something outside this function reads the view's pixels at coordinates it works out
	// for itself:
	//   * a draw carrying a depth attachment. One vertex transform serves both attachments, so
	//     displacing the colour view displaces the depth rows with it, into guest Z that belongs to
	//     other pixels. Fixing that means containing the depth view by the same delta, which is
	//     deferred (design 0.5).
	//   * the display base (design rule 9). GetOutput hands the frontend a texture and a y_offset,
	//     with no column offset beside it, and MaterialiseDisplayBuffers' seed draw carries the
	//     display rect untranslated.
	// A ZERO offset displaces nothing, so both callers keep the cross-PSM same-base merge -- which
	// is the half of containment gt4opb's win mostly rests on. A view already folded at a nonzero
	// offset when the refusal first arrives is unfolded, which the seed road repairs like any other
	// change of page owner.
	GSTileSurfaceId EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect,
		const GSPageBitmap& pages, GSVector2i* out_offset = nullptr, bool may_offset = false);

	// Pages of `pages` whose bytes surface `id`'s texture does not hold newest for every plane
	// in `planes` (Tile's PagesNeedingUpload).
	GSPageBitmap PagesNeedingSeed(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const;

	// Rule 2 of the VRAM model's texel road: the ONE resident target that can serve this texture
	// read whole, or kGSTileNoSurface for the byte road. Everything it asks is a proof, not a
	// guess -- the page model names a sole owner of every page of the window, the owner's pixel
	// space is the read's own layout (same base, same stride, byte-compatible format,
	// page-aligned), its texture actually holds texels on those pages, and the window fits inside
	// it. Anything short of that is the byte road, which is always correct and never asks.
	//
	// The draw's own targets are excluded here rather than checked later: a draw sampling what its
	// pass renders into is the in-pass feedback road, and this stage does not have one.
	//
	// Not const only because one refusal is counted where it happens: the base clause, which
	// surface-identity containment is the one thing that can break while every other clause holds.
	GSTileSurfaceId TargetForTextureRead(const GSTileSurfaceLayout& tex_l, u32 tw, u32 th,
		const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id, GSTileSurfaceId z_id);

	// Whether the device can bind a resident target as a draw's texture. Read once at
	// construction, because a rule-2 draw is served by work the renderer DOES NOT DO (its read
	// window is never composed into the ring) -- asking late, or asking a device that then
	// refuses, would leave the fragment stage decoding bytes nobody wrote.
	bool m_bindless_targets = false;
	/// The device serves a pass that reads its own colour attachment in rasterization order. Read once
	/// at construction: it decides the target pool's image usage, so it cannot be discovered later.
	bool m_self_read = false;
	/// The device's fragment module declares a second colour output at index 1, so the As blend
	/// factor rides there. Read once at construction: the module was compiled at device creation, and
	/// a draw planned under one answer and executed under the other either loses its blend factor or
	/// names a Vulkan factor the pipeline cannot have.
	bool m_dual_source = true;
	/// Test scaffolding (EmuCore/GS TileGpuForceSelfRead, gsrunner -tilermw): admit every draw the
	/// classifier can name, not only the classes whose repairs have landed. Read once, like the other
	/// levers here, because it moves pass boundaries.
	///
	/// It also reaches past the device's admission narrowing, because the instrument exists to
	/// exercise the road and a device that refuses a class would otherwise leave it unexercised. A
	/// forced run on a device that taxes declaring is a measurement, not a shipping shape.
	bool m_force_self_read = false;
	/// EmuCore/GS/TileGpuShaderWriteMask, ANDed with the device having the read road at all: a draw's
	/// colour write mask leaves the pipeline and the fragment stage serves it, so two draws differing
	/// only in FBMSK share a pipeline and merge into one indirect call. Read once at construction like
	/// the levers above -- it decides which pass a draw keys into, and a lever that moved mid-frame
	/// would split a pass on a question nothing asked.
	///
	/// ⚠️ Deliberately OUTSIDE the per-class declaring budget, which is the one judgement in this lever
	/// and rests on gsTileGpuMasksInShader's invariant: it moves only draws that ALREADY declare, so
	/// there is no new tax for the budget to price. That is also why it could not be priced there if it
	/// were asked -- the budget weighs a class's declaring tax against a fixed line because what those
	/// classes buy is exactness, which it cannot measure, while this buys SPEED, whose size scales with
	/// the same draw count the tax does. A line fitted to the first question refuses exactly the
	/// population the second pays best on: Spider-Man 3's masked draws are ~3,400 in the budget's unit
	/// against a line of 256, on the title the lever exists for.
	bool m_shader_write_mask = false;

	// Whether this device would rather have MORE passes than mixed depth state inside one
	// (GSDevice::TileGpuPrefersDepthUniformPasses). Read once at construction, for the same reason
	// the pass sim's lever is: it decides pass boundaries, and boundaries that moved mid-frame would
	// put a draw's prep ops in a pass the draw is not in. Both grouping sites read THIS member --
	// accumulation's open-pass test and the plan build's cut -- so they cannot disagree.
	bool m_depth_uniform_passes = false;
	// The per-frame depth-pass predictor (EmuCore/GS/TileGpuAdaptiveDepthPasses), in two rights.
	//
	// The CENSUS counts both groupings of every frame and runs the picker over them. It moves no
	// boundary and no pixel, so the key alone turns it on, anywhere -- which is what makes the
	// thresholds checkable against a corpus on a machine that is not the one they were calibrated on.
	bool m_depth_predictor_census = false;
	// DECIDING additionally writes the picker's answer into m_depth_uniform_passes, and only where
	// the device asked for uniform with no force key set. With the key off -- and on every device
	// that already wants merged -- nothing here runs and the frame is byte-identical to the shipped
	// one.
	//
	// The write happens at the FRAME BOUNDARY, after the frame's plan has been built and executed and
	// before the next frame's first draw is accumulated. That is the one instant at which moving the
	// polarity is safe: the constraint the member above states is that both grouping sites see one
	// value for one frame's draws, not that the value never changes.
	bool m_depth_predictor = false;
	GSTileGpuDepthPolicyPicker m_depth_picker;
	// The most draws one pass may hold, 0 meaning no cap (GSDevice::TileGpuMaxPassDraws, overridable
	// by EmuCore/GS/TileGpuMaxPassDraws). Read once at construction and held in a member for exactly
	// the reason the bit above is: both grouping sites read THIS, so a pass length they disagree
	// about is not expressible.
	//
	// Independent of every other boundary policy -- it is a ceiling, not a choice of arrangement, so
	// it composes with the depth polarity and the reader segregation rather than replacing either.
	// That composition is the point: the deciding device round is a four-arm crossing of the depth
	// policy against this cap.
	u32 m_max_pass_draws = 0;
	// The most extra pipeline binds one plan may pay for freezing per-draw GS state into fragment
	// programs, 0 meaning no guard (GSDevice::TileGpuMaxSpecializationBinds, overridable by
	// EmuCore/GS/TileGpuMaxSpecializationBinds). Read once at construction like every other policy
	// this expensive, so an archived log says which arm ran.
	//
	// Unlike the three above this is NOT a pass boundary -- it moves no draw between passes and only
	// one grouping site reads it -- but it is held the same way because it has the same failure shape:
	// a large, invisible effect on frame time with no symptom in the frame.
	u32 m_max_spec_binds = 0;
	// Whether declaring the in-pass destination read taxes every draw of the pass
	// (GSDevice::TileGpuSegregatesSelfRead). Read once at construction and used through the same
	// key function for the same reason: it is a pass boundary, and both grouping sites must agree.
	//
	// ONE member for both of that bit's consequences -- a declaring pass holds only its readers, and
	// the admission classes are put on a per-frame density budget rather than admitted outright.
	// They come from one silicon fact, so a second member would be a second chance to disagree with
	// it. The pass-key half reads it through gsTileGpuPassKeyFor; the admission half through
	// m_declaring_budget, which is inert unless this is true.
	bool m_segregate_self_read = false;

	// --- draw reordering (EmuCore/GS/TileGpuReorderRuns) ---------------------------------------
	//
	// A RUN is a group of draws sharing one pass key, staged so they can be emitted contiguously.
	// The planner may hold several open at once and admits a draw to one only where the page model
	// proves it touches nothing any OTHER open run touches -- so every pair of open runs is
	// page-independent, which is what makes emitting them one after another legal.
	//
	// Read once at construction like every other boundary policy: a value that moved mid-run would
	// leave one frame's draws scheduled two ways.
	int m_reorder_setting = 0;   ///< the raw lever, for the log line and the report
	bool m_reorder_census = false; ///< run the model and report it, moving nothing (setting < 0)
	bool m_reorder_active = false; ///< stage and splice for real (setting > 0)
	u32 m_reorder_max_runs = 0;  ///< |setting|, clamped to the scheduler's ceiling; 0 when off

	/// The staging and the emission order, shared by the count-only model and the live road -- they
	/// never both run, and one implementation is what keeps the census honest about the other.
	GSTileGpuReorderScheduler m_reorder;

	/// Run the admission model over the plan as it stands and record what it would have moved.
	/// Decides nothing: the caller's plan is untouched, and only the ModelFrame counters change.
	void ReorderCensus();

	/// Stage draw `index` -- the plan entry just pushed -- in the run AccumulateDraw put it in. A
	/// no-op unless the lever is positive. Every site that appends to m_plan_pending must call it, or
	/// the emission order comes out short and the splice drops a draw.
	void StageDraw(u32 index, const GSPageBitmap& written, const GSPageBitmap& read);

	/// The pages every device-held palette was loaded from. A draw whose palette the device holds
	/// reads those pages through a prep op of its own, and which record it will name is not known
	/// until its texel road is resolved -- which is after the run has to be chosen. So the reorder
	/// takes the union: a superset can only refuse a move, never permit a wrong one. Empty on every
	/// title that never gathers a CLUT, which is most of them.
	GSPageBitmap LiveClutPages() const;

	/// Emit the staged runs and put the plan's per-draw arrays in that order. A no-op unless the
	/// lever is positive; with one run the permutation is the identity and the gather is a copy.
	void SpliceRuns();

	/// Scratch for the splice, held so a frame's gather allocates nothing. Filled and swapped in.
	std::vector<GSDevice::GSTileGpuIndirectDraw> m_splice_draws;
	std::vector<GSDevice::GSTileGpuTopology> m_splice_topologies;
	std::vector<u32> m_splice_blend_keys;
	std::vector<GSDevice::GSTileGpuDepthMode> m_splice_depth_modes;
	std::vector<PendingDraw> m_splice_pending;
	std::vector<GSPageBitmap> m_splice_write_pages;
	std::vector<GSPageBitmap> m_splice_read_pages;
	std::vector<GSDevice::GSTileGpuPrepOp> m_splice_prep_ops;

	// The per-class density budget (GSTileGpuDeclaringBudget). Rolled once per video frame, beside
	// the model frame it is measured from, so the verdict is constant for every draw of a frame --
	// including across a mid-frame plan build, because a verdict that moved mid-frame would key two
	// draws of one class into different passes.
	GSTileGpuDeclaringBudget m_declaring_budget;
	// Run tracking for the budget's cost measure. A class's run is broken by the attachment group
	// changing or by a draw that does not want the class, and both are answered by carrying the
	// previous draw's group and wanted set.
	GSTileGpuPassKey m_budget_group{};
	u32 m_budget_prev_wanted = 0;

	// --- rule 3, the source cache: BUILT and SAMPLED ------------------------------------------
	// The three library caches the materialised-source road is built out of. A direct-colour window
	// the probe admits is MATERIALISED -- an ordinary RGBA8 image, built on the device out of the
	// same ring bytes the flat road reads, cached and invalidated by the same predicate -- and the
	// draw that admitted it then SAMPLES it: one hardware fetch in place of the swizzle arithmetic,
	// and under LINEAR one fetch in place of four. Every refusal (region wrap, pin, capacity, a
	// palette pair not yet admitted, a failed build) falls back to the byte road, which is always
	// correct and never asks. A paletted window takes the same road in two stages: the index image out
	// of the ring bytes, and the palette expansion on top of it, admitted on second sight across a
	// frame boundary so a window drawn once never pays for an expansion.
	GSTileTextureSource m_tex_source;
	GSTilePaletteCache m_palette_cache;
	GSTileExpandedCache m_expand_cache;

	// The pin discipline's clock (GSTileTextureSource::SetFrame). It counts PLANS, not video
	// frames: a cache entry a recorded draw names is held down until the plan carrying that draw
	// has been handed to the executor, which is BuildAndExecutePlan -- at VSync usually, but also
	// at any mid-frame flush. Starts at 1 because zero means "discipline off", which is what the
	// Tile renderer leaves these caches at.
	u64 m_source_frame = 1;

	// Move the pin clock on: every entry the plan just executed named is released. Called from
	// exactly one place, the end of BuildAndExecutePlan, and from construction to arm the
	// discipline in the first place.
	void AdvanceSourcePinFrame();

	// Rule 3's DONOR build road: the window's texels read straight out of the ONE live target that
	// owns its pages, through the GS swizzle, instead of out of the frame's byte ring. It is what
	// removes the round trip the byte road pays for exactly these windows -- a writeback compute
	// pass reswizzling the target into ring slots, then a materialise unswizzling them back -- for
	// bytes that never leave the GPU. It also gives them an IDENTITY: a window over a rendered
	// target has no honest page fingerprint (the CPU shadow does not hold those bytes), so every
	// moved gen stamp read as a rebuild and every identity derived from it churned with it.
	//
	// Serves PALETTED windows (PSMT8/PSMT4) over a page-aligned CT32/CT24 colour target, which is
	// the class rule 2 refuses by construction -- rule 2 binds a target only where the read's own
	// layout IS the target's, and a palettised read of a colour surface never is. A direct-colour
	// window at the target's own base and stride is rule 2's already; one at an offset or a
	// different stride would need a copy leg this road does not have, and is counted rather than
	// served (src_donor_direct).
	struct DonorPlan
	{
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 src_bp = 0;     ///< the window's TBP0 (blocks)
		u32 src_bwpg = 0;   ///< the window's width in pages, as the byte road computes it
		u32 owner_bp = 0;   ///< the owner's base (page-aligned blocks)
		u32 owner_bwpg = 0; ///< the owner's width in pages
		u32 psm = 0;        ///< the window's TEX0.PSM, which picks the index format
	};

	// Can one live target serve this window whole, as a donor? Every clause is a proof, in the
	// shape rule 2's are: one owner for EVERY plane of every page at whole-page granularity (the
	// reinterpretation reads any byte of the owner's cell, alpha included), a page-aligned CT32/CT24
	// colour layout so the owner's pixel space is the guest layout, its texture actually holding
	// texels on those pages, the window starting at or after the owner's base, and a format the
	// swizzle forms express. Anything short of that keeps the byte road, which is always correct.
	// The draw's own targets are excluded here rather than checked later: sampling what the pass
	// renders into is the feedback road, and this stage does not have one.
	bool DonorForTextureRead(const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id,
		GSTileSurfaceId z_id, DonorPlan& out);

	// "The owner's texels changed", as a monotonic counter, and the whole of the donor road's
	// content identity (GSTileTextureSource::TargetToken). It is the renderer's own because the page
	// model tracks BYTES: a page's write generation moves when anything claims the page, including a
	// depth draw aliasing a colour target's pages, which changes no colour texel at all. The counter
	// is bumped by the three things that write a pool texture -- a draw rendering into it, a seed
	// filling it, and the pool allocating or growing it -- and by nothing else, so it is exactly as
	// strict as it must be and no stricter. Globally monotonic, never per-surface: a surface id is a
	// recycled slot, and a per-surface counter would let a new surface reproduce its predecessor's
	// version.
	//
	// ⚠️ Being over-eager here can only cost a rebuild, never a wrong serve: the cache consults the
	// token ONLY when the gen stamp has already moved, so a version that bumps for nothing loses a
	// rescue and cannot gain a stale one.
	std::vector<u64> m_surface_version;
	u64 m_surface_version_counter = 0;
	void BumpSurfaceVersion(GSTileSurfaceId id);
	u64 SurfaceVersion(GSTileSurfaceId id) const;

	// The emission half of rule 3's build. The cache owns what to build, the identity it is
	// filed under and when it dies; this owns how the texels get there -- a materialise pass
	// queued into the frame's prep-op stream, executed at the head of the emitting draw's pass.
	// The split is the point: the cache is library code that knows nothing about deferred
	// execution, and the renderer is where deferred execution lives.
	struct SourceMaterialiser final : public GSTileSourceBuilder
	{
		GSRendererTileGpu* r = nullptr;
		u32 epoch = 0; ///< the page-table epoch the emitting draw reads through
		const DonorPlan* donor = nullptr; ///< non-null: build off the owner target, not off the ring
		bool BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA) override;
	};

	// Queue the pass that fills `tex` with the window's texels read out of the donor's owner target.
	// Unlike a materialise this reads an IMAGE, so it is only safe at the head of a pass none of
	// whose already-recorded draws has written that image -- the caller runs DonorHoistCollides and
	// breaks the pass when it has, exactly as a writeback does.
	bool EmitDonorOp(GSTexture* tex, const GIFRegTEX0& tex0, const DonorPlan& donor);

	// Would a donor build queued for the draw being accumulated read pixels a draw already in the
	// open pass has written? Then it cannot run at that pass's head and its draw opens its own.
	// The writeback's own test, narrowed to the one hazard a donor has (it rewrites no ring slot,
	// so the read half of WritebackHoistCollides has nothing to say about it).
	bool DonorHoistCollides(GSTileSurfaceId owner, const GSPageBitmap& pages) const;

	// Queue the pass that fills `tex` with the window at tex0/texa, reading the ring at `epoch`.
	// Emitted AFTER the window's composition ops so array order puts it behind them at the pass
	// head; safe to hoist there for the same reason a seed is, since the ring is staged whole-frame
	// and the composition it depends on is in the same op range ahead of it.
	bool EmitMaterialiseOp(GSTexture* tex, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 epoch);

	// The same split for the paletted road's second stage. GSTileExpandedCache owns admission,
	// identity and lifetime; this owns the emission -- which for a renderer that records draws has to
	// be an op in the stream rather than a pass issued now, and an op that lands AFTER the materialise
	// whose index image it reads.
	struct SourceExpander final : public GSTileExpandBuilder
	{
		GSRendererTileGpu* r = nullptr;
		bool BuildTileExpansion(GSTexture* index, GSTexture* palette, GSTexture* dst) override;
	};

	// Queue the pass that fills `dst` with palette[index] per texel. Emitted from inside the expanded
	// cache's build, which the road below runs immediately after the index build, so array order puts
	// it behind the materialise it depends on.
	bool EmitExpandOp(GSTexture* dst, GSTexture* index, GSTexture* palette);

	// This frame's slot in the plan's prep-texture list for a texture a prep op names (a source image,
	// an index image, a palette). Deduped: one palette serves many windows and one index image serves
	// many palettes, and a prep op names each of them by index.
	u32 PrepTextureIndex(GSTexture* tex);

	// Build (or find) the materialised source for a window the probe admitted, and on success put
	// this draw ON it: `pd` takes a bind slot and road SOURCE, and its fragment stage samples the
	// image instead of decoding ring bytes. A direct-colour window is one image and one pass. A
	// PALETTED one is two of each -- the window materialises to an index image, and that image times
	// this draw's palette expands into the RGBA8 the draw actually samples -- because the source
	// cache's key deliberately excludes the palette, so one index build serves every palette a game
	// cycles through it. `window` indexes m_probe_windows. Returns false on every refusal, and a
	// refusal is not an error: the caller has already put the draw on the byte road and that road is
	// correct on its own.
	//
	// `donor`, when given, builds the source off the owner target named there instead of off the
	// ring, and files it under that target's identity rather than under a page fingerprint. The
	// caller takes that road BEFORE composing the window -- not composing it is the point.
	bool MaterialiseSourceRoad(PendingDraw& pd, u32 window, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
		u32 epoch, const DonorPlan* donor = nullptr);

	// This frame's slot for the (image, sampler) pair, appending it to m_plan_sources on first sight.
	// kNoSourceSlot once the frame's table is full -- a hard bound, not a hope, and counted.
	u32 SourceSlotFor(GSTexture* tex, u8 sampler);

	// The sampler a draw's TEX1/CLAMP asks for, as a GSHWDrawConfig::SamplerSelector key: bit 0 =
	// REPEAT on U, bit 1 = REPEAT on V, bit 2 = bilinear. Only the eight combinations rule 3 admits
	// exist -- the REGION modes refused before they got here.
	static u8 SourceSamplerKey(u32 wms, u32 wmt, bool ltf);

	static constexpr u32 kNoSourceSlot = GSDevice::GSTileGpuPassPlan::kNoSourceSlot;
	// The descriptor array rule 3 binds through (design §6), and a contract constant rather than a
	// tunable: the shader declares an array of exactly this size. A frame needing more distinct
	// sources refuses the overflow to the byte road -- counted, and 128 rather than 64 because the
	// corpus census found GT4 and OutRun pinned at 64 with draws refused behind them.
	static constexpr u32 kMaxSourceSlots = GSDevice::GSTileGpuPassPlan::kMaxSources;

	// One distinct texture window the frame probed, in first-appearance order.
	struct ProbedWindow
	{
		GSTileTextureSource::WindowKey key;
		u64 stamp = 0;     ///< GSTileTextureSource::GenStamp of the window's pages at first sight
		/// What this window's CONTENT is, and where that answer came from. Two different windows'
		/// truth lives in two different places and the numbering spaces must not meet: a window the
		/// CPU shadow holds gets a fingerprint of its page bytes, and one whose truth is a rendered
		/// target gets the owning target's id and version (the donor road's identity), which is why
		/// this is a tagged pair and not a bare number. Unknown -- the tier declined and no donor
		/// serves it -- is never equal to anything, including another unknown.
		GSTileTextureSource::ContentToken content;
		u64 build_id = 0;  ///< content identity: carried over while the CONTENT holds, stamp or no stamp
		bool admitted = false;     ///< a draw of THIS frame has been admitted on this window. It is the
		                           ///< pin's marker (a rebuild under it would land in the same texture an
		                           ///< already-recorded draw names) and the frame's distinct-source census.
		bool prev_seen = false;    ///< the frame before also probed this window
		bool prev_current = false; ///< ...and its bytes are the same ones, by stamp or by fingerprint
	};
	// This frame's windows and last frame's. Two frames is the whole history the counters need:
	// the stamp question is "did this window's content hold still since the frame before", and
	// the build identity a palette pair keys on only has to survive the frame boundary the
	// expanded cache admits across. A real cache holds 512 entries and would hit on windows
	// older than this, so the hit column here is a LOWER bound and the rebuild column an upper
	// one.
	std::vector<ProbedWindow> m_probe_windows;
	std::vector<ProbedWindow> m_probe_prev;
	u64 m_probe_build_counter = 0;

	// --- the CLUT gather: a palette loaded off a render target -------------------------------
	//
	// The GS loads its palette out of local memory when TEX0 is written, and when the words it
	// loads were rendered by a native draw they are in a target's texture and NOWHERE on the CPU.
	// Today that load is a full drain -- flush the plan, read the pages back, load from the shadow
	// -- and it is the dominant stall on the two GT4 dumps: 671 and 1272 a frame, which is also,
	// numerically, their whole mid-frame plan fragmentation. This road deletes it: the load is
	// classified where it happens, an eligible one takes no readback at all, and its words reach the
	// consumers ON THE DEVICE -- as a copy of the palette's blocks into the frame's palette stream
	// for a byte-road draw, and as an N x 1 gathered texture for a rule-3 one.
	//
	// The shape is exact-Tile's (GSRendererTile.cpp's PreClutLoad/PostClutLoad, GpuPalette,
	// GSTileClutMirror, PruneGpuPalettes, SyncClutToCpu) with ONE structural difference, which is
	// where all the design pressure was: this renderer's consumers are DEFERRED. A draw is recorded
	// now and issued at the plan, so a palette a recorded draw names must outlive the load that
	// wrote it and the load that displaced it -- which is why pruning happens at the plan tail
	// rather than at the load (design record: umbrella campaigns/p3-c6-gather-design-2026-08-20).

	/// Which mirror slots a CSM1 32-bit load writes: an eight-bit palette fills slots CSA..15
	/// (WriteCLUT_T32_I8_CSM1's loop), a four-bit one fills the single slot CSA.
	static void ClutLoadSlots(const GIFRegTEX0& TEX0, u32& first, u32& count);
	/// The load shapes the device gather serves: CSM1, a 32-bit palette, and either a four-bit index
	/// (one slot) or an eight-bit one at CSA 0 (all sixteen). A 32-bit entry is RGBA verbatim under
	/// Read32, so within this shape there is no TEXA and no CPSM decode to disagree about.
	static bool ClutGatherServesLoad(const GIFRegTEX0& TEX0);

	/// One device palette: a CLUT load the gather served. Fresh per load and never looked up by
	/// content -- the probe measured every eligible load's tuple as new on both axes on both prize
	/// titles (gt4 631/631, gt4opb 1259/1259), so a cache cannot hit and is not built.
	struct GpuPalette
	{
		u32 handle = 0; ///< the mirror's name for it; never GSTileClutMirror::kCpu
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 cbp = 0;
		u32 entries = 0; ///< 16 or 256
		u32 first_slot = 0; ///< the first mirror slot the load wrote
		u32 owner_bp = 0, owner_bwpg = 0;
		GSPageBitmap pages; ///< the load's source pages
		u64 pal_id = 0; ///< namespace-tagged content identity, for the expanded cache
		GIFRegTEX0 tex0 = {}; ///< the load's registers, for the CPU sync seam
		GIFRegTEXCLUT texclut = {};
		/// The owner's texels on these pages have been overwritten since the load, so the words are
		/// gone from the device too. Set precisely, from the two things that write a pool texture on
		/// those pages (a draw and a seed) plus a surface slot being recycled.
		bool source_lost = false;
		u64 stream_plan = 0; ///< the plan its words were copied into the palette stream for (0 = none)
		u32 stream_offset = 0; ///< ...and the word offset they landed at
		GSTexture* gather_tex = nullptr; ///< rule 3's N x 1 palette, gathered on demand
	};

	GSTileClutMirror m_clut_mirror;
	std::vector<GpuPalette> m_gpu_palettes; ///< sorted by handle: handles only ever increase
	std::vector<u32> m_clut_live; ///< indices of the records the mirror still names (at most sixteen)
	u32 m_gpu_palette_next = 1;
	/// Off for the session once the device refuses the road (the CLUT loaders' word order no longer
	/// fits a closed form, or a gather pipeline failed to build). Probed once, lazily.
	bool m_clut_gather_serves = true;
	bool m_clut_gather_probed = false;
	bool m_warned_clut_lost = false;
	/// The swizzle forms, fitted here as well as in the device. Both fits run over the same tables
	/// with the same code and cannot disagree; what the renderer needs them for is the block copy's
	/// SOURCE RECTS, which are a CPU-side answer the shader never computes.
	GSTileSwizzleForms::FormSet m_clut_forms;
	/// GSClut::EntryToWordCSM1_32's tables, taken once: entry -> the source word the loader read it
	/// from. Only the CPU sync seam needs them (the shaders take the fitted forms instead).
	bool m_clut_word_map_taken = false;
	std::array<u16, 256> m_clut_word_i8{};
	std::array<u16, 16> m_clut_word_i4{};

	/// What the InvalidateLocalMem(clut) calls of ONE load saw, carried to PostClutLoad where TEX0
	/// finally names the load's shape and the slots it wrote.
	struct ClutPendingLoad
	{
		u32 calls = 0;
		u32 deferred_blocks = 0; ///< blocks whose readback was skipped because the gather may serve them
		bool stalling = false; ///< some block's pages are held newest by a target
		bool readback = false; ///< ...and a readback actually happened for one of them
		bool refused = false; ///< ...and some block's owner conditions did not hold
		u32 refusal = 0; ///< which clause (ClutRefusal), for the counters
		GSPageBitmap pages; ///< the deferred blocks' pages
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 owner_bp = 0, owner_bwpg = 0;
	};
	ClutPendingLoad m_clut_pending;

	enum ClutRefusal : u32
	{
		kClutRefNone = 0,
		kClutRefMulti, ///< no single live surface owns every plane of every page (multi-owner or partial)
		kClutRefDead, ///< the owner is not alive, or has no pool texture
		kClutRefLayout, ///< not a page-aligned CT32/CT24 colour surface, or CBP below its base
		kClutRefResidency, ///< its texture does not span the pages
		kClutRefTexels, ///< ...spans them but has never written texels there
		kClutRefMixedOwner, ///< two of the load's blocks are owned by different surfaces
		kClutRefCount
	};

	void PreClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT) override;
	void PostClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT) override;

	/// One InvalidateLocalMem(clut) call's block, asked the donor road's own owner questions. True
	/// when its readback is DEFERRED to PostClutLoad; false leaves the caller on today's road, which
	/// is always correct.
	bool ClutLoadDefer(const GSPageBitmap& pages);
	/// Whether the device can serve the gather at all (the CLUT loaders' word order fits). Asked once.
	bool ClutGatherServes();

	GpuPalette* FindGpuPalette(u32 handle);
	/// Rebuild the short list of records the mirror still names, after anything that changes it.
	void RefreshClutLive();
	/// A pool texture's pages were just written (a draw into the surface, or a seed): every record
	/// whose source those pages carry has lost its words. Cheap by construction -- the list it walks
	/// is at most sixteen long and empty on a title with no gathered palette at all.
	void NoteClutSourceWritten(GSTileSurfaceId id, const GSPageBitmap& pages);
	/// The surface id has been recycled into a new surface: every palette on it has lost its words.
	void NoteClutSurfaceReplaced(GSTileSurfaceId id);

	/// Which device palette this draw's slots resolve to, or nullptr for the CPU road. Handles the
	/// two roads the mirror cannot serve -- slots from different loads, and a 256-entry palette read
	/// at a non-zero CSA -- by making the CPU words whole first, which is always correct.
	/// ⚠️ May FLUSH the pending plan (the sync reads pages back), so it must run before the caller
	/// has emitted anything into its prep-op range.
	GpuPalette* ResolveDrawPalette(const GIFRegTEX0& tex0, u32 pal_entries, u32& first_texel, bool& synced);

	/// The record's words in the frame's palette stream: a reserved run of `entries` zero words and
	/// one image-to-buffer copy of the palette's blocks out of the owner. Idempotent within a plan
	/// and emitted at the current op position, which is what makes it usable both from a consumer
	/// and from the write that is about to destroy the source.
	bool CaptureClutRecordWords(GpuPalette& gp);
	/// The same, for a draw that is about to READ those words: adds the donor hoist test, so a copy
	/// that cannot run at the open pass's head opens a new one. False when the source is gone.
	bool EnsureClutStreamWords(GpuPalette& gp, PendingDraw& pd);
	/// The record's N x 1 palette texture for rule 3, gathered off the owner by a ClutGather op in
	/// the calling draw's prep range. Null when the source is gone or the allocation fails.
	GSTexture* ClutGatherTexture(GpuPalette& gp, PendingDraw& pd);
	/// A prep op that cannot be hoisted over the open pass opens a new one -- the donor's own test,
	/// asked for a CLUT op's owner.
	void ClutHoistBreak(const GpuPalette& gp, PendingDraw& pd);

	/// Every device palette's words back into the CPU's CLUT RAM, and its slots back to the CPU.
	/// The whole-of-truth seam (savestate, renderer switch, purge) and the two draw-side roads the
	/// mirror cannot serve. ⚠️ Reads pages back, so it flushes the pending plan.
	void SyncClutToCpu();
	void SyncClutRecordToCpu(GpuPalette& gp);
	/// A record's words are gone and nothing can recover them: count it, say so once, and hand its
	/// slots back to the CPU so the stale words are used at most once per load rather than forever.
	void RetireLostClutRecord(GpuPalette& gp);

	/// Records the mirror no longer names are recycled -- at the tail of the plan and nowhere else.
	/// Tile prunes at the load, and that is not sound here: a recorded-but-unissued draw may still
	/// sample a palette whose slots a later load has already displaced. At the plan tail every
	/// recorded op has issued and there are no pending draws, so Tile's own predicate is correct.
	void PruneGpuPalettes();
	void ReleaseGpuPalettes();

	/// The two halves of GSTilePaletteCache's id space, named here because both roads of this
	/// renderer's palette question use them: a palette built from CPU words takes OwnPalId, and one
	/// the gather loaded off a target -- which has no words to hash -- takes ForeignPalId over the
	/// load's tuple. The reserved bit is what keeps the expanded cache from fusing a pair across them.
	static u64 ClutCpuPalId(u64 content_id) { return GSTilePaletteCache::OwnPalId(content_id); }

	// Rule 3's admission question. Computes the window's key and content stamp, asks the palette
	// pair whether it would be admitted, and either admits the draw or counts one named refusal.
	// Called only where rule 2 declined, and it writes nothing but counters and the window record --
	// the BUILD is a separate act, after the window's bytes have been composed, and it is the build
	// that puts the draw on the road. Returns the admitted window's index in m_probe_windows, or
	// kNoSourceSlot.
	//
	// `donor_token`, when Known, is the window's content identity taken from the target that owns
	// its pages -- and it replaces the page-fingerprint tier outright rather than joining it, both
	// because the fingerprint cannot answer for GPU-side bytes (it declines and returns nothing) and
	// because it costs an 8 KB hash per moved page to decline.
	u32 ProbeSourceRoad(const PendingDraw& pd, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages, bool paletted,
		bool mip_active, const GSTileTextureSource::ContentToken& donor_token);

	// Which pages of a surface's texture actually hold texels. The memory model tracks BYTES, and
	// by it a page the surface owns no truth for is in perfect order -- its bytes are in the CPU
	// shadow, and everything reading through the byte road gets them. Nothing reads the texture
	// itself that way: the present hands the pool texture straight to the display, so a page of
	// the target that no draw covered and no seed filled reaches the screen as whatever the
	// allocator left there (on this device, magenta). The texture is a rectangle, so this is not a
	// rare corner: a target is allocated tall enough for the pages drawn so far and the rest of
	// its rows are simply never written.
	//
	// Blocks enter `filled` when a seed writes their page or a draw the planner has proved total
	// covers them; `pages` is the whole page set the texture spans, recomputed when the pool grows
	// it. The first draw into a surface seeds the difference, so the cost is one full-target seed
	// per surface lifetime. Block granularity, not page granularity, because the question it is
	// asked against -- the model's TruthMask -- is per block: see GSTileBlockFill.
	struct SurfaceTexels
	{
		GSPageBitmap pages; // every page the texture rectangle spans
		GSTileBlockFill filled;
		int height = 0; // the texture height `pages` was computed for
	};
	std::vector<SurfaceTexels> m_surface_texels;
	SurfaceTexels& Texels(GSTileSurfaceId id);
	void NoteTextureGeometry(GSTileSurfaceId id, int height);

	// -- surface-identity containment ----------------------------------------------------------
	//
	// Where a view of GS memory goes: into a surface of its own, or into a rectangle of a container
	// that already holds another view. The placement is decided here for every colour view the
	// renderer looks up, and TileGpuContainSurfaces decides whether EnsureSurface acts on it or
	// merely counts it. Off the lever every view is its own surface, every offset is (0, 0), and
	// the frame is byte-identical -- the census arm this grew out of.
	//
	// It is asked BEFORE the exact-match road creates anything, because a fold is precisely that
	// create not happening: under containment the view never has a surface, so nothing may look one
	// up for it later. The decision is taken at FIRST SIGHT and is sticky, held in m_contain_alias
	// across frames. There is no retro-folding and no re-folding: moving a view between surfaces
	// would move page ownership and re-seed the pages, every frame, as the draw order shifted.
	//
	// `first_sight` is EnsureSurface's own road -- true when the model holds no surface for this
	// layout, which is the one moment a fold can be decided. `may_offset` is the caller's veto (see
	// EnsureSurface); a false here holds the layout to zero-offset placements permanently, and
	// unfolds it if it is already sitting at a nonzero one. While kGSTileContainDisplacedViews is
	// false the standing rule holds EVERY layout to that, so the veto and the unfold it carries are
	// both dead weight -- correct dead weight, which the displaced rung turns back on.
	GSTileContainedView PlaceContainedView(const GSTileSurfaceLayout& layout, const GSVector4i& rect,
		bool first_sight, bool may_offset);
	// The other half: this view took a surface of its own, so that surface is its own container and
	// holds the view's page rows. Split out of the placement because it needs the id, which the
	// placement runs before the create to learn.
	void NoteOwnContainer(GSTileSurfaceId id, const GSTileSurfaceLayout& layout, const GSVector4i& rect);
	// ...and the id it took may be a slot an earlier surface used, whose rows and containees are
	// not this one's.
	void NoteSurfaceRecycled(GSTileSurfaceId id);
	// A pass key change between consecutive draws, counted twice over: once on the surfaces the
	// draws actually take, and once on the containers they would take. Their difference IS the
	// saving, and it is only visible at this level -- EnsureSurface sees one draw at a time. (With
	// the lever on the two columns are the same number, because the fold is the road taken.)
	void CountContainmentBreaks(GSTileSurfaceId fb_id, GSTileSurfaceId z_id, bool z_used);
	// The container's page-row count after admitting a view that reaches `end_row`.
	void NoteContainerRows(GSTileSurfaceId id, u32 end_row);
	/// Whether `bp` is an enabled PCRTC display base right now. Design rule 9, and the moment it is
	/// asked at.
	bool IsDisplayBase(u32 bp) const;
	GSTileViewAliasTable m_contain_alias;
	/// Layouts that may only ever be placed at a ZERO offset, however legal the predicate finds a
	/// displaced one: the display bases, and every view a draw has ever rendered with a depth
	/// attachment. Permanent, because the reason arrives later than the fold decision does and a
	/// view that folded and unfolded and folded again would re-seed its pages each time.
	std::unordered_set<u32> m_contain_no_offset;
	/// Per surface id, the page ROWS the surface holds as a container -- its own view's rows and
	/// every folded view's. Off the lever nothing grows, so this cannot be read back off the pool;
	/// on it, it is still the arithmetic that says whether a later fold needs the container to grow
	/// or already fits inside it, which the pool's height alone cannot answer.
	std::vector<u32> m_contain_rows;
	/// The lever, read once at construction like every other pass-shape lever here: a mid-run flip
	/// would leave folded views with no surface and unfolded ones with no container.
	bool m_contain_surfaces = false;
	GSTileSurfaceId m_contain_view = kGSTileNoSurface; ///< the last colour placement's answer
	GSTileSurfaceId m_contain_prev_color = kGSTileNoSurface;
	GSTileSurfaceId m_contain_prev_folded = kGSTileNoSurface;
	GSTileSurfaceId m_contain_prev_depth = kGSTileNoSurface;
	bool m_contain_prev_depth_used = false;
	/// Cleared at the frame boundary: a break is a change WITHIN a frame, and the first draw of a
	/// frame follows the last draw of the one before it in nothing but wall-clock order.
	bool m_contain_prev_valid = false;

	// Bring each enabled display buffer's texture up to date with its bytes, as a draw-less tail
	// of the frame's plan. The present is the one reader that takes the texture instead of the
	// bytes, so a page the display surface does not hold -- never written, or written into a
	// surface that has since taken truth of it -- reaches the screen as stale texels however
	// correct the byte model is. Fired from VSync, just before the plan is built.
	void MaterialiseDisplayBuffers();

	// Emit a prep op over `pages` for surface `id` (Writeback or Seed) on the pending draw being
	// accumulated; block masks per page come from the model for writebacks, full for seeds.
	// `seed_blocks` narrows a SEED to part of each page it names; the default is the whole page,
	// which is what every road but the upload merge wants (see GSTileGpuPrepOp::seed_blocks).
	void EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages,
		u32 seed_blocks = GSVramModel::kFullBlockMask);

	// Append a pending draw that renders nothing and exists only to carry the prep ops emitted
	// since `first_prep_op`. The executor runs a pass's op range at the pass head, and it takes
	// that range from the pass's DRAWS -- so an op emitted outside every draw's range is inside no
	// pass and is silently never run. This is what gives one to the two roads that compose bytes
	// with no draw of their own to hang them on (the display materialise, the upload merge), and it
	// always breaks the pass, so the ops land behind every draw recorded so far and ahead of every
	// draw after: that is the sequence point both roads need.
	//
	// `color` is the surface the pass binds -- a pass with no attachment at all is skipped by the
	// executor. Returns false when the range is empty, having appended nothing.
	//
	// `write_pages` is the guest footprint its ops land in, for m_plan_write_pages -- the pages of
	// the surface the seeds fill. It reads nothing, so its read footprint is empty.
	bool AppendPrepOnlyDraw(GSTileSurfaceId color, const GSVector4i& rect, u32 first_prep_op,
		const GSPageBitmap& write_pages);

	// Truth on `pages` cannot reach the byte store at all: count it and warn once. The depth plane
	// (no writeback shader) and surfaces whose layout has no byte road are the two roads here.
	void NoteLossyPages(const GSPageBitmap& pages, GSTileSurfaceKind kind);

	// Of `pages`, the ones a DEPTH SEED may take: the compose can serve their real guest bytes
	// (gsTilePageByteTruthReachable) AND every plane holding truth on them holds it through a pixel
	// space that IS this Z buffer's (gsTileDepthSeedSourceMatches). The rest keep whatever the depth
	// image has, exactly as they did before the depth seed existed, and stay counted lossy.
	// ⚠️ Only meaningful BEFORE the compose: see gsTilePageByteTruthReachable.
	GSPageBitmap PagesDepthSeedable(const GSPageBitmap& pages, const GSTileSurfaceLayout& z_layout) const;

	// One page's four planes as the ring's compose sees them. The shape EnsureRingSlot decides a
	// prefill from and PagesWithoutByteTruth decides seedability from; shared so the two cannot come
	// to read the model differently.
	void RingPlaneStateFor(u32 page, GSTileRingPlaneState (&planes)[kGSTilePlaneCount]) const;

	// Truth on `pages` is taken by a surface with no byte road: mark it synced without moving
	// bytes, and count it lossy.
	void LossySteal(const GSPageBitmap& pages, GSTileSurfaceKind kind);

	// Compose `pages` into the ring for the draw being accumulated, breaking the open pass if the
	// writebacks it emitted cannot be hoisted to that pass's head. The idiom a texture read has
	// always used, shared with the depth claim's spill.
	void ComposeForPendingDraw(const GSPageBitmap& pages, PendingDraw& pd);

	// One draw's colour and depth surfaces claim the same pages -- the game packed FRAME and ZBUF
	// close enough that a single draw's two footprints share them. Only one surface can be the
	// model's holder of a page, so the other's byte truth there is dropped: same move as a
	// road-less steal, counted apart because the cause is the game's memory layout rather than a
	// surface this stage cannot write back yet.
	void AliasSteal(const GSPageBitmap& pages);

	// Resolve a surface's pool texture into the plan's target list (once per frame per surface).
	u32 PlanTargetIndex(GSTileSurfaceId id);

	// Draw `d`'s indirect-run key, off the plan arrays as they stand mid-build. The same key the
	// executor cuts its runs on (GSTileGpuPassPlan::RunKeyAt) -- the plan does not exist yet at the
	// point the call-cost counters need it, and two spellings of "what starts a new run" would make
	// those counters describe a submission shape the executor does not produce.
	GSDevice::GSTileGpuPassPlan::GSTileGpuRunKey PlanRunKeyAt(u32 d) const
	{
		return GSDevice::GSTileGpuPassPlan::GSTileGpuRunKey{m_plan_topologies[d], m_plan_blend_keys[d],
			m_plan_depth_modes[d],
			(m_plan_variant_keys.size() == m_plan_draws.size()) ? m_plan_variant_keys[d] : 0u};
	}

	// Draw `d`'s FRAGMENT VARIANT, as the executor will compile it: its own texel road, its own byte
	// road decode arm, what it alone needs the destination read for, whether its own output is what a
	// 16-bit frame stores, and -- the specialization half -- the per-draw GS state the program takes
	// as a compile-time constant instead of reading out of the state row. `pass_self_mask` is the
	// union its pass declares.
	//
	// The specialization half is purely per draw. Every field is one this draw already carries in its
	// row, so freezing it cannot change what the fragment computes; what it changes is that the value
	// is not fetched and not branched on. There is no pass union for these axes and there should not
	// be: the four masks above are ORed over the pass because the pass's DESCRIPTORS depend on them,
	// and nothing about a pass depends on which alpha comparison its draws run.
	//
	// Two of the four axes are not purely per-draw and the promotion below is what keeps this
	// mechanism PIXEL-INERT rather than merely narrower:
	//
	//  - DATE. Where a pass declares the read for its destination-alpha test it takes no snapshot
	//    copy, so a draw with DATE in that pass has nothing to read but the live pixel, whether or
	//    not its own admission asked for one. Narrowing it to the snapshot road would fetch a null
	//    texture.
	//  - The AFAIL alpha keep. Its state-row bit is set for every draw the fold names, admitted or
	//    not, so a refused draw inside a pass that compiled the bit-mask arm keeps its alpha today.
	//    That is an inconsistency in the row (see the report), not something this lane changes.
	//
	// Both promotions are unreachable in either shipped configuration -- a non-segregating device
	// admits every class, and a segregating one keys non-readers into their own pass -- so they cost
	// nothing measurable and exist so that a future admission policy cannot silently move a pixel.
	u32 PlanVariantKeyAt(u32 d, u32 pass_self_mask) const
	{
		const PendingDraw& pd = m_plan_pending[d];
		u32 texel = 0;
		if (pd.tex_enable && (pd.road_mask & GSDevice::kGSTileGpuRoadByte) != 0)
		{
			texel = GSDevice::GSTileGpuTexelArm(pd.index_format);
			if (pd.pal_mode != 0)
				texel |= GSDevice::kGSTileGpuTexelPalGather;
		}
		u32 self = pd.self_mask;
		if (pd.date != 0)
			self |= pass_self_mask & GSDevice::kGSTileGpuSelfDate;
		if (pd.afail_keep_alpha)
			self |= pass_self_mask & GSDevice::kGSTileGpuSelfMask;
		GSDevice::GSTileGpuFragmentSpec spec;
		if (!GSConfig.TileGpuUnspecializedFragmentVariant)
		{
			// The road mask answers the textured block's own gate, so no key bit is spent on
			// tex_enable -- and the two must agree, because the shader stops testing the row once a
			// road is named. Every road is set inside the tex_enable branch of AccumulateDraw, so
			// they do; this is where that would be caught if it ever stopped being true.
			pxAssertMsg(pd.tex_enable == (pd.road_mask != 0),
				"TileGpu draw's texel road disagrees with whether it samples at all");
			spec.valid = true;
			spec.fst = pd.fst ? 1 : 0;
			spec.ltf = pd.ltf ? 1 : 0;
			spec.tfx = static_cast<u8>(pd.tfx & 3u);
			spec.tcc = (pd.tcc != 0) ? 1 : 0;
			spec.atst = static_cast<u8>(pd.atst);
			spec.fge = pd.fge ? 1 : 0;
			spec.date = static_cast<u8>(pd.date);
			spec.wms = static_cast<u8>(pd.wms & 3u);
			spec.wmt = static_cast<u8>(pd.wmt & 3u);
			spec.texa = static_cast<u8>(pd.texa & 3u);
			spec.NarrowToRoad(pd.road_mask);
		}
		return GSDevice::GSTileGpuPassPlan::PackVariantKey(pd.road_mask, texel, self, pd.quantise_5551, spec);
	}

	// Fill m_plan_variant_keys for every accumulated draw, and apply the specialization bind guard.
	//
	// Runs before the grouping walk in BuildAndExecutePlan and over the same pass cut, because the
	// guard's question is answered per PLAN and not per pass: how many pipeline binds does freezing
	// per-draw GS state add to this whole submission? A pass cannot know that while it is being
	// built, and the grouping walk emits each pass's draws as it goes.
	void PlanFragmentVariants();
	/// One pass, as PlanFragmentVariants measured it: its draw range and the runs its draws make with
	/// and without the frozen state. Held so the guard's second phase does not walk the keys twice.
	struct SpecPass
	{
		u32 first;
		u32 end;
		u32 runs;
		u32 runs_despec;
	};
	std::vector<SpecPass> m_spec_passes;
	/// Said once, the first time the guard actually withholds anything, so an archived log carries
	/// that it ENGAGED and not merely that a budget was set.
	bool m_specguard_announced = false;

	// Copy one flushed batch's geometry into the frame streams, drive the memory model for its
	// target and texture footprints (seeds, writebacks, claims), and record its indirect draw +
	// PendingDraw. Fired from Draw() beside ObserveDraw().
	void AccumulateDraw();

	// Group the accumulated draws into passes, resolve the state rows and targets, build the
	// ring, and submit the assembled plan to the device executor. Fired from VSync before the
	// base presents (so the rendered targets are ready when GetOutput runs), and mid-frame by
	// the stall roads (a CPU reader of GPU-newest pages needs everything before it rendered).
	void BuildAndExecutePlan();

	// Pull `pages` (GPU-newest, any owner) down into the CPU shadow: flush the plan so the
	// targets hold every draw so far, then the pool's synchronous readback per owner. The one
	// stall road; `site` names the caller for the counters.
	enum class StallSite : u32
	{
		UploadSubBlock = 0, ///< an upload partially overwrites blocks only a target holds
		LocalRead,          ///< a local->host transfer (or a move's source) of GPU-newest pages
		Clut,               ///< a CLUT load from GPU-newest pages
		SyncAll,            ///< whole-of-truth: savestate / switch / purge
		Count
	};
	void ReadbackToShadow(const GSPageBitmap& pages, StallSite site);
	/// The same, for a caller whose ask is a RECT: refines "which pages must be pulled" through
	/// the model's block sidecar, so a read landing only on blocks the owner never wrote costs
	/// nothing at all. `pages` must be the rect's own page set.
	void ReadbackToShadow(const GSTileSurfaceLayout& layout, const GSVector4i& r, const GSPageBitmap& pages,
		StallSite site);
	/// The pull, shared by both: the plan is already flushed and `need` already settled.
	void PullToShadow(const GSPageBitmap& need, StallSite site);

public:
	/// One pool readback: a page set of ONE owner surface, under one truth block mask and one
	/// byte window. The unit the device round trip is charged in.
	struct PullCall
	{
		GSTileSurfaceId owner;
		u32 block_mask;
		u32 write_mask;
		GSPageBitmap pages;
	};

	/// Plan `need`'s pull as a list of calls, in the order they must be issued.
	///
	/// Static and model-only so the two properties it exists for can be pinned without a device:
	/// pages sharing an (owner, block mask, byte window) collapse into ONE call, and a page whose
	/// pull needs more than one call is never collapsed with anything -- because two calls on one
	/// page can have OVERLAPPING byte windows (a depth plane's is all four bytes, a colour plane's
	/// the low three), so their order is load-bearing, and bucketing across pages can float the
	/// second ahead of the first. See gs_tilegpu_pull_grouping_tests.cpp.
	static void PlanPull(const GSVramModel& model, const GSPageBitmap& need, std::vector<PullCall>& out);


private:
	std::vector<PullCall> m_pull_calls; ///< PlanPull's scratch, kept so a pull allocates nothing

	void SyncAllTruthToCpu();

	// Submit whatever the frame has planned so far, so the resident targets hold every draw up to
	// this point (counted as a mid-frame flush); a no-op when nothing is pending. Separately
	// callable because the flush RETIRES EVERY SYNCED CLAIM -- the ring slots those claims named
	// are spent, and their bytes were never in the CPU shadow -- so a caller whose question is
	// phrased in terms of `synced` has to ask it on the far side of this, never before.
	void FlushPendingPlan();

	// --- the open pass, as accumulation sees it ----------------------------------------------
	//
	// One of these per RUN. A run is a pass the planner is still adding draws to; there is exactly
	// one until draw reordering opens more, and everything below is then per-run rather than global
	// so a draw's prep ops are tested against the pass they will actually land in.
	struct OpenRun
	{
		// The executor runs a pass's prep ops before the pass opens, so a writeback emitted for the
		// draw being accumulated is hoisted over every draw of the open pass already planned. That
		// is what the next four track: what those draws have done, so the hoist can be tested
		// instead of assumed unsafe. A pass has exactly one colour and one depth surface
		// (BuildAndExecutePlan groups on the pair), so two write bitmaps cover every surface its
		// draws can render into.
		//
		// The read side is per RING SLOT, not per page: `read` says which pages the pass's draws
		// sample and `read_slot` which slot each of them read (entry index + 1), because a page
		// whose slot was superseded since is a different slot and rewriting it is invisible to the
		// earlier read. `pass` is the grouping key itself, the same type the plan build groups on,
		// so the two cannot drift; everything resets whenever the pass breaks.
		GSTileGpuPassKey pass;
		GSPageBitmap color_written;
		GSPageBitmap z_written;
		GSPageBitmap read;
		std::array<u16, GS_MAX_PAGES> read_slot{}; // meaningful only where `read` is set

		// The rule-2 targets the open pass's draws sample, in the order they were first bound --
		// which IS the slot numbering the executor and the shader use, so it is rebuilt the same way
		// at plan build (asserted there). A pass binds at most kMaxTexSourcesPerPass of them; a draw
		// whose source would be one too many opens a pass of its own.
		std::array<GSTileSurfaceId, GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass> tex_src{};
		u32 tex_count = 0;

		// How many draws the open pass already holds, for m_max_pass_draws. Counted where the draw
		// joins the pass and reset with everything else in BreakOpenPass, so it stays in step with
		// the plan build's own count of the pass it is cutting (j - i there).
		//
		// The two are the same number because accumulation pushes exactly one plan entry per draw it
		// counts, with no return between the count and the push. The display-buffer seed
		// pseudo-draws push plan entries accumulation never counts, but each carries break_before
		// AND calls BreakOpenPass after itself, so no draw is ever counted against one on either
		// side.
		u32 draw_count = 0;

		// The DATE snapshot's own run tracking, which outlives an ordinary pass break and so is NOT
		// cleared by BreakOpenPass: the colour surface a stretch of draws has been rendering into,
		// and the rect union they have written into it since the last FORCED break. A DATE draw
		// whose rect intersects that union would read pixels the snapshot no longer has, so it opens
		// a pass of its own. (These were m_run_surface / m_run_written.)
		GSTileSurfaceId date_surface = kGSTileNoSurface;
		GSVector4i date_written = GSVector4i::zero();
	};

	// One per run, in open order, and `m_open_run` is the one the draw being accumulated joins.
	// ⚠️ It must be chosen BEFORE any prep op of that draw is emitted: everything below the choice
	// reads the open pass to decide what may be hoisted into it and which rule-2 slot the draw
	// takes, and a run picked after those reads would test them against the wrong pass.
	std::array<OpenRun, GSTileGpuReorderScheduler::kMaxRuns> m_open_runs;
	u32 m_open_run = 0;
	OpenRun& CurrentRun() { return m_open_runs[m_open_run]; }
	const OpenRun& CurrentRun() const { return m_open_runs[m_open_run]; }

	// The CURRENT run's open pass is closed: the next draw accumulated into that run is the first of
	// a new pass, so nothing recorded above constrains it. Idempotent, and the state it leaves
	// matches no draw, so it also serves as the initial and between-frames state. It deliberately
	// leaves the DATE snapshot's run alone -- that one spans ordinary pass breaks.
	void BreakOpenPass();

	// A flush emitted `runs_emitted` runs, so the pass the next draw follows is the LAST of them.
	// Carry that run's open pass into run 0 -- the run the next draw joins, since the flush emptied
	// the backlog -- and reset the rest. See the definition: getting this wrong is not a slow frame.
	void CarryOpenPassAcrossFlush(u32 runs_emitted);

	// Every run goes back to its initial state and the current one is run 0. The plan boundary, and
	// only the plan boundary: a plan build emits every run, so nothing any of them recorded can
	// constrain a draw of the next plan -- the DATE snapshot's run included, since the snapshot is
	// retaken with the pass.
	void ResetOpenRuns();

	// Would the writeback ops appended since `first_op` read or write something the open pass's
	// draws already touched? Then they cannot run at that pass's head and their draw must open
	// its own.
	bool WritebackHoistCollides(u32 first_op) const;

	// The model's per-frame traffic, mean/p50 at teardown beside the pass structure. These are
	// the structural counters the stage-1 gate reads.
	struct ModelFrame
	{
		u32 surfaces_live = 0;
		u32 ring_pages = 0;      // ring entries this frame
		u32 ring_prefill = 0;    // ...of which prefilled from S / a version copy
		u32 ring_versions = 0;   // version copies taken (uploads superseding a live slot)
		// Slots the per-plane proxy would have left for the GPU to compose whole, that the
		// byte-exact coverage test caught: their planned writebacks do not reach every byte of every
		// block, so they take the S prefill instead of shipping the ring offset's previous tenant.
		// Nonzero is not a fault -- it is the class the two rules can disagree on, made visible, and
		// on today's corpus it is a PSMCT24 surface's unwritten alpha byte in every case.
		u32 prefill_uncomposed = 0;
		u32 epochs = 0;
		u32 writeback_ops = 0;
		u32 writeback_pages = 0;
		u32 writeback_breaks = 0; // draws that opened a pass because their read needed a writeback
		u32 seed_ops = 0;
		u32 seed_pages = 0;
		// ...of which the DEPTH seeds', counted apart because their population is a different
		// question: how much of a Z buffer a game rebuilds by writing bytes over it, and how much of
		// that this road now carries instead of leaving the depth image accumulating.
		u32 seed_ops_depth = 0;
		u32 seed_pages_depth = 0;
		u32 seed_breaks = 0;     // draws that opened a pass because their target needed seeding
		// The CPU->GPU upload merge: spills served on the device instead of by draining the target
		// to the shadow. `merge_ops` is the population that used to be StallSite::UploadSubBlock
		// stalls, so the two columns add up to the spills a frame takes, and the refusals say which
		// clause sent the rest down the blocking road.
		u32 merge_ops = 0;
		u32 merge_pages = 0;
		u32 merge_ref_owner = 0;    // no single byte-road owner holding every plane of the page
		u32 merge_ref_cpu = 0;      // a CPU read has already had to pull this page down once
		u32 merge_ref_bytes = 0;    // the write owns half a byte somewhere: no byte mask expresses it
		u32 merge_ref_slice = 0;    // the rect is a claim, not a record (a sliced or truncated upload)
		u32 merge_ref_overflow = 0; // the footprint lost its block masks, so coverage is unknown
		u32 date_breaks = 0;     // draws that opened a pass because their DATE read needed a fresh snapshot
		u32 snapshots = 0;       // passes that took a snapshot of their target
		// The fragment read-modify-write road. Both are zero on a device without it, and zero on a
		// frame no draw of which needs it -- which is the gate the road ships under: the SotC gate
		// scene has FBMSK 0 on every draw, PABE 0, no 16-bit frame and no DATE, so it must admit
		// nothing at all and pay nothing at all.
		u32 self_read_draws = 0;  // draws admitted to the in-pass destination read
		u32 self_read_passes = 0; // passes that therefore declared it
		// Draws sitting inside a declaring pass, reader or not. The one of these three a device that
		// charges for declaring actually pays -- on Adreno every draw in a declaring pass rasterizes
		// under FLUSH_PER_OVERLAP_AND_OVERWRITE, so six readers inside a giant effect pass tax the
		// whole pass. Under GSDevice::TileGpuSegregatesSelfRead it converges on the reader count.
		u32 self_read_pass_draws = 0;
		// The per-class admission census, which the three totals above cannot give: every title's
		// readers look alike in a total, and that is how Xenosaga spent weeks recorded as a
		// 16-bit-saturated title when its bulk is a single-bit alpha FBMSK on a 32-bit frame.
		//   wanted   draws that would be admitted for the class, whatever the budget said
		//   exposed  of those, the ones standing behind another draw of the class in what would be
		//            one declaring pass -- the quantity the budget is kept in
		//   refused  1 if the budget refused the class at the END of this frame, so the mean is the
		//            fraction of frames it finished off. ⚠️ END of frame: under the bootstrap an
		//            unpriced class can be admitted for most of a frame and still be recorded
		//            refused for it. The admitted-draw total on the line above is what says how
		//            much actually got through.
		u32 class_wanted[kGSTileGpuAdmissionClasses] = {};
		u32 class_taxed[kGSTileGpuAdmissionClasses] = {};  // of those, standing behind another of the class
		u32 class_runs[kGSTileGpuAdmissionClasses] = {};   // ...and the declared runs they fall into
		u32 class_peak[kGSTileGpuAdmissionClasses] = {};   // the decayed cost peak the verdict was made on
		u32 class_refused[kGSTileGpuAdmissionClasses] = {};
		// The colour write mask's own census, counted whether or not TileGpuShaderWriteMask is on --
		// the population is what decides whether the lever is worth a device round on a title, and a
		// counter that only exists in the arm that moved cannot answer that.
		//   masked    draws carrying a partial colour write mask: the raw population, and the number
		//             of run cuts it is worth is at most this
		//   servable  of those, the ones whose FRAGMENT output is what lands, so the shader road is
		//             bit-exact for them
		//   moved     ...and the ones that actually took it this frame (zero with the lever off)
		u32 mask_partial_draws = 0;
		u32 mask_servable_draws = 0;
		u32 mask_shader_draws = 0;
		u32 self_reads = 0;      // draws sampling pages their own pass target holds (snapshot semantics)
		u32 tex_binds = 0;       // draws served by rule 2: the read window came off a resident target
		u32 tex_bind_breaks = 0; // draws that opened a pass because their bind could not join the open one

		// Rule 3 as probed (ProbeSourceRoad) -- the admission question, asked for every rule-2-declined
		// draw whatever its format. The direct-colour half of `src_eligible` goes on to be built and
		// SERVED (src_served below); the paletted half is still measurement only.
		u32 tex_draws = 0;         // textured draws in a format either road samples (the denominator)
		u32 tex_unsupported = 0;   // TME draws in a format neither road samples: rule 3's format refusal
		u32 tex_hi_draws = 0;      // ...of tex_draws, the alpha-byte views (PSMT8H/PSMT4HL/PSMT4HH).
		                           // Counted apart because they are the one paletted family whose page
		                           // geometry is CT32's, so a page-term regression shows only here --
		                           // and because a dump reading zero of them is a dump whose frames a
		                           // change to this road must leave byte-identical.
		u32 src_eligible = 0;      // rule-2-declined draws rule 3 would serve
		u32 src_hit = 0;           // ...served with no build at all (window and stamp both already seen)
		u32 src_rebuild = 0;       // ...served after a rebuild: the window is known and its bytes moved
		u32 src_fresh = 0;         // ...served after a first build: the window was not seen last frame
		u32 src_ref_region = 0;    // refused: REGION_CLAMP / REGION_REPEAT on either axis
		// Refused because the MATERIALISE has no arm for the window's format -- the 16-bit
		// families, which the byte road samples directly and nothing builds into an image. Its own
		// counter and not src_ref_region's: that one is a window SHAPE the road declines, this is a
		// road that does not exist.
		u32 src_ref_srcfmt = 0;
		u32 src_mip_draws = 0;     // NOT a refusal: MXL != 0, eligible, served at level 0 like every
		                           // other road serves it today (mip LEVEL SELECTION is M4)
		u32 src_mip_live = 0;      // ...of those, draws where mipmapping is actually active: the ones
		                           // whose correct level is the one M4 will have to choose
		u32 src_ref_capacity = 0;  // refused: the frame's distinct sources exceed the source array
		u32 src_ref_palette = 0;   // refused: first sight of the (index, palette) pair, admitted next frame
		u32 src_ref_pin = 0;       // refused: rebuilding a window an earlier draw of this frame named
		u32 src_distinct = 0;      // distinct source windows the frame would hold

		// Rule 3 as BUILT (MaterialiseSourceRoad): what the cache actually did for the admitted
		// direct-colour windows. These are the real cache's numbers, where the columns above are
		// the probe's model of it -- the two agree or one of them is wrong.
		u32 src_builds = 0;        // materialise passes emitted: the window was fresh or its bytes moved
		u32 src_cache_hits = 0;    // served by an entry whose gen stamp was still current
		u32 src_cache_rescued = 0; // ...whose stamp had moved but whose content token had not
		u32 src_pin_refusals = 0;  // the cache refused a rebuild under a draw this frame has recorded
		u32 src_cap_refusals = 0;  // ...refused because every entry is pinned by this frame
		u32 src_build_failed = 0;  // allocation failed, or the op could not be queued
		u32 src_served = 0;        // draws that TOOK road SOURCE: their fragment stage sampled the image
		u32 src_bind_slots = 0;    // distinct (image, sampler) pairs the frame bound
		u32 src_ref_bind = 0;      // refused: the frame's bind table is full (kMaxSourceSlots)

		// The paletted road's second stage. A window materialises to an INDEX image (counted in
		// src_builds beside the direct-colour ones, since it is the same build); the colour comes from
		// the (index x palette) expansion, and THESE are its columns. High deferrals beside high hits is
		// the designed shape, not a fault: a pair is admitted on second sight across a frame boundary,
		// so a window drawn once in its life never pays for an expansion pass at all.
		u32 src_pal_draws = 0;      // draws admitted on a paletted window (the denominator here)
		u32 src_pal_missing = 0;    // ...whose palette texture could not be built: refused to the byte road
		u32 src_expand_hits = 0;    // served by an expansion the cache already held
		u32 src_expand_builds = 0;  // expansion passes emitted: the pair earned one this frame
		u32 src_expand_defer = 0;   // the pair was first-sighted at BUILD time: refused for one frame
		u32 src_expand_cap = 0;     // ...refused because every expansion entry is pinned by this frame
		u32 src_expand_failed = 0;  // allocation failed, or the op could not be queued

		// The DONOR road: rule 3's build taken off the owner target instead of off the ring. Its
		// draws are counted in src_served beside the ring-built ones -- the road a draw takes is
		// still SOURCE either way, and what changes is where the image came from and what did NOT
		// have to happen for it (no writeback, no ring slots, no page fingerprint).
		u32 src_donor_eligible = 0;  // admitted windows one live target could serve whole
		u32 src_donor_served = 0;    // ...that actually took it: no compose, no ring traffic
		u32 src_donor_builds = 0;    // reinterpretation passes emitted
		u32 src_donor_hits = 0;      // served by an entry whose gen stamp was still current
		u32 src_donor_rescued = 0;   // ...whose stamp had moved but whose TARGET identity had not
		u32 src_donor_refused = 0;   // eligible, then refused downstream (pin, capacity, palette, build)
		u32 src_donor_breaks = 0;    // draws that opened a pass because the build could not hoist
		u32 src_donor_direct = 0;    // direct-colour windows a sole owner holds that rule 2 did not
		                             // take: the population a donor COPY leg would serve, measured
		                             // rather than assumed (this road reinterprets, it does not copy)

		u32 src_extra_calls = 0;   // extra indirect calls the sampled-binding split actually cost
		u32 src_stamp_kept = 0;    // windows also probed last frame whose gen stamp held
		u32 src_stamp_moved = 0;   // ...and whose gen stamp moved (the raw number, C1's: the two
		                           // below are subsets of THIS one, not additions to it)
		u32 src_stamp_rescued = 0; // ...of those, the stamp moved but the pages' bytes hashed
		                           // identical, so the window is unchanged and keeps its identity
		u32 src_stamp_opaque = 0;  // ...of those, the fingerprint could not be taken (GPU-side truth
		                           // under the window), so the move has to be believed. Disjoint
		                           // from rescued; a rebuild the probe counts but cannot prove.
		                           // Rebuilds the probe believes = moved - rescued, of which
		                           // `opaque` are unproven and `moved - rescued - opaque` are
		                           // bytes that demonstrably changed.
		u32 alias_steal_pages = 0; // pages one draw claimed through both its surfaces (FRAME/ZBUF packing)
		u32 lossy_pages = 0;     // truth moved without a byte road (depth, or a base that is not page-aligned)
		// ...of which the DEPTH plane's. The gap here USED to be the whole depth direction; the
		// SEED half now exists (GSTileGpuPrepKind::SeedDepth), so what is left in this counter is
		// the WRITEBACK half -- pages whose newest Z lives in a depth attachment that something
		// else wants as bytes, which no shader can carry back. Split out from `lossy_pages`
		// because "colour lossy pages" is a gate and "depth lossy pages" is a backlog row, and one
		// number cannot be both.
		u32 lossy_pages_depth = 0;
		u32 skipped_draws = 0;   // draws no surface could be built for (format / stride)

		// Surface-identity containment as placed (PlaceContainedView): off the lever, the fold the
		// renderer did NOT take; on it, the fold it did. The two break pairs are the whole point --
		// the same draw stream keyed on today's surfaces and on the containers, so their difference
		// is what the fold removes (and, on the lever, has removed).
		// ⚠️ The census's frame denominator, and it counts DRAWS and not surface lookups: a frame
		// the game presents without drawing into it still materialises its display buffer, so a
		// lookup count says every frame was drawn and halves every mean on a title that presents
		// two frames per drawn one (Yu-Gi-Oh! does exactly this).
		u32 contain_draws = 0;
		u32 contain_first_sight = 0; // ...a view with no surface and no container yet
		u32 contain_folds = 0; // ...that a live container could have held: the fold set
		u32 contain_folded_draws = 0; // draws whose colour target would be a container, not their own
		u32 contain_growth_pages = 0; // pages the containers grew by to admit them
		u32 contain_max_pages = 0; // the largest page footprint any container reached (a MAX, not a sum)
		// A view whose later draws outgrew the clause that admitted it. Nonzero says the fold was
		// decided on an extent that did not hold, which is a first-sight problem no growth budget
		// fixes -- the admission has to be re-asked, or the view has to be evicted.
		u32 contain_regrow_refused = 0;
		u32 contain_ref_none = 0; // no live colour surface to test at all
		u32 contain_ref_family = 0;
		u32 contain_ref_stride = 0;
		u32 contain_ref_align = 0;
		u32 contain_ref_delta = 0;
		u32 contain_ref_column = 0;
		u32 contain_ref_extent = 0;
		u32 contain_ref_wrap = 0;
		// First sights whose only legal containers wanted to DISPLACE them. Not a predicate refusal
		// -- the geometry is perfectly legal -- so it is counted apart from the clauses above, and
		// it is the census the displaced-containment rung reads to size its own work: while
		// kGSTileContainDisplacedViews is false this is every displaced candidate the corpus has,
		// and when it is true it goes back to being the per-view veto's own count (the display
		// bases, design rule 9, and the views a depth-carrying draw named).
		u32 contain_ref_displaced = 0;
		// ...and folds undone, because the veto arrived after the displaced fold did. Every one of
		// these re-seeds the view's pages into a surface of its own, so a number that does not
		// settle to zero after the first frames is a policy that is thrashing. Zero by construction
		// while displacement is off: nothing is ever folded at a nonzero offset to undo.
		u32 contain_unfolded = 0;
		// Q5 of the design's open questions: draws where rule 2's target bind was legal in every
		// respect but the base, because the window's pages are owned by the container the read's own
		// view is folded into. The offset bind is not built (design 3.6), so the draw takes the byte
		// road -- correct, three to four times the fragment instructions, and invisible without this.
		u32 contain_bind_refused = 0;
		// Draw reordering, counted and not taken (TileGpuReorderRuns < 0). The scheduler runs over the
		// finished plan exactly as it would over the open one -- a plan IS the reorder window, since
		// every road that can observe a target's bytes flushes the plan before it asks anything -- so
		// these are what the lever would do, not an approximation of it.
		// ⚠️ Per DRAWN frame, like the containment census above and for the same reason.
		u32 reorder_draws = 0;    // the census's frame denominator: draws the model saw
		u32 reorder_admitted = 0; // ...that joined a run
		u32 reorder_ref_feedback = 0; // refused: the draw samples pages it renders into
		u32 reorder_ref_date = 0;     // refused: destination-alpha test on the pass-snapshot road
		u32 reorder_ref_roaa = 0;     // refused: an in-pass destination read (self_mask)
		u32 reorder_ref_prep = 0;     // refused: a prep-only pseudo-draw, which renders nothing
		// Hazards, per REFUSED DRAW and not per conflicting run: a draw that collides with two runs on
		// the same channel is one refusal, and counting it twice would say more draws were refused
		// than the frame has.
		u32 reorder_haz_raw = 0; // the draw reads pages a queued run writes
		u32 reorder_haz_war = 0; // the draw writes pages a queued run reads
		u32 reorder_haz_waw = 0; // the draw writes pages a queued run writes
		// ...and the one that is not a hazard in an ordinary renderer: the draw READS pages a queued
		// run reads. The first reader of a page in guest order is the one whose prep ops compose its
		// ring slot, build its source image or gather its palette; every later reader emits nothing
		// and hits what that one built. Invert two readers and the hit runs before the build.
		u32 reorder_haz_rar = 0;
		u32 reorder_runs = 0;         // runs emitted over the frame
		u32 reorder_passes = 0;       // passes the plan cuts into as it stands
		u32 reorder_passes_moved = 0; // ...and as the reordered emission order would cut into
		u32 reorder_peak_draws = 0;   // the most draws held across all open runs at once (a MAX)
		u32 reorder_peak_runs = 0;    // ...and the most runs open at once (a MAX)
		u32 contain_breaks = 0; // colour-surface changes between consecutive draws, today
		u32 contain_breaks_folded = 0; // ...under containment
		u32 contain_full_breaks = 0; // ...with the depth surface and its presence in the key
		u32 contain_full_breaks_folded = 0;
		// The alpha test decided at plan time, over the draws that would otherwise have carried a
		// per-fragment test: a comparison the draw's constant fragment alpha provably fails, or
		// provably passes. ATST NEVER and ALWAYS are NOT counted -- they never reached the fragment
		// stage. These are the population the fold moves, per frame, which is what says a dump the
		// fold cannot touch must come back byte-identical.
		u32 atst_fold_fail = 0;
		u32 atst_fold_pass = 0;
		// The exact-AFAIL population: draws whose alpha test genuinely VARIES per fragment under an
		// AFAIL that keeps something alive, so the fragment stage's discard is not what the console
		// does. Split three ways because the three are owed different things -- RGB_ONLY's alpha-keep
		// is servable by the destination read, and the depth half of any of them is not servable by a
		// COLOUR read at all.
		u32 afail_varies = 0;           // fold == Varies and AFAIL != KEEP
		u32 afail_varies_rgb_only = 0;  // ...of which AFAIL == RGB_ONLY (the alpha-keep half)
		u32 afail_varies_depth = 0;     // ...of which the draw also uses depth (the half a colour read cannot serve)
		u32 stalls[static_cast<u32>(StallSite::Count)] = {};
		u32 stall_pages[static_cast<u32>(StallSite::Count)] = {};
		// Pool calls the stalls issued. Not derivable from either column above: one consumer ask
		// becomes one call per (owner, block mask, byte window) over the whole page set it needs,
		// and the device round trip is charged per CALL -- so this is the number a coalescing
		// change moves, where stalls and pages both stay put.
		u32 pull_calls = 0;
		u32 flushes = 0;         // mid-frame plan submissions
		u32 passes = 0;
		// The pass-length distribution the max-pass-draws cap acts on, so a run says mechanically
		// whether the cap engaged rather than leaving it to be eyeballed off a pass total.
		//
		// `capped_passes` counts passes the cap CLOSED -- a pass at exactly the cap whose next draw
		// would have joined it. Not "passes of cap length": a pass can reach the cap and end there
		// anyway because the key changed or the frame ran out, and counting those would report the
		// cap as engaged on a frame it never touched.
		u32 biggest_pass_draws = 0; // the longest pass built this frame
		u32 capped_passes = 0;      // ...and how many the cap itself ended
		// The depth predictor's two counterfactual pass counts and its denominator, summed over every
		// plan the frame built (a mid-frame flush makes more than one). Both polarities are counted,
		// including the one the frame actually took, so the pair is self-consistent -- taking the
		// active side from m_plan_passes and only the other side from the counterfactual would compare
		// a number that includes the display-seed pseudo-draw's pass against one that does not.
		//
		// Zero unless the predictor is engaged: counting them is two extra cuts of the draw list, and
		// a run that is not deciding anything must not pay for them.
		u32 passes_uniform_key = 0;
		u32 passes_merged_key = 0;
		u32 planned_draws = 0;
		// The texel-arm axis of the fragment variant (GSDevice::kGSTileGpuTexel*). A pass whose
		// byte-road draws all decode through one arm gets a program carrying that arm alone; one
		// that spans two carries both, and is the only case the shatter does not shrink all the way.
		// Watched rather than assumed: the mixed share is what says the axis still pays.
		u32 texel_passes = 0;       // passes with at least one byte-road draw
		u32 texel_mixed_passes = 0; // ...whose byte-road draws span more than one arm
		u32 texel_mixed_draws = 0;  // the byte-road draws inside those passes

		// The fragment variant as a RUN key rather than a pass property
		// (GSTileGpuPassPlan::variant_keys). Three columns, and they are the before-and-after of the
		// same trade: how many draws stop executing their pass's union program, and what the extra
		// indirect calls cost.
		u32 variant_runs = 0;        // maximal runs of constant variant inside a pass (the population)
		u32 variant_extra_calls = 0; // ...of which cut a run nothing else would have cut: the COST
		// Draws whose own variant is narrower than their pass's union -- the population that stops
		// paying for a program it does not run.
		u32 variant_narrowed_draws = 0;
		// ...and the census's own column, kept in its own units so the before-and-after is directly
		// comparable to the pass-cause table: draws NOT on the byte road sitting in a pass whose
		// union takes it. Those are the ones that were executing a 1039-1748 instruction decoder for
		// nothing. The counter measures the PASS unions, so it reads the same on both arms -- it is
		// the size of the population rescued, not a residue that shrinks.
		u32 variant_byte_freeloaders = 0;
		// The SPECIALIZATION half of the variant, priced on its own. A fragment-program change costs
		// a pipeline bind, and on Adreno a bind is an instruction-RAM DMA -- so what the frozen state
		// costs is not the runs it produces but the runs it produces THAT THE ROAD DID NOT ALREADY.
		//
		// `variant_runs_despec` is the run count the same frame would have had with the spec half
		// withheld, which is exactly the TileGpuUnspecializedFragmentVariant arm's number. Counting
		// both off ONE run is the whole point: the difference is the specialization's own bind bill,
		// with no second scene to confound it.
		u32 variant_runs_despec = 0;
		// ...and the run count the guard DECIDED on: what the plan would have submitted with every
		// pass left specialized. Equal to variant_runs on a run where the guard never fires, and the
		// only way a guarded run can still report the pressure that made it fire.
		u32 variant_runs_unguarded = 0;
		// ...and the same before-and-after in PROGRAMS rather than binds: how many distinct fragment
		// programs a pass alternates among, summed over the frame's passes. This is the working set an
		// instruction cache either holds or does not, so it is the number the pass guard is about,
		// while the run counts above are how often that set is cycled through.
		u32 variant_pass_programs = 0;
		u32 variant_pass_programs_despec = 0;
		// The guard's own engagement: passes it de-specialized and the draws inside them. Zero on a
		// title the guard never fires on, which is how a run says the winners were left alone.
		u32 specguard_passes = 0;
		u32 specguard_draws = 0;

		// The GS scissor, priced. It is a per-call vkCmdSetScissor on every device, and what that
		// costs is one more indirect call wherever the scissor changes inside a run nothing else
		// cuts. These columns are that price, counted off the plan the executor actually submits
		// and against the executor's own cut rule.
		u32 scissor_draws = 0;   // draws the plan carries
		u32 scissor_subrect = 0; // ...whose scissor is narrower than their colour target
		u32 scissor_cuts = 0;    // ...and rejects part of that draw's own geometry
		u32 scissor_distinct = 0;          // distinct scissor rects, summed over the frame's passes
		u32 scissor_pass_distinct_max = 0; // ...the most any one pass of this frame carried
		u32 scissor_extra_calls = 0;       // calls the scissor adds: a cut nothing else makes

		// The As blend factor, priced as a device question, the same way the scissor above is. It
		// rides as a second fragment output (SRC1), which is a device FEATURE (dualSrcBlend) the Mali
		// blob on the RG477V hardcodes absent -- and a pipeline naming a SRC1 factor there does not
		// render wrongly, it fails to be created. These columns say how much of a frame the feature
		// carries and what each of the roads that need no feature would have to take.
		u32 dualsrc_draws = 0;      // fixed-function blended draws whose row names As as a factor
		u32 dualsrc_src_only = 0;   // ...where As multiplies the SOURCE only (see gsTileGpuDualSrcRoad
		                            //    on why that is now only informational)
		u32 dualsrc_carrier = 0;    // ...road Carrier: the alpha channel is free, nothing to give back
		u32 dualsrc_restore = 0;    // ...road CarrierRestore: the alpha blend equation gives it back
		u32 dualsrc_needs_comp = 0; // ...road CarrierCompanion: a second draw has to write the alpha
		u32 dualsrc_companions = 0; // ...and the companions this frame actually ISSUED, which is zero
		                            //    wherever the device took the second colour output instead

		// The CLUT gather. Every CLUT load is classified once (clut_loads), and the machinery has to
		// be invisible on the loads it does not serve: sotc runs ~1789 a frame and not one of them
		// needs anything at all, which is what clut_no_stall counts.
		u32 clut_loads = 0;       // CLUT loads seen
		u32 clut_no_stall = 0;    // ...whose source pages no target holds newest: the CPU road, untouched
		u32 clut_gathered = 0;    // ...served by the gather: no flush, no readback, no CPU words
		u32 clut_ref_shape = 0;   // ...owner conditions held, the load's shape is not one the gather serves
		u32 clut_ref_owner = 0;   // ...the owner conditions did not hold: today's readback road
		u32 clut_ro[kClutRefCount] = {}; // ...bucketed by the clause that refused
		u32 clut_draws = 0;       // paletted draws whose every slot came from ONE device palette
		u32 clut_draws_r3 = 0;    // ...served by rule 3, off the N x 1 gather
		u32 clut_draws_byte = 0;  // ...served on the byte road, out of the copied blocks in the stream
		u32 clut_draws_sync = 0;  // paletted draws the mirror could not serve whole: synced to the CPU
		u32 clut_r3_refused = 0;  // rule 3 refused a gathered palette (a sub-slot view, or no source)
		u32 clut_copies = 0;      // block copies emitted (one per record per plan, at a pass head)
		u32 clut_gathers = 0;     // N x 1 gather passes emitted (rule 3's volume, not the loads')
		u32 clut_breaks = 0;      // draws that opened a pass because a CLUT op could not hoist
		u32 clut_syncs = 0;       // device palettes handed back to the CPU (seams + the two unservable roads)
		u32 clut_lost = 0;        // ...of which had already lost their source: the words are unrecoverable
		u32 clut_pruned = 0;      // records recycled at the plan tail
	};
	ModelFrame m_frame = {};
	std::vector<ModelFrame> m_model_frames;

	// The fragment-variant draw census, accumulated over the whole run rather than per frame: how
	// many draws COMPILE each variant, and how many would have compiled it under the pass's union.
	// Two histograms off one run, which is the point -- the before-and-after of the decoupling with
	// no second run to confound it, and the thing you join against the device's own per-variant
	// SPIR-V word line to get a draw-weighted program size.
	//
	// A flat vector scanned linearly: a frame binds a handful of variants (six on Shadow of the
	// Colossus), so a map would cost more than it saves and would allocate on a CPU-bound path.
	std::vector<std::pair<u32, u64>> m_variant_census_own;
	std::vector<std::pair<u32, u64>> m_variant_census_union;
	static void CensusAdd(std::vector<std::pair<u32, u64>>& hist, u32 key, u64 n)
	{
		for (auto& e : hist)
		{
			if (e.first == key)
			{
				e.second += n;
				return;
			}
		}
		hist.emplace_back(key, n);
	}
	/// Print both histograms, biggest population first. Read beside the device's "TileGpu fragment
	/// variant ... N SPIR-V words" lines in the same log, which is what turns draws into a
	/// draw-weighted program size.
	void ReportVariantCensus();

	// The PASS WORKING SET census: for each distinct set of fragment programs some pass alternates
	// among, how many passes had that set and how many draws and runs they carried.
	//
	// The draw census above is frame-wide and cannot answer the question a bind cost asks, which is
	// per pass: a title binding thirty-five programs a frame is cheap if each pass uses two of them
	// and expensive if every pass cycles all thirty-five. Keyed on the SET rather than counted per
	// pass because the sets repeat -- a title's passes come in a handful of shapes -- so the histogram
	// stays small while carrying the keys an offline instrlen table joins against.
	/// Sort, unique in place, and say how many distinct values were left. Free function because the
	/// two callers want the sorted vector afterwards as much as they want the count.
	template <typename T>
	static u32 gsTileGpuSortUniqueCount(std::vector<T>& v)
	{
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return static_cast<u32>(v.size());
	}
	/// Scratch for the two counts above, held rather than local so a pass costs no allocation.
	std::vector<u32> m_shape_keys;
	std::vector<u32> m_shape_keys_despec;
	/// ...and for the per-pass scissor census. A GS scissor is four register fields of eleven bits,
	/// so [0, 2048] on every edge and the four pack into one u64 without loss.
	std::vector<u64> m_scissor_keys;

	struct PassShape
	{
		std::vector<u32> keys; ///< the pass's distinct variant keys, sorted
		u64 passes = 0;
		u64 draws = 0;
		u64 runs = 0; ///< runs under the shipped key
		u64 runs_despec = 0;
	};
	std::vector<PassShape> m_pass_shape_census;
	/// Passes whose shape arrived after the census was full, counted rather than folded into some
	/// other shape's row. Zero on every corpus dump; a non-zero one says read the census as partial.
	u64 m_pass_shapes_dropped = 0;
	void PassShapeAdd(std::vector<u32>& keys, u64 draws, u64 runs, u64 runs_despec);
	void ReportPassShapeCensus();

	bool m_warned_lossy = false;
	bool m_warned_alias = false;

	// Set the first time BuildAndExecutePlan finds a non-empty plan and no executor to give it
	// to (the device failed the TileGpu contract at construction -- GSDeviceVK.cpp's
	// tilegpu_device_capable). Reachable only under EmuCore/GS/TileGpuIgnoreDeviceContract now:
	// a contract-absent device otherwise resolves to Classic at variant selection and never
	// constructs this renderer. Every draw is dropped whether this fires or not; it only gates
	// the one-time Console.Error that says so, so a bring-up run cannot render black with a
	// clean exit code and no explanation.
	bool m_warned_no_executor = false;
};
