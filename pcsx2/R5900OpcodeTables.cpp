// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//all tables for R5900 are define here..

#include "R5900OpcodeTables.h"
#include "R5900.h"

namespace R5900
{
	namespace Opcodes
	{
		// Generates an entry for the given opcode name.
		// Assumes the default function naming schemes for interpreter and recompiler  functions.
#if defined(_M_X86) || defined(ARCH_ARM64) // ARM64 EE recompiler is supported
	#	define MakeOpcode( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::name, \
			::R5900::Dynarec::OpcodeImpl::rec##name, \
			::R5900::OpcodeDisasm::name \
		}

#	define MakeOpcodeM( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::MMI::name, \
			::R5900::Dynarec::OpcodeImpl::MMI::rec##name, \
			::R5900::OpcodeDisasm::name \
		}

#	define MakeOpcode0( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP0::name, \
			::R5900::Dynarec::OpcodeImpl::COP0::rec##name, \
			::R5900::OpcodeDisasm::name \
		}

	#	define MakeOpcode1( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP1::name, \
			::R5900::Dynarec::OpcodeImpl::COP1::rec##name, \
			::R5900::OpcodeDisasm::name \
		}
#else
	#	define MakeOpcode( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::name, \
			nullptr, \
			::R5900::OpcodeDisasm::name \
		}

#	define MakeOpcodeM( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::MMI::name, \
			nullptr, \
			::R5900::OpcodeDisasm::name \
		}

#	define MakeOpcode0( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP0::name, \
			nullptr, \
			::R5900::OpcodeDisasm::name \
		}

	#	define MakeOpcode1( name, flags ) \
		static const OPCODE name = { \
			#name, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP1::name, \
			nullptr, \
			::R5900::OpcodeDisasm::name \
		}
