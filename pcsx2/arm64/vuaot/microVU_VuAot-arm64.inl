// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// vu-aot live tier: tier-up policy and the glue between microVU's chunk store
// and vu-aot's regions. Included at the end of microVU-arm64.cpp (it needs
// microVU's TU-local types and helpers); the policy constants and the part of
// the interface other code may use are in vuaot/VuAotLive.h.
//
// The unit is a routine: a root block and what is compiled from it, kept by
// the chunk store under a head (mVUChunkHead) whatever address it is uploaded
// at. The tier counts the runs of every copy of a routine on its head. A hot
// routine gets one job: the worker builds its regions from a copy of micro
// memory. The VU1 thread then compiles the routine once more, in block tables
// it throws away, with the job's region code copied in where the root is
// compiled; the chunk store records that compile as the head's tiered body.
// From then on every copy of the routine is a placement of that body: entry
// stubs and slots of its own, the code shared.
//
// Threads. Everything here runs on the thread that executes VU1 (the EE thread,
// or the MTVU thread), except VuAotJob::Run and what it calls, which run on the
// tier's worker thread (vuaot::bg::Worker). A job holds copies of everything it
// reads and writes only its own memory, so the two threads share nothing but the
// worker's queue. A finished job is built into a body on the VU1 thread at a
// slow path microVU already takes (a dispatch that missed the quick path, or a
// JR/JALR the jump cache missed), after checking that every micro-memory word
// the job read is unchanged. A reset (settings change, savestate load, cache
// exhaustion) arrives from a thread that has waited for VU1 to go idle; it
// bumps the epoch, and whatever a job made before it is dropped unread.

// (The vu-aot headers are included at the top of microVU-arm64.cpp: they cannot
// follow microVU's macros.)

#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

// A job that is dropped (queue full), declined by a region build that wrapped,
// or not placed is asked for again after another kTierUpRuns runs, this many
// times at most per routine.
static constexpr u32 kMaxGenAttempts = 4;
// Tiered bodies and "no region" marks one head keeps: routines that begin
// alike and go on differently each need their own.
static constexpr size_t kMaxBodiesPerHead = 8;
static_assert(kMaxBodiesPerHead == mVUChunkPack::kMaxTieredPerRoot, "the disk pack brings back as many of the tier's bodies as a head keeps");

// A region node as a job sees it, and the block it became when the job's code
// was placed.
struct VuAotWBlock
{
	alignas(16) microRegInfo pState;
	u32 pc = 0;
	// Its entry, as an offset into the job's code.
	bool hasEntry = false;
	u32 entryOff = 0;
	microBlock* real = nullptr;
};


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

// The regions of one routine, built on the worker from the routine's root on
// an empty block graph.
struct VuAotJob final : vuaot::bg::Job
{
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
	// What each placement slot the region's code uses holds, in the order the
	// builder asked for them.
	enum SlotKind : u8
	{
		SlotBlock,
		SlotZero,
		SlotProgram,
	};

	// Identity (the worker never dereferences `head`).
	u64 epoch = 0;
	mVUChunkHead* head = nullptr;
	bool retier = false;
	bool viaJump = false;
	u32 pc = 0;
	alignas(16) microRegInfo key;
	u64 requestedAt = 0;   // VU1 dispatches so far
	u64 missesAtRequest = 0;

	std::unique_ptr<u8[]> micro; // micro memory when the job was made (kept: the build compares against it)
	std::map<u32, std::vector<u32>> jumpTargets;
	vuaot::MvuEmitEnv env;      // microVU's values; the worker swaps in its own callbacks
	bool flagHack = false;      // block search follows it (microBlockManager::search)
	const u8* cacheLo = nullptr; // microVU's code cache, where the region lands
	const u8* cacheHi = nullptr;
	VuAotRegionOptions opts;
	std::vector<std::unique_ptr<VuAotWBlock>> blocks; // the job's nodes

	bool accepted = false;
	std::string why;
	vuaot::MvuBlockTrace rootTrace; // the port's first pass of the root
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
	std::vector<std::pair<PhKind, u32>> ph; // per placeholder id
	std::vector<std::pair<SlotKind, VuAotWBlock*>> slots;
	std::vector<u8> readMask;               // per micro-memory word: the compile read it
	// The regions run on from the last pair of micro memory to pair 0: at
	// another address they would be other code.
	bool wrapped = false;
	u32 regions = 0, regionNodes = 0, regionExits = 0, regionMerged = 0;
	u64 regionBytes = 0;
	// The flag-reader census of the compile, and whether it dropped the sticky STATUS byte.
	u32 statusReaders = 0, stickyReaders = 0, stickyDivReaders = 0, stickyDropped = 0, stickyKept = 0;
	std::map<std::string, u32> declined;
	u64 workerIns = 0;

	void Run() override;
};

namespace
{
	struct VuAotLive
	{
		bool enabled = false;
		// The counting lookup asked for the full search (set by the fast path,
		// consumed by mVUvuAotAfterSearch on the same dispatch), and whether it
		// counted the dispatch before asking.
		bool pending = false;
		bool pendingCounted = false;
		u32 tierUpRuns = vuaot::live::kTierUpRuns;
		// ... and for a run counted on the JR/JALR slow path.
		u32 jumpTierUpRuns = vuaot::live::kJumpTierUpRuns;
		// JR/JALR the jump cache missed, VU1.
		u64 jumpMisses = 0;
		// Nesting of mVUcompile for VU1.
		int compileDepth = 0;
		vuaot::live::Stats stats;

		// Deterministic mode (tests, pcsx2-eerunner --vuaot-sync): every job is
		// waited for and built where it was asked for.
		bool synchronous = false;
		// Bumped at every reset: a job from an earlier epoch is dropped unread.
		u64 epoch = 1;
		// VU1 dispatches while the tier is on (build lag).
		u64 dispatches = 0;
		// A routine's tiered body is being compiled: the hooks are handed out,
		// and the root's compile places `prepared`.
		bool building = false;
		VuAotJob* prepared = nullptr;
		std::unique_ptr<vuaot::bg::Worker> worker;
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
	if (s_trip.tripped.exchange(true))
		return;
	s_trip.cause.store(static_cast<u32>(vuaot::live::TripCause::Remembered), std::memory_order_relaxed);
	s_trip.tripsBy[static_cast<u32>(vuaot::live::TripCause::Remembered)].fetch_add(1, std::memory_order_relaxed);
	if (s_trip.dropLive.load())
		s_trip.rebuildDue.store(true, std::memory_order_release);
	Console.WriteLn(Color_StrongBlue, "vu-aot: sticky STATUS tripwire set: this title set it off in an earlier session.");
}

#ifdef PCSX2_RECOMPILER_TESTS
static std::atomic<u32> s_vuAotWorkerDelayUs{0};
#endif


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
		case TripCause::OutsideRead: return "VU0 read VU1's STATUS register";
		case TripCause::Forced: return "forced";
		case TripCause::Remembered: return "this title set it off in an earlier session";
		default: return "?";
	}
}

