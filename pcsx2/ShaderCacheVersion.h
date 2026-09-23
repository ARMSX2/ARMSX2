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
// 116: upstream PR 14824, the PrimID DATE init shaders take PRIMID_MIN/MAX defines.
// 117: upstream PR 14743, ps_fbmask reads the destination alpha in the RTA-scaled domain and
// ROV channel masking goes through FBMASK.
// 118: the four deinterlace shaders and the TFX scan-mask test reduce a device row to its native
// line before testing field parity, so interlace.* gains a constant-buffer field and tfx.* changes.
// 119: the TFX dither test (ps_dither, PS_DITHER == 1) gets the same device-row/column-to-native
// fix as 118's scan mask, on both axes, so tfx.* changes again.
// 120: 118's native-line deinterlace is reverted -- interlace.* is back to testing field parity on
// the device row -- and the constant buffer's second vector now carries the undrawn top band.
// 121: the dither index change of 119 is reverted after a visual comparison; tfx.* changes back.
// 122: scaled dither indexes the native pixel again, and the matrix is rotated under that index by
// a per-axis phase the CPU picks, so tfx.* changes and the PS constant buffer's last pad is named.
// 123: a sprite that minifies a GS-memory texture under a nearest sampler reads the texel its
// native pixel read, so tfx.* gains PS_NATIVE_TEXEL_GRID and the PS constant buffer gains a vector.
// 124: the TFX dither index and scan-mask test divide by the render target's scale, carried in the
// PS constant buffer's former pad after RcpScaleFactor, instead of the texture's.
// 125: the weave and MAD buffering passes fill the undrawn field band where the display rect
// starts, read from FieldPad.xy as a row range, instead of from row 0 down to a row count.
static constexpr u32 SHADER_CACHE_VERSION = 125; // 108 was upstream PR 14688; their 109 = our 111, their 110 = our 115, their 112 = our 116, their 113 = our 117
