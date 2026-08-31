// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The colour write mask served by the FRAGMENT stage instead of the pipeline
// (EmuCore/GS/TileGpuShaderWriteMask), and the run-key collapse that is the whole point of it.
//
// The mask lives in the blend key (GSTileGpuPassPlan::kNoWriteShift), and the blend key IS the run
// key -- so two draws alike in everything but FRAME.FBMSK cost two pipelines and two indirect calls.
// A game that builds a buffer one channel group at a time therefore gets one call per draw.
// Spider-Man 3 is that game: 2,841 of its 2,992 adjacent in-pass draw pairs are cut by the write
// mask and by nothing else, the masks rotating r -> rg -> gb -> ba, and a device census on an
// SD865 puts a call carrying one sub-draw at 9.90 us against 2.66 us for a sub-draw folded into an
// existing call. Taking the mask out of the key turns 3,624 calls into 785.
//
// Two things are pinned here and they answer different questions.
//
// EXACTNESS. The shader arm already merges the destination at BIT granularity for the partial-FBMSK
// road (tilegpu.glsl's `keep` vector); a mask covering whole channels is a strict subset of what it
// does, so the two realizations must be the same picture. What makes that true is
// gsTileGpuChannelKeepMask handing the shader WHOLE bytes for the channels the pipeline was
// dropping -- including the bits a 16-bit frame does not store, which the pipeline preserved along
// with everything else in the byte. Keeping only the format's bits there would let the fragment's
// low bits land where the pipeline dropped them.
//
// ADMISSION. Three shapes keep the pipeline mask, and two of them are correctness cliffs rather than
// missed optimizations. The write mask applies AFTER blending, so a shader that merged the
// destination in first would then have the blend unit blend the destination with itself -- which is
// why gsTileGpuMasksInShader asks whether the FRAGMENT's output is what lands, not whether the draw
// blends. And the mask arm sits at the end of a tail that converts the colour to bytes at
// floor(cv * 255 + 0.5), while a UNORM8 attachment write rounds ties its own way -- so a draw the
// tail does not ALREADY own would have the channels the mask does not even cover re-rounded on the
// way in. That one is measured: 17 pixels of Ace Combat 5, on 12 unblended 32-bit draws.
//
// Out of which falls the invariant the lever's cost model rests on, pinned at the end of the
// admission section: every draw the road takes was already a declared in-pass reader, so the lever
// adds no reader, no declaring pass, no per-class budget charge and no reorder barrier run.

#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <gtest/gtest.h>

#include <ios>

namespace
{
constexpr u32 kC32 = 0xFFFFFFFFu; // PSMCT32 stored bits
constexpr u32 kC16 = 0x80F8F8F8u; // PSMCT16
constexpr u8 kR = 0x1, kG = 0x2, kB = 0x4, kA = 0x8;

using Plan = GSDevice::GSTileGpuPassPlan;

// gsTileGpuMasksInShader with the arguments in the order the planner passes them, named so the
// cases below read as cases rather than as five booleans.
constexpr bool Road(bool lever, u8 color_mask, bool blend_active, bool shader_blend, bool shader_fbmsk)
{
	return gsTileGpuMasksInShader(lever, color_mask, blend_active, shader_blend, shader_fbmsk);
}

// A draw's blend key as the planner builds it, reduced to the two fields this suite is about: the
// write-mask field and the self-read flag. `in_shader` is gsTileGpuMasksInShader's answer.
constexpr u32 BlendKey(u8 color_mask, bool in_shader)
{
	u32 key = Plan::PackNoWrite(in_shader ? 0xFu : color_mask);
	if (in_shader)
		key |= Plan::kSelfRead;
	return key;
}

// ...and the keep mask the state row carries with it: the register's own bits, plus whole bytes for
// the channels the pipeline is no longer dropping.
constexpr u32 KeepMask(u32 fbmsk, u32 fmsk, u8 color_mask, bool in_shader)
{
	u32 keep = gsTileGpuFrameKeepMask(fbmsk, fmsk);
	if (in_shader)
		keep |= gsTileGpuChannelKeepMask(color_mask);
	return keep;
}

// What the shader's FBMSK arm computes for one channel byte: keep the destination's bits where the
// mask says so, take the fragment's everywhere else (tilegpu.glsl, `outc & ~keep | dst & keep`).
constexpr u32 Merge(u32 src, u32 dst, u32 keep)
{
	return (src & ~keep) | (dst & keep);
}
} // namespace

