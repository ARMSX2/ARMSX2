// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/HashCombine.h"

#include "vixl/aarch64/constants-aarch64.h"
#include "vixl/aarch64/macro-assembler-aarch64.h"

#include <unordered_map>

#define RWRET vixl::aarch64::w0
#define RXRET vixl::aarch64::x0
#define RQRET vixl::aarch64::q0

#define RWARG1 vixl::aarch64::w0
#define RWARG2 vixl::aarch64::w1
#define RWARG3 vixl::aarch64::w2
#define RWARG4 vixl::aarch64::w3
#define RXARG1 vixl::aarch64::x0
#define RXARG2 vixl::aarch64::x1
#define RXARG3 vixl::aarch64::x2
#define RXARG4 vixl::aarch64::x3

#define RXVIXLSCRATCH vixl::aarch64::x16  // Reserved for VIXL internal use — do NOT use in rec code
#define RWVIXLSCRATCH vixl::aarch64::w16  // Reserved for VIXL internal use — do NOT use in rec code
#define RSCRATCHADDR vixl::aarch64::x17   // Address scratch — removed from VIXL pool in armStartBlock

// General-purpose value scratch registers for recompiler use.
// These are caller-saved and NOT in VIXL's scratch pool.
#define RXSCRATCH vixl::aarch64::x8
#define RWSCRATCH vixl::aarch64::w8

#define RQSCRATCH vixl::aarch64::q30
#define RDSCRATCH vixl::aarch64::d30
#define RSSCRATCH vixl::aarch64::s30
#define RQSCRATCH2 vixl::aarch64::q31
#define RDSCRATCH2 vixl::aarch64::d31
#define RSSCRATCH2 vixl::aarch64::s31
#define RQSCRATCH3 vixl::aarch64::q29
#define RDSCRATCH3 vixl::aarch64::d29
#define RSSCRATCH3 vixl::aarch64::s29

#define RQSCRATCHI vixl::aarch64::VRegister(30, 128, 16)
#define RQSCRATCHF vixl::aarch64::VRegister(30, 128, 4)
#define RQSCRATCHD vixl::aarch64::VRegister(30, 128, 2)

#define RQSCRATCH2I vixl::aarch64::VRegister(31, 128, 16)
#define RQSCRATCH2F vixl::aarch64::VRegister(31, 128, 4)
#define RQSCRATCH2D vixl::aarch64::VRegister(31, 128, 2)



static inline s64 GetPCDisplacement(const void* current, const void* target)
{
	return static_cast<s64>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)) >> 2);
}

const vixl::aarch64::Register& armWRegister(int n);
const vixl::aarch64::Register& armXRegister(int n);
const vixl::aarch64::VRegister& armSRegister(int n);
const vixl::aarch64::VRegister& armDRegister(int n);
const vixl::aarch64::VRegister& armQRegister(int n);

class ArmConstantPool;

// Address-emission observer for the on-disk VU program cache. While a
// recorder is attached (mVU code-cache episodes only — see mVUopenCodeCache),
// the emit helpers below report every host-address-bearing emission so the
// recorder can build a relocation fixup table, and let it force canonical
// fixed-width forms where the default encoding couldn't be patched after the
// code block moves:
//   - armMoveAddressToReg of a volatile (heap) target → movz+movk×3 (16 bytes,
//     patchable) instead of the shortest mov/adrp form.
//   - armEmitCondBranch to a relocatable target → inverted-cond skip + B imm26
//     (B.cond's ±1MB imm19 can't survive arbitrary replacement).
// `at` arguments are the address of the first emitted instruction of the
// reported shape. All hooks are no-ops when no recorder is attached.
class ArmAddressRecorder
{
public:
	enum class MoveForm
	{
		Default, // emit in shortest form; recorder may still log it
		CanonicalAbs, // emit fixed-width movz+movk×3 so the operand is patchable
	};

	virtual ~ArmAddressRecorder() = default;

	// armMoveAddressToReg: pick the emission form for `addr`.
	virtual MoveForm ClassifyMove(const void* addr) = 0;
	// armMoveAddressToReg emitted the canonical 16-byte movz+movk×3 at `at`.
	virtual void OnCanonicalAbsMove(u8* at, const void* addr) = 0;
	// armMoveAddressToReg emitted ADRP (+Add/Orr) at `at`; the page offset is
	// PC-relative and must be re-paged if this code moves.
	virtual void OnAdrp(u8* at, const void* addr) = 0;
	// armEmitJmp/armEmitCall/armEmitCondBranch emitted a direct B/BL imm26 at
	// `at` targeting `target`.
	virtual void OnDirectBranch(u8* at, const void* target, bool is_call) = 0;
	// armEmitCondBranch: return true to force the long (cond-skip + B) form.
	virtual bool WantsLongCondBranch(const void* target) = 0;
	// An absolute (movz/movk-materialized) target with no patch site — emitted
	// by the out-of-range paths of armEmitJmp/armEmitCall/armMoveAddressToReg.
	// Recorder uses this to verify the target is run-invariant.
	virtual void OnAbsoluteTarget(const void* target) = 0;
};

