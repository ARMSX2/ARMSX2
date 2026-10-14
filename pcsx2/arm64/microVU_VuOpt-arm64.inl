// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vuopt live tier: tier-up policy and the glue between microVU's programs and
// blocks and vuopt's regions. Included at the end of microVU-arm64.cpp (it
// needs microVU's TU-local types and helpers); the policy constants and the
// part of the interface other code may use are in vuopt/VuOptLive.h, the
// design in compiler-design.md sections 9x (the tier) and 9aa (the background
// compile).
//
// Threads. Everything here runs on the thread that executes VU1 (the EE thread,
// or the MTVU thread), except VuoptJob::Run and what it calls, which run on the
// tier's worker thread (vuopt::bg::Worker). A job holds copies of everything it
// reads (micro memory, the program's block keys and entries, the jump targets,
// microVU's addresses as numbers) and writes only its own memory, so the two
// threads share nothing but the worker's queue. What a job made is published
// on the VU1 thread at a slow path microVU already takes (a dispatch that
// missed the quick path, or a JR/JALR the jump cache missed), after checking
// that the program and every micro-memory word the compile read are still what
// they were. A reset (settings change, savestate load, cache exhaustion)
// reaches this file from a thread that has waited for VU1 to go idle; it bumps
// the epoch, and whatever a job made before it is dropped unread.

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

// A first tier-up whose job is dropped (queue full) or discarded (micro memory
// changed under it) is asked for again after another kTierUpRuns runs, this
// many times at most.
static constexpr u32 kMaxGenAttempts = 4;

struct VuoptJob;

// What the tier keeps for a program it has started to tier up (microProgram::vuopt).
// `gen` says whether vuopt's code is in place: 0 while microVU's own graph still
// runs (the first generation's job is queued or running), then one more for every
// published generation.
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

	// Section 9aa: the background compile.
	u32 gen = 0;
	// A generation job is queued, running, or finished and not yet published.
	bool genOutstanding = false;
	// First-generation jobs dropped or discarded (kMaxGenAttempts).
	u32 genAttempts = 0;
	// vuoptTotalRuns at the last generation request.
	u32 requestRuns = 0;
	// Finished jobs waiting to be published, in completion order.
	std::vector<std::shared_ptr<VuoptJob>> ready;
};

// A block as a job sees it. The pipeline state comes first, where microBlock
// keeps it: a region's bail records hold the node's block pointer, and the bail
// (vuoptBailTarget) reads only the state through it, so the node's handle can
// stand in for the microBlock the worker cannot touch. Handles live until the
// next reset, like the code that points at them.
struct VuoptWBlock
{
	alignas(16) microRegInfo pState;
	u32 pc = 0;
	// A block of the program's graph when the job was made: its entry.
	bool snapshot = false;
	void* entry = nullptr;
	// A node of the job's region: its entry, as an offset into the job's code.
	bool hasEntry = false;
	u32 entryOff = 0;
	// The microBlock it became at publication.
	microBlock* real = nullptr;
};
static_assert(offsetof(VuoptWBlock, pState) == 0 && offsetof(microBlock, pState) == 0,
	"bail records read the pipeline state at the start of the block they point at");

// The options of the region builder a job runs (the live tier uses the
// defaults; pcsx2-vurunner's background arm copies its own).
struct VuoptRegionOptions
{
	s32 maxVHomes = 14, maxGHomes = 6;
	bool schedule = true, splitHomes = true, stickyAcc = true, accAlways = false, handoffLiveness = true, dump = false;
	vuopt::RegionTier::HandoffMutation handoffMutation = vuopt::RegionTier::HandoffMutation::None;

	void From(const vuopt::RegionTier& t)
	{
		maxVHomes = t.maxVHomes;
		maxGHomes = t.maxGHomes;
		schedule = t.schedule;
		splitHomes = t.splitHomes;
		stickyAcc = t.stickyAcc;
		accAlways = t.accAlways;
		handoffLiveness = t.handoffLiveness;
		handoffMutation = t.handoffMutation;
		dump = t.dump;
	}
	void To(vuopt::RegionTier& t) const
	{
		t.maxVHomes = maxVHomes;
		t.maxGHomes = maxGHomes;
		t.schedule = schedule;
		t.splitHomes = splitHomes;
		t.stickyAcc = stickyAcc;
		t.accAlways = accAlways;
		t.handoffLiveness = handoffLiveness;
		t.handoffMutation = handoffMutation;
		t.dump = dump;
	}
};

// One region to build on the worker: rooted at the entry the tier-up came
// through (Generation: a new graph for the program, published by setting the
// old one aside), or at a block microVU met in a program already tiered up
// (Root: published into the program's current graph).
struct VuoptJob final : vuopt::bg::Job
{
	enum Kind : u8
	{
		Generation,
		Root,
	};

	// Placeholder ids for the outside targets the region branches to.
	enum Stub : u32
	{
		kPhBailTarget,
		kPhClearLpState,
		kPhEndFlagsB,
		kPhExitEBit,
		kPhXgkick,
		kPhCycleBreak,
		kPhStubs,
	};
	enum PhKind : u8
	{
		PhStub,
		PhExit,
		PhTail,
	};

	// Identity (the worker never dereferences `prog`).
	u64 epoch = 0;
	microProgram* prog = nullptr;
	u32 gen = 0;
	Kind kind = Generation;
	bool retier = false;
	bool viaJump = false;
	u32 pc = 0;
	alignas(16) microRegInfo key;
	u64 requestedAt = 0;   // VU1 dispatches so far
	u64 missesAtRequest = 0;

	// Inputs.
	std::unique_ptr<u8[]> micro; // micro memory when the job was made (kept: publication compares against it)
	std::map<u32, std::vector<u32>> jumpTargets;
	bool haveTrace = false;
	vuopt::MvuBlockTrace trace; // microVU's first pass of the root (Root jobs)
	vuopt::MvuEmitEnv env;      // microVU's values; the worker swaps in its own callbacks
	bool flagHack = false;      // block search follows it (microBlockManager::search)
	const u8* cacheLo = nullptr; // microVU's code cache, where the region lands
	const u8* cacheHi = nullptr;
	VuoptRegionOptions opts;
	std::vector<std::unique_ptr<VuoptWBlock>> blocks; // the snapshot, then the job's nodes

