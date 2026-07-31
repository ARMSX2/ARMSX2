// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE's divide/square-root unit is not correctly rounded, and this file is
// the measurement that says so.
//
// Provenance: two first-party captures on an SCPH-90000 over ps2link, 1075 and
// 1156 operand pairs, the rows below generated from the capture files rather
// than transcribed. Each pair is run as sqrt.s Ft, rsqrt.s Fs, Ft and div.s
// Fs, S where S is the sqrt.s Ft the same silicon had just produced; the
// second probe adds div.s Fs, Ft straight off the pair, which is the con_div
// column below. FCR31 is cleared before every op and every result is read
// twice. The first probe re-runs byte-identical, 15/15 corpus controls
// reproduce, and the probe's sqrt.s column matches the corpus's own on all 80
// shared operands.
//
// 1. RSQRT.S is sqrt-then-divide, exactly. rsqrt.s Fs, Ft was bit-identical to
//    div.s Fs, S on 1075/1075 and 1156/1156 rows: no fused
//    reciprocal-square-root, and no extra precision carried between the two
//    steps -- the intermediate is a plain 24-bit single. That is what RSQRT_S
//    in FPU.cpp does (eeDivide of eeSqrtBits), and RsqrtIsSqrtThenDivide below
//    is what keeps it that way: an arm64 fast path tempted by FRSQRTE would
//    fail it.
//
// 2. Neither step is correctly rounded. The reference was computed two
//    independent ways -- an exact integer model over dyadic rationals, and the
//    host FPU's own single-precision sqrt and divide -- which agreed with each
//    other on all 1759 rows where both operands and result are ordinary
//    singles. Against that reference, silicon comes out one ULP away on a
//    large minority of arbitrary operands, capture 1 / capture 2:
//
//        sqrt.s     196 / 1075  and  290 / 1156 rows one ULP low, none high
//        div.s      212 low + 22 high / 1075,  153 low + 36 high / 1156
//        rsqrt.s    345 / 1075 and 341 / 1156, and 47 of those are two ULPs
//                   out because a low root makes the quotient high
//
//    Both captures are deliberately enriched for rows that can show the error;
//    the uniform-random figure is the 120 unbiased pairs inside capture 1,
//    where sqrt.s misses on 33, div.s on 13 and rsqrt.s on 35.
//
//    The error is deterministic, but it is not a rounding mode and not a
//    function of either operand alone: with the divisor held fixed and the
//    numerator swept so the exact quotient walks across its ULP, the rounding
//    boundary interleaves on 22 of 24 divisors, and on all 12 numerators with
//    the roles swapped. A reciprocal-then-multiply model scores 69% against
//    correctly-rounded's 84%.
//
// So this tree computes the correctly-rounded result, the closest simple model
// of the unit there is; the rows below pin where it lands on silicon and where
// it does not.
//
// This is also the answer to corpus case 220, rsqrt EEMAX, EEMAX -- the last
// result-axis row where the interpreter misses the console. It is one sample
// of the divide unit's approximation and the only row in all 1147 corpus cases
// that can see it: every other div/sqrt case there has an exact result below
// the halfway point, where nearest and truncation agree, which is why eight
// DIV.S rows read as round-to-nearest while this one reads as truncation.
//
// A model of the unit -- the way eeMulRound/eeMulDefectiveFt already reproduce
// the multiplier's deficit -- would collapse SiliconIsOneUlpOffInTheseExactWays
// and InterpMatchesConsoleWhereTheUnitIsExact into one conformance test and
// have to pass the tripwire at the bottom. Plan and acceptance criteria:
// WORKORDER-divsqrt-model.md in the notes tree.
//
// Evidence archive: captures/fpmatrix/divsqrt/ in the notes tree -- rsqprobe.c,
// hw-rsq-run{1,2,3}.bin and the scoring scripts -- with
// PROBE-divsqrt-rounding.md beside it.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <iterator>

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 5, kFt = 6, kFd = 7, kFtmp = 8;

