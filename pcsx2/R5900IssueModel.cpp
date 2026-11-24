// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "R5900IssueModel.h"

#include <algorithm>
#include <cstring>

namespace R5900::Issue
{
#ifdef PCSX2_RECOMPILER_TESTS
	u32 g_testExtraPer1024 = 0;
#endif

	namespace
	{
		// Issue classes. Two integer pipelines and one of everything else: an
		// MMI op takes both integer pipelines; a load or store pairs with
		// integer, MMI and FPU work; an FPU or VU0 macro op pairs with integer
		// work but not with another one. SOLO issues alone and takes a fixed
		// number of cycles.
		enum Cls : u8
		{
			INT,
			LS,
			BR,
			MMI,
			FP,
			HL,
			SOLO,
			NONE = 0xFF,
		};

		constexpr u8 PairMask(Cls a)
		{
			switch (a)
			{
				case INT: return (1u << INT) | (1u << LS) | (1u << BR) | (1u << FP) | (1u << HL);
				case LS:  return (1u << INT) | (1u << BR) | (1u << MMI) | (1u << FP) | (1u << HL);
				case BR:  return (1u << INT) | (1u << LS) | (1u << FP) | (1u << HL);
				case MMI: return (1u << LS) | (1u << FP);
				case FP:  return (1u << INT) | (1u << LS) | (1u << BR) | (1u << HL) | (1u << MMI);
				case HL:  return (1u << INT) | (1u << LS) | (1u << BR) | (1u << FP);
				default:  return 0;
			}
		}

		constexpr u8 kPair[7] = {PairMask(INT), PairMask(LS), PairMask(BR), PairMask(MMI), PairMask(FP),
			PairMask(HL), PairMask(SOLO)};

		bool Pairs(u8 first, u8 second)
		{
			return first < 7 && (kPair[first] >> second) & 1;
		}

		// Register slots.
		constexpr u8 R_FPR = 32;
		constexpr u8 R_VF = 64;
		constexpr u8 R_HI0 = 96, R_LO0 = 97, R_HI1 = 98, R_LO1 = 99;
		constexpr u8 R_FACC = 100, R_FCOND = 101, R_VACC = 102, R_SA = 103;
		constexpr u8 R_VI = 104;

		// Unpipelined units.
		enum Unit : u8
		{
			U_MUL0,
			U_MUL1,
			U_PMUL,
			U_FDIV,
			U_VDIV,
			U_NONE = 0xFF,
		};

		enum class Br : u8
		{
			None,
			Cond,
			CondLikely,
			Jump,     // J, JAL
			RegJump,  // JR, JALR
		};

		struct Info
		{
			u8 cls = INT;
			u8 n_reads = 0, n_late = 0, n_late2 = 0, n_writes = 0;
			u8 reads[4];
			u8 late_reads[1];
			u8 late2_reads[1];
			u8 writes[6];
			u8 lat = 1;
			u8 hilo_lat = 0;   // nonzero: HI/LO written ready this many cycles after issue
			u8 unit = U_NONE;
			u8 busy = 0;
			u8 wait_unit = U_NONE;
			u8 solo_cost = 0;
			bool is_load = false;
			bool is_store = false;
			bool is_mfc0 = false;
			s8 jr_reg = -1;
			Br br = Br::None;

			void Read(u32 r) { reads[n_reads++] = static_cast<u8>(r); }
			void Write(u32 r) { writes[n_writes++] = static_cast<u8>(r); }
		};

		constexpr u32 Rs(u32 w) { return (w >> 21) & 31; }
		constexpr u32 Rt(u32 w) { return (w >> 16) & 31; }
		constexpr u32 Rd(u32 w) { return (w >> 11) & 31; }
		constexpr u32 Sa(u32 w) { return (w >> 6) & 31; }
		constexpr u32 Fn(u32 w) { return w & 63; }

