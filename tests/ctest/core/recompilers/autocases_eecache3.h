// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GENERATED from a capture taken on a real PS2. Do not edit.
//
// Round 3 of the EE cache capture, run on a real PS2 over ps2link.

#pragma once

#include "common/Pcsx2Types.h"

namespace console_eecache3
{
constexpr u32 kGbuf = 0x00131000u;
constexpr u32 kLine = 0x00131100u;
constexpr u32 kDcacheSet = 4u;

constexpr u32 kFlagDirty = 0x40u;
constexpr u32 kFlagValid = 0x20u;
constexpr u32 kFlagLrf = 0x10u;

// Five ordinary compiled functions 8 KB apart, so they share one
// instruction-cache set and carry five distinct tags.
constexpr int kBaitCount = 5;
constexpr u32 kBaits[5] = {0x00104000u, 0x00106000u, 0x00108000u, 0x0010A000u, 0x0010C000u};
constexpr u32 kIcacheSet = 0u;
constexpr int kSampledLines = 4;

// One row per (depth, sampled line). `init_lrf0`/`init_lrf1` is the
// UNIQUE initial LRF state that reproduces the observation under
// `way = LRF0 ^ LRF1, toggle the filled way` -- twenty rows, twenty
// unique fits. The generator refuses to emit if any row admits zero or
// more than one.
struct EeCache3Fill
{
	int k;
	int line;
	u32 tag_way0;
	u32 tag_way1;
	int init_lrf0;
	int init_lrf1;
};

constexpr EeCache3Fill kIcacheLadder[] = {
	{1, 0, 0x00104030u, 0x0010E000u, 0, 0},
	{1, 1, 0x00104020u, 0x0010E010u, 1, 1},
	{1, 2, 0x00104030u, 0x0010E000u, 0, 0},
	{1, 3, 0x00104020u, 0x0010E010u, 1, 1},
	{2, 0, 0x00104020u, 0x00106020u, 1, 1},
	{2, 1, 0x00104030u, 0x00106030u, 0, 0},
	{2, 2, 0x00104020u, 0x00106020u, 1, 1},
	{2, 3, 0x00104030u, 0x00106030u, 0, 0},
	{3, 0, 0x00108030u, 0x00106020u, 1, 1},
	{3, 1, 0x00108020u, 0x00106030u, 0, 0},
	{3, 2, 0x00108030u, 0x00106020u, 1, 1},
	{3, 3, 0x00108020u, 0x00106030u, 0, 0},
	{4, 0, 0x00108030u, 0x0010A030u, 1, 1},
	{4, 1, 0x00108020u, 0x0010A020u, 0, 0},
	{4, 2, 0x00108030u, 0x0010A030u, 1, 1},
	{4, 3, 0x00108020u, 0x0010A020u, 0, 0},
	{5, 0, 0x0010C030u, 0x0010A020u, 0, 0},
	{5, 1, 0x0010C020u, 0x0010A030u, 1, 1},
	{5, 2, 0x0010C030u, 0x0010A020u, 0, 0},
	{5, 3, 0x0010C020u, 0x0010A030u, 1, 1},
};
constexpr int kIcacheLadderCount =
	static_cast<int>(sizeof(kIcacheLadder) / sizeof(kIcacheLadder[0]));

struct EeCache3Obs
{
	const char* name;
	bool is_tag;
	u32 raw;
};

struct EeCache3Case
{
	int id;
	const char* name;
	u32 p;
	u32 m;
	int n_obs;
	const EeCache3Obs* obs;
};

constexpr EeCache3Obs kObs1[] = {
	{"ram_after_preset", false, 0xA5A50001u},
	{"ram_after_cached_store", false, 0xA5A50001u},
	{"ram_after_dhwbin", false, 0x5A5A0001u},
	{"cached_read", false, 0x5A5A0001u},
};
constexpr EeCache3Obs kObs20[] = {
	{"cached_load_of_clean_line", false, 0xA5A50014u},
	{"tag_way0", true, 0x00131030u},
	{"tag_way1", true, 0x00001060u},
	{"ram_after_uncached_store", false, 0x5A5A0014u},
	{"cached_read_after_uncached_store", false, 0xA5A50014u},
	{"tag_way0_after", true, 0x00131030u},
	{"tag_way1_after", true, 0x00001060u},
	{"cached_read_after_invalidate", false, 0x5A5A0014u},
};
constexpr EeCache3Obs kObs21[] = {
	{"ram_after_cached_store", false, 0xA5A50015u},
	{"tag_way0", true, 0x00131070u},
	{"tag_way1", true, 0x00001060u},
	{"ram_after_uncached_store", false, 0x5A5A0035u},
	{"cached_read_after_uncached_store", false, 0x5A5A0015u},
	{"ram_after_writeback", false, 0x5A5A0015u},
	{"tag_way0_after", true, 0x00131010u},
	{"cached_read_final", false, 0x5A5A0015u},
};
constexpr EeCache3Obs kObs22[] = {
	{"ram_word0_after_cached_store", false, 0xA5A50016u},
	{"ram_word1_after_cached_store", false, 0xA5A50017u},
	{"ram_word1_after_uncached_store", false, 0x5A5A0036u},
	{"ram_word0_after_writeback", false, 0x5A5A0016u},
	{"ram_word1_after_writeback", false, 0xA5A50017u},
	{"cached_read_word1", false, 0xA5A50017u},
};
constexpr EeCache3Obs kObs30[] = {
	{"way0_before", true, 0x0010E020u},
	{"way1_before", true, 0x0010C020u},
	{"way0_after_ixin_way0", true, 0x0010E000u},
	{"way1_after_ixin_way0", true, 0x0010C020u},
	{"way0_after_ixin_way1", true, 0x0010E000u},
	{"way1_after_ixin_way1", true, 0x0010C000u},
};

constexpr EeCache3Case kEeCache3Cases[] = {
	{1, "control: write-back cache, uncached alias, DHWBIN reaches RAM",
	 0xA5A50001u, 0x5A5A0001u, 4, kObs1},
	{20, "uncached store to a resident CLEAN line",
	 0xA5A50014u, 0x5A5A0014u, 8, kObs20},
	{21, "uncached store to a resident DIRTY line, then write-back",
	 0xA5A50015u, 0x5A5A0015u, 8, kObs21},
	{22, "uncached store to another word of a dirty line, then write-back",
	 0xA5A50016u, 0x5A5A0016u, 6, kObs22},
	{30, "cache 0x07: is it IXIN?",
	 0x0010C000u, 0x00000000u, 6, kObs30},
};
constexpr int kEeCache3CaseCount =
	static_cast<int>(sizeof(kEeCache3Cases) / sizeof(kEeCache3Cases[0]));
} // namespace console_eecache3
