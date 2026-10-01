// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "arm64/FPU-divunit-arm64.h"

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP1 {

#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
/*	The same recurrence with nothing on the digit-to-digit path but the
	carry-save add and one compare. The loop is bound by the integer pipes,
	not by its chain -- every form of it issues three to four instructions a
	cycle -- so what a digit costs is how many instructions it takes, and this
	form has fewer because it computes neither the estimate, nor a second
	compare, nor the six-way select, nor the quotient.

	What the selector compares is V - delta, V the true partial remainder and
	delta = low24(sum & carry) the carry the partial assimilation leaves
	pending. V is not carried: it is rho_i + k*D with rho_i = 4*((2^i ma) mod
	mb), which the bits of the truncated quotient walk off the critical path,
	and k either 0 (the quotient so far is the truncation's) or -1 (one
	ahead). Each step is then one compare of delta against a threshold that
	depends on i and k:

	    k =  0:  digit +1 iff delta <= rho - 2^23,      else 0
	    k = -1:  digit  0 iff delta <= rho - D + 2^24,  else -1

	The flag c of that compare is "the upper of the two digits this state can
	produce" in both states, so d = c + k and k' = k + h_i - c, h_i =
	[2 rho_i >= D] being the quotient bit. The two operands of every select the
	flag drives are prepared from k before the flag arrives.

	The words are kept shifted left by 8: the register width discards bits
	24..31, which nothing below bit 23 reads, and sum & carry is then
	delta * 256 with no masking. The thresholds are theta * 256 + 255 as
	signed 64-bit values against the zero-extended word, so a threshold below
	0 or at 2^24 and above needs no clamping. A zero digit selects from the
	un-recompressed pair, whose sum & carry is the g of the step, which is
	also its carry word.

	No quotient is accumulated: the answer is T or T + 1 and the last state
	says which. It is assembly because the compiler folds the carry shift back
	into the chain and materialises the flag. */
u32 eeDivideSignificandArm64(u32 sma, u32 smb, u64 T2, u32 T)
{
	const u32 lt = sma < smb;
	const u32 D = smb << 2;
	const u32 Dp = D << 8;                   // addend for digit -1
	const u32 Dm = ~Dp & 0xFFFFFF00u;        // addend for digit +1: ~D, padding clear
	const s64 c1 = ((s64)(1 << 24) - (s64)D) * 256 + 255; // theta8(-1) - rho*256
	const s64 dth = ((s64)D - (s64)3 * (1 << 23)) * 256;  // theta8(0) - theta8(-1)
	// The chain's registers: ad the addend, cc the carry word with the +1 in,
	// ns and nc the pair before the shift, th the threshold. Off it: nr is
	// -r_i, base is theta8_i(-1), and k lives in k0 and k1 by turns so the
	// zero-digit test can read the old one after the new one is written.
	u64 k0 = lt ? ~0ull : 0ull, k1 = 0;
	s64 nr = -(s64)(lt ? sma : sma - smb);   // -r_0, r_i = (2^i ma) mod mb
	s64 base = c1 - (nr << 10);
	s64 th = base + (lt ? 0 : dth);
	u32 ad = Dm, cc = 256, ns = sma << 9, nc = 0, c;
	u64 tA, tB, tC, tD, tE, tF;
	const u64 T2inv = ~T2;
	// One digit. `bit` is the quotient bit this step's h comes from, `ko` the
	// register holding k on entry and `kn` the one it leaves k in.
#define EE_DIVUNIT_STEP(bit, ko, kn) \
	/* the next step's operands, all from k and h before the flag arrives */ \
	"sbfx %x[tA], %x[T2inv], #" #bit ", #1\n"  /* h - 1 */ \
	"bic  %x[tB], %x[smb], %x[tA]\n"          /* h ? mb : 0 */ \
	"add  %x[nr], %x[tB], %x[nr], lsl #1\n"   /* -r for the next step */ \
	"sub  %x[base], %x[c1], %x[nr], lsl #10\n" /* theta8(-1) */ \
	"add  %x[tC], %x[" #ko "], %x[tA]\n"       /* k + h - 1 */ \
	"bic  %w[tD], %w[Dm], %w[" #ko "]\n"       /* addend on c when k = 0 */ \
	"and  %w[tE], %w[Dp], %w[" #ko "]\n"       /* addend on !c when k = -1 */ \
	"bic  %w[tB], %w[c256], %w[" #ko "]\n"     /* the +1 that goes with ~D */ \
	"lsl  %w[tF], %w[nc], #2\n"               /* the carry word */ \
	"add  %w[tB], %w[tB], %w[nc], lsl #2\n"   /* the carry word plus it */ \
	/* the flag: c = upper digit */ \
	"csel %w[ad], %w[tD], %w[tE], gt\n" \
	"csel %w[cc], %w[tB], %w[tF], gt\n" \
	"csinc %x[" #kn "], %x[tC], %x[tC], gt\n"  /* k' = k + h - c */ \
	"csetm %x[tA], gt\n"                      /* c ? -1 : 0 */ \
	/* the carry-save add of the shifted pair, the carry word and the addend */ \
	"eor  %w[tD], %w[cc], %w[ns], lsl #1\n" \
	"and  %w[tE], %w[cc], %w[ns], lsl #1\n" \
	"cmp  %x[" #ko "], %x[tA]\n"               /* ne: the digit is not 0 */ \
	"bic  %x[tC], %x[dth], %x[" #kn "]\n"      /* [k' = 0] * (theta8(0) - theta8(-1)) */ \
	"eor  %w[ns], %w[tD], %w[ad]\n" \
	"and  %w[tF], %w[tD], %w[ad]\n" \
	"add  %x[th], %x[base], %x[tC]\n"         /* theta8(k') */ \
	"orr  %w[nc], %w[tE], %w[tF]\n" \
	/* delta * 256 of the pair the selector sees: the recompressed one, or on */ \
	/* a zero digit the un-recompressed one, whose sum & carry is nc itself */ \
	"and  %w[tD], %w[ns], %w[nc], lsl #1\n" \
	"csel %w[tD], %w[tD], %w[nc], ne\n" \
	"cmp  %x[th], %x[tD]\n"                   /* gt: the upper digit */

	__asm__ volatile(
		// step 0 consumes the digit +1 and has no zero-digit case
		"eor  %w[tD], %w[cc], %w[ns], lsl #1\n"
		"and  %w[tE], %w[cc], %w[ns], lsl #1\n"
		"eor  %w[ns], %w[tD], %w[ad]\n"
		"and  %w[tF], %w[tD], %w[ad]\n"
		"orr  %w[nc], %w[tE], %w[tF]\n"
		"and  %w[tD], %w[ns], %w[nc], lsl #1\n"
		"cmp  %x[th], %x[tD]\n"
		EE_DIVUNIT_STEP(23, k0, k1)
		EE_DIVUNIT_STEP(22, k1, k0)
		EE_DIVUNIT_STEP(21, k0, k1)
		EE_DIVUNIT_STEP(20, k1, k0)
		EE_DIVUNIT_STEP(19, k0, k1)
		EE_DIVUNIT_STEP(18, k1, k0)
		EE_DIVUNIT_STEP(17, k0, k1)
		EE_DIVUNIT_STEP(16, k1, k0)
		EE_DIVUNIT_STEP(15, k0, k1)
		EE_DIVUNIT_STEP(14, k1, k0)
		EE_DIVUNIT_STEP(13, k0, k1)
		EE_DIVUNIT_STEP(12, k1, k0)
		EE_DIVUNIT_STEP(11, k0, k1)
		EE_DIVUNIT_STEP(10, k1, k0)
		EE_DIVUNIT_STEP(9, k0, k1)
		EE_DIVUNIT_STEP(8, k1, k0)
		EE_DIVUNIT_STEP(7, k0, k1)
		EE_DIVUNIT_STEP(6, k1, k0)
		EE_DIVUNIT_STEP(5, k0, k1)
		EE_DIVUNIT_STEP(4, k1, k0)
		EE_DIVUNIT_STEP(3, k0, k1)
		EE_DIVUNIT_STEP(2, k1, k0)
		EE_DIVUNIT_STEP(1, k0, k1)
		"cset %w[c], gt\n"
		: [k0] "+&r"(k0), [k1] "+&r"(k1), [nr] "+&r"(nr), [base] "+&r"(base), [th] "+&r"(th),
		  [ad] "+&r"(ad), [cc] "+&r"(cc), [ns] "+&r"(ns), [nc] "+&r"(nc), [c] "=&r"(c),
		  [tA] "=&r"(tA), [tB] "=&r"(tB), [tC] "=&r"(tC), [tD] "=&r"(tD), [tE] "=&r"(tE), [tF] "=&r"(tF)
		: [T2inv] "r"(T2inv), [smb] "r"((u64)smb), [c1] "r"(c1), [dth] "r"(dth), [Dm] "r"(Dm), [Dp] "r"(Dp), [c256] "r"(256u)
		: "cc");
#undef EE_DIVUNIT_STEP
	// k_23 is in k1; h_23 is bit 0 of T2.
	const bool ahead = k1 != 0;
	const bool h = (T2 & 1u) != 0;
	u32 up;
	if (lt)
		up = c ? (ahead || !h) : (ahead && !h);  // k_24 == -1
	else
		up = ahead && c;                         // k_23 == -1 and the 25th digit is 0
	const u32 q = T + up;
	return lt ? q : (q << 1);
}
#endif

} // namespace COP1
} // namespace OpcodeImpl
} // namespace Interpreter
} // namespace R5900
