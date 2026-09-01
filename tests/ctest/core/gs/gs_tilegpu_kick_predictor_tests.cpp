// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu mid-frame kick's PER-FRAME CADENCE PREDICTOR (EmuCore/GS/TileGpuAdaptiveKick).
//
// The cadence half of the kick offers a submit every N render passes. Where the GPU is starving it
// is the largest single win this renderer has: Stuntman goes -33.3% on the SD865 and -38.7% on the
// RG477V. Where the GPU is not starving the submits are pure tax: Flatout 2 goes +17.2% on the
// SD865. And the same title wants OPPOSITE answers on the two devices -- Flatout 2 is -8.7% on the
// RG477V, off the same binary, same content, same ~500 render passes, same ~7 cadence submits a
// frame, zero blocking wait on both arms of both devices. Nothing structural separates those two
// runs. What separates them is what a mid-frame submit costs the recording thread, ~0.11 ms on
// Turnip against ~0.02 ms on the RG477V. So the decision is per frame, from a quantity measured on
// the device, and there is no vendor gate anywhere in it.
//
// ⚠️ THE TRAP THIS SHAPE EXISTS TO AVOID, and it is the reason the credit is a LATCHED PEAK rather
// than the frame's own reading. The cadence's job is to remove blocking wait. On a title where it
// works, the wait that justified it is GONE -- so a predictor that re-reads the wait every frame
// reads "no bubble here", switches itself off, watches the bubble return, switches back on, and
// oscillates on exactly the case it exists for. You cannot observe a bubble you have filled. The
// tests below pin that the latch prevents it.
//
// ⚠️ AND THE SECOND TRAP, which cost a round to find because it is invisible on every title above.
// ONE peak makes the credit the wait that EXISTS, and nothing in that reading distinguishes a wait
// the cadence would remove from a wait the cadence CAUSES. On Stuntman on the M2 it causes it:
// 69.74 ms of blocking wait a drawn frame with the cadence off, 121.52 ms with it on, +34.7 ms of
// wall. So the credit is TWO peaks, one per state, and what it is worth is their DIFFERENCE. Every
// case below is fitted to numbers where the off frame stalls more than the on frame, so the two
// readings agree there and the corpus alone cannot tell them apart -- kStuntmanM2 is the case that
// can, and it is the reason the difference is pinned in both directions.
//
// What is pinned here:
//
//  - AN EMPTY FRAME IS NOT EVIDENCE. A frame that opened no render pass gave the cadence no
//    opportunity: it does not vote, it does not clear the confirmation count, and above all it does
//    not decay the peak. The declaring budget learned this one the expensive way -- its peak
//    decayed on the empty frames a 30 Hz title presents every second vsync, fell under its own
//    re-admission line in ONE step, and re-admitted a class the counter read as 100% refused.
//  - HYSTERESIS. Entering the ON state needs the bubble worth twice the price; staying needs it
//    worth the price once, and either move needs two consecutive frames to ask for it.
//  - A CREDIT IS A DIFFERENCE, AND IT HAS TO BE MEASURED. A wait the cadence causes is not credited
//    to it, a spike that lands in both states cancels instead of licensing the lever, and a run
//    that has never had the cadence off spends the confirm window off to find out what one costs --
//    once at the start and once per decay period, which is the excursion budget the decay constant
//    was chosen for.
//  - BOUNDED OSCILLATION. A title whose bubble is invisible while the cadence runs spends the
//    overwhelming majority of its frames ON, not half of them: the peak decays at 1/256 a frame, so
//    the re-measurement excursion is the confirm window in ~180 frames rather than a flip-flop.
//  - AN UNPRICED DEVICE STAYS ON. Until a submit has been priced there is no evidence, and the
//    predictor may only move off the shipped arrangement on evidence.
//  - THE FIVE DEVICE CASES the design was fitted against reach the states they must reach, and stay
//    there.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

