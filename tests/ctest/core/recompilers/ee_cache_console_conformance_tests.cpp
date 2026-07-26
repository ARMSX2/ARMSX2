// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// EE data/instruction cache conformance against a real PS2.
// A probe was run on the console over ps2link and its capture reshaped into
// autocases_eecache.h.
//
// PCSX2 models the R5900 D-cache in pcsx2/Cache.cpp; the arm64 EE recompiler
// models nothing (`recCACHE() {}`). Two of the model's own comments admit to
// guessing:
//
//   DXLTG  "Our tags don't contain PS2 paddrs (instead they contain x86
//          addrs)"  -> TagLo = line.tag.flags(), so the address half is gone.
//   DXLTG  "demands that SYNC.L is called before this command, which forces
//          the cache to write back ... For speed, we will do it here."
//          -> PCSX2 flushes a dirty line on a tag READ.
//
// Both are wrong, and both are load-bearing: ps2sdk's own _SyncDCache /
// _InvalidDCache (ee/kernel/src/kernel.S) issue DXLTG and then range-check
// `(TagLo & 0xFFFFF000) + index` against the caller's buffer to decide
// whether to touch each line. Under PCSX2 that address is always zero.
//
// What the console reported, in one run of 13 cases:
//
//   * DXLTG returns PTagLo in bits 31:12 with the flags in 6..3 (D V R L).
//     Bits 10..7 and 2..0 do not exist: DXSTG accepts 0xFFFFFFBF and reads
//     back 0xFFFFF038.
//   * DXLTG does NOT write a dirty line back, and reports D=1. PCSX2 does the
//     opposite on both halves of that sentence.
//   * Invalidating clears V and D but leaves the physical tag readable, and
//     DXIN leaves the LRF bit alone. PCSX2's clear() keeps LRF and drops the
//     address.
//   * Replacement is exactly `way = LRF0 ^ LRF1` with the filled way's LRF
//     toggling -- which is precisely what PCSX2 already does. Five fills into
//     one set alternate 0,1,0,1,0 on both.
//   * A hit op issued at the 0x20000000 uncached alias of a dirty line did
//     not write it back; the same op at the cached address did.
//   * The instruction cache is real and readable: `cache 0x00` / `cache 0x01`
//     return its tags and its instruction words, and the words matched the
//     ELF byte for byte. PCSX2 implements neither op and has no I-cache.
//
// The replay drives PCSX2's model through its own entry points -- readCache32
// / writeCache32 for cached accesses, memRead32 / memWrite32 for uncached
// ones, and the interpreter's CACHE opcode for the cache ops -- so a
// divergence here is the model's, not the harness's.
//
// Two classes of observation are deliberately NOT scored, each with its
// reason carried in the generated header next to the number:
//
//   * which of the two ways a line lands in. Invalidation preserves LRF, so
//     the console's fills went where its history sent them and a model reset
//     to all-zero sends them elsewhere. Both obey the same rule, and
//     ReplacementIsLrfXorWithToggle is what pins the rule.
//   * anything reached through the 0x20000000 alias. Memory.cpp unmaps
//     0x20000000..0x7FFFFFFF, so in this harness that address is not a second
//     view of the line at all.
//
// Tag observations are never compared way by way: PCSX2's DXLTG returns no
// address, so there is no way to ask it which line a way holds. What IS
// comparable is how many ways hold a valid, or dirty, line of the probe's own
// buffer, and that is what ValidAndDirtyLineCountsMatchConsole checks.
//
// Divergences are recorded from the real run, never derived from a rule.
// DISABLED_AllEeCacheMatchesConsole is the graduation tripwire.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Cache.h"
#include "Memory.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#include <cstdio>
#include <vector>

#include "autocases_eecache.h"

using namespace console_eecache;
using recompiler_tests::EeRecTestHarness;
using recompiler_tests::RecompilerTestEnvironment;

namespace
{
// The harness scratch region. Five lines a page apart share one cache set,
// exactly as the probe's did.
constexpr u32 kBase = 0x00020000;
constexpr u32 kStride = 0x1000;

// KSEG0 and KSEG1 both map to physical 0 in PCSX2 (Memory.cpp, vtlb_VMap of
// 0x80000000 and 0xA0000000). KUSEG from 0x20000000 up is explicitly unmapped
// (vtlb_VMapUnmap(0x20000000, 0x60000000)), which is why the probe's own
// uncached alias cannot be replayed here.
constexpr u32 kKseg0 = 0x80000000;

constexpr u32 kFlagDirty = 0x40;
constexpr u32 kFlagValid = 0x20;

constexpr u32 kCacheOpcode = 0x2Fu;

void RunCacheOp(u32 op, u32 addr)
{
	// CACHE rt, offset(rs) -- the interpreter reads rs, rt and the immediate
	// straight out of cpuRegs.code.
	cpuRegs.GPR.r[mips::reg::t0].UD[0] = static_cast<s64>(static_cast<s32>(addr));
	cpuRegs.code = (kCacheOpcode << 26) | (static_cast<u32>(mips::reg::t0) << 21) |
	               ((op & 0x1F) << 16);
	R5900::Interpreter::OpcodeImpl::CACHE();
}

u32 ReadTag(u32 addr)
{
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x10, addr); // DXLTG
	return cpuRegs.CP0.n.TagLo;
}

