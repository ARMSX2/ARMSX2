// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The VU1 chunk store. What it is and when it is used: microVU_ChunkStore-arm64.h.
// The types are in microVU-arm64.h, next to the compile-time state they are
// built from.

#include "vuaot/VuAotMvuTrace.h"

void mVUsetupRange(microVU& mVU, s32 pc, bool isStartPC);
static void mVUaddKeptBits(microVU& mVU, u32 addr, u32 kept);

static mVUChunkStore::Stats s_mVUchunkStats;

mVUChunkStore::Stats mVUChunkStore::GetStats()
{
	mVUChunkStore::Stats s = s_mVUchunkStats;
	if (const mVUChunkStoreData* store = microVU1.chunkStore.get())
	{
		s.entries = store->entries.size();
		s.entryCodeBytes = store->codeBytes;
	}
	return s;
}

static void mVUchunkStoreClear(microVU& mVU)
{
	if (mVUChunkStoreData* store = mVU.chunkStore.get())
	{
		store->entries.clear();
		store->index.clear();
		store->codeBytes = 0;
	}
}

static void mVUchunkStoreDumpStats(const microVU& mVU, const char* tag)
{
	const mVUChunkStore::Stats s = mVUChunkStore::GetStats();
	if (!s.episodes)
		return;
	Console.WriteLn(Color_StrongGreen,
		"mVUChunkStore[%u] %s: episodes=%llu hits=%llu compiles=%llu skipped=%llu "
		"miss(noEntry/bounds/readSet/blockExists/linkTarget/noRoom)=%llu/%llu/%llu/%llu/%llu/%llu "
		"feed(added/repeats/wrapped/other)=%llu/%llu/%llu/%llu entries=%llu "
		"codeKiB(compiled/installed/stubs/entries)=%llu/%llu/%llu/%llu cacheUsedKiB=%llu slots=%u",
		mVU.index, tag, (unsigned long long)s.episodes, (unsigned long long)s.hits,
		(unsigned long long)(s.episodes - s.hits), (unsigned long long)s.skipped,
		(unsigned long long)s.missNoEntry, (unsigned long long)s.missBounds, (unsigned long long)s.missReadSet,
		(unsigned long long)s.missBlockExists, (unsigned long long)s.missLinkTarget, (unsigned long long)s.missNoRoom,
		(unsigned long long)s.fed, (unsigned long long)s.feedRepeats, (unsigned long long)s.feedWrapped,
		(unsigned long long)s.feedOther, (unsigned long long)s.entries,
		(unsigned long long)(s.codeBytesCompiled / 1024), (unsigned long long)(s.codeBytesInstalled / 1024),
		(unsigned long long)(s.stubBytes / 1024), (unsigned long long)(s.entryCodeBytes / 1024),
		(unsigned long long)((mVU.prog.x86ptr - mVU.prog.x86start) / 1024), mVU.placeSlotsUsed);
}

// The entries were compiled under one set of options; an entry made under
// another set is other code.
static void mVUchunkStoreSyncOptions(microVU& mVU)
{
	mVUChunkStoreData& store = *mVU.chunkStore;
	if (store.options.low64 == mVU.optionsSentinel.low64 && store.options.high64 == mVU.optionsSentinel.high64)
		return;
	mVUchunkStoreClear(mVU);
	store.options = mVU.optionsSentinel;
}

// The bits of a lower instruction word the compiler reads at compile time.
// Under the IbitHack gamefix the immediates of these ops are read from micro
// memory when the block runs (mVUemitLoadCurI).
static u32 mVUchunkLowerMask(u32 lower)
{
	if (!EmuConfig.Gamefixes.IbitHack)
		return ~0u;
	const u32 kept = mVUibitHackKeptBits(lower);
	return kept ? kept : ~0u;
}

// The mask of the root pair's lower word in the index key: every bit the
// compile is certain to have read.
static u32 mVUchunkRootLowerMask(u32 lower, u32 upper)
{
	if (upper & _Ibit_)
		return EmuConfig.Gamefixes.IbitHack ? 0u : ~0u;
	return mVUchunkLowerMask(lower);
}

