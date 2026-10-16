// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ABI-digest guard — the #1 corruption backstop for the persisted-JIT VU
// program cache.
//
// The on-disk cache trusts kMvuCompilerAbiVersion (mixed into the VERSION
// handshake) to mean "the emitters that produced a .vuprog are the emitters
// running now." An emitter change WITHOUT an ABI bump silently runs stale
// code shapes hydrated from disk — the payload checksum can't see it (the
// bytes match what was saved; it's the saver that changed). This test pins
// the emitted SHAPE per ABI version so that drift fails loudly at commit
// time instead.
//
// The digest (mVUPersist::TestComputeEmitDigest) masks every operand that
// may legitimately differ between correct emissions — 64-bit mov-chain
// immediates, B/BL displacements, ADRP pages, recorded B.cond displacements,
// image and data-arena offsets in fixup records — and
// hashes what remains: opcode selection, register allocation, instruction
// order, block/chunk/fixup structure. That is deterministic across runs,
// machines, and PIE/ASLR.
//
// WHEN THIS TEST GOES RED:
//   1. You changed mVU codegen (any microVU_*-arm64 emit path, AsmHelpers
//      canonical forms, the serializer layout): bump kMvuCompilerAbiVersion
//      in microVU-arm64.h (+ the mirror in mvu_progcache_versioning_tests),
//      then add the new {abi, digests} row below. The bump evicts every
//      stale on-disk cache — that's the point.
//   2. You changed a default config value that alters emitted forms (clamp
//      mode etc.): the options sentinel already evicts those caches; just
//      re-pin the digests here (no ABI bump needed).
//   Never "fix" this test by re-pinning without deciding which case you
//   are in.
//
// Recording is enabled during compilation because the cache only ever
// stores recording-enabled forms (fixed-shape MOVZ/MOVK runs for recorded
// addresses) — those are the shapes worth pinning.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "EeFpuModel.h"
#include "VU.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Config.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"

extern bool mVUvuAotVU0WindowHook(); // (microVU_VuAot-arm64.inl)

#include <gtest/gtest.h>

#include <cinttypes>
#include <cstdio>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper)
{
	return IBit(VuOp{VLitZero(), upper});
}

inline VuOp LowerOnly(u32 lower)
{
	return VuOp{lower, VNOP_U()};
}

// One digest per probe program. The set mirrors the round-trip suite's
// coverage axes: FMAC straight-line (upper pipeline + clamp emitters),
// conditional branch both-arms (block linking, block-object fixups), and
// indirect jump (two emission episodes, jump-cache path, stub calls).
struct DigestSet
{
	u64 straightLine;
	u64 branchBothArms;
	u64 indirectJump;
	// Broadcast-FMAC transform chain — pins the lane-indexed FMUL fold shape
	// (AX-14, unconditional since ABI v6). 0 in a pin row means "probe did
	// not exist for that ABI"; the assert skips zero pins.
	u64 broadcastChain;
	// Conditional branch in a branch delay slot — pins the condEvilBranch
	// target-select emission (ported at ABI v7; MGS2 VU0 solver hang).
	u64 condEvilBranch;
	// All-NOP VI-branch spin loop — pins the VU0 spin-wait fast-forward
	// head (mVUemitSpinFF, ABI v13). 0 in a pin row = probe absent.
	u64 spinLoop;
	// The only VU1 probe (added at ABI v15). Every probe above compiles on VU0, so
	// VU1 emitter drift was invisible to this backstop - which is exactly how the
	// ABI-15 change (E-bit flag validity, which applies to both VUs) could alter VU1
	// codegen without moving a single digest above. Branch whose arms reach a
	// program end, i.e. the shape that fix touches. 0 in a pin row = probe absent.
	u64 vu1BranchToEbit;
	// DIV / SQRT / RSQRT, the only div-unit probe (added at ABI v17); why it
	// exists is at its build site. 0 in a pin row = probe absent.
	u64 divUnit;
	// MADDA/MADD under vuClampMode:2 (added at ABI v17); why it exists is at its
	// build site. 0 in a pin row = probe absent.
	u64 maddClampE;
	// MSUBA/MSUB, the other half of that pair (added at abi 18).
	u64 msubClampE;
	// A multiply and an add under vuClampMode:3, the mode where mVUclamp2's
	// sign-preserving operand clamp emits, which every probe above misses. 0 in
	// a pin row = probe absent.
	u64 signClampMulAdd;
	// The same pair with a single-lane dest field, which is the only shape that
	// reads mVUglob.macWeights.bySSShift -- a different row of the table from
	// the one signClampMulAdd loads.
	u64 signClampSS;
	// The same two programs at vuClampMode:4, where the FMAC's exact models are
	// emitted -- all but one. CompileAndDigest forces vuFlagHack on to match
	// production and these programs leave the multiply's flags dead, so
	// mVUwantExactU comes back false and MAC U is in no digest in this table;
	// mVUwantExactO does not read flag liveness, because the ceiling
	// substitution reads its predicate, so MAC O and the ceiling are.
	// 0 in a pin row = probe absent.
	u64 exactMulAdd;
	u64 exactSS;
	// The divUnit probe at vuClampMode:3, where it keeps the host divide, and
	// again at 4, where the three ops call the model instead. divUnit itself
	// compiles below both. 0 in a pin row = probe absent.
	u64 signClampDivUnit;
	u64 exactDivUnit;
	// Three EFU ops at vuClampMode:3, where the recompiler still evaluates its
	// series in host arithmetic, and again at 4, where each calls one model
	// entry point. The EFU is VU1-only, so this is the one probe that is not
	// VU0 -- where the thirteen ops are NOPs. 0 in a pin row = probe absent.
	u64 signClampEfu;
	u64 exactEfu;
	// The load/store address path, on the VU whose data memory the recompiler
	// reaches off the pinned register file. Every probe above is memory-free,
	// which is why abi 4's constant-address fold moved no digest at all.
	// 0 in a pin row = probe absent.
	u64 vu1LoadStore;
	// A program end under MTVU, the setting every probe above compiles with off
	// -- and off is the setting under which a VU1 end raises no interrupt, so
	// the exit an MTVU end takes is in none of the digests above.
	// 0 in a pin row = probe absent.
	u64 vu1EbitMtvu;
	// CLIP, the one upper op that produces a GPR flag word rather than a vector
	// result. Its comparison-to-bits gather is emitted nowhere else, and no
	// probe above carries a CLIP, so the whole of it could be rewritten without
	// moving a digest. 0 in a pin row = probe absent.
	u64 clipFlag;
	// Four consecutively numbered VF reads, so mvuPreloadRegisters queues
	// 1,2,3,4 and both pairs are adjacent in the state block. Every probe
	// above preloads too, but a probe whose register numbers make no pair
	// cannot tell a lost fold from a reordered queue. 0 in a pin row = probe
	// absent.
	u64 preloadPairs;
	// Two masked writes whose destinations are also preloaded, so each one
	// reaches clearNeeded with a second cached copy of its own register in
	// front of it. Every probe above writes whole registers, so none of them
	// reaches the merge at all. 0 in a pin row = probe absent.
	u64 mergeFold;
	// The single-lane dest program at vuClampMode:2, the mode where the
	// result clamp answers a single-lane dest with its own sequence. The two
	// probes that carry that sequence today are DIV and RSQRT; the FMACs with
	// a single-lane dest above compile at modes 3 and 4, where the
	// sign-preserving clamp takes them instead. 0 in a pin row = probe absent.
	u64 clampESS;
	// MADDA with a dest field that is neither one lane nor all four, and MADDA
	// with one lane: the two shapes whose accumulate runs on a scratch copy of
	// ACC. Every MADD and MSUB above writes all four lanes, which is the third
	// branch. Compiled at vuClampMode 2, where only the wider of the two folds
	// its copy into the clamp, and again at 3, where both do.
	// 0 in a pin row = probe absent.
	u64 clampEPartialAcc;
	u64 signClampPartialAcc;
	// The default-mode shapes the probes above do not reach, on VU1: MAX
	// against VF0, a broadcast multiply by VF0.w behind the operand clamp, the
	// OP ops at a w-less field, FCOR, Q into a single-lane MADD and a single-lane
	// ADD (both computed in place at this mode), IBEQ between two live VIs, and
	// LQ/SQ off a live VI. And VU0's load/store path, which reaches its memory
	// through a pointer VU1's does not need. Added at abi 57.
	// 0 in a pin row = probe absent.
	u64 vu1DefaultShapes;
	u64 vu0LoadStore;
};

struct AbiPin
{
	u32 abi;
	DigestSet digests;
};

