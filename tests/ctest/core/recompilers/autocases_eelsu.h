// GENERATED from unknownbrackets/ps2autotests tests/cpu/ee/lsu.expected
// (real PS2 hardware). Do not edit by hand.
#pragma once
#include <common/Pcsx2Types.h>

namespace ps2auto_eelsu {

// C_PATTERN, 3 x 16 bytes. Loads read it in place (base = +16);
// stores get a fresh copy per case.
inline constexpr u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
// C_GARBAGE1, the quad register preset.
inline constexpr u32 kGarbage1[4] = {0x00001337u, 0x00001338u, 0x00001339u, 0x0000133Au};
// The lui/ori register preset, before sign extension.
inline constexpr u32 kImmPreset = 0xABCD4321u;

// `into_zero` cases load into $0 and read it back through
// `ori rt, $0, 0`, so lo must be 0 and hi must survive.
struct LoadCase {
	const char* label; const char* op1; int off1;
	const char* op2; int off2;   // op2 null for single-op cases
	bool imm_preset; bool into_zero;
	u64 lo, hi;                  // expected 128-bit rt
};

inline constexpr LoadCase kLoadCases[] = {
	{"lb +0 [imm]", "lb", 0, nullptr, 0, true, false, 0xFFFFFFFFFFFFFF89ull, 0x0000133A00001339ull},
	{"lb +16 [imm]", "lb", 16, nullptr, 0, true, false, 0xFFFFFFFFFFFFFFBBull, 0x0000133A00001339ull},
	{"lb -16 [imm]", "lb", -16, nullptr, 0, true, false, 0x0000000000000023ull, 0x0000133A00001339ull},
	{"lb +0 [quad]", "lb", 0, nullptr, 0, false, false, 0xFFFFFFFFFFFFFF89ull, 0x0000133A00001339ull},
	{"lb +16 [quad]", "lb", 16, nullptr, 0, false, false, 0xFFFFFFFFFFFFFFBBull, 0x0000133A00001339ull},
	{"lb -16 [quad]", "lb", -16, nullptr, 0, false, false, 0x0000000000000023ull, 0x0000133A00001339ull},
	{"lb -> $0", "lb", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lbu +0 [imm]", "lbu", 0, nullptr, 0, true, false, 0x0000000000000089ull, 0x0000133A00001339ull},
	{"lbu +16 [imm]", "lbu", 16, nullptr, 0, true, false, 0x00000000000000BBull, 0x0000133A00001339ull},
	{"lbu -16 [imm]", "lbu", -16, nullptr, 0, true, false, 0x0000000000000023ull, 0x0000133A00001339ull},
	{"lbu +0 [quad]", "lbu", 0, nullptr, 0, false, false, 0x0000000000000089ull, 0x0000133A00001339ull},
	{"lbu +16 [quad]", "lbu", 16, nullptr, 0, false, false, 0x00000000000000BBull, 0x0000133A00001339ull},
	{"lbu -16 [quad]", "lbu", -16, nullptr, 0, false, false, 0x0000000000000023ull, 0x0000133A00001339ull},
	{"lbu -> $0", "lbu", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"ld +0 [imm]", "ld", 0, nullptr, 0, true, false, 0xABCDEF0123456789ull, 0x0000133A00001339ull},
	{"ld +16 [imm]", "ld", 16, nullptr, 0, true, false, 0xCCDDEEFF8899AABBull, 0x0000133A00001339ull},
	{"ld -16 [imm]", "ld", -16, nullptr, 0, true, false, 0x9ABCDEF045678123ull, 0x0000133A00001339ull},
	{"ld +0 [quad]", "ld", 0, nullptr, 0, false, false, 0xABCDEF0123456789ull, 0x0000133A00001339ull},
	{"ld +16 [quad]", "ld", 16, nullptr, 0, false, false, 0xCCDDEEFF8899AABBull, 0x0000133A00001339ull},
	{"ld -16 [quad]", "ld", -16, nullptr, 0, false, false, 0x9ABCDEF045678123ull, 0x0000133A00001339ull},
	{"ld -> $0", "ld", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"ldl +0 [imm]", "ldl", 0, nullptr, 0, true, false, 0x89FFFFFFABCD4321ull, 0x0000133A00001339ull},
	{"ldr +0 [imm]", "ldr", 0, nullptr, 0, true, false, 0xABCDEF0123456789ull, 0x0000133A00001339ull},
	{"ldl +1 [imm]", "ldl", 1, nullptr, 0, true, false, 0x6789FFFFABCD4321ull, 0x0000133A00001339ull},
	{"ldr +1 [imm]", "ldr", 1, nullptr, 0, true, false, 0xFFABCDEF01234567ull, 0x0000133A00001339ull},
	{"ldl +0/ldr +7 [imm]", "ldl", 0, "ldr", 7, true, false, 0x89FFFFFFABCD43ABull, 0x0000133A00001339ull},
	{"ldl +0 [quad]", "ldl", 0, nullptr, 0, false, false, 0x8900133800001337ull, 0x0000133A00001339ull},
	{"ldr +0 [quad]", "ldr", 0, nullptr, 0, false, false, 0xABCDEF0123456789ull, 0x0000133A00001339ull},
	{"ldl +1 [quad]", "ldl", 1, nullptr, 0, false, false, 0x6789133800001337ull, 0x0000133A00001339ull},
	{"ldr +1 [quad]", "ldr", 1, nullptr, 0, false, false, 0x00ABCDEF01234567ull, 0x0000133A00001339ull},
	{"ldl +0/ldr +7 [quad]", "ldl", 0, "ldr", 7, false, false, 0x89001338000013ABull, 0x0000133A00001339ull},
	{"ldl -> $0", "ldl", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"ldr -> $0", "ldr", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lh +0 [imm]", "lh", 0, nullptr, 0, true, false, 0x0000000000006789ull, 0x0000133A00001339ull},
	{"lh +16 [imm]", "lh", 16, nullptr, 0, true, false, 0xFFFFFFFFFFFFAABBull, 0x0000133A00001339ull},
	{"lh -16 [imm]", "lh", -16, nullptr, 0, true, false, 0xFFFFFFFFFFFF8123ull, 0x0000133A00001339ull},
	{"lh +0 [quad]", "lh", 0, nullptr, 0, false, false, 0x0000000000006789ull, 0x0000133A00001339ull},
	{"lh +16 [quad]", "lh", 16, nullptr, 0, false, false, 0xFFFFFFFFFFFFAABBull, 0x0000133A00001339ull},
	{"lh -16 [quad]", "lh", -16, nullptr, 0, false, false, 0xFFFFFFFFFFFF8123ull, 0x0000133A00001339ull},
	{"lh -> $0", "lh", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lhu +0 [imm]", "lhu", 0, nullptr, 0, true, false, 0x0000000000006789ull, 0x0000133A00001339ull},
	{"lhu +16 [imm]", "lhu", 16, nullptr, 0, true, false, 0x000000000000AABBull, 0x0000133A00001339ull},
	{"lhu -16 [imm]", "lhu", -16, nullptr, 0, true, false, 0x0000000000008123ull, 0x0000133A00001339ull},
	{"lhu +0 [quad]", "lhu", 0, nullptr, 0, false, false, 0x0000000000006789ull, 0x0000133A00001339ull},
	{"lhu +16 [quad]", "lhu", 16, nullptr, 0, false, false, 0x000000000000AABBull, 0x0000133A00001339ull},
	{"lhu -16 [quad]", "lhu", -16, nullptr, 0, false, false, 0x0000000000008123ull, 0x0000133A00001339ull},
	{"lhu -> $0", "lhu", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lw +0 [imm]", "lw", 0, nullptr, 0, true, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lw +16 [imm]", "lw", 16, nullptr, 0, true, false, 0xFFFFFFFF8899AABBull, 0x0000133A00001339ull},
	{"lw -16 [imm]", "lw", -16, nullptr, 0, true, false, 0x0000000045678123ull, 0x0000133A00001339ull},
	{"lw +0 [quad]", "lw", 0, nullptr, 0, false, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lw +16 [quad]", "lw", 16, nullptr, 0, false, false, 0xFFFFFFFF8899AABBull, 0x0000133A00001339ull},
	{"lw -16 [quad]", "lw", -16, nullptr, 0, false, false, 0x0000000045678123ull, 0x0000133A00001339ull},
	{"lw -> $0", "lw", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lwl +0 [imm]", "lwl", 0, nullptr, 0, true, false, 0xFFFFFFFF89CD4321ull, 0x0000133A00001339ull},
	{"lwr +0 [imm]", "lwr", 0, nullptr, 0, true, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lwl +1 [imm]", "lwl", 1, nullptr, 0, true, false, 0x0000000067894321ull, 0x0000133A00001339ull},
	{"lwr +1 [imm]", "lwr", 1, nullptr, 0, true, false, 0xFFFFFFFFAB234567ull, 0x0000133A00001339ull},
	{"lwl +0/lwr +3 [imm]", "lwl", 0, "lwr", 3, true, false, 0xFFFFFFFF89CD4323ull, 0x0000133A00001339ull},
	{"lwl +0 [quad]", "lwl", 0, nullptr, 0, false, false, 0xFFFFFFFF89001337ull, 0x0000133A00001339ull},
	{"lwr +0 [quad]", "lwr", 0, nullptr, 0, false, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lwl +1 [quad]", "lwl", 1, nullptr, 0, false, false, 0x0000000067891337ull, 0x0000133A00001339ull},
	{"lwr +1 [quad]", "lwr", 1, nullptr, 0, false, false, 0x0000133800234567ull, 0x0000133A00001339ull},
	{"lwl +0/lwr +3 [quad]", "lwl", 0, "lwr", 3, false, false, 0xFFFFFFFF89001323ull, 0x0000133A00001339ull},
	{"lwl -> $0", "lwl", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lwr -> $0", "lwr", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lwu +0 [imm]", "lwu", 0, nullptr, 0, true, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lwu +16 [imm]", "lwu", 16, nullptr, 0, true, false, 0x000000008899AABBull, 0x0000133A00001339ull},
	{"lwu -16 [imm]", "lwu", -16, nullptr, 0, true, false, 0x0000000045678123ull, 0x0000133A00001339ull},
	{"lwu +0 [quad]", "lwu", 0, nullptr, 0, false, false, 0x0000000023456789ull, 0x0000133A00001339ull},
	{"lwu +16 [quad]", "lwu", 16, nullptr, 0, false, false, 0x000000008899AABBull, 0x0000133A00001339ull},
	{"lwu -16 [quad]", "lwu", -16, nullptr, 0, false, false, 0x0000000045678123ull, 0x0000133A00001339ull},
	{"lwu -> $0", "lwu", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
	{"lq +0 [imm]", "lq", 0, nullptr, 0, true, false, 0xABCDEF0123456789ull, 0xC0DEC0DEBEEFDEADull},
	{"lq +16 [imm]", "lq", 16, nullptr, 0, true, false, 0xCCDDEEFF8899AABBull, 0x4455667700112233ull},
	{"lq -16 [imm]", "lq", -16, nullptr, 0, true, false, 0x9ABCDEF045678123ull, 0xC0DE1337DEADBEEFull},
	{"lq +11 [imm]", "lq", 11, nullptr, 0, true, false, 0xABCDEF0123456789ull, 0xC0DEC0DEBEEFDEADull},
	{"lq +0 [quad]", "lq", 0, nullptr, 0, false, false, 0xABCDEF0123456789ull, 0xC0DEC0DEBEEFDEADull},
	{"lq +16 [quad]", "lq", 16, nullptr, 0, false, false, 0xCCDDEEFF8899AABBull, 0x4455667700112233ull},
	{"lq -16 [quad]", "lq", -16, nullptr, 0, false, false, 0x9ABCDEF045678123ull, 0xC0DE1337DEADBEEFull},
	{"lq +11 [quad]", "lq", 11, nullptr, 0, false, false, 0xABCDEF0123456789ull, 0xC0DEC0DEBEEFDEADull},
	{"lq -> $0", "lq", 0, nullptr, 0, false, true, 0x0000000000000000ull, 0x0000133A00001339ull},
};

// `block` is the byte delta from the base register to the
// 16-byte block the capture printed.
struct StoreCase {
	const char* label; const char* op1; int off1;
	const char* op2; int off2;
	bool imm_preset; int block;
	u32 mem[4];                  // word0..word3 of that block
};

inline constexpr StoreCase kStoreCases[] = {
	{"sb +0 [imm]", "sb", 0, nullptr, 0, true, 0, {0x23456721u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sb +16 [imm]", "sb", 16, nullptr, 0, true, 16, {0x8899AA21u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sb -16 [imm]", "sb", -16, nullptr, 0, true, -16, {0x45678121u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sb +0 [quad]", "sb", 0, nullptr, 0, false, 0, {0x23456737u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sb +16 [quad]", "sb", 16, nullptr, 0, false, 16, {0x8899AA37u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sb -16 [quad]", "sb", -16, nullptr, 0, false, -16, {0x45678137u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sd +0 [imm]", "sd", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xFFFFFFFFu, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sd +16 [imm]", "sd", 16, nullptr, 0, true, 16, {0xABCD4321u, 0xFFFFFFFFu, 0x00112233u, 0x44556677u}},
	{"sd -16 [imm]", "sd", -16, nullptr, 0, true, -16, {0xABCD4321u, 0xFFFFFFFFu, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sd +0 [quad]", "sd", 0, nullptr, 0, false, 0, {0x00001337u, 0x00001338u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sd +16 [quad]", "sd", 16, nullptr, 0, false, 16, {0x00001337u, 0x00001338u, 0x00112233u, 0x44556677u}},
	{"sd -16 [quad]", "sd", -16, nullptr, 0, false, -16, {0x00001337u, 0x00001338u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sdl +0 [imm]", "sdl", 0, nullptr, 0, true, 0, {0x234567FFu, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdr +0 [imm]", "sdr", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xFFFFFFFFu, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdl +1 [imm]", "sdl", 1, nullptr, 0, true, 0, {0x2345FFFFu, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdr +1 [imm]", "sdr", 1, nullptr, 0, true, 0, {0xCD432189u, 0xFFFFFFABu, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdl +0/sdr +7 [imm]", "sdl", 0, "sdr", 7, true, 0, {0x234567FFu, 0x21CDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdl +0 [quad]", "sdl", 0, nullptr, 0, false, 0, {0x23456700u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdr +0 [quad]", "sdr", 0, nullptr, 0, false, 0, {0x00001337u, 0x00001338u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdl +1 [quad]", "sdl", 1, nullptr, 0, false, 0, {0x23450000u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdr +1 [quad]", "sdr", 1, nullptr, 0, false, 0, {0x00133789u, 0x00133800u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sdl +0/sdr +7 [quad]", "sdl", 0, "sdr", 7, false, 0, {0x23456700u, 0x37CDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sh +0 [imm]", "sh", 0, nullptr, 0, true, 0, {0x23454321u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sh +16 [imm]", "sh", 16, nullptr, 0, true, 16, {0x88994321u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sh -16 [imm]", "sh", -16, nullptr, 0, true, -16, {0x45674321u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sh +0 [quad]", "sh", 0, nullptr, 0, false, 0, {0x23451337u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sh +16 [quad]", "sh", 16, nullptr, 0, false, 16, {0x88991337u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sh -16 [quad]", "sh", -16, nullptr, 0, false, -16, {0x45671337u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sw +0 [imm]", "sw", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sw +16 [imm]", "sw", 16, nullptr, 0, true, 16, {0xABCD4321u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sw -16 [imm]", "sw", -16, nullptr, 0, true, -16, {0xABCD4321u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"sw +0 [quad]", "sw", 0, nullptr, 0, false, 0, {0x00001337u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sw +16 [quad]", "sw", 16, nullptr, 0, false, 16, {0x00001337u, 0xCCDDEEFFu, 0x00112233u, 0x44556677u}},
	{"sw -16 [quad]", "sw", -16, nullptr, 0, false, -16, {0x00001337u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u}},
	{"swl +0 [imm]", "swl", 0, nullptr, 0, true, 0, {0x234567ABu, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swr +0 [imm]", "swr", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swl +1 [imm]", "swl", 1, nullptr, 0, true, 0, {0x2345ABCDu, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swr +1 [imm]", "swr", 1, nullptr, 0, true, 0, {0xCD432189u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swl +0/swr +3 [imm]", "swl", 0, "swr", 3, true, 0, {0x214567ABu, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swl +0 [quad]", "swl", 0, nullptr, 0, false, 0, {0x23456700u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swr +0 [quad]", "swr", 0, nullptr, 0, false, 0, {0x00001337u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swl +1 [quad]", "swl", 1, nullptr, 0, false, 0, {0x23450000u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swr +1 [quad]", "swr", 1, nullptr, 0, false, 0, {0x00133789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"swl +0/swr +3 [quad]", "swl", 0, "swr", 3, false, 0, {0x37456700u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu}},
	{"sq +0 [imm]", "sq", 0, nullptr, 0, true, 0, {0xABCD4321u, 0xFFFFFFFFu, 0x00001339u, 0x0000133Au}},
	{"sq +16 [imm]", "sq", 16, nullptr, 0, true, 16, {0xABCD4321u, 0xFFFFFFFFu, 0x00001339u, 0x0000133Au}},
	{"sq -16 [imm]", "sq", -16, nullptr, 0, true, -16, {0xABCD4321u, 0xFFFFFFFFu, 0x00001339u, 0x0000133Au}},
	{"sq +11 [imm]", "sq", 11, nullptr, 0, true, 0, {0xABCD4321u, 0xFFFFFFFFu, 0x00001339u, 0x0000133Au}},
	{"sq +0 [quad]", "sq", 0, nullptr, 0, false, 0, {0x00001337u, 0x00001338u, 0x00001339u, 0x0000133Au}},
	{"sq +16 [quad]", "sq", 16, nullptr, 0, false, 16, {0x00001337u, 0x00001338u, 0x00001339u, 0x0000133Au}},
	{"sq -16 [quad]", "sq", -16, nullptr, 0, false, -16, {0x00001337u, 0x00001338u, 0x00001339u, 0x0000133Au}},
	{"sq +11 [quad]", "sq", 11, nullptr, 0, false, 0, {0x00001337u, 0x00001338u, 0x00001339u, 0x0000133Au}},
};

inline constexpr int kLoadCaseCount = 82;
inline constexpr int kStoreCaseCount = 52;

} // namespace ps2auto_eelsu
