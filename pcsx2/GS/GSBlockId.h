// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

/// The identity of one 256-byte block of GS local memory, for keys that live inside one run: the per-block hash
/// cache of the texture cache, the texture keys of lookups that are not dumped, replaced or upscaled, and the burst
/// memo's block checks. Equal bytes give equal values; different bytes give different values except with odds of
/// about 2^-64 per pair compared. The value depends on the CPU (which instructions it has) and on nothing else, so it
/// must never be written anywhere or used to name anything: the texture names of dumps and replacement packs are the
/// XXH3 texel hash and are made elsewhere (HashCacheKey::Create with raw_blocks false).
///
/// Three implementations. The ones this CPU has are timed against each other once, at the first use (the GS thread),
/// and the fastest is used for the rest of the run; AES is kept unless another is more than a tenth faster. The choice
/// never changes during a run, even if the thread later moves to another kind of core.
///   AES   four lanes, two AES rounds per 16 bytes (AESE+AESMC with the data as the first round key, a constant as
///         the second), the lanes chained into one state and taken through two more rounds, the halves folded to 64
///         bits. Each round is a bijection of the state for fixed data and of the data for a fixed state, so blocks
///         that differ in one 16-byte word always end in different states; for more words to cancel, a later word's
///         change would have to equal a full 16-byte difference spread by two rounds. The odds of equal 64-bit values
///         are then those of a random function, about 2^-64: a heuristic, the same model as the XXH3 it replaces,
///         which has no proof either.
///   CRC   CRC32 and CRC32C over the even 8-byte words and again over the odd ones, the two 64-bit halves combined by
///         a bijective mix. Every word is covered by 64 bits of check (the product of the two generator polynomials),
///         so a difference inside one half is missed only if it is a multiple of a degree-64 polynomial.
///   XXH3  XXH3_64bits of the block (other CPUs, and x86).
namespace GSBlockId
{
	enum class Kind : u8
	{
		AES,
		CRC,
		XXH3,
	};

	/// The implementation this run uses (chosen at the first call of any function here).
	Kind Selected();

	/// For tests only: use `kind` from now on, if this CPU has it. Identities taken before (in caches) are then from
	/// another function, so a test that forces must not mix the two. False if the CPU lacks it.
	bool ForceForTests(Kind kind);
	/// For tests only: the time the startup choice measured for `kind` (ns for 128 blocks), 0 if not timed.
	u64 TimedNanosecondsForTests(Kind kind);
	const char* KindName(Kind kind);

	/// The identity of the 256 bytes at `block`.
	u64 Hash(const void* block);

	/// The identity of the block whose 32-bit words are those at `block` ANDed with `mask` (the H formats and the
	/// 24-bit ones, whose other bits belong to other data).
	u64 HashMasked(const void* block, u32 mask);

	/// out[i] = HashMasked(blocks[i], mask) for each of `count` blocks (Hash() where the mask is all ones), in one call.
	void HashMany(const u8* const* blocks, u64* out, u32 count, u32 mask);

	/// One value for a sequence of identities, in order: equal sequences give equal values, different ones (in any
	/// identity, the order or the length) different values except with odds of about 2^-64. Inside one run only,
	/// like the identities.
	u64 Combine(const u64* ids, u32 count);

	/// The identity of a block filled with `word`: equal to Hash() of such a block.
	u64 HashFill(u32 word);

	/// For tests: the identity with a given implementation, if this CPU has it. False if it does not.
	bool HashWith(Kind kind, const void* block, u64* out);
} // namespace GSBlockId