namespace
{
	constexpr u64 kMs = 1000000; // nanoseconds in a millisecond

	// The SD865's measured price for one mid-frame submit, as the suite of 2026-08-31 read it off
	// the arm the cadence shipped on -- the gs_cpu difference between the arms over the submits
	// taken. Turnip / Adreno 650 charges ~0.110 ms and the RG477V ~0.020 ms, and THAT is what makes
	// the same title decide differently. Used here only to build each title's frame bill; the
	// predictor itself never sees a per-submit number, it measures a rate per render pass.
	constexpr u64 kTaxSD865 = 110000;

	// A frame as the predictor sees it.
	struct Frame
	{
		u64 wait_ns;
		u32 submits;
		u64 cadence_ns;
		u32 passes;
	};

	// The frame a title presents while the cadence is ON, and the frame it presents while the
	// cadence is OFF. A title has both, and they are different frames -- that IS the problem.
	struct Title
	{
		Frame on;
		Frame off;
	};

	// Run `n` frames of a title, feeding whichever of its two frames matches the state the picker is
	// actually in. This is the harness the oscillation tests need: a picker that flips gets fed the
	// other world on the next frame, exactly as the device would feed it.
	GSTileGpuKickPolicyPicker RunTitle(u32 n, const Title& t, GSTileGpuKickPolicyPicker picker = {})
	{
		for (u32 i = 0; i < n; i++)
		{
			const Frame& f = picker.on ? t.on : t.off;
			picker.Observe(f.wait_ns, f.submits, f.cadence_ns, f.passes);
		}
		return picker;
	}

	// Count how many of `n` frames ran with the cadence on.
	u32 FramesOn(u32 n, const Title& t, GSTileGpuKickPolicyPicker picker = {})
	{
		u32 on = 0;
		for (u32 i = 0; i < n; i++)
		{
			on += picker.on ? 1u : 0u;
			const Frame& f = picker.on ? t.on : t.off;
			picker.Observe(f.wait_ns, f.submits, f.cadence_ns, f.passes);
		}
		return on;
	}

	// ---- the five device cases, in the numbers the suite of 2026-08-31 measured ----------------
	//
	// `wait_ns` is the blocking-wait total the census prints (sync + out-of-band + source-set wrap)
	// per drawn frame. `cadence_ns` on the ON frame is what the cadence's offers and submits cost the
	// GS thread, read off the arms' gs_cpu difference. ⚠️ An OFF frame carries submits 0 and cost 0
	// BY CONSTRUCTION -- the cadence did not run, so there is nothing to have cost anything. That is
	// what the device reports and it is why the counterfactual has to exist at all.

