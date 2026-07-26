// GENERATED from unknownbrackets/ps2autotests tests/cpu/iop/lsu.expected and
// branch.expected (real PS2 hardware). Do not edit by hand.
#pragma once
#include <common/Pcsx2Types.h>

namespace ps2auto_iopmisc {

// C_PATTERN, 12 words. Loads read it in place with the base
// register at word 4; stores get a fresh 6-word copy per case.
inline constexpr u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
// C_GARBAGE1[0], the second store preset.
inline constexpr u32 kMemPreset = 0x00001337u;
// The lui/ori store preset.
inline constexpr u32 kImmPreset = 0xABCD4321u;
// $t6 when the lwl/lwr chain begins -- see the module comment.
inline constexpr u32 kLwlrSeed = 0xC0DE1337u;

// `expect` is the harness's printed value: $t6 ^ $t7, with $t7
// zeroed before the op. Only `ld` (two lw into t6,t7) makes the
// xor do anything. `into_zero` cases load into $0 and read it
// back through `ori res, $0, 0`.
struct IopLoadCase {
	const char* label; const char* op1; int off1;
	const char* op2; int off2;   // op2 null for single-op cases
	bool into_zero; u32 expect;
};

inline constexpr IopLoadCase kIopLoadCases[] = {
	{"lb +0 [imm]", "lb", 0, nullptr, 0, false, 0xFFFFFF89u},
	{"lb +4 [imm]", "lb", 4, nullptr, 0, false, 0x00000001u},
	{"lb -4 [imm]", "lb", -4, nullptr, 0, false, 0x00000037u},
	{"lb +0 [mem]", "lb", 0, nullptr, 0, false, 0xFFFFFF89u},
	{"lb +4 [mem]", "lb", 4, nullptr, 0, false, 0x00000001u},
	{"lb -4 [mem]", "lb", -4, nullptr, 0, false, 0x00000037u},
	{"lb -> $0", "lb", 0, nullptr, 0, true, 0x00000000u},
	{"lbu +0 [imm]", "lbu", 0, nullptr, 0, false, 0x00000089u},
	{"lbu +4 [imm]", "lbu", 4, nullptr, 0, false, 0x00000001u},
	{"lbu -4 [imm]", "lbu", -4, nullptr, 0, false, 0x00000037u},
	{"lbu +0 [mem]", "lbu", 0, nullptr, 0, false, 0x00000089u},
	{"lbu +4 [mem]", "lbu", 4, nullptr, 0, false, 0x00000001u},
	{"lbu -4 [mem]", "lbu", -4, nullptr, 0, false, 0x00000037u},
	{"lbu -> $0", "lbu", 0, nullptr, 0, true, 0x00000000u},
	{"ld +0 [imm]", "ld", 0, nullptr, 0, false, 0x88888888u},
	{"ld +4 [imm]", "ld", 4, nullptr, 0, false, 0x152231ACu},
	{"ld -4 [imm]", "ld", -4, nullptr, 0, false, 0xE39B74BEu},
	{"ld +0 [mem]", "ld", 0, nullptr, 0, false, 0x88888888u},
	{"ld +4 [mem]", "ld", 4, nullptr, 0, false, 0x152231ACu},
	{"ld -4 [mem]", "ld", -4, nullptr, 0, false, 0xE39B74BEu},
	{"ld -> $0", "ld", 0, nullptr, 0, true, 0x00000000u},
	{"lh +0 [imm]", "lh", 0, nullptr, 0, false, 0x00006789u},
	{"lh +4 [imm]", "lh", 4, nullptr, 0, false, 0xFFFFEF01u},
	{"lh -4 [imm]", "lh", -4, nullptr, 0, false, 0x00001337u},
	{"lh +0 [mem]", "lh", 0, nullptr, 0, false, 0x00006789u},
	{"lh +4 [mem]", "lh", 4, nullptr, 0, false, 0xFFFFEF01u},
	{"lh -4 [mem]", "lh", -4, nullptr, 0, false, 0x00001337u},
	{"lh -> $0", "lh", 0, nullptr, 0, true, 0x00000000u},
	{"lhu +0 [imm]", "lhu", 0, nullptr, 0, false, 0x00006789u},
	{"lhu +4 [imm]", "lhu", 4, nullptr, 0, false, 0x0000EF01u},
	{"lhu -4 [imm]", "lhu", -4, nullptr, 0, false, 0x00001337u},
	{"lhu +0 [mem]", "lhu", 0, nullptr, 0, false, 0x00006789u},
	{"lhu +4 [mem]", "lhu", 4, nullptr, 0, false, 0x0000EF01u},
	{"lhu -4 [mem]", "lhu", -4, nullptr, 0, false, 0x00001337u},
	{"lhu -> $0", "lhu", 0, nullptr, 0, true, 0x00000000u},
	{"lw +0 [imm]", "lw", 0, nullptr, 0, false, 0x23456789u},
	{"lw +4 [imm]", "lw", 4, nullptr, 0, false, 0xABCDEF01u},
	{"lw -4 [imm]", "lw", -4, nullptr, 0, false, 0xC0DE1337u},
	{"lw +0 [mem]", "lw", 0, nullptr, 0, false, 0x23456789u},
	{"lw +4 [mem]", "lw", 4, nullptr, 0, false, 0xABCDEF01u},
	{"lw -4 [mem]", "lw", -4, nullptr, 0, false, 0xC0DE1337u},
	{"lw -> $0", "lw", 0, nullptr, 0, true, 0x00000000u},
	{"lwl +0 [imm]", "lwl", 0, nullptr, 0, false, 0x89DE1337u},
	{"lwr +0 [imm]", "lwr", 0, nullptr, 0, false, 0x23456789u},
	{"lwl +1 [imm]", "lwl", 1, nullptr, 0, false, 0x67896789u},
	{"lwr +1 [imm]", "lwr", 1, nullptr, 0, false, 0x67234567u},
	{"lwl +0/lwr +3 [imm]", "lwl", 0, "lwr", 3, false, 0x89234523u},
	{"lwl +0 [mem]", "lwl", 0, nullptr, 0, false, 0x89234523u},
	{"lwr +0 [mem]", "lwr", 0, nullptr, 0, false, 0x23456789u},
	{"lwl +1 [mem]", "lwl", 1, nullptr, 0, false, 0x67896789u},
	{"lwr +1 [mem]", "lwr", 1, nullptr, 0, false, 0x67234567u},
	{"lwl +0/lwr +3 [mem]", "lwl", 0, "lwr", 3, false, 0x89234523u},
	{"lwl -> $0", "lwl", 0, nullptr, 0, true, 0x00000000u},
	{"lwr -> $0", "lwr", 0, nullptr, 0, true, 0x00000000u},
};

// `block` is the byte delta from the base register to the two
// words the capture printed. `imm_preset` picks kImmPreset over
// kMemPreset as the stored value.
struct IopStoreCase {
	const char* label; const char* op1; int off1;
	const char* op2; int off2;
	bool imm_preset; int block;
	u32 mem[2];
};

inline constexpr IopStoreCase kIopStoreCases[] = {
	{"sb +0 [imm]", "sb", 0, nullptr, 0, true, 0, {0x9ABCDE21u, 0xDEADBEEFu}},
	{"sb +4 [imm]", "sb", 4, nullptr, 0, true, 4, {0xDEADBE21u, 0xC0DE1337u}},
	{"sb -4 [imm]", "sb", -4, nullptr, 0, true, -4, {0x45678121u, 0x9ABCDEF0u}},
	{"sb +0 [mem]", "sb", 0, nullptr, 0, false, 0, {0x9ABCDE37u, 0xDEADBEEFu}},
	{"sb +4 [mem]", "sb", 4, nullptr, 0, false, 4, {0xDEADBE37u, 0xC0DE1337u}},
	{"sb -4 [mem]", "sb", -4, nullptr, 0, false, -4, {0x45678137u, 0x9ABCDEF0u}},
	{"sd +0 [imm]", "sd", 0, nullptr, 0, true, 0, {0xABCD4321u, 0x00000000u}},
	{"sd +4 [imm]", "sd", 4, nullptr, 0, true, 4, {0xABCD4321u, 0x00000000u}},
	{"sd -4 [imm]", "sd", -4, nullptr, 0, true, -4, {0xABCD4321u, 0x00000000u}},
	{"sd +0 [mem]", "sd", 0, nullptr, 0, false, 0, {0x00001337u, 0x00000000u}},
	{"sd +4 [mem]", "sd", 4, nullptr, 0, false, 4, {0x00001337u, 0x00000000u}},
	{"sd -4 [mem]", "sd", -4, nullptr, 0, false, -4, {0x00001337u, 0x00000000u}},
	{"sh +0 [imm]", "sh", 0, nullptr, 0, true, 0, {0x9ABC4321u, 0xDEADBEEFu}},
	{"sh +4 [imm]", "sh", 4, nullptr, 0, true, 4, {0xDEAD4321u, 0xC0DE1337u}},
	{"sh -4 [imm]", "sh", -4, nullptr, 0, true, -4, {0x45674321u, 0x9ABCDEF0u}},
	{"sh +0 [mem]", "sh", 0, nullptr, 0, false, 0, {0x9ABC1337u, 0xDEADBEEFu}},
	{"sh +4 [mem]", "sh", 4, nullptr, 0, false, 4, {0xDEAD1337u, 0xC0DE1337u}},
	{"sh -4 [mem]", "sh", -4, nullptr, 0, false, -4, {0x45671337u, 0x9ABCDEF0u}},
	{"sw +0 [imm]", "sw", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xDEADBEEFu}},
	{"sw +4 [imm]", "sw", 4, nullptr, 0, true, 4, {0xABCD4321u, 0xC0DE1337u}},
	{"sw -4 [imm]", "sw", -4, nullptr, 0, true, -4, {0xABCD4321u, 0x9ABCDEF0u}},
	{"sw +0 [mem]", "sw", 0, nullptr, 0, false, 0, {0x00001337u, 0xDEADBEEFu}},
	{"sw +4 [mem]", "sw", 4, nullptr, 0, false, 4, {0x00001337u, 0xC0DE1337u}},
	{"sw -4 [mem]", "sw", -4, nullptr, 0, false, -4, {0x00001337u, 0x9ABCDEF0u}},
	{"swl +0 [imm]", "swl", 0, nullptr, 0, true, 0, {0x9ABCDEABu, 0xDEADBEEFu}},
	{"swr +0 [imm]", "swr", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xDEADBEEFu}},
	{"swl +1 [imm]", "swl", 1, nullptr, 0, true, 0, {0x9ABCABCDu, 0xDEADBEEFu}},
	{"swr +1 [imm]", "swr", 1, nullptr, 0, true, 0, {0xCD4321F0u, 0xDEADBEEFu}},
	{"swl +0/swr +3 [imm]", "swl", 0, "swr", 3, true, 0, {0x21BCDEABu, 0xDEADBEEFu}},
	{"swl +0 [mem]", "swl", 0, nullptr, 0, false, 0, {0x9ABCDE00u, 0xDEADBEEFu}},
	{"swr +0 [mem]", "swr", 0, nullptr, 0, false, 0, {0x00001337u, 0xDEADBEEFu}},
	{"swl +1 [mem]", "swl", 1, nullptr, 0, false, 0, {0x9ABC0000u, 0xDEADBEEFu}},
	{"swr +1 [mem]", "swr", 1, nullptr, 0, false, 0, {0x001337F0u, 0xDEADBEEFu}},
	{"swl +0/swr +3 [mem]", "swl", 0, "swr", 3, false, 0, {0x37BCDE00u, 0xDEADBEEFu}},
};