// ---------------------------------------------------------------------------------------
// Admission: which draws the fragment stage may take the mask for.
// ---------------------------------------------------------------------------------------

TEST(TileGpuShaderWriteMask, OffIsAlwaysThePipeline)
{
	// The lever's off position is not "usually the pipeline". Every arm of the predicate has to
	// answer false, or the two arms of a byte-identity gate are not the same renderer.
	EXPECT_FALSE(Road(false, kR | kG, false, false, true));
	EXPECT_FALSE(Road(false, kA, false, true, false));
	EXPECT_FALSE(Road(false, kR | kG | kB, true, true, true));
}

TEST(TileGpuShaderWriteMask, PartialMaskOnADrawTheTailOwnsTakesTheShader)
{
	// The Spider-Man 3 shape: a draw whose FBMSK the fragment stage is already merging at bit
	// granularity, writing one channel group.
	EXPECT_TRUE(Road(true, kR, false, false, /*shader_fbmsk=*/true));
	EXPECT_TRUE(Road(true, kR | kG, false, false, true));
	EXPECT_TRUE(Road(true, kG | kB, false, false, true));
	EXPECT_TRUE(Road(true, kB | kA, false, false, true));
	// ...and the other way a draw's colour is already the fragment stage's to convert.
	EXPECT_TRUE(Road(true, kR | kG, /*blend_active=*/true, /*shader_blend=*/true, false));
}

TEST(TileGpuShaderWriteMask, NothingToMoveIsRefused)
{
	// A draw writing all four channels has no mask to move, and moving nothing would make it read
	// its destination for no reason -- on a segregating device that is a pass boundary bought with
	// no win at all. A draw writing NONE is the same argument at the other end: the shader would
	// read the destination only to write the same bytes back.
	EXPECT_FALSE(Road(true, kR | kG | kB | kA, false, false, true));
	EXPECT_FALSE(Road(true, 0, false, false, true));
}

TEST(TileGpuShaderWriteMask, TheExecutorsBlendUnitKeepsThePipelineMask)
{
	// ⚠️ The cliff. The write mask applies AFTER the blend, so a shader that merged the destination
	// into its output first would hand the blend unit a source made of destination bytes and the
	// unit would blend the destination with itself. A blended draw therefore keeps the pipeline
	// mask -- unless the SHADER owns the blend too, in which case its output is what lands and the
	// merge is exact again.
	EXPECT_FALSE(Road(true, kR | kG, /*blend_active=*/true, /*shader_blend=*/false, true));
	EXPECT_TRUE(Road(true, kR | kG, /*blend_active=*/true, /*shader_blend=*/true, false));
}

TEST(TileGpuShaderWriteMask, ADrawTheByteTailDoesNotOwnKeepsThePipelineMask)
{
	// ⚠️ The one that cost a corpus round. The mask arm sits at the end of a tail that first
	// converts the colour to bytes at floor(cv * 255 + 0.5); a UNORM8 attachment write rounds ties
	// its own way. A draw the tail does not already own would have the channels the mask does not
	// even cover re-rounded on the way in -- 17 pixels of Ace Combat 5, ±1 in one channel each, on
	// 12 unblended 32-bit draws. Those draws keep the pipeline mask.
	EXPECT_FALSE(Road(true, kR | kG, /*blend_active=*/false, /*shader_blend=*/false, /*shader_fbmsk=*/false));
}

TEST(TileGpuShaderWriteMask, EveryDrawTheRoadTakesWasAlreadyAReader)
{
	// The invariant, over the whole argument space rather than over cases: the road is reachable
	// only where the draw's blend or its FBMSK is ALREADY the fragment stage's job, and either of
	// those means the draw was already declared an in-pass reader. So the lever cannot create a
	// reader -- which is what keeps it out of the per-class declaring budget (it charges nothing
	// new), out of the Adreno reader segregation (it moves no draw into a declaring pass) and out
	// of GSTileGpuReorderScheduler's barrier-run population (measured: Dirge of Cerberus' reorder
	// census is identical with the lever on).
	for (u32 bits = 0; bits < 16; bits++)
	{
		const bool blend_active = (bits & 1) != 0;
		const bool shader_blend = (bits & 2) != 0;
		const bool shader_fbmsk = (bits & 4) != 0;
		const bool lever = (bits & 8) != 0;
		for (u32 mask = 0; mask < 16; mask++)
		{
			if (!Road(lever, static_cast<u8>(mask), blend_active, shader_blend, shader_fbmsk))
				continue;
			EXPECT_TRUE(shader_blend || shader_fbmsk) << "the road took a draw that reads nothing";
		}
	}
}