// The index key: the root's entry state and the root pair. The root pair is
// the one pair every compile from this root reads; the rest of the read set is
// compared word by word.
static u64 mVUchunkRootHash(microVU& mVU, u32 rootPC, const microRegInfo* state)
{
	const u32* mem = reinterpret_cast<const u32*>(mVU.regs().Micro) + rootPC / 4;
	struct
	{
		u8 state[sizeof(microRegInfo)];
		u32 lower;
		u32 upper;
	} key;
	std::memcpy(key.state, state, sizeof(microRegInfo));
	key.lower = mem[0] & mVUchunkRootLowerMask(mem[0], mem[1]);
	key.upper = mem[1];
	return XXH3_64bits(&key, sizeof(key));
}

static bool mVUchunkStateEqual(const microRegInfo& a, const microRegInfo& b)
{
	return std::memcmp(&a, &b, sizeof(microRegInfo)) == 0;
}

// An outermost VU1 compile begins: nothing has been read or emitted yet.
static void mVUchunkBegin(microVU& mVU)
{
	mVUChunkEpisode& ep = mVU.chunk;
	std::memset(ep.readMark, 0, sizeof(ep.readMark));
	ep.runtimeBits.clear();
	ep.ranges.clear();
	ep.keptBits.clear();
	ep.wrapped = false;
	ep.statusImm = 0;
	ep.codeStart = armGetCurrentCodePointer();
	ep.recording = true;

	// The VU AOT tier compiles a program it has taken over through its hooks,
	// and the code a hook emits is its own. A compile for the block a region
	// bails to runs with the block already registered. A test that watches or
	// instruments every compile has to see one.
	vuaot::VuAotRegionHook region = vuaot::g_vuAotRegionHook;
	vuaot::VuAotBodyHook body = vuaot::g_vuAotBodyHook;
	void* user = nullptr;
	if (!region && !body)
		mVUvuAotLiveHooks(mVU, region, user, body, user);
	bool hooked = region || body || vuaot::g_vuAotShadowCompile;
#ifdef PCSX2_RECOMPILER_TESTS
	hooked = hooked || vuaot::g_mvuBlockHook || mvu_divtrace::g_enabled.load(std::memory_order_relaxed);
#endif
	ep.lookup = !hooked;
	ep.unstorable = hooked || mVU.prog.cur->vuaot != nullptr;
}

// Installs `entry` as a placement rooted at `rootPC` in the program being
// compiled. Returns the root block's entry, or null and how far it got.
enum mVUchunkMiss : int
{
	mVUchunkMissNoEntry,
	mVUchunkMissBounds,
	mVUchunkMissReadSet,
	mVUchunkMissBlockExists,
	mVUchunkMissLinkTarget,
	mVUchunkMissNoRoom,
};