// One console row. `con_*` is what the SCPH-90000 returned; `ieee_*` is the
// correctly-rounded value, which is what this tree computes. They differ on
// exactly the rows where the hardware unit's approximation shows.
struct ConsoleRow
{
	u32 fs, ft;
	u32 con_sqrt, con_div, con_rsqrt;
	u32 ieee_sqrt, ieee_div, ieee_rsqrt;
	bool jit_agrees;  // false when the row touches the top binade, where the
	                  // arm64 fast path's operand clamp and result saturation
	                  // legitimately part company with the interpreter
	const char* what;
};

constexpr ConsoleRow kRows[] = {
	{ 0x3F800000u, 0x3F800000u,  0x3F800000u, 0x3F800000u, 0x3F800000u,  0x3F800000u, 0x3F800000u, 0x3F800000u,  true , "control" },
	{ 0x7FFFFFFFu, 0x7FFFFFFFu,  0x5FB504F3u, 0x3F800000u, 0x5FB504F2u,  0x5FB504F3u, 0x3F800000u, 0x5FB504F3u,  false, "control" },
	{ 0x3F800000u, 0xBF800000u,  0x3F800000u, 0xBF800000u, 0x3F800000u,  0x3F800000u, 0xBF800000u, 0x3F800000u,  true , "control" },
	{ 0x7F7FFFFFu, 0x7F7FFFFFu,  0x5F7FFFFFu, 0x3F800000u, 0x5F800000u,  0x5F7FFFFFu, 0x3F800000u, 0x5F800000u,  true , "control" },
	{ 0xBF800000u, 0x40800000u,  0x40000000u, 0xBE800000u, 0xBF000000u,  0x40000000u, 0xBE800000u, 0xBF000000u,  true , "control" },
	{ 0x3F800000u, 0x40800000u,  0x40000000u, 0x3E800000u, 0x3F000000u,  0x40000000u, 0x3E800000u, 0x3F000000u,  true , "control" },
	{ 0xBF800000u, 0xBF800000u,  0x3F800000u, 0x3F800000u, 0xBF800000u,  0x3F800000u, 0x3F800000u, 0xBF800000u,  true , "control" },
	{ 0xDEADBEEFu, 0x3F800000u,  0x3F800000u, 0xDEADBEEFu, 0xDEADBEEFu,  0x3F800000u, 0xDEADBEEFu, 0xDEADBEEFu,  true , "control" },
	{ 0x3F800000u, 0x00800000u,  0x20000000u, 0x7E800000u, 0x5F000000u,  0x20000000u, 0x7E800000u, 0x5F000000u,  true , "control" },
	{ 0x3F800000u, 0x7F800000u,  0x5F800000u, 0x00000000u, 0x1F800000u,  0x5F800000u, 0x00000000u, 0x1F800000u,  false, "control" },
	{ 0x3F800000u, 0x40400000u,  0x3FDDB3D7u, 0x3EAAAAABu, 0x3F13CD3Au,  0x3FDDB3D7u, 0x3EAAAAABu, 0x3F13CD3Au,  true , "control" },
	{ 0x7F800000u, 0x7F800000u,  0x5F800000u, 0x3F800000u, 0x5F800000u,  0x5F800000u, 0x3F800000u, 0x5F800000u,  false, "control" },
	{ 0x3F800000u, 0x40000000u,  0x3FB504F3u, 0x3F000000u, 0x3F3504F3u,  0x3FB504F3u, 0x3F000000u, 0x3F3504F3u,  true , "control" },
	{ 0x3F800000u, 0x3FC00000u,  0x3F9CC471u, 0x3F2AAAABu, 0x3F5105EBu,  0x3F9CC471u, 0x3F2AAAABu, 0x3F5105EBu,  true , "control" },
	{ 0x7F7FFFFFu, 0x00800000u,  0x20000000u, 0x7FFFFFFFu, 0x7FFFFFFFu,  0x20000000u, 0x7FFFFFFFu, 0x7FFFFFFFu,  false, "control" },
	{ 0x4758D617u, 0x45DAB6CDu,  0x42A75179u, 0x40FDCD57u, 0x4425E1BAu,  0x42A7517Au, 0x40FDCD57u, 0x4425E1BAu,  true , "sqrt off by one" },
	{ 0x3895AEC3u, 0x4938608Bu,  0x445941C1u, 0x2ECFD404u, 0x33B06019u,  0x445941C2u, 0x2ECFD404u, 0x33B06019u,  true , "sqrt off by one" },
	{ 0x3F144D9Au, 0x46601FEDu,  0x42EF8860u, 0x3829651Du, 0x3B9E7FA5u,  0x42EF8861u, 0x3829651Eu, 0x3B9E7FA4u,  true , "sqrt off by one" },
	{ 0x482A9322u, 0x3CFE8F78u,  0x3E348278u, 0x4AAB8A13u, 0x4971E906u,  0x3E348279u, 0x4AAB8A14u, 0x4971E905u,  true , "sqrt off by one" },
	{ 0x401BB0A2u, 0x365AF7C1u,  0x3AECC2CEu, 0x49360541u, 0x44A8575Bu,  0x3AECC2CFu, 0x49360541u, 0x44A8575Bu,  true , "sqrt off by one" },
	{ 0x494F69CEu, 0x35CFF4D5u,  0x3AA326C9u, 0x52FF54DBu, 0x4E22B9B7u,  0x3AA326CAu, 0x52FF54DBu, 0x4E22B9B6u,  true , "sqrt off by one" },
	{ 0x363F2184u, 0x44403A4Du,  0x41DDD57Du, 0x317E8A10u, 0x33DC9177u,  0x41DDD57Eu, 0x317E8A10u, 0x33DC9175u,  true , "sqrt off by one" },
	{ 0x3C733289u, 0x365AF7C1u,  0x3AECC2CEu, 0x458E29E7u, 0x41037AD0u,  0x3AECC2CFu, 0x458E29E7u, 0x41037ACFu,  true , "sqrt off by one" },
	{ 0x33D5E248u, 0x45DAB6CDu,  0x42A75179u, 0x2D7A58AEu, 0x30A39F87u,  0x42A7517Au, 0x2D7A58AEu, 0x30A39F86u,  true , "sqrt off by one" },
	{ 0x4A807DE6u, 0x3F15F9ACu,  0x3F43F16Au, 0x4ADB542Cu, 0x4AA7DFF4u,  0x3F43F16Bu, 0x4ADB542Cu, 0x4AA7DFF3u,  true , "sqrt off by one" },
	{ 0x371D8746u, 0x45DAB6CDu,  0x42A75179u, 0x30B86230u, 0x33F10578u,  0x42A7517Au, 0x30B86230u, 0x33F10577u,  true , "sqrt off by one" },
	{ 0x3245D98Cu, 0x37645EBBu,  0x3B71CA69u, 0x3A5DC985u, 0x36517A13u,  0x3B71CA6Au, 0x3A5DC984u, 0x36517A12u,  true , "sqrt off by one" },
	{ 0x42C654F9u, 0x3C908E7Bu,  0x3E0806D0u, 0x45AF9DC4u, 0x443AA0FAu,  0x3E0806D0u, 0x45AF9DC5u, 0x443AA0FBu,  true , "div low" },
	{ 0x3DD20934u, 0x48374241u,  0x43D898D4u, 0x3512B3F2u, 0x39783ED2u,  0x43D898D4u, 0x3512B3F3u, 0x39783ED2u,  true , "div low" },
	{ 0x494F69CEu, 0x34A817F6u,  0x3A12AEEAu, 0x541DF0F1u, 0x4EB4FEA9u,  0x3A12AEEBu, 0x541DF0F2u, 0x4EB4FEA8u,  true , "div low" },
	{ 0x358B02CFu, 0x45DAB6CDu,  0x42A75179u, 0x2F22B593u, 0x3254B079u,  0x42A7517Au, 0x2F22B594u, 0x3254B077u,  true , "div low" },
	{ 0x41C0AF79u, 0x41A02D93u,  0x408F301Du, 0x3F99FA1Eu, 0x40AC3F4Fu,  0x408F301Du, 0x3F99FA1Fu, 0x40AC3F4Fu,  true , "div low" },
	{ 0x32CCA53Eu, 0x419FDD7Fu,  0x408F0C4Eu, 0x30A3DABAu, 0x31B71E1Cu,  0x408F0C4Eu, 0x30A3DABBu, 0x31B71E1Cu,  true , "div low" },
	{ 0x43C1DAF0u, 0x393F5153u,  0x3C5D4EE7u, 0x4A01B29Eu, 0x46E03E59u,  0x3C5D4EE7u, 0x4A01B29Fu, 0x46E03E59u,  true , "div low" },
	{ 0x3DBFA5A1u, 0x4B6B8E1Du,  0x4575909Cu, 0x31D047E7u, 0x37C7CA79u,  0x4575909Cu, 0x31D047E8u, 0x37C7CA79u,  true , "div low" },
	{ 0x3FC5664Au, 0x393F5153u,  0x3C5D4EE7u, 0x460411ADu, 0x42E457EFu,  0x3C5D4EE7u, 0x460411AEu, 0x42E457EFu,  true , "div low" },
	{ 0x3F54A9E1u, 0x43539E3Au,  0x4168C0EAu, 0x3B80A1E4u, 0x3D69E74Du,  0x4168C0EAu, 0x3B80A1E5u, 0x3D69E74Du,  true , "div low" },
	{ 0x3A26C7D7u, 0x46016B05u,  0x42B604F0u, 0x33A4F404u, 0x36EA9153u,  0x42B604F0u, 0x33A4F405u, 0x36EA9153u,  true , "div low" },
	{ 0x482A9322u, 0x3E97A495u,  0x3F0B5221u, 0x490FFAEAu, 0x489CB6DDu,  0x3F0B5221u, 0x490FFAEBu, 0x489CB6DDu,  true , "div low" },
	{ 0x44933C6Bu, 0x3ECD12D0u,  0x3F220445u, 0x4537CCB1u, 0x44E8A532u,  0x3F220445u, 0x4537CCB0u, 0x44E8A532u,  true , "div high" },
	{ 0x45246870u, 0x48EE62F7u,  0x442EAE5Cu, 0x3BB08E2Fu, 0x4070F1C7u,  0x442EAE5Cu, 0x3BB08E2Eu, 0x4070F1C7u,  true , "div high" },
	{ 0x43CD0CEBu, 0x365AF7C1u,  0x3AECC2CEu, 0x4CEFBA9Du, 0x485DB675u,  0x3AECC2CFu, 0x4CEFBA9Cu, 0x485DB675u,  true , "div high" },
	{ 0x3F354F0Au, 0x393F5153u,  0x3C5D4EE7u, 0x45729B70u, 0x4251BAF5u,  0x3C5D4EE7u, 0x45729B6Fu, 0x4251BAF5u,  true , "div high" },
	{ 0x43E217A7u, 0x37645EBBu,  0x3B71CA69u, 0x4BFD7261u, 0x47EF6112u,  0x3B71CA6Au, 0x4BFD7260u, 0x47EF6111u,  true , "div high" },
	{ 0x44265C16u, 0x4B6B8E1Du,  0x4575909Cu, 0x3834CC7Fu, 0x3E2D6DD7u,  0x4575909Cu, 0x3834CC7Eu, 0x3E2D6DD7u,  true , "div high" },
	{ 0x343DA5A8u, 0x44A43E1Du,  0x4210FE49u, 0x2F13CC70u, 0x31A76B9Bu,  0x4210FE49u, 0x2F13CC70u, 0x31A76B9Cu,  true , "rsqrt off by one" },
	{ 0x3F4E233Au, 0x44403A4Du,  0x41DDD57Du, 0x3A894323u, 0x3CEDE2DCu,  0x41DDD57Eu, 0x3A894323u, 0x3CEDE2DBu,  true , "rsqrt off by one" },
	{ 0x36ABDED5u, 0x44F097D7u,  0x422F7CD7u, 0x3136E063u, 0x33FAB925u,  0x422F7CD7u, 0x3136E063u, 0x33FAB926u,  true , "rsqrt off by one" },
	{ 0x494F69CEu, 0x44A7E7DEu,  0x421299EDu, 0x441E1E2Fu, 0x46B51892u,  0x421299EDu, 0x441E1E2Fu, 0x46B51893u,  true , "rsqrt off by one" },
	{ 0x39AB2EADu, 0x4938608Bu,  0x445941C1u, 0x2FEDADF9u, 0x34C9B585u,  0x445941C2u, 0x2FEDADF9u, 0x34C9B584u,  true , "rsqrt off by one" },
	{ 0x482A9322u, 0x412AC876u,  0x40511829u, 0x467FB010u, 0x4750D6E0u,  0x40511829u, 0x467FB010u, 0x4750D6DFu,  true , "rsqrt off by one" },
	{ 0x3B3DB89Eu, 0x46601FEDu,  0x42EF8860u, 0x3458B41Bu, 0x37CAC397u,  0x42EF8861u, 0x3458B41Bu, 0x37CAC396u,  true , "rsqrt off by one" },
	{ 0x391BB34Du, 0x44F097D7u,  0x422F7CD7u, 0x33A5ABC6u, 0x3663226Eu,  0x422F7CD7u, 0x33A5ABC6u, 0x3663226Fu,  true , "rsqrt off by one" },
	{ 0x4AFA2876u, 0x395648F4u,  0x3C6A3732u, 0x51156DA2u, 0x4E08B66Eu,  0x3C6A3732u, 0x51156DA2u, 0x4E08B66Fu,  true , "rsqrt off by one" },
	{ 0x3F54A9E1u, 0x42323762u,  0x40D598A7u, 0x3C98BDB1u, 0x3DFEE1D3u,  0x40D598A8u, 0x3C98BDB1u, 0x3DFEE1D2u,  true , "rsqrt off by one" },
	{ 0x3941F084u, 0x44403A4Du,  0x41DDD57Du, 0x348123CCu, 0x36DFCF33u,  0x41DDD57Eu, 0x348123CCu, 0x36DFCF32u,  true , "rsqrt off by one" },
	{ 0x44933C6Bu, 0x4AE21B47u,  0x452A1F57u, 0x3926B3B7u, 0x3EDD8F81u,  0x452A1F57u, 0x3926B3B7u, 0x3EDD8F80u,  true , "rsqrt off by one" },
	{ 0x46DBCF4Au, 0x3B5E753Du,  0x3D6EA3F1u, 0x4AFCF3D5u, 0x48EBCCADu,  0x3D6EA3F1u, 0x4AFCF3D5u, 0x48EBCCADu,  true , "all three exact" },
	{ 0x4AF4F195u, 0x35B110EBu,  0x3A968C15u, 0x54B1117Cu, 0x4FD04246u,  0x3A968C15u, 0x54B1117Cu, 0x4FD04246u,  true , "all three exact" },
	{ 0x391E63DCu, 0x4B6B8E1Du,  0x4575909Cu, 0x2D2C2330u, 0x33251EEAu,  0x4575909Cu, 0x2D2C2330u, 0x33251EEAu,  true , "all three exact" },
	{ 0x40561BEDu, 0x37A2524Eu,  0x3B902490u, 0x4828D669u, 0x443E2170u,  0x3B902490u, 0x4828D669u, 0x443E2170u,  true , "all three exact" },
	{ 0x4A75A02Cu, 0x46016B05u,  0x42B604F0u, 0x43F2EF30u, 0x472CBABBu,  0x42B604F0u, 0x43F2EF30u, 0x472CBABBu,  true , "all three exact" },
	{ 0x329CD2FBu, 0x41A02D93u,  0x408F301Du, 0x307AA3C7u, 0x318C3097u,  0x408F301Du, 0x307AA3C7u, 0x318C3097u,  true , "all three exact" },
	{ 0x44933C6Bu, 0x39B01B0Bu,  0x3C96236Au, 0x4A560873u, 0x477B0D1Fu,  0x3C96236Au, 0x4A560873u, 0x477B0D1Fu,  true , "all three exact" },
	{ 0x3642F159u, 0x4A6EA2F1u,  0x44F72A73u, 0x2B512087u, 0x30C9E910u,  0x44F72A73u, 0x2B512087u, 0x30C9E910u,  true , "all three exact" },
	{ 0x42C654F9u, 0x45D9AC31u,  0x42A6EB60u, 0x3C6940FBu, 0x3F981698u,  0x42A6EB60u, 0x3C6940FBu, 0x3F981698u,  true , "all three exact" },
	{ 0x4A807DE6u, 0x4155DE98u,  0x4069FD0Bu, 0x4899CDB8u, 0x498C9443u,  0x4069FD0Bu, 0x4899CDB8u, 0x498C9443u,  true , "all three exact" },
	{ 0x482A9322u, 0x32D58CCDu,  0x392554C8u, 0x54CC7B65u, 0x4E840F41u,  0x392554C8u, 0x54CC7B65u, 0x4E840F41u,  true , "all three exact" },
	{ 0x494F69CEu, 0x3C4DB009u,  0x3DE57812u, 0x4C8112EAu, 0x4AE764EBu,  0x3DE57812u, 0x4C8112EAu, 0x4AE764EBu,  true , "all three exact" },
	{ 0x44933C6Bu, 0x37402F6Au,  0x3B5DCF35u, 0x4CC42020u, 0x48A9EE7Bu,  0x3B5DCF35u, 0x4CC42020u, 0x48A9EE7Bu,  true , "all three exact" },
	{ 0x482A9322u, 0x43F65004u,  0x41B18FB0u, 0x43B1488Eu, 0x45F5ED57u,  0x41B18FB0u, 0x43B1488Eu, 0x45F5ED57u,  true , "all three exact" },
	{ 0x3F543660u, 0x419FDD7Fu,  0x408F0C4Eu, 0x3D29E9BEu, 0x3E3DE377u,  0x408F0C4Eu, 0x3D29E9BEu, 0x3E3DE377u,  true , "all three exact" },
	{ 0x4A807DE6u, 0x3EFF7AF4u,  0x3F34D5E3u, 0x4B00C0D0u, 0x4AB5E64Au,  0x3F34D5E3u, 0x4B00C0D0u, 0x4AB5E64Au,  true , "all three exact" },
	{ 0x343DA5A8u, 0x336A599Cu,  0x3974EF99u, 0x404F2AD2u, 0x3A4636B5u,  0x3974EF99u, 0x404F2AD2u, 0x3A4636B5u,  true , "all three exact" },
	{ 0x343DA5A8u, 0x3C758CA3u,  0x3DFAB861u, 0x3745B7F1u, 0x35C1A409u,  0x3DFAB861u, 0x3745B7F1u, 0x35C1A409u,  true , "all three exact" },
	{ 0x3FA2334Bu, 0x4574B6DDu,  0x427A4B26u, 0x39A9AE42u, 0x3CA5E5FCu,  0x427A4B26u, 0x39A9AE42u, 0x3CA5E5FCu,  true , "all three exact" },
	{ 0x3F54A9E1u, 0x471080FBu,  0x434055E9u, 0x37BC600Eu, 0x3B8D8742u,  0x434055E9u, 0x37BC600Eu, 0x3B8D8742u,  true , "all three exact" },
	{ 0x49729641u, 0x3B5E753Du,  0x3D6EA3F1u, 0x4D8B94FEu, 0x4B821DE7u,  0x3D6EA3F1u, 0x4D8B94FEu, 0x4B821DE7u,  true , "all three exact" },
	{ 0x47AF5171u, 0x3B5E753Du,  0x3D6EA3F1u, 0x4BC9C0A1u, 0x49BC1249u,  0x3D6EA3F1u, 0x4BC9C0A1u, 0x49BC1249u,  true , "all three exact" },
	{ 0x366BA9C1u, 0x393F5153u,  0x3C5D4EE7u, 0x3C9DAB47u, 0x39884D75u,  0x3C5D4EE7u, 0x3C9DAB47u, 0x39884D75u,  true , "all three exact" },
	{ 0x393CDA39u, 0x41A02D93u,  0x408F301Du, 0x3716E9FEu, 0x3828D224u,  0x408F301Du, 0x3716E9FEu, 0x3828D224u,  true , "all three exact" },
	{ 0x48707798u, 0x3B5E753Du,  0x3D6EA3F1u, 0x4C8A5CC0u, 0x4A80FAD6u,  0x3D6EA3F1u, 0x4C8A5CC0u, 0x4A80FAD6u,  true , "all three exact" },
	{ 0x45246870u, 0x378F4465u,  0x3B876B29u, 0x4D12E350u, 0x491B66B8u,  0x3B876B29u, 0x4D12E350u, 0x491B66B8u,  true , "all three exact" },
	{ 0x371ABDBAu, 0x42DB98E1u,  0x4127A7DDu, 0x33B46487u, 0x356C47BBu,  0x4127A7DDu, 0x33B46487u, 0x356C47BBu,  true , "all three exact" },
	{ 0x42C654F9u, 0x3FCCFB0Bu,  0x3FA1FAE1u, 0x4277B249u, 0x429CB9DFu,  0x3FA1FAE1u, 0x4277B249u, 0x429CB9DFu,  true , "all three exact" },
	{ 0x4ACE4068u, 0x37CD7383u,  0x3BA22A74u, 0x52807FA7u, 0x4EA2CC2Du,  0x3BA22A74u, 0x52807FA7u, 0x4EA2CC2Du,  true , "all three exact" },
	{ 0x35119D30u, 0x46016B05u,  0x42B604F0u, 0x2E9004BDu, 0x31CCCC4Au,  0x42B604F0u, 0x2E9004BDu, 0x31CCCC4Au,  true , "all three exact" },
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));