u32 SymValue(EeCacheSym s, const EeCacheCase& c)
{
	switch (s)
	{
		case ECC_S_ZERO: return 0;
		case ECC_S_PRESET: return c.preset;
		case ECC_S_MAGIC: return c.magic;
		case ECC_S_MAGIC1: return c.magic + 1;
		case ECC_S_MAGIC2: return c.magic + 2;
		case ECC_S_MAGIC15: return c.magic + 15;
		case ECC_S_PAYLOAD: return 0xDEADBE00u | static_cast<u32>(c.id);
		case ECC_S_TAG_PADDR_V: return (kBase & 0x1FFFF000u) | kFlagValid;
		case ECC_S_TAG_ALLBUTD: return 0xFFFFFFFFu & ~kFlagDirty;
		default: return 0;
	}
}

EeCacheSym SymOf(u32 v, const EeCacheCase& c)
{
	for (EeCacheSym s : {ECC_S_ZERO, ECC_S_PRESET, ECC_S_MAGIC, ECC_S_MAGIC1,
	                     ECC_S_MAGIC2, ECC_S_MAGIC15, ECC_S_PAYLOAD,
	                     ECC_S_TAG_PADDR_V, ECC_S_TAG_ALLBUTD})
	{
		if (SymValue(s, c) == v)
			return s;
	}
	return ECC_S_OTHER;
}

const char* SymName(EeCacheSym s)
{
	switch (s)
	{
		case ECC_S_ZERO: return "ZERO";
		case ECC_S_PRESET: return "PRESET";
		case ECC_S_MAGIC: return "MAGIC";
		case ECC_S_MAGIC1: return "MAGIC+1";
		case ECC_S_MAGIC2: return "MAGIC+2";
		case ECC_S_MAGIC15: return "MAGIC+15";
		case ECC_S_PAYLOAD: return "PAYLOAD";
		case ECC_S_TAG_PADDR_V: return "PADDR|V";
		case ECC_S_TAG_ALLBUTD: return "ALLBUTD";
		default: return "OTHER";
	}
}

// Runs one case's step list against PCSX2's model and returns what each
// observed step produced, in the console's order.
std::vector<u32> Replay(const EeCacheCase& c)
{
	std::vector<u32> out;
	resetCache();

	// Seed every line with a word the case never writes, so an observation
	// that reads the wrong line is visible rather than plausible.
	for (u32 k = 0; k < 5; k++)
	{
		for (u32 w = 0; w < 64; w += 4)
			memWrite32(kBase + kStride * k + w, 0xBADF00D0u | k);
	}

	for (int i = 0; i < c.step_count; i++)
	{
		const EeCacheStep& s = c.steps[i];
		const u32 addr = kBase + static_cast<u32>(s.off);
		u32 v = 0;
		switch (s.kind)
		{
			case ECC_CACHE: RunCacheOp(s.op, addr); break;
			case ECC_CACHE_U:
				// 0x20000000 is unmapped here; every observation downstream of
				// this op is marked unscored in the generated header.
				break;
			case ECC_ST_C: writeCache32(addr, SymValue(s.sym, c)); break;
			case ECC_ST_U: memWrite32(addr, SymValue(s.sym, c)); break;
			case ECC_LD_C: v = readCache32(addr); break;
			case ECC_LD_U: v = memRead32(addr); break;
			case ECC_TAGLO_WR: cpuRegs.CP0.n.TagLo = SymValue(s.sym, c); break;
			case ECC_TAGHI_WR: cpuRegs.CP0.n.TagHi = SymValue(s.sym, c); break;
			case ECC_TAGLO_RD: v = cpuRegs.CP0.n.TagLo; break;
			case ECC_TAGHI_RD: v = cpuRegs.CP0.n.TagHi; break;
		}
		if (s.observed)
			out.push_back(v);
	}
	return out;
}

const EeCacheCase& CaseById(int id)
{
	for (const EeCacheCase& c : kCases)
	{
		if (c.id == id)
			return c;
	}
	ADD_FAILURE() << "no case " << id;
	return kCases[0];
}