// How the capture's asm wrapper feeds the op.
enum BranchForm {
	BF_RSRT,   // beq/bne rs, rt, label
	BF_RS,     // bgez/bgtz/blez/bltz/{bgez,bltz}al rs, label
	BF_NONE,   // j / jal label
	BF_JALR,   // jalr rd, rs   (rd checked for a link write)
	BF_JR,     // jr rs
};

// `flags` is the capture's three English words, bit 0
// followed, bit 1 set ra, bit 2 ran delay slot.
struct IopBranchCase {
	const char* label; const char* op; int form;
	u32 rs, rt; u32 flags;
};

inline constexpr u32 BR_FOLLOWED = 1u;
inline constexpr u32 BR_SET_RA = 2u;
inline constexpr u32 BR_DELAY_SLOT = 4u;

inline constexpr IopBranchCase kIopBranchCases[] = {
	{"beq 0, 0", "beq", BF_RSRT, 0x00000000u, 0x00000000u, 5u},
	{"beq 0, 1", "beq", BF_RSRT, 0x00000000u, 0x00000001u, 4u},
	{"beq 1, 1", "beq", BF_RSRT, 0x00000001u, 0x00000001u, 5u},
	{"beq 1, 0", "beq", BF_RSRT, 0x00000001u, 0x00000000u, 4u},
	{"beq 2, 2", "beq", BF_RSRT, 0x00000002u, 0x00000002u, 5u},
	{"beq -1, 1", "beq", BF_RSRT, 0xFFFFFFFFu, 0x00000001u, 4u},
	{"beq -1, -1", "beq", BF_RSRT, 0xFFFFFFFFu, 0xFFFFFFFFu, 5u},
	{"beq C_ZERO, C_ZERO", "beq", BF_RSRT, 0x00000000u, 0x00000000u, 5u},
	{"beq C_ZERO, C_ONE", "beq", BF_RSRT, 0x00000000u, 0x00000001u, 4u},
	{"beq C_ONE, C_ZERO", "beq", BF_RSRT, 0x00000001u, 0x00000000u, 4u},
	{"beq C_ONE, C_ONE", "beq", BF_RSRT, 0x00000001u, 0x00000001u, 5u},
	{"beq C_ONE, C_NEGONE", "beq", BF_RSRT, 0x00000001u, 0xFFFFFFFFu, 4u},
	{"beq C_S16_MAX, C_S16_MAX", "beq", BF_RSRT, 0x00007FFFu, 0x00007FFFu, 5u},
	{"beq C_S16_MIN, C_S16_MIN", "beq", BF_RSRT, 0xFFFF8000u, 0xFFFF8000u, 5u},
	{"beq C_S32_MAX, C_S32_MAX", "beq", BF_RSRT, 0x7FFFFFFFu, 0x7FFFFFFFu, 5u},
	{"beq C_S32_MIN, C_S32_MIN", "beq", BF_RSRT, 0x80000000u, 0x80000000u, 5u},
	{"beq C_S64_MAX, C_S64_MAX", "beq", BF_RSRT, 0xFFFFFFFFu, 0xFFFFFFFFu, 5u},
	{"beq C_S64_MIN, C_S64_MIN", "beq", BF_RSRT, 0x00000000u, 0x00000000u, 5u},
	{"beq C_GARBAGE1, C_GARBAGE2", "beq", BF_RSRT, 0x00001337u, 0xDEADBEEFu, 4u},
	{"bgez 0", "bgez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"bgez 1", "bgez", BF_RS, 0x00000001u, 0x00000000u, 5u},
	{"bgez 2", "bgez", BF_RS, 0x00000002u, 0x00000000u, 5u},
	{"bgez -1", "bgez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgez 2147483647", "bgez", BF_RS, 0x7FFFFFFFu, 0x00000000u, 5u},
	{"bgez -2147483648", "bgez", BF_RS, 0x80000000u, 0x00000000u, 4u},
	{"bgez C_ZERO", "bgez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"bgez C_ONE", "bgez", BF_RS, 0x00000001u, 0x00000000u, 5u},
	{"bgez C_NEGONE", "bgez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgez C_S16_MAX", "bgez", BF_RS, 0x00007FFFu, 0x00000000u, 5u},
	{"bgez C_S16_MIN", "bgez", BF_RS, 0xFFFF8000u, 0x00000000u, 4u},
	{"bgez C_S32_MAX", "bgez", BF_RS, 0x7FFFFFFFu, 0x00000000u, 5u},
	{"bgez C_S32_MIN", "bgez", BF_RS, 0x80000000u, 0x00000000u, 4u},
	{"bgez C_S64_MAX", "bgez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgez C_S64_MIN", "bgez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"bgez C_GARBAGE1", "bgez", BF_RS, 0x00001337u, 0x00000000u, 5u},
	{"bgez C_GARBAGE2", "bgez", BF_RS, 0xDEADBEEFu, 0x00000000u, 4u},
	{"bgezal 0", "bgezal", BF_RS, 0x00000000u, 0x00000000u, 7u},
	{"bgezal 1", "bgezal", BF_RS, 0x00000001u, 0x00000000u, 7u},
	{"bgezal 2", "bgezal", BF_RS, 0x00000002u, 0x00000000u, 7u},
	{"bgezal -1", "bgezal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 6u},
	{"bgezal 2147483647", "bgezal", BF_RS, 0x7FFFFFFFu, 0x00000000u, 7u},
	{"bgezal -2147483648", "bgezal", BF_RS, 0x80000000u, 0x00000000u, 6u},
	{"bgezal C_ZERO", "bgezal", BF_RS, 0x00000000u, 0x00000000u, 7u},
	{"bgezal C_ONE", "bgezal", BF_RS, 0x00000001u, 0x00000000u, 7u},
	{"bgezal C_NEGONE", "bgezal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 6u},
	{"bgezal C_S16_MAX", "bgezal", BF_RS, 0x00007FFFu, 0x00000000u, 7u},
	{"bgezal C_S16_MIN", "bgezal", BF_RS, 0xFFFF8000u, 0x00000000u, 6u},
	{"bgezal C_S32_MAX", "bgezal", BF_RS, 0x7FFFFFFFu, 0x00000000u, 7u},
	{"bgezal C_S32_MIN", "bgezal", BF_RS, 0x80000000u, 0x00000000u, 6u},
	{"bgezal C_S64_MAX", "bgezal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 6u},
	{"bgezal C_S64_MIN", "bgezal", BF_RS, 0x00000000u, 0x00000000u, 7u},
	{"bgezal C_GARBAGE1", "bgezal", BF_RS, 0x00001337u, 0x00000000u, 7u},
	{"bgezal C_GARBAGE2", "bgezal", BF_RS, 0xDEADBEEFu, 0x00000000u, 6u},
	{"bgtz 0", "bgtz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bgtz 1", "bgtz", BF_RS, 0x00000001u, 0x00000000u, 5u},
	{"bgtz 2", "bgtz", BF_RS, 0x00000002u, 0x00000000u, 5u},
	{"bgtz -1", "bgtz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgtz 2147483647", "bgtz", BF_RS, 0x7FFFFFFFu, 0x00000000u, 5u},
	{"bgtz -2147483648", "bgtz", BF_RS, 0x80000000u, 0x00000000u, 4u},
	{"bgtz C_ZERO", "bgtz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bgtz C_ONE", "bgtz", BF_RS, 0x00000001u, 0x00000000u, 5u},
	{"bgtz C_NEGONE", "bgtz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgtz C_S16_MAX", "bgtz", BF_RS, 0x00007FFFu, 0x00000000u, 5u},
	{"bgtz C_S16_MIN", "bgtz", BF_RS, 0xFFFF8000u, 0x00000000u, 4u},
	{"bgtz C_S32_MAX", "bgtz", BF_RS, 0x7FFFFFFFu, 0x00000000u, 5u},
	{"bgtz C_S32_MIN", "bgtz", BF_RS, 0x80000000u, 0x00000000u, 4u},
	{"bgtz C_S64_MAX", "bgtz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 4u},
	{"bgtz C_S64_MIN", "bgtz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bgtz C_GARBAGE1", "bgtz", BF_RS, 0x00001337u, 0x00000000u, 5u},
	{"bgtz C_GARBAGE2", "bgtz", BF_RS, 0xDEADBEEFu, 0x00000000u, 4u},
	{"blez 0", "blez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"blez 1", "blez", BF_RS, 0x00000001u, 0x00000000u, 4u},
	{"blez 2", "blez", BF_RS, 0x00000002u, 0x00000000u, 4u},
	{"blez -1", "blez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"blez 2147483647", "blez", BF_RS, 0x7FFFFFFFu, 0x00000000u, 4u},
	{"blez -2147483648", "blez", BF_RS, 0x80000000u, 0x00000000u, 5u},
	{"blez C_ZERO", "blez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"blez C_ONE", "blez", BF_RS, 0x00000001u, 0x00000000u, 4u},
	{"blez C_NEGONE", "blez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"blez C_S16_MAX", "blez", BF_RS, 0x00007FFFu, 0x00000000u, 4u},
	{"blez C_S16_MIN", "blez", BF_RS, 0xFFFF8000u, 0x00000000u, 5u},
	{"blez C_S32_MAX", "blez", BF_RS, 0x7FFFFFFFu, 0x00000000u, 4u},
	{"blez C_S32_MIN", "blez", BF_RS, 0x80000000u, 0x00000000u, 5u},
	{"blez C_S64_MAX", "blez", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"blez C_S64_MIN", "blez", BF_RS, 0x00000000u, 0x00000000u, 5u},
	{"blez C_GARBAGE1", "blez", BF_RS, 0x00001337u, 0x00000000u, 4u},
	{"blez C_GARBAGE2", "blez", BF_RS, 0xDEADBEEFu, 0x00000000u, 5u},
	{"bltz 0", "bltz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bltz 1", "bltz", BF_RS, 0x00000001u, 0x00000000u, 4u},
	{"bltz 2", "bltz", BF_RS, 0x00000002u, 0x00000000u, 4u},
	{"bltz -1", "bltz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"bltz 2147483647", "bltz", BF_RS, 0x7FFFFFFFu, 0x00000000u, 4u},
	{"bltz -2147483648", "bltz", BF_RS, 0x80000000u, 0x00000000u, 5u},
	{"bltz C_ZERO", "bltz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bltz C_ONE", "bltz", BF_RS, 0x00000001u, 0x00000000u, 4u},
	{"bltz C_NEGONE", "bltz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"bltz C_S16_MAX", "bltz", BF_RS, 0x00007FFFu, 0x00000000u, 4u},
	{"bltz C_S16_MIN", "bltz", BF_RS, 0xFFFF8000u, 0x00000000u, 5u},
	{"bltz C_S32_MAX", "bltz", BF_RS, 0x7FFFFFFFu, 0x00000000u, 4u},
	{"bltz C_S32_MIN", "bltz", BF_RS, 0x80000000u, 0x00000000u, 5u},
	{"bltz C_S64_MAX", "bltz", BF_RS, 0xFFFFFFFFu, 0x00000000u, 5u},
	{"bltz C_S64_MIN", "bltz", BF_RS, 0x00000000u, 0x00000000u, 4u},
	{"bltz C_GARBAGE1", "bltz", BF_RS, 0x00001337u, 0x00000000u, 4u},
	{"bltz C_GARBAGE2", "bltz", BF_RS, 0xDEADBEEFu, 0x00000000u, 5u},
	{"bltzal 0", "bltzal", BF_RS, 0x00000000u, 0x00000000u, 6u},
	{"bltzal 1", "bltzal", BF_RS, 0x00000001u, 0x00000000u, 6u},
	{"bltzal 2", "bltzal", BF_RS, 0x00000002u, 0x00000000u, 6u},
	{"bltzal -1", "bltzal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 7u},
	{"bltzal 2147483647", "bltzal", BF_RS, 0x7FFFFFFFu, 0x00000000u, 6u},
	{"bltzal -2147483648", "bltzal", BF_RS, 0x80000000u, 0x00000000u, 7u},
	{"bltzal C_ZERO", "bltzal", BF_RS, 0x00000000u, 0x00000000u, 6u},
	{"bltzal C_ONE", "bltzal", BF_RS, 0x00000001u, 0x00000000u, 6u},
	{"bltzal C_NEGONE", "bltzal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 7u},
	{"bltzal C_S16_MAX", "bltzal", BF_RS, 0x00007FFFu, 0x00000000u, 6u},
	{"bltzal C_S16_MIN", "bltzal", BF_RS, 0xFFFF8000u, 0x00000000u, 7u},
	{"bltzal C_S32_MAX", "bltzal", BF_RS, 0x7FFFFFFFu, 0x00000000u, 6u},
	{"bltzal C_S32_MIN", "bltzal", BF_RS, 0x80000000u, 0x00000000u, 7u},
	{"bltzal C_S64_MAX", "bltzal", BF_RS, 0xFFFFFFFFu, 0x00000000u, 7u},
	{"bltzal C_S64_MIN", "bltzal", BF_RS, 0x00000000u, 0x00000000u, 6u},
	{"bltzal C_GARBAGE1", "bltzal", BF_RS, 0x00001337u, 0x00000000u, 6u},
	{"bltzal C_GARBAGE2", "bltzal", BF_RS, 0xDEADBEEFu, 0x00000000u, 7u},
	{"bne 0, 0", "bne", BF_RSRT, 0x00000000u, 0x00000000u, 4u},
	{"bne 0, 1", "bne", BF_RSRT, 0x00000000u, 0x00000001u, 5u},
	{"bne 1, 1", "bne", BF_RSRT, 0x00000001u, 0x00000001u, 4u},
	{"bne 1, 0", "bne", BF_RSRT, 0x00000001u, 0x00000000u, 5u},
	{"bne 2, 2", "bne", BF_RSRT, 0x00000002u, 0x00000002u, 4u},
	{"bne -1, 1", "bne", BF_RSRT, 0xFFFFFFFFu, 0x00000001u, 5u},
	{"bne -1, -1", "bne", BF_RSRT, 0xFFFFFFFFu, 0xFFFFFFFFu, 4u},
	{"bne C_ZERO, C_ZERO", "bne", BF_RSRT, 0x00000000u, 0x00000000u, 4u},
	{"bne C_ZERO, C_ONE", "bne", BF_RSRT, 0x00000000u, 0x00000001u, 5u},
	{"bne C_ONE, C_ZERO", "bne", BF_RSRT, 0x00000001u, 0x00000000u, 5u},
	{"bne C_ONE, C_ONE", "bne", BF_RSRT, 0x00000001u, 0x00000001u, 4u},
	{"bne C_ONE, C_NEGONE", "bne", BF_RSRT, 0x00000001u, 0xFFFFFFFFu, 5u},
	{"bne C_S16_MAX, C_S16_MAX", "bne", BF_RSRT, 0x00007FFFu, 0x00007FFFu, 4u},
	{"bne C_S16_MIN, C_S16_MIN", "bne", BF_RSRT, 0xFFFF8000u, 0xFFFF8000u, 4u},
	{"bne C_S32_MAX, C_S32_MAX", "bne", BF_RSRT, 0x7FFFFFFFu, 0x7FFFFFFFu, 4u},
	{"bne C_S32_MIN, C_S32_MIN", "bne", BF_RSRT, 0x80000000u, 0x80000000u, 4u},
	{"bne C_S64_MAX, C_S64_MAX", "bne", BF_RSRT, 0xFFFFFFFFu, 0xFFFFFFFFu, 4u},
	{"bne C_S64_MIN, C_S64_MIN", "bne", BF_RSRT, 0x00000000u, 0x00000000u, 4u},
	{"bne C_GARBAGE1, C_GARBAGE2", "bne", BF_RSRT, 0x00001337u, 0xDEADBEEFu, 5u},
	{"j", "j", BF_NONE, 0x00000000u, 0x00000000u, 5u},
	{"jal", "jal", BF_NONE, 0x00000000u, 0x00000000u, 7u},
	{"jalr", "jalr", BF_JALR, 0x00000000u, 0x00000000u, 7u},
	{"jr", "jr", BF_JR, 0x00000000u, 0x00000000u, 5u},
};

inline constexpr int kIopLoadCaseCount = 54;
inline constexpr int kIopStoreCaseCount = 34;
inline constexpr int kIopBranchCaseCount = 144;

} // namespace ps2auto_iopmisc