#ifdef PCSX2_RECOMPILER_TESTS
static bool vuAotMutated(int m) { return (s_trip.mutation.load(std::memory_order_relaxed) >> m) & 1; }
// vuaot::live::TripAtNextDroppedBodyPlacement.
static std::atomic<bool> s_vuAotTripAtPlacement{false};
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
	if (!s_vuaot.enabled || s_trip.tripped.exchange(true))
		return;
	s_trip.cause.store(static_cast<u32>(why), std::memory_order_relaxed);
	s_trip.trips.fetch_add(1, std::memory_order_relaxed);
	s_trip.tripsBy[static_cast<u32>(why)].fetch_add(1, std::memory_order_relaxed);
	const bool rebuild = s_trip.dropLive.load() && !vuAotMutated(vuaot::live::kTripMutNoRebuild);
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

// A tiered body that leaves the sticky byte of STATUS out is placed only while
// no reader of it is known.
bool mVUvuAotBodyAllowed(bool droppedSticky)
{
	return !droppedSticky || !s_trip.tripped.load(std::memory_order_acquire) || vuAotMutated(vuaot::live::kTripMutPlace);
}

void mVUvuAotOnBodyPlaced(microVU&, bool droppedSticky)
{
	if (!droppedSticky)
		return;
#ifdef PCSX2_RECOMPILER_TESTS
	if (s_vuAotTripAtPlacement.exchange(false))
		vuAotTrip(vuaot::live::TripCause::Forced);
#endif
	s_trip.dropLive.store(true);
	// A trip from another thread (VU0 reading VU1's STATUS) between the check
	// that allowed this body and the store above saw nothing to rebuild.
	if (s_trip.tripped.load() && !vuAotMutated(vuaot::live::kTripMutNoRebuild))
		s_trip.rebuildDue.store(true, std::memory_order_release);
}