// ---------------------------------------------------------------- divergences
//
// Recorded from a run of this file against the console capture. `slot` is the
// observation index within the case; the strings are what each side produced.

struct ValueDivergence
{
	int case_id;
	int slot;
	const char* console;
	const char* pcsx2;
	const char* cause;
};

constexpr ValueDivergence kValueDivergences[] = {
	// PCSX2's DXLTG calls writeBackIfNeeded() before reading the flags, so a
	// tag read flushes the line. The console's DXLTG leaves RAM alone -- the
	// dirty word is still only in the cache after two of them.
	{5, 3, "PRESET", "MAGIC", "Cache.cpp DXLTG writes the line back"},
};

struct CountDivergence
{
	int case_id;
	int snap;
	int console_valid;
	int console_dirty;
	int pcsx2_valid;
	int pcsx2_dirty;
	const char* cause;
};

// Every one of these is the same root cause as the row above: PCSX2 flushes
// inside DXLTG, so its DXLTG can never report a dirty line. The dirty count
// is 1 on the console and 0 in the model wherever a dirty line is resident.
constexpr CountDivergence kCountDivergences[] = {
	{5, 0, 1, 1, 1, 0, "DXLTG cleared D by writing back"},
	{5, 1, 1, 1, 1, 0, "DXLTG cleared D by writing back"},
	{9, 0, 1, 1, 1, 0, "DXLTG cleared D by writing back"},
	{10, 0, 1, 1, 1, 0, "DXLTG cleared D by writing back"},
	{11, 0, 1, 1, 1, 0, "DXLTG cleared D by writing back"},
};

template <typename T, size_t N>
constexpr int Count(const T (&)[N])
{
	return static_cast<int>(N);
}

bool IsRecordedValue(int cid, int slot)
{
	for (const ValueDivergence& d : kValueDivergences)
	{
		if (d.case_id == cid && d.slot == slot)
			return true;
	}
	return false;
}

const CountDivergence* FindCount(int cid, int snap)
{
	for (const CountDivergence& d : kCountDivergences)
	{
		if (d.case_id == cid && d.snap == snap)
			return &d;
	}
	return nullptr;
}

} // namespace

// ===========================================================================
// The replay
// ===========================================================================

// Every memory word the probe observed, on both engines, except the ones the
// header marks unscored. This is where the model's visible behaviour is
// pinned: write-allocate, write-back, which cache ops reach RAM, and what
// DXLDT and DXSDT put where.
TEST(EeCacheConsole, MemoryVisibilityMatchesConsole)
{
	EeRecTestHarness h;
	int scored = 0, skipped = 0, diverged = 0;
	for (int cid : kReplayCases)
	{
		const EeCacheCase& c = CaseById(cid);
		const std::vector<u32> got = Replay(c);
		ASSERT_EQ(static_cast<int>(got.size()), c.obs_count)
			<< "case " << c.name << ": the step table and the capture disagree "
			<< "on how many observations it makes";
		for (int i = 0; i < c.obs_count; i++)
		{
			const EeCacheObs& e = c.obs[i];
			if (e.is_tag)
				continue; // ValidAndDirtyLineCountsMatchConsole scores these
			if (!e.scored)
			{
				skipped++;
				EXPECT_NE(e.note, nullptr) << "an unscored observation needs a reason";
				continue;
			}
			scored++;
			const EeCacheSym gs = SymOf(got[i], c);
			const bool match = (gs == e.sym);
			if (IsRecordedValue(cid, i))
			{
				diverged++;
				EXPECT_FALSE(match)
					<< "case " << c.name << " r" << i << " now MATCHES the console ("
					<< SymName(e.sym) << "). Drop it from kValueDivergences.";
			}
			else
			{
				EXPECT_TRUE(match)
					<< "case " << c.name << " r" << i << ": console " << SymName(e.sym)
					<< " (" << std::hex << e.raw << ") vs pcsx2 " << SymName(gs) << " ("
					<< got[i] << ")";
			}
		}
	}
	EXPECT_EQ(diverged, Count(kValueDivergences))
		<< "a recorded value divergence did not reproduce";
	// Coverage floor: if a future change stops scoring observations, this
	// notices before the suite quietly shrinks.
	EXPECT_GE(scored, 30);
	EXPECT_EQ(skipped, 4);
}

