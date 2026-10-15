// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotTier.h"

#include "arm64/AsmHelpers.h"
#include "vixl/aarch64/disasm-aarch64.h"

#include <cstdio>
#include <cstring>

namespace vuaot
{
	bool BodyTier::Decline(const std::string& why, std::string* out)
	{
		declined_[why]++;
		if (out)
			*out = why;
		return false;
	}

	bool BodyTier::TryLower(const MvuBlockTrace& b, const MvuEmitEnv& env, std::string* why)
	{
		if (b.vuIndex != vu_)
			return false;
		blocks_++;
		if (!an_)
		{
			PipeConfig cfg;
			cfg.vuIndex = vu_;
			cfg.flagHack = env.flagHack;
			cfg.xgkickHack = env.xgkickHack;
			cfg.ibitHack = env.ibitHack;
			an_ = std::make_unique<PipeAnalyzer>(micro_, cfg);
		}
		// Every block goes through the port, declined or not, so its per-slot
		// history follows microVU's compile order.
		PipeState st;
		std::memcpy(st.bytes, b.entryState, 96);
		const BlockResult br = an_->Analyze(b.startPC, st);
		MvuBlockTrace ours;
		br.ToTrace(ours, vu_);
		if (!CompareBlockTraces(b, ours).empty())
			return Decline("timing port disagrees", why);
		if (env.vuIndex != 1)
			return Decline("VU0", why);
		if (env.clampExact)
			return Decline("clamp mode 4", why);
		if (env.xgkickHack)
			return Decline("XGKickHack", why);
		if (env.ibitHack)
			return Decline("IbitHack", why);
		if (env.overflowHack)
			return Decline("VUOverflowHack", why);
		if (env.addSubHack)
			return Decline("VuAddSubHack", why);
		const s32 index = static_cast<s32>(blocks_) - 1;
		if ((only >= 0 && index != only) || (limit >= 0 && index >= limit) || skip.count(index))
			return Decline("bisect", why);

		ir::BuildOptions bo;
		bo.vuIndex = vu_;
		bo.clampIImmediate = env.clampOverflow && !env.clampSign;
		bo.clampExtra = env.clampExtra;
		bo.clampSign = env.clampSign;
		const ir::Function fn = ir::BuildNodeLocal(br, bo);
		LowerEnv le;
		le.vuIndex = env.vuIndex;
		le.offDivFlag = env.offDivFlag;
		le.offVIbackup = env.offVIbackup;
		le.offVIxgkick = env.offVIxgkick;
		le.offBranch = env.offBranch;
		le.offNeonBackup = env.offNeonBackup;
		le.poolLimitV = poolLimitV;
		le.poolLimitG = poolLimitG;
		le.xgkickFn = env.xgkickFn;
		le.vifTop = env.vifTop;
		le.vifItop = env.vifItop;
		le.clampOverflow = env.clampOverflow;
		le.clampExtra = env.clampExtra;
		le.clampSign = env.clampSign;
		le.softwareFlush = env.softwareFlush;
		le.schedule = schedule;
		le.vuMemPinned = env.vuMemPinned;
		const u8* codeStart = armGetCurrentCodePointer();
		std::vector<std::pair<const u8*, ir::ValueId>> marks;
		if (dump)
			g_lowerMarks = &marks;
		const LowerResult r = LowerNodeBody(fn, le);
		g_lowerMarks = nullptr;
		if (!r.ok)
			return Decline(r.why, why);
		if (dump)
		{
			ProgramGraph g;
			g.nodes.emplace_back();
			g.nodes.back().pc = br.startPC;
			g.nodes.back().entry = br.entry;
			g.nodes.back().block = br;
			std::fputs(ir::Dump(fn, g).c_str(), stdout);
			std::fflush(stdout);
			vixl::aarch64::PrintDisassembler dis(stdout);
			marks.push_back({codeStart + r.bytes, 0});
			const u8* at = codeStart;
			for (size_t k = 0; k < marks.size(); k++)
			{
				if (marks[k].first > at)
					dis.DisassembleBuffer(reinterpret_cast<const vixl::aarch64::Instruction*>(at),
						reinterpret_cast<const vixl::aarch64::Instruction*>(marks[k].first));
				at = marks[k].first;
				if (marks[k].second)
				{
					std::fflush(stdout);
					std::printf("  ; ---- %%%u\n", marks[k].second);
				}
				std::fflush(stdout);
			}
			std::fflush(stdout);
		}
		lowered_++;
		bytes_ += r.bytes;
		return true;
	}
} // namespace vuaot