	// Outputs.
	bool accepted = false;
	std::string why;
	vuopt::MvuBlockTrace rootTrace; // the port's first pass of the root (Generation jobs)
	std::vector<u8> code;
	std::vector<vuopt::bg::Fixup> fixups;
	VuoptWBlock* root = nullptr;
	std::vector<VuoptWBlock*> nodes; // registration order, the root first
	std::vector<std::pair<u32, u32>> ranges;
	struct Exit
	{
		u32 pc;
		alignas(16) microRegInfo key;
	};
	std::vector<Exit> exits;
	struct Tail
	{
		VuoptWBlock* blk;
		alignas(16) microRegInfo endKey;
	};
	std::vector<Tail> tails;
	std::vector<std::pair<PhKind, u32>> ph; // per placeholder id
	std::vector<u8> readMask;               // per micro-memory word: the compile read it
	u32 regions = 0, regionNodes = 0, regionExits = 0;
	u64 regionBytes = 0;
	std::map<std::string, u32> declined;
	u64 workerIns = 0;

	void Run() override;
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
		std::unique_ptr<vuopt::BodyTier> body;
		vuopt::live::Stats stats;

		// Section 9aa.
		// Deterministic mode (tests, pcsx2-eerunner --vuopt-sync): every job is
		// waited for and published where it was asked for, which is where the
		// synchronous tier compiled.
		bool synchronous = false;
		// Bumped at every reset: a job from an earlier epoch is dropped unread.
		u64 epoch = 1;
		// VU1 dispatches while the tier is on (publication lag).
		u64 dispatches = 0;
		// The compile depth of a block whose region was handed to the worker:
		// the blocks microVU compiles under it now belong to that region.
		int plainRootDepth = 0;
		// A generation being published: its root's compile places it.
		VuoptJob* prepared = nullptr;
		std::unique_ptr<vuopt::bg::Worker> worker;
		// Node handles of placed regions (VuoptWBlock).
		std::vector<std::unique_ptr<VuoptWBlock>> handles;
		std::atomic<u64> workerIns{0};
	};
	// Never destroyed: recMicroVU1's own destructor can reach mVUclose (and so
	// mVUvuoptOnClose) after this translation unit's statics are gone.
	VuoptLive& s_vuopt = *new VuoptLive();
} // namespace

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
static bool vuoptCountingIns() { return mvu_test_hooks::g_vu1InsCount.load(std::memory_order_relaxed); }
static u64 vuoptReadIns() { return vuoptCountingIns() ? mvu_test_hooks::ReadThreadInstructions() : 0; }
#else
static u64 vuoptReadIns() { return 0; }
#endif

//------------------------------------------------------------------
// Config
//------------------------------------------------------------------

static vuopt::bg::Worker& vuoptWorker()
{
	if (!s_vuopt.worker)
		s_vuopt.worker = std::make_unique<vuopt::bg::Worker>();
	s_vuopt.worker->Start();
	return *s_vuopt.worker;
}

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

	// Whatever the worker holds or has made belongs to the code this reset
	// throws away. A job still running finishes into its own memory and is
	// dropped when it is collected (its epoch is old).
	s_vuopt.epoch++;
	s_vuopt.prepared = nullptr;
	s_vuopt.plainRootDepth = 0;
	s_vuopt.handles.clear();
	if (s_vuopt.worker)
	{
		if (want)
		{
			s_vuopt.worker->RemoveQueued([](const vuopt::bg::Job&) { return true; });
			std::vector<std::shared_ptr<vuopt::bg::Job>> stale;
			s_vuopt.worker->TakeCompleted(stale);
		}
		else
			s_vuopt.worker.reset(); // (stops it: no worker thread with the tier off)
	}
	if (want)
		vuoptWorker();
}

bool mVUvuoptEnabled() { return s_vuopt.enabled; }
bool mVUvuoptPending() { return s_vuopt.pending; }

namespace vuopt::live
{
	bool IsEnabled() { return s_vuopt.enabled; }

#ifdef PCSX2_RECOMPILER_TESTS
	void SetTierUpRuns(u32 runs) { s_vuopt.tierUpRuns = runs ? runs : 1; }
	void SetJumpTierUpRuns(u32 runs) { s_vuopt.jumpTierUpRuns = runs ? runs : 1; }
	void SetSynchronous(bool on) { s_vuopt.synchronous = on; }
	bool IsSynchronous() { return s_vuopt.synchronous; }
	bool WorkerRunning() { return s_vuopt.worker && s_vuopt.worker->Running(); }
	void HoldWorker(bool on)
	{
		if (s_vuopt.worker)
			s_vuopt.worker->Hold(on);
	}
	void SetWorkerGate(std::function<void()> gate)
	{
		if (s_vuopt.worker)
			s_vuopt.worker->SetGate(std::move(gate));
	}
	void WaitWorkerIdle()
	{
		if (s_vuopt.worker)
			s_vuopt.worker->WaitIdle();
	}
	size_t WorkerQueued() { return s_vuopt.worker ? s_vuopt.worker->Queued() : 0; }
#endif

	Stats GetStats()
	{
		Stats s = s_vuopt.stats;
		if (s_vuopt.body)
		{
			s.bodyBlocks = s_vuopt.body->Blocks();
			s.bodiesLowered = s_vuopt.body->Lowered();
			s.bodyBytes = s_vuopt.body->BodyBytes();
		}
		s.jumpMisses = s_vuopt.jumpMisses;
		s.workerIns = s_vuopt.workerIns.load(std::memory_order_relaxed);
		return s;
	}

