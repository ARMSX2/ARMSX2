// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GENERATED from a capture taken on a real PS2 -- do not edit.
//
// One run of the cache probe on a real PS2.  Status 70030C11 (user mode, CU0 set),
// Config 00073443 (D-cache and I-cache both enabled), PRid 00002E43.
// The probe's buffer was at 00124000, the line under test at 00124100, its
// uncached alias at 20124100.
#pragma once

#include "common/Pcsx2Defs.h"

namespace console_eecache {

// Console-side geometry, so the replay can name the same lines.
constexpr u32 kConsoleBase = 0x00124100u;
constexpr u32 kConsoleLineStride = 0x1000u;
constexpr u32 kConsoleBaitLine = 0x001020C0u;

enum EeCacheStepKind : u8
{
	ECC_CACHE,
	ECC_CACHE_U,
	ECC_ST_C,
	ECC_ST_U,
	ECC_LD_C,
	ECC_LD_U,
	ECC_TAGLO_WR,
	ECC_TAGLO_RD,
	ECC_TAGHI_WR,
	ECC_TAGHI_RD,
};

enum EeCacheSym : u8
{
	ECC_S_ZERO,
	ECC_S_PRESET,
	ECC_S_MAGIC,
	ECC_S_MAGIC1,
	ECC_S_MAGIC2,
	ECC_S_MAGIC15,
	ECC_S_PAYLOAD,
	ECC_S_TAG_PADDR_V,
	ECC_S_TAG_ALLBUTD,
	ECC_S_OTHER,
};

struct EeCacheStep
{
	EeCacheStepKind kind;
	u8 op;        // cache op number, for ECC_CACHE*
	EeCacheSym sym;  // value to store / write, for the store and TagLo kinds
	s32 off;      // byte offset from the base line
	bool observed;
};

// What the console reported at each observed step, in order.
// `sym` is the memory word normalised to the case's own preset/magic
// pair (ECC_S_OTHER when it is neither); `raw` is the untouched word.
// For tag reads, `flags` is raw & 0x7F and `line` is which of the
// probe's five lines the address field names, or -1 for a line
// outside the buffer (leftovers from ps2link and the C runtime share
// these sets, and are not comparable against a freshly reset model).
struct EeCacheObs
{
	bool is_tag;
	bool scored;   // false when the console and a reset model are not
	               // asking the same question; `note` says why
	EeCacheSym sym;
	u32 raw;
	u32 flags;
	s8 line;
	const char* note;
};

// A way0/way1 DXLTG pair taken at one point in a case.  Tags cannot be
// compared way by way -- PCSX2's DXLTG returns no address, so there is
// no way to ask it which line a way holds -- but the COUNT of ways
// holding a valid (or dirty) line of the probe's own buffer is the same
// question on both sides.  The console's own sets also hold ps2link's
// lines; those are excluded from the counts, and a reset model has none.
struct EeCacheSnapshot
{
	int obs0;
	int obs1;
	int valid_self;
	int dirty_self;
};

struct EeCacheCase
{
	const char* name;
	int id;
	const EeCacheStep* steps;
	int step_count;
	const EeCacheObs* obs;
	int obs_count;
	const EeCacheSnapshot* snaps;
	int snap_count;
	u32 preset;
	u32 magic;
};

// case 1 -- write_back_and_alias
inline constexpr EeCacheStep kSteps1[] = {
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x18, ECC_S_ZERO, 0, false},  // DHWBIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs1[] = {
	{false, true, ECC_S_PRESET, 0xA5A50001u, 0u, -1, nullptr},  // LD_U
	{false, true, ECC_S_PRESET, 0xA5A50001u, 0u, -1, nullptr},  // LD_U
	{false, true, ECC_S_MAGIC, 0x5A5A0001u, 0u, -1, nullptr},  // LD_U
	{false, true, ECC_S_MAGIC, 0x5A5A0001u, 0u, -1, nullptr},  // LD_C
};