		bool IsMmiIntFunc(u32 fn)
		{
			switch (fn)
			{
				case 0x00: case 0x01: case 0x10: case 0x11: case 0x12: case 0x13:
				case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x20: case 0x21:
					return true;
				default:
					return false;
			}
		}

		// MMI2 / MMI3 sub-opcodes (sa) that multiply or multiply-accumulate,
		// and those that divide.
		bool IsPmult(u32 fn, u32 sa)
		{
			if (fn == 0x09) // MMI2: pmaddw pmsubw pmultw pmaddh phmadh pmsubh phmsbh pmulth
				return sa == 0 || sa == 4 || sa == 12 || sa == 16 || sa == 17 || sa == 20 || sa == 21 || sa == 28;
			if (fn == 0x29) // MMI3: pmadduw pmultuw
				return sa == 0 || sa == 12;
			return false;
		}
		bool IsPdiv(u32 fn, u32 sa)
		{
			return (fn == 0x09 && (sa == 13 || sa == 29)) || (fn == 0x29 && sa == 13);
		}

		// The GPR reads and writes of an MMI0-3 sub-op (128-bit view): 0 if
		// reserved, 'T' for rd = f(rs, rt), 'B' for rd = f(rt), 'D' for a
		// divide (reads rs, rt), 'F' for PMFHI/PMFLO (writes rd), 'M' for
		// PMTHI/PMTLO (reads rs).
		char MmiSubShape(u32 fn, u32 sa)
		{
			static const char* const k0 = "TTTTTTTTTTT00000TTTTTTTTTTTT00BB"; // MMI0: pext5 ppac5 are rd = f(rt)
			static const char* const k1 = "0BTTTBTT00T00000TTT0TTT0TTTT0000"; // MMI1: pabsw pabsh
			static const char* const k2 = "T0TTT000FFT0TDT0TTTTTT0000BBTDBB"; // MMI2
			static const char* const k3 = "T00T0000MMT0TDT000TT000000BB00B0"; // MMI3
			const char* t = fn == 0x08 ? k0 : fn == 0x28 ? k1 : fn == 0x09 ? k2 : k3;
			return t[sa];
		}

		bool IsSyncP(u32 w) { return (w & 0xFC00043F) == 0x0000040F; }

		// VU0 macro SPECIAL2 index sets.
		bool InVu2Acc(u32 idx) { return idx <= 15 || (idx >= 24 && idx <= 28) || idx == 30 || (idx >= 32 && idx <= 46); }
		bool InVu2Ft(u32 idx) { return (idx >= 16 && idx <= 23) || idx == 29 || idx == 48 || idx == 49; }

