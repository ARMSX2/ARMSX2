// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotDecode.h"

#include <cstdio>
#include <cstring>

namespace vuaot
{
	namespace
	{
		struct UDesc
		{
			UOp op;
			UClass cls;
			FmacKind kind;
			FtForm form;
			bool isAcc;
		};

		constexpr UDesc kBad = {UOp::Unknown, UClass::Bad, FmacKind::None, FtForm::None, false};

		// Primary upper table, indexed by code & 0x3f (mVU_UPPER_OPCODE).
		UDesc PrimaryUpper(u32 idx)
		{
			const FtForm Bc = FtForm::Bc, Rg = FtForm::Reg, I = FtForm::I, Q = FtForm::Q;
			switch (idx)
			{
				case 0x00: case 0x01: case 0x02: case 0x03: return {UOp::ADDbc, UClass::FmacA, FmacKind::Add, Bc, false};
				case 0x04: case 0x05: case 0x06: case 0x07: return {UOp::SUBbc, UClass::FmacA, FmacKind::Sub, Bc, false};
				case 0x08: case 0x09: case 0x0A: case 0x0B: return {UOp::MADDbc, UClass::FmacC, FmacKind::Madd, Bc, false};
				case 0x0C: case 0x0D: case 0x0E: case 0x0F: return {UOp::MSUBbc, UClass::FmacD, FmacKind::Msub, Bc, false};
				case 0x10: case 0x11: case 0x12: case 0x13: return {UOp::MAXbc, UClass::FmacA, FmacKind::Max, Bc, false};
				case 0x14: case 0x15: case 0x16: case 0x17: return {UOp::MINIbc, UClass::FmacA, FmacKind::Mini, Bc, false};
				case 0x18: case 0x19: case 0x1A: case 0x1B: return {UOp::MULbc, UClass::FmacA, FmacKind::Mul, Bc, false};
				case 0x1C: return {UOp::MULq, UClass::FmacA, FmacKind::Mul, Q, false};
				case 0x1D: return {UOp::MAXi, UClass::FmacA, FmacKind::Max, I, false};
				case 0x1E: return {UOp::MULi, UClass::FmacA, FmacKind::Mul, I, false};
				case 0x1F: return {UOp::MINIi, UClass::FmacA, FmacKind::Mini, I, false};
				case 0x20: return {UOp::ADDq, UClass::FmacA, FmacKind::Add, Q, false};
				case 0x21: return {UOp::MADDq, UClass::FmacC, FmacKind::Madd, Q, false};
				case 0x22: return {UOp::ADDi, UClass::FmacA, FmacKind::Add, I, false};
				case 0x23: return {UOp::MADDi, UClass::FmacC, FmacKind::Madd, I, false};
				case 0x24: return {UOp::SUBq, UClass::FmacA, FmacKind::Sub, Q, false};
				case 0x25: return {UOp::MSUBq, UClass::FmacD, FmacKind::Msub, Q, false};
				case 0x26: return {UOp::SUBi, UClass::FmacA, FmacKind::Sub, I, false};
				case 0x27: return {UOp::MSUBi, UClass::FmacD, FmacKind::Msub, I, false};
				case 0x28: return {UOp::ADD, UClass::FmacA, FmacKind::Add, Rg, false};
				case 0x29: return {UOp::MADD, UClass::FmacC, FmacKind::Madd, Rg, false};
				case 0x2A: return {UOp::MUL, UClass::FmacA, FmacKind::Mul, Rg, false};
				case 0x2B: return {UOp::MAX, UClass::FmacA, FmacKind::Max, Rg, false};
				case 0x2C: return {UOp::SUB, UClass::FmacA, FmacKind::Sub, Rg, false};
				case 0x2D: return {UOp::MSUB, UClass::FmacD, FmacKind::Msub, Rg, false};
				case 0x2E: return {UOp::OPMSUB, UClass::Opmsub, FmacKind::None, Rg, false};
				case 0x2F: return {UOp::MINI, UClass::FmacA, FmacKind::Mini, Rg, false};
				default: return kBad;
			}
		}

