// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vu-aot live tier: tier-up policy and the glue between microVU's programs and
// blocks and vu-aot's regions. Included at the end of microVU-arm64.cpp (it
// needs microVU's TU-local types and helpers); the policy constants and the
// part of the interface other code may use are in vuaot/VuAotLive.h, the
// design in compiler-design.md sections 9x (the tier) and 9aa (the background
// compile).
//
// Threads. Everything here runs on the thread that executes VU1 (the EE thread,
// or the MTVU thread), except VuAotJob::Run and what it calls, which run on the
// tier's worker thread (vuaot::bg::Worker). A job holds copies of everything it
// reads (micro memory, the program's block keys and entries, the jump targets,
// microVU's addresses as numbers) and writes only its own memory, so the two
// threads share nothing but the worker's queue. What a job made is published
// on the VU1 thread at a slow path microVU already takes (a dispatch that
// missed the quick path, or a JR/JALR the jump cache missed), after checking
// that the program and every micro-memory word the compile read are still what
// they were. A reset (settings change, savestate load, cache exhaustion)
// reaches this file from a thread that has waited for VU1 to go idle; it bumps
// the epoch, and whatever a job made before it is dropped unread.

// (The vu-aot headers are included at the top of microVU-arm64.cpp: they cannot
// follow microVU's macros.)

#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

//------------------------------------------------------------------
// Per-program state
//------------------------------------------------------------------

static u32 vuAotTotalRuns(const microProgram& p) { return p.vuAotRuns + p.vuAotJumpRuns; }

// vuaot::live::kRetierCheckMask + 1 as a shift
static constexpr u32 kCheckShift = 10;
static_assert(vuaot::live::kRetierCheckMask + 1 == (1u << kCheckShift), "kCheckShift follows kRetierCheckMask");

// A first tier-up whose job is dropped (queue full) or discarded (micro memory
// changed under it) is asked for again after another kTierUpRuns runs, this
// many times at most.
static constexpr u32 kMaxGenAttempts = 4;

struct VuAotJob;

// What the tier keeps for a program it has started to tier up (microProgram::vuaot).
// `gen` says whether vu-aot's code is in place: 0 while microVU's own graph still
// runs (the first generation's job is queued or running), then one more for every
// published generation.
struct VuAotProgramState
{
	// Compiles of this program's graph under the tier: 1 after the tier-up,
	// one more per re-tier.
	u32 tiers = 0;
	// vuAotTotalRuns when the program was last compiled under the tier.
	u32 runsAtTier = 0;
	// The tier's jump-miss count when it last looked for new targets.
	u64 missesSeen = 0;
	// Looks for new targets that found none, in a row; at vuaot::live::kSettleChecks
	// the program is settled and no longer looks.
	u32 emptyChecks = 0;
	bool settled = false;
	// The program's runs (dispatches and JR slow-path entries, vuAotTotalRuns) in units of
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
	// vuAotTotalRuns at the last generation request.
	u32 requestRuns = 0;
	// Finished jobs waiting to be published, in completion order.
	std::vector<std::shared_ptr<VuAotJob>> ready;
	// Blocks microVU compiled as its own while a root job for them was out (the
	// root and everything its compile reached). A job's copy of the graph leaves
	// them out, so a region grows over them as the synchronous tier's would have
	// (there they would not exist), and the region that holds one takes over its
	// entry. A block stays here until a region does.
	std::unordered_set<const microBlock*> provisional;

	// The program cache's sidecar (persistence-design.md, the plan half; vuaot/VuAotPersist.h).
	// The plans of the jobs published into the current generation, in publication order (the
	// generation first): a new generation starts the list again. Kept only while the cache is on.
	std::vector<std::shared_ptr<const vuaot::persist::Record>> published;
	// `published` changed since the program's sidecar was last handed to the writer.
	bool dirty = false;
	// vuAotTotalRuns at the last publication: a program is saved once it has run this many
	// runs and more without one (kSaveQuietRuns), or at the next reset.
	u32 publishRuns = 0;
	// XXH3 of the sidecar the program was loaded with (0: none); an identical plan is not written again.
	u64 loadedChecksum = 0;
	// From a loaded sidecar: the root plans, sent to the worker when the generation is published.
	std::vector<vuaot::persist::Record> planRoots;
	// Root jobs sent from planRoots, into generation `planGen`. When microVU compiles one of their
	// roots first, the region hook keeps its first pass here (instead of asking for a second job),
	// and the publication compares it with the port's, as a generation's publication does.
	struct PlanRoot
	{
		u32 pc = 0;
		alignas(16) microRegInfo key;
		const VuAotJob* job = nullptr;
		vuaot::persist::Record rec; // (saved again while the job is out)
		bool haveTrace = false;
		vuaot::MvuBlockTrace trace;
	};
	std::vector<std::unique_ptr<PlanRoot>> planOut;
	u32 planGen = 0;
	// The generation came from a loaded plan (the time-to-first-region figures are split by it).
	bool fromPlan = false;
	// A region of the program has been placed (the time-to-first-region figures count the first).
	bool hadRegion = false;
};

// A block as a job sees it. The pipeline state comes first, where microBlock
// keeps it: a region's bail records hold the node's block pointer, and the bail
// (vuAotBailTarget) reads only the state through it, so the node's handle can
// stand in for the microBlock the worker cannot touch. Handles live until the
// next reset, like the code that points at them.
struct VuAotWBlock
{
	alignas(16) microRegInfo pState;
	u32 pc = 0;
	// A block of the program's graph when the job was made: its entry.
	bool snapshot = false;
	void* entry = nullptr;
	// A node of the job's region: its entry, as an offset into the job's code.
	bool hasEntry = false;
	u32 entryOff = 0;
	// A node an earlier job of the same program and graph built (claimed) and
	// may have published by the time this one is: a link to it is aimed at
	// publication, at whatever the program's graph holds for its key then.
	bool pending = false;
	s32 phId = -1;
	// The microBlock it became at publication.
	microBlock* real = nullptr;
};
static_assert(offsetof(VuAotWBlock, pState) == 0 && offsetof(microBlock, pState) == 0,
	"bail records read the pipeline state at the start of the block they point at");

// The options of the region builder a job runs (the live tier uses the
// defaults; pcsx2-vurunner's background arm copies its own).
struct VuAotRegionOptions
{
	s32 maxVHomes = 14, maxGHomes = 6;
	bool schedule = true, splitHomes = true, stickyAcc = true, accAlways = false, handoffLiveness = true, dump = false;
	bool dropSticky = true;
	int dropStickyMutation = 0;
	vuaot::RegionTier::HandoffMutation handoffMutation = vuaot::RegionTier::HandoffMutation::None;

	void From(const vuaot::RegionTier& t)
	{
		maxVHomes = t.maxVHomes;
		maxGHomes = t.maxGHomes;
		schedule = t.schedule;
		splitHomes = t.splitHomes;
		stickyAcc = t.stickyAcc;
		accAlways = t.accAlways;
		handoffLiveness = t.handoffLiveness;
		handoffMutation = t.handoffMutation;
		dropSticky = t.dropSticky;
		dropStickyMutation = t.dropStickyMutation;
		dump = t.dump;
	}
	void To(vuaot::RegionTier& t) const
	{
		t.maxVHomes = maxVHomes;
		t.maxGHomes = maxGHomes;
		t.schedule = schedule;
		t.splitHomes = splitHomes;
		t.stickyAcc = stickyAcc;
		t.accAlways = accAlways;
		t.handoffLiveness = handoffLiveness;
		t.handoffMutation = handoffMutation;
		t.dropSticky = dropSticky;
		t.dropStickyMutation = dropStickyMutation;
		t.dump = dump;
	}
};

// One region to build on the worker: rooted at the entry the tier-up came
// through (Generation: a new graph for the program, published by setting the
// old one aside), or at a block microVU met in a program already tiered up
// (Root: published into the program's current graph).
struct VuAotJob final : vuaot::bg::Job
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
		PhPending,
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
	// Sent from a loaded sidecar's plan (a generation at load, a root once the generation is in).
	bool fromPlan = false;

	// Inputs.
	std::unique_ptr<u8[]> micro; // micro memory when the job was made (kept: publication compares against it)
	std::map<u32, std::vector<u32>> jumpTargets;
	bool haveTrace = false;
	vuaot::MvuBlockTrace trace; // microVU's first pass of the root (Root jobs)
	vuaot::MvuEmitEnv env;      // microVU's values; the worker swaps in its own callbacks
	bool flagHack = false;      // block search follows it (microBlockManager::search)
	const u8* cacheLo = nullptr; // microVU's code cache, where the region lands
	const u8* cacheHi = nullptr;
	VuAotRegionOptions opts;
	std::vector<std::unique_ptr<VuAotWBlock>> blocks; // the snapshot, then the job's nodes

	// Outputs.
	bool accepted = false;
	std::string why;
	vuaot::MvuBlockTrace rootTrace; // the port's first pass of the root (Generation jobs)
	std::vector<u8> code;
	std::vector<vuaot::bg::Fixup> fixups;
	VuAotWBlock* root = nullptr;
	std::vector<VuAotWBlock*> nodes; // registration order, the root first
	std::vector<std::pair<u32, u32>> ranges;
	struct Exit
	{
		u32 pc;
		alignas(16) microRegInfo key;
	};
	std::vector<Exit> exits;
	struct Tail
	{
		VuAotWBlock* blk;
		alignas(16) microRegInfo endKey;
	};
	std::vector<Tail> tails;
	std::vector<VuAotWBlock*> pendingRefs; // PhPending placeholders
	std::vector<std::pair<PhKind, u32>> ph; // per placeholder id
	std::vector<u8> readMask;               // per micro-memory word: the compile read it
	u32 regions = 0, regionNodes = 0, regionExits = 0;
	u64 regionBytes = 0;
	std::map<std::string, u32> declined;
	// The flag-reader census of the compile (9ab), and whether it dropped the sticky STATUS
	// byte (relaxation 3: decided by scanning the job's copy of micro memory; publication
	// scans live micro memory again and discards the job if a sticky reader has appeared).
	u32 statusReaders = 0, stickyReaders = 0, stickyDivReaders = 0, stickyDropped = 0, stickyImages = 0;
	u64 workerIns = 0;

	void Run() override;
};

namespace
{
	struct VuAotLive
	{
		// The tier runs this session.
		bool enabled = false;
		// The counting lookup asked for the full search (set by the fast path,
		// consumed by mVUvuAotAfterSearch on the same dispatch).
		bool pending = false;
		u32 tierUpRuns = vuaot::live::kTierUpRuns;
		// ... and for a run counted on the JR/JALR slow path.
		u32 jumpTierUpRuns = vuaot::live::kJumpTierUpRuns;
		// JR/JALR the jump cache missed, VU1.
		u64 jumpMisses = 0;
		// Nesting of mVUcompile for VU1.
		int compileDepth = 0;
		std::unique_ptr<vuaot::BodyTier> body;
		vuaot::live::Stats stats;

		// Section 9aa.
		// Deterministic mode (tests, pcsx2-eerunner --vuaot-sync): every job is
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
		VuAotJob* prepared = nullptr;
		std::unique_ptr<vuaot::bg::Worker> worker;
		// Node handles of placed regions (VuAotWBlock).
		std::vector<std::unique_ptr<VuAotWBlock>> handles;
		std::atomic<u64> workerIns{0};
	};
	// Never destroyed: recMicroVU1's own destructor can reach mVUclose (and so
	// mVUvuAotOnClose) after this translation unit's statics are gone.
	VuAotLive& s_vuaot = *new VuAotLive();