// Tags cannot be compared way by way -- PCSX2's DXLTG returns no address --
// but "how many ways hold a valid line of the probe's buffer, and how many
// hold a dirty one" is the same question on both sides.
TEST(EeCacheConsole, ValidAndDirtyLineCountsMatchConsole)
{
	EeRecTestHarness h;
	int checked = 0, diverged = 0;
	for (int cid : kReplayCases)
	{
		const EeCacheCase& c = CaseById(cid);
		if (c.snap_count == 0)
			continue;
		const std::vector<u32> got = Replay(c);
		ASSERT_EQ(static_cast<int>(got.size()), c.obs_count);
		for (int s = 0; s < c.snap_count; s++)
		{
			const EeCacheSnapshot& sn = c.snaps[s];
			const u32 t0 = got[sn.obs0], t1 = got[sn.obs1];
			const int gv = ((t0 & kFlagValid) ? 1 : 0) + ((t1 & kFlagValid) ? 1 : 0);
			const int gd = ((t0 & kFlagDirty) ? 1 : 0) + ((t1 & kFlagDirty) ? 1 : 0);
			checked++;
			const CountDivergence* rec = FindCount(cid, s);
			if (rec)
			{
				diverged++;
				EXPECT_EQ(rec->console_valid, sn.valid_self);
				EXPECT_EQ(rec->console_dirty, sn.dirty_self);
				EXPECT_EQ(gv, rec->pcsx2_valid) << "case " << c.name << " snap " << s;
				EXPECT_EQ(gd, rec->pcsx2_dirty) << "case " << c.name << " snap " << s;
				EXPECT_NE(gd, sn.dirty_self)
					<< "case " << c.name << " snap " << s
					<< " now agrees with the console. Drop it from kCountDivergences.";
			}
			else
			{
				EXPECT_EQ(gv, sn.valid_self)
					<< "case " << c.name << " snap " << s << ": valid-line count";
				EXPECT_EQ(gd, sn.dirty_self)
					<< "case " << c.name << " snap " << s << ": dirty-line count";
			}
		}
	}
	EXPECT_GE(checked, 15);
	EXPECT_EQ(diverged, Count(kCountDivergences));
}

// ===========================================================================
// The four defects, each stated once with a witness
// ===========================================================================

// DEFECT 1 of 4. The console's DXLTG puts the line's physical address in bits
// 31:12; PCSX2's returns `line.tag.flags()`, which is at most 0x7FF. This one
// row explains every tag address in the capture, and it is the reason the
// replay above compares line COUNTS rather than tags.
TEST(EeCacheConsole, Pcsx2DxltgReturnsNoPhysicalAddress)
{
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0x11112222u);
	EXPECT_EQ(readCache32(kBase), 0x11112222u);

	const u32 tag = ReadTag(kBase);
	EXPECT_TRUE(tag & kFlagValid) << "the line should be resident";
	EXPECT_LT(tag, 0x800u)
		<< "PCSX2 grew an address field in DXLTG: " << std::hex << tag;

	// The console, on the same shape of sequence, returned base|V.
	const EeCacheCase& c = CaseById(2);
	const EeCacheObs& live = c.obs[2];
	EXPECT_EQ(live.raw, (kConsoleBase & 0xFFFFF000u) | kFlagValid);
	EXPECT_EQ(live.line, 0) << "the console tag names the line under test";

	// ps2sdk's _SyncDCache/_InvalidDCache reconstruct a line's address as
	// (TagLo & 0xFFFFF000) + index and skip any line outside the caller's
	// buffer. Under PCSX2 that reconstruction yields the index alone.
	EXPECT_EQ(tag & 0xFFFFF000u, 0u);
}

// DEFECT 2 of 4. Cache.cpp calls writeBackIfNeeded() inside DXLTG, on the
// grounds that hardware requires SYNC.L first. The console does neither: the
// dirty word stays in the cache and the tag still reports D.
TEST(EeCacheConsole, Pcsx2DxltgWritesBackAndLosesTheDirtyBit)
{
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0xA5A5A5A5u);
	writeCache32(kBase, 0x5A5A5A5Au);
	ASSERT_EQ(memRead32(kBase), 0xA5A5A5A5u) << "the store should not write through";

	const u32 tag = ReadTag(kBase);
	EXPECT_TRUE(tag & kFlagValid);
	EXPECT_FALSE(tag & kFlagDirty) << "PCSX2 grew a dirty bit in DXLTG";
	EXPECT_EQ(memRead32(kBase), 0x5A5A5A5Au) << "PCSX2 stopped flushing in DXLTG";

	// Console case 5: the same sequence left RAM at PRESET and the tag at
	// paddr|D|V|R.
	const EeCacheCase& c = CaseById(5);
	EXPECT_EQ(c.obs[3].sym, ECC_S_PRESET) << "console RAM after two DXLTGs";
	EXPECT_EQ(c.obs[2].flags & (kFlagDirty | kFlagValid), kFlagDirty | kFlagValid);
	EXPECT_EQ(c.obs[2].line, 0);
}