		// The four FD tables share a layout; `bc` (code & 3) selects the table.
		UDesc FdUpper(u32 idx, u32 bc)
		{
			const FtForm Bc = FtForm::Bc, Rg = FtForm::Reg, I = FtForm::I, Q = FtForm::Q;
			switch (idx)
			{
				case 0x00: return {UOp::ADDAbc, UClass::FmacA, FmacKind::Add, Bc, true};
				case 0x01: return {UOp::SUBAbc, UClass::FmacA, FmacKind::Sub, Bc, true};
				case 0x02: return {UOp::MADDAbc, UClass::FmacB, FmacKind::Madd, Bc, true};
				case 0x03: return {UOp::MSUBAbc, UClass::FmacB, FmacKind::Msub, Bc, true};
				case 0x04:
				{
					static constexpr UOp k[4] = {UOp::ITOF0, UOp::ITOF4, UOp::ITOF12, UOp::ITOF15};
					return {k[bc], UClass::Fmac2, FmacKind::None, FtForm::None, false};
				}
				case 0x05:
				{
					static constexpr UOp k[4] = {UOp::FTOI0, UOp::FTOI4, UOp::FTOI12, UOp::FTOI15};
					return {k[bc], UClass::Fmac2, FmacKind::None, FtForm::None, false};
				}
				case 0x06: return {UOp::MULAbc, UClass::FmacA, FmacKind::Mul, Bc, true};
				case 0x07:
					switch (bc)
					{
						case 0: return {UOp::MULAq, UClass::FmacA, FmacKind::Mul, Q, true};
						case 1: return {UOp::ABS, UClass::Fmac2, FmacKind::None, FtForm::None, false};
						case 2: return {UOp::MULAi, UClass::FmacA, FmacKind::Mul, I, true};
						default: return {UOp::CLIP, UClass::Clip, FmacKind::None, FtForm::None, false};
					}
				case 0x08:
					switch (bc)
					{
						case 0: return {UOp::ADDAq, UClass::FmacA, FmacKind::Add, Q, true};
						case 1: return {UOp::MADDAq, UClass::FmacB, FmacKind::Madd, Q, true};
						case 2: return {UOp::ADDAi, UClass::FmacA, FmacKind::Add, I, true};
						default: return {UOp::MADDAi, UClass::FmacB, FmacKind::Madd, I, true};
					}
				case 0x09:
					switch (bc)
					{
						case 0: return {UOp::SUBAq, UClass::FmacA, FmacKind::Sub, Q, true};
						case 1: return {UOp::MSUBAq, UClass::FmacB, FmacKind::Msub, Q, true};
						case 2: return {UOp::SUBAi, UClass::FmacA, FmacKind::Sub, I, true};
						default: return {UOp::MSUBAi, UClass::FmacB, FmacKind::Msub, I, true};
					}
				case 0x0A:
					switch (bc)
					{
						case 0: return {UOp::ADDA, UClass::FmacA, FmacKind::Add, Rg, true};
						case 1: return {UOp::MADDA, UClass::FmacB, FmacKind::Madd, Rg, true};
						case 2: return {UOp::MULA, UClass::FmacA, FmacKind::Mul, Rg, true};
						default: return kBad;
					}
				case 0x0B:
					switch (bc)
					{
						case 0: return {UOp::SUBA, UClass::FmacA, FmacKind::Sub, Rg, true};
						case 1: return {UOp::MSUBA, UClass::FmacB, FmacKind::Msub, Rg, true};
						case 2: return {UOp::OPMULA, UClass::Opmula, FmacKind::None, Rg, true};
						default: return {UOp::NOP, UClass::None, FmacKind::None, FtForm::None, false};
					}
				default: return kBad;
			}
		}

		const char* const kUOpNames[] = {
			"???", "NOP",
			"ADD", "ADDi", "ADDq", "ADDbc",
			"SUB", "SUBi", "SUBq", "SUBbc",
			"MUL", "MULi", "MULq", "MULbc",
			"MADD", "MADDi", "MADDq", "MADDbc",
			"MSUB", "MSUBi", "MSUBq", "MSUBbc",
			"MAX", "MAXi", "MAXbc",
			"MINI", "MINIi", "MINIbc",
			"ADDA", "ADDAi", "ADDAq", "ADDAbc",
			"SUBA", "SUBAi", "SUBAq", "SUBAbc",
			"MULA", "MULAi", "MULAq", "MULAbc",
			"MADDA", "MADDAi", "MADDAq", "MADDAbc",
			"MSUBA", "MSUBAi", "MSUBAq", "MSUBAbc",
			"OPMULA", "OPMSUB",
			"ABS",
			"FTOI0", "FTOI4", "FTOI12", "FTOI15",
			"ITOF0", "ITOF4", "ITOF12", "ITOF15",
			"CLIP",
		};
		static_assert(sizeof(kUOpNames) / sizeof(kUOpNames[0]) == static_cast<size_t>(UOp::Count));

