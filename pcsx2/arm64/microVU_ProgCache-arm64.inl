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
#include "common/HostSys.h"
#include "common/Path.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
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
//     packs/<title>/vu{0,1}-<sentinel hex>.pack
//     vuaot_tripped.txt                  (vu-aot's tripwire memory, not ours)
//
// <title> is the disc serial and CRC ("SLUS-21065_A1B2C3D4"), "_" for none.
// Older layouts (vu0/, vu1/ with an INDEX and a file per program, and the
// vu{0,1}.stale.* directories they set aside) are deleted on sight.
//
// A pack: PackHeader, then records (RecordHeader + `length` bytes) until the
// end of the file. Records are only ever appended; the writer rewrites the
// file whole when more than half of it is superseded records.
//------------------------------------------------------------------
static constexpr char kRootDirName[]   = "vu_jit";
static constexpr char kPacksDirName[]  = "packs";
static constexpr char kPackExtension[] = ".pack";
static constexpr u32  kPackMagic       = 0x4B415056u; // 'VPAK'
static constexpr u32  kRecordMagic     = 0x43455256u; // 'VREC'

static_assert(sizeof(XXH128_hash_t) == 16, "XXH128_hash_t expected 16 bytes");

#pragma pack(push, 1)
struct PackHeader
{
	u32           magic;              // kPackMagic
	u32           formatVersion;      // kProgCacheFormatVersion
	u32           compilerAbiVersion; // kMvuCompilerAbiVersion
	u32           vuIndex;
	XXH128_hash_t optionsSentinel;
	char          archTag[16];        // "arm64-jit"
	u64           buildTag;           // the build that wrote it (mVUPersist::GetBuildId, folded)
	u32           pageSize;           // the host's page size
	u32           layoutStamp;        // LayoutStamp()
};
static_assert(sizeof(PackHeader) == 64, "PackHeader layout drift");

struct RecordHeader
{
	u32           magic;    // kRecordMagic
	u8            kind;     // RecordKind
	u8            tier;     // a body record that belongs to the VU AOT tier (ClearVuAot removes those)
	u8            reserved[2];
	u32           length;   // bytes after the header
	u32           reserved2;
	XXH128_hash_t hash;     // a program's content hash; a body's root hash (low) and record hash (high)
	u64           checksum; // RecordChecksum
};
static_assert(sizeof(RecordHeader) == 40, "RecordHeader layout drift");
#pragma pack(pop)

enum RecordKind : u8
{
	kRecordProgram = 1, // an mVUPersist image (VU0)
	kRecordBody = 2,    // a body of the VU1 chunk store (microVU_ChunkPack-arm64.inl)
};

// How often the writer looks for something to write.
static constexpr auto kFlushInterval = std::chrono::seconds(30);
// How long Close lets the writer go on (a slow card at quit).
static constexpr auto kCloseDeadline = std::chrono::seconds(2);
// How long lookups may wait, in all, for a pack's read.
static constexpr auto kLoadWaitBudget = std::chrono::seconds(3);
// A pack is rewritten whole when it is at least this big and more than half of it is superseded.
static constexpr u64 kRewriteMinBytes = 1u << 20;
// Packs not opened for this long are deleted. The cost of a wrong guess is one cold session of that game.
static constexpr int kMaxUnusedDays = 60;
// The size of the blocks the table's bytes are kept in (a record larger than this gets a block of its own).
static constexpr size_t kBlockBytes = 4u << 20;
// The RAM cap, which is also the largest a pack file gets. VU0's holds program images: a sixteenth of the
// machine, between 64 and 256 MiB. Games that keep data next to their microcode in VU memory store tens of
// thousands of programs a session (one per whole-memory image), and nearly none of them comes back. VU1's
// holds the chunk store's bodies, one per distinct routine: a few MiB at most.
static constexpr size_t kRamCapMin = 64u << 20;
static constexpr size_t kRamCapMax = 256u << 20;
static constexpr size_t kRamCapBodies = 16u << 20;

using Clock = std::chrono::steady_clock;

// The table's bytes are held in a few large blocks, never in a heap block per record. Android's allocator
// gives every block above a few KiB mappings of its own, and a process may hold about 65,000 mappings
// (vm.max_map_count): a pack of 30,000 records read into a block each took the process down at every boot.
struct Block
{
	std::unique_ptr<u8[]> bytes;
	size_t size = 0;
};

// A record's bytes, inside a Block that it keeps alive.
struct Blob
{
	std::shared_ptr<const Block> block;
	const u8* data = nullptr;
	u32 size = 0;

	explicit operator bool() const { return data != nullptr; }
	bool operator==(const Blob& o) const { return data == o.data && size == o.size; }
};

// Where a pack's bytes go: appended to the last block, or to a new one when they do not fit. Nothing is
// freed until the arena is replaced as a whole (the writer's compaction); superseded bytes count until then.
struct Arena
{
	std::vector<std::shared_ptr<Block>> blocks;
	size_t used = 0; // bytes handed out
	size_t tailFree = 0;

	// Room for `size` bytes, for the caller to fill through `out`.
	Blob Take(size_t size, u8** out)
	{
		if (blocks.empty() || size > tailFree)
		{
			auto b = std::make_shared<Block>();
			b->size = std::max(size, kBlockBytes);
			b->bytes.reset(new u8[b->size]);
			blocks.push_back(b);
			tailFree = b->size;
		}
		const std::shared_ptr<Block>& b = blocks.back();
		u8* p = b->bytes.get() + (b->size - tailFree);
		tailFree -= size;
		used += size;
		*out = p;
		return Blob{b, p, static_cast<u32>(size)};
	}

	Blob Copy(const u8* data, size_t size)
	{
		u8* p;
		Blob blob = Take(size, &p);
		if (size)
			std::memcpy(p, data, size);
		return blob;
	}
};

struct Entry
{
	Blob  program;               // the record's bytes: an mVUPersist image, or a chunk store body (`kind`)
	u8    kind = kRecordProgram; // kRecordProgram or kRecordBody
	bool  tier = false;          // a body that belongs to the VU AOT tier
	u64   shape = 0;             // mVUPersist::ImageShape(program)
	u64   use = 0;               // when it was last stored or served (Pack::useClock): the writer keeps the latest
	bool  programDirty = false;  // not on disk yet
	u32   programRecord = 0;     // the size of its record on disk (for the superseded-bytes count)
};

using Table = std::unordered_map<XXH128_hash_t, Entry, MvuContentHashHash, MvuContentHashEq>;

struct Pack
{
	std::string path;
	std::string title;
	XXH128_hash_t sentinel = {};
	Table table;
	// The chunk store's bodies in the table: a routine's root hash (the low half of a body's key) -> the
	// high half of the key of each of its bodies. An entry may outlive its body (compaction, a drop).
	std::unordered_multimap<u64, u64> byRoot;
	bool loaded = false;      // the read is over (the file's records are in the table, up to the cap)
	bool partial = false;     // the read was stopped: the file holds records the table does not, so it is never rewritten whole
	bool discardFile = false; // the file is of another format: start it again at the next write
	s64 truncateTo = -1;      // the log ends in a broken record at this offset: cut it there before appending
	bool rewriteFailed = false; // a rewrite failed (a full card): only appends from now on, or it would retry every pass
	u64 fileBytes = 0;
	u64 liveBytes = 0;        // ...of which records not superseded
	Arena arena;              // the table's bytes
	// Entry::use. A session's stores and hits start above every record the read numbers (by its place in the
	// file), so they rank as more recent than anything read.
	u64 useClock = u64(1) << 40;
	Clock::duration waitLeft = kLoadWaitBudget;
};

