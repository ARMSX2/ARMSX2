// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vuopt live tier: tier-up policy and the glue between microVU's programs and
// blocks and vuopt's regions. Included at the end of microVU-arm64.cpp (it
// needs microVU's TU-local types and helpers); the policy constants and the
// part of the interface other code may use are in vuopt/VuOptLive.h, the
// design in compiler-design.md section 9x.
//
// Thread: everything here runs on the thread that executes VU1 (the EE thread,
// or the MTVU thread), and so does every compile it triggers, exactly like
// microVU's own. A reset (settings change, savestate load, cache exhaustion)
// reaches it from a thread that has waited for VU1 to go idle.

// (The vuopt headers are included at the top of microVU-arm64.cpp: they cannot
// follow microVU's macros.)

#include <map>
#include <memory>
#include <vector>

//------------------------------------------------------------------
// Per-program state
//------------------------------------------------------------------

static u32 vuoptTotalRuns(const microProgram& p) { return p.vuoptRuns + p.vuoptJumpRuns; }

// vuopt::live::kRetierCheckMask + 1 as a shift
static constexpr u32 kCheckShift = 10;
static_assert(vuopt::live::kRetierCheckMask + 1 == (1u << kCheckShift), "kCheckShift follows kRetierCheckMask");

// What the tier keeps for a program it has tiered up (microProgram::vuopt).
struct VuoptProgramState
{
	// Compiles of this program's graph under the tier: 1 after the tier-up,
	// one more per re-tier.
	u32 tiers = 0;
	// vuoptTotalRuns when the program was last compiled under the tier.
	u32 runsAtTier = 0;
	// The tier's jump-miss count when it last looked for new targets.
	u64 missesSeen = 0;
	// Looks for new targets that found none, in a row; at vuopt::live::kSettleChecks
	// the program is settled and no longer looks.
	u32 emptyChecks = 0;
	bool settled = false;
	// The program's runs (dispatches and JR slow-path entries, vuoptTotalRuns) in units of
	// kRetierCheckMask + 1 as of the last time a look for new targets was due. The count
	// climbs on two paths, so it can step over a multiple; a look is due whenever it has
	// passed into a new unit.
	u32 checkBlock = 0;
	// JR/JALR targets seen for each jumping block, by the jumping block's start
	// PC (bytes) and in ascending order: what the regions dispatch directly.
	std::map<u32, std::vector<u32>> jumpTargets;
	u32 targetCount = 0;
	// The block managers of every earlier generation of the program's graph.
	// Their code is still reachable (stale jump caches of other programs'
	// blocks hold its entries, a parked resume can point into it) and it
	// addresses its blocks by raw pointer, so they live until the program is
	// deleted at the next reset, which also throws the code away.
	std::vector<microBlockManager*> graveyard;
};

namespace
{
	struct VuoptLive
	{
		// The tier runs this session.
		bool enabled = false;
		// The counting lookup asked for the full search (set by the fast path,
		// consumed by mVUvuoptAfterSearch on the same dispatch).
		bool pending = false;
		u32 tierUpRuns = vuopt::live::kTierUpRuns;
		// ... and for a run counted on the JR/JALR slow path.
		u32 jumpTierUpRuns = vuopt::live::kJumpTierUpRuns;
		// JR/JALR the jump cache missed, VU1.
		u64 jumpMisses = 0;
		// Nesting of mVUcompile for VU1.
		int compileDepth = 0;
		std::unique_ptr<vuopt::RegionTier> region;
		std::unique_ptr<vuopt::BodyTier> body;
		vuopt::live::Stats stats;
	};
	VuoptLive s_vuopt;
} // namespace

//------------------------------------------------------------------
// Config
//------------------------------------------------------------------

void mVUvuoptSyncConfig(microVU& mVU)
{
	if (mVU.index != 1)
		return;

	// The gamefixes and clamp mode vuopt does not model (it declines every
	// block under them, see RegionTier::OnCompile): tiering a program up would
	// only set its microVU code aside.
	const Pcsx2Config::RecompilerOptions& rc = EmuConfig.Cpu.Recompiler;
	const bool supported = !CHECK_VU_EXACT(1) && !CHECK_XGKICKHACK && !EmuConfig.Gamefixes.IbitHack &&
		!CHECK_VUOVERFLOWHACK && !CHECK_VUADDSUBHACK && !EmuConfig.Gamefixes.VUSyncHack &&
		!EmuConfig.Gamefixes.FullVU0SyncHack;
	const bool want = rc.EnableVUOpt && rc.EnableVU1 && supported;

	if (want != s_vuopt.enabled)
		DevCon.WriteLn(Color_StrongBlue, "vuopt: live tier %s%s", want ? "ENABLED" : "disabled",
			(rc.EnableVUOpt && !want) ? " (a gamefix or clamp mode it does not model is on)" : "");
	s_vuopt.enabled = want;
	s_vuopt.pending = false;
	s_vuopt.compileDepth = 0;
}