	// Section 9ad, owner decision j: relaxation 3 (regions drop the sticky byte of STATUS) holds
	// in every flag-hack mode while no sticky reader is known. This is the "known" part. It lives
	// apart from VuAotLive because a reset (settings change, savestate load, cache exhaustion)
	// rebuilds that one and must not forget what a game has already shown: only a VM reset or boot
	// clears it (vuaot::live::ResetStickyTripwire). Atomics, because the writers are on three
	// threads: microVU's compiles and the tier's publications run on the thread that executes VU1,
	// and a VU0 access to VU1's STATUS runs on the EE thread (after MTVU has gone idle).
	struct VuAotTripwire
	{
		std::atomic<bool> tripped{false};
		// Regions built without the sticky byte may exist and must go: VU1 resets at its next run.
		std::atomic<bool> rebuildDue{false};
		// A region built without the sticky byte has been placed since VU1's last reset.
		std::atomic<bool> dropLive{false};
		std::atomic<u32> cause{0};
		std::atomic<u64> trips{0}, resets{0};
		std::atomic<u64> tripsBy[vuaot::live::kTripCauses] = {};
		// Test only: a mask of vuaot::live::TripMask(TripMutation).
#ifdef PCSX2_RECOMPILER_TESTS
		std::atomic<int> mutation{0};
#endif
	};
	VuAotTripwire& s_trip = *new VuAotTripwire();

	// The tripwire's memory across sessions: the titles that set it off are lines ("SERIAL CRC") of
	// vu_jit/vuaot_tripped.txt under the cache folder, read when a session starts or the title changes and
	// appended to at a trip. The running title is kept here. Any thread (a trip can come from VU0 on the
	// EE thread); small and rare, so one mutex.
	struct VuAotTitles
	{
		std::mutex mu;
		std::string title; // "SERIAL CRC" of the running title; empty when there is none
	};
	VuAotTitles& s_titles = *new VuAotTitles();
} // namespace

static std::string vuAotTrippedPath()
{
	if (EmuFolders::Cache.empty())
		return {};
	return Path::Combine(Path::Combine(EmuFolders::Cache, "vu_jit"), "vuaot_tripped.txt");
}

// (s_titles.mu held) Whether the file at `path` has the running title.
static bool vuAotTitleInFileLocked(const std::string& path)
{
	const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	if (!text.has_value())
		return false;
	size_t at = 0;
	while (at < text->size())
	{
		size_t end = text->find('\n', at);
		if (end == std::string::npos)
			end = text->size();
		if (text->compare(at, end - at, s_titles.title) == 0)
			return true;
		at = end + 1;
	}
	return false;
}

// Whether the running title set the tripwire off in an earlier session (only with the program cache on:
// the memory lives in its folder).
static bool vuAotTitleRemembered()
{
	std::lock_guard<std::mutex> lock(s_titles.mu);
	const std::string path = vuAotTrippedPath();
	if (s_titles.title.empty() || path.empty() || !EmuConfig.Cpu.Recompiler.UseVUProgramCache())
		return false;
	return vuAotTitleInFileLocked(path);
}

// The running title has set the tripwire off: remember it.
static void vuAotRememberTitle()
{
	std::lock_guard<std::mutex> lock(s_titles.mu);
	const std::string path = vuAotTrippedPath();
	if (s_titles.title.empty() || path.empty() || !EmuConfig.Cpu.Recompiler.UseVUProgramCache() || vuAotTitleInFileLocked(path))
		return;
	Error error;
	const std::string line = s_titles.title + "\n";
	if (!FileSystem::EnsureDirectoryExists(std::string(Path::GetDirectory(path)).c_str(), true, &error))
		return;
	if (std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "ab"))
	{
		std::fwrite(line.data(), 1, line.size(), fp);
		std::fclose(fp);
	}
}

// The tripwire set because the running title set it off before: as a trip (regions keep the sticky
// byte from now on; any placed without it are rebuilt), but neither counted as one nor remembered again.
static void vuAotTripRemembered()
{
	if (s_trip.tripped.exchange(true, std::memory_order_acq_rel))
		return;
	s_trip.cause.store(static_cast<u32>(vuaot::live::TripCause::Remembered), std::memory_order_relaxed);
	s_trip.tripsBy[static_cast<u32>(vuaot::live::TripCause::Remembered)].fetch_add(1, std::memory_order_relaxed);
	if (s_trip.dropLive.load(std::memory_order_acquire))
		s_trip.rebuildDue.store(true, std::memory_order_release);
	Console.WriteLn(Color_StrongBlue, "vu-aot: sticky STATUS tripwire set: this title set it off in an earlier session.");
}

#ifdef PCSX2_RECOMPILER_TESTS
// vuaot::live::SetWorkerDelayUs
static std::atomic<u32> s_vuAotWorkerDelayUs{0};
#endif

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
static bool vuAotCountingIns() { return mvu_test_hooks::g_vu1InsCount.load(std::memory_order_relaxed); }
static u64 vuAotReadIns() { return vuAotCountingIns() ? mvu_test_hooks::ReadThreadInstructions() : 0; }
#else
static u64 vuAotReadIns() { return 0; }
#endif

//------------------------------------------------------------------
// Config
//------------------------------------------------------------------

static vuaot::bg::Worker& vuAotWorker()
{
	if (!s_vuaot.worker)
		s_vuaot.worker = std::make_unique<vuaot::bg::Worker>();
	s_vuaot.worker->Start();
	return *s_vuaot.worker;
}

const char* vuaot::live::TripCauseName(TripCause c)
{
	switch (c)
	{
		case TripCause::None: return "none";
		case TripCause::ProgramImage: return "a micro image with a sticky STATUS reader was loaded";
		case TripCause::Compile: return "microVU compiled a sticky STATUS reader";
		case TripCause::Job: return "a sticky STATUS reader is in the micro memory a region would run under";
		case TripCause::OutsideRead: return "VU0 read VU1's STATUS register";
		case TripCause::Forced: return "forced";
		case TripCause::Remembered: return "this title set it off in an earlier session";
		default: return "?";
	}
}

#ifdef PCSX2_RECOMPILER_TESTS
static bool vuAotMutated(int m) { return (s_trip.mutation.load(std::memory_order_relaxed) >> m) & 1; }
#else
// (shipped builds: the tripwire's mutation checks fold away)
static constexpr bool vuAotMutated(int) { return false; }
#endif

// The first reader of the sticky bits that anything has shown: from here on regions keep the byte,
// and the ones built without it are rebuilt. Any thread.
static void vuAotTrip(vuaot::live::TripCause why)
{
	// (with the tier off there are no regions, and the offline runners' region tiers have no
	// session to trip)
	if (!s_vuaot.enabled || s_trip.tripped.exchange(true, std::memory_order_acq_rel))
		return;
	s_trip.cause.store(static_cast<u32>(why), std::memory_order_relaxed);
	s_trip.trips.fetch_add(1, std::memory_order_relaxed);
	s_trip.tripsBy[static_cast<u32>(why)].fetch_add(1, std::memory_order_relaxed);
	const bool rebuild = s_trip.dropLive.load(std::memory_order_acquire) && !vuAotMutated(vuaot::live::kTripMutNoRebuild);
	if (rebuild)
		s_trip.rebuildDue.store(true, std::memory_order_release);
	Console.WriteLn(Color_StrongBlue, "vu-aot: sticky STATUS tripwire: %s. Regions keep the sticky byte of STATUS from now on%s.",
		vuaot::live::TripCauseName(why), rebuild ? "; VU1 resets at its next run to rebuild the regions that dropped it" : "");
	vuAotRememberTitle();
}

// A VU0 access to VU1's STATUS register (the VU0 interpreter's GET_VU_MEM, microVU0's window seam).
void vuAotNoteOutsideStatusRead()
{
	if (vuAotMutated(vuaot::live::kTripMutOutside))
		return;
	vuAotTrip(vuaot::live::TripCause::OutsideRead);
}

// microVU's analysis of an FSAND, FSEQ or FSOR that reads a sticky bit (bits 6-11 of the
// immediate): the code is about to be compiled, so it can run.
void mVUvuAotOnStatusReader(microVU& mVU, u32 imm12)
{
	if (mVU.index != 1 || !(imm12 & 0xFC0) || !s_vuaot.enabled || vuAotMutated(vuaot::live::kTripMutCompile))
		return;
	vuAotTrip(vuaot::live::TripCause::Compile);
}

// A program is created (or loaded from the disk cache) for the micro image now in memory: any
// reader of the sticky bits in it, executed or not, sets the tripwire off. Once per program, at a
// path that is about to compile it, so the scan of the 16 KB image is not measurable.
void mVUvuAotOnNewProgram(microVU& mVU)
{
	if (mVU.index != 1 || !s_vuaot.enabled || s_trip.tripped.load(std::memory_order_relaxed) ||
		vuAotMutated(vuaot::live::kTripMutProgramImage))
		return;
	if (vuaot::RegionTier::MicroReadsSticky(mVU.regs().Micro, 1))
		vuAotTrip(vuaot::live::TripCause::ProgramImage);
}

// VU1's run entry. The tripwire has gone off and regions without the sticky byte exist: a reset
// throws away every program and region, and the next compiles are exact. Not while a program is
// parked at a cycle-budget stop (a reset would drop the pipeline state it resumes from); the next
// run that starts a program finds it due again.
void mVUvuAotRebuildIfDue()
{
	if (!s_trip.rebuildDue.load(std::memory_order_acquire) || microVU1.resumeEntry)
		return;
	s_trip.resets.fetch_add(1, std::memory_order_relaxed);
	Console.WriteLn(Color_StrongBlue, "vu-aot: resetting VU1: the regions that dropped the sticky byte of STATUS are rebuilt");
	mVUreset(microVU1, false, /*vu1Idle=*/true);
}

void vuAotResetStickyTripwire() { vuaot::live::ResetStickyTripwire(); }
void vuAotSetTitle(const std::string& serial, u32 crc) { vuaot::live::SetTitle(serial, crc); }

namespace vuaot::live
{
	bool StickyTripped() { return s_trip.tripped.load(std::memory_order_acquire); }
	TripCause StickyTripCause() { return static_cast<TripCause>(s_trip.cause.load(std::memory_order_relaxed)); }
	void ResetStickyTripwire()
	{
		s_trip.tripped.store(false, std::memory_order_release);
		s_trip.cause.store(0, std::memory_order_relaxed);
		// A new session of a title that set it off before starts with it set.
		if (vuAotTitleRemembered())
		{
			s_trip.tripped.store(true, std::memory_order_release);
			s_trip.cause.store(static_cast<u32>(TripCause::Remembered), std::memory_order_relaxed);
			s_trip.tripsBy[static_cast<u32>(TripCause::Remembered)].fetch_add(1, std::memory_order_relaxed);
		}
	}
	void SetTitle(const std::string& serial, u32 crc)
	{
		{
			std::lock_guard<std::mutex> lock(s_titles.mu);
			s_titles.title.clear();
			if (!serial.empty())
				s_titles.title = fmt::format("{} {:08X}", serial, crc);
		}
		// (a title changed in mid-session, which a boot or reset does not come after)
		if (s_vuaot.enabled && vuAotTitleRemembered())
			vuAotTripRemembered();
	}
#ifdef PCSX2_RECOMPILER_TESTS
	void TripStickyForTest() { vuAotTrip(TripCause::Forced); }
	void SetTripwireMutation(int mask) { s_trip.mutation.store(mask, std::memory_order_relaxed); }
#endif
} // namespace vuaot::live

// Whether the config asks for the tier and allows it: the setting, VU1 recompiled, and none of the
// gamefixes or the clamp mode vu-aot does not model (it declines every block under them, see
// RegionTier::OnCompile, so tiering a program up would only set its microVU code aside).
static bool vuAotWantedByConfig()
{
	const Pcsx2Config::RecompilerOptions& rc = EmuConfig.Cpu.Recompiler;
	const bool supported = !CHECK_VU_EXACT(1) && !CHECK_XGKICKHACK && !EmuConfig.Gamefixes.IbitHack &&
		!CHECK_VUOVERFLOWHACK && !CHECK_VUADDSUBHACK && !EmuConfig.Gamefixes.VUSyncHack &&
		!EmuConfig.Gamefixes.FullVU0SyncHack;
	return rc.EnableVUAot && rc.EnableVU1 && supported;
}

// microVU0 emits the tripwire's window hook (mVUaddrFix) only while the tier is wanted, so with the
// setting off VU0's code is what it was before vu-aot. Latched at VU0's own init and reset, which a
// change of any of the settings above always goes through (VMManager::CheckForCPUConfigChanges resets
// both VUs), and which also builds VU0's options sentinel from it: a persisted VU0 program with the
// hook is never loaded into a session without it, or the other way round.
static bool s_vu0WindowHook = false;
bool mVUvuAotVU0WindowHook() { return s_vu0WindowHook; }

