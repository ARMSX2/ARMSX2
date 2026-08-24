// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu SPECIALIZATION BIND GUARD: a budget on how many pipeline binds one plan may add by
// freezing per-draw GS state into fragment programs, and the withdrawal it performs when the plan
// goes over.
//
// Why it exists. Freezing a draw's own alpha comparison, fog, texture function, wrap modes, filter,
// coordinate kind, DATE and TEXA into its fragment program makes the program much smaller -- 247 of
// the corpus's 251 distinct variants at eight full registers or fewer on an Adreno 650, so wave128,
// against two of seventeen unspecialized. It also makes the program part of the indirect-run key, so
// two consecutive draws differing only in their alpha comparison stop being one run and cost a
// pipeline bind. On an SD865 (2026-08-24) that trade wins Shadow of the Colossus -20.5% and Gran
// Turismo 4 -7.6% with GPU time falling, and LOSES both Ratchet & Clank scenes, +9.2% and +8.4%,
// with GPU time RISING (+59% on the gameplay scene).
//
// What separates them, measured: not the bind total -- Shadow of the Colossus binds 763 a frame and
// wins where Ratchet's gameplay scene binds 905 and loses -- but how many of those binds the
// freezing ADDED over what the per-draw road narrowing already required. 141 and 150 a frame on the
// winners, 525 and 859 on the losers, and 72 or below across the other fourteen corpus dumps.
//
// ⚠️ And what does NOT separate them, because the first hypothesis was that an Adreno program change
// is an instruction-RAM DMA and the a650's cache holds 127 instrlen units. Compiled offline against
// the device's own Turnip, the Ratchet gameplay scene's busiest pass shape alternates among five
// programs totalling 32 units -- a quarter of the cache -- while Shadow of the Colossus, the biggest
// winner, runs 110-to-182-unit working sets and streams 2.6x the program bytes a frame. Working set,
// program size and over-cache draw share all rank the wrong way round. The cost tracks how OFTEN the
// program changes, so the budget counts binds.
//
// What is pinned here, and why each one:
//
//  - The THREE-STATE setting, like the pass cap's: "ask the device" and "force the guard off" are
//    different instructions, and only the second gives an Adreno the unguarded arm the deciding A/B
//    needs.
//  - The BOUNDARY, at exactly the budget and not one bind either side.
//  - That the withheld form is the DE-SPECIALIZED key and not the pass UNION -- over the whole
//    product of the ten frozen axes. This is the correctness claim the guard rests on: the key it
//    writes is bit-for-bit the one EmuCore/GS/TileGpuUnspecializedFragmentVariant writes, which is
//    the arm gated byte-identical over the corpus. It is also the better trade, and the test says
//    why by pinning that the road half survives: the union would widen it back to the pass's, which
//    on the Ratchet gameplay scene puts 1,499 draws a frame back on a byte decoder they never enter.
//  - That a pass adding NO bind keeps its specialization -- free wave128, nothing saved by taking
//    it -- and, following from that, that a guarded plan's bind total is EXACTLY the unspecialized
//    arm's whichever branch each pass took.
//  - That runs actually MERGE, driven through the planner's own gsTileGpuRunCount rather than a
//    re-implementation of it, and that de-specializing does not merge across a cut the topology,
//    blend or depth mode made.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
using Plan = GSDevice::GSTileGpuPassPlan;
using Spec = GSDevice::GSTileGpuFragmentSpec;

/// The device answers a planner consults, named rather than spelt as bare numbers so the setting
/// tests read as the questions they are.
constexpr u32 kDeviceNoGuard = 0;
constexpr u32 kDeviceAdreno = 300;

/// The unguarded spelling, which is 0 and not "no value": every site takes 0 to mean no guard.
constexpr u32 kNoGuard = 0;

/// The row's own alpha-test encoding: 0 = no test, else TEST.ATST + 1, and only LESS..NOTEQUAL
/// (3..8) can reach the fragment stage -- NEVER and ALWAYS are folded into the write flags.
constexpr u8 kAtst[7] = {0, 3, 4, 5, 6, 7, 8};

