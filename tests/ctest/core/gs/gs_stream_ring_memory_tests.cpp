// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins which host-visible memory the Vulkan stream rings are allocated from
// (GS/Renderers/Common/GSStreamRingMemoryPolicy.h).
//
// The decision is a pure function of the device's memory-type table and one driver-database bit,
// which is the only reason the roads can be pinned at all: only the MQ65 takes a cached road today,
// and the cached coherent road is taken by nothing on this desk. Each table below is written from a
// real device's reported types, named, so a future reader can tell a measured shape from an
// invented one.
//
// The load-bearing pair is SD865-with-and-without-the-bit. Without it the A650 must land on the
// write-combined type it had before this policy, because the keyless confirmation round found the
// cached coherent road cost legosw +6.16% and ac5 +5.54% on that part with non-overlapping rep
// ranges. With it, it must land on the cached coherent type -- nothing sets the bit for an A650
// today, and pinning the road keeps a future A650 rule a one-line database change rather than a
// policy change.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSStreamRingMemoryPolicy.h"

#include <gtest/gtest.h>

namespace
{
	constexpr u32 DEVICE_LOCAL = GS_MEMORY_PROPERTY_DEVICE_LOCAL;
	constexpr u32 HOST_VISIBLE = GS_MEMORY_PROPERTY_HOST_VISIBLE;
	constexpr u32 HOST_COHERENT = GS_MEMORY_PROPERTY_HOST_COHERENT;
	constexpr u32 HOST_CACHED = GS_MEMORY_PROPERTY_HOST_CACHED;

	// MQ65: Adreno 610 on Turnip 26.1.2. Type 0 is the write-combined type Turnip always offers;
	// type 1 is the cached NON-coherent type, which on aarch64 Turnip also always exists. The
	// cached COHERENT type is absent -- it needs an IO-coherent GPU and msm_minor_version >= 8,
	// and this part has neither. Both types are device-local: unified memory.
	constexpr u32 MQ65_TYPES[] = {
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED,
	};

	// SD865: Adreno 650 on the same driver, from the guard round's own ring log. Type 1 carries
	// all four bits, which is why the rule below never gets a say on this part.
	constexpr u32 SD865_TYPES[] = {
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED,
	};

	// RG 477V: MediaTek MT6897, Mali-G615 MC6, Arm r44p1, from its own ring log. Bit for bit the
	// MQ65's table -- write-combined type 0, cached non-coherent type 1, both device-local -- and
	// the opposite answer, because on this part the cache clean costs more than the write-combined
	// stores it replaces: every title in the suite ran slower on the cached type, +2.6% to +12.4%.
	// No rule names it, so it keeps what it has.
	constexpr u32 RG477V_TYPES[] = {
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED,
	};

	// M2 Max under Asahi's Honeykrisp (Mesa 25.3.6), from this lane's own log line: one
	// host-visible type, and it is already cached, coherent and device-local. The dev box has to
	// come out of this decision on exactly the type it went in on, or its identity grid proves
	// nothing.
	constexpr u32 M2_TYPES[] = {
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED,
	};

	// A desktop discrete GPU: VRAM, a small device-local host-visible BAR window, and two
	// host-local (system RAM) types, one of them cached. The cached one is the trap -- it satisfies
	// every property bit the policy wants except DEVICE_LOCAL, and taking it would put every
	// vertex, index, uniform and staging byte the GPU reads across PCIe.
	constexpr u32 DISCRETE_TYPES[] = {
		DEVICE_LOCAL,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT,
		HOST_VISIBLE | HOST_COHERENT,
		HOST_VISIBLE | HOST_COHERENT | HOST_CACHED,
	};

	constexpr GSStreamRingMemoryInputs Device(const u32* types, u32 count, bool rule)
	{
		GSStreamRingMemoryInputs in;
		in.type_flags = types;
		in.type_count = count;
		in.prefer_cached_over_write_combined = rule;
		return in;
	}
} // namespace

// The MQ65's road: no cached coherent type, the database says the write-combined road is expensive
// here, so the rings take the cached non-coherent type and CommitMemory's flush goes live.
TEST(GSStreamRingMemory, MQ65TakesCachedNonCoherent)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(MQ65_TYPES, 2, true));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::CachedNonCoherent);
	EXPECT_EQ(d.type_index, 1u);
	EXPECT_EQ(d.extra_required_flags, DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED);
	EXPECT_STREQ(GSStreamRingMemoryRoadName(d.road), "cached-noncoherent");
}

// The same table without the rule. The non-coherent road costs a cache clean per commit, so it is
// taken only where a device round says the road it replaces is worse. Nobody gets it by accident.
TEST(GSStreamRingMemory, MQ65ShapeWithoutTheRuleStaysWriteCombined)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(MQ65_TYPES, 2, false));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::WriteCombined);
	EXPECT_EQ(d.type_index, 0u);
	EXPECT_EQ(d.extra_required_flags, 0u);
	EXPECT_STREQ(GSStreamRingMemoryRoadName(d.road), "write-combined");
}