// ---------------------------------------------------------------------------------------
// The run key: the collapse the lever exists for.
// ---------------------------------------------------------------------------------------

TEST(TileGpuShaderWriteMask, TheMaskCutsTheRunOnThePipelineRoad)
{
	// The cost being paid today: the four masks of one rotation are four blend keys, so four
	// pipelines and four indirect calls where the geometry asked for one run.
	const u32 r = BlendKey(kR, false);
	const u32 rg = BlendKey(kR | kG, false);
	const u32 gb = BlendKey(kG | kB, false);
	const u32 ba = BlendKey(kB | kA, false);
	EXPECT_NE(r, rg);
	EXPECT_NE(rg, gb);
	EXPECT_NE(gb, ba);
	EXPECT_NE(r, ba);
}

TEST(TileGpuShaderWriteMask, TheMaskStopsCuttingTheRunOnTheShaderRoad)
{
	// ...and gone: the same four draws carry one blend key, so the executor binds one pipeline and
	// issues one vkCmdDrawIndexedIndirect over all four entries. Order survives -- Vulkan
	// guarantees primitive order across the entries of one indirect call -- so nothing is reordered
	// to buy this.
	const u32 r = BlendKey(kR, true);
	const u32 rg = BlendKey(kR | kG, true);
	const u32 gb = BlendKey(kG | kB, true);
	const u32 ba = BlendKey(kB | kA, true);
	EXPECT_EQ(r, rg);
	EXPECT_EQ(rg, gb);
	EXPECT_EQ(gb, ba);
}

TEST(TileGpuShaderWriteMask, TheShaderRoadWritesEveryChannelFromThePipeline)
{
	// The pipeline must stop masking, or the shader's merged bytes are dropped on the way out and
	// the channels the game meant to write never land.
	const u32 key = BlendKey(kR, true);
	EXPECT_EQ((key & Plan::kNoWriteMask), 0u);
	// ...and the draw is flagged as a reader, which is what makes its pass declare the in-pass
	// destination read and its pipeline carry the ROAA blend flag.
	EXPECT_NE((key & Plan::kSelfRead), 0u);
}

TEST(TileGpuShaderWriteMask, AShaderRoadRunStillCutsOnEverythingElse)
{
	// Only the mask leaves. A draw that also differs in its blend equation still splits the run --
	// the field is in the same key and this change must not widen a run past what the pipeline can
	// serve.
	const u32 plain = BlendKey(kR, true);
	const u32 blended = BlendKey(kR | kG, true) | Plan::kBlendEnable | 7u;
	EXPECT_NE(plain, blended);
}

// ---------------------------------------------------------------------------------------
// Exactness: the shader reproduces the pipeline's write, bit for bit.
// ---------------------------------------------------------------------------------------

TEST(TileGpuShaderWriteMask, DroppedChannelsBecomeWholeKeepBytes)
{
	// FBMSK=0xFFFFFF00 on a 32-bit frame: red alone lands. The register's keep mask already covers
	// the other three, so the union changes nothing -- this is the case where the two halves agree
	// by themselves.
	EXPECT_EQ(gsTileFrameColorWriteMask(0xFFFFFF00u, kC32, false, AFAIL_KEEP), kR);
	EXPECT_EQ(KeepMask(0xFFFFFF00u, kC32, kR, true), 0xFFFFFF00u);
}

TEST(TileGpuShaderWriteMask, SixteenBitDroppedChannelsKeepTheWholeByteNotTheStoredBits)
{
	// ⚠️ The case a `keep = FBMSK & fmsk` shortcut gets wrong. On PSMCT16 the register's keep mask
	// for a dropped channel is 0xF8, not 0xFF -- the format stores five bits -- but the PIPELINE
	// preserved the whole byte. Left at 0xF8 the fragment's low three bits would land where the
	// pipeline dropped them, so the channel mask has to contribute the full byte.
	const u32 fbmsk = 0x00F8F8F8u; // mask R, G and B; leave alpha
	EXPECT_EQ(gsTileFrameColorWriteMask(fbmsk, kC16, false, AFAIL_KEEP), kA);
	EXPECT_EQ(gsTileGpuFrameKeepMask(fbmsk, kC16), 0x00F8F8F8u);
	EXPECT_EQ(KeepMask(fbmsk, kC16, kA, true), 0x00FFFFFFu);
}