// case 2 -- dxltg_contents
inline constexpr EeCacheStep kSteps2[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGHI_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGHI_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGHI_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGHI_RD, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs2[] = {
	{true, true, ECC_S_OTHER, 0x0001B010u, 0x10u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way1
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLTG
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLTG
};
inline constexpr EeCacheSnapshot kSnaps2[] = {
	{0, 1, 0, 0},
	{2, 3, 1, 0},
};

// case 3 -- index_and_way
inline constexpr EeCacheStep kSteps3[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, false},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 4096, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 2, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 3, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 4096, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 4097, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 64, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 65, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs3[] = {
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00125020u, 0x20u, 1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00125020u, 0x20u, 1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00125020u, 0x20u, 1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00001070u, 0x70u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00123010u, 0x10u, -1, nullptr},  // DXLTG way1
};
inline constexpr EeCacheSnapshot kSnaps3[] = {
	{0, 1, 2, 0},
	{2, 3, 2, 0},
	{4, 5, 2, 0},
	{6, 7, 0, 0},
};

// case 4 -- replacement_ladder
inline constexpr EeCacheStep kSteps4[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 4096, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 8192, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 12288, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 16384, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs4[] = {
	{true, true, ECC_S_OTHER, 0x0001B010u, 0x10u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00125020u, 0x20u, 1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00126030u, 0x30u, 2, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00125020u, 0x20u, 1, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00126030u, 0x30u, 2, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00127030u, 0x30u, 3, nullptr},  // DXLTG way1
	{true, true, ECC_S_OTHER, 0x00128020u, 0x20u, 4, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00127030u, 0x30u, 3, nullptr},  // DXLTG way1
};
inline constexpr EeCacheSnapshot kSnaps4[] = {
	{0, 1, 0, 0},
	{2, 3, 1, 0},
	{4, 5, 2, 0},
	{6, 7, 2, 0},
	{8, 9, 2, 0},
	{10, 11, 2, 0},
};

// case 5 -- write_allocate_and_dxltg_writeback
inline constexpr EeCacheStep kSteps5[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x18, ECC_S_ZERO, 0, false},  // DHWBIN
};
inline constexpr EeCacheObs kObs5[] = {
	{false, true, ECC_S_PRESET, 0xA5A50005u, 0u, -1, nullptr},  // LD_U
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124070u, 0x70u, 0, nullptr},  // DXLTG way1
	{false, true, ECC_S_PRESET, 0xA5A50005u, 0u, -1, nullptr},  // LD_U
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124070u, 0x70u, 0, nullptr},  // DXLTG way1
	{false, true, ECC_S_MAGIC, 0x5A5A0005u, 0u, -1, nullptr},  // LD_C
};
inline constexpr EeCacheSnapshot kSnaps5[] = {
	{1, 2, 1, 1},
	{4, 5, 1, 1},
};

// case 6 -- dhwoin
inline constexpr EeCacheStep kSteps6[] = {
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x1C, ECC_S_ZERO, 0, false},  // DHWOIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
};
inline constexpr EeCacheObs kObs6[] = {
	{false, true, ECC_S_PRESET, 0xA5A50006u, 0u, -1, nullptr},  // LD_U
	{false, true, ECC_S_MAGIC, 0x5A5A0006u, 0u, -1, nullptr},  // LD_U
	{true, true, ECC_S_OTHER, 0x00124030u, 0x30u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001060u, 0x60u, -1, nullptr},  // DXLTG way1
	{false, true, ECC_S_MAGIC, 0x5A5A0006u, 0u, -1, nullptr},  // LD_C
};
inline constexpr EeCacheSnapshot kSnaps6[] = {
	{2, 3, 1, 0},
};

// case 7 -- dhin_drops_dirty
inline constexpr EeCacheStep kSteps7[] = {
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs7[] = {
	{false, true, ECC_S_PRESET, 0xA5A50007u, 0u, -1, nullptr},  // LD_U
	{false, true, ECC_S_PRESET, 0xA5A50007u, 0u, -1, nullptr},  // LD_U
	{true, true, ECC_S_OTHER, 0x00001060u, 0x60u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124000u, 0x00u, 0, nullptr},  // DXLTG way1
	{false, true, ECC_S_PRESET, 0xA5A50007u, 0u, -1, nullptr},  // LD_C
};
inline constexpr EeCacheSnapshot kSnaps7[] = {
	{2, 3, 0, 0},
};

// case 8 -- dxwbin
inline constexpr EeCacheStep kSteps8[] = {
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x14, ECC_S_ZERO, 0, false},  // DXWBIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x14, ECC_S_ZERO, 1, false},  // DXWBIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs8[] = {
	{false, true, ECC_S_PRESET, 0xA5A50008u, 0u, -1, nullptr},  // LD_U
	{false, false, ECC_S_PRESET, 0xA5A50008u, 0u, -1, "per-way DXWBIN: which way held the line"},  // LD_U
	{false, false, ECC_S_MAGIC, 0x5A5A0008u, 0u, -1, "per-way DXWBIN: which way held the line"},  // LD_U
	{true, true, ECC_S_OTHER, 0x00001000u, 0x00u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124000u, 0x00u, 0, nullptr},  // DXLTG way1
	{false, true, ECC_S_MAGIC, 0x5A5A0008u, 0u, -1, nullptr},  // LD_C
};
inline constexpr EeCacheSnapshot kSnaps8[] = {
	{3, 4, 0, 0},
};