struct State
{
	u32  vu = 0;
	bool initialized = false;
	bool enabled = true;

	// Guards everything below, and every Pack's contents.
	std::mutex mu;
	std::condition_variable cv;       // a read, a build or a flush finished
	std::condition_variable writerCv; // the writer has work, or must stop

	std::shared_ptr<Pack> pack;                  // open
	std::vector<std::shared_ptr<Pack>> retiring; // switched away from, still to be written

	std::thread loader;
	std::atomic<bool> loaderStop{false};
	std::atomic<bool> loadHold{false};
	std::atomic<bool> loadHeld{false};

	std::thread writer;
	bool writerRunning = false;
	bool writerStop = false;
	bool writerKick = false;
	Clock::time_point writeDeadline = Clock::time_point::max();
	u64 flushRequested = 0;
	u64 flushDone = 0;

	size_t ramCap = 0;      // 0: the default (TestSetRamCap)
	u32 closeDeadlineMs = 0;
	std::atomic<u32> writeDelayMs{0};

	Stats st;

	// Joinable-thread backstop: a std::thread destroyed while joinable
	// terminates the process. Close/ResetForTest join explicitly; this
	// covers exit paths that skip mVUclose.
	~State()
	{
		loaderStop.store(true);
		if (loader.joinable())
			loader.join();
		{
			std::lock_guard<std::mutex> lock(mu);
			writerStop = true;
		}
		writerCv.notify_all();
		if (writer.joinable())
			writer.join();
	}
};

static State g_state[2];

static std::mutex s_titleMutex;
static std::string s_title = "_";
static std::atomic<bool> s_leftoversClaimed{false}; // the leftover pass, once per process

static std::string CurrentTitle()
{
	std::lock_guard<std::mutex> lock(s_titleMutex);
	return s_title;
}

static bool SameHash(const XXH128_hash_t& a, const XXH128_hash_t& b)
{
	return a.low64 == b.low64 && a.high64 == b.high64;
}

// (the paths here are UTF-8, as FileSystem takes them; a std::string handed to std::filesystem::path is the
// ANSI code page on Windows, where a profile directory with an accented name would then not be found at all)
static std::filesystem::path Utf8Path(const std::string& path)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

static std::string RootDir()
{
	return Path::Combine(EmuFolders::Cache, kRootDirName);
}

static std::string PackPath(const std::string& title, u32 vu, const XXH128_hash_t& sentinel)
{
	char name[48];
	std::snprintf(name, sizeof(name), "vu%u-%016" PRIx64 "%016" PRIx64 "%s", vu & 1u, sentinel.high64,
		sentinel.low64, kPackExtension);
	return Path::Combine(Path::Combine(Path::Combine(RootDir(), kPacksDirName), title), name);
}

// What the code in a pack assumes about the process besides the build: the sizes of the data arena and of
// the structures it indexes, and how many slots and model stubs there are. The build id fixes all of them
// where there is one; where there is none (the fallback is this file's compile time) this still moves
// when they do.
static u32 LayoutStamp()
{
	const u64 parts[] = {HostMemoryMap::MainSize, HostMemoryMap::CodeSize, sizeof(microVU), sizeof(microBlock), sizeof(VURegs),
		mVUplaceSlotCapacity, static_cast<u64>(mVUModelStubCount)};
	return static_cast<u32>(XXH3_64bits(parts, sizeof(parts)));
}

static PackHeader MakeHeader(u32 vu, const XXH128_hash_t& sentinel)
{
	PackHeader h = {};
	// A record's image fixups are offsets into the build that wrote it.
	u64 buildLo, buildHi;
	mVUPersist::GetBuildId(buildLo, buildHi);
	h.buildTag = buildLo ^ buildHi;
	h.pageSize = static_cast<u32>(__pagesize);
	h.layoutStamp = LayoutStamp();
	h.magic = kPackMagic;
	h.formatVersion = kProgCacheFormatVersion;
	h.compilerAbiVersion = kMvuCompilerAbiVersion;
	h.vuIndex = vu & 1u;
	h.optionsSentinel = sentinel;
	std::strncpy(h.archTag, "arm64-jit", sizeof(h.archTag) - 1);
	return h;
}

// The record's checksum covers its header (with the checksum field zero) and its bytes.
static u64 RecordChecksum(RecordHeader h, const u8* data, size_t size)
{
	h.checksum = 0;
	return XXH3_64bits_withSeed(data, size, XXH3_64bits(&h, sizeof(h)));
}

static void AppendRecord(std::vector<u8>& out, RecordKind kind, bool tier, const XXH128_hash_t& hash, const Blob& data)
{
	RecordHeader h = {};
	h.magic = kRecordMagic;
	h.kind = kind;
	h.tier = tier ? 1 : 0;
	h.length = data.size;
	h.hash = hash;
	h.checksum = RecordChecksum(h, data.data, data.size);
	const u8* p = reinterpret_cast<const u8*>(&h);
	out.insert(out.end(), p, p + sizeof(h));
	out.insert(out.end(), data.data, data.data + data.size);
}

static u32 RecordSize(const Blob& b)
{
	return b ? static_cast<u32>(sizeof(RecordHeader) + b.size) : 0;
}

static size_t RamCap(const State& s)
{
	if (s.ramCap)
		return s.ramCap;
	if (s.vu)
		return kRamCapBodies;
	return std::clamp<size_t>(static_cast<size_t>(GetPhysicalMemory() / 16), kRamCapMin, kRamCapMax);
}

// Deletes, once per process, what no pack will read again: the older layouts' directories, writes cut
// short (a .tmp an hour old), and packs not opened for kMaxUnusedDays (opening one touches it, once its
// read is done; an open pack is never deleted). The tripwire's memory in vu_jit/ is not touched. On the
// first writer thread.
static void DeleteLeftovers(State& s)
{
	namespace fs = std::filesystem;
	if (EmuFolders::Cache.empty())
		return;
	std::vector<std::string> open;
	for (State& other : g_state)
	{
		std::lock_guard<std::mutex> lock(other.mu);
		if (other.pack)
			open.push_back(other.pack->path);
	}
	const auto now = fs::file_time_type::clock::now();
	const fs::path root = Utf8Path(RootDir());
	const auto endsWith = [](const std::string& name, const char* suffix) {
		const size_t n = std::strlen(suffix);
		return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
	};

	std::vector<fs::path> doomed;
	std::error_code ec;
	for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code sec;
		const std::string name = it->path().filename().string();
		if (it->is_directory(sec) && (name == "vu0" || name == "vu1" || name.rfind("vu0.stale.", 0) == 0 ||
										 name.rfind("vu1.stale.", 0) == 0))
			doomed.push_back(it->path());
	}
	std::vector<fs::path> titles;
	for (fs::directory_iterator it(root / kPacksDirName, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code sec;
		if (it->is_directory(sec))
			titles.push_back(it->path());
	}
	for (const fs::path& dir : titles)
	{
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code sec;
			const std::string name = it->path().filename().string();
			const auto age = now - fs::last_write_time(it->path(), sec);
			if (sec)
				continue;
			// (compared as paths, element by element: the separators may differ)
			const bool isOpen = std::any_of(open.begin(), open.end(), [&](const std::string& o) { return Utf8Path(o) == it->path(); });
			if ((endsWith(name, ".tmp") && age > std::chrono::hours(1)) ||
				(endsWith(name, kPackExtension) && age > std::chrono::hours(24 * kMaxUnusedDays) && !isOpen))
				doomed.push_back(it->path());
		}
	}

	u64 files = 0, bytes = 0;
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
					files++;
					bytes += it->file_size(fec);
				}
			}
			fs::remove_all(path, sec);
		}
		else
		{
			files++;
			bytes += fs::file_size(path, sec);
			fs::remove(path, sec);
		}
	}
	for (const fs::path& dir : titles)
	{
		std::error_code sec;
		if (fs::is_empty(dir, sec) && !sec)
			fs::remove(dir, sec);
	}
	if (files)
	{
		Console.WriteLn("mVUProgCache: deleted %llu files (%llu KiB) of older layouts, cut-short writes and packs unused for %d days",
			static_cast<unsigned long long>(files), static_cast<unsigned long long>(bytes / 1024), kMaxUnusedDays);
		std::lock_guard<std::mutex> lock(s.mu);
		s.st.leftoverFiles += files;
		s.st.leftoverBytes += bytes;
	}
}