bool mVUvuoptEnabled() { return s_vuopt.enabled; }
bool mVUvuoptPending() { return s_vuopt.pending; }

namespace vuopt::live
{
	bool IsEnabled() { return s_vuopt.enabled; }

#ifdef PCSX2_RECOMPILER_TESTS
	void SetTierUpRuns(u32 runs) { s_vuopt.tierUpRuns = runs ? runs : 1; }
	void SetJumpTierUpRuns(u32 runs) { s_vuopt.jumpTierUpRuns = runs ? runs : 1; }
#endif

	Stats GetStats()
	{
		Stats s = s_vuopt.stats;
		if (s_vuopt.region)
		{
			s.rootsCompiled = s_vuopt.region->Compiles();
			s.regions = s_vuopt.region->Regions();
			s.regionNodes = s_vuopt.region->Nodes();
			s.regionExits = s_vuopt.region->Exits();
			s.regionBytes = s_vuopt.region->Bytes();
		}
		if (s_vuopt.body)
		{
			s.bodyBlocks = s_vuopt.body->Blocks();
			s.bodiesLowered = s_vuopt.body->Lowered();
			s.bodyBytes = s_vuopt.body->BodyBytes();
		}
		s.jumpMisses = s_vuopt.jumpMisses;
		// the bails of the programs still alive (the deleted ones were added as they went)
		for (const auto& entry : microVU1.mvuContentMap)
		{
			s.bailsEntry += entry.second->vuoptBails[0];
			s.bailsLoopHead += entry.second->vuoptBails[1];
		}
		for (const microProgram* prog : microVU1.mvuOrphanedProgs)
		{
			s.bailsEntry += prog->vuoptBails[0];
			s.bailsLoopHead += prog->vuoptBails[1];
		}
		return s;
	}

	std::string FormatStats()
	{
		const Stats s = GetStats();
		std::string out;
		char buf[768];
		std::snprintf(buf, sizeof(buf),
			"vuopt live tier: tierups=%llu retiers=%llu retierchecks=%llu blocks_set_aside=%llu targets=%llu jumpmisses=%llu "
			"roots=%llu regions=%llu rnodes=%llu rexits=%llu rbytes=%llu bodyblocks=%llu bodies=%llu bodybytes=%llu bails=%llu "
			"jrtierups=%llu jrcounted=%llu xprogtargets=%llu bails_entry=%llu bails_loophead=%llu",
			(unsigned long long)s.tierUps, (unsigned long long)s.retiers, (unsigned long long)s.retierChecks,
			(unsigned long long)s.blocksSetAside, (unsigned long long)s.targetsRecorded, (unsigned long long)s.jumpMisses,
			(unsigned long long)s.rootsCompiled, (unsigned long long)s.regions, (unsigned long long)s.regionNodes,
			(unsigned long long)s.regionExits, (unsigned long long)s.regionBytes, (unsigned long long)s.bodyBlocks,
			(unsigned long long)s.bodiesLowered, (unsigned long long)s.bodyBytes, (unsigned long long)s.bailCompiles,
			(unsigned long long)s.jumpTierUps, (unsigned long long)s.jumpCounted, (unsigned long long)s.crossProgTargets,
			(unsigned long long)s.bailsEntry, (unsigned long long)s.bailsLoopHead);
		out = buf;
		if (s.compileInsTiered || s.compileInsPlain)
		{
			std::snprintf(buf, sizeof(buf), "\n  compile instructions: tiered programs %llu, others %llu",
				(unsigned long long)s.compileInsTiered, (unsigned long long)s.compileInsPlain);
			out += buf;
		}
		const auto add = [&](const char* tier, const std::map<std::string, u32>& m) {
			for (const auto& d : m)
			{
				std::snprintf(buf, sizeof(buf), "\n  %s declined: %s = %u", tier, d.first.c_str(), d.second);
				out += buf;
			}
		};
		if (s_vuopt.region)
			add("region", s_vuopt.region->Declined());
		if (s_vuopt.body)
			add("body", s_vuopt.body->Declined());
		return out;
	}
} // namespace vuopt::live

//------------------------------------------------------------------
// The dispatch path
//------------------------------------------------------------------

