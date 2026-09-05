// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Implementation of microVU_ProgCache-arm64.h. Included exactly once from
// microVU-arm64.cpp — the mVU module is a single TU (the microVU_*-arm64.inl
// files emit non-inline function bodies), so this implementation lives next to
// them rather than as a stand-alone .cpp.

#include "Config.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mVUProgCache
{
namespace
{
//------------------------------------------------------------------
// Disk layout
//   $EmuFolders::Cache/vu_jit/
//     vu0/
//       VERSION                          (64-byte handshake block)
//       INDEX                            (append-only stream of entries)
//       <hh>/...                         (payload artifacts)
//     vu1/  (same shape)
//
// VERSION + INDEX are per-VU so each VU initializes independently from the
// sequential mVUinit(microVU0) / mVUinit(microVU1) calls in cpuMicroVU::Init.
//------------------------------------------------------------------
static constexpr u32  kVersionMagic       = 0x4A56554Du; // 'MVUJ'
static constexpr char kRootDirName[]      = "vu_jit";
static constexpr char kVersionFileName[]  = "VERSION";
static constexpr char kIndexFileName[]    = "INDEX";

static_assert(sizeof(XXH128_hash_t) == 16, "XXH128_hash_t expected 16 bytes");

#pragma pack(push, 1)
struct VersionHeader
{
	u32           magic;              // kVersionMagic
	u32           formatVersion;      // kProgCacheFormatVersion
	u32           compilerAbiVersion; // kMvuCompilerAbiVersion
	u32           vuIndex;            // 0 or 1 — sentinel mixes VU index
	XXH128_hash_t optionsSentinel;    // this VU's mVU.optionsSentinel
	char          archTag[32];        // "arm64-jit"
};
static_assert(sizeof(VersionHeader) == 64, "VersionHeader layout drift");

// One INDEX record: a fixed 64 bytes, appended whole. A record whose
// flags bit 0 is clear is a tombstone and is skipped on load.
struct IndexEntryHeader
{
	XXH128_hash_t contentHash;  // 16 — the program's identity
	u32           vuIndex;      //  4
	u32           codeSize;     //  4 — size of the last-written .vuprog
	                            //      payload (0 = no payload on disk)
	u32           blockCount;   //  4
	u32           flags;        //  4 — bit 0 = valid; tombstones leave it clear
	u64           lastUsedNs;   //  8
	u64           execCount;    //  8 — running dispatch count, reserved
	u8            pad[16];      // 16 — round to 64 for trivial mmap
};
static_assert(sizeof(IndexEntryHeader) == 64,
              "IndexEntryHeader layout drift");
#pragma pack(pop)

struct State
{
	bool        initialized   = false;
	bool        enabled       = true;
	std::string root;        // $EmuFolders::Cache/vu_jit/vu{0,1}
	std::string indexPath;   // $root/INDEX
	std::string versionPath; // $root/VERSION

	std::unordered_map<XXH128_hash_t, IndexEntryHeader,
	                   MvuContentHashHash, MvuContentHashEq> entries;

	u64 wouldBeHits        = 0;
	u64 misses             = 0;
	u64 savesAttempted     = 0;
	u64 savesWritten       = 0;
	u64 staleEvictions     = 0;
	u64 dispatchProbes     = 0; // mVUsearchProg dispatch-time probes
	u64 dispatchIndexAvail = 0; // subset where the hash is in the INDEX
	u64 payloadWrites      = 0;
	u64 payloadHits        = 0;
	u64 payloadMissing     = 0;
	u64 payloadRejects     = 0;

	// Background payload preload. InitImpl spawns the thread when the INDEX
	// carries payload-bearing entries; it fills `preloaded` (most-recently-
	// used first, kPreloadBudgetBytes bound) and exits, so first-dispatch
	// hydration on the EE/MTVU threads is a map lookup instead of an eMMC
	// read. Buffers are consumed (moved out + erased) on hydration, so the
	// resident cost drains as the game warms up. preloadMutex guards
	// `preloaded` + the preload* counters; everything else in State keeps
	// its existing single-thread/reset-quiesced discipline.
	std::thread       preloadThread;
	std::mutex        preloadMutex;
	std::atomic<bool> preloadStop{false};
	std::atomic<bool> preloadHold{false}; // test hooks, see TestHoldPreload
	std::atomic<bool> preloadHeld{false};
	std::unordered_map<XXH128_hash_t, std::vector<u8>,
	                   MvuContentHashHash, MvuContentHashEq> preloaded;
	// The hashes this session has written a payload or a sidecar for, under preloadMutex. The preload
	// reads a file and puts it in `preloaded` a moment later, and a program that appears in between reads
	// the file itself and may write a newer one: what the preload then holds is the file from before, and
	// must not be answered after the newer one is on disk. A hash in these sets is never put in the maps.
	std::unordered_set<XXH128_hash_t, MvuContentHashHash, MvuContentHashEq> payloadsWritten;
	std::unordered_set<XXH128_hash_t, MvuContentHashHash, MvuContentHashEq> sidecarsWritten;
	u64 preloadedPayloads = 0;
	u64 preloadedBytes    = 0;
	u64 preloadHits       = 0;
	// vu-aot sidecars the preload read (consumed by ReadSidecar, like the payloads).
	std::unordered_map<XXH128_hash_t, std::vector<u8>,
	                   MvuContentHashHash, MvuContentHashEq> preloadedSidecars;
	u64 sidecarsPreloaded   = 0;
	u64 sidecarPreloadBytes = 0;
	u64 sidecarPreloadHits  = 0;
	u64 sidecarReads        = 0;
	u64 preloadBudgetHit    = 0;
	u64 preloadWantedBytes  = 0;
	u64 preloadReadBytes    = 0; // payload + sidecar bytes the preload read, over the session
	u64 ageOutFiles         = 0; // files the preload's age-out pass deleted
	u64 ageOutBytes         = 0;
	size_t preloadBudget    = 0; // 0: kPreloadBudgetBytes (a test sets another)

	// The sidecar writer (QueueSidecar): one thread, started by the first request and stopped
	// (after draining its queue) by Close. writerMutex guards the queue and the flags; the writer
	// reads `root`, which stays fixed while the cache is initialized.
	std::thread             writerThread;
	std::mutex              writerMutex;
	std::condition_variable writerCv;     // work queued, or stop
	std::condition_variable writerIdleCv; // queue empty and nothing being written
	std::deque<XXH128_hash_t> writerOrder;
	std::unordered_map<XXH128_hash_t, std::function<std::vector<u8>()>,
	                   MvuContentHashHash, MvuContentHashEq> writerQueue;
	bool writerStop = false;
	bool writerBusy = false;
	std::atomic<u64> sidecarWrites{0}, sidecarWriteBytes{0}, sidecarSkips{0};

	void StopPreload()
	{
		preloadStop.store(true, std::memory_order_relaxed);
		if (preloadThread.joinable())
			preloadThread.join();
	}

	// Writes whatever is queued, then ends the writer thread.
	void StopWriter()
	{
		{
			std::lock_guard<std::mutex> lock(writerMutex);
			writerStop = true;
		}
		writerCv.notify_all();
		if (writerThread.joinable())
			writerThread.join();
		std::lock_guard<std::mutex> lock(writerMutex);
		writerStop = false;
		writerOrder.clear();
		writerQueue.clear();
	}

	// Joinable-thread backstop: a std::thread destroyed while joinable
	// terminates the process. Close/ResetForTest join explicitly; this
	// covers exit paths that skip mVUclose.
	~State()
	{
		StopPreload();
		StopWriter();
	}
};

static State g_state[2];

static u64 NowNs()
{
	return static_cast<u64>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void BuildVersionHeader(u32 vu_index,
                                const XXH128_hash_t& options_sentinel,
                                VersionHeader& out)
{
	std::memset(&out, 0, sizeof(out));
	out.magic              = kVersionMagic;
	out.formatVersion      = kProgCacheFormatVersion;
	out.compilerAbiVersion = kMvuCompilerAbiVersion;
	out.vuIndex            = vu_index & 1u;
	out.optionsSentinel    = options_sentinel;
	std::strncpy(out.archTag, "arm64-jit", sizeof(out.archTag) - 1);
}

static bool VersionMatches(const VersionHeader& a, const VersionHeader& b)
{
	return a.magic              == b.magic
	    && a.formatVersion      == b.formatVersion
	    && a.compilerAbiVersion == b.compilerAbiVersion
	    && a.vuIndex            == b.vuIndex
	    && a.optionsSentinel.low64  == b.optionsSentinel.low64
	    && a.optionsSentinel.high64 == b.optionsSentinel.high64
	    && std::memcmp(a.archTag, b.archTag, sizeof(a.archTag)) == 0;
}

static bool WriteVersion(const std::string& path, const VersionHeader& hdr)
{
	// Atomic-replace via tmp+rename — keeps a partial write from poisoning
	// the cache on power-loss.
	const std::string tmp = path + ".tmp";
	if (!FileSystem::WriteBinaryFile(tmp.c_str(), &hdr, sizeof(hdr)))
		return false;
	return std::rename(tmp.c_str(), path.c_str()) == 0;
}

static void EvictStaleCache(State& s)
{
	if (!FileSystem::DirectoryExists(s.root.c_str()))
		return;
	const std::string stale = s.root + ".stale." + std::to_string(NowNs());
	if (std::rename(s.root.c_str(), stale.c_str()) != 0)
	{
		Console.Warning("mVUProgCache: rename %s -> %s failed",
		                s.root.c_str(), stale.c_str());
	}
	else
	{
		Console.WriteLn("mVUProgCache: evicted stale cache %s -> %s",
		                s.root.c_str(), stale.c_str());
		++s.staleEvictions;
	}
}

// Parse the INDEX: a stream of fixed 64-byte IndexEntryHeader records.
// A truncated tail (partial record) is dropped with a warning — the next
// SaveProgram appends a clean record, and the partial bytes don't poison
// any in-memory state. Last-writer-wins on duplicate contentHash so a
// re-save (payload growth) supersedes the earlier record.
static void LoadIndex(State& s)
{
	auto bytes = FileSystem::ReadBinaryFile(s.indexPath.c_str());
	if (!bytes.has_value())
		return;
	const u8* base = bytes->data();
	const size_t total = bytes->size();
	size_t off = 0;
	while (off + sizeof(IndexEntryHeader) <= total)
	{
		IndexEntryHeader hdr;
		std::memcpy(&hdr, base + off, sizeof(hdr));
		off += sizeof(hdr);

		if ((hdr.flags & 1u) == 0u)
			continue; // tombstone

		s.entries[hdr.contentHash] = hdr; // last-writer-wins
	}
	if (off != total)
	{
		Console.Warning("mVUProgCache: INDEX %s has %zu trailing bytes past last "
		                "record — ignoring", s.indexPath.c_str(), total - off);
	}
}

// Append one record with a single fwrite, so a process death can't leave a
// header without its tail.
static bool AppendIndexEntry(const State& s, const IndexEntryHeader& e)
{
	std::FILE* fp = FileSystem::OpenCFile(s.indexPath.c_str(), "ab");
	if (!fp)
		return false;
	const bool ok = std::fwrite(&e, sizeof(e), 1, fp) == 1;
	std::fclose(fp);
	return ok;
}

// Persist an entry: append to disk + register in the in-memory map.
static bool PersistIndexEntry(State& s, const IndexEntryHeader& e)
{
	if (!AppendIndexEntry(s, e))
	{
		Console.Warning("mVUProgCache[%u]: INDEX append failed", e.vuIndex);
		return false;
	}
	s.entries[e.contentHash] = e;
	++s.savesWritten;
	return true;
}

//------------------------------------------------------------------
// .vuprog payload files — the serialized block graph (mVUPersist image),
// content-addressed under a 256-way shard:
//   $root/<hh>/<hash32hex>.vuprog
// where <hh> is the first hex byte of the 128-bit content hash.
//------------------------------------------------------------------

static std::string PayloadPath(const State& s, const XXH128_hash_t& hash,
                                std::string* out_shard_dir = nullptr,
                                const char* extension = ".vuprog")
{
	char name[48];
	std::snprintf(name, sizeof(name), "%016" PRIx64 "%016" PRIx64 "%s",
		hash.high64, hash.low64, extension);
	const char shard[3] = {name[0], name[1], 0};
	std::string shard_dir = Path::Combine(s.root, shard);
	std::string path = Path::Combine(shard_dir, name);
	if (out_shard_dir)
		*out_shard_dir = std::move(shard_dir);
	return path;
}

// Atomic-replace write (tmp+rename), creating the shard dir on demand.
static bool WritePayload(State& s, const XXH128_hash_t& hash,
                          const std::vector<u8>& image)
{
	std::string shard_dir;
	const std::string path = PayloadPath(s, hash, &shard_dir);
	Error error;
	if (!FileSystem::EnsureDirectoryExists(shard_dir.c_str(), false, &error))
	{
		Console.Warning("mVUProgCache: cannot create payload shard for %s: %s",
			path.c_str(), error.GetDescription().c_str());
		return false;
	}
	const std::string tmp = path + ".tmp";
	if (!FileSystem::WriteBinaryFile(tmp.c_str(), image.data(), image.size()))
		return false;
	if (std::rename(tmp.c_str(), path.c_str()) != 0)
		return false;

	// A rewritten payload supersedes any not-yet-consumed preloaded copy
	// (which would otherwise serve the pre-growth image — sound under the
	// consistent-prefix rule, but pointlessly stale), and one the preload
	// has read and not yet put in memory (payloadsWritten).
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		s.payloadsWritten.insert(hash);
		auto pit = s.preloaded.find(hash);
		if (pit != s.preloaded.end())
		{
			s.preloadedBytes -= pit->second.size();
			s.preloaded.erase(pit);
		}
	}
	return true;
}

static constexpr char kSidecarExtension[] = ".vuaot";

static std::string SidecarPath(const State& s, const XXH128_hash_t& hash,
                                std::string* out_shard_dir = nullptr)
{
	return PayloadPath(s, hash, out_shard_dir, kSidecarExtension);
}

// The sidecar writer's loop: one request at a time, tmp + rename, until stopped with nothing left.
static void SidecarWriterLoop(State& s)
{
	std::unique_lock<std::mutex> lock(s.writerMutex);
	for (;;)
	{
		s.writerCv.wait(lock, [&] { return s.writerStop || !s.writerOrder.empty(); });
		if (s.writerOrder.empty())
			break; // (stopping, and drained)
		const XXH128_hash_t hash = s.writerOrder.front();
		s.writerOrder.pop_front();
		auto it = s.writerQueue.find(hash);
		if (it == s.writerQueue.end())
			continue;
		std::function<std::vector<u8>()> build = std::move(it->second);
		s.writerQueue.erase(it);
		s.writerBusy = true;
		lock.unlock();

		const std::vector<u8> bytes = build();
		if (bytes.empty())
			s.sidecarSkips.fetch_add(1, std::memory_order_relaxed);
		else
		{
			std::string shard_dir;
			const std::string path = SidecarPath(s, hash, &shard_dir);
			const std::string tmp = path + ".tmp";
			Error error;
			if (FileSystem::EnsureDirectoryExists(shard_dir.c_str(), false, &error) &&
				FileSystem::WriteBinaryFile(tmp.c_str(), bytes.data(), bytes.size()) &&
				std::rename(tmp.c_str(), path.c_str()) == 0)
			{
				s.sidecarWrites.fetch_add(1, std::memory_order_relaxed);
				s.sidecarWriteBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
				// The file supersedes any copy the preload holds or is about to put in memory.
				std::lock_guard<std::mutex> preloadLock(s.preloadMutex);
				s.sidecarsWritten.insert(hash);
				auto it = s.preloadedSidecars.find(hash);
				if (it != s.preloadedSidecars.end())
				{
					s.preloadedBytes -= it->second.size();
					s.preloadedSidecars.erase(it);
				}
			}
			else
				Console.Warning("mVUProgCache: sidecar write failed: %s", path.c_str());
		}

		lock.lock();
		s.writerBusy = false;
		if (s.writerOrder.empty())
			s.writerIdleCv.notify_all();
	}
	s.writerBusy = false;
	s.writerIdleCv.notify_all();
}

// A program's last use is the modification time of its payload: a hydration touches it
// (TouchPayload) and a save writes it, so it survives a crash and needs no INDEX rewrite. The INDEX's
// own lastUsedNs only changes when a payload grows, which is a write time, not a use.
static std::filesystem::file_time_type::rep FileTimeNow()
{
	return std::filesystem::file_time_type::clock::now().time_since_epoch().count();
}

// (the paths here are UTF-8, as FileSystem takes them; a std::string handed to std::filesystem::path is the
// ANSI code page on Windows, where a profile directory with an accented name would then not be found at all)
static std::filesystem::path Utf8Path(const std::string& path)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

static void TouchPayload(const std::string& path)
{
	std::error_code ec; // (best effort: a read-only or foreign cache just keeps its old rank)
	std::filesystem::last_write_time(Utf8Path(path), std::filesystem::file_time_type::clock::now(), ec);
}

// Programs not used for this long are deleted, payload and sidecar, by the preload's background
// thread when the cache comes up. The cost of a wrong guess is one cold compile of that program.
static constexpr int kMaxUnusedDays = 60;

// Read the payload-bearing INDEX entries into RAM on a background thread, the most recently used
// program first (payload mtime, see above), until kPreloadBudgetBytes. The bound is a cap, not a
// target: resident cost matters on low-memory devices, but buffers drain as hydrations consume
// them. A program's vu-aot sidecar, when it has one, is read with its payload and counts against
// the same budget. The directory is shared by every game played under the same settings, so it can
// hold more than the budget: what is not preloaded is read when its program appears.
static constexpr size_t kPreloadBudgetBytes = 32 * 1024 * 1024;

static void StartPreload(State& s)
{
	struct Item
	{
		XXH128_hash_t hash;
		u64 lastUsedNs; // the INDEX's (a write time): breaks ties between equal mtimes
		u32 size;
	};
	std::vector<Item> items;
	for (const auto& kv : s.entries)
	{
		if (kv.second.codeSize > 0)
			items.push_back({kv.first, kv.second.lastUsedNs, kv.second.codeSize});
	}
	if (items.empty())
		return;

	struct File
	{
		XXH128_hash_t hash;
		std::string payload, sidecar;
		u32 payloadSize;
		u64 lastUsedNs;
		std::filesystem::file_time_type::rep mtime;
	};
	// (sidecars are read for the vu-aot tier only: with it off they would sit unread)
	const bool sidecars = EmuConfig.Cpu.Recompiler.EnableVUAot;
	std::vector<File> files;
	for (const Item& it : items)
		files.push_back({it.hash, PayloadPath(s, it.hash), SidecarPath(s, it.hash), it.size, it.lastUsedNs, 0});
	const size_t budgetBytes = s.preloadBudget ? s.preloadBudget : kPreloadBudgetBytes;

	s.preloadStop.store(false, std::memory_order_relaxed);
	// The &s capture is intentional and safe: s aliases a process-lifetime
	// static (g_state[]), and StopPreload() joins this thread before Close/
	// ResetForTest ever mutate that state. Don't "fix" this into a by-value or
	// shared_ptr capture — there is no lifetime bug to solve.
	s.preloadThread = std::thread([&s, sidecars, budgetBytes, files = std::move(files)]() mutable {
		// Rank by last use and age out what has not been used for a long time. A payload that is not
		// on disk (evicted by hand, or aged out earlier) has nothing to preload.
		const auto now = FileTimeNow();
		const auto maxAge = std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
			std::chrono::hours(24 * kMaxUnusedDays)).count();
		std::vector<File> ranked;
		ranked.reserve(files.size());
		u64 agedFiles = 0, agedBytes = 0;
		for (File& f : files)
		{
			if (s.preloadStop.load(std::memory_order_relaxed))
				return;
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(Utf8Path(f.payload), ec);
			if (ec)
				continue;
			f.mtime = t.time_since_epoch().count();
			if (now - f.mtime > maxAge)
			{
				for (const std::string* path : {&f.payload, &f.sidecar})
				{
					const s64 size = FileSystem::GetPathFileSize(path->c_str());
					if (FileSystem::DeleteFilePath(path->c_str()))
					{
						agedFiles++;
						agedBytes += static_cast<u64>(std::max<s64>(size, 0));
					}
				}
				continue;
			}
			ranked.push_back(std::move(f));
		}
		// Newest first. Equal times (a file system with coarse stamps; a session loads all its programs
		// within seconds) fall back to the INDEX's time, then the hash, so the order is a function of
		// the files and the INDEX alone.
		std::sort(ranked.begin(), ranked.end(), [](const File& a, const File& b) {
			if (a.mtime != b.mtime)
				return a.mtime > b.mtime;
			if (a.lastUsedNs != b.lastUsedNs)
				return a.lastUsedNs > b.lastUsedNs;
			if (a.hash.high64 != b.hash.high64)
				return a.hash.high64 > b.hash.high64;
			return a.hash.low64 > b.hash.low64;
		});

		// Strict MRU prefix (the first program over budget stops the reads, no
		// skip-and-refill) so what got preloaded is a deterministic function of
		// the files and the INDEX, not of file-size interleaving. Past the
		// budget the walk only sizes the files, for the stats.
		size_t budget = 0, wanted = 0, kept = 0;
		u64 readBytes = 0;
		bool full = false;
		for (const File& f : ranked)
		{
			if (s.preloadStop.load(std::memory_order_relaxed))
				return;
			const s64 sideSize = sidecars ? FileSystem::GetPathFileSize(f.sidecar.c_str()) : 0;
			const size_t need = f.payloadSize + static_cast<size_t>(std::max<s64>(sideSize, 0));
			wanted += need;
			if (full || budget + need > budgetBytes)
			{
				full = true;
				continue;
			}
			budget += need;
			kept++;
			auto bytes = FileSystem::ReadBinaryFile(f.payload.c_str());
			std::optional<std::vector<u8>> side;
			if (sideSize > 0)
				side = FileSystem::ReadBinaryFile(f.sidecar.c_str());
			while (s.preloadHold.load(std::memory_order_acquire) && !s.preloadStop.load(std::memory_order_relaxed))
			{
				s.preloadHeld.store(true, std::memory_order_release);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			s.preloadHeld.store(false, std::memory_order_release);
			std::lock_guard<std::mutex> lock(s.preloadMutex);
			if (bytes.has_value() && !bytes->empty() && !s.payloadsWritten.count(f.hash))
			{
				s.preloadedBytes += bytes->size();
				readBytes += bytes->size();
				++s.preloadedPayloads;
				s.preloaded[f.hash] = std::move(*bytes);
			}
			// (an unreadable payload: the dispatch-time read will count payloadMissing)
			if (side.has_value() && !side->empty() && !s.sidecarsWritten.count(f.hash))
			{
				s.preloadedBytes += side->size();
				s.sidecarPreloadBytes += side->size();
				readBytes += side->size();
				++s.sidecarsPreloaded;
				s.preloadedSidecars[f.hash] = std::move(*side);
			}
		}
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		s.preloadWantedBytes = wanted;
		s.preloadBudgetHit = full ? 1 : 0;
		s.preloadReadBytes += readBytes;
		s.ageOutFiles += agedFiles;
		s.ageOutBytes += agedBytes;
		if (agedFiles)
		{
			Console.WriteLn("mVUProgCache: %llu files (%llu KiB) unused for %d days deleted",
			                static_cast<unsigned long long>(agedFiles),
			                static_cast<unsigned long long>(agedBytes / 1024), kMaxUnusedDays);
		}
		if (full)
		{
			Console.WriteLn("mVUProgCache: preload budget (%zu MiB) reached — "
			                "%zu of %zu programs stay on disk (%zu KiB wanted)",
			                budgetBytes / (1024 * 1024),
			                ranked.size() - kept, ranked.size(), wanted / 1024);
		}
	});
}
} // anonymous namespace

namespace
{
// microVU-free initialization core. Production Init wraps this with
// (mVU.index, mVU.optionsSentinel); tests reach the same code path
// through InitWithSentinel without dragging the microVU struct through
// the test TU (which would re-emit the .inl bodies).
static void InitImpl(u32 vu_index, const XXH128_hash_t& options_sentinel)
{
	State& s = g_state[vu_index & 1];
	if (s.initialized)
		return;

	// Determinism gate for offline tooling (pcsx2-vurunner --no-progcache). The
	// program cache is a process-persistent, on-disk side effect, so two
	// "identical" runs are not byte-identical. SetProcessDisable(true) forces the
	// cache off for the whole process so a JIT-vs-interp divergence run is
	// reproducible. No effect in production (never set there).
	if (mVUPersist::IsProcessDisabled())
	{
		s.enabled = false;
		s.initialized = true;
		return;
	}

	// EmuFolders::Cache is set by the Qt frontend after parsing user paths; the
	// unit-test harness leaves it empty (RecompilerTestEnvironment doesn't run
	// the full VMManager bring-up). Skip the on-disk cache cleanly in that
	// mode — tests run faster and don't leave a ./vu_jit/ in their cwd.
	if (EmuFolders::Cache.empty())
	{
		s.enabled = false;
		s.initialized = true;
		return;
	}

	const std::string root = Path::Combine(EmuFolders::Cache, kRootDirName);
	s.root        = Path::Combine(root, (vu_index & 1) ? "vu1" : "vu0");
	s.indexPath   = Path::Combine(s.root, kIndexFileName);
	s.versionPath = Path::Combine(s.root, kVersionFileName);

	if (!s.enabled)
	{
		s.initialized = true;
		return;
	}

	// Read VERSION BEFORE creating $root so a true fresh-init (no $root,
	// no VERSION) doesn't trip the EvictStaleCache path (which would
	// rename the just-created empty $root aside and inflate
	// staleEvictions). EvictStaleCache itself is a no-op if $root doesn't
	// exist, so the rebuild branch below covers both "directory was
	// missing entirely" and "directory existed with mismatched VERSION."
	VersionHeader live;
	BuildVersionHeader(vu_index, options_sentinel, live);

	bool versionOk = false;
	auto vbytes = FileSystem::ReadBinaryFile(s.versionPath.c_str());
	if (vbytes.has_value() && vbytes->size() == sizeof(VersionHeader))
	{
		VersionHeader disk;
		std::memcpy(&disk, vbytes->data(), sizeof(disk));
		versionOk = VersionMatches(disk, live);
	}

	Error error;
	if (!versionOk)
	{
		// If the directory exists, treat it as stale (rename it aside).
		// If it doesn't, this is a no-op and execution falls through to mkdir +
		// WriteVersion below.
		EvictStaleCache(s);
		if (!FileSystem::EnsureDirectoryExists(s.root.c_str(), true, &error))
		{
			Console.Warning("mVUProgCache[%u]: cannot create %s: %s — disabling",
			                vu_index, s.root.c_str(),
			                error.GetDescription().c_str());
			s.enabled = false;
			s.initialized = true;
			return;
		}
		if (!WriteVersion(s.versionPath, live))
		{
			Console.Warning("mVUProgCache[%u]: write VERSION failed — disabling",
			                vu_index);
			s.enabled = false;
			s.initialized = true;
			return;
		}
	}

	s.entries.clear();
	LoadIndex(s);

	Console.WriteLn(Color_StrongBlue,
		"mVUProgCache[%u]: init root=%s entries=%zu",
		vu_index, s.root.c_str(), s.entries.size());

	StartPreload(s);

	s.initialized = true;
}
} // anonymous namespace

void Init(microVU& mVU)
{
	// Production gate: the whole on-disk cache — init, INDEX
	// telemetry, .vuprog payloads, dispatch-time hydration — sits behind the
	// EmuCore/CPU/Recompiler EnableVUProgramCache INI bool, or EnableVUAot,
	// which is bound to the cache (RecompilerOptions::UseVUProgramCache). Both
	// off means no disk side effects at all. Toggling either on at
	// runtime works because mVUreset re-enters here after every settings-driven
	// recompiler clear; with both bits clear that same reset closes a cache that
	// is up (Close, without saving), so nothing is written after the toggle.
	// Tests bypass this gate through InitWithSentinel / TestReinitFromLiveSentinel.
	if (!EmuConfig.Cpu.Recompiler.UseVUProgramCache())
		return;
	InitImpl(mVU.index & 1u, mVU.optionsSentinel);
}

void InitWithSentinel(u32 vu_index, const XXH128_hash_t& options_sentinel)
{
	InitImpl(vu_index & 1u, options_sentinel);
}

void Close(microVU& mVU)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized)
		return;
	// Sidecars still queued are written first (the tier handed its last ones over just before).
	s.StopWriter();
	DumpStats(mVU, "close");
	// No file handles kept open (per-call open/append/close). Clear the
	// in-memory index so the next mVUreset starts fresh.
	s.StopPreload();
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		s.preloaded.clear();
		s.preloadedSidecars.clear();
		s.payloadsWritten.clear();
		s.sidecarsWritten.clear();
		s.preloadedBytes = 0;
		// Zero the cumulative preload counters too (mirror ResetForTest) so a
		// Close+Init cycle starts from a consistent baseline — otherwise
		// GetStats would report preloadedBytes==0 next to stale nonzero
		// preloadedPayloads/preloadHits. DumpStats("close") above already
		// emitted this lifecycle's lifetime totals.
		s.preloadedPayloads = 0;
		s.preloadHits = 0;
		s.sidecarsPreloaded = 0;
		s.sidecarPreloadBytes = 0;
		s.sidecarPreloadHits = 0;
		s.sidecarReads = 0;
		s.preloadBudgetHit = 0;
		s.preloadWantedBytes = 0;
		s.preloadReadBytes = 0;
		s.ageOutFiles = 0;
		s.ageOutBytes = 0;
	}
	s.sidecarWrites = 0;
	s.sidecarWriteBytes = 0;
	s.sidecarSkips = 0;
	s.initialized = false;
	s.entries.clear();
}