	std::string FormatStats()
	{
		const Stats s = GetStats();
		std::string out;
		char buf[1024];
		std::snprintf(buf, sizeof(buf),
			"vuopt live tier: tierups=%llu retiers=%llu retierchecks=%llu blocks_set_aside=%llu targets=%llu jumpmisses=%llu "
			"roots=%llu regions=%llu rnodes=%llu rexits=%llu rbytes=%llu bodyblocks=%llu bodies=%llu bodybytes=%llu bails=%llu "
			"jrtierups=%llu jrcounted=%llu xprogtargets=%llu",
			(unsigned long long)s.tierUps, (unsigned long long)s.retiers, (unsigned long long)s.retierChecks,
			(unsigned long long)s.blocksSetAside, (unsigned long long)s.targetsRecorded, (unsigned long long)s.jumpMisses,
			(unsigned long long)s.rootsCompiled, (unsigned long long)s.regions, (unsigned long long)s.regionNodes,
			(unsigned long long)s.regionExits, (unsigned long long)s.regionBytes, (unsigned long long)s.bodyBlocks,
			(unsigned long long)s.bodiesLowered, (unsigned long long)s.bodyBytes, (unsigned long long)s.bailCompiles,
			(unsigned long long)s.jumpTierUps, (unsigned long long)s.jumpCounted, (unsigned long long)s.crossProgTargets);
		out = buf;
		std::snprintf(buf, sizeof(buf),
			"\n  background: sync=%d jobs=%llu (gen %llu, root %llu) dropped=%llu done=%llu published=%llu (gen %llu, root %llu) "
			"declined=%llu coalesced=%llu discarded: stale=%llu gen=%llu memory=%llu space=%llu trace=%llu | "
			"lag(dispatches) gen avg=%.1f max=%llu root avg=%.1f max=%llu | worker_ins=%llu publish_ins=%llu veneers=%llu",
			s_vuopt.synchronous ? 1 : 0, (unsigned long long)(s.jobsGen + s.jobsRoot), (unsigned long long)s.jobsGen,
			(unsigned long long)s.jobsRoot, (unsigned long long)s.jobsDropped, (unsigned long long)s.jobsDone,
			(unsigned long long)(s.publishedGen + s.publishedRoot), (unsigned long long)s.publishedGen,
			(unsigned long long)s.publishedRoot, (unsigned long long)s.jobsDeclined, (unsigned long long)s.jobsCoalesced,
			(unsigned long long)s.discardStale, (unsigned long long)s.discardGen, (unsigned long long)s.discardMemory,
			(unsigned long long)s.discardSpace, (unsigned long long)s.discardTrace,
			s.publishedGen ? static_cast<double>(s.lagGenSum) / s.publishedGen : 0.0, (unsigned long long)s.lagGenMax,
			s.publishedRoot ? static_cast<double>(s.lagRootSum) / s.publishedRoot : 0.0, (unsigned long long)s.lagRootMax,
			(unsigned long long)s.workerIns, (unsigned long long)s.publishIns, (unsigned long long)s.veneers);
		out += buf;
		if (s.compileInsTiered || s.compileInsPlain)
		{
			std::snprintf(buf, sizeof(buf), "\n  compile instructions: tiered programs %llu, others %llu",
				(unsigned long long)s.compileInsTiered, (unsigned long long)s.compileInsPlain);
			out += buf;
		}
		for (const auto& d : s_vuopt.stats.regionDeclined)
		{
			std::snprintf(buf, sizeof(buf), "\n  region declined: %s = %u", d.first.c_str(), d.second);
			out += buf;
		}
		if (s_vuopt.body)
			for (const auto& d : s_vuopt.body->Declined())
			{
				std::snprintf(buf, sizeof(buf), "\n  body declined: %s = %u", d.first.c_str(), d.second);
				out += buf;
			}
		return out;
	}
} // namespace vuopt::live

//------------------------------------------------------------------
// The worker's side of a job
//------------------------------------------------------------------

namespace
{
	// microBlockManager's lookup over a job's blocks, by the same rules: a key
	// that needs an exact match compares all 96 bytes against the blocks
	// registered with exact keys; any other key compares the first 8 bytes
	// (flag-hack bits masked as microVU masks them) and takes the first block
	// registered with them, whichever list it is on.
	struct VuoptGraph
	{
		struct Slot
		{
			std::vector<VuoptWBlock*> all, full; // registration order; the exact-key blocks
		};
		std::map<u32, Slot> slots;
		bool flagHack = false;

		VuoptWBlock* Search(u32 pc, const microRegInfo& k) const
		{
			const auto it = slots.find(pc / 8);
			if (it == slots.end())
				return nullptr;
			if (k.needExactMatch)
			{
				for (VuoptWBlock* b : it->second.full)
					if (std::memcmp(&b->pState, &k, sizeof(k)) == 0)
						return b;
				return nullptr;
			}
			for (VuoptWBlock* b : it->second.all)
			{
				if (flagHack ? ((b->pState.quick64[0] & ~0x3002ull) != (k.quick64[0] & ~0x3002ull)) : (b->pState.quick64[0] != k.quick64[0]))
					continue;
				return b;
			}
			return nullptr;
		}
		void Add(VuoptWBlock* b)
		{
			Slot& s = slots[b->pc / 8];
			s.all.push_back(b);
			if (b->pState.needExactMatch)
				s.full.push_back(b);
		}
	};

	struct VuoptWorkerContext
	{
		VuoptJob* job = nullptr;
		vuopt::bg::CodeSink* sink = nullptr;
		VuoptGraph graph;
	};
	thread_local VuoptWorkerContext* t_vuoptCtx = nullptr;

	VuoptWBlock* vuoptNewNode(VuoptJob& j, u32 pc, const void* key)
	{
		auto w = std::make_unique<VuoptWBlock>();
		std::memcpy(&w->pState, key, sizeof(microRegInfo));
		w->pc = pc;
		VuoptWBlock* const p = w.get();
		j.blocks.push_back(std::move(w));
		j.nodes.push_back(p);
		t_vuoptCtx->graph.Add(p);
		return p;
	}

	const void* vuoptPlaceholder(VuoptJob& j, VuoptJob::PhKind kind, u32 index)
	{
		const u32 id = static_cast<u32>(j.ph.size());
		pxAssertRel(id < vuopt::bg::CodeSink::kMaxPlaceholders, "vuopt worker: out of placeholders");
		j.ph.push_back({kind, index});
		return t_vuoptCtx->sink->Placeholder(id);
	}

	// The region builder's microVU callbacks, on the worker.
	void* vuoptWSearch(u32, u32 pc, const u8* key)
	{
		alignas(16) microRegInfo k;
		std::memcpy(&k, key, sizeof(k));
		return t_vuoptCtx->graph.Search(pc & 0x3ff8, k);
	}
	void* vuoptWAdd(u32, u32 pc, const u8* key) { return vuoptNewNode(*t_vuoptCtx->job, pc & 0x3ff8, key); }
	void vuoptWSetEntry(void* block, void* entry)
	{
		VuoptWBlock* b = static_cast<VuoptWBlock*>(block);
		pxAssertRel(!b->snapshot, "vuopt worker: an entry set on a block of the program's graph");
		b->hasEntry = true;
		b->entryOff = static_cast<u32>(static_cast<u8*>(entry) - t_vuoptCtx->sink->Base());
	}
	void* vuoptWEntry(void* block)
	{
		const VuoptWBlock* b = static_cast<const VuoptWBlock*>(block);
		if (b->snapshot)
			return b->entry;
		// (a node's entry is known only once the region is emitted; the only
		// one asked for before that is the root's, which is where emission starts)
		pxAssertRel(b->hasEntry, "vuopt worker: the entry of a node not yet emitted");
		return t_vuoptCtx->sink->Base() + b->entryOff;
	}
	void* vuoptWFetch(u32, u32 pc, const u8* key)
	{
		VuoptJob& j = *t_vuoptCtx->job;
		VuoptJob::Exit e;
		e.pc = pc & 0x3ff8;
		std::memcpy(&e.key, key, sizeof(e.key));
		j.exits.push_back(e);
		return const_cast<void*>(vuoptPlaceholder(j, VuoptJob::PhExit, static_cast<u32>(j.exits.size() - 1)));
	}
	void vuoptWRange(u32, u32 startPC, u32 lastPC) { t_vuoptCtx->job->ranges.push_back({startPC, lastPC}); }
	void vuoptWTail(u32, void* block, const u8* endKey)
	{
		VuoptJob& j = *t_vuoptCtx->job;
		VuoptJob::Tail t;
		t.blk = static_cast<VuoptWBlock*>(block);
		std::memcpy(&t.endKey, endKey, sizeof(t.endKey));
		j.tails.push_back(t);
		armEmitJmp(vuoptPlaceholder(j, VuoptJob::PhTail, static_cast<u32>(j.tails.size() - 1)));
	}
} // namespace