// === THE PIN TABLE === (see header comment for the update protocol)
constexpr AbiPin kPins[] = {
	// abi 4: vi00 const-addr loadstore fold (6018936dc). The probes below use no
	// LQ/SQ/ILW/ISW, so the folded ops leave their emitted shape unchanged — the
	// digests are bit-identical to abi 3; the bump is to evict on-disk caches
	// recorded with the pre-fold loadstore shape.
	{4, {0x4c3b6e1330199619, 0xd6f530cc13f0d0aa, 0xfcead342cc0b7df8, 0, 0}},
	// abi 5: mVUclamp2 2-row sign-clamp bounds (AX-02). The probes run under
	// the default clamp config, where the sign-overflow path never emits —
	// digests are bit-identical to abi 4; the bump evicts on-disk caches
	// recorded with the old all-lane sign-clamp shape (the options sentinel
	// can't distinguish those: same config, different emitter).
	{5, {0x4c3b6e1330199619, 0xd6f530cc13f0d0aa, 0xfcead342cc0b7df8, 0, 0}},
	// abi 6: lane-indexed FMUL broadcast fold unconditional (AX-14). The three
	// original probes contain no broadcast ops, so their digests are
	// bit-identical to abi 5; the bump evicts caches recorded with the old
	// Dup-materialized broadcast shape, and the new broadcastChain probe pins
	// the folded emission from here on (harvested from the first,
	// deliberately red, run).
	{6, {0x4c3b6e1330199619, 0xd6f530cc13f0d0aa, 0xfcead342cc0b7df8, 0x44bd2acfb23dff74, 0}},
	// abi 7: condEvilBranch ported (conditional branch in a branch delay slot
	// emits the badBranch/evilBranch target-select sequence; MGS2 VU0 solver
	// hang). The four original probes contain no branch-in-delay-slot, so
	// their digests are bit-identical to abi 6; the bump evicts caches
	// recorded when that sequence emitted nothing, and the new condEvilBranch
	// probe pins the ported emission from here on (harvested from the first,
	// deliberately red, run).
	{7, {0x4c3b6e1330199619, 0xd6f530cc13f0d0aa, 0xfcead342cc0b7df8, 0x44bd2acfb23dff74, 0xd04db07f3eb1a343}},
	// abi 8: hot microVU scalars (divFlag/branch/VIbackup/VIxgkick/cycles/…)
	// moved adjacent to the flag block and addressed as [gprMVUFlag, #imm]
	// via mVUfieldMem instead of per-site absolute materialization. Every
	// probe that touches those fields changes shape, and pre-8 payloads
	// bake the old field addresses/offsets, so the bump must evict them.
	{8, {0xb35dd0237372d734, 0xc3c40fd5a5ec19c7, 0x23682664f86a2f8d, 0xbdfce8a7ecebe6a6, 0x45837d5d1d23009f}},
	// abi 9: IBcc condition carry (doBranchCondCarry) — the condition
	// computes into a pool temp and condBranch's tail Cmps it directly
	// instead of the Ldrsh reload. Only the branch-bearing probe moved;
	// the other four contain no conditional branch and are bit-identical
	// to abi 8.
	{9, {0xb35dd0237372d734, 0xb6dfab5c9a56d900, 0x23682664f86a2f8d, 0xbdfce8a7ecebe6a6, 0x45837d5d1d23009f}},
	// abi 10: inline jump-cache probe in normJumpCompile. The two
	// jump-bearing probes (indirectJump, condEvilBranch — its continuation
	// compiles a normal JR tail) change shape; the branch-only and
	// straight-line probes are bit-identical to abi 9.
	{10, {0xb35dd0237372d734, 0xb6dfab5c9a56d900, 0xc9abe2f224fb5710, 0xbdfce8a7ecebe6a6, 0x1fe80e2917de1c2d}},
	// abi 11: resume-aware dispatch (VE-07). mVUtestCycles' budget-break
	// exit B's mVU.cycleBreak — a new stub id in the fixup stream. The
	// instruction count and shape of every block are unchanged, but every
	// block carries a testCycles, so every probe's fixup structure (and
	// therefore digest) moves.
	{11, {0x5606c91c74538771, 0xf50098b57b42c70c, 0xdda10863aa6fe8f7, 0xb93a633324c1d588, 0x6efd9e660ba61479}},
	// abi 12: exit-stub gprF re-save removed (VE-03); the defensive
	// compile-failed guards grow a 5-insn inline flag backup. Only the two
	// probes that emit those guards (indirectJump via normJumpCompile,
	// condEvilBranch via condBranch badBranch) change shape; the
	// straight-line, branch-both-arms, and broadcast probes are
	// bit-identical to abi 11.
	{12, {0x5606c91c74538771, 0xf50098b57b42c70c, 0x421bbc34e2552655, 0xb93a633324c1d588, 0x29d9172f7ccbd58f}},
	{13, {0x5606c91c74538771, 0xf50098b57b42c70c, 0x421bbc34e2552655, 0xb93a633324c1d588, 0x29d9172f7ccbd58f, 0xe9028a53cd86dcb7}},
	// abi 14: mVUsetupFlags no longer emits status-flag register self-moves at
	// block links (getFlagReg(i) == gprF[i], so an identity ring phase emitted up
	// to four no-op ORRs; vixl keeps Mov(Wd,Wd) because the 32-bit move clears
	// bits 63:32). Only the two status-flag-linked probes move (indirectJump and
	// condEvilBranch); straightLine/branchBothArms/broadcastChain have no
	// exact-match status link, and the spin loop's all-NOP body writes no status
	// flag, so those four are bit-identical to abi 13. Harvested from the first,
	// deliberately red, run.
	{14, {0x5606c91c74538771, 0xf50098b57b42c70c, 0x8022f1986a924c1c, 0xb93a633324c1d588, 0x49548c4995cf112f, 0xe9028a53cd86dcb7}},
	// abi 15: E-bit flag validity (mVU.needFlagFinalize). A block reaching a program
	// end used to elide the tail FMACs' flag writes that mVUendProgram then stores
	// into VI[REG_*_FLAG], so finalisation read a never-written ring instance. Now
	// the last tail FMAC's writes are emitted, and getLastFlagInst recovers an
	// unwritten flag from the incoming ring phase. straightLine / branchBothArms /
	// broadcastChain move (each ends in an E-bit) and the new vu1BranchToEbit pins
	// the VU1 shape; indirectJump is unchanged (it forces the bits on the JR/JALR
	// arm, not the E-bit arm), condEvilBranch is unchanged (no E-bit inside its
	// 4-instruction scan window), and the all-NOP spin loop writes no flag - those
	// three are bit-identical to abi 14. Harvested from the first, deliberately
	// red, run.
	{15, {0x52d7ab0dcf5ff0b0, 0xe306afec81428b0c, 0x8022f1986a924c1c, 0x119ed5c369c1435c, 0x49548c4995cf112f, 0xe9028a53cd86dcb7, 0x383abeec076a40fb}},
	// abi 16: mVUupdateFlags packs sign and zero in a single SLI/AND/ADDV against
	// a mVUglob-resident weight vector instead of two literal-pool movemask
	// chains, folding AND_XYZW and SHIFT_XYZW into that vector (16 insns of flag
	// packing per FMAC down to 7, and the per-site 16-byte literal-pool slot
	// gone). Every flag-writing FMAC changes shape, and since abi 15 every probe
	// ends in one, so all seven digests move. Harvested from the first,
	// deliberately red, run.
	{16, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xcedb19689232b21c, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075}},
	// abi 17: two-step FMACs drop the operand clamp on the product the multiply
	// step just clamped as a result. The seven digests above are unchanged;
	// maddClampE is the field this bump adds.
	//
	// divUnit stays at the shape the tree emits -- the quotient is not flushed,
	// which is the open defect VuSticky*.DISABLED_*DivUnitFlushesDenormalQToSignedZero
	// record. Whichever bump lands that flush re-pins this field.
	{17, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xcedb19689232b21c, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075, 0xf7b84d8c08fa2266, 0xde92be2516a10fbb}},
	// v18: RSQRT's zero path ORs into divFlag rather than assigning, so the
	// preceding sign test's I survives a -0 divisor. divUnit is the only field
	// that moves -- DIV and SQRT keep their shapes, and the other nine probes
	// never reach an RSQRT. msubClampE is new here; a probe changes no emitted
	// code, so it needs no bump of its own.
	{18, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xcedb19689232b21c, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075, 0x3c5065e7ab8cf631, 0xde92be2516a10fbb, 0x1270eee2b9725c68}},
	// v19: DIV's and RSQRT's zero paths build 0x7FFFFFFF with one MVNI instead
	// of loading the signbit/maxvals pair, so each loses two instructions and
	// two mVUglob operands; the three ops' zero tests read the operand's
	// exponent field instead of comparing it against 0.0, which costs a Umov
	// apiece, and RSQRT's divisor test is on the radicand rather than the
	// root. Every FMAC changes shape too: the weight table gains a variant
	// dimension so every weight load's [x25, #imm] moves, the I immediate is
	// stored whole, and at vuClampMode:4 a flag-writing multiply emits its MAC U
	// predicate ahead of the operand clamp, every FMAC its MAC O the same way
	// and then substitutes the FMAC's ceiling for the clamp's FLT_MAX, ADD and
	// SUB mask their guard bits, and the three divide-unit ops and the thirteen
	// EFU ops call their models out of line.
	// The ten fields above compile below mode 3 with a full dest field, so only
	// divUnit moves among them; the eight new probes pin the two gated modes
	// from here on. The value flush that rides with the zero tests is in none
	// of these rows: they compile under the default VU FPCR, which sets FZ, and
	// it is emitted only when that is clear.
	//
	// The multiplier's one-ULP deficit (armEmitVuDefectiveMul) rides in this
	// row rather than a bump of its own. It changes shape only at vuClampMode
	// 4, and vuClampMode 4 arrived with abi 19: the options sentinel carries
	// vu{0,1}ExactMode, so a cache that could hold a mode-4 shape was written
	// by an abi-19 build or none at all, and the ABI field already separates
	// every earlier one. exactMulAdd and exactSS carry it; the mode-3 pair does
	// not, a multiply below 4 being a bare FMUL, and neither does the EFU pair,
	// whose polynomials keep the plain multiply at either mode. Once 19 ships,
	// a mode-4 change needs its own bump like any other.
	{19, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xcedb19689232b21c, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075, 0x7cfc9e2b6a3e852d, 0xde92be2516a10fbb, 0x1270eee2b9725c68, 0x3e1c524e13373c98, 0x00410ea5fd07a5f9, 0xa2465092b0e3404a, 0x04899f265502aa58, 0x6b119d8d1e4fd199, 0x97c76bda811bc8e4, 0xd933afa738820832, 0xa7ad93456cba5eb2}},
	// abi 20: the zero-divisor Q is 0x7FFFFFFF from vuClampMode 3. Only divUnit
	// moves -- the default-mode probe; signClampDivUnit and exactDivUnit keep
	// abi 19's values.
	{20, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xcedb19689232b21c, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075, 0x01dc53e64a60783b, 0xde92be2516a10fbb, 0x1270eee2b9725c68, 0x3e1c524e13373c98, 0x00410ea5fd07a5f9, 0xa2465092b0e3404a, 0x04899f265502aa58, 0x6b119d8d1e4fd199, 0x97c76bda811bc8e4, 0xd933afa738820832, 0xa7ad93456cba5eb2}},
	// abi 21: an operand clamp reads the register the clone-write copy in front
	// of it copied, and the copy goes. Five probes move -- broadcastChain at
	// the default mode and maddClampE at 2, through mVUclamp1's four-lane arm,
	// and signClampMulAdd, signClampDivUnit and signClampEfu at 3, through
	// mVUclamp2's. The rest emit copies the clamp cannot reach, because
	// something stands between: an allocation of the other operand
	// (msubClampE) or a MAC predicate (the vuClampMode:4 probes). signClampSS
	// emits no whole-register copy at all -- a single-lane clone rotates the
	// lane it wants into place instead.
	{21, {0xea70f53db2854bca, 0x9157dafe405a3a55, 0xb13784e6118693ae, 0xc745a3959fa555ed, 0x65186fa7d80a9143, 0x6f61eab8d8b08e06, 0x75d083cba14f4075, 0x01dc53e64a60783b, 0x7b2cf129a0112eb5, 0x1270eee2b9725c68, 0xdd9e01f04dfdf231, 0x00410ea5fd07a5f9, 0xa2465092b0e3404a, 0x04899f265502aa58, 0xe2c3e03122ce9e70, 0x97c76bda811bc8e4, 0x9600d0c470905660, 0xa7ad93456cba5eb2}},
	// 22: mVUclamp1 reads its bounds out of q25/q26 instead of loading them,
	// the block lays the pair down once at its entry, and q25/q26 leave the
	// VF pool in micro mode -- so every probe moves, the clamp-free ones
	// through the entry Ldp and what the allocator hands out.
	{22, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0x0126dc75bb4430b9, 0xaebf14b1ed6decc1, 0xb43ff459f5b10828, 0x7b02c5ad05cbf2da, 0xbb97e4783596605e, 0xe140eff6bb95297c}},
	// abi 23: the model-call seam moved behind a per-target stub and the
	// serializer gained a stub id per target.
	// Only the four exact-mode probes move; divUnit and signClampDivUnit run
	// below mode 4 and stay bit-identical to abi 22.
	{23, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5}},
	// abi 24: ILW and ILWR add their lane offset to the address in the register
	// width mVUaddrFix answered in rather than the low half of it. No probe
	// here reaches a memory op, so all eighteen digests are bit-identical to
	// abi 23; the bump evicts caches holding the truncating form.
	{24, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5}},
	// abi 25: VU1's data memory moved into the object that holds the register
	// file, a fixed distance from the pointer the recompiler keeps pinned to
	// it, so a VU1 access reaches memory off that pin and carries the distance
	// in its own displacement instead of loading VURegs::Mem. The eighteen
	// probes above are memory-free and stay bit-identical to abi 24; the bump
	// evicts caches recorded with the pointer-load shape, and the new
	// vu1LoadStore probe pins the address path from here on.
	{25, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x4ce85fa74802244b}},
	// abi 26: LQD and SQD off a vi00 base take the pre-decrement every other
	// base takes. The eighteen memory-free probes stay bit-identical to abi
	// 25; vu1LoadStore gains the LQD that pins the shape, and the bump evicts
	// caches recorded while a vi00 base answered a fixed address.
	{26, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x76e983749424f6a7}},
	// abi 27: ILW and ILWR read the lane their dest field's two-bit code names.
	// The eighteen memory-free probes stay bit-identical to abi 26;
	// vu1LoadStore's ILW moves to a field the two rules disagree about, and
	// the bump evicts caches recorded with the first-bit-set offset.
	{27, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x2a33091e21e64b2e}},
	// abi 28: a VU1 program ending on the E bit under MTVU branches to an exit
	// entry that raises the interrupt instead of calling mVUEBit and then
	// branching to the plain exit. The nineteen probes above compile with MTVU
	// off, where no raise was emitted either way, and stay bit-identical to abi
	// 27; the bump evicts caches recorded with the call, and the new
	// vu1EbitMtvu probe pins the MTVU end from here on.
	{28, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x2a33091e21e64b2e, 0xc43c3130b53ef6c2}},
	// abi 29: no emitted shape changes -- the options sentinel gained
	// THREAD_VU1, and the bump evicts the caches recorded while it could not
	// tell an MTVU run from a run without it. All twenty probes are
	// bit-identical to abi 28. The clipFlag probe is new in this row; a probe
	// changes no emitted code, so it needs no bump of its own.
	{29, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x2a33091e21e64b2e, 0xc43c3130b53ef6c2, 0x6054df75e702ca90}},
	// abi 30: CLIP packs its six comparison results with one UZP1 and a
	// weighted ADDV instead of moving each comparison lane to a GPR and
	// shifting it into place. clipFlag is the only probe that carries a CLIP,
	// so it is the only digest that moves; the weight vector is appended past
	// macWeights, which leaves every other [x25, #imm] where it was.
	{30, {0x7282c445048bef4b, 0x89652dee7bcd0ce6, 0xb8d7c5cd93fbb74e, 0x49385e15e4f6e37e, 0x389454f62983c56c, 0x7ee1c5b565aaee67, 0x1771f7876dde341b, 0xb39c16ac7a312e7c, 0xd7ba3d958fcf1701, 0x339ea6032537601a, 0xbf94567a340e484f, 0xd58dea7aac63b17d, 0xd12f010786dd4b74, 0x6f406715e3b136e3, 0xb43ff459f5b10828, 0xbc94317b2bbc5f9f, 0xbb97e4783596605e, 0x5106c85d18b5c7a5, 0x2a33091e21e64b2e, 0xc43c3130b53ef6c2, 0x32907d678adc7b1c}},
	// abi 31: block-start VF preloads queued and emitted together. Every digest
	// in the row moves, because every probe preloads both VI and VF and the VF
	// loads now follow the VI ones instead of interleaving with them. The
	// preloadPairs probe is new in this row.
	{31, {0xa863f1f9879ae2b5, 0x8fdfe3b98dfea42d, 0x0dbe431ba7baaf38, 0xec2e364d3f85ea15, 0x0642ad7febc371dd, 0xe3db98da4cbd7d0b, 0x13165636b400bc74, 0xa4a62656a8a9349d, 0xc580902ac88802bc, 0x20f440e8c49d3b85, 0x9922363b6464ec7a, 0x23ea8e71f7369f2e, 0x553f416f68fea579, 0xef4f9d0d4006e176, 0x8e18f3dc58066cc7, 0xf295da959d87f2eb, 0x0ca04784dfd0f42e, 0x13c623d3df5f5258, 0x609feb3860a77eb4, 0x0d2f4a1d43a8196f, 0xce62e252482f10f7, 0x65d99aa82d01f2eb}},
	// abi 32: a three-component write keeps its own slot and takes the fourth
	// component from the other cached copy. No probe above writes a masked
	// destination, so every digest in the row is the abi 31 value; the
	// mergeFold probe is new and is the only one that reaches the merge.
	{32, {0xa863f1f9879ae2b5, 0x8fdfe3b98dfea42d, 0x0dbe431ba7baaf38, 0xec2e364d3f85ea15, 0x0642ad7febc371dd, 0xe3db98da4cbd7d0b, 0x13165636b400bc74, 0xa4a62656a8a9349d, 0xc580902ac88802bc, 0x20f440e8c49d3b85, 0x9922363b6464ec7a, 0x23ea8e71f7369f2e, 0x553f416f68fea579, 0xef4f9d0d4006e176, 0x8e18f3dc58066cc7, 0xf295da959d87f2eb, 0x0ca04784dfd0f42e, 0x13c623d3df5f5258, 0x609feb3860a77eb4, 0x0d2f4a1d43a8196f, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8}},
	// abi 33: the flag queue reorder at a block link. indirectJump and
	// condEvilBranch are the two probes that link blocks at all, and both
	// move; every other digest in the row is the abi 32 value.
	{33, {0xa863f1f9879ae2b5, 0x8fdfe3b98dfea42d, 0x8331e0b01b2391b0, 0xec2e364d3f85ea15, 0xd192a204bad1f3e0, 0xe3db98da4cbd7d0b, 0x13165636b400bc74, 0xa4a62656a8a9349d, 0xc580902ac88802bc, 0x20f440e8c49d3b85, 0x9922363b6464ec7a, 0x23ea8e71f7369f2e, 0x553f416f68fea579, 0xef4f9d0d4006e176, 0x8e18f3dc58066cc7, 0xf295da959d87f2eb, 0x0ca04784dfd0f42e, 0x13c623d3df5f5258, 0x609feb3860a77eb4, 0x0d2f4a1d43a8196f, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8}},
	// abi 34: the single-lane result clamp. divUnit and signClampDivUnit move,
	// because DIV and RSQRT are the only ops in this table that reach the
	// single-lane case: the FMACs with a single-lane dest field compile at
	// modes 3 and 4, where the sign-preserving clamp takes their operands
	// instead. Every other digest in the row is the abi 33 value.
	{34, {0xa863f1f9879ae2b5, 0x8fdfe3b98dfea42d, 0x8331e0b01b2391b0, 0xec2e364d3f85ea15, 0xd192a204bad1f3e0, 0xe3db98da4cbd7d0b, 0x13165636b400bc74, 0x5025a647a5291be9, 0xc580902ac88802bc, 0x20f440e8c49d3b85, 0x9922363b6464ec7a, 0x23ea8e71f7369f2e, 0x553f416f68fea579, 0xef4f9d0d4006e176, 0xef2d5c2fc47b5fbc, 0xf295da959d87f2eb, 0x0ca04784dfd0f42e, 0x13c623d3df5f5258, 0x609feb3860a77eb4, 0x0d2f4a1d43a8196f, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8, 0x03f5e94967268aad}},
	// abi 35: the sign-preserving clamp's paired bounds. The eight probes that
	// compile at vuClampMode 3 and 4 move, which is every one that reaches
	// mVUclamp2's integer path at all; clampESS is a mode below it and holds
	// the abi 34 value, as does every digest above signClampMulAdd.
	{35, {0xa863f1f9879ae2b5, 0x8fdfe3b98dfea42d, 0x8331e0b01b2391b0, 0xec2e364d3f85ea15, 0xd192a204bad1f3e0, 0xe3db98da4cbd7d0b, 0x13165636b400bc74, 0x5025a647a5291be9, 0xc580902ac88802bc, 0x20f440e8c49d3b85, 0xa826c26b402d511f, 0xbed63e7a97c66fe9, 0x7138104cc75f1f14, 0xc924075bd9ae8ce3, 0x65f71dc1b0e950ea, 0x547b51b54e2f65d5, 0xf0fdbefe96fe37bf, 0x9a85250029e6d52e, 0x609feb3860a77eb4, 0x0d2f4a1d43a8196f, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8, 0x03f5e94967268aad}},
	// abi 36: operand clamps take their folds up front. Sixteen digests move,
	// which is every probe that clamps two operands together: at the default
	// clamp mode the FMAC body does that, and from vuClampMode 2 up the body's
	// own calls are switched off and the arithmetic step does it instead, so
	// the two halves of the change land on disjoint probes. Every other digest
	// in the row is the abi 35 value.
	{36, {0x7900a833415f808c, 0x7f9ea711b0215957, 0x8331e0b01b2391b0, 0xec2e364d3f85ea15, 0xa104a156e75bad17, 0xe3db98da4cbd7d0b, 0x3bb0d5a0635e95e1, 0x5025a647a5291be9, 0x58571595e2afc721, 0x4773d7af5676cbf6, 0x739e35859f782c59, 0x018c5e3bc50fbde8, 0xcecfc92a38a94129, 0x0a01fc0a416b74b4, 0x2f11a92405afafa3, 0xebbb4c0fe0ded02f, 0x4b83b4d5ec1cf0fb, 0x134f8aa4c8fef68c, 0x609feb3860a77eb4, 0x19ae51a331f34432, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8, 0xb995f93ecebfab9c}},
	// abi 37: the accumulate's own copy of ACC joins the fold. No digest above
	// moves -- every MADD and MSUB in the table writes all four lanes, which is
	// the branch that accumulates into ACC itself and makes no copy -- so the
	// two probes for the other branches are added here, and the rest of the row
	// is the abi 36 value.
	{37, {0x7900a833415f808c, 0x7f9ea711b0215957, 0x8331e0b01b2391b0, 0xec2e364d3f85ea15, 0xa104a156e75bad17, 0xe3db98da4cbd7d0b, 0x3bb0d5a0635e95e1, 0x5025a647a5291be9, 0x58571595e2afc721, 0x4773d7af5676cbf6, 0x739e35859f782c59, 0x018c5e3bc50fbde8, 0xcecfc92a38a94129, 0x0a01fc0a416b74b4, 0x2f11a92405afafa3, 0xebbb4c0fe0ded02f, 0x4b83b4d5ec1cf0fb, 0x134f8aa4c8fef68c, 0x609feb3860a77eb4, 0x19ae51a331f34432, 0xce62e252482f10f7, 0x65d99aa82d01f2eb, 0x2872d007bb0040b8, 0xb995f93ecebfab9c, 0x4b3ab298c551a643, 0xa7196c456f0fe346}},

	// abi 38: the cycle-budget break moves into a shared stub. Every digest
	// moves — mVUtestCycles runs at the top of every block, so every probe
	// carries the arm.
	{38, {0x1b6417d39088c445, 0x4fb21f7df2e0eba1, 0x5b22843577880a34, 0x367caabd4ce98b70, 0x931e4ce41aaddfee, 0x528f4bc3805ce760, 0x474ca3bc38053234, 0xf3c90aa7361cb856, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0x2179f3c725e74523, 0x28d99b6d7e13c0c8, 0x0f96630605ddf7d5, 0x008451d3443b1898, 0x6635ce78819a2162, 0x3ad5ab94e92fe23a, 0x631b8efb8b3cb5e5, 0xa19f8472ae85bb94, 0x205545fc7b500ddb, 0xfd15d794fe441fbf, 0x51aad4123c80fc0f, 0xc4b3d7d6d8249b94, 0x742e2538da750e4d, 0xebe95cc4b6a325c2, 0x025ba9f9ef343b1c, 0x1b2a82838a2443b2}},
	// abi 39: the sign-preserving clamp's all-lane row reads the resident bounds
	// instead of loading them. The probes that compile at vuClampMode 3 and 4
	// move; the single-lane forms keep their Ldp, and modes 0 to 2 never emit it.
	{39, {0x1b6417d39088c445, 0x4fb21f7df2e0eba1, 0x5b22843577880a34, 0x367caabd4ce98b70, 0x931e4ce41aaddfee, 0x528f4bc3805ce760, 0x474ca3bc38053234, 0xf3c90aa7361cb856, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0xa1e2f7d5c83033f5, 0x28d99b6d7e13c0c8, 0x191cc45df004b45c, 0x008451d3443b1898, 0xb4d7293333bb33b9, 0x22851a1d6e1da4f7, 0x218d866b851d4396, 0xf59d5b3e255ec796, 0x205545fc7b500ddb, 0xfd15d794fe441fbf, 0x51aad4123c80fc0f, 0xc4b3d7d6d8249b94, 0x742e2538da750e4d, 0xebe95cc4b6a325c2, 0x025ba9f9ef343b1c, 0x7b59f4d79964c4b3}},
	// abi 40: the single-lane clone-write rotation reads the cached register. Every
	// probe with a y, z or w single-lane FMAC or div-unit operand moves.
	{40, {0x1b6417d39088c445, 0x4fb21f7df2e0eba1, 0x5b22843577880a34, 0x367caabd4ce98b70, 0x931e4ce41aaddfee, 0x528f4bc3805ce760, 0x474ca3bc38053234, 0xc97e9916ce78f2c0, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0xa1e2f7d5c83033f5, 0xe2b98245075f607d, 0x191cc45df004b45c, 0xe5bcbd769f0cc83d, 0xb7e746b32d5eb2ab, 0xf22bf72b61864a31, 0x415b885932416527, 0xcdc6cfb5fea6f42e, 0x205545fc7b500ddb, 0xfd15d794fe441fbf, 0x8ca5c650154688b7, 0xc4b3d7d6d8249b94, 0x742e2538da750e4d, 0xff2b4b6d7bf2a662, 0xf05dcc1e61ded6ec, 0xc20855db43aa02ee}},
	// abi 41: the arithmetic step folds the clone-write copy in front of it, as the
	// operand clamp already did. Every probe whose FMAC reads a cached VF moves.
	{41, {0x30a4b851f584d87b, 0x4fb21f7df2e0eba1, 0x7be4a67c17a34d58, 0x42cdb6c017ffbcb7, 0xdc148443ac907bfd, 0xd53e1c6f0ab93994, 0x474ca3bc38053234, 0x08508bd89801b6f3, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0xa1e2f7d5c83033f5, 0xe2b98245075f607d, 0x191cc45df004b45c, 0xe5bcbd769f0cc83d, 0xb7e746b32d5eb2ab, 0xf22bf72b61864a31, 0x415b885932416527, 0xcdc6cfb5fea6f42e, 0xd9b404bd2887a881, 0xfd15d794fe441fbf, 0x285938277c630ba8, 0xc0f07710fba14955, 0xca2ddace4f03527c, 0xff2b4b6d7bf2a662, 0xf05dcc1e61ded6ec, 0xc20855db43aa02ee}},
	// abi 42: MAX/MINI against VF0 compile to one integer max or min. No probe
	// issues one, so the row repeats abi 41's.
	{42, {0x30a4b851f584d87b, 0x4fb21f7df2e0eba1, 0x7be4a67c17a34d58, 0x42cdb6c017ffbcb7, 0xdc148443ac907bfd, 0xd53e1c6f0ab93994, 0x474ca3bc38053234, 0x08508bd89801b6f3, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0xa1e2f7d5c83033f5, 0xe2b98245075f607d, 0x191cc45df004b45c, 0xe5bcbd769f0cc83d, 0xb7e746b32d5eb2ab, 0xf22bf72b61864a31, 0x415b885932416527, 0xcdc6cfb5fea6f42e, 0xd9b404bd2887a881, 0xfd15d794fe441fbf, 0x285938277c630ba8, 0xc0f07710fba14955, 0xca2ddace4f03527c, 0xff2b4b6d7bf2a662, 0xf05dcc1e61ded6ec, 0xc20855db43aa02ee}},
	// abi 43: VI arithmetic reads the cached source and folds its immediate. No
	// probe adds to a cached VI, so the row repeats abi 42's.
	{43, {0x30a4b851f584d87b, 0x4fb21f7df2e0eba1, 0x7be4a67c17a34d58, 0x42cdb6c017ffbcb7, 0xdc148443ac907bfd, 0xd53e1c6f0ab93994, 0x474ca3bc38053234, 0x08508bd89801b6f3, 0x97abca8e1e797c8e, 0x3a85dc8fbf46e675, 0xa1e2f7d5c83033f5, 0xe2b98245075f607d, 0x191cc45df004b45c, 0xe5bcbd769f0cc83d, 0xb7e746b32d5eb2ab, 0xf22bf72b61864a31, 0x415b885932416527, 0xcdc6cfb5fea6f42e, 0xd9b404bd2887a881, 0xfd15d794fe441fbf, 0x285938277c630ba8, 0xc0f07710fba14955, 0xca2ddace4f03527c, 0xff2b4b6d7bf2a662, 0xf05dcc1e61ded6ec, 0xc20855db43aa02ee}},
	// abi 44: the status non-sticky clear reads the previous instance in place.
	// Every probe ends in a flag-writing FMAC whose status the end forces.
	{44, {0xcc5d8ed2aff67456, 0xf22354557ba8d4ba, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0xb261732330cbb6c5, 0x59776b355a957571, 0x6bc6dc459520c9da, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0xba027c76c09e3d5f, 0xf6775d1893001ca3, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 45: VU1 address formation reads the base VI in place. Only the VU1
	// load/store probe moves.
	{45, {0xcc5d8ed2aff67456, 0xf22354557ba8d4ba, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0xb261732330cbb6c5, 0x59776b355a957571, 0x6bc6dc459520c9da, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0xf6775d1893001ca3, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 46: IBEQ/IBNE fold a vi00 operand and test the carried condition's low
	// halfword. The probes whose branch is an IBNE against vi00 move.
	{46, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 47: FCOR is a BICS of the bits its immediate leaves clear. No probe
	// issues one, so the row repeats abi 46's.
	{47, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 48: single-lane and Q multiplies take the by-element fold. No probe
	// multiplies by Q or broadcasts into a single lane, so the row repeats 47's.
	{48, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 49: the OP ops permute from the cached multiplicands. No probe issues
	// one, so the row repeats abi 48's.
	{49, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xb7ef7494b89aa3d8, 0x021b1d8ea47dc8e8, 0x47545cdb987b6602, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0x8775d22d00a22275, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xc3952268061ca731, 0x1e28c18d2d06b66a, 0xb492272d123efd35}},
	// abi 50: a single-lane op clamps its scalar destination full width. The
	// single-lane probes at clamp modes 2-4 and the EFU at mode 3 move.
	{50, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xb1e0cfecf4302713, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 51: CLIP reads its operands in place. Only the CLIP probe moves.
	{51, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x6dcbc8ee9c41c950, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x3a7def77c71d10af, 0x9fbf8b955489dc19, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 52: DIV tests its operands for zero with FCMP under FZ. The three
	// div-unit probes move.
	{52, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x9d323643d87a1b0a, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 53: micro mode addresses VU memory off the x29 pin. Only the VU1
	// load/store probe moves; the pin itself is set in the dispatcher.
	{53, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x321e1e6abe00ab51, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 54: a full-width ACC write keeps its result where it was computed.
	// Only broadcastChain, which opens with an xyzw MULAx, moves.
	{54, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 55: the identity multiply by VF0.w behind a clamp is dropped. No probe
	// multiplies by VF0.w, so the row repeats abi 54's.
	{55, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 56: IBEQ/IBNE XOR the cached Is in place. No probe compares two live
	// VIs, so the row repeats abi 55's.
	{56, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052}},
	// abi 57: single-lane y/z/w FMACs compute in place below clampE. Every
	// single-lane probe runs at clampE or above, so the row repeats abi 56's.
	// It also pins vu1DefaultShapes and vu0LoadStore, added at this version.
	{57, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x0e5205525af143af}},
	// abi 58: single-lane MOVE/MR32 insert into a cached destination. No probe
	// issues one, so the row repeats abi 57's.
	{58, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x0e5205525af143af}},
	// abi 59: an E-bit conditional branch keeps its flag instances in place. Only
	// shapes whose branch target reads flags early move; no probe has one.
	{59, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x0e5205525af143af}},
	// abi 60: VU0's window onto VU1's registers calls the transfer thunk for the STATUS quadword
	// without MTVU too (vu0LoadStore only).
	{60, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x48975533172d5ce5}},
	// abi 61: that hook only while the vu-aot tier is wanted. The digests run with the setting off,
	// so vu0LoadStore is abi 59's again.
	{61, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0xd91488a73ad2068a, 0xd304c548206d672b, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x0e5205525af143af}},
	// abi 62: a write allocated with an empty field leaves a temp (the vu-aot branch took this from
	// mvu-local-rules, where it is abi 60). Only the two EFU probes move: ESUM is encoded with a zero
	// field, which for it is not a destination mask, and its Fs slot is no longer recorded as VF0.
	// Every other column is abi 61's. Pinned from the merged code.
	{62, {0xcc5d8ed2aff67456, 0x3740d9d84d609b95, 0x49140181ed6daa6d, 0x8cd5553cb2a8daba, 0x30d527dd8fe4d10c, 0x1a887bba90ef1a41, 0x51df58673dbf1b10, 0x929029ca42ca98c1, 0x01e61f64bdbc1a38, 0x01c604cf32776547, 0x024ff6cd7fabd2c8, 0xace1f4023f0e8bf8, 0x021b1d8ea47dc8e8, 0xc8c50b58f3f9badd, 0x530cf428707c7f11, 0x0be3b676bc50b217, 0x791e61d19068a3bd, 0x14765f7c645b4aee, 0x0cc9eeeb40bf56e4, 0x43b9ebeda584952a, 0xed424f10e0661fdf, 0x029663d8580ead20, 0x72a4cdceb608619f, 0xe34a4dda6a35b3df, 0xf9c3d13580d762e9, 0x2c012558a97e0052, 0xdb4ba733afd41dec, 0x0e5205525af143af}},
	// abi 63: recorded programs become relocatable across processes (the vu-progcache-aslr branch,
	// numbered 40 there). Block pointers are runs as wide as the pointer (were four), the fixup
	// records change shape, and a recorded cross-chunk B.cond keeps its short form. The digest also
	// changed with it (it masks recorded B.cond displacements and the image and data-arena offsets
	// in fixup records), so every column moves and none of the rows above is comparable to this one.
	// Pinned from the merged code, stable across shuffled runs.
	{63, {0xa48aa35478a78bdc, 0xd2cd8a66fb118db3, 0x3eb36a9cd84f76c7, 0x9cdcaa9d4fbe4aed, 0xac9ba26d82b481cf, 0x15c7e07ec93fdeb, 0x5dfd36d8bf388956, 0x2f39a76f7c0463d, 0xd6505b9f58da0e3d, 0xb167cadfd82d572a, 0xcff9c1f9cf303b6f, 0x20c6e112f962a426, 0x85ca05f33a95303a, 0x4fb1bf9c5361c248, 0xfcd572c1c3cd38fb, 0x48a8cdab505c6ae3, 0x9f82581ec2b20523, 0xbe0aaa4977c08f5a, 0x1d33ada241458b9a, 0x586a9035b7d90693, 0xe03d34d038f6e0a9, 0x4a888cdf933e9c1a, 0xc3d6512831b59ba7, 0x3a4c8b9888d132b1, 0xb391e1e028a4ecbd, 0x424df76350b0cc50, 0xacc16ea21944cbbd, 0x326f8e96526c9b16}},
};

u64 CompileAndDigest(std::initializer_list<vu::VuOp> pairs,
	const char* requireVu0Divergence = nullptr)
{
	// The ABI digest pins the emitted shape that lands in the on-disk cache in
	// PRODUCTION, where vuFlagHack defaults on (and the options sentinel keeps
	// flaghack-on/off caches separate). The recompiler test environment now pins
	// vuFlagHack off for JIT-vs-interp determinism, so force it back on here —
	// otherwise the pins would track the non-production flaghack-off shape and
	// drift with the harness default rather than with real emitter changes.
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	EmuConfig.Speedhacks.vuFlagHack = true;

	VuTestHarness h(0);
	h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.SetVi(1, 1);
	h.LoadProgram(pairs);
	if (requireVu0Divergence)
		h.RunRequiringDivergence(requireVu0Divergence);
	else
		h.Run();
	h.RunJitPreserveBlockCache();
	u64 digest = 0;
	EXPECT_TRUE(mVUPersist::TestComputeEmitDigest(0, digest));
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
	return digest;
}

// Same contract as CompileAndDigest, on VU1. Kept separate rather than
// parameterised so the VU0 pins above can't shift if this one is edited.
u64 CompileAndDigestVu1(std::initializer_list<vu::VuOp> pairs,
	const char* requireVu1Divergence = nullptr)
{
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	EmuConfig.Speedhacks.vuFlagHack = true;

	VuTestHarness h(1);
	h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.SetVi(1, 1);
	h.LoadProgram(pairs);
	if (requireVu1Divergence)
		h.RunRequiringDivergence(requireVu1Divergence);
	else
		h.Run();
	h.RunJitPreserveBlockCache();
	u64 digest = 0;
	EXPECT_TRUE(mVUPersist::TestComputeEmitDigest(1, digest));
	RecompilerTestEnvironment::ResetVuBlockCache(1);

	EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
	return digest;
}

// Same contract again, with MTVU on -- what decides how a program ending on the
// E bit leaves. The interrupt word is restored around it because the compiled
// code raises the E-bit flag in it as the harness runs.
u64 CompileAndDigestVu1Mtvu(std::initializer_list<vu::VuOp> pairs)
{
	const bool savedThread = EmuConfig.Speedhacks.vuThread;
	const u32 savedInterrupts = vu1Thread.mtvuInterrupts.load(std::memory_order_relaxed);
	EmuConfig.Speedhacks.vuThread = true;

	const u64 digest = CompileAndDigestVu1(pairs);

	EmuConfig.Speedhacks.vuThread = savedThread;
	vu1Thread.mtvuInterrupts.store(savedInterrupts, std::memory_order_relaxed);
	return digest;
}

// vuClampMode:2 (vu0Overflow + vu0ExtraOverflow, sign off) — the mode where the
// FMAC path emits operand and result clamps. 113 GameIndex entries select it.
u64 CompileAndDigestClampE(std::initializer_list<vu::VuOp> pairs)
{
	const bool savedOverflow = EmuConfig.Cpu.Recompiler.vu0Overflow;
	const bool savedExtra    = EmuConfig.Cpu.Recompiler.vu0ExtraOverflow;
	const bool savedSign     = EmuConfig.Cpu.Recompiler.vu0SignOverflow;
	EmuConfig.Cpu.Recompiler.vu0Overflow     = true;
	EmuConfig.Cpu.Recompiler.vu0ExtraOverflow = true;
	EmuConfig.Cpu.Recompiler.vu0SignOverflow = false;

	const u64 digest = CompileAndDigest(pairs);

	EmuConfig.Cpu.Recompiler.vu0Overflow     = savedOverflow;
	EmuConfig.Cpu.Recompiler.vu0ExtraOverflow = savedExtra;
	EmuConfig.Cpu.Recompiler.vu0SignOverflow = savedSign;
	return digest;
}

u64 CompileAndDigestSignClamp(std::initializer_list<vu::VuOp> pairs)
{
	const bool savedOverflow = EmuConfig.Cpu.Recompiler.vu0Overflow;
	const bool savedExtra    = EmuConfig.Cpu.Recompiler.vu0ExtraOverflow;
	const bool savedSign     = EmuConfig.Cpu.Recompiler.vu0SignOverflow;
	EmuConfig.Cpu.Recompiler.vu0Overflow      = true;
	EmuConfig.Cpu.Recompiler.vu0ExtraOverflow = true;
	EmuConfig.Cpu.Recompiler.vu0SignOverflow  = true;

	const u64 digest = CompileAndDigest(pairs);

	EmuConfig.Cpu.Recompiler.vu0Overflow      = savedOverflow;
	EmuConfig.Cpu.Recompiler.vu0ExtraOverflow = savedExtra;
	EmuConfig.Cpu.Recompiler.vu0SignOverflow  = savedSign;
	return digest;
}

u64 CompileAndDigestExact(std::initializer_list<vu::VuOp> pairs)
{
	const bool savedExact = EmuConfig.Cpu.Recompiler.vu0ExactMode;
	EmuConfig.Cpu.Recompiler.vu0ExactMode = true;
	const u64 digest = CompileAndDigestSignClamp(pairs);
	EmuConfig.Cpu.Recompiler.vu0ExactMode = savedExact;
	return digest;
}

// vuClampMode:3 on VU1, which the EFU needs: its ops NOP on VU0.
u64 CompileAndDigestVu1SignClamp(std::initializer_list<vu::VuOp> pairs,
	const char* requireVu1Divergence = nullptr)
{
	const bool savedOverflow = EmuConfig.Cpu.Recompiler.vu1Overflow;
	const bool savedExtra    = EmuConfig.Cpu.Recompiler.vu1ExtraOverflow;
	const bool savedSign     = EmuConfig.Cpu.Recompiler.vu1SignOverflow;
	EmuConfig.Cpu.Recompiler.vu1Overflow      = true;
	EmuConfig.Cpu.Recompiler.vu1ExtraOverflow = true;
	EmuConfig.Cpu.Recompiler.vu1SignOverflow  = true;

	const u64 digest = CompileAndDigestVu1(pairs, requireVu1Divergence);

	EmuConfig.Cpu.Recompiler.vu1Overflow      = savedOverflow;
	EmuConfig.Cpu.Recompiler.vu1ExtraOverflow = savedExtra;
	EmuConfig.Cpu.Recompiler.vu1SignOverflow  = savedSign;
	return digest;
}

u64 CompileAndDigestVu1Exact(std::initializer_list<vu::VuOp> pairs)
{
	const bool savedExact = EmuConfig.Cpu.Recompiler.vu1ExactMode;
	EmuConfig.Cpu.Recompiler.vu1ExactMode = true;
	const u64 digest = CompileAndDigestVu1SignClamp(pairs);
	EmuConfig.Cpu.Recompiler.vu1ExactMode = savedExact;
	return digest;
}

} // namespace

TEST(MvuAbiDigest, EmittedShapePinnedPerAbiVersion)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	mVUPersist::SetRecordingEnabled(true);

	DigestSet actual = {};
	actual.straightLine = CompileAndDigest({
		UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(VMUL_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf4, vf::vf1)),
	});
	actual.branchBothArms = CompileAndDigest({
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, 3)),
		UpperOnly(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)),
		NopPair(),
		UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf6, vf::vf1, vf::vf2)),
	});
	actual.indirectJump = CompileAndDigest({
		LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 4)),
		LowerOnly(VJR_L(vi::vi1)),
		NopPair(),
		NopPair(),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	});
	actual.broadcastChain = CompileAndDigest({
		UpperOnly(VMULAx_U(mask::xyzw, vf::vf3, vf::vf2)),
		UpperOnly(VMADDAy_U(mask::xyzw, vf::vf4, vf::vf2)),
		UpperOnly(VMSUBAz_U(mask::xyzw, vf::vf5, vf::vf2)),
		UpperOnly(bits::E | VMADDw_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf2)),
	},
		// vf5 is zero and vf2.z is -1.0, so the VMSUBAz stage multiplies out a
		// -0. The interpreter carries the multiply stage's sign into the status
		// flag's sticky S; neither emitter does.
		"the status sticky field takes S from the result alone, not from the "
		"multiply stage");
	// Taken branch with a taken conditional branch in its delay slot. The
	// E-bit stays OUT of the evil continuation window (one plain op at #1's
	// target, then #2 lands on a common E-bit tail) — E-bit inside an evil
	// branch is not implemented on x86 either and diverges from the interp.
	actual.condEvilBranch = CompileAndDigest({
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, 2)),                 // pair 0 → pair 3
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, 3)),                 // pair 1 (delay slot) → pair 5
		UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)), // pair 2: skipped
		UpperOnly(VMUL_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)), // pair 3: #1 target (1 op runs)
		UpperOnly(VSUB_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)), // pair 4: skipped
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf6, vf::vf1, vf::vf2)), // pair 5: #2 target
	});

	// The UYA-shape spin loop (all-NOP body, exact NOP encodings). IBNE with
	// the harness's vi1=1 exits immediately at runtime (deterministic Run)
	// while the compile still emits the FF head — the shape being pinned.
	actual.spinLoop = CompileAndDigest({
		VuOp{VIBNE_L(vi::vi1, vi::vi0, 3), 0x000002FFu},  // exit → pair 4
		VuOp{0x8000033Cu, 0x000002FFu},                   // ds NOP
		VuOp{VB_L(-3), 0x000002FFu},                      // back to head
		VuOp{0x8000033Cu, 0x000002FFu},                   // ds NOP
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 42)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	});

	// All three div-unit ops plus a VWAITQ, so the probe covers mVU_DIV,
	// mVU_SQRT and mVU_RSQRT (each of which ends in writeQreg) and the Q read
	// back out. No probe above issues a div-unit op -- the lower-pipe ops they
	// carry are branch and integer shaped -- so without this one the entire
	// div-unit emitter could be rewritten without moving a digest.
	actual.divUnit = CompileAndDigest({
		LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/0)),
		LowerOnly(VSQRT_L(vf::vf2, /*ftf=*/1)),
		LowerOnly(VRSQRT_L(vf::vf1, /*fsf=*/2, vf::vf2, /*ftf=*/3)),
		VuOp{VWAITQ_L(), VNOP_U()},
		UpperOnly(bits::E | VADDq_U(mask::xyzw, vf::vf3, vf::vf1)),
	});

	// The two-step FMACs under vuClampMode:2 -- MADDA/MSUBA are mVU_FMACb's two
	// opTypes, MADD is mVU_FMACc, MSUB is mVU_FMACd. Every probe above compiles
	// at the default clamp mode, where mVUclamp3 and mVUclamp4 never emit -- a
	// quarter of emitted VU code that was invisible to this backstop until now.
	//
	// Two ops per program, not one longer chain: CompileAndDigest forces
	// vuFlagHack on to match production, and a chain of three flag-writing FMACs
	// drops the interpreter's sticky-sign write (STATUS 0x41 against 0xc1),
	// firing Run()'s engine diff on the speedhack instead of on emitted shape.
	actual.maddClampE = CompileAndDigestClampE({
		UpperOnly(VMADDA_U(mask::xyzw, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VMADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	});
	actual.msubClampE = CompileAndDigestClampE({
		UpperOnly(VMSUBA_U(mask::xyzw, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VMSUB_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	});

	// The two gated modes, which no probe above reaches: vuClampMode:3, where
	// the sign-preserving operand clamp emits, and 4, where the FMAC's exact
	// models do. A multiply and an add, and the second pair with a single-lane
	// dest field, which is the only shape that reads the bySSShift half of the
	// weight table.
	const std::initializer_list<vu::VuOp> mulAddProgram = {
		UpperOnly(VMUL_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
	};
	const std::initializer_list<vu::VuOp> ssProgram = {
		UpperOnly(VMULy_U(mask::z, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::z, vf::vf4, vf::vf1, vf::vf2)),
	};
	actual.signClampMulAdd = CompileAndDigestSignClamp(mulAddProgram);
	actual.signClampSS = CompileAndDigestSignClamp(ssProgram);
	actual.exactMulAdd = CompileAndDigestExact(mulAddProgram);
	actual.exactSS = CompileAndDigestExact(ssProgram);
	// The same single-lane program one mode down, where the result clamp is
	// the one mVUclamp1 emits rather than the sign-preserving integer pair.
	actual.clampESS = CompileAndDigestClampE(ssProgram);

	const std::initializer_list<vu::VuOp> partialAccProgram = {
		UpperOnly(VMADDA_U(mask::x | mask::y, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VMADDA_U(mask::z, vf::vf1, vf::vf2)),
	};
	actual.clampEPartialAcc = CompileAndDigestClampE(partialAccProgram);
	actual.signClampPartialAcc = CompileAndDigestSignClamp(partialAccProgram);

	actual.vu1DefaultShapes = CompileAndDigestVu1({
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 5)),
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 5)),
		LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/1)),
		UpperOnly(VMAX_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf0)),
		UpperOnly(VMULw_U(mask::y, vf::vf4, vf::vf1, vf::vf0)),
		UpperOnly(VOPMULA_U(mask::xyz, vf::vf1, vf::vf2)),
		UpperOnly(VOPMSUB_U(mask::xyz, vf::vf5, vf::vf1, vf::vf2)),
		LowerOnly(VFCOR_L(0xFBEFBEu)),
		LowerOnly(VLQ_L(mask::xyzw, vf::vf6, vi::vi2, 1)),
		LowerOnly(VSQ_L(mask::xyzw, vf::vf1, vi::vi3, 2)),
		VuOp{VWAITQ_L(), VNOP_U()},
		UpperOnly(VMADDq_U(mask::w, vf::vf7, vf::vf1)),
		UpperOnly(VADDw_U(mask::y, vf::vf8, vf::vf1, vf::vf2)),
		LowerOnly(VIBEQ_L(vi::vi3, vi::vi2, 3)),
		UpperOnly(VADD_U(mask::xyzw, vf::vf9, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf10, vf::vf1, vf::vf2)),
		NopPair(),
		UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf11, vf::vf1, vf::vf2)),
	},
		// More than four flag writers ahead of the end: the forced flag hack
		// leaves a zero's sticky bit out of the recompiler's status that the
		// interpreter keeps.
		"the flag hack drops a sticky zero the interpreter keeps");
	actual.vu0LoadStore = CompileAndDigest({
		LowerOnly(VLQ_L(mask::xyzw, vf::vf4, vi::vi0, 3)),
		LowerOnly(VSQ_L(mask::xyzw, vf::vf4, vi::vi1, 2)),
		LowerOnly(VILW_L(mask::x, vi::vi2, vi::vi1, 4)),
		LowerOnly(VISWR_L(mask::x, vi::vi2, vi::vi1)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf6, vf::vf4, vf::vf1)),
	});

	// The divUnit program under the same mode. Its three ops keep the host
	// divide everywhere below it, so the arm that calls the model is emitted
	// only here.
	actual.signClampDivUnit = CompileAndDigestSignClamp({
		LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/0)),
		LowerOnly(VSQRT_L(vf::vf2, /*ftf=*/1)),
		LowerOnly(VRSQRT_L(vf::vf1, /*fsf=*/2, vf::vf2, /*ftf=*/3)),
		VuOp{VWAITQ_L(), VNOP_U()},
		UpperOnly(bits::E | VADDq_U(mask::xyzw, vf::vf3, vf::vf1)),
	});

	// The divUnit program under the same two modes, where the arm that calls the
	// model is the only thing between them.
	const std::initializer_list<vu::VuOp> divUnitProgram = {
		LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/0)),
		LowerOnly(VSQRT_L(vf::vf2, /*ftf=*/1)),
		LowerOnly(VRSQRT_L(vf::vf1, /*fsf=*/2, vf::vf2, /*ftf=*/3)),
		VuOp{VWAITQ_L(), VNOP_U()},
		UpperOnly(bits::E | VADDq_U(mask::xyzw, vf::vf3, vf::vf1)),
	};
	actual.signClampDivUnit = CompileAndDigestSignClamp(divUnitProgram);
	actual.exactDivUnit = CompileAndDigestExact(divUnitProgram);

	// VU1 counterpart of branchBothArms: both arms reach a program end, which is
	// the shape the ABI-15 E-bit lookahead forcing touches. Every other probe
	// here is VU0, so without this one a VU1-only emitter change moves no digest.
	// The same program compiles twice, the second time under MTVU, which is what
	// decides the exit those two ends take.
	const std::initializer_list<vu::VuOp> vu1EbitProgram = {
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, 3)),
		UpperOnly(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)),
		NopPair(),
		UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf6, vf::vf1, vf::vf2)),
	};
	actual.vu1BranchToEbit = CompileAndDigestVu1(vu1EbitProgram);
	actual.vu1EbitMtvu = CompileAndDigestVu1Mtvu(vu1EbitProgram);

	// The EFU under the same two modes, on the only VU that has one. A scalar
	// form, a four-lane form and a two-lane form, so a change to how the
	// operands reach the call moves the digest whichever shape it touches.
	const std::initializer_list<vu::VuOp> efuProgram = {
		LowerOnly(VESQRT_L(vf::vf1, /*fsf=*/2)),
		LowerOnly(VESUM_L(vf::vf2)),
		LowerOnly(VEATANXY_L(vf::vf1)),
		VuOp{VWAITP_L(), VNOP_U()},
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	};
	// One mode below the models, the recompiler evaluates all thirteen series in
	// host arithmetic and the interpreter still runs VuEfuModel, so P parts by a
	// couple of ULP. That is the gate doing its job, not a compile fault.
	actual.signClampEfu = CompileAndDigestVu1SignClamp(efuProgram,
		"the EFU's models are a mode above this one");
	actual.exactEfu = CompileAndDigestVu1Exact(efuProgram);

	// The load/store address path. The two vi00 loads take the constant-address
	// fold on either side of a halfword's displacement reach -- one folded
	// whole, one back on the pointer -- the LQD steps off vi00, and the rest
	// address off a live VI. The ILW's dest field is one the lane code and the
	// first-bit-set rule disagree about, so its offset is pinned as well.
	actual.vu1LoadStore = CompileAndDigestVu1({
		LowerOnly(VLQ_L(mask::xyzw, vf::vf4, vi::vi0, 3)),
		LowerOnly(VLQ_L(mask::xyzw, vf::vf5, vi::vi0, 1000)),
		LowerOnly(VSQ_L(mask::xyzw, vf::vf4, vi::vi1, 2)),
		LowerOnly(VLQD_L(mask::xyzw, vf::vf7, vi::vi0)),
		LowerOnly(VILW_L(mask::y | mask::z, vi::vi2, vi::vi1, 4)),
		LowerOnly(VISWR_L(mask::xyzw, vi::vi2, vi::vi1)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf6, vf::vf4, vf::vf5)),
	});

	// Two CLIPs, so the second one's shift of the first one's result is in the
	// shape too. Operands the other way round from every probe above: vf2's
	// components against vf1's w, which is the small one, so the comparisons
	// come out mixed rather than all false.
	actual.clipFlag = CompileAndDigest({
		UpperOnly(VCLIP_U(vf::vf2, vf::vf1)),
		UpperOnly(VCLIP_U(vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	});

	// Queues vf1,vf2,vf3,vf4 in that order, so the preload walk hands the
	// fold two adjacent pairs. Only vf1 and vf2 are seeded, so vf3 and vf4
	// are each read alongside a seeded register rather than each other: a
	// zero result would set MAC Z, and its sticky bit is one the forced
	// flag hack leaves out of the recompiler's status but not the
	// interpreter's, which fails the harness diff for reasons that have
	// nothing to do with the emitted load.
	actual.preloadPairs = CompileAndDigest({
		UpperOnly(VADD_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf2)),
		UpperOnly(VADD_U(mask::xyzw, vf::vf6, vf::vf3, vf::vf2)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf7, vf::vf4, vf::vf1)),
	});

	// Two three-component writes with different masks, so which lane the
	// reversal reads is in the shape twice. Both destinations are written
	// partially, which is what makes mvuPreloadRegisters load them, and the
	// preloaded copy is the one the merge folds against. vf1 and vf2 are the
	// seeded pair and no component of either difference is zero.
	actual.mergeFold = CompileAndDigest({
		UpperOnly(VSUB_U(mask::x | mask::y | mask::z, vf::vf5, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::x | mask::z | mask::w, vf::vf6, vf::vf2, vf::vf1)),
	});

	mVUPersist::SetRecordingEnabled(false);

	ASSERT_NE(actual.straightLine, 0u);
	ASSERT_NE(actual.branchBothArms, 0u);
	ASSERT_NE(actual.indirectJump, 0u);
	ASSERT_NE(actual.broadcastChain, 0u);
	ASSERT_NE(actual.condEvilBranch, 0u);
	ASSERT_NE(actual.vu1BranchToEbit, 0u);
	ASSERT_NE(actual.divUnit, 0u);
	ASSERT_NE(actual.maddClampE, 0u);
	ASSERT_NE(actual.msubClampE, 0u);
	ASSERT_NE(actual.signClampMulAdd, 0u);
	ASSERT_NE(actual.signClampSS, 0u);
	ASSERT_NE(actual.exactMulAdd, 0u);
	ASSERT_NE(actual.exactSS, 0u);
	ASSERT_NE(actual.signClampDivUnit, 0u);
	ASSERT_NE(actual.exactDivUnit, 0u);
	ASSERT_NE(actual.signClampEfu, 0u);
	ASSERT_NE(actual.exactEfu, 0u);
	ASSERT_NE(actual.vu1LoadStore, 0u);
	ASSERT_NE(actual.vu1EbitMtvu, 0u);
	ASSERT_NE(actual.clipFlag, 0u);
	ASSERT_NE(actual.preloadPairs, 0u);
	ASSERT_NE(actual.mergeFold, 0u);
	ASSERT_NE(actual.clampESS, 0u);
	ASSERT_NE(actual.clampEPartialAcc, 0u);
	ASSERT_NE(actual.signClampPartialAcc, 0u);
	ASSERT_NE(actual.vu1DefaultShapes, 0u);
	ASSERT_NE(actual.vu0LoadStore, 0u);
	// MTVU is the only thing between the two, and it has to reach the emitter:
	// equal digests mean the same exit was emitted either way and the probe
	// above pins nothing.
	ASSERT_NE(actual.vu1EbitMtvu, actual.vu1BranchToEbit);