	// Stuntman on the SD865: 13.84 ms of wait SURVIVES the cadence -- the GPU is the bottleneck, so
	// the CPU still blocks on it -- against a 5.13 ms bill. The credit never even needs the latch
	// here. -33.3%.
	constexpr Title kStuntmanSD865 = {
		{14 * kMs, 45, 45 * kTaxSD865, 2264},
		{50 * kMs, 0, 0, 2264},
	};
	// Stuntman on the RG477V: the cadence drains the bubble to 1.88 ms, which is BELOW its own
	// 3.29 ms bill. Without the latch this title switches itself off. -38.7%.
	constexpr Title kStuntmanRG477V = {
		{1880000, 22, 3290000, 807},
		{25 * kMs, 0, 0, 807},
	};
	// Flatout 2 on the SD865: zero blocking wait on either arm and a 1.65 ms bill. +17.2%.
	constexpr Title kFlatout2SD865 = {
		{0, 8, 1650000, 511},
		{800000, 0, 0, 511},
	};
	// Flatout 2 on the RG477V: the same title, the same passes, the same submits -- and a bill of
	// 0.05 ms instead of 1.65, against 1.48 ms of wait the cadence removes. -8.7%.
	constexpr Title kFlatout2RG477V = {
		{0, 7, 50000, 413},
		{1480000, 0, 0, 413},
	};
	// R&C UYA effects on the SD865: zero blocking wait on BOTH arms -- the cadence removed every
	// wait the title had and still lost. +2.471 ms.
	constexpr Title kRcuyaSD865 = {
		{0, 6, 790000, 479},
		{0, 0, 0, 479},
	};
	// ...and on the RG477V, where the loss flipped its accuracy budget over.
	constexpr Title kRcuyaRG477V = {
		{0, 7, 100000, 438},
		{0, 0, 0, 438},
	};
	// ★ Stuntman on the M2 Max under Honeykrisp, the case that has the OPPOSITE sign to every one
	// above and the reason the credit is a difference. Measured 2026-08-31 with the cadence pinned
	// each way over the same dump: 69.74 ms of blocking wait a drawn frame with it OFF and 121.52
	// with it ON, wall 104.63 ms against 139.29, and 3.77 ms a drawn frame of GS-thread CPU for the
	// privilege. The lever does not drain a backlog here, it manufactures the stall. A model whose
	// credit is the wait that EXISTS reads the 121.52 as the bubble and pins the cadence on for the
	// run; the difference reads it as the cadence's own doing and refuses.
	constexpr Title kStuntmanM2 = {
		{121520000, 6, 3770000, 2066},
		{69740000, 0, 0, 2066},
	};
} // namespace

// ---------------------------------------------------------------------------------------------
// The starting position
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, StartsOnBecauseThatIsWhatShips)
{
	const GSTileGpuKickPolicyPicker picker;
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.frames, 0u);
	EXPECT_EQ(picker.switches, 0u);
	EXPECT_EQ(picker.BubbleNs(), 0u);
	EXPECT_EQ(picker.wait_off_ns, 0u);
	EXPECT_EQ(picker.wait_on_ns, 0u);
	EXPECT_EQ(picker.TaxNs(), 0u);
}

TEST(TileGpuKickPredictor, AnUnpricedDeviceStaysOn)
{
	// No submit has been priced, so there is no evidence, so the shipped arrangement stands --
	// however long the run goes and whatever the wait reads.
	GSTileGpuKickPolicyPicker picker;
	for (u32 i = 0; i < 100; i++)
		picker.Observe(0, 0, 0, 500);
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.switches, 0u);
	EXPECT_EQ(picker.TaxNs(), 0u);
}

// ---------------------------------------------------------------------------------------------
// The empty-frame rule -- AN EMPTY FRAME IS NOT EVIDENCE
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, AnEmptyFrameChangesNothingAtAll)
{
	GSTileGpuKickPolicyPicker picker;
	picker.Observe(9 * kMs, 4, 4 * kTaxSD865, 500); // one real frame, to have state
	const GSTileGpuKickPolicyPicker before = picker;

	// A frame that opened no render pass. Not the peak, not the verdict, not the census, not the
	// tax estimate -- nothing moves.
	EXPECT_TRUE(picker.Observe(50 * kMs, 99, 99 * kMs, 0));
	EXPECT_EQ(picker.frames, before.frames);
	EXPECT_EQ(picker.frames_on, before.frames_on);
	EXPECT_EQ(picker.wait_off_ns, before.wait_off_ns);
	EXPECT_EQ(picker.wait_on_ns, before.wait_on_ns);
	EXPECT_EQ(picker.confirming, before.confirming);
	EXPECT_EQ(picker.switches, before.switches);
	EXPECT_EQ(picker.tax_total_ns, before.tax_total_ns);
	EXPECT_EQ(picker.tax_passes, before.tax_passes);
	EXPECT_EQ(picker.submits_taken, before.submits_taken);
}