// DEFECT 3 of 4. Invalidation. CacheTag::clear() is `rawValue &= LRF_FLAG`,
// which keeps the LRF bit and discards the address. The console keeps both --
// after DHIN the tag still names the line, with V and D clear.
TEST(EeCacheConsole, InvalidateKeepsThePhysicalTagOnConsole)
{
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0xA5A5A5A5u);
	writeCache32(kBase, 0x5A5A5A5Au);
	RunCacheOp(0x1A, kBase); // DHIN
	const u32 tag = ReadTag(kBase);
	EXPECT_EQ(tag & (kFlagValid | kFlagDirty), 0u) << "the line should be gone";
	EXPECT_EQ(tag & 0xFFFFF000u, 0u) << "PCSX2 grew a surviving address field";

	// Console case 7 r3: V and D clear, address still the line under test.
	const EeCacheCase& c7 = CaseById(7);
	EXPECT_EQ(c7.obs[3].raw, kConsoleBase & 0xFFFFF000u);
	EXPECT_EQ(c7.obs[3].flags & (kFlagValid | kFlagDirty), 0u);
	EXPECT_EQ(c7.obs[3].line, 0);

	// And DXIN leaves the LRF bit standing as well as the address -- console
	// case 2 r0, read straight after invalidating both ways.
	const EeCacheCase& c2 = CaseById(2);
	EXPECT_EQ(c2.obs[0].flags, 0x10u) << "DXIN cleared more than V and D";
	EXPECT_NE(c2.obs[0].raw & 0xFFFFF000u, 0u);
}

// DEFECT 4 of 4. The tag's implemented bits are PTagLo[31:12] and D V R L at
// 6..3. DXSTG accepted 0xFFFFFFBF and read back 0xFFFFF038, so bits 10..7 and
// 2..0 do not exist. PCSX2 masks with ALL_FLAGS = 0x7FF and reads back 0x7BF,
// keeping all seven of them.
//
// Its own bit 11 (isValidPFN) is NOT reachable this way, which was worth
// checking rather than assuming: setAddr() masks with ALL_BITS = 0xFFF and so
// preserves bit 11 across the store, and flags() masks it back off on the way
// out, so a guest DXSTG can neither set nor read it.
TEST(EeCacheConsole, DxstgWritableMaskIsPtagLoPlus0x38)
{
	EeRecTestHarness h;
	resetCache();
	RunCacheOp(0x16, kBase);     // DXIN way 0
	RunCacheOp(0x16, kBase + 1); // DXIN way 1

	cpuRegs.CP0.n.TagLo = 0xFFFFFFFFu & ~kFlagDirty;
	RunCacheOp(0x12, kBase); // DXSTG
	const u32 back = ReadTag(kBase);

	const EeCacheCase& c = CaseById(12);
	ASSERT_EQ(c.obs[1].raw, 0xFFFFF038u) << "the capture changed under us";
	EXPECT_NE(back, c.obs[1].raw) << "PCSX2 now matches; update the record";
	EXPECT_EQ(back, 0x7BFu) << "PCSX2's readback changed shape";
	// The seven bits hardware drops and PCSX2 keeps.
	EXPECT_EQ(back & ~c.obs[1].raw, 0x787u);
	EXPECT_FALSE(back & 0x800u) << "flags() masks isValidPFN off, as it should";

	// A clean round trip of paddr|V is the part both agree on, modulo the
	// missing address field.
	resetCache();
	RunCacheOp(0x16, kBase);
	cpuRegs.CP0.n.TagLo = (kBase & 0x1FFFF000u) | kFlagValid;
	RunCacheOp(0x12, kBase);
	EXPECT_EQ(ReadTag(kBase) & 0x7Fu, kFlagValid);
	EXPECT_EQ(c.obs[0].flags, kFlagValid);
	EXPECT_EQ(c.obs[0].raw, (kConsoleBase & 0xFFFFF000u) | kFlagValid);
}

// ===========================================================================
// What PCSX2 gets right, pinned so it cannot drift
// ===========================================================================

