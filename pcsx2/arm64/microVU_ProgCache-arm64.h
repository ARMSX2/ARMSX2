// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <string>
#include <vector>

#ifndef XXH_versionNumber
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"
#endif

struct microVU;
struct microProgram;

// The persisted-JIT VU program cache, held in RAM for the whole session.
//
// One pack file per game, VU and options sentinel:
//   $EmuFolders::Cache/vu_jit/packs/<title>/vu{0,1}-<sentinel hex>.pack
// A pack is a header (format, compiler ABI, VU, sentinel, arch) and then an
// append-only log of records, each a program's serialized block graph (the
// mVUPersist image) or its vu-aot sidecar, keyed by the program's content hash
// and checksummed. The last record of a kind for a hash wins; a record cut
// short ends the log.
//
// When a pack opens (the first VU reset of a session, or a reset after the
// title or the sentinel changed) a background thread reads the whole file in
// one pass into an in-memory table, and the table is the cache from then on:
// loading a program reads RAM and never the disk, and a program comes back
// from RAM as often as resets throw it away. Saves update the table; a
// background writer appends what changed every 30 seconds, and once more,
// within a deadline, when the cache closes. The emulation and VU threads never
// touch the disk. A lookup made while the pack is still being read waits for
// the read, up to a few seconds per pack, then counts as a miss.
//
// Recompiled code is relocatable: the image records a fixup for every host
// address in it, patched against the running process at hydration
// (mVUPersist::HydrateProgram); any rejection falls back to a normal compile.
namespace mVUProgCache
{
	// Bumped together with kMvuCompilerAbiVersion when the on-disk layout
	// changes. A pack of another format is ignored and replaced.
	//
	// 5: one pack per game, VU and sentinel (a log of program and sidecar
	//    records) in place of a directory per VU and sentinel holding an
	//    INDEX, a .vuprog per program and a .vuaot per sidecar.
	static constexpr u32 kProgCacheFormatVersion = 5;

	// The game whose pack the next Init or Rekey opens: its serial and CRC (both empty for none).
	// Called with the disc details, before the VM boot's VU reset.
	void SetTitle(const std::string& serial, u32 crc);

	// Opens this VU's pack for the current title and the VU's options sentinel,
	// and starts the read. Gated on the EmuCore/CPU/Recompiler
	// EnableVUProgramCache config bool or EnableVUAot, which turns the cache on
	// (UseVUProgramCache; neither = no disk side effects at all). Idempotent:
	// called from every mVUreset (the reset is what activates a runtime config
	// toggle), not from mVUinit, which runs before the settings are loaded.
	void Init(microVU& mVU);

	// Same Init logic but takes (vu_index, options_sentinel) directly so
	// test TUs don't have to pull microVU-arm64.h (which includes the .inl
	// bodies and would link-collide with libpcsx2.a).
	void InitWithSentinel(u32 vu_index, const XXH128_hash_t& options_sentinel);

	// After a reset's save: when the title or the VU's sentinel is no longer the
	// open pack's, hands the open pack to the writer to finish and opens the
	// right one. Called from mVUreset after SaveAllPrograms.
	void Rekey(microVU& mVU);

	// Writes what changed (within the close deadline), stops the threads and
	// drops the table. Called from mVUclose.
	void Close(microVU& mVU);

	// Stores every live program whose block graph grew since it was last
	// stored (or was never stored). Called from mVUreset (pre-program-free)
	// and mVUclose. RAM only: the writer puts it on disk.
	void SaveAllPrograms(microVU& mVU);

	// Stores one program's serialized block graph in the table, unless the
	// stored image already holds every block (mVUPersist::LogShape). A program
	// with no persist log (recording off, or non-persistable) stores nothing.
	void SaveProgram(microVU& mVU, const microProgram& prog);

	// Promote a stored program to a live one: hydrates the block graph through
	// mVUPersist::HydrateProgram and returns the new program (registered in
	// mVU.mvuContentMap, dispatchable immediately). Returns nullptr when the
	// table has no image for the hash or hydration refused it (the caller
	// recompiles). Called from mVUsearchProg with the code cache OPEN; the
	// (empty) emission episode is closed around the hydration and reopened
	// before returning.
	microProgram* TryLoadProgram(microVU& mVU, const XXH128_hash_t& contentHash);