TEST(TileGpuKickPredictor, EmptyFramesDoNotDecayThePeak)
{
	// The declaring budget's trap, in this predictor's currency: a 30 Hz title presents an empty
	// frame every second vsync. If those decayed the peak it would fall twice as fast in wall time
	// as the design says, and the ON state would be given up on a schedule nobody chose.
	GSTileGpuKickPolicyPicker picker;
	picker.Observe(20 * kMs, 4, 4 * kTaxSD865, 500);
	const u64 peak_off = picker.wait_off_ns;
	const u64 peak_on = picker.wait_on_ns;
	for (u32 i = 0; i < 500; i++)
		picker.Observe(0, 0, 0, 0);
	EXPECT_EQ(picker.wait_off_ns, peak_off);
	EXPECT_EQ(picker.wait_on_ns, peak_on);
	EXPECT_EQ(picker.frames, 1u);
}

TEST(TileGpuKickPredictor, EmptyFramesDoNotClearTheConfirmationCount)
{
	// The depth predictor's version of the same trap: if an empty frame counted as AGREEMENT it
	// would clear `confirming`, and a title that alternates a drawn frame with an empty one would
	// never reach two consecutive votes -- so it could never switch at all.
	GSTileGpuKickPolicyPicker picker;
	picker.Observe(0, 6, 6 * kTaxSD865, 500); // asks to go off (bubble 0 < bill)
	EXPECT_EQ(picker.confirming, 1u);
	EXPECT_TRUE(picker.on);
	picker.Observe(0, 0, 0, 0); // an empty frame in between
	EXPECT_EQ(picker.confirming, 1u);
	picker.Observe(0, 6, 6 * kTaxSD865, 500); // the second consecutive vote
	EXPECT_FALSE(picker.on);
	EXPECT_EQ(picker.switches, 1u);
}

// ---------------------------------------------------------------------------------------------
// Hysteresis
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, OneAnomalousFrameDoesNotSwitch)
{
	// A load screen, a full-screen wipe. The confirmation count is what this answers; the band
	// below answers a metric sitting on the threshold.
	//
	// ⚠️ The off-state peak is seeded, because a credit is a DIFFERENCE and a picker that has never
	// run with the cadence off has not measured one. The frames themselves carry no wait: that is
	// the latch's own case, the cadence having filled the bubble it is being judged on.
	GSTileGpuKickPolicyPicker picker;
	picker.wait_off_ns = 20 * kMs;
	picker.Observe(0, 4, 4 * kTaxSD865, 500); // healthy: stay on
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.confirming, 0u);
	picker.Observe(0, 40, 40 * kMs, 500); // one wild frame asking for off
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.confirming, 1u);
	picker.Observe(0, 4, 4 * kTaxSD865, 500); // back to healthy
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.confirming, 0u);
	EXPECT_EQ(picker.switches, 0u);
}

TEST(TileGpuKickPredictor, EnteringOnCostsTwiceWhatStayingOnCosts)
{
	// The 2x band, in both directions, on the same numbers. A bubble worth 1.5x the price KEEPS the
	// cadence on but will not TURN it on.
	//
	// 320 passes at a measured 3125 ns a pass = 1.0 ms, the same debit in both states.
	constexpr u32 kPasses = 320;
	constexpr u64 kDebit = 1000000; // 1.0 ms
	const auto priced = [](bool on) {
		GSTileGpuKickPolicyPicker p;
		p.on = on;
		p.tax_total_ns = kDebit; // the rate: kDebit over kPasses passes
		p.tax_passes = kPasses;
		return p;
	};
	{
		// Staying: 1.5x the price is enough. The off-state peak carries the credit and the frames
		// themselves show no wait, which is the latch's own case -- the cadence has filled the
		// bubble it is being judged on.
		GSTileGpuKickPolicyPicker picker = priced(true);
		picker.wait_off_ns = (3 * kDebit) / 2;
		for (u32 i = 0; i < 8; i++)
			picker.Observe(0, 10, kDebit, kPasses);
		EXPECT_TRUE(picker.on);
	}
	{
		// Entering: 1.5x is NOT enough, it needs 2x.
		GSTileGpuKickPolicyPicker picker = priced(false);
		for (u32 i = 0; i < 8; i++)
			picker.Observe((3 * kDebit) / 2, 0, 0, kPasses);
		EXPECT_FALSE(picker.on);
	}
	{
		// ...and at 2x it goes on, after the confirm window and not before.
		GSTileGpuKickPolicyPicker picker = priced(false);
		picker.Observe(2 * kDebit, 0, 0, kPasses);
		EXPECT_FALSE(picker.on);
		picker.Observe(2 * kDebit, 0, 0, kPasses);
		EXPECT_TRUE(picker.on);
		EXPECT_EQ(picker.switches, 1u);
	}
}

