// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The live tier: vu-aot running inside the emulator, behind
// EmuCore/CPU/Recompiler/EnableVUAot (default off). The machinery lives in
// microVU's translation unit (microVU_VuAot-arm64.inl), because it works on
// microVU's programs and block managers; this header is the policy and what
// the rest of the tree (and the test runners) may see of it.
//
// What it does, in one paragraph: microVU keeps compiling and running every VU1
// program as it always has. With the tier on, the dispatcher counts the runs of
// each program, and the JR/JALR slow path counts the entries it resolves to a
// program (the ones the dispatcher never enters). A program that reaches
// kTierUpRuns (or kJumpTierUpRuns of the latter) asks for its tier-up: the
// JR/JALR targets microVU has recorded for it (its jump caches) are collected
// and a job goes to the tier's worker thread, which builds the vu-aot region
// rooted at the entry the request came through while microVU runs on. When the
// job is done, a slow path on the VU1 thread publishes it: the program's microVU
// blocks are set aside (not freed: older code and stale jump caches can still
// reach them) and its root compiles again, taking the region's code in. Blocks
// microVU meets later in a tiered program become root jobs of their own, and
// framed bodies where the region builder declines them. microVU remains the
// fallback at every block boundary. A tiered program that meets new jump
// targets asks for a new generation, a bounded number of times. Regions leave
// the sticky bits of STATUS out (the exactness contract's relaxation 3) while
// no reader of them is known, in either flag-hack mode; the first reader the
// tier hears of sets the tripwire (StickyTripped below) and VU1 rebuilds exact.
// Design: compiler-design.md sections 9x, 9aa and 9ad.