void mVUvuAotSyncConfig(microVU& mVU)
{
	if (mVU.index != 1)
	{
		s_vu0WindowHook = vuAotWantedByConfig();
		return;
	}
	// (this reset throws away every region there was; the next ones are built under the tripwire)
	s_trip.rebuildDue.store(false, std::memory_order_release);
	s_trip.dropLive.store(false, std::memory_order_release);

	const Pcsx2Config::RecompilerOptions& rc = EmuConfig.Cpu.Recompiler;
	const bool want = vuAotWantedByConfig();

	if (want != s_vuaot.enabled)
		DevCon.WriteLn(Color_StrongBlue, "vu-aot: live tier %s%s", want ? "ENABLED" : "disabled",
			(rc.EnableVUAot && !want) ? " (a gamefix or clamp mode it does not model is on)" : "");
	s_vuaot.enabled = want;
	s_vuaot.pending = false;
	s_vuaot.compileDepth = 0;

	// Whatever the worker holds or has made belongs to the code this reset
	// throws away. A job still running finishes into its own memory and is
	// dropped when it is collected (its epoch is old).
	s_vuaot.epoch++;
	s_vuaot.prepared = nullptr;
	s_vuaot.plainRootDepth = 0;
	s_vuaot.handles.clear();
	if (s_vuaot.worker)
	{
		if (want)
		{
			s_vuaot.worker->RemoveQueued([](const vuaot::bg::Job&) { return true; });
			std::vector<std::shared_ptr<vuaot::bg::Job>> stale;
			s_vuaot.worker->TakeCompleted(stale);
		}
		else
			s_vuaot.worker.reset(); // (stops it: no worker thread with the tier off)
	}
	if (want)
		vuAotWorker();
}

bool mVUvuAotEnabled() { return s_vuaot.enabled; }
bool mVUvuAotPending() { return s_vuaot.pending; }

namespace vuaot::live
{
	bool IsEnabled() { return s_vuaot.enabled; }

#ifdef PCSX2_RECOMPILER_TESTS
	void SetTierUpRuns(u32 runs) { s_vuaot.tierUpRuns = runs ? runs : 1; }
	void SetJumpTierUpRuns(u32 runs) { s_vuaot.jumpTierUpRuns = runs ? runs : 1; }
	void SetSynchronous(bool on) { s_vuaot.synchronous = on; }
	bool IsSynchronous() { return s_vuaot.synchronous; }
	bool WorkerRunning() { return s_vuaot.worker && s_vuaot.worker->Running(); }
	void HoldWorker(bool on)
	{
		if (s_vuaot.worker)
			s_vuaot.worker->Hold(on);
	}
	void SetWorkerGate(std::function<void()> gate)
	{
		if (s_vuaot.worker)
			s_vuaot.worker->SetGate(std::move(gate));
	}
	void WaitWorkerIdle()
	{
		if (s_vuaot.worker)
			s_vuaot.worker->WaitIdle();
	}
	size_t WorkerQueued() { return s_vuaot.worker ? s_vuaot.worker->Queued() : 0; }
	void SetWorkerDelayUs(u32 us) { s_vuAotWorkerDelayUs.store(us, std::memory_order_relaxed); }
#endif

	Stats GetStats()
	{
		Stats s = s_vuaot.stats;
		if (s_vuaot.body)
		{
			s.bodyBlocks = s_vuaot.body->Blocks();
			s.bodiesLowered = s_vuaot.body->Lowered();
			s.bodyBytes = s_vuaot.body->BodyBytes();
		}
		s.jumpMisses = s_vuaot.jumpMisses;
		// the bails of the programs still alive (the deleted ones were added as they went)
		for (const auto& entry : microVU1.mvuContentMap)
		{
			s.bailsEntry += entry.second->vuAotBails[0];
			s.bailsLoopHead += entry.second->vuAotBails[1];
		}
		for (const microProgram* prog : microVU1.mvuOrphanedProgs)
		{
			s.bailsEntry += prog->vuAotBails[0];
			s.bailsLoopHead += prog->vuAotBails[1];
		}
		s.workerIns = s_vuaot.workerIns.load(std::memory_order_relaxed);
		s.stickyTrips = s_trip.trips.load(std::memory_order_relaxed);
		for (u32 i = 0; i < kTripCauses; i++)
			s.stickyTripsBy[i] = s_trip.tripsBy[i].load(std::memory_order_relaxed);
		s.stickyTripResets = s_trip.resets.load(std::memory_order_relaxed);
		s.stickyRemembered = s_trip.tripsBy[static_cast<u32>(TripCause::Remembered)].load(std::memory_order_relaxed);
		return s;
	}

	std::string FormatStats()
	{
		const Stats s = GetStats();
		std::string out;
		char buf[1536];
		std::snprintf(buf, sizeof(buf),
			"vu-aot live tier: tierups=%llu retiers=%llu retierchecks=%llu blocks_set_aside=%llu targets=%llu jumpmisses=%llu "
			"roots=%llu regions=%llu rnodes=%llu rexits=%llu rbytes=%llu bodyblocks=%llu bodies=%llu bodybytes=%llu bails=%llu "
			"jrtierups=%llu jrcounted=%llu xprogtargets=%llu bails_entry=%llu bails_loophead=%llu "
			"status_readers=%llu sticky_readers=%llu sticky_div_readers=%llu sticky_dropped=%llu sticky_images=%llu "
			"sticky_trips=%llu sticky_trip_resets=%llu sticky_tripped=%d sticky_remembered=%llu",
			(unsigned long long)s.tierUps, (unsigned long long)s.retiers, (unsigned long long)s.retierChecks,
			(unsigned long long)s.blocksSetAside, (unsigned long long)s.targetsRecorded, (unsigned long long)s.jumpMisses,
			(unsigned long long)s.rootsCompiled, (unsigned long long)s.regions, (unsigned long long)s.regionNodes,
			(unsigned long long)s.regionExits, (unsigned long long)s.regionBytes, (unsigned long long)s.bodyBlocks,
			(unsigned long long)s.bodiesLowered, (unsigned long long)s.bodyBytes, (unsigned long long)s.bailCompiles,
			(unsigned long long)s.jumpTierUps, (unsigned long long)s.jumpCounted, (unsigned long long)s.crossProgTargets,
			(unsigned long long)s.bailsEntry, (unsigned long long)s.bailsLoopHead, (unsigned long long)s.statusReaders,
			(unsigned long long)s.stickyReaders, (unsigned long long)s.stickyDivReaders, (unsigned long long)s.stickyDropped,
			(unsigned long long)s.stickyImages, (unsigned long long)s.stickyTrips, (unsigned long long)s.stickyTripResets, StickyTripped() ? 1 : 0,
			(unsigned long long)s.stickyRemembered);
		out = buf;
		if (s.stickyTrips)
		{
			// (which readers set it off, in the order of TripCause; the last one's reason in words)
			std::snprintf(buf, sizeof(buf),
				"\n  sticky tripwire: trips=%llu (image %llu, compile %llu, job %llu, outside %llu, forced %llu) now %s: %s",
				(unsigned long long)s.stickyTrips, (unsigned long long)s.stickyTripsBy[1], (unsigned long long)s.stickyTripsBy[2],
				(unsigned long long)s.stickyTripsBy[3], (unsigned long long)s.stickyTripsBy[4], (unsigned long long)s.stickyTripsBy[5],
				StickyTripped() ? "set" : "re-armed", TripCauseName(StickyTripCause()));
			out += buf;
		}
		std::snprintf(buf, sizeof(buf),
			"\n  background: sync=%d jobs=%llu (gen %llu, root %llu) dropped=%llu done=%llu published=%llu (gen %llu, root %llu) "
			"declined=%llu coalesced=%llu discarded: stale=%llu gen=%llu memory=%llu sticky=%llu space=%llu trace=%llu | "
			"lag(dispatches) gen avg=%.1f max=%llu root avg=%.1f max=%llu | worker_ins=%llu publish_ins=%llu veneers=%llu",
			s_vuaot.synchronous ? 1 : 0, (unsigned long long)(s.jobsGen + s.jobsRoot), (unsigned long long)s.jobsGen,
			(unsigned long long)s.jobsRoot, (unsigned long long)s.jobsDropped, (unsigned long long)s.jobsDone,
			(unsigned long long)(s.publishedGen + s.publishedRoot), (unsigned long long)s.publishedGen,
			(unsigned long long)s.publishedRoot, (unsigned long long)s.jobsDeclined, (unsigned long long)s.jobsCoalesced,
			(unsigned long long)s.discardStale, (unsigned long long)s.discardGen, (unsigned long long)s.discardMemory,
			(unsigned long long)s.discardSticky, (unsigned long long)s.discardSpace, (unsigned long long)s.discardTrace,
			s.publishedGen ? static_cast<double>(s.lagGenSum) / s.publishedGen : 0.0, (unsigned long long)s.lagGenMax,
			s.publishedRoot ? static_cast<double>(s.lagRootSum) / s.publishedRoot : 0.0, (unsigned long long)s.lagRootMax,
			(unsigned long long)s.workerIns, (unsigned long long)s.publishIns, (unsigned long long)s.veneers);
		out += buf;
		if (s.planLookups || s.sidecarsQueued)
		{
			std::snprintf(buf, sizeof(buf),
				"\n  persist: lookups=%llu loaded=%llu rejected=%llu stale=%llu gen_jobs=%llu root_jobs=%llu roots_stale=%llu "
				"roots_covered=%llu roots_traced=%llu roots_unplaced=%llu published(gen %llu, root %llu) queued=%llu",
				(unsigned long long)s.planLookups, (unsigned long long)s.planLoaded, (unsigned long long)s.planRejected,
				(unsigned long long)s.planStale, (unsigned long long)s.planGenJobs, (unsigned long long)s.planRootJobs,
				(unsigned long long)s.planRootsStale, (unsigned long long)s.planRootsCovered, (unsigned long long)s.planRootsTraced,
				(unsigned long long)s.planRootsUnplaced, (unsigned long long)s.planPublishedGen, (unsigned long long)s.planPublishedRoot,
				(unsigned long long)s.sidecarsQueued);
			out += buf;
			for (u32 i = 1; i < static_cast<u32>(vuaot::persist::ParseResult::kCount); i++)
				if (s.planRejectedBy[i])
				{
					std::snprintf(buf, sizeof(buf), "\n  persist rejected: %s = %llu",
						vuaot::persist::ParseResultName(static_cast<vuaot::persist::ParseResult>(i)), (unsigned long long)s.planRejectedBy[i]);
					out += buf;
				}
		}
		if (s.firstRegions)
		{
			const auto avg = [](u64 sum, u64 n) { return n ? static_cast<double>(sum) / n : 0.0; };
			std::snprintf(buf, sizeof(buf),
				"\n  first region: programs=%llu runs avg=%.1f dispatches avg=%.1f vu1_cycles avg=%.0f | from a plan: programs=%llu "
				"runs avg=%.1f dispatches avg=%.1f vu1_cycles avg=%.0f",
				(unsigned long long)s.firstRegions, avg(s.firstRegionRuns, s.firstRegions), avg(s.firstRegionDispatches, s.firstRegions),
				avg(s.firstRegionCycles, s.firstRegions), (unsigned long long)s.planFirstRegions,
				avg(s.planFirstRegionRuns, s.planFirstRegions), avg(s.planFirstRegionDispatches, s.planFirstRegions),
				avg(s.planFirstRegionCycles, s.planFirstRegions));
			out += buf;
		}
		if (s.compileInsTiered || s.compileInsPlain)
		{
			std::snprintf(buf, sizeof(buf), "\n  compile instructions: tiered programs %llu, others %llu",
				(unsigned long long)s.compileInsTiered, (unsigned long long)s.compileInsPlain);
			out += buf;
		}
		for (const auto& d : s_vuaot.stats.regionDeclined)
		{
			std::snprintf(buf, sizeof(buf), "\n  region declined: %s = %u", d.first.c_str(), d.second);
			out += buf;
		}
		if (s_vuaot.body)
			for (const auto& d : s_vuaot.body->Declined())
			{
				std::snprintf(buf, sizeof(buf), "\n  body declined: %s = %u", d.first.c_str(), d.second);
				out += buf;
			}
		return out;
	}
} // namespace vuaot::live

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
	struct VuAotGraph
	{
		struct Slot
		{
			std::vector<VuAotWBlock*> all, full; // registration order; the exact-key blocks
		};
		std::map<u32, Slot> slots;
		bool flagHack = false;

		VuAotWBlock* Search(u32 pc, const microRegInfo& k) const
		{
			const auto it = slots.find(pc / 8);
			if (it == slots.end())
				return nullptr;
			if (k.needExactMatch)
			{
				for (VuAotWBlock* b : it->second.full)
					if (std::memcmp(&b->pState, &k, sizeof(k)) == 0)
						return b;
				return nullptr;
			}
			for (VuAotWBlock* b : it->second.all)
			{
				if (flagHack ? ((b->pState.quick64[0] & ~0x3002ull) != (k.quick64[0] & ~0x3002ull)) : (b->pState.quick64[0] != k.quick64[0]))
					continue;
				return b;
			}
			return nullptr;
		}
		void Add(VuAotWBlock* b)
		{
			Slot& s = slots[b->pc / 8];
			s.all.push_back(b);
			if (b->pState.needExactMatch)
				s.full.push_back(b);
		}
	};

	// The nodes each accepted job built, per (epoch, program, graph generation),
	// in order; worker thread only. A root job sees the ones built before it as
	// blocks of the graph (pending), so two jobs of one program asked for close
	// together do not both grow over the same blocks, as the synchronous tier's
	// regions, built one after another, would not.
	struct VuAotClaimKey
	{
		u64 epoch;
		const void* prog;
		u32 gen;
		bool operator<(const VuAotClaimKey& o) const
		{
			return epoch != o.epoch ? epoch < o.epoch : (prog != o.prog ? prog < o.prog : gen < o.gen);
		}
	};
	struct VuAotClaim
	{
		u32 pc;
		alignas(16) microRegInfo key;
	};
	thread_local std::map<VuAotClaimKey, std::vector<VuAotClaim>> t_vuAotClaims;

	struct VuAotWorkerContext
	{
		VuAotJob* job = nullptr;
		vuaot::bg::CodeSink* sink = nullptr;
		VuAotGraph graph;
	};
	thread_local VuAotWorkerContext* t_vuAotCtx = nullptr;

	VuAotWBlock* vuAotNewNode(VuAotJob& j, u32 pc, const void* key)
	{
		auto w = std::make_unique<VuAotWBlock>();
		std::memcpy(&w->pState, key, sizeof(microRegInfo));
		w->pc = pc;
		VuAotWBlock* const p = w.get();
		j.blocks.push_back(std::move(w));
		j.nodes.push_back(p);
		t_vuAotCtx->graph.Add(p);
		return p;
	}

	const void* vuAotPlaceholder(VuAotJob& j, VuAotJob::PhKind kind, u32 index)
	{
		const u32 id = static_cast<u32>(j.ph.size());
		pxAssertRel(id < vuaot::bg::CodeSink::kMaxPlaceholders, "vu-aot worker: out of placeholders");
		j.ph.push_back({kind, index});
		return t_vuAotCtx->sink->Placeholder(id);
	}

	// The region builder's microVU callbacks, on the worker.
	void* vuAotWSearch(u32, u32 pc, const u8* key)
	{
		alignas(16) microRegInfo k;
		std::memcpy(&k, key, sizeof(k));
		return t_vuAotCtx->graph.Search(pc & 0x3ff8, k);
	}
	void* vuAotWAdd(u32, u32 pc, const u8* key) { return vuAotNewNode(*t_vuAotCtx->job, pc & 0x3ff8, key); }
	void vuAotWSetEntry(void* block, void* entry)
	{
		VuAotWBlock* b = static_cast<VuAotWBlock*>(block);
		pxAssertRel(!b->snapshot, "vu-aot worker: an entry set on a block of the program's graph");
		b->hasEntry = true;
		b->entryOff = static_cast<u32>(static_cast<u8*>(entry) - t_vuAotCtx->sink->Base());
	}
	void* vuAotWEntry(void* block)
	{
		VuAotWBlock* b = static_cast<VuAotWBlock*>(block);
		if (b->snapshot)
			return b->entry;
		if (b->pending)
		{
			VuAotJob& j = *t_vuAotCtx->job;
			if (b->phId < 0)
			{
				j.pendingRefs.push_back(b);
				b->phId = static_cast<s32>(j.ph.size());
				vuAotPlaceholder(j, VuAotJob::PhPending, static_cast<u32>(j.pendingRefs.size() - 1));
			}
			return const_cast<void*>(t_vuAotCtx->sink->Placeholder(static_cast<u32>(b->phId)));
		}
		// (a node's entry is known only once the region is emitted; the only
		// one asked for before that is the root's, which is where emission starts)
		pxAssertRel(b->hasEntry, "vu-aot worker: the entry of a node not yet emitted");
		return t_vuAotCtx->sink->Base() + b->entryOff;
	}
	void* vuAotWFetch(u32, u32 pc, const u8* key)
	{
		VuAotJob& j = *t_vuAotCtx->job;
		VuAotJob::Exit e;
		e.pc = pc & 0x3ff8;
		std::memcpy(&e.key, key, sizeof(e.key));
		j.exits.push_back(e);
		return const_cast<void*>(vuAotPlaceholder(j, VuAotJob::PhExit, static_cast<u32>(j.exits.size() - 1)));
	}
	void vuAotWRange(u32, u32 startPC, u32 lastPC) { t_vuAotCtx->job->ranges.push_back({startPC, lastPC}); }
	void vuAotWTail(u32, void* block, const u8* endKey)
	{
		VuAotJob& j = *t_vuAotCtx->job;
		VuAotJob::Tail t;
		t.blk = static_cast<VuAotWBlock*>(block);
		std::memcpy(&t.endKey, endKey, sizeof(t.endKey));
		j.tails.push_back(t);
		armEmitJmp(vuAotPlaceholder(j, VuAotJob::PhTail, static_cast<u32>(j.tails.size() - 1)));
	}
} // namespace