/// One specialized key over a chosen road, so a test can say "the same draw, with this state frozen".
u32 SpecializedKey(u32 road, u32 texel, u32 self, bool q, u8 atst, u8 fge = 0, u8 tfx = 0)
{
	Spec s;
	s.valid = true;
	s.atst = atst;
	s.fge = fge;
	s.tfx = tfx;
	return Plan::PackVariantKey(road, texel, self, q, s);
}

/// A run count over a literal key list, with no other cut -- the planner's own function, driven the
/// way the planner drives it.
u32 Runs(const std::vector<u32>& keys)
{
	return gsTileGpuRunCount(
		0, static_cast<u32>(keys.size()), [&](u32 d) { return keys[d]; }, [](u32) { return false; });
}

/// ...and the same list de-specialized, which is what the guard leaves behind.
std::vector<u32> Despecialized(const std::vector<u32>& keys)
{
	std::vector<u32> out;
	out.reserve(keys.size());
	for (const u32 k : keys)
		out.push_back(gsTileGpuDespecializeVariantKey(k));
	return out;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The setting: three states, because two cannot ask for the unguarded arm on a device that guards.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuSpecGuard, ZeroAsksTheDevice)
{
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(0, kDeviceAdreno), kDeviceAdreno);
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(0, kDeviceNoGuard), kNoGuard);
}

TEST(TileGpuSpecGuard, NegativeForcesTheGuardOffOverTheDevice)
{
	// The arm the device round cannot otherwise ask for: an Adreno with the guard off.
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(-1, kDeviceAdreno), kNoGuard);
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(-4096, kDeviceAdreno), kNoGuard);
}

TEST(TileGpuSpecGuard, PositivePinsThatBudgetOnAnyDevice)
{
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(64, kDeviceNoGuard), 64u);
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(1, kDeviceAdreno), 1u);
	// A pinned budget equal to the device's own is still a pin, and reads the same.
	EXPECT_EQ(gsTileGpuMaxSpecializationBinds(300, kDeviceAdreno), kDeviceAdreno);
}

// ---------------------------------------------------------------------------------------------
// The boundary.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuSpecGuard, ABudgetOfZeroNeverGuards)
{
	// 0 is "no guard", not "guard at zero binds" -- the difference between every non-Adreno device
	// keeping its wave size and every non-Adreno device losing it.
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(0, kNoGuard));
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(1, kNoGuard));
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(1000000, kNoGuard));
}

TEST(TileGpuSpecGuard, TheBudgetIsCrossedOnlyAboveIt)
{
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(299, kDeviceAdreno));
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(300, kDeviceAdreno));
	EXPECT_TRUE(gsTileGpuGuardsSpecialization(301, kDeviceAdreno));
}

TEST(TileGpuSpecGuard, TheMeasuredCorpusFallsOnTheSidesItWasMeasuredOn)
{
	// The four device-measured titles at the shipped budget, so a change to the number has to argue
	// with the round that chose it rather than merely recompiling. Added binds a frame, SD865
	// 2026-08-24, specialized against the same binary's unspecialized arm.
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(141, kDeviceAdreno)); // sotc, -20.5%: leave it alone
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(150, kDeviceAdreno)); // gt4, -7.6%: leave it alone
	EXPECT_TRUE(gsTileGpuGuardsSpecialization(525, kDeviceAdreno));  // rcuya-effects, +8.4%
	EXPECT_TRUE(gsTileGpuGuardsSpecialization(859, kDeviceAdreno));  // rcuya-gameplay, +9.2%
	// ...and the loudest dump that is neither, which must also be left alone.
	EXPECT_FALSE(gsTileGpuGuardsSpecialization(72, kDeviceAdreno)); // gt4opb
}

