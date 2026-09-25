// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Timer.h"

#include <atomic>

/// Cumulative counters for shader and pipeline compilation, split by stage, so a hitch can be
/// attributed to source generation, SPIR-V compilation, module creation or pipeline creation.
/// Never reset: a reader takes deltas. Incremented from whichever thread does the work, so the
/// stage counters include work done on compile workers; GSThreadStallNs is the part the GS thread
/// actually waited for.
namespace GSCompileStats
{
	enum Counter : u32
	{
		ShaderSources, ///< TFX shader source strings assembled (a module cache miss).
		ShaderSourceNs,
		SpirvCompiles, ///< GLSL to SPIR-V compilations (a SPIR-V cache miss).
		SpirvCompileNs,
		SpirvCacheHits, ///< SPIR-V read back from the on-disk cache instead of compiled.
		ModuleCreateNs, ///< vkCreateShaderModule.
		PipelineCreates, ///< vkCreate*Pipelines calls.
		PipelineCreateNs,
		TFXPipelineMisses, ///< Draws whose TFX pipeline was not in the in-memory map.
		GSThreadStallNs, ///< GS thread time spent obtaining a TFX pipeline it did not have.
		CacheFlushNs, ///< Pipeline cache serialisation.
		PrecompileWaits, ///< Draws that waited for a pipeline a precompile worker was building.
		PrecompileWaitNs,
		PrecompileBuilt, ///< Pipelines precompile workers finished.
		Count
	};

	inline std::atomic<u64> s_counters[Count] = {};

	inline void Add(Counter c, u64 v) { s_counters[c].fetch_add(v, std::memory_order_relaxed); }
	inline u64 Get(Counter c) { return s_counters[c].load(std::memory_order_relaxed); }

	struct ScopedTimer
	{
		Counter counter;
		Common::Timer::Value start;

		explicit ScopedTimer(Counter c)
			: counter(c)
			, start(Common::Timer::GetCurrentValue())
		{
		}
		~ScopedTimer()
		{
			Add(counter, static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - start)));
		}

		ScopedTimer(const ScopedTimer&) = delete;
		ScopedTimer& operator=(const ScopedTimer&) = delete;
	};
} // namespace GSCompileStats
