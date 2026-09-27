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

#include "common/Pcsx2Defs.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GSCacheFile
{
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
	/// rejects, and an fsync can stall the GS thread for hundreds of milliseconds on phone flash.
	bool WriteFileAtomic(const std::string& path, const void* data, size_t size);

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
	bool WriteFramedFile(const std::string& path, Kind kind, const Stamp& stamp, const void* data, size_t size);
	ReadResult ReadFramedFile(const std::string& path, Kind kind, const Stamp& stamp, std::vector<u8>* payload);

	/// A file of fixed-size records written in place, each record carrying its own checksum in its
	/// last four bytes. The header also holds one u64 the owner can use (a session counter).
	std::vector<u8> MakeRecordFileHeader(Kind kind, const Stamp& stamp, u32 record_size, u64 user);
	size_t GetRecordFileHeaderSize();
	/// Checks the header; on success returns the records that pass their checksum, in file order.
	/// A torn record at the end is ignored.
	ReadResult ParseRecordFile(const std::vector<u8>& data, Kind kind, const Stamp& stamp, u32 record_size,
		u64* user, std::vector<std::vector<u8>>* records, u32* dropped = nullptr);
	/// Fills the last four bytes of `record` with its checksum.
	void SealRecord(void* record, u32 record_size);
	bool IsRecordSealed(const void* record, u32 record_size);

	/// An append-only store of blobs under fixed-size keys, as a pair of files: `<base>.idx` holds
	/// the entries, `<base>.bin` the data. Entries are content-addressed by their key; a later entry
	/// under the same key replaces an earlier one.
	///
	/// One process writes at a time, enforced with a lock on the index. Another process that finds
	/// it locked reads what is there and writes nothing: two writers appending to one pair could
	/// point an entry at the other's data. Where there is no lock to take (a libretro frontend's
	/// file system, a mount without locks) the store is used unlocked; the checksums still keep a
	/// mixed-up entry from reaching the driver.
	class BlobStore
	{
	public:
		/// A store whose data file grows past this is started over when opened, since entries for
		/// sources a newer build no longer produces are never removed one by one.
		static constexpr u64 DEFAULT_MAX_DATA_SIZE = 256ull * 1024 * 1024;

		struct OpenInfo
		{
			/// Why an existing store was not used, Ok if it was (or none existed).
			ReadResult discarded = ReadResult::Ok;
			u32 entries = 0;
			/// Index bytes cut off at the first entry that failed its checksum or pointed past the data.
			u64 truncated_bytes = 0;
			bool read_only = false;
		};

		BlobStore();
		~BlobStore();

		BlobStore(const BlobStore&) = delete;
		BlobStore& operator=(const BlobStore&) = delete;

		bool Open(const std::string& base_path, Kind kind, const Stamp& stamp, u32 key_size,
			u64 max_data_size = DEFAULT_MAX_DATA_SIZE);
		void Close();
		/// Empties the store under the same stamp, in place, keeping the lock.
		bool Recreate();

		bool IsOpen() const { return m_index_file != nullptr; }
		/// False when another process holds the store, or after an append failed this session.
		bool IsWritable() const;
		const std::string& GetBasePath() const { return m_base_path; }
		bool OwnsPath(const std::string& path) const;
		const OpenInfo& GetOpenInfo() const { return m_open_info; }
		size_t GetEntryCount() const;

		/// False if the key is absent, or its data could not be read or failed its checksum; such an
		/// entry is forgotten, so the caller rebuilds it and inserts it again.
		bool Lookup(const void* key, std::vector<u8>* data, u32* aux = nullptr);
		bool Insert(const void* key, const void* data, size_t size, u32 aux = 0);

		/// Tests and the log: entries whose data failed its checksum since Open.
		u32 GetBadDataCount() const { return m_bad_data; }

	private:
		struct Entry
		{
			u64 offset;
			u32 size;
			u32 aux;
			u64 data_hash;
		};

		bool CreateNew();
		bool ReadExisting();
		bool ResetFiles();
		void CloseFiles();
		size_t GetEntrySize() const { return m_key_size + 32; }

		std::string m_base_path;
		Kind m_kind = KIND_TEST;
		Digest m_stamp = {};
		std::string m_stamp_text;
		u32 m_key_size = 0;
		u64 m_max_data_size = DEFAULT_MAX_DATA_SIZE;

		std::FILE* m_index_file = nullptr;
		std::FILE* m_data_file = nullptr;
		bool m_read_only = false;
		bool m_write_failed = false;
		OpenInfo m_open_info;
		u32 m_bad_data = 0;

		mutable std::mutex m_mutex;
		std::unordered_map<std::string, Entry> m_entries;
	};

	/// Deletes every GS shader and pipeline cache file under `cache_dir`. A store that is open in this
	/// process is emptied in place instead, so it keeps its lock and never writes to a deleted file.
	/// Must run where nothing else writes the other caches: on the GS thread while it runs (see
	/// GSClearShaderCache). Returns how many files were removed or emptied.
	u32 DeleteAll(const std::string& cache_dir);
} // namespace GSCacheFile
