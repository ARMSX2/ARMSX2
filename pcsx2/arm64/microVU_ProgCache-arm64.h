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

// Disk-side scaffolding for the persisted-JIT VU program cache. Owns:
//   1. The per-VU vu_jit/vu{0,1}/{VERSION,INDEX,<shard>/...} filesystem layout.
//   2. Version handshake: a build-time ABI version + options sentinel + arch tag
//      anchors the cache to a specific PCSX2 binary + config. Mismatch on startup
//      atomically renames vu_jit.../<old>/ → vu_jit.../<old>.stale.<ns>/ and
//      starts fresh.
//   3. In-memory INDEX populated from disk at Init. Subsequent Save calls append
//      one variable-length entry per program to disk + in-memory map.
//   4. TryLoadProgram hydrates the block graph via mVUPersist::HydrateProgram
//      on an INDEX+payload hit. Constant-VA arena layout + a placement-relative
//      fixup table make the persisted vixl output relocatable (absolute
//      armEmitCall / armMoveAddressToReg targets are patched on load); any
//      miss or payload rejection falls back to a normal recompile.
namespace mVUProgCache
{
	// Bumped together with kMvuCompilerAbiVersion when the on-disk layout
	// changes. Mismatch nukes the cache directory at startup.
	//
	// 4: an INDEX record is a fixed 64 bytes (content hash, sizes, flags,
	//    timestamps). Version 3 followed each record with a list of entry
	//    PCs; nothing read that list outside tests (hydration looks a
	//    program up by content hash) and the dispatcher no longer kept it
	//    complete, so it is gone. A version-3 directory is evicted rather
	//    than parsed with the wrong record size.
	static constexpr u32 kProgCacheFormatVersion = 4;

	// Initialize the per-VU on-disk cache. Gated on the
	// EmuCore/CPU/Recompiler EnableVUProgramCache config bool or EnableVUAot,
	// which turns the cache on (UseVUProgramCache; neither = no disk side
	// effects at all). Reads VERSION; if it diverges from the running
	// build's (compiler ABI, options sentinel, arch tag), evicts the stale
	// dir and starts fresh. Loads INDEX into memory. Idempotent — called
	// from every mVUreset (the reset call is what activates a runtime config
	// toggle), not from mVUinit, which runs before the settings are loaded;
	// only does real work on first effective call.
	void Init(microVU& mVU);

	// Same Init logic but takes (vu_index, options_sentinel) directly so
	// test TUs don't have to pull microVU-arm64.h (which includes the .inl
	// bodies and would link-collide with libpcsx2.a). Not for production
	// use; mVU.optionsSentinel is built by mVUbuildOptionsSentinel during
	// mVUreset and that path must run before any Init.
	void InitWithSentinel(u32 vu_index, const XXH128_hash_t& options_sentinel);

	// Persist pending writes, close handles. Called from mVUclose.
	void Close(microVU& mVU);

	// Walk mVU.mvuContentMap and save every program whose contentHash is not
	// already represented in the on-disk INDEX (or whose recorded block graph
	// grew since its payload was written). Called from mVUreset
	// (pre-program-free) and mVUclose so the next process boot has artifacts.
	void SaveAllPrograms(microVU& mVU);

	// Save a single program: append an INDEX entry, and — when the program
	// carries a persist log (mVUPersist recording was enabled while it
	// compiled) — write the serialized block graph as a content-addressed
	// .vuprog payload (<root>/<hh>/<hash32hex>.vuprog, tmp+rename). The
	// payload is rewritten when the recorded block graph grew past what is
	// on disk; the INDEX append short-circuits when the payload is current.
	void SaveProgram(microVU& mVU, const microProgram& prog);

	// Promote an on-disk program to a live one. On an INDEX hit with a
	// readable .vuprog payload, hydrates the block graph through
	// mVUPersist::HydrateProgram and returns the new program (registered in
	// mVU.mvuContentMap, dispatchable immediately). Returns nullptr on
	// INDEX miss, missing/corrupt payload, or any hydration rejection
	// (layout-base / content-hash / structural mismatch) — the caller falls
	// back to recompiling. Called from mVUsearchProg with the code cache
	// OPEN; the (empty) emission episode is closed around the hydration and
	// reopened before returning.
	microProgram* TryLoadProgram(microVU& mVU, const XXH128_hash_t& contentHash);