		// What the issue simulator needs to know about one instruction word.
		void Classify(u32 w, Info& i)
		{
			i = Info();
			if (w == 0)
				return; // NOP: an integer op with no operands
			const u32 op = w >> 26;
			const u32 rs = Rs(w), rt = Rt(w), rd = Rd(w), sa = Sa(w), fn = Fn(w);

			switch (op)
			{
				case 0x00: // SPECIAL
					switch (fn)
					{
						case 0x00: case 0x02: case 0x03: case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
							i.Write(rd); i.Read(rt); break; // shifts by sa
						case 0x04: case 0x06: case 0x07: case 0x14: case 0x16: case 0x17:
							i.Write(rd); i.Read(rs); i.Read(rt); break; // shifts by rs
						case 0x0A: case 0x0B: // MOVZ, MOVN
							i.Write(rd); i.Read(rs); i.Read(rt); i.lat = 2; break;
						case 0x08: // JR
							i.cls = BR; i.br = Br::RegJump; i.Read(rs); i.jr_reg = static_cast<s8>(rs); break;
						case 0x09: // JALR
							i.cls = BR; i.br = Br::RegJump; i.Write(rd); i.Read(rs); i.jr_reg = static_cast<s8>(rs); break;
						case 0x0C: case 0x0D: // SYSCALL, BREAK
							i.cls = SOLO; i.solo_cost = 1; break;
						case 0x0F: // SYNC, SYNC.P
							i.cls = SOLO; i.solo_cost = IsSyncP(w) ? 5 : 1; break;
						case 0x18: case 0x19: // MULT, MULTU: rd, HI, LO
							i.Read(rs); i.Read(rt);
							if (rd) i.Write(rd);
							i.Write(R_HI0); i.Write(R_LO0);
							i.lat = 4; i.unit = U_MUL0; i.busy = 2; break;
						case 0x1A: case 0x1B: // DIV, DIVU
							i.Read(rs); i.Read(rt); i.Write(R_HI0); i.Write(R_LO0);
							i.lat = 37; i.unit = U_MUL0; i.busy = 37; break;
						case 0x10: i.cls = HL; i.Write(rd); i.Read(R_HI0); break; // MFHI
						case 0x12: i.cls = HL; i.Write(rd); i.Read(R_LO0); break; // MFLO
						case 0x11: i.cls = HL; i.Read(rs); i.Write(R_HI0); i.lat = 2; break; // MTHI
						case 0x13: i.cls = HL; i.Read(rs); i.Write(R_LO0); i.lat = 2; break; // MTLO
						case 0x28: i.cls = HL; i.Write(rd); i.Read(R_SA); break; // MFSA
						case 0x29: i.cls = HL; i.Read(rs); i.Write(R_SA); i.lat = 2; break; // MTSA
						case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
						case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
							i.Write(rd); i.Read(rs); i.Read(rt); break;
						case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36: // traps: rs only
							i.Read(rs); break;
						default:
							break;
					}
					break;

				case 0x01: // REGIMM
					switch (rt)
					{
						case 0x00: case 0x01: case 0x10: case 0x11:
							i.cls = BR; i.br = Br::Cond; i.Read(rs); break;
						case 0x02: case 0x03: case 0x12: case 0x13:
							i.cls = BR; i.br = Br::CondLikely; i.Read(rs); break;
						case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0F: // traps (and two reserved) read rs
							i.Read(rs); break;
						case 0x18: case 0x19: // MTSAB, MTSAH
							i.cls = HL; i.Read(rs); i.Write(R_SA); break;
						default:
							break;
					}
					break;

				case 0x02: // J
					i.cls = BR; i.br = Br::Jump; break;
				case 0x03: // JAL
					i.cls = BR; i.br = Br::Jump; i.Write(31); break;
				case 0x04: case 0x05: // BEQ, BNE
					i.cls = BR; i.br = Br::Cond; i.Read(rs); i.Read(rt); break;
				case 0x06: case 0x07: // BLEZ, BGTZ
					i.cls = BR; i.br = Br::Cond; i.Read(rs); break;
				case 0x14: case 0x15: // BEQL, BNEL
					i.cls = BR; i.br = Br::CondLikely; i.Read(rs); i.Read(rt); break;
				case 0x16: case 0x17: // BLEZL, BGTZL
					i.cls = BR; i.br = Br::CondLikely; i.Read(rs); break;

				case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x18: case 0x19:
					i.Write(rt); i.Read(rs); break; // immediate ALU
				case 0x0F: // LUI
					i.Write(rt); break;

				case 0x10: // COP0
					if (rs == 8)
					{
						if (rt <= 3)
						{
							i.cls = BR; i.br = rt >= 2 ? Br::CondLikely : Br::Cond;
							break;
						}
						i.cls = SOLO; i.solo_cost = 1;
						break;
					}
					i.cls = SOLO;
					i.solo_cost = 1;
					if (rs == 0 || rs == 2) // MFC0 (CFC0)
					{
						i.Write(rt);
						i.is_mfc0 = (rs == 0);
					}
					else if (rs == 16)
					{
						if (fn == 0x18) i.solo_cost = 6;      // ERET
						else if (fn == 0x38) i.solo_cost = 2; // EI
						else if (fn == 0x08) i.solo_cost = 3; // TLBP
					}
					break;

				case 0x11: // COP1
					if (rs == 8)
					{
						if (rt <= 3)
						{
							i.cls = BR; i.br = rt >= 2 ? Br::CondLikely : Br::Cond;
							i.Read(R_FCOND);
							break;
						}
						i.cls = FP; // reserved BC1 encoding
						break;
					}
					i.cls = FP;
					{
						const u32 fs = rd, ft = rt, fd = sa;
						if (rs == 0) // MFC1
						{
							i.late_reads[i.n_late++] = static_cast<u8>(R_FPR + fs);
							if (rt) i.Write(rt);
							i.lat = 2;
						}
						else if (rs == 4) // MTC1
						{
							i.Read(rt);
							i.Write(R_FPR + fs);
							i.lat = 2;
						}
						else if (rs == 2 || rs == 6) // CFC1, CTC1
						{
							i.cls = SOLO;
							i.solo_cost = 4;
							if (rs == 2) i.Write(rt);
							else i.Read(rt);
						}
						else if (rs == 0x10) // S
						{
							if (fn == 48 || fn == 50 || fn == 52 || fn == 54) // C.F C.EQ C.LT C.LE
							{
								i.Read(R_FPR + fs); i.Read(R_FPR + ft); i.Write(R_FCOND); i.lat = 6;
							}
							else if (fn == 24 || fn == 25 || fn == 26 || fn == 30 || fn == 31) // ADDA SUBA MULA MADDA MSUBA
							{
								i.Read(R_FPR + fs); i.Read(R_FPR + ft);
								if (fn == 30 || fn == 31) i.Read(R_FACC);
								i.Write(R_FACC);
								i.lat = 1;
							}
							else
							{
								i.Read(R_FPR + fs);
								if (!(fn == 5 || fn == 6 || fn == 7 || fn == 4 || fn == 36)) // ABS MOV NEG SQRT CVT.W
									i.Read(R_FPR + ft);
								if (fn == 28 || fn == 29) // MADD MSUB
									i.Read(R_FACC);
								i.Write(R_FPR + fd);
								i.lat = 4;
								if (fn == 3 || fn == 4) // DIV SQRT
								{
									i.lat = 7; i.unit = U_FDIV; i.busy = 7;
								}
								else if (fn == 22) // RSQRT
								{
									i.lat = 13; i.unit = U_FDIV; i.busy = 13;
								}
							}
						}
						else if (rs == 0x14) // W: CVT.S.W
						{
							i.Read(R_FPR + fs); i.Write(R_FPR + fd); i.lat = 4;
						}
					}
					break;

				case 0x12: // COP2
					if (rs == 8)
					{
						if (rt <= 3)
						{
							i.cls = BR; i.br = rt >= 2 ? Br::CondLikely : Br::Cond;
							break;
						}
						i.cls = FP;
						break;
					}
					i.cls = FP;
					if (w & 0x02000000) // macro op
					{
						const u32 fd = sa, fs = rd, ft = rt;
						i.Read(R_VF + fs); i.Read(R_VF + ft);
						if (fn < 0x3C)
						{
							i.Write(R_VF + fd);
							i.lat = 4;
							if ((fn >= 0x08 && fn <= 0x0F) || fn == 0x21 || fn == 0x23 || fn == 0x25 || fn == 0x27 || fn == 0x29 || fn == 0x2D)
								i.Read(R_VACC);
							if (fn >= 0x30 && fn <= 0x35) // VIADD..VIOR, VIADDI: 4-bit VI fields
							{
								i.n_reads = 0;
								i.Read(R_VI + (rd & 15));
								if (fn != 0x32)
									i.Read(R_VI + (rt & 15));
								i.n_writes = 0;
								i.Write(R_VI + ((fn != 0x32 ? sa : rt) & 15));
								i.lat = 4;
							}
						}
						else
						{
							const u32 idx = (sa << 2) | (w & 3);
							if (InVu2Acc(idx))
							{
								i.Write(R_VACC);
								i.lat = 1;
								if ((idx >= 8 && idx <= 15) || idx == 33 || idx == 35 || idx == 37 || idx == 39 || idx == 41 || idx == 43 || idx == 45)
									i.Read(R_VACC);
							}
							else if (InVu2Ft(idx))
							{
								i.n_reads = 0;
								i.Read(R_VF + fs);
								i.Write(R_VF + ft);
								i.lat = 4;
							}
							else if (idx == 56 || idx == 57 || idx == 58) // VDIV VSQRT VRSQRT
							{
								i.unit = U_VDIV;
								i.busy = idx == 58 ? 13 : 7;
							}
							else if (idx == 59) // VWAITQ: 3 after the divider is free
							{
								i.cls = SOLO;
								i.solo_cost = 3;
								i.wait_unit = U_VDIV;
							}
						}
					}
					else if (rs == 1 || rs == 2) // QMFC2, CFC2
					{
						if (rs == 1)
							i.late_reads[i.n_late++] = static_cast<u8>(R_VF + rd);
						else if (rd < 16)
							i.late_reads[i.n_late++] = static_cast<u8>(R_VI + rd);
						if (rt) i.Write(rt);
						i.lat = 2;
					}
					else if (rs == 5 || rs == 6) // QMTC2, CTC2
					{
						i.Read(rt);
						if (rs == 5)
							i.Write(R_VF + rd);
						else if (rd < 16)
							i.Write(R_VI + rd);
						i.lat = 2;
					}
					break;

				case 0x1C: // MMI
					if (IsMmiIntFunc(fn))
					{
						const bool pipe1 = fn >= 0x10;
						const u8 hi = pipe1 ? R_HI1 : R_HI0, lo = pipe1 ? R_LO1 : R_LO0;
						const u8 unit = pipe1 ? U_MUL1 : U_MUL0;
						switch (fn)
						{
							case 0x00: case 0x01: case 0x20: case 0x21: // MADD, MADDU, MADD1, MADDU1
							case 0x18: case 0x19: // MULT1, MULTU1
								i.Read(rs); i.Read(rt);
								if (rd) i.Write(rd);
								i.Write(hi); i.Write(lo);
								i.lat = 4; i.unit = unit; i.busy = 2; break;
							case 0x1A: case 0x1B: // DIV1, DIVU1
								i.Read(rs); i.Read(rt); i.Write(hi); i.Write(lo);
								i.lat = 37; i.unit = unit; i.busy = 37; break;
							case 0x10: i.cls = HL; i.Write(rd); i.Read(hi); break; // MFHI1
							case 0x12: i.cls = HL; i.Write(rd); i.Read(lo); break; // MFLO1
							case 0x11: i.cls = HL; i.Read(rs); i.Write(hi); i.lat = 2; break; // MTHI1
							case 0x13: i.cls = HL; i.Read(rs); i.Write(lo); i.lat = 2; break; // MTLO1
						}
						break;
					}
					i.cls = MMI;
					switch (fn)
					{
						case 0x04: // PLZCW
							i.Read(rs); i.Write(rd); break;
						case 0x08: case 0x09: case 0x28: case 0x29: // MMI0..3
						{
							switch (MmiSubShape(fn, sa))
							{
								case 'T': i.Write(rd); i.Read(rs); i.Read(rt); break;
								case 'B': i.Write(rd); i.Read(rt); break;
								case 'D': i.Read(rs); i.Read(rt); break;
								case 'F': i.Write(rd); break;
								case 'M': i.Read(rs); break;
								default: break;
							}
							if (IsPmult(fn, sa))
							{
								i.lat = 4; i.unit = U_PMUL; i.busy = 2; i.hilo_lat = 3;
								i.Write(R_HI0); i.Write(R_LO0); i.Write(R_HI1); i.Write(R_LO1);
							}
							else if (IsPdiv(fn, sa))
							{
								i.lat = 37; i.unit = U_PMUL; i.busy = 37;
								i.Write(R_HI0); i.Write(R_LO0); i.Write(R_HI1); i.Write(R_LO1);
							}
							else if ((fn == 0x09 || fn == 0x29) && (sa == 8 || sa == 9))
							{
								if (fn == 0x09) // PMFHI, PMFLO
								{
									i.Read(R_HI0); i.Read(R_LO0);
								}
								else // PMTHI, PMTLO
								{
									i.n_writes = 0;
									i.Write(R_HI0); i.Write(R_LO0);
									i.lat = 2;
								}
							}
							break;
						}
						case 0x30: // PMFHL
							i.Read(rd); i.Write(rd); i.Read(R_HI0); i.Read(R_LO0); break;
						case 0x31: // PMTHL
							i.Read(rs); i.Write(R_HI0); i.Write(R_LO0); i.lat = 2; break;
						case 0x34: case 0x36: case 0x37: case 0x3C: case 0x3E: case 0x3F: // PSLLH .. PSRAW
							i.Write(rd); i.Read(rt); break;
						default:
							break;
					}
					break;

				// Loads: rt from memory at rs + imm.
				case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37: case 0x1E:
				case 0x22: case 0x26: case 0x1A: case 0x1B: // LWL LWR LDL LDR merge without waiting for rt
					i.cls = LS; i.lat = 2; i.is_load = true;
					i.Write(rt); i.Read(rs); break;
				case 0x31: // LWC1: 3 per pair with an ADD.S of it
					i.cls = LS; i.lat = 3; i.is_load = true;
					i.Read(rs); i.Write(R_FPR + rt); break;
				case 0x36: // LQC2
					i.cls = LS; i.lat = 3; i.is_load = true;
					i.Read(rs); i.Write(R_VF + rt); break;
				// Stores.
				case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x3F: case 0x1F:
					i.cls = LS; i.lat = 2; i.is_store = true;
					i.Read(rt); i.Read(rs); break;
				case 0x39: // SWC1: its value is wanted two cycles after it is ready
					i.cls = LS; i.lat = 2; i.is_store = true;
					i.Read(rs); i.late2_reads[i.n_late2++] = static_cast<u8>(R_FPR + rt); break;
				case 0x3E: // SQC2
					i.cls = LS; i.lat = 2; i.is_store = true;
					i.Read(rs); i.late2_reads[i.n_late2++] = static_cast<u8>(R_VF + rt); break;

				case 0x2F: // CACHE
					i.cls = SOLO; i.solo_cost = 3; i.Read(rs); break;
				case 0x33: // PREF
					i.cls = SOLO; i.solo_cost = 2; break;

				default:
					break;
			}

			// $zero is never waited for nor written.
			u8 n = 0;
			for (u8 k = 0; k < i.n_reads; k++)
				if (i.reads[k] != 0)
					i.reads[n++] = i.reads[k];
			i.n_reads = n;
			n = 0;
			for (u8 k = 0; k < i.n_writes; k++)
				if (i.writes[k] != 0)
					i.writes[n++] = i.writes[k];
			i.n_writes = n;
		}

