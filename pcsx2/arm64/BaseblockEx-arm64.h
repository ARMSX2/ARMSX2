// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64-specific BaseBlocks with block linking that stays signal-safe.
//
// x86 BaseBlocks (pcsx2/x86/BaseblockEx.h) uses a std::multimap to track
// pending link sites and rewrites them whenever a block is added or
// removed. The multimap is not used here because Remove() runs from the
// SIGSEGV fastmem handler, where mutating an STL container is unsafe.
//
// This implementation supports block linking while keeping Remove() signal-safe:
//   - Link()/New() touch the multimap, but they only run from the
//     compile path (single-threaded, never from a signal).
//   - Remove() does NOT walk the link map. Instead it overwrites the
//     first 4 bytes of each removed block with `B DispatcherReg`, so any
//     stale link re-dispatches through the recLUT. Block memory isn't
//     reclaimed until a full reset, so this 4-byte rewrite always lands
//     on memory the recompiler still owns.
//
// Stale sites divert to the DISPATCHER, never to JITCompile. Every link
// tail stores the target guest pc into cpuRegs.pc before its branch (both
// recs, all forms — see SetBranchImm / psxSetBranchImm), so a diverted
// site re-dispatches from a pc that is already correct; the recLUT (the
// single SMC-invalidation rewrite point) then routes it to the current
// code, or through JITCompile if the target is genuinely uncompiled.
// Routing stale sites at JITCompile directly instead re-enters
// recRecompile on an already-compiled block whenever the target has been
// removed-and-recompiled — tripping the fnptr assert on Devel, and on
// Release spuriously superseding live code in place, which strands
// orphaned callers in the stale pre-SMC compile (the NFS Carbon
// SLUS-21493 block-cache corruption). AetherSX2's Remove() and the
// pre-transplant linker enforce the same fallback-to-dispatcher policy.
//
// The patch site for each link is the address of a single B instruction
// emitted by SetBranchImm (see iR5900-arm64.cpp). Aligned 32-bit stores
// are atomic on AArch64, and `cacheflush(2)` on the patch site is
// async-signal-safe, so the redirect-stub Remove() write is signal-safe.
//
// Range: B imm26 covers ±128 MB. The EE recompiler region is 64 MB
// (HostMemoryMap::EErecSize), and JITCompile lives in the same region,
// so all link sites are reachable with a single B.
//
// Link-map liveness (2026-07-20). Entries used to accumulate for the whole
// lifetime of the map: Link() inserts unconditionally, and nothing pruned.
// Every recompile of a caller emits its branch at a *fresh* code address and
// appended another entry, so New(pc) re-patched every site the map had ever
// seen for that pc — each with its own 4-byte icache flush. Measured on
// Dirge of Cerberus: 13,372 stale sites on one PC, and 93.5% of EE-thread
// cycles inside __aarch64_sync_cache_range. Each entry now carries the
// identity of the block that emitted it (owner startpc + that block's code
// address), and both Link() and New() erase entries whose owner is gone or has
// since been recompiled. Cost becomes O(live callers) instead of O(all callers
// ever), and the map stops growing without bound.
//
// Liveness rests on two invariants: block code is bump-allocated and never
// reclaimed until a full Reset() (which also clears the map), so an fnptr
// value never identifies two different compiles; and BASEBLOCKEX::fnptr is
// authoritative for the block's *current* code, which is why New() updates
// it when a startpc is recompiled in place rather than leaving it stale.

#pragma once

#include <algorithm>
#include <map>

#include "common/HostSys.h"
#include "arm64/AsmHelpers.h" // armPatchCodeWord (self-scoped W^X code patching)
#include "arm64/BlockPageIndex-arm64.h"

// The IOP keeps its flat index and integer positions for its clear walk.
class Arm64FlatBlockIndex
{
public:
	using Position = int;
	static constexpr Position Invalid() { return -1; }