void mVUvuAotOnBodyLoaded(microVU& mVU, mVUChunkHead& head, std::unique_ptr<mVUChunkBody> body)
{
	if (head.tiered.size() >= kMaxBodiesPerHead)
		return;
	body->id = head.nextBodyId++;
	if (!body->declined)
	{
		mVU.chunkStore->tieredBodies++;
		mVU.chunkStore->tieredCodeBytes += body->codeBytes;
		s_vuaot.stats.bodiesLoaded++;
		// (its copies look for new jump targets as those of a body built here do)
		head.next[1] = head.runs[1] + vuaot::live::kRetierCheckMask + 1;
	}
	head.tiered.push_back(std::move(body));
	head.serial++;
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
	void TripAtNextDroppedBodyPlacement() { s_vuAotTripAtPlacement.store(true); }
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
	s_vuaot.pendingCounted = false;
	s_vuaot.compileDepth = 0;

	// Whatever the worker holds or has made belongs to the code this reset
	// throws away. A job still running finishes into its own memory and is
	// dropped when it is collected (its epoch is old).
	s_vuaot.epoch++;
	s_vuaot.prepared = nullptr;
	s_vuaot.building = false;
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
bool mVUvuAotBuilding(microVU& mVU) { return mVU.index == 1 && s_vuaot.building; }
u32 mVUvuAotTierUpRuns() { return s_vuaot.tierUpRuns; }


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
		s.jumpMisses = s_vuaot.jumpMisses;
		s.bailsEntry = microVU1.vuAotBails[0];
		s.bailsLoopHead = microVU1.vuAotBails[1];
		const mVUChunkStore::Stats cs = mVUChunkStore::GetStats();
		s.tieredBodies = cs.tieredBodies;
		s.tieredBodyBytes = cs.tieredCodeBytes;
		s.tieredPlaced = cs.tieredPlaced;
		s.tieredReAimed = cs.tieredReAimed;
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
			"vu-aot live tier: tierups=%llu retiers=%llu retierchecks=%llu tiered_bodies=%llu tiered_body_bytes=%llu "
			"placed=%llu reaimed=%llu targets=%llu jumpmisses=%llu slow_dispatches=%llu "
			"roots=%llu regions=%llu rnodes=%llu rmerged=%llu rexits=%llu rbytes=%llu bails=%llu "
			"jrtierups=%llu jrcounted=%llu xprogtargets=%llu bails_entry=%llu bails_loophead=%llu "
			"status_readers=%llu sticky_readers=%llu sticky_div_readers=%llu sticky_dropped=%llu sticky_kept=%llu "
			"sticky_trips=%llu sticky_trip_resets=%llu sticky_tripped=%d sticky_remembered=%llu",
			(unsigned long long)s.tierUps, (unsigned long long)s.retiers, (unsigned long long)s.retierChecks,
			(unsigned long long)s.tieredBodies, (unsigned long long)s.tieredBodyBytes, (unsigned long long)s.tieredPlaced,
			(unsigned long long)s.tieredReAimed, (unsigned long long)s.targetsRecorded, (unsigned long long)s.jumpMisses,
			(unsigned long long)s.slowDispatches,
			(unsigned long long)s.rootsCompiled, (unsigned long long)s.regions, (unsigned long long)s.regionNodes,
			(unsigned long long)s.regionMerged, (unsigned long long)s.regionExits, (unsigned long long)s.regionBytes,
			(unsigned long long)s.bailCompiles,
			(unsigned long long)s.jumpTierUps, (unsigned long long)s.jumpCounted, (unsigned long long)s.crossProgTargets,
			(unsigned long long)s.bailsEntry, (unsigned long long)s.bailsLoopHead, (unsigned long long)s.statusReaders,
			(unsigned long long)s.stickyReaders, (unsigned long long)s.stickyDivReaders, (unsigned long long)s.stickyDropped,
			(unsigned long long)s.stickyKept, (unsigned long long)s.stickyTrips, (unsigned long long)s.stickyTripResets, StickyTripped() ? 1 : 0,
			(unsigned long long)s.stickyRemembered);
		out = buf;
		if (s.stickyTrips)
		{
			// (which readers set it off, in the order of TripCause; the last one's reason in words)
			std::snprintf(buf, sizeof(buf),
				"\n  sticky tripwire: trips=%llu (image %llu, compile %llu, outside %llu, forced %llu) now %s: %s",
				(unsigned long long)s.stickyTrips, (unsigned long long)s.stickyTripsBy[1], (unsigned long long)s.stickyTripsBy[2],
				(unsigned long long)s.stickyTripsBy[3], (unsigned long long)s.stickyTripsBy[4],
				StickyTripped() ? "set" : "re-armed", TripCauseName(StickyTripCause()));
			out += buf;
		}
		std::snprintf(buf, sizeof(buf),
			"\n  background: sync=%d jobs=%llu dropped=%llu done=%llu built=%llu "
			"declined=%llu discarded: stale=%llu memory=%llu sticky=%llu space=%llu trace=%llu unstorable=%llu | "
			"lag(dispatches) avg=%.1f max=%llu | worker_ins=%llu build_ins=%llu veneers=%llu",
			s_vuaot.synchronous ? 1 : 0, (unsigned long long)s.jobs, (unsigned long long)s.jobsDropped, (unsigned long long)s.jobsDone,
			(unsigned long long)s.built, (unsigned long long)s.jobsDeclined,
			(unsigned long long)s.discardStale, (unsigned long long)s.discardMemory, (unsigned long long)s.discardSticky, (unsigned long long)s.discardSpace, (unsigned long long)s.discardTrace,
			(unsigned long long)s.discardUnstorable,
			s.built ? static_cast<double>(s.lagSum) / s.built : 0.0, (unsigned long long)s.lagMax,
			(unsigned long long)s.workerIns, (unsigned long long)s.buildIns, (unsigned long long)s.veneers);
		out += buf;
		if (s.compileInsTiered || s.compileInsPlain)
		{
			std::snprintf(buf, sizeof(buf), "\n  compile instructions: tiered builds %llu, others %llu",
				(unsigned long long)s.compileInsTiered, (unsigned long long)s.compileInsPlain);
			out += buf;
		}
		for (const auto& d : s_vuaot.stats.regionDeclined)
		{
			std::snprintf(buf, sizeof(buf), "\n  region declined: %s = %u", d.first.c_str(), d.second);
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
		b->hasEntry = true;
		b->entryOff = static_cast<u32>(static_cast<u8*>(entry) - t_vuAotCtx->sink->Base());
	}
	void* vuAotWEntry(void* block)
	{
		VuAotWBlock* b = static_cast<VuAotWBlock*>(block);
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
	u32 vuAotWNodeSlots(u32, void* block)
	{
		VuAotJob& j = *t_vuAotCtx->job;
		const u32 n = static_cast<u32>(j.slots.size());
		j.slots.push_back({VuAotJob::SlotBlock, static_cast<VuAotWBlock*>(block)});
		j.slots.push_back({VuAotJob::SlotZero, nullptr});
		return n;
	}
	u32 vuAotWProgramSlot(u32)
	{
		VuAotJob& j = *t_vuAotCtx->job;
		j.slots.push_back({VuAotJob::SlotProgram, nullptr});
		return static_cast<u32>(j.slots.size() - 1);
	}
	void vuAotWNotePlaceStub(const u8* at) { t_vuAotCtx->sink->Note(vuaot::bg::Fixup::PlaceStub, at); }
	void vuAotWNoteSlotMovz(const u8* at) { t_vuAotCtx->sink->Note(vuaot::bg::Fixup::SlotMovz, at); }

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
	t_vuAotCtx = &ctx;

	readMask.assign(0x4000 / 4, 0);
	vuaot::g_pipeReadMask = readMask.data();

	vuaot::RegionTier tier(1, micro.get());
	opts.To(tier);
	tier.jumpTargets = jumpTargets;

	// No microVU compile to take the root's first pass from, so use the port's
	// own; the build compares it against microVU's when microVU compiles the root.
	vuaot::MvuBlockTrace b;
	{
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
	e.addNodeSlots = &vuAotWNodeSlots;
	e.addProgramSlot = &vuAotWProgramSlot;
	e.notePlaceStub = &vuAotWNotePlaceStub;
	e.noteSlotMovz = &vuAotWNoteSlotMovz;
	e.placeWord = 0;
	// microVU's stubs live in the code cache, where the region will land: a
	// placeholder, patched to a direct branch. A C helper keeps its address and
	// gets armEmitCall's usual form from here (absolute when it is out of reach,
	// which from this buffer it nearly always is; a direct branch is recorded and
	// re-aimed, through a veneer if need be).
	const auto stub = [&](const void* real, u32 id) -> const void* {
		const u8* p = static_cast<const u8*>(real);
		return (p >= cacheLo && p < cacheHi) ? sink->Placeholder(id) : real;
	};
	e.bailTarget = reinterpret_cast<void* (*)(u32, u64*, u32, void*)>(const_cast<void*>(stub(reinterpret_cast<const void*>(env.bailTarget), kPhBailTarget)));
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
	// A block that runs across the end of micro memory, or a compile that read
	// both its last and its first pair.
	for (const auto& r : ranges)
		wrapped = wrapped || r.second < r.first;
	wrapped = wrapped || ((readMask[0x4000 / 4 - 2] || readMask[0x4000 / 4 - 1]) && (readMask[0] || readMask[1]));
	if (accepted && wrapped)
	{
		accepted = false;
		why = "runs across the end of micro memory";
		declined[why]++;
	}
	if (accepted)
	{
		code.assign(sink->Base(), sink->Base() + size);
		fixups = sink->fixups;
		regions = tier.Regions();
		regionNodes = tier.Nodes();
		regionMerged = tier.Merged();
		regionExits = tier.Exits();
		regionBytes = tier.Bytes();
	}
	for (const auto& d : tier.Declined())
		declined[d.first] += d.second;
	statusReaders = tier.StatusReaders();
	stickyReaders = tier.StickyReaders();
	stickyDivReaders = tier.StickyDivReaders();
	stickyDropped = tier.StickyDropped();
	stickyKept = tier.StickyReaderImages();
	workerIns = vuAotReadIns() - ins0;
	s_vuaot.workerIns.fetch_add(workerIns, std::memory_order_relaxed);
}

// The JR/JALR targets microVU recorded for a program: every jumping block
// keeps the targets it reached when the jump table missed (the program each
// resolved to is ours when the target is our own code). By the jumping
// block's start PC (bytes), ascending.
static std::map<u32, std::vector<u32>> vuAotHarvestTargets(microProgram& prog)
{
	std::map<u32, std::vector<u32>> out;
	u32 total = 0;
	for (u32 i = 0; i < mProgSize / 2; i++)
	{
		microBlockManager* m = prog.block[i];
		if (!m)
			continue;
		m->forEachBlock([&](microBlock& b) {
			if (!b.jumpTargets)
				return;
			std::vector<u32>* v = nullptr;
			for (const microJumpTarget& t : *b.jumpTargets)
			{
				if (t.prog != &prog)
				{
					// (A target that resolved to another program is not ours to
					// compile: its code would enter this program's ranges and make it
					// match only while that code is loaded, and a direct edge would skip
					// the jump cache's check that the live program is still that one.)
					s_vuaot.stats.crossProgTargets++;
					continue;
				}
				if (!v)
					v = &out[i * 8];
				const u32 pc = t.pc;
				const auto at = std::lower_bound(v->begin(), v->end(), pc);
				if (at != v->end() && *at == pc)
					continue;
				if (v->size() >= vuaot::live::kMaxTargetsPerJump || total >= vuaot::live::kMaxTargetsPerProgram)
					continue;
				v->insert(at, pc);
				total++;
			}
		});
	}
	return out;
}

static std::shared_ptr<VuAotJob> vuAotMakeJob(microVU& mVU, mVUChunkHead* head, u32 pc, const void* key)
{
	auto j = std::make_shared<VuAotJob>();
	j->epoch = s_vuaot.epoch;
	j->head = head;
	j->pc = pc & 0x3ff8;
	std::memcpy(&j->key, key, sizeof(j->key));
	j->requestedAt = s_vuaot.dispatches;
	j->missesAtRequest = s_vuaot.jumpMisses;
	j->micro.reset(new u8[0x4000]);
	std::memcpy(j->micro.get(), mVU.regs().Micro, 0x4000);
	j->env = mVUbuildEmitEnv(mVU);
	// The job's root is the root of the chunk its code will be compiled into.
	j->env.rootPC = j->pc;
	j->env.flagHack = CHECK_VU_FLAGHACK;
	j->flagHack = CHECK_VU_FLAGHACK;
	j->cacheLo = mVU.cache;
	j->cacheHi = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	// A reader of the sticky bits anywhere in the image the regions are built
	// from sets the tripwire off. One uploaded outside the ranges of the
	// program that runs made no new program, so mVUvuAotOnNewProgram has not
	// seen it.
	if (!s_trip.tripped.load(std::memory_order_relaxed) && !vuAotMutated(vuaot::live::kTripMutProgramImage) &&
		vuaot::RegionTier::MicroReadsSticky(j->micro.get(), 1))
		vuAotTrip(vuaot::live::TripCause::ProgramImage);
	// Regions drop the sticky byte of STATUS only while no sticky reader is known.
	j->opts.dropSticky = !s_trip.tripped.load(std::memory_order_acquire) || vuAotMutated(vuaot::live::kTripMutJobOption);
	return j;
}

static bool vuAotSubmit(const std::shared_ptr<VuAotJob>& j)
{
	if (!vuAotWorker().Submit(j))
	{
		s_vuaot.stats.jobsDropped++;
		return false;
	}
	s_vuaot.stats.jobs++;
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
	s_vuaot.stats.stickyKept += j.stickyKept;
}

// Whether the routine at `pc` is the one the job compiled: every micro-memory
// word the job read, at the same distance from `pc` as it was from the job's
// root, is inside micro memory and is what the job read. The routine may have
// been uploaded somewhere else while the worker ran.
static bool vuAotJobFits(microVU& mVU, const VuAotJob& j, u32 pc)
{
	const u32* live = reinterpret_cast<const u32*>(mVU.regs().Micro);
	const u32* was = reinterpret_cast<const u32*>(j.micro.get());
	const s32 words = static_cast<s32>(j.readMask.size());
	const s32 shift = (static_cast<s32>(pc) - static_cast<s32>(j.pc)) / 4;
	for (s32 w = 0; w < words; w++)
	{
		if (!j.readMask[w])
			continue;
		const s32 at = w + shift;
		if (at < 0 || at >= words || live[at] != was[w])
			return false;
	}
	return true;
}

// Moves a finished job to the copy of its routine at `pc` (vuAotJobFits): every
// micro-memory address it holds shifts by the distance. Its code holds none.
static void vuAotRebase(VuAotJob& j, u32 pc)
{
	if (pc == j.pc)
		return;
	const u32 d = pc - j.pc;
	const auto at = [d](u32 a) { return (a + d) & 0x3ff8; };
	for (auto& b : j.blocks)
		b->pc = at(b->pc);
	for (VuAotJob::Exit& x : j.exits)
		x.pc = at(x.pc);
	for (auto& r : j.ranges)
		r = {at(r.first), at(r.second)};
	std::map<u32, std::vector<u32>> targets;
	for (const auto& jt : j.jumpTargets)
	{
		std::vector<u32>& v = targets[at(jt.first)];
		for (u32 t : jt.second)
			v.push_back(at(t));
	}
	j.jumpTargets = std::move(targets);
	const u32 words = static_cast<u32>(j.readMask.size());
	std::vector<u8> mask(words, 0);
	std::unique_ptr<u8[]> micro(new u8[words * 4]());
	for (u32 w = 0; w < words; w++)
		if (j.readMask[w])
		{
			const u32 to = (w + d / 4) & (words - 1);
			mask[to] = 1;
			std::memcpy(micro.get() + to * 4, j.micro.get() + w * 4, 4);
		}
	j.readMask = std::move(mask);
	j.micro = std::move(micro);
	j.rootTrace.startPC = at(j.rootTrace.startPC);
	for (vuaot::MvuOpTrace& op : j.rootTrace.ops)
	{
		op.pc = at(op.pc);
		if (op.doXGKICK)
			op.XGKICKPC = at(op.XGKICKPC);
	}
	j.pc = pc;
}

// Room for the job's code in the code cache, as a compile session may use it
// (up to the cache's physical end; the reset comes after the session).
static bool vuAotHasRoom(microVU& mVU, const VuAotJob& j)
{
	const u8* const limit = mVU.prog.x86end + (mVUcacheSafeZone * _1mb);
	return armGetCurrentCodePointer() + j.code.size() + (1u << 20) < limit;
}

// Registers a region node as the block of mVU.prog.cur for (pc, key): `rootReal`
// when microVU is compiling the node (the job's root), else the block the graph
// has for the key (whose entry the node takes over), else a new one. Its entry
// becomes `entry`.
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
	b->x86ptrStart = entry;
	b->hostEntry = entry;
	b->extent = 0; // vu-aot's code; its size is not recorded
	return b;
}