// Five lines a page apart, read into one two-way set, both ways sampled after
// each fill. The rule -- replace the way given by LRF0 ^ LRF1, then toggle
// that way's LRF -- is computed here from the flags alone, identically on
// both sides, because the flag bits are the only part of the tag PCSX2
// reports.
TEST(EeCacheConsole, ReplacementIsLrfXorWithToggle)
{
	const EeCacheCase& c = CaseById(4);
	ASSERT_EQ(c.snap_count, 6);

	// Console side, straight out of the capture.
	std::vector<int> console_filled;
	for (int s = 1; s < c.snap_count; s++)
	{
		const EeCacheObs& p0 = c.obs[c.snaps[s - 1].obs0];
		const EeCacheObs& p1 = c.obs[c.snaps[s - 1].obs1];
		const EeCacheObs& n0 = c.obs[c.snaps[s].obs0];
		const EeCacheObs& n1 = c.obs[c.snaps[s].obs1];
		ASSERT_NE(n0.raw == p0.raw, n1.raw == p1.raw)
			<< "console fill " << s << " touched both ways or neither";
		console_filled.push_back(n0.raw != p0.raw ? 0 : 1);
		const int lrf0 = (p0.flags >> 4) & 1, lrf1 = (p1.flags >> 4) & 1;
		EXPECT_EQ(console_filled.back(), lrf0 ^ lrf1)
			<< "console fill " << s << ": way != LRF0 ^ LRF1";
	}
	EXPECT_EQ(console_filled, (std::vector<int>{0, 1, 0, 1, 0}));

	EeRecTestHarness h;
	const std::vector<u32> got = Replay(c);
	std::vector<int> pcsx2_filled;
	for (int s = 1; s < c.snap_count; s++)
	{
		const u32 p0 = got[c.snaps[s - 1].obs0], p1 = got[c.snaps[s - 1].obs1];
		const u32 n0 = got[c.snaps[s].obs0], n1 = got[c.snaps[s].obs1];
		const int lrf0 = (p0 >> 4) & 1, lrf1 = (p1 >> 4) & 1;
		// The way that was written is the one whose LRF toggled.
		const bool t0 = ((n0 >> 4) & 1) != lrf0;
		const bool t1 = ((n1 >> 4) & 1) != lrf1;
		ASSERT_NE(t0, t1) << "pcsx2 fill " << s << " toggled both LRF bits or neither";
		pcsx2_filled.push_back(t0 ? 0 : 1);
		EXPECT_EQ(pcsx2_filled.back(), lrf0 ^ lrf1) << "pcsx2 fill " << s;
	}
	EXPECT_EQ(pcsx2_filled, console_filled)
		<< "PCSX2's replacement order no longer matches the console's";
}

// The index is vaddr[11:6] and the way is vaddr[0]; bit 1 is ignored, and a
// line a page away shares the set. Console case 3 says so, and PCSX2 agrees
// -- checked here on the flags, which is all its DXLTG reports.
TEST(EeCacheConsole, IndexAndWaySelectionMatchConsole)
{
	const EeCacheCase& c = CaseById(3);
	// Console: two distinct tags in one set, reachable at offsets 0/1, 2/3
	// and 0x1000/0x1001 alike, and neither of them at offset 0x40.
	EXPECT_EQ(c.obs[0].line, 0);
	EXPECT_EQ(c.obs[1].line, 1);
	EXPECT_EQ(c.obs[2].raw, c.obs[0].raw) << "address bit 1 is not ignored";
	EXPECT_EQ(c.obs[3].raw, c.obs[1].raw);
	EXPECT_EQ(c.obs[4].raw, c.obs[0].raw) << "a page away is not the same set";
	EXPECT_EQ(c.obs[5].raw, c.obs[1].raw);
	EXPECT_LT(c.obs[6].line, 0) << "+0x40 should be a different set";
	EXPECT_LT(c.obs[7].line, 0);

	EeRecTestHarness h;
	resetCache();
	readCache32(kBase);
	readCache32(kBase + kStride);
	const u32 w0 = ReadTag(kBase), w1 = ReadTag(kBase + 1);
	EXPECT_TRUE(w0 & kFlagValid);
	EXPECT_TRUE(w1 & kFlagValid) << "the second line did not land in the other way";
	EXPECT_EQ(ReadTag(kBase + 2) & 0x7Fu, w0 & 0x7Fu);
	EXPECT_EQ(ReadTag(kBase + 3) & 0x7Fu, w1 & 0x7Fu);
	EXPECT_EQ(ReadTag(kBase + kStride) & 0x7Fu, w0 & 0x7Fu);
	EXPECT_FALSE(ReadTag(kBase + 0x40) & kFlagValid) << "+0x40 shares the set";
}

// ===========================================================================
// What PCSX2 does not model at all
// ===========================================================================