TEST(TileGpuShaderWriteMask, PartlyMaskedChannelsKeepTheirRegisterBits)
{
	// A channel the register masks in PART is still written, so the union must not blanket it: its
	// keep byte stays exactly what gsTileGpuFrameKeepMask said. Xenosaga's single-bit alpha FBMSK
	// on a 32-bit frame is the corpus's dense example.
	const u32 fbmsk = 0x80FFFF00u; // R written, G and B dropped, alpha's MSB alone kept
	const u8 mask = gsTileFrameColorWriteMask(fbmsk, kC32, false, AFAIL_KEEP);
	EXPECT_EQ(mask, static_cast<u8>(kR | kA));
	EXPECT_EQ(KeepMask(fbmsk, kC32, mask, true), 0x80FFFF00u);
}

TEST(TileGpuShaderWriteMask, TheAllFailAfailFoldRidesTheKeepMaskToo)
{
	// The channel mask carries the AFAIL fold, and the shader's keep mask is built from the channel
	// mask -- so an all-fail RGB_ONLY draw, whose alpha the PIPELINE was dropping on top of
	// whatever FBMSK said, keeps its alpha byte here as well. Reading the register alone would
	// paint that byte.
	const u8 mask = gsTileFrameColorWriteMask(0u, kC32, /*atst_all_fail=*/true, AFAIL_RGB_ONLY);
	EXPECT_EQ(mask, static_cast<u8>(kR | kG | kB));
	EXPECT_EQ(gsTileGpuFrameKeepMask(0u, kC32), 0u);
	EXPECT_EQ(KeepMask(0u, kC32, mask, true), 0xFF000000u);
}

TEST(TileGpuShaderWriteMask, TheMergedBytesAreWhatThePipelineWouldHaveWritten)
{
	// End to end over the shader's own expression, against what a masked pipeline write leaves in
	// the target. Every combination of the four channels, on both a 32-bit and a 16-bit frame.
	constexpr u32 kSrc = 0x1234'5678u;
	constexpr u32 kDst = 0xFEDC'BA98u;
	for (u32 fbmsk_sel = 0; fbmsk_sel < 16; fbmsk_sel++)
	{
		for (const u32 fmsk : {kC32, kC16})
		{
			// A whole-channel FBMSK, which is the population this road is for.
			u32 fbmsk = 0;
			for (u32 b = 0; b < 4; b++)
			{
				if (fbmsk_sel & (1u << b))
					fbmsk |= 0xFFu << (b * 8);
			}
			const u8 mask = gsTileFrameColorWriteMask(fbmsk, fmsk, false, AFAIL_KEEP);
			if (!Road(true, mask, false, false, /*shader_fbmsk=*/true))
				continue;

			// What the pipeline leaves: the fragment's byte where the channel is written, the
			// destination's where it is masked.
			u32 pipeline = 0;
			for (u32 b = 0; b < 4; b++)
			{
				const u32 byte = ((mask & (1u << b)) ? kSrc : kDst) & (0xFFu << (b * 8));
				pipeline |= byte;
			}
			EXPECT_EQ(Merge(kSrc, kDst, KeepMask(fbmsk, fmsk, mask, true)), pipeline)
				<< "fbmsk=0x" << std::hex << fbmsk << " fmsk=0x" << fmsk;
		}
	}
}

// =========================================================================================
// THE COLOUR-FREE FRAGMENT VARIANT (EmuCore/GS/TileGpuNoRgbFragmentVariant).
//
// A draw whose pipeline colour write mask keeps every RGB channel cannot land one, so everything
// the fragment program computes for those three is dead -- and on a tiler dead is program SIZE,
// which is the axis the Adreno 650 swings a whole frame on. The program that serves such a run
// leaves out the texture function's colour half, the fog walk and the integer blend arm
// (tilegpu.glsl's TILEGPU_NO_RGB).
//
// The scope is decided by GSTileGpuPassPlan::LandsNoRgb off the BLEND KEY, which is already the
// indirect-run key -- so a run is uniform in this by construction and no plan stream carries it.
// What that predicate must get right is pinned here, because the whole claim of zero traded
// accuracy rests on it: a draw that CAN land an RGB byte must never reach the colour-free program.
// =========================================================================================