// ---------------------------------------------------------------------------------------------
// What the guard writes: the de-specialized key, over the whole product of the frozen axes.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuSpecGuard, DeSpecializingIsExactlyTheKeyTheUnspecializedArmWrites)
{
	// The correctness claim the guard rests on, checked over all 43,008 frozen states rather than a
	// sample: whatever is frozen, withholding it lands on the 4-argument packer's own output. That is
	// the key EmuCore/GS/TileGpuUnspecializedFragmentVariant writes, and that arm was gated
	// byte-identical against the specialized one over the whole corpus. So the guard never picks a
	// program anything has to reason about; it picks one already proved to render the same pixels.
	//
	// Swept over four road/texel/self/quantise combinations too, because the point is that the road
	// half survives the transform untouched.
	struct Road
	{
		u32 road, texel, self;
		bool q;
	};
	static constexpr Road kRoads[4] = {
		{0, 0, 0, false},
		{GSDevice::kGSTileGpuRoadSource, 0, 0, false},
		{GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, true},
		{GSDevice::kGSTileGpuRoadByte | GSDevice::kGSTileGpuRoadTarget,
			GSDevice::kGSTileGpuTexelIndex4 | GSDevice::kGSTileGpuTexelPalGather,
			GSDevice::kGSTileGpuSelfBlend | GSDevice::kGSTileGpuSelfMask, false},
	};
	for (const Road& r : kRoads)
	{
		const u32 bare = Plan::PackVariantKey(r.road, r.texel, r.self, r.q);
		for (const u8 fst : {0, 1})
			for (const u8 ltf : {0, 1})
				for (const u8 tfx : {0, 1, 2, 3})
					for (const u8 tcc : {0, 1})
						for (const u8 atst : kAtst)
							for (const u8 fge : {0, 1})
								for (const u8 date : {0, 1, 2})
									for (const u8 wms : {0, 1, 2, 3})
										for (const u8 wmt : {0, 1, 2, 3})
											for (const u8 texa : {0, 1, 2, 3})
											{
												Spec s;
												s.valid = true;
												s.fst = fst;
												s.ltf = ltf;
												s.tfx = tfx;
												s.tcc = tcc;
												s.atst = atst;
												s.fge = fge;
												s.date = date;
												s.wms = wms;
												s.wmt = wmt;
												s.texa = texa;
												const u32 spec = Plan::PackVariantKey(r.road, r.texel, r.self, r.q, s);
												ASSERT_EQ(gsTileGpuDespecializeVariantKey(spec), bare);
											}
	}
}

TEST(TileGpuSpecGuard, DeSpecializingKeepsTheDrawsOwnRoadAndNotItsPassUnion)
{
	// The deviation from the obvious design, pinned. Writing the pass's UNION key would also be
	// pixel-inert -- it is what a plan carrying no variant stream falls back to -- and it would be a
	// much worse trade, because the union widens the road half back to whatever the pass's loudest
	// draw needed. On the Ratchet gameplay scene that is 1,499 draws a frame put back on a byte
	// decoder they never enter, and its draw-weighted program from 2,684 SPIR-V words to 11,000.
	const u32 draw = SpecializedKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false, 6);
	const u32 pass_union = Plan::PackVariantKey(
		GSDevice::kGSTileGpuRoadSource | GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, false);
	const u32 guarded = gsTileGpuDespecializeVariantKey(draw);
	EXPECT_NE(guarded, pass_union);
	EXPECT_EQ(Plan::VariantRoadMask(guarded), GSDevice::kGSTileGpuRoadSource);
	EXPECT_EQ(Plan::VariantTexelMask(guarded), 0u);
	EXPECT_FALSE(Plan::VariantSpec(guarded).valid);
}

TEST(TileGpuSpecGuard, DeSpecializingAnAlreadyUnspecializedKeyChangesNothing)
{
	// Idempotent, because the guard runs over keys the planner may already have written bare -- an
	// untextured draw on a device that freezes nothing, or a second guarded plan in the same frame.
	for (const u32 road : {0u, GSDevice::kGSTileGpuRoadSource, GSDevice::kGSTileGpuRoadByte})
	{
		const u32 bare = Plan::PackVariantKey(road, 0, 0, false);
		EXPECT_EQ(gsTileGpuDespecializeVariantKey(bare), bare);
		EXPECT_EQ(gsTileGpuDespecializeVariantKey(gsTileGpuDespecializeVariantKey(bare)), bare);
	}
}