// The instruction cache. `cache 0x00` and `cache 0x01` returned a tag naming
// the bait function's line and the instruction words at that line, matching
// the linked ELF exactly -- the generator checks that against the disassembly
// of the probe's ELF, and this test checks the words the data path read back.
// PCSX2 warns "Cache mode 0 not implemented" and leaves TagLo alone.
TEST(EeCacheConsole, InstructionCacheIsReadableOnConsoleAndAbsentInPcsx2)
{
	const EeCacheCase& c = CaseById(13);
	ASSERT_EQ(c.obs_count, 9);

	const int way =
		(c.obs[0].raw & 0xFFFFF000u) == (kConsoleBaitLine & 0xFFFFF000u) ? 0 : 1;
	EXPECT_EQ(c.obs[way].raw & 0xFFFFF000u, kConsoleBaitLine & 0xFFFFF000u)
		<< "IXLTG did not name the bait's line";
	EXPECT_TRUE(c.obs[way].raw & kFlagValid);
	// IXLDT of that way's first two words == what a data load reads there.
	EXPECT_EQ(c.obs[2 + way].raw, c.obs[7].raw);
	EXPECT_EQ(c.obs[4 + way].raw, c.obs[8].raw);
	// TagHi is untouched by the I-cache ops, as by the D-cache ones.
	EXPECT_EQ(c.obs[6].raw, 0u);

	// PCSX2: neither op is implemented, so TagLo keeps whatever it held.
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x00, kBase);
	EXPECT_EQ(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u) << "PCSX2 grew an IXLTG";
	RunCacheOp(0x01, kBase);
	EXPECT_EQ(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u) << "PCSX2 grew an IXLDT";
}

// The console fact, with the mechanism left open. A hit write-back-invalidate
// issued at the 0x20000000 alias of a dirty line did not write it back, and
// the line was still Dirty+Valid afterwards; the identical op at the cached
// address did write it back. Two mechanisms fit -- cache ops suppressed on an
// uncached page, or a tag compare that is virtual rather than physical -- and
// this capture cannot separate them, because the probe's pages are
// identity-mapped so vaddr == paddr.
//
// PCSX2 cannot be asked the same question here (0x20000000 is unmapped in the
// test environment), but its mechanism is alias-blind by construction:
// doCacheHitOp resolves the address to a host pointer and compares that, so
// any two mapped addresses over one physical line hit the same cache line.
// KSEG0 is the alias this environment does map, and it shows exactly that.
TEST(EeCacheConsole, HitOpAtTheUncachedAliasDoesNotHitOnConsole)
{
	const EeCacheCase& c = CaseById(9);
	EXPECT_EQ(c.obs[1].sym, ECC_S_PRESET) << "the alias op wrote the line back";
	EXPECT_FALSE(c.obs[1].scored);
	ASSERT_NE(c.obs[1].note, nullptr);

	// The line survived the alias op, still dirty, and the cached-address op
	// then flushed it.
	ASSERT_GE(c.snap_count, 1);
	const int self =
		c.obs[c.snaps[0].obs0].line >= 0 ? c.snaps[0].obs0 : c.snaps[0].obs1;
	EXPECT_EQ(c.obs[self].flags & (kFlagDirty | kFlagValid), kFlagDirty | kFlagValid);
	EXPECT_EQ(c.obs[4].sym, ECC_S_MAGIC);

	// PCSX2 hits through an alias, shown with the one this environment maps.
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0xA5A5A5A5u);
	writeCache32(kBase, 0x5A5A5A5Au);
	ASSERT_EQ(memRead32(kBase), 0xA5A5A5A5u);
	RunCacheOp(0x18, kKseg0 | kBase); // DHWBIN through KSEG0
	EXPECT_EQ(memRead32(kBase), 0x5A5A5A5Au)
		<< "PCSX2's hit ops stopped being alias-blind";
}

// The arm64 EE recompiler's CACHE is an empty function body, and the
// cache-routed load/store paths in vtlb.cpp are all gated on !CHECK_EEREC, so
// under the recompiler the whole model above is unreachable. Not a defect on
// its own -- PCSX2 ships with EnableEECache off -- but it is why none of these
// divergences can be reached by the JIT, and it should not change silently.
TEST(EeCacheConsole, Arm64RecompilerIgnoresCacheEntirely)
{
	EeRecTestHarness h;
	h.EnableCop0();
	h.SetGpr(mips::reg::t0, kBase);
	h.LoadProgram({(kCacheOpcode << 26) | (static_cast<u32>(mips::reg::t0) << 21) |
	                   (0x18u << 16),
	               mips::NOP});
	resetCache();
	memWrite32(kBase, 0xA5A5A5A5u);
	writeCache32(kBase, 0x5A5A5A5Au);
	h.RunJitNoDiff();
	// The JIT executed a DHWBIN and the dirty line is still only in the cache.
	EXPECT_EQ(memRead32(kBase), 0xA5A5A5A5u)
		<< "the arm64 recompiler grew a CACHE implementation";
}

