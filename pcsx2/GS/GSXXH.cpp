// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MultiISA.h"

// These get pulled in by xxhash.h in non-PCH mode, so we need to include them in global namespace scope.
#include <cmath>
#include <cstdlib>
#include <cstring>

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
namespace CURRENT_ISA // XXH doesn't seem to use symbols that allow the compiler to deduplicate, but just in case...
{
#include <xxhash.h>
}

MULTI_ISA_UNSHARED_IMPL;

// Include this after xxhash so we can add namespaces (GSXXH is set up to not include xxhash header if it's already been included)
#include "GSXXH.h"

u64 __noinline CURRENT_ISA::GSXXH3_64_Long(const void* data, size_t len)
{
	// XXH marks its function that calls this noinline, and it would be silly to stack noinline functions, so call the internal function directly
	return XXH3_hashLong_64b_internal(data, len, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_accumulate, XXH3_scrambleAcc);
}

// A GS block is XXH3's whole internal buffer, so after any update of a block the buffer holds exactly
// that block and nothing has consumed it yet. GSXXH3_64_Block() reproduces the state XXH3_update()
// leaves, without ever filling the buffer: the block is only remembered, and its stripes are
// consumed from the caller's memory when the next block arrives.
static constexpr size_t BlockSize = 256;
static_assert(BlockSize == XXH3_INTERNALBUFFER_SIZE, "a GS block must be the size of XXH3's internal buffer");

// Put the remembered block where XXH3_update() would have left it, so the plain XXH3 functions can continue from it.
static void FlushPendingBlock(GSXXH3BlockState* h)
{
	if (h->pending)
	{
		memcpy(h->state.buffer, h->pending, BlockSize);
		h->state.bufferedSize = BlockSize;
		h->pending = nullptr;
	}
}

void CURRENT_ISA::GSXXH3_64_Block(void* hasher, const void* block)
{
	GSXXH3BlockState* h = static_cast<GSXXH3BlockState*>(hasher);
	XXH3_state_t* st = &h->state;

	if (h->pending)
	{
		// XXH3_update() on a full buffer consumes the buffer and then buffers the new block.
		const unsigned char* secret = (st->extSecret == nullptr) ? st->customSecret : st->extSecret;
		XXH3_consumeStripes(st->acc, &st->nbStripesSoFar, st->nbStripesPerBlock,
			h->pending, BlockSize / XXH_STRIPE_LEN, secret, st->secretLimit, XXH3_accumulate, XXH3_scrambleAcc);
	}
	else if (st->totalLen != 0)
	{
		// Something other than a run of blocks has been fed, so the buffer is not in the state this
		// path assumes. Let XXH3 deal with it.
		XXH3_64bits_update(st, block, BlockSize);
		return;
	}

	// Nothing is buffered and nothing consumed yet, or the previous block was just consumed.
	h->pending = static_cast<const xxh_u8*>(block);
	st->totalLen += BlockSize;
}

u32 CURRENT_ISA::GSXXH3_64_Update(void* hasher, const void* data, size_t len)
{
	GSXXH3BlockState* h = static_cast<GSXXH3BlockState*>(hasher);
	FlushPendingBlock(h);
	return XXH3_64bits_update(&h->state, static_cast<const xxh_u8*>(data), len);
}

u64 CURRENT_ISA::GSXXH3_64_Digest(void* hasher)
{
	GSXXH3BlockState* h = static_cast<GSXXH3BlockState*>(hasher);
	FlushPendingBlock(h);
	return XXH3_64bits_digest(&h->state);
}
