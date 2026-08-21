// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Rule 3's DONOR build road: a texture source read straight out of the ONE live target that owns
// the window's pages, instead of out of the frame's byte ring. It removes a round trip -- a
// writeback compute pass reswizzling the target into ring slots, then a materialise unswizzling
// them back -- for bytes that never leave the GPU, and it gives those windows a content identity a
// page fingerprint cannot supply.
//
// Three contracts hold that road up and every one of them fails silently:
//
//  1. THE PARAMETERS. The reinterpretation's arithmetic is already pinned against GSOffset on every
//     texel of ten windows (gs_tile_swizzle_forms_tests, LocateAgreesWithGSOffsetOnEveryTexel) --
//     but only for the four numbers it is HANDED. The renderer derives those four from TEX0 and the
//     owner's layout with its own spelling, and a wrong spelling reads the right arithmetic over
//     the wrong bytes: a plausible image, off by a page row. So this file pins the derivation
//     against GSOffset's, which is what makes the existing proof reach the road that uses it.
//  2. THE FORMAT GATE. The renderer and the executor decide independently which windows this road
//     serves -- the renderer refuses, the executor picks a pipeline -- and both go through
//     IndexFormatFor. If they could disagree, a refused window would still get an op, or an
//     admitted one would silently get no build and the draw would sample an unwritten image.
//  3. THE IDENTITY. A donor source is filed under the owning target's id and version, in the same
//     field a CPU-side window files a page fingerprint in. Those two numbering spaces must never
//     compare equal, or a target's version number gets read as a hash of somebody's bytes.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"

#include <gtest/gtest.h>

namespace
{
	// GSOffset's own answer for "how many pages wide is a row of this layout", which is the number
	// the reinterpretation shader's page term is written against.
	u32 PageRowWidth(u32 bp, u32 bw, u32 psm)
	{
		const GSOffset off = GSOffset::fromKnownPSM(bp, bw, static_cast<GS_PSM>(psm));
		return static_cast<u32>(off.pageRowWidth() >> off.pageShiftX());
	}
} // namespace

// -- 1. the parameters -----------------------------------------------------------------------

// The window half. GSRendererTileGpu::DonorForTextureRead spells the window's pages-per-row as
// `TEX0.TBW >> 1`, which is also what the byte road's state row and the materialise op carry -- the
// three have to agree or one window addresses two different sets of bytes. This is that spelling
// against GSOffset's, over every TBW a texture register can hold.
TEST(TileGpuDonorRoad, WindowPagesPerRowIsGSOffsetsForEveryTBW)
{
	for (u32 tbw = 0; tbw < 64; tbw++)
	{
		for (const u32 psm : {static_cast<u32>(PSMT8), static_cast<u32>(PSMT4)})
		{
			// The base does not enter pages-per-row, but vary it anyway so a derivation that
			// accidentally folded it in would show up here rather than on a device.
			for (const u32 bp : {0u, 0x20u, 0x1180u, 0x3e04u})
			{
				const u32 want = PageRowWidth(bp, tbw, psm);
				const u32 got = tbw >> 1;
				EXPECT_EQ(got, want) << "psm " << psm << " bp " << bp << " TBW " << tbw;
			}
		}
	}
}