bool SidecarsEnabled(u32 vu_index)
{
	const State& s = g_state[vu_index & 1];
	return s.initialized && s.enabled;
}

bool ReadSidecar(u32 vu_index, const XXH128_hash_t& hash, std::vector<u8>& out)
{
	State& s = g_state[vu_index & 1];
	if (!s.initialized || !s.enabled)
		return false;
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		auto it = s.preloadedSidecars.find(hash);
		if (it != s.preloadedSidecars.end())
		{
			out = std::move(it->second);
			s.preloadedBytes -= out.size();
			s.preloadedSidecars.erase(it);
			++s.sidecarPreloadHits;
			++s.sidecarReads;
			return true;
		}
	}
	// (a sidecar still queued for writing is newer than the disk's: this session wrote it, and a
	// program asks only once per session for its own, so it is not asked for here)
	auto bytes = FileSystem::ReadBinaryFile(SidecarPath(s, hash).c_str());
	if (!bytes.has_value() || bytes->empty())
		return false;
	out = std::move(*bytes);
	std::lock_guard<std::mutex> lock(s.preloadMutex);
	++s.sidecarReads;
	return true;
}

void QueueSidecar(u32 vu_index, const XXH128_hash_t& hash, std::function<std::vector<u8>()> build)
{
	State& s = g_state[vu_index & 1];
	if (!s.initialized || !s.enabled)
		return;
	{
		std::lock_guard<std::mutex> lock(s.writerMutex);
		auto it = s.writerQueue.find(hash);
		if (it != s.writerQueue.end())
			it->second = std::move(build); // (keeps its place in the queue)
		else
		{
			s.writerQueue.emplace(hash, std::move(build));
			s.writerOrder.push_back(hash);
		}
		if (!s.writerThread.joinable())
			s.writerThread = std::thread([&s]() { SidecarWriterLoop(s); });
	}
	s.writerCv.notify_one();
}