void VuoptJob::Run()
{
	const u64 ins0 = vuoptReadIns();
	thread_local std::unique_ptr<vuopt::bg::CodeSink> sink;
	if (!sink)
		sink = std::make_unique<vuopt::bg::CodeSink>();
	VuoptWorkerContext ctx;
	ctx.job = this;
	ctx.sink = sink.get();
	ctx.graph.flagHack = flagHack;
	for (const auto& b : blocks)
		ctx.graph.Add(b.get());
	t_vuoptCtx = &ctx;

	readMask.assign(0x4000 / 4, 0);
	vuopt::g_pipeReadMask = readMask.data();

	vuopt::RegionTier tier(1, micro.get());
	opts.To(tier);
	tier.jumpTargets = jumpTargets;

	vuopt::MvuBlockTrace b;
	if (haveTrace)
		b = std::move(trace);
	else
	{
		// No compile of microVU's to take the root's first pass from: the
		// port's own, which the publication compares against microVU's when it
		// compiles the root (as the synchronous tier compared it here).
		vuopt::PipeConfig cfg;
		cfg.vuIndex = 1;
		cfg.flagHack = env.flagHack;
		cfg.xgkickHack = env.xgkickHack;
		cfg.ibitHack = env.ibitHack;
		vuopt::PipeAnalyzer an(micro.get(), cfg);
		vuopt::PipeState st;
		std::memcpy(st.bytes, &key, sizeof(st.bytes));
		an.Analyze(pc, st).ToTrace(b, 1);
		rootTrace = b;
	}

	ph.clear();
	for (u32 i = 0; i < kPhStubs; i++)
		ph.push_back({PhStub, i});
	vuopt::MvuEmitEnv e = env;
	e.searchBlock = &vuoptWSearch;
	e.addBlock = &vuoptWAdd;
	e.setBlockEntry = &vuoptWSetEntry;
	e.blockEntry = &vuoptWEntry;
	e.fetchBlock = &vuoptWFetch;
	e.addRange = &vuoptWRange;
	e.emitJumpTail = &vuoptWTail;
	// microVU's stubs live in the code cache, where the region will land: a
	// placeholder, patched to a direct branch. A C helper keeps its address and
	// gets armEmitCall's usual form from here (absolute when it is out of reach,
	// which from this buffer it nearly always is; a direct branch is recorded and
	// re-aimed, through a veneer if need be).
	const auto stub = [&](const void* real, u32 id) -> const void* {
		const u8* p = static_cast<const u8*>(real);
		return (p >= cacheLo && p < cacheHi) ? sink->Placeholder(id) : real;
	};
	e.bailTarget = reinterpret_cast<void* (*)(u32, u8*)>(const_cast<void*>(stub(reinterpret_cast<const void*>(env.bailTarget), kPhBailTarget)));
	e.clearLpState = stub(env.clearLpState, kPhClearLpState);
	e.endFlagsB = stub(env.endFlagsB, kPhEndFlagsB);
	e.exitEBit = stub(env.exitEBit, kPhExitEBit);
	e.xgkickFn = stub(env.xgkickFn, kPhXgkick);
	e.cycleBreak = env.cycleBreak ? stub(env.cycleBreak, kPhCycleBreak) : nullptr;

	sink->Begin();
	root = vuoptNewNode(*this, pc, &key);
	root->hasEntry = true;
	root->entryOff = 0;
	e.rootBlock = root;
	accepted = tier.OnCompile(b, e, &why);
	const u32 size = sink->End();
	vuopt::g_pipeReadMask = nullptr;
	t_vuoptCtx = nullptr;

	if (accepted && !sink->failed.empty())
	{
		accepted = false;
		why = "relocation: " + sink->failed;
		declined[why]++;
	}
	if (accepted)
	{
		code.assign(sink->Base(), sink->Base() + size);
		fixups = sink->fixups;
		regions = tier.Regions();
		regionNodes = tier.Nodes();
		regionExits = tier.Exits();
		regionBytes = tier.Bytes();
	}
	for (const auto& d : tier.Declined())
		declined[d.first] += d.second;
	workerIns = vuoptReadIns() - ins0;
	s_vuopt.workerIns.fetch_add(workerIns, std::memory_order_relaxed);
}

//------------------------------------------------------------------
// Making jobs (VU1 thread)
//------------------------------------------------------------------

static std::shared_ptr<VuoptJob> vuoptMakeJob(microVU& mVU, microProgram& prog, VuoptJob::Kind kind, u32 pc, const void* key)
{
	auto j = std::make_shared<VuoptJob>();
	j->epoch = s_vuopt.epoch;
	j->prog = &prog;
	j->gen = prog.vuopt ? prog.vuopt->gen : 0;
	j->kind = kind;
	j->pc = pc & 0x3ff8;
	std::memcpy(&j->key, key, sizeof(j->key));
	j->requestedAt = s_vuopt.dispatches;
	j->missesAtRequest = s_vuopt.jumpMisses;
	j->micro.reset(new u8[0x4000]);
	std::memcpy(j->micro.get(), mVU.regs().Micro, 0x4000);
	j->env = mVUbuildEmitEnv(mVU);
	j->env.program = &prog;
	j->env.flagHack = CHECK_VU_FLAGHACK;
	j->flagHack = CHECK_VU_FLAGHACK;
	j->cacheLo = mVU.cache;
	j->cacheHi = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	if (prog.vuopt)
		j->jumpTargets = prog.vuopt->jumpTargets;
	return j;
}

// The program's current graph, for a job whose region must link to what is
// there (a Root job): every block but `exclude` (the one microVU is compiling).
static void vuoptSnapshotGraph(microProgram& prog, const void* exclude, VuoptJob& j)
{
	for (u32 slot = 0; slot < mProgSize / 2; slot++)
	{
		microBlockManager* m = prog.block[slot];
		if (!m)
			continue;
		m->forEachInSearchOrder([&](microBlock& b) {
			if (&b == exclude)
				return;
			auto w = std::make_unique<VuoptWBlock>();
			w->pState = b.pState;
			w->pc = slot * 8;
			w->snapshot = true;
			w->entry = b.hostEntry;
			j.blocks.push_back(std::move(w));
		});
	}
}

