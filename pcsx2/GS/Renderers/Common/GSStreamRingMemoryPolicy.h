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
// devs/bmdhacks/campaigns/gs-classic-tiler/sd662-tier/):
//
//   * MQ65 (Adreno 610, Turnip 26.1.2) offers NO cached coherent host-visible type. Moving the
//     rings to the cached NON-coherent type, paying the per-region flush CommitMemory already
//     issues, took GS-thread p50 down 29.5% on legosw, 28.4% on gow2, 21.0% on yugioh and 20.6%
//     on ac5. Draw-heavy controls flat, 24 of 24 frame grids byte-identical.
//   * RG 477V (MT6897, Mali-G615) reports the MQ65's memory table bit for bit and gives the
//     opposite answer: its cached NON-coherent type made every title slower, +2.6% (legosw) to
//     +12.4% (ac5), scaling with the flush count.
//   * SD865 (Adreno 650, same driver) DOES offer a cached coherent type, and it is NOT free. The
//     keyed round (one binary, runtime key on/off) read gt4opb -13.8% against legosw +5.3% and
//     ac5 +3.6%, and the -13.8% was taken as the headline. The keyless confirmation round -- two
//     real binaries, pre-policy 8a836e93b9 against 535200c107, banners verified on every run,
//     n=3 + warm, loop 10 -- reproduced the two regressions and not the win: legosw +6.16% and
//     ac5 +5.54% with non-overlapping rep ranges across three reps, gow2 +3.03% borderline,
//     gt4opb -1.08% inside a 5.98% policy-arm spread. Two losses that replicate under two
//     independent methods, one win that replicates under neither.
//   * The store INSTRUCTION is not the story. Replacing the storent (STNP) copy with memcpy moved
//     nothing anywhere, on any device, with or without the memory-type change.
//
// So: write-combined is the default everywhere, and the driver database's
// PreferCachedStreamRingMemory bit is what grants a cached road at all. Given the bit, the memory
// table picks WHICH cached road: coherent if the device has such a type, non-coherent otherwise.
//
// ⚠️ Neither cached road is a shape the policy may infer. That was the original mistake and it is
// easy to make twice, because each road has an argument for why it should be free. The
// non-coherent road TRADES -- cached stores in exchange for a cache clean per commit -- and
// whether the trade pays is a property of the host's cores and its cache maintenance, not of the
// memory table: the MQ65 and the RG 477V satisfy "has no cached coherent type" identically and
// land 30% apart in opposite directions. The coherent road pays no flush at all, which is why it
// was originally taken without asking, and the SD865 still lost two titles on it reproducibly for
// a reason nobody has named. So the database bit means "measured, on this part, and it won", for
// both roads. A device with a cached coherent type and no rule stays write-combined; the A650 is
// that device today.
//
// ⚠️ Both cached roads require DEVICE_LOCAL, and that requirement is load-bearing on desktop. On a
// discrete GPU a HOST_VISIBLE|HOST_COHERENT|HOST_CACHED type is ordinary system RAM, and moving
// the rings there would send every vertex, index, uniform and texture staging byte across PCIe on
// the GPU's side of the read. Today's HOST_COHERENT-only ask lands on the device-local BAR type
// there, which is the right answer for memory the GPU reads. A device whose only cached
// host-visible types are host-local therefore stays on the write-combined road even with the bit
// set.
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
	/// The default on every device, and the only road that leaves VMA's selection alone:
	/// HOST_VISIBLE required, HOST_COHERENT preferred, whatever that resolves to. Write-combined
	/// on Turnip.
	WriteCombined,
	/// A device-local type that is both cached and coherent. No flush: coherent means the GPU sees
	/// the stores without one, so CommitMemory's vmaFlushAllocation stays the no-op it has always
	/// been. Costing nothing on paper is not the same as costing nothing, so this road is granted
	/// by the database bit like the other one.
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

	/// The driver database has MEASURED this part and found a cached road faster than the
	/// write-combined one (DriverWorkaround::PreferCachedStreamRingMemory). Without it the rings
	/// stay write-combined whatever the memory table offers; with it, the table decides which
	/// cached road. Never infer this bit from the memory table: the RG 477V's table is the MQ65's
	/// and its answer is the opposite, and the SD865 lost two titles on the road its table says it
	/// should want.
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

	constexpr u32 cached_coherent = GS_MEMORY_PROPERTY_DEVICE_LOCAL | GS_MEMORY_PROPERTY_HOST_VISIBLE |
									GS_MEMORY_PROPERTY_HOST_COHERENT | GS_MEMORY_PROPERTY_HOST_CACHED;
	constexpr u32 cached_only =
		GS_MEMORY_PROPERTY_DEVICE_LOCAL | GS_MEMORY_PROPERTY_HOST_VISIBLE | GS_MEMORY_PROPERTY_HOST_CACHED;

	// (a) The bit is what opens either cached road. No bit, no cached memory, whatever the table
	// says it could offer -- which is what every device did before this policy existed, so a
	// device with no rule is bit for bit where it was.
	if (in.prefer_cached_over_write_combined)
	{
		// (a1) Cached AND coherent AND device-local, preferred over the non-coherent type because
		// it gets the cached stores without the cache clean. Nothing on this desk takes this road
		// today: the A650 is the only part that has such a type and no rule names it.
		const u32 coherent_index = GSPickStreamRingMemoryType(in, cached_coherent, cached_coherent);
		if (coherent_index != GS_INVALID_MEMORY_TYPE)
		{
			decision.road = GSStreamRingMemoryRoad::CachedCoherent;
			decision.type_index = coherent_index;
			decision.extra_required_flags = cached_coherent;
			return decision;
		}

		// (a2) Cached, device-local, not coherent -- the MQ65's road. The cost is a cache clean per
		// commit; the gain is that the stores become ordinary cached stores instead of uncached
		// ones the store buffer has to merge. Which is larger is not visible from here, which is
		// why a device gets here only by being named.
		const u32 cached_index =
			GSPickStreamRingMemoryType(in, cached_only, cached_only | GS_MEMORY_PROPERTY_HOST_COHERENT);
		if (cached_index != GS_INVALID_MEMORY_TYPE)
		{
			decision.road = GSStreamRingMemoryRoad::CachedNonCoherent;
			decision.type_index = cached_index;
			decision.extra_required_flags = cached_only;
			return decision;
		}

		// A named device with nothing cached and device-local to move to. Falls through: the rings
		// still have to live somewhere, and where they already are is the answer.
	}

	// (b) HOST_VISIBLE required, HOST_COHERENT and DEVICE_LOCAL preferred, VMA's choice. Nothing is
	// added to the required set, so this road is the old selection and not a reconstruction of it.
	// On the M2 dev box (Honeykrisp, Mesa 25.3.6) the single host-visible type is cached, coherent
	// and device-local, so this resolves to memory type 0 there either way -- the decision cannot
	// move that host, which is what makes its identity grid a gate on the code rather than a
	// measurement of the road.
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