void FlushSidecars(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	std::unique_lock<std::mutex> lock(s.writerMutex);
	s.writerIdleCv.wait(lock, [&] { return !s.writerThread.joinable() || (s.writerOrder.empty() && !s.writerBusy); });
}

std::string TestSidecarPath(u32 vu_index, const XXH128_hash_t& hash)
{
	return SidecarPath(g_state[vu_index & 1], hash);
}

ClearResult ClearVuAot(bool wholeCache)
{
	ClearResult result;

	// The threads come first. The writer would put back a file this deletes, and the preload would read
	// one it has deleted. A request still queued is dropped, not written (the one being written when this
	// starts is finished by the join, and deleted below).
	for (State& s : g_state)
	{
		{
			std::lock_guard<std::mutex> lock(s.writerMutex);
			s.writerOrder.clear();
			s.writerQueue.clear();
		}
		s.StopWriter();
		s.StopPreload();
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		for (const auto& kv : s.preloadedSidecars)
			s.preloadedBytes -= kv.second.size();
		s.preloadedSidecars.clear();
		s.sidecarsWritten.clear();
		if (wholeCache)
		{
			s.preloaded.clear();
			s.payloadsWritten.clear();
			s.preloadedBytes = 0;
			s.initialized = false;
			s.entries.clear();
		}
	}

	if (EmuFolders::Cache.empty())
		return result;

	// (every path is UTF-8 through Utf8Path; the error_code overloads keep a missing or unreadable
	// directory from throwing)
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path root = Utf8Path(Path::Combine(EmuFolders::Cache, kRootDirName));
	if (!fs::is_directory(root, ec))
		return result;

	const auto isCacheDir = [](const std::string& name) {
		for (const char* vu : {"vu0", "vu1"})
		{
			const std::string stale = std::string(vu) + ".stale.";
			if (name == vu || name.compare(0, stale.size(), stale) == 0)
				return true;
		}
		return false;
	};

	std::vector<fs::path> dirs;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code sec;
		if (it->is_directory(sec) && isCacheDir(it->path().filename().string()))
			dirs.push_back(it->path());
	}

	const auto endsWith = [](const std::string& name, const char* suffix) {
		const size_t n = std::strlen(suffix);
		return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
	};

	for (const fs::path& dir : dirs)
	{
		std::vector<fs::path> doomed;
		if (wholeCache)
			doomed.push_back(dir);
		else
		{
			std::error_code wec;
			for (fs::recursive_directory_iterator it(dir, wec), end; !wec && it != end; it.increment(wec))
			{
				std::error_code sec;
				if (!it->is_regular_file(sec))
					continue;
				// (".vuaot.tmp" is a write that was cut short)
				const std::string name = it->path().filename().string();
				if (endsWith(name, ".vuaot") || endsWith(name, ".vuaot.tmp"))
					doomed.push_back(it->path());
			}
		}

		for (const fs::path& path : doomed)
		{
			std::error_code sec;
			if (fs::is_directory(path, sec))
			{
				for (fs::recursive_directory_iterator it(path, sec), end; !sec && it != end; it.increment(sec))
				{
					std::error_code fec;
					if (it->is_regular_file(fec))
					{
						++result.files;
						result.bytes += it->file_size(fec);
					}
				}
				fs::remove_all(path, sec);
			}
			else
			{
				++result.files;
				result.bytes += fs::file_size(path, sec);
				fs::remove(path, sec);
			}
		}
	}

	if (result.files)
		Console.WriteLn(Color_StrongBlue, "vu-aot: cleared the %s: %" PRIu64 " files, %.1f MiB",
			wholeCache ? "VU program cache" : "compiled cache (sidecars)", result.files,
			static_cast<double>(result.bytes) / (1024.0 * 1024.0));
	return result;
}