// One record of a pack being read.
struct LoadedRecord
{
	Blob program;
	u8 kind = kRecordProgram;
	bool tier = false;
	u32 programRecord = 0;
	u64 order = 0; // its place in the file
};

// Where to start reading a pack past the cap: the first record from which the rest of the file fits in it.
// The newest records are at the end. Walks the record headers only.
static u64 FirstRecordToKeep(const std::string& path, u64 fileSize, size_t cap)
{
	// (a small buffer of its own: a header read then costs a few KiB of the card, not the record around it)
	auto file = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	if (!file)
		return sizeof(PackHeader);
	std::FILE* fp = file.get();
	std::setvbuf(fp, nullptr, _IOFBF, 4096);
	std::vector<u64> starts;
	u64 off = sizeof(PackHeader);
	RecordHeader h;
	while (fileSize - off >= sizeof(h) && FileSystem::FSeek64(fp, static_cast<s64>(off), SEEK_SET) == 0 &&
		   std::fread(&h, sizeof(h), 1, fp) == 1 && h.magic == kRecordMagic && h.length <= fileSize - off - sizeof(h))
	{
		starts.push_back(off);
		off += sizeof(h) + h.length;
	}
	// (`off` is where the readable log ends)
	const auto first = std::find_if(starts.begin(), starts.end(), [&](u64 start) { return off - start + sizeof(PackHeader) <= cap; });
	return first != starts.end() ? *first : off;
}

// The read: the file in one sequential pass (through a large stdio buffer, a record at a time, straight into
// the read's own arena), then its newest record of each kind for each hash into the table, where nothing
// this session stored already sits there (that is newer). Of a file past the RAM cap (older builds let packs
// grow without bound) only the newest records that fit in the cap are read; the writer's next pass rewrites
// the file from them. On the loader thread.
static void LoadPack(State& s, std::shared_ptr<Pack> p)
{
	const Clock::time_point t0 = Clock::now();
	size_t cap;
	{
		std::lock_guard<std::mutex> lock(s.mu);
		cap = RamCap(s);
	}
	std::unordered_map<XXH128_hash_t, LoadedRecord, MvuContentHashHash, MvuContentHashEq> recs;
	Arena arena;
	u64 live = 0, records = 0, fileSize = 0, off = 0, start = sizeof(PackHeader);
	bool otherFormat = false, stopped = false, torn = false, capHit = false;
	s64 cutAt = -1;
	if (auto fp = FileSystem::OpenManagedCFile(p->path.c_str(), "rb"))
	{
		const s64 size = FileSystem::FSize64(fp.get());
		fileSize = static_cast<u64>(std::max<s64>(size, 0));
		std::setvbuf(fp.get(), nullptr, _IOFBF, 1u << 20);
		const PackHeader want = MakeHeader(s.vu, p->sentinel);
		PackHeader have;
		if (fileSize && (std::fread(&have, sizeof(have), 1, fp.get()) != 1 || std::memcmp(&have, &want, sizeof(want)) != 0))
			otherFormat = true;
		off = sizeof(PackHeader);
		if (fileSize > cap && !otherFormat)
		{
			start = FirstRecordToKeep(p->path, fileSize, cap);
			capHit = start > sizeof(PackHeader);
			off = start;
			if (FileSystem::FSeek64(fp.get(), static_cast<s64>(off), SEEK_SET) != 0)
				torn = true;
		}
		while (fileSize && !otherFormat && !torn && off < fileSize)
		{
			// (a read cut short leaves the rest alone: its end would look like a broken record, and the file be cut there)
			if (s.loaderStop.load(std::memory_order_relaxed))
			{
				stopped = true;
				break;
			}
			RecordHeader h;
			if (fileSize - off < sizeof(h) || std::fread(&h, sizeof(h), 1, fp.get()) != 1 || h.magic != kRecordMagic ||
				h.kind != (s.vu ? kRecordBody : kRecordProgram) || h.length > fileSize - off - sizeof(h))
			{
				torn = true;
				break;
			}
			u8* bytes;
			Blob data = arena.Take(h.length, &bytes);
			if ((h.length && std::fread(bytes, h.length, 1, fp.get()) != 1) || RecordChecksum(h, bytes, h.length) != h.checksum)
			{
				torn = true;
				break;
			}
			const u32 recordSize = static_cast<u32>(sizeof(h) + h.length);
			LoadedRecord& r = recs[h.hash];
			r.kind = h.kind;
			r.tier = h.tier != 0;
			live -= r.programRecord;
			r.program = std::move(data);
			r.programRecord = recordSize;
			live += recordSize;
			r.order = ++records;
			off += recordSize;
		}
		if (torn)
			cutAt = static_cast<s64>(off);
	}

	while (s.loadHold.load(std::memory_order_acquire) && !s.loaderStop.load(std::memory_order_relaxed))
	{
		s.loadHeld.store(true, std::memory_order_release);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	s.loadHeld.store(false, std::memory_order_release);

	// (mark the use, for DeleteLeftovers in a later session)
	if (fileSize)
	{
		std::error_code ec;
		std::filesystem::last_write_time(Utf8Path(p->path), std::filesystem::file_time_type::clock::now(), ec);
	}

	std::lock_guard<std::mutex> lock(s.mu);
	for (auto& kv : recs)
	{
		Entry& e = p->table[kv.first];
		LoadedRecord& r = kv.second;
		// (what this session stored under the hash already is newer)
		if (r.program && !e.program)
		{
			e.program = std::move(r.program);
			e.kind = r.kind;
			e.tier = r.tier;
			e.shape = r.kind == kRecordProgram ? mVUPersist::ImageShape(e.program.data, e.program.size) : 0;
			e.programRecord = r.programRecord;
			if (r.kind == kRecordBody)
				p->byRoot.emplace(kv.first.low64, kv.first.high64);
			e.use = std::max(e.use, r.order);
		}
		if (!e.program)
			p->table.erase(kv.first);
	}
	// (the read's blocks go ahead of the session's, whose last block stays the one stores append to)
	p->arena.blocks.insert(p->arena.blocks.begin(), arena.blocks.begin(), arena.blocks.end());
	p->arena.used += arena.used;
	p->fileBytes = cutAt >= 0 ? static_cast<u64>(cutAt) : fileSize;
	p->liveBytes = live;
	p->discardFile = otherFormat;
	p->truncateTo = cutAt;
	p->partial = stopped;
	p->loaded = true;
	s.st.packsLoaded += fileSize ? 1 : 0;
	s.st.loadedRecords += records;
	s.st.loadedBytes += fileSize && off > start ? off - start : 0;
	s.st.loadNs += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
	s.st.ramCapHit |= capHit ? 1 : 0;
	s.st.tornTails += torn ? 1 : 0;
	s.st.otherFormat += otherFormat ? 1 : 0;
	s.cv.notify_all();
	if (capHit)
		Console.Warning("mVUProgCache[%u]: %s is larger than the RAM cap (%zu MiB); only its newest records were read",
			s.vu, p->path.c_str(), cap >> 20);
}

// Waits for the open pack's read, from the budget the pack has left for it. False when it gave up.
static bool WaitForLoad(State& s, std::unique_lock<std::mutex>& lock, Pack& p)
{
	if (p.loaded)
		return true;
	const Clock::time_point t0 = Clock::now();
	if (p.waitLeft > Clock::duration::zero())
		s.cv.wait_for(lock, p.waitLeft, [&] { return p.loaded; });
	const Clock::duration waited = Clock::now() - t0;
	p.waitLeft -= waited;
	s.st.loadWaitNs += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
	if (!p.loaded)
		s.st.missesDuringLoad++;
	return p.loaded;
}

// Whether `size` more bytes may go in the table. The writer brings it back under the cap at its next pass;
// until then it may grow to twice the cap, and no further (nor when it cannot rewrite the pack at all).
static bool MayStore(State& s, const Pack& p, size_t size)
{
	if (p.arena.used + size <= 2 * RamCap(s))
		return true;
	s.st.storesRefused++;
	return false;
}

static bool WriteAll(std::FILE* fp, const void* data, size_t size)
{
	return size == 0 || std::fwrite(data, 1, size, fp) == size;
}

// One record to write: what it is, and the bytes as they were when the writer took them.
struct Pending
{
	XXH128_hash_t hash;
	RecordKind kind;
	bool tier;
	Blob bytes;
	bool dirty; // not in the file before this pass (a rewrite also writes what is)
};

// Copies the table into a new arena, which drops the superseded bytes, and with them, past `budget` (in
// bytes of file: records with their headers), the entries stored or served longest ago. The copy is made with the lock released (it can be a hundred MiB,
// and the VU thread looks programs up under the lock); what changed meanwhile is taken as it now is. The
// old blocks go when the last record taken from them is let go. Lock held on entry and exit.
static void Compact(State& s, std::unique_lock<std::mutex>& lock, Pack& p, size_t budget)
{
	struct Item
	{
		XXH128_hash_t hash;
		u64 use;
		Blob program;    // as it was when the copy started
		Blob newProgram; // its copy
		bool keep;
	};
	std::vector<Item> items;
	items.reserve(p.table.size());
	for (const auto& kv : p.table)
		items.push_back({kv.first, kv.second.use, kv.second.program, {}, false});
	lock.unlock();

	std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.use > b.use; });
	Arena fresh;
	size_t kept = 0;
	for (Item& it : items)
	{
		const size_t need = RecordSize(it.program);
		if (kept + need > budget)
			continue;
		kept += need;
		it.keep = true;
		if (it.program)
			it.newProgram = fresh.Copy(it.program.data, it.program.size);
	}

	lock.lock();
	u64 evicted = 0;
	for (const Item& it : items)
	{
		auto found = p.table.find(it.hash);
		if (found == p.table.end())
			continue;
		Entry& e = found->second;
		if (!(e.program == it.program))
			continue; // (stored or cleared during the copy: newer, taken as it is below)
		if (!it.keep)
		{
			p.table.erase(found);
			evicted++;
			continue;
		}
		e.program = it.newProgram;
	}
	// What the copy did not take (changed meanwhile, or stored under a hash it had not seen) moves now.
	const auto inFresh = [&](const Blob& b) {
		return std::find(fresh.blocks.begin(), fresh.blocks.end(), b.block) != fresh.blocks.end();
	};
	for (auto& kv : p.table)
	{
		Entry& e = kv.second;
		if (e.program && !inFresh(e.program))
			e.program = fresh.Copy(e.program.data, e.program.size);
	}
	p.arena = std::move(fresh);
	if (evicted)
	{
		p.byRoot.clear();
		for (const auto& kv : p.table)
			if (kv.second.kind == kRecordBody && kv.second.program)
				p.byRoot.emplace(kv.first.low64, kv.first.high64);
	}
	s.st.evicted += evicted;
	if (evicted)
		Console.WriteLn("mVUProgCache[%u]: %s reached the RAM cap; dropped the %" PRIu64 " entries used longest ago",
			s.vu, p.path.c_str(), evicted);
}