// Aims the B or BL at `at` (placed code) at `to`. A target out of B/BL range (a C helper, from the code
// cache) goes through a veneer placed at the cursor, one per target: x16 is the branch's scratch, as in
// armEmitCall's long form.
static void vuAotPatchBranch(u8* at, const void* to, std::map<const void*, const u8*>& veneers)
{
	u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
	if (vuaot::bg::PatchRel26(insn, at, to))
	{
		mVUPersist::NoteBranch(microVU1, at, to);
		return;
	}
	auto it = veneers.find(to);
	if (it == veneers.end())
	{
		const u8* v = armGetCurrentCodePointer();
		// (through the address helper, so the chunk store's recorder sees it)
		armMoveAddressToReg(a64::x16, to);
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

// Places a job's code at the cursor, inside the compile of its root (the chunk
// being compiled is the one its code becomes part of): its nodes become blocks
// of mVU.prog.cur for their keys, each with the two slots its code uses; its
// entry stubs take the chunk's place word; its ranges join the program's; its
// jump tails and exits are microVU's own code, compiled now; and every
// placeholder is patched to what it stands for. `rootReal`: the root's
// microBlock.
static void vuAotPlace(microVU& mVU, VuAotJob& j, microBlock* rootReal)
{
	u8* const base = armGetCurrentCodePointer();
	pxAssertRel(base == rootReal->hostEntry, "vu-aot: a region placed away from its root's entry");
	{
		vixl::CodeBufferCheckScope scope(armAsm, j.code.size(), vixl::CodeBufferCheckScope::kReserveBufferSpace,
			vixl::CodeBufferCheckScope::kNoAssert);
		armAsm->GetBuffer()->EmitData(j.code.data(), j.code.size());
	}

	for (VuAotWBlock* n : j.nodes)
	{
		pxAssertRel(n->hasEntry, "vu-aot: a region node without an entry");
		n->real = vuAotRegisterNode(mVU, n->pc, n->pState, base + n->entryOff, (n == j.root) ? rootReal : nullptr);
		mVU.chunk.blocks.push_back({n->real, base + n->entryOff, n->pc, base + n->entryOff + mVUrootStubBytes});
	}
	// The job numbered its slots from 0; the chunk's next free one is where they start.
	const u32 firstSlot = static_cast<u32>(mVU.chunk.slots.size());
	for (const auto& s : j.slots)
	{
		if (s.first == VuAotJob::SlotBlock)
			mVUchunkAddSlot(mVU, s.second->real, nullptr, 0, nullptr);
		else
			mVUchunkAddDataSlot(mVU, s.first == VuAotJob::SlotProgram ? mVUslotProgram : mVUslotZero);
	}
	const u32 placeWord = mVUplaceWord(mVU.compileRoot, mVU.chunk.slotBase);
	for (const vuaot::bg::Fixup& f : j.fixups)
	{
		u32* const insn = reinterpret_cast<u32*>(armGetWritableCodePtr(base + f.offset));
		if (f.kind == vuaot::bg::Fixup::IntraAbs)
		{
			pxAssertRel(vuaot::bg::PatchAbsMov(insn, reinterpret_cast<u64>(base + f.value)), "vu-aot: an address in a region is not where its fixup says");
			mVUPersist::NoteMovRun(mVU, base + f.offset, 4, base + f.value);
		}
		else if (f.kind == vuaot::bg::Fixup::OutsideAbs)
			mVUPersist::NoteMovRun(mVU, base + f.offset, f.id, reinterpret_cast<const void*>(f.value));
		else if (f.kind == vuaot::bg::Fixup::PlaceStub)
			pxAssertRel(vuaot::bg::PatchPlaceStub(insn, placeWord), "vu-aot: a region entry stub is not where its fixup says");
		else if (f.kind == vuaot::bg::Fixup::SlotMovz)
			pxAssertRel(vuaot::bg::PatchSlotMovz(insn, firstSlot), "vu-aot: a region slot number is not where its fixup says");
	}
	for (const auto& r : j.ranges)
		vuAotAddRange(1, r.first, r.second);
	// What the job read is part of what the chunk read.
	if (mVU.chunk.recording)
		for (u32 w = 0; w < j.readMask.size(); w++)
			mVU.chunk.readMark[w] |= j.readMask[w];
	// A reader of the sticky bits in the regions is as good as one microVU compiled.
	if (j.stickyReaders)
		mVUnoteStatusReader(mVU, 0xFC0);

	std::vector<const void*> target(j.ph.size(), nullptr);
	for (size_t id = 0; id < j.ph.size(); id++)
		if (j.ph[id].first == VuAotJob::PhStub)
			target[id] = vuAotStubTarget(j.env, j.ph[id].second);
	// microVU's dispatch for the JR/JALR nodes, then the exits, compiled now
	// in the order the region met them. An exit is a block of the same chunk
	// and is entered past its stub; a block that was there before is entered
	// at its stub, by code only this placement can then run.
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
			void* const entry = mVUblockFetch(mVU, x.pc, reinterpret_cast<uptr>(&x.key));
			const u8* const body = mVUchunkBody(mVU, entry);
			if (!body)
				mVU.chunk.unstorable = true;
			target[id] = body ? body : entry;
		}
	for (const vuaot::bg::Fixup& f : j.fixups)
		if (f.kind == vuaot::bg::Fixup::SiteAbs)
		{
			pxAssertRel(vuaot::bg::PatchAbsMov(reinterpret_cast<u32*>(armGetWritableCodePtr(base + f.offset)), reinterpret_cast<u64>(target[f.id])),
				"vu-aot: an address in a region is not where its fixup says");
			mVUPersist::NoteMovRun(mVU, base + f.offset, 4, target[f.id]);
		}

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

	s_vuaot.stats.regions += j.regions;
	s_vuaot.stats.regionNodes += j.regionNodes;
	s_vuaot.stats.regionMerged += j.regionMerged;
	s_vuaot.stats.regionExits += j.regionExits;
	s_vuaot.stats.regionBytes += j.regionBytes;
}

// Collects the worker's finished jobs onto their heads. A job the builder
// declined leaves a mark on the head: no region for this root over the words
// the job read. Any other waits there for the next copy of the routine it
// compiled to be entered (vuAotConsider).
static void vuAotCollectFinished()
{
	if (!s_vuaot.worker || !s_vuaot.worker->CompletedCount())
		return;
	std::vector<std::shared_ptr<vuaot::bg::Job>> done;
	s_vuaot.worker->TakeCompleted(done);
	for (auto& jb : done)
	{
		auto j = std::static_pointer_cast<VuAotJob>(jb);
		vuAotCollect(*j);
		if (j->epoch != s_vuaot.epoch || !j->head)
		{
			s_vuaot.stats.discardStale++;
			continue;
		}
		mVUChunkHead& head = *j->head;
		// (every copy of the routine looks at the head again at its next entry)
		head.serial++;
		if (j->accepted)
		{
			// It waits for a copy it fits; meanwhile a copy of another variant
			// may ask for a job of its own. The oldest waiting job makes room.
			head.outstanding = false;
			head.next[0] = head.runs[0] + s_vuaot.tierUpRuns;
			head.next[1] = std::min(head.next[1], head.runs[1] + s_vuaot.tierUpRuns);
			head.jobs.push_back(std::move(j));
			if (head.tiered.size() + head.jobs.size() > kMaxBodiesPerHead)
			{
				head.jobs.erase(head.jobs.begin());
				s_vuaot.stats.discardMemory++;
			}
			continue;
		}
		head.outstanding = false;
		head.next[0] = head.runs[0] + s_vuaot.tierUpRuns;
		auto mark = std::make_unique<mVUChunkBody>();
		mark->tiered = true;
		mark->declined = true;
		mark->id = head.nextBodyId++;
		const s32 rootWord = static_cast<s32>(j->pc / 4);
		const u32* const was = reinterpret_cast<const u32*>(j->micro.get());
		// (the root pair at least: what every compile from this root reads)
		j->readMask[rootWord] = j->readMask[rootWord + 1] = 1;
		for (u32 w = 0; w < j->readMask.size(); w++)
			if (j->readMask[w])
				mark->reads.push_back({static_cast<s32>(w) - rootWord, was[w], ~0u});
		mark->lo = mark->reads.front().off;
		mark->hi = mark->reads.back().off;
		// (a job that wrapped says nothing about the routine anywhere else)
		if (j->wrapped)
			head.attempts++;
		else
		{
			head.tiered.push_back(std::move(mark));
			mVUchunkPackSave(microVU1, j->key, mVUchunkRootHashOf(was[rootWord], was[rootWord + 1], &j->key), *head.tiered.back());
		}
	}
}

// Compiles the routine at `j.pc` in `prog` once more, in block tables that are
// thrown away, with the job's regions placed where its root is compiled
// (vuAotLiveRegionHook), and files what the chunk store recorded as a tiered
// body of the routine's head. Micro memory holds what the job read. Returns
// whether the head gained a body.
static bool vuAotBuildBody(microVU& mVU, microProgram& prog, mVUChunkHead& head, VuAotJob& j)
{
	const auto failed = [&](u64& counter) {
		counter++;
		head.attempts++;
		head.next[0] = head.runs[0] + s_vuaot.tierUpRuns;
		return false;
	};
	// A region that dropped the sticky STATUS byte was built while no reader of
	// it was known. That must still hold.
	if (j.stickyDropped && !vuAotMutated(vuaot::live::kTripMutPublish) && s_trip.tripped.load(std::memory_order_acquire))
		return failed(s_vuaot.stats.discardSticky);
	if (!vuAotHasRoom(mVU, j) || mVU.placeSlotsUsed >= mVUplaceSlotResetAt)
		return failed(s_vuaot.stats.discardSpace);

	const u64 ins0 = vuAotReadIns();
	const microRegInfo savedLp = mVU.prog.lpState;
	microProgram* const savedCur = mVU.prog.cur;
	mVU.prog.cur = &prog;
	std::vector<microBlockManager*> graph(prog.block, prog.block + mProgSize / 2);
	std::fill(prog.block, prog.block + mProgSize / 2, nullptr);

	// In a code-cache episode of its own: the slow path's episode can hold
	// blocks the program cache's recorder is keeping for another program.
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);
	mVU.chunkStore->built.reset();
	s_vuaot.building = true;
	s_vuaot.prepared = &j;
	mVUblockFetch(mVU, j.pc, reinterpret_cast<uptr>(&j.key));
	const bool placed = s_vuaot.prepared != &j;
	s_vuaot.prepared = nullptr;
	s_vuaot.building = false;
	mVUcloseCodeCache(mVU);
	mVUopenCodeCache(mVU);

	// The scratch tables go: shared code names no block object.
	for (u32 i = 0; i < mProgSize / 2; i++)
	{
		if (prog.block[i])
			mVU.progBytes -= prog.block[i]->getBlockCount() * mVUblockBytes;
		delete prog.block[i];
		prog.block[i] = graph[i];
	}
	mVU.prog.cur = savedCur;
	// (the compiles above leave their keys in the dispatcher's carried state)
	mVU.prog.lpState = savedLp;
	s_vuaot.stats.buildIns += vuAotReadIns() - ins0;

	std::unique_ptr<mVUChunkBody> body = std::move(mVU.chunkStore->built);
	if (!placed)
		return failed(s_vuaot.stats.discardTrace);
	if (!body)
		return failed(s_vuaot.stats.discardUnstorable);

	const u64 lag = s_vuaot.dispatches - j.requestedAt;
	s_vuaot.stats.built++;
	s_vuaot.stats.lagSum += lag;
	s_vuaot.stats.lagMax = std::max(s_vuaot.stats.lagMax, lag);
	(j.retier ? s_vuaot.stats.retiers : s_vuaot.stats.tierUps)++;
	if (j.viaJump)
		s_vuaot.stats.jumpTierUps++;

	body->id = head.nextBodyId++;
	body->droppedSticky = j.stickyDropped != 0;
	for (const auto& jt : j.jumpTargets)
	{
		std::vector<u32>& v = body->targets[(jt.first - j.pc) & 0x3ff8];
		for (u32 t : jt.second)
			v.push_back((t - j.pc) & 0x3ff8);
		std::sort(v.begin(), v.end());
	}
	mVU.chunkStore->tieredBodies++;
	mVU.chunkStore->tieredCodeBytes += body->codeBytes;
	DevCon.WriteLn(Color_StrongBlue, "vu-aot: %s of the routine at 0x%04x after %u runs and %u jumps: %u regions, %u bytes%s",
		j.retier ? "re-tier" : "tier-up", j.pc, head.runs[0] + head.runs[1], head.jumpRuns, j.regions, body->codeBytes,
		body->droppedSticky ? ", sticky STATUS byte dropped" : "");
	head.tiered.push_back(std::move(body));
	mVUchunkPackSave(mVU, j.key, mVUchunkRootHash(mVU, j.pc, &j.key), *head.tiered.back());
	head.serial++;
	if (j.retier)
		head.retiers++;
	head.runsAtTier = head.runs[1];
	head.missesSeen = j.missesAtRequest;
	head.emptyChecks = 0;
	head.next[0] = head.runs[0] + s_vuaot.tierUpRuns;
	head.next[1] = head.runs[1] + vuaot::live::kRetierCheckMask + 1;
	return true;
}

