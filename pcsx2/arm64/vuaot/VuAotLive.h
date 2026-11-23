// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The live tier: vu-aot running inside the emulator, behind
// EmuCore/CPU/Recompiler/EnableVUAot. The machinery lives in microVU's
// translation unit (microVU_VuAot-arm64.inl) because it works on microVU's
// programs, block managers and chunk store; this header declares the policy
// and what the rest of the tree and the test runners may see of it.
//
// microVU keeps compiling and running every VU1 program as before. The unit
// the tier works on is a routine: a root block and the code compiled from it,
// which the chunk store keeps by what the compile read, whatever address the
// game uploads it at. The dispatcher counts the runs of every copy of a
// routine together, and the JR/JALR slow path counts the entries it resolves
// to one. A routine that reaches kTierUpRuns (or kJumpTierUpRuns slow-path
// entries) is queued to the worker thread, which builds vu-aot regions from
// its root while microVU keeps running. A slow path on the VU1 thread then
// compiles the routine once with those regions in place, and the chunk store
// keeps the result as the routine's tiered body. Every copy of the routine,
// present or uploaded later, is placed onto that one body. microVU is the
// fallback at every block boundary. A routine whose jumps meet targets its
// body does not hold is built again a bounded number of times.
//
// Regions leave out the sticky bits of STATUS while no reader of them is known.
// The first reader the tier hears of sets the tripwire (StickyTripped below)
// and VU1 rebuilds exact.