// ---------------------------------------------------------------------------------------------
// Bounded oscillation -- the latch
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, TheLatchKeepsTheCadenceOnWhereItsOwnSuccessHidesTheBubble)
{
	// Flatout 2 on the RG477V. While the cadence runs the frame shows ZERO blocking wait -- the
	// cadence removed it -- so the frame's own reading says "back off". The latched peak, taken
	// from the frames where the cadence was off, is what keeps it on. -8.7% depends on this.
	//
	// Over 600 frames it must spend the overwhelming majority on, not half of them: a flip-flop
	// would land halfway between the two arms and give back half the win.
	const u32 on = FramesOn(600, kFlatout2RG477V);
	EXPECT_GT(on, 540u); // > 90%
	const GSTileGpuKickPolicyPicker picker = RunTitle(600, kFlatout2RG477V);
	EXPECT_TRUE(picker.on);
}

TEST(TileGpuKickPredictor, TheRemeasurementExcursionIsBoundedAndRare)
{
	// The other half of the same property: the peak MUST decay, or one scene latches the lever for
	// the rest of the run. So the predictor does spend frames off, re-measuring an undistorted
	// world -- it just has to be a small share. Stuntman on the RG477V is the case where the
	// cadence drains its own evidence to below its own bill.
	const u32 on = FramesOn(2000, kStuntmanRG477V);
	EXPECT_GT(on, 1900u); // > 95% -- the excursion is the confirm window, not a duty cycle
	EXPECT_LT(on, 2000u); // ...and it DOES re-measure; a peak that never decays is the other bug
}

TEST(TileGpuKickPredictor, ASettledOffTitleDoesNotChurn)
{
	// The mirror property. R&C UYA effects has zero blocking wait on BOTH arms, so once off there
	// is nothing to bring it back and the switch count must stop moving.
	const GSTileGpuKickPolicyPicker a = RunTitle(200, kRcuyaSD865);
	const GSTileGpuKickPolicyPicker b = RunTitle(2000, kRcuyaSD865);
	EXPECT_FALSE(a.on);
	EXPECT_FALSE(b.on);
	EXPECT_EQ(a.switches, b.switches);
	EXPECT_EQ(b.switches, 1u); // on -> off, once, and never again
}

// ---------------------------------------------------------------------------------------------
// The five device cases
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, StuntmanKeepsItsCadenceOnBothDevices)
{
	// The -33.3% and the -38.7%. On the SD865 a large wait survives the cadence and on the RG477V
	// it does not; either way the OFF frames stall far more than the ON frames, which is what the
	// credit reads.
	//
	// ⚠️ 97.5% rather than the 99% this pinned before 2026-08-31, and the missing frames are a
	// MEASUREMENT and not churn. A credit is the difference between the two states, so a run that
	// has never had the cadence off has not measured one; the predictor spends the confirm window
	// off, once at the start and once per decay period after, and that is the only way the
	// difference can be known at all. It is inside the excursion budget the decay constant is
	// chosen for -- the confirm window in ~180 frames, under 2.5%.
	EXPECT_GT(FramesOn(600, kStuntmanSD865), 585u);  // > 97.5%
	EXPECT_GT(FramesOn(600, kStuntmanRG477V), 570u); // > 95%
}