// The block the slow path's search found for (startPC, key) in `prog`.
static microBlock* vuAotBlockOf(microVU& mVU, microProgram& prog, u32 startPC, const microRegInfo& key)
{
	microBlockManager* const m = prog.block[startPC / 8];
	if (!m)
		return nullptr;
	alignas(16) microRegInfo k = key;
	return m->search(mVU, &k);
}

// mVUlookupProg_VU1 plus the run counter. The entry is counted on the head of
// the routine its block is the root of: on runs[0] for a copy on microVU's
// code, on runs[1] for one on a body of its routine. A count that reaches the
// head's next threshold, a head that has changed since this copy was last
// checked against it, and a finished job all return null: the dispatcher then takes the full
// path (mVUexecuteVU1), which ends in mVUvuAotAfterSearch.
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

	mVUChunkHead* const head = static_cast<mVUChunkHead*>(pBlock->tierHead);
	if (!head || pBlock->tierSerial != head->serial || (s_vuaot.worker && s_vuaot.worker->CompletedCount()))
	{
		s_vuaot.pending = true;
		s_vuaot.pendingCounted = false;
		return nullptr;
	}
	// (a copy the tier declined, or one taken into another routine's body, has
	// nothing to count towards)
	const u32 state = pBlock->tierState;
	if (state == mVUtierNone || state >= mVUtierBody)
	{
		const u32 k = state != mVUtierNone;
		if (++head->runs[k] >= head->next[k])
		{
			s_vuaot.pending = true;
			s_vuaot.pendingCounted = true;
			return nullptr;
		}
	}
	return pBlock->hostEntry;
}