	// vu-aot sidecars (VU1): `<root>/<hh>/<hash32hex>.vuaot`, beside the program's payload, written
	// and read for the vu-aot live tier (vuaot/VuAotPersist.h has the format). The cache's directory
	// handshake covers them like the payloads: an options change renames them aside with the rest.
	//
	// Whether the cache is up for this VU (sidecars can be read and written).
	bool SidecarsEnabled(u32 vu_index);
	// The sidecar for `hash`: from the preload (consumed), else from disk. False when there is none.
	bool ReadSidecar(u32 vu_index, const XXH128_hash_t& hash, std::vector<u8>& out);
	// Hands a sidecar to the cache's writer thread: `build` runs there and returns the bytes to write
	// (tmp + rename), or nothing to skip the write. A newer request for a hash replaces an older one
	// still queued. The caller's thread never touches the disk.
	void QueueSidecar(u32 vu_index, const XXH128_hash_t& hash, std::function<std::vector<u8>()> build);
	// Waits until every queued sidecar has been written (Close does this too).
	void FlushSidecars(u32 vu_index);

	// What ClearVuAot removed.
	struct ClearResult
	{
		u64 files = 0;
		u64 bytes = 0;
	};

	// Removes the vu-aot cache from disk, for both VUs: the sidecars (`.vuaot`), and with `wholeCache` the
	// rest of the program cache too (the `.vuprog` payloads, INDEX and VERSION, and the `.stale.*`
	// directories an options change set aside). The tripwire's memory (`vu_jit/vuaot_tripped.txt`) is
	// kept: it is a safety record, a few bytes. The writer and preload threads are stopped first, so nothing
	// writes back what is deleted and nothing reads what is gone, and what they held is dropped (a sidecar
	// still queued for writing is not written). With `wholeCache` the cache in memory is closed as well, so
	// the next Init starts a fresh one. Safe when the directories do not exist. CPU thread, with the VU
	// recompilers idle (a settings change has just reset them). Logs one line when it removed anything.
	ClearResult ClearVuAot(bool wholeCache);

	// What a change of the EnableVUAot setting owes the disk cache: nothing, unless it went from on to
	// off. Then the sidecars go, and the whole cache with them when nothing else keeps it
	// (`programCacheStillOn` is EnableVUProgramCache after the change).
	ClearResult OnVuAotSettingChanged(bool wasOn, bool isOn, bool programCacheStillOn);

	// Dispatch-time probe used by mVUsearchProg on the in-process-miss
	// path. Tracks whether the INDEX already knows the (program, startPC)
	// tuple the dispatcher is about to recompile — the hit-rate equals the
	// upper bound on what a real hydration path would have served. Cheap
	// (one unordered_map lookup), called once per program-switch — kept
	// off the per-dispatch fast path.
	void ObserveDispatchHash(microVU& mVU, const XXH128_hash_t& contentHash,
	                          u32 startPC_bytes);

	// DevCon summary of save / hit / miss / evict counters.
	void DumpStats(const microVU& mVU, const char* tag);

	// Test-only: drop per-VU in-memory state (entries, stats, initialized
	// flag) so the next Init() re-runs the disk-handshake path. Does NOT
	// touch the on-disk cache directory; the caller is responsible for
	// that.
	void ResetForTest(u32 vu_index);

	// The running build's kMvuCompilerAbiVersion (microVU-arm64.h can't be
	// included from test TUs — it re-emits the .inl bodies). Used by the
	// ABI-digest guard test to key its pinned-digest table.
	u32 GetCompilerAbiVersion();

	// Test-only: re-run Init for the given VU using the LIVE microVU's
	// options sentinel (production Init is config-gated and only fires from
	// mVUinit/mVUreset — this lets a test point EmuFolders::Cache at a temp
	// dir and bring the disk cache up mid-process without rebuilding the
	// whole VU, regardless of EmuConfig). Rebuilds the sentinel first if a
	// recording-state change invalidated it. Call ResetForTest first.
	void TestReinitFromLiveSentinel(u32 vu_index);

	// Test-only: the given VU's live options sentinel, rebuilt first if
	// stale. Lets tests pin sentinel-identity properties (e.g. that the
	// mVUPersist recording state is mixed in — a cache of recording-enabled
	// code forms must never be served to a recording-disabled run and vice
	// versa).
	bool TestGetLiveSentinel(u32 vu_index, XXH128_hash_t* out_sentinel);