void VuAotJob::Run()
{
#ifdef PCSX2_RECOMPILER_TESTS
	if (const u32 us = s_vuAotWorkerDelayUs.load(std::memory_order_relaxed))
		std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
	const u64 ins0 = vuAotReadIns();
	thread_local std::unique_ptr<vuaot::bg::CodeSink> sink;
	if (!sink)
		sink = std::make_unique<vuaot::bg::CodeSink>();
	VuAotWorkerContext ctx;
	ctx.job = this;
	ctx.sink = sink.get();
	ctx.graph.flagHack = flagHack;
	for (const auto& b : blocks)
		ctx.graph.Add(b.get());
	t_vuAotCtx = &ctx;
	// (claims of earlier epochs belong to code a reset threw away)
	for (auto it = t_vuAotClaims.begin(); it != t_vuAotClaims.end();)
		it = (it->first.epoch != epoch) ? t_vuAotClaims.erase(it) : std::next(it);
	const VuAotClaimKey claimKey{epoch, prog, kind == Root ? gen : gen + 1};
	if (kind == Root)
	{
		const auto it = t_vuAotClaims.find(claimKey);
		if (it != t_vuAotClaims.end())
			for (const VuAotClaim& c : it->second)
			{
				auto w = std::make_unique<VuAotWBlock>();
				w->pState = c.key;
				w->pc = c.pc;
				w->pending = true;
				ctx.graph.Add(w.get());
				blocks.push_back(std::move(w));
			}
	}
	else
		t_vuAotClaims.erase(claimKey);

	readMask.assign(0x4000 / 4, 0);
	vuaot::g_pipeReadMask = readMask.data();

	vuaot::RegionTier tier(1, micro.get());
	opts.To(tier);
	tier.jumpTargets = jumpTargets;

	vuaot::MvuBlockTrace b;
	if (haveTrace)
		b = std::move(trace);
	else
	{
		// No compile of microVU's to take the root's first pass from: the
		// port's own, which the publication compares against microVU's when it
		// compiles the root (as the synchronous tier compared it here).
		vuaot::PipeConfig cfg;
		cfg.vuIndex = 1;
		cfg.flagHack = env.flagHack;
		cfg.xgkickHack = env.xgkickHack;
		cfg.ibitHack = env.ibitHack;
		vuaot::PipeAnalyzer an(micro.get(), cfg);
		vuaot::PipeState st;
		std::memcpy(st.bytes, &key, sizeof(st.bytes));
		an.Analyze(pc, st).ToTrace(b, 1);
		rootTrace = b;
	}

	ph.clear();
	for (u32 i = 0; i < kPhStubs; i++)
		ph.push_back({PhStub, i});
	vuaot::MvuEmitEnv e = env;
	e.searchBlock = &vuAotWSearch;
	e.addBlock = &vuAotWAdd;
	e.setBlockEntry = &vuAotWSetEntry;
	e.blockEntry = &vuAotWEntry;
	e.fetchBlock = &vuAotWFetch;
	e.addRange = &vuAotWRange;
	e.emitJumpTail = &vuAotWTail;
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
	root = vuAotNewNode(*this, pc, &key);
	root->hasEntry = true;
	root->entryOff = 0;
	e.rootBlock = root;
	accepted = tier.OnCompile(b, e, &why);
	const u32 size = sink->End();
	vuaot::g_pipeReadMask = nullptr;
	t_vuAotCtx = nullptr;

	if (accepted && !sink->failed.empty())
	{
		accepted = false;
		why = "relocation: " + sink->failed;
		declined[why]++;
	}
	if (accepted)
	{
		std::vector<VuAotClaim>& claims = t_vuAotClaims[claimKey];
		for (const VuAotWBlock* n : nodes)
			claims.push_back({n->pc, n->pState});
		code.assign(sink->Base(), sink->Base() + size);
		fixups = sink->fixups;
		regions = tier.Regions();
		regionNodes = tier.Nodes();
		regionExits = tier.Exits();
		regionBytes = tier.Bytes();
	}
	for (const auto& d : tier.Declined())
		declined[d.first] += d.second;
	statusReaders = tier.StatusReaders();
	stickyReaders = tier.StickyReaders();
	stickyDivReaders = tier.StickyDivReaders();
	stickyDropped = tier.StickyDropped();
	stickyImages = tier.StickyReaderImages();
	workerIns = vuAotReadIns() - ins0;
	s_vuaot.workerIns.fetch_add(workerIns, std::memory_order_relaxed);
}

//------------------------------------------------------------------
// Making jobs (VU1 thread)
//------------------------------------------------------------------

static std::shared_ptr<VuAotJob> vuAotMakeJob(microVU& mVU, microProgram& prog, VuAotJob::Kind kind, u32 pc, const void* key)
{
	auto j = std::make_shared<VuAotJob>();
	j->epoch = s_vuaot.epoch;
	j->prog = &prog;
	j->gen = prog.vuaot ? prog.vuaot->gen : 0;
	j->kind = kind;
	j->pc = pc & 0x3ff8;
	std::memcpy(&j->key, key, sizeof(j->key));
	j->requestedAt = s_vuaot.dispatches;
	j->missesAtRequest = s_vuaot.jumpMisses;
	j->micro.reset(new u8[0x4000]);
	std::memcpy(j->micro.get(), mVU.regs().Micro, 0x4000);
	j->env = mVUbuildEmitEnv(mVU);
	j->env.program = &prog;
	j->env.flagHack = CHECK_VU_FLAGHACK;
	j->flagHack = CHECK_VU_FLAGHACK;
	j->cacheLo = mVU.cache;
	j->cacheHi = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	// Relaxation 3 holds while no sticky reader is known (section 9ad).
	j->opts.dropSticky = !s_trip.tripped.load(std::memory_order_acquire) || vuAotMutated(vuaot::live::kTripMutJobOption);
	if (prog.vuaot)
		j->jumpTargets = prog.vuaot->jumpTargets;
	return j;
}

