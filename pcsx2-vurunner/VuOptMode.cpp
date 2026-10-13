// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// pcsx2-vurunner --vuopt-ir
//
// For each capture: replay it through microVU (JIT only) with the first-pass
// hook armed, collecting every block microVU compiles together with its
// decisions. Then run vuopt's port of that pass on the same program image,
// block by block in microVU's compile order and from the same entry state,
// and compare every decision field. Output is one summary line per capture
// plus the mismatching fields; a final line aggregates the run.

#include "VuOptMode.h"

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuReplay.h"

#include "Config.h"
#include "arm64/vuopt/VuOptMvuTrace.h"
#include "arm64/vuopt/VuOptPipe.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct HookSink
	{
		u32 vuIndex = 1;
		std::vector<vuopt::MvuBlockTrace> blocks;
	};

	void OnBlock(const vuopt::MvuBlockTrace& b, void* user)
	{
		HookSink* s = static_cast<HookSink*>(user);
		if (b.vuIndex == s->vuIndex)
			s->blocks.push_back(b);
	}

	// Field name of a mismatch line, for the per-class histogram.
	std::string MismatchClass(const std::string& line)
	{
		// "op N pc 0xPPPP field: ..." or "block field: ..."
		size_t colon = line.find(':');
		size_t start = 0;
		if (line.rfind("op ", 0) == 0)
		{
			size_t pc = line.find(" pc 0x");
			start = line.find(' ', pc + 6) + 1;
		}
		else if (line.rfind("block ", 0) == 0)
		{
			start = 6;
		}
		std::string f = line.substr(start, colon - start);
		const size_t br = f.find('[');
		if (br != std::string::npos)
			f = f.substr(0, br);
		return f;
	}
} // namespace

int RunVuoptIr(const std::vector<vu_capture::CaptureRecord>& records,
	const std::vector<std::string>& names,
	const VuoptModeOptions& opts,
	void (*applyConfig)(const vu_capture::CaptureRecord&))
{
	u64 totBlocks = 0, totMatched = 0, totOps = 0;
	std::map<std::string, u64> totClasses;
	int failures = 0;

	for (size_t fi = 0; fi < records.size(); ++fi)
	{
		const auto& rec = records[fi];
		applyConfig(rec);
		EmuConfig.Speedhacks.vuFlagHack = opts.flagHack != 0;

		HookSink sink;
		sink.vuIndex = rec.vu_index;
		vuopt::g_mvuBlockHook = &OnBlock;
		vuopt::g_mvuBlockHookUser = &sink;
		const auto r = recompiler_tests::ReplayJitOnly(rec, opts.cycleOverride);
		vuopt::g_mvuBlockHook = nullptr;
		vuopt::g_mvuBlockHookUser = nullptr;
		if (!r.ok)
		{
			std::printf("[vuopt] %s replay-failed\n", names[fi].c_str());
			failures++;
			continue;
		}

		vuopt::PipeConfig cfg;
		cfg.vuIndex = rec.vu_index;
		cfg.flagHack = opts.flagHack != 0;
		cfg.xgkickHack = EmuConfig.Gamefixes.XgKickHack;
		cfg.ibitHack = EmuConfig.Gamefixes.IbitHack;
		vuopt::PipeAnalyzer an(rec.microcode.data(), cfg);

		u64 matched = 0, ops = 0;
		std::map<std::string, u64> classes;
		std::vector<std::string> firstLines;
		for (const auto& mb : sink.blocks)
		{
			vuopt::PipeState entry;
			std::memcpy(entry.bytes, mb.entryState, 96);
			const vuopt::BlockResult br = an.Analyze(mb.startPC, entry);
			vuopt::MvuBlockTrace ours;
			br.ToTrace(ours, rec.vu_index);
			const auto diffs = vuopt::CompareBlockTraces(mb, ours);
			ops += mb.ops.size();
			if (diffs.empty())
			{
				matched++;
				continue;
			}
			std::map<std::string, int> seen;
			for (const auto& d : diffs)
			{
				const std::string c = MismatchClass(d);
				if (!seen[c]++)
					classes[c]++;
				if (opts.verbose || firstLines.size() < 6)
				{
					char head[48];
					std::snprintf(head, sizeof(head), "block@%04x: ", mb.startPC);
					firstLines.push_back(head + d);
				}
			}
		}

		std::printf("[vuopt] %s vu%u pc=%04x blocks=%zu match=%llu ops=%llu ebit=%d",
			names[fi].c_str(), rec.vu_index, rec.start_pc, sink.blocks.size(),
			static_cast<unsigned long long>(matched), static_cast<unsigned long long>(ops), r.jit_ebit ? 1 : 0);
		for (const auto& c : classes)
			std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
		std::printf("\n");
		for (const auto& l : firstLines)
			std::printf("  %s\n", l.c_str());

		totBlocks += sink.blocks.size();
		totMatched += matched;
		totOps += ops;
		for (const auto& c : classes)
			totClasses[c.first] += c.second;
	}

	std::printf("[vuopt-total] captures=%zu blocks=%llu match=%llu ops=%llu",
		records.size(), static_cast<unsigned long long>(totBlocks),
		static_cast<unsigned long long>(totMatched), static_cast<unsigned long long>(totOps));
	for (const auto& c : totClasses)
		std::printf(" %s:%llu", c.first.c_str(), static_cast<unsigned long long>(c.second));
	std::printf("\n");
	return failures ? 4 : 0;
}
