// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vu-aot live tier: tier-up policy and the glue between microVU's programs and
// blocks and vu-aot's regions. Included at the end of microVU-arm64.cpp (it
// needs microVU's TU-local types and helpers); the policy constants and the
// part of the interface other code may use are in vuaot/VuAotLive.h.
//
// Threads. Everything here runs on the thread that executes VU1 (the EE thread,
// or the MTVU thread), except VuAotJob::Run and what it calls, which run on the
// tier's worker thread (vuaot::bg::Worker). A job holds copies of everything it
// reads and writes only its own memory, so the two threads share nothing but the
// worker's queue. A finished job is published on the VU1 thread at a slow path
// microVU already takes (a dispatch that missed the quick path, or a JR/JALR the
// jump cache missed), after checking that the program and every micro-memory
// word the compile read are unchanged. A reset (settings change, savestate load,
// cache exhaustion) arrives from a thread that has waited for VU1 to go idle; it
// bumps the epoch, and whatever a job made before it is dropped unread.

// (The vu-aot headers are included at the top of microVU-arm64.cpp: they cannot
// follow microVU's macros.)

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

static u32 vuAotTotalRuns(const microProgram& p) { return p.vuAotRuns + p.vuAotJumpRuns; }

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

	// The program cache's sidecar (vuaot/VuAotPersist.h): the plans of the jobs published into
	// the current generation, in publication order (the generation first). A new generation
	// starts the list again. Kept only while the cache is on.
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
	// Roots the builder declined in the current generation (saved with the sidecar), and those a loaded
	// sidecar named: microVU compiling one of those asks for no job (the answer is known: no region).
	std::vector<vuaot::persist::DeclinedRoot> declined;
	std::set<std::pair<u32, vuaot::persist::Key>> knownDeclined;
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
	bool merge = true;
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
		merge = t.merge;
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
		t.merge = merge;
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
	// The program cache keeps sidecars: the job also describes its code as a record's (recordCode).
	bool wantRecord = false;

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
	u32 regions = 0, regionNodes = 0, regionExits = 0, regionMerged = 0;
	u64 regionBytes = 0;
	u32 wMemHomes = 0; // vector homes whose w lane stayed in microVU's location (HomeMap::wMem)
	std::map<std::string, u32> declined;
	// The flag-reader census of the compile, and whether it dropped the sticky STATUS byte
	// (decided by scanning the job's copy of micro memory; publication scans live micro memory
	// again and discards the job if a sticky reader has appeared).
	u32 statusReaders = 0, stickyReaders = 0, stickyDivReaders = 0, stickyDropped = 0, stickyImages = 0;
	u64 workerIns = 0;
	// With wantRecord: the code as a saved record holds it, every address in it a fixup, or why
	// the job cannot be saved with its code (an address nothing can re-base in another process;
	// the job is still published, and saved as its plan).
	vuaot::persist::RegionCode recordCode;
	std::string notPersistable;

	void Run() override;
	void MakeRecordCode();
};

namespace
{
	struct VuAotLive
	{
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

	// Regions drop the sticky byte of STATUS, in every flag-hack mode, while no sticky reader is
	// known; this is the "known" part. It lives apart from VuAotLive because a reset (settings
	// change, savestate load, cache exhaustion) rebuilds that one, and this must survive it: only
	// a VM reset or boot clears it (vuaot::live::ResetStickyTripwire). Atomics, because the
	// writers are microVU's compiles and the tier's publications (VU1 thread) and VU0 accesses
	// to VU1's STATUS (EE thread, after MTVU has gone idle).
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