static const u32 SP_SCRATCH_OFFSET = 0;

extern thread_local vixl::aarch64::MacroAssembler* armAsm;
extern thread_local u8* armAsmPtr;
extern thread_local size_t armAsmCapacity;
extern thread_local ArmConstantPool* armConstantPool;
extern thread_local ArmAddressRecorder* armAddressRecorder;

static __fi bool armHasBlock()
{
	return (armAsm != nullptr);
}

static __fi u8* armGetCurrentCodePointer()
{
	return static_cast<u8*>(armAsmPtr) + armAsm->GetCursorOffset();
}

__fi static u8* armGetAsmPtr()
{
	return armAsmPtr;
}

void armSetAsmPtr(void* ptr, size_t capacity, ArmConstantPool* pool);
void armAlignAsmPtr();
// iOS dual-map W^X: RX pointer -> writable alias (rx + g_code_rw_offset).
// Identity everywhere else (and on Apple platforms where the offset is 0).
// EVERY store into the code region that bypasses armAsm's buffer must route
// its write pointer through this; displacements/flushes stay on the RX ptr.
u8* armGetWritableCodePtr(u8* rx_ptr);
u8* armStartBlock();
u8* armEndBlock();

void armDisassembleAndDumpCode(const void* ptr, size_t size);
void armEmitJmp(const void* ptr, bool force_inline = false);
void armEmitCall(const void* ptr, bool force_inline = false);
// Store one instruction word into code memory, opening its own W^X scope when
// the target lies outside the open emit window. Use for anything that patches
// code outside the block being emitted — link sites, entry stubs, fastmem
// backpatch. No cache maintenance: callers own the flush policy.
void armPatchCodeWord(void* site, u32 instr);
// In-place patch: overwrite the 4-byte B at `code_address` with a branch to
// `target`. Used by EE block chaining to rewrite a link site (not tied to the
// current emit cursor). `code_address` must already hold a single B instruction.
void armEmitJmpPtr(void* code_address, const void* target, bool flush_icache = true);
void armEmitCbnz(const vixl::aarch64::Register& reg, const void* ptr);
void armEmitCondBranch(vixl::aarch64::Condition cond, const void* ptr);
void armMoveAddressToReg(const vixl::aarch64::Register& reg, const void* addr);
void armLoadPtr(const vixl::aarch64::CPURegister& reg, const void* addr);
void armStorePtr(const vixl::aarch64::CPURegister& reg, const void* addr);
void armBeginStackFrame(bool save_fpr);
void armEndStackFrame(bool save_fpr);
bool armIsCalleeSavedRegister(int reg);

// Emits the EE JIT's caller-saved pin flush-before / reload-after (see
// kEEPinTable in iR5900-arm64.h). Out-of-line bridges for emission contexts
// that can't include the EE rec header — currently only mVU macro-mode emit
// bodies that emit C calls inline into EE blocks (mVUaddrFix's waitMTVU).
// The flush is a lazy-dirty-mode no-op (EE_PIN_LAZY_DIRTY).
void armEmitEEClobberedPinFlushForCOP2();
void armEmitEEClobberedPinReloadForCOP2();

vixl::aarch64::MemOperand armOffsetMemOperand(const vixl::aarch64::MemOperand& op, s64 offset);
void armGetMemOperandInRegister(const vixl::aarch64::Register& addr_reg,
	const vixl::aarch64::MemOperand& op, s64 extra_offset = 0);

void armLoadConstant128(const vixl::aarch64::VRegister& reg, const void* ptr);

// Per-lane weight for armEmitPackSignZeroBits' combined weight vector.
//
// Lane `lane` contributes its zero bit at result bit `bit` and its sign bit at
// bit `bit + 4`, where bit = reverse ? (3 - lane) : lane — PS2 MAC flag order
// is bit0=W, bit3=X, the reverse of NEON lane order, which is what x86 pays a
// PSHUF.D 0x1B for. Lanes outside `keepMask` weigh zero, so the dest-field
// mask (x86's AND_XYZW) costs nothing; `shift` likewise absorbs x86's
// single-scalar SHIFT_XYZW rotate. Both are emit-time constants.
//
// `withUnderflow` adds the MAC U nibble at bit + 8, for the callers that pass
// armEmitPackSignZeroBits a third predicate.
//
// The caller must keep `bit + shift <= 3` so the zero half stays inside the
// low nibble — see the SLI note in armEmitPackSignZeroBits.
static constexpr u32 armPackLaneWeight(int lane, u32 keepMask, bool reverse, int shift,
	bool withUnderflow = false)
{
	const int bit = reverse ? (3 - lane) : lane;
	if (!(keepMask & (1u << bit)))
		return 0;
	const u32 w = 1u << (bit + shift);
	return w | (w << 4) | (withUnderflow ? (w << 8) : 0);
}

