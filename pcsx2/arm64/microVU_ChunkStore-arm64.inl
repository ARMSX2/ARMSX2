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
		s.entries = store->plainBodies;
		s.entryCodeBytes = store->codeBytes;
		s.tieredBodies = store->tieredBodies;
		s.tieredCodeBytes = store->tieredCodeBytes;
	}
	return s;
}

#ifdef PCSX2_RECOMPILER_TESTS
std::vector<u32> mVUChunkStore::TestJumpTargets(u32 blockPC)
{
	std::vector<u32> out;
	microVU& mVU = microVU1;
	microProgram* const prog = mVU.prog.cur;
	microBlockManager* const mgr = prog ? prog->block[(blockPC & 0x3ff8) / 8] : nullptr;
	if (!mgr)
		return out;
	mgr->forEachBlock([&](microBlock& b) {
		if (!b.jumpCache)
			return;
		for (u32 t = 0; t < mProgSize / 2; t++)
			if (b.jumpCache[t].prog == prog && b.jumpCache[t].hostEntry)
				out.push_back(t * 8);
	});
	std::sort(out.begin(), out.end());
	return out;
}
#endif

static void mVUchunkStoreClear(microVU& mVU)
{
	if (mVUChunkStoreData* store = mVU.chunkStore.get())
	{
		store->heads.clear();
		store->retired.clear();
		store->index.clear();
		store->built.reset();
		store->codeBytes = 0;
		store->plainBodies = 0;
		store->tieredBodies = 0;
		store->tieredCodeBytes = 0;
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
		"codeKiB(compiled/installed/stubs/entries)=%llu/%llu/%llu/%llu cacheUsedKiB=%llu slots=%u "
		"tiered(bodies/codeKiB/placed/reaimed)=%llu/%llu/%llu/%llu",
		mVU.index, tag, (unsigned long long)s.episodes, (unsigned long long)s.hits,
		(unsigned long long)(s.episodes - s.hits), (unsigned long long)s.skipped,
		(unsigned long long)s.missNoEntry, (unsigned long long)s.missBounds, (unsigned long long)s.missReadSet,
		(unsigned long long)s.missBlockExists, (unsigned long long)s.missLinkTarget, (unsigned long long)s.missNoRoom,
		(unsigned long long)s.fed, (unsigned long long)s.feedRepeats, (unsigned long long)s.feedWrapped,
		(unsigned long long)s.feedOther, (unsigned long long)s.entries,
		(unsigned long long)(s.codeBytesCompiled / 1024), (unsigned long long)(s.codeBytesInstalled / 1024),
		(unsigned long long)(s.stubBytes / 1024), (unsigned long long)(s.entryCodeBytes / 1024),
		(unsigned long long)((mVU.prog.x86ptr - mVU.prog.x86start) / 1024), mVU.placeSlotsUsed,
		(unsigned long long)s.tieredBodies, (unsigned long long)(s.tieredCodeBytes / 1024),
		(unsigned long long)s.tieredPlaced, (unsigned long long)s.tieredReAimed);
}

// The entries were compiled under one set of options; an entry made under
// another set is other code.
static void mVUchunkStoreSyncOptions(microVU& mVU)
{
	mVUChunkStoreData& store = *mVU.chunkStore;
	if (store.options.low64 == mVU.optionsSentinel.low64 && store.options.high64 == mVU.optionsSentinel.high64)
		return;
	// The programs that ran the old bodies may still be there, with blocks
	// that point at these heads: the heads stay, with nothing in them.
	std::vector<std::unique_ptr<mVUChunkHead>> old = std::move(store.heads);
	std::vector<std::unique_ptr<mVUChunkHead>> retired = std::move(store.retired);
	mVUchunkStoreClear(mVU);
	for (auto& head : old)
	{
		head->plain.clear();
		head->tiered.clear();
		head->job.reset();
		head->next[0] = head->next[1] = ~0u;
		retired.push_back(std::move(head));
	}
	store.retired = std::move(retired);
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

	// A test's compile hooks emit code of their own, and a test that watches or
	// instruments every compile has to see one. A compile for the block a
	// region bails to runs with the block already registered. While the VU
	// AOT tier builds a routine's regions, what is compiled is the tiered
	// body itself.
	bool hooked = vuaot::g_vuAotRegionHook || vuaot::g_vuAotBodyHook || vuaot::g_vuAotShadowCompile;
#ifdef PCSX2_RECOMPILER_TESTS
	hooked = hooked || vuaot::g_mvuBlockHook || mvu_divtrace::g_enabled.load(std::memory_order_relaxed);
#endif
	ep.tiered = !hooked && mVUvuAotBuilding(mVU);
	ep.lookup = !hooked && !ep.tiered;
	ep.unstorable = hooked;
}