	Position AtOrBefore(u32 pc) const
	{
		int lo = 0;
		int hi = static_cast<int>(m_blocks.size());
		while (lo < hi)
		{
			const int mid = lo + (hi - lo) / 2;
			if (m_blocks[mid].startpc <= pc)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo - 1;
	}

	Position Next(Position pos) const
	{
		return pos + 1 < static_cast<int>(m_blocks.size()) ? pos + 1 : Invalid();
	}

	BASEBLOCKEX* Get(Position pos) const
	{
		return pos >= 0 && pos < static_cast<int>(m_blocks.size()) ? &m_blocks[pos] : nullptr;
	}

	const BASEBLOCKEX* FindExact(u32 pc) const
	{
		const BASEBLOCKEX* const block = Get(AtOrBefore(pc));
		return block && block->startpc == pc ? block : nullptr;
	}

	BASEBLOCKEX* UpdateOrInsert(u32 pc, uptr fnptr)
	{
		BASEBLOCKEX* block = Get(AtOrBefore(pc));
		if (!block || block->startpc != pc)
			return m_blocks.insert(pc, fnptr);
		block->fnptr = fnptr;
		return block;
	}

	void Erase(Position first, Position last) { m_blocks.erase(first, last + 1); }
	void Reset() { m_blocks.clear(); }
	u32 size() const { return m_blocks.size(); }

private:
	BaseBlockArray m_blocks{0x4000};
};

template <typename BlockIndex>
class Arm64BaseBlocksImpl
{
protected:
	// A registered branch site, tagged with the block that emitted it so
	// New() can tell live sites from ones left behind by a superseded
	// compile. See the liveness note in the file header.
	struct LinkSite
	{
		// Patch site address, with kLinkSiteCallBit in bit 0.
		uptr site;
		// Block that emitted this site. owner_fnptr == 0 means "untracked":
		// the site was registered outside a compile (only the direct unit
		// tests do this), and is treated as permanently live.
		u32 owner_startpc;
		uptr owner_fnptr;
	};

	using linkmap_t = std::multimap<u32, LinkSite>;

	BlockIndex blocks;
	linkmap_t links;
	uptr jitcompile = 0;
	uptr dispatcher = 0;

	// Where a stale site (unresolved Link(), Remove() entry stamp) diverts.
	// The dispatcher when one is registered — re-dispatch from the pc the
	// site's own tail stored — with a JITCompile fallback for direct
	// instantiation (unit tests) that never registers one.
	__fi uptr StaleDispatchTarget() const
	{
		return dispatcher ? dispatcher : jitcompile;
	}

	// The block currently being compiled, published by New(). Link() stamps
	// it onto every site it records.
	u32 emitting_startpc = 0;
	uptr emitting_fnptr = 0;

	// Link sites are 4-byte-aligned instruction addresses, so bit 0 of the
	// stored uptr is free: it tags the site's branch form. Untagged = B,
	// tagged = BL (call-ret stack call sites — the BL pushes the hardware
	// return-address stack so the callee's paired RET predicts).
	static constexpr uptr kLinkSiteCallBit = 1;

	// Encode a B/BL imm26 from `site` to `target`. ARM64 encoding:
	//   B : bits[31:26] = 000101, BL: bits[31:26] = 100101
	//   bits[25:0] = sign-extended ((target - site) >> 2)
	static u32 EncodeB(uptr site, uptr target, bool call = false)
	{
		const intptr_t off = static_cast<intptr_t>(target) - static_cast<intptr_t>(site);
		pxAssertRel((off & 3) == 0, "Branch offset not 4-byte aligned");
		const intptr_t imm26 = off >> 2;
		pxAssertRel(imm26 >= -(1 << 25) && imm26 < (1 << 25), "Branch offset out of B imm26 range");
		return (call ? 0x94000000u : 0x14000000u) | (static_cast<u32>(imm26) & 0x03FFFFFFu);
	}