// case 9 -- hit_op_through_uncached_alias
inline constexpr EeCacheStep kSteps9[] = {
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
	{ECC_ST_U, 0x00, ECC_S_PRESET, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE_U, 0x18, ECC_S_ZERO, 0, false},  // DHWBIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x18, ECC_S_ZERO, 0, false},  // DHWBIN
	{ECC_LD_U, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs9[] = {
	{false, true, ECC_S_PRESET, 0xA5A50009u, 0u, -1, nullptr},  // LD_U
	{false, false, ECC_S_PRESET, 0xA5A50009u, 0u, -1, "the uncached alias is unmapped in the test environment"},  // LD_U
	{true, true, ECC_S_OTHER, 0x00001060u, 0x60u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124060u, 0x60u, 0, nullptr},  // DXLTG way1
	{false, true, ECC_S_MAGIC, 0x5A5A0009u, 0u, -1, nullptr},  // LD_U
};
inline constexpr EeCacheSnapshot kSnaps9[] = {
	{2, 3, 1, 1},
};

// case 10 -- dxldt_word_select
inline constexpr EeCacheStep kSteps10[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 0, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC1, 4, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC2, 8, false},
	{ECC_ST_C, 0x00, ECC_S_MAGIC15, 60, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 0, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 1, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 4, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 5, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 60, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 61, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 2, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGHI_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 0, false},  // DXLDT
	{ECC_TAGHI_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x18, ECC_S_ZERO, 0, false},  // DHWBIN
};
inline constexpr EeCacheObs kObs10[] = {
	{false, true, ECC_S_MAGIC, 0x5A5A000Au, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_MAGIC1, 0x5A5A000Bu, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_MAGIC15, 0x5A5A0019u, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_MAGIC, 0x5A5A000Au, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // DXLDT
	{true, true, ECC_S_OTHER, 0x00124060u, 0x60u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way1
};
inline constexpr EeCacheSnapshot kSnaps10[] = {
	{8, 9, 1, 1},
};

// case 11 -- dxsdt
inline constexpr EeCacheStep kSteps11[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_ST_C, 0x00, ECC_S_MAGIC, 8, false},
	{ECC_TAGLO_WR, 0x00, ECC_S_PAYLOAD, 0, false},
	{ECC_CACHE, 0x13, ECC_S_ZERO, 8, false},  // DXSDT
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 8, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 8, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_PAYLOAD, 0, false},
	{ECC_CACHE, 0x13, ECC_S_ZERO, 9, false},  // DXSDT
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 9, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x11, ECC_S_ZERO, 8, false},  // DXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_CACHE, 0x1A, ECC_S_ZERO, 0, false},  // DHIN
};
inline constexpr EeCacheObs kObs11[] = {
	{false, true, ECC_S_PAYLOAD, 0xDEADBE0Bu, 0u, -1, nullptr},  // DXLDT
	{false, false, ECC_S_MAGIC, 0x5A5A000Bu, 0u, -1, "cached load between the two per-way DXSDTs"},  // LD_C
	{false, true, ECC_S_PAYLOAD, 0xDEADBE0Bu, 0u, -1, nullptr},  // DXLDT
	{false, true, ECC_S_PAYLOAD, 0xDEADBE0Bu, 0u, -1, nullptr},  // DXLDT
	{true, true, ECC_S_OTHER, 0x00001010u, 0x10u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00124070u, 0x70u, 0, nullptr},  // DXLTG way1
};
inline constexpr EeCacheSnapshot kSnaps11[] = {
	{4, 5, 1, 1},
};

