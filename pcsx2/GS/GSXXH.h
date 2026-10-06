// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MultiISA.h"

#ifndef XXH_versionNumber
	#define XXH_STATIC_LINKING_ONLY 1
	#define XXH_INLINE_ALL 1
	#include <xxhash.h>
#endif

/// XXH3 state for hashing a run of GS blocks that are scattered in memory.
///
/// XXH3's streaming update copies every 256-byte input into its internal 256-byte buffer before it
/// consumes it, and a block is exactly that size, so hashing a texture block by block copied the whole
/// texture. This state holds a pointer to the last block instead and consumes it from there when the
/// next one (or anything else) arrives. The values it produces are the ones XXH3 gives for the same
/// bytes hashed in one piece.
///
/// The block handed to GSXXH3_64bits_block() must stay unchanged until the next call on the state.
struct GSXXH3BlockState
{
	XXH3_state_t state;
	const u8* pending; ///< Last block, counted in state.totalLen but not yet consumed. state.bufferedSize is 0 while set.
};

MULTI_ISA_DEF(u64 GSXXH3_64_Long(const void* data, size_t len);)
MULTI_ISA_DEF(void GSXXH3_64_Block(void* hasher, const void* block);)
MULTI_ISA_DEF(u32 GSXXH3_64_Update(void* hasher, const void* data, size_t len);)
MULTI_ISA_DEF(u64 GSXXH3_64_Digest(void* hasher);)

static inline u64 __forceinline GSXXH3_64bits(const void* data, size_t len)
{
	// XXH3 has optimized functions for small inputs and they aren't vectorized
	if (len <= XXH3_MIDSIZE_MAX)
		return XXH3_64bits(data, len);
	return MultiISAFunctions::GSXXH3_64_Long(data, len);
}

static inline void __forceinline GSXXH3_block_reset(GSXXH3BlockState& hasher)
{
	XXH3_64bits_reset(&hasher.state);
	hasher.pending = nullptr;
}

/// Hash the next 256 bytes of the stream, read from `block`.
static inline void __forceinline GSXXH3_64bits_block(GSXXH3BlockState* hasher, const void* block)
{
	MultiISAFunctions::GSXXH3_64_Block(static_cast<void*>(hasher), block);
}

/// Hash the next `len` bytes of the stream, of any length.
static inline XXH_errorcode __forceinline GSXXH3_64bits_update(GSXXH3BlockState* hasher, const void* input, size_t len)
{
	// XXH3 update has no optimized functions for small inputs
	return static_cast<XXH_errorcode>(MultiISAFunctions::GSXXH3_64_Update(static_cast<void*>(hasher), input, len));
}

/// The hash of everything fed so far. The state can take more input afterwards.
static inline u64 __forceinline GSXXH3_64bits_digest(GSXXH3BlockState* hasher)
{
	return MultiISAFunctions::GSXXH3_64_Digest(static_cast<void*>(hasher));
}