// ---------------------------------------------------------------------------------------------
// Which passes the guard acts on, and what that buys.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuSpecGuard, APassWhoseFrozenStateCutNothingKeepsIt)
{
	// Free wave128: the program is smaller and no bind was spent on it. Taking it away would give up
	// the wave size and save nothing at all.
	EXPECT_FALSE(gsTileGpuDespecializesPass(1, 1));
	EXPECT_FALSE(gsTileGpuDespecializesPass(17, 17));
	EXPECT_TRUE(gsTileGpuDespecializesPass(2, 1));
	EXPECT_TRUE(gsTileGpuDespecializesPass(41, 1));
}

TEST(TileGpuSpecGuard, AGuardedPlansBindTotalIsExactlyTheUnspecializedArms)
{
	// Follows from the rule above and is worth pinning, because it is the claim the device round will
	// be read against: the guard does not approximate the unspecialized arm's submission, it lands on
	// it exactly, while keeping the smaller program everywhere it was free.
	struct P
	{
		u32 runs, runs_despec;
	};
	// Ratchet's gameplay scene as the M2 census measures it in the device's pass shape: a frame of
	// enormous alternating passes -- 64 draws, one road, forty-odd binds, one bind unspecialized --
	// plus a tail of small passes the freezing never cut. Around 31 passes and 859 added binds.
	std::vector<P> passes;
	for (u32 n = 0; n < 6; n++)
		passes.push_back(P{41, 1});
	for (u32 n = 0; n < 3; n++)
		passes.push_back(P{24, 1});
	for (u32 n = 0; n < 4; n++)
		passes.push_back(P{33, 1});
	for (u32 n = 0; n < 3; n++)
		passes.push_back(P{50, 1});
	for (u32 n = 0; n < 5; n++)
		passes.push_back(P{47, 1});
	for (u32 n = 0; n < 3; n++)
		passes.push_back(P{38, 1});
	for (u32 n = 0; n < 15; n++)
		passes.push_back(P{n % 3 + 1, n % 3 + 1}); // the tail: nothing was cut, nothing is withheld
	u32 submitted = 0, despec = 0, added = 0;
	for (const P& p : passes)
	{
		despec += p.runs_despec;
		added += p.runs - p.runs_despec;
	}
	ASSERT_GT(added, 800u); // the scene's own scale, so the budget test below is the real one
	ASSERT_TRUE(gsTileGpuGuardsSpecialization(added, kDeviceAdreno));
	for (const P& p : passes)
		submitted += gsTileGpuDespecializesPass(p.runs, p.runs_despec) ? p.runs_despec : p.runs;
	EXPECT_EQ(submitted, despec);
	// ...and the tail really did keep its specialization: fewer passes were touched than exist.
	u32 touched = 0;
	for (const P& p : passes)
		touched += gsTileGpuDespecializesPass(p.runs, p.runs_despec) ? 1u : 0u;
	EXPECT_EQ(touched, 24u);
	EXPECT_LT(touched, passes.size());
}

// ---------------------------------------------------------------------------------------------
// The merge itself, through the planner's own run cut.
// ---------------------------------------------------------------------------------------------

TEST(TileGpuSpecGuard, AlternatingFrozenStateShattersTheRunAndGuardingMergesIt)
{
	// Eight draws on one road, alternating between two alpha comparisons -- the exact shape the
	// Ratchet passes take. Specialized that is eight runs and eight pipeline binds; guarded it is one.
	std::vector<u32> keys;
	for (u32 n = 0; n < 4; n++)
	{
		keys.push_back(SpecializedKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false, 6));
		keys.push_back(SpecializedKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false, 7));
	}
	EXPECT_EQ(Runs(keys), 8u);
	EXPECT_EQ(Runs(Despecialized(keys)), 1u);
}