enum Op { OP_SQRT, OP_DIV, OP_RSQRT };

const char* OpName(Op op)
{
	return op == OP_SQRT ? "sqrt.s" : op == OP_DIV ? "div.s" : "rsqrt.s";
}

u32 Encode(Op op)
{
	switch (op)
	{
		case OP_SQRT: return SQRT_S(kFd, kFt);
		case OP_DIV:  return DIV_S(kFd, kFs, kFt);
		default:      return RSQRT_S(kFd, kFs, kFt);
	}
}

u32 Con(const ConsoleRow& r, Op op)
{
	return op == OP_SQRT ? r.con_sqrt : op == OP_DIV ? r.con_div : r.con_rsqrt;
}

u32 Ieee(const ConsoleRow& r, Op op)
{
	return op == OP_SQRT ? r.ieee_sqrt : op == OP_DIV ? r.ieee_div : r.ieee_rsqrt;
}

// The interpreter is the subject here: it is the accuracy engine.
// RunInterpOnly() rather than Run() because a handful of rows sit in the top
// binade, where the fast path's clamp diverges on purpose --
// JitAgreesWithTheInterpreterOffTheTopBinade covers the rest.
u32 RunInterp(const ConsoleRow& r, Op op)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetFprBits(kFs, r.fs);
	h.SetFprBits(kFt, r.ft);
	h.LoadProgram({ Encode(op) });
	h.RunInterpOnly();
	return h.GetFprBitsInterp(kFd);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The decomposition, asserted in the engine: RSQRT.S is SQRT.S then DIV.S,
