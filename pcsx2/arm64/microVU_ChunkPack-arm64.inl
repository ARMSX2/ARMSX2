// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU1 chunk store on disk: a body as a record of the program cache's pack
// (microVU_ProgCache-arm64.inl), and a record made back into a body.
//
// A record is everything mVUChunkBody holds, with the code as bytes and every
// host address in it as a fixup (mVUPersist::PersistFixup). Nothing in it
// names a program or an address in micro memory: offsets are from the root,
// slots are numbered from 0, and the entry stubs inside the code are not
// entered in a copy, because every placement has stubs of its own.
//
// The VU AOT tier's state for a routine goes with its bodies: a tiered body
// carries the jump targets its regions hold, the tier's "no region" answer is
// a record with a read set and no code, and a routine the tier has stopped
// looking for new targets in has a small record that says so. Run counts are
// not kept: a copy that fits a tiered body is placed on it at its first
// lookup. A copy that fits one that cannot be used now (it leaves the sticky
// byte of STATUS out and a reader of it is known, or its code was refused)
// asks for its regions at its first run: the routine was worth them before.
//
// Written when the store gains a body (mVUchunkPackSave, on the thread that
// runs VU1; the cache's writer thread puts it on disk). Read back per routine:
// the first lookup of a root after a reset asks the pack for that root's
// records (mVUchunkPackFetch) and files each one that passes under its head.
//
// Included from microVU-arm64.cpp after microVU_Persist-arm64.inl, whose
// fixup forms and patcher it uses.

namespace mVUChunkPack
{
	static constexpr u32 kRecordMagic = 0x59444256u; // 'VBDY'
	static constexpr u32 kFlagTiered = 1u << 0;
	static constexpr u32 kFlagDeclined = 1u << 1;
	static constexpr u32 kFlagDroppedSticky = 1u << 2;
	static constexpr u32 kFlagMask = kFlagTiered | kFlagDeclined | kFlagDroppedSticky;
	// Bounds a record is held to before anything is allocated for it.
	static constexpr u32 kMaxCodeBytes = 8u << 20;
	static constexpr u32 kMaxSlots = 1u << 18;

	// Tiered bodies and "no region" marks made from the pack for one routine:
	// what one head keeps (kMaxBodiesPerHead, vuaot/microVU_VuAot-arm64.inl),
	// the ones stored last. The records past it leave the pack.
	static constexpr size_t kMaxTieredPerRoot = 8;

	// The routine's head is settled (mVUChunkHead::settled).
	static constexpr u32 kSettledMagic = 0x54455356u; // 'VSET'
	struct SettledRecord
	{
		u32 magic;
		u8 rootState[sizeof(microRegInfo)];
	};

#pragma pack(push, 1)
	struct Header
	{
		u32 magic;
		u32 flags;
		u8 rootState[sizeof(microRegInfo)];
		s32 lo;
		s32 hi;
		u32 slotCount;
		u32 codeBytes;
		u32 statusImm;
		u32 readCount;
		u32 blockCount;
		u32 linkCount;
		u32 dataSlotCount;
		u32 rangeCount;
		u32 keptCount;
		u32 targetWords;
		u32 fixupCount;
	};
	struct DiskRead
	{
		s32 off;
		u32 value;
		u32 mask;
	};
	struct DiskBlock
	{
		s32 pairOff;
		u32 slot;
		u32 stubOff;
		u32 bodyOff;
		u32 hasJumpTable;
		u8 pState[sizeof(microRegInfo)];
		u8 pStateEnd[sizeof(microRegInfo)];
	};
	struct DiskLink
	{
		s32 pairOff;
		u32 slot;
		u8 key[sizeof(microRegInfo)];
	};
	struct DiskPair
	{
		s32 a;
		u32 b;
	};
#pragma pack(pop)

	template <typename T>
	static void Put(std::vector<u8>& out, const T& v)
	{
		const u8* p = reinterpret_cast<const u8*>(&v);
		out.insert(out.end(), p, p + sizeof(T));
	}

