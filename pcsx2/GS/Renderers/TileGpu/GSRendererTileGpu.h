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

#include <array>
#include <unordered_map>
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
// which of the page's 32 blocks they hold, whether the ring already carries them, and whether
// that owner has a writeback shader at all.
struct GSTileRingPlaneState
{
	GSTileSurfaceId owner = kGSTileNoSurface;
	u32 truth_mask = 0; ///< GSVramModel::TruthMask for this page/plane (0 = no truth)
	bool synced = false; ///< the ring already holds these bytes
	bool byte_road = false; ///< the owner's layout has a writeback shader
};

/// The blocks of a page that the writebacks ComposeRingPages is about to emit will actually
/// compose, given the four planes' state.
///
/// This is EmitPrepOp's rule, stated once so the two cannot drift: an owner joins the compose
/// when it holds UNSYNCED truth on some plane and has a byte road, and each owner that joins
/// writes back the union of the truth masks of EVERY plane it owns on the page. Anything outside
/// that union is a block no writeback covers, and the slot must be prefilled from S or those
/// bytes are whatever the executor left in the slot -- which is zero.
u32 gsTileComposableBlocks(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount]);

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
///   declared runs   ...and a declaring pass costs about 77 us of its OWN, whatever it holds --
///                   transitions, load forcing, the input-attachment bind. The previous cost model
///                   scored a run of one draw as FREE, and the device refuted it in the loudest
///                   available way: FlatOut 2's exotic blends are 896 draws in 896 solo declared
///                   passes, scored ZERO, admitted, and the frame came back at 122.04 ms -- +120%
///                   against the arm before the budget existed and +383% against the arm before the
///                   read road existed, with the pass count going 235 -> 1,100.
///
/// So cost = taxed + alpha * runs.
///
/// ⚠️ alpha IS ONE, and the reason is not that a pass and an overlap happen to cost the same -- it
/// is that alpha = 1 is the only value whose verdict does not depend on a number this planner
/// cannot measure. At alpha = 1 the cost is arithmetically taxed + runs = the class's DRAW COUNT,
/// which is invariant to how those draws fall into passes; at any other alpha the split matters,
/// and the split is exactly what the run counter gets wrong. It counts runs broken by the
/// attachment group changing, and cannot see the pass breaks a prep op or a texture bind makes
/// later in the same draw -- on Ratchet & Clank's effects scene it counts SEVEN runs where the
/// device declared 357 passes. Feed that error into a term with a coefficient and it moves
/// verdicts; feed it into alpha = 1 and it cancels. The interval the device constrains is
/// [0.75, 1] and this is the robust end of it.
///
/// Kept as two terms with alpha named rather than collapsed to a draw count, because the two halves
/// were measured separately and a device round that prices a declared pass differently should be
/// able to say so here rather than reverse-engineer it.
constexpr u32 kGSTileGpuRunCostNumerator = 1;
constexpr u32 kGSTileGpuRunCostDenominator = 1;

constexpr u32 gsTileGpuClassCost(u32 taxed_draws, u32 declared_runs)
{
	return taxed_draws + declared_runs * kGSTileGpuRunCostNumerator / kGSTileGpuRunCostDenominator;
}