	// Store only, no cache maintenance. Fine for a site inside the block
	// currently being emitted (that buffer has not been executed yet, and
	// armEndBlock() issues one whole-range flush over it), and for the
	// New() repoint loop, whose callers flush via FlushPatchedSites.
	// Never use this on code that is already live without a flush; use
	// PatchAtomic.
	static void PatchWord(uptr site, u32 instr)
	{
		// armPatchCodeWord owns the W^X handling — sites outside the open
		// emit window get their own page-granular write scope. Nothing on
		// the New()/Remove() paths holds one.
		armPatchCodeWord(reinterpret_cast<u8*>(site), instr);
	}

	// (HostSys::FlushInstructionCache, not the raw builtin: on Darwin the
	// builtin lowers to a compiler-rt ___clear_cache call that the iOS link
	// doesn't provide; the wrapper uses sys_icache_invalidate there.)
	static void FlushRange(uptr lo, uptr hi)
	{
		HostSys::FlushInstructionCache(reinterpret_cast<void*>(lo), static_cast<u32>(hi - lo));
	}

	static void PatchAtomic(uptr site, u32 instr)
	{
		PatchWord(site, instr);
		// Then make sure cores fetching instructions see the new word.
		FlushRange(site, site + 4);
		// Signal-safe (counter-only) — Remove() can call this from the
		// SIGSEGV fastmem handler.
	}

	// Drop every entry for `dest_pc` whose owner is gone or has been
	// recompiled since. Called from both Link() and New(): New() alone is not
	// enough, because a destination that is never recompiled again would never
	// have its list revisited, and the entries its callers keep re-registering
	// would accumulate for the life of the map. Erasing a multimap element
	// invalidates only that element's iterator, so the end of the range stays
	// valid across the walk.
	void PruneDeadLinks(u32 dest_pc)
	{
		const auto range = links.equal_range(dest_pc);
		for (auto it = range.first; it != range.second;)
		{
			if (IsOwnerLive(it->second))
				++it;
			else
				it = links.erase(it);
		}
	}

	// True when `ls` still lives inside a block that is both present and has
	// not been recompiled since the site was registered.
	bool IsOwnerLive(const LinkSite& ls)
	{
		if (ls.owner_fnptr == 0)
			return true; // untracked registration (direct unit tests)

		const BASEBLOCKEX* const owner = blocks.FindExact(ls.owner_startpc);
		return owner && owner->fnptr == ls.owner_fnptr;
	}

	// Coalesce the just-patched sites into as few cache-maintenance ranges as
	// possible. The per-call cost is dominated by the `dsb ish` / `isb` pair,
	// not by the DC/IC ops, so N adjacent 4-byte flushes cost ~N barrier pairs
	// where one range flush costs one. Sites emitted by the same block sit a
	// few instructions apart, so this usually collapses to a single range.
	static void FlushPatchedSites(uptr* sites, u32 count)
	{
		if (count == 0)
			return;

		std::sort(sites, sites + count);

		constexpr uptr kMaxGap = 256;
		uptr lo = sites[0], hi = sites[0] + 4;
		for (u32 i = 1; i < count; i++)
		{
			if (sites[i] - hi <= kMaxGap)
			{
				hi = sites[i] + 4;
				continue;
			}
			FlushRange(lo, hi);
			lo = sites[i];
			hi = sites[i] + 4;
		}
		FlushRange(lo, hi);
	}

public:
	using Position = typename BlockIndex::Position;

	void SetJITCompile(const void* recompiler_)
	{
		jitcompile = reinterpret_cast<uptr>(recompiler_);
	}

	void SetDispatcher(const void* dispatcher_)
	{
		dispatcher = reinterpret_cast<uptr>(dispatcher_);
	}

