// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GENERATED from a capture taken on a real PS2. Do not edit.
//
// Round 2 of the EE cache capture, run on a real PS2 over ps2link.
// The generator re-derives every structural claim below from the raw
// words and refuses to emit if one fails.

#pragma once

#include "common/Pcsx2Types.h"

namespace console_eecache2
{
// The probe's own geometry, from the capture's env record.
constexpr u32 kGbuf = 0x00126000u;
constexpr u32 kLine = 0x00126100u;
constexpr u32 kSetIndex = 4u;
constexpr u32 kAliasVa = 0x40000000u;

// Measured cache geometry. The D-cache numbers are round 1's, re-run
// here as the control that says the scan works; the I-cache numbers are
// the new result.
constexpr int kDcacheSets = 64;
constexpr int kIcacheSets = 128;
constexpr u32 kDcacheIndexMask = 0x3Fu;  // vaddr[11:6]
constexpr u32 kIcacheIndexMask = 0x7Fu;  // vaddr[12:6]
constexpr int kIcacheWaySizeBytes = 8192;

// Tag flags. Round 1 established D V R L at 6..3 and nothing else; the
// D bit is writable through DXSTG, which round 1 could not say because
// it deliberately never wrote one.
constexpr u32 kFlagDirty = 0x40u;
constexpr u32 kFlagValid = 0x20u;
constexpr u32 kFlagLrf = 0x10u;
constexpr u32 kFlagLock = 0x08u;
constexpr u32 kDxstgWritableFlags = 0x78u;
constexpr u32 kIcacheObservedFlags = 0x30u;  // V|R, never D

struct EeCache2Scan
{
	const char* name;
	int diff0;   // sets where probing +0x1000 returned a different tag
	int diff1;   // same, way 1
	int valid;
	int agree;   // valid entries whose tag bit 12 matches the set's bit 6
	int zero;
	int probes;
	u32 flags_or;
	u32 flags_and;
	u32 sample_addr[8];
	u32 sample_tag[8];
};

constexpr EeCache2Scan kDcacheScan = {
	"D-cache: is bit 12 in the set index? (known answer: no)",
	0, 0, 99, 47, 0, 128, 0x070u, 0x000u,
	{0x0000u, 0x0001u, 0x1000u, 0x1001u, 0x0040u, 0x1040u, 0x2000u, 0x3000u},
	{0x00124010u, 0x0012D060u, 0x00124010u, 0x0012D060u, 0x00124000u, 0x00124000u, 0x00124010u, 0x00124010u},
};

constexpr EeCache2Scan kIcacheScan = {
	"I-cache: is bit 12 in the set index?",
	64, 64, 254, 254, 0, 256, 0x030u, 0x000u,
	{0x0000u, 0x0001u, 0x1000u, 0x1001u, 0x0040u, 0x1040u, 0x2000u, 0x3000u},
	{0x00102020u, 0x00108030u, 0x00109020u, 0x00105020u, 0x00108020u, 0x00103030u, 0x00102020u, 0x00109020u},
};

// A resident instruction line, its tag, and the two words the cache and
// RAM each hold at the address reconstructed from tag and set.
struct EeCache2Recon
{
	const char* name;
	u32 found, set, way, tag, ixldt0, ixldt1;
	u32 recon_tag_bit12, recon_index_bit12;
	u32 mem_a0, mem_a1, mem_b0, mem_b1;
};

constexpr EeCache2Recon kReconUpperHalf = {
	"sets 64..127",
	1u, 64u, 0u, 0x00109020u, 0x24650001u, 0x00031980u,
	0x00109000u, 0x00109000u,
	0x24650001u, 0x00031980u, 0x24650001u, 0x00031980u,
};

constexpr EeCache2Recon kReconLowerHalf = {
	"sets 0..63",
	1u, 0u, 0u, 0x00102020u, 0x006F1826u, 0x00463024u,
	0x00102000u, 0x00102000u,
	0x006F1826u, 0x00463024u, 0x006F1826u, 0x00463024u,
};

struct EeCache2Obs
{
	const char* name;
	bool is_tag;
	u32 raw;
};

struct EeCache2Case
{
	int id;
	const char* name;
	u32 p;      // the case's first parameter word (preset, or a tag)
	u32 m;      // its second (magic)
	u32 x0, x1, x2, x3;  // per-case extras: alias base, or target/page
	int n_obs;
	const EeCache2Obs* obs;
};

constexpr EeCache2Obs kObs1[] = {
	{"ram_after_preset", false, 0xA5A50001u},
	{"ram_after_cached_store", false, 0xA5A50001u},
	{"ram_after_dhwbin", false, 0x5A5A0001u},
	{"cached_read", false, 0x5A5A0001u},
};
constexpr EeCache2Obs kObs5[] = {
	{"ram_preset", false, 0xA5A50005u},
	{"tag_way0", true, 0x00126070u},
	{"tag_way1", true, 0x00001000u},
	{"ram_after_hit_op", false, 0x5A5A0005u},
	{"tag_way0_after", true, 0x00126010u},
	{"tag_way1_after", true, 0x00001000u},
	{"cached_read", false, 0x5A5A0005u},
	{"ram_final", false, 0x5A5A0005u},
};
constexpr EeCache2Obs kObs6[] = {
	{"ram_preset", false, 0xA5A50006u},
	{"tag_way0", true, 0x00126070u},
	{"tag_way1", true, 0x00001000u},
	{"ram_after_hit_op", false, 0xA5A50006u},
	{"tag_way0_after", true, 0x00126070u},
	{"tag_way1_after", true, 0x00001000u},
	{"cached_read", false, 0x5A5A0006u},
	{"ram_final", false, 0x5A5A0006u},
};
constexpr EeCache2Obs kObs7[] = {
	{"ram_preset", false, 0xA5A50007u},
	{"alias_cached_load", false, 0x5A5A0007u},
	{"tag_way0", true, 0x00001000u},
	{"tag_way1", true, 0x00126060u},
	{"ram_after_alias_load", false, 0xA5A50007u},
	{"primary_load_after_alias_store", false, 0x5A5A0008u},
	{"tag_way0_after", true, 0x00001000u},
	{"tag_way1_after", true, 0x00126060u},
	{"ram_final", false, 0x5A5A0008u},
	{"alias_final", false, 0x5A5A0008u},
};
constexpr EeCache2Obs kObs8[] = {
	{"target_before", false, 0x7A46E700u},
	{"occupant_way0", true, 0x00127070u},
	{"occupant_way1", true, 0x00126060u},
	{"tag_readback", true, 0x00128020u},
	{"target0_after_wb_way0", false, 0x7A46E700u},
	{"target1_after_wb_way0", false, 0x7A46E701u},
	{"tag_way0_after", true, 0x00128000u},
	{"target0_after_wb_way1", false, 0x7A46E700u},
	{"cached_read_src", false, 0x5A5A0008u},
};
constexpr EeCache2Obs kObs9[] = {
	{"target_before", false, 0x7A46E700u},
	{"occupant_way0", true, 0x00127070u},
	{"occupant_way1", true, 0x00126060u},
	{"tag_readback", true, 0x00129060u},
	{"target0_after_wb_way0", false, 0x5A5A001Au},
	{"target1_after_wb_way0", false, 0x00000000u},
	{"tag_way0_after", true, 0x00129000u},
	{"target0_after_wb_way1", false, 0x5A5A001Au},
	{"cached_read_src", false, 0x5A5A0009u},
};
constexpr EeCache2Obs kObs10[] = {
	{"target_before", false, 0x7A46E700u},
	{"occupant_way0", true, 0x00127070u},
	{"occupant_way1", true, 0x00126060u},
	{"tag_readback", true, 0x0012A060u},
	{"target0_after_evict", false, 0x5A5A001Bu},
	{"target1_after_evict", false, 0x00000000u},
	{"tag_way0_after", true, 0x0012B030u},
	{"tag_way1_after", true, 0x0012C030u},
};

constexpr EeCache2Case kEeCache2Cases[] = {
	{1, "control: write-back cache, uncached alias, DHWBIN reaches RAM",
	 0xA5A50001u, 0x5A5A0001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	 4, kObs1},
	{5, "hit op through a second CACHED virtual address",
	 0xA5A50005u, 0x5A5A0005u, 0x40000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	 8, kObs5},
	{6, "hit op through the UNCACHED alias (round 1's case 9, re-run)",
	 0xA5A50006u, 0x5A5A0006u, 0x40000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	 8, kObs6},
	{7, "ordinary loads and stores through both virtual addresses",
	 0xA5A50007u, 0x5A5A0007u, 0x40000000u, 0x00000000u, 0x00000000u, 0x00000000u,
	 10, kObs7},
	{8, "DXSTG with D clear, then DXWBIN (control)",
	 0x00128020u, 0x5A5A0008u, 0x00128100u, 0x00128000u, 0x7A46E700u, 0x7A46E701u,
	 9, kObs8},
	{9, "DXSTG with D set, then DXWBIN",
	 0x00129060u, 0x5A5A0009u, 0x00129100u, 0x00129000u, 0x5A5A001Au, 0x00000000u,
	 9, kObs9},
	{10, "DXSTG with D set, then ordinary replacement",
	 0x0012A060u, 0x5A5A000Au, 0x0012A100u, 0x0012A000u, 0x5A5A001Bu, 0x00000000u,
	 8, kObs10},
};
constexpr int kEeCache2CaseCount =
	static_cast<int>(sizeof(kEeCache2Cases) / sizeof(kEeCache2Cases[0]));

// The live TLB entry that maps the probe's buffer, measured with tlbp
// rather than taken from ps2sdk's static table -- which this capture
// shows differs from the live one at index 0 (EntryLo 0x1F, not 0x07).
constexpr u32 kGbufTlbIndex = 14u;
constexpr u32 kGbufPageMask = 0x0007E000u;  // 256 KB pages
constexpr u32 kGbufEntryHi = 0x00100000u;
constexpr u32 kGbufEntryLo0 = 0x0000401Fu;  // PFN 00100, C=3, identity
constexpr u32 kTlbIndex0EntryLo0 = 0x8000001Fu;
constexpr u32 kWired = 31u;
} // namespace console_eecache2