ClearResult OnVuAotSettingChanged(bool wasOn, bool isOn, bool programCacheStillOn)
{
	if (!wasOn || isOn)
		return {};
	return ClearVuAot(!programCacheStillOn);
}

void SaveProgram(microVU& mVU, const microProgram& prog)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized || !s.enabled || !prog.contentHashValid)
		return;

	++s.savesAttempted;

	u32 blockCount = 0;
	for (u32 i = 0; i < (mVU.progSize / 2); ++i)
	{
		microBlockManager* bm = prog.block[i];
		if (!bm)
			continue;
		blockCount += static_cast<u32>(bm->getFullListCount());
	}

	// Serialize the recorded block graph. Empty when mVUPersist recording
	// was off while this program compiled, or the program was marked
	// non-persistable — the INDEX entry is then telemetry-only (no
	// payload).
	std::vector<u8> image;
	const bool have_image = mVUPersist::SerializeProgram(
		mVU, prog, image);

	// Write/refresh the .vuprog payload when the serialized graph grew (a
	// hydrated program compiled new blocks since the last save) or the
	// file is missing. codeSize tracks the last-written payload size —
	// the cheap growth signal (same program + same emitters can't change
	// bytes without changing size).
	auto write_payload_if_grown = [&](IndexEntryHeader& e) -> bool {
		if (!have_image)
			return false;
		if (e.codeSize == static_cast<u32>(image.size()) &&
			FileSystem::FileExists(PayloadPath(s, prog.contentHash).c_str()))
			return false;
		if (!WritePayload(s, prog.contentHash, image))
		{
			Console.Warning("mVUProgCache[%u]: payload write failed", mVU.index);
			return false;
		}
		e.codeSize = static_cast<u32>(image.size());
		++s.payloadWrites;
		return true;
	};

	// A hash seen before already has an INDEX record. Re-append it only when
	// the payload grew (a hydrated program compiled new blocks), so the
	// record carries the new size; last-writer-wins on the next LoadIndex.
	auto it_existing = s.entries.find(prog.contentHash);
	if (it_existing != s.entries.end())
	{
		IndexEntryHeader& existing = it_existing->second;
		if (!write_payload_if_grown(existing))
			return; // payload current (or none to write) — nothing to record

		IndexEntryHeader e = existing;
		e.lastUsedNs = NowNs();
		e.blockCount = blockCount;
		PersistIndexEntry(s, e);
		return;
	}

	// First-time save for this hash.
	IndexEntryHeader e = {};
	e.contentHash = prog.contentHash;
	e.vuIndex     = mVU.index & 1u;
	e.codeSize    = 0; // set by write_payload_if_grown on success
	e.blockCount  = blockCount;
	e.flags       = 1u;
	e.lastUsedNs  = NowNs();
	e.execCount   = 0;

	write_payload_if_grown(e);
	PersistIndexEntry(s, e);
}

