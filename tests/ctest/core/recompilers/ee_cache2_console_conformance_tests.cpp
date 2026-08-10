// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// EE cache conformance against a real PS2, round 2 -- the three questions
// round 1 left open. A probe was run on the console over ps2link and its
// capture reshaped into autocases_eecache2.h, with every structural claim
// re-derived from the raw words before emission.
//
// Round 1 closed the D-cache geometry, DXLTG's payload, the LRF replacement
// rule and the existence of a readable instruction cache. It could not close:
//
//   1. Why a hit op at the 0x20000000 uncached alias of a resident dirty line
//      did nothing. Every page round 1 touched was identity-mapped, so "a
//      different virtual address" and "an uncached virtual address" were the
//      same experiment. This probe installed its own TLB entry to give the
//      buffer a second CACHED virtual address and ran the identical sequence
//      through each. Cached alias: hits and writes back. Uncached alias:
//      nothing. The tag compare is physical; the attribute of the page is
//      what suppresses the op.
//
//   2. Whether DXSTG can set the dirty bit. Round 1 wrote every tag bit
//      *except* D on purpose -- a D=1 tag with an address of the prober's
//      choosing turns the next eviction into a 64-byte write wherever it
//      points -- so its recorded writable mask, PTagLo | 0x38, only ever said
//      that D reads back 0 when 0 is written. It does not. D is writable, and
//      it steers the write-back at the address in the tag, on an explicit
//      DXWBIN and on ordinary replacement alike. The implemented flag field is
//      D V R L, mask 0x78.
//
//   3. The instruction cache's index width. Round 1 could not tell 6 bits from
//      7 because bit 12 of both the probe address and the returned tag
//      happened to be 0. Rather than relocate code -- which would have meant
//      executing hand-assembled bytes on the console -- the probe asked the
//      cache: probe every set at addr and at addr+0x1000 and count how many
//      differ. All 64 pairs differ, so the index is vaddr[12:6]: 128 sets,
//      8 KB per way, 16 KB total. The same scan against the D-cache returns 0,
//      which is the known-answer control that says the method works.
//
// What PCSX2 gets right, and is pinned here so it stays right: the D-cache
// index is six bits, DXSTG accepts the dirty bit, the write-back target is
// `tag page | index << 6`, and a hit op through a second cached mapping of the
// same physical line hits.
//
// Round 2's finding was one defect, and it was worse than a wrong number.
// PCSX2's tag holds a *host* pointer where hardware holds a guest physical
// address, and DXSTG copied a guest-supplied 32-bit word straight into it, so
// writeBackIfNeeded dereferenced that word. It now translates the tag page and
// takes isValidPFN from the same translation (Cache.cpp, the DXSTG case); the
// write-back precondition that moves with it is at
// DxstgWriteBackTargetsTheTaggedGuestPage.
//
// EnableEECache, which PCSX2 ships off, bounded the old defect but only
// indirectly, and the distinction still matters when reading the tests.
// CHECK_CACHE gates only vtlb.cpp's guest load and store paths; the CACHE
// opcode itself and the cache entry points run whatever it is set to. The
// tests below therefore do not read the setting. They drive the model through
// its own entry points -- readCache32/writeCache32 and the interpreter's
// CACHE -- so everything here reproduces with EnableEECache at its default
// false.
//
// Everything else here is unfixed. Divergences are recorded from the real run.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Cache.h"
#include "Memory.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#if __has_include(<sys/mman.h>)
#include <sys/mman.h>
#endif

#include <cstdio>
#include <cstring>

#include "autocases_eecache2.h"

using namespace console_eecache2;
using recompiler_tests::EeRecTestHarness;