	// Whether every instruction of `code` that addresses something relative to
	// itself stays inside the code or is covered by a fixup. The recorder sees
	// what goes through the emit helpers; this sees the rest (a literal pool
	// vixl put after the body, a branch written by hand).
	static bool CodeIsSelfContained(const u8* code, u32 codeBytes, const std::vector<mVUPersist::PersistFixup>& fixups)
	{
		const u32 n = codeBytes / 4;
		std::vector<u8> covered(n, 0);
		for (const mVUPersist::PersistFixup& f : fixups)
		{
			const u32 first = f.codeOffset / 4;
			for (u32 i = 0; i < mVUPersist::FixupSpanInsns(f) && first + i < n; i++)
				covered[first + i] = 1;
		}
		const uptr begin = reinterpret_cast<uptr>(code);
		for (u32 i = 0; i < n; i++)
		{
			if (covered[i])
				continue;
			const a64::Instruction* insn = reinterpret_cast<const a64::Instruction*>(code + i * 4);
			if (!insn->IsUncondBranchImm() && !insn->IsCondBranchImm() && !insn->IsCompareBranch() && !insn->IsTestBranch() &&
				!insn->IsLoadLiteral() && !insn->IsPCRelAddressing())
				continue;
			const uptr target = reinterpret_cast<uptr>(insn->GetImmPCOffsetTarget());
			if (target < begin || target >= begin + codeBytes)
				return false;
		}
		return true;
	}

	static void Serialize(const microRegInfo& rootState, const mVUChunkBody& b, std::vector<u8>& out)
	{
		out.clear();
		Header h = {};
		h.magic = kRecordMagic;
		h.flags = (b.tiered ? kFlagTiered : 0) | (b.declined ? kFlagDeclined : 0) | (b.droppedSticky ? kFlagDroppedSticky : 0);
		std::memcpy(h.rootState, &rootState, sizeof(microRegInfo));
		h.lo = b.lo;
		h.hi = b.hi;
		h.slotCount = b.slotCount;
		h.codeBytes = b.declined ? 0 : b.codeBytes;
		h.statusImm = b.statusImm;
		h.readCount = static_cast<u32>(b.reads.size());
		h.blockCount = static_cast<u32>(b.blocks.size());
		h.linkCount = static_cast<u32>(b.links.size());
		h.dataSlotCount = static_cast<u32>(b.dataSlots.size());
		h.rangeCount = static_cast<u32>(b.ranges.size());
		h.keptCount = static_cast<u32>(b.keptBits.size());
		for (const auto& t : b.targets)
			h.targetWords += 2 + static_cast<u32>(t.second.size());
		h.fixupCount = static_cast<u32>(b.fixups.size());
		Put(out, h);

		for (const mVUChunkBody::Word& w : b.reads)
			Put(out, DiskRead{w.off, w.value, w.mask});
		for (const mVUChunkBody::Block& blk : b.blocks)
		{
			DiskBlock d = {};
			d.pairOff = blk.pairOff;
			d.slot = blk.slot;
			d.stubOff = static_cast<u32>(blk.stub - b.code);
			d.bodyOff = static_cast<u32>(blk.body - b.code);
			d.hasJumpTable = blk.jumpCache ? 1 : 0;
			std::memcpy(d.pState, &blk.pState, sizeof(microRegInfo));
			std::memcpy(d.pStateEnd, &blk.pStateEnd, sizeof(microRegInfo));
			Put(out, d);
		}
		for (const mVUChunkBody::Link& l : b.links)
		{
			DiskLink d = {};
			d.pairOff = l.pairOff;
			d.slot = l.slot;
			std::memcpy(d.key, &l.key, sizeof(microRegInfo));
			Put(out, d);
		}
		for (const mVUChunkBody::DataSlot& d : b.dataSlots)
			Put(out, DiskPair{static_cast<s32>(d.slot), d.kind});
		for (const mVUChunkBody::Range& r : b.ranges)
			Put(out, DiskPair{r.off, r.isStart ? 1u : 0u});
		for (const mVUChunkBody::KeptBits& k : b.keptBits)
			Put(out, DiskPair{k.off, k.kept});
		for (const auto& t : b.targets)
		{
			Put(out, t.first);
			Put(out, static_cast<u32>(t.second.size()));
			for (u32 to : t.second)
				Put(out, to);
		}
		for (const mVUPersist::PersistFixup& f : b.fixups)
			Put(out, f);
		if (h.codeBytes)
			out.insert(out.end(), b.code, b.code + h.codeBytes);
	}