		const char* const kLOpNames[] = {
			"???",
			"LQ", "SQ", "ILW", "ISW", "IADDIU", "ISUBIU",
			"FCEQ", "FCSET", "FCAND", "FCOR", "FSEQ", "FSSET", "FSAND", "FSOR", "FMEQ", "FMAND", "FMOR", "FCGET",
			"B", "BAL", "JR", "JALR", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ",
			"IADD", "ISUB", "IADDI", "IAND", "IOR",
			"MOVE", "LQI", "DIV", "MTIR", "RNEXT", "MFP", "XTOP", "XGKICK",
			"ESADD", "EATANxy", "ESQRT", "ESIN",
			"MR32", "SQI", "SQRT", "MFIR", "RGET", "XITOP",
			"ERSADD", "EATANxz", "ERSQRT", "EATAN",
			"LQD", "RSQRT", "ILWR", "RINIT", "ELENG", "ESUM", "ERCPR", "EEXP",
			"SQD", "WAITQ", "ISWR", "RXOR", "ERLENG", "WAITP",
		};
		static_assert(sizeof(kLOpNames) / sizeof(kLOpNames[0]) == static_cast<size_t>(LOp::Count));

		LOp LowerT3(u32 table, u32 idx)
		{
			switch (table)
			{
				case 0:
					switch (idx)
					{
						case 0x0C: return LOp::MOVE;
						case 0x0D: return LOp::LQI;
						case 0x0E: return LOp::DIV;
						case 0x0F: return LOp::MTIR;
						case 0x10: return LOp::RNEXT;
						case 0x19: return LOp::MFP;
						case 0x1A: return LOp::XTOP;
						case 0x1B: return LOp::XGKICK;
						case 0x1C: return LOp::ESADD;
						case 0x1D: return LOp::EATANxy;
						case 0x1E: return LOp::ESQRT;
						case 0x1F: return LOp::ESIN;
						default: return LOp::Unknown;
					}
				case 1:
					switch (idx)
					{
						case 0x0C: return LOp::MR32;
						case 0x0D: return LOp::SQI;
						case 0x0E: return LOp::SQRT;
						case 0x0F: return LOp::MFIR;
						case 0x10: return LOp::RGET;
						case 0x1A: return LOp::XITOP;
						case 0x1C: return LOp::ERSADD;
						case 0x1D: return LOp::EATANxz;
						case 0x1E: return LOp::ERSQRT;
						case 0x1F: return LOp::EATAN;
						default: return LOp::Unknown;
					}
				case 2:
					switch (idx)
					{
						case 0x0D: return LOp::LQD;
						case 0x0E: return LOp::RSQRT;
						case 0x0F: return LOp::ILWR;
						case 0x10: return LOp::RINIT;
						case 0x1C: return LOp::ELENG;
						case 0x1D: return LOp::ESUM;
						case 0x1E: return LOp::ERCPR;
						case 0x1F: return LOp::EEXP;
						default: return LOp::Unknown;
					}
				default:
					switch (idx)
					{
						case 0x0D: return LOp::SQD;
						case 0x0E: return LOp::WAITQ;
						case 0x0F: return LOp::ISWR;
						case 0x10: return LOp::RXOR;
						case 0x1C: return LOp::ERLENG;
						case 0x1E: return LOp::WAITP;
						default: return LOp::Unknown;
					}
			}
		}
	} // namespace

	bool LowerInst::isBranch() const
	{
		switch (op)
		{
			case LOp::B: case LOp::BAL: case LOp::JR: case LOp::JALR:
			case LOp::IBEQ: case LOp::IBNE: case LOp::IBLTZ: case LOp::IBGTZ: case LOp::IBLEZ: case LOp::IBGEZ:
				return true;
			default:
				return false;
		}
	}

	bool LowerInst::isCondBranch() const
	{
		switch (op)
		{
			case LOp::IBEQ: case LOp::IBNE: case LOp::IBLTZ: case LOp::IBGTZ: case LOp::IBLEZ: case LOp::IBGEZ:
				return true;
			default:
				return false;
		}
	}

	bool LowerInst::isJump() const
	{
		return op == LOp::JR || op == LOp::JALR;
	}