namespace
{
// Mirrors the probe's own layout: a line at page + 0x100, so the set index is
// 4 and `page | index << 6` is a claim with something in it rather than the
// degenerate `page | 0`.
constexpr u32 kBase = 0x00020000;
constexpr u32 kProbeLine = kBase + 0x100;
constexpr u32 kStride = 0x1000;

// KSEG0 and KUSEG both map to physical 0 in PCSX2 (Memory.cpp), so
// kKseg0 + x is a second CACHED virtual address for the same physical line --
// the harness's counterpart of the TLB alias the probe installed.
constexpr u32 kKseg0 = 0x80000000;

constexpr u32 kCacheOpcode = 0x2Fu;

void RunCacheOp(u32 op, u32 addr)
{
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

const EeCache2Case& CaseById(int id)
{
	for (int i = 0; i < kEeCache2CaseCount; i++)
	{
		if (kEeCache2Cases[i].id == id)
			return kEeCache2Cases[i];
	}
	ADD_FAILURE() << "no console case " << id;
	return kEeCache2Cases[0];
}

u32 Obs(int id, const char* name)
{
	const EeCache2Case& c = CaseById(id);
	for (int i = 0; i < c.n_obs; i++)
	{
		if (std::strcmp(c.obs[i].name, name) == 0)
			return c.obs[i].raw;
	}
	ADD_FAILURE() << "case " << id << " has no observation " << name;
	return 0;
}

// A page mapped at a host address we choose, so the wild write PCSX2 performs
// lands somewhere observable instead of killing the process. Several
// candidates because any one of them may already be taken by the loader.
// MAP_FIXED_NOREPLACE is Linux-only (4.17+). Demanding an exact address
// without it means either MAP_FIXED, which silently unmaps whatever already
// lives there, or a hint the kernel may ignore — neither is safe in a test
// process. Elsewhere the two callers skip.
//
// The candidates are 4K-aligned but none is 16K-aligned, so on a 16K-page
// kernel — Asahi, Apple Silicon, some Android — every one is rejected outright.
// Re-picking them 16K-aligned would need the tag/index constraints re-derived
// against the console capture, so it is left to whoever holds that data.
//
// A null return is therefore routine, not exceptional, and callers should treat
// the mapping as an optional negative control rather than a precondition. Only
// DxstgDirtyStaysInsideGuestMemory is wholly about the host page and has to
// skip; the write-back check keeps its guest-side half running everywhere.
void* MapAt(u32* chosen)
{
#if defined(MAP_FIXED_NOREPLACE)
	static const u32 kCandidates[] = {0x00129000u, 0x00229000u, 0x00329000u,
	                                  0x10429000u};
	for (u32 want : kCandidates)
	{
		void* p = mmap(reinterpret_cast<void*>(static_cast<uptr>(want)), 0x1000,
		               PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == reinterpret_cast<void*>(static_cast<uptr>(want)))
		{
			*chosen = want;
			return p;
		}
		if (p != MAP_FAILED)
			munmap(p, 0x1000);
	}
#else
	(void)chosen;
#endif
	return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------
// The capture itself. The generator checks these at regeneration time; this
// checks them at build time, so a hand-edited header cannot quietly change
// what every test below believes.

TEST(EeCache2Console, ConsoleCaptureIsSelfConsistent)
{
	// The control: 8 KB / 2 ways / 64 B is 64 sets, so bit 12 cannot reach the
	// D-cache index. Counters and raw samples, two routes to one fact.
	EXPECT_EQ(kDcacheScan.diff0, 0);
	EXPECT_EQ(kDcacheScan.diff1, 0);
	EXPECT_EQ(kDcacheScan.sample_tag[0], kDcacheScan.sample_tag[2]);
	EXPECT_EQ(kDcacheScan.sample_tag[1], kDcacheScan.sample_tag[3]);
	EXPECT_EQ(kDcacheScan.probes, kDcacheSets * 2);

	// The result: every probed pair differs, so bit 12 does select the set.
	EXPECT_EQ(kIcacheScan.diff0, 64);
	EXPECT_EQ(kIcacheScan.diff1, 64);
	EXPECT_NE(kIcacheScan.sample_tag[0], kIcacheScan.sample_tag[2]);
	EXPECT_EQ(kIcacheScan.probes, kIcacheSets * 2);

	// And the index wraps at 8 KB, which is what fixes the size at 16 KB
	// rather than merely "more than 8".
	EXPECT_EQ(kIcacheScan.sample_tag[6], kIcacheScan.sample_tag[0]);
	EXPECT_EQ(kIcacheScan.sample_tag[7], kIcacheScan.sample_tag[2]);
	EXPECT_EQ(kIcacheWaySizeBytes, kIcacheSets * 64);

	// A read-only cache has no dirty bit, and neither cache showed a lock bit.
	EXPECT_EQ(kIcacheScan.flags_or & kFlagDirty, 0u);
	EXPECT_EQ(kIcacheScan.flags_or & ~(kFlagValid | kFlagLrf), 0u);
	EXPECT_EQ(kDcacheScan.flags_or & ~(kFlagDirty | kFlagValid | kFlagLrf), 0u);
}

// The instruction cache's index is seven bits wide, and the tag carries bit 12
// as well -- so `(tag & 0xFFFFF000) | (set << 6)` reconstructs the line
// whether you take bit 12 from the tag or from the index.
TEST(EeCache2Console, IcacheLineAddressReconstructsFromTagAndSet)
{
	for (const EeCache2Recon* r : {&kReconUpperHalf, &kReconLowerHalf})
	{
		SCOPED_TRACE(r->name);
		ASSERT_EQ(r->found, 1u);
		EXPECT_EQ(r->recon_tag_bit12, r->recon_index_bit12);
		EXPECT_EQ(r->recon_tag_bit12, (r->tag & 0xFFFFF000u) | ((r->set & 0x7Fu) << 6));

		// The self-check that makes this a measurement rather than an
		// assertion: nothing told the probe this address, and the two words
		// the instruction cache holds are the two words RAM holds there.
		EXPECT_EQ(r->mem_a0, r->ixldt0);
		EXPECT_EQ(r->mem_a1, r->ixldt1);
		EXPECT_EQ(r->mem_b0, r->ixldt0);
		EXPECT_EQ(r->mem_b1, r->ixldt1);
	}
	// The upper-half entry is the one that separates the two reconstructions,
	// and its tag carries the bit.
	EXPECT_GE(kReconUpperHalf.set, 64u);
	EXPECT_EQ((kReconUpperHalf.tag >> 12) & 1u, 1u);
	EXPECT_EQ(kIcacheScan.agree, kIcacheScan.valid);
}

// ---------------------------------------------------------------------------
// Where PCSX2 agrees with the console. Pinned so it stays that way.

TEST(EeCache2Console, DcacheIndexIsSixBitsInPcsx2Too)
{
	EeRecTestHarness h;
	resetCache();
	writeCache32(kProbeLine, 0x5A5A0002u);

	// Same set, reached by an address a page higher: the console's scan found
	// no pair in 64 that differed, and PCSX2 must agree.
	const u32 here = ReadTag(kProbeLine);
	const u32 page_up = ReadTag(kProbeLine + kStride);
	EXPECT_EQ(here, page_up) << "PCSX2 put bit 12 in the D-cache index";

	// And it wraps at 0x1000, which is what makes it 64 sets and not more.
	EXPECT_EQ(here, ReadTag(kProbeLine + 0x2000));
	EXPECT_EQ(static_cast<int>((kProbeLine >> 6) & kDcacheIndexMask),
	          static_cast<int>(kSetIndex));
}

// The console sets D through DXSTG and reads it straight back, so the
// implemented flag field is D V R L and round 1's recorded mask -- PTagLo |
// 0x38, measured by writing every bit except D -- was one bit short.
TEST(EeCache2Console, DxstgSetsTheDirtyBitOnConsole)
{
	for (int id : {9, 10})
	{
		SCOPED_TRACE(id);
		EXPECT_EQ(Obs(id, "tag_readback") & kFlagDirty, kFlagDirty);
		EXPECT_EQ(Obs(id, "tag_readback"), CaseById(id).p);
	}
	// Case 8 wrote V alone and read back V alone -- the control that says the
	// bit above came from what was written and not from the line's history.
	EXPECT_EQ(Obs(8, "tag_readback") & kFlagDirty, 0u);
	EXPECT_EQ(kDxstgWritableFlags, kFlagDirty | kFlagValid | kFlagLrf | kFlagLock);
}

// PCSX2 stores the bit -- ALL_FLAGS is 0x7FF and covers it -- but it cannot be
// asked. DXLTG writes a dirty line back before reporting (Cache.cpp), and
// clearDirty() runs on the way out, so a tag read always answers D=0. That is
// round 1's second defect showing through a new door: the only instrument for
// reading the bit is the one that destroys it.
//
// So the bit is measured by its consequence instead. The write-back below
// happens if and only if D survived the DXSTG, and it is the same sequence
// DxstgWriteBackTargetsTheTaggedGuestPage uses -- with no DXLTG anywhere
// between the store and the eviction.
TEST(EeCache2Console, Pcsx2AcceptsDirtyFromDxstgButCannotReportIt)
{
	// The consequence is observed in guest memory, at the physical page the tag
	// names, which is where DXSTG now steers the write-back.
	constexpr u32 kTargetPage = 0x00129000;
	constexpr u32 kTarget = kTargetPage + kSetIndex * 64;

	// Asking destroys it.
	{
		EeRecTestHarness h;
		resetCache();
		RunCacheOp(0x16, kProbeLine); // DXIN way 0, so no write-back can fire
		cpuRegs.CP0.n.TagLo = kTargetPage | kFlagDirty | kFlagValid;
		RunCacheOp(0x12, kProbeLine); // DXSTG
		EXPECT_EQ(ReadTag(kProbeLine) & kFlagDirty, 0u)
			<< "PCSX2's DXLTG now reports D; the console's 0x" << std::hex
			<< Obs(9, "tag_readback") << " is reachable and this is fixed";
	}

	// Not asking shows it was there: the eviction only writes if D is set.
	{
		EeRecTestHarness h;
		resetCache();
		memWrite32(kTarget, 0xEEEEEEEEu);
		writeCache32(kProbeLine, 0x5A5A0009u);
		cpuRegs.CP0.n.TagLo = kTargetPage | kFlagDirty | kFlagValid;
		RunCacheOp(0x12, kProbeLine); // DXSTG, D set
		RunCacheOp(0x14, kProbeLine); // DXWBIN
		EXPECT_EQ(memRead32(kTarget), 0x5A5A0009u) << "D did not survive the DXSTG";
	}

	// The control: the same sequence with D clear moves nothing, so the write
	// above is attributable to the bit and not to DXWBIN writing regardless.
	{
		EeRecTestHarness h;
		resetCache();
		memWrite32(kTarget, 0xEEEEEEEEu);
		writeCache32(kProbeLine, 0x5A5A0009u);
		cpuRegs.CP0.n.TagLo = kTargetPage | kFlagValid; // no D
		RunCacheOp(0x12, kProbeLine);
		RunCacheOp(0x14, kProbeLine);
		EXPECT_EQ(memRead32(kTarget), 0xEEEEEEEEu) << "DXWBIN wrote back a clean line";
	}
}

// The write-back target is the tag's page with the *set index* supplying the
// line -- the tag chooses the page and nothing more. Both engines compute it
// the same way; they disagree only about what address space the result is in,
// which is the next test.
TEST(EeCache2Console, WriteBackTargetIsTagPageOrIndexOnBothEngines)
{
	for (int id : {8, 9, 10})
	{
		SCOPED_TRACE(id);
		const EeCache2Case& c = CaseById(id);
		const u32 target_line = c.x0;
		const u32 target_page = c.x1;
		EXPECT_EQ(target_page | (kSetIndex << 6), target_line);
		EXPECT_EQ(c.p & 0xFFFFF000u, target_page);
	}
	// The write landed at page|index<<6 and carried the line's own words: word
	// 0 from whichever source line held way 0, and zero after it because the
	// probe's buffer is .bss.
	EXPECT_NE(Obs(9, "target0_after_wb_way0"), Obs(9, "target_before"));
	EXPECT_EQ(Obs(9, "target1_after_wb_way0"), 0u);
	// And the control did not move at all.
	EXPECT_EQ(Obs(8, "target0_after_wb_way0"), Obs(8, "target_before"));
	EXPECT_EQ(Obs(8, "target0_after_wb_way1"), Obs(8, "target_before"));
}

// A hit op issued through a second CACHED virtual address for the same
// physical line hits, on both engines. On the console that address came from a
// TLB entry the probe installed; here it is KSEG0, which Memory.cpp maps to
// the same physical page as KUSEG.
TEST(EeCache2Console, HitOpThroughASecondCachedMappingHits)
{
	// Console case 5: the line was dirty, the op at the alias wrote it back
	// and left the line invalid with its address still readable.
	EXPECT_EQ(Obs(5, "ram_after_hit_op"), CaseById(5).m);
	EXPECT_EQ(Obs(5, "tag_way0_after") & (kFlagValid | kFlagDirty), 0u);
	EXPECT_EQ(Obs(5, "tag_way0_after") & 0xFFFFF000u,
	          Obs(5, "tag_way0") & 0xFFFFF000u);

	EeRecTestHarness h;
	resetCache();
	memWrite32(kProbeLine, 0xA5A50005u);
	writeCache32(kProbeLine, 0x5A5A0005u);
	ASSERT_EQ(memRead32(kProbeLine), 0xA5A50005u) << "not a write-back cache";

	RunCacheOp(0x18, kKseg0 + kProbeLine); // DHWBIN through the second mapping
	EXPECT_EQ(memRead32(kProbeLine), 0x5A5A0005u)
		<< "PCSX2 missed a hit op through a second cached mapping";
}

// Round 1 recorded that the same op at the 0x20000000 alias did nothing, and
// could not say why. Cases 5 and 6 are the identical instruction sequence
// differing in one base register, so the difference is the page's cache
// attribute and nothing else: the compare is physical, and an uncached page is
// excluded from it.
TEST(EeCache2Console, TheUncachedAliasMissIsAboutTheCacheAttribute)
{
	// Same starting state in both.
	EXPECT_EQ(Obs(5, "ram_preset"), CaseById(5).p);
	EXPECT_EQ(Obs(6, "ram_preset"), CaseById(6).p);
	EXPECT_EQ(Obs(5, "tag_way0") & 0x7Fu, Obs(6, "tag_way0") & 0x7Fu);

	// Cached alias: wrote back. Uncached alias: did not, and left the tag
	// exactly as it found it.
	EXPECT_EQ(Obs(5, "ram_after_hit_op"), CaseById(5).m);
	EXPECT_EQ(Obs(6, "ram_after_hit_op"), CaseById(6).p);
	EXPECT_EQ(Obs(6, "tag_way0_after"), Obs(6, "tag_way0"));

	// Ordinary loads and stores go through the same physical line from both
	// virtual addresses, and only one way ever holds it -- so the cache is
	// physically tagged and does not duplicate on a virtual alias.
	EXPECT_EQ(Obs(7, "alias_cached_load"), CaseById(7).m);
	EXPECT_EQ(Obs(7, "primary_load_after_alias_store"), CaseById(7).m + 1);
	EXPECT_EQ(Obs(7, "ram_after_alias_load"), CaseById(7).p)
		<< "the alias load forced a write-back";
	const int valid = ((Obs(7, "tag_way0_after") & kFlagValid) ? 1 : 0) +
	                  ((Obs(7, "tag_way1_after") & kFlagValid) ? 1 : 0);
	EXPECT_EQ(valid, 1) << "a virtual alias duplicated the line";
}

// PCSX2 has no instruction cache at all, so it has no geometry to get right.
TEST(EeCache2Console, Pcsx2HasNoInstructionCacheGeometry)
{
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x00, kProbeLine); // IXLTG
	EXPECT_EQ(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u)
		<< "PCSX2 now answers IXLTG; record what it answers";

	// Meanwhile the console's is 16 KB in 128 sets of two 64-byte ways.
	EXPECT_EQ(kIcacheSets, 128);
	EXPECT_EQ(kIcacheWaySizeBytes * 2, 16384);
	EXPECT_EQ(kIcacheIndexMask, 0x7Fu);
}

// ---------------------------------------------------------------------------
// Hardware's tag holds a guest physical address, so a write-back steered by
// DXSTG can only ever reach guest memory. PCSX2's now does too (Cache.cpp, the
// DXSTG case).
//
// The old write-back precondition was "the line is resident when the DXSTG
// lands": isValidPFN is bit 11 of the same word, DXSTG's mask (ALL_FLAGS =
// 0x7FF) could not reach it, and clear() dropped it. isValidPFN now moves with
// the address, so a DXSTG naming a resolvable page arms the line whether or not
// it was ever filled. All three histories are exercised below.
TEST(EeCache2Console, DxstgWriteBackTargetsTheTaggedGuestPage)
{
	// A physical page in main RAM, chosen to equal one of MapAt's candidates so
	// the host page of the same number can be held open as a negative control.
	constexpr u32 kTargetPage = 0x00129000;
	constexpr u32 kTarget = kTargetPage + kSetIndex * 64;

	// The host mapping is only the negative control: proof that the write-back
	// did not ALSO reach the host page that happens to carry the same number.
	// Everything else here is guest-side and needs nothing from the host, so it
	// runs unconditionally. On a 16K-page kernel -- Asahi, Apple Silicon, some
	// Android -- MapAt cannot honour a 4K-aligned request and returns null;
	// only the control is skipped then, not the whole test.
	u32 page = 0;
	void* p = MapAt(&page);
	const u32* host = nullptr;
	if (p)
	{
		ASSERT_EQ(page, kTargetPage) << "the control page moved; re-point kTargetPage";
		std::memset(p, 0xEE, 0x1000);
		host = reinterpret_cast<const u32*>(
			static_cast<uptr>(page) + kSetIndex * 64);
	}

	{
		EeRecTestHarness h;
		resetCache();
		memWrite32(kTarget, 0u);
		memWrite32(kTarget + 4, 0u);
		writeCache32(kProbeLine, 0x5A5A0009u);
		writeCache32(kProbeLine + 4, 0xDEADBEEFu);

		cpuRegs.CP0.n.TagLo = kTargetPage | kFlagDirty | kFlagValid;
		RunCacheOp(0x12, kProbeLine); // DXSTG
		RunCacheOp(0x14, kProbeLine); // DXWBIN

		// 64 bytes of guest cache line, at the guest physical page the tag names.
		EXPECT_EQ(memRead32(kTarget), 0x5A5A0009u);
		EXPECT_EQ(memRead32(kTarget + 4), 0xDEADBEEFu);
		if (host)
			EXPECT_EQ(host[0], 0xEEEEEEEEu) << "the write-back still reaches a host address";
	}

	// Never filled, and filled-then-invalidated. Both used to be declined
	// because bit 11 was clear; both now write back, carrying the cleared line.
	for (const bool invalidate_first : {false, true})
	{
		SCOPED_TRACE(invalidate_first ? "filled then invalidated" : "never filled");
		if (p)
			std::memset(p, 0xEE, 0x1000);
		EeRecTestHarness h;
		resetCache();
		memWrite32(kTarget, 0xA5A5A5A5u);
		if (invalidate_first)
		{
			writeCache32(kProbeLine, 0xABCD1234u);
			RunCacheOp(0x14, kProbeLine); // DXWBIN: writes back, then clears
		}
		else
		{
			RunCacheOp(0x16, kProbeLine); // DXIN
		}
		memWrite32(kTarget, 0xA5A5A5A5u);
		cpuRegs.CP0.n.TagLo = kTargetPage | kFlagDirty | kFlagValid;
		RunCacheOp(0x12, kProbeLine);
		RunCacheOp(0x14, kProbeLine);
		EXPECT_EQ(memRead32(kTarget), 0u) << "the cleared line did not write back";
		if (host)
			EXPECT_EQ(host[0], 0xEEEEEEEEu) << "the write-back still reaches a host address";
	}

	if (p)
		munmap(p, 0x1000);
}

// A DXSTG naming a page that does not resolve to plain guest memory leaves the
// line unbacked, so the write-back declines rather than dereferencing anything.
//
// 0x60129000 is past the end of the physical map, and it is the page with
// teeth: the lookup used to keep only the low 29 bits of the tag, so this page
// folded onto 0x00129000 -- ordinary main RAM -- and wrote sixty-four bytes
// there. The witness turns "we did not fault" into "we did not write somewhere
// the guest never named". An SCPH-30001 agrees on that much: an eviction
// steered above the end of RAM puts nothing into RAM.
//
// This check used to name 0x1FFFF000, calling it unmapped. That is the last
// page of the 4 MB BIOS ROM at 0x1FC00000, so it is real backing memory and the
// declining branch never ran at all.
TEST(EeCache2Console, DxstgOnAnUnresolvablePageDeclinesTheWriteBack)
{
	constexpr u32 kUnresolvablePage = 0x60129000u;
	constexpr u32 kWitness = 0x00129000u + kSetIndex * 64;

	EeRecTestHarness h;
	resetCache();
	memWrite32(kWitness, 0xA5A5A5A5u);
	writeCache32(kProbeLine, 0x5A5A0009u);
	cpuRegs.CP0.n.TagLo = kUnresolvablePage | kFlagDirty | kFlagValid;
	RunCacheOp(0x12, kProbeLine); // DXSTG
	RunCacheOp(0x14, kProbeLine); // DXWBIN -- must be a no-op, not a store

	EXPECT_EQ(memRead32(kWitness), 0xA5A5A5A5u)
		<< "the write-back reached guest memory the tag never named";
	EXPECT_EQ(ReadTag(kProbeLine) & (kFlagValid | kFlagDirty), 0u);
}

// Deliberately unpinned: where an eviction goes when the tag names one of the
// emulator's main-RAM mirrors at 0x20000000 or 0x30000000. Those are our
// physical map's mirrors, not the console's -- a console has no RAM at those
// physical addresses, and an eviction aimed there reached nothing on the
// SCPH-30001. Any assertion here would freeze an emulator-specific answer to a
// question no game asks, so leave it undefined.

// ---------------------------------------------------------------------------
// Tripwires. DxstgDirtyStaysInsideGuestMemory has graduated and holds; the rest
// fail today and turn green when the missing model appears.

TEST(EeCache2Console, DxstgDirtyStaysInsideGuestMemory)
{
	u32 page = 0;
	void* p = MapAt(&page);
	if (!p)
		GTEST_SKIP() << "could not map a page at any candidate host address";
	std::memset(p, 0xEE, 0x1000);

	EeRecTestHarness h;
	resetCache();
	writeCache32(kProbeLine, 0x5A5A0009u);
	cpuRegs.CP0.n.TagLo = page | kFlagDirty | kFlagValid;
	RunCacheOp(0x12, kProbeLine);
	RunCacheOp(0x14, kProbeLine);

	const u32* target = reinterpret_cast<const u32*>(
		static_cast<uptr>(page) + kSetIndex * 64);
	EXPECT_EQ(target[0], 0xEEEEEEEEu)
		<< "a guest cache op still reaches an arbitrary host address";
	munmap(p, 0x1000);
}

TEST(EeCache2Console, DISABLED_InstructionCacheGeometryIsModelled)
{
	EeRecTestHarness h;
	resetCache();
	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x00, kProbeLine);
	EXPECT_NE(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u);

	// 128 sets: two addresses a page apart must select different ones.
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x00, kProbeLine);
	const u32 a = cpuRegs.CP0.n.TagLo;
	cpuRegs.CP0.n.TagLo = 0;
	RunCacheOp(0x00, kProbeLine + kStride);
	EXPECT_NE(a, cpuRegs.CP0.n.TagLo);
}