bool TestHasIndexEntry(u32 vu_index, const XXH128_hash_t& hash)
{
	State& s = g_state[vu_index & 1];
	return s.initialized && s.entries.find(hash) != s.entries.end();
}

bool TestAppendIndexEntry(u32 vu_index, const XXH128_hash_t& hash)
{
	State& s = g_state[vu_index & 1];
	if (!s.initialized || !s.enabled)
		return false;

	++s.savesAttempted;

	if (s.entries.find(hash) != s.entries.end())
		return false;

	IndexEntryHeader e = {};
	e.contentHash = hash;
	e.vuIndex     = vu_index & 1u;
	e.flags       = 1u;
	e.lastUsedNs  = NowNs();

	return PersistIndexEntry(s, e);
}

void SaveAllPrograms(microVU& mVU)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized || !s.enabled)
		return;
	for (const auto& kv : mVU.mvuContentMap)
	{
		if (kv.second)
			SaveProgram(mVU, *kv.second);
	}
}

microProgram* TryLoadProgram(microVU& mVU, const XXH128_hash_t& contentHash)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized || !s.enabled)
	{
		// Cache disabled/not initialized: the INDEX was never consulted, so
		// this is not an INDEX miss. Leave `misses` alone (it counts genuine
		// hash-not-in-INDEX lookups below) — otherwise the production default
		// (the cache off) inflates the miss count on every dispatch.
		return nullptr;
	}
	auto it = s.entries.find(contentHash);
	if (it == s.entries.end())
	{
		++s.misses;
		return nullptr;
	}
	++s.wouldBeHits;

	// RAM first: consume the preloaded buffer if the background preload got
	// to this payload (moved out + erased, so the resident footprint drains
	// as the game warms up). Disk is the fallback for entries past the
	// preload budget or dispatched before the preload thread reached them.
	std::optional<std::vector<u8>> payload;
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		auto pit = s.preloaded.find(contentHash);
		if (pit != s.preloaded.end())
		{
			payload = std::move(pit->second);
			s.preloadedBytes -= payload->size();
			s.preloaded.erase(pit);
			++s.preloadHits;
		}
	}
	if (!payload.has_value())
		payload = FileSystem::ReadBinaryFile(PayloadPath(s, contentHash).c_str());
	if (!payload.has_value() || payload->empty())
	{
		// Telemetry-only entry (saved with recording off) or the payload
		// was evicted independently of the INDEX. Recompile.
		++s.payloadMissing;
		return nullptr;
	}

	// mVUsearchProg calls this inside a freshly-opened — still empty —
	// emission episode, but HydrateProgram needs the code cache closed so
	// it can run its own open/close pair. Close around the hydration and
	// restore on the way out; closing an empty episode is a recorded
	// no-op, and new blocks compiled after the reopen attach to the
	// hydrated program's rebuilt persist log as growth chunks.
	const bool was_open = (armAsm != nullptr);
	if (was_open)
		mVUcloseCodeCache(mVU);
	microProgram* prog = mVUPersist::HydrateProgram(mVU, payload->data(), payload->size());
	if (was_open)
		mVUopenCodeCache(mVU);

	if (!prog)
	{
		// Another build's payload, a damaged one, or a content mismatch: degrade
		// to recompile, never corrupt. HydrateProgram counted the reason. The
		// file is dead weight, so the next save replaces it even when the
		// recompiled image comes out the same size (SaveProgram skips a payload
		// whose recorded size matches).
		++s.payloadRejects;
		it->second.codeSize = 0;
		return nullptr;
	}
	++s.payloadHits;
	it->second.lastUsedNs = NowNs();
	TouchPayload(PayloadPath(s, contentHash));
	return prog;
}