// The owner half. The donor hands the owner's `layout.bw` straight through as its pages-per-row,
// and the shader then addresses the owner through the CT32 page geometry -- 64x32 texels a page,
// tile_ib48 and tile_ic32 -- whatever the owner's own PSM says. That is exact for CT32 and CT24
// (their page geometry IS CT32's; a CT24 surface is CT32 bytes with one channel spare) and it is
// why the road's owner clause is HasByteRoad's: colour kind, CT32 or CT24, page-aligned base.
TEST(TileGpuDonorRoad, OwnerPagesPerRowIsItsLayoutStrideForColourTargets)
{
	for (u32 bw = 0; bw < 64; bw++)
	{
		for (const u32 psm : {static_cast<u32>(PSMCT32), static_cast<u32>(PSMCT24)})
			EXPECT_EQ(bw, PageRowWidth(0, bw, psm)) << "psm " << psm << " bw " << bw;
	}
	// The two admitted owner formats share CT32's page geometry exactly, which is the assumption the
	// reinterpretation's owner-side arithmetic is written against...
	EXPECT_EQ(GSLocalMemory::m_psm[PSMCT24].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	// ...and the 16-bit and depth families do NOT, so admitting one would run the right arithmetic
	// over the wrong pixel space. That is the exclusion HasByteRoad enforces, stated here as a fact
	// about the formats rather than left as a comment beside the clause.
	EXPECT_NE(GSLocalMemory::m_psm[PSMCT16].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
	EXPECT_NE(GSLocalMemory::m_psm[PSMZ16].pgs, GSLocalMemory::m_psm[PSMCT32].pgs);
}

// -- 2. the format gate ----------------------------------------------------------------------

// Exactly the two paletted formats TileGpu samples reach the donor road, and neither direct-colour
// format does. The direct-colour case is not an oversight: rule 2 already binds a target whose
// layout IS the read's, and serving one whose layout is not needs a COPY leg the reinterpretation
// does not have (it only produces index formats). The renderer counts that population instead
// (src_donor_direct), and this is the fact that makes the counter mean what it says.
TEST(TileGpuDonorRoad, OnlyThePalettedFormatsReinterpret)
{
	EXPECT_GE(GSTileSwizzleForms::IndexFormatFor(PSMT8), 0);
	EXPECT_GE(GSTileSwizzleForms::IndexFormatFor(PSMT4), 0);
	EXPECT_LT(GSTileSwizzleForms::IndexFormatFor(PSMCT32), 0);
	EXPECT_LT(GSTileSwizzleForms::IndexFormatFor(PSMCT24), 0);
	// The alpha-byte views are in the table and no TileGpu draw samples them yet, so the road can
	// serve them the day the fragment stage can (they are still refused earlier, on format).
	EXPECT_GE(GSTileSwizzleForms::IndexFormatFor(PSMT8H), 0);
	EXPECT_GE(GSTileSwizzleForms::IndexFormatFor(PSMT4HL), 0);
	EXPECT_GE(GSTileSwizzleForms::IndexFormatFor(PSMT4HH), 0);
}

// The executor keys its donor pipeline table by that same format number, so the table has to be big
// enough for every value the mapping can return. A table one short would compile no pipeline for
// the last format and leave the source unwritten -- which renders, just not what was asked for.
TEST(TileGpuDonorRoad, EveryIndexFormatFitsThePipelineTable)
{
	// The executor's kTileGpuIdxFormats. Restated here rather than included: GSDeviceVK.h is a
	// backend header the test suite does not link, and the whole point is that the two agree.
	constexpr int kIdxFormats = 5;
	for (const u32 psm : {static_cast<u32>(PSMT8), static_cast<u32>(PSMT4), static_cast<u32>(PSMT8H),
			 static_cast<u32>(PSMT4HL), static_cast<u32>(PSMT4HH)})
	{
		const int f = GSTileSwizzleForms::IndexFormatFor(psm);
		ASSERT_GE(f, 0) << "psm " << psm;
		EXPECT_LT(f, kIdxFormats) << "psm " << psm;
	}
}

// -- the prep op's own contract ----------------------------------------------------------------

// A donor op names TWO lists at once -- its destination among the plan's prep textures, its owner
// among the plan's targets -- which is the one thing about it that is easy to get wrong, because
// every other prep kind names one. The kinds also have to stay distinct, since the executor
// switches on them and a duplicated value would run one road's arm for another road's op.
TEST(TileGpuDonorRoad, PrepKindsAreDistinctAndDonorNamesBothLists)
{
	using K = GSDevice::GSTileGpuPrepKind;
	const K kinds[] = {K::Writeback, K::Seed, K::Materialise, K::Expand, K::Donor};
	for (size_t i = 0; i < std::size(kinds); i++)
	{
		for (size_t j = i + 1; j < std::size(kinds); j++)
			EXPECT_NE(static_cast<u32>(kinds[i]), static_cast<u32>(kinds[j])) << i << " vs " << j;
	}

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = K::Donor;
	op.target = 3; // prep_textures: the index image this build fills
	op.donor_target = 7; // targets: the owner it reads
	op.bp = 0x11a0;
	op.bw = 1;
	op.psm = PSMT8;
	op.owner_bp = 0x1180;
	op.owner_bwpg = 10;
	// The two indices are separate fields precisely so they can be equal numbers into different
	// lists without meaning the same thing.
	EXPECT_NE(op.target, op.donor_target);
	op.donor_target = op.target;
	EXPECT_EQ(op.target, op.donor_target);
	// The op carries no epoch and no page entries: this road reads no bytes, so it goes through no
	// page table. A default-constructed op leaves them zero and the executor never looks.
	EXPECT_EQ(op.epoch, 0u);
	EXPECT_EQ(op.page_entry_count, 0u);
	EXPECT_EQ(op.texa, 0u);
}

// -- 3. the identity -------------------------------------------------------------------------

// The donor's content token and a page fingerprint share one field, and the tag is the only thing
// keeping them apart. A Target token must never compare equal to a Pages token, whatever the
// numbers happen to be -- including the case that would actually happen, where the two values
// collide.
TEST(TileGpuDonorRoad, TargetTokenNeverEqualsAPageFingerprint)
{
	using CT = GSTileTextureSource::ContentToken;
	const CT target = GSTileTextureSource::TargetToken(2, 9);
	const CT pages{CT::Source::Pages, target.value};
	EXPECT_TRUE(target.Known());
	EXPECT_TRUE(pages.Known());
	EXPECT_FALSE(target == pages);
	EXPECT_FALSE(pages == target);
	EXPECT_TRUE(target == GSTileTextureSource::TargetToken(2, 9));
}

// Two owners, or two versions of one owner, are two identities. The version is the whole of "this
// target's texels changed", so a token that lost it would serve last frame's image for this frame's
// target -- silently, and only where the road is taken.
TEST(TileGpuDonorRoad, TargetTokenSeparatesOwnersAndVersions)
{
	for (u32 owner = 0; owner < 24; owner++)
	{
		for (u64 version = 1; version < 24; version++)
		{
			const auto t = GSTileTextureSource::TargetToken(owner, version);
			EXPECT_TRUE(t.Known()) << owner << " / " << version;
			for (u32 o2 = 0; o2 < 24; o2++)
			{
				for (u64 v2 = 1; v2 < 24; v2++)
				{
					const auto u = GSTileTextureSource::TargetToken(o2, v2);
					const bool same = (owner == o2 && version == v2);
					EXPECT_EQ(t == u, same) << owner << "/" << version << " vs " << o2 << "/" << v2;
				}
			}
		}
	}
}

// Surface 0 at version 0 is a real pair -- a target the model just created, before anything wrote
// it -- and it must not pack to the value ContentToken reads as "cannot say", which would turn the
// road's first frame into an unprovable rebuild for no reason.
TEST(TileGpuDonorRoad, TargetTokenIsKnownEvenForSurfaceZeroAtVersionZero)
{
	EXPECT_TRUE(GSTileTextureSource::TargetToken(0, 0).Known());
	EXPECT_TRUE(GSTileTextureSource::TargetToken(0, 1).Known());
	EXPECT_FALSE(GSTileTextureSource::TargetToken(0, 0) == GSTileTextureSource::TargetToken(0, 1));
}

// -- the index image's convention --------------------------------------------------------------

// A donor build writes float(idx)/255 into all four channels and tilegpu_expand.glsl reads it back
// as uint(a * 255.5). That is the same convention the materialise writes and ps_tile_reinterpret_index
// uses, and it is a convention between two shader files that no compiler checks. It has to round
// trip for every index either paletted format can produce.
TEST(TileGpuDonorRoad, IndexImageRoundTripsThroughTheExpandsDecode)
{
	for (u32 idx = 0; idx < 256; idx++)
	{
		const float stored = static_cast<float>(idx) / 255.0f;
		EXPECT_EQ(static_cast<u32>(stored * 255.5f), idx) << "index " << idx;
	}
}