		u32 LoadCharge(u32 k, u32 hundredths)
		{
			// round(k * L) - round((k - 1) * L), half up.
			return (k * hundredths + 50) / 100 - ((k - 1) * hundredths + 50) / 100;
		}

		u32 Cycles(const State& s)
		{
			return s.t + (s.n_in ? 1 : 0);
		}

		// The path's cycles once every unpipelined unit still running more than
		// two cycles past its end has finished (a divide issued near the end).
		u32 CyclesWithTail(const State& s)
		{
			u32 c = Cycles(s);
			for (u32 k = 0; k < s.n_units; k++)
			{
				const u32 free = s.unit_free[s.unit_order[k]];
				if (free > c && free - c > 2)
					c = free;
			}
			return c;
		}

		Br BranchKind(u32 code)
		{
			Info i;
			Classify(code, i);
			return i.br;
		}

		u32 FallThrough(u32 branch_pc) { return branch_pc + 8; }

		u32 Target(u32 pc, u32 code)
		{
			return pc + 4 + (static_cast<u32>(static_cast<s32>(static_cast<s16>(code & 0xFFFF))) << 2);
		}
	} // namespace

	void Reset(State& s)
	{
		std::memset(&s, 0, sizeof(s));
		s.cls_in = NONE;
		s.prev_load_dst = -1;
	}