void ObserveDispatchHash(microVU& mVU, const XXH128_hash_t& contentHash,
                          u32 startPC_bytes)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized || !s.enabled)
		return;
	++s.dispatchProbes;
	(void)startPC_bytes;
	if (s.entries.find(contentHash) != s.entries.end())
		++s.dispatchIndexAvail;
}

Stats GetStats(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	Stats out;
	out.initialized        = s.initialized;
	out.enabled            = s.enabled;
	out.entries            = s.entries.size();
	out.savesAttempted     = s.savesAttempted;
	out.savesWritten       = s.savesWritten;
	out.wouldBeHits        = s.wouldBeHits;
	out.misses             = s.misses;
	out.staleEvictions     = s.staleEvictions;
	out.dispatchProbes     = s.dispatchProbes;
	out.dispatchIndexAvail = s.dispatchIndexAvail;
	out.payloadWrites      = s.payloadWrites;
	out.payloadHits        = s.payloadHits;
	out.payloadMissing     = s.payloadMissing;
	out.payloadRejects     = s.payloadRejects;
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		out.preloadedPayloads = s.preloadedPayloads;
		out.preloadedBytes    = s.preloadedBytes;
		out.preloadHits       = s.preloadHits;
		out.sidecarReads        = s.sidecarReads;
		out.sidecarPreloadHits  = s.sidecarPreloadHits;
		out.sidecarsPreloaded   = s.sidecarsPreloaded;
		out.sidecarPreloadBytes = s.sidecarPreloadBytes;
		out.preloadBudgetHit    = s.preloadBudgetHit;
		out.preloadWantedBytes  = s.preloadWantedBytes;
		out.preloadReadBytes    = s.preloadReadBytes;
		out.ageOutFiles         = s.ageOutFiles;
		out.ageOutBytes         = s.ageOutBytes;
	}
	out.sidecarWrites     = s.sidecarWrites.load(std::memory_order_relaxed);
	out.sidecarWriteBytes = s.sidecarWriteBytes.load(std::memory_order_relaxed);
	out.sidecarSkips      = s.sidecarSkips.load(std::memory_order_relaxed);
	return out;
}