	namespace
	{
		// Decoding is a pure function of the word and the compile passes ask
		// for the same words many times: a small direct-mapped memo per thread.
		//
		// The table is heap storage the thread owns, reached through a thread_local
		// pointer. A thread_local array of 4096 entries (two tables, about 262 KB)
		// made the program's static TLS block 265 KB, and glibc carves static TLS out
		// of the stack of every thread it creates: cubeb's ALSA thread asks for 256 KiB
		// and pthread_create failed on the handhelds. The pointer is constant-initialised
		// (nullptr), so the lookup still needs no per-access TLS init guard; the first
		// lookup of a thread allocates the zeroed table, and a thread_local holder
		// frees it when the thread exits.
		template <typename T, T (*Decode)(u32)>
		const T& Memo(u32 code)
		{
			struct Entry
			{
				u32 key; // code + 1 (0: empty)
				alignas(T) unsigned char value[sizeof(T)];
			};
			static thread_local Entry* t_table = nullptr;
			Entry* table = t_table;
			if (table == nullptr) [[unlikely]]
			{
				struct Owner
				{
					Entry* table = new Entry[4096](); // (value-initialised: every key 0)
					~Owner()
					{
						t_table = nullptr;
						delete[] table;
					}
				};
				static thread_local Owner owner;
				table = t_table = owner.table;
			}
			Entry& e = table[(code ^ (code >> 12) ^ (code >> 24)) & 4095];
			T* v = reinterpret_cast<T*>(e.value);
			if (e.key != code + 1)
			{
				*v = Decode(code);
				e.key = code + 1;
			}
			return *v;
		}
	} // namespace

	static UpperInst DecodeUpperRawImpl(u32 code);
	static LowerInst DecodeLowerRawImpl(u32 code);
	UpperInst DecodeUpper(u32 code) { return Memo<UpperInst, DecodeUpperRawImpl>(code); }
	LowerInst DecodeLower(u32 code) { return Memo<LowerInst, DecodeLowerRawImpl>(code); }
	const UpperInst& DecodeUpperRef(u32 code) { return Memo<UpperInst, DecodeUpperRawImpl>(code); }
	const LowerInst& DecodeLowerRef(u32 code) { return Memo<LowerInst, DecodeLowerRawImpl>(code); }

	static UpperInst DecodeUpperRawImpl(u32 code)
	{
		UpperInst u;
		u.word = code;
		const u32 prim = code & 0x3f;
		UDesc d = (prim >= 0x3C) ? FdUpper((code >> 6) & 0x1f, code & 3) : PrimaryUpper(prim);
		u.op = d.op;
		u.cls = d.cls;
		u.kind = d.kind;
		u.form = d.form;
		u.isAcc = d.isAcc;
		u.ft = (code >> 16) & 0x1f;
		u.fs = (code >> 11) & 0x1f;
		u.fd = (code >> 6) & 0x1f;
		u.mask = (code >> 21) & 0xf;
		u.bc = code & 3;
		u.iBit = (code >> 31) & 1;
		u.eBit = (code >> 30) & 1;
		u.mBit = (code >> 29) & 1;
		u.dBit = (code >> 28) & 1;
		u.tBit = (code >> 27) & 1;

		switch (u.cls)
		{
			case UClass::FmacA:
				u.flagWriter = (u.kind != FmacKind::Max && u.kind != FmacKind::Mini);
				u.foundPass4 = u.flagWriter;
				break;
			case UClass::FmacB:
			case UClass::FmacC:
			case UClass::FmacD:
			case UClass::Opmula:
			case UClass::Opmsub:
				u.flagWriter = true;
				u.foundPass4 = true;
				break;
			default:
				break;
		}
		return u;
	}