	// A record, checked and taken apart. Pointers are into the record's bytes.
	struct Parsed
	{
		Header h;
		const DiskRead* reads;
		const DiskBlock* blocks;
		const DiskLink* links;
		const DiskPair* dataSlots;
		const DiskPair* ranges;
		const DiskPair* kept;
		const u32* targets;
		const u8* fixups; // PersistFixup records, not aligned for one: read through Fixup()
		const u8* code;

		mVUPersist::PersistFixup Fixup(u32 i) const
		{
			mVUPersist::PersistFixup f;
			std::memcpy(&f, fixups + static_cast<size_t>(i) * sizeof(f), sizeof(f));
			return f;
		}
	};

	// The host address a fixup stands for in this process, with the body's
	// code at `base`.
	static u64 FixupValue(microVU& mVU, const mVUPersist::PersistFixup& f, const u8* base)
	{
		switch (f.target)
		{
			case mVUPersist::kTargetStub:
				return reinterpret_cast<u64>(mVUPersist::StubAddress(mVU, static_cast<u8>(f.index)));
			case mVUPersist::kTargetBody:
				return reinterpret_cast<u64>(base) + static_cast<u64>(f.offset);
			default:
				return static_cast<u64>(mVUPersist::RegionBase(f.target)) + static_cast<u64>(f.offset);
		}
	}