	bool EndsPath(u32 code)
	{
		const u32 op = code >> 26;
		if (op == 0)
		{
			const u32 fn = Fn(code);
			return fn == 0x0C || fn == 0x0D || (fn >= 0x30 && fn <= 0x34) || fn == 0x36; // SYSCALL BREAK traps
		}
		if (op == 1)
		{
			const u32 rt = Rt(code);
			return (rt >= 0x08 && rt <= 0x0C) || rt == 0x0E; // traps with an immediate
		}
		return op == 0x10 && Rs(code) == 16 && (Fn(code) == 0x18 || Fn(code) == 0x38); // ERET, EI
	}

	bool IsBranch(u32 code)
	{
		return BranchKind(code) != Br::None;
	}

	u32 Step(State& s, u32 pc, u32 code, u32 load_hundredths)
	{
		u32 charge = 0;
		if (s.complete || (s.steps != 0 && !s.branch_pending && (pc & 0xFFF) == 0))
		{
			charge = EndCost(s, s.complete ? ExitFor(s, FallThrough(s.branch_pc)) : Exit::None);
			Reset(s);
		}

		Info inf;
		Classify(code, inf);
		const u32 before = Cycles(s);

		if (s.branch_pending)
		{
			s.branch_pending = false;
			s.complete = true; // the delay slot: the path ends here unless an exit says otherwise
		}
		else if (inf.br != Br::None)
		{
			s.branch_pending = true;
			s.branch_pc = pc;
			s.branch_code = code;
		}
		s.steps++;
		if (EndsPath(code))
			s.complete = true;

		u32 load_charge = 0;
		if (inf.is_load)
			load_charge = LoadCharge(++s.loads, load_hundredths);
		if (inf.jr_reg >= 0 && s.prev_load_dst >= 0 && inf.jr_reg == s.prev_load_dst)
			s.jr_after_load = true;
		s.prev_load_dst = -1;
		if ((inf.is_load || inf.is_mfc0) && inf.n_writes)
			s.prev_load_dst = inf.writes[0];

		if (inf.cls == SOLO)
		{
			if (s.n_in)
				s.t++;
			if (inf.wait_unit != U_NONE && s.unit_free[inf.wait_unit] > s.t)
				s.t = s.unit_free[inf.wait_unit];
			s.t += inf.solo_cost;
			s.n_in = 0;
			s.cls_in = NONE;
			s.n_prev_writes = 0;
			for (u8 k = 0; k < inf.n_writes; k++)
				s.ready[inf.writes[k]] = s.t;
			return charge + (Cycles(s) - before) + load_charge;
		}

		u32 need = 0;
		for (u8 k = 0; k < inf.n_reads; k++)
			need = std::max(need, s.ready[inf.reads[k]]);
		for (u8 k = 0; k < inf.n_late; k++)
			need = std::max(need, s.ready[inf.late_reads[k]] + 1);
		for (u8 k = 0; k < inf.n_late2; k++)
			need = std::max(need, s.ready[inf.late2_reads[k]] + 2);
		if (inf.unit != U_NONE)
			need = std::max(need, s.unit_free[inf.unit]);

		bool may = s.n_in == 1;
		if (may && inf.n_writes && s.n_prev_writes)
		{
			// Two writes of one register do not issue together.
			for (u8 a = 0; a < inf.n_writes && may; a++)
				for (u8 b = 0; b < s.n_prev_writes; b++)
					if (inf.writes[a] == s.prev_writes[b])
					{
						may = false;
						break;
					}
		}

		u32 at;
		if (may && need <= s.t && Pairs(s.cls_in, inf.cls))
		{
			at = s.t;
			s.n_in = 2;
		}
		else
		{
			if (s.n_in)
				s.t++;
			if (need > s.t)
				s.t = need;
			at = s.t;
			s.n_in = 1;
			s.cls_in = inf.cls;
		}

		s.n_prev_writes = inf.n_writes;
		for (u8 k = 0; k < inf.n_writes; k++)
		{
			s.prev_writes[k] = inf.writes[k];
			s.ready[inf.writes[k]] = at + inf.lat;
			if (inf.hilo_lat && inf.writes[k] >= R_HI0 && inf.writes[k] <= R_LO1)
				s.ready[inf.writes[k]] = at + inf.hilo_lat;
		}
		if (inf.unit != U_NONE)
		{
			bool seen = false;
			for (u8 k = 0; k < s.n_units; k++)
				seen |= (s.unit_order[k] == inf.unit);
			if (!seen)
				s.unit_order[s.n_units++] = inf.unit;
			s.unit_free[inf.unit] = at + inf.busy;
		}

		return charge + (Cycles(s) - before) + load_charge;
	}