static bool vuoptSubmit(const std::shared_ptr<VuoptJob>& j)
{
	if (!vuoptWorker().Submit(j))
	{
		s_vuopt.stats.jobsDropped++;
		return false;
	}
	(j->kind == VuoptJob::Generation ? s_vuopt.stats.jobsGen : s_vuopt.stats.jobsRoot)++;
	return true;
}

// Counters a finished job brings back, whatever becomes of it.
static void vuoptCollect(VuoptJob& j)
{
	s_vuopt.stats.jobsDone++;
	s_vuopt.stats.rootsCompiled++;
	if (!j.accepted)
		s_vuopt.stats.jobsDeclined++;
	for (const auto& d : j.declined)
		s_vuopt.stats.regionDeclined[d.first] += d.second;
}

//------------------------------------------------------------------
// Publishing (VU1 thread)
//------------------------------------------------------------------

// The program is loaded: micro memory holds its image over its ranges
// (mVUcmpProg without its side effects).
static bool vuoptProgramLoaded(microVU& mVU, const microProgram& prog)
{
	for (const auto& r : *prog.ranges)
		if (std::memcmp(reinterpret_cast<const u8*>(prog.data) + r.start, mVU.regs().Micro + r.start, r.end - r.start))
			return false;
	return true;
}

// Every micro-memory word the job's compile read is still what it read.
static bool vuoptMicroMatches(microVU& mVU, const VuoptJob& j, const u8* snapshot)
{
	const u32* live = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const u32* was = reinterpret_cast<const u32*>(snapshot);
	for (u32 w = 0; w < j.readMask.size(); w++)
		if (j.readMask[w] && live[w] != was[w])
			return false;
	return true;
}

// Room for the job's code in the code cache, as a compile session may use it
// (up to the cache's physical end; the reset comes after the session).
static bool vuoptHasRoom(microVU& mVU, const VuoptJob& j)
{
	const u8* const limit = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	return armGetCurrentCodePointer() + j.code.size() + (1u << 20) < limit;
}

// Places a job's code at the cursor of the open episode and wires it into
// the graph of mVU.prog.cur (the job's program): its nodes become the
// program's blocks for their keys (taking over the entry of a block already
// there), its ranges join the program's, its jump tails and exits are
// microVU's own code, compiled now, and every placeholder is patched to what
// it stands for. `rootReal`: the root's microBlock, when microVU is compiling
// the root and the code lands where microVU expects it.
static void vuoptPlace(microVU& mVU, VuoptJob& j, microBlock* rootReal)
{
	u8* const base = armGetCurrentCodePointer();
	if (rootReal)
		pxAssertRel(base == rootReal->hostEntry, "vuopt: a region placed away from its root's entry");
	{
		vixl::CodeBufferCheckScope scope(armAsm, j.code.size(), vixl::CodeBufferCheckScope::kReserveBufferSpace,
			vixl::CodeBufferCheckScope::kNoAssert);
		armAsm->GetBuffer()->EmitData(j.code.data(), j.code.size());
	}
	for (const vuopt::bg::Fixup& f : j.fixups)
		if (f.kind == vuopt::bg::Fixup::IntraAbs)
			pxAssertRel(vuopt::bg::PatchAbsMov(reinterpret_cast<u32*>(armGetWritableCodePtr(base + f.offset)), reinterpret_cast<u64>(base + f.value)),
				"vuopt: an address in a region is not where its fixup says");

	// The nodes, the root first, in the order the region registered them.
	for (VuoptWBlock* n : j.nodes)
	{
		microBlock* b = (n == j.root) ? rootReal : nullptr;
		if (!b)
		{
			blockCreate(n->pc / 8);
			b = mVUblocks[n->pc / 8]->search(mVU, &n->pState);
			if (!b)
			{
				alignas(16) microBlock t;
				std::memset(&t, 0, sizeof(t));
				t.pState = n->pState;
				b = mVUblocks[n->pc / 8]->add(mVU, &t);
			}
		}
		pxAssertRel(n->hasEntry, "vuopt: a region node without an entry");
		b->x86ptrStart = base + n->entryOff;
		b->hostEntry = base + n->entryOff;
		n->real = b;
	}
	for (const auto& r : j.ranges)
		vuoptAddRange(1, r.first, r.second);

	std::vector<const void*> target(j.ph.size(), nullptr);
	for (size_t id = 0; id < j.ph.size(); id++)
	{
		if (j.ph[id].first != VuoptJob::PhStub)
			continue;
		switch (j.ph[id].second)
		{
			case VuoptJob::kPhBailTarget: target[id] = reinterpret_cast<const void*>(j.env.bailTarget); break;
			case VuoptJob::kPhClearLpState: target[id] = j.env.clearLpState; break;
			case VuoptJob::kPhEndFlagsB: target[id] = j.env.endFlagsB; break;
			case VuoptJob::kPhExitEBit: target[id] = j.env.exitEBit; break;
			case VuoptJob::kPhXgkick: target[id] = j.env.xgkickFn; break;
			case VuoptJob::kPhCycleBreak: target[id] = j.env.cycleBreak; break;
		}
	}
	// microVU's dispatch for the JR/JALR nodes, then the exits, compiled now
	// in the order the region met them (as the synchronous tier compiled them
	// after the region).
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuoptJob::PhTail)
		{
			const VuoptJob::Tail& t = j.tails[j.ph[id].second];
			target[id] = armGetCurrentCodePointer();
			vuoptEmitJumpTail(1, t.blk->real, reinterpret_cast<const u8*>(&t.endKey));
		}
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuoptJob::PhExit)
		{
			VuoptJob::Exit& x = j.exits[j.ph[id].second];
			target[id] = mVUblockFetch(mVU, x.pc, reinterpret_cast<uptr>(&x.key));
		}

	// The branches to placeholders and to the outside world. A target out of
	// B/BL range (a C helper, from the code cache) goes through a veneer
	// placed after the code: x16 is the branch's scratch, as in armEmitCall's
	// long form.
	std::map<const void*, const u8*> veneers;
	for (const vuopt::bg::Fixup& f : j.fixups)
	{
		if (f.kind == vuopt::bg::Fixup::IntraAbs)
			continue;
		const void* to = (f.kind == vuopt::bg::Fixup::Site26) ? target[f.id] : reinterpret_cast<const void*>(f.value);
		pxAssertRel(to, "vuopt: a region branch with no target");
		u8* const at = base + f.offset;
		u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
		if (vuopt::bg::PatchRel26(insn, at, to))
			continue;
		auto it = veneers.find(to);
		if (it == veneers.end())
		{
			const u8* v = armGetCurrentCodePointer();
			armAsm->Mov(a64::x16, reinterpret_cast<uintptr_t>(to));
			armAsm->Br(a64::x16);
			it = veneers.emplace(to, v).first;
			s_vuopt.stats.veneers++;
		}
		pxAssertRel(vuopt::bg::PatchRel26(insn, at, it->second), "vuopt: a region branch out of reach of its veneer");
	}

	for (auto& b : j.blocks)
		if (!b->snapshot)
			s_vuopt.handles.push_back(std::move(b));
	j.blocks.clear();
	s_vuopt.stats.regions += j.regions;
	s_vuopt.stats.regionNodes += j.regionNodes;
	s_vuopt.stats.regionExits += j.regionExits;
	s_vuopt.stats.regionBytes += j.regionBytes;
}