u32 GetCompilerAbiVersion()
{
	return kMvuCompilerAbiVersion;
}

void TestReinitFromLiveSentinel(u32 vu_index)
{
	microVU& mVU = (vu_index & 1) ? microVU1 : microVU0;
	if (!mVU.optionsSentinelValid)
		mVUbuildOptionsSentinel(mVU);
	InitImpl(vu_index & 1, mVU.optionsSentinel);
}

bool TestGetLiveSentinel(u32 vu_index, XXH128_hash_t* out_sentinel)
{
	if (!out_sentinel)
		return false;
	microVU& mVU = (vu_index & 1) ? microVU1 : microVU0;
	if (!mVU.optionsSentinelValid)
		mVUbuildOptionsSentinel(mVU);
	*out_sentinel = mVU.optionsSentinel;
	return true;
}

bool TestBuildSentinelForIndex(u32 vu_index, XXH128_hash_t* out_sentinel)
{
	if (!out_sentinel)
		return false;
	microVU& mVU = (vu_index & 1) ? microVU1 : microVU0;
	mVU.index = vu_index & 1;
	mVU.optionsSentinelValid = false;
	mVUbuildOptionsSentinel(mVU);
	*out_sentinel = mVU.optionsSentinel;
	return true;
}

void ResetForTest(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	s.preloadHold.store(false, std::memory_order_release);
	s.StopWriter();
	s.StopPreload();
	s.initialized      = false;
	s.enabled          = true;
	s.root.clear();
	s.indexPath.clear();
	s.versionPath.clear();
	s.entries.clear();
	s.wouldBeHits        = 0;
	s.misses             = 0;
	s.savesAttempted     = 0;
	s.savesWritten       = 0;
	s.staleEvictions     = 0;
	s.dispatchProbes     = 0;
	s.dispatchIndexAvail = 0;
	s.payloadWrites      = 0;
	s.payloadHits        = 0;
	s.payloadMissing     = 0;
	s.payloadRejects     = 0;
	{
		std::lock_guard<std::mutex> lock(s.preloadMutex);
		s.preloaded.clear();
		s.payloadsWritten.clear();
		s.sidecarsWritten.clear();
		s.preloadedPayloads = 0;
		s.preloadedBytes    = 0;
		s.preloadHits       = 0;
		s.preloadedSidecars.clear();
		s.sidecarsPreloaded   = 0;
		s.sidecarPreloadBytes = 0;
		s.sidecarPreloadHits  = 0;
		s.sidecarReads        = 0;
		s.preloadBudgetHit    = 0;
		s.preloadWantedBytes  = 0;
		s.preloadReadBytes    = 0;
		s.ageOutFiles         = 0;
		s.ageOutBytes         = 0;
	}
	s.preloadBudget = 0;
	s.sidecarWrites = 0;
	s.sidecarWriteBytes = 0;
	s.sidecarSkips = 0;
}