static void* mVUchunkInstall(microVU& mVU, mVUChunkEntry& entry, u32 rootPC, int& miss)
{
	mVUChunkBody& b = entry.plain;
	const s32 rootPair = static_cast<s32>(rootPC / 8);

	// An install never replaces a block's entry and never compiles: every
	// block of the chunk has to be new here, and every block it branches to
	// has to be here already.
	for (mVUChunkBody::Block& blk : b.blocks)
	{
		microBlockManager* const mgr = mVUblocks[rootPair + blk.pairOff];
		if (mgr && mgr->search(mVU, &blk.pState))
		{
			miss = std::max<int>(miss, mVUchunkMissBlockExists);
			return nullptr;
		}
	}
	std::vector<microBlock*>& targets = mVU.chunkStore->scratchTargets;
	targets.clear();
	for (mVUChunkBody::Link& link : b.links)
	{
		microBlockManager* const mgr = mVUblocks[rootPair + link.pairOff];
		microBlock* const target = mgr ? mgr->search(mVU, &link.key) : nullptr;
		if (!target)
		{
			miss = std::max<int>(miss, mVUchunkMissLinkTarget);
			return nullptr;
		}
		targets.push_back(target);
	}

	const size_t stubBytes = b.blocks.size() * (mVUrootStubBytes + 4);
	const u8* const cacheEnd = mVU.prog.x86end + (mVUcacheSafeZone * _1mb) / 2;
	if (mVU.placeSlotsUsed >= mVUplaceSlotResetAt || b.slotCount > mVUplaceSlotCapacity - mVU.placeSlotsUsed ||
		armGetCurrentCodePointer() + stubBytes + 4096 > cacheEnd)
	{
		miss = std::max<int>(miss, mVUchunkMissNoRoom);
		return nullptr;
	}

	// The stubs name code the program cache's recorder has no record of.
	mVUPersist::DropEpisode(mVU, "chunk store placement");

	const u32 slotBase = mVU.placeSlotsUsed;
	mVU.placeSlotsUsed += b.slotCount;
	const u32 word = mVUplaceWord(rootPC, slotBase);
	void* rootEntry = nullptr;
	for (mVUChunkBody::Block& blk : b.blocks)
	{
		u8* const stub = armGetCurrentCodePointer();
		mVUemitPlaceStub(mVU, word);
		armEmitJmp(blk.stub + mVUrootStubBytes);

		alignas(16) microBlock fresh;
		std::memset(&fresh, 0, sizeof(fresh));
		fresh.pState = blk.pState;
		fresh.pStateEnd = blk.pStateEnd;
		fresh.x86ptrStart = stub;
		fresh.hostEntry = stub;
		blockCreate(rootPair + blk.pairOff);
		microBlock* const placed = mVUblocks[rootPair + blk.pairOff]->add(mVU, &fresh);
		pxAssertRel(placed->hostEntry == stub, "microVU: a placement's block was already registered");
		mVU.placeSlots[slotBase + blk.slot] = reinterpret_cast<u64>(placed);
		if (!rootEntry)
			rootEntry = stub;
	}
	for (size_t i = 0; i < b.links.size(); i++)
		mVU.placeSlots[slotBase + b.links[i].slot] = reinterpret_cast<u64>(targets[i]->hostEntry);

	// The ranges the compile added, in its order. mVUsetupRange copies each
	// from micro memory into the program as it closes.
	for (const mVUChunkBody::Range& r : b.ranges)
		mVUsetupRange(mVU, static_cast<s32>(rootPC) + r.off, r.isStart);
	// And the words of those ranges the program compares through a mask.
	for (const mVUChunkBody::KeptBits& k : b.keptBits)
		mVUaddKeptBits(mVU, rootPC + k.off, k.kept);

	if (b.statusImm)
		mVUvuAotOnStatusReader(mVU, b.statusImm);

	s_mVUchunkStats.codeBytesInstalled += b.codeBytes;
	s_mVUchunkStats.stubBytes += stubBytes;
	return rootEntry;
}

// An outermost VU1 compile was asked for `rootPC` with the entry state at
// `pState`. Returns the entry of an installed placement, or null: compile.
static void* mVUchunkLookup(microVU& mVU, u32 rootPC, uptr pState)
{
	mVUChunkEpisode& ep = mVU.chunk;
	mVUChunkStore::Stats& st = s_mVUchunkStats;
	st.episodes++;
	if (!ep.lookup)
	{
		st.skipped++;
		return nullptr;
	}
	mVUchunkStoreSyncOptions(mVU);
	mVUChunkStoreData& store = *mVU.chunkStore;
	const microRegInfo& state = *reinterpret_cast<const microRegInfo*>(pState);
	const u32* const mem = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const s32 rootWord = static_cast<s32>(rootPC / 4);
	const s32 words = static_cast<s32>(mVU.microMemSize / 4);

	int miss = mVUchunkMissNoEntry;
	const auto range = store.index.equal_range(mVUchunkRootHash(mVU, rootPC, &state));
	for (auto it = range.first; it != range.second; ++it)
	{
		mVUChunkEntry& entry = *store.entries[it->second];
		if (!mVUchunkStateEqual(entry.rootState, state))
			continue;
		const mVUChunkBody& b = entry.plain;
		if (rootWord + b.lo < 0 || rootWord + b.hi >= words)
		{
			miss = std::max<int>(miss, mVUchunkMissBounds);
			continue;
		}
		bool same = true;
		for (const mVUChunkBody::Word& w : b.reads)
		{
			if ((mem[rootWord + w.off] ^ w.value) & w.mask)
			{
				same = false;
				break;
			}
		}
		if (!same)
		{
			miss = std::max<int>(miss, mVUchunkMissReadSet);
			continue;
		}
		// The replayed ranges are not this compile's.
		ep.recording = false;
		if (void* const rootEntry = mVUchunkInstall(mVU, entry, rootPC, miss))
		{
			st.hits++;
			return rootEntry;
		}
		ep.recording = true;
	}

	switch (miss)
	{
		case mVUchunkMissNoEntry:     st.missNoEntry++; break;
		case mVUchunkMissBounds:      st.missBounds++; break;
		case mVUchunkMissReadSet:     st.missReadSet++; break;
		case mVUchunkMissBlockExists: st.missBlockExists++; break;
		case mVUchunkMissLinkTarget:  st.missLinkTarget++; break;
		default:                      st.missNoRoom++; break;
	}
	return nullptr;
}