// The program's current graph, for a job whose region must link to what is
// there (a Root job): every block but `exclude` (the one microVU is compiling).
static void vuAotSnapshotGraph(microProgram& prog, const void* exclude, VuAotJob& j)
{
	for (u32 slot = 0; slot < mProgSize / 2; slot++)
	{
		microBlockManager* m = prog.block[slot];
		if (!m)
			continue;
		m->forEachInSearchOrder([&](microBlock& b) {
			if (&b == exclude || (prog.vuaot && prog.vuaot->provisional.count(&b)))
				return;
			auto w = std::make_unique<VuAotWBlock>();
			w->pState = b.pState;
			w->pc = slot * 8;
			w->snapshot = true;
			w->entry = b.hostEntry;
			j.blocks.push_back(std::move(w));
		});
	}
}

static bool vuAotSubmit(const std::shared_ptr<VuAotJob>& j)
{
	if (!vuAotWorker().Submit(j))
	{
		s_vuaot.stats.jobsDropped++;
		return false;
	}
	(j->kind == VuAotJob::Generation ? s_vuaot.stats.jobsGen : s_vuaot.stats.jobsRoot)++;
	return true;
}

// Counters a finished job brings back, whatever becomes of it.
static void vuAotCollect(VuAotJob& j)
{
	s_vuaot.stats.jobsDone++;
	s_vuaot.stats.rootsCompiled++;
	if (!j.accepted)
		s_vuaot.stats.jobsDeclined++;
	for (const auto& d : j.declined)
		s_vuaot.stats.regionDeclined[d.first] += d.second;
	s_vuaot.stats.statusReaders += j.statusReaders;
	s_vuaot.stats.stickyReaders += j.stickyReaders;
	s_vuaot.stats.stickyDivReaders += j.stickyDivReaders;
	s_vuaot.stats.stickyDropped += j.stickyDropped;
	s_vuaot.stats.stickyImages += j.stickyImages;
	// The micro memory the job compiled under has a reader of the sticky bits (the scan is of the
	// whole image, so this is the same fact the program-creation scan reports, found by another path).
	if (j.stickyImages)
		vuAotTrip(vuaot::live::TripCause::Job);
}

//------------------------------------------------------------------
// The saved plan (VU1 thread): persistence-design.md, the plan half
//------------------------------------------------------------------

// A program saves its plan once it has gone this many runs without a publication (the re-tier
// check interval), or at the next reset or close.
static constexpr u32 kSaveQuietRuns = vuaot::live::kRetierCheckMask + 1;

static bool vuAotSidecarsOn(const microProgram& prog)
{
	return prog.contentHashValid && mVUProgCache::SidecarsEnabled(1);
}

static vuaot::persist::Identity vuAotSidecarIdentity(const microProgram& prog)
{
	vuaot::persist::Identity id;
	id.contentHashLo = prog.contentHash.low64;
	id.contentHashHi = prog.contentHash.high64;
	if (!microVU1.optionsSentinelValid)
		mVUbuildOptionsSentinel(microVU1);
	id.sentinelLo = microVU1.optionsSentinel.low64;
	id.sentinelHi = microVU1.optionsSentinel.high64;
	mVUPersist::GetBuildId(id.buildLo, id.buildHi);
	id.mvuAbi = kMvuCompilerAbiVersion;
	return id;
}

// A published job's plan joins its program's list (a generation starts the list again).
static void vuAotRecordPublished(microProgram& prog, const VuAotJob& j)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || !vuAotSidecarsOn(prog))
		return;
	static_assert(sizeof(microRegInfo) == vuaot::persist::kKeyBytes, "a plan's key is a microRegInfo");
	if (j.kind == VuAotJob::Generation)
		st->published.clear();
	auto r = std::make_shared<vuaot::persist::Record>();
	r->kind = (j.kind == VuAotJob::Generation) ? vuaot::persist::kGeneration : vuaot::persist::kRoot;
	r->flags = static_cast<u8>((j.viaJump ? vuaot::persist::kViaJump : 0) | (j.opts.dropSticky ? vuaot::persist::kDropSticky : 0) |
							   (j.stickyDropped ? vuaot::persist::kStickyDropped : 0) | (j.accepted ? vuaot::persist::kAccepted : 0));
	r->pc = j.pc;
	std::memcpy(r->key, &j.key, sizeof(j.key));
	r->SetReadSet(j.readMask.data(), j.micro.get());
	st->published.push_back(std::move(r));
	st->dirty = true;
	st->publishRuns = vuAotTotalRuns(prog);
}

// Hands the program's plan to the program cache's writer, which serializes and writes it off this
// thread. A plan without its generation is not a plan: nothing to write.
static void vuAotSaveProgram(microProgram& prog)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || !st->dirty)
		return;
	st->dirty = false;
	if (st->published.empty() || st->published.front()->kind != vuaot::persist::kGeneration || !vuAotSidecarsOn(prog))
		return;
	struct Request
	{
		vuaot::persist::Identity id;
		u32 tiers;
		bool settled;
		std::map<u32, std::vector<u32>> jumpTargets;
		std::vector<std::shared_ptr<const vuaot::persist::Record>> records;
		u64 loadedChecksum;
	};
	auto req = std::make_shared<Request>(Request{vuAotSidecarIdentity(prog), st->tiers, st->settled, st->jumpTargets, st->published, st->loadedChecksum});
	// Root plans of a loaded sidecar not yet published stay in the plan: a session reset before their jobs
	// came back must not write a smaller plan over the one it loaded.
	if (st->planGen == st->gen)
		for (const auto& pr : st->planOut)
			req->records.push_back(std::make_shared<const vuaot::persist::Record>(pr->rec));
	for (const vuaot::persist::Record& r : st->planRoots)
		req->records.push_back(std::make_shared<const vuaot::persist::Record>(r));
	mVUProgCache::QueueSidecar(1, prog.contentHash, [req]() {
		vuaot::persist::File f;
		f.id = req->id;
		f.tiers = req->tiers;
		f.settled = req->settled;
		f.jumpTargets = req->jumpTargets;
		f.records.reserve(req->records.size());
		for (const auto& r : req->records)
			f.records.push_back(*r);
		std::vector<u8> bytes = vuaot::persist::Serialize(f);
		// (the plan this program was loaded with, unchanged: the file on disk already holds it)
		if (req->loadedChecksum && XXH3_64bits(bytes.data(), bytes.size()) == req->loadedChecksum)
			bytes.clear();
		return bytes;
	});
	s_vuaot.stats.sidecarsQueued++;
}

static void vuAotMaybeSave(microProgram& prog, u32 total)
{
	const VuAotProgramState& st = *prog.vuaot;
	if (st.dirty && !st.genOutstanding && st.ready.empty() && total - st.publishRuns >= kSaveQuietRuns)
		vuAotSaveProgram(prog);
}

// A program was created or loaded for the image now in micro memory, and nothing names it yet (no
// quick slot, no jump cache, no code). With its sidecar found and accepted, and its generation's read
// set matching micro memory, the program starts tiered: its generation goes to the worker now, rooted
// where the saved one was and with the saved jump targets, and its root plans wait for the generation
// to be published. Nothing is installed here, so the program runs microVU's code until a slow path
// publishes the job, exactly as after a tier-up.
void mVUvuAotOnProgramReady(microVU& mVU, microProgram& prog)
{
	prog.vuAotBornAt = s_vuaot.dispatches;
	prog.vuAotBornCycle = mVU.regs().cycle;
	if (mVU.index != 1 || !s_vuaot.enabled || prog.vuaot || !vuAotSidecarsOn(prog))
		return;
	s_vuaot.stats.planLookups++;
	std::vector<u8> bytes;
	if (!mVUProgCache::ReadSidecar(1, prog.contentHash, bytes))
		return;
	vuaot::persist::File f;
	vuaot::persist::ParseResult pr = vuaot::persist::Parse(bytes.data(), bytes.size(), vuAotSidecarIdentity(prog), f);
	if (pr == vuaot::persist::ParseResult::Ok && (f.records.empty() || f.records.front().kind != vuaot::persist::kGeneration))
		pr = vuaot::persist::ParseResult::Malformed;
	if (pr != vuaot::persist::ParseResult::Ok)
	{
		s_vuaot.stats.planRejected++;
		s_vuaot.stats.planRejectedBy[std::min<u32>(static_cast<u32>(pr), 15)]++;
		DevCon.WriteLn(Color_StrongBlue, "vu-aot: prog[%03d] sidecar refused (%s)", prog.idx, vuaot::persist::ParseResultName(pr));
		return;
	}
	const vuaot::persist::Record& gen = f.records.front();
	if (!gen.MatchesMicro(mVU.regs().Micro))
	{
		s_vuaot.stats.planStale++;
		return;
	}
	s_vuaot.stats.planLoaded++;

	VuAotProgramState* const st = prog.vuaot = new VuAotProgramState();
	st->jumpTargets = f.jumpTargets;
	for (const auto& jt : st->jumpTargets)
		st->targetCount += static_cast<u32>(jt.second.size());
	st->settled = f.settled;
	// (the publication counts the generation it brings back)
	st->tiers = f.tiers ? f.tiers - 1 : 0;
	st->loadedChecksum = XXH3_64bits(bytes.data(), bytes.size());
	st->fromPlan = true;
	for (size_t i = 1; i < f.records.size(); i++)
	{
		if (f.records[i].MatchesMicro(mVU.regs().Micro))
			st->planRoots.push_back(std::move(f.records[i]));
		else
			s_vuaot.stats.planRootsStale++;
	}

	auto j = vuAotMakeJob(mVU, prog, VuAotJob::Generation, gen.pc, gen.key);
	j->viaJump = (gen.flags & vuaot::persist::kViaJump) != 0;
	j->fromPlan = true;
	st->requestRuns = 0;
	st->missesSeen = s_vuaot.jumpMisses;
	if (!vuAotSubmit(j))
	{
		st->genAttempts++;
		return;
	}
	st->genOutstanding = true;
	s_vuaot.stats.planGenJobs++;
	// Deterministic mode: the job is finished before the program first runs, and the slow path that
	// is loading the program publishes it (mVUvuAotAfterSearch, mVUvuAotAfterJump).
	if (s_vuaot.synchronous)
		vuAotWorker().Wait(j);
}

// The root plans of a loaded sidecar, once its generation is in: each goes to the worker, built against
// the graph as it is now, unless the graph already holds its root.
static void vuAotSubmitPlanRoots(microVU& mVU, microProgram& prog, VuAotProgramState& st)
{
	std::vector<vuaot::persist::Record> roots;
	roots.swap(st.planRoots);
	st.planOut.clear();
	st.planGen = st.gen;
	for (const vuaot::persist::Record& r : roots)
	{
		if (!r.MatchesMicro(mVU.regs().Micro))
		{
			s_vuaot.stats.planRootsStale++;
			continue;
		}
		// (exact keys, without microBlockManager::search, which reorders what it finds)
		bool covered = false;
		if (microBlockManager* m = prog.block[r.pc / 8])
			m->forEachBlock([&](microBlock& b) { covered |= std::memcmp(&b.pState, r.key, sizeof(b.pState)) == 0; });
		if (covered)
		{
			s_vuaot.stats.planRootsCovered++;
			continue;
		}
		auto j = vuAotMakeJob(mVU, prog, VuAotJob::Root, r.pc, r.key);
		j->fromPlan = true;
		vuAotSnapshotGraph(prog, nullptr, *j);
		if (!vuAotSubmit(j))
			continue;
		auto out = std::make_unique<VuAotProgramState::PlanRoot>();
		out->pc = j->pc;
		std::memcpy(&out->key, r.key, sizeof(out->key));
		out->job = j.get();
		out->rec = r;
		st.planOut.push_back(std::move(out));
		s_vuaot.stats.planRootJobs++;
		if (s_vuaot.synchronous)
			vuAotWorker().Wait(j);
	}
}

// The plan root a job was sent for, or null.
static VuAotProgramState::PlanRoot* vuAotPlanRootOf(VuAotProgramState& st, const VuAotJob& j)
{
	if (!j.fromPlan || j.kind != VuAotJob::Root || st.planGen != st.gen)
		return nullptr;
	for (auto& pr : st.planOut)
		if (pr->job == &j)
			return pr.get();
	return nullptr;
}

