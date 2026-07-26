// GENERATED from unknownbrackets/ps2autotests tests/cpu/vu0_macro/integer.expected
// (real PS2 hardware). Do not edit by hand.
#pragma once
#include <common/Pcsx2Types.h>

namespace ps2auto_vu0macro {

enum VuMacroForm {
	VM_RRR = 0,  // VOP id, is, it
	VM_RRI = 1,  // VOP it, is, imm5
};

struct VuMacroCase {
	const char* op; const char* label; int form;
	u32 vs, vt; int imm; u32 vd;
};

inline constexpr u32 kVdPre = 0x1337u;

inline constexpr VuMacroCase kVuMacroCases[] = {
	{"viadd", "viadd 0, 0", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viadd", "viadd 0, 1", 0, 0x0000u, 0x0001u, 0, 0x0001u},
	{"viadd", "viadd 1, 1", 0, 0x0001u, 0x0001u, 0, 0x0002u},
	{"viadd", "viadd 1, 0", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"viadd", "viadd 2, 2", 0, 0x0002u, 0x0002u, 0, 0x0004u},
	{"viadd", "viadd -1, 1", 0, 0xFFFFu, 0x0001u, 0, 0x0000u},
	{"viadd", "viadd -1, -1", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFEu},
	{"viadd", "viadd 57005, 65535", 0, 0xDEADu, 0xFFFFu, 0, 0xDEACu},
	{"viadd", "viadd 57005, 3855", 0, 0xDEADu, 0x0F0Fu, 0, 0xEDBCu},
	{"viadd", "viadd CVI_ZERO, CVI_ZERO", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viadd", "viadd CVI_ZERO, CVI_ONE", 0, 0x0000u, 0x0001u, 0, 0x0001u},
	{"viadd", "viadd CVI_ONE, CVI_ZERO", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"viadd", "viadd CVI_ONE, CVI_ONE", 0, 0x0001u, 0x0001u, 0, 0x0002u},
	{"viadd", "viadd CVI_ONE, CVI_NEGONE", 0, 0x0001u, 0xFFFFu, 0, 0x0000u},
	{"viadd", "viadd CVI_S16_MAX, CVI_S16_MAX", 0, 0x7FFFu, 0x7FFFu, 0, 0xFFFEu},
	{"viadd", "viadd CVI_S16_MIN, CVI_S16_MIN", 0, 0x8000u, 0x8000u, 0, 0x0000u},
	{"viadd", "viadd CVI_S32_MAX, CVI_S32_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFEu},
	{"viadd", "viadd CVI_S32_MIN, CVI_S32_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viadd", "viadd CVI_S64_MAX, CVI_S64_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFEu},
	{"viadd", "viadd CVI_S64_MIN, CVI_S64_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viadd", "viadd CVI_GARBAGE1, CVI_GARBAGE2", 0, 0x1337u, 0xBEEFu, 0, 0xD226u},
	{"viaddi", "viaddi 0, 0", 1, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viaddi", "viaddi 1, 1", 1, 0x0001u, 0x0000u, 1, 0x0002u},
	{"viaddi", "viaddi 0, 15", 1, 0x0000u, 0x0000u, 15, 0x000Fu},
	{"viaddi", "viaddi 0, -16", 1, 0x0000u, 0x0000u, -16, 0xFFF0u},
	{"viaddi", "viaddi 1233, 1", 1, 0x04D1u, 0x0000u, 1, 0x04D2u},
	{"viaddi", "viaddi CVI_ZERO, 0", 1, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viaddi", "viaddi CVI_ZERO, 1", 1, 0x0000u, 0x0000u, 1, 0x0001u},
	{"viaddi", "viaddi CVI_ONE, 0", 1, 0x0001u, 0x0000u, 0, 0x0001u},
	{"viaddi", "viaddi CVI_ONE, 1", 1, 0x0001u, 0x0000u, 1, 0x0002u},
	{"viaddi", "viaddi CVI_ONE, -1", 1, 0x0001u, 0x0000u, -1, 0x0000u},
	{"viaddi", "viaddi CVI_S16_MAX, 15", 1, 0x7FFFu, 0x0000u, 15, 0x800Eu},
	{"viaddi", "viaddi CVI_S16_MIN, -16", 1, 0x8000u, 0x0000u, -16, 0x7FF0u},
	{"viaddi", "viaddi CVI_S32_MAX, 15", 1, 0xFFFFu, 0x0000u, 15, 0x000Eu},
	{"viaddi", "viaddi CVI_S32_MIN, -16", 1, 0x0000u, 0x0000u, -16, 0xFFF0u},
	{"viaddi", "viaddi CVI_S64_MAX, 15", 1, 0xFFFFu, 0x0000u, 15, 0x000Eu},
	{"viaddi", "viaddi CVI_S64_MIN, -15", 1, 0x0000u, 0x0000u, -15, 0xFFF1u},
	{"viaddi", "viaddi CVI_GARBAGE1, 3", 1, 0x1337u, 0x0000u, 3, 0x133Au},
	{"viand", "viand 0, 0", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viand", "viand 0, 1", 0, 0x0000u, 0x0001u, 0, 0x0000u},
	{"viand", "viand 1, 1", 0, 0x0001u, 0x0001u, 0, 0x0001u},
	{"viand", "viand 1, 0", 0, 0x0001u, 0x0000u, 0, 0x0000u},
	{"viand", "viand 2, 2", 0, 0x0002u, 0x0002u, 0, 0x0002u},
	{"viand", "viand -1, 1", 0, 0xFFFFu, 0x0001u, 0, 0x0001u},
	{"viand", "viand -1, -1", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"viand", "viand 57005, 65535", 0, 0xDEADu, 0xFFFFu, 0, 0xDEADu},
	{"viand", "viand 57005, 3855", 0, 0xDEADu, 0x0F0Fu, 0, 0x0E0Du},
	{"viand", "viand CVI_ZERO, CVI_ZERO", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viand", "viand CVI_ZERO, CVI_ONE", 0, 0x0000u, 0x0001u, 0, 0x0000u},
	{"viand", "viand CVI_ONE, CVI_ZERO", 0, 0x0001u, 0x0000u, 0, 0x0000u},
	{"viand", "viand CVI_ONE, CVI_ONE", 0, 0x0001u, 0x0001u, 0, 0x0001u},
	{"viand", "viand CVI_ONE, CVI_NEGONE", 0, 0x0001u, 0xFFFFu, 0, 0x0001u},
	{"viand", "viand CVI_S16_MAX, CVI_S16_MAX", 0, 0x7FFFu, 0x7FFFu, 0, 0x7FFFu},
	{"viand", "viand CVI_S16_MIN, CVI_S16_MIN", 0, 0x8000u, 0x8000u, 0, 0x8000u},
	{"viand", "viand CVI_S32_MAX, CVI_S32_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"viand", "viand CVI_S32_MIN, CVI_S32_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viand", "viand CVI_S64_MAX, CVI_S64_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"viand", "viand CVI_S64_MIN, CVI_S64_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"viand", "viand CVI_GARBAGE1, CVI_GARBAGE2", 0, 0x1337u, 0xBEEFu, 0, 0x1227u},
	{"vior", "vior 0, 0", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"vior", "vior 0, 1", 0, 0x0000u, 0x0001u, 0, 0x0001u},
	{"vior", "vior 1, 1", 0, 0x0001u, 0x0001u, 0, 0x0001u},
	{"vior", "vior 1, 0", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"vior", "vior 2, 2", 0, 0x0002u, 0x0002u, 0, 0x0002u},
	{"vior", "vior -1, 1", 0, 0xFFFFu, 0x0001u, 0, 0xFFFFu},
	{"vior", "vior -1, -1", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"vior", "vior 57005, 65535", 0, 0xDEADu, 0xFFFFu, 0, 0xFFFFu},
	{"vior", "vior 57005, 3855", 0, 0xDEADu, 0x0F0Fu, 0, 0xDFAFu},
	{"vior", "vior CVI_ZERO, CVI_ZERO", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"vior", "vior CVI_ZERO, CVI_ONE", 0, 0x0000u, 0x0001u, 0, 0x0001u},
	{"vior", "vior CVI_ONE, CVI_ZERO", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"vior", "vior CVI_ONE, CVI_ONE", 0, 0x0001u, 0x0001u, 0, 0x0001u},
	{"vior", "vior CVI_ONE, CVI_NEGONE", 0, 0x0001u, 0xFFFFu, 0, 0xFFFFu},
	{"vior", "vior CVI_S16_MAX, CVI_S16_MAX", 0, 0x7FFFu, 0x7FFFu, 0, 0x7FFFu},
	{"vior", "vior CVI_S16_MIN, CVI_S16_MIN", 0, 0x8000u, 0x8000u, 0, 0x8000u},
	{"vior", "vior CVI_S32_MAX, CVI_S32_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"vior", "vior CVI_S32_MIN, CVI_S32_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"vior", "vior CVI_S64_MAX, CVI_S64_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0xFFFFu},
	{"vior", "vior CVI_S64_MIN, CVI_S64_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"vior", "vior CVI_GARBAGE1, CVI_GARBAGE2", 0, 0x1337u, 0xBEEFu, 0, 0xBFFFu},
	{"visub", "visub 0, 0", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"visub", "visub 0, 1", 0, 0x0000u, 0x0001u, 0, 0xFFFFu},
	{"visub", "visub 1, 1", 0, 0x0001u, 0x0001u, 0, 0x0000u},
	{"visub", "visub 1, 0", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"visub", "visub 2, 2", 0, 0x0002u, 0x0002u, 0, 0x0000u},
	{"visub", "visub -1, 1", 0, 0xFFFFu, 0x0001u, 0, 0xFFFEu},
	{"visub", "visub -1, -1", 0, 0xFFFFu, 0xFFFFu, 0, 0x0000u},
	{"visub", "visub 57005, 65535", 0, 0xDEADu, 0xFFFFu, 0, 0xDEAEu},
	{"visub", "visub 57005, 3855", 0, 0xDEADu, 0x0F0Fu, 0, 0xCF9Eu},
	{"visub", "visub CVI_ZERO, CVI_ZERO", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"visub", "visub CVI_ZERO, CVI_ONE", 0, 0x0000u, 0x0001u, 0, 0xFFFFu},
	{"visub", "visub CVI_ONE, CVI_ZERO", 0, 0x0001u, 0x0000u, 0, 0x0001u},
	{"visub", "visub CVI_ONE, CVI_ONE", 0, 0x0001u, 0x0001u, 0, 0x0000u},
	{"visub", "visub CVI_ONE, CVI_NEGONE", 0, 0x0001u, 0xFFFFu, 0, 0x0002u},
	{"visub", "visub CVI_S16_MAX, CVI_S16_MAX", 0, 0x7FFFu, 0x7FFFu, 0, 0x0000u},
	{"visub", "visub CVI_S16_MIN, CVI_S16_MIN", 0, 0x8000u, 0x8000u, 0, 0x0000u},
	{"visub", "visub CVI_S32_MAX, CVI_S32_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0x0000u},
	{"visub", "visub CVI_S32_MIN, CVI_S32_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"visub", "visub CVI_S64_MAX, CVI_S64_MAX", 0, 0xFFFFu, 0xFFFFu, 0, 0x0000u},
	{"visub", "visub CVI_S64_MIN, CVI_S64_MIN", 0, 0x0000u, 0x0000u, 0, 0x0000u},
	{"visub", "visub CVI_GARBAGE1, CVI_GARBAGE2", 0, 0x1337u, 0xBEEFu, 0, 0x5448u},
};

inline constexpr int kVuMacroCaseCount = 101;

// Each op's block ends by writing vi00 and reading it back; the
// console printed 0000 for every one.
struct VuMacroZeroCase { const char* op; u32 vd; };
inline constexpr VuMacroZeroCase kVuMacroZeroCases[] = {
	{"viadd", 0x0000u},
	{"viaddi", 0x0000u},
	{"viand", 0x0000u},
	{"vior", 0x0000u},
	{"visub", 0x0000u},
};

inline constexpr int kVuMacroZeroCaseCount = 5;

// CTC2 write-mask sweep: `addiu $t0, $0, -1; ctc2 $t0, viNN;
// cfc2 rd, viNN` per control register, straight off the console.
// `defined` is false for the four reserved indices -- recorded,
// not asserted; see the generator header.
struct Ctc2MaskCase {
	int reg; const char* name; bool defined; u32 readback;
};
inline constexpr Ctc2MaskCase kCtc2MaskCases[] = {
	{0, "VI00 (hardwired zero)", true, 0x00000000u},
	{1, "VI01 (general integer)", true, 0x0000FFFFu},
	{16, "STATUS flag", true, 0x00000FC0u},
	{17, "MAC flag", true, 0x00000000u},
	{18, "CLIP flag", true, 0x00FFFFFFu},
	{19, "reserved", false, 0x00002E30u},
	{20, "R (random)", true, 0x007FFFFFu},
	{21, "I (immediate)", true, 0xFFFFFFFFu},
	{22, "Q (quotient)", true, 0xFFFFFFFFu},
	{23, "reserved", false, 0x00000000u},
	{24, "reserved", false, 0x00000C0Cu},
	{25, "reserved", false, 0x00000000u},
	{27, "CMSAR0", true, 0x0000FFFFu},
	{28, "FBRST", true, 0x00000000u},
	{29, "VPU-STAT", true, 0x00000000u},
	{31, "CMSAR1", true, 0x0000FFFFu},
};

inline constexpr int kCtc2MaskCaseCount = 16;

} // namespace ps2auto_vu0macro