#if !(defined(__linux__) && !defined(__ANDROID__) && defined(__GLIBCXX__))
	// The pinned values embed guest-state field offsets baked into the emitted
	// code, and those offsets shift with the C++ standard library's struct
	// layout (libc++ containers are smaller than libstdc++'s — every kPins row
	// drifts wholesale on macOS, first seen on the macOS CI leg 2026-07-19).
	// Pins are harvested on desktop Linux/libstdc++ where development happens;
	// on other platform ABIs the digest machinery is still exercised above and
	// by EmittedShapeIndependentOfPriorCompile, but the values can't be
	// compared against the Linux table.
	GTEST_SKIP() << "digest pins are harvested on Linux/libstdc++; this "
					"platform's std-library struct layout shifts the emitted "
					"field offsets, so the pinned values do not apply";
#endif

	const u32 abi = mVUProgCache::GetCompilerAbiVersion();
	const AbiPin* pin = nullptr;
	for (const AbiPin& p : kPins)
	{
		if (p.abi == abi)
			pin = &p;
	}
	ASSERT_NE(pin, nullptr)
		<< "kMvuCompilerAbiVersion=" << abi << " has no digest pin — add a "
		<< "row to kPins with the values printed below.\n"
		<< "  actual: {0x" << std::hex << actual.straightLine
		<< ", 0x" << actual.branchBothArms
		<< ", 0x" << actual.indirectJump
		<< ", 0x" << actual.broadcastChain
		<< ", 0x" << actual.condEvilBranch
		<< ", 0x" << actual.spinLoop
		<< ", 0x" << actual.vu1BranchToEbit
		<< ", 0x" << actual.divUnit
		<< ", 0x" << actual.maddClampE
		<< ", 0x" << actual.msubClampE
		<< ", 0x" << actual.signClampMulAdd
		<< ", 0x" << actual.signClampSS
		<< ", 0x" << actual.exactMulAdd
		<< ", 0x" << actual.exactSS
		<< ", 0x" << actual.signClampDivUnit
		<< ", 0x" << actual.exactDivUnit
		<< ", 0x" << actual.signClampEfu
		<< ", 0x" << actual.exactEfu
		<< ", 0x" << actual.vu1LoadStore
		<< ", 0x" << actual.vu1EbitMtvu
		<< ", 0x" << actual.clipFlag
		<< ", 0x" << actual.preloadPairs
		<< ", 0x" << actual.mergeFold
		<< ", 0x" << actual.clampESS
		<< ", 0x" << actual.clampEPartialAcc
		<< ", 0x" << actual.signClampPartialAcc
		<< ", 0x" << actual.vu1DefaultShapes
		<< ", 0x" << actual.vu0LoadStore << "}";

	const auto explain = [&](const char* which, u64 got, u64 want) {
		char buf[256];
		std::snprintf(buf, sizeof(buf),
			"%s digest drifted for ABI v%u: got 0x%016" PRIx64 ", pinned 0x%016" PRIx64 ".\n"
			"Emitted code shape changed — bump kMvuCompilerAbiVersion (emitter "
			"change) or re-pin (config-default change). See file header.",
			which, abi, got, want);
		return std::string(buf);
	};
	EXPECT_EQ(actual.straightLine, pin->digests.straightLine)
		<< explain("straightLine", actual.straightLine, pin->digests.straightLine);
	EXPECT_EQ(actual.branchBothArms, pin->digests.branchBothArms)
		<< explain("branchBothArms", actual.branchBothArms, pin->digests.branchBothArms);
	EXPECT_EQ(actual.indirectJump, pin->digests.indirectJump)
		<< explain("indirectJump", actual.indirectJump, pin->digests.indirectJump);
	if (pin->digests.broadcastChain != 0) // probe added at abi 6; older rows unpinned
	{
		EXPECT_EQ(actual.broadcastChain, pin->digests.broadcastChain)
			<< explain("broadcastChain", actual.broadcastChain, pin->digests.broadcastChain);
	}
	if (pin->digests.condEvilBranch != 0) // probe added at abi 7; older rows unpinned
	{
		EXPECT_EQ(actual.condEvilBranch, pin->digests.condEvilBranch)
			<< explain("condEvilBranch", actual.condEvilBranch, pin->digests.condEvilBranch);
	}
	if (pin->digests.spinLoop != 0) // probe added at abi 13; older rows unpinned
	{
		EXPECT_EQ(actual.spinLoop, pin->digests.spinLoop)
			<< explain("spinLoop", actual.spinLoop, pin->digests.spinLoop);
	}
	if (pin->digests.vu1BranchToEbit != 0) // probe added at abi 15; older rows unpinned
	{
		EXPECT_EQ(actual.vu1BranchToEbit, pin->digests.vu1BranchToEbit)
			<< explain("vu1BranchToEbit", actual.vu1BranchToEbit, pin->digests.vu1BranchToEbit);
	}
	if (pin->digests.divUnit != 0) // probe added at abi 17; older rows unpinned
	{
		EXPECT_EQ(actual.divUnit, pin->digests.divUnit)
			<< explain("divUnit", actual.divUnit, pin->digests.divUnit);
	}
	if (pin->digests.maddClampE != 0) // probe added at abi 17; older rows unpinned
	{
		EXPECT_EQ(actual.maddClampE, pin->digests.maddClampE)
			<< explain("maddClampE", actual.maddClampE, pin->digests.maddClampE);
	}
	if (pin->digests.msubClampE != 0) // probe added at abi 18; older rows unpinned
	{
		EXPECT_EQ(actual.msubClampE, pin->digests.msubClampE)
			<< explain("msubClampE", actual.msubClampE, pin->digests.msubClampE);
	}
	// The four exact-mode probes call the EE FPU / EFU model, and a call site
	// spills what EEFPU_MODEL_CALL does not spare (EeFpuModel.h). The compiler
	// picks that: clang-cl has no mangling for preserve_all and takes the wide
	// spill, a shape the pin table carries no row for.
	const bool modelCallShapePinned = EEFPU_MODEL_CALL_SPARES_MOST != 0;
	if (pin->digests.signClampMulAdd != 0) // probes added at abi 19; older rows unpinned
	{
		EXPECT_EQ(actual.signClampMulAdd, pin->digests.signClampMulAdd)
			<< explain("signClampMulAdd", actual.signClampMulAdd, pin->digests.signClampMulAdd);
		EXPECT_EQ(actual.signClampSS, pin->digests.signClampSS)
			<< explain("signClampSS", actual.signClampSS, pin->digests.signClampSS);
		if (modelCallShapePinned)
		{
			EXPECT_EQ(actual.exactMulAdd, pin->digests.exactMulAdd)
				<< explain("exactMulAdd", actual.exactMulAdd, pin->digests.exactMulAdd);
			EXPECT_EQ(actual.exactSS, pin->digests.exactSS)
				<< explain("exactSS", actual.exactSS, pin->digests.exactSS);
		}
	}
	if (pin->digests.signClampDivUnit != 0) // probes added at abi 19; older rows unpinned
	{
		EXPECT_EQ(actual.signClampDivUnit, pin->digests.signClampDivUnit)
			<< explain("signClampDivUnit", actual.signClampDivUnit, pin->digests.signClampDivUnit);
		if (modelCallShapePinned)
		{
			EXPECT_EQ(actual.exactDivUnit, pin->digests.exactDivUnit)
				<< explain("exactDivUnit", actual.exactDivUnit, pin->digests.exactDivUnit);
		}
	}
	if (pin->digests.signClampEfu != 0) // probes added at abi 19; older rows unpinned
	{
		EXPECT_EQ(actual.signClampEfu, pin->digests.signClampEfu)
			<< explain("signClampEfu", actual.signClampEfu, pin->digests.signClampEfu);
		if (modelCallShapePinned)
		{
			EXPECT_EQ(actual.exactEfu, pin->digests.exactEfu)
				<< explain("exactEfu", actual.exactEfu, pin->digests.exactEfu);
		}
	}
	if (pin->digests.vu1EbitMtvu != 0) // probe added at abi 28; older rows unpinned
	{
		EXPECT_EQ(actual.vu1EbitMtvu, pin->digests.vu1EbitMtvu)
			<< explain("vu1EbitMtvu", actual.vu1EbitMtvu, pin->digests.vu1EbitMtvu);
	}
	if (pin->digests.clipFlag != 0) // probe added at abi 29; older rows unpinned
	{
		EXPECT_EQ(actual.clipFlag, pin->digests.clipFlag)
			<< explain("clipFlag", actual.clipFlag, pin->digests.clipFlag);
	}
	if (pin->digests.preloadPairs != 0) // probe added at abi 31; older rows unpinned
	{
		EXPECT_EQ(actual.preloadPairs, pin->digests.preloadPairs)
			<< explain("preloadPairs", actual.preloadPairs, pin->digests.preloadPairs);
	}
	if (pin->digests.mergeFold != 0) // probe added at abi 32; older rows unpinned
	{
		EXPECT_EQ(actual.mergeFold, pin->digests.mergeFold)
			<< explain("mergeFold", actual.mergeFold, pin->digests.mergeFold);
	}
	if (pin->digests.clampESS != 0) // probe added at abi 34; older rows unpinned
	{
		EXPECT_EQ(actual.clampESS, pin->digests.clampESS)
			<< explain("clampESS", actual.clampESS, pin->digests.clampESS);
	}
	if (pin->digests.clampEPartialAcc != 0) // probes added at abi 37; older rows unpinned
	{
		EXPECT_EQ(actual.clampEPartialAcc, pin->digests.clampEPartialAcc)
			<< explain("clampEPartialAcc", actual.clampEPartialAcc, pin->digests.clampEPartialAcc);
		EXPECT_EQ(actual.signClampPartialAcc, pin->digests.signClampPartialAcc)
			<< explain("signClampPartialAcc", actual.signClampPartialAcc, pin->digests.signClampPartialAcc);
	}
	if (pin->digests.vu1DefaultShapes != 0) // probes added at abi 57; older rows unpinned
	{
		EXPECT_EQ(actual.vu1DefaultShapes, pin->digests.vu1DefaultShapes)
			<< explain("vu1DefaultShapes", actual.vu1DefaultShapes, pin->digests.vu1DefaultShapes);
		EXPECT_EQ(actual.vu0LoadStore, pin->digests.vu0LoadStore)
			<< explain("vu0LoadStore", actual.vu0LoadStore, pin->digests.vu0LoadStore);
	}
	if (pin->digests.vu1LoadStore != 0) // probe added at abi 24; older rows unpinned
	{
		EXPECT_EQ(actual.vu1LoadStore, pin->digests.vu1LoadStore)
			<< explain("vu1LoadStore", actual.vu1LoadStore, pin->digests.vu1LoadStore);
	}
	ASSERT_NE(actual.spinLoop, 0u);
}