static void vuAotForgetPlanRoot(VuAotProgramState& st, const VuAotJob& j)
{
	for (auto it = st.planOut.begin(); it != st.planOut.end(); ++it)
		if ((*it)->job == &j)
		{
			st.planOut.erase(it);
			return;
		}
}

void mVUvuAotSaveAll(microVU& mVU)
{
	if (mVU.index != 1)
		return;
	for (const auto& entry : mVU.mvuContentMap)
		if (entry.second)
			vuAotSaveProgram(*entry.second);
	for (microProgram* prog : mVU.mvuOrphanedProgs)
		vuAotSaveProgram(*prog);
}

//------------------------------------------------------------------
// Publishing (VU1 thread)
//------------------------------------------------------------------

// The program is loaded: micro memory holds its image over its ranges
// (mVUcmpProg without its side effects).
static bool vuAotProgramLoaded(microVU& mVU, const microProgram& prog)
{
	for (const auto& r : *prog.ranges)
		if (std::memcmp(reinterpret_cast<const u8*>(prog.data) + r.start, mVU.regs().Micro + r.start, r.end - r.start))
			return false;
	return true;
}

// Every micro-memory word the job's compile read is still what it read.
static bool vuAotMicroMatches(microVU& mVU, const VuAotJob& j, const u8* snapshot)
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
static bool vuAotHasRoom(microVU& mVU, const VuAotJob& j)
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
static void vuAotPlace(microVU& mVU, VuAotJob& j, microBlock* rootReal)
{
	if (j.stickyDropped)
		s_trip.dropLive.store(true, std::memory_order_release);
	u8* const base = armGetCurrentCodePointer();
	if (rootReal)
		pxAssertRel(base == rootReal->hostEntry, "vu-aot: a region placed away from its root's entry");
	{
		vixl::CodeBufferCheckScope scope(armAsm, j.code.size(), vixl::CodeBufferCheckScope::kReserveBufferSpace,
			vixl::CodeBufferCheckScope::kNoAssert);
		armAsm->GetBuffer()->EmitData(j.code.data(), j.code.size());
	}
	for (const vuaot::bg::Fixup& f : j.fixups)
		if (f.kind == vuaot::bg::Fixup::IntraAbs)
			pxAssertRel(vuaot::bg::PatchAbsMov(reinterpret_cast<u32*>(armGetWritableCodePtr(base + f.offset)), reinterpret_cast<u64>(base + f.value)),
				"vu-aot: an address in a region is not where its fixup says");

	// The nodes, the root first, in the order the region registered them.
	for (VuAotWBlock* n : j.nodes)
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
		pxAssertRel(n->hasEntry, "vu-aot: a region node without an entry");
		if (VuAotProgramState* st = mVU.prog.cur->vuaot)
			st->provisional.erase(b);
		b->x86ptrStart = base + n->entryOff;
		b->hostEntry = base + n->entryOff;
		n->real = b;
	}
	for (const auto& r : j.ranges)
		vuAotAddRange(1, r.first, r.second);

	std::vector<const void*> target(j.ph.size(), nullptr);
	for (size_t id = 0; id < j.ph.size(); id++)
	{
		if (j.ph[id].first != VuAotJob::PhStub)
			continue;
		switch (j.ph[id].second)
		{
			case VuAotJob::kPhBailTarget: target[id] = reinterpret_cast<const void*>(j.env.bailTarget); break;
			case VuAotJob::kPhClearLpState: target[id] = j.env.clearLpState; break;
			case VuAotJob::kPhEndFlagsB: target[id] = j.env.endFlagsB; break;
			case VuAotJob::kPhExitEBit: target[id] = j.env.exitEBit; break;
			case VuAotJob::kPhXgkick: target[id] = j.env.xgkickFn; break;
			case VuAotJob::kPhCycleBreak: target[id] = j.env.cycleBreak; break;
		}
	}
	// microVU's dispatch for the JR/JALR nodes, then the exits, compiled now
	// in the order the region met them (as the synchronous tier compiled them
	// after the region).
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuAotJob::PhTail)
		{
			const VuAotJob::Tail& t = j.tails[j.ph[id].second];
			target[id] = armGetCurrentCodePointer();
			vuAotEmitJumpTail(1, t.blk->real, reinterpret_cast<const u8*>(&t.endKey));
		}
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuAotJob::PhExit)
		{
			VuAotJob::Exit& x = j.exits[j.ph[id].second];
			target[id] = mVUblockFetch(mVU, x.pc, reinterpret_cast<uptr>(&x.key));
		}
	// Links to an earlier job's nodes: what the graph holds for their keys now
	// (that job's region if it was published, else microVU's block, compiled
	// now if there is none).
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuAotJob::PhPending)
		{
			VuAotWBlock& w = *j.pendingRefs[j.ph[id].second];
			target[id] = mVUblockFetch(mVU, w.pc, reinterpret_cast<uptr>(&w.pState));
		}
	for (const vuaot::bg::Fixup& f : j.fixups)
		if (f.kind == vuaot::bg::Fixup::SiteAbs)
			pxAssertRel(vuaot::bg::PatchAbsMov(reinterpret_cast<u32*>(armGetWritableCodePtr(base + f.offset)), reinterpret_cast<u64>(target[f.id])),
				"vu-aot: an address in a region is not where its fixup says");

	// The branches to placeholders and to the outside world. A target out of
	// B/BL range (a C helper, from the code cache) goes through a veneer
	// placed after the code: x16 is the branch's scratch, as in armEmitCall's
	// long form.
	std::map<const void*, const u8*> veneers;
	for (const vuaot::bg::Fixup& f : j.fixups)
	{
		if (f.kind == vuaot::bg::Fixup::IntraAbs || f.kind == vuaot::bg::Fixup::SiteAbs)
			continue;
		const void* to = (f.kind == vuaot::bg::Fixup::Site26) ? target[f.id] : reinterpret_cast<const void*>(f.value);
		pxAssertRel(to, "vu-aot: a region branch with no target");
		u8* const at = base + f.offset;
		u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
		if (vuaot::bg::PatchRel26(insn, at, to))
			continue;
		auto it = veneers.find(to);
		if (it == veneers.end())
		{
			const u8* v = armGetCurrentCodePointer();
			armAsm->Mov(a64::x16, reinterpret_cast<uintptr_t>(to));
			armAsm->Br(a64::x16);
			it = veneers.emplace(to, v).first;
			s_vuaot.stats.veneers++;
		}
		pxAssertRel(vuaot::bg::PatchRel26(insn, at, it->second), "vu-aot: a region branch out of reach of its veneer");
	}

	for (auto& b : j.blocks)
		if (!b->snapshot && !b->pending)
			s_vuaot.handles.push_back(std::move(b));
	j.blocks.clear();
	if (VuAotProgramState* const st = mVU.prog.cur->vuaot; st && j.regions && !st->hadRegion)
	{
		// How long the program ran on microVU's code before its first region (of any job).
		st->hadRegion = true;
		const microProgram& prog = *mVU.prog.cur;
		const u64 runs = vuAotTotalRuns(prog), disp = s_vuaot.dispatches - prog.vuAotBornAt, cyc = mVU.regs().cycle - prog.vuAotBornCycle;
		s_vuaot.stats.firstRegions++;
		s_vuaot.stats.firstRegionRuns += runs;
		s_vuaot.stats.firstRegionDispatches += disp;
		s_vuaot.stats.firstRegionCycles += cyc;
		if (st->fromPlan)
		{
			s_vuaot.stats.planFirstRegions++;
			s_vuaot.stats.planFirstRegionRuns += runs;
			s_vuaot.stats.planFirstRegionDispatches += disp;
			s_vuaot.stats.planFirstRegionCycles += cyc;
		}
	}
	s_vuaot.stats.regions += j.regions;
	s_vuaot.stats.regionNodes += j.regionNodes;
	s_vuaot.stats.regionExits += j.regionExits;
	s_vuaot.stats.regionBytes += j.regionBytes;
}

// Moves the program's microVU graph out of the way (the graveyard): the next
// compile of any of its blocks starts from an empty graph.
static u32 vuAotSetAside(microVU& mVU, microProgram& prog, VuAotProgramState& st)
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
// that compile finds the job prepared and places the region (vuAotLiveRegionHook).
static void vuAotPublishGeneration(microVU& mVU, VuAotJob& j)
{
	microProgram& prog = *j.prog;
	VuAotProgramState& st = *prog.vuaot;
	const u32 setAside = vuAotSetAside(mVU, prog, st);
	st.provisional.clear();
	st.gen++;
	st.tiers++;
	st.runsAtTier = vuAotTotalRuns(prog);
	st.checkBlock = vuAotTotalRuns(prog) >> kCheckShift;
	st.missesSeen = j.missesAtRequest;
	if (st.tiers > vuaot::live::kMaxRetiers)
		st.settled = true;
	if (j.retier)
		s_vuaot.stats.retiers++;
	else
		s_vuaot.stats.tierUps++;
	if (j.viaJump)
		s_vuaot.stats.jumpTierUps++;
	s_vuaot.stats.blocksSetAside += setAside;
	DevCon.WriteLn(Color_StrongBlue, "vu-aot: %s prog[%03d] after %u runs: %u blocks set aside, %u jump targets over %zu jumps%s",
		j.retier ? "re-tier" : "tier-up", prog.idx, vuAotTotalRuns(prog), setAside, st.targetCount, st.jumpTargets.size(),
		j.accepted ? "" : " (region declined)");

	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	// In a code-cache episode of its own, as a root's publication: the slow path's episode can hold blocks
	// it compiled for another program, still untiered, and the persisted-program recorder drops a whole
	// episode once a block of a tiered program is registered in it (it would lose those blocks).
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	s_vuaot.prepared = &j;
	mVUblockFetch(mVU, j.pc, reinterpret_cast<uptr>(&j.key));
	if (s_vuaot.prepared == &j)
	{
		// (the root was compiled without asking the hook: nothing placed)
		s_vuaot.prepared = nullptr;
		s_vuaot.stats.discardTrace++;
	}
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	mVU.prog.cur = savedCur;
}

// Empties every quick slot that holds `prog`, so every jump cache that knew its old entries asks again.
static void vuAotDropQuickSlots(microVU& mVU, microProgram& prog)
{
	for (u32 i = 0; i < mVU.progSize / 2; i++)
		if (mVU.prog.quick[i].prog == &prog)
		{
			mVU.prog.quick[i].prog = nullptr;
			mVU.prog.quick[i].block = nullptr;
			// prog.quickOcc has a bit for each slot that holds a program, and mVUclear visits only those.
			mVU.prog.quickOcc[i >> 6] &= ~(u64(1) << (i & 63));
		}
}

// A region rooted at a block microVU compiled on its own while the job ran:
// placed in a code-cache episode of its own (so the persisted-program recorder
// never takes its bytes for microVU's), its nodes take over the entries of the
// blocks there, and the program's quick slots are dropped so every jump cache
// that knew the old entries asks again.
static void vuAotPublishRoot(microVU& mVU, VuAotJob& j)
{
	microProgram& prog = *j.prog;
	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	vuAotPlace(mVU, j, nullptr);
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	mVU.prog.cur = savedCur;
	vuAotDropQuickSlots(mVU, prog);
}

// A root from a loaded plan whose root microVU has not compiled: its root is compiled now, in an
// episode of its own, and the compile finds the job prepared and places it there (as a generation's
// root compile does: vuAotLiveRegionHook compares microVU's first pass with the port's first). The
// region's other nodes may take over blocks microVU compiled since the generation, so the program's
// quick slots are dropped as for any root. Returns whether the region went in.
static bool vuAotPublishPlanRootByCompile(microVU& mVU, VuAotJob& j)
{
	microProgram& prog = *j.prog;
	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	s_vuaot.prepared = &j;
	mVUblockFetch(mVU, j.pc, reinterpret_cast<uptr>(&j.key));
	const bool placed = s_vuaot.prepared != &j;
	s_vuaot.prepared = nullptr;
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	mVU.prog.cur = savedCur;
	vuAotDropQuickSlots(mVU, prog);
	return placed;
}