// ===========================================================================
// Tripwires
// ===========================================================================

TEST(EeCacheConsole, DISABLED_AllEeCacheMatchesConsole)
{
	EeRecTestHarness h;
	for (int cid : kReplayCases)
	{
		const EeCacheCase& c = CaseById(cid);
		const std::vector<u32> got = Replay(c);
		ASSERT_EQ(static_cast<int>(got.size()), c.obs_count);
		for (int i = 0; i < c.obs_count; i++)
		{
			const EeCacheObs& e = c.obs[i];
			if (e.is_tag)
				EXPECT_EQ(got[i], e.raw) << c.name << " r" << i << ": tags still differ";
			else if (e.scored)
				EXPECT_EQ(SymOf(got[i], c), e.sym) << c.name << " r" << i;
		}
	}
}

TEST(EeCacheConsole, DISABLED_DxltgReturnsThePhysicalTag)
{
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0x11112222u);
	readCache32(kBase);
	EXPECT_EQ(ReadTag(kBase), (kBase & 0x1FFFF000u) | kFlagValid);
}

TEST(EeCacheConsole, DISABLED_DxltgLeavesDirtyLinesAlone)
{
	EeRecTestHarness h;
	resetCache();
	memWrite32(kBase, 0xA5A5A5A5u);
	writeCache32(kBase, 0x5A5A5A5Au);
	EXPECT_TRUE(ReadTag(kBase) & kFlagDirty);
	EXPECT_EQ(memRead32(kBase), 0xA5A5A5A5u);
}

TEST(EeCacheConsole, DISABLED_InvalidateKeepsThePhysicalTag)
{
	EeRecTestHarness h;
	resetCache();
	readCache32(kBase);
	RunCacheOp(0x1A, kBase); // DHIN
	const u32 tag = ReadTag(kBase);
	EXPECT_EQ(tag & (kFlagValid | kFlagDirty), 0u);
	EXPECT_EQ(tag & 0xFFFFF000u, kBase & 0x1FFFF000u);
}

TEST(EeCacheConsole, DISABLED_DxstgDropsUnimplementedTagBits)
{
	EeRecTestHarness h;
	resetCache();
	RunCacheOp(0x16, kBase);
	cpuRegs.CP0.n.TagLo = 0xFFFFFFFFu & ~kFlagDirty;
	RunCacheOp(0x12, kBase);
	EXPECT_EQ(ReadTag(kBase), 0xFFFFF038u);
}

TEST(EeCacheConsole, DISABLED_InstructionCacheIsModelled)
{
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x00, kBase); // IXLTG
	EXPECT_NE(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u);
}

// ---------------------------------------------------------------------------

TEST(EeCacheConsole, DISABLED_DumpReplay)
{
	EeRecTestHarness h;
	for (int cid : kReplayCases)
	{
		const EeCacheCase& c = CaseById(cid);
		const std::vector<u32> got = Replay(c);
		printf("\n== case %d %s (%d obs, replayed %d)\n", c.id, c.name, c.obs_count,
		       static_cast<int>(got.size()));
		for (int i = 0; i < c.obs_count && i < static_cast<int>(got.size()); i++)
		{
			const EeCacheObs& e = c.obs[i];
			if (e.is_tag)
			{
				printf("  r%-2d TAG console %08X [f=%02X line=%d]  pcsx2 %08X [f=%02X] %s\n",
				       i, e.raw, e.flags, e.line, got[i], got[i] & 0x7F,
				       e.scored ? "" : e.note);
			}
			else
			{
				printf("  r%-2d VAL console %08X %-9s  pcsx2 %08X %-9s  %s%s\n", i, e.raw,
				       SymName(e.sym), got[i], SymName(SymOf(got[i], c)),
				       SymOf(got[i], c) == e.sym ? "" : "<-- DIVERGENCE ",
				       e.scored ? "" : e.note);
			}
		}
		for (int s = 0; s < c.snap_count; s++)
		{
			const EeCacheSnapshot& sn = c.snaps[s];
			printf("  snap %d (r%d,r%d): console V=%d D=%d  pcsx2 V=%d D=%d\n", s,
			       sn.obs0, sn.obs1, sn.valid_self, sn.dirty_self,
			       ((got[sn.obs0] & kFlagValid) ? 1 : 0) +
			           ((got[sn.obs1] & kFlagValid) ? 1 : 0),
			       ((got[sn.obs0] & kFlagDirty) ? 1 : 0) +
			           ((got[sn.obs1] & kFlagDirty) ? 1 : 0));
		}
	}
}