// The RG 477V. The table is identical to the MQ65's, and this is the case that stops the policy
// from being written as "no cached coherent type -> take the cached one": that rule would have
// shipped a regression on every title of this device. The database names parts, not shapes.
TEST(GSStreamRingMemory, RG477VKeepsWriteCombinedBecauseNoRuleNamesIt)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(RG477V_TYPES, 2, false));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::WriteCombined);
	EXPECT_EQ(d.type_index, 0u);
	EXPECT_EQ(d.extra_required_flags, 0u);
}

// The same part if a rule ever named it. Its only cached type is non-coherent, so that is the road
// it would get -- the one its own round measured at +2.6% to +12.4%. Here to show the bit selects
// a road rather than a device: the table, not the rule, decides which.
TEST(GSStreamRingMemory, RG477VWithARuleWouldTakeCachedNonCoherent)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(RG477V_TYPES, 2, true));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::CachedNonCoherent);
	EXPECT_EQ(d.type_index, 1u);
}

// The SD865 as it ships: a cached coherent type exists and no rule names the A650, so the rings
// stay on the write-combined type 0 they were on before this policy. The confirmation round
// measured that road on this exact part and it lost two titles reproducibly; a type table is not a
// measurement.
TEST(GSStreamRingMemory, SD865WithoutARuleStaysWriteCombined)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(SD865_TYPES, 2, false));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::WriteCombined);
	EXPECT_EQ(d.type_index, 0u);
	EXPECT_EQ(d.extra_required_flags, 0u);
	EXPECT_STREQ(GSStreamRingMemoryRoadName(d.road), "write-combined");
}

// The same table with the bit set. Nothing sets it for an A650 today; this pins where such a rule
// would land the rings, so adding one stays a database edit.
TEST(GSStreamRingMemory, SD865WithTheRuleTakesCachedCoherent)
{
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(SD865_TYPES, 2, true));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::CachedCoherent);
	EXPECT_EQ(d.type_index, 1u);
	EXPECT_EQ(d.extra_required_flags, DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED);
	EXPECT_STREQ(GSStreamRingMemoryRoadName(d.road), "cached-coherent");
}

// The dev box. Its one host-visible type already carries every bit, so every road resolves to the
// type the rings have always been on and nothing about the M2 moves whichever way the bit goes.
// This is what makes the 24-row byte-identity grid a gate on the code rather than a measurement of
// the road.
TEST(GSStreamRingMemory, M2StaysOnTheTypeItAlreadyHas)
{
	const GSStreamRingMemoryDecision without = GSDecideStreamRingMemory(Device(M2_TYPES, 1, false));
	EXPECT_EQ(without.road, GSStreamRingMemoryRoad::WriteCombined);
	EXPECT_EQ(without.type_index, 0u);
	EXPECT_EQ(without.extra_required_flags, 0u);

	const GSStreamRingMemoryDecision with = GSDecideStreamRingMemory(Device(M2_TYPES, 1, true));
	EXPECT_EQ(with.road, GSStreamRingMemoryRoad::CachedCoherent);
	EXPECT_EQ(with.type_index, 0u);
}

// A discrete GPU, with the bit set as well as clear. Its cached host-visible type is host-local,
// so both cached roads decline it and the rings stay on the device-local write-combined BAR type --
// where they are today, and the right place for memory the GPU reads over PCIe. The DEVICE_LOCAL
// requirement is the whole of what stops a named desktop part sending its rings across PCIe.
TEST(GSStreamRingMemory, DiscreteGpuKeepsItsRingsInDeviceLocalMemory)
{
	for (const bool rule : {false, true})
	{
		const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(DISCRETE_TYPES, 4, rule));
		EXPECT_EQ(d.road, GSStreamRingMemoryRoad::WriteCombined);
		EXPECT_EQ(d.type_index, 1u);
		EXPECT_EQ(d.extra_required_flags, 0u);
	}
}

// A device that offers no cached host-visible type at all, with the rule set. There is nothing to
// move to, so the answer is the road it is already on rather than a failed allocation.
TEST(GSStreamRingMemory, NoCachedTypeAnywhereStaysWriteCombined)
{
	constexpr u32 types[] = {DEVICE_LOCAL, DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT};
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(types, 2, true));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::WriteCombined);
	EXPECT_EQ(d.type_index, 1u);
}

// VMA breaks a tie by taking the lowest index, and the banner's predicted index has to break it the
// same way or the line names a type the rings are not on.
TEST(GSStreamRingMemory, TwoEqualTypesResolveToTheLowerIndex)
{
	constexpr u32 types[] = {
		DEVICE_LOCAL | HOST_VISIBLE,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED,
		DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED,
	};
	const GSStreamRingMemoryDecision d = GSDecideStreamRingMemory(Device(types, 3, true));
	EXPECT_EQ(d.road, GSStreamRingMemoryRoad::CachedCoherent);
	EXPECT_EQ(d.type_index, 1u);
}