// case 12 -- dxstg_writable_mask
inline constexpr EeCacheStep kSteps12[] = {
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_CACHE, 0x16, ECC_S_ZERO, 1, false},  // DXIN
	{ECC_TAGLO_WR, 0x00, ECC_S_TAG_PADDR_V, 0, false},
	{ECC_CACHE, 0x12, ECC_S_ZERO, 0, false},  // DXSTG
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_TAG_ALLBUTD, 0, false},
	{ECC_CACHE, 0x12, ECC_S_ZERO, 0, false},  // DXSTG
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x12, ECC_S_ZERO, 0, false},  // DXSTG
	{ECC_CACHE, 0x16, ECC_S_ZERO, 0, false},  // DXIN
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 0, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x10, ECC_S_ZERO, 1, false},  // DXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
};
inline constexpr EeCacheObs kObs12[] = {
	{true, true, ECC_S_OTHER, 0x00124020u, 0x20u, 0, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0xFFFFF038u, 0x38u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00000000u, 0x00u, -1, nullptr},  // DXLTG way0
	{true, true, ECC_S_OTHER, 0x00001000u, 0x00u, -1, nullptr},  // DXLTG way1
};

// case 13 -- instruction_cache
inline constexpr EeCacheStep kSteps13[] = {
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x00, ECC_S_ZERO, 0, false},  // IXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x00, ECC_S_ZERO, 1, false},  // IXLTG
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x01, ECC_S_ZERO, 0, false},  // IXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x01, ECC_S_ZERO, 1, false},  // IXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x01, ECC_S_ZERO, 4, false},  // IXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGLO_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x01, ECC_S_ZERO, 5, false},  // IXLDT
	{ECC_TAGLO_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_TAGHI_WR, 0x00, ECC_S_ZERO, 0, false},
	{ECC_CACHE, 0x00, ECC_S_ZERO, 0, false},  // IXLTG
	{ECC_TAGHI_RD, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 0, true},
	{ECC_LD_C, 0x00, ECC_S_ZERO, 4, true},
};
inline constexpr EeCacheObs kObs13[] = {
	{true, true, ECC_S_OTHER, 0x0011C020u, 0x20u, -1, nullptr},  // IXLTG way0
	{true, true, ECC_S_OTHER, 0x00102020u, 0x20u, -1, nullptr},  // IXLTG way1
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // IXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // IXLDT
	{false, true, ECC_S_OTHER, 0xDE0209F0u, 0u, -1, nullptr},  // IXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // IXLDT
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // IXLTG
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // LD_C
	{false, true, ECC_S_ZERO, 0x00000000u, 0u, -1, nullptr},  // LD_C
};

inline constexpr EeCacheCase kCases[] = {
	{"write_back_and_alias", 1, kSteps1, 8, kObs1, 4, nullptr, 0, 0xA5A50001u, 0x5A5A0001u},
	{"dxltg_contents", 2, kSteps2, 21, kObs2, 6, kSnaps2, 2, 0xA5A50002u, 0x5A5A0002u},
	{"index_and_way", 3, kSteps3, 28, kObs3, 8, kSnaps3, 4, 0xA5A50003u, 0x5A5A0003u},
	{"replacement_ladder", 4, kSteps4, 43, kObs4, 12, kSnaps4, 6, 0xA5A50004u, 0x5A5A0004u},
	{"write_allocate_and_dxltg_writeback", 5, kSteps5, 20, kObs5, 7, kSnaps5, 2, 0xA5A50005u, 0x5A5A0005u},
	{"dhwoin", 6, kSteps6, 14, kObs6, 5, kSnaps6, 1, 0xA5A50006u, 0x5A5A0006u},
	{"dhin_drops_dirty", 7, kSteps7, 13, kObs7, 5, kSnaps7, 1, 0xA5A50007u, 0x5A5A0007u},
	{"dxwbin", 8, kSteps8, 17, kObs8, 6, kSnaps8, 1, 0xA5A50008u, 0x5A5A0008u},
	{"hit_op_through_uncached_alias", 9, kSteps9, 14, kObs9, 5, kSnaps9, 1, 0xA5A50009u, 0x5A5A0009u},
	{"dxldt_word_select", 10, kSteps10, 37, kObs10, 10, kSnaps10, 1, 0xA5A5000Au, 0x5A5A000Au},
	{"dxsdt", 11, kSteps11, 24, kObs11, 6, kSnaps11, 1, 0xA5A5000Bu, 0x5A5A000Bu},
	{"dxstg_writable_mask", 12, kSteps12, 21, kObs12, 4, nullptr, 0, 0xA5A5000Cu, 0x5A5A000Cu},
	{"instruction_cache", 13, kSteps13, 23, kObs13, 9, nullptr, 0, 0xA5A5000Du, 0x5A5A000Du},
};
inline constexpr int kCaseCount = 13;

// Cases 1..12 exercise the D-cache, which PCSX2 models; case 13 is the
// instruction cache, which it does not model at all.
inline constexpr int kReplayCases[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
inline constexpr int kReplayCaseCount = 12;

} // namespace console_eecache