	// Register a link site that wants to branch directly to the block at
	// `pc`. Patches immediately if a block already exists; otherwise
	// records the site so New(pc, ...) can patch it later. `call` sites
	// get BL instead of B (and keep it across every re-patch).
	void Link(u32 pc, void* patch_site, bool call = false)
	{
		pxAssertRel(jitcompile, "SetJITCompile() must be called before Link()");
		pxAssertRel((reinterpret_cast<uptr>(patch_site) & kLinkSiteCallBit) == 0,
			"Link patch site must be 4-byte aligned");

		const BASEBLOCKEX* const target = blocks.FindExact(pc);
		const uptr target_addr = target ? target->fnptr : StaleDispatchTarget();
		// No flush: the site is in the block being emitted right now, and
		// armEndBlock() range-flushes that buffer. See PatchWord.
		PatchWord(reinterpret_cast<uptr>(patch_site),
			EncodeB(reinterpret_cast<uptr>(patch_site), target_addr, call));

		// Reap this destination's dead entries before adding ours, so its list
		// tracks live callers rather than every caller it has ever had.
		PruneDeadLinks(pc);

		links.insert({pc,
			LinkSite{reinterpret_cast<uptr>(patch_site) | (call ? kLinkSiteCallBit : 0),
				emitting_startpc, emitting_fnptr}});
	}

	// Begin compiling the block at `startpc`, whose code starts at `fnptr`.
	// Returns its BASEBLOCKEX, creating one if this startpc has no live block.
	//
	// A startpc can be recompiled while its BASEBLOCKEX is still in the array
	// (recClear resets BLOCK->fnptr across a straddled extent but deliberately
	// spares the in-progress block's entry). Reuse the entry and retarget it
	// so links and later invalidations use the current code address.
	BASEBLOCKEX* New(u32 startpc, uptr fnptr)
	{
		// Published for Link() to stamp onto the sites this block emits.
		emitting_startpc = startpc;
		emitting_fnptr = fnptr;

		BASEBLOCKEX* const block = blocks.UpdateOrInsert(startpc, fnptr);

		// Repoint the links waiting on this PC at the new code, dropping any
		// left behind by a block that has since been removed or recompiled.
		// Those sites are unreachable code; patching them is pure waste, and
		// it is that waste which used to dominate the EE thread.
		uptr patched[64];
		u32 npatched = 0;

		const auto range = links.equal_range(startpc);
		for (auto it = range.first; it != range.second;)
		{
			if (!IsOwnerLive(it->second))
			{
				it = links.erase(it);
				continue;
			}

			const uptr site = it->second.site & ~kLinkSiteCallBit;
			PatchWord(site, EncodeB(site, fnptr, (it->second.site & kLinkSiteCallBit) != 0));
			if (npatched < std::size(patched))
				patched[npatched++] = site;
			else
				FlushRange(site, site + 4); // overflow: flush as we go
			++it;
		}

		FlushPatchedSites(patched, npatched);
		return block;
	}

	Position AtOrBefore(u32 startpc) const
	{
		return blocks.AtOrBefore(startpc);
	}

	__fi Position Index(u32 pc) const
	{
		const Position pos = AtOrBefore(pc);
		const BASEBLOCKEX* const block = blocks.Get(pos);
		if (!block || (block->size && pc >= block->startpc + block->size * 4))
			return BlockIndex::Invalid();
		return pos;
	}

	Position Next(Position pos) const { return blocks.Next(pos); }
	Position Previous(Position pos) const { return blocks.Previous(pos); }
	BASEBLOCKEX* operator[](Position pos) { return blocks.Get(pos); }

	__fi BASEBLOCKEX* Get(u32 startpc)
	{
		return blocks.Get(Index(startpc));
	}

	// Signal-safe: writes a redirect stub at each removed block's entry
	// point so any stale link re-dispatches through the recLUT (see the
	// stale-dispatch policy note in the file header — the diverting site's
	// tail already stored the correct pc, so DispatcherReg routes it to the
	// current code; JITCompile here would re-enter recRecompile on a
	// removed-and-recompiled block), then erases from the affected pages.
	// This leaves the link map alone. The entries this strands are reaped by the next
	// New() for their destination PC, which sees the owner is gone; until
	// then the redirect stub keeps any site still pointing at this block
	// correct.
	//
	// SL-1: a resident self-loop's back-edge is an internal B to the loop-top
	// (past the entry redirect), so it gets its own atomic repoint — to the
	// block's cold spill stub, which writes back the pinned set, publishes pc,
	// and exits via DispatcherEvent. Page storage stays allocated.
	__fi void Remove(Position first, Position last)
	{
		pxAssert(blocks.Get(first) && blocks.Get(last) &&
			blocks.Get(first)->startpc <= blocks.Get(last)->startpc);
		for (Position pos = first; pos != BlockIndex::Invalid(); pos = Next(pos))
		{
			Invalidate(pos);
			if (pos == last)
				break;
		}

		blocks.Erase(first, last);
	}