// Asks the worker for the regions of the routine rooted at (startPC, key),
// with the jump targets `prog`'s jump caches know now.
static bool vuAotRequest(microVU& mVU, microProgram& prog, mVUChunkHead& head, u32 startPC, const microRegInfo& key, bool retier, bool viaJump,
	std::map<u32, std::vector<u32>>&& targets)
{
	auto j = vuAotMakeJob(mVU, &head, startPC, &key);
	j->retier = retier;
	j->viaJump = viaJump;
	j->jumpTargets = std::move(targets);
	for (const auto& jt : j->jumpTargets)
		s_vuaot.stats.targetsRecorded += jt.second.size();
	head.missesSeen = s_vuaot.jumpMisses;
	// (the jump path asks again only after as many more jump entries)
	head.nextJump = head.jumpRuns + s_vuaot.jumpTierUpRuns;
	if (!vuAotSubmit(j))
	{
		head.attempts++;
		head.next[0] = head.runs[0] + s_vuaot.tierUpRuns;
		return false;
	}
	head.outstanding = true;
	if (s_vuaot.synchronous)
	{
		vuAotWorker().Wait(j);
		vuAotCollectFinished();
	}
	return true;
}

// The decision both slow paths share. `blk` is the block the search found for
// (startPC, key) in `prog` (mVU.prog.cur), the root of a copy of `head`'s
// routine; `result` is its entry. Returns the entry to continue at.
//
// A finished job of the head is built into a tiered body here if this copy is
// the routine it compiled. The copy is moved onto the head's newest body that
// matches it. A copy still on microVU's code asks for the routine's regions
// when `tierNow`; one on a tiered body asks for them again when its jumps have
// met targets the body does not hold.
static void* vuAotConsider(microVU& mVU, microProgram& prog, microBlock& blk, mVUChunkHead& head, u32 startPC, const microRegInfo& key,
	void* result, bool tierNow, bool viaJump)
{
	const auto build = [&]() {
		// (a copy taken into another routine's body leaves the jobs for one that is not)
		if (blk.tierState == mVUtierCovered)
			return;
		for (auto it = head.jobs.begin(); it != head.jobs.end(); ++it)
		{
			const std::shared_ptr<VuAotJob> j = std::static_pointer_cast<VuAotJob>(*it);
			if (std::memcmp(&j->key, &key, sizeof(key)) != 0 || !vuAotJobFits(mVU, *j, startPC))
				continue;
			head.jobs.erase(it);
			vuAotRebase(*j, startPC);
			vuAotBuildBody(mVU, prog, head, *j);
			return;
		}
	};
	const auto place = [&]() {
		if (blk.tierHead == &head && blk.tierSerial == head.serial)
			return;
		blk.tierHead = &head;
		if (void* const entry = mVUchunkReAimCopy(mVU, head, &blk, startPC))
			result = entry;
	};
	build();
	place();

	if (blk.tierState == mVUtierNone)
	{
		const bool canAsk = !head.outstanding && head.attempts < kMaxGenAttempts && head.tiered.size() < kMaxBodiesPerHead;
		if (canAsk && tierNow)
		{
			std::map<u32, std::vector<u32>> targets = vuAotHarvestTargets(prog);
			// A routine tiered in an earlier session whose body cannot be
			// placed, at its first run in this one: the jump caches are empty,
			// so its regions are asked for with the targets its newest body
			// from the pack holds. An edge to a
			// target is taken only when the jump goes there, so a target this
			// copy never reaches costs a compare.
			if (targets.empty() && head.tieredInPack)
			{
				for (size_t n = head.tiered.size(); n-- > 0 && targets.empty();)
					if (!head.tiered[n]->declined)
						for (const auto& jt : head.tiered[n]->targets)
							for (u32 to : jt.second)
								targets[(jt.first + startPC) & 0x3ff8].push_back((to + startPC) & 0x3ff8);
				for (auto& jt : targets)
					std::sort(jt.second.begin(), jt.second.end());
			}
			if (vuAotRequest(mVU, prog, head, startPC, key, false, viaJump, std::move(targets)))
			{
				build();
				place();
			}
		}
		// (nothing to ask for: the count stops sending entries here until a job comes back)
		if (head.runs[0] >= head.next[0])
			head.next[0] = canAsk && !head.outstanding ? head.runs[0] + 1 : ~0u;
		return result;
	}
	if (blk.tierState < mVUtierBody || head.runs[1] < head.next[1])
		return result;

	// A copy on a tiered body, at the periodic look for new jump targets.
	head.next[1] = head.runs[1] + vuaot::live::kRetierCheckMask + 1;
	if (head.settled || head.retiers >= vuaot::live::kMaxRetiers || head.tiered.size() >= kMaxBodiesPerHead)
	{
		head.next[1] = ~0u;
		return result;
	}
	if (head.outstanding || head.runs[1] - head.runsAtTier < vuaot::live::kRetierMinRuns || head.missesSeen == s_vuaot.jumpMisses)
		return result;
	s_vuaot.stats.retierChecks++;
	head.missesSeen = s_vuaot.jumpMisses;
	const mVUChunkBody* running = nullptr;
	for (const auto& b : head.tiered)
		if (mVUtierBody + b->id == blk.tierState)
			running = b.get();
	if (!running)
		return result;
	// Targets the jump caches of the body's own jumping blocks saw that its
	// regions do not hold. If there are none the routine stays as it is.
	std::map<u32, std::vector<u32>> targets = vuAotHarvestTargets(prog);
	bool fresh = false;
	for (const auto& jt : targets)
	{
		const u32 off = (jt.first - startPC) & 0x3ff8;
		const s32 pairOff = static_cast<s32>(jt.first / 8) - static_cast<s32>(startPC / 8);
		bool ours = false;
		for (const mVUChunkBody::Block& b : running->blocks)
			ours = ours || (b.jumpCache && b.pairOff == pairOff);
		if (!ours)
			continue;
		const auto have = running->targets.find(off);
		for (u32 t : jt.second)
			fresh = fresh || have == running->targets.end() ||
					!std::binary_search(have->second.begin(), have->second.end(), (t - startPC) & 0x3ff8);
	}
	if (!fresh)
	{
		if (++head.emptyChecks >= vuaot::live::kSettleChecks)
		{
			head.settled = true;
			mVUchunkPackSaveSettled(mVU, key, mVUchunkRootHash(mVU, startPC, &key));
		}
		return result;
	}
	head.emptyChecks = 0;
	if (vuAotRequest(mVU, prog, head, startPC, key, true, viaJump, std::move(targets)))
	{
		build();
		place();
	}
	return result;
}