	// vu-aot sidecars (VU1): a second record per program in the same pack, read and written for the
	// vu-aot live tier (vuaot/VuAotPersist.h has the format).
	//
	// Whether the cache is up for this VU (sidecars can be read and written).
	bool SidecarsEnabled(u32 vu_index);
	// The sidecar for `hash`, from the table. A request still queued for it is built first, so the answer
	// is always the newest. False when there is none.
	bool ReadSidecar(u32 vu_index, const XXH128_hash_t& hash, std::vector<u8>& out);
	// Queues a sidecar: `build` runs on the writer thread at its next flush (or on the reader's thread, if
	// ReadSidecar asks first) and returns the bytes to store, or nothing to keep what is stored. A newer
	// request for a hash replaces an older one still queued. The caller's thread never touches the disk.
	void QueueSidecar(u32 vu_index, const XXH128_hash_t& hash, std::function<std::vector<u8>()> build);
	// Builds every queued sidecar and writes everything that changed to disk now, and waits for it.
	void FlushSidecars(u32 vu_index);

	// What ClearVuAot removed: records dropped from packs (or, with the whole cache, files deleted) and
	// the bytes they held on disk.
	struct ClearResult
	{
		u64 files = 0;
		u64 bytes = 0;
	};

	// Removes the vu-aot cache, for both VUs: every sidecar record, from the open tables, the queue and
	// every pack on disk; with `wholeCache` every pack, and the directories of the older layouts. The
	// tripwire's memory (`vu_jit/vuaot_tripped.txt`) is kept: it is a safety record, a few bytes. A
	// sidecar still queued is dropped, not written. With `wholeCache` the cache in memory is closed as
	// well, so the next Init starts a fresh one. Safe when the directories do not exist. CPU thread, with
	// the VU recompilers idle (a settings change has just reset them). Logs one line when it removed anything.
	ClearResult ClearVuAot(bool wholeCache);

	// What a change of the EnableVUAot setting owes the disk cache: nothing, unless it went from on to
	// off. Then the sidecars go, and the whole cache with them when nothing else keeps it
	// (`programCacheStillOn` is EnableVUProgramCache after the change).
	ClearResult OnVuAotSettingChanged(bool wasOn, bool isOn, bool programCacheStillOn);

	// Dispatch-time probe used by mVUsearchProg on the in-process-miss
	// path: whether the table holds an image for the program the dispatcher
	// is about to make. Called once per program-switch, off the per-dispatch
	// fast path.
	void ObserveDispatchHash(microVU& mVU, const XXH128_hash_t& contentHash,
	                          u32 startPC_bytes);

	// DevCon summary of the counters below.
	void DumpStats(const microVU& mVU, const char* tag);

	// Test-only: what Close does with no deadline (everything is written),
	// plus the counters, the test overrides and the once-per-process
	// leftover pass, so the next Init behaves like a new process's.
	void ResetForTest(u32 vu_index);

	// The running build's kMvuCompilerAbiVersion (microVU-arm64.h can't be
	// included from test TUs — it re-emits the .inl bodies). Used by the
	// ABI-digest guard test to key its pinned-digest table.
	u32 GetCompilerAbiVersion();

	// Test-only: re-run Init for the given VU using the LIVE microVU's
	// options sentinel (production Init is config-gated and only fires from
	// mVUreset — this lets a test point EmuFolders::Cache at a temp dir and
	// bring the cache up mid-process without rebuilding the whole VU,
	// regardless of EmuConfig). Rebuilds the sentinel first if a
	// recording-state change invalidated it. Call ResetForTest first.
	void TestReinitFromLiveSentinel(u32 vu_index);

	// Test-only: the given VU's live options sentinel, rebuilt first if
	// stale. Lets tests pin sentinel-identity properties (e.g. that the
	// mVUPersist recording state is mixed in — a cache of recording-enabled
	// code forms must never be served to a recording-disabled run and vice
	// versa).
	bool TestGetLiveSentinel(u32 vu_index, XXH128_hash_t* out_sentinel);

