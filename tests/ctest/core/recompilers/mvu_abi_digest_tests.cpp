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
// immediates, B/BL displacements, ADRP pages, fixup address payloads — and
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
// stores recording-enabled forms (canonical movs, forced-long cond
// branches) — those are the shapes worth pinning.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "EeFpuModel.h"
#include "VU.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Config.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"

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
// conditional branch both-arms (block linking, SelfBlockAbs fixups), and
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
	// exit BLs copyPLStateResume — a new stub id in the fixup stream. The
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
		<< ", 0x" << actual.preloadPairs << "}";

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

} // namespace recompiler_tests