// The dispatcher's slow path: collects what the worker made, and is where a
// routine asks for its regions and a copy is moved onto them. mVU.prog.cur is
// the program the search resolved and `result` its entry for `startPC`.
void* mVUvuAotAfterSearch(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuaot.enabled || mVU.index != 1)
		return result;
	// The counting lookup already counted a dispatch that asked for this.
	const bool counted = s_vuaot.pending && s_vuaot.pendingCounted;
	s_vuaot.pending = false;
	s_vuaot.stats.slowDispatches++;
	vuAotCollectFinished();
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
		return result;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	microBlock* const blk = vuAotBlockOf(mVU, *prog, startPC, key);
	if (!blk)
		return result;
	mVUChunkHead* head = static_cast<mVUChunkHead*>(blk->tierHead);
	if (!head)
		head = mVUchunkFindHead(mVU, startPC, key, true);
	if (!counted)
	{
		if (blk->tierHead != head || blk->tierState == mVUtierNone)
			head->runs[0]++;
		else if (blk->tierState >= mVUtierBody)
			head->runs[1]++;
	}
	return vuAotConsider(mVU, *prog, *blk, *head, startPC, key, result, head->runs[0] >= head->next[0], false);
}

// The same for a JR/JALR the jump cache missed (mVUcompileJIT), after the search resolved
// its target to a block. Counts the entry on that block's routine. A routine the dispatcher
// never enters (an overlay a stub program jumps into) has no dispatch count, so jumps decide
// its tier-up; one the dispatcher does enter keeps its dispatch threshold, since tiering it
// from a jump into its middle would root its regions there.
void* mVUvuAotAfterJump(microVU& mVU, u32 startPC, uptr pState, void* result)
{
	if (!s_vuaot.enabled || mVU.index != 1)
		return result;
	vuAotCollectFinished();
	microProgram* const prog = mVU.prog.cur;
	if (!prog || !result)
		return result;
	alignas(16) microRegInfo key;
	std::memcpy(&key, reinterpret_cast<const void*>(pState), sizeof(key));
	microBlock* const blk = vuAotBlockOf(mVU, *prog, startPC, key);
	if (!blk)
		return result;
	mVUChunkHead* head = static_cast<mVUChunkHead*>(blk->tierHead);
	if (!head)
		head = mVUchunkFindHead(mVU, startPC, key, true);
	const u32 jumps = ++head->jumpRuns;
	s_vuaot.stats.jumpCounted++;
	if (blk->tierHead == head && blk->tierState >= mVUtierBody)
		head->runs[1]++;
	// Only when jumps dominate the routine's entries (16 to 1 over dispatches).
	const bool tierNow = jumps >= std::max(head->nextJump, s_vuaot.jumpTierUpRuns) && static_cast<u64>(head->runs[0]) * 16 <= jumps;
	return vuAotConsider(mVU, *prog, *blk, *head, startPC, key, result, tierNow, true);
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
	if (s_vuaot.stats.tierUps || s_vuaot.stats.jobs)
		Console.WriteLn(Color_StrongBlue, "%s", vuaot::live::FormatStats().c_str());
#ifdef PCSX2_RECOMPILER_TESTS
	const mvu_test_hooks::JumpTableUsage jt = mvu_test_hooks::GetJumpTableUsage();
	Console.WriteLn("@JUMPTABLES@ live=%llu live_written_kb=%llu held=%llu held_written_kb=%llu",
		(unsigned long long)jt.liveTables, (unsigned long long)(jt.liveWritten >> 10),
		(unsigned long long)jt.heldTables, (unsigned long long)(jt.heldWritten >> 10));
#endif
	s_vuaot.worker.reset();
}