// Moves the program's microVU graph out of the way (the graveyard): the next
// compile of any of its blocks starts from an empty graph.
static u32 vuoptSetAside(microVU& mVU, microProgram& prog, VuoptProgramState& st)
{
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
	return setAside;
}

// A generation: the program's graph is set aside and its root compiled again;
// that compile finds the job prepared and places the region (vuoptLiveRegionHook).
static void vuoptPublishGeneration(microVU& mVU, VuoptJob& j)
{
	microProgram& prog = *j.prog;
	VuoptProgramState& st = *prog.vuopt;
	const u32 setAside = vuoptSetAside(mVU, prog, st);
	st.gen++;
	st.tiers++;
	st.runsAtTier = vuoptTotalRuns(prog);
	st.checkBlock = vuoptTotalRuns(prog) >> kCheckShift;
	st.missesSeen = j.missesAtRequest;
	if (st.tiers > vuopt::live::kMaxRetiers)
		st.settled = true;
	if (j.retier)
		s_vuopt.stats.retiers++;
	else
		s_vuopt.stats.tierUps++;
	if (j.viaJump)
		s_vuopt.stats.jumpTierUps++;
	s_vuopt.stats.blocksSetAside += setAside;
	DevCon.WriteLn(Color_StrongBlue, "vuopt: %s prog[%03d] after %u runs: %u blocks set aside, %u jump targets over %zu jumps%s",
		j.retier ? "re-tier" : "tier-up", prog.idx, vuoptTotalRuns(prog), setAside, st.targetCount, st.jumpTargets.size(),
		j.accepted ? "" : " (region declined)");

	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	s_vuopt.prepared = &j;
	mVUblockFetch(mVU, j.pc, reinterpret_cast<uptr>(&j.key));
	if (s_vuopt.prepared == &j)
	{
		// (the root was compiled without asking the hook: nothing placed)
		s_vuopt.prepared = nullptr;
		s_vuopt.stats.discardTrace++;
	}
	mVU.prog.cur = savedCur;
}

// A region rooted at a block microVU compiled on its own while the job ran:
// placed in a code-cache episode of its own (so the persisted-program recorder
// never takes its bytes for microVU's), its nodes take over the entries of the
// blocks there, and the program's quick slots are dropped so every jump cache
// that knew the old entries asks again.
static void vuoptPublishRoot(microVU& mVU, VuoptJob& j)
{
	microProgram& prog = *j.prog;
	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	vuoptPlace(mVU, j, nullptr);
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	mVU.prog.cur = savedCur;
	for (u32 i = 0; i < mVU.progSize / 2; i++)
		if (mVU.prog.quick[i].prog == &prog)
		{
			mVU.prog.quick[i].prog = nullptr;
			mVU.prog.quick[i].block = nullptr;
		}
}

// Publishes what is ready for `prog`. `loaded`: the slow path has just resolved
// it from micro memory. Returns true when its graph changed.
static bool vuoptPublishReady(microVU& mVU, microProgram& prog, bool loaded)
{
	VuoptProgramState* const st = prog.vuopt;
	if (!st || st->ready.empty())
		return false;
	if (!loaded && !vuoptProgramLoaded(mVU, prog))
		return false;
	const u64 ins0 = vuoptReadIns();
	const microRegInfo savedLp = mVU.prog.lpState;
	std::vector<std::shared_ptr<VuoptJob>> ready;
	ready.swap(st->ready);
	bool changed = false;
	for (auto& jp : ready)
	{
		VuoptJob& j = *jp;
		const bool gen = j.kind == VuoptJob::Generation;
		const auto drop = [&](u64& counter) {
			counter++;
			if (gen)
			{
				st->genOutstanding = false;
				if (st->gen == 0)
					st->genAttempts++;
			}
		};
		if (j.gen != st->gen)
		{
			drop(s_vuopt.stats.discardGen);
			continue;
		}
		if (!vuoptMicroMatches(mVU, j, j.micro.get()))
		{
			drop(s_vuopt.stats.discardMemory);
			continue;
		}
		if (j.accepted && !vuoptHasRoom(mVU, j))
		{
			drop(s_vuopt.stats.discardSpace);
			continue;
		}
		const u64 lag = s_vuopt.dispatches - j.requestedAt;
		if (gen)
		{
			s_vuopt.stats.publishedGen++;
			s_vuopt.stats.lagGenSum += lag;
			s_vuopt.stats.lagGenMax = std::max(s_vuopt.stats.lagGenMax, lag);
			st->genOutstanding = false;
			vuoptPublishGeneration(mVU, j);
		}
		else
		{
			s_vuopt.stats.publishedRoot++;
			s_vuopt.stats.lagRootSum += lag;
			s_vuopt.stats.lagRootMax = std::max(s_vuopt.stats.lagRootMax, lag);
			vuoptPublishRoot(mVU, j);
		}
		changed = true;
	}
	// (the compiles above leave their keys in the dispatcher's carried state)
	mVU.prog.lpState = savedLp;
	s_vuopt.stats.publishIns += vuoptReadIns() - ins0;
	return changed;
}