// Pack the sign and zero predicates of a 4-lane float vector into one GPR as
// the PS2's 8-bit "sign nibble : zero nibble" MAC value, with a SINGLE
// horizontal add.
//
// The lever is SLI. CMLT/FCMEQ both yield all-ones-or-zero lanes, so
//   sli vZero.4s, vSign.4s, #4
// leaves bits [31:4] = sign and bits [3:0] = zero in one register, and a
// single AND against armPackLaneWeight's vector then selects lane i's sign
// into bit (i+4) and its zero into bit i. The per-lane bit sets are disjoint,
// so ADDV's sum is an OR. That replaces the two independent movemask
// sequences (two AND/ADDV/UMOV chains plus the GPR AND/SHL/AND/OR that glue
// them) the x86 path needs, and the weight vector swallows the field mask and
// the single-scalar rotate on the way past.
//
// `vSign` and `vZero` are clobbered; `vZero` receives the result. `weights`
// may alias `vSign` — the sign predicate is dead by the time load_weights
// runs, which is what lets the whole thing work in the two temporaries the
// caller already had. load_weights(weights) emits the weight-vector load;
// callers choose their cheapest route to it (a pinned-base Ldr in microVU, a
// literal in the EE's COP2 macro path).
//
// `vExactZero`, when given, is a third temporary holding all-ones per lane
// where a zero result is an exact zero rather than a flushed underflow. It is
// turned into the U predicate here (a zero result that was NOT exact) and
// inserted at nibble 2, so the caller's weights must come from
// armPackLaneWeight with withUnderflow set. The second SLI shifts by 8 and so
// leaves the sign nibble at [7:4] and the zero nibble at [3:0] standing; the
// order of the two inserts is therefore fixed.
//
// Emits 6 insns + the weight load, 8 with vExactZero.
template <typename LoadWeightsFn>
__fi static void armEmitPackSignZeroBits(const vixl::aarch64::Register& dst,
	const vixl::aarch64::VRegister& src, const vixl::aarch64::VRegister& vSign,
	const vixl::aarch64::VRegister& vZero, const vixl::aarch64::VRegister& weights,
	LoadWeightsFn&& load_weights,
	const vixl::aarch64::VRegister& vExactZero = vixl::aarch64::NoVReg)
{
	armAsm->Cmlt(vSign.V4S(), src.V4S(), 0);    // all-1s where negative
	armAsm->Fcmeq(vZero.V4S(), src.V4S(), 0.0); // all-1s where zero
	if (vExactZero.IsValid())
		armAsm->Bic(vExactZero.V16B(), vZero.V16B(), vExactZero.V16B()); // zero, not exactly
	armAsm->Sli(vZero.V4S(), vSign.V4S(), 4);   // [31:4] = sign, [3:0] = zero
	if (vExactZero.IsValid())
		armAsm->Sli(vZero.V4S(), vExactZero.V4S(), 8); // [31:8] = underflow
	load_weights(weights);
	armAsm->And(vZero.V16B(), vZero.V16B(), weights.V16B());
	armAsm->Addv(vixl::aarch64::VRegister(vZero.GetCode(), 32), vZero.V4S());
	armAsm->Fmov(dst, vixl::aarch64::VRegister(vZero.GetCode(), 32));
}

// The EE FPR word <-> stored double relocation of EeFpuFormat.h, emitted.
//
// Sign-extending the word puts its sign bit at 63 but also fills 62..60; this
// clears those three. 61 contiguous ones under rotation, so a logical immediate.
static constexpr u64 kEeFprWidenMask = 0x8FFFFFFFFFFFFFFFULL;

// dst = widen(word). `tmp` is an X scratch and may be `word`'s X form. Only
// the low 32 bits of `word` are read.
__fi static void armEmitEeFprWiden(const vixl::aarch64::VRegister& dst,
	const vixl::aarch64::Register& word, const vixl::aarch64::Register& tmp)
{
	armAsm->Sbfiz(tmp.X(), word.X(), 29, 32);
	armAsm->And(tmp.X(), tmp.X(), static_cast<int64_t>(kEeFprWidenMask));
	armAsm->Fmov(dst.D(), tmp.X());
}