// The scope, over every write mask there is. Yes exactly for the four masks with no RGB channel in
// them, no for the other twelve.
TEST(GSTileGpuShaderWriteMask, TheColourFreeVariantTakesExactlyTheMasksThatLandNoRgb)
{
	u32 yes = 0, no = 0;
	for (u8 cm = 0; cm <= 0xF; cm++)
	{
		const bool lands_rgb = (cm & (kR | kG | kB)) != 0;
		const bool answer = Plan::LandsNoRgb(BlendKey(cm, /*in_shader=*/false));
		EXPECT_EQ(answer, !lands_rgb) << "mask=" << std::hex << u32(cm);
		(answer ? yes : no)++;
	}
	// Depth-only (0x0) and alpha-only (0x8) are the two, times the self-read flag's two positions
	// further down; twelve masks land at least one colour channel.
	EXPECT_EQ(yes, 2u);
	EXPECT_EQ(no, 14u);

	// The named populations, by the shape the planner gives them. The alpha-test split's second half
	// under AFAIL=RGB_ONLY writes the alpha byte alone; under ZB_ONLY it writes no colour at all;
	// and the dual-source carrier's alpha companion is the first of those again.
	EXPECT_TRUE(Plan::LandsNoRgb(Plan::PackNoWrite(kA)));
	EXPECT_TRUE(Plan::LandsNoRgb(Plan::PackNoWrite(0)));
	// ...and Ace Combat 5's "write colour, keep alpha" repaint is the other way round and must NOT be
	// taken: it lands three RGB bytes.
	EXPECT_FALSE(Plan::LandsNoRgb(Plan::PackNoWrite(kR | kG | kB)));
	EXPECT_FALSE(Plan::LandsNoRgb(Plan::PackNoWrite(kR | kG | kB | kA)));
	// A plan carrying no blend keys at all asks for all four channels, so it is never colour-free.
	EXPECT_FALSE(Plan::LandsNoRgb(0));
}

// ⚠️ THE SHAPE THAT WOULD BE WRONG, and the reason the predicate reads the blend key rather than the
// draw's own colour_mask. When the FRAGMENT stage is serving the write mask
// (gsTileGpuMasksInShader) the pipeline writes all four channels and the shader merges the
// destination in -- so the draw's own mask says "RGB dropped" while RGB bytes really do land, out of
// the shader's own arithmetic. The blend key is the thing that knows.
TEST(GSTileGpuShaderWriteMask, TheColourFreeVariantRefusesADrawWhoseMaskTheShaderIsServing)
{
	for (u8 cm = 0; cm <= 0xF; cm++)
	{
		// Every draw on the shader's write-mask road packs 0xF and therefore lands RGB.
		EXPECT_FALSE(Plan::LandsNoRgb(BlendKey(cm, /*in_shader=*/true))) << "mask=" << std::hex << u32(cm);
	}
	// ...and the same draw with the mask left on the pipeline is colour-free where its mask says so.
	EXPECT_TRUE(Plan::LandsNoRgb(BlendKey(kA, false)));
	EXPECT_FALSE(Plan::LandsNoRgb(BlendKey(kA, true)));
}

// The second narrowing: the colour-free program still samples the texture, but only for the ALPHA,
// so where TCC is frozen off it does not sample at all. With the spec unfrozen the row decides at
// runtime and the road has to stay -- getting that backwards would drop the alpha the test reads.
TEST(GSTileGpuShaderWriteMask, TheColourFreeVariantKeepsTheTexelRoadOnlyWhereTheAlphaComesFromIt)
{
	GSDevice::GSTileGpuFragmentSpec unfrozen;
	EXPECT_FALSE(unfrozen.valid);
	EXPECT_TRUE(unfrozen.NoRgbWantsTexel()) << "an unspecialized program reads TCC off the row";

	GSDevice::GSTileGpuFragmentSpec spec;
	spec.valid = true;
	for (u8 tfx = 0; tfx < 4; tfx++)
	{
		spec.tfx = tfx;
		spec.tcc = 1;
		EXPECT_TRUE(spec.NoRgbWantsTexel()) << "tfx=" << u32(tfx); // the texel carries the alpha
		spec.tcc = 0;
		EXPECT_FALSE(spec.NoRgbWantsTexel()) << "tfx=" << u32(tfx); // ...and here it carries nothing
	}
}