//    with a plain 24-bit single in between, as it is on silicon.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitConsole, RsqrtIsSqrtThenDivide)
{
	ASSERT_EQ(kRowCount, 87) << "row table truncated";

	for (const ConsoleRow& r : kRows)
	{
		const u32 s = RunInterp(r, OP_SQRT);

		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(kFs, r.fs);
		h.SetFprBits(kFtmp, s);
		h.LoadProgram({ DIV_S(kFd, kFs, kFtmp) });
		h.RunInterpOnly();
		const u32 two_step = h.GetFprBitsInterp(kFd);

		EXPECT_EQ(RunInterp(r, OP_RSQRT), two_step)
			<< "rsqrt.s must be div.s(Fs, sqrt.s(Ft)) and nothing cleverer -- "
			<< "silicon is, on every row measured. fs=" << std::hex << r.fs
			<< " ft=" << r.ft << " sqrt=" << s;
	}
}

// ---------------------------------------------------------------------------
// 2. Where the hardware unit happens to be exact, this tree must reproduce it.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitConsole, InterpMatchesConsoleWhereTheUnitIsExact)
{
	int checked = 0;
	for (const ConsoleRow& r : kRows)
	{
		for (Op op : { OP_SQRT, OP_DIV, OP_RSQRT })
		{
			if (Con(r, op) != Ieee(r, op))
				continue;
			++checked;
			EXPECT_EQ(RunInterp(r, op), Con(r, op))
				<< OpName(op) << " fs=" << std::hex << r.fs << " ft=" << r.ft
				<< " (" << r.what << ")";
		}
	}
	EXPECT_EQ(checked, 193) << "the exact-row population moved; re-derive it "
							   "from the capture rather than editing this number";
}