void TestSetPreloadBudget(u32 vu_index, size_t bytes)
{
	g_state[vu_index & 1].preloadBudget = bytes;
}

bool TestIsPreloaded(u32 vu_index, const XXH128_hash_t& hash, bool sidecar)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.preloadMutex);
	return sidecar ? s.preloadedSidecars.count(hash) != 0 : s.preloaded.count(hash) != 0;
}

void TestHoldPreload(u32 vu_index, bool hold)
{
	g_state[vu_index & 1].preloadHold.store(hold, std::memory_order_release);
}

bool TestWritePayload(u32 vu_index, const XXH128_hash_t& hash, const std::vector<u8>& image)
{
	return WritePayload(g_state[vu_index & 1], hash, image);
}

bool TestPreloadIsHeld(u32 vu_index)
{
	return g_state[vu_index & 1].preloadHeld.load(std::memory_order_acquire);
}

void TestWaitForPreload(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	if (s.preloadThread.joinable())
		s.preloadThread.join();
}

void DumpStats(const microVU& mVU, const char* tag)
{
	const Stats s = GetStats(mVU.index & 1);
	const u64 entries = s.entries;
	Console.WriteLn(Color_StrongGreen,
		"mVUProgCache[%u] %s: entries=%llu saves(att/written)=%llu/%llu "
		"loads(indexHit/miss)=%llu/%llu staleEvicts=%llu "
		"dispatchProbes=%llu dispatchIndexAvail=%llu "
		"payload(writes/hits/missing/rejects)=%llu/%llu/%llu/%llu "
		"preload(loaded/residentB/hits)=%llu/%llu/%llu "
		"sidecars(reads/preloadHits/preloaded/preloadB/writes/writeB/skips)=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"preload(wantedB/budgetHit)=%llu/%llu "
		"preload(readB)=%llu ageOut(files/B)=%llu/%llu",
		mVU.index, tag,
		(unsigned long long)entries,
		(unsigned long long)s.savesAttempted,
		(unsigned long long)s.savesWritten,
		(unsigned long long)s.wouldBeHits,
		(unsigned long long)s.misses,
		(unsigned long long)s.staleEvictions,
		(unsigned long long)s.dispatchProbes,
		(unsigned long long)s.dispatchIndexAvail,
		(unsigned long long)s.payloadWrites,
		(unsigned long long)s.payloadHits,
		(unsigned long long)s.payloadMissing,
		(unsigned long long)s.payloadRejects,
		(unsigned long long)s.preloadedPayloads,
		(unsigned long long)s.preloadedBytes,
		(unsigned long long)s.preloadHits,
		(unsigned long long)s.sidecarReads,
		(unsigned long long)s.sidecarPreloadHits,
		(unsigned long long)s.sidecarsPreloaded,
		(unsigned long long)s.sidecarPreloadBytes,
		(unsigned long long)s.sidecarWrites,
		(unsigned long long)s.sidecarWriteBytes,
		(unsigned long long)s.sidecarSkips,
		(unsigned long long)s.preloadWantedBytes,
		(unsigned long long)s.preloadBudgetHit,
		(unsigned long long)s.preloadReadBytes,
		(unsigned long long)s.ageOutFiles,
		(unsigned long long)s.ageOutBytes);
}
}
