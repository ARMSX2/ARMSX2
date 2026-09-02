// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/// Version number for GS and other shaders. Increment whenever any of the contents of the
/// shaders change, to invalidate the cache.
// 109: driver-workaround shader wrappers (gpu_bitwise_and / gpu_bitwise_not / gpu_boolean_not /
// gpu_matrix_element). Every TFX and convert shader's source text changed, so a cached blob from
// 108 no longer matches the source that produced it — leaving this alone hands users stale
// binaries and garbage rendering after the update.
// 110: Vulkan emits the gpu_bitwise_and / gpu_matrix_element wrappers as bare #defines when no
// driver workaround is active, so unaffected drivers get the same SPIR-V they had at 108. 109 wrapped
// them in real functions on EVERY driver, and Qualcomm's SPIR-V compiler segfaults compiling a TFX
// pipeline containing those calls.
// 111: the 2026-08 upstream sync drops the unused SW_DEPTH term from the Vulkan TFX ZWRITE
// condition, changing that shader's source text. ⚠️ Upstream numbered the same change 109, which
// is BELOW our 110 — taking their value would hand every user a stale blob for a source they no
// longer have. Our counter has been ahead of theirs since 109 and cannot be resynced by adopting
// their numbers; always bump past our own last value.
// 112: PS_QUANTIZE_COLOR. Every TFX shader gains the define and the colour-clamp block's guard
// gains a term, so the source text of every TFX permutation changed.
// 113: PS_SUBSTITUTE_ALPHA. Every TFX shader gains the define, two constant-buffer pad words
// become named fields, and the colour-clamp block's guard gains another term.
// 114: PS_AF_IN_SRC1. The Vulkan TFX shader gains the define and a block that overrides
// alpha_blend with the fixed AFIX value, so its source text changed after 113 was set.
// 115: upstream PR 14897, the depth conversion shaders floor the bilinear result.
static constexpr u32 SHADER_CACHE_VERSION = 115; // 108 was upstream PR 14688; their 109 = our 111, their 110 = our 115