/// The line, and it is a fit to a constraint set rather than a round number.
///
/// Per-class costs over the 18-dump corpus under the Adreno pass shape, with the device's verdict
/// on each where it ran:
///
///   must refuse   Xenosaga's alpha-MSB FBMSK 4805, FlatOut 2's exotic blends 896, Baldur's Gate's
///                 16-bit blends 478, Ratchet & Clank's effect blends 364
///   must admit    Katamari's punctuation FBMSK 211, Beyond Good & Evil's FBMSK 62, Shadow of the
///                 Colossus' exotic blends 52, Yu-Gi-Oh's 16-bit blend 46, GT4 Online Beta's 45,
///                 and twenty-odd classes under 32
///
/// 211 and 364 are the two that bind, and the line sits between them. MGS3's exotic blends are the
/// one class in neither group: 162 draws in 162 solo declared passes, which this line admits. At
/// ~77 us a declared pass that is around 12 ms of its 29.1 ms device frame, so it is a real
/// question and not a settled one -- but no line on this metric separates 162 from Katamari's 211,
/// and separating them needs alpha ABOVE one, which this device round does not pin. Named here so
/// the next round can aim at it.
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
	// contract with its own shader (tilegpu.glsl's StateRow, std430, 80 bytes), opaque to the
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
		s32 ofx, ofy;           // XYOFFSET, 12.4 fixed: the vertex-to-pixel origin the scissor is in
		s32 sc_x0, sc_y0;       // the GS scissor in target pixels, [x0, x1) x [y0, y1) -- four VS clip planes
		s32 sc_x1, sc_y1;
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
		// 1.0). The enable bit is zero on every draw the executor blends for. It took one of the
		// row's two explicit tail padding words -- see below on why the tail is where a new field
		// goes.
		u32 blend;
		// FRAME.FBMSK reduced to the bits the frame FORMAT actually stores (FBMSK & fmsk), as a
		// per-channel KEEP mask on the target's expanded RGBA8 bytes. A 16-bit frame's mask lands
		// here unchanged because the console packs FBMSK by the same 5551 packing as the colour, so
		// bit k of a stored channel is bit k of the expanded byte. Read only by a draw that reads
		// its destination; the channel-granular half of the same mask stays in the pipeline.
		//
		// It took the row's other explicit tail padding word. The padding was there because
		// alignas(16) rounds the C++ row to 160 while std430's array stride over the fields above
		// would have been 152, and two sides on different strides read every row but the first from
		// the wrong place. Two real fields hold the stride at 160 just as well as two dead ones, and
		// the assert below is what says the row still ENDS there.
		u32 fbmsk;
	};
	static_assert(sizeof(StateRow) == 160, "TileGpu StateRow must be 160 bytes to match tilegpu.glsl std430");
	static_assert(offsetof(StateRow, blend) == 152,
		"TileGpu StateRow must end at 160 bytes with no implicit tail padding -- std430's stride would differ");

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
		u32 draw_index;
		u32 first_prep_op;    // reconciliation ops that must run before this draw's pass
		u32 prep_op_count;

		// Texture inputs, resolved at accumulation. tex_enable is set for the formats this stage
		// samples (direct 32-bit and paletted); every other textured draw falls back to the
		// vertex-colour path. See AccumulateDraw.
		bool tex_enable;
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
	std::vector<u32> m_plan_blend_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::blend_keys)
	std::vector<u32> m_plan_bind_keys; // one per m_plan_draws entry (GSTileGpuPassPlan::bind_keys)
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
	std::vector<GSDevice::GSTileGpuPass> m_plan_passes;
	std::vector<GSDevice::GSTileGpuTargetPair> m_plan_target_pairs;
	std::vector<GSDevice::GSTileGpuSnapshotCopy> m_plan_snapshots;
	std::vector<GSDevice::GSTileGpuPrepOp> m_plan_prep_ops;
	std::vector<GSDevice::GSTileGpuPageEntry> m_plan_page_entries;
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
	u32 EnsureRingSlot(u32 page);

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

	// Ensure a surface for `layout` covering `pages` exists in the model and the pool (grown as
	// needed). Returns kGSTileNoSurface on allocation failure.
	GSTileSurfaceId EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect, const GSPageBitmap& pages);

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
	GSTileSurfaceId TargetForTextureRead(const GSTileSurfaceLayout& tex_l, u32 tw, u32 th,
		const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id, GSTileSurfaceId z_id) const;

	// Whether the device can bind a resident target as a draw's texture. Read once at
	// construction, because a rule-2 draw is served by work the renderer DOES NOT DO (its read
	// window is never composed into the ring) -- asking late, or asking a device that then
	// refuses, would leave the fragment stage decoding bytes nobody wrote.
	bool m_bindless_targets = false;
	/// The device serves a pass that reads its own colour attachment in rasterization order. Read once
	/// at construction: it decides the target pool's image usage, so it cannot be discovered later.
	bool m_self_read = false;
	/// Test scaffolding (EmuCore/GS TileGpuForceSelfRead, gsrunner -tilermw): admit every draw the
	/// classifier can name, not only the classes whose repairs have landed. Read once, like the other
	/// levers here, because it moves pass boundaries.
	///
	/// It also reaches past the device's admission narrowing, because the instrument exists to
	/// exercise the road and a device that refuses a class would otherwise leave it unexercised. A
	/// forced run on a device that taxes declaring is a measurement, not a shipping shape.
	bool m_force_self_read = false;

	// Whether this device would rather have MORE passes than mixed depth state inside one
	// (GSDevice::TileGpuPrefersDepthUniformPasses). Read once at construction, for the same reason
	// the pass sim's lever is: it decides pass boundaries, and boundaries that moved mid-frame would
	// put a draw's prep ops in a pass the draw is not in. Both grouping sites read THIS member --
	// accumulation's open-pass test and the plan build's cut -- so they cannot disagree.
	bool m_depth_uniform_passes = false;
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

	// Bring each enabled display buffer's texture up to date with its bytes, as a draw-less tail
	// of the frame's plan. The present is the one reader that takes the texture instead of the
	// bytes, so a page the display surface does not hold -- never written, or written into a
	// surface that has since taken truth of it -- reaches the screen as stale texels however
	// correct the byte model is. Fired from VSync, just before the plan is built.
	void MaterialiseDisplayBuffers();

	// Emit a prep op over `pages` for surface `id` (Writeback or Seed) on the pending draw being
	// accumulated; block masks per page come from the model for writebacks, full for seeds.
	void EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages);

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
		return GSDevice::GSTileGpuPassPlan::GSTileGpuRunKey{
			m_plan_topologies[d], m_plan_blend_keys[d], m_plan_depth_modes[d]};
	}

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
	void SyncAllTruthToCpu();

	// Submit whatever the frame has planned so far, so the resident targets hold every draw up to
	// this point (counted as a mid-frame flush); a no-op when nothing is pending. Separately
	// callable because the flush RETIRES EVERY SYNCED CLAIM -- the ring slots those claims named
	// are spent, and their bytes were never in the CPU shadow -- so a caller whose question is
	// phrased in terms of `synced` has to ask it on the far side of this, never before.
	void FlushPendingPlan();

	// The rect union a run of draws has written into the current colour surface since the last
	// forced pass break: a DATE draw whose rect intersects it reads pixels the pass would also
	// write, so it opens a new pass (a fresh snapshot). Reset on surface change and on break.
	GSTileSurfaceId m_run_surface = kGSTileNoSurface;
	GSVector4i m_run_written = GSVector4i::zero();

	// --- the open pass, as accumulation sees it ----------------------------------------------
	// The executor runs a pass's prep ops before the pass opens, so a writeback emitted for the
	// draw being accumulated is hoisted over every draw of the open pass already planned. That
	// is what these track: what those draws have done, so the hoist can be tested instead of
	// assumed unsafe. A pass has exactly one colour and one depth surface (BuildAndExecutePlan
	// groups on the pair), so two write bitmaps cover every surface its draws can render into.
	// The read side is per RING SLOT, not per page: m_open_read says which pages the pass's
	// draws sample and m_open_read_slot which slot each of them read (entry index + 1), because
	// a page whose slot was superseded since is a different slot and rewriting it is invisible
	// to the earlier read. m_open_pass is the grouping key itself, the same type the plan build
	// groups on, so the two cannot drift; everything resets whenever a pass breaks.
	GSTileGpuPassKey m_open_pass;
	GSPageBitmap m_open_color_written;
	GSPageBitmap m_open_z_written;
	GSPageBitmap m_open_read;
	std::array<u16, GS_MAX_PAGES> m_open_read_slot{}; // meaningful only where m_open_read is set

	// The rule-2 targets the open pass's draws sample, in the order they were first bound -- which
	// IS the slot numbering the executor and the shader use, so it is rebuilt the same way at plan
	// build (asserted there). A pass binds at most kMaxTexSourcesPerPass of them; a draw whose
	// source would be one too many opens a pass of its own.
	std::array<GSTileSurfaceId, GSDevice::GSTileGpuPassPlan::kMaxTexSourcesPerPass> m_open_tex_src{};
	u32 m_open_tex_count = 0;

	// The open pass is closed: the next draw accumulated is the first of a new one, so nothing
	// recorded above constrains it. Idempotent, and the state it leaves matches no draw, so it
	// also serves as the initial and between-frames state.
	void BreakOpenPass();

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
		// block-exact coverage test caught: their planned writebacks do not reach every block, so
		// they take the S prefill instead of shipping executor zeros. Nonzero is not a fault --
		// it is the class the two rules can disagree on, made visible.
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
		//   refused  1 if the budget refused the class for this frame, so the mean is the fraction
		//            of frames it was off
		u32 class_wanted[kGSTileGpuAdmissionClasses] = {};
		u32 class_taxed[kGSTileGpuAdmissionClasses] = {};  // of those, standing behind another of the class
		u32 class_runs[kGSTileGpuAdmissionClasses] = {};   // ...and the declared runs they fall into
		u32 class_peak[kGSTileGpuAdmissionClasses] = {};   // the decayed cost peak the verdict was made on
		u32 class_refused[kGSTileGpuAdmissionClasses] = {};
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
		u32 flushes = 0;         // mid-frame plan submissions
		u32 passes = 0;
		// The texel-arm axis of the fragment variant (GSDevice::kGSTileGpuTexel*). A pass whose
		// byte-road draws all decode through one arm gets a program carrying that arm alone; one
		// that spans two carries both, and is the only case the shatter does not shrink all the way.
		// Watched rather than assumed: the mixed share is what says the axis still pays.
		u32 texel_passes = 0;       // passes with at least one byte-road draw
		u32 texel_mixed_passes = 0; // ...whose byte-road draws span more than one arm
		u32 texel_mixed_draws = 0;  // the byte-road draws inside those passes

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
	bool m_warned_lossy = false;
	bool m_warned_alias = false;

	// Set the first time BuildAndExecutePlan finds a non-empty plan and no executor to give it
	// to (the device failed the TileGpu contract at construction -- GSDeviceVK.cpp's
	// tilegpu_device_capable). Every draw is dropped whether this fires or not; it only gates
	// the one-time Console.Error that says so, so a contract-absent device cannot render black
	// with a clean exit code and no explanation.
	bool m_warned_no_executor = false;
};