	static LowerInst DecodeLowerRawImpl(u32 code)
	{
		LowerInst l;
		l.word = code;
		l.ft = (code >> 16) & 0x1f;
		l.fs = (code >> 11) & 0x1f;
		l.fd = (code >> 6) & 0x1f;
		l.it = (code >> 16) & 0xf;
		l.is = (code >> 11) & 0xf;
		l.id = (code >> 6) & 0xf;
		l.mask = (code >> 21) & 0xf;
		l.fsf = (code >> 21) & 3;
		l.ftf = (code >> 23) & 3;
		l.imm5 = static_cast<s16>(((code & 0x400) ? 0xfff0 : 0) | ((code >> 6) & 0xf));
		l.imm11 = (code & 0x400) ? static_cast<s32>(0xfffffc00u | (code & 0x3ff)) : static_cast<s32>(code & 0x3ff);
		l.imm12 = (((code >> 21) & 0x1) << 11) | (code & 0x7ff);
		l.imm15 = ((code >> 10) & 0x7800) | (code & 0x7ff);
		l.imm24 = code & 0xffffff;

		switch (code >> 25)
		{
			case 0x00: l.op = LOp::LQ; break;
			case 0x01: l.op = LOp::SQ; break;
			case 0x04: l.op = LOp::ILW; break;
			case 0x05: l.op = LOp::ISW; break;
			case 0x08: l.op = LOp::IADDIU; break;
			case 0x09: l.op = LOp::ISUBIU; break;
			case 0x10: l.op = LOp::FCEQ; break;
			case 0x11: l.op = LOp::FCSET; break;
			case 0x12: l.op = LOp::FCAND; break;
			case 0x13: l.op = LOp::FCOR; break;
			case 0x14: l.op = LOp::FSEQ; break;
			case 0x15: l.op = LOp::FSSET; break;
			case 0x16: l.op = LOp::FSAND; break;
			case 0x17: l.op = LOp::FSOR; break;
			case 0x18: l.op = LOp::FMEQ; break;
			case 0x1A: l.op = LOp::FMAND; break;
			case 0x1B: l.op = LOp::FMOR; break;
			case 0x1C: l.op = LOp::FCGET; break;
			case 0x20: l.op = LOp::B; break;
			case 0x21: l.op = LOp::BAL; break;
			case 0x24: l.op = LOp::JR; break;
			case 0x25: l.op = LOp::JALR; break;
			case 0x28: l.op = LOp::IBEQ; break;
			case 0x29: l.op = LOp::IBNE; break;
			case 0x2C: l.op = LOp::IBLTZ; break;
			case 0x2D: l.op = LOp::IBGTZ; break;
			case 0x2E: l.op = LOp::IBLEZ; break;
			case 0x2F: l.op = LOp::IBGEZ; break;
			case 0x40:
				switch (code & 0x3f)
				{
					case 0x30: l.op = LOp::IADD; break;
					case 0x31: l.op = LOp::ISUB; break;
					case 0x32: l.op = LOp::IADDI; break;
					case 0x34: l.op = LOp::IAND; break;
					case 0x35: l.op = LOp::IOR; break;
					case 0x3C: l.op = LowerT3(0, (code >> 6) & 0x1f); break;
					case 0x3D: l.op = LowerT3(1, (code >> 6) & 0x1f); break;
					case 0x3E: l.op = LowerT3(2, (code >> 6) & 0x1f); break;
					case 0x3F: l.op = LowerT3(3, (code >> 6) & 0x1f); break;
					default: l.op = LOp::Unknown; break;
				}
				break;
			default:
				l.op = LOp::Unknown;
				break;
		}
		return l;
	}

	InstPair DecodePair(const u8* micro, u32 memSize, u32 pc)
	{
		InstPair p;
		p.pc = pc & (memSize - 8);
		u32 lo, hi;
		std::memcpy(&lo, micro + p.pc, 4);
		std::memcpy(&hi, micro + p.pc + 4, 4);
		p.upper = DecodeUpper(hi);
		p.lower = DecodeLower(lo);
		return p;
	}

	const char* UOpName(UOp op)
	{
		return kUOpNames[static_cast<size_t>(op)];
	}

	const char* LOpName(LOp op)
	{
		return kLOpNames[static_cast<size_t>(op)];
	}