TEST(TileGpuKickPredictor, FlatoutDecidesOppositeWaysOnTheTwoDevicesFromOneBinary)
{
	// ★ The case the predictor exists for. Same title, same content, same ~500 render passes, same
	// ~7 cadence submits a frame, zero blocking wait on both. The ONLY input that differs is what a
	// submit costs -- 1.65 ms of GS-thread time on the SD865 against 0.05 ms on the RG477V -- and
	// that one number has to carry a +17.2% and a -8.7% to opposite verdicts.
	const u32 sd = FramesOn(600, kFlatout2SD865);
	const u32 rg = FramesOn(600, kFlatout2RG477V);
	EXPECT_LT(sd, 30u);  // SD865: off, and stays off
	EXPECT_GT(rg, 540u); // RG477V: on, and stays on
}

TEST(TileGpuKickPredictor, RcuyaEffectsBacksOffOnBothDevices)
{
	// The title that gave the cadence back every blocking wait it had -- 12 waits to 0 -- and lost
	// anyway, because the per-submit tax outweighed a bubble that was never there. Zero wait on
	// both arms is what makes this one settle rather than probe.
	EXPECT_LT(FramesOn(600, kRcuyaSD865), 30u);
	EXPECT_LT(FramesOn(600, kRcuyaRG477V), 30u);
}

// ---------------------------------------------------------------------------------------------
// The census
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, TheRateIsMeasuredPerRenderPassOverTheFramesTheCadenceRanIn)
{
	// ⚠️ PER PASS, not per submit, and it cost a census round to learn why. The cadence's bill is
	// its OFFERS, one at every pass tail past the cadence -- Spider-Man 3 on the M2 offers 380 a
	// frame and the fence gate takes 19. Divided by the submits, that whole offer bill lands on the
	// few that went and the unit price comes out ~20x high; the off state then multiplies it by
	// `render_passes / cadence`, over-counting the submits by the same ~20x. The two states priced
	// the same frame differently and the predictor flipped between them 244 times in 1,548 frames.
	GSTileGpuKickPolicyPicker picker;
	picker.Observe(0, 4, 400000, 200);
	picker.Observe(0, 6, 200000, 400);
	EXPECT_EQ(picker.tax_passes, 600u);
	EXPECT_EQ(picker.tax_total_ns, 600000u);
	EXPECT_EQ(picker.TaxNs(), 1000u); // 1 us a render pass
	EXPECT_EQ(picker.submits_taken, 10u); // census only -- nothing is decided on this
}

TEST(TileGpuKickPredictor, AnOffFramesPassesDoNotDiluteTheMeasuredRate)
{
	// The rate has to be measured only where the cadence RAN. Counting an off frame's passes in the
	// denominator would make the lever look cheaper the longer it stays off -- which is precisely
	// the feedback that switches it back on for no reason and then straight off again.
	GSTileGpuKickPolicyPicker picker;
	picker.Observe(0, 4, 400000, 400); // on: 1000 ns a pass
	EXPECT_EQ(picker.TaxNs(), 1000u);
	picker.on = false;
	for (u32 i = 0; i < 20; i++)
		picker.Observe(0, 0, 0, 400);
	EXPECT_EQ(picker.tax_passes, 400u);
	EXPECT_EQ(picker.TaxNs(), 1000u);
}

TEST(TileGpuKickPredictor, ACadenceThatNeverFiresChargesNothingAndTheStateStands)
{
	// The cadence key at zero, or set past any frame's pass count: no offer, no cost, no rate, so
	// the debit is zero and the shipped arrangement stands. A predictor that divided by the cadence
	// instead of measuring would have to special-case this; measuring makes it fall out.
	GSTileGpuKickPolicyPicker picker;
	picker.on = false;
	for (u32 i = 0; i < 10; i++)
		picker.Observe(0, 0, 0, 500);
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.TaxNs(), 0u);
}