// Puts the pack's changes on disk: appended, or the whole file rewritten when most of it is superseded.
// Lock held on entry and exit; the disk is touched with it released.
static void WritePack(State& s, std::unique_lock<std::mutex>& lock, Pack& p, Clock::time_point deadline)
{
	// The table is only complete once the read is over (a file of another format, or a broken tail, is
	// only known then). The read always ends, and Close stops it before the last pass.
	s.cv.wait(lock, [&] { return p.loaded; });

	// A rewrite (compaction) is all or nothing, and only between closes: the deadline cuts appends only. It
	// comes when most of the file is superseded records, or when the table or the file is past the RAM cap;
	// a table past the cap keeps half of it, so the next one is a while off; a file past it with a table
	// under it (read from an older build's pack) keeps all that fits. A pack whose read was stopped is never
	// rewritten (what the table lacks is the newest end of the file); past the cap it is compacted in RAM
	// only, and appended to.
	const size_t cap = RamCap(s);
	const bool tableOverCap = p.arena.used > cap;
	const bool overCap = tableOverCap || p.fileBytes > cap;
	const bool rewrite = !p.partial && !p.discardFile && !p.rewriteFailed && deadline == Clock::time_point::max() &&
		(overCap || (p.fileBytes >= kRewriteMinBytes && p.fileBytes > 2 * (p.liveBytes + sizeof(PackHeader))));
	if (rewrite)
		Compact(s, lock, p, tableOverCap ? cap / 2 : overCap ? cap - sizeof(PackHeader) : SIZE_MAX);
	else if (tableOverCap && deadline == Clock::time_point::max())
		Compact(s, lock, p, cap / 2);
	std::vector<std::pair<u64, Pending>> order;
	for (auto& kv : p.table)
	{
		Entry& e = kv.second;
		if (e.program && (rewrite || e.programDirty))
			order.push_back({e.use, {kv.first, static_cast<RecordKind>(e.kind), e.tier, e.program, e.programDirty}});
		e.programDirty = false;
	}
	// In the order of their last use, oldest first: a later read numbers records by their place in the file,
	// and one past the cap keeps the end of it.
	std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<Pending> todo;
	todo.reserve(order.size());
	for (auto& o : order)
		todo.push_back(std::move(o.second));
	if (todo.empty() && p.truncateTo < 0 && !p.discardFile)
		return;
	const std::string path = p.path;
	const PackHeader header = MakeHeader(s.vu, p.sentinel);
	const s64 truncateTo = p.truncateTo;
	const bool discard = p.discardFile;
	const u64 fileBytes = discard ? 0 : p.fileBytes;
	lock.unlock();

	namespace fs = std::filesystem;
	Error error;
	bool ok = FileSystem::EnsureDirectoryExists(std::string(Path::GetDirectory(path)).c_str(), true, &error);
	const std::string target = rewrite ? path + ".tmp" : path;
	if (ok && discard)
	{
		// (another format: nothing in it was read, and the table holds what this session made)
		std::error_code ec;
		fs::remove(Utf8Path(path), ec);
	}
	else if (ok && !rewrite && truncateTo >= 0)
	{
		std::error_code ec;
		fs::resize_file(Utf8Path(path), static_cast<uintmax_t>(truncateTo), ec);
		ok = !ec;
	}
	size_t written = 0;
	u64 bytes = 0;
	bool deadlineHit = false;
	if (ok)
	{
		auto fp = FileSystem::OpenManagedCFile(target.c_str(), rewrite ? "wb" : "ab");
		ok = fp != nullptr;
		// (where an append starts is not where the position reads right after opening, on every C library)
		if (ok && !rewrite && FileSystem::FSeek64(fp.get(), 0, SEEK_END) != 0)
			ok = false;
		if (ok && (rewrite || FileSystem::FTell64(fp.get()) == 0))
		{
			ok = WriteAll(fp.get(), &header, sizeof(header));
			bytes += sizeof(header);
		}
		std::vector<u8> buf;
		for (; ok && written < todo.size(); written++)
		{
			if (Clock::now() >= deadline)
			{
				deadlineHit = true;
				break;
			}
			if (const u32 ms = s.writeDelayMs.load(std::memory_order_relaxed))
				std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			buf.clear();
			AppendRecord(buf, todo[written].kind, todo[written].tier, todo[written].hash, todo[written].bytes);
			ok = WriteAll(fp.get(), buf.data(), buf.size());
			bytes += buf.size();
		}
		if (fp && std::fflush(fp.get()) != 0)
			ok = false;
	}
	if (ok && rewrite)
	{
		std::error_code ec;
		fs::rename(Utf8Path(target), Utf8Path(path), ec);
		ok = !ec;
	}
	else if (!ok && rewrite)
	{
		std::error_code ec;
		fs::remove(Utf8Path(target), ec);
	}
	const bool rewriteFailed = !ok && rewrite;

	lock.lock();
	p.rewriteFailed |= rewriteFailed;
	if (!ok)
	{
		if (s.st.writeFailures++ == 0)
			Console.Warning("mVUProgCache[%u]: cannot write %s", s.vu, path.c_str());
		written = rewrite ? 0 : written;
	}
	else
	{
		s.st.flushes++;
		p.truncateTo = -1;
		p.discardFile = false;
		if (rewrite)
		{
			s.st.rewrites++;
			p.fileBytes = bytes;
			p.liveBytes = bytes - sizeof(PackHeader);
		}
		else
			p.fileBytes = (truncateTo >= 0 && !discard ? static_cast<u64>(truncateTo) : fileBytes) + bytes;
	}
	// Account for what reached the disk; whatever did not, and was not replaced meanwhile, is written next time.
	for (size_t i = 0; i < todo.size(); i++)
	{
		const Pending& w = todo[i];
		auto it = p.table.find(w.hash);
		if (it == p.table.end())
			continue;
		Entry& e = it->second;
		const Blob& now = e.program;
		u32& record = e.programRecord;
		if (i < written && ok)
		{
			s.st.recordsWritten++;
			s.st.bytesWritten += RecordSize(w.bytes);
			if (!rewrite)
			{
				p.liveBytes += RecordSize(w.bytes);
				p.liveBytes -= std::min<u64>(record, p.liveBytes);
			}
			record = RecordSize(w.bytes);
		}
		else if (now == w.bytes && w.dirty)
			e.programDirty = true;
	}
	if (deadlineHit)
	{
		s.st.closeDeadlineHit = 1;
		s.st.recordsDropped += todo.size() - written;
	}
}