#include "common/Pcsx2Defs.h"
#include "VuAotMvuTrace.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vuaot
{
	namespace live
	{
		// Runs of one routine (dispatches that enter a copy of it) before it is
		// tiered up. 150-300 runs is a fraction of a second of play.
		constexpr u32 kTierUpRuns = 200;

		// JR/JALR slow-path entries (the jump cache missed and the search resolved
		// the target to a block) before a routine the dispatcher almost never
		// enters is tiered up. God of War II's overlays are such routines: the
		// dispatcher enters a stub program, which jumps by JR into code microVU files
		// as other programs, and every upload over an overlay nulls that program's
		// quick slot, so each hot overlay entry goes through the slow path.
		// A routine the dispatcher does enter keeps kTierUpRuns.
		// A slow-path entry is an entry, so this is far below the break-even of a
		// tier-up and only decides how soon the tier-up comes. Lower values tier more
		// routines sooner; 20 and 50 measured within 1% of each other.
		constexpr u32 kJumpTierUpRuns = 32;

		// A tiered routine checks for new jump targets every kRetierCheckMask + 1 runs
		// (when any JR/JALR missed the jump cache since), and is built again when it
		// finds some, at most kMaxRetiers times and not within kRetierMinRuns runs of its
		// last build. After kSettleChecks checks in a row that find nothing it stops
		// checking: the jump cache misses whenever a game rewrites micro memory, so the
		// miss count alone says little.
		constexpr u32 kRetierCheckMask = 0x3ff;
		constexpr u32 kMaxRetiers = 2;
		constexpr u32 kRetierMinRuns = 1024;
		constexpr u32 kSettleChecks = 3;

		// Recorded targets kept per jumping block and per program: a data-driven
		// jump (GTA, Burnout 3) can reach hundreds, and every one becomes a node.
		constexpr u32 kMaxTargetsPerJump = 48;
		constexpr u32 kMaxTargetsPerProgram = 512;

		// What set the sticky-STATUS tripwire off. While it is clear, regions drop the sticky byte
		// of STATUS (RegionTier::dropSticky) whatever the flag hack; once anything below happens it
		// stays set until the VM is reset or booted, and regions are built and kept exact.
		enum class TripCause : u8
		{
			None,
			ProgramImage, // a program was created (or loaded from the disk cache), or a tier-up job made, over a micro image with a sticky reader
			Compile,      // microVU compiled an FSAND/FSEQ/FSOR that selects a sticky bit
			OutsideRead,  // VU0 (micro or macro mode) touched VU1's STATUS register through its window onto VU1's registers
			Forced,       // a test or a runner flag set it off
			Remembered,   // the running title set it off in an earlier session
			kCount,
		};
		constexpr u32 kTripCauses = static_cast<u32>(TripCause::kCount);
		const char* TripCauseName(TripCause c);

		struct Stats
		{
			u64 tierUps = 0;          // routines tiered up: a first tiered body built
			u64 jumpTierUps = 0;      // ... of them, asked for by the JR/JALR slow path (counted in tierUps too)
			u64 jumpCounted = 0;      // JR/JALR slow-path entries counted on a routine
			u64 crossProgTargets = 0; // jump-cache entries seen at a harvest that resolved to another program
			u64 retiers = 0;          // tiered bodies built again for new jump targets
			u64 retierChecks = 0;     // times a tiered routine looked for new targets
			// The chunk store's tiered bodies: held now, their code, placements of one for a copy of
			// its routine, and of those the copies that were running microVU's code before.
			u64 tieredBodies = 0;
			u64 tieredBodyBytes = 0;
			u64 tieredPlaced = 0;
			u64 tieredReAimed = 0;
			u64 targetsRecorded = 0;  // jump targets handed to regions
			u64 jumpMisses = 0;       // JR/JALR the jump cache did not know (VU1)
			u64 slowDispatches = 0;   // VU1 dispatches that took the full search while the tier is on
			u64 rootsCompiled = 0;    // roots offered to the region tier
			u64 regions = 0;
			u64 regionNodes = 0;
			u64 regionMerged = 0;     // ... of them, aliases sharing another node's body (variant merging)
			u64 regionExits = 0;
			u64 regionBytes = 0;
			u64 bodyBlocks = 0;       // blocks offered to the body tier (compiled around the regions)
			u64 bodiesLowered = 0;
			u64 bodyBytes = 0;
			u64 bailCompiles = 0;     // regions' blocks microVU compiled at run time (budget shortfalls)
			u64 bailsEntry = 0;       // budget bails taken by regions at an entry from microVU
			u64 bailsLoopHead = 0;    // ... and at a loop head
			// Static census of the built regions' flag reads: status readers (FSAND/FSEQ/FSOR), those
			// whose mask selects a sticky bit, regions that dropped the sticky byte, and regions that
			// kept it because one of their own nodes reads it.
			u64 statusReaders = 0;
			u64 stickyReaders = 0;
			u64 stickyDivReaders = 0; // ... of those, the ones that read the sticky I / D bits (STATUS bits 10-11)
			u64 stickyDropped = 0;
			u64 stickyKept = 0;
			// The sticky-STATUS tripwire: the number of times it went off (0 in a normal session;
			// a VM reset or boot re-arms it), by what set it off, and the VU1 resets it asked for
			// to rebuild the regions that had dropped the sticky byte.
			u64 stickyTrips = 0;
			u64 stickyTripsBy[kTripCauses] = {};
			u64 stickyTripResets = 0;
			// Sessions (VM boots and resets, or a title change) that started with the tripwire set because
			// the running title set it off before (TripCause::Remembered; not counted in stickyTrips).
			u64 stickyRemembered = 0;
			// pcsx2-eerunner --count-vu1-ins only: user-mode instructions VU1's compile
			// sessions retired, in the builds of tiered bodies and in the rest.
			u64 compileInsTiered = 0;
			u64 compileInsPlain = 0;

			// Background jobs. A job builds the regions of one routine on the worker thread.
			u64 jobs = 0;
			u64 jobsDropped = 0;      // the worker's queue was full
			u64 jobsDone = 0;
			u64 jobsDeclined = 0;     // the region builder declined the root
			u64 built = 0;            // jobs that became a tiered body
			// Finished jobs thrown away: made before a reset (stale), for a routine no copy of
			// which was entered again (memory), for a root that another routine's body took in
			// as a node meanwhile (covered), made with the sticky
			// byte dropped before the tripwire went off (sticky), with no room left in the code
			// cache (space), whose root microVU analysed differently when it compiled it (trace),
			// or whose compile the chunk store could not keep (unstorable).
			u64 discardStale = 0, discardMemory = 0, discardCovered = 0, discardSticky = 0, discardSpace = 0, discardTrace = 0,
				discardUnstorable = 0;
			// VU1 dispatches between a job's request and its build.
			u64 lagSum = 0, lagMax = 0;
			// pcsx2-eerunner --count-vu1-ins only: user-mode instructions the worker
			// retired in jobs, and the VU1 thread in builds.
			u64 workerIns = 0;
			u64 buildIns = 0;
			u64 veneers = 0;          // branches from placed code to targets out of B/BL range
			std::map<std::string, u32> regionDeclined;
		};

		// Whether the tier runs this session (EnableVUAot, VU1 recompiled, no
		// gamefix vu-aot declines). Settled at init and at every reset.
		bool IsEnabled();

		// The sticky-STATUS tripwire across sessions: a title (disc serial and CRC) that set the
		// tripwire off is remembered in the program cache's directory (vu_jit/vuaot_tripped.txt),
		// and every later session of it starts with the tripwire set. SetTitle is what VMManager
		// calls when the disc details change; an empty serial is no title.
		void SetTitle(const std::string& serial, u32 crc);

		// The sticky-STATUS tripwire. Armed (false) at the start; set by the first sticky reader
		// the tier hears of -- a micro image containing one is uploaded or compiled, or anything
		// outside VU1 reads VU1's STATUS -- and then kept for the session: a savestate load or a
		// settings change does not re-arm it, a VM reset or boot does (ResetStickyTripwire).
		// Setting it off counts it, logs it once, and has VU1 reset at its next run so the regions
		// built without the sticky byte are rebuilt exact; a job made before it is not built into
		// a body, and a body made before it is not placed.
		bool StickyTripped();
		TripCause StickyTripCause();
		void ResetStickyTripwire();

		Stats GetStats();
		// Human-readable counters, including every decline reason.
		std::string FormatStats();

#ifdef PCSX2_RECOMPILER_TESTS
		// Test runners only: tier routines up after `runs` dispatches instead of
		// kTierUpRuns (a low number puts almost every routine through vu-aot).
		void SetTierUpRuns(u32 runs);
		// ... and after `runs` JR/JALR slow-path entries instead of kJumpTierUpRuns.
		void SetJumpTierUpRuns(u32 runs);

		// Deterministic mode: every job is waited for and built into a tiered body
		// at the slow path that asked for it. Off: the worker builds while microVU
		// runs.
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
		// ... at the next placement of a body that dropped the sticky byte, after the
		// check that allowed it and before the note that one is live: the moment a
		// trip from another thread can land.
		void TripAtNextDroppedBodyPlacement();
#endif
		// Test only: break parts of the tripwire on purpose, to show the tests notice. A mask of
		// TripMask(TripMutation), 0 = none. (Declared in every build so the tier's checks compile; in a
		// build without the recompiler test hooks they are constant false and generate nothing.)
		enum TripMutation : int
		{
			kTripMutNone = 0,
			kTripMutProgramImage = 1,  // no scan of the micro image when a program is created or a job is made
			kTripMutCompile = 2,       // no hook on a compiled sticky reader
			kTripMutOutside = 3,       // VU0 touching VU1's STATUS is not noticed
			kTripMutPublish = 4,       // a job made with the drop is built into a body after the trip
			kTripMutJobOption = 5,     // a job made after the trip still drops the sticky byte
			kTripMutNoRebuild = 6,     // the trip does not have VU1 rebuild its regions
			kTripMutPlace = 7,         // a body made with the drop is placed after the trip
		};
		constexpr int TripMask(int mutation) { return 1 << mutation; }
#ifdef PCSX2_RECOMPILER_TESTS
		void SetTripwireMutation(int mask);
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
	// The heap memory a VU's programs and blocks hold (microVU::progBytes), the
	// programs alive, the amount at which the recompiler resets, and what one
	// program and one block count for.
	struct ProgramMemory
	{
		u64 bytes = 0, programs = 0, resetAt = 0, programBytes = 0, blockBytes = 0;
	};
	ProgramMemory GetProgramMemory(u32 vu);
	// The jump tables the blocks of VU1's programs use (a table shared by several blocks counts once), and
	// how many bytes of them the host has resident (0 where it cannot say).
	struct JumpTableUsage
	{
		u64 liveTables = 0, liveResident = 0;
		// Bytes in the pages that hold an entry. A read of a table maps the host's shared zero page, which
		// `Resident` counts and the process does not pay for.
		u64 liveWritten = 0;
		// Every table VU1 holds until its next reset, used by a live block or not (microVU::jumpTables), and
		// the bytes in their pages that hold an entry.
		u64 heldTables = 0, heldWritten = 0;
	};
	JumpTableUsage GetJumpTableUsage();
} // namespace mvu_test_hooks
#endif