	// Checks every count, offset and slot of a record against its own size and
	// the store's limits. Nothing in a record that passes can index outside
	// the record, the slot run or micro memory.
	static bool Parse(microVU& mVU, const u8* data, size_t size, Parsed& out)
	{
		if (size < sizeof(Header) || (reinterpret_cast<uptr>(data) & 3))
			return false;
		Header& h = out.h;
		std::memcpy(&h, data, sizeof(h));
		const s32 words = static_cast<s32>(mVU.microMemSize / 4);
		if (h.magic != kRecordMagic || (h.flags & ~kFlagMask) || h.codeBytes > kMaxCodeBytes || (h.codeBytes & 3) ||
			h.slotCount > kMaxSlots || h.lo > 0 || h.hi < 1 || h.lo <= -words || h.hi >= words)
			return false;
		const bool tiered = (h.flags & kFlagTiered) != 0, declined = (h.flags & kFlagDeclined) != 0;
		if ((declined && !tiered) || (!tiered && (h.flags & kFlagDroppedSticky)))
			return false;

		// The sections in order; their sizes must add up to the record exactly.
		const u64 counts[] = {h.readCount, h.blockCount, h.linkCount, h.dataSlotCount, h.rangeCount, h.keptCount, h.targetWords, h.fixupCount};
		const u64 sizes[] = {sizeof(DiskRead), sizeof(DiskBlock), sizeof(DiskLink), sizeof(DiskPair), sizeof(DiskPair), sizeof(DiskPair),
			sizeof(u32), sizeof(mVUPersist::PersistFixup)};
		u64 at = sizeof(Header);
		const u8* section[8];
		for (int i = 0; i < 8; i++)
		{
			if (counts[i] > size)
				return false;
			section[i] = data + at;
			at += counts[i] * sizes[i];
			if (at > size)
				return false;
		}
		if (at + h.codeBytes != size)
			return false;
		out.reads = reinterpret_cast<const DiskRead*>(section[0]);
		out.blocks = reinterpret_cast<const DiskBlock*>(section[1]);
		out.links = reinterpret_cast<const DiskLink*>(section[2]);
		out.dataSlots = reinterpret_cast<const DiskPair*>(section[3]);
		out.ranges = reinterpret_cast<const DiskPair*>(section[4]);
		out.kept = reinterpret_cast<const DiskPair*>(section[5]);
		out.targets = reinterpret_cast<const u32*>(section[6]);
		out.fixups = section[7];
		out.code = data + at;

		// The read set: ascending, inside the bounds, and the root pair in it.
		if (h.readCount < 2)
			return false;
		bool rootUpper = false;
		for (u32 i = 0; i < h.readCount; i++)
		{
			const DiskRead& r = out.reads[i];
			if (r.off < h.lo || r.off > h.hi || (i && r.off <= out.reads[i - 1].off) || (r.value & ~r.mask))
				return false;
			rootUpper = rootUpper || (r.off == 1 && r.mask == ~0u);
		}
		if (!rootUpper)
			return false;

		if (declined)
			return h.codeBytes == 0 && h.slotCount == 0 && !h.blockCount && !h.linkCount && !h.dataSlotCount && !h.rangeCount &&
				   !h.keptCount && !h.targetWords && !h.fixupCount;

		// Blocks, links and data slots: each slot of the run named once.
		if (!h.blockCount || !h.codeBytes || (tiered && h.linkCount) ||
			static_cast<u64>(h.blockCount) + h.linkCount + h.dataSlotCount != h.slotCount)
			return false;
		std::vector<u8> slotUsed(h.slotCount, 0);
		const auto takeSlot = [&](u32 slot) {
			if (slot >= h.slotCount || slotUsed[slot])
				return false;
			slotUsed[slot] = 1;
			return true;
		};
		const auto pairInside = [&](s32 pairOff) { return pairOff > -words && pairOff < words && pairOff * 2 >= h.lo && pairOff * 2 + 1 <= h.hi; };
		for (u32 i = 0; i < h.blockCount; i++)
		{
			const DiskBlock& blk = out.blocks[i];
			if (!takeSlot(blk.slot) || !pairInside(blk.pairOff) || (blk.stubOff & 3) || (blk.bodyOff & 3) || blk.stubOff > blk.bodyOff ||
				blk.bodyOff >= h.codeBytes || blk.hasJumpTable > 1)
				return false;
		}
		// (the first block is the root: the head is found by its entry state)
		if (out.blocks[0].pairOff != 0 || std::memcmp(out.blocks[0].pState, h.rootState, sizeof(microRegInfo)) != 0)
			return false;
		for (u32 i = 0; i < h.linkCount; i++)
			if (!takeSlot(out.links[i].slot) || !pairInside(out.links[i].pairOff))
				return false;
		for (u32 i = 0; i < h.dataSlotCount; i++)
			if (out.dataSlots[i].a < 0 || !takeSlot(static_cast<u32>(out.dataSlots[i].a)) ||
				(out.dataSlots[i].b != mVUslotZero && out.dataSlots[i].b != mVUslotProgram))
				return false;
		for (u32 i = 0; i < h.rangeCount; i++)
			if (out.ranges[i].a < h.lo * 4 || out.ranges[i].a > h.hi * 4 + 4 || (out.ranges[i].a & 3) || out.ranges[i].b > 1)
				return false;
		for (u32 i = 0; i < h.keptCount; i++)
			if (out.kept[i].a < h.lo * 4 || out.kept[i].a > h.hi * 4 || (out.kept[i].a & 3))
				return false;
		for (u32 i = 0; i < h.targetWords;)
		{
			if (h.targetWords - i < 2 || out.targets[i] >= mVU.microMemSize || (out.targets[i] & 7) || out.targets[i + 1] > h.targetWords - i - 2)
				return false;
			const u32 n = out.targets[i + 1];
			for (u32 k = 0; k < n; k++)
				if (out.targets[i + 2 + k] >= mVU.microMemSize || (out.targets[i + 2 + k] & 7))
					return false;
			i += 2 + n;
		}

		// Fixups: ascending, apart, inside the code, of a form and target a
		// body can have.
		u32 nextFree = 0;
		for (u32 i = 0; i < h.fixupCount; i++)
		{
			const mVUPersist::PersistFixup f = out.Fixup(i);
			if (f.form != mVUPersist::kFormMovRun && f.form != mVUPersist::kFormAdrp && f.form != mVUPersist::kFormRel26 &&
				f.form != mVUPersist::kFormRel19)
				return false;
			if (f.form == mVUPersist::kFormMovRun && (f.width < 1 || f.width > 4))
				return false;
			const u64 end = static_cast<u64>(f.codeOffset) + mVUPersist::FixupSpanInsns(f) * 4;
			if ((f.codeOffset & 3) || f.codeOffset < nextFree || end > h.codeBytes)
				return false;
			nextFree = static_cast<u32>(end);
			switch (f.target)
			{
				case mVUPersist::kTargetStub:
					if (f.index >= mVUPersist::kStubCount || !mVUPersist::StubAddress(mVU, static_cast<u8>(f.index)))
						return false;
					break;
				case mVUPersist::kTargetBody:
					if (f.offset < 0 || f.offset >= static_cast<s64>(h.codeBytes) || f.form != mVUPersist::kFormMovRun)
						return false;
					break;
				case mVUPersist::kTargetImage:
				case mVUPersist::kTargetData:
					if (f.offset < 0 || !mVUPersist::GetRegions().valid)
						return false;
					break;
				default:
					return false;
			}
		}
		return true;
	}