	void FormatPair(const InstPair& p, char* buf, size_t bufSize)
	{
		static const char* const kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw",
			"x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
		static const char kLane[4] = {'x', 'y', 'z', 'w'};
		const UpperInst& u = p.upper;
		char ub[64];
		if (u.cls == UClass::None)
			std::snprintf(ub, sizeof(ub), "NOP");
		else if (u.cls == UClass::Clip)
			std::snprintf(ub, sizeof(ub), "CLIP vf%02u, vf%02u.w", u.fs, u.ft);
		else if (u.cls == UClass::Fmac2)
			std::snprintf(ub, sizeof(ub), "%s.%s vf%02u, vf%02u", UOpName(u.op), kMask[u.mask], u.ft, u.fs);
		else if (u.cls == UClass::Bad)
			std::snprintf(ub, sizeof(ub), "??? %08x", u.word);
		else
		{
			char dst[8];
			if (u.isAcc)
				std::snprintf(dst, sizeof(dst), "ACC");
			else
				std::snprintf(dst, sizeof(dst), "vf%02u", u.fd);
			switch (u.form)
			{
				case FtForm::Bc: std::snprintf(ub, sizeof(ub), "%s%c.%s %s, vf%02u, vf%02u", UOpName(u.op), kLane[u.bc], kMask[u.mask], dst, u.fs, u.ft); break;
				case FtForm::I: std::snprintf(ub, sizeof(ub), "%s.%s %s, vf%02u, I", UOpName(u.op), kMask[u.mask], dst, u.fs); break;
				case FtForm::Q: std::snprintf(ub, sizeof(ub), "%s.%s %s, vf%02u, Q", UOpName(u.op), kMask[u.mask], dst, u.fs); break;
				default: std::snprintf(ub, sizeof(ub), "%s.%s %s, vf%02u, vf%02u", UOpName(u.op), kMask[u.mask], dst, u.fs, u.ft); break;
			}
		}
		char bits[8] = {};
		int nb = 0;
		if (u.iBit) bits[nb++] = 'I';
		if (u.eBit) bits[nb++] = 'E';
		if (u.mBit) bits[nb++] = 'M';
		if (u.dBit) bits[nb++] = 'D';
		if (u.tBit) bits[nb++] = 'T';

		char lb[64];
		const LowerInst& l = p.lower;
		static const char kF[4] = {'x', 'y', 'z', 'w'};
		const char* n = LOpName(l.op);
		const char* m = kMask[l.mask];
		const u32 target = (p.pc + 8 + l.imm11 * 8) & 0x3ff8;
		if (u.iBit)
			std::snprintf(lb, sizeof(lb), "[I=0x%08x]", l.word);
		else if (l.word == 0x8000033c)
			std::snprintf(lb, sizeof(lb), "NOP");
		else
		{
			switch (l.op)
			{
				case LOp::LQ: std::snprintf(lb, sizeof(lb), "LQ.%s vf%02u, %d(vi%02u)", m, l.ft, l.imm11, l.is); break;
				case LOp::LQI: std::snprintf(lb, sizeof(lb), "LQI.%s vf%02u, (vi%02u++)", m, l.ft, l.is); break;
				case LOp::LQD: std::snprintf(lb, sizeof(lb), "LQD.%s vf%02u, (--vi%02u)", m, l.ft, l.is); break;
				case LOp::SQ: std::snprintf(lb, sizeof(lb), "SQ.%s vf%02u, %d(vi%02u)", m, l.fs, l.imm11, l.it); break;
				case LOp::SQI: std::snprintf(lb, sizeof(lb), "SQI.%s vf%02u, (vi%02u++)", m, l.fs, l.it); break;
				case LOp::SQD: std::snprintf(lb, sizeof(lb), "SQD.%s vf%02u, (--vi%02u)", m, l.fs, l.it); break;
				case LOp::ILW: std::snprintf(lb, sizeof(lb), "ILW.%s vi%02u, %d(vi%02u)", m, l.it, l.imm11, l.is); break;
				case LOp::ISW: std::snprintf(lb, sizeof(lb), "ISW.%s vi%02u, %d(vi%02u)", m, l.it, l.imm11, l.is); break;
				case LOp::ILWR: std::snprintf(lb, sizeof(lb), "ILWR.%s vi%02u, (vi%02u)", m, l.it, l.is); break;
				case LOp::ISWR: std::snprintf(lb, sizeof(lb), "ISWR.%s vi%02u, (vi%02u)", m, l.it, l.is); break;
				case LOp::IADD: case LOp::ISUB: case LOp::IAND: case LOp::IOR:
					std::snprintf(lb, sizeof(lb), "%s vi%02u, vi%02u, vi%02u", n, l.id, l.is, l.it); break;
				case LOp::IADDI: std::snprintf(lb, sizeof(lb), "IADDI vi%02u, vi%02u, %d", l.it, l.is, l.imm5); break;
				case LOp::IADDIU: case LOp::ISUBIU: std::snprintf(lb, sizeof(lb), "%s vi%02u, vi%02u, 0x%x", n, l.it, l.is, l.imm15); break;
				case LOp::B: std::snprintf(lb, sizeof(lb), "B 0x%04x", target); break;
				case LOp::BAL: std::snprintf(lb, sizeof(lb), "BAL vi%02u, 0x%04x", l.it, target); break;
				case LOp::JR: std::snprintf(lb, sizeof(lb), "JR vi%02u", l.is); break;
				case LOp::JALR: std::snprintf(lb, sizeof(lb), "JALR vi%02u, vi%02u", l.it, l.is); break;
				case LOp::IBEQ: case LOp::IBNE: std::snprintf(lb, sizeof(lb), "%s vi%02u, vi%02u, 0x%04x", n, l.it, l.is, target); break;
				case LOp::IBLTZ: case LOp::IBGTZ: case LOp::IBLEZ: case LOp::IBGEZ: std::snprintf(lb, sizeof(lb), "%s vi%02u, 0x%04x", n, l.is, target); break;
				case LOp::FCAND: case LOp::FCEQ: case LOp::FCOR: case LOp::FCSET: std::snprintf(lb, sizeof(lb), "%s 0x%06x", n, l.imm24); break;
				case LOp::FCGET: std::snprintf(lb, sizeof(lb), "FCGET vi%02u", l.it); break;
				case LOp::FMAND: case LOp::FMEQ: case LOp::FMOR: std::snprintf(lb, sizeof(lb), "%s vi%02u, vi%02u", n, l.it, l.is); break;
				case LOp::FSAND: case LOp::FSEQ: case LOp::FSOR: std::snprintf(lb, sizeof(lb), "%s vi%02u, 0x%03x", n, l.it, l.imm12); break;
				case LOp::FSSET: std::snprintf(lb, sizeof(lb), "FSSET 0x%03x", l.imm12); break;
				case LOp::MOVE: case LOp::MR32: std::snprintf(lb, sizeof(lb), "%s.%s vf%02u, vf%02u", n, m, l.ft, l.fs); break;
				case LOp::MFIR: std::snprintf(lb, sizeof(lb), "MFIR.%s vf%02u, vi%02u", m, l.ft, l.is); break;
				case LOp::MTIR: std::snprintf(lb, sizeof(lb), "MTIR vi%02u, vf%02u.%c", l.it, l.fs, kF[l.fsf]); break;
				case LOp::MFP: std::snprintf(lb, sizeof(lb), "MFP.%s vf%02u, P", m, l.ft); break;
				case LOp::DIV: case LOp::RSQRT: std::snprintf(lb, sizeof(lb), "%s Q, vf%02u.%c, vf%02u.%c", n, l.fs, kF[l.fsf], l.ft, kF[l.ftf]); break;
				case LOp::SQRT: std::snprintf(lb, sizeof(lb), "SQRT Q, vf%02u.%c", l.ft, kF[l.ftf]); break;
				case LOp::EATAN: case LOp::EEXP: case LOp::ERCPR: case LOp::ERSQRT: case LOp::ESIN: case LOp::ESQRT:
					std::snprintf(lb, sizeof(lb), "%s P, vf%02u.%c", n, l.fs, kF[l.fsf]); break;
				case LOp::EATANxy: case LOp::EATANxz: case LOp::ELENG: case LOp::ERLENG: case LOp::ERSADD: case LOp::ESADD: case LOp::ESUM:
					std::snprintf(lb, sizeof(lb), "%s P, vf%02u", n, l.fs); break;
				case LOp::RINIT: case LOp::RXOR: std::snprintf(lb, sizeof(lb), "%s R, vf%02u.%c", n, l.fs, kF[l.fsf]); break;
				case LOp::RGET: case LOp::RNEXT: std::snprintf(lb, sizeof(lb), "%s.%s vf%02u, R", n, m, l.ft); break;
				case LOp::XTOP: case LOp::XITOP: std::snprintf(lb, sizeof(lb), "%s vi%02u", n, l.it); break;
				case LOp::XGKICK: std::snprintf(lb, sizeof(lb), "XGKICK vi%02u", l.is); break;
				case LOp::WAITQ: case LOp::WAITP: std::snprintf(lb, sizeof(lb), "%s", n); break;
				default: std::snprintf(lb, sizeof(lb), "??? %08x", l.word); break;
			}
		}
		std::snprintf(buf, bufSize, "%04x: %-34s | %s%s%s", p.pc, ub, lb, nb ? " " : "", bits);
	}
} // namespace vuaot
