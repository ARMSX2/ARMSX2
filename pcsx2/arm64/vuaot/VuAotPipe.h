// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Pipeline-time resolution for vu-aot: a port of microVU's first pass
// (mVUcompile's analysis loop, x86/microVU_Analyze.inl, _mVUflagPass /
// mVUsetFlagInfo, mVUsetFlags, mVUoptimizePipeState).
//
// vu-aot's timing model has to be microVU's: which flag instance an FMAC writes and a reader reads, which
// Q/P lane is current, stall cycles per pair, which flag updates the flag hack drops. So this runs
// microVU's own algorithm over its block structure and keys instead of re-deriving them, and
// pcsx2-vurunner --vuaot-ir checks the result against microVU's live decisions (VuAotMvuTrace.h).
// Names follow microVU's.

#include "VuAotDecode.h"
#include "VuAotMvuTrace.h"

#include "common/Pcsx2Defs.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace vuaot
{
	// Mirror of microRegInfo, byte for byte (microVU-arm64.h). It is both the
	// pipeline state flowing between blocks and microVU's block-lookup key.
	struct RegCycleInfo
	{
		u8 x : 4;
		u8 y : 4;
		u8 z : 4;
		u8 w : 4;
	};

	union alignas(16) PipeState
	{
		struct
		{
			union
			{
				struct
				{
					u8 needExactMatch;
					u8 flagInfo;
					u8 q;
					u8 p;
					u8 xgkick;
					u8 viBackUp;
					u8 blockType;
					u8 r;
				};
				u64 quick64[1];
				u32 quick32[2];
			};
			u32 xgkickcycles;
			u8 unused;
			u8 vi15v;
			u16 vi15;
			struct
			{
				u8 VI[16];
				RegCycleInfo VF[32];
			};
		};
		u64 full64[96 / sizeof(u64)];
		u32 full32[96 / sizeof(u32)];
		u8 bytes[96];

		PipeState() { std::memset(bytes, 0, sizeof(bytes)); }
		bool operator==(const PipeState& o) const { return std::memcmp(bytes, o.bytes, sizeof(bytes)) == 0; }
		bool operator!=(const PipeState& o) const { return !(*this == o); }
	};
	static_assert(sizeof(PipeState) == 96, "PipeState must mirror microRegInfo");

	struct PipeConfig
	{
		u32 vuIndex = 1;
		bool flagHack = true;    // EmuConfig.Speedhacks.vuFlagHack (shipped default on)
		bool xgkickHack = false; // Gamefix XgKickHack
		bool ibitHack = false;   // Gamefix IbitHack (only affects IADDI const-prop, which is off)

		u32 microMemSize() const { return vuIndex ? 0x4000u : 0x1000u; }
		u32 progMemMask() const { return microMemSize() / 4 - 1; }
	};

	struct MvuVFReg
	{
		u8 reg, x, y, z, w;
	};
	struct MvuVIReg
	{
		u8 reg, used;
	};
	struct MvuConst
	{
		u8 isValid;
		u32 regValue;
	};
	struct MvuUpper
	{
		bool eBit, iBit, mBit, tBit, dBit;
		MvuVFReg VF_write;
		MvuVFReg VF_read[2];
	};
	struct MvuLower
	{
		MvuVFReg VF_write;
		MvuVFReg VF_read[2];
		MvuVIReg VI_write;
		MvuVIReg VI_read[2];
		MvuConst constJump;
		u32 branch;
		u32 kickcycles;
		bool badBranch, evilBranch, isNOP, isFSSET, noWriteVF, backupVI, memReadIs, memReadIt, readFlags, isMemWrite, isKick;
	};
	struct MvuFlagInst
	{
		bool doFlag, doNonSticky;
		u8 write, lastWrite, read;
	};

	// Mirror of microOp.
	struct MvuOp
	{
		u8 stall;
		bool isBadOp, isEOB, isBdelay, swapOps, backupVF, doXGKICK;
		u32 XGKICKPC;
		bool doDivFlag;
		int readQ, writeQ, readP, writeP;
		MvuFlagInst sFlag, mFlag, cFlag;
		MvuUpper uOp;
		MvuLower lOp;
	};

	struct MvuFlagCycles
	{
		int xStatus[4];
		int xMac[4];
		int xClip[4];
		int cycles;
	};

	// How a microVU block ends, as mVUcompile's second pass decides it.
	enum class BlockEnd : u8
	{
		EBit,       // program end (E-bit, bad upper op, or the walk ran off its count)
		Branch,     // B/BAL: one successor
		CondBranch, // IBxx: taken + fall-through
		Jump,       // JR/JALR: target known only at run time
		EvilJump,   // branch in a branch delay slot (blockType 2): run-time select
		MBit,       // VU0 M-bit break
	};

	struct BlockResult
	{
		u32 startPC = 0;
		PipeState entry;     // microVU's block key (mVUpBlock->pState)
		PipeState end;       // successor key (mVUregs after setFlags + optimize)
		u32 count = 0;
		u32 cycles = 0;
		bool needFlagFinalize = false;
		bool flagHack = false;
		std::vector<MvuOp> ops;
		std::vector<u32> pcs;
		std::vector<InstPair> insts;
		MvuFlagCycles mFC = {};
		int xS = 0, xM = 0, xC = 0; // ring cursors after the block
		int qLane = 0, pLane = 0;   // mVU.q / mVU.p after the block: which qmmPQ lane holds Q / P

		BlockEnd endKind = BlockEnd::EBit;
		u32 branchKind = 0;         // microVU lOp.branch of the branch op (1..10)
		u32 branchIndex = 0;        // index in ops of the branch op
		u32 takenPC = 0;            // branch target (Branch / CondBranch / E-bit-on-branch TPC)
		u32 fallPC = 0;             // CondBranch fall-through
		bool eBitOnBranch = false;  // program ends after the delay slot, TPC = target
		bool tBitOnBranch = false;  // run-time FBRST check on the branch
		u32 endTPC = 0;             // TPC stored at a plain E-bit end
		std::vector<u32> tBitExits; // op indices of mid-block T-bit run-time exits

		void ToTrace(MvuBlockTrace& out, u32 vuIndex) const;
	};

	// While set (the live tier's background worker sets it around a job), every analyzer on this thread
	// marks each micro-memory word it reads (one byte per 32-bit word). The job's code is valid only while
	// those words are unchanged, which its publication checks.
	extern thread_local u8* g_pipeReadMask;

	// One analyzer per program image. Like microVU's IRinfo, its per-slot op records persist across blocks:
	// microVU reads a neighbouring slot's record in two places (setBranchA's delay-slot test and the flag
	// lookahead) without having written it in the current compile. Analysing blocks in microVU's compile
	// order reproduces those reads.
	class PipeAnalyzer
	{
	public:
		PipeAnalyzer(const u8* micro, const PipeConfig& cfg);

		BlockResult Analyze(u32 startPC, const PipeState& entry);

		const PipeConfig& Config() const { return cfg_; }

	private:
		// microVU state, same names
		const u8* micro_;
		std::vector<u32> passed_; // flagPass's visited start PCs (kept to reuse its buffer)
		PipeConfig cfg_;
		std::vector<MvuOp> info_;
		PipeState regs_;      // mVUregs
		PipeState pState_;    // mVUpBlock->pState
		struct
		{
			RegCycleInfo VF[2];
			u8 VFreg[2];
			u8 VI;
			u8 VIreg;
			u8 q, p, r, xgkick;
		} regsTemp_;
		MvuConst constReg_[16];
		u32 iPC_ = 0;
		u32 code_ = 0;
		u32 count_ = 0;
		u32 cycles_ = 0;
		u32 startPC_ = 0; // word index
		int branch_ = 0;  // mVUbranch
		int q_ = 0, p_ = 0;
		bool needFlagFinalize_ = false;
		bool sFlagHack_ = false;

		u32 curI() const;
		void incPC(int x);
		void incPC2(int x);
		void setCode() { code_ = curI(); }
		u32 xPC() const { return (iPC_ / 2) * 8; }
		MvuOp& info() { return info_[iPC_ / 2]; }
		bool isVU1() const { return cfg_.vuIndex != 0; }
		bool isEvilBlock() const { return pState_.blockType == 2; }
		u32 branchAddr() const;
		u32 bSaveAddr() const;

		u32 Ft() const { return (code_ >> 16) & 0x1F; }
		u32 Fs() const { return (code_ >> 11) & 0x1F; }
		u32 Fd() const { return (code_ >> 6) & 0x1F; }
		u32 It() const { return (code_ >> 16) & 0xF; }
		u32 Is() const { return (code_ >> 11) & 0xF; }
		u32 Id() const { return (code_ >> 6) & 0xF; }
		bool X() const { return (code_ >> 24) & 1; }
		bool Y() const { return (code_ >> 23) & 1; }
		bool Z() const { return (code_ >> 22) & 1; }
		bool W() const { return (code_ >> 21) & 1; }
		u32 Fsf() const { return (code_ >> 21) & 3; }
		u32 Ftf() const { return (code_ >> 23) & 3; }
		s32 Imm11() const { return (code_ & 0x400) ? static_cast<s32>(0xfffffc00u | (code_ & 0x3ff)) : static_cast<s32>(code_ & 0x3ff); }
		s16 Imm5() const { return static_cast<s16>(((code_ & 0x400) ? 0xfff0 : 0) | ((code_ >> 6) & 0xf)); }
		u32 Imm15() const { return ((code_ >> 10) & 0x7800) | (code_ & 0x7ff); }

		// microVU_Compile pipeline helpers
		void incCycles(int x);
		void setCycles();
		void optimizePipeState();

		// microVU_Analyze
		void analyzeReg1(int xReg, MvuVFReg& vfRead);
		void analyzeReg2(int xReg, MvuVFReg& vfWrite, bool isLowOp);
		void analyzeReg3(int xReg, MvuVFReg& vfRead);
		void analyzeReg4(int xReg, MvuVFReg& vfRead);
		void analyzeReg5(int xReg, int fxf, MvuVFReg& vfRead);
		void analyzeReg6(int xReg, MvuVFReg& vfRead);
		void analyzeVIreg1(int xReg, MvuVIReg& viRead);
		void analyzeVIreg2(int xReg, MvuVIReg& viWrite, int aCycles);
		void analyzeQreg(u8 x);
		void analyzePreg(u8 x);
		void setConstReg(int x, u32 v);
		void analyzeFMAC1(int Fd, int Fs, int Ft);
		void analyzeFMAC2(int Fs, int Ft);
		void analyzeFMAC3(int Fd, int Fs, int Ft);
		void analyzeFMAC4(int Fs, int Ft);
		void analyzeIALU1(int Id, int Is, int It);
		void analyzeIALU2(int Is, int It);
		void analyzeIADDI(int Is, int It, s16 imm);
		void analyzeMR32(int Fs, int Ft);
		void analyzeFDIV(int Fs, int Fsf, int Ft, int Ftf, u8 xCycles);
		void analyzeEFU1(int Fs, int Fsf, u8 xCycles);
		void analyzeEFU2(int Fs, u8 xCycles);
		void analyzeMFP(int Ft);
		void analyzeMOVE(int Fs, int Ft);
		void analyzeLQ(int Ft, int Is, bool writeIs);
		void analyzeSQ(int Fs, int It, bool writeIt);
		void analyzeR1(int Fs, int Fsf);
		void analyzeR2(int Ft, bool canBeNOP);
		void flagSet(bool setMacFlag);
		void analyzeSflag(int It);
		void analyzeFSSET();
		void analyzeMflag(int Is, int It);
		void analyzeCflag(int It);
		void analyzeXGkick(int Fs, int xCycles);
		void analyzeBranchVI(int xReg, bool& infoVar);
		int branchCheck();
		void analyzeCondBranch1(int Is);
		void analyzeCondBranch2(int Is, int It);
		void analyzeNormBranch(int It, bool isBAL);
		void analyzeJump(int Is, int It, bool isJALR);
		void setBranchA(int pass, int x, int x_);

		// op dispatch (pass 0 = analysis, pass 3 = flag lookahead)
		void opU(int pass);
		void opL(int pass);

		void flagPass(u32 startPC, u32 sCount, u32 found, std::vector<u32>& v);
		void setFlagInfo();
		void statusFlagOp();
		void setFlags(MvuFlagCycles& mFC, int& xS, int& xM, int& xC);

		// pass-1 helpers from mVUcompile
		void eBitPass1(int& branch);
		void branchWarning();
		void eBitWarning();
	};

	// microVU helpers the IR builder needs to read the ring.
	int FindFlagInst(const int* fFlag, int cycles);
	int SortFlag(const int* fFlag, int* bFlag, int cycles);
	int GetLastFlagInst(const PipeState& pState, const int* xFlag, int flagType, int isEbit);

	// Field-by-field comparison of two block traces. Returns one line per
	// mismatching field ("op 3 pc 0x0128 sFlag.read: mvu=1 vuaot=2").
	std::vector<std::string> CompareBlockTraces(const MvuBlockTrace& mvu, const MvuBlockTrace& ours);
} // namespace vuaot