	// Test-only: the options sentinel the given VU hashes against, rebuilt from
	// the live config rather than trusting the cached one. Unlike
	// TestGetLiveSentinel it also does not require the microVU object to have
	// been through mVUinit — it stamps the VU index itself — so a test TU with
	// no VM behind it can still compare the two VUs' program identities.
	bool TestBuildSentinelForIndex(u32 vu_index, XXH128_hash_t* out_sentinel);

	// Test-only: block until the open pack's read (and the leftover pass) has finished.
	void TestWaitForPreload(u32 vu_index);
	// Test-only: the path of the open pack ("" when none).
	std::string TestPackPath(u32 vu_index);
	// Test-only: the open table holds the program image (or, with `sidecar`, the sidecar) of `hash`.
	bool TestHasEntry(u32 vu_index, const XXH128_hash_t& hash, bool sidecar);
	// Test-only: the RAM cap for the next pack read of this VU (0: the default). ResetForTest puts the default back.
	void TestSetRamCap(u32 vu_index, size_t bytes);
	// Test-only: while held, the pack read has read the file but does not put it in the table. Set before
	// Init; released with false. ResetForTest releases it.
	void TestHoldLoad(u32 vu_index, bool hold);
	// Test-only: the read is waiting in the hold.
	bool TestLoadIsHeld(u32 vu_index);
	// Test-only: what Close does (the writer's last pass within the close deadline), keeping the counters.
	void TestClose(u32 vu_index);
	// Test-only: the deadline Close gives the writer (0: the default).
	void TestSetCloseDeadlineMs(u32 vu_index, u32 ms);
	// Test-only: while set, every record the writer writes waits this long first (a slow card).
	void TestSetWriteDelayMs(u32 vu_index, u32 ms);

	struct Stats
	{
		bool initialized        = false;
		bool enabled            = false;
		u64  entries            = 0; // hashes in the open table
		u64  ramBytes           = 0; // image and sidecar bytes the open table holds
		u64  savesAttempted     = 0; // SaveProgram calls
		u64  payloadWrites      = 0; // ...that stored an image (new, or grown)
		u64  wouldBeHits        = 0; // TryLoadProgram lookups the table knew
		u64  misses             = 0; // ...and ones it did not
		u64  payloadHits        = 0; // hydrations served
		u64  payloadMissing     = 0; // known hash, no image (only a sidecar, or a refused image)
		u64  payloadRejects     = 0; // image read but hydration refused
		u64  dispatchProbes     = 0;
		u64  dispatchIndexAvail = 0; // subset where the table had an image
		u64  packsLoaded        = 0; // pack files read
		u64  loadedRecords      = 0; // records they held
		u64  loadedBytes        = 0; // bytes read
		u64  loadNs             = 0; // time the reads took
		u64  loadWaitNs         = 0; // time lookups waited for a read
		u64  missesDuringLoad   = 0; // lookups that gave up waiting
		u64  ramCapHit          = 0; // 1 when a read stopped putting records in RAM at the cap
		u64  tornTails          = 0; // packs whose log ended in a broken record (cut off on the next write)
		u64  otherFormat        = 0; // packs of another format (replaced on the next write)
		u64  sidecarReads       = 0; // ReadSidecar calls that found one
		u64  sidecarWrites      = 0; // sidecars stored (built)
		u64  sidecarWriteBytes  = 0;
		u64  sidecarSkips       = 0; // requests whose build returned nothing
		u64  flushes            = 0; // writer passes that wrote something
		u64  recordsWritten     = 0;
		u64  bytesWritten       = 0;
		u64  rewrites           = 0; // packs written whole (compaction)
		u64  writeFailures      = 0;
		u64  closeDeadlineHit   = 0; // 1 when Close stopped writing at the deadline
		u64  recordsDropped     = 0; // ...and the records it left unwritten
		u64  leftoverFiles      = 0; // files of older layouts, interrupted writes and packs unused too long, deleted
		u64  leftoverBytes      = 0;
	};
	Stats GetStats(u32 vu_index);
}