// A program's emitted shape must depend ONLY on the program — never on what
// compiled before it. mVUcompile passes endCount = whole-micro-memory size to
// mvuPreloadRegisters; the preload walks until it runs out of free registers,
// not until the block ends. mVUreset does NOT clear mVU.prog.IRinfo.info[], so
// an E-bit-terminated block whose preload over-runs its own end reads stale
// VF/VI usage left by a PRIOR compile and preloads registers the program never
// touches. The runtime result is identical (an unused reg load), but the
// emitted bytes drift with compile history — which corrupts the persisted-JIT
// ABI digest's "same emitter ⇒ same shape" contract. The mvuPreloadRegisters
// isEOB break is the fix; this is its deterministic regression guard (the main
// pin test only catches it under --gtest_shuffle, which CI may not run).
TEST(MvuAbiDigest, EmittedShapeIndependentOfPriorCompile)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	mVUPersist::SetRecordingEnabled(true);

	// The probe: a short pure-FMAC, E-bit-terminated block (no branch — so the
	// only thing that can bound its preload is the isEOB break).
	const auto probe = {
		UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(bits::E | VMUL_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
	};

	// Two "polluter" programs, longer than the probe, that leave DIFFERENT VI
	// read-usage in IRinfo.info[] at indices PAST the probe's own end. The probe
	// (2 ops + E-bit delay) clears info[0..~3]; the leading NOPs push the
	// VI-reading VIADDs out to indices >= 4 so they survive the probe's analysis.
	// VIADD reads two source VIs. If the probe's preload over-runs its block end,
	// it picks up these (differing) VIs and the two digests diverge.
	const auto polluteViLow = {
		NopPair(), NopPair(), NopPair(), NopPair(),
		LowerOnly(VIADD_L(vi::vi3, vi::vi5, vi::vi6)),
		LowerOnly(VIADD_L(vi::vi3, vi::vi5, vi::vi6)),
		LowerOnly(VIADD_L(vi::vi3, vi::vi5, vi::vi6)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	};
	const auto polluteViHigh = {
		NopPair(), NopPair(), NopPair(), NopPair(),
		LowerOnly(VIADD_L(vi::vi3, vi::vi9, vi::vi10)),
		LowerOnly(VIADD_L(vi::vi3, vi::vi9, vi::vi10)),
		LowerOnly(VIADD_L(vi::vi3, vi::vi9, vi::vi10)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	};

	(void)CompileAndDigest(polluteViLow);
	const u64 afterLow = CompileAndDigest(probe);
	(void)CompileAndDigest(polluteViHigh);
	const u64 afterHigh = CompileAndDigest(probe);

	mVUPersist::SetRecordingEnabled(false);

	ASSERT_NE(afterLow, 0u);
	EXPECT_EQ(afterLow, afterHigh)
		<< "Probe digest changed with the preceding compile (0x" << std::hex
		<< afterLow << " after VIADD vi5,vi6 vs 0x" << afterHigh
		<< " after VIADD vi9,vi10) — mvuPreloadRegisters over-ran the block end "
		   "into stale IRinfo.info[]. Emitted shape must be history-independent.";
}

// The vu-aot tripwire's hook on VU1's STATUS quadword of VU0's register window is emitted only while
// the tier is wanted (section 9ad, ABI 61): with EnableVUAot off VU0's load/store shape is the one
// from before the hook (the current pin, vu0LoadStore), and with it on it is the hook's shape. Rows
// 59 to 62 of the table were computed before the digest changed with ABI 63 and no longer compare
// with anything; the hook-on shape is pinned here instead. VU0's reset (the harness resets before
// each compile) is where the choice is taken, as VMManager's settings change resets both VUs.
TEST(MvuAbiDigest, Vu0WindowHookOnlyWhileVuAotIsWanted)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	mVUPersist::SetRecordingEnabled(true);
	const auto findPin = [](u32 abi) -> u64 {
		for (const AbiPin& p : kPins)
			if (p.abi == abi)
				return p.digests.vu0LoadStore;
		return 0;
	};
	const auto probe = {
		LowerOnly(VLQ_L(mask::xyzw, vf::vf4, vi::vi0, 3)),
		LowerOnly(VSQ_L(mask::xyzw, vf::vf4, vi::vi1, 2)),
		LowerOnly(VILW_L(mask::x, vi::vi2, vi::vi1, 4)),
		LowerOnly(VISWR_L(mask::x, vi::vi2, vi::vi1)),
		UpperOnly(bits::E | VADD_U(mask::xyzw, vf::vf6, vf::vf4, vf::vf1)),
	};
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUAot;
	EmuConfig.Cpu.Recompiler.EnableVUAot = false;
	const u64 off = CompileAndDigest(probe);
	const bool hookOff = mVUvuAotVU0WindowHook();
	EmuConfig.Cpu.Recompiler.EnableVUAot = true;
	const u64 on = CompileAndDigest(probe);
	const bool hookOn = mVUvuAotVU0WindowHook();
	EmuConfig.Cpu.Recompiler.EnableVUAot = savedOpt;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	mVUPersist::SetRecordingEnabled(false);

	EXPECT_FALSE(hookOff);
	EXPECT_TRUE(hookOn);
	const u32 abi = mVUProgCache::GetCompilerAbiVersion();
	EXPECT_EQ(off, findPin(abi)) << std::hex << "setting off: 0x" << off << ", abi " << std::dec << abi << " pinned 0x" << std::hex << findPin(abi);
	EXPECT_NE(on, off) << "the hook changed nothing";
	// Pinned from the merged code at abi 63. Re-pin it with the table when an emitter change moves it.
	constexpr u64 kHookOnDigestAbi63 = 0xccc45e874390054b;
	EXPECT_EQ(abi, 63u) << "the hook-on pin is for abi 63: re-pin kHookOnDigestAbi63";
	EXPECT_EQ(on, kHookOnDigestAbi63) << std::hex << "setting on: 0x" << on << ", pinned 0x" << kHookOnDigestAbi63;
}

} // namespace recompiler_tests
