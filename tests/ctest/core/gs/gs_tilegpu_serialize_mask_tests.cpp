// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu executor's two ordering levers: the blanket GRADE (how hard) and the site MASK
// (where). Both are diagnostic, both ship at zero, and what is pinned here is the arithmetic that
// decides what a device arm actually engaged -- because that is the only part a run record can be
// wrong about silently.
//
// The mask exists because the grade cannot name an edge. On the RG477V's r44p1 Mali, grade 1 --
// full barriers, one command buffer, no submit split -- collapses FlatOut 2 from 79.7% run-to-run
// pixel diff to byte-identical. That proves the hazard closes under barriers alone and says nothing
// about which of the seven boundaries carries it, and re-running blankets cannot narrow it. A mask
// bisects seven sites in three rounds.
//
// What is pinned:
//
//  - ZERO is off, at both keys, and off records nothing.
//  - The site NUMBERING is fixed. A mask value quoted in a device record has to keep meaning what it
//    meant when the record was written, so the enum is append-only and the values are asserted here
//    rather than left to declaration order.
//  - Bits above the sites that exist are DROPPED, not refused -- a dev-only key must not be able to
//    stop a run over a typo.
//  - A NEGATIVE is off, not "all". A bisection arm that silently became the blanket would read as
//    the blanket's result, which is the one failure mode that produces a wrong conclusion rather
//    than a wasted round.
//  - kGSTileGpuSerializeMaskAll and the site count agree, which is the one way this could be
//    self-inconsistent: the executor uses the constant for the blanket road and the enum for the
//    per-site road, and a disagreement would make the blanket skip a boundary.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

TEST(TileGpuSerializeMask, ZeroIsOffAndIsTheShippedArm)
{
	EXPECT_EQ(gsTileGpuSerializeMask(0), 0u);
	EXPECT_EQ(gsTileGpuSerializeOps(0), 0u) << "the grade ships off too, and the two are read together";
}

TEST(TileGpuSerializeMask, NegativeIsOffAndNotEverything)
{
	// The dangerous typo. A negative read as "all bits" would engage every site while the record
	// says a bisection arm ran, and the round would report the blanket's answer under another name.
	EXPECT_EQ(gsTileGpuSerializeMask(-1), 0u);
	EXPECT_EQ(gsTileGpuSerializeMask(-64), 0u);
	EXPECT_EQ(gsTileGpuSerializeMask(std::numeric_limits<int>::min()), 0u);
}

TEST(TileGpuSerializeMask, SiteNumberingIsFixedBecauseRecordsQuoteIt)
{
	// Append-only. Changing any of these silently reinterprets every mask value already written down.
	static_assert(kGSTileGpuSerializeSiteExpand == 0, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteDonor == 1, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteClutCopy == 2, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteMaterialise == 3, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteByteRoad == 4, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteSnapshot == 5, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSitePassTail == 6, "site numbering is append-only");
	static_assert(kGSTileGpuSerializeSiteCount == 7, "the executor records seven boundaries");
}

TEST(TileGpuSerializeMask, OneBitSelectsExactlyOneSite)
{
	for (u32 site = 0; site < kGSTileGpuSerializeSiteCount; site++)
	{
		const u32 want = 1u << site;
		EXPECT_EQ(gsTileGpuSerializeMask(static_cast<int>(want)), want) << "site " << site;
	}
}

TEST(TileGpuSerializeMask, TheBisectionArmsSurviveTheClamp)
{
	// The halves a first device round actually asks for, written the way a run record would carry
	// them: 0x0F is the four prep-op tails, 0x70 is the byte road, the snapshot and the pass tail.
	EXPECT_EQ(gsTileGpuSerializeMask(0x0F), 0x0Fu);
	EXPECT_EQ(gsTileGpuSerializeMask(0x70), 0x70u);
	EXPECT_EQ(gsTileGpuSerializeMask(0x0F) | gsTileGpuSerializeMask(0x70), kGSTileGpuSerializeMaskAll)
		<< "the two halves partition the sites, or a bisection can miss the culprit";
}

TEST(TileGpuSerializeMask, BitsAboveTheSitesAreDroppedNotRefused)
{
	EXPECT_EQ(gsTileGpuSerializeMask(static_cast<int>(kGSTileGpuSerializeMaskAll)), kGSTileGpuSerializeMaskAll);
	EXPECT_EQ(gsTileGpuSerializeMask(static_cast<int>(kGSTileGpuSerializeMaskAll) + 1), 0u)
		<< "the first bit past the sites names no site, so it engages none";
	EXPECT_EQ(gsTileGpuSerializeMask(0x7FFFFFFF), kGSTileGpuSerializeMaskAll) << "and stays clamped, however wild";
}

TEST(TileGpuSerializeMask, EveryBitSetIsTheSameSetOfSitesAsTheBlanket)
{
	// The self-consistency check. The executor takes kGSTileGpuSerializeMaskAll for the blanket road
	// and the enum bits for the per-site one; if they disagreed, the blanket would skip a boundary and
	// the two roads would not be comparable -- which is the whole point of having both.
	static_assert(kGSTileGpuSerializeMaskAll == (1u << kGSTileGpuSerializeSiteCount) - 1u,
		"the blanket engages exactly the sites that exist");
	u32 from_enum = 0;
	for (u32 site = 0; site < kGSTileGpuSerializeSiteCount; site++)
		from_enum |= 1u << site;
	EXPECT_EQ(from_enum, kGSTileGpuSerializeMaskAll);
}

TEST(TileGpuSerializeMask, TheGradeLadderIsUnchanged)
{
	// The mask is a second key on the same lever, so the grade's own arithmetic is re-pinned here:
	// a mask round that turned out to have moved the grade would be unattributable.
	EXPECT_EQ(gsTileGpuSerializeOps(1), 1u);
	EXPECT_EQ(gsTileGpuSerializeOps(kGSTileGpuSerializeMax), kGSTileGpuSerializeMax);
	EXPECT_EQ(gsTileGpuSerializeOps(static_cast<int>(kGSTileGpuSerializeMax) + 1), kGSTileGpuSerializeMax);
	EXPECT_EQ(gsTileGpuSerializeOps(-1), 0u);
}