// mVUlookupProg_VU1 plus the run counter. A program reaches the tier-up
// threshold, and a tiered one the periodic look for new jump targets, by
// returning null: the dispatcher then takes the full path (mVUexecuteVU1), which
// ends in mVUvuoptAfterSearch.
void* mVUlookupProgTier_VU1(u32 startPC, u32 cycles)
{
	microVU& mVU = microVU1;
	mVU.cycles = cycles;
	mVU.totalCycles = cycles;
	const u32 maskedPC = startPC & 0x3ff8;
#ifdef PCSX2_RECOMPILER_TESTS
	vu_capture::MaybeCapture(1, maskedPC, cycles,
		mVU.regs().Micro, mVU.microMemSize,
		mVU.regs().Mem, mVU.microMemSize,
		mVU.regs());
#endif
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	microProgram* const prog = quick.prog;
	if (!prog)
		return nullptr;
	microBlockManager* block = prog->block[maskedPC / 8];
	if (!block)
		return nullptr;
	microBlock* pBlock = block->search(mVU, (microRegInfo*)&mVU.prog.lpState);
	if (!pBlock)
		return nullptr;

	mVU.prog.isSame = -1;
	mVU.prog.cur = prog;
	quick.block = block;
	if (mVUPersist::IsRecordingEnabled())
		prog->observed.record(maskedPC);

	const u32 runs = ++prog->vuoptRuns;
	VuoptProgramState* const st = prog->vuopt;
	if (!st)
	{
		if (runs >= s_vuopt.tierUpRuns)
		{
			s_vuopt.pending = true;
			return nullptr;
		}
	}
	else if (!st->settled)
	{
		const u32 total = runs + prog->vuoptJumpRuns;
		if ((total >> kCheckShift) != st->checkBlock)
		{
			// a look for new targets is due; the crossing is used up either way
			st->checkBlock = total >> kCheckShift;
			if (st->missesSeen != s_vuopt.jumpMisses)
			{
				s_vuopt.pending = true;
				return nullptr;
			}
		}
	}
	return pBlock->hostEntry;
}

// The JR/JALR targets microVU recorded for a program: every jumping block's
// jump cache holds one entry per target it has reached (the entry's program is
// the one the target resolved to, ours when the target is our own code).
// Adds the ones `st` does not have yet, in ascending order within each jumping
// block; returns how many it added.
static u32 vuoptHarvestTargets(microProgram& prog, VuoptProgramState& st)
{
	u32 added = 0;
	for (u32 i = 0; i < mProgSize / 2; i++)
	{
		microBlockManager* m = prog.block[i];
		if (!m)
			continue;
		m->forEachBlock([&](microBlock& b) {
			if (!b.jumpCache)
				return;
			std::vector<u32>& v = st.jumpTargets[i * 8];
			for (u32 t = 0; t < mProgSize / 2; t++)
			{
				if (b.jumpCache[t].prog != &prog)
				{
					// (A target that resolved to another program is not ours to
					// compile: its code would enter this program's ranges and make it
					// match only while that code is loaded, and a direct edge would skip
					// the jump cache's check that the live program is still that one.)
					if (b.jumpCache[t].prog)
						s_vuopt.stats.crossProgTargets++;
					continue;
				}
				const u32 pc = t * 8;
				const auto at = std::lower_bound(v.begin(), v.end(), pc);
				if (at != v.end() && *at == pc)
					continue;
				if (v.size() >= vuopt::live::kMaxTargetsPerJump || st.targetCount >= vuopt::live::kMaxTargetsPerProgram)
					continue;
				v.insert(at, pc);
				st.targetCount++;
				added++;
			}
		});
	}
	// A jumping block with no target left an empty entry behind.
	for (auto it = st.jumpTargets.begin(); it != st.jumpTargets.end();)
		it = it->second.empty() ? st.jumpTargets.erase(it) : std::next(it);
	s_vuopt.stats.targetsRecorded += added;
	return added;
}