// Whether live micro memory has a sticky STATUS reader; if it has, the tripwire goes off.
static bool vuAotLiveImageReadsSticky(microVU& mVU)
{
	if (!vuaot::RegionTier::MicroReadsSticky(mVU.regs().Micro, 1))
		return false;
	vuAotTrip(vuaot::live::TripCause::Job);
	return true;
}

// Publishes what is ready for `prog`. `loaded`: the slow path has just resolved
// it from micro memory. Returns true when its graph changed.
static bool vuAotPublishReady(microVU& mVU, microProgram& prog, bool loaded)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || st->ready.empty())
		return false;
	if (!loaded && !vuAotProgramLoaded(mVU, prog))
		return false;
	const u64 ins0 = vuAotReadIns();
	const microRegInfo savedLp = mVU.prog.lpState;
	std::vector<std::shared_ptr<VuAotJob>> ready;
	ready.swap(st->ready);
	bool changed = false;
	for (auto& jp : ready)
	{
		VuAotJob& j = *jp;
		const bool gen = j.kind == VuAotJob::Generation;
		const auto drop = [&](u64& counter) {
			counter++;
			if (gen)
			{
				st->genOutstanding = false;
				if (st->gen == 0)
					st->genAttempts++;
			}
			else if (j.fromPlan)
				vuAotForgetPlanRoot(*st, j);
		};
		if (j.gen != st->gen)
		{
			drop(s_vuaot.stats.discardGen);
			continue;
		}
		if (!vuAotMicroMatches(mVU, j, j.micro.get()))
		{
			drop(s_vuaot.stats.discardMemory);
			continue;
		}
		// The region dropped the sticky STATUS byte because the job's micro image had no
		// reader of it and none was known when the job was made. The tripwire must still be armed
		// and the image a region runs under must not have a reader either (a scan that finds one
		// sets the tripwire off).
		if (j.stickyDropped && !vuAotMutated(vuaot::live::kTripMutPublish) &&
			(s_trip.tripped.load(std::memory_order_acquire) || vuAotLiveImageReadsSticky(mVU)))
		{
			drop(s_vuaot.stats.discardSticky);
			continue;
		}
		if (j.accepted && !vuAotHasRoom(mVU, j))
		{
			drop(s_vuaot.stats.discardSpace);
			continue;
		}
		const u64 lag = s_vuaot.dispatches - j.requestedAt;
		if (gen)
		{
			s_vuaot.stats.publishedGen++;
			s_vuaot.stats.lagGenSum += lag;
			s_vuaot.stats.lagGenMax = std::max(s_vuaot.stats.lagGenMax, lag);
			st->genOutstanding = false;
			vuAotPublishGeneration(mVU, j);
			vuAotRecordPublished(prog, j);
			if (j.fromPlan)
				s_vuaot.stats.planPublishedGen++;
			if (!st->planRoots.empty())
				vuAotSubmitPlanRoots(mVU, prog, *st);
		}
		else if (VuAotProgramState::PlanRoot* const pr = vuAotPlanRootOf(*st, j))
		{
			// A root from a loaded plan: microVU's first pass of the root against the port's, from the
			// compile microVU made of it meanwhile, or from the compile made for it now.
			bool placed = false;
			if (pr->haveTrace)
			{
				if (!vuaot::CompareBlockTraces(pr->trace, j.rootTrace).empty())
				{
					s_vuaot.stats.discardTrace++;
					s_vuaot.stats.regionDeclined["timing port disagrees"]++;
				}
				else
				{
					vuAotPublishRoot(mVU, j);
					placed = true;
				}
			}
			else if (!(placed = vuAotPublishPlanRootByCompile(mVU, j)))
				s_vuaot.stats.planRootsUnplaced++;
			vuAotForgetPlanRoot(*st, j);
			if (placed)
			{
				s_vuaot.stats.publishedRoot++;
				s_vuaot.stats.planPublishedRoot++;
				s_vuaot.stats.lagRootSum += lag;
				s_vuaot.stats.lagRootMax = std::max(s_vuaot.stats.lagRootMax, lag);
				vuAotRecordPublished(prog, j);
			}
		}
		else
		{
			s_vuaot.stats.publishedRoot++;
			s_vuaot.stats.lagRootSum += lag;
			s_vuaot.stats.lagRootMax = std::max(s_vuaot.stats.lagRootMax, lag);
			vuAotPublishRoot(mVU, j);
			vuAotRecordPublished(prog, j);
		}
		changed = true;
	}
	// (the compiles above leave their keys in the dispatcher's carried state)
	mVU.prog.lpState = savedLp;
	s_vuaot.stats.publishIns += vuAotReadIns() - ins0;
	return changed;
}

// Collects the worker's finished jobs (all but `except`, which the caller takes
// itself) onto their programs' ready lists; `touched` gets each program that
// received one.
static void vuAotCollectFinished(std::vector<microProgram*>& touched, const VuAotJob* except)
{
	if (s_vuaot.worker && s_vuaot.worker->CompletedCount())
	{
		std::vector<std::shared_ptr<vuaot::bg::Job>> done;
		s_vuaot.worker->TakeCompleted(done);
		for (auto& jb : done)
		{
			if (jb.get() == except)
				continue;
			auto j = std::static_pointer_cast<VuAotJob>(jb);
			vuAotCollect(*j);
			if (j->epoch != s_vuaot.epoch || !j->prog->vuaot)
			{
				s_vuaot.stats.discardStale++;
				continue;
			}
			VuAotProgramState& st = *j->prog->vuaot;
			if (j->kind == VuAotJob::Root && !j->accepted)
			{
				// (nothing to publish; microVU's block stays)
				if (j->fromPlan)
					vuAotForgetPlanRoot(st, *j);
				continue;
			}
			if (std::find(touched.begin(), touched.end(), j->prog) == touched.end())
				touched.push_back(j->prog);
			st.ready.push_back(std::move(j));
		}
	}
}

// Collects the worker's finished jobs onto their programs, then publishes what
// it can: `cur`'s (resolved by the slow path) and any other program's whose
// image is in micro memory now. Returns true when `cur`'s graph changed.
static bool vuAotDrainAndPublish(microVU& mVU, microProgram* cur)
{
	std::vector<microProgram*> touched;
	vuAotCollectFinished(touched, nullptr);
	bool changed = false;
	if (cur)
		changed = vuAotPublishReady(mVU, *cur, true);
	for (microProgram* p : touched)
		if (p != cur)
			vuAotPublishReady(mVU, *p, false);
	return changed;
}

// The entry for (startPC, key) in `prog`'s graph as it is now (compiled with
// the hooks if it is not there): where a slow path continues after a publication.
//
// The slot for the slow path's address is put back to `prog`, which the search
// has just resolved from micro memory. A root publication empties every quick
// slot of its program, and a JR slow path (mVUcompileJIT) copies this slot's
// program into the jump cache entry it fills, next to the entry returned here.
// Left empty, that would be an entry of no program with live code, and the
// inline probe in front of the JR (normJumpCompile) takes an entry whose program
// equals the slot's without asking whether it is null: every later JR there
// would jump to `prog`'s code whenever the slot is empty, as it is after any
// micro-memory write over that address, whatever code is in memory then.
static void* vuAotRefetch(microVU& mVU, microProgram& prog, u32 startPC, const microRegInfo& key)
{
	alignas(16) microRegInfo k = key;
	void* const entry = mVUblockFetch(mVU, startPC, reinterpret_cast<uptr>(&k));
	const u32 slot = mVU.regs().start_pc / 8;
	microProgramQuick& quick = mVU.prog.quick[slot];
	mVUquickInstall(mVU, slot, &prog); // sets the slot's bit in prog.quickOcc, which mVUclear walks
	quick.block = prog.block[startPC / 8];
	return entry;
}

//------------------------------------------------------------------
// The dispatch path
//------------------------------------------------------------------

// mVUlookupProg_VU1 plus the run counter. A program reaches the tier-up
// threshold, a tiered one the periodic look for new jump targets, and a
// finished job its publication by returning null: the dispatcher then takes
// the full path (mVUexecuteVU1), which ends in mVUvuAotAfterSearch.
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
	s_vuaot.dispatches++;
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

	const u32 runs = ++prog->vuAotRuns;
	// The worker has finished something: one slow path collects it.
	if (s_vuaot.worker && s_vuaot.worker->CompletedCount())
	{
		s_vuaot.pending = true;
		return nullptr;
	}
	VuAotProgramState* const st = prog->vuaot;
	if (!st)
	{
		if (runs >= s_vuaot.tierUpRuns)
		{
			s_vuaot.pending = true;
			return nullptr;
		}
	}
	else if (!st->ready.empty())
	{
		s_vuaot.pending = true;
		return nullptr;
	}
	else if (st->gen == 0)
	{
		if (!st->genOutstanding && st->genAttempts < kMaxGenAttempts && vuAotTotalRuns(*prog) - st->requestRuns >= s_vuaot.tierUpRuns)
		{
			s_vuaot.pending = true;
			return nullptr;
		}
	}
	else
	{
		const u32 total = runs + prog->vuAotJumpRuns;
		if ((total >> kCheckShift) != st->checkBlock)
		{
			// A quiet program with a new plan hands it to the program cache's writer (no slow path
			// needed). (A settled program no longer moves checkBlock, so this asks at every run
			// until the plan is handed over.)
			if (st->dirty)
				vuAotMaybeSave(*prog, total);
			if (!st->settled && !st->genOutstanding)
			{
				// a look for new targets is due; the crossing is used up either way
				st->checkBlock = total >> kCheckShift;
				if (st->missesSeen != s_vuaot.jumpMisses)
				{
					s_vuaot.pending = true;
					return nullptr;
				}
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
static u32 vuAotHarvestTargets(microProgram& prog, VuAotProgramState& st)
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
						s_vuaot.stats.crossProgTargets++;
					continue;
				}
				const u32 pc = t * 8;
				const auto at = std::lower_bound(v.begin(), v.end(), pc);
				if (at != v.end() && *at == pc)
					continue;
				if (v.size() >= vuaot::live::kMaxTargetsPerJump || st.targetCount >= vuaot::live::kMaxTargetsPerProgram)
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
	s_vuaot.stats.targetsRecorded += added;
	return added;
}

// Asks the worker for a new generation of the program's graph, rooted at the
// entry this slow path came through. In deterministic mode the job is waited
// for and published here, and the slow path continues in the new graph.
static void* vuAotRequestGeneration(microVU& mVU, microProgram& prog, VuAotProgramState& st, bool retier, bool viaJump,
	u32 startPC, const microRegInfo& key, void* result)
{
	auto j = vuAotMakeJob(mVU, prog, VuAotJob::Generation, startPC, &key);
	j->retier = retier;
	j->viaJump = viaJump;
	st.requestRuns = vuAotTotalRuns(prog);
	st.missesSeen = s_vuaot.jumpMisses;
	// Root jobs for the program still waiting in the queue would build into the
	// graph this generation replaces: drop them.
	if (s_vuaot.worker)
		s_vuaot.worker->RemoveQueued([&](const vuaot::bg::Job& b) {
			const VuAotJob& q = static_cast<const VuAotJob&>(b);
			const bool drop = q.prog == &prog && q.kind == VuAotJob::Root;
			s_vuaot.stats.jobsCoalesced += drop;
			return drop;
		});
	if (!vuAotSubmit(j))
	{
		if (st.gen == 0)
			st.genAttempts++;
		return result;
	}
	st.genOutstanding = true;
	if (!s_vuaot.synchronous)
		return result;
	vuAotWorker().Wait(j);
	if (vuAotDrainAndPublish(mVU, &prog))
		return vuAotRefetch(mVU, prog, startPC, key);
	return result;
}

// The decision both slow paths share. `prog` (mVU.prog.cur) has been resolved and its
// run counters bumped; `result` is the entry the search found for `startPC`. An untiered
// program asks for its tier-up when `tierNow` (the caller's threshold test); a tiered one
// looks for new jump targets when it has met some.
static void* vuAotConsider(microVU& mVU, microProgram& prog, u32 startPC, const microRegInfo& key, void* result, bool tierNow, bool viaJump)
{
	VuAotProgramState* st = prog.vuaot;
	if (!st || st->gen == 0)
	{
		if (st && (st->genOutstanding || st->genAttempts >= kMaxGenAttempts || vuAotTotalRuns(prog) - st->requestRuns < s_vuaot.tierUpRuns))
			return result;
		if (!tierNow)
			return result;
		if (!st)
			st = prog.vuaot = new VuAotProgramState();
		// Targets first: the job takes a copy of what the jump caches know now.
		vuAotHarvestTargets(prog, *st);
		return vuAotRequestGeneration(mVU, prog, *st, false, viaJump, startPC, key, result);
	}
	if (st->genOutstanding || st->settled || vuAotTotalRuns(prog) - st->runsAtTier < vuaot::live::kRetierMinRuns ||
		st->missesSeen == s_vuaot.jumpMisses)
		return result;
	s_vuaot.stats.retierChecks++;
	st->missesSeen = s_vuaot.jumpMisses;
	// Targets the jump caches saw that the regions do not hold. If there are
	// none the program stays as it is.
	if (vuAotHarvestTargets(prog, *st) == 0)
	{
		if (++st->emptyChecks >= vuaot::live::kSettleChecks)
			st->settled = true;
		return result;
	}
	st->emptyChecks = 0;
	return vuAotRequestGeneration(mVU, prog, *st, true, viaJump, startPC, key, result);
}

// The dispatcher's slow path: collects and publishes what the worker made, and
// is where a program asks for its tier-up. mVU.prog.cur is the program the
// search resolved and `result` its entry for `startPC`.
void* mVUvuAotAfterSearch(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuaot.enabled || mVU.index != 1)
		return result;
	// The counting lookup already counted a dispatch that asked for this.
	const bool counted = s_vuaot.pending;
	s_vuaot.pending = false;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
	{
		vuAotDrainAndPublish(mVU, nullptr);
		return result;
	}
	if (!counted)
		prog->vuAotRuns++;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	if (vuAotDrainAndPublish(mVU, prog))
		result = vuAotRefetch(mVU, *prog, startPC, key);
	return vuAotConsider(mVU, *prog, startPC, key, result, prog->vuAotRuns >= s_vuaot.tierUpRuns, false);
}