TEST(TileGpuSpecGuard, EveryFrozenAxisOnItsOwnShattersTheRunAndGuardingMergesIt)
{
	// One axis at a time, because the guard has to undo all ten and a key half-cleared would merge
	// some passes and not others -- which reads as a partial win rather than as a bug.
	Spec base;
	base.valid = true;
	const auto pair = [](const Spec& a, const Spec& b) {
		return std::vector<u32>{Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte,
									GSDevice::kGSTileGpuTexelIndex8, 0, false, a),
			Plan::PackVariantKey(GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, false, b)};
	};
	std::vector<Spec> others;
	for (u32 axis = 0; axis < 10; axis++)
	{
		Spec o = base;
		switch (axis)
		{
			case 0: o.fst = 1; break;
			case 1: o.ltf = 1; break;
			case 2: o.tfx = 2; break;
			case 3: o.tcc = 1; break;
			case 4: o.atst = 6; break;
			case 5: o.fge = 1; break;
			case 6: o.date = 1; break;
			case 7: o.wms = 3; break;
			case 8: o.wmt = 3; break;
			case 9: o.texa = 3; break;
			default: break;
		}
		others.push_back(o);
	}
	for (const Spec& o : others)
	{
		const std::vector<u32> keys = pair(base, o);
		ASSERT_EQ(Runs(keys), 2u);
		ASSERT_EQ(Runs(Despecialized(keys)), 1u);
	}
	// ...and the presence flag itself: a specialized key never merges with an unspecialized one until
	// the guard makes them both bare.
	Spec none;
	const std::vector<u32> mixed = pair(base, none);
	EXPECT_EQ(Runs(mixed), 2u);
	EXPECT_EQ(Runs(Despecialized(mixed)), 1u);
}

TEST(TileGpuSpecGuard, GuardingDoesNotMergeAcrossTheRestOfTheRunKey)
{
	// The guard withdraws ONE axis of the run key. A cut the topology, the blend or the depth mode
	// made is still a cut, and merging across one would bind the wrong pipeline for half the draws.
	const std::vector<u32> keys(6, SpecializedKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false, 6));
	const std::vector<u32> bare = Despecialized(keys);
	// A blend change at draw 3 and nowhere else.
	const auto other_cut = [](u32 d) { return d == 3; };
	EXPECT_EQ(gsTileGpuRunCount(
				  0, 6, [&](u32 d) { return bare[d]; }, other_cut),
		2u);
	// And with nothing else cutting, the same six draws are one run.
	EXPECT_EQ(Runs(bare), 1u);
}

TEST(TileGpuSpecGuard, GuardingDoesNotMergeDrawsOnDifferentRoads)
{
	// The road half is what the guard must NOT touch, so two draws whose roads differ stay two runs
	// however much frozen state they shed. This is the union design's failure, pinned as a
	// non-failure of this one.
	std::vector<u32> keys;
	keys.push_back(SpecializedKey(GSDevice::kGSTileGpuRoadSource, 0, 0, false, 6));
	keys.push_back(SpecializedKey(GSDevice::kGSTileGpuRoadByte, GSDevice::kGSTileGpuTexelIndex8, 0, false, 6));
	EXPECT_EQ(Runs(keys), 2u);
	EXPECT_EQ(Runs(Despecialized(keys)), 2u);
}

TEST(TileGpuSpecGuard, TheRunCountIsTheOneThePlannerSubmits)
{
	// gsTileGpuRunCount is what both the guard's measurement and the executor's own cut are about, so
	// pin the plain properties rather than leaving them to the callers: a single draw is one run, an
	// empty stretch is none, and a constant key over N draws is one run whatever N is.
	EXPECT_EQ(gsTileGpuRunCount(
				  0, 0, [](u32) { return 0u; }, [](u32) { return false; }),
		0u);
	EXPECT_EQ(gsTileGpuRunCount(
				  0, 1, [](u32) { return 7u; }, [](u32) { return false; }),
		1u);
	EXPECT_EQ(gsTileGpuRunCount(
				  0, 64, [](u32) { return 7u; }, [](u32) { return false; }),
		1u);
	// A run count taken over a middle stretch starts a run at its own first draw, because a pass
	// boundary is a pipeline bind whatever the keys either side of it say.
	EXPECT_EQ(gsTileGpuRunCount(
				  10, 20, [](u32) { return 7u; }, [](u32) { return false; }),
		1u);
}