// Sets the program's microVU graph aside and arms the compile hooks for it:
// the next compile of any of its blocks goes to vuopt first.
static void vuoptTierUp(microVU& mVU, microProgram& prog, bool retier)
{
	if (!prog.vuopt)
		prog.vuopt = new VuoptProgramState();
	VuoptProgramState& st = *prog.vuopt;

	// Targets first: the jump caches go with the blocks.
	vuoptHarvestTargets(prog, st);

	u32 setAside = 0;
	for (u32 i = 0; i < mProgSize / 2; i++)
	{
		if (!prog.block[i])
			continue;
		prog.block[i]->forEachBlock([&](microBlock&) { setAside++; });
		st.graveyard.push_back(prog.block[i]);
		prog.block[i] = nullptr;
	}
	for (u32 i = 0; i < mVU.progSize / 2; i++)
		if (mVU.prog.quick[i].prog == &prog)
			mVU.prog.quick[i].block = nullptr;
	mVU.resumeEntry = nullptr;
	mVU.resumeBlock = nullptr;

	st.tiers++;
	st.runsAtTier = vuoptTotalRuns(prog);
	st.checkBlock = vuoptTotalRuns(prog) >> kCheckShift;
	st.missesSeen = s_vuopt.jumpMisses;
	if (st.tiers > vuopt::live::kMaxRetiers)
		st.settled = true;

	if (retier)
		s_vuopt.stats.retiers++;
	else
		s_vuopt.stats.tierUps++;
	s_vuopt.stats.blocksSetAside += setAside;
	DevCon.WriteLn(Color_StrongBlue, "vuopt: %s prog[%03d] after %u runs: %u blocks set aside, %u jump targets over %zu jumps",
		retier ? "re-tier" : "tier-up", prog.idx, vuoptTotalRuns(prog), setAside, st.targetCount, st.jumpTargets.size());
}

// The decision both slow paths share. `prog` (mVU.prog.cur) has been resolved and its
// run counters bumped; `result` is the entry the search found for `startPC`. An untiered
// program is tiered up when `tierNow` (the caller's threshold test); a tiered one looks for
// new jump targets when it has met some. Either way the entry is compiled again with the
// hooks on.
static void* vuoptConsider(microVU& mVU, microProgram& prog, u32 startPC, uptr pState, void* result, bool tierNow, bool viaJump)
{
	VuoptProgramState* const st = prog.vuopt;
	if (!st)
	{
		if (!tierNow)
			return result;
		vuoptTierUp(mVU, prog, false);
		if (viaJump)
			s_vuopt.stats.jumpTierUps++;
	}
	else
	{
		if (st->settled || vuoptTotalRuns(prog) - st->runsAtTier < vuopt::live::kRetierMinRuns ||
			st->missesSeen == s_vuopt.jumpMisses)
			return result;
		s_vuopt.stats.retierChecks++;
		st->missesSeen = s_vuopt.jumpMisses;
		// Targets the jump caches saw that the regions do not hold. If there are
		// none the program stays as it is.
		if (vuoptHarvestTargets(prog, *st) == 0)
		{
			if (++st->emptyChecks >= vuopt::live::kSettleChecks)
				st->settled = true;
			return result;
		}
		st->emptyChecks = 0;
		vuoptTierUp(mVU, prog, true);
	}

	// Compile the entry again, now with the hooks: what mVUsearchProg does for
	// a program with no block at this PC.
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	void* const entry = mVUblockFetch(mVU, startPC, pState);
	quick.block = prog.block[startPC / 8];
	return entry;
}

// The dispatcher's slow path's chance to tier a program up: it has been resolved
// (mVU.prog.cur is its program) and `result` is the entry mVUsearchProg found for it.
void* mVUvuoptAfterSearch(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuopt.enabled || mVU.index != 1)
		return result;
	// The counting lookup already counted a dispatch that asked for this.
	const bool counted = s_vuopt.pending;
	s_vuopt.pending = false;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
		return result;
	if (!counted)
		prog->vuoptRuns++;
	return vuoptConsider(mVU, *prog, startPC, pState, result, prog->vuoptRuns >= s_vuopt.tierUpRuns, false);
}

// The same for a JR/JALR the jump cache missed (mVUcompileJIT), after the search resolved
// its target to a program. Counts the entry on that program, and tiers it up when jumps are
// how it is reached: a program the dispatcher never enters (an overlay a stub program jumps
// into) has no dispatch count to go by. A program the dispatcher does enter keeps its
// dispatch threshold: tiering it from a jump into the middle of it would root its first
// compile there, which splits what a compile from the program's entry keeps in one region.
void* mVUvuoptAfterJump(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuopt.enabled || mVU.index != 1)
		return result;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
		return result;
	const u32 jumps = ++prog->vuoptJumpRuns;
	s_vuopt.stats.jumpCounted++;
	bool tierNow = false;
	if (VuoptProgramState* const st = prog->vuopt)
	{
		// A tiered program looks for new targets once per unit of runs, as on the
		// dispatch path, not at every entry.
		const u32 total = vuoptTotalRuns(*prog);
		if (st->settled || (total >> kCheckShift) == st->checkBlock)
			return result;
		st->checkBlock = total >> kCheckShift;
	}
	else
	{
		// (jumps dominate: sixteen to one over dispatches)
		tierNow = jumps >= s_vuopt.jumpTierUpRuns && static_cast<u64>(prog->vuoptRuns) * 16 <= jumps;
	}
	return vuoptConsider(mVU, *prog, startPC, pState, result, tierNow, true);
}

