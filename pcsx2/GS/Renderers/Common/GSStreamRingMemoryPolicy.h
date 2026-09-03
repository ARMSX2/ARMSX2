// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>

// Which host-visible memory the six Vulkan stream rings live in.
//
// The rings -- vertex, index, expand-index, VS uniform, PS uniform, texture upload -- are written
// by the CPU and read by the GPU, and nothing ever reads one back. Until this decision existed,
// VKStreamBuffer::Create asked VMA for HOST_VISIBLE with HOST_COHERENT preferred and took whatever
// came out, which on every Turnip device is memory type 0: the write-combined, uncached type. That
// is not a free choice. An uncached store is only fast if the core's store buffer merges adjacent
// writes into full bursts, and the small in-order-ish cores this emulator targets merge badly.
//
// What the devices said (rung T1, 2026-09-03, records under
// devs/bmdhacks/campaigns/gs-classic-tiler/sd662-tier/rung-t1-vertex-ring-writes/):
//
//   * MQ65 (Adreno 610, Turnip 26.1.2) offers NO cached coherent host-visible type. Moving the
//     rings to the cached NON-coherent type, paying the per-region flush CommitMemory already
//     issues, took GS-thread p50 down 29.5% on legosw, 28.4% on gow2, 21.0% on yugioh and 20.6%
//     on ac5. Draw-heavy controls flat, 24 of 24 frame grids byte-identical.
//   * SD865 (Adreno 650, same driver) DOES offer a cached coherent type and we were not using it,
//     because VMA returns the first zero-cost type and the write-combined one scores zero. On the
//     cached coherent type: gt4opb -13.8%, yugioh -2.7%, gow2 and stuntman flat, ac5 +3.6% and
//     legosw +5.3% -- the two regressions about 0.11 ms on titles running 5-15x under budget.
//   * The store INSTRUCTION is not the story. Replacing the storent (STNP) copy with memcpy moved
//     nothing anywhere, on either device, with or without the memory-type change.
//
// So: take a cached coherent type wherever one exists, take a cached non-coherent one where the
// driver database says the write-combined road is expensive on this GPU, and otherwise change
// nothing.
//
// ⚠️ Both cached roads require DEVICE_LOCAL, and that requirement is load-bearing on desktop. On a
// discrete GPU a HOST_VISIBLE|HOST_COHERENT|HOST_CACHED type is ordinary system RAM, and moving
// the rings there would send every vertex, index, uniform and texture staging byte across PCIe on
// the GPU's side of the read. Today's HOST_COHERENT-only ask lands on the device-local BAR type
// there, which is the right answer for memory the GPU reads. A device whose only cached
// host-visible types are host-local therefore stays on the write-combined road.
//
// Written as a pure function of the memory-type table and one database bit so the roads no device
// on this desk takes can still be pinned. See gs_stream_ring_memory_tests.cpp.

/// The Vulkan memory property bits, mirrored so this header stays backend-neutral like the other
/// GS policies. VKStreamBuffer static_asserts each one against the VK_MEMORY_PROPERTY_* value.
enum : u32
{
	GS_MEMORY_PROPERTY_DEVICE_LOCAL = 0x0001,
	GS_MEMORY_PROPERTY_HOST_VISIBLE = 0x0002,
	GS_MEMORY_PROPERTY_HOST_COHERENT = 0x0004,
	GS_MEMORY_PROPERTY_HOST_CACHED = 0x0008,
};

/// "No such type." Not a memory type index the device could ever report -- Vulkan caps the count
/// at VK_MAX_MEMORY_TYPES (32).
constexpr u32 GS_INVALID_MEMORY_TYPE = 0xFFFFFFFFu;

enum class GSStreamRingMemoryRoad : u8
{
	/// Today's behaviour, and the only road that leaves VMA's selection alone: HOST_VISIBLE
	/// required, HOST_COHERENT preferred, whatever that resolves to. Write-combined on Turnip.
	WriteCombined,
	/// A device-local type that is both cached and coherent. No flush: coherent means the GPU sees
	/// the stores without one, so CommitMemory's vmaFlushAllocation stays the no-op it has always
	/// been.
	CachedCoherent,
	/// A device-local cached type that is NOT coherent, with CommitMemory's flush now live -- a
	/// real cache clean over the range just written, once per commit.
	CachedNonCoherent,
};

struct GSStreamRingMemoryInputs
{
	/// The device's memory types in index order, each as a mask of the GS_MEMORY_PROPERTY_ bits
	/// above. Index order matters: VMA breaks ties by taking the lowest index, and this policy
	/// reproduces that so the index it predicts is the one the rings actually get.
	const u32* type_flags = nullptr;
	u32 type_count = 0;

	/// The driver database says CPU writes into a write-combined ring are expensive on this GPU
	/// (DriverWorkaround::PreferCachedStreamRingMemory). Only consulted when no cached coherent
	/// type exists -- a device that has one takes it whatever the database thinks, because that
	/// road costs no flushes and was measured on the SD865.
	bool prefer_cached_over_write_combined = false;
};

struct GSStreamRingMemoryDecision
{
	GSStreamRingMemoryRoad road = GSStreamRingMemoryRoad::WriteCombined;

	/// The memory type index the rings are expected to land on. Predicted, not commanded: the
	/// flags below are what VMA is actually asked for, and VKStreamBuffer prints the index VMA
	/// returned and complains if the two disagree. GS_INVALID_MEMORY_TYPE when the device offers no
	/// host-visible type at all, which is a device that cannot run this backend.
	u32 type_index = GS_INVALID_MEMORY_TYPE;