// ---------------------------------------------------------------------------
// 3. Where it is not, the miss is one ULP per rounding step and this tree sits
//    on the correctly-rounded side of it. Pinned so that any future model of
//    the unit has to come here and say so.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitConsole, SiliconIsOneUlpOffInTheseExactWays)
{
	int off_sqrt = 0, off_div = 0, off_rsqrt = 0, two_ulp = 0;
	for (const ConsoleRow& r : kRows)
	{
		for (Op op : { OP_SQRT, OP_DIV, OP_RSQRT })
		{
			const u32 con = Con(r, op), ieee = Ieee(r, op);
			if (con == ieee)
				continue;
			(op == OP_SQRT ? off_sqrt : op == OP_DIV ? off_div : off_rsqrt)++;

			// One ULP per rounding step, and the sign is never in question.
			// RSQRT.S gets two steps, so a low root and the high quotient it
			// produces compound to two.
			const u32 dc = con & 0x7FFFFFFFu, di = ieee & 0x7FFFFFFFu;
			const u32 ulps = dc > di ? dc - di : di - dc;
			EXPECT_EQ(con & 0x80000000u, ieee & 0x80000000u);
			EXPECT_LE(ulps, op == OP_RSQRT ? 2u : 1u)
				<< OpName(op) << " console " << std::hex << con << " vs "
				<< "correctly-rounded " << ieee;
			EXPECT_GE(ulps, 1u);
			if (ulps == 2)
				++two_ulp;

			EXPECT_EQ(RunInterp(r, op), ieee)
				<< OpName(op) << " fs=" << std::hex << r.fs << " ft=" << r.ft
				<< ": this tree computes the correctly-rounded value";
			EXPECT_NE(RunInterp(r, op), con)
				<< OpName(op) << " reached the console value -- if that is a "
				<< "deliberate new model of the divide unit, this file is the "
				<< "place to say so and the tripwire below should be enabled";
		}
	}
	EXPECT_EQ(off_sqrt, 21);
	EXPECT_EQ(off_div, 21);
	EXPECT_EQ(off_rsqrt, 26);
	EXPECT_EQ(two_ulp, 2) << "the compounded-error rows are part of what this "
							  "file documents; do not quietly lose them";
}