void mVUvuoptOnJumpMiss(microVU& mVU)
{
	if (s_vuopt.enabled && mVU.index == 1)
		s_vuopt.jumpMisses++;
}

void mVUvuoptOnBail()
{
	s_vuopt.stats.bailCompiles++;
}

// The tier's counters, once, when VU1 shuts down.
void mVUvuoptOnClose(microVU& mVU)
{
	if (mVU.index == 1 && s_vuopt.stats.tierUps)
		Console.WriteLn(Color_StrongBlue, "%s", vuopt::live::FormatStats().c_str());
}

void mVUvuoptOnProgramDeleted(microProgram& prog)
{
	s_vuopt.stats.bailsEntry += prog.vuoptBails[0];
	s_vuopt.stats.bailsLoopHead += prog.vuoptBails[1];
	VuoptProgramState* st = prog.vuopt;
	if (!st)
		return;
	for (microBlockManager* m : st->graveyard)
		delete m;
	delete st;
	prog.vuopt = nullptr;
}

//------------------------------------------------------------------
// The compile hooks
//------------------------------------------------------------------

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
// With pcsx2-eerunner --count-vu1-ins: the user-mode instructions the outermost
// compile sessions of VU1 retire, split by whether the program was tiered up
// (vuopt compiles) or not (microVU's own), for reading the tier's compile cost
// next to its run-time gain. Totals are in vuopt::live::Stats.
static u64 s_vuoptSessionIns0 = 0;
static bool s_vuoptSessionTiered = false;
#endif

void mVUvuoptCompileBegin(microVU& mVU)
{
	if (mVU.index == 1 && s_vuopt.enabled)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (s_vuopt.compileDepth == 0 && mvu_test_hooks::g_vu1InsCount.load(std::memory_order_relaxed))
		{
			s_vuoptSessionIns0 = mvu_test_hooks::ReadThreadInstructions();
			s_vuoptSessionTiered = mVU.prog.cur && mVU.prog.cur->vuopt;
		}
#endif
		s_vuopt.compileDepth++;
	}
}

void mVUvuoptCompileEnd(microVU& mVU)
{
	if (mVU.index != 1 || !s_vuopt.enabled || s_vuopt.compileDepth <= 0)
		return;
	if (--s_vuopt.compileDepth == 0)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (mvu_test_hooks::g_vu1InsCount.load(std::memory_order_relaxed))
		{
			const u64 d = mvu_test_hooks::ReadThreadInstructions() - s_vuoptSessionIns0;
			(s_vuoptSessionTiered ? s_vuopt.stats.compileInsTiered : s_vuopt.stats.compileInsPlain) += d;
		}
#endif
		if (s_vuopt.region)
			s_vuopt.region->ResetAnalyzer();
		if (s_vuopt.body)
			s_vuopt.body->ResetAnalyzer();
	}
}

static bool vuoptLiveRegionHook(const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void* user)
{
	const VuoptProgramState& st = *static_cast<const VuoptProgramState*>(user);
	if (!s_vuopt.region)
		s_vuopt.region = std::make_unique<vuopt::RegionTier>(1, microVU1.regs().Micro);
	s_vuopt.region->jumpTargets = st.jumpTargets;
	return s_vuopt.region->OnCompile(b, env, nullptr);
}

static bool vuoptLiveBodyHook(const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void*)
{
	if (!s_vuopt.body)
		s_vuopt.body = std::make_unique<vuopt::BodyTier>(1, microVU1.regs().Micro);
	return s_vuopt.body->TryLower(b, env, nullptr);
}

void mVUvuoptLiveHooks(microVU& mVU, vuopt::VuoptRegionHook& region, void*& regionUser,
	vuopt::VuoptBodyHook& body, void*& bodyUser)
{
	if (mVU.index != 1 || !s_vuopt.enabled)
		return;
	microProgram* const cur = mVU.prog.cur;
	if (!cur || !cur->vuopt)
		return;
	region = &vuoptLiveRegionHook;
	regionUser = cur->vuopt;
	body = &vuoptLiveBodyHook;
	bodyUser = cur->vuopt;
}