// dst = widen(*src). Ldrsw does the sign extension the widen needs anyway.
__fi static void armEmitEeFprWidenFromMem(const vixl::aarch64::VRegister& dst,
	const vixl::aarch64::MemOperand& src, const vixl::aarch64::Register& tmp)
{
	armAsm->Ldrsw(tmp.X(), src);
	armAsm->Lsl(tmp.X(), tmp.X(), 29);
	armAsm->And(tmp.X(), tmp.X(), static_cast<int64_t>(kEeFprWidenMask));
	armAsm->Fmov(dst.D(), tmp.X());
}

// dst = narrow(src), zero-extended: dst.W() is the word and dst.X() is that
// word with the top half clear. `tmp` is an X scratch and must not be `dst`.
__fi static void armEmitEeFprNarrow(const vixl::aarch64::Register& dst,
	const vixl::aarch64::VRegister& src, const vixl::aarch64::Register& tmp)
{
	armAsm->Fmov(tmp.X(), src.D());
	armAsm->Lsr(dst.X(), tmp.X(), 32);
	armAsm->Bfxil(dst.X(), tmp.X(), 29, 31);
}

// *dst = narrow(src). `word` receives the narrowed word; neither scratch may
// be the other.
__fi static void armEmitEeFprNarrowToMem(const vixl::aarch64::MemOperand& dst,
	const vixl::aarch64::VRegister& src, const vixl::aarch64::Register& word,
	const vixl::aarch64::Register& tmp)
{
	armEmitEeFprNarrow(word, src, tmp);
	armAsm->Str(word.W(), dst);
}

// The same relocation against an fpuRegs slot: memory holds the stored double
// and a register holds the architectural word.

// dst = the word in *slot, zero-extended. `tmp` is an X scratch, not `dst`.
__fi static void armEmitEeFprLoadSlotWord(const vixl::aarch64::Register& dst,
	const vixl::aarch64::MemOperand& slot, const vixl::aarch64::Register& tmp)
{
	armAsm->Ldr(tmp.X(), slot);
	armAsm->Lsr(dst.X(), tmp.X(), 32);
	armAsm->Bfxil(dst.X(), tmp.X(), 29, 31);
}

// *slot = `word`. `tmp` is an X scratch and may be `word`'s X form.
__fi static void armEmitEeFprStoreSlotWord(const vixl::aarch64::MemOperand& slot,
	const vixl::aarch64::Register& word, const vixl::aarch64::Register& tmp)
{
	armAsm->Sbfiz(tmp.X(), word.X(), 29, 32);
	armAsm->And(tmp.X(), tmp.X(), static_cast<int64_t>(kEeFprWidenMask));
	armAsm->Str(tmp.X(), slot);
}

// Bridge for emitters holding a result as the architectural single in an S
// register while the slot they store it to holds the stored double: iFPUd's
// bodies, the fast path's SQRT, and LWC1's fastmem load. Clobbers `tmp`.
__fi static void armEmitEeFprFromS(const vixl::aarch64::VRegister& slot,
	const vixl::aarch64::VRegister& src, const vixl::aarch64::Register& tmp)
{
	armAsm->Fmov(tmp.W(), src.S());
	armEmitEeFprWiden(slot, tmp, tmp);
}

// may clobber RSCRATCH/RSCRATCH2. they shouldn't be inputs.
void armEmitVTBL(const vixl::aarch64::VRegister& dst, const vixl::aarch64::VRegister& src1,
	const vixl::aarch64::VRegister& src2, const vixl::aarch64::VRegister& tbl);

//////////////////////////////////////////////////////////////////////////

class ArmConstantPool
{
public:
	void Init(void* ptr, u32 capacity);
	void Destroy();
	void Reset();

	u8* GetJumpTrampoline(const void* target);
	u8* GetLiteral(u64 value);
	u8* GetLiteral(const u128& value);
	u8* GetLiteral(const u8* bytes, size_t len);
	// Contiguous 8-aligned copy of an arbitrary-length blob (no dedup).
	// Returns nullptr when the pool is full — callers must have a fallback.
	u8* GetBlob(const u8* bytes, size_t len);

	void EmitLoadLiteral(const vixl::aarch64::CPURegister& reg, const u8* literal) const;

private:
	__fi u32 GetRemainingCapacity() const { return m_capacity - m_used; }

	struct u128_hash
	{
		std::size_t operator()(const u128& v) const
		{
			std::size_t s = 0;
			HashCombine(s, v.lo, v.hi);
			return s;
		}
	};

	std::unordered_map<const void*, u32> m_jump_targets;
	std::unordered_map<u128, u32, u128_hash> m_literals;

	u8* m_base_ptr = nullptr;
	u32 m_capacity = 0;
	u32 m_used = 0;
};