// The writer: the leftover pass once per process, then a flush every kFlushInterval, on request, and a
// last one (within writeDeadline) when told to stop.
static void WriterLoop(State& s)
{
	Threading::SetNameOfCurrentThread(s.vu ? "VU1 cache writer" : "VU0 cache writer");
	if (!s_leftoversClaimed.exchange(true))
		DeleteLeftovers(s);

	std::unique_lock<std::mutex> lock(s.mu);
	for (;;)
	{
		s.writerCv.wait_for(lock, kFlushInterval, [&] { return s.writerStop || s.writerKick; });
		const bool stopping = s.writerStop;
		s.writerKick = false;
		const u64 request = s.flushRequested;
		const Clock::time_point deadline = stopping ? s.writeDeadline : Clock::time_point::max();

		std::vector<std::shared_ptr<Pack>> packs = s.retiring;
		if (s.pack)
			packs.push_back(s.pack);
		for (const std::shared_ptr<Pack>& p : packs)
			WritePack(s, lock, *p, deadline);
		// (a pack retired while those were written waits for the next pass)
		for (const std::shared_ptr<Pack>& p : packs)
			s.retiring.erase(std::remove(s.retiring.begin(), s.retiring.end(), p), s.retiring.end());

		s.flushDone = std::max(s.flushDone, request);
		if (stopping)
			s.writerRunning = false;
		s.cv.notify_all();
		if (stopping)
			break;
	}
}

static void StartWriter(State& s)
{
	{
		std::lock_guard<std::mutex> lock(s.mu);
		s.writerRunning = true;
		s.writerStop = false;
		s.writerKick = false;
		s.writeDeadline = Clock::time_point::max();
	}
	s.writer = std::thread([&s] { WriterLoop(s); });
}

// Writes what is left, until `deadline`, and ends the writer.
static void StopWriter(State& s, Clock::time_point deadline)
{
	{
		std::lock_guard<std::mutex> lock(s.mu);
		s.writerStop = true;
		s.writeDeadline = deadline;
	}
	s.writerCv.notify_all();
	if (s.writer.joinable())
		s.writer.join();
	std::lock_guard<std::mutex> lock(s.mu);
	s.writerStop = false;
}

// Stops the read of the open pack, if one is running; a pack whose read it cut short is partial.
static void StopLoader(State& s)
{
	s.loaderStop.store(true, std::memory_order_relaxed);
	if (s.loader.joinable())
		s.loader.join();
	s.loaderStop.store(false, std::memory_order_relaxed);
}

// Opens the pack of (title, sentinel) and starts its read. The previous one, if any, must be retired. A
// pack retired and not written yet is taken back as it is: its file does not hold its changes yet. (VU0's
// sentinel follows the EE cycle rate, which can move back and forth within seconds.)
static void OpenPack(State& s, const std::string& title, const XXH128_hash_t& sentinel)
{
	const std::string path = PackPath(title, s.vu, sentinel);
	std::shared_ptr<Pack> p;
	{
		std::lock_guard<std::mutex> lock(s.mu);
		for (auto it = s.retiring.begin(); it != s.retiring.end(); ++it)
		{
			if ((*it)->path == path)
			{
				s.pack = *it;
				s.retiring.erase(it);
				return;
			}
		}
		p = std::make_shared<Pack>();
		p->title = title;
		p->sentinel = sentinel;
		p->path = path;
		s.pack = p;
	}
	s.loader = std::thread([&s, p] {
		Threading::SetNameOfCurrentThread(s.vu ? "VU1 cache read" : "VU0 cache read");
		LoadPack(s, p);
	});
}

