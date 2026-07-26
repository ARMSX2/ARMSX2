// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// GENERATED from a first-party capture taken on a real PS2.  Do not edit.
//
// VU sticky-flag ground truth.  Earlier VU captures cleared STATUS between
// ops, so none of them constrained how the six sticky bits accumulate; these
// cases never clear.  Group A is VU0 macro mode (EE COP2), group B the same
// questions asked of VU0 micro mode.
#pragma once

#include "common/Pcsx2Types.h"

namespace console_vusticky
{
// Op kinds, mirroring the case bodies the probe ran.
enum VuStickyOpKind
{
	VS_NOP = 0,
	VS_MUL,        // vmul.x  vf6, vf4, vf5
	VS_ADD,        // vadd.x  vf6, vf4, vf5
	VS_MUL_MASK0,  // vmul    vf6, vf4, vf5 with an EMPTY destination mask
	VS_DIV,        // vdiv    Q, vf4x, vf5x
	VS_SQRT,       // vsqrt   Q, vf5x
	VS_RSQRT,      // vrsqrt  Q, vf4x, vf5x
	VS_CLIP,       // vclipw.xyz vf4, vf5
	VS_IADD,       // viadd   vi1, vi2, vi3
	VS_CTC2_ZERO,  // ctc2    $0, $vi16
	VS_CTC2_FFF,   // ctc2    (0xFFF), $vi16
};

// `mask` is the COP2 destination mask the probe issued (x=8, y=4, z=2, w=1).
// Most cases are scalar `vmul.x`; the four-lane case is 0xF and the empty-mask
// case is 0.  The four per-lane operand words are the quadword the console
// loaded with LQC2, so a case that needs a different event per lane can say so.
struct VuStickyOp
{
	VuStickyOpKind kind;
	u32 mask; // COP2 destination mask, x=8 .. w=1; 0 for the empty-mask case
	u32 fs[4];
	u32 ft[4];
};

// STATUS, MAC and Q as the console read them back through CFC2.
struct VuStickyRead
{
	u32 status;
	u32 mac;
	u32 q;
};

struct VuStickyCase
{
	const char* tag;
	const char* rule;
	VuStickyOp op[3];
	// read[0] is the post-prologue control; read[1..3] follow each op.
	VuStickyRead read[4];
	u32 clip;
	u32 vf6[4];
};

inline constexpr VuStickyCase kVuStickyCases[] = {
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE",
	 "An FMAC ORs its ZSUO events into the sticky field: after an underflow (Z+U) an overflow (O) leaves sticky ZUO, not sticky O alone",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(1), 0x8u, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000348u, 0x00008000u, 0x3F800000u}, {0x00000348u, 0x00008000u, 0x3F800000u}},
	 0x00000000u, {0x7FFFFFFFu, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_SURVIVES_SILENT_FMAC",
	 "A sticky bit survives an FMAC that raises nothing: the clean add clears the cause nibble and leaves sticky ZU standing",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(2), 0x8u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000140u, 0x00000000u, 0x3F800000u}, {0x00000140u, 0x00000000u, 0x3F800000u}},
	 0x00000000u, {0x40000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_ONE_OP_ALL_FOUR",
	 "A single four-lane FMAC sets all four ZSUO stickies at once, one per lane: x=+0 (Z), y=-1 (S), z underflows (Z+U), w overflows (O)",
	 {{static_cast<VuStickyOpKind>(1), 0xFu, {0x00000000u, 0xBF800000u, 0x00800000u, 0x7F000000u}, {0x00000000u, 0x3F800000u, 0x3F000000u, 0x7F000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x000003CFu, 0x0000124Au, 0x3F800000u}, {0x000003CFu, 0x0000124Au, 0x3F800000u}, {0x000003CFu, 0x0000124Au, 0x3F800000u}},
	 0x00000000u, {0x00000000u, 0xBF800000u, 0x00000000u, 0x7FFFFFFFu}},
	{"VUSTICKY_DIV_DI_ACCUMULATE",
	 "The div unit ORs into sticky D/I as well: x/0 then 0/0 leaves BOTH sticky D and sticky I, though the cause nibble holds only the newer I",
	 {{static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(4), 0x0u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000820u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C10u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C10u, 0x00000000u, 0x7FFFFFFFu}},
	 0x00000000u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS",
	 "A div-unit op disturbs neither the FMAC cause bits nor the FMAC stickies: the underflow's ZU cause and ZU sticky both survive the divide",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000965u, 0x00000808u, 0x7FFFFFFFu}, {0x00000965u, 0x00000808u, 0x7FFFFFFFu}},
	 0x00000000u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_FMAC_KEEPS_DI",
	 "KNOWN-ANSWER CONTROL: an FMAC leaves the div unit's D cause and sticky D alone, reproducing the earlier D-persists-across-FMAC result from a new angle",
	 {{static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(2), 0x8u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000820u, 0x00000000u, 0x7FFFFFFFu}, {0x00000820u, 0x00000000u, 0x7FFFFFFFu}, {0x00000820u, 0x00000000u, 0x7FFFFFFFu}},
	 0x00000000u, {0x40000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_DI_ACCUMULATE_SQRT_DIV",
	 "Second witness for the D/I accumulation, with a different pair of ops: sqrt of a negative raises I, then x/0 raises D, and both stickies stand",
	 {{static_cast<VuStickyOpKind>(5), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u}}, {static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000410u, 0x00000000u, 0x3F800000u}, {0x00000C20u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C20u, 0x00000000u, 0x7FFFFFFFu}},
	 0x00000000u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE",
	 "CTC2 of zero to STATUS clears the sticky field and leaves the cause nibble standing: the cause is not a bit of the register, it tracks MAC",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(9), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000005u, 0x00000808u, 0x3F800000u}, {0x00000005u, 0x00000808u, 0x3F800000u}},
	 0x00000000u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_CTC2_WRITTEN_AND_OP_SET_ALIKE",
	 "A CTC2-written sticky is indistinguishable from an op-set one: all six survive a clean add, and a later underflow ORs into them",
	 {{static_cast<VuStickyOpKind>(10), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(2), 0x8u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000FC0u, 0x00000000u, 0x3F800000u}, {0x00000FC0u, 0x00000000u, 0x3F800000u}, {0x00000FC5u, 0x00000808u, 0x3F800000u}},
	 0x00000000u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_EMPTY_DEST_MASK_SILENT",
	 "An FMAC with an empty destination mask clears the whole MAC and raises nothing -- not even the events its lanes would have produced",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(3), 0x0u, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000140u, 0x00000000u, 0x3F800000u}, {0x00000140u, 0x00000000u, 0x3F800000u}},
	 0x00000000u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP",
	 "VCLIP writes CLIP and leaves STATUS and MAC alone; the operands are chosen so CLIP actually changes, which is what separates this from 'did not run'",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(7), 0x0u, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}},
	 0x00000015u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_INTEGER_OP_SILENT",
	 "An integer op leaves the entire flag file untouched",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(8), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}},
	 0x00000000u, {0x00000000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_Z_AND_S_ARE_STICKY",
	 "Sticky Z and sticky S are real and independent: +0 then -1 leaves both, though the cause nibble holds only S",
	 {{static_cast<VuStickyOpKind>(2), 0x8u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(2), 0x8u, {0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000041u, 0x00000008u, 0x3F800000u}, {0x000000C2u, 0x00000080u, 0x3F800000u}, {0x000000C2u, 0x00000080u, 0x3F800000u}},
	 0x00000000u, {0xBF800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION",
	 "Three ops, three different events, one accumulation: underflow, overflow and a negative result leave sticky ZSUO with only S in the cause",
	 {{static_cast<VuStickyOpKind>(1), 0x8u, {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}}, {static_cast<VuStickyOpKind>(1), 0x8u, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}}, {static_cast<VuStickyOpKind>(2), 0x8u, {0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000145u, 0x00000808u, 0x3F800000u}, {0x00000348u, 0x00008000u, 0x3F800000u}, {0x000003C2u, 0x00000080u, 0x3F800000u}},
	 0x00000000u, {0xBF800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_DI_CAUSE_REPLACED_STICKY_KEPT",
	 "Each div-unit op REPLACES the D/I cause pair while the stickies only grow: 0/0 then x/0 then sqrt(-1) ends with cause I and sticky D+I",
	 {{static_cast<VuStickyOpKind>(4), 0x0u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}}, {static_cast<VuStickyOpKind>(5), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000410u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C20u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C10u, 0x00000000u, 0x3F800000u}},
	 0x00000000u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
	{"VUSTICKY_CLEAN_DIV_KEEPS_STICKY_DI",
	 "The sharpest form: rsqrt of -0 raises both D and I, then a perfectly clean 1/1 clears the cause pair and leaves sticky D and sticky I standing",
	 {{static_cast<VuStickyOpKind>(6), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}}, {static_cast<VuStickyOpKind>(4), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}, {static_cast<VuStickyOpKind>(0), 0x0u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}}},
	 {{0x00000000u, 0x00000000u, 0x3F800000u}, {0x00000C30u, 0x00000000u, 0x7FFFFFFFu}, {0x00000C00u, 0x00000000u, 0x3F800000u}, {0x00000C00u, 0x00000000u, 0x3F800000u}},
	 0x00000000u, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}},
};