// The same for a JR/JALR the jump cache missed (mVUcompileJIT), after the search resolved
// its target to a program. Counts the entry on that program, and asks for its tier-up when
// jumps are how it is reached: a program the dispatcher never enters (an overlay a stub
// program jumps into) has no dispatch count to go by. A program the dispatcher does enter
// keeps its dispatch threshold: tiering it from a jump into the middle of it would root its
// first compile there, which splits what a compile from the program's entry keeps in one region.
void* mVUvuAotAfterJump(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuaot.enabled || mVU.index != 1)
		return result;
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
	{
		vuAotDrainAndPublish(mVU, nullptr);
		return result;
	}
	const u32 jumps = ++prog->vuAotJumpRuns;
	s_vuaot.stats.jumpCounted++;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	if (vuAotDrainAndPublish(mVU, prog))
		result = vuAotRefetch(mVU, *prog, startPC, key);
	bool tierNow = false;
	VuAotProgramState* const st = prog->vuaot;
	if (st && st->gen > 0)
	{
		// A tiered program looks for new targets once per unit of runs, as on the
		// dispatch path, not at every entry.
		const u32 total = vuAotTotalRuns(*prog);
		// (a program the dispatcher never enters saves its plan from here)
		if (st->dirty && (total >> kCheckShift) != st->checkBlock)
			vuAotMaybeSave(*prog, total);
		if (st->settled || (total >> kCheckShift) == st->checkBlock)
			return result;
		st->checkBlock = total >> kCheckShift;
	}
	else
	{
		// (jumps dominate: sixteen to one over dispatches)
		tierNow = jumps >= s_vuaot.jumpTierUpRuns && static_cast<u64>(prog->vuAotRuns) * 16 <= jumps;
	}
	return vuAotConsider(mVU, *prog, startPC, key, result, tierNow, true);
}

void mVUvuAotOnJumpMiss(microVU& mVU)
{
	if (s_vuaot.enabled && mVU.index == 1)
		s_vuaot.jumpMisses++;
}

void mVUvuAotOnBail()
{
	s_vuaot.stats.bailCompiles++;
}

// The tier's counters, once, when VU1 shuts down; the worker stops with it.
void mVUvuAotOnClose(microVU& mVU)
{
	if (mVU.index != 1)
		return;
	if (s_vuaot.stats.tierUps || s_vuaot.stats.jobsGen)
		Console.WriteLn(Color_StrongBlue, "%s", vuaot::live::FormatStats().c_str());
	s_vuaot.worker.reset();
}

void mVUvuAotOnProgramDeleted(microProgram& prog)
{
	s_vuaot.stats.bailsEntry += prog.vuAotBails[0];
	s_vuaot.stats.bailsLoopHead += prog.vuAotBails[1];
	VuAotProgramState* st = prog.vuaot;
	if (!st)
		return;
	for (microBlockManager* m : st->graveyard)
		delete m;
	delete st;
	prog.vuaot = nullptr;
}

//------------------------------------------------------------------
// The compile hooks
//------------------------------------------------------------------

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
// With pcsx2-eerunner --count-vu1-ins: the user-mode instructions the outermost
// compile sessions of VU1 retire, split by whether the program was tiered up
// (vu-aot compiles) or not (microVU's own), for reading the tier's compile cost
// next to its run-time gain. Totals are in vuaot::live::Stats.
static u64 s_vuAotSessionIns0 = 0;
static bool s_vuAotSessionTiered = false;
#endif

void mVUvuAotCompileBegin(microVU& mVU)
{
	if (mVU.index == 1 && s_vuaot.enabled)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (s_vuaot.compileDepth == 0 && vuAotCountingIns())
		{
			s_vuAotSessionIns0 = mvu_test_hooks::ReadThreadInstructions();
			s_vuAotSessionTiered = mVU.prog.cur && mVU.prog.cur->vuaot && mVU.prog.cur->vuaot->gen > 0;
		}
#endif
		s_vuaot.compileDepth++;
	}
}

void mVUvuAotCompileEnd(microVU& mVU)
{
	if (mVU.index != 1 || !s_vuaot.enabled || s_vuaot.compileDepth <= 0)
		return;
	if (--s_vuaot.compileDepth < s_vuaot.plainRootDepth)
		s_vuaot.plainRootDepth = 0;
	if (s_vuaot.compileDepth == 0)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (vuAotCountingIns())
		{
			const u64 d = mvu_test_hooks::ReadThreadInstructions() - s_vuAotSessionIns0;
			(s_vuAotSessionTiered ? s_vuaot.stats.compileInsTiered : s_vuaot.stats.compileInsPlain) += d;
		}
#endif
		if (s_vuaot.body)
			s_vuaot.body->ResetAnalyzer();
	}
}

// The block being compiled belongs to a region a job is building: microVU
// compiles it as its own.
static bool vuAotUnderPlainRoot() { return s_vuaot.plainRootDepth && s_vuaot.compileDepth > s_vuaot.plainRootDepth; }

static bool vuAotLiveRegionHook(const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user)
{
	microVU& mVU = microVU1;
	VuAotProgramState& st = *static_cast<VuAotProgramState*>(user);
	// A generation being published: its root's compile places it.
	if (VuAotJob* j = s_vuaot.prepared)
	{
		if (j->prog == mVU.prog.cur && b.startPC == j->pc && std::memcmp(b.entryState, &j->key, sizeof(j->key)) == 0)
		{
			s_vuaot.prepared = nullptr;
			if (!j->accepted)
				return false;
			// microVU's own first pass of the root against the port's, the check the
			// synchronous tier made on every root.
			if (!vuaot::CompareBlockTraces(b, j->rootTrace).empty())
			{
				s_vuaot.stats.discardTrace++;
				s_vuaot.stats.regionDeclined["timing port disagrees"]++;
				return false;
			}
			vuAotPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
			return true;
		}
	}
	// A root a loaded plan has already sent to the worker: no second job. microVU compiles the block
	// as its own for now (as for any root whose job is out), and its first pass is kept for the
	// publication's check.
	if (!st.planOut.empty() && st.planGen == st.gen)
		for (auto& pr : st.planOut)
			if (!pr->haveTrace && pr->pc == b.startPC && std::memcmp(&pr->key, b.entryState, sizeof(pr->key)) == 0)
			{
				pr->haveTrace = true;
				pr->trace = b;
				s_vuaot.stats.planRootsTraced++;
				st.provisional.insert(static_cast<const microBlock*>(env.rootBlock));
				if (!vuAotUnderPlainRoot())
					s_vuaot.plainRootDepth = s_vuaot.compileDepth;
				return false;
			}
	if (vuAotUnderPlainRoot())
	{
		st.provisional.insert(static_cast<const microBlock*>(env.rootBlock));
		return false;
	}

	// A block microVU meets in a program already tiered up: a region rooted at
	// it goes to the worker, built against the program's graph as it is now.
	auto j = vuAotMakeJob(mVU, *mVU.prog.cur, VuAotJob::Root, b.startPC, b.entryState);
	j->haveTrace = true;
	j->trace = b;
	vuAotSnapshotGraph(*mVU.prog.cur, env.rootBlock, *j);
	if (s_vuaot.synchronous)
	{
		if (!vuAotSubmit(j))
			return false;
		vuAotWorker().Wait(j);
		// (taken out of the worker's finished list here, not by a slow path; any other finished job,
		// one a loaded plan sent, goes to its program's ready list for the next slow path)
		std::vector<microProgram*> touched;
		vuAotCollectFinished(touched, j.get());
		vuAotCollect(*j);
		if (!j->accepted)
			return false;
		s_vuaot.stats.publishedRoot++;
		vuAotPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
		vuAotRecordPublished(*mVU.prog.cur, *j);
		return true;
	}
	vuAotSubmit(j);
	// microVU compiles the block, and what it reaches from it, as its own for now.
	st.provisional.insert(static_cast<const microBlock*>(env.rootBlock));
	s_vuaot.plainRootDepth = s_vuaot.compileDepth;
	return false;
}

static bool vuAotLiveBodyHook(const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void*)
{
	if (vuAotUnderPlainRoot())
		return false;
	if (!s_vuaot.body)
		s_vuaot.body = std::make_unique<vuaot::BodyTier>(1, microVU1.regs().Micro);
	return s_vuaot.body->TryLower(b, env, nullptr);
}

void mVUvuAotLiveHooks(microVU& mVU, vuaot::VuAotRegionHook& region, void*& regionUser,
	vuaot::VuAotBodyHook& body, void*& bodyUser)
{
	if (mVU.index != 1 || !s_vuaot.enabled)
		return;
	microProgram* const cur = mVU.prog.cur;
	if (!cur || !cur->vuaot || cur->vuaot->gen == 0)
		return;
	region = &vuAotLiveRegionHook;
	regionUser = cur->vuaot;
	body = &vuAotLiveBodyHook;
	bodyUser = cur->vuaot;
}

#ifdef PCSX2_RECOMPILER_TESTS
namespace vuaot::live
{
	// pcsx2-vurunner --vuaot-region-bg: the region hook through the worker. A
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
		auto j = vuAotMakeJob(mVU, *mVU.prog.cur, VuAotJob::Root, b.startPC, b.entryState);
		j->gen = 0;
		j->haveTrace = true;
		j->trace = b;
		j->env = env;
		j->opts.From(tmpl);
		j->jumpTargets = tmpl.jumpTargets;
		vuAotSnapshotGraph(*mVU.prog.cur, env.rootBlock, *j);
		if (!vuAotSubmit(j))
			return false;
		vuAotWorker().Wait(j);
		std::vector<std::shared_ptr<vuaot::bg::Job>> done;
		s_vuaot.worker->TakeCompleted(done);
		vuAotCollect(*j);
		if (!j->accepted)
			return false;
		vuAotPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
		return true;
	}
} // namespace vuaot::live
#endif