static void InitImpl(u32 vu_index, const XXH128_hash_t& options_sentinel)
{
	State& s = g_state[vu_index & 1];
	if (s.initialized)
		return;
	s.vu = vu_index & 1;

	// Determinism gate for offline tooling (pcsx2-vurunner --no-progcache). The
	// program cache is a process-persistent, on-disk side effect, so two
	// "identical" runs are not byte-identical. SetProcessDisable(true) forces the
	// cache off for the whole process so a JIT-vs-interp divergence run is
	// reproducible. No effect in production (never set there).
	//
	// EmuFolders::Cache is set by the Qt frontend after parsing user paths; the
	// unit-test harness leaves it empty (RecompilerTestEnvironment doesn't run
	// the full VMManager bring-up). Skip the cache cleanly in that mode.
	if (mVUPersist::IsProcessDisabled() || EmuFolders::Cache.empty() || !s.enabled)
	{
		s.enabled = false;
		s.initialized = true;
		return;
	}

	const std::string title = CurrentTitle();
	OpenPack(s, title, options_sentinel);
	StartWriter(s);
	s.initialized = true;
	Console.WriteLn(Color_StrongBlue, "mVUProgCache[%u]: %s", s.vu, s.pack->path.c_str());
}

// Ends the session's cache: the read stopped, the writer's last pass bounded by `deadline`, the table dropped.
static void CloseImpl(State& s, Clock::time_point deadline)
{
	StopLoader(s);
	{
		std::lock_guard<std::mutex> lock(s.mu);
		if (s.pack && !s.pack->loaded)
		{
			s.pack->loaded = true;
			s.pack->partial = true;
		}
	}
	StopWriter(s, deadline);
	std::lock_guard<std::mutex> lock(s.mu);
	if (s.pack)
		s.st.fileBytes = s.pack->fileBytes;
	s.pack.reset();
	s.retiring.clear();
	s.initialized = false;
}
} // anonymous namespace

void SetTitle(const std::string& serial, u32 crc)
{
	std::string name;
	if (!serial.empty() || crc)
	{
		name = serial;
		for (char& c : name)
		{
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-'))
				c = '_';
		}
		char crcHex[16];
		std::snprintf(crcHex, sizeof(crcHex), "_%08X", crc);
		name += crcHex;
	}
	else
		name = "_";
	std::lock_guard<std::mutex> lock(s_titleMutex);
	s_title = std::move(name);
}

void Init(microVU& mVU)
{
	// Production gate: the whole cache sits behind the EmuCore/CPU/Recompiler
	// EnableVUProgramCache INI bool, or EnableVUAot, which is bound to the cache
	// (RecompilerOptions::UseVUProgramCache). Both off means no disk side
	// effects at all. Toggling either on at runtime works because mVUreset
	// re-enters here after every settings-driven recompiler clear; with both
	// bits clear that same reset closes a cache that is up (Close, without
	// saving), so nothing is stored after the toggle.
	// Tests bypass this gate through InitWithSentinel / TestReinitFromLiveSentinel.
	if (!EmuConfig.Cpu.Recompiler.UseVUProgramCache())
		return;
	InitImpl(mVU.index & 1u, mVU.optionsSentinel);
}

void InitWithSentinel(u32 vu_index, const XXH128_hash_t& options_sentinel)
{
	InitImpl(vu_index & 1u, options_sentinel);
}

void Rekey(microVU& mVU)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized || !s.enabled)
		return;
	const std::string title = CurrentTitle();
	{
		std::lock_guard<std::mutex> lock(s.mu);
		if (!s.pack || (s.pack->title == title && SameHash(s.pack->sentinel, mVU.optionsSentinel)))
			return;
	}
	StopLoader(s);
	{
		std::lock_guard<std::mutex> lock(s.mu);
		if (!s.pack->loaded)
		{
			s.pack->loaded = true;
			s.pack->partial = true;
		}
		s.retiring.push_back(std::move(s.pack));
		s.writerKick = true;
	}
	s.writerCv.notify_all();
	OpenPack(s, title, mVU.optionsSentinel);
}

void Close(microVU& mVU)
{
	State& s = g_state[mVU.index & 1];
	if (!s.initialized)
		return;
	const auto limit = s.closeDeadlineMs ? std::chrono::milliseconds(s.closeDeadlineMs) :
	                                       std::chrono::duration_cast<std::chrono::milliseconds>(kCloseDeadline);
	CloseImpl(s, Clock::now() + limit);
	// (the counters run on for the process: ResetForTest is what zeroes them)
	DumpStats(mVU, "close");
}

// Asks the writer for a pass now and waits for it.
static void FlushNow(State& s)
{
	std::unique_lock<std::mutex> lock(s.mu);
	if (!s.writerRunning)
		return;
	const u64 request = ++s.flushRequested;
	s.writerKick = true;
	s.writerCv.notify_all();
	s.cv.wait(lock, [&] { return s.flushDone >= request || !s.writerRunning; });
}

void TestFlush(u32 vu_index)
{
	FlushNow(g_state[vu_index & 1]);
}

void TestStoreRecord(u32 vu_index, const XXH128_hash_t& hash, const u8* data, size_t size)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	if (!s.initialized || !s.enabled || !s.pack || !MayStore(s, *s.pack, size))
		return;
	Pack& p = *s.pack;
	const bool fresh = !p.table.count(hash);
	Entry& e = p.table[hash];
	e.program = p.arena.Copy(data, size);
	e.kind = s.vu ? kRecordBody : kRecordProgram;
	e.shape = 0;
	e.use = ++p.useClock;
	e.programDirty = true;
	if (fresh && s.vu)
		p.byRoot.emplace(hash.low64, hash.high64);
}

bool TestReadRecord(u32 vu_index, const XXH128_hash_t& hash, std::vector<u8>& out)
{
	State& s = g_state[vu_index & 1];
	std::unique_lock<std::mutex> lock(s.mu);
	if (!s.initialized || !s.enabled || !s.pack)
		return false;
	const std::shared_ptr<Pack> p = s.pack;
	WaitForLoad(s, lock, *p);
	const auto it = p->table.find(hash);
	if (it == p->table.end() || !it->second.program)
		return false;
	out.assign(it->second.program.data, it->second.program.data + it->second.program.size);
	it->second.use = ++p->useClock;
	return true;
}