#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
// With pcsx2-eerunner --count-vu1-ins: the user-mode instructions the outermost
// compile sessions of VU1 retire, split by whether the session built a tiered
// body or was microVU's own, for reading the tier's compile cost
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
			s_vuAotSessionTiered = s_vuaot.building;
		}
#endif
		s_vuaot.compileDepth++;
	}
}

void mVUvuAotCompileEnd(microVU& mVU)
{
	if (mVU.index != 1 || !s_vuaot.enabled || s_vuaot.compileDepth <= 0)
		return;
	if (--s_vuaot.compileDepth == 0)
	{
#if defined(PCSX2_RECOMPILER_TESTS) && defined(__linux__)
		if (vuAotCountingIns())
		{
			const u64 d = mvu_test_hooks::ReadThreadInstructions() - s_vuAotSessionIns0;
			(s_vuAotSessionTiered ? s_vuaot.stats.compileInsTiered : s_vuaot.stats.compileInsPlain) += d;
		}
#endif
	}
}

// The compile of the root of the routine being built: its regions go in here,
// if microVU's own first pass of the root agrees with the port's.
static bool vuAotLiveRegionHook(const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void*)
{
	VuAotJob* const j = s_vuaot.prepared;
	if (!j || b.startPC != j->pc || std::memcmp(b.entryState, &j->key, sizeof(j->key)) != 0)
		return false;
	if (!vuaot::CompareBlockTraces(b, j->rootTrace).empty())
	{
		s_vuaot.stats.regionDeclined["timing port disagrees"]++;
		return false;
	}
	s_vuaot.prepared = nullptr;
	vuAotPlace(microVU1, *j, static_cast<microBlock*>(env.rootBlock));
	return true;
}

void mVUvuAotLiveHooks(microVU& mVU, vuaot::VuAotRegionHook& region, void*& regionUser)
{
	if (mVU.index != 1 || !s_vuaot.enabled || !s_vuaot.building)
		return;
	region = &vuAotLiveRegionHook;
	regionUser = nullptr;
}

#ifdef PCSX2_RECOMPILER_TESTS
namespace mvu_test_hooks
{

	static u64 JumpTableResidentBytes(const void* table)
	{
#if defined(__linux__)
		const size_t bytes = sizeof(microJumpCache) * (mProgSize / 2);
		const uptr begin = Common::AlignDownPow2(reinterpret_cast<uptr>(table), __pagesize);
		const uptr end = Common::AlignUpPow2(reinterpret_cast<uptr>(table) + bytes, __pagesize);
		std::vector<unsigned char> pages((end - begin) / __pagesize);
		if (mincore(reinterpret_cast<void*>(begin), end - begin, pages.data()) != 0)
			return 0;
		u64 resident = 0;
		for (unsigned char p : pages)
			resident += p & 1;
		return resident * __pagesize;
#else
		(void)table;
		return 0;
#endif
	}

	// The bytes of the pages of a table that hold any entry.
	static u64 JumpTableWrittenBytes(const microJumpCache* table)
	{
		const u8* const b = reinterpret_cast<const u8*>(table);
		const size_t bytes = sizeof(microJumpCache) * (mProgSize / 2);
		u64 written = 0;
		for (size_t off = 0; off < bytes; off += __pagesize)
		{
			const size_t end = std::min<size_t>(bytes, off + __pagesize);
			written += std::any_of(b + off, b + end, [](u8 x) { return x != 0; }) ? (end - off) : 0;
		}
		return written;
	}

	JumpTableUsage GetJumpTableUsage()
	{
		JumpTableUsage u;
		// (a table several blocks share counts once)
		std::unordered_set<const microJumpCache*> seen;
		const auto visit = [&](microProgram* prog) {
			if (!prog)
				return;
			for (u32 i = 0; i < mProgSize / 2; i++)
				if (microBlockManager* m = prog->block[i])
					m->forEachBlock([&](microBlock& b) {
						if (b.jumpCache && seen.insert(b.jumpCache).second)
						{
							u.liveTables++;
							u.liveResident += JumpTableResidentBytes(b.jumpCache);
							u.liveWritten += JumpTableWrittenBytes(b.jumpCache);
						}
					});
		};
		for (const auto& e : microVU1.mvuContentMap)
			visit(e.second);
		for (microProgram* p : microVU1.mvuOrphanedProgs)
			visit(p);
		for (const void* table : microVU1.jumpTables)
		{
			u.heldTables++;
			u.heldWritten += JumpTableWrittenBytes(static_cast<const microJumpCache*>(table));
		}
		return u;
	}
} // namespace mvu_test_hooks
#endif

#ifdef PCSX2_RECOMPILER_TESTS
namespace vuaot::live
{
	// pcsx2-vurunner --vuaot-region-bg: the region hook through the worker. A job is
	// made for the block microVU is compiling, run on the worker and waited for, and
	// its code copied in where microVU is compiling. The regions are the live tier's:
	// built on an empty graph from the port's own first pass of the root, which has
	// to agree with microVU's, their nodes taking over the blocks that are there.
	bool RegionHookViaWorker(const MvuBlockTrace& b, const MvuEmitEnv& env, void* user)
	{
		microVU& mVU = microVU1;
		if (b.vuIndex != 1 || !mVU.prog.cur)
			return false;
		const RegionTier& tmpl = *static_cast<const RegionTier*>(user);
		auto j = vuAotMakeJob(mVU, nullptr, b.startPC, b.entryState);
		j->env = env;
		j->opts.From(tmpl);
		// (the runner has no tripwire: a reader of the sticky bits anywhere in the image keeps them)
		j->opts.dropSticky = tmpl.dropSticky && !RegionTier::MicroReadsSticky(mVU.regs().Micro, 1);
		j->jumpTargets = tmpl.jumpTargets;
		if (!vuAotSubmit(j))
			return false;
		vuAotWorker().Wait(j);
		std::vector<std::shared_ptr<vuaot::bg::Job>> done;
		s_vuaot.worker->TakeCompleted(done);
		vuAotCollect(*j);
		if (!j->accepted)
			return false;
		if (!CompareBlockTraces(b, j->rootTrace).empty())
		{
			s_vuaot.stats.regionDeclined["timing port disagrees"]++;
			return false;
		}
		vuAotPlace(mVU, *j, static_cast<microBlock*>(env.rootBlock));
		return true;
	}
} // namespace vuaot::live
#endif
