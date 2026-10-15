// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Decoder for VU micro instruction pairs, for the whole-program VU1 compiler
// (vu-aot). One entry per mnemonic, with the operand fields every later stage
// needs. The tables follow microVU's dispatch tables (x86/microVU_Tables.inl)
// slot for slot, so an opcode microVU treats as unknown is unknown here too.

#include "common/Pcsx2Defs.h"

namespace vuaot
{
	enum class UOp : u8
	{
		Unknown,
		NOP,
		// FMAC family. The broadcast (bc) forms carry the lane in `bc`.
		ADD, ADDi, ADDq, ADDbc,
		SUB, SUBi, SUBq, SUBbc,
		MUL, MULi, MULq, MULbc,
		MADD, MADDi, MADDq, MADDbc,
		MSUB, MSUBi, MSUBq, MSUBbc,
		MAX, MAXi, MAXbc,
		MINI, MINIi, MINIbc,
		ADDA, ADDAi, ADDAq, ADDAbc,
		SUBA, SUBAi, SUBAq, SUBAbc,
		MULA, MULAi, MULAq, MULAbc,
		MADDA, MADDAi, MADDAq, MADDAbc,
		MSUBA, MSUBAi, MSUBAq, MSUBAbc,
		OPMULA, OPMSUB,
		ABS,
		FTOI0, FTOI4, FTOI12, FTOI15,
		ITOF0, ITOF4, ITOF12, ITOF15,
		CLIP,
		Count
	};

	enum class LOp : u8
	{
		Unknown,
		LQ, SQ, ILW, ISW, IADDIU, ISUBIU,
		FCEQ, FCSET, FCAND, FCOR, FSEQ, FSSET, FSAND, FSOR, FMEQ, FMAND, FMOR, FCGET,
		B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ,
		IADD, ISUB, IADDI, IAND, IOR,
		MOVE, LQI, DIV, MTIR, RNEXT, MFP, XTOP, XGKICK,
		ESADD, EATANxy, ESQRT, ESIN,
		MR32, SQI, SQRT, MFIR, RGET, XITOP,
		ERSADD, EATANxz, ERSQRT, EATAN,
		LQD, RSQRT, ILWR, RINIT, ELENG, ESUM, ERCPR, EEXP,
		SQD, WAITQ, ISWR, RXOR, ERLENG, WAITP,
		Count
	};

	// How microVU's pass 1 analyses an upper op (setupPass1 / mVUanalyzeFMACn).
	enum class UClass : u8
	{
		None,    // NOP
		Bad,     // unknown opcode
		FmacA,   // ADD/SUB/MUL/MAX/MINI and the ADDA/SUBA/MULA forms (mVU_FMACa)
		FmacB,   // MADDA/MSUBA forms (mVU_FMACb)
		FmacC,   // MADD forms (mVU_FMACc)
		FmacD,   // MSUB forms (mVU_FMACd)
		Fmac2,   // ABS/FTOI/ITOF
		Opmula,
		Opmsub,
		Clip,
	};

	// The arithmetic an FMAC-family op performs, independent of its operand form.
	enum class FmacKind : u8
	{
		None, Add, Sub, Mul, Madd, Msub, Max, Mini
	};

	// The source of an FMAC's second operand (microVU's opCase 1..4).
	enum class FtForm : u8
	{
		None = 0,
		Reg = 1,   // Ft lanes
		Bc = 2,    // Ft.bc broadcast
		I = 3,     // I register
		Q = 4,     // Q register
	};

	struct UpperInst
	{
		u32 word = 0;
		UOp op = UOp::Unknown;
		UClass cls = UClass::Bad;
		FmacKind kind = FmacKind::None;
		FtForm form = FtForm::None;
		bool isAcc = false;     // writes ACC instead of Fd
		bool flagWriter = false; // microVU sFLAG.doFlag in pass 1 (FMAC1/FMAC3, not MAX/MINI)
		bool foundPass4 = false; // sets needExactMatch bit 3 in microVU's pass 4
		u8 ft = 0, fs = 0, fd = 0;
		u8 mask = 0;            // dest mask, bit 3 = x .. bit 0 = w (microVU _X_Y_Z_W)
		u8 bc = 0;              // broadcast lane 0..3 = x..w
		bool iBit = false, eBit = false, mBit = false, dBit = false, tBit = false;
	};

	struct LowerInst
	{
		u32 word = 0;
		LOp op = LOp::Unknown;
		u8 ft = 0, fs = 0, fd = 0;   // 5-bit fields
		u8 it = 0, is = 0, id = 0;   // 4-bit VI fields
		u8 mask = 0;                 // dest mask, bit 3 = x .. bit 0 = w
		u8 fsf = 0, ftf = 0;
		s16 imm5 = 0;
		s32 imm11 = 0;
		u32 imm12 = 0, imm15 = 0, imm24 = 0;

		bool isBranch() const;       // B..IBGEZ, JR, JALR
		bool isCondBranch() const;   // IBxx
		bool isJump() const;         // JR/JALR
	};

	struct InstPair
	{
		u32 pc = 0;          // byte offset of the pair in micro memory
		UpperInst upper;
		LowerInst lower;     // meaningless when upper.iBit (the word is the I immediate)
	};

	UpperInst DecodeUpper(u32 word);
	LowerInst DecodeLower(u32 word);
	// The same from a per-thread memo, by reference: valid only until the
	// next decode on this thread.
	const UpperInst& DecodeUpperRef(u32 word);
	const LowerInst& DecodeLowerRef(u32 word);
	InstPair DecodePair(const u8* micro, u32 memSize, u32 pc);

	const char* UOpName(UOp op);
	const char* LOpName(LOp op);

	// Short disassembly for IR dumps and test messages.
	void FormatPair(const InstPair& p, char* buf, size_t bufSize);
} // namespace vuaot