	// Stamp the redirect stub over one block's entry (and repoint its resident
	// back-edge) while retaining its metadata. Same protocol as Remove():
	// the next dispatch of this startpc goes through JITCompile once the caller
	// resets the LUT entry, and any link site still branching into the old code
	// lands on the stub and re-dispatches. New() reuses the entry and retargets
	// fnptr when it recompiles a startpc whose BASEBLOCKEX is still present,
	// which is also the state recClear leaves the in-progress block in.
	//
	// Keeping the metadata avoids compaction when a cycle-rate transition
	// invalidates thousands of blocks at once.
	__fi void Invalidate(Position pos)
	{
		if (!jitcompile)
			return;

		const BASEBLOCKEX& block = *blocks.Get(pos);
		const uptr site = block.fnptr;
		PatchAtomic(site, EncodeB(site, StaleDispatchTarget()));

		if (block.backedge_site)
			PatchAtomic(block.backedge_site, EncodeB(block.backedge_site, block.backedge_stub));
	}

	__fi void Reset()
	{
		blocks.Reset();
		links.clear();
		emitting_startpc = 0;
		emitting_fnptr = 0;
	}

	// Number of registered blocks: the denominator for anything that walks or
	// repairs every live block, such as a cycle-rate transition's cost.
	__fi u32 size() const { return blocks.size(); }

	// The block whose code holds host address `pc`, or null. A walk of the
	// whole index, for the crash post-mortem (RecCrashReport-arm64.h).
	// The compile thread owns the index and calls this from its fault
	// handler while executing generated code.
	const BASEBLOCKEX* FindByHostPc(uptr pc) const
	{
		for (Position pos = Next(BlockIndex::Invalid()); const BASEBLOCKEX* block = blocks.Get(pos); pos = Next(pos))
		{
			if (pc - block->fnptr < block->x86size)
				return block;
		}
		return nullptr;
	}

#ifdef PCSX2_RECOMPILER_TESTS
	// Test-only introspection. Returns true iff a link patch site within the
	// block containing src_pc targets a block at dst_pc. The link multimap
	// is keyed by destination PC, so the entries for dst_pc are walked to check
	// whether the patch site lies inside [block.fnptr, block.fnptr + x86size),
	// where x86size is BASEBLOCKEX's (legacy-named) host machine-code byte size.
	// O(L_d + log B) where L_d is the number of links to dst_pc and B is the
	// block count.
	bool IsLinked(u32 src_pc, u32 dst_pc) const
	{
		const BASEBLOCKEX* const block = blocks.Get(Index(src_pc));
		if (!block || src_pc >= block->startpc + block->size * 4)
			return false;
		const uptr lo = block->fnptr;
		const uptr hi = block->fnptr + block->x86size;
		const auto range = links.equal_range(dst_pc);
		for (auto it = range.first; it != range.second; ++it)
		{
			const uptr site = it->second.site & ~kLinkSiteCallBit;
			if (site >= lo && site < hi)
				return true;
		}
		return false;
	}

	// Test-only: number of registered link sites targeting dst_pc, and the
	// total across all destinations. Used to pin that the map stays bounded
	// by *live* sites rather than growing with every recompile.
	size_t LinkCount(u32 dst_pc) const
	{
		const auto range = links.equal_range(dst_pc);
		return static_cast<size_t>(std::distance(range.first, range.second));
	}

	size_t TotalLinkCount() const { return links.size(); }
#endif
};

using Arm64BaseBlocks = Arm64BaseBlocksImpl<Arm64BlockPageIndex>;
using Arm64IopBaseBlocks = Arm64BaseBlocksImpl<Arm64FlatBlockIndex>;