// Collects the worker's finished jobs onto their programs, then publishes what
// it can: `cur`'s (resolved by the slow path) and any other program's whose
// image is in micro memory now. Returns true when `cur`'s graph changed.
static bool vuoptDrainAndPublish(microVU& mVU, microProgram* cur)
{
	std::vector<microProgram*> touched;
	if (s_vuopt.worker && s_vuopt.worker->CompletedCount())
	{
		std::vector<std::shared_ptr<vuopt::bg::Job>> done;
		s_vuopt.worker->TakeCompleted(done);
		for (auto& jb : done)
		{
			auto j = std::static_pointer_cast<VuoptJob>(jb);
			vuoptCollect(*j);
			if (j->epoch != s_vuopt.epoch || !j->prog->vuopt)
			{
				s_vuopt.stats.discardStale++;
				continue;
			}
			VuoptProgramState& st = *j->prog->vuopt;
			if (j->kind == VuoptJob::Root && !j->accepted)
				continue; // (nothing to publish; microVU's block stays)
			if (std::find(touched.begin(), touched.end(), j->prog) == touched.end())
				touched.push_back(j->prog);
			st.ready.push_back(std::move(j));
		}
	}
	bool changed = false;
	if (cur)
		changed = vuoptPublishReady(mVU, *cur, true);
	for (microProgram* p : touched)
		if (p != cur)
			vuoptPublishReady(mVU, *p, false);
	return changed;
}

// The entry for (startPC, key) in `prog`'s graph as it is now (compiled with
// the hooks if it is not there): where a slow path continues after a publication.
static void* vuoptRefetch(microVU& mVU, microProgram& prog, u32 startPC, const microRegInfo& key)
{
	alignas(16) microRegInfo k = key;
	void* const entry = mVUblockFetch(mVU, startPC, reinterpret_cast<uptr>(&k));
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	if (quick.prog == &prog)
		quick.block = prog.block[startPC / 8];
	return entry;
}

//------------------------------------------------------------------
// The dispatch path
//------------------------------------------------------------------

// mVUlookupProg_VU1 plus the run counter. A program reaches the tier-up
// threshold, a tiered one the periodic look for new jump targets, and a
// finished job its publication by returning null: the dispatcher then takes
// the full path (mVUexecuteVU1), which ends in mVUvuoptAfterSearch.
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
	s_vuopt.dispatches++;
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
	// The worker has finished something: one slow path collects it.
	if (s_vuopt.worker && s_vuopt.worker->CompletedCount())
	{
		s_vuopt.pending = true;
		return nullptr;
	}
	VuoptProgramState* const st = prog->vuopt;
	if (!st)
	{
		if (runs >= s_vuopt.tierUpRuns)
		{
			s_vuopt.pending = true;
			return nullptr;
		}
	}
	else if (!st->ready.empty())
	{
		s_vuopt.pending = true;
		return nullptr;
	}
	else if (st->gen == 0)
	{
		if (!st->genOutstanding && st->genAttempts < kMaxGenAttempts && vuoptTotalRuns(*prog) - st->requestRuns >= s_vuopt.tierUpRuns)
		{
			s_vuopt.pending = true;
			return nullptr;
		}
	}
	else if (!st->settled && !st->genOutstanding)
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

// Asks the worker for a new generation of the program's graph, rooted at the
// entry this slow path came through. In deterministic mode the job is waited
// for and published here, and the slow path continues in the new graph.
static void* vuoptRequestGeneration(microVU& mVU, microProgram& prog, VuoptProgramState& st, bool retier, bool viaJump,
	u32 startPC, const microRegInfo& key, void* result)
{
	auto j = vuoptMakeJob(mVU, prog, VuoptJob::Generation, startPC, &key);
	j->retier = retier;
	j->viaJump = viaJump;
	st.requestRuns = vuoptTotalRuns(prog);
	st.missesSeen = s_vuopt.jumpMisses;
	// Root jobs for the program still waiting in the queue would build into the
	// graph this generation replaces: drop them.
	if (s_vuopt.worker)
		s_vuopt.worker->RemoveQueued([&](const vuopt::bg::Job& b) {
			const VuoptJob& q = static_cast<const VuoptJob&>(b);
			const bool drop = q.prog == &prog && q.kind == VuoptJob::Root;
			s_vuopt.stats.jobsCoalesced += drop;
			return drop;
		});
	if (!vuoptSubmit(j))
	{
		if (st.gen == 0)
			st.genAttempts++;
		return result;
	}
	st.genOutstanding = true;
	if (!s_vuopt.synchronous)
		return result;
	vuoptWorker().Wait(j);
	if (vuoptDrainAndPublish(mVU, &prog))
		return vuoptRefetch(mVU, prog, startPC, key);
	return result;
}

// The decision both slow paths share. `prog` (mVU.prog.cur) has been resolved and its
// run counters bumped; `result` is the entry the search found for `startPC`. An untiered
// program asks for its tier-up when `tierNow` (the caller's threshold test); a tiered one
// looks for new jump targets when it has met some.
static void* vuoptConsider(microVU& mVU, microProgram& prog, u32 startPC, const microRegInfo& key, void* result, bool tierNow, bool viaJump)
{
	VuoptProgramState* st = prog.vuopt;
	if (!st || st->gen == 0)
	{
		if (st && (st->genOutstanding || st->genAttempts >= kMaxGenAttempts || vuoptTotalRuns(prog) - st->requestRuns < s_vuopt.tierUpRuns))
			return result;
		if (!tierNow)
			return result;
		if (!st)
			st = prog.vuopt = new VuoptProgramState();
		// Targets first: the job takes a copy of what the jump caches know now.
		vuoptHarvestTargets(prog, *st);
		return vuoptRequestGeneration(mVU, prog, *st, false, viaJump, startPC, key, result);
	}
	if (st->genOutstanding || st->settled || vuoptTotalRuns(prog) - st->runsAtTier < vuopt::live::kRetierMinRuns ||
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
	return vuoptRequestGeneration(mVU, prog, *st, true, viaJump, startPC, key, result);
}

// The dispatcher's slow path: collects and publishes what the worker made, and
// is where a program asks for its tier-up. mVU.prog.cur is the program the
// search resolved and `result` its entry for `startPC`.
void* mVUvuoptAfterSearch(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuopt.enabled || mVU.index != 1)
		return result;
	// The counting lookup already counted a dispatch that asked for this.
	const bool counted = s_vuopt.pending;
	s_vuopt.pending = false;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
	{
		vuoptDrainAndPublish(mVU, nullptr);
		return result;
	}
	if (!counted)
		prog->vuoptRuns++;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	if (vuoptDrainAndPublish(mVU, prog))
		result = vuoptRefetch(mVU, *prog, startPC, key);
	return vuoptConsider(mVU, *prog, startPC, key, result, prog->vuoptRuns >= s_vuopt.tierUpRuns, false);
}