	// Test-only: the preload's byte budget for the next Init of this VU (0: the default). ResetForTest
	// puts the default back.
	void TestSetPreloadBudget(u32 vu_index, size_t bytes);
	// Test-only: the preload holds the payload (or, with `sidecar`, the vu-aot sidecar) of `hash` in RAM.
	bool TestIsPreloaded(u32 vu_index, const XXH128_hash_t& hash, bool sidecar);
	// Test-only: while held, the preload thread has read each file it is going to keep but does not put it in
	// the map (a preload slower than the first loads of the session, which on a loaded machine or a slow
	// disk it can be). Set before Init; released with false. ResetForTest releases it.
	void TestHoldPreload(u32 vu_index, bool hold);
	// Test-only: write a payload the way SaveProgram does (tmp + rename), and so supersede any copy of it the
	// preload holds or is about to put in memory.
	bool TestWritePayload(u32 vu_index, const XXH128_hash_t& hash, const std::vector<u8>& image);
	// Test-only: the preload thread is waiting in the hold, with a file read and not yet in the map.
	bool TestPreloadIsHeld(u32 vu_index);
	// Test-only: block until the background payload preload spawned by Init
	// (if any) has finished. Lets tests deterministically assert the RAM-
	// served hydration path (e.g. by deleting the on-disk payload after the
	// preload and proving hydration still succeeds).
	void TestWaitForPreload(u32 vu_index);
	// Test-only: the path a sidecar for `hash` is written to.
	std::string TestSidecarPath(u32 vu_index, const XXH128_hash_t& hash);

	// Test-only: append an INDEX entry for `hash`, applying the same
	// persistence path SaveProgram uses (single atomic append + in-memory
	// registration; dedupe on existing hash). Returns true iff the entry was
	// written. Lets format tests drive the record serializer without
	// constructing a microVU/microProgram.
	bool TestAppendIndexEntry(u32 vu_index, const XXH128_hash_t& hash);

	// Test-only: true if the in-memory INDEX of the given VU holds a live
	// entry for `hash`. Used by the IndexFormat tests to pin the record
	// serializer's round-trip behavior.
	bool TestHasIndexEntry(u32 vu_index, const XXH128_hash_t& hash);

	// Test-only: read back the per-VU counters DumpStats would log. Used by
	// the versioning tests to assert that eviction fired (staleEvictions++)
	// or that the cache came up cleanly.
	struct Stats
	{
		bool initialized        = false;
		bool enabled            = false;
		u64  entries            = 0;
		u64  savesAttempted     = 0;
		u64  savesWritten       = 0;
		u64  wouldBeHits        = 0;
		u64  misses             = 0;
		u64  staleEvictions     = 0;
		u64  dispatchProbes     = 0;
		u64  dispatchIndexAvail = 0;
		u64  payloadWrites      = 0; // .vuprog files written (tmp+rename)
		u64  payloadHits        = 0; // TryLoadProgram hydrations served
		u64  payloadMissing     = 0; // INDEX hit but no .vuprog on disk
		u64  payloadRejects     = 0; // payload read but hydration refused
		u64  preloadedPayloads  = 0; // .vuprog files read by the Init-time
		                             // background preload thread
		u64  preloadedBytes     = 0; // bytes currently held by the preload
		                             // map (consumed buffers are released)
		u64  preloadHits        = 0; // hydrations served from RAM instead of
		                             // a dispatch-thread disk read
		u64  sidecarReads       = 0; // ReadSidecar calls that found one
		u64  sidecarPreloadHits = 0; // ... of them, served from the preload
		u64  sidecarsPreloaded  = 0; // .vuaot files the preload read
		u64  sidecarPreloadBytes = 0; // ... and their bytes (inside the preload budget)
		u64  sidecarWrites      = 0; // .vuaot files written
		u64  sidecarWriteBytes  = 0; // ... and their bytes
		u64  sidecarSkips       = 0; // requests whose build returned nothing
		u64  preloadBudgetHit   = 0; // 1 when the preload stopped at its budget
		u64  preloadWantedBytes = 0; // payload + sidecar bytes the preload would have read without one
		u64  preloadReadBytes   = 0; // payload + sidecar bytes the preload did read
		u64  ageOutFiles        = 0; // payload and sidecar files deleted for being unused too long
		u64  ageOutBytes        = 0;
	};
	Stats GetStats(u32 vu_index);
}