	// Whether every fixup of `p` matches the instruction at its site and can
	// hold its target with the code at `base`.
	static bool FixupsFit(microVU& mVU, const Parsed& p, const u8* base)
	{
		for (u32 i = 0; i < p.h.fixupCount; i++)
		{
			const mVUPersist::PersistFixup f = p.Fixup(i);
			u32 insn[4] = {};
			std::memcpy(insn, p.code + f.codeOffset, mVUPersist::FixupSpanInsns(f) * 4);
			if (mVUPersist::PatchSite(insn, base + f.codeOffset, f, FixupValue(mVU, f, base), false) != mVUPersist::PatchResult::Ok)
				return false;
		}
		return true;
	}

	// Makes a body of a parsed record: for one with code, the code is copied to
	// the cursor and patched. Null when the code cache has no room or a fixup
	// does not fit; `reject` then says whether the record itself is at fault.
	static std::unique_ptr<mVUChunkBody> Materialize(microVU& mVU, const Parsed& p, bool& reject)
	{
		reject = false;
		const Header& h = p.h;
		auto body = std::make_unique<mVUChunkBody>();
		mVUChunkBody& b = *body;
		b.tiered = (h.flags & kFlagTiered) != 0;
		b.declined = (h.flags & kFlagDeclined) != 0;
		b.droppedSticky = (h.flags & kFlagDroppedSticky) != 0;
		b.lo = h.lo;
		b.hi = h.hi;
		b.slotCount = h.slotCount;
		b.codeBytes = h.codeBytes;
		b.statusImm = h.statusImm;
		b.inPack = true;
		b.reads.reserve(h.readCount);
		for (u32 i = 0; i < h.readCount; i++)
			b.reads.push_back({p.reads[i].off, p.reads[i].value, p.reads[i].mask});
		if (b.declined)
			return body;

		const u8* const cacheEnd = mVU.prog.x86end + (mVUcacheSafeZone * _1mb) / 2;
		if (armGetCurrentCodePointer() + h.codeBytes + 4096 > cacheEnd)
			return nullptr;
		while (armAsm->GetCursorOffset() & 15)
			armAsm->Nop();
		u8* const base = armGetCurrentCodePointer();
		if (!FixupsFit(mVU, p, base))
		{
			reject = true;
			return nullptr;
		}
		{
			vixl::CodeBufferCheckScope scope(armAsm, h.codeBytes, vixl::CodeBufferCheckScope::kReserveBufferSpace,
				vixl::CodeBufferCheckScope::kNoAssert);
			armAsm->GetBuffer()->EmitData(p.code, h.codeBytes);
		}
		for (u32 i = 0; i < h.fixupCount; i++)
		{
			const mVUPersist::PersistFixup f = p.Fixup(i);
			u8* const at = base + f.codeOffset;
			mVUPersist::PatchSite(reinterpret_cast<u32*>(armGetWritableCodePtr(at)), at, f, FixupValue(mVU, f, base), true);
		}

		b.code = base;
		b.blocks.reserve(h.blockCount);
		for (u32 i = 0; i < h.blockCount; i++)
		{
			const DiskBlock& d = p.blocks[i];
			mVUChunkBody::Block blk;
			blk.pairOff = d.pairOff;
			blk.slot = d.slot;
			blk.stub = base + d.stubOff;
			blk.body = base + d.bodyOff;
			blk.jumpCache = d.hasJumpTable ? mVUnewJumpTable(mVU) : nullptr;
			std::memcpy(&blk.pState, d.pState, sizeof(microRegInfo));
			std::memcpy(&blk.pStateEnd, d.pStateEnd, sizeof(microRegInfo));
			b.blocks.push_back(blk);
		}
		for (u32 i = 0; i < h.linkCount; i++)
		{
			mVUChunkBody::Link l;
			l.pairOff = p.links[i].pairOff;
			l.slot = p.links[i].slot;
			std::memcpy(&l.key, p.links[i].key, sizeof(microRegInfo));
			b.links.push_back(l);
		}
		for (u32 i = 0; i < h.dataSlotCount; i++)
			b.dataSlots.push_back({static_cast<u32>(p.dataSlots[i].a), static_cast<u8>(p.dataSlots[i].b)});
		for (u32 i = 0; i < h.rangeCount; i++)
			b.ranges.push_back({p.ranges[i].a, p.ranges[i].b != 0});
		for (u32 i = 0; i < h.keptCount; i++)
			b.keptBits.push_back({p.kept[i].a, p.kept[i].b});
		for (u32 i = 0; i < h.targetWords;)
		{
			const u32 n = p.targets[i + 1];
			std::vector<u32>& v = b.targets[p.targets[i]];
			v.assign(p.targets + i + 2, p.targets + i + 2 + n);
			i += 2 + n;
		}
		return body;
	}

