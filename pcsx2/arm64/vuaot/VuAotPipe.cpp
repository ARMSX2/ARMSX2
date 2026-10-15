// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Port of microVU's first pass. Each function names the microVU function it
// mirrors; keep them in step. The line-by-line shape is deliberate: the point
// is that a reader can hold this file next to microVU_Compile-arm64.inl,
// x86/microVU_Analyze.inl and microVU_Flags-arm64.inl and see the same
// algorithm, and that pcsx2-vurunner --vuaot-ir can prove it.

#include "VuAotPipe.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace vuaot
{
	namespace
	{
		constexpr u32 kEbit = 1u << 30;
		constexpr u32 kMbit = 1u << 29;
		constexpr u32 kDbit = 1u << 28;
		constexpr u32 kTbit = 1u << 27;
		constexpr u32 kIbit = 1u << 31;
		constexpr bool doConstProp = false;
		constexpr bool doDBitHandling = false;
		constexpr bool noFlagOpts = false;

		u8 optimizeReg(u8 rState) { return (rState == 1) ? 0 : rState; }
		u8 calcCycles(u8 reg, u8 x) { return (reg > x) ? (reg - x) : 0; }
		u8 tCycles(u8 dest, u8 src) { return std::max(dest, src); }
	} // namespace

	int FindFlagInst(const int* fFlag, int cycles)
	{
		int j = 0, jValue = -1;
		for (int i = 0; i < 4; i++)
		{
			if ((fFlag[i] <= cycles) && (fFlag[i] > jValue))
			{
				j = i;
				jValue = fFlag[i];
			}
		}
		return j;
	}

	int SortFlag(const int* fFlag, int* bFlag, int cycles)
	{
		int lFlag = -5;
		int x = 0;
		for (int i = 0; i < 4; i++)
		{
			bFlag[i] = FindFlagInst(fFlag, cycles);
			if (lFlag != bFlag[i])
				x++;
			lFlag = bFlag[i];
			cycles++;
		}
		return x;
	}

	// getIncomingFlagInst + getLastFlagInst (microVU_Branch-arm64.inl).
	int GetLastFlagInst(const PipeState& pState, const int* xFlag, int flagType, int isEbit)
	{
		const auto incoming = [&]() { return (((pState.flagInfo >> (2 * flagType + 2)) & 3) - 1) & 3; };
		if (isEbit)
		{
			const int inst = FindFlagInst(xFlag, 0x7fffffff);
			if (xFlag[inst] < 0)
				return incoming();
			return inst;
		}
		if (pState.needExactMatch & (1 << flagType))
			return 3;
		return incoming();
	}

	PipeAnalyzer::PipeAnalyzer(const u8* micro, const PipeConfig& cfg)
		: micro_(micro)
		, cfg_(cfg)
	{
		info_.resize(cfg_.microMemSize() / 8);
		std::memset(info_.data(), 0, info_.size() * sizeof(MvuOp));
		std::memset(&regsTemp_, 0, sizeof(regsTemp_));
		std::memset(constReg_, 0, sizeof(constReg_));
	}

	thread_local u8* g_pipeReadMask = nullptr;

	u32 PipeAnalyzer::curI() const
	{
		u32 w;
		std::memcpy(&w, micro_ + iPC_ * 4, 4);
		if (g_pipeReadMask)
			g_pipeReadMask[iPC_] = 1;
		return w;
	}

	void PipeAnalyzer::incPC(int x)
	{
		iPC_ = (iPC_ + x) & cfg_.progMemMask();
		code_ = curI();
	}

	void PipeAnalyzer::incPC2(int x)
	{
		iPC_ = (iPC_ + x) & cfg_.progMemMask();
	}

	u32 PipeAnalyzer::branchAddr() const
	{
		return (((iPC_ + 2) + (Imm11() * 2)) & cfg_.progMemMask()) * 4;
	}

	u32 PipeAnalyzer::bSaveAddr() const
	{
		return ((xPC() + 16) & (cfg_.microMemSize() - 8)) / 8;
	}

	//------------------------------------------------------------------
	// microVU_Compile-arm64.inl: pipeline helpers
	//------------------------------------------------------------------

	void PipeAnalyzer::optimizePipeState()
	{
		for (int i = 0; i < 32; i++)
		{
			regs_.VF[i].x = optimizeReg(regs_.VF[i].x);
			regs_.VF[i].y = optimizeReg(regs_.VF[i].y);
			regs_.VF[i].z = optimizeReg(regs_.VF[i].z);
			regs_.VF[i].w = optimizeReg(regs_.VF[i].w);
		}
		for (int i = 0; i < 16; i++)
			regs_.VI[i] = optimizeReg(regs_.VI[i]);
		if (regs_.q) { regs_.q = optimizeReg(regs_.q); if (!regs_.q) { q_ ^= 1; } }
		if (regs_.p) { regs_.p = optimizeReg(regs_.p); if (!regs_.p) { p_ ^= 1; } }
		regs_.r = 0;
	}

	void PipeAnalyzer::incCycles(int x)
	{
		cycles_ += x;
		for (int z = 31; z > 0; z--)
		{
			regs_.VF[z].x = calcCycles(regs_.VF[z].x, x);
			regs_.VF[z].y = calcCycles(regs_.VF[z].y, x);
			regs_.VF[z].z = calcCycles(regs_.VF[z].z, x);
			regs_.VF[z].w = calcCycles(regs_.VF[z].w, x);
		}
		for (int z = 15; z > 0; z--)
			regs_.VI[z] = calcCycles(regs_.VI[z], x);
		if (regs_.q)
		{
			if (regs_.q > 4)
			{
				regs_.q = calcCycles(regs_.q, x);
				if (regs_.q <= 4)
					info().doDivFlag = 1;
			}
			else
			{
				regs_.q = calcCycles(regs_.q, x);
			}
			if (!regs_.q)
				q_ ^= 1;
		}
		if (regs_.p)
		{
			regs_.p = calcCycles(regs_.p, x);
			if (!regs_.p || regsTemp_.p)
				p_ ^= 1;
		}
		if (regs_.xgkick)
		{
			regs_.xgkick = calcCycles(regs_.xgkick, x);
			if (!regs_.xgkick)
			{
				info().doXGKICK = 1;
				info().XGKICKPC = xPC();
			}
		}
		regs_.r = calcCycles(regs_.r, x);
	}

	static void cmpVFregs(const MvuVFReg& VFreg1, const MvuVFReg& VFreg2, bool& xVar)
	{
		if (VFreg1.reg == VFreg2.reg)
		{
			if ((VFreg1.x && VFreg2.x) || (VFreg1.y && VFreg2.y) || (VFreg1.z && VFreg2.z) || (VFreg1.w && VFreg2.w))
				xVar = 1;
		}
	}

	void PipeAnalyzer::setCycles()
	{
		incCycles(info().stall);
		MvuOp& in = info();

		if ((regsTemp_.VFreg[0] == regsTemp_.VFreg[1]) && regsTemp_.VFreg[0])
		{
			if (regsTemp_.r || regsTemp_.VI)
				in.lOp.noWriteVF = true;
			else
				in.lOp.isNOP = true;
		}

		if ((in.lOp.VF_read[0].reg || in.lOp.VF_read[1].reg) && in.uOp.VF_write.reg)
		{
			cmpVFregs(in.uOp.VF_write, in.lOp.VF_read[0], in.swapOps);
			cmpVFregs(in.uOp.VF_write, in.lOp.VF_read[1], in.swapOps);
		}

		if (in.swapOps && ((in.uOp.VF_read[0].reg || in.uOp.VF_read[1].reg) && in.lOp.VF_write.reg))
		{
			cmpVFregs(in.lOp.VF_write, in.uOp.VF_read[0], in.backupVF);
			cmpVFregs(in.lOp.VF_write, in.uOp.VF_read[1], in.backupVF);
		}

		for (int k = 0; k < 2; k++)
		{
			RegCycleInfo& d = regs_.VF[regsTemp_.VFreg[k]];
			d.x = tCycles(d.x, regsTemp_.VF[k].x);
			d.y = tCycles(d.y, regsTemp_.VF[k].y);
			d.z = tCycles(d.z, regsTemp_.VF[k].z);
			d.w = tCycles(d.w, regsTemp_.VF[k].w);
		}
		regs_.VI[regsTemp_.VIreg] = tCycles(regs_.VI[regsTemp_.VIreg], regsTemp_.VI);
		regs_.q = tCycles(regs_.q, regsTemp_.q);
		regs_.p = tCycles(regs_.p, regsTemp_.p);
		regs_.r = tCycles(regs_.r, regsTemp_.r);
		regs_.xgkick = tCycles(regs_.xgkick, regsTemp_.xgkick);
		std::memset(&regsTemp_, 0, sizeof(regsTemp_));
	}

	//------------------------------------------------------------------
	// x86/microVU_Analyze.inl
	//------------------------------------------------------------------

	void PipeAnalyzer::analyzeReg1(int xReg, MvuVFReg& vfRead)
	{
		u8& stall = info().stall;
		if (xReg)
		{
			if (X()) { stall = std::max<u8>(stall, regs_.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
			if (Y()) { stall = std::max<u8>(stall, regs_.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
			if (Z()) { stall = std::max<u8>(stall, regs_.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
			if (W()) { stall = std::max<u8>(stall, regs_.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
		}
	}

	void PipeAnalyzer::analyzeReg2(int xReg, MvuVFReg& vfWrite, bool isLowOp)
	{
		if (xReg)
		{
			const int y = isLowOp ? 1 : 0;
			if (X()) { regsTemp_.VFreg[y] = xReg; regsTemp_.VF[y].x = 4; vfWrite.reg = xReg; vfWrite.x = 4; }
			if (Y()) { regsTemp_.VFreg[y] = xReg; regsTemp_.VF[y].y = 4; vfWrite.reg = xReg; vfWrite.y = 4; }
			if (Z()) { regsTemp_.VFreg[y] = xReg; regsTemp_.VF[y].z = 4; vfWrite.reg = xReg; vfWrite.z = 4; }
			if (W()) { regsTemp_.VFreg[y] = xReg; regsTemp_.VF[y].w = 4; vfWrite.reg = xReg; vfWrite.w = 4; }
		}
	}

	void PipeAnalyzer::analyzeReg3(int xReg, MvuVFReg& vfRead)
	{
		u8& stall = info().stall;
		if (xReg)
		{
			const u32 bc = code_ & 3;
			if (bc == 0) { stall = std::max<u8>(stall, regs_.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
			else if (bc == 1) { stall = std::max<u8>(stall, regs_.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
			else if (bc == 2) { stall = std::max<u8>(stall, regs_.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
			else { stall = std::max<u8>(stall, regs_.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
		}
	}

	void PipeAnalyzer::analyzeReg4(int xReg, MvuVFReg& vfRead)
	{
		u8& stall = info().stall;
		if (xReg)
		{
			stall = std::max<u8>(stall, regs_.VF[xReg].w);
			vfRead.reg = xReg;
			vfRead.w = 1;
		}
	}

	void PipeAnalyzer::analyzeReg5(int xReg, int fxf, MvuVFReg& vfRead)
	{
		u8& stall = info().stall;
		if (xReg)
		{
			switch (fxf)
			{
				case 0: stall = std::max<u8>(stall, regs_.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; break;
				case 1: stall = std::max<u8>(stall, regs_.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; break;
				case 2: stall = std::max<u8>(stall, regs_.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; break;
				case 3: stall = std::max<u8>(stall, regs_.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; break;
			}
		}
	}

	void PipeAnalyzer::analyzeReg6(int xReg, MvuVFReg& vfRead)
	{
		u8& stall = info().stall;
		if (xReg)
		{
			if (X()) { stall = std::max<u8>(stall, regs_.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
			if (Y()) { stall = std::max<u8>(stall, regs_.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
			if (Z()) { stall = std::max<u8>(stall, regs_.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
			if (W()) { stall = std::max<u8>(stall, regs_.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
		}
	}

	void PipeAnalyzer::analyzeVIreg1(int xReg, MvuVIReg& viRead)
	{
		if (xReg)
		{
			info().stall = std::max<u8>(info().stall, regs_.VI[xReg]);
			viRead.reg = xReg;
			viRead.used = 1;
		}
	}

	void PipeAnalyzer::analyzeVIreg2(int xReg, MvuVIReg& viWrite, int aCycles)
	{
		if (xReg)
		{
			constReg_[xReg].isValid = 0;
			regsTemp_.VIreg = xReg;
			regsTemp_.VI = aCycles;
			viWrite.reg = xReg;
			viWrite.used = aCycles;
		}
	}

	void PipeAnalyzer::analyzeQreg(u8 x)
	{
		regsTemp_.q = x;
		info().stall = std::max<u8>(info().stall, regs_.q);
	}

	void PipeAnalyzer::analyzePreg(u8 x)
	{
		regsTemp_.p = x;
		info().stall = std::max<u8>(info().stall, static_cast<u8>(regs_.p ? (regs_.p - 1) : 0));
	}

	void PipeAnalyzer::setConstReg(int x, u32 v)
	{
		if (x)
		{
			constReg_[x].isValid = 1;
			constReg_[x].regValue = v;
		}
	}

	void PipeAnalyzer::analyzeFMAC1(int Fd, int Fs, int Ft)
	{
		info().sFlag.doFlag = 1;
		analyzeReg1(Fs, info().uOp.VF_read[0]);
		analyzeReg1(Ft, info().uOp.VF_read[1]);
		analyzeReg2(Fd, info().uOp.VF_write, 0);
	}

	void PipeAnalyzer::analyzeFMAC2(int Fs, int Ft)
	{
		analyzeReg1(Fs, info().uOp.VF_read[0]);
		analyzeReg2(Ft, info().uOp.VF_write, 0);
	}

	void PipeAnalyzer::analyzeFMAC3(int Fd, int Fs, int Ft)
	{
		info().sFlag.doFlag = 1;
		analyzeReg1(Fs, info().uOp.VF_read[0]);
		analyzeReg3(Ft, info().uOp.VF_read[1]);
		analyzeReg2(Fd, info().uOp.VF_write, 0);
	}

	void PipeAnalyzer::analyzeFMAC4(int Fs, int Ft)
	{
		info().cFlag.doFlag = 1;
		analyzeReg1(Fs, info().uOp.VF_read[0]);
		analyzeReg4(Ft, info().uOp.VF_read[1]);
	}

	void PipeAnalyzer::analyzeIALU1(int Id, int Is, int It)
	{
		if (!Id)
			info().lOp.isNOP = 1;
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		analyzeVIreg1(It, info().lOp.VI_read[1]);
		analyzeVIreg2(Id, info().lOp.VI_write, 1);
	}

	void PipeAnalyzer::analyzeIALU2(int Is, int It)
	{
		if (!It)
			info().lOp.isNOP = 1;
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		analyzeVIreg2(It, info().lOp.VI_write, 1);
	}

	void PipeAnalyzer::analyzeIADDI(int Is, int It, s16 imm)
	{
		analyzeIALU2(Is, It);
		if (!Is && !cfg_.ibitHack)
			setConstReg(It, static_cast<u32>(static_cast<s32>(imm)));
	}

	void PipeAnalyzer::analyzeMR32(int Fs, int Ft)
	{
		if (!Ft)
			info().lOp.isNOP = 1;
		analyzeReg6(Fs, info().lOp.VF_read[0]);
		analyzeReg2(Ft, info().lOp.VF_write, 1);
	}

	void PipeAnalyzer::analyzeFDIV(int Fs, int Fsf, int Ft, int Ftf, u8 xCycles)
	{
		analyzeReg5(Fs, Fsf, info().lOp.VF_read[0]);
		analyzeReg5(Ft, Ftf, info().lOp.VF_read[1]);
		analyzeQreg(xCycles);
	}

	void PipeAnalyzer::analyzeEFU1(int Fs, int Fsf, u8 xCycles)
	{
		analyzeReg5(Fs, Fsf, info().lOp.VF_read[0]);
		analyzePreg(xCycles);
	}

	void PipeAnalyzer::analyzeEFU2(int Fs, u8 xCycles)
	{
		analyzeReg1(Fs, info().lOp.VF_read[0]);
		analyzePreg(xCycles);
	}

	void PipeAnalyzer::analyzeMFP(int Ft)
	{
		if (!Ft)
			info().lOp.isNOP = 1;
		analyzeReg2(Ft, info().lOp.VF_write, 1);
	}

	void PipeAnalyzer::analyzeMOVE(int Fs, int Ft)
	{
		if (!Ft || (Ft == Fs))
			info().lOp.isNOP = 1;
		analyzeReg1(Fs, info().lOp.VF_read[0]);
		analyzeReg2(Ft, info().lOp.VF_write, 1);
	}

	void PipeAnalyzer::analyzeLQ(int Ft, int Is, bool writeIs)
	{
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		analyzeReg2(Ft, info().lOp.VF_write, 1);
		if (!Ft)
		{
			if (writeIs && Is)
				info().lOp.noWriteVF = 1;
			else
				info().lOp.isNOP = 1;
		}
		if (writeIs)
			analyzeVIreg2(Is, info().lOp.VI_write, 1);
	}

	void PipeAnalyzer::analyzeSQ(int Fs, int It, bool writeIt)
	{
		info().lOp.isMemWrite = true;
		analyzeReg1(Fs, info().lOp.VF_read[0]);
		analyzeVIreg1(It, info().lOp.VI_read[0]);
		if (writeIt)
			analyzeVIreg2(It, info().lOp.VI_write, 1);
	}

	void PipeAnalyzer::analyzeR1(int Fs, int Fsf)
	{
		analyzeReg5(Fs, Fsf, info().lOp.VF_read[0]);
		regsTemp_.r = 1;
	}

	void PipeAnalyzer::analyzeR2(int Ft, bool canBeNOP)
	{
		if (!Ft)
		{
			if (canBeNOP)
				info().lOp.isNOP = 1;
			else
				info().lOp.noWriteVF = 1;
		}
		analyzeReg2(Ft, info().lOp.VF_write, 1);
		regsTemp_.r = 1;
	}

	void PipeAnalyzer::flagSet(bool setMacFlag)
	{
		const u32 curPC = iPC_;
		int calcOPS = 0;
		for (int i = count_, j = 0; i > 0; i--, j++)
		{
			j += info().stall;
			incPC(-2);
			if (calcOPS >= 4 && info().uOp.VF_write.reg)
				break;
			if (info().sFlag.doFlag && (j >= 3))
			{
				if (setMacFlag)
					info().mFlag.doFlag = 1;
				info().sFlag.doNonSticky = 1;
				calcOPS++;
			}
		}
		iPC_ = curPC;
		setCode();
	}

	void PipeAnalyzer::analyzeSflag(int It)
	{
		info().lOp.readFlags = true;
		analyzeVIreg2(It, info().lOp.VI_write, 1);
		if (!It)
			info().lOp.isNOP = 1;
		else
		{
			info().swapOps = 1;
			flagSet(false);
		}
	}

	void PipeAnalyzer::analyzeFSSET()
	{
		info().lOp.isFSSET = 1;
		info().lOp.readFlags = true;
	}

	void PipeAnalyzer::analyzeMflag(int Is, int It)
	{
		info().lOp.readFlags = true;
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		analyzeVIreg2(It, info().lOp.VI_write, 1);
		if (!It)
			info().lOp.isNOP = 1;
		else
		{
			info().swapOps = 1;
			flagSet(true);
		}
	}

	void PipeAnalyzer::analyzeCflag(int It)
	{
		info().swapOps = 1;
		info().lOp.readFlags = true;
		analyzeVIreg2(It, info().lOp.VI_write, 1);
	}

	void PipeAnalyzer::analyzeXGkick(int Fs, int xCycles)
	{
		info().lOp.isKick = true;
		regs_.xgkickcycles = 0;
		info().lOp.kickcycles = 0;
		analyzeVIreg1(Fs, info().lOp.VI_read[0]);
		if (!cfg_.xgkickHack)
		{
			info().stall = std::max<u8>(info().stall, regs_.xgkick);
			regsTemp_.xgkick = xCycles;
		}
	}

	void PipeAnalyzer::analyzeBranchVI(int xReg, bool& infoVar)
	{
		if (!xReg)
			return;
		if (info().stall)
			return;
		int i, j = 0;
		int cyc = 0;
		const int iEnd = 4;
		const u32 bPC = iPC_;
		incPC2(-2);
		for (i = 0; i < iEnd && cyc < iEnd; i++)
		{
			if (i == static_cast<int>(count_))
			{
				if (pState_.viBackUp == xReg)
				{
					infoVar = true;
					j = i;
					i++;
				}
				break;
			}
			if ((info().lOp.VI_write.reg == xReg) && info().lOp.VI_write.used)
			{
				if (info().lOp.readFlags)
					break;
				j = i;
			}
			else if (i == 0)
			{
				break;
			}
			cyc += info().stall + 1;
			incPC2(-2);
		}

		if (i)
		{
			if (!infoVar)
			{
				iPC_ = bPC;
				incPC2(-2 * (j + 1));
				info().lOp.backupVI = true;
				infoVar = true;
			}
			iPC_ = bPC;
		}
		else
		{
			iPC_ = bPC;
		}
	}

	int PipeAnalyzer::branchCheck()
	{
		if (!count_ && !isEvilBlock())
			return 0;

		if (isEvilBlock())
		{
			info().lOp.evilBranch = true;
			regs_.blockType = 2;
			regs_.needExactMatch |= 7;
			regs_.flagInfo = 0;
			return 1;
		}

		incPC(-2);
		if (info().lOp.branch)
		{
			// doBranchInDelaySlot is on in microVU.
			info().lOp.badBranch = true;
			incPC(2);
			info().lOp.evilBranch = true;
			regs_.blockType = 2;
			regs_.needExactMatch |= 7;
			regs_.flagInfo = 0;
			return 1;
		}
		incPC(2);
		return 0;
	}

	void PipeAnalyzer::analyzeCondBranch1(int Is)
	{
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		if (!branchCheck())
			analyzeBranchVI(Is, info().lOp.memReadIs);
	}

	void PipeAnalyzer::analyzeCondBranch2(int Is, int It)
	{
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		analyzeVIreg1(It, info().lOp.VI_read[1]);
		if (!branchCheck())
		{
			analyzeBranchVI(Is, info().lOp.memReadIs);
			analyzeBranchVI(It, info().lOp.memReadIt);
		}
	}

	void PipeAnalyzer::analyzeNormBranch(int It, bool isBAL)
	{
		branchCheck();
		if (isBAL)
		{
			analyzeVIreg2(It, info().lOp.VI_write, 1);
			if (!info().lOp.evilBranch)
				setConstReg(It, bSaveAddr());
		}
	}

	void PipeAnalyzer::analyzeJump(int Is, int It, bool isJALR)
	{
		info().lOp.branch = isJALR ? 10 : 9;
		branchCheck();
		if (constReg_[Is].isValid && doConstProp)
		{
			info().lOp.constJump.isValid = 1;
			info().lOp.constJump.regValue = constReg_[Is].regValue;
		}
		analyzeVIreg1(Is, info().lOp.VI_read[0]);
		if (isJALR)
		{
			analyzeVIreg2(It, info().lOp.VI_write, 1);
			if (!info().lOp.evilBranch)
				setConstReg(It, bSaveAddr());
		}
	}

	// setBranchA (microVU_Lower-arm64.inl): runs in every pass.
	void PipeAnalyzer::setBranchA(int pass, int x, int x_)
	{
		bool isBranchDelaySlot = false;
		incPC(-2);
		if (info().lOp.branch)
			isBranchDelaySlot = true;
		incPC(2);

		const bool optimized = (Imm11() == 1 && !x_ && !isBranchDelaySlot);
		if (pass == 0)
		{
			if (optimized)
			{
				info().lOp.isNOP = true;
				return;
			}
			branch_ = x;
			info().lOp.branch = x;
		}
		else if (pass == 3)
		{
			if (optimized)
				return;
			branch_ = x;
		}
	}

	//------------------------------------------------------------------
	// Op dispatch: the pass1 / pass4 bodies of each mVUop
	//------------------------------------------------------------------

	void PipeAnalyzer::opU(int pass)
	{
		if (pass == 3)
		{
			if (DecodeUpperRef(code_).foundPass4)
				regs_.needExactMatch |= 8;
			return;
		}
		const UpperInst u = DecodeUpper(code_);
		// pass 0
		const int opCase = static_cast<int>(u.form);
		auto setupPass1 = [&](bool isACC, bool noFlagUpdate) {
			const int fd = isACC ? 0 : static_cast<int>(Fd());
			switch (opCase)
			{
				case 1: analyzeFMAC1(fd, Fs(), Ft()); break;
				case 2: analyzeFMAC3(fd, Fs(), Ft()); break;
				case 3: analyzeFMAC1(fd, Fs(), 0); break;
				case 4: analyzeFMAC1(fd, Fs(), 0); break;
				default: break;
			}
			if (noFlagUpdate)
				info().sFlag.doFlag = false;
		};
		switch (u.cls)
		{
			case UClass::FmacA:
				setupPass1(u.isAcc, u.kind == FmacKind::Max || u.kind == FmacKind::Mini);
				break;
			case UClass::FmacB:
				setupPass1(true, false);
				break;
			case UClass::FmacC:
			case UClass::FmacD:
				setupPass1(false, false);
				break;
			case UClass::Fmac2:
				analyzeFMAC2(Fs(), Ft());
				break;
			case UClass::Opmula:
				analyzeFMAC1(0, Fs(), Ft());
				break;
			case UClass::Opmsub:
				analyzeFMAC1(Fd(), Fs(), Ft());
				break;
			case UClass::Clip:
				analyzeFMAC4(Fs(), Ft());
				break;
			case UClass::None:
				break;
			case UClass::Bad:
				if (code_ != 0x8000033c)
					info().isBadOp = true;
				break;
		}
	}

	void PipeAnalyzer::opL(int pass)
	{
		if (pass == 3)
		{
			switch (DecodeLowerRef(code_).op)
			{
				case LOp::FCAND: case LOp::FCEQ: case LOp::FCGET: case LOp::FCOR: regs_.needExactMatch |= 4; break;
				case LOp::FMAND: case LOp::FMEQ: case LOp::FMOR: regs_.needExactMatch |= 2; break;
				case LOp::FSAND: case LOp::FSOR: case LOp::FSEQ: regs_.needExactMatch |= 1; break;
				case LOp::B: setBranchA(3, 1, 0); break;
				case LOp::BAL: setBranchA(3, 2, It()); break;
				case LOp::IBEQ: setBranchA(3, 3, 0); break;
				case LOp::IBGEZ: setBranchA(3, 4, 0); break;
				case LOp::IBGTZ: setBranchA(3, 5, 0); break;
				case LOp::IBLEZ: setBranchA(3, 6, 0); break;
				case LOp::IBLTZ: setBranchA(3, 7, 0); break;
				case LOp::IBNE: setBranchA(3, 8, 0); break;
				case LOp::JR: branch_ = 9; break;
				case LOp::JALR: branch_ = 10; break;
				default: break;
			}
			return;
		}

		const LowerInst l = DecodeLower(code_);
		const bool vu0 = !isVU1();
		MvuLower& lo = info().lOp;
		switch (l.op)
		{
			case LOp::DIV: analyzeFDIV(Fs(), Fsf(), Ft(), Ftf(), 7); break;
			case LOp::SQRT: analyzeFDIV(0, 0, Ft(), Ftf(), 7); break;
			case LOp::RSQRT: analyzeFDIV(Fs(), Fsf(), Ft(), Ftf(), 13); break;

			case LOp::EATAN: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 54); break;
			case LOp::EATANxy: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 54); break;
			case LOp::EATANxz: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 54); break;
			case LOp::EEXP: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 44); break;
			case LOp::ELENG: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 18); break;
			case LOp::ERCPR: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 12); break;
			case LOp::ERLENG: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 24); break;
			case LOp::ERSADD: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 18); break;
			case LOp::ERSQRT: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 18); break;
			case LOp::ESADD: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 11); break;
			case LOp::ESIN: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 29); break;
			case LOp::ESQRT: if (vu0) { lo.isNOP = true; break; } analyzeEFU1(Fs(), Fsf(), 12); break;
			case LOp::ESUM: if (vu0) { lo.isNOP = true; break; } analyzeEFU2(Fs(), 12); break;

			case LOp::FCAND: case LOp::FCEQ: case LOp::FCOR: analyzeCflag(1); break;
			case LOp::FCGET: analyzeCflag(It()); break;
			case LOp::FCSET: info().cFlag.doFlag = true; break;
			case LOp::FMAND: case LOp::FMEQ: case LOp::FMOR: analyzeMflag(Is(), It()); break;
			case LOp::FSAND: case LOp::FSOR: case LOp::FSEQ: analyzeSflag(It()); break;
			case LOp::FSSET: analyzeFSSET(); break;

			case LOp::IADD: case LOp::IAND: case LOp::IOR: case LOp::ISUB: analyzeIALU1(Id(), Is(), It()); break;
			case LOp::IADDI: analyzeIADDI(Is(), It(), Imm5()); break;
			case LOp::IADDIU: analyzeIADDI(Is(), It(), static_cast<s16>(Imm15())); break;
			case LOp::ISUBIU: analyzeIALU2(Is(), It()); break;

			case LOp::MFIR:
				if (!Ft())
					lo.isNOP = true;
				analyzeVIreg1(Is(), lo.VI_read[0]);
				analyzeReg2(Ft(), lo.VF_write, 1);
				break;
			case LOp::MFP: if (vu0) { lo.isNOP = true; break; } analyzeMFP(Ft()); break;
			case LOp::MOVE: analyzeMOVE(Fs(), Ft()); break;
			case LOp::MR32: analyzeMR32(Fs(), Ft()); break;
			case LOp::MTIR:
				if (!It())
					lo.isNOP = true;
				analyzeReg5(Fs(), Fsf(), lo.VF_read[0]);
				analyzeVIreg2(It(), lo.VI_write, 1);
				break;

			case LOp::ILW: case LOp::ILWR:
				if (!It())
					lo.isNOP = true;
				analyzeVIreg1(Is(), lo.VI_read[0]);
				analyzeVIreg2(It(), lo.VI_write, 4);
				break;
			case LOp::ISW: case LOp::ISWR:
				lo.isMemWrite = true;
				analyzeVIreg1(Is(), lo.VI_read[0]);
				analyzeVIreg1(It(), lo.VI_read[1]);
				break;

			case LOp::LQ: analyzeLQ(Ft(), Is(), false); break;
			case LOp::LQD: case LOp::LQI: analyzeLQ(Ft(), Is(), true); break;
			case LOp::SQ: analyzeSQ(Fs(), It(), false); break;
			case LOp::SQD: case LOp::SQI: analyzeSQ(Fs(), It(), true); break;

			case LOp::RINIT: case LOp::RXOR: analyzeR1(Fs(), Fsf()); break;
			case LOp::RGET: analyzeR2(Ft(), true); break;
			case LOp::RNEXT: analyzeR2(Ft(), false); break;

			case LOp::WAITP:
				if (vu0) { lo.isNOP = true; break; }
				info().stall = std::max<u8>(info().stall, static_cast<u8>(regs_.p ? (regs_.p - 1) : 0));
				break;
			case LOp::WAITQ:
				info().stall = std::max<u8>(info().stall, regs_.q);
				break;

			case LOp::XTOP:
				if (vu0) { lo.isNOP = true; break; }
				if (!It())
					lo.isNOP = true;
				analyzeVIreg2(It(), lo.VI_write, 1);
				break;
			case LOp::XITOP:
				if (!It())
					lo.isNOP = true;
				analyzeVIreg2(It(), lo.VI_write, 1);
				break;
			case LOp::XGKICK:
				if (vu0) { lo.isNOP = true; break; }
				analyzeXGkick(Is(), 1);
				break;

			case LOp::B: setBranchA(0, 1, 0); analyzeNormBranch(0, false); break;
			case LOp::BAL: setBranchA(0, 2, It()); analyzeNormBranch(It(), true); break;
			case LOp::IBEQ: setBranchA(0, 3, 0); analyzeCondBranch2(Is(), It()); break;
			case LOp::IBGEZ: setBranchA(0, 4, 0); analyzeCondBranch1(Is()); break;
			case LOp::IBGTZ: setBranchA(0, 5, 0); analyzeCondBranch1(Is()); break;
			case LOp::IBLEZ: setBranchA(0, 6, 0); analyzeCondBranch1(Is()); break;
			case LOp::IBLTZ: setBranchA(0, 7, 0); analyzeCondBranch1(Is()); break;
			case LOp::IBNE: setBranchA(0, 8, 0); analyzeCondBranch2(Is(), It()); break;
			case LOp::JR: branch_ = 9; analyzeJump(Is(), 0, false); break;
			case LOp::JALR: branch_ = 10; analyzeJump(Is(), It(), true); break;

			case LOp::Unknown:
			default:
				if (code_ != 0x8000033c)
					info().isBadOp = true;
				break;
		}
	}

	//------------------------------------------------------------------
	// Flag lookahead (microVU_Compile-arm64.inl _mVUflagPass / mVUsetFlagInfo)
	//------------------------------------------------------------------

	void PipeAnalyzer::flagPass(u32 startPC, u32 sCount, u32 found, std::vector<u32>& v)
	{
		for (u32 x : v)
		{
			if (x == startPC)
				return;
		}
		v.push_back(startPC);

		const u32 oldPC = iPC_;
		const int oldBranch = branch_;
		u32 aBranchAddr = 0;
		iPC_ = startPC / 4;
		branch_ = 0;
		for (int branch = 0; sCount < 4; sCount += found)
		{
			regs_.needExactMatch &= 7;
			incPC(1);
			opU(3);
			found |= (regs_.needExactMatch & 8) >> 3;
			regs_.needExactMatch &= 7;
			if (curI() & kEbit)
				branch = 1;
			if (curI() & kTbit)
				branch = 6;
			if ((curI() & kDbit) && doDBitHandling)
				branch = 6;
			if (!(curI() & kIbit))
			{
				incPC(-1);
				opL(3);
				incPC(1);
			}

			if (branch >= 2)
			{
				if ((branch == 3) || (branch == 4))
				{
					flagPass(aBranchAddr, sCount + found, found, v);
					if (branch == 3)
						break;
					branch = 0;
				}
				else if (branch == 5)
				{
					if (sCount + found < 4)
						regs_.needExactMatch |= 7;
					break;
				}
				else
				{
					needFlagFinalize_ = true;
					break;
				}
			}
			else if (branch == 1)
			{
				branch = 2;
			}
			if (branch_)
			{
				branch = ((branch_ > 8) ? (5) : ((branch_ < 3) ? 3 : 4));
				incPC(-1);
				aBranchAddr = branchAddr();
				incPC(1);
				branch_ = 0;
			}
			incPC(1);
			if ((regs_.needExactMatch & 7) == 7)
				break;
		}
		iPC_ = oldPC;
		branch_ = oldBranch;
		regs_.needExactMatch &= 7;
		setCode();
	}

	void PipeAnalyzer::setFlagInfo()
	{
		if (branch_ <= 2) // B/BAL
		{
			incPC(-1);
			passed_.clear();
			flagPass(branchAddr(), 0, 0, passed_);
			incPC(1);
			regs_.needExactMatch &= 0x7;
		}
		else if (branch_ <= 8) // Conditional Branch
		{
			incPC(-1);
			passed_.clear();
			flagPass(branchAddr(), 0, 0, passed_);
			const int backupFlagInfo = regs_.needExactMatch;
			regs_.needExactMatch = 0;
			incPC(4);
			passed_.clear();
			flagPass(xPC(), 0, 0, passed_);
			incPC(-3);
			regs_.needExactMatch |= backupFlagInfo;
			regs_.needExactMatch &= 0x7;
		}
		else // JR/JALR: doConstProp is off, so every flag is live.
		{
			regs_.needExactMatch |= 0x7;
			regs_.needExactMatch &= 0x7;
		}
	}

	//------------------------------------------------------------------
	// microVU_Flags-arm64.inl
	//------------------------------------------------------------------

	void PipeAnalyzer::statusFlagOp()
	{
		const u32 curPC = iPC_;
		int i = count_;
		bool runLoop = true;
		if (info().sFlag.doFlag)
		{
			info().sFlag.doNonSticky = true;
		}
		else
		{
			for (; i > 0; i--)
			{
				incPC2(-2);
				if (info().sFlag.doNonSticky)
				{
					runLoop = false;
					break;
				}
				else if (info().sFlag.doFlag)
				{
					info().sFlag.doNonSticky = true;
					break;
				}
			}
		}
		if (runLoop)
		{
			for (; i > 0; i--)
			{
				incPC2(-2);
				if (info().sFlag.doNonSticky)
					break;
				info().sFlag.doFlag = false;
			}
		}
		iPC_ = curPC;
	}

	void PipeAnalyzer::setFlags(MvuFlagCycles& mFC, int& xSout, int& xMout, int& xCout)
	{
		const u32 endPC = iPC_;
		u32 aCount = 0;
		const bool status = regs_.needExactMatch & 1;
		const bool mac = regs_.needExactMatch & 2;
		const bool clip = regs_.needExactMatch & 4;

		const bool finalize = needFlagFinalize_;
		bool finalized = false;
		for (int i = count_; i > 0; i--, aCount++)
		{
			if (info().sFlag.doFlag)
			{
				if (mac || (finalize && !finalized))
					info().mFlag.doFlag = true;
				if (status || (finalize && !finalized))
					info().sFlag.doNonSticky = true;
				finalized = true;
				if (aCount >= 3)
					break;
			}
			incPC2(-2);
		}

		int xS = 0, xM = 0, xC = 0;
		for (int i = 0; i < 4; i++)
		{
			mFC.xStatus[i] = i;
			mFC.xMac[i] = i;
			mFC.xClip[i] = i;
		}

		if (!(pState_.needExactMatch & 1))
		{
			xS = (pState_.flagInfo >> 2) & 3;
			for (int i = 0; i < 4; i++)
				mFC.xStatus[i] = -1;
			mFC.xStatus[(xS - 1) & 3] = 0;
		}
		if (!(pState_.needExactMatch & 2))
		{
			for (int i = 0; i < 4; i++)
				mFC.xMac[i] = -1;
		}
		if (!(pState_.needExactMatch & 4))
		{
			xC = (pState_.flagInfo >> 6) & 3;
			for (int i = 0; i < 4; i++)
				mFC.xClip[i] = -1;
			mFC.xClip[(xC - 1) & 3] = 0;
		}

		mFC.cycles = 0;
		const u32 xCount = count_;
		iPC_ = startPC_;
		for (count_ = 0; count_ < xCount; count_++)
		{
			MvuOp& in = info();
			if (in.lOp.isFSSET && !noFlagOpts)
			{
				if (status)
				{
					if ((xCount - count_) > aCount)
						statusFlagOp();
				}
				else
					statusFlagOp();
			}
			mFC.cycles += in.stall;

			in.sFlag.read = FindFlagInst(mFC.xStatus, mFC.cycles);
			in.mFlag.read = FindFlagInst(mFC.xMac, mFC.cycles);
			in.cFlag.read = FindFlagInst(mFC.xClip, mFC.cycles);

			in.sFlag.write = xS;
			in.mFlag.write = xM;
			in.cFlag.write = xC;

			in.sFlag.lastWrite = (xS - 1) & 3;
			in.mFlag.lastWrite = (xM - 1) & 3;
			in.cFlag.lastWrite = (xC - 1) & 3;

			if (sFlagHack_ && !in.sFlag.doNonSticky)
				in.sFlag.doFlag = false;

			if (in.sFlag.doFlag || in.lOp.isFSSET || in.doDivFlag)
			{
				mFC.xStatus[xS] = mFC.cycles + 4;
				xS = (xS + 1) & 3;
			}
			if (in.mFlag.doFlag)
			{
				mFC.xMac[xM] = mFC.cycles + 4;
				xM = (xM + 1) & 3;
			}
			if (in.cFlag.doFlag)
			{
				mFC.xClip[xC] = mFC.cycles + 4;
				xC = (xC + 1) & 3;
			}

			mFC.cycles++;
			incPC2(2);
		}

		regs_.flagInfo |= (status ? 0 : (xS << 2));
		regs_.flagInfo |= (xM << 4);
		regs_.flagInfo |= (clip ? 0 : (xC << 6));
		iPC_ = endPC;
		xSout = xS;
		xMout = xM;
		xCout = xC;
	}

	//------------------------------------------------------------------
	// mVUcompile pass 1
	//------------------------------------------------------------------

	void PipeAnalyzer::eBitPass1(int& branch)
	{
		if (regs_.blockType != 1)
		{
			branch = 1;
			info().uOp.eBit = true;
		}
	}

	void PipeAnalyzer::branchWarning()
	{
		incPC(-2);
		if (info().uOp.eBit && branch_)
		{
			incPC(2);
			info().lOp.isNOP = true;
		}
		else
			incPC(2);

		MvuOp& in = info();
		if (in.isBdelay && !in.lOp.evilBranch)
		{
			if (in.lOp.VI_write.reg && in.lOp.VI_write.used && !in.lOp.readFlags)
			{
				in.lOp.backupVI = true;
				regs_.viBackUp = in.lOp.VI_write.reg;
			}
		}
	}

	void PipeAnalyzer::eBitWarning()
	{
		incPC(2);
		if (curI() & kEbit)
			regs_.blockType = 1;
		incPC(-2);
	}

	BlockResult PipeAnalyzer::Analyze(u32 startPC, const PipeState& entry)
	{
		BlockResult res;
		res.startPC = startPC;
		res.entry = entry;

		const u32 endCount = entry.blockType ? 1 : (cfg_.microMemSize() / 8);

		iPC_ = startPC / 4;
		setCode();
		std::memset(&regsTemp_, 0, sizeof(regsTemp_));

		// mVUinitFirstPass
		startPC_ = iPC_;
		branch_ = 0;
		count_ = 0;
		cycles_ = 0;
		p_ = 0;
		q_ = 0;
		regs_ = entry;
		pState_ = entry;
		regs_.needExactMatch = entry.blockType ? 7 : 0;
		needFlagFinalize_ = false;
		regs_.blockType = 0;
		regs_.viBackUp = 0;
		regs_.flagInfo = 0;
		sFlagHack_ = cfg_.flagHack;
		for (int i = 0; i < 16; i++)
		{
			constReg_[i].isValid = 0;
			constReg_[i].regValue = 0;
		}
		constReg_[15].isValid = regs_.vi15v;
		constReg_[15].regValue = regs_.vi15v ? regs_.vi15 : 0;
		branch_ = 0;

		for (int branch = 0; count_ < endCount;)
		{
			incPC(1);
			std::memset(&info(), 0, sizeof(MvuOp));
			std::memset(&regsTemp_, 0, sizeof(regsTemp_));
			incCycles(1);
			opU(0);
			if (info().isBadOp && code_ != 0x8000033c)
				info().isEOB = true;

			if (curI() & kEbit)
			{
				eBitPass1(branch);
				needFlagFinalize_ = true;
			}

			if ((curI() & kMbit) && !isVU1())
			{
				if (xPC() > 0)
				{
					incPC(-2);
					if (!(curI() & kMbit))
					{
						incPC(2);
						info().uOp.mBit = true;
					}
					else
						incPC(2);
				}
				else
					info().uOp.mBit = true;
			}

			if (curI() & kIbit)
			{
				info().lOp.isNOP = true;
				info().uOp.iBit = true;
			}
			else
			{
				incPC(-1);
				opL(0);
				incPC(1);
			}

			if (curI() & kDbit) info().uOp.dBit = true;
			if (curI() & kTbit) info().uOp.tBit = true;
			setCycles();

			MvuOp& in = info();
			if (!in.lOp.isKick)
			{
				regs_.xgkickcycles += 1 + in.stall;
				if (in.lOp.isMemWrite)
				{
					in.lOp.kickcycles = regs_.xgkickcycles;
					regs_.xgkickcycles = 0;
				}
			}
			else
			{
				regs_.xgkickcycles = 1;
				in.lOp.kickcycles = 0;
			}

			in.readQ = q_;
			in.writeQ = !q_;
			in.readP = p_ && isVU1();
			in.writeP = !p_ && isVU1();
			count_++;

			if (branch >= 2)
			{
				in.isEOB = true;
				if (branch == 3)
					in.isBdelay = true;
				branchWarning();
				if (regs_.xgkickcycles)
				{
					info().lOp.kickcycles = regs_.xgkickcycles;
					regs_.xgkickcycles = 0;
				}
				break;
			}
			else if (branch == 1)
			{
				branch = 2;
			}

			if (branch_)
			{
				setFlagInfo();
				eBitWarning();
				branch = 3;
				branch_ = 0;
			}

			if (info().uOp.mBit && !branch && !info().uOp.eBit)
			{
				regs_.needExactMatch |= 7;
				if (regs_.xgkickcycles)
				{
					info().lOp.kickcycles = regs_.xgkickcycles;
					regs_.xgkickcycles = 0;
				}
				break;
			}

			if (info().isEOB)
			{
				if (regs_.xgkickcycles)
				{
					info().lOp.kickcycles = regs_.xgkickcycles;
					regs_.xgkickcycles = 0;
				}
				break;
			}

			incPC(1);
		}

		regs_.vi15 = 0;
		regs_.vi15v = 0;
		setFlags(res.mFC, res.xS, res.xM, res.xC);
		optimizePipeState();
		res.qLane = q_;
		res.pLane = p_;

		// Collect the block in program order.
		res.count = count_;
		res.cycles = cycles_;
		res.needFlagFinalize = needFlagFinalize_;
		res.flagHack = sFlagHack_;
		res.end = regs_;
		res.ops.reserve(count_);
		res.pcs.reserve(count_);
		res.insts.reserve(count_);
		{
			u32 w = startPC_;
			for (u32 i = 0; i < count_; i++)
			{
				res.ops.push_back(info_[w / 2]);
				res.pcs.push_back((w / 2) * 8);
				res.insts.push_back(DecodePair(micro_, cfg_.microMemSize(), (w / 2) * 8));
				if (g_pipeReadMask)
					g_pipeReadMask[w & ~1u] = g_pipeReadMask[w | 1u] = 1;
				w = (w + 2) & cfg_.progMemMask();
			}
		}

		// Classify the end the way mVUcompile's second pass does.
		const MvuOp& last = res.ops.back();
		const u32 memMask = cfg_.microMemSize() - 8;
		if (pState_.blockType == 2)
		{
			res.endKind = BlockEnd::EvilJump;
		}
		else if (last.isBdelay && res.count >= 2)
		{
			const u32 bi = res.count - 2;
			const MvuOp& br = res.ops[bi];
			const InstPair& bp = res.insts[bi];
			res.branchIndex = bi;
			res.branchKind = br.lOp.branch;
			res.takenPC = (bp.pc + 8 + bp.lower.imm11 * 8) & memMask;
			res.fallPC = (bp.pc + 16) & memMask;
			res.eBitOnBranch = br.uOp.eBit;
			res.tBitOnBranch = br.uOp.tBit;
			switch (br.lOp.branch)
			{
				case 1: case 2: res.endKind = br.uOp.eBit ? BlockEnd::EBit : BlockEnd::Branch; break;
				case 9: case 10: res.endKind = br.uOp.eBit ? BlockEnd::EBit : BlockEnd::Jump; break;
				default: res.endKind = br.uOp.eBit ? BlockEnd::EBit : BlockEnd::CondBranch; break;
			}
		}
		else if (last.uOp.mBit && !last.uOp.eBit)
		{
			res.endKind = BlockEnd::MBit;
		}
		else
		{
			res.endKind = BlockEnd::EBit;
			res.endTPC = (res.pcs.back() + 8) & memMask;
		}
		for (u32 i = 0; i < res.count; i++)
		{
			const MvuOp& o = res.ops[i];
			if (!o.isBdelay && !o.lOp.branch && o.uOp.tBit)
				res.tBitExits.push_back(i);
		}
		return res;
	}

	//------------------------------------------------------------------
	// Trace conversion + comparison
	//------------------------------------------------------------------

	static MvuVFRegTrace VFT(const MvuVFReg& r)
	{
		MvuVFRegTrace t;
		t.reg = r.reg;
		t.x = r.x;
		t.y = r.y;
		t.z = r.z;
		t.w = r.w;
		return t;
	}

	void BlockResult::ToTrace(MvuBlockTrace& out, u32 vuIndex) const
	{
		out.vuIndex = vuIndex;
		out.startPC = startPC;
		std::memcpy(out.entryState, entry.bytes, 96);
		std::memcpy(out.endState, end.bytes, 96);
		out.count = count;
		out.cycles = cycles;
		out.flagHack = flagHack;
		out.needFlagFinalize = needFlagFinalize;
		out.ops.clear();
		out.ops.reserve(ops.size());
		for (size_t i = 0; i < ops.size(); i++)
		{
			const MvuOp& o = ops[i];
			MvuOpTrace t;
			std::memset(&t, 0, sizeof(t)); // padding too: callers hash the bytes
			t.pc = pcs[i];
			t.stall = o.stall;
			t.isBadOp = o.isBadOp;
			t.isEOB = o.isEOB;
			t.isBdelay = o.isBdelay;
			t.swapOps = o.swapOps;
			t.backupVF = o.backupVF;
			t.doXGKICK = o.doXGKICK;
			t.doDivFlag = o.doDivFlag;
			t.XGKICKPC = o.XGKICKPC;
			t.readQ = o.readQ;
			t.writeQ = o.writeQ;
			t.readP = o.readP;
			t.writeP = o.writeP;
			t.sFlag = {o.sFlag.doFlag, o.sFlag.doNonSticky, o.sFlag.write, o.sFlag.lastWrite, o.sFlag.read};
			t.mFlag = {o.mFlag.doFlag, o.mFlag.doNonSticky, o.mFlag.write, o.mFlag.lastWrite, o.mFlag.read};
			t.cFlag = {o.cFlag.doFlag, o.cFlag.doNonSticky, o.cFlag.write, o.cFlag.lastWrite, o.cFlag.read};
			t.eBit = o.uOp.eBit;
			t.iBit = o.uOp.iBit;
			t.mBit = o.uOp.mBit;
			t.tBit = o.uOp.tBit;
			t.dBit = o.uOp.dBit;
			t.uVFwrite = VFT(o.uOp.VF_write);
			t.uVFread0 = VFT(o.uOp.VF_read[0]);
			t.uVFread1 = VFT(o.uOp.VF_read[1]);
			t.lVFwrite = VFT(o.lOp.VF_write);
			t.lVFread0 = VFT(o.lOp.VF_read[0]);
			t.lVFread1 = VFT(o.lOp.VF_read[1]);
			t.VIwriteReg = o.lOp.VI_write.reg;
			t.VIwriteUsed = o.lOp.VI_write.used;
			t.VIread0Reg = o.lOp.VI_read[0].reg;
			t.VIread0Used = o.lOp.VI_read[0].used;
			t.VIread1Reg = o.lOp.VI_read[1].reg;
			t.VIread1Used = o.lOp.VI_read[1].used;
			t.constJumpValid = o.lOp.constJump.isValid;
			t.constJumpValue = o.lOp.constJump.regValue;
			t.branch = o.lOp.branch;
			t.kickcycles = o.lOp.kickcycles;
			t.badBranch = o.lOp.badBranch;
			t.evilBranch = o.lOp.evilBranch;
			t.isNOP = o.lOp.isNOP;
			t.isFSSET = o.lOp.isFSSET;
			t.noWriteVF = o.lOp.noWriteVF;
			t.backupVI = o.lOp.backupVI;
			t.memReadIs = o.lOp.memReadIs;
			t.memReadIt = o.lOp.memReadIt;
			t.readFlags = o.lOp.readFlags;
			t.isMemWrite = o.lOp.isMemWrite;
			t.isKick = o.lOp.isKick;
			out.ops.push_back(t);
		}
	}

	std::vector<std::string> CompareBlockTraces(const MvuBlockTrace& a, const MvuBlockTrace& b)
	{
		std::vector<std::string> out;
		char buf[192];
		auto field = [&](const char* name, long long va, long long vb, int op, u32 pc) {
			if (va == vb)
				return;
			if (op < 0)
				std::snprintf(buf, sizeof(buf), "block %s: mvu=%lld vuaot=%lld", name, va, vb);
			else
				std::snprintf(buf, sizeof(buf), "op %d pc 0x%04x %s: mvu=%lld vuaot=%lld", op, pc, name, va, vb);
			out.emplace_back(buf);
		};
		field("startPC", a.startPC, b.startPC, -1, 0);
		field("count", a.count, b.count, -1, 0);
		field("cycles", a.cycles, b.cycles, -1, 0);
		field("needFlagFinalize", a.needFlagFinalize, b.needFlagFinalize, -1, 0);
		for (int i = 0; i < 96; i++)
		{
			if (a.endState[i] != b.endState[i])
			{
				std::snprintf(buf, sizeof(buf), "block endState[%d]: mvu=%u vuaot=%u", i, a.endState[i], b.endState[i]);
				out.emplace_back(buf);
			}
		}
		const size_t n = std::min(a.ops.size(), b.ops.size());
		for (size_t k = 0; k < n; k++)
		{
			const MvuOpTrace& x = a.ops[k];
			const MvuOpTrace& y = b.ops[k];
			const int i = static_cast<int>(k);
			const u32 pc = x.pc;
#define F(f) field(#f, static_cast<long long>(x.f), static_cast<long long>(y.f), i, pc)
#define FV(f) do { F(f.reg); F(f.x); F(f.y); F(f.z); F(f.w); } while (0)
#define FF(f) do { F(f.doFlag); F(f.doNonSticky); F(f.write); F(f.lastWrite); F(f.read); } while (0)
			F(pc); F(stall); F(isBadOp); F(isEOB); F(isBdelay); F(swapOps); F(backupVF);
			F(doXGKICK); F(doDivFlag); F(XGKICKPC); F(readQ); F(writeQ); F(readP); F(writeP);
			FF(sFlag); FF(mFlag); FF(cFlag);
			F(eBit); F(iBit); F(mBit); F(tBit); F(dBit);
			FV(uVFwrite); FV(uVFread0); FV(uVFread1); FV(lVFwrite); FV(lVFread0); FV(lVFread1);
			F(VIwriteReg); F(VIwriteUsed); F(VIread0Reg); F(VIread0Used); F(VIread1Reg); F(VIread1Used);
			F(constJumpValid); F(constJumpValue); F(branch); F(kickcycles);
			F(badBranch); F(evilBranch); F(isNOP); F(isFSSET); F(noWriteVF);
			F(backupVI); F(memReadIs); F(memReadIt); F(readFlags); F(isMemWrite); F(isKick);
#undef F
#undef FV
#undef FF
		}
		return out;
	}
} // namespace vuaot