#include "common/Pcsx2Defs.h"
#include "VuAotMvuTrace.h"
#include "VuAotPersist.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vuaot
{
	namespace live
	{
		// Runs of one program (dispatches that resolve to it) before it is tiered
		// up. 9n of compiler-design.md: 150-300 runs, a fraction of a second of
		// play, put 85-93% of GoW2's runs in vu-aot and compile 140-430 programs
		// per scene.
		constexpr u32 kTierUpRuns = 200;

		// A program the dispatcher (almost) never enters, reached through JR/JALR
		// slow-path entries (the jump cache missed and the search resolved the target to
		// it), is tiered up after this many of them. God of War II's overlays are such
		// programs: the dispatcher enters a stub program, which jumps by JR into code
		// microVU files as other programs, and every upload over an overlay nulls the quick
		// slot of the program there, so each hot overlay entry goes through the slow path
		// about 1,200 times in 300 frames. A program the dispatcher does enter keeps
		// kTierUpRuns: tiering it from a jump into its middle roots its first compile there,
		// and the regions come out split (Katamari at 900 frames, 1.6% more instructions).
		// A slow-path entry is an entry, so the number is far below the break-even of a tier-up
		// (a compile of about 30-70 M instructions, a quarter of a run saved); it only decides
		// how soon the tier-up comes. Measured on three GoW2 savestates at 300 and 900 frames
		// with 20, 50, 100, 200 and 400: lower tiers more programs, sooner, and wins at 900
		// frames; 20 and 50 are within 1% of each other. 32.
		constexpr u32 kJumpTierUpRuns = 32;

		// A tiered program looks for new jump targets every this many runs
		// (when any JR/JALR missed the jump cache since), and is compiled again
		// when it finds some, at most kMaxRetiers times and not within
		// kRetierMinRuns runs of its last compile. After kSettleChecks looks in a
		// row that found nothing it stops looking: the jump cache misses whenever
		// a game rewrites micro memory, so the miss count alone says little.
		constexpr u32 kRetierCheckMask = 0x3ff;
		constexpr u32 kMaxRetiers = 2;
		constexpr u32 kRetierMinRuns = 1024;
		constexpr u32 kSettleChecks = 3;

		// Recorded targets kept per jumping block and per program: a data-driven
		// jump (GTA, Burnout 3) can reach hundreds, and every one becomes a node.
		constexpr u32 kMaxTargetsPerJump = 48;
		constexpr u32 kMaxTargetsPerProgram = 512;

		// What set the sticky-STATUS tripwire off (owner decision j, 2026-10-14). While it is clear the
		// regions drop the sticky byte of STATUS (RegionTier::dropSticky) whatever the flag hack; once
		// anything below happens it stays set until the VM is reset or booted, and regions are built
		// and kept exact.
		enum class TripCause : u8
		{
			None,
			ProgramImage, // a program was created (or loaded from the disk cache) over a micro image with a sticky reader
			Compile,      // microVU compiled an FSAND/FSEQ/FSOR that selects a sticky bit
			Job,          // a tier job's copy of micro memory, or live micro memory at publication, has a sticky reader
			OutsideRead,  // VU0 (micro or macro mode) touched VU1's STATUS register through its window onto VU1's registers
			Forced,       // a test or a runner flag set it off
			Remembered,   // the running title set it off in an earlier session (owner's default on decision 3, 2026-10-15)
			kCount,
		};
		constexpr u32 kTripCauses = static_cast<u32>(TripCause::kCount);
		const char* TripCauseName(TripCause c);

		struct Stats
		{
			u64 tierUps = 0;          // programs tiered up
			u64 jumpTierUps = 0;      // ... of them, by the JR/JALR slow path (counted in tierUps too)
			u64 jumpCounted = 0;      // JR/JALR slow-path entries counted on a program
			u64 crossProgTargets = 0; // jump-cache entries seen at a harvest that resolved to another program
			u64 retiers = 0;          // compiles of a tiered program for new jump targets
			u64 retierChecks = 0;     // times a tiered program looked for new targets
			u64 blocksSetAside = 0;   // microVU blocks moved out of the way by tier-ups
			u64 targetsRecorded = 0;  // jump targets handed to regions
			u64 jumpMisses = 0;       // JR/JALR the jump cache did not know (VU1)
			u64 rootsCompiled = 0;    // blocks offered to the region tier
			u64 regions = 0;
			u64 regionNodes = 0;
			u64 regionMerged = 0;     // ... of them, aliases sharing another node's body (variant merging)
			u64 regionExits = 0;
			u64 regionBytes = 0;
			u64 bodyBlocks = 0;       // blocks offered to the body tier (the region declined them)
			u64 bodiesLowered = 0;
			u64 bodyBytes = 0;
			u64 bailCompiles = 0;     // regions' blocks microVU compiled at run time (budget shortfalls)
			u64 bailsEntry = 0;       // budget bails taken by regions at an entry from microVU (programs deleted so far)
			u64 bailsLoopHead = 0;    // ... and at a loop head
			// Static census of the compiled regions' flag reads (exactness contract, relaxation 3):
			// status readers (FSAND/FSEQ/FSOR), those whose mask selects a sticky bit, compiles that
			// dropped the sticky byte, and compiles whose micro memory has a sticky reader.
			u64 statusReaders = 0;
			u64 stickyReaders = 0;
			u64 stickyDivReaders = 0; // ... of those, the ones that read the sticky I / D bits (STATUS bits 10-11)
			u64 stickyDropped = 0;
			u64 stickyImages = 0;
			// The sticky-STATUS tripwire (section 9ad): the number of times it went off (0 in a
			// normal session; a VM reset or boot re-arms it), by what set it off, and the VU1
			// resets it asked for to rebuild the regions that had dropped the sticky byte.
			u64 stickyTrips = 0;
			u64 stickyTripsBy[kTripCauses] = {};
			u64 stickyTripResets = 0;
			// Sessions (VM boots and resets, or a title change) that started with the tripwire set because
			// the running title set it off before (TripCause::Remembered; not counted in stickyTrips).
			u64 stickyRemembered = 0;
			// pcsx2-eerunner --count-vu1-ins only: user-mode instructions VU1's compile
			// sessions retired, for programs the tier took over and for the rest.
			u64 compileInsTiered = 0;
			u64 compileInsPlain = 0;

			// Section 9aa, the background compile. A job builds one region on the worker
			// thread: a generation (the tier-up or a re-tier: a new graph for the
			// program, rooted at the entry the request came through) or a root (a block
			// microVU met in a program already tiered up).
			u64 jobsGen = 0, jobsRoot = 0;
			u64 jobsDropped = 0;      // the worker's queue was full
			u64 jobsDone = 0;
			u64 jobsDeclined = 0;     // the region builder declined the root
			u64 jobsCoalesced = 0;    // root jobs dropped from the queue by a new generation of their program
			u64 publishedGen = 0, publishedRoot = 0;
			// Finished jobs thrown away: made before a reset (stale), for a graph the
			// program no longer has (gen), over micro memory that has changed since
			// (memory), under a micro image that has gained a sticky STATUS reader since a region dropped the
			// sticky byte (sticky), with no room left in the code cache (space), or whose root
			// microVU analysed differently when it compiled it (trace).
			u64 discardStale = 0, discardGen = 0, discardMemory = 0, discardSticky = 0, discardSpace = 0, discardTrace = 0;
			// VU1 dispatches between a job's request and its publication.
			u64 lagGenSum = 0, lagGenMax = 0, lagRootSum = 0, lagRootMax = 0;
			// pcsx2-eerunner --count-vu1-ins only: user-mode instructions the worker
			// retired in jobs, and the VU1 thread in publications (copies, exits, jump
			// tails; the root compile of a generation included).
			u64 workerIns = 0;
			u64 publishIns = 0;
			u64 veneers = 0;          // branches from placed code to targets out of B/BL range
			std::map<std::string, u32> regionDeclined;

			// The program cache's sidecars (the saved plans, persistence-design.md). A program created or
			// loaded with the cache on looks for its sidecar: found and accepted (planLoaded), refused by
			// the parser (planRejected, by reason in planRejectedBy), or accepted but its generation's
			// read set no longer matches micro memory (planStale). An accepted plan sends its generation
			// at once (planGenJobs) and its roots when the generation is in (planRootJobs); a root plan is
			// dropped when its read set no longer matches (planRootsStale) or when the graph already holds
			// its root (planRootsCovered). planRootsTraced: microVU compiled a sent root before its job
			// came back; planRootsUnplaced: a root job came back but its root was already compiled
			// without the hook seeing it, so nothing was placed.
			u64 planLookups = 0, planLoaded = 0, planRejected = 0, planStale = 0;
			u64 planRejectedBy[16] = {};
			u64 planGenJobs = 0, planRootJobs = 0, planRootsStale = 0, planRootsCovered = 0;
			u64 planRootsTraced = 0, planRootsUnplaced = 0, planPublishedGen = 0, planPublishedRoot = 0;
			u64 sidecarsQueued = 0;   // programs handed to the writer
			// Time to the first region: per program that got a region (from any job), VU1 runs of the
			// program (dispatches and JR slow-path entries) before it, VU1 dispatches of all programs from
			// the program's creation, and VU1 cycles in that time (VURegs::cycle; 0 under MTVU). "plan": the
			// subset whose tier-up came from a loaded sidecar.
			u64 firstRegions = 0, firstRegionRuns = 0, firstRegionDispatches = 0, firstRegionCycles = 0;
			u64 planFirstRegions = 0, planFirstRegionRuns = 0, planFirstRegionDispatches = 0, planFirstRegionCycles = 0;

			// Saved region code (persistence-design.md, the install half). A loaded sidecar whose generation
			// record can be installed puts its records' code in place at load (aotPrograms programs,
			// aotRecords records holding aotRegions regions and aotBytes bytes of code); a record that
			// cannot is refused for the reason in aotRejectedBy (AotReject) and its plan goes to the worker.
			// aotRootsToPlan: root records refused while their generation went in. aotNoCode: published
			// jobs saved without their code (an address nothing can re-base). installIns:
			// pcsx2-eerunner --count-vu1-ins only, the VU1 thread's instructions in installs.
			u64 aotPrograms = 0, aotRecords = 0, aotRegions = 0, aotBytes = 0, aotRootsToPlan = 0, aotNoCode = 0;
			u64 aotRejectedBy[8] = {};
			u64 installIns = 0;
			// Roots the builder declined in a program's current generation, saved with its sidecar: loaded
			// (declinedLoaded), and the root jobs not asked for because a loaded one said so (declinedSkipped).
			u64 declinedLoaded = 0, declinedSkipped = 0;
		};

		// Why a saved record's code was not installed (Stats::aotRejectedBy).
		enum AotReject : u8
		{
			kAotOk = 0,
			kAotNoCode,   // the record holds its plan only
			kAotSticky,   // it dropped the sticky STATUS byte and the tripwire is set
			kAotAddress,  // an address outside this process's module or data arena, or too wide for its run
			kAotShape,    // an operand that is not the shape its fixup says, or an unknown stub
			kAotCacheRun, // a code-cache address could be too wide for a run that links to a block
			kAotSpace,    // no room in the code cache for the program's records
			kAotRejects,
		};
		const char* AotRejectName(AotReject r);

		// Whether the tier runs this session (EnableVUAot, VU1 recompiled, no
		// gamefix vu-aot declines). Settled at init and at every reset.
		bool IsEnabled();

		// The sticky-STATUS tripwire across sessions (persistence-design.md, the owner's default on decision
		// 3): a title (disc serial and CRC) that set the tripwire off is remembered in the program cache's
		// directory (vu_jit/vuaot_tripped.txt), and every later session of it starts with the tripwire set.
		// SetTitle is what VMManager calls when the disc details change; an empty serial is no title.
		void SetTitle(const std::string& serial, u32 crc);

		// The sticky-STATUS tripwire (section 9ad). Armed (false) at the start; set by the first
		// sticky reader the tier hears of -- a micro image containing one is uploaded or compiled, or
		// anything outside VU1 reads VU1's STATUS -- and then kept for the session: a savestate load or
		// a settings change does not re-arm it, a VM reset or boot does (ResetStickyTripwire).
		// Setting it off counts it, logs it once, and has VU1 reset at its next run so the regions
		// built without the sticky byte are rebuilt exact; jobs made before it are not published.
		bool StickyTripped();
		TripCause StickyTripCause();
		void ResetStickyTripwire();

		Stats GetStats();
		// Human-readable counters, including every decline reason.
		std::string FormatStats();

#ifdef PCSX2_RECOMPILER_TESTS
		// Test runners only: tier programs up after `runs` dispatches instead of
		// kTierUpRuns (a low number puts almost every program through vu-aot).
		void SetTierUpRuns(u32 runs);
		// ... and after `runs` JR/JALR slow-path entries instead of kJumpTierUpRuns.
		void SetJumpTierUpRuns(u32 runs);

		// Deterministic mode: every job is waited for and published where it was
		// asked for (at the tier-up's dispatch, or in place of the block microVU
		// was about to compile), which is where the synchronous tier of 9x
		// compiled. Off: the worker builds while microVU runs.
		void SetSynchronous(bool on);
		bool IsSynchronous();
		// The worker thread: whether it exists, and controls for interleaving tests.
		// HoldWorker: no job starts while held. SetWorkerGate: called on the worker
		// at the start of every job (block in it to keep a job in flight).
		bool WorkerRunning();
		void HoldWorker(bool on);
		void SetWorkerGate(std::function<void()> gate);
		void WaitWorkerIdle();
		size_t WorkerQueued();
		// pcsx2-eerunner --vuaot-worker-delay-us: the worker sleeps this long before each job,
		// so publication lags the request as on a loaded or slow machine (0: no delay).
		void SetWorkerDelayUs(u32 us);
		// The tripwire, as a test or a runner flag sets it off.
		void TripStickyForTest();
#endif
		// Test only: break parts of the tripwire on purpose, to show the tests notice. A mask of
		// TripMask(TripMutation), 0 = none. (Declared in every build so the tier's checks compile; in a
		// build without the recompiler test hooks they are constant false and generate nothing.)
		enum TripMutation : int
		{
			kTripMutNone = 0,
			kTripMutProgramImage = 1,  // no scan when a program is created
			kTripMutCompile = 2,       // no hook on a compiled sticky reader
			kTripMutOutside = 3,       // VU0 touching VU1's STATUS is not noticed
			kTripMutPublish = 4,       // a job made with the drop is published after the trip
			kTripMutJobOption = 5,     // a job made after the trip still drops the sticky byte
			kTripMutNoRebuild = 6,     // the trip does not have VU1 rebuild its regions
		};
		constexpr int TripMask(int mutation) { return 1 << mutation; }
#ifdef PCSX2_RECOMPILER_TESTS
		void SetTripwireMutation(int mask);
		// pcsx2-eerunner --vuaot-no-install: loaded sidecars are used as plans only (no saved code runs).
		void SetInstallEnabled(bool on);
		// The offset from VU1's code cache of the last region placed, by a publication or an install.
		size_t LastPlacedOffset();
		// Moves VU1's code-cache cursor on by `bytes` of BRK (a test that installs saved code wants it to
		// land somewhere other than where it was made).
		void TestPadCodeCache(u32 bytes);
		// The record code (vuaot/VuAotPersist.h) of every job published since the last call, with why a
		// job had none ("" when it has one), for jobs made while the program cache keeps sidecars.
		struct RecordCodeSample
		{
			persist::RegionCode code;
			std::string notPersistable;
			bool generation = false;
			u32 wMemHomes = 0; // vector homes of the job's region whose w lane stayed in microVU's location
		};
		std::vector<RecordCodeSample> TakeRecordCodeSamples();
		// pcsx2-vurunner --vuaot-region-bg: a region hook (VuAotMvuTrace.h's
		// VuAotRegionHook, `user` a RegionTier whose options it copies) that builds
		// every region through the worker and copies it in.
		bool RegionHookViaWorker(const MvuBlockTrace& b, const MvuEmitEnv& env, void* user);
#endif
	} // namespace live
} // namespace vuaot

#ifdef PCSX2_RECOMPILER_TESTS
namespace mvu_test_hooks
{
	// Test runners only (Linux): count the user-mode instructions the thread that
	// runs VU1 retires inside recMicroVU1::Execute; read the total and the number
	// of dispatches back after the run.
	void EnableVu1InstructionCount(bool on);
	void GetVu1InstructionCount(u64& instructions, u64& dispatches);
	// VU1 dispatches that ended on a cycle-budget break, all runs of a test build.
	u64 GetVu1Stops();
} // namespace mvu_test_hooks
#endif