// The same for a JR/JALR the jump cache missed (mVUcompileJIT), after the search resolved
// its target to a program. Counts the entry on that program, and asks for its tier-up when
// jumps are how it is reached: a program the dispatcher never enters (an overlay a stub
// program jumps into) has no dispatch count to go by. A program the dispatcher does enter
// keeps its dispatch threshold: tiering it from a jump into the middle of it would root its
// first compile there, which splits what a compile from the program's entry keeps in one region.
void* mVUvuoptAfterJump(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuopt.enabled || mVU.index != 1)
		return result;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
	{
		vuoptDrainAndPublish(mVU, nullptr);
		return result;
	}
	const u32 jumps = ++prog->vuoptJumpRuns;
	s_vuopt.stats.jumpCounted++;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	if (vuoptDrainAndPublish(mVU, prog))
		result = vuoptRefetch(mVU, *prog, startPC, key);
	bool tierNow = false;
	VuoptProgramState* const st = prog->vuopt;
	if (st && st->gen > 0)
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
	return vuoptConsider(mVU, *prog, startPC, key, result, tierNow, true);
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

// The tier's counters, once, when VU1 shuts down; the worker stops with it.
void mVUvuoptOnClose(microVU& mVU)
{
	if (mVU.index != 1)
		return;
	if (s_vuopt.stats.tierUps || s_vuopt.stats.jobsGen)
		Console.WriteLn(Color_StrongBlue, "%s", vuopt::live::FormatStats().c_str());
	s_vuopt.worker.reset();
}

void mVUvuoptOnProgramDeleted(microProgram& prog)
{
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
		if (s_vuopt.compileDepth == 0 && vuoptCountingIns())
		{
			s_vuoptSessionIns0 = mvu_test_hooks::ReadThreadInstructions();
			s_vuoptSessionTiered = mVU.prog.cur && mVU.prog.cur->vuopt && mVU.prog.cur->vuopt->gen > 0;
		}
#endif
		s_vuopt.compileDepth++;
	}
}

void mVUvuoptCompileEnd(microVU& mVU)
{
	if (mVU.index != 1 || !s_vuopt.enabled || s_vuopt.compileDepth <= 0)
		return;
	if (--s_vuopt.compileDepth < s_vuopt.plainRootDepth)
		s_vuopt.plainRootDepth = 0;
	if (s_vuopt.compileDepth == 0)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (vuoptCountingIns())
		{
			const u64 d = mvu_test_hooks::ReadThreadInstructions() - s_vuoptSessionIns0;
			(s_vuoptSessionTiered ? s_vuopt.stats.compileInsTiered : s_vuopt.stats.compileInsPlain) += d;
		}
#endif
		if (s_vuopt.body)
			s_vuopt.body->ResetAnalyzer();
	}
}

// The block being compiled belongs to a region a job is building: microVU
// compiles it as its own.
static bool vuoptUnderPlainRoot() { return s_vuopt.plainRootDepth && s_vuopt.compileDepth > s_vuopt.plainRootDepth; }

static bool vuoptLiveRegionHook(const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void* user)
{
	microVU& mVU = microVU1;
	VuoptProgramState& st = *static_cast<VuoptProgramState*>(user);
	(void)st;
	// A generation being published: its root's compile places it.
	if (VuoptJob* j = s_vuopt.prepared)
	{
		if (j->prog == mVU.prog.cur && b.startPC == j->pc && std::memcmp(b.entryState, &j->key, sizeof(j->key)) == 0)
		{
			s_vuopt.prepared = nullptr;
			if (!j->accepted)
				return false;
			// microVU's own first pass of the root against the port's, the check the
			// synchronous tier made on every root.
			if (!vuopt::CompareBlockTraces(b, j->rootTrace).empty())
			{
				s_vuopt.stats.discardTrace++;
				s_vuopt.stats.regionDeclined["timing port disagrees"]++;
				return false;
			}
			vuoptPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
			return true;
		}
	}
	if (vuoptUnderPlainRoot())
		return false;

	// A block microVU meets in a program already tiered up: a region rooted at
	// it goes to the worker, built against the program's graph as it is now.
	auto j = vuoptMakeJob(mVU, *mVU.prog.cur, VuoptJob::Root, b.startPC, b.entryState);
	j->haveTrace = true;
	j->trace = b;
	vuoptSnapshotGraph(*mVU.prog.cur, env.rootBlock, *j);
	if (s_vuopt.synchronous)
	{
		if (!vuoptSubmit(j))
			return false;
		vuoptWorker().Wait(j);
		// (taken out of the worker's finished list here, not by a slow path)
		std::vector<std::shared_ptr<vuopt::bg::Job>> done;
		s_vuopt.worker->TakeCompleted(done);
		vuoptCollect(*j);
		if (!j->accepted)
			return false;
		s_vuopt.stats.publishedRoot++;
		vuoptPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
		return true;
	}
	vuoptSubmit(j);
	// microVU compiles the block, and what it reaches from it, as its own for now.
	s_vuopt.plainRootDepth = s_vuopt.compileDepth;
	return false;
}

static bool vuoptLiveBodyHook(const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void*)
{
	if (vuoptUnderPlainRoot())
		return false;
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
	if (!cur || !cur->vuopt || cur->vuopt->gen == 0)
		return;
	region = &vuoptLiveRegionHook;
	regionUser = cur->vuopt;
	body = &vuoptLiveBodyHook;
	bodyUser = cur->vuopt;
}

#ifdef PCSX2_RECOMPILER_TESTS
namespace vuopt::live
{
	// pcsx2-vurunner --vuopt-region-bg: the region hook through the worker. A
	// job is made from the block microVU is compiling and the graph around it,
	// run on the worker and waited for, and its code copied in where microVU
	// is compiling: the synchronous region, by way of everything the live
	// tier's background path does to it.
	bool RegionHookViaWorker(const MvuBlockTrace& b, const MvuEmitEnv& env, void* user)
	{
		microVU& mVU = microVU1;
		if (b.vuIndex != 1 || !mVU.prog.cur)
			return false;
		const RegionTier& tmpl = *static_cast<const RegionTier*>(user);
		auto j = vuoptMakeJob(mVU, *mVU.prog.cur, VuoptJob::Root, b.startPC, b.entryState);
		j->gen = 0;
		j->haveTrace = true;
		j->trace = b;
		j->env = env;
		j->opts.From(tmpl);
		j->jumpTargets = tmpl.jumpTargets;
		vuoptSnapshotGraph(*mVU.prog.cur, env.rootBlock, *j);
		if (!vuoptSubmit(j))
			return false;
		vuoptWorker().Wait(j);
		std::vector<std::shared_ptr<vuopt::bg::Job>> done;
		s_vuopt.worker->TakeCompleted(done);
		vuoptCollect(*j);
		if (!j->accepted)
			return false;
		vuoptPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
		return true;
	}
} // namespace vuopt::live
#endif