TEST(EeCache2Console, DISABLED_AllEeCache2MatchesConsole)
{
	// Graduation: everything above that is currently recorded as a divergence
	// has to hold at once. The DXSTG half already does; the instruction cache
	// is what keeps this disabled.
	u32 page = 0;
	void* p = MapAt(&page);
	if (!p)
		GTEST_SKIP() << "could not map a page at any candidate host address";
	std::memset(p, 0xEE, 0x1000);

	EeRecTestHarness h;
	resetCache();
	writeCache32(kProbeLine, 0x5A5A0009u);
	cpuRegs.CP0.n.TagLo = page | kFlagDirty | kFlagValid;
	RunCacheOp(0x12, kProbeLine);
	RunCacheOp(0x14, kProbeLine);
	const u32* target = reinterpret_cast<const u32*>(
		static_cast<uptr>(page) + kSetIndex * 64);
	EXPECT_EQ(target[0], 0xEEEEEEEEu);

	cpuRegs.CP0.n.TagLo = 0xC0FFEE00u;
	RunCacheOp(0x00, kProbeLine);
	EXPECT_NE(cpuRegs.CP0.n.TagLo, 0xC0FFEE00u);
	munmap(p, 0x1000);
}

TEST(EeCache2Console, DISABLED_DumpConsoleRound2)
{
	printf("\nD-cache scan: diff %d/%d valid %d probes %d flagsOR %03x\n",
	       kDcacheScan.diff0, kDcacheScan.diff1, kDcacheScan.valid,
	       kDcacheScan.probes, kDcacheScan.flags_or);
	printf("I-cache scan: diff %d/%d valid %d agree %d probes %d flagsOR %03x\n",
	       kIcacheScan.diff0, kIcacheScan.diff1, kIcacheScan.valid,
	       kIcacheScan.agree, kIcacheScan.probes, kIcacheScan.flags_or);
	for (int k = 0; k < 8; k++)
	{
		printf("  sample @%04x  D %08x   I %08x\n", kDcacheScan.sample_addr[k],
		       kDcacheScan.sample_tag[k], kIcacheScan.sample_tag[k]);
	}
	for (const EeCache2Recon* r : {&kReconUpperHalf, &kReconLowerHalf})
	{
		printf("recon %s: set %u way %u tag %08x -> %08x / %08x\n", r->name,
		       r->set, r->way, r->tag, r->recon_tag_bit12, r->recon_index_bit12);
		printf("   ixldt %08x %08x   memA %08x %08x   memB %08x %08x\n",
		       r->ixldt0, r->ixldt1, r->mem_a0, r->mem_a1, r->mem_b0, r->mem_b1);
	}
	for (int i = 0; i < kEeCache2CaseCount; i++)
	{
		const EeCache2Case& c = kEeCache2Cases[i];
		printf("\n== case %d %s\n   p=%08x m=%08x x=%08x %08x %08x %08x\n", c.id,
		       c.name, c.p, c.m, c.x0, c.x1, c.x2, c.x3);
		for (int k = 0; k < c.n_obs; k++)
		{
			printf("   %-32s %08x%s\n", c.obs[k].name, c.obs[k].raw,
			       c.obs[k].is_tag ? "  (tag)" : "");
		}
	}
}