ClearResult ClearVuAot(bool wholeCache)
{
	ClearResult result;

	// The tables first, then the threads: the writer would put back what this deletes. With the program
	// cache kept, the other records' changes still go to disk before the packs are filtered below.
	for (State& s : g_state)
	{
		if (!s.initialized || !s.enabled)
			continue;
		StopLoader(s);
		{
			std::lock_guard<std::mutex> lock(s.mu);
			std::vector<std::shared_ptr<Pack>> packs = s.retiring;
			if (s.pack)
				packs.push_back(s.pack);
			for (const std::shared_ptr<Pack>& p : packs)
			{
				if (!p->loaded)
				{
					p->loaded = true;
					p->partial = true;
				}
				for (auto it = p->table.begin(); it != p->table.end();)
				{
					Entry& e = it->second;
					it = (wholeCache || e.tier) ? p->table.erase(it) : std::next(it);
				}
			}
		}
		if (wholeCache)
			CloseImpl(s, Clock::time_point::max());
		else
		{
			StopWriter(s, Clock::time_point::max());
			std::lock_guard<std::mutex> lock(s.mu);
			s.retiring.clear();
		}
	}

	if (!EmuFolders::Cache.empty())
	{
		// (every path is UTF-8 through Utf8Path; the error_code overloads keep a missing or unreadable
		// directory from throwing)
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path root = Utf8Path(RootDir());
		if (wholeCache)
		{
			std::vector<fs::path> doomed;
			for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
			{
				std::error_code sec;
				const std::string name = it->path().filename().string();
				if (it->is_directory(sec) && (name == kPacksDirName || name == "vu0" || name == "vu1" ||
												 name.rfind("vu0.stale.", 0) == 0 || name.rfind("vu1.stale.", 0) == 0))
					doomed.push_back(it->path());
			}
			for (const fs::path& dir : doomed)
			{
				std::error_code sec;
				for (fs::recursive_directory_iterator it(dir, sec), end; !sec && it != end; it.increment(sec))
				{
					std::error_code fec;
					if (it->is_regular_file(fec))
					{
						result.files++;
						result.bytes += it->file_size(fec);
					}
				}
				fs::remove_all(dir, sec);
			}
		}
		else
		{
			// Every pack on disk, the open ones included, loses the tier's records and keeps the rest.
			std::vector<fs::path> packs;
			for (fs::recursive_directory_iterator it(root / kPacksDirName, ec), end; !ec && it != end; it.increment(ec))
			{
				std::error_code sec;
				if (it->is_regular_file(sec) && it->path().extension() == kPackExtension)
					packs.push_back(it->path());
			}
			for (const fs::path& path : packs)
			{
				const std::string p8 = std::string(reinterpret_cast<const char*>(path.u8string().c_str()));
				std::optional<std::vector<u8>> file = FileSystem::ReadBinaryFile(p8.c_str());
				if (!file.has_value() || file->size() < sizeof(PackHeader))
					continue;
				std::vector<u8> kept(file->begin(), file->begin() + sizeof(PackHeader));
				u64 dropped = 0, droppedBytes = 0;
				size_t off = sizeof(PackHeader);
				while (file->size() - off >= sizeof(RecordHeader))
				{
					RecordHeader h;
					std::memcpy(&h, file->data() + off, sizeof(h));
					if (h.magic != kRecordMagic || h.length > file->size() - off - sizeof(h))
						break;
					const size_t size = sizeof(h) + h.length;
					if (h.tier)
					{
						dropped++;
						droppedBytes += size;
					}
					else
						kept.insert(kept.end(), file->begin() + off, file->begin() + off + size);
					off += size;
				}
				if (!dropped)
					continue;
				const std::string tmp = p8 + ".tmp";
				std::error_code sec;
				if (FileSystem::WriteBinaryFile(tmp.c_str(), kept.data(), kept.size()))
					fs::rename(Utf8Path(tmp), path, sec);
				if (sec)
					continue;
				result.files += dropped;
				result.bytes += droppedBytes;
			}
		}
	}

	// With the program cache kept, the open caches go on: their files changed under them, so their size
	// is read again, and the writer starts again.
	if (!wholeCache)
	{
		for (State& s : g_state)
		{
			if (!s.initialized || !s.enabled)
				continue;
			{
				std::lock_guard<std::mutex> lock(s.mu);
				if (s.pack)
				{
					const s64 size = FileSystem::GetPathFileSize(s.pack->path.c_str());
					s.pack->fileBytes = size > 0 ? static_cast<u64>(size) : 0;
					s.pack->liveBytes = s.pack->fileBytes;
					s.pack->truncateTo = -1;
				}
			}
			StartWriter(s);
		}
	}

	if (result.files)
		Console.WriteLn(Color_StrongBlue, "vu-aot: cleared the %s: %" PRIu64 " %s, %.1f MiB",
			wholeCache ? "VU program cache" : "compiled cache (tiered bodies)", result.files,
			wholeCache ? "files" : "records", static_cast<double>(result.bytes) / (1024.0 * 1024.0));
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
	std::shared_ptr<Pack> p;
	const u64 shape = mVUPersist::LogShape(prog);
	{
		std::lock_guard<std::mutex> lock(s.mu);
		s.st.savesAttempted++;
		p = s.pack;
		if (!p || !shape)
			return; // (no persist log: recording off, or the program is not persistable)
		auto it = p->table.find(prog.contentHash);
		if (it != p->table.end() && it->second.program && it->second.shape == shape)
			return; // (the stored image already holds every block)
	}

	std::vector<u8> image;
	if (!mVUPersist::SerializeProgram(mVU, prog, image))
		return;

	std::lock_guard<std::mutex> lock(s.mu);
	if (!MayStore(s, *p, image.size()))
		return;
	Entry& e = p->table[prog.contentHash];
	e.program = p->arena.Copy(image.data(), image.size());
	e.shape = shape;
	e.use = ++p->useClock;
	e.programDirty = true;
	s.st.payloadWrites++;
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
		// Cache disabled/not initialized: not a miss. Leave `misses` alone (it counts genuine
		// hash-not-in-table lookups below) — otherwise the cache off inflates the miss count on
		// every dispatch.
		return nullptr;
	}
	std::shared_ptr<Pack> p;
	Blob image;
	{
		std::unique_lock<std::mutex> lock(s.mu);
		p = s.pack;
		if (!p)
			return nullptr;
		WaitForLoad(s, lock, *p);
		auto it = p->table.find(contentHash);
		if (it == p->table.end() || it->second.kind != kRecordProgram)
		{
			s.st.misses++;
			return nullptr;
		}
		s.st.wouldBeHits++;
		image = it->second.program;
		if (!image)
		{
			s.st.payloadMissing++;
			return nullptr;
		}
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
	microProgram* prog = mVUPersist::HydrateProgram(mVU, image.data, image.size);
	if (was_open)
		mVUopenCodeCache(mVU);

	std::lock_guard<std::mutex> lock(s.mu);
	if (!prog)
	{
		// Another build's image, a damaged one, or a content mismatch: degrade
		// to recompile, never corrupt. HydrateProgram counted the reason. The
		// image is dropped, so the next save stores the recompiled one.
		s.st.payloadRejects++;
		auto it = p->table.find(contentHash);
		// (by content: a compaction during the hydration moves the bytes, not what they are)
		const Blob& now = it != p->table.end() ? it->second.program : Blob{};
		if (now && now.size == image.size && std::memcmp(now.data, image.data, image.size) == 0)
		{
			it->second.program = {};
			it->second.shape = 0;
		}
		return nullptr;
	}
	s.st.payloadHits++;
	if (auto it = p->table.find(contentHash); it != p->table.end())
		it->second.use = ++p->useClock;
	return prog;
}

bool BodiesEnabled(u32 vu_index)
{
	const State& s = g_state[vu_index & 1];
	return s.initialized && s.enabled;
}

// A body's key: the routine's root hash, and a hash of the record.
static XXH128_hash_t BodyKey(u64 rootHash, const u8* data, size_t size)
{
	XXH128_hash_t key;
	key.low64 = rootHash;
	key.high64 = XXH3_64bits(data, size);
	return key;
}

bool StoreBody(u32 vu_index, u64 rootHash, const u8* data, size_t size, bool tier)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	if (!s.initialized || !s.enabled || !s.pack)
		return false;
	Pack& p = *s.pack;
	const XXH128_hash_t key = BodyKey(rootHash, data, size);
	if (auto it = p.table.find(key); it != p.table.end() && it->second.program)
	{
		it->second.use = ++p.useClock;
		return true;
	}
	if (!MayStore(s, p, size))
		return false;
	Entry& e = p.table[key];
	e.program = p.arena.Copy(data, size);
	e.kind = kRecordBody;
	e.tier = tier;
	e.use = ++p.useClock;
	e.programDirty = true;
	p.byRoot.emplace(key.low64, key.high64);
	s.st.bodiesStored++;
	return true;
}