// The routine whose root block is at `rootPC` with the entry state `state`.
static mVUChunkHead* mVUchunkFindHead(microVU& mVU, u32 rootPC, const microRegInfo& state, bool create)
{
	mVUchunkStoreSyncOptions(mVU);
	mVUChunkStoreData& store = *mVU.chunkStore;
	const u64 hash = mVUchunkRootHash(mVU, rootPC, &state);
	const auto range = store.index.equal_range(hash);
	for (auto it = range.first; it != range.second; ++it)
	{
		mVUChunkHead& head = *store.heads[it->second];
		if (mVUchunkStateEqual(head.rootState, state))
			return &head;
	}
	if (!create)
		return nullptr;
	auto head = std::make_unique<mVUChunkHead>();
	head->rootState = state;
	head->next[0] = mVUvuAotTierUpRuns();
	head->next[1] = ~0u;
	store.index.emplace(hash, static_cast<u32>(store.heads.size()));
	store.heads.push_back(std::move(head));
	return store.heads.back().get();
}

// The tier's view of `block`, the root of a copy of `head`'s routine: it has
// been checked against the head's tiered bodies as they are now, and runs
// `state` (microBlock::tierState).
__fi static void mVUchunkMarkRoot(microBlock* block, mVUChunkHead* head, u32 state)
{
	if (!block)
		return;
	block->tierHead = head;
	block->tierSerial = head->serial;
	block->tierState = state;
}

// How far a lookup that fell through to the JIT got.
enum mVUchunkMiss : int
{
	mVUchunkMissNoEntry,
	mVUchunkMissBounds,
	mVUchunkMissReadSet,
	mVUchunkMissBlockExists,
	mVUchunkMissLinkTarget,
	mVUchunkMissNoRoom,
};

// Whether `b` is the code for micro memory as it is at `rootPC`.
static bool mVUchunkBodyMatches(microVU& mVU, const mVUChunkBody& b, u32 rootPC, int& miss)
{
	const u32* const mem = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const s32 rootWord = static_cast<s32>(rootPC / 4);
	const s32 words = static_cast<s32>(mVU.microMemSize / 4);
	if (rootWord + b.lo < 0 || rootWord + b.hi >= words)
	{
		miss = std::max<int>(miss, mVUchunkMissBounds);
		return false;
	}
	for (const mVUChunkBody::Word& w : b.reads)
	{
		if ((mem[rootWord + w.off] ^ w.value) & w.mask)
		{
			miss = std::max<int>(miss, mVUchunkMissReadSet);
			return false;
		}
	}
	return true;
}

// Points every reference to `block`'s code at `stub`: the first instruction
// of its old entry becomes a branch there, so the quick slots, jump caches
// and links that hold the old entry need no change.
static void mVUchunkReAim(microBlock* block, u8* stub)
{
	if (u8* const old = static_cast<u8*>(block->hostEntry))
	{
		u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(old));
		*insn = 0x14000000u | (static_cast<u32>((stub - old) / 4) & 0x03FFFFFFu);
		HostSys::FlushInstructionCache(old, 4);
	}
	block->x86ptrStart = stub;
	block->hostEntry = stub;
	block->extent = 0;
}