// ---------------------------------------------------------------------------------------------
// The credit is a DIFFERENCE -- what the cadence removes, not what the frame stalls for
// ---------------------------------------------------------------------------------------------

TEST(TileGpuKickPredictor, AWaitTheCadenceCausesIsNotCreditedToTheCadence)
{
	// ★ The M2's Stuntman. Every other case in this file has the off frame stalling more than the
	// on frame, so a credit taken from either reads roughly the same; this one has it the other way
	// round, and the two readings differ by the whole verdict. Pinning both directions is what says
	// the difference is doing the work rather than a threshold that happens to fit.
	EXPECT_LT(FramesOn(600, kStuntmanM2), 30u); // off, and it stays off
	const GSTileGpuKickPolicyPicker picker = RunTitle(600, kStuntmanM2);
	EXPECT_FALSE(picker.on);
	// ...and the reason, stated in the numbers the decision actually used: the on-state peak is the
	// larger of the two, so there is no wait to credit the lever with at all.
	EXPECT_GT(picker.wait_on_ns, picker.wait_off_ns);
	EXPECT_EQ(picker.BubbleNs(), 0u);
}

TEST(TileGpuKickPredictor, ASpikeSeenInBothStatesCancelsInsteadOfLatchingTheLeverOn)
{
	// Spider-Man 3 on the M2. A periodic stall that has nothing to do with the cadence lands in
	// whichever state the frame happened to run in, and over a run it lands in both. Under one peak
	// it was pure credit -- a single 79.4 ms frame licensed the cadence for the twenty-two that
	// followed, whose own waits were 0-39 ms -- and the frame-clock fix of 2026-08-31 stretched that
	// licence from about two frames to about a hundred and seventy-seven. Under two it cancels.
	//
	// The bill is 12 ms a frame against a spike of 79.4 every twelfth frame, so a model that
	// credits the spike says ON and a model that cancels it says OFF.
	u32 on = 0;
	GSTileGpuKickPolicyPicker picker;
	for (u32 i = 0; i < 600; i++)
	{
		on += picker.on ? 1u : 0u;
		const u64 wait = (i % 12) == 0 ? 79400000 : 0;
		if (picker.on)
			picker.Observe(wait, 5, 12 * kMs, 1146);
		else
			picker.Observe(wait, 0, 0, 1146);
	}
	EXPECT_LT(on, 120u); // under a fifth of the run, against ~all of it under one peak
}

TEST(TileGpuKickPredictor, TheOffStateHasToBeMeasuredOnceBeforeAnyCreditExists)
{
	// The startup excursion, pinned so it stays BOUNDED rather than becoming a duty cycle. A run
	// begins with the cadence on and no measurement of what a frame costs without it, so the credit
	// is not zero, it is unknown -- and the only way to learn it is to spend the confirm window off.
	// Stuntman on the SD865 is the case where the old single peak never had to: it read the 14 ms
	// that SURVIVES the cadence as the bubble and stayed on from frame one.
	GSTileGpuKickPolicyPicker picker;
	for (u32 i = 0; i < 8; i++)
	{
		const Frame& f = picker.on ? kStuntmanSD865.on : kStuntmanSD865.off;
		picker.Observe(f.wait_ns, f.submits, f.cadence_ns, f.passes);
	}
	// It went off to look, found the 50 ms, and came straight back.
	EXPECT_TRUE(picker.on);
	EXPECT_EQ(picker.switches, 2u);
	EXPECT_GT(picker.wait_off_ns, 48 * kMs);  // the off world, measured, less a few frames of decay
	EXPECT_LE(picker.wait_off_ns, 50 * kMs);
	EXPECT_GT(picker.BubbleNs(), 30 * kMs); // ...and now there is a real credit
}