// ---------------------------------------------------------------------------
// 4. The fast path is the same arithmetic off the top binade, so it must not
//    drift from the interpreter there.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitConsole, JitAgreesWithTheInterpreterOffTheTopBinade)
{
	int checked = 0;
	for (const ConsoleRow& r : kRows)
	{
		if (!r.jit_agrees)
			continue;
		for (Op op : { OP_SQRT, OP_DIV, OP_RSQRT })
		{
			++checked;
			EeRecTestHarness h;
			h.EnableCop1();
			h.SetFprBits(kFs, r.fs);
			h.SetFprBits(kFt, r.ft);
			h.LoadProgram({ Encode(op) });
			h.Run();  // auto-diffs the two engines
			EXPECT_EQ(h.GetFprBitsJit(kFd), Ieee(r, op))
				<< OpName(op) << " fs=" << std::hex << r.fs << " ft=" << r.ft;
		}
	}
	EXPECT_EQ(checked, 249);
}

// ---------------------------------------------------------------------------
// 5. The tripwire. Enabling this means someone has modelled the EE's divide/
//    square-root unit rather than computing the correctly-rounded answer.
//    Nothing in the tree does today and nothing upstream does either -- both
//    recompilers inherit the host's correctly-rounded fdiv/fsqrt.
// ---------------------------------------------------------------------------
TEST(EeFpuDivUnitConsole, DISABLED_InterpMatchesConsoleOnEveryRow)
{
	for (const ConsoleRow& r : kRows)
	{
		for (Op op : { OP_SQRT, OP_DIV, OP_RSQRT })
		{
			EXPECT_EQ(RunInterp(r, op), Con(r, op))
				<< OpName(op) << " fs=" << std::hex << r.fs << " ft=" << r.ft
				<< " (" << r.what << ")";
		}
	}
}