	// The tripwire's memory across sessions: titles that set it off are lines ("SERIAL CRC") of
	// vu_jit/vuaot_tripped.txt under the cache folder, read when a session starts or the title
	// changes and appended to at a trip. Any thread can trip (VU0 runs on the EE thread), and
	// trips are rare, so one mutex.
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
static std::atomic<u32> s_vuAotWorkerDelayUs{0};
// VU1 thread only.
static std::vector<vuaot::live::RecordCodeSample> s_vuAotRecordSamples;
static bool s_vuAotInstall = true;
#else
static constexpr bool s_vuAotInstall = true;
#endif
static size_t s_vuAotLastPlaced = 0;

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
static bool vuAotCountingIns() { return mvu_test_hooks::g_vu1InsCount.load(std::memory_order_relaxed); }
static u64 vuAotReadIns() { return vuAotCountingIns() ? mvu_test_hooks::ReadThreadInstructions() : 0; }
#else
static u64 vuAotReadIns() { return 0; }
#endif

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
void vuAotSettingChanged(bool wasOn, bool isOn, bool programCacheStillOn) { mVUProgCache::OnVuAotSettingChanged(wasOn, isOn, programCacheStillOn); }

// The global (base layer) value of EnableVUAot, as last seen with no VM running. It starts at the setting's
// default; VMManager records the loaded value at startup and as each VM ends.
static bool s_vuAotGlobalOn = true;
void vuAotNoteGlobalSetting(bool on) { s_vuAotGlobalOn = on; }
void vuAotNoVmSettingsApplied(bool on, bool programCacheStillOn)
{
	const bool was = std::exchange(s_vuAotGlobalOn, on);
	mVUProgCache::OnVuAotSettingChanged(was, on, programCacheStillOn);
}
// The disc details, for the tripwire's memory and for the program cache, whose packs are per title.
void vuAotSetTitle(const std::string& serial, u32 crc)
{
	vuaot::live::SetTitle(serial, crc);
	mVUProgCache::SetTitle(serial, crc);
}

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
	void SetInstallEnabled(bool on) { s_vuAotInstall = on; }
	void TestPadCodeCache(u32 bytes)
	{
		microVU& mVU = microVU1;
		const bool wasOpen = armAsm != nullptr;
		if (wasOpen)
			mVUcloseCodeCache(mVU);
		mVUopenCodeCache(mVU);
		{
			vixl::ExactAssemblyScope scope(armAsm, bytes & ~3u);
			for (u32 i = 0; i < bytes / 4; i++)
				armAsm->dc32(0xD4200000u); // brk #0
		}
		mVUcloseCodeCache(mVU);
		if (wasOpen)
			mVUopenCodeCache(mVU);
	}
	size_t LastPlacedOffset() { return s_vuAotLastPlaced; }
	std::vector<RecordCodeSample> TakeRecordCodeSamples()
	{
		std::vector<RecordCodeSample> out;
		out.swap(s_vuAotRecordSamples);
		return out;
	}
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
			"roots=%llu regions=%llu rnodes=%llu rmerged=%llu rexits=%llu rbytes=%llu bodyblocks=%llu bodies=%llu bodybytes=%llu bails=%llu "
			"jrtierups=%llu jrcounted=%llu xprogtargets=%llu bails_entry=%llu bails_loophead=%llu "
			"status_readers=%llu sticky_readers=%llu sticky_div_readers=%llu sticky_dropped=%llu sticky_images=%llu "
			"sticky_trips=%llu sticky_trip_resets=%llu sticky_tripped=%d sticky_remembered=%llu",
			(unsigned long long)s.tierUps, (unsigned long long)s.retiers, (unsigned long long)s.retierChecks,
			(unsigned long long)s.blocksSetAside, (unsigned long long)s.targetsRecorded, (unsigned long long)s.jumpMisses,
			(unsigned long long)s.rootsCompiled, (unsigned long long)s.regions, (unsigned long long)s.regionNodes,
			(unsigned long long)s.regionMerged, (unsigned long long)s.regionExits, (unsigned long long)s.regionBytes, (unsigned long long)s.bodyBlocks,
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
		if (s.aotPrograms || s.aotNoCode || s.aotRejectedBy[kAotNoCode] || s.aotRejectedBy[kAotSticky] || s.aotRejectedBy[kAotAddress] ||
			s.aotRejectedBy[kAotShape] || s.aotRejectedBy[kAotCacheRun] || s.aotRejectedBy[kAotSpace])
		{
			std::snprintf(buf, sizeof(buf),
				"\n  installed: programs=%llu records=%llu regions=%llu bytes=%llu roots_to_plan=%llu saved_without_code=%llu install_ins=%llu "
				"declined_loaded=%llu declined_skipped=%llu",
				(unsigned long long)s.aotPrograms, (unsigned long long)s.aotRecords, (unsigned long long)s.aotRegions,
				(unsigned long long)s.aotBytes, (unsigned long long)s.aotRootsToPlan, (unsigned long long)s.aotNoCode,
				(unsigned long long)s.installIns, (unsigned long long)s.declinedLoaded, (unsigned long long)s.declinedSkipped);
			out += buf;
			for (u32 i = 1; i < kAotRejects; i++)
				if (s.aotRejectedBy[i])
				{
					std::snprintf(buf, sizeof(buf), "\n  install refused: %s = %llu", AotRejectName(static_cast<AotReject>(i)),
						(unsigned long long)s.aotRejectedBy[i]);
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
	void vuAotWNoteData(const u8* at, u64 value) { t_vuAotCtx->sink->NoteData64(at, value); }
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
		// No microVU compile to take the root's first pass from, so use the port's
		// own; publication compares it against microVU's when microVU compiles the root.
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
	e.noteDataWord = &vuAotWNoteData;
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
		regionMerged = tier.Merged();
		regionExits = tier.Exits();
		regionBytes = tier.Bytes();
		wMemHomes = tier.WMemHomes();
		if (wantRecord)
			MakeRecordCode();
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

// (worker) The job's code as a saved record holds it. Every address in it becomes a fixup with a
// target another process can resolve: the job's own code, a placeholder (microVU code the install
// compiles or looks up), an offset into the module image or the data arena (C helpers, globals, VU
// state), a block of the program's graph by its PC and key, the program, a node's handle. Anything
// else (a heap address: --vuaot-profile's counters) leaves the job without a saved copy of its code.
void VuAotJob::MakeRecordCode()
{
	namespace ps = vuaot::persist;
	ps::RegionCode& c = recordCode;
	c = ps::RegionCode{};
	const auto fail = [&](const char* why) {
		if (notPersistable.empty())
			notPersistable = why;
	};
	std::map<ps::Key, u32> keyIndex;
	const auto key = [&](const void* k) -> u32 {
		ps::Key kk;
		std::memcpy(kk.data(), k, ps::kKeyBytes);
		const auto it = keyIndex.find(kk);
		if (it != keyIndex.end())
			return it->second;
		const u32 i = static_cast<u32>(c.keys.size());
		c.keys.push_back(kk);
		keyIndex.emplace(kk, i);
		return i;
	};
	std::map<const VuAotWBlock*, u32> nodeIndex;
	for (const VuAotWBlock* n : nodes)
	{
		if (!n->hasEntry)
		{
			fail("a node without an entry");
			return;
		}
		nodeIndex.emplace(n, static_cast<u32>(c.nodes.size()));
		c.nodes.push_back({n->pc, key(&n->pState), n->entryOff});
	}
	for (const Exit& e : exits)
		c.exits.push_back({e.pc, key(&e.key)});
	for (const VuAotWBlock* w : pendingRefs)
		c.pending.push_back({w->pc, key(&w->pState)});
	for (const Tail& t : tails)
	{
		const auto it = nodeIndex.find(t.blk);
		if (it == nodeIndex.end())
		{
			fail("a jump tail of a block that is not a node");
			return;
		}
		c.tails.push_back({it->second, key(&t.endKey)});
	}
	for (const auto& r : ranges)
		c.ranges.push_back({r.first, r.second});
	for (const auto& p : ph)
	{
		ps::Placeholder q;
		q.kind = p.first == PhStub ? ps::kPhStub : p.first == PhExit ? ps::kPhExit : p.first == PhTail ? ps::kPhTail : ps::kPhPending;
		q.index = p.second;
		c.ph.push_back(q);
	}
	// The program's blocks by their entry (a region links to one through its address): one block per entry,
	// or the link cannot be told from another.
	std::map<const void*, const VuAotWBlock*> byEntry;
	for (const auto& b : blocks)
		if (b->snapshot && b->entry)
		{
			const auto it = byEntry.find(b->entry);
			if (it == byEntry.end())
				byEntry.emplace(b->entry, b.get());
			else if (it->second && std::memcmp(&it->second->pState, &b->pState, sizeof(microRegInfo)) != 0)
				it->second = nullptr;
		}
	std::map<const VuAotWBlock*, u32> refIndex;
	const auto classify = [&](u64 value, ps::Fixup& f) -> bool {
		const void* addr = reinterpret_cast<const void*>(value);
		const u8 region = mVUPersist::RegionOf(addr);
		if (region == mVUPersist::kTargetImage || region == mVUPersist::kTargetData)
		{
			f.target = region == mVUPersist::kTargetImage ? ps::kTargetImage : ps::kTargetData;
			f.value = static_cast<s64>(value - mVUPersist::RegionBase(region));
			return true;
		}
		return false;
	};
	for (const vuaot::bg::Fixup& g : fixups)
	{
		ps::Fixup f;
		f.offset = g.offset;
		switch (g.kind)
		{
			case vuaot::bg::Fixup::IntraAbs:
				f.form = ps::kFormMovRun;
				f.width = 4;
				f.target = ps::kTargetIntra;
				f.value = static_cast<s64>(g.value);
				break;
			case vuaot::bg::Fixup::SiteAbs:
			case vuaot::bg::Fixup::Site26:
				f.form = g.kind == vuaot::bg::Fixup::SiteAbs ? ps::kFormMovRun : ps::kFormRel26;
				f.width = g.kind == vuaot::bg::Fixup::SiteAbs ? 4 : 0;
				f.target = ps::kTargetPlaceholder;
				f.index = g.id;
				break;
			case vuaot::bg::Fixup::External26:
				f.form = ps::kFormRel26;
				if (!classify(g.value, f))
				{
					fail("a branch to an address outside the image");
					return;
				}
				break;
			case vuaot::bg::Fixup::OutsideAbs:
			{
				f.form = ps::kFormMovRun;
				f.width = static_cast<u8>(g.id);
				const u8* addr = reinterpret_cast<const u8*>(g.value);
				if (addr >= cacheLo && addr < cacheHi)
				{
					// (a link to a block of the program's graph: microVU's code, or an earlier region's node)
					const auto it = byEntry.find(addr);
					if (it == byEntry.end() || !it->second)
					{
						fail(it == byEntry.end() ? "a code-cache address that is no block's entry" : "two blocks at one entry");
						return;
					}
					auto [ri, added] = refIndex.emplace(it->second, static_cast<u32>(c.blockRefs.size()));
					if (added)
						c.blockRefs.push_back({it->second->pc, key(&it->second->pState)});
					f.target = ps::kTargetBlockRef;
					f.index = ri->second;
				}
				else if (!classify(g.value, f))
				{
					fail("an address outside the image and the data arena");
					return;
				}
				break;
			}
			case vuaot::bg::Fixup::Data64:
			{
				f.form = ps::kFormData64;
				if (g.value == reinterpret_cast<u64>(env.program))
					f.target = ps::kTargetProgram;
				else if (const auto it = nodeIndex.find(reinterpret_cast<const VuAotWBlock*>(g.value)); it != nodeIndex.end())
				{
					f.target = ps::kTargetNodeHandle;
					f.index = it->second;
					// The bail slot after the block pointer is written at run time and must start empty.
					u64 slot;
					if (g.offset + 16 > code.size() || (std::memcpy(&slot, code.data() + g.offset + 8, 8), slot != 0))
					{
						fail("a bail record whose slot is not empty");
						return;
					}
				}
				else
				{
					fail("a data word that is neither the program nor a node");
					return;
				}
				break;
			}
		}
		c.fixups.push_back(f);
	}
	c.code = code;
	c.regions = regions;
	c.regionNodes = regionNodes;
	c.regionMerged = regionMerged;
	c.regionExits = regionExits;
	c.regionBytes = regionBytes;
}

static bool vuAotSidecarsOn(const microProgram& prog);

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
	// Regions drop the sticky byte of STATUS only while no sticky reader is known.
	j->opts.dropSticky = !s_trip.tripped.load(std::memory_order_acquire) || vuAotMutated(vuaot::live::kTripMutJobOption);
	if (prog.vuaot)
		j->jumpTargets = prog.vuaot->jumpTargets;
	j->wantRecord = vuAotSidecarsOn(prog);
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

static bool vuAotTryInstall(microVU& mVU, microProgram& prog, VuAotProgramState& st, vuaot::persist::File& f);
static void vuAotSubmitPlanRoots(microVU& mVU, microProgram& prog, VuAotProgramState& st);

// A program saves its plan once it has gone this many runs without a publication (the re-tier
// check interval), or at the next reset or close.
static constexpr u32 kSaveQuietRuns = vuaot::live::kRetierCheckMask + 1;

static bool vuAotSidecarsOn(const microProgram& prog)
{
	return prog.contentHashValid && mVUProgCache::SidecarsEnabled(1);
}

// The struct offsets and sizes region code bakes in as immediates: saved code from a build whose
// layout differs is refused even if the build id did not change.
static u32 vuAotLayoutHash()
{
	static const u32 hash = [] {
		const vuaot::MvuEmitEnv e = mVUbuildEmitEnv(microVU1);
		const s64 words[] = {e.offDivFlag, e.offVIbackup, e.offVIxgkick, e.offBranch, e.offNeonBackup, e.offCycles, e.offProgBails,
			static_cast<s64>(offsetof(VURegs, VI)), static_cast<s64>(offsetof(VURegs, pending_q)),
			static_cast<s64>(offsetof(VURegs, pending_p)), static_cast<s64>(offsetof(VURegs, start_pc)),
			static_cast<s64>(offsetof(mVU_Globals, minvals)), static_cast<s64>(offsetof(mVU_Globals, maxvals)),
			static_cast<s64>(sizeof(microRegInfo)), static_cast<s64>(offsetof(microBlock, pState)),
			static_cast<s64>(offsetof(microBlock, jumpCache)), static_cast<s64>(sizeof(microJumpCache)),
			static_cast<s64>(offsetof(microJumpCache, prog)), static_cast<s64>(offsetof(microJumpCache, hostEntry)),
			static_cast<s64>(offsetof(VuAotWBlock, pState)), static_cast<s64>(sizeof(VURegs))};
		return static_cast<u32>(XXH3_64bits(words, sizeof(words)));
	}();
	return hash;
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
	id.layoutHash = vuAotLayoutHash();
	return id;
}

// A published job's record joins its program's list (a generation starts the list again): its plan, and its
// code when the worker could describe every address in it.
static void vuAotRecordPublished(microProgram& prog, VuAotJob& j)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || !vuAotSidecarsOn(prog))
		return;
	static_assert(sizeof(microRegInfo) == vuaot::persist::kKeyBytes, "a plan's key is a microRegInfo");
	if (j.kind == VuAotJob::Generation)
	{
		st->published.clear();
		// (a generation from a loaded plan rebuilds the one the declined roots were met in)
		if (!j.fromPlan)
		{
			st->declined.clear();
			st->knownDeclined.clear();
		}
	}
	auto r = std::make_shared<vuaot::persist::Record>();
	r->kind = (j.kind == VuAotJob::Generation) ? vuaot::persist::kGeneration : vuaot::persist::kRoot;
	r->flags = static_cast<u8>((j.viaJump ? vuaot::persist::kViaJump : 0) | (j.opts.dropSticky ? vuaot::persist::kDropSticky : 0) |
							   (j.stickyDropped ? vuaot::persist::kStickyDropped : 0) | (j.accepted ? vuaot::persist::kAccepted : 0));
	r->pc = j.pc;
	std::memcpy(r->key, &j.key, sizeof(j.key));
	r->SetReadSet(j.readMask.data(), j.micro.get());
	if (j.accepted && j.wantRecord)
	{
		if (j.notPersistable.empty() && !j.recordCode.code.empty())
		{
			r->flags |= vuaot::persist::kHasCode;
			r->code = std::move(j.recordCode);
		}
		else
			s_vuaot.stats.aotNoCode++;
	}
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
		std::vector<vuaot::persist::DeclinedRoot> declined;
	};
	auto req = std::make_shared<Request>(
		Request{vuAotSidecarIdentity(prog), st->tiers, st->settled, st->jumpTargets, st->published, st->loadedChecksum, st->declined});
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
		f.declinedRoots = req->declined;
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
	st->declined = f.declinedRoots;
	for (const vuaot::persist::DeclinedRoot& d : f.declinedRoots)
		st->knownDeclined.insert({d.pc, d.key});
	s_vuaot.stats.declinedLoaded += f.declinedRoots.size();
	for (size_t i = 1; i < f.records.size(); i++)
		if (!f.records[i].MatchesMicro(mVU.regs().Micro))
		{
			s_vuaot.stats.planRootsStale++;
			f.records[i].kind = 0xff; // (dropped)
		}