	/// Property bits added to VMA's required set on top of the HOST_VISIBLE that
	/// VMA_MEMORY_USAGE_CPU_TO_GPU already requires. Zero on the write-combined road, which is
	/// what makes that road bit-for-bit the selection every device had before this policy.
	u32 extra_required_flags = 0;
};

/// VMA's own type selection, reproduced: among the types carrying every required bit, the one
/// missing the fewest preferred bits wins, and a tie goes to the lower index
/// (3rdparty/vulkan/include/vk_mem_alloc.h, VmaAllocator_T::FindMemoryTypeIndex). Used to predict
/// which index a set of flags will resolve to, so the banner can name it before any ring exists.
constexpr u32 GSPickStreamRingMemoryType(const GSStreamRingMemoryInputs& in, u32 required, u32 preferred)
{
	u32 best_index = GS_INVALID_MEMORY_TYPE;
	u32 best_cost = 0;
	for (u32 i = 0; i < in.type_count; i++)
	{
		const u32 flags = in.type_flags[i];
		if ((flags & required) != required)
			continue;

		u32 cost = 0;
		for (u32 bit = 1; bit <= GS_MEMORY_PROPERTY_HOST_CACHED; bit <<= 1)
		{
			if ((preferred & bit) != 0 && (flags & bit) == 0)
				cost++;
		}

		if (best_index == GS_INVALID_MEMORY_TYPE || cost < best_cost)
		{
			best_index = i;
			best_cost = cost;
		}
	}
	return best_index;
}

/// Which memory the stream rings should be allocated from on this device.
constexpr GSStreamRingMemoryDecision GSDecideStreamRingMemory(const GSStreamRingMemoryInputs& in)
{
	GSStreamRingMemoryDecision decision;

	// (a) Cached AND coherent AND device-local. Free on the CPU side and free on the GPU side, so
	// it is taken wherever it exists without asking the database anything. On the M2 under
	// MoltenVK this is memory type 0, which is the type the rings already have -- the decision
	// changes nothing there, which is what makes that host's identity grid meaningful.
	constexpr u32 cached_coherent = GS_MEMORY_PROPERTY_DEVICE_LOCAL | GS_MEMORY_PROPERTY_HOST_VISIBLE |
									GS_MEMORY_PROPERTY_HOST_COHERENT | GS_MEMORY_PROPERTY_HOST_CACHED;
	const u32 coherent_index = GSPickStreamRingMemoryType(in, cached_coherent, cached_coherent);
	if (coherent_index != GS_INVALID_MEMORY_TYPE)
	{
		decision.road = GSStreamRingMemoryRoad::CachedCoherent;
		decision.type_index = coherent_index;
		decision.extra_required_flags = cached_coherent;
		return decision;
	}

	// (b) Cached, device-local, not coherent -- the MQ65's road, and only on a device the database
	// names. The cost is a cache clean per commit; the gain is that the stores themselves become
	// ordinary cached stores instead of uncached ones the store buffer has to merge.
	constexpr u32 cached_only =
		GS_MEMORY_PROPERTY_DEVICE_LOCAL | GS_MEMORY_PROPERTY_HOST_VISIBLE | GS_MEMORY_PROPERTY_HOST_CACHED;
	if (in.prefer_cached_over_write_combined)
	{
		const u32 cached_index =
			GSPickStreamRingMemoryType(in, cached_only, cached_only | GS_MEMORY_PROPERTY_HOST_COHERENT);
		if (cached_index != GS_INVALID_MEMORY_TYPE)
		{
			decision.road = GSStreamRingMemoryRoad::CachedNonCoherent;
			decision.type_index = cached_index;
			decision.extra_required_flags = cached_only;
			return decision;
		}
	}

	// (c) What every device did before this policy: HOST_VISIBLE required, HOST_COHERENT and
	// DEVICE_LOCAL preferred, VMA's choice. Nothing is added to the required set, so this road is
	// the old selection and not a reconstruction of it.
	decision.type_index = GSPickStreamRingMemoryType(in, GS_MEMORY_PROPERTY_HOST_VISIBLE,
		GS_MEMORY_PROPERTY_HOST_COHERENT | GS_MEMORY_PROPERTY_DEVICE_LOCAL);
	return decision;
}

/// The property bits of one memory type, spelled out. Written rather than printed as hex because
/// the whole point of naming a type in a log is that a reader can tell at a glance whether the ring
/// is cached or write-combined.
inline std::string GSDescribeMemoryProperties(u32 flags)
{
	static constexpr struct
	{
		u32 bit;
		const char* name;
	} names[] = {
		{GS_MEMORY_PROPERTY_DEVICE_LOCAL, "DEVICE_LOCAL"},
		{GS_MEMORY_PROPERTY_HOST_VISIBLE, "HOST_VISIBLE"},
		{GS_MEMORY_PROPERTY_HOST_COHERENT, "HOST_COHERENT"},
		{GS_MEMORY_PROPERTY_HOST_CACHED, "HOST_CACHED"},
		{0x0010, "LAZILY_ALLOCATED"},
	};

	std::string str;
	for (const auto& entry : names)
	{
		if (!(flags & entry.bit))
			continue;
		if (!str.empty())
			str.push_back('|');
		str.append(entry.name);
	}
	return str.empty() ? std::string("none") : str;
}

/// The road's name, for the device banner and for stats.json's run block. Short and stable: a
/// round is audited by grepping for these.
constexpr const char* GSStreamRingMemoryRoadName(GSStreamRingMemoryRoad road)
{
	switch (road)
	{
		case GSStreamRingMemoryRoad::CachedCoherent:
			return "cached-coherent";
		case GSStreamRingMemoryRoad::CachedNonCoherent:
			return "cached-noncoherent";
		case GSStreamRingMemoryRoad::WriteCombined:
		default:
			return "write-combined";
	}
}
