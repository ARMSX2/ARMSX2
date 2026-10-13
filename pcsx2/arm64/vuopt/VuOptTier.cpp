// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuOptTier.h"

#include <cstdio>
#include <cstring>

namespace vuopt
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
		if (!env.clampOverflow || env.clampExtra || env.clampSign || env.clampExact)
			return Decline("clamp mode other than 1", why);
		if (env.xgkickHack)
			return Decline("XGKickHack", why);
		if (env.ibitHack)
			return Decline("IbitHack", why);
		if (env.overflowHack)
			return Decline("VUOverflowHack", why);
		if (env.addSubHack)
			return Decline("VuAddSubHack", why);
		const s32 index = static_cast<s32>(blocks_) - 1;
		if ((only >= 0 && index != only) || (limit >= 0 && index >= limit))
			return Decline("bisect", why);

		ir::BuildOptions bo;
		bo.vuIndex = vu_;
		bo.clampIImmediate = env.clampOverflow && !env.clampSign;
		const ir::Function fn = ir::BuildNodeLocal(br, bo);
		LowerEnv le;
		le.vuIndex = env.vuIndex;
		le.offDivFlag = env.offDivFlag;
		le.offVIbackup = env.offVIbackup;
		le.offVIxgkick = env.offVIxgkick;
		le.offBranch = env.offBranch;
		le.xgkickFn = env.xgkickFn;
		le.vifTop = env.vifTop;
		le.vifItop = env.vifItop;
		le.clampOverflow = env.clampOverflow;
		le.softwareFlush = env.softwareFlush;
		const LowerResult r = LowerNodeBody(fn, le);
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
		}
		lowered_++;
		bytes_ += r.bytes;
		return true;
	}
} // namespace vuopt