// Installs `b` as a placement rooted at `rootPC` in the program being
// compiled. Returns the root block's entry, or null and how far it got.
//
// A plain body is installed only where none of its blocks exists yet. A
// tiered body takes over the blocks that exist: each gets a new entry stub,
// and its old entry is pointed at it.
static void* mVUchunkInstall(microVU& mVU, mVUChunkBody& b, u32 rootPC, int& miss)
{
	const s32 rootPair = static_cast<s32>(rootPC / 8);

	// An install never compiles: every block the chunk branches to has to be
	// here already.
	if (!b.tiered)
	{
		for (mVUChunkBody::Block& blk : b.blocks)
		{
			microBlockManager* const mgr = mVUblocks[rootPair + blk.pairOff];
			if (mgr && mgr->search(mVU, &blk.pState))
			{
				miss = std::max<int>(miss, mVUchunkMissBlockExists);
				return nullptr;
			}
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

		blockCreate(rootPair + blk.pairOff);
		microBlockManager* const mgr = mVUblocks[rootPair + blk.pairOff];
		microBlock* placed = b.tiered ? mgr->search(mVU, &blk.pState) : nullptr;
		if (placed)
		{
			mVUchunkReAim(placed, stub);
			// (the root's state is its caller's to set)
			if (placed->tierState == mVUtierNone)
				placed->tierState = mVUtierCovered;
			// (a block that ends in a JR or JALR keeps its end state: microVU's
			// code for it, which a bail runs, reads it too)
			if (!placed->jumpCache)
				placed->pStateEnd = blk.pStateEnd;
		}
		else
		{
			alignas(16) microBlock fresh;
			std::memset(&fresh, 0, sizeof(fresh));
			fresh.pState = blk.pState;
			fresh.pStateEnd = blk.pStateEnd;
			fresh.x86ptrStart = stub;
			fresh.hostEntry = stub;
			placed = mgr->add(mVU, &fresh);
			pxAssertRel(placed->hostEntry == stub, "microVU: a placement's block was already registered");
			if (b.tiered)
				placed->tierState = mVUtierCovered;
		}
		if (blk.jumps && !placed->jumpCache)
			placed->jumpCache = static_cast<microJumpCache*>(mVUJumpTable::Alloc());
		mVU.placeSlots[slotBase + blk.slot] = reinterpret_cast<u64>(placed);
		if (!rootEntry)
			rootEntry = stub;
	}
	for (size_t i = 0; i < b.links.size(); i++)
		mVU.placeSlots[slotBase + b.links[i].slot] = reinterpret_cast<u64>(targets[i]->hostEntry);
	for (const mVUChunkBody::DataSlot& d : b.dataSlots)
		mVU.placeSlots[slotBase + d.slot] = d.kind == mVUslotProgram ? reinterpret_cast<u64>(mVU.prog.cur) : 0;

	// The ranges the compile added, in its order. mVUsetupRange copies each
	// from micro memory into the program as it closes.
	const bool wasRecording = std::exchange(mVU.chunk.recording, false);
	for (const mVUChunkBody::Range& r : b.ranges)
		mVUsetupRange(mVU, static_cast<s32>(rootPC) + r.off, r.isStart);
	// And the words of those ranges the program compares through a mask.
	for (const mVUChunkBody::KeptBits& k : b.keptBits)
		mVUaddKeptBits(mVU, rootPC + k.off, k.kept);
	mVU.chunk.recording = wasRecording;

	if (b.statusImm)
		mVUvuAotOnStatusReader(mVU, b.statusImm);
	if (b.tiered)
	{
		mVUvuAotOnBodyPlaced(mVU, b.droppedSticky);
		s_mVUchunkStats.tieredPlaced++;
	}

	s_mVUchunkStats.codeBytesInstalled += b.codeBytes;
	s_mVUchunkStats.stubBytes += stubBytes;
	return rootEntry;
}

// The newest tiered body of `head` that is the code for micro memory at
// `rootPC` and may be placed now, or null. A body the tier declined counts
// only when no other matches.
static mVUChunkBody* mVUchunkTieredFor(microVU& mVU, mVUChunkHead& head, u32 rootPC)
{
	mVUChunkBody* declined = nullptr;
	for (size_t n = head.tiered.size(); n-- > 0;)
	{
		mVUChunkBody& b = *head.tiered[n];
		int miss = 0;
		if (!mVUchunkBodyMatches(mVU, b, rootPC, miss))
			continue;
		if (b.declined)
		{
			if (!declined)
				declined = &b;
			continue;
		}
		if (mVUvuAotBodyAllowed(b.droppedSticky))
			return &b;
	}
	return declined;
}

// Moves the copy of `head`'s routine whose root block is `root` (at `rootPC`,
// in the program being compiled) onto the newest tiered body that matches it.
// Returns the root's new entry, or null when the copy stays as it is.
static void* mVUchunkReAimCopy(microVU& mVU, mVUChunkHead& head, microBlock* root, u32 rootPC)
{
	mVUChunkBody* const b = mVUchunkTieredFor(mVU, head, rootPC);
	if (!b)
	{
		mVUchunkMarkRoot(root, &head, root->tierState == mVUtierCovered ? mVUtierCovered : mVUtierNone);
		return nullptr;
	}
	if (b->declined)
	{
		mVUchunkMarkRoot(root, &head, root->tierState == mVUtierCovered ? mVUtierCovered : mVUtierDeclined);
		return nullptr;
	}
	if (root->tierState == mVUtierBody + b->id)
	{
		mVUchunkMarkRoot(root, &head, mVUtierBody + b->id);
		return nullptr;
	}
	int miss = 0;
	void* const entry = mVUchunkInstall(mVU, *b, rootPC, miss);
	if (!entry)
	{
		mVUchunkMarkRoot(root, &head, root->tierState);
		return nullptr;
	}
	pxAssertRel(root->hostEntry == entry, "microVU: a tiered body's root is not the copy's root block");
	mVUchunkMarkRoot(root, &head, mVUtierBody + b->id);
	s_mVUchunkStats.tieredReAimed++;
	return entry;
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
		if (ep.tiered)
			st.tieredBuilds++;
		return nullptr;
	}
	const microRegInfo& state = *reinterpret_cast<const microRegInfo*>(pState);
	const s32 rootPair = static_cast<s32>(rootPC / 8);
	const auto rootBlock = [&]() { return mVUblocks[rootPair]->search(mVU, const_cast<microRegInfo*>(&state)); };

	int miss = mVUchunkMissNoEntry;
	if (mVUChunkHead* const head = mVUchunkFindHead(mVU, rootPC, state, false))
	{
		// The replayed ranges are not this compile's.
		ep.recording = false;
		// A tiered body first.
		mVUChunkBody* const tiered = mVUchunkTieredFor(mVU, *head, rootPC);
		if (tiered && !tiered->declined)
		{
			if (void* const rootEntry = mVUchunkInstall(mVU, *tiered, rootPC, miss))
			{
				mVUchunkMarkRoot(rootBlock(), head, mVUtierBody + tiered->id);
				st.hits++;
				st.tieredHits++;
				return rootEntry;
			}
		}
		for (auto& body : head->plain)
		{
			if (!mVUchunkBodyMatches(mVU, *body, rootPC, miss))
				continue;
			if (void* const rootEntry = mVUchunkInstall(mVU, *body, rootPC, miss))
			{
				mVUchunkMarkRoot(rootBlock(), head, (tiered && tiered->declined) ? mVUtierDeclined : mVUtierNone);
				st.hits++;
				return rootEntry;
			}
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

	auto body = std::make_unique<mVUChunkBody>();
	mVUChunkBody& b = *body;
	const microRegInfo rootState = ep.blocks[0].block->pState;

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
		if (slot.kind == mVUslotZero || slot.kind == mVUslotProgram)
		{
			b.dataSlots.push_back({n, slot.kind});
			continue;
		}
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
		blk.jumps = cb->block->jumpCache != nullptr;
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

	// A build episode's body goes to the tier, which files it under its head.
	if (ep.tiered)
	{
		if (!b.links.empty())
		{
			st.feedOther++;
			return;
		}
		b.tiered = true;
		mVU.chunkStore->built = std::move(body);
		return;
	}

	mVUChunkHead* const head = mVUchunkFindHead(mVU, rootPC, rootState, true);
	mVUChunkStoreData& store = *mVU.chunkStore;
	// (the lookup that came before this compile placed no tiered body for it)
	{
		const mVUChunkBody* const tiered = mVUchunkTieredFor(mVU, *head, rootPC);
		mVUchunkMarkRoot(ep.blocks[0].block, head, (tiered && tiered->declined) ? mVUtierDeclined : mVUtierNone);
	}
	for (const auto& have : head->plain)
	{
		const mVUChunkBody& h = *have;
		if (h.reads.size() != b.reads.size() || h.blocks.size() != b.blocks.size() || h.links.size() != b.links.size())
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
	head->plain.push_back(std::move(body));
	store.plainBodies++;
	store.codeBytes += codeBytes;
	st.fed++;
}