	// The saved code, when the generation's can go in: no compile, the regions run from the first dispatch.
	if (vuAotTryInstall(mVU, prog, *st, f))
		return;

	for (size_t i = 1; i < f.records.size(); i++)
		if (f.records[i].kind == vuaot::persist::kRoot)
		{
			f.records[i].DropCode();
			st->planRoots.push_back(std::move(f.records[i]));
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

// A root job of the program's current generation came back declined: the program's sidecar remembers it, so
// a later session does not build it again.
static void vuAotNoteDeclined(microProgram& prog, const VuAotJob& j)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || j.gen != st->gen || st->gen == 0 || !vuAotSidecarsOn(prog))
		return;
	vuaot::persist::DeclinedRoot d;
	d.pc = j.pc;
	std::memcpy(d.key.data(), &j.key, vuaot::persist::kKeyBytes);
	if (!st->knownDeclined.insert({d.pc, d.key}).second)
		return;
	st->declined.push_back(d);
	st->dirty = true;
}

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

// Registers a region node as the program's (mVU.prog.cur's) block for (pc, key): `rootReal` when microVU is
// compiling the node (a generation's root), else the block the graph already has for the key (whose entry the
// node takes over), else a new one. Its entry becomes `entry`.
static microBlock* vuAotRegisterNode(microVU& mVU, u32 pc, const microRegInfo& key, u8* entry, microBlock* rootReal)
{
	microBlock* b = rootReal;
	if (!b)
	{
		alignas(16) microRegInfo k = key;
		blockCreate(pc / 8);
		b = mVUblocks[pc / 8]->search(mVU, &k);
		if (!b)
		{
			alignas(16) microBlock t;
			std::memset(&t, 0, sizeof(t));
			t.pState = key;
			b = mVUblocks[pc / 8]->add(mVU, &t);
		}
	}
	if (VuAotProgramState* st = mVU.prog.cur->vuaot)
		st->provisional.erase(b);
	b->x86ptrStart = entry;
	b->hostEntry = entry;
	return b;
}

// Aims the B or BL at `at` (placed code) at `to`. A target out of B/BL range (a C helper, from the code
// cache) goes through a veneer placed at the cursor, one per target: x16 is the branch's scratch, as in
// armEmitCall's long form.
static void vuAotPatchBranch(u8* at, const void* to, std::map<const void*, const u8*>& veneers)
{
	u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
	if (vuaot::bg::PatchRel26(insn, at, to))
		return;
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

// What a stub placeholder stands for, in the process that places the code.
static const void* vuAotStubTarget(const vuaot::MvuEmitEnv& e, u32 id)
{
	switch (id)
	{
		case VuAotJob::kPhBailTarget: return reinterpret_cast<const void*>(e.bailTarget);
		case VuAotJob::kPhClearLpState: return e.clearLpState;
		case VuAotJob::kPhEndFlagsB: return e.endFlagsB;
		case VuAotJob::kPhExitEBit: return e.exitEBit;
		case VuAotJob::kPhXgkick: return e.xgkickFn;
		case VuAotJob::kPhCycleBreak: return e.cycleBreak;
		default: return nullptr;
	}
}

// A region of `prog` is in place: the first one of the program counts how long it ran on microVU's code
// before it (of any job).
static void vuAotNoteRegionPlaced(microVU& mVU, const microProgram& prog)
{
	VuAotProgramState* const st = prog.vuaot;
	if (!st || st->hadRegion)
		return;
	st->hadRegion = true;
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
	s_vuAotLastPlaced = static_cast<size_t>(base - mVU.cache);
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

	for (VuAotWBlock* n : j.nodes)
	{
		pxAssertRel(n->hasEntry, "vu-aot: a region node without an entry");
		n->real = vuAotRegisterNode(mVU, n->pc, n->pState, base + n->entryOff, (n == j.root) ? rootReal : nullptr);
	}
	for (const auto& r : j.ranges)
		vuAotAddRange(1, r.first, r.second);

	std::vector<const void*> target(j.ph.size(), nullptr);
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuAotJob::PhStub)
			target[id] = vuAotStubTarget(j.env, j.ph[id].second);
	// microVU's dispatch for the JR/JALR nodes, then the exits, compiled now
	// in the order the region met them.
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

	// The branches to placeholders and to the outside world (through a veneer
	// placed after the code when out of reach).
	std::map<const void*, const u8*> veneers;
	for (const vuaot::bg::Fixup& f : j.fixups)
	{
		if (f.kind != vuaot::bg::Fixup::Site26 && f.kind != vuaot::bg::Fixup::External26)
			continue;
		const void* to = (f.kind == vuaot::bg::Fixup::Site26) ? target[f.id] : reinterpret_cast<const void*>(f.value);
		pxAssertRel(to, "vu-aot: a region branch with no target");
		vuAotPatchBranch(base + f.offset, to, veneers);
	}

	for (auto& b : j.blocks)
		if (!b->snapshot && !b->pending)
			s_vuaot.handles.push_back(std::move(b));
	j.blocks.clear();
	if (j.regions)
		vuAotNoteRegionPlaced(mVU, *mVU.prog.cur);
#ifdef PCSX2_RECOMPILER_TESTS
	if (j.wantRecord && j.accepted)
		s_vuAotRecordSamples.push_back({j.recordCode, j.notPersistable, j.kind == VuAotJob::Generation, j.wMemHomes});
#endif
	s_vuaot.stats.regions += j.regions;
	s_vuaot.stats.regionNodes += j.regionNodes;
	s_vuaot.stats.regionMerged += j.regionMerged;
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

const char* vuaot::live::AotRejectName(AotReject r)
{
	switch (r)
	{
		case kAotOk: return "ok";
		case kAotNoCode: return "plan only";
		case kAotSticky: return "dropped the sticky byte, tripwire set";
		case kAotAddress: return "address outside this process's module or arena";
		case kAotShape: return "operand of the wrong shape";
		case kAotCacheRun: return "code-cache address too wide for its run";
		case kAotSpace: return "no room in the code cache";
		default: return "?";
	}
}

// The MOVZ + MOVK x (width-1) run a fixup names, on one 64-bit register.
static bool vuAotMovRunShape(const u32* insn, u32 width)
{
	if ((insn[0] & 0xFFE00000u) != 0xD2800000u)
		return false;
	const u32 rd = insn[0] & 31;
	for (u32 k = 1; k < width; k++)
		if ((insn[k] & 0xFFE0001Fu) != (0xF2800000u | (k << 21) | rd))
			return false;
	return true;
}

static void vuAotPatchMovRun(u8* at, u32 width, u64 value)
{
	u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
	pxAssertRel(vuAotMovRunShape(insn, width) && armMovRunWidth(value) <= width, "vu-aot: a saved run does not take its address");
	for (u32 k = 0; k < width; k++)
		insn[k] = (insn[k] & ~(0xFFFFu << 5)) | (static_cast<u32>((value >> (16 * k)) & 0xFFFFu) << 5);
}

// The address an image or data-arena fixup stands for in this process, or null.
static const void* vuAotResolveRegion(const vuaot::persist::Fixup& f)
{
	const u8 region = f.target == vuaot::persist::kTargetImage ? mVUPersist::kTargetImage : mVUPersist::kTargetData;
	const void* addr = reinterpret_cast<const void*>(mVUPersist::RegionBase(region) + static_cast<u64>(f.value));
	return mVUPersist::RegionOf(addr) == region ? addr : nullptr;
}

// Whether a record's code can go in, in this process, under the live tripwire: every operand where its fixup
// says and of that shape, every image and arena address inside this process's, wide enough for its run, every
// stub known. Nothing is written. (The read set is checked by the caller; the parser has checked the shape of
// the tables.)
static vuaot::live::AotReject vuAotCheckRecord(microVU& mVU, const vuaot::persist::Record& r, const vuaot::MvuEmitEnv& env)
{
	namespace ps = vuaot::persist;
	if (!r.HasCode())
		return vuaot::live::kAotNoCode;
	// A region that dropped the sticky STATUS byte runs only while no sticky reader is known: the
	// image scan has run before this (mVUvuAotOnNewProgram), so a reader in it has set the tripwire.
	if ((r.flags & ps::kStickyDropped) && s_trip.tripped.load(std::memory_order_acquire))
		return vuaot::live::kAotSticky;
	const ps::RegionCode& c = r.code;
	std::vector<u32> words(c.code.size() / 4);
	std::memcpy(words.data(), c.code.data(), words.size() * 4);
	const u8* const cacheLo = mVU.cache;
	const u8* const cacheHi = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	const u32 cacheWidth = std::max(armMovRunWidth(reinterpret_cast<u64>(cacheLo)), armMovRunWidth(reinterpret_cast<u64>(cacheHi - 1)));
	for (const ps::Fixup& f : c.fixups)
	{
		const u32* insn = words.data() + f.offset / 4;
		if (f.form == ps::kFormMovRun && !vuAotMovRunShape(insn, f.width))
			return vuaot::live::kAotShape;
		if (f.form == ps::kFormRel26 && (*insn & 0x7C000000u) != 0x14000000u)
			return vuaot::live::kAotShape;
		switch (f.target)
		{
			case ps::kTargetPlaceholder:
			{
				const ps::Placeholder& ph = c.ph[f.index];
				if (ph.kind == ps::kPhStub && (ph.index >= VuAotJob::kPhStubs || !vuAotStubTarget(env, ph.index)))
					return vuaot::live::kAotShape;
				break;
			}
			case ps::kTargetImage:
			case ps::kTargetData:
			{
				const void* addr = vuAotResolveRegion(f);
				if (!addr || (f.form == ps::kFormMovRun && armMovRunWidth(reinterpret_cast<u64>(addr)) > f.width))
					return vuaot::live::kAotAddress;
				break;
			}
			case ps::kTargetBlockRef:
				if (cacheWidth > f.width)
					return vuaot::live::kAotCacheRun;
				break;
			default:
				break;
		}
	}
	return vuaot::live::kAotOk;
}

// The entry of the block of `prog`'s graph whose key is exactly `key`, else the one microVU finds or compiles
// for it (the region linked to that exact block in the session that saved it).
static void* vuAotBlockRefEntry(microVU& mVU, microProgram& prog, u32 pc, const u8* key)
{
	void* entry = nullptr;
	if (microBlockManager* m = prog.block[pc / 8])
		m->forEachBlock([&](microBlock& b) {
			if (!entry && b.hostEntry && std::memcmp(&b.pState, key, sizeof(microRegInfo)) == 0)
				entry = b.hostEntry;
		});
	if (entry)
		return entry;
	alignas(16) microRegInfo k;
	std::memcpy(&k, key, sizeof(k));
	return mVUblockFetch(mVU, pc, reinterpret_cast<uptr>(&k));
}

// Puts `recs` (the generation first, then roots, in the order they were published) in place for `prog`, which
// mVUsearchProg has just created or hydrated and nothing names yet (no quick slot, no jump cache, no code).
// The program's microVU graph (from its payload) is set aside, as a generation's publication does; every
// record's code is copied in and its nodes registered; then, with every node of every record in the graph,
// each record is linked: its jump tails and exits compiled by microVU (or found among the nodes), its links to
// earlier records' nodes and to the graph's blocks looked up, every placeholder, image, arena, program and
// handle operand patched. Nothing runs between the copy and the last patch.
static void vuAotInstall(microVU& mVU, microProgram& prog, VuAotProgramState& st,
	const std::vector<std::shared_ptr<const vuaot::persist::Record>>& recs)
{
	namespace ps = vuaot::persist;
	const u64 ins0 = vuAotReadIns();
	const microRegInfo savedLp = mVU.prog.lpState;
	pxAssert(mVU.prog.cur == &prog);
	for (u32 i = 0; i < mVU.progSize / 2; i++)
		pxAssertRel(mVU.prog.quick[i].prog != &prog, "vu-aot: an install into a program a quick slot already holds");

	u32 setAside = 0;
	for (u32 i = 0; i < mProgSize / 2; i++)
	{
		if (!prog.block[i])
			continue;
		prog.block[i]->forEachBlock([&](microBlock&) { setAside++; });
		st.graveyard.push_back(prog.block[i]);
		prog.block[i] = nullptr;
	}
	s_vuaot.stats.blocksSetAside += setAside;
	st.gen = 1;
	st.runsAtTier = vuAotTotalRuns(prog);
	st.checkBlock = vuAotTotalRuns(prog) >> kCheckShift;
	st.missesSeen = s_vuaot.jumpMisses;
	st.tiers++;
	if (st.tiers > vuaot::live::kMaxRetiers)
		st.settled = true;
	// What the program saves from now on: these records, then whatever is published into this generation.
	st.published = recs;
	st.dirty = false;
	st.publishRuns = vuAotTotalRuns(prog);

	const bool wasOpen = armAsm != nullptr;
	if (wasOpen)
		mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);

	struct Placed
	{
		u8* base = nullptr;
		std::vector<microBlock*> real;
		std::vector<VuAotWBlock*> handle;
	};
	std::vector<Placed> placed(recs.size());
	for (size_t ri = 0; ri < recs.size(); ri++)
	{
		const ps::Record& r = *recs[ri];
		if (!r.HasCode())
			continue;
		const ps::RegionCode& c = r.code;
		if (r.flags & ps::kStickyDropped)
			s_trip.dropLive.store(true, std::memory_order_release);
		Placed& p = placed[ri];
		p.base = armGetCurrentCodePointer();
		s_vuAotLastPlaced = static_cast<size_t>(p.base - mVU.cache);
		{
			vixl::CodeBufferCheckScope scope(armAsm, c.code.size(), vixl::CodeBufferCheckScope::kReserveBufferSpace,
				vixl::CodeBufferCheckScope::kNoAssert);
			armAsm->GetBuffer()->EmitData(c.code.data(), c.code.size());
		}
		for (const ps::Fixup& f : c.fixups)
			if (f.target == ps::kTargetIntra)
				vuAotPatchMovRun(p.base + f.offset, 4, reinterpret_cast<u64>(p.base + f.value));
		for (const ps::CodeNode& n : c.nodes)
		{
			auto w = std::make_unique<VuAotWBlock>();
			std::memcpy(&w->pState, c.keys[n.key].data(), sizeof(microRegInfo));
			w->pc = n.pc;
			w->hasEntry = true;
			w->entryOff = n.entryOff;
			w->real = vuAotRegisterNode(mVU, n.pc, w->pState, p.base + n.entryOff, nullptr);
			p.real.push_back(w->real);
			p.handle.push_back(w.get());
			s_vuaot.handles.push_back(std::move(w));
		}
		for (const ps::CodeRange& rg : c.ranges)
			vuAotAddRange(1, rg.start, rg.last);
		s_vuaot.stats.aotRecords++;
		s_vuaot.stats.aotRegions += c.regions;
		s_vuaot.stats.aotBytes += c.code.size();
	}

	const vuaot::MvuEmitEnv env = mVUbuildEmitEnv(mVU);
	for (size_t ri = 0; ri < recs.size(); ri++)
	{
		const ps::Record& r = *recs[ri];
		if (!r.HasCode())
			continue;
		const ps::RegionCode& c = r.code;
		Placed& p = placed[ri];
		// Placeholders, in vuAotPlace's order: stubs, jump tails, exits, earlier records' nodes.
		std::vector<const void*> target(c.ph.size(), nullptr);
		for (size_t id = 0; id < c.ph.size(); id++)
			if (c.ph[id].kind == ps::kPhStub)
				target[id] = vuAotStubTarget(env, c.ph[id].index);
		for (size_t id = 0; id < c.ph.size(); id++)
			if (c.ph[id].kind == ps::kPhTail)
			{
				const ps::CodeTail& t = c.tails[c.ph[id].index];
				target[id] = armGetCurrentCodePointer();
				vuAotEmitJumpTail(1, p.real[t.node], c.keys[t.key].data());
			}
		for (size_t id = 0; id < c.ph.size(); id++)
			if (c.ph[id].kind == ps::kPhExit)
			{
				const ps::BlockAt& x = c.exits[c.ph[id].index];
				alignas(16) microRegInfo k;
				std::memcpy(&k, c.keys[x.key].data(), sizeof(k));
				target[id] = mVUblockFetch(mVU, x.pc, reinterpret_cast<uptr>(&k));
			}
		for (size_t id = 0; id < c.ph.size(); id++)
			if (c.ph[id].kind == ps::kPhPending)
			{
				const ps::BlockAt& x = c.pending[c.ph[id].index];
				alignas(16) microRegInfo k;
				std::memcpy(&k, c.keys[x.key].data(), sizeof(k));
				target[id] = mVUblockFetch(mVU, x.pc, reinterpret_cast<uptr>(&k));
			}
		std::vector<void*> refs(c.blockRefs.size(), nullptr);
		for (size_t i = 0; i < c.blockRefs.size(); i++)
			refs[i] = vuAotBlockRefEntry(mVU, prog, c.blockRefs[i].pc, c.keys[c.blockRefs[i].key].data());

		std::map<const void*, const u8*> veneers;
		for (const ps::Fixup& f : c.fixups)
		{
			u8* const at = p.base + f.offset;
			const void* to = nullptr;
			switch (f.target)
			{
				case ps::kTargetIntra: continue; // (done at the copy)
				case ps::kTargetPlaceholder: to = target[f.index]; break;
				case ps::kTargetImage:
				case ps::kTargetData: to = vuAotResolveRegion(f); break;
				case ps::kTargetBlockRef: to = refs[f.index]; break;
				case ps::kTargetProgram: to = &prog; break;
				case ps::kTargetNodeHandle: to = p.handle[f.index]; break;
			}
			pxAssertRel(to, "vu-aot: a saved region operand with no target");
			if (f.form == ps::kFormMovRun)
				vuAotPatchMovRun(at, f.width, reinterpret_cast<u64>(to));
			else if (f.form == ps::kFormRel26)
				vuAotPatchBranch(at, to, veneers);
			else
			{
				const u64 v = reinterpret_cast<u64>(to);
				std::memcpy(armGetWritableCodePtr(at), &v, sizeof(v));
			}
		}
	}

	// The program joins the list of every start PC it holds a block for. A dispatch finds its program
	// by walking the list at its start PC and comparing ranges (mVUsearchProg), and only a search adds
	// a program to a list. An installed program's regions take its jumps inside their code, so none of
	// its PCs has been searched; with the lists empty, a dispatch after a micro-memory write that misses
	// the program's ranges would hash the changed image and compile the same code again as a new program.
	// The range comparison keeps the lists exact: a program whose ranges no longer match is skipped.
	for (u32 i = 0; i < mVU.progSize / 2; i++)
		if (prog.block[i])
			mVUdequePushUnique(mVU.prog.prog[i], &prog);
	mVUcloseCodeCache(mVU);
	if (wasOpen)
		mVUopenCodeCache(mVU);
	// (the compiles above leave their keys in the dispatcher's carried state)
	mVU.prog.lpState = savedLp;
	s_vuaot.stats.aotPrograms++;
	vuAotNoteRegionPlaced(mVU, prog);
	s_vuaot.stats.installIns += vuAotReadIns() - ins0;
	DevCon.WriteLn(Color_StrongBlue, "vu-aot: prog[%03d] installed %zu saved records (%u microVU blocks set aside)", prog.idx, recs.size(), setAside);
}

// A program created or hydrated with its sidecar loaded and its generation's read set matching micro memory:
// installs the saved code when the generation's record can go in (its root records that cannot become plans,
// sent to the worker once the rest is in). Returns false, with nothing done, when the generation's cannot:
// the caller sends the plans instead. Records whose read set no longer matches are marked dropped by the
// caller (kind 0xff).
static bool vuAotTryInstall(microVU& mVU, microProgram& prog, VuAotProgramState& st, vuaot::persist::File& f)
{
	namespace ps = vuaot::persist;
	if (!s_vuAotInstall)
		return false;
	const vuaot::MvuEmitEnv env = mVUbuildEmitEnv(mVU);
	const ps::Record& gen = f.records.front();
	// A generation the builder declined placed no code (its publication set the graph aside); its roots may have.
	const bool genDeclined = !(gen.flags & ps::kAccepted);
	if (!genDeclined)
	{
		const vuaot::live::AotReject why = vuAotCheckRecord(mVU, gen, env);
		if (why != vuaot::live::kAotOk)
		{
			s_vuaot.stats.aotRejectedBy[why]++;
			return false;
		}
	}
	std::vector<size_t> install, plan;
	std::vector<vuaot::live::AotReject> whys;
	size_t bytes = gen.code.code.size();
	for (size_t i = 1; i < f.records.size(); i++)
	{
		const ps::Record& r = f.records[i];
		if (r.kind != ps::kRoot)
			continue;
		const vuaot::live::AotReject why = vuAotCheckRecord(mVU, r, env);
		if (why == vuaot::live::kAotOk)
		{
			install.push_back(i);
			bytes += r.code.code.size();
		}
		else
		{
			plan.push_back(i);
			whys.push_back(why);
		}
	}
	// Room for every record, its exits, jump tails and veneers, as a compile session may use it.
	const u8* const limit = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	if (armGetCurrentCodePointer() + bytes + (2u << 20) >= limit)
	{
		s_vuaot.stats.aotRejectedBy[vuaot::live::kAotSpace]++;
		return false;
	}
	for (vuaot::live::AotReject why : whys)
		s_vuaot.stats.aotRejectedBy[why]++;
	std::vector<std::shared_ptr<const ps::Record>> recs;
	recs.push_back(std::make_shared<const ps::Record>(std::move(f.records.front())));
	for (size_t i : install)
		recs.push_back(std::make_shared<const ps::Record>(std::move(f.records[i])));
	std::vector<ps::Record> toPlan;
	for (size_t i : plan)
	{
		f.records[i].DropCode();
		toPlan.push_back(std::move(f.records[i]));
	}
	vuAotInstall(mVU, prog, st, recs);
	s_vuaot.stats.aotRootsToPlan += toPlan.size();
	if (!toPlan.empty())
	{
		st.planRoots = std::move(toPlan);
		vuAotSubmitPlanRoots(mVU, prog, st);
	}
	return true;
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
		// reader of it and none was known when the job was made. That must still hold: the
		// tripwire not set, and no reader in the image a region runs under (a scan that finds
		// one sets the tripwire).
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
				vuAotNoteDeclined(*j->prog, *j);
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
// Also puts the quick slot for the slow path's address back to `prog`, which the search
// has just resolved from micro memory. A root publication empties every quick slot of its
// program, and the JR slow path (mVUcompileJIT) copies this slot's program into the jump
// cache entry it fills. With the slot empty, that entry would name no program, and the
// inline probe in front of the JR (normJumpCompile) takes an entry whose program equals the
// slot's without checking for null: every later JR there would jump to `prog`'s code
// whenever the slot is empty (as it is after any micro-memory write over that address),
// whatever code is in memory then.
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
// its target to a program. Counts the entry on that program. A program the dispatcher never
// enters (an overlay a stub program jumps into) has no dispatch count, so jumps decide its
// tier-up; one the dispatcher does enter keeps its dispatch threshold, since tiering it from
// a jump into its middle would root its first compile there and split the region.
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
		// Only when jumps dominate the program's entries (16 to 1 over dispatches).
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

	// A root a loaded sidecar says the builder declined in this generation: no job. microVU compiles it, and
	// what it reaches, as its own, as after any root job (and as after this one's, which came back empty).
	if (!st.knownDeclined.empty())
	{
		std::pair<u32, vuaot::persist::Key> k;
		k.first = b.startPC;
		std::memcpy(k.second.data(), b.entryState, vuaot::persist::kKeyBytes);
		if (st.knownDeclined.count(k))
		{
			s_vuaot.stats.declinedSkipped++;
			st.provisional.insert(static_cast<const microBlock*>(env.rootBlock));
			s_vuaot.plainRootDepth = s_vuaot.compileDepth;
			return false;
		}
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
		{
			vuAotNoteDeclined(*mVU.prog.cur, *j);
			return false;
		}
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
	// pcsx2-vurunner --vuaot-region-bg: the region hook through the worker. A job is
	// made from the block microVU is compiling and the graph around it, run on the
	// worker and waited for, and its code copied in where microVU is compiling. The
	// result is the synchronous region, produced by the live tier's background path.
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