	// Whether the record's read set is micro memory as it is at `rootPC`.
	static bool ReadsFit(microVU& mVU, const Parsed& p, u32 rootPC)
	{
		const u32* const mem = reinterpret_cast<const u32*>(mVU.regs().Micro);
		const s32 rootWord = static_cast<s32>(rootPC / 4);
		if (rootWord + p.h.lo < 0 || rootWord + p.h.hi >= static_cast<s32>(mVU.microMemSize / 4))
			return false;
		for (u32 i = 0; i < p.h.readCount; i++)
			if ((mem[rootWord + p.reads[i].off] ^ p.reads[i].value) & p.reads[i].mask)
				return false;
		return true;
	}

	static bool SameReads(const mVUChunkBody& a, const mVUChunkBody& b)
	{
		return a.reads.size() == b.reads.size() &&
			   std::equal(a.reads.begin(), a.reads.end(), b.reads.begin(),
				   [](const mVUChunkBody::Word& x, const mVUChunkBody::Word& y) { return x.off == y.off && x.value == y.value && x.mask == y.mask; });
	}
} // namespace mVUChunkPack

// Whether bodies are kept on disk now: the cache is up for VU1.
static bool mVUchunkPackOn(microVU& mVU)
{
	return mVU.index == 1 && mVUProgCache::BodiesEnabled(1);
}

// The store gained `b`, a body of the routine whose root has the entry state
// `rootState` and the index hash `rootHash`: writes it to the pack's table.
static void mVUchunkPackSave(microVU& mVU, const microRegInfo& rootState, u64 rootHash, mVUChunkBody& b)
{
	if (b.inPack || !mVUchunkPackOn(mVU))
		return;
	mVUChunkStore::Stats& st = s_mVUchunkStats;
	if (!b.declined && (!b.packable || !mVUChunkPack::CodeIsSelfContained(b.code, b.codeBytes, b.fixups)))
	{
		st.packUnpackable++;
		b.fixups = {};
		return;
	}
	std::vector<u8>& bytes = mVU.chunkStore->scratchRecord;
	mVUChunkPack::Serialize(rootState, b, bytes);
	b.fixups = {};
	if (!mVUProgCache::StoreBody(1, rootHash, bytes.data(), bytes.size(), b.tiered))
		return;
	b.inPack = true;
	st.packSaved++;
	st.packSavedBytes += bytes.size();
}

