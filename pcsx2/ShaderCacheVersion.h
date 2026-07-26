// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/// Version number for GS and other shaders. Increment whenever any of the contents of the
/// shaders change, to invalidate the cache.
// 109: driver-workaround shader wrappers (gpu_bitwise_and / gpu_bitwise_not / gpu_boolean_not /
// gpu_matrix_element). Every TFX and convert shader's source text changed, so a cached blob from
// 108 no longer matches the source that produced it — leaving this alone hands users stale
// binaries and garbage rendering after the update.
static constexpr u32 SHADER_CACHE_VERSION = 109; // 108 was upstream PR 14688
