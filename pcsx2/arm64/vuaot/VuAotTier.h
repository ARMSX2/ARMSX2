// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Milestone 2's test-only tier: called from microVU's block-body hook
// (VuAotMvuTrace.h), it lowers the block from vu-aot's IR if it can and
// otherwise leaves the block to microVU. One BodyTier per program run: it
// owns the first-pass port, which has to see every block in microVU's compile
// order (VuAotPipe.h).

#include "VuAotLower.h"
#include "VuAotMvuTrace.h"

#include <map>
#include <set>
#include <memory>
#include <string>

namespace vuaot
{
	class BodyTier
	{
	public:
		BodyTier(u32 vuIndex, const u8* micro)
			: vu_(vuIndex)
			, micro_(micro)
		{
		}

		// Returns true when it emitted the body. `why` names the reason when not.
		bool TryLower(const MvuBlockTrace& block, const MvuEmitEnv& env, std::string* why);

		// See RegionTier::ResetAnalyzer.
		void ResetAnalyzer() { an_.reset(); }

		u32 Blocks() const { return blocks_; }
		u32 Lowered() const { return lowered_; }
		u64 BodyBytes() const { return bytes_; }
		const std::map<std::string, u32>& Declined() const { return declined_; }

		// Lower only block `only` (compile order) or the first `limit` blocks:
		// bisecting a divergence. -1 = no restriction.
		s32 only = -1, limit = -1;
		std::set<s32> skip;
		// Testing: shrink the allocatable register pools (LowerEnv).
		u32 poolLimitV = 0, poolLimitG = 0;
		// Milestone 9: list-schedule the bodies (regions do by default).
		bool schedule = false;
		// Print each lowered block's IR to stdout.
		bool dump = false;

	private:
		u32 vu_;
		const u8* micro_;
		std::unique_ptr<PipeAnalyzer> an_;
		u32 blocks_ = 0, lowered_ = 0;
		u64 bytes_ = 0;
		std::map<std::string, u32> declined_;

		bool Decline(const std::string& why, std::string* out);
	};
} // namespace vuaot