void FetchBodies(u32 vu_index, u64 rootHash, std::vector<std::vector<u8>>& out)
{
	State& s = g_state[vu_index & 1];
	std::unique_lock<std::mutex> lock(s.mu);
	if (!s.initialized || !s.enabled || !s.pack)
		return;
	const std::shared_ptr<Pack> p = s.pack;
	WaitForLoad(s, lock, *p);
	std::vector<std::pair<u64, Entry*>> found;
	const auto range = p->byRoot.equal_range(rootHash);
	for (auto it = range.first; it != range.second; ++it)
	{
		XXH128_hash_t key;
		key.low64 = rootHash;
		key.high64 = it->second;
		const auto e = p->table.find(key);
		if (e != p->table.end() && e->second.kind == kRecordBody && e->second.program)
			found.push_back({e->second.use, &e->second});
	}
	// Oldest first: a routine's later bodies are filed after its earlier ones.
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (auto& f : found)
	{
		const Blob& b = f.second->program;
		out.emplace_back(b.data, b.data + b.size);
		s.st.bodiesServed++;
	}
	// (after the sort, so the order among them is kept for the next session)
	for (auto& f : found)
		f.second->use = ++p->useClock;
}

void DropBody(u32 vu_index, u64 rootHash, const u8* data, size_t size)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	if (!s.initialized || !s.enabled || !s.pack)
		return;
	Pack& p = *s.pack;
	const auto it = p.table.find(BodyKey(rootHash, data, size));
	if (it == p.table.end() || it->second.kind != kRecordBody)
		return;
	// (its record in the file is superseded: a rewrite leaves it out)
	p.liveBytes -= std::min<u64>(it->second.programRecord, p.liveBytes);
	p.table.erase(it);
	s.st.bodiesDropped++;
}

Stats GetStats(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	Stats out = s.st;
	out.initialized = s.initialized;
	out.enabled = s.initialized && s.enabled;
	out.entries = s.pack ? s.pack->table.size() : 0;
	out.ramBytes = s.pack ? s.pack->arena.used : 0;
	out.ramBlocks = s.pack ? s.pack->arena.blocks.size() : 0;
	out.fileBytes = s.pack ? s.pack->fileBytes : s.st.fileBytes;
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
	s.loadHold.store(false, std::memory_order_release);
	s.writeDelayMs.store(0, std::memory_order_relaxed);
	if (s.initialized)
		CloseImpl(s, Clock::time_point::max());
	std::lock_guard<std::mutex> lock(s.mu);
	s.initialized = false;
	s.enabled = true;
	s.pack.reset();
	s.retiring.clear();
	s.st = Stats{};
	s.ramCap = 0;
	s.closeDeadlineMs = 0;
	s.flushRequested = s.flushDone = 0;
	s_leftoversClaimed = false; // (the leftover pass runs again at the next Init, as in a new process)
}

void TestClose(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	if (!s.initialized)
		return;
	const auto limit = s.closeDeadlineMs ? std::chrono::milliseconds(s.closeDeadlineMs) :
	                                       std::chrono::duration_cast<std::chrono::milliseconds>(kCloseDeadline);
	CloseImpl(s, Clock::now() + limit);
}

void TestWaitForPreload(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	std::unique_lock<std::mutex> lock(s.mu);
	if (s.pack)
	{
		const std::shared_ptr<Pack> p = s.pack;
		s.cv.wait(lock, [&] { return p->loaded; });
	}
	lock.unlock();
	// (the leftover pass runs first on the writer thread, so a flush that has come back has seen it through)
	TestFlush(vu_index);
}

std::string TestPackPath(u32 vu_index)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	return s.pack ? s.pack->path : std::string();
}

bool TestHasEntry(u32 vu_index, const XXH128_hash_t& hash)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	if (!s.pack)
		return false;
	auto it = s.pack->table.find(hash);
	return it != s.pack->table.end() && static_cast<bool>(it->second.program);
}

void TestSetRamCap(u32 vu_index, size_t bytes)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	s.ramCap = bytes;
}

void TestHoldLoad(u32 vu_index, bool hold)
{
	g_state[vu_index & 1].loadHold.store(hold, std::memory_order_release);
}

bool TestLoadIsHeld(u32 vu_index)
{
	return g_state[vu_index & 1].loadHeld.load(std::memory_order_acquire);
}

void TestSetCloseDeadlineMs(u32 vu_index, u32 ms)
{
	State& s = g_state[vu_index & 1];
	std::lock_guard<std::mutex> lock(s.mu);
	s.closeDeadlineMs = ms;
}

void TestSetWriteDelayMs(u32 vu_index, u32 ms)
{
	g_state[vu_index & 1].writeDelayMs.store(ms, std::memory_order_relaxed);
}

void DumpStats(const microVU& mVU, const char* tag)
{
	const Stats s = GetStats(mVU.index & 1);
	Console.WriteLn(Color_StrongGreen,
		"mVUProgCache[%u] %s: entries=%llu ram=%.1fMiB "
		"read(packs/records/ms/waitMs/missesDuringLoad/capHit/torn/otherFormat)=%llu/%llu/%.1f/%.1f/%llu/%llu/%llu/%llu "
		"programs(saves/stored/hits/known/misses/missing/rejects)=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"bodies(stored/served/dropped)=%llu/%llu/%llu fileKiB=%llu "
		"written(flushes/records/KiB/rewrites/failures)=%llu/%llu/%llu/%llu/%llu "
		"close(deadlineHit/dropped)=%llu/%llu leftovers(files/KiB)=%llu/%llu cap(evicted/refused)=%llu/%llu",
		mVU.index, tag, (unsigned long long)s.entries, static_cast<double>(s.ramBytes) / (1024.0 * 1024.0),
		(unsigned long long)s.packsLoaded, (unsigned long long)s.loadedRecords, static_cast<double>(s.loadNs) / 1e6,
		static_cast<double>(s.loadWaitNs) / 1e6, (unsigned long long)s.missesDuringLoad, (unsigned long long)s.ramCapHit,
		(unsigned long long)s.tornTails, (unsigned long long)s.otherFormat,
		(unsigned long long)s.savesAttempted, (unsigned long long)s.payloadWrites, (unsigned long long)s.payloadHits,
		(unsigned long long)s.wouldBeHits, (unsigned long long)s.misses, (unsigned long long)s.payloadMissing,
		(unsigned long long)s.payloadRejects,
		(unsigned long long)s.bodiesStored, (unsigned long long)s.bodiesServed, (unsigned long long)s.bodiesDropped,
		(unsigned long long)(s.fileBytes / 1024),
		(unsigned long long)s.flushes, (unsigned long long)s.recordsWritten, (unsigned long long)(s.bytesWritten / 1024),
		(unsigned long long)s.rewrites, (unsigned long long)s.writeFailures,
		(unsigned long long)s.closeDeadlineHit, (unsigned long long)s.recordsDropped,
		(unsigned long long)s.leftoverFiles, (unsigned long long)(s.leftoverBytes / 1024),
		(unsigned long long)s.evicted, (unsigned long long)s.storesRefused);
}
}