#endif

	#	define MakeOpcodeClass( name ) \
		static const OPCODE name = { \
			#name, \
			0, \
			R5900::Opcodes::Class_##name, \
			NULL, \
			NULL, \
			NULL \
		}

		MakeOpcode( Unknown, 0 );
		MakeOpcode( MMI_Unknown, 0 );
		MakeOpcode( COP0_Unknown, 0 );
		MakeOpcode( COP1_Unknown, 0 );

		// Class Subset Opcodes
		// (not really opcodes, but rather entire subsets of other opcode classes)

		MakeOpcodeClass( SPECIAL );
		MakeOpcodeClass( REGIMM );
		//MakeOpcodeClass( COP2 );
		MakeOpcodeClass( MMI );
		MakeOpcodeClass( MMI0 );
		MakeOpcodeClass( MMI2 );
		MakeOpcodeClass( MMI1 );
		MakeOpcodeClass( MMI3 );

		MakeOpcodeClass( COP0 );
		MakeOpcodeClass( COP1 );

		// Misc Junk

		MakeOpcode( COP2, 0 );

		MakeOpcode( CACHE, 0 );
		MakeOpcode( PREF, 0 );
		MakeOpcode( SYSCALL, IS_BRANCH|BRANCHTYPE_SYSCALL );
		MakeOpcode( BREAK, 0 );
		MakeOpcode( SYNC, 0 );

		// Branch/Jump Opcodes

		MakeOpcode( J , IS_BRANCH|BRANCHTYPE_JUMP );
		MakeOpcode( JAL, IS_BRANCH|BRANCHTYPE_JUMP|IS_LINKED );
		MakeOpcode( JR, IS_BRANCH|BRANCHTYPE_REGISTER );
		MakeOpcode( JALR, IS_BRANCH|BRANCHTYPE_REGISTER|IS_LINKED );

		MakeOpcode( BEQ, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ );
		MakeOpcode( BNE, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE );
		MakeOpcode( BLEZ, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ );
		MakeOpcode( BGTZ, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ );
		MakeOpcode( BEQL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ|IS_LIKELY );
		MakeOpcode( BNEL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE|IS_LIKELY );
		MakeOpcode( BLEZL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ|IS_LIKELY );
		MakeOpcode( BGTZL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ|IS_LIKELY );
		MakeOpcode( BLTZ, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ );
		MakeOpcode( BGEZ, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ );
		MakeOpcode( BLTZL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LIKELY );
		MakeOpcode( BGEZL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LIKELY );
		MakeOpcode( BLTZAL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED );
		MakeOpcode( BGEZAL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED );
		MakeOpcode( BLTZALL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED|IS_LIKELY );
		MakeOpcode( BGEZALL, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED|IS_LIKELY );

		MakeOpcode( TGEI, 0 );
		MakeOpcode( TGEIU, 0 );
		MakeOpcode( TLTI, 0 );
		MakeOpcode( TLTIU, 0 );
		MakeOpcode( TEQI, 0 );
		MakeOpcode( TNEI, 0 );
		MakeOpcode( TGE, 0 );
		MakeOpcode( TGEU, 0 );
		MakeOpcode( TLT, 0 );
		MakeOpcode( TLTU, 0 );
		MakeOpcode( TEQ, 0 );
		MakeOpcode( TNE, 0 );

		// Arithmetic

		MakeOpcode( MULT, 0 );
		MakeOpcode( MULTU, 0 );
		MakeOpcode( MULT1, 0 );
		MakeOpcode( MULTU1, 0 );
		MakeOpcode( MADD, 0 );
		MakeOpcode( MADDU, 0 );
		MakeOpcode( MADD1, 0 );
		MakeOpcode( MADDU1, 0 );
		MakeOpcode( DIV, 0 );
		MakeOpcode( DIVU, 0 );
		MakeOpcode( DIV1, 0 );
		MakeOpcode( DIVU1, 0 );

		MakeOpcode( ADDI, IS_ALU|ALUTYPE_ADDI );
		MakeOpcode( ADDIU, IS_ALU|ALUTYPE_ADDI );
		MakeOpcode( DADDI, IS_ALU|ALUTYPE_ADDI|IS_64BIT );
		MakeOpcode( DADDIU, IS_ALU|ALUTYPE_ADDI|IS_64BIT );
		MakeOpcode( DADD, IS_ALU|ALUTYPE_ADD|IS_64BIT );
		MakeOpcode( DADDU, IS_ALU|ALUTYPE_ADD|IS_64BIT );
		MakeOpcode( DSUB, IS_ALU|ALUTYPE_SUB|IS_64BIT );
		MakeOpcode( DSUBU, IS_ALU|ALUTYPE_SUB|IS_64BIT );
		MakeOpcode( ADD, IS_ALU|ALUTYPE_ADD );
		MakeOpcode( ADDU, IS_ALU|ALUTYPE_ADD );
		MakeOpcode( SUB, IS_ALU|ALUTYPE_SUB );
		MakeOpcode( SUBU, IS_ALU|ALUTYPE_SUB );

		MakeOpcode( ANDI, 0 );
		MakeOpcode( ORI, 0 );
		MakeOpcode( XORI, 0 );
		MakeOpcode( AND, 0 );
		MakeOpcode( OR, 0 );
		MakeOpcode( XOR, 0 );
		MakeOpcode( NOR, 0 );
		MakeOpcode( SLTI, 0 );
		MakeOpcode( SLTIU, 0 );
		MakeOpcode( SLT, 0 );
		MakeOpcode( SLTU, 0 );
		MakeOpcode( LUI, 0 );
		MakeOpcode( SLL, 0 );
		MakeOpcode( SRL, 0 );
		MakeOpcode( SRA, 0 );
		MakeOpcode( SLLV, 0 );
		MakeOpcode( SRLV, 0 );
		MakeOpcode( SRAV, 0 );
		MakeOpcode( MOVZ, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_EQ );
		MakeOpcode( MOVN, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_NE );
		MakeOpcode( DSLLV, 0 );
		MakeOpcode( DSRLV, 0 );
		MakeOpcode( DSRAV, 0 );
		MakeOpcode( DSLL, 0 );
		MakeOpcode( DSRL, 0 );
		MakeOpcode( DSRA, 0 );
		MakeOpcode( DSLL32, 0 );
		MakeOpcode( DSRL32, 0 );
		MakeOpcode( DSRA32, 0 );

		MakeOpcode( MFHI, 0 );
		MakeOpcode( MTHI, 0 );
		MakeOpcode( MFLO, 0 );
		MakeOpcode( MTLO, 0 );
		MakeOpcode( MFSA, 0 );
		MakeOpcode( MTSA, 0 );
		MakeOpcode( MTSAB, 0 );
		MakeOpcode( MTSAH, 0 );
		MakeOpcode( MFHI1, 0 );
		MakeOpcode( MTHI1, 0 );
		MakeOpcode( MFLO1, 0 );
		MakeOpcode( MTLO1, 0 );

		// Loads!

		MakeOpcode( LDL, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_LEFT );
		MakeOpcode( LDR, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_RIGHT );
		MakeOpcode( LQ, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD );
		MakeOpcode( LB, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE );
		MakeOpcode( LH, IS_MEMORY|IS_LOAD|MEMTYPE_HALF );
		MakeOpcode( LWL, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_LEFT );
		MakeOpcode( LW, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LBU, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE );
		MakeOpcode( LHU, IS_MEMORY|IS_LOAD|MEMTYPE_HALF );
		MakeOpcode( LWR, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_RIGHT );
		MakeOpcode( LWU, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LWC1, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LQC2, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD );
		MakeOpcode( LD, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD );

		// Stores!

		MakeOpcode( SQ, IS_MEMORY|IS_STORE|MEMTYPE_QWORD );
		MakeOpcode( SB, IS_MEMORY|IS_STORE|MEMTYPE_BYTE );
		MakeOpcode( SH, IS_MEMORY|IS_STORE|MEMTYPE_HALF );
		MakeOpcode( SWL, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_LEFT );
		MakeOpcode( SW, IS_MEMORY|IS_STORE|MEMTYPE_WORD );
		MakeOpcode( SDL, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_LEFT );
		MakeOpcode( SDR, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_RIGHT );
		MakeOpcode( SWR, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_RIGHT );
		MakeOpcode( SWC1, IS_MEMORY|IS_STORE|MEMTYPE_WORD );
		MakeOpcode( SQC2, IS_MEMORY|IS_STORE|MEMTYPE_QWORD );
		MakeOpcode( SD, IS_MEMORY|IS_STORE|MEMTYPE_DWORD );


		// Multimedia Instructions!

		MakeOpcodeM( PLZCW, 0 );
		MakeOpcodeM( PMFHL, 0 );
		MakeOpcodeM( PMTHL, 0 );
		MakeOpcodeM( PSLLH, 0 );
		MakeOpcodeM( PSRLH, 0 );
		MakeOpcodeM( PSRAH, 0 );
		MakeOpcodeM( PSLLW, 0 );
		MakeOpcodeM( PSRLW, 0 );
		MakeOpcodeM( PSRAW, 0 );

		MakeOpcodeM( PADDW, 0 );
		MakeOpcodeM( PADDH, 0 );
		MakeOpcodeM( PADDB, 0 );
		MakeOpcodeM( PADDSW, 0 );
		MakeOpcodeM( PADDSH, 0 );
		MakeOpcodeM( PADDSB, 0 );
		MakeOpcodeM( PADDUW, 0 );
		MakeOpcodeM( PADDUH, 0 );
		MakeOpcodeM( PADDUB, 0 );
		MakeOpcodeM( PSUBW, 0 );
		MakeOpcodeM( PSUBH, 0 );
		MakeOpcodeM( PSUBB, 0 );
		MakeOpcodeM( PSUBSW, 0 );
		MakeOpcodeM( PSUBSH, 0 );
		MakeOpcodeM( PSUBSB, 0 );
		MakeOpcodeM( PSUBUW, 0 );
		MakeOpcodeM( PSUBUH, 0 );
		MakeOpcodeM( PSUBUB, 0 );

		MakeOpcodeM( PCGTW, 0 );
		MakeOpcodeM( PMAXW, 0 );
		MakeOpcodeM( PMAXH, 0 );
		MakeOpcodeM( PCGTH, 0 );
		MakeOpcodeM( PCGTB, 0 );
		MakeOpcodeM( PEXTLW, 0 );
		MakeOpcodeM( PEXTLH, 0 );
		MakeOpcodeM( PEXTLB, 0 );
		MakeOpcodeM( PEXT5, 0 );
		MakeOpcodeM( PPACW, 0 );
		MakeOpcodeM( PPACH, 0 );
		MakeOpcodeM( PPACB, 0 );
		MakeOpcodeM( PPAC5, 0 );

		MakeOpcodeM( PABSW, 0 );
		MakeOpcodeM( PABSH, 0 );
		MakeOpcodeM( PCEQW, 0 );
		MakeOpcodeM( PMINW, 0 );
		MakeOpcodeM( PMINH, 0 );
		MakeOpcodeM( PADSBH, 0 );
		MakeOpcodeM( PCEQH, 0 );
		MakeOpcodeM( PCEQB, 0 );
		MakeOpcodeM( PEXTUW, 0 );
		MakeOpcodeM( PEXTUH, 0 );
		MakeOpcodeM( PEXTUB, 0 );
		MakeOpcodeM( PSLLVW, 0 );
		MakeOpcodeM( PSRLVW, 0 );

		MakeOpcodeM( QFSRV, 0 );

		MakeOpcodeM( PMADDH, 0 );
		MakeOpcodeM( PHMADH, 0 );
		MakeOpcodeM( PMSUBH, 0 );
		MakeOpcodeM( PHMSBH, 0 );
		MakeOpcodeM( PMULTH, 0 );
		MakeOpcodeM( PMADDW, 0 );
		MakeOpcodeM( PMSUBW, 0 );
		MakeOpcodeM( PMFHI, 0 );
		MakeOpcodeM( PMFLO, 0 );
		MakeOpcodeM( PMULTW, 0 );
		MakeOpcodeM( PMADDUW, 0 );
		MakeOpcodeM( PMULTUW, 0 );
		MakeOpcodeM( PDIVUW, 0 );
		MakeOpcodeM( PDIVW, 0 );
		MakeOpcodeM( PDIVBW, 0 );

		MakeOpcodeM( PINTH, 0 );
		MakeOpcodeM( PCPYLD, 0 );
		MakeOpcodeM( PAND, 0 );
		MakeOpcodeM( PXOR, 0 );
		MakeOpcodeM( PEXEH, 0 );
		MakeOpcodeM( PREVH, 0 );
		MakeOpcodeM( PEXEW, 0 );
		MakeOpcodeM( PROT3W, 0 );

		MakeOpcodeM( PSRAVW, 0 );
		MakeOpcodeM( PMTHI, 0 );
		MakeOpcodeM( PMTLO, 0 );
		MakeOpcodeM( PINTEH, 0 );
		MakeOpcodeM( PCPYUD, 0 );
		MakeOpcodeM( POR, 0 );
		MakeOpcodeM( PNOR, 0 );
		MakeOpcodeM( PEXCH, 0 );
		MakeOpcodeM( PCPYH, 0 );
		MakeOpcodeM( PEXCW, 0 );

		//////////////////////////////////////////////////////////
		// COP0 Instructions

		MakeOpcodeClass( COP0_C0 );
		MakeOpcodeClass( COP0_BC0 );

		MakeOpcode0( MFC0, 0 );
		MakeOpcode0( MTC0, 0 );

		MakeOpcode0(BC0F, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ);
		MakeOpcode0(BC0T, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE);
		MakeOpcode0(BC0FL, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ | IS_LIKELY);
		MakeOpcode0(BC0TL, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE | IS_LIKELY);

		MakeOpcode0( TLBR, 0 );
		MakeOpcode0( TLBWI, 0 );
		MakeOpcode0( TLBWR, 0 );
		MakeOpcode0( TLBP, 0 );
		MakeOpcode0( ERET, IS_BRANCH|BRANCHTYPE_ERET );
		MakeOpcode0( EI, 0 );
		MakeOpcode0( DI, 0 );

		//////////////////////////////////////////////////////////
		// COP1 Instructions!

		MakeOpcodeClass( COP1_BC1 );
		MakeOpcodeClass( COP1_S );
		MakeOpcodeClass( COP1_W );		// contains CVT_S instruction *only*

		MakeOpcode1( MFC1, 0 );
		MakeOpcode1( CFC1, 0 );
		MakeOpcode1( MTC1, 0 );
		MakeOpcode1( CTC1, 0 );

		MakeOpcode1( BC1F, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ );
		MakeOpcode1( BC1T, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE );
		MakeOpcode1( BC1FL, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ|IS_LIKELY );
		MakeOpcode1( BC1TL, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE|IS_LIKELY );

		MakeOpcode1( ADD_S, 0 );
		MakeOpcode1( ADDA_S, 0 );
		MakeOpcode1( SUB_S, 0 );
		MakeOpcode1( SUBA_S, 0 );

		MakeOpcode1( ABS_S, 0 );
		MakeOpcode1( MOV_S, 0 );
		MakeOpcode1( NEG_S, 0 );
		MakeOpcode1( MAX_S, 0 );
		MakeOpcode1( MIN_S, 0 );

		MakeOpcode1( MUL_S, 0 );
		MakeOpcode1( DIV_S, 0 );
		MakeOpcode1( SQRT_S, 0 );
		MakeOpcode1( RSQRT_S, 0 );
		MakeOpcode1( MULA_S, 0 );
		MakeOpcode1( MADD_S, 0 );
		MakeOpcode1( MSUB_S, 0 );
		MakeOpcode1( MADDA_S, 0 );
		MakeOpcode1( MSUBA_S, 0 );

		MakeOpcode1( C_F, 0 );
		MakeOpcode1( C_EQ, 0 );
		MakeOpcode1( C_LT, 0 );
		MakeOpcode1( C_LE, 0 );

		MakeOpcode1( CVT_S, 0 );
		MakeOpcode1( CVT_W, 0 );
	}

	namespace OpcodeTables
	{
		using namespace Opcodes;

		const OPCODE tbl_Standard[64] =
		{
			SPECIAL,       REGIMM,        J,             JAL,     BEQ,           BNE,     BLEZ,  BGTZ,
			ADDI,          ADDIU,         SLTI,          SLTIU,   ANDI,          ORI,     XORI,  LUI,
			COP0,          COP1,          COP2,          Unknown, BEQL,          BNEL,    BLEZL, BGTZL,
			DADDI,         DADDIU,        LDL,           LDR,     MMI,           Unknown, LQ,    SQ,
			LB,            LH,            LWL,           LW,      LBU,           LHU,     LWR,   LWU,
			SB,            SH,            SWL,           SW,      SDL,           SDR,     SWR,   CACHE,
			Unknown,       LWC1,          Unknown,       PREF,    Unknown,       Unknown, LQC2,  LD,
			Unknown,       SWC1,          Unknown,       Unknown, Unknown,       Unknown, SQC2,  SD
		};

		static const OPCODE tbl_Special[64] =
		{
			SLL,      Unknown,  SRL,      SRA,      SLLV,    Unknown, SRLV,    SRAV,
			JR,       JALR,     MOVZ,     MOVN,     SYSCALL, BREAK,   Unknown, SYNC,
			MFHI,     MTHI,     MFLO,     MTLO,     DSLLV,   Unknown, DSRLV,   DSRAV,
			MULT,     MULTU,    DIV,      DIVU,     Unknown, Unknown, Unknown, Unknown,
			ADD,      ADDU,     SUB,      SUBU,     AND,     OR,      XOR,     NOR,
			MFSA,     MTSA,     SLT,      SLTU,     DADD,    DADDU,   DSUB,    DSUBU,
			TGE,      TGEU,     TLT,      TLTU,     TEQ,     Unknown, TNE,     Unknown,
			DSLL,     Unknown,  DSRL,     DSRA,     DSLL32,  Unknown, DSRL32,  DSRA32
		};

		static const OPCODE tbl_RegImm[32] = {
			BLTZ,   BGEZ,   BLTZL,      BGEZL,   Unknown, Unknown, Unknown, Unknown,
			TGEI,   TGEIU,  TLTI,       TLTIU,   TEQI,    Unknown, TNEI,    Unknown,
			BLTZAL, BGEZAL, BLTZALL,    BGEZALL, Unknown, Unknown, Unknown, Unknown,
			MTSAB,  MTSAH , Unknown,    Unknown, Unknown, Unknown, Unknown, Unknown,
		};

		static const OPCODE tbl_MMI[64] =
		{
			MADD,               MADDU,           MMI_Unknown,          MMI_Unknown,          PLZCW,            MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MMI0,      MMI2,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MFHI1,              MTHI1,           MFLO1,                MTLO1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MULT1,              MULTU1,          DIV1,                 DIVU1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MADD1,              MADDU1,          MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MMI1,      MMI3,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			PMFHL,              PMTHL,           MMI_Unknown,          MMI_Unknown,          PSLLH,            MMI_Unknown,       PSRLH,                PSRAH,
			MMI_Unknown,        MMI_Unknown,     MMI_Unknown,          MMI_Unknown,          PSLLW,            MMI_Unknown,       PSRLW,                PSRAW,
		};

		static const OPCODE tbl_MMI0[32] =
		{
			PADDW,         PSUBW,         PCGTW,          PMAXW,
			PADDH,         PSUBH,         PCGTH,          PMAXH,
			PADDB,         PSUBB,         PCGTB,          MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,    MMI_Unknown,
			PADDSW,        PSUBSW,        PEXTLW,         PPACW,
			PADDSH,        PSUBSH,        PEXTLH,         PPACH,
			PADDSB,        PSUBSB,        PEXTLB,         PPACB,
			MMI_Unknown,   MMI_Unknown,   PEXT5,          PPAC5,
		};

		static const OPCODE tbl_MMI1[32] =
		{
			MMI_Unknown,   PABSW,         PCEQW,         PMINW,
			PADSBH,        PABSH,         PCEQH,         PMINH,
			MMI_Unknown,   MMI_Unknown,   PCEQB,         MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PADDUW,        PSUBUW,        PEXTUW,        MMI_Unknown,
			PADDUH,        PSUBUH,        PEXTUH,        MMI_Unknown,
			PADDUB,        PSUBUB,        PEXTUB,        QFSRV,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
		};


		static const OPCODE tbl_MMI2[32] =
		{
			PMADDW,        MMI_Unknown,   PSLLVW,        PSRLVW,
			PMSUBW,        MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PMFHI,         PMFLO,         PINTH,         MMI_Unknown,
			PMULTW,        PDIVW,         PCPYLD,        MMI_Unknown,
			PMADDH,        PHMADH,        PAND,          PXOR,
			PMSUBH,        PHMSBH,        MMI_Unknown,   MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   PEXEH,         PREVH,
			PMULTH,        PDIVBW,        PEXEW,         PROT3W,
		};

		static const OPCODE tbl_MMI3[32] =
		{
			PMADDUW,       MMI_Unknown,   MMI_Unknown,   PSRAVW,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PMTHI,         PMTLO,         PINTEH,        MMI_Unknown,
			PMULTUW,       PDIVUW,        PCPYUD,        MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   POR,           PNOR,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   PEXCH,         PCPYH,
			MMI_Unknown,   MMI_Unknown,   PEXCW,         MMI_Unknown,
		};

		static const OPCODE tbl_COP0[32] =
		{
			MFC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown, MTC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_BC0, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_C0,  COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
		};

		static const OPCODE tbl_COP0_BC0[32] =
		{
			BC0F,         BC0T,         BC0FL,        BC0TL,        COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
		};

		static const OPCODE tbl_COP0_C0[64] =
		{
			COP0_Unknown, TLBR,         TLBWI,        COP0_Unknown, COP0_Unknown, COP0_Unknown, TLBWR,        COP0_Unknown,
			TLBP,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			ERET,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			EI,           DI,           COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown
		};

		static const OPCODE tbl_COP1[32] =
		{
			MFC1,         COP1_Unknown, CFC1,         COP1_Unknown, MTC1,         COP1_Unknown, CTC1,         COP1_Unknown,
			COP1_BC1, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_S,   COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_W, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
		};

		static const OPCODE tbl_COP1_BC1[32] =
		{
			BC1F,         BC1T,         BC1FL,        BC1TL,        COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
		};

		static const OPCODE tbl_COP1_S[64] =
		{
			ADD_S,       SUB_S,       MUL_S,       DIV_S,       SQRT_S,      ABS_S,       MOV_S,       NEG_S,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,RSQRT_S,     COP1_Unknown,
			ADDA_S,      SUBA_S,      MULA_S,      COP1_Unknown,MADD_S,      MSUB_S,      MADDA_S,     MSUBA_S,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,CVT_W,       COP1_Unknown,COP1_Unknown,COP1_Unknown,
			MAX_S,       MIN_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			C_F,         COP1_Unknown,C_EQ,        COP1_Unknown,C_LT,        COP1_Unknown,C_LE,        COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
		};

		static const OPCODE tbl_COP1_W[64] =
		{
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			CVT_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
		};

	}	// end namespace R5900::OpcodeTables

	namespace Opcodes
	{
		using namespace OpcodeTables;

		const OPCODE& Class_SPECIAL(u32 op) { return tbl_Special[op & 0x3F]; }
		const OPCODE& Class_REGIMM(u32 op)  { return tbl_RegImm[(op >> 16) & 0x1F]; }

		const OPCODE& Class_MMI(u32 op)  { return tbl_MMI[op & 0x3F]; }
		const OPCODE& Class_MMI0(u32 op) { return tbl_MMI0[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI1(u32 op) { return tbl_MMI1[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI2(u32 op) { return tbl_MMI2[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI3(u32 op) { return tbl_MMI3[(op >> 6) & 0x1F]; }

		const OPCODE& Class_COP0(u32 op) { return tbl_COP0[(op >> 21) & 0x1F]; }
		const OPCODE& Class_COP0_BC0(u32 op) { return tbl_COP0_BC0[(op >> 16) & 0x03]; }
		const OPCODE& Class_COP0_C0(u32 op) { return tbl_COP0_C0[op & 0x3F]; }

		const OPCODE& Class_COP1(u32 op) { return tbl_COP1[(op >> 21) & 0x1F]; }
		const OPCODE& Class_COP1_BC1(u32 op) { return tbl_COP1_BC1[(op >> 16) & 0x1F]; }
		const OPCODE& Class_COP1_S(u32 op) { return tbl_COP1_S[op & 0x3F]; }
		const OPCODE& Class_COP1_W(u32 op) { return tbl_COP1_W[op & 0x3F]; }

		// These are for future use when the COP2 tables are completed.
		//const OPCODE& Class_COP2() { return tbl_COP2[_Rs_]; }
		//const OPCODE& Class_COP2_BC2() { return tbl_COP2_BC2[_Rt_]; }
		//const OPCODE& Class_COP2_SPECIAL() { return tbl_COP2_SPECIAL[_Funct_]; }
		//const OPCODE& Class_COP2_SPECIAL2() { return tbl_COP2_SPECIAL2[(cpuRegs.code & 0x3) | ((cpuRegs.code >> 4) & 0x7c)]; }
	}
}	// end namespace R5900

void (*Int_COP2PrintTable[32])() = {
    COP2_Unknown, QMFC2,        CFC2,         COP2_Unknown, COP2_Unknown, QMTC2,        CTC2,         COP2_Unknown,
    COP2_BC2,     COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
	COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
};

void (*Int_COP2BC2PrintTable[32])() = {
    BC2F,         BC2T,         BC2FL,        BC2TL,        COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
};

void (*Int_COP2SPECIAL1PrintTable[64])() =
{
 VADDx,       VADDy,       VADDz,       VADDw,       VSUBx,        VSUBy,        VSUBz,        VSUBw,
 VMADDx,      VMADDy,      VMADDz,      VMADDw,      VMSUBx,       VMSUBy,       VMSUBz,       VMSUBw,
 VMAXx,       VMAXy,       VMAXz,       VMAXw,       VMINIx,       VMINIy,       VMINIz,       VMINIw,
 VMULx,       VMULy,       VMULz,       VMULw,       VMULq,        VMAXi,        VMULi,        VMINIi,
 VADDq,       VMADDq,      VADDi,       VMADDi,      VSUBq,        VMSUBq,       VSUBi,        VMSUBi,
 VADD,        VMADD,       VMUL,        VMAX,        VSUB,         VMSUB,        VOPMSUB,      VMINI,
 VIADD,       VISUB,       VIADDI,      COP2_Unknown,VIAND,        VIOR,         COP2_Unknown, COP2_Unknown,
 VCALLMS,     VCALLMSR,    COP2_Unknown,COP2_Unknown,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,
};

void (*Int_COP2SPECIAL2PrintTable[128])() =
{
 VADDAx      ,VADDAy      ,VADDAz      ,VADDAw      ,VSUBAx      ,VSUBAy      ,VSUBAz      ,VSUBAw,
 VMADDAx     ,VMADDAy     ,VMADDAz     ,VMADDAw     ,VMSUBAx     ,VMSUBAy     ,VMSUBAz     ,VMSUBAw,
 VITOF0      ,VITOF4      ,VITOF12     ,VITOF15     ,VFTOI0      ,VFTOI4      ,VFTOI12     ,VFTOI15,
 VMULAx      ,VMULAy      ,VMULAz      ,VMULAw      ,VMULAq      ,VABS        ,VMULAi      ,VCLIPw,
 VADDAq      ,VMADDAq     ,VADDAi      ,VMADDAi     ,VSUBAq      ,VMSUBAq     ,VSUBAi      ,VMSUBAi,
 VADDA       ,VMADDA      ,VMULA       ,COP2_Unknown,VSUBA       ,VMSUBA      ,VOPMULA     ,VNOP,
 VMOVE       ,VMR32       ,COP2_Unknown,COP2_Unknown,VLQI        ,VSQI        ,VLQD        ,VSQD,
 VDIV        ,VSQRT       ,VRSQRT      ,VWAITQ      ,VMTIR       ,VMFIR       ,VILWR       ,VISWR,
 VRNEXT      ,VRGET       ,VRINIT      ,VRXOR       ,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
};