	u32 EndCost(const State& s, Exit exit)
	{
		u32 c = CyclesWithTail(s) - Cycles(s);
		switch (exit)
		{
			case Exit::None:
			case Exit::NotTaken:
				break;
			case Exit::Taken:
				c += 1;
				break;
			case Exit::LikelyNotTaken:
				c += 3;
				break;
			case Exit::RegisterJump:
				c += s.jr_after_load ? 5 : 2;
				break;
		}
		return c;
	}

	Exit TakenExit(const State& s)
	{
		if (s.steps == 0 || s.branch_code == 0)
			return Exit::None;
		switch (BranchKind(s.branch_code))
		{
			case Br::RegJump: return Exit::RegisterJump;
			case Br::Jump:
			case Br::Cond:
			case Br::CondLikely: return Exit::Taken;
			default: return Exit::None;
		}
	}

	Exit ExitFor(const State& s, u32 target)
	{
		if (s.steps == 0 || s.branch_code == 0)
			return Exit::None;
		const u32 code = s.branch_code;
		switch (BranchKind(code))
		{
			case Br::RegJump:
				return Exit::RegisterJump;
			case Br::Jump:
				return Exit::Taken;
			case Br::CondLikely:
				if (s.branch_pending)
					return Exit::LikelyNotTaken; // left before the delay slot
				return Exit::Taken;
			case Br::Cond:
			{
				const u32 t = Target(s.branch_pc, code);
				if (t == FallThrough(s.branch_pc))
					return Exit::Taken;
				return target == t ? Exit::Taken : Exit::NotTaken;
			}
			default:
				return Exit::None;
		}
	}

	u32 PathCycles(u32 pc, const u32* words, u32 count, Exit exit, u32 load_hundredths)
	{
		State s;
		Reset(s);
		u32 c = 0;
		for (u32 k = 0; k < count; k++)
			c += Step(s, pc + 4 * k, words[k], load_hundredths);
		return c + EndCost(s, exit);
	}

	IssueResult Issue(u32 pc, const u32* words, u32 count)
	{
		State s;
		Reset(s);
		u32 c = 0;
		for (u32 k = 0; k < count; k++)
			c += Step(s, pc + 4 * k, words[k], 0);
		return {c + EndCost(s, Exit::None), s.loads, s.jr_after_load};
	}
} // namespace R5900::Issue