// An outermost VU1 compile returned: what it emitted becomes an entry, unless
// the store holds it already or cannot describe it.
static void mVUchunkEnd(microVU& mVU)
{
	mVUChunkEpisode& ep = mVU.chunk;
	if (!ep.recording)
		return;
	ep.recording = false;

	mVUChunkStore::Stats& st = s_mVUchunkStats;
	const u32 codeBytes = static_cast<u32>(armGetCurrentCodePointer() - ep.codeStart);
	st.codeBytesCompiled += codeBytes;

	const u32 rootPC = mVU.compileRoot;
	if (ep.unstorable || ep.blocks.empty() || ep.blocks[0].startPC != rootPC)
	{
		st.feedOther++;
		return;
	}
	if (ep.wrapped)
	{
		st.feedWrapped++;
		return;
	}

	const u32* const mem = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const s32 rootWord = static_cast<s32>(rootPC / 4);
	const s32 rootPair = static_cast<s32>(rootPC / 8);
	const u32 words = mVU.microMemSize / 4;

	auto entry = std::make_unique<mVUChunkEntry>();
	mVUChunkBody& b = entry->plain;
	entry->rootState = ep.blocks[0].block->pState;

	// The read set: every word the compile read, with the bits it left to
	// run time masked out. The root pair is in it with at least the bits the
	// index key holds.
	u32 mask[2] = {mVUchunkRootLowerMask(mem[rootWord], mem[rootWord + 1]), ~0u};
	if (mask[0])
		ep.readMark[rootWord] = 1;
	ep.readMark[rootWord + 1] = 1;
	for (u32 w = 0; w < words; w++)
	{
		if (!ep.readMark[w])
			continue;
		mVUChunkBody::Word rec;
		rec.off = static_cast<s32>(w) - rootWord;
		rec.value = mem[w];
		rec.mask = ~0u;
		b.reads.push_back(rec);
	}
	const auto readAt = [&](u32 w) -> mVUChunkBody::Word* {
		const s32 off = static_cast<s32>(w) - rootWord;
		const auto it = std::lower_bound(b.reads.begin(), b.reads.end(), off,
			[](const mVUChunkBody::Word& r, s32 o) { return r.off < o; });
		return (it != b.reads.end() && it->off == off) ? &*it : nullptr;
	};
	for (const mVUChunkEpisode::RuntimeBits& rb : ep.runtimeBits)
	{
		if (mVUChunkBody::Word* r = readAt(rb.word))
			r->mask &= ~rb.bits;
	}
	readAt(rootWord + 1)->mask = ~0u;
	if (mask[0])
		readAt(rootWord)->mask |= mask[0];
	for (mVUChunkBody::Word& r : b.reads)
		r.value &= r.mask;

	s32 lo = 0, hi = 1;
	const auto span = [&](s32 first, s32 last) {
		lo = std::min(lo, first);
		hi = std::max(hi, last);
	};
	span(b.reads.front().off, b.reads.back().off);

	// The blocks, in the order they were registered, each with its slot, and
	// the links out of the chunk.
	for (u32 n = 0; n < ep.slots.size(); n++)
	{
		const mVUChunkSlot& slot = ep.slots[n];
		if (slot.target)
		{
			mVUChunkBody::Link link;
			link.pairOff = static_cast<s32>(slot.targetPC / 8) - rootPair;
			link.slot = n;
			link.key = slot.key;
			b.links.push_back(link);
			span(link.pairOff * 2, link.pairOff * 2 + 1);
			continue;
		}
		const auto cb = std::find_if(ep.blocks.begin(), ep.blocks.end(),
			[&](const mVUChunkEpisode::Block& x) { return x.block == slot.block; });
		if (cb == ep.blocks.end() || cb->block->hostEntry != cb->stub)
		{
			st.feedOther++;
			return;
		}
		mVUChunkBody::Block blk;
		blk.pairOff = static_cast<s32>(cb->startPC / 8) - rootPair;
		blk.slot = n;
		blk.stub = cb->stub;
		blk.pState = cb->block->pState;
		blk.pStateEnd = cb->block->pStateEnd;
		b.blocks.push_back(blk);
		span(blk.pairOff * 2, blk.pairOff * 2 + 1);
	}
	if (b.blocks.size() != ep.blocks.size() || b.blocks[0].stub != ep.blocks[0].stub)
	{
		st.feedOther++;
		return;
	}

	for (const mVUChunkEpisode::Range& r : ep.ranges)
	{
		mVUChunkBody::Range rec;
		rec.off = r.pc - static_cast<s32>(rootPC);
		rec.isStart = r.isStart;
		b.ranges.push_back(rec);
		// A range's end is the first byte past it.
		const s32 word = (r.isStart ? rec.off : rec.off - 4) / 4;
		if (r.isStart || rec.off > 0)
			span(word, word);
	}
	for (const microKeptBits& k : ep.keptBits)
		b.keptBits.push_back({static_cast<s32>(k.addr) - static_cast<s32>(rootPC), k.kept});
	b.lo = lo;
	b.hi = hi;
	b.slotCount = static_cast<u32>(ep.slots.size());
	b.codeBytes = codeBytes;
	b.statusImm = ep.statusImm;

	mVUchunkStoreSyncOptions(mVU);
	mVUChunkStoreData& store = *mVU.chunkStore;
	const u64 hash = mVUchunkRootHash(mVU, rootPC, &entry->rootState);
	const auto range = store.index.equal_range(hash);
	for (auto it = range.first; it != range.second; ++it)
	{
		const mVUChunkEntry& have = *store.entries[it->second];
		const mVUChunkBody& h = have.plain;
		if (!mVUchunkStateEqual(have.rootState, entry->rootState) || h.reads.size() != b.reads.size() ||
			h.blocks.size() != b.blocks.size() || h.links.size() != b.links.size())
			continue;
		bool same = std::equal(h.reads.begin(), h.reads.end(), b.reads.begin(),
			[](const mVUChunkBody::Word& x, const mVUChunkBody::Word& y) { return x.off == y.off && x.value == y.value && x.mask == y.mask; });
		same = same && std::equal(h.blocks.begin(), h.blocks.end(), b.blocks.begin(),
			[](const mVUChunkBody::Block& x, const mVUChunkBody::Block& y) { return x.pairOff == y.pairOff && x.slot == y.slot && mVUchunkStateEqual(x.pState, y.pState); });
		same = same && std::equal(h.links.begin(), h.links.end(), b.links.begin(),
			[](const mVUChunkBody::Link& x, const mVUChunkBody::Link& y) { return x.pairOff == y.pairOff && x.slot == y.slot && mVUchunkStateEqual(x.key, y.key); });
		if (same)
		{
			st.feedRepeats++;
			return;
		}
	}
	store.index.emplace(hash, static_cast<u32>(store.entries.size()));
	store.entries.push_back(std::move(entry));
	store.codeBytes += codeBytes;
	st.fed++;
}
