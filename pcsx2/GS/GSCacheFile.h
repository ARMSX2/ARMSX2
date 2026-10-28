// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// On-disk storage shared by every GS shader and pipeline cache.
//
// The rules every cache follows:
//  - Each file starts with a header carrying a stamp: a digest of everything the contents depend on
//    (build, driver, compiler, options). A stamp that does not match discards the file whole.
//  - Every entry or payload carries its own checksum. Anything that fails it is dropped, never
//    handed to a compiler or driver: a driver fed a damaged binary or SPIR-V may crash rather than
//    report an error.
//  - Whole files are replaced by writing a temporary file and renaming it over the old one. Files
//    that grow by appending are read up to the first bad entry and cut there, so an app killed
//    mid-write loses only what it was writing.
//  - The emulation and GS threads never wait on the disk during play: a pack is read whole when it
//    opens, and a background thread (PackWriter) writes what changed.

#include "common/Pcsx2Defs.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace GSCacheFile
{
	using Clock = std::chrono::steady_clock;

	struct Digest
	{
		u8 bytes[16];

		bool operator==(const Digest& rhs) const;
		bool operator!=(const Digest& rhs) const { return !(*this == rhs); }
		std::string ToHex() const;
	};

	Digest Hash128(const void* data, size_t size);
	u64 Hash64(const void* data, size_t size);

	/// Identity of the GS code and shader sources this binary was built from: a hash of every file
	/// under pcsx2/GS and bin/resources/shaders, computed by the build, so no change to them can
	/// leave it unchanged. Builds without the generated header (the Visual Studio project) use the
	/// git hash.
	const std::string& GetBuildId();

	/// The build identity folded to the single version word the Direct3D caches carry, so they too are
	/// discarded by any GS change instead of only by a SHADER_CACHE_VERSION bump.
	u32 GetBuildVersionWord();

	/// Everything a cache's contents depend on, as named text. The digest goes into the file; the
	/// text is only for the log line that says why a cache was discarded.
	class Stamp
	{
	public:
		Stamp& Add(std::string_view name, std::string_view value);
		Stamp& Add(std::string_view name, u64 value);
		Stamp& AddHex(std::string_view name, const void* data, size_t size);

		Digest GetDigest() const;
		const std::string& GetText() const { return m_text; }

	private:
		std::string m_text;
	};

	/// File kinds, so one cache's file can never be read as another's.
	enum Kind : u32
	{
		KIND_VK_SPIRV = 0x56505331, // "1SPV"
		KIND_VK_PIPELINES = 0x4C505631, // "1VPL"
		KIND_VK_PIPELINE_KEYS = 0x59454B31, // "1KEY"
		KIND_GL_PROGRAMS = 0x474C5031, // "1PLG"
		KIND_LSFG_SPIRV = 0x47465331, // "1SFG"
		KIND_TEST = 0x54534554, // "TEST"
	};

	enum class ReadResult
	{
		Ok,
		Missing,
		BadHeader,
		StampMismatch,
		Truncated,
		ChecksumMismatch,
	};
	const char* ReadResultString(ReadResult result);

	/// Writes to a temporary file beside `path`, named for this process, and renames it over `path`.
	/// No fsync: a power cut can at worst leave a short or empty file, which the reader's checksum
	/// rejects, and an fsync can stall for hundreds of milliseconds on phone flash. A write still
	/// going at `deadline` is abandoned, leaving `path` as it was.
	bool WriteFileAtomic(const std::string& path, const void* data, size_t size,
		Clock::time_point deadline = Clock::time_point::max());

	/// Deletes temporary files a killed write left in `dir`: those whose process is gone, or a day old.
	u32 CleanStaleTempFiles(const std::string& dir);

	/// The first eight hex digits of a stamp's digest, for file names. A build- or driver-stamped
	/// file carries it, so two builds sharing a data root (a stable and a nightly install) each keep
	/// their own file instead of discarding each other's at every start.
	std::string ShortName(const Digest& digest);

	/// Deletes the files in `dir` whose names start with `prefix`, grouped by stem (the name up to the
	/// first '.'), except `current_stem` and the `keep - 1` other stems used most recently.
	u32 PruneOtherIdentities(const std::string& dir, std::string_view prefix, std::string_view current_stem, u32 keep);

	/// A whole file: header, then one payload covered by one checksum.
	bool WriteFramedFile(const std::string& path, Kind kind, const Stamp& stamp, const void* data, size_t size,
		Clock::time_point deadline = Clock::time_point::max());
	ReadResult ReadFramedFile(const std::string& path, Kind kind, const Stamp& stamp, std::vector<u8>* payload);

	/// Where a game's packs live: `<cache_dir>/gs_packs/<serial>_<CRC>`, or `.../_` for what a device
	/// builds before any game is known (an empty serial).
	std::string GetPackDirectory(const std::string& cache_dir, const std::string& serial, u32 crc);

	/// How much of one pack is kept in RAM: a thirty-second of physical memory, at least 32 MiB.
	u64 GetDefaultPackRamCap();

	/// A cache file held in RAM while it is open: entries under a type and a key, each with a payload
	/// and one u32 for the owner.
	///
	/// On disk it is a header and then an append-only log of checksummed records, the newest record
	/// for a key winning; an erase is a record too. Open reads the whole file in one pass, and from
	/// then on lookups never touch the disk. Changes stay in RAM until Write, which appends them, or
	/// writes the whole pack again (a temporary file renamed over it) when it is more than half
	/// garbage. A record that fails its checksum ends the log, and the next Write cuts it off.
	///
	/// Internally locked: lookups and changes may come from any thread, and Write from another.
	class Pack
	{
	public:
		struct LoadInfo
		{
			/// Ok when an existing pack was read; Missing when there was none. Any other answer means
			/// the file was not used, and the first Write replaces it.
			ReadResult result = ReadResult::Missing;
			u32 records = 0;
			u64 bytes = 0;
			u64 ns = 0;
			/// Bytes after the last good record.
			u64 torn_bytes = 0;
			/// The RAM cap was reached: later records were left on disk, and the pack is only
			/// appended to from now on, never written whole.
			bool ram_cap_hit = false;
		};

		struct WriteResult
		{
			u32 records = 0;
			u64 bytes = 0;
			/// The pack was written whole.
			bool rewrote = false;
			/// Stopped at the deadline; the rest is still pending.
			bool deadline_hit = false;
			/// Another process is writing the pack; nothing was written.
			bool contended = false;
			/// A write failed (usually a full disk). The pack is not written again this session.
			bool failed = false;
		};

		using Visitor = std::function<void(const u8* key, u32 key_size, const std::vector<u8>& data, u32 aux)>;

		Pack();
		~Pack();

		Pack(const Pack&) = delete;
		Pack& operator=(const Pack&) = delete;

		/// Reads the file at `path` (if any) into RAM, up to `ram_cap` bytes of payload. Never fails:
		/// a missing or unusable file gives an empty pack. Closes what was open first; nothing is written.
		void Open(const std::string& path, Kind kind, const Stamp& stamp, u64 ram_cap);
		/// Drops the table. Does not write: the owner writes first.
		void Close();

		bool IsOpen() const;
		std::string GetPath() const;
		LoadInfo GetLoadInfo() const;
		size_t GetEntryCount() const;
		/// Payload bytes held in RAM.
		u64 GetRamBytes() const;
		/// Bytes in the file, as far as the pack knows.
		u64 GetFileBytes() const;

		bool Get(u16 type, const void* key, u32 key_size, std::vector<u8>* data, u32* aux = nullptr) const;
		/// Stores an entry. An entry with the same payload and aux is left alone, and with `replace`
		/// false so is any existing one. Returns whether anything changed.
		bool Put(u16 type, const void* key, u32 key_size, const void* data, size_t size, u32 aux = 0,
			bool replace = true);
		void Erase(u16 type, const void* key, u32 key_size);
		/// Calls `fn` for every entry of `type`, in the order the keys were first stored. No other
		/// method of this pack may be called from `fn`.
		void ForEach(u16 type, const Visitor& fn) const;
		/// Drops every entry. The next Write leaves an empty pack.
		void Clear();

		bool HasPendingWrites() const;
		/// Puts what changed on disk. Stops before a record that would start after `deadline`.
		WriteResult Write(Clock::time_point deadline = Clock::time_point::max());

	private:
		struct Entry
		{
			std::shared_ptr<const std::vector<u8>> data;
			u32 aux = 0;
			bool erased = false;
			/// The order its key was first stored in, kept across replacements and rewrites.
			u64 seq = 0;
			/// Bumped by every change; equal to written_gen once the change is on disk.
			u64 gen = 0;
			u64 written_gen = 0;
			/// Size of the record on disk that holds it now, 0 if none.
			u64 disk_bytes = 0;
		};

		struct Pending
		{
			std::string map_key;
			std::shared_ptr<const std::vector<u8>> data;
			u32 aux;
			bool erased;
			u64 gen;
			u64 seq;
		};

		static std::string MapKey(u16 type, const void* key, u32 key_size);
		void Load();
		/// Writes `pending` in order; `written` gets the index and record size of each one written.
		static void WriteRecords(std::FILE* fp, const std::vector<Pending>& pending, bool whole,
			Clock::time_point deadline, WriteResult* res, std::vector<std::pair<size_t, u64>>* written);

		std::string m_path;
		Kind m_kind = KIND_TEST;
		Digest m_stamp = {};
		u64 m_ram_cap = 0;

		/// Held for the whole of a Write, so two writes of one pack never interleave.
		std::mutex m_io_mutex;

		mutable std::mutex m_mutex;
		bool m_open = false;
		LoadInfo m_load_info;
		std::unordered_map<std::string, Entry> m_entries;
		u64 m_next_seq = 0;
		u64 m_ram_bytes = 0;
		u64 m_file_bytes = 0;
		/// Bytes of the records the table's entries live in; the rest of the file is garbage.
		u64 m_live_bytes = 0;
		/// The file has to be written whole: it is of another format, ends in a broken record, or was cleared.
		bool m_replace = false;
		/// Bumped by Clear, so a Write that straddles one knows its appends are stale.
		u64 m_epoch = 0;
		/// Some records were left on disk at the RAM cap.
		bool m_partial = false;
		/// The file ends in a broken record after m_file_bytes. Cut by the next Write.
		bool m_torn = false;
		bool m_write_failed = false;
	};

	/// Opens `<dir>/<stem>.pack`, creating `dir` if need be, after removing the packs in `dir` of the
	/// same `prefix` but another stem, all but the two used most recently (other builds or drivers).
	std::shared_ptr<Pack> OpenPack(const std::string& dir, std::string_view prefix, const std::string& stem, Kind kind,
		const Stamp& stamp, u64 ram_cap = GetDefaultPackRamCap());

	/// The thread that writes a renderer's packs in the background: every `interval`, when asked, and
	/// once more, within a deadline, when it stops. `extra` runs at the end of each pass, for whatever
	/// else the owner saves; `asked` is true for a Flush or the last pass.
	class PackWriter
	{
	public:
		using Extra = std::function<void(Clock::time_point deadline, bool asked)>;

		static constexpr std::chrono::seconds DEFAULT_INTERVAL{30};
		/// How long the last pass may take when a renderer closes.
		static constexpr std::chrono::milliseconds CLOSE_DEADLINE{2000};

		PackWriter();
		~PackWriter();

		PackWriter(const PackWriter&) = delete;
		PackWriter& operator=(const PackWriter&) = delete;

		/// Also removes, once per process, what older caches and unused packs left under `cache_dir`
		/// (see CleanLeftovers).
		void Start(const std::string& cache_dir, Extra extra = {}, Clock::duration interval = DEFAULT_INTERVAL);
		/// Runs the last pass, stopping at `deadline`, and joins the thread. The packs stay open.
		void Stop(Clock::time_point deadline);
		bool IsRunning() const;

		void Add(std::shared_ptr<Pack> pack);
		/// Writes `pack` once more at the next pass, then lets it go. Without a running writer it is
		/// let go at once.
		void Release(std::shared_ptr<Pack> pack);

		/// Starts a pass now. With `wait`, returns once it has finished.
		void Flush(bool wait);

	private:
		void Run();
		void Pass(Clock::time_point deadline, bool asked);

		std::string m_cache_dir;
		Extra m_extra;
		Clock::duration m_interval{};
		std::thread m_thread;

		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		std::condition_variable m_done_cv;
		std::vector<std::shared_ptr<Pack>> m_packs;
		std::vector<std::shared_ptr<Pack>> m_released;
		bool m_running = false;
		bool m_stop = false;
		Clock::time_point m_stop_deadline{};
		/// Passes asked for, and passes finished: a Flush waits for its number to come up.
		u64 m_asked = 0;
		u64 m_finished = 0;
	};

	/// Deletes what is left of the caches older builds used (the shared SPIR-V and GL program stores
	/// and the pipeline key lists) once no build has written them for a week, so a stable and a
	/// nightly install sharing a data root do not wipe each other's; and packs not opened for 60 days,
	/// never an open one; and interrupted writes. Returns how many files went.
	u32 CleanLeftovers(const std::string& cache_dir);

	/// Deletes every GS shader and pipeline cache file under `cache_dir`. A pack that is open in this
	/// process is emptied in place instead, so its owner never writes to a deleted file. Must run where
	/// nothing else writes the other caches: on the GS thread while it runs (see GSClearShaderCache).
	/// Returns how many files were removed or emptied.
	u32 DeleteAll(const std::string& cache_dir);
} // namespace GSCacheFile