// ---- group B: VU0 micro mode ----
//
// Each program is a run of {lower, upper} pairs ending on the E bit; the test
// harness appends the architectural delay-slot NOP itself.  vi02 and vi03 are
// FSAND snapshots the program took of STATUS after its first and second op.
//
// `inherited_mac` records that the console ran the eight programs back to
// back: a program with no FMAC of its own leaves MAC -- and therefore the
// STATUS ZSUO cause, which tracks MAC -- holding the previous program's value.
// A harness that starts from a clean VU cannot reproduce those four bits, so
// tests score the sticky field and the D/I cause and leave the ZSUO cause to
// the MAC-derived law that group A pins.
struct VuStickyProgram
{
	const char* tag;
	const char* rule;
	const u32* lower;
	const u32* upper;
	u32 n_pairs;
	u32 seed_fs1[4];
	u32 seed_ft1[4];
	u32 seed_fs2[4];
	u32 seed_ft2[4];
	u32 status_after_op1; // vi02
	u32 status_after_op2; // vi03
	u32 final_status;
	u32 final_mac;
	u32 final_clip;
	u32 final_q;
	u32 final_vi01;
	bool has_own_fmac;
};

inline constexpr u32 kVuStickyProg0Lower[] = {
	0x10010123u, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
};
inline constexpr u32 kVuStickyProg0Upper[] = {
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x400002FFu,
};
inline constexpr u32 kVuStickyProg1Lower[] = {
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg1Upper[] = {
	0x010521AAu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x01083A6Au, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg2Lower[] = {
	0x800523BCu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x800003BFu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x80083BBCu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x800003BFu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg2Upper[] = {
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg3Lower[] = {
	0x800523BEu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x800003BFu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x80083BBCu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x800003BFu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg3Upper[] = {
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg4Lower[] = {
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x2A000000u, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg4Upper[] = {
	0x010521AAu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg5Lower[] = {
	0x2A2007C0u, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg5Upper[] = {
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg6Lower[] = {
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x2A200000u, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg6Upper[] = {
	0x010521AAu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};
inline constexpr u32 kVuStickyProg7Lower[] = {
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2207FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu, 0x8000033Cu, 0x2C2307FFu, 0x8000033Cu, 0x8000033Cu, 0x8000033Cu,
	0x8000033Cu,
};
inline constexpr u32 kVuStickyProg7Upper[] = {
	0x010521AAu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x01083A68u, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu, 0x000002FFu,
	0x400002FFu,
};

inline constexpr VuStickyProgram kVuStickyPrograms[] = {
	{"VUSTICKY_MICRO_PATH_CONTROL",
	 "Path validation: a microprogram with no flag traffic at all. VI01 == 0x123 is what proves upload + kick + E-bit + read-back before anything else here",
	 kVuStickyProg0Lower, kVuStickyProg0Upper, 9u,
	 {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000000u, 0x00000000u,
	 0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u, 0x00000123u, false},
	{"VUSTICKY_MICRO_FMAC_ZSUO_ACCUMULATE",
	 "Micro mode accumulates the ZSUO stickies exactly as macro mode does",
	 kVuStickyProg1Lower, kVuStickyProg1Upper, 25u,
	 {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u}, {0x7F000000u, 0x7F000000u, 0x7F000000u, 0x7F000000u},
	 0x00000145u, 0x00000348u,
	 0x00000348u, 0x00008000u, 0x00000000u, 0x3F800000u, 0x00000000u, true},
	{"VUSTICKY_MICRO_DIV_DI_ACCUMULATE",
	 "Micro mode accumulates sticky D/I too: x/0 then 0/0 leaves both. PCSX2's micro path sets NO sticky D or I at all, so this is a divergence in the opposite direction from the macro path's",
	 kVuStickyProg2Lower, kVuStickyProg2Upper, 25u,
	 {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
	 0x00000828u, 0x00000C18u,
	 0x00000C18u, 0x00008000u, 0x00000000u, 0x7FFFFFFFu, 0x00000000u, false},
	{"VUSTICKY_MICRO_CLEAN_DIV_KEEPS_STICKY_DI",
	 "Micro-mode counterpart of the clean-divide case: the cause pair clears, sticky D and I stand",
	 kVuStickyProg3Lower, kVuStickyProg3Upper, 25u,
	 {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000C38u, 0x00000C08u,
	 0x00000C08u, 0x00008000u, 0x00000000u, 0x3F800000u, 0x00000000u, false},
	{"VUSTICKY_MICRO_FSSET_CLEARS",
	 "FSSET 0 clears the sticky field an FMAC had set",
	 kVuStickyProg4Lower, kVuStickyProg4Upper, 25u,
	 {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000145u, 0x00000005u,
	 0x00000005u, 0x00000808u, 0x00000000u, 0x3F800000u, 0x00000000u, true},
	{"VUSTICKY_MICRO_FSSET_WRITE_MASK",
	 "FSSET writes all six sticky bits: 0xFC0 from a clean STATUS reads back whole",
	 kVuStickyProg5Lower, kVuStickyProg5Upper, 25u,
	 {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000FC5u, 0x00000FC5u,
	 0x00000FC5u, 0x00000808u, 0x00000000u, 0x3F800000u, 0x00000000u, false},
	{"VUSTICKY_MICRO_FSSET_ASSIGNS_NOT_ORS",
	 "FSSET ASSIGNS the sticky field rather than ORing into it: after an underflow leaves sticky ZU, FSSET 0x800 reads back sticky D alone, not sticky ZUD",
	 kVuStickyProg6Lower, kVuStickyProg6Upper, 25u,
	 {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000145u, 0x00000805u,
	 0x00000805u, 0x00000808u, 0x00000000u, 0x3F800000u, 0x00000000u, true},
	{"VUSTICKY_MICRO_SURVIVES_SILENT_FMAC",
	 "Micro-mode counterpart of the silent-FMAC case",
	 kVuStickyProg7Lower, kVuStickyProg7Upper, 25u,
	 {0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u}, {0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}, {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u},
	 0x00000145u, 0x00000140u,
	 0x00000140u, 0x00000000u, 0x00000000u, 0x3F800000u, 0x00000000u, true},
};

} // namespace console_vusticky
