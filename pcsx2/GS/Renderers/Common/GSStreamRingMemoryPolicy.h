// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Which host-visible memory the six Vulkan stream rings live in.
//
// The rings -- vertex, index, expand-index, VS uniform, PS uniform, texture upload -- are written by
// the CPU, read by the GPU, and never read back. VMA's default pick (HOST_VISIBLE, HOST_COHERENT
// preferred) is write-combined uncached memory on Turnip. Uncached stores are only fast when the
// core's store buffer merges them into full bursts, and small cores merge badly.
//
// Write-combined is the default everywhere. The driver database's PreferCachedStreamRingMemory bit
// is what grants a cached road; given the bit, the memory table picks coherent if the device has
// such a type, non-coherent otherwise.
//
// ⚠️ Never infer a cached road from the memory table. The non-coherent road trades cached stores
// for a cache clean per commit, and whether that pays depends on the host's cores and cache
// maintenance: MQ65 (Adreno 610) and RG 477V (Mali-G615) report identical memory tables and moved
// in opposite directions. The coherent road has no flush, yet SD865 (Adreno 650) reproducibly lost
// on it. So the bit means "measured on this part, and it won", for both roads. A device with a
// cached coherent type and no rule stays write-combined.
//
// ⚠️ Both cached roads require DEVICE_LOCAL. On a discrete GPU a HOST_VISIBLE|HOST_COHERENT|
// HOST_CACHED type is system RAM, and the rings would cross PCIe on every GPU read; the default ask
// lands on the device-local BAR type there. A device whose only cached host-visible types are
// host-local stays write-combined even with the bit set.
//
// Pure function of the memory-type table and one database bit, so every road can be pinned without
// the device. See gs_stream_ring_memory_tests.cpp.

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
	/// The default. Leaves VMA's selection alone: HOST_VISIBLE required, HOST_COHERENT preferred.
	/// Write-combined on Turnip.
	WriteCombined,
	/// A device-local cached coherent type. CommitMemory's vmaFlushAllocation is a no-op. Still
	/// granted only by the database bit.
	CachedCoherent,
	/// A device-local cached non-coherent type. The written range must be cleaned before the GPU
	/// reads it (see GSStreamRingFlushRange.h).
	CachedNonCoherent,
};

struct GSStreamRingMemoryInputs
{
	/// The device's memory types in index order, as GS_MEMORY_PROPERTY_ masks. Order matters: VMA
	/// breaks ties on the lowest index, and this policy reproduces that.
	const u32* type_flags = nullptr;
	u32 type_count = 0;

	/// DriverWorkaround::PreferCachedStreamRingMemory: a cached road was measured faster on this
	/// part. Without it the rings stay write-combined; with it, the table picks which cached road.
	/// Never infer this from the memory table (see above).
	bool prefer_cached_over_write_combined = false;
};

struct GSStreamRingMemoryDecision
{
	GSStreamRingMemoryRoad road = GSStreamRingMemoryRoad::WriteCombined;

	/// The memory type index the rings are expected to land on. Predicted, not commanded: VMA is
	/// asked for the flags below, and VKStreamBuffer warns if its result differs.
	/// GS_INVALID_MEMORY_TYPE when the device has no host-visible type at all.
	u32 type_index = GS_INVALID_MEMORY_TYPE;

	/// Bits added to VMA's required set beyond the HOST_VISIBLE of VMA_MEMORY_USAGE_CPU_TO_GPU.
	/// Zero on the write-combined road, leaving VMA's selection unchanged.
	u32 extra_required_flags = 0;
};

/// VMA's own type selection, reproduced: among the types carrying every required bit, the one
/// missing the fewest preferred bits wins, and a tie goes to the lower index
/// (3rdparty/vulkan/include/vk_mem_alloc.h, VmaAllocator_T::FindMemoryTypeIndex). Used to predict
/// which index a set of flags will resolve to, before any ring exists.
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

	// (a) Only the database bit opens a cached road.
	if (in.prefer_cached_over_write_combined)
	{
		// (a1) Cached, coherent, device-local: preferred, since it needs no cache clean.
		const u32 coherent_index = GSPickStreamRingMemoryType(in, cached_coherent, cached_coherent);
		if (coherent_index != GS_INVALID_MEMORY_TYPE)
		{
			decision.road = GSStreamRingMemoryRoad::CachedCoherent;
			decision.type_index = coherent_index;
			decision.extra_required_flags = cached_coherent;
			return decision;
		}

		// (a2) Cached, device-local, not coherent: cached stores at the cost of a cache clean.
		const u32 cached_index =
			GSPickStreamRingMemoryType(in, cached_only, cached_only | GS_MEMORY_PROPERTY_HOST_COHERENT);
		if (cached_index != GS_INVALID_MEMORY_TYPE)
		{
			decision.road = GSStreamRingMemoryRoad::CachedNonCoherent;
			decision.type_index = cached_index;
			decision.extra_required_flags = cached_only;
			return decision;
		}

		// Nothing cached and device-local: fall through to the default.
	}

	// (b) HOST_VISIBLE required, HOST_COHERENT and DEVICE_LOCAL preferred; nothing added to the
	// required set, so this is VMA's own selection. On Honeykrisp the only host-visible type is
	// cached, coherent and device-local, so the decision cannot move that host.
	decision.type_index = GSPickStreamRingMemoryType(in, GS_MEMORY_PROPERTY_HOST_VISIBLE,
		GS_MEMORY_PROPERTY_HOST_COHERENT | GS_MEMORY_PROPERTY_DEVICE_LOCAL);
	return decision;
}

/// The road's name, for the device banner. The unit tests pin these strings.
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