// The first lookup since the last reset of the routine whose root is at
// `rootPC` with the entry state `state`: makes a body of each record the pack
// holds for it and files it under the routine's head. Runs inside the lookup's
// outermost compile, before anything of that compile is emitted.
static void mVUchunkPackFetch(microVU& mVU, u32 rootPC, const microRegInfo& state)
{
	mVUChunkStoreData& store = *mVU.chunkStore;
	const u64 rootHash = mVUchunkRootHash(mVU, rootPC, &state);
	if (!store.packAsked.insert(rootHash).second)
		return;
	std::vector<std::vector<u8>>& records = store.scratchRecords;
	records.clear();
	mVUProgCache::FetchBodies(1, rootHash, records);
	if (records.empty())
		return;

	mVUChunkStore::Stats& st = s_mVUchunkStats;
	const auto t0 = std::chrono::steady_clock::now();
	const bool tierOn = mVUvuAotEnabled();
	bool sawTiered = false, settled = false;
	// (the records come oldest first: this many plain ones, and this many of
	// the tier's, are passed over)
	size_t plainToSkip = 0, tieredToSkip = 0;
	{
		size_t plain = 0, tiered = 0;
		for (const std::vector<u8>& rec : records)
		{
			mVUChunkPack::Header h;
			if (rec.size() >= sizeof(h))
			{
				std::memcpy(&h, rec.data(), sizeof(h));
				if (h.magic == mVUChunkPack::kRecordMagic && std::memcmp(h.rootState, &state, sizeof(microRegInfo)) == 0)
					((h.flags & mVUChunkPack::kFlagTiered) ? tiered : plain)++;
			}
		}
		// A routine with an immediate compiled in that a game rewrites between
		// uploads gets a record per value; without a bound a later session
		// would make a body of every one of them here. Past what a head keeps,
		// the records leave the pack.
		plainToSkip = plain > mVUchunkMaxPlainPerHead ? plain - mVUchunkMaxPlainPerHead : 0;
		tieredToSkip = (tierOn && tiered > mVUChunkPack::kMaxTieredPerRoot) ? tiered - mVUChunkPack::kMaxTieredPerRoot : 0;
	}
	for (const std::vector<u8>& rec : records)
	{
		if (rec.size() == sizeof(mVUChunkPack::SettledRecord))
		{
			mVUChunkPack::SettledRecord sr;
			std::memcpy(&sr, rec.data(), sizeof(sr));
			if (sr.magic == mVUChunkPack::kSettledMagic)
			{
				settled = settled || std::memcmp(sr.rootState, &state, sizeof(microRegInfo)) == 0;
				continue;
			}
		}
		mVUChunkPack::Parsed p;
		bool reject = !mVUChunkPack::Parse(mVU, rec.data(), rec.size(), p);
		// (another routine under the same hash)
		if (!reject && std::memcmp(p.h.rootState, &state, sizeof(microRegInfo)) != 0)
			continue;
		const bool tiered = !reject && (p.h.flags & mVUChunkPack::kFlagTiered);
		if (tiered && !tierOn)
			continue;
		if (!reject && (tiered ? tieredToSkip : plainToSkip))
		{
			(tiered ? tieredToSkip : plainToSkip)--;
			st.packDropped++;
			mVUProgCache::DropBody(1, rootHash, rec.data(), rec.size());
			continue;
		}
		sawTiered = sawTiered || (tiered && !(p.h.flags & mVUChunkPack::kFlagDeclined) && mVUChunkPack::ReadsFit(mVU, p, rootPC));
		std::unique_ptr<mVUChunkBody> body;
		if (!reject)
			body = mVUChunkPack::Materialize(mVU, p, reject);
		if (reject)
		{
			// Not this build's or not whole: it goes, and the routine compiles.
			st.packRejected++;
			mVUProgCache::DropBody(1, rootHash, rec.data(), rec.size());
			continue;
		}
		if (!body)
		{
			st.missNoRoom++;
			continue;
		}
		mVUChunkHead* const head = mVUchunkFindHead(mVU, rootPC, state, true);
		std::vector<std::unique_ptr<mVUChunkBody>>& list = tiered ? head->tiered : head->plain;
		// (a routine can have one tiered body with the sticky byte and one without, over the same words)
		if (std::any_of(list.begin(), list.end(), [&](const auto& have) {
				return have->droppedSticky == body->droppedSticky && have->declined == body->declined && mVUChunkPack::SameReads(*have, *body);
			}))
			continue;
		st.packLoaded++;
		st.packLoadedCodeBytes += body->codeBytes;
		if (tiered)
			mVUvuAotOnBodyLoaded(mVU, *head, std::move(body));
		else
			mVUchunkAddPlain(mVU, *head, std::move(body));
	}
	if (tierOn && (sawTiered || settled))
	{
		mVUChunkHead* const head = mVUchunkFindHead(mVU, rootPC, state, true);
		head->settled = head->settled || settled;
		// A tiered body of this very code was in the pack. If none of the
		// head's can be placed for it now, the copy asks at its first run.
		const mVUChunkBody* const usable = sawTiered ? mVUchunkTieredFor(mVU, *head, rootPC) : nullptr;
		if (sawTiered && (!usable || usable->declined))
		{
			head->tieredInPack = true;
			head->next[0] = std::min(head->next[0], head->runs[0] + 1);
		}
	}
	st.packLoadNs += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

// The tier has stopped looking for new jump targets in the routine.
static void mVUchunkPackSaveSettled(microVU& mVU, const microRegInfo& rootState, u64 rootHash)
{
	if (!mVUchunkPackOn(mVU))
		return;
	mVUChunkPack::SettledRecord sr = {};
	sr.magic = mVUChunkPack::kSettledMagic;
	std::memcpy(sr.rootState, &rootState, sizeof(microRegInfo));
	mVUProgCache::StoreBody(1, rootHash, reinterpret_cast<const u8*>(&sr), sizeof(sr), true);
}

// The shape of the code the store's plain bodies hold, for the test that pins
// the emitter against the compiler ABI version: every body's blocks, links,
// fixups and code, with the operands that differ between two correct runs
// masked (mVUPersist::CanonicalizeInsn, and the slot numbers in the entry
// stubs). False when the store holds no body, or one whose host addresses
// were not recorded.
static bool mVUchunkTestEmitDigest(u64& out)
{
	using mVUChunkPack::Put;
	out = 0;
	const mVUChunkStoreData* const store = microVU1.chunkStore.get();
	if (!store)
		return false;
	std::vector<u8> canon;
	u32 bodies = 0;
	for (const auto& head : store->heads)
	{
		for (const auto& body : head->plain)
		{
			const mVUChunkBody& b = *body;
			if (!b.packable || b.inPack || (b.codeBytes & 3))
				return false;
			bodies++;
			Put(canon, static_cast<u32>(b.blocks.size()));
			Put(canon, static_cast<u32>(b.links.size()));
			Put(canon, b.slotCount);
			Put(canon, b.codeBytes);
			Put(canon, static_cast<u32>(b.fixups.size()));
			std::vector<u8> masked(b.codeBytes / 4, 0);
			for (const mVUChunkBody::Block& blk : b.blocks)
			{
				const u32 stubOff = static_cast<u32>(blk.stub - b.code);
				Put(canon, blk.pairOff);
				Put(canon, blk.slot);
				Put(canon, stubOff);
				Put(canon, static_cast<u32>(blk.body - b.code));
				Put(canon, static_cast<u32>(blk.jumpCache ? 1 : 0));
				// (movz, movk: the place word, whose slot bits depend on what was compiled before)
				masked[stubOff / 4] = masked[stubOff / 4 + 1] = 1;
			}
			for (const mVUChunkBody::Link& l : b.links)
			{
				Put(canon, l.pairOff);
				Put(canon, l.slot);
			}
			for (const mVUPersist::PersistFixup& f : b.fixups)
			{
				Put(canon, f.codeOffset);
				Put(canon, f.form);
				Put(canon, f.target);
				Put(canon, f.width);
				Put(canon, f.index);
				if (f.form == mVUPersist::kFormRel19)
					masked[f.codeOffset / 4] = 2;
			}
			for (u32 off = 0; off < b.codeBytes; off += 4)
			{
				u32 insn;
				std::memcpy(&insn, b.code + off, 4);
				u32 c = mVUPersist::CanonicalizeInsn(insn);
				if (masked[off / 4] == 1)
					c &= ~(0xFFFFu << 5);
				else if (masked[off / 4] == 2)
					c &= ~(0x7FFFFu << 5);
				Put(canon, c);
			}
		}
	}
	if (!bodies)
		return false;
	out = XXH3_64bits(canon.data(), canon.size());
	return true;
}

#ifdef PCSX2_RECOMPILER_TESTS
bool mVUChunkStore::TestParseRecord(const u8* data, size_t size)
{
	// (aligned, as the pack's table hands records out)
	std::vector<u32> aligned((size + 3) / 4);
	if (size)
		std::memcpy(aligned.data(), data, size);
	mVUChunkPack::Parsed p;
	return mVUChunkPack::Parse(microVU1, reinterpret_cast<const u8*>(aligned.data()), size, p);
}
#endif
