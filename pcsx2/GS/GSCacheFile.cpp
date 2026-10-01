// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCacheFile.h"

#include "BuildVersion.h"
#include "ShaderCacheVersion.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <iterator>
#include <optional>

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <io.h>
#else
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#if __has_include("gs_cache_build_id.h")
#include "gs_cache_build_id.h"
#endif

namespace GSCacheFile
{
	namespace
	{
		static constexpr u32 FRAMED_MAGIC = 0x46435347; // "GSCF"
		static constexpr u32 INDEX_MAGIC = 0x49435347; // "GSCI"
		static constexpr u32 RECORD_MAGIC = 0x52435347; // "GSCR"
		/// Bumped when the layout of these headers or entries changes.
		static constexpr u32 FORMAT = 1;

#pragma pack(push, 4)
		// The magic and the kind come first and both are large, so a build from before this format,
		// which reads the first words as a small version number or a Vulkan pipeline cache header,
		// rejects the file.
		struct FileHeader
		{
			u32 magic;
			u32 kind;
			u32 format;
			u32 param; // framed: 0; index: key size; records: record size
			Digest stamp;
			u64 size; // framed: payload size; index: 0; records: the owner's u64
			u64 payload_hash; // framed only
			u64 header_hash; // of every field above
		};
#pragma pack(pop)
		static_assert(sizeof(FileHeader) == 56);

		static FileHeader MakeHeader(u32 magic, Kind kind, u32 param, const Digest& stamp, u64 size, u64 payload_hash)
		{
			FileHeader h = {};
			h.magic = magic;
			h.kind = kind;
			h.format = FORMAT;
			h.param = param;
			h.stamp = stamp;
			h.size = size;
			h.payload_hash = payload_hash;
			h.header_hash = Hash64(&h, offsetof(FileHeader, header_hash));
			return h;
		}

		/// Ok, BadHeader or StampMismatch. The stamp is checked last, so a mismatch means an intact
		/// header from another build, driver or configuration.
		static ReadResult CheckHeader(const FileHeader& h, u32 magic, Kind kind, u32 param, const Digest& stamp)
		{
			if (h.magic != magic || h.kind != kind || h.format != FORMAT || h.param != param ||
				h.header_hash != Hash64(&h, offsetof(FileHeader, header_hash)))
			{
				return ReadResult::BadHeader;
			}
			return (h.stamp == stamp) ? ReadResult::Ok : ReadResult::StampMismatch;
		}

		static int FileDescriptor(std::FILE* fp)
		{
#ifdef _WIN32
			return _fileno(fp);
#else
			return fileno(fp);
#endif
		}

		/// False where the stream has no descriptor (a libretro frontend's VFS) or the call fails.
		static bool TruncateFile(std::FILE* fp, u64 size)
		{
			std::fflush(fp);
			const int fd = FileDescriptor(fp);
			if (fd < 0)
				return false;
#ifdef _WIN32
			return (_chsize_s(fd, static_cast<__int64>(size)) == 0);
#else
			return (ftruncate(fd, static_cast<off_t>(size)) == 0);
#endif
		}

		enum class LockResult
		{
			Locked,
			/// Another open file holds it: the only answer that means "someone else is writing".
			Contended,
			/// No descriptor, or the file system has no locks (ENOLCK on some FUSE and network mounts).
			Unavailable,
		};

		static LockResult TryLockFile(std::FILE* fp, std::string* why)
		{
			const int fd = FileDescriptor(fp);
			if (fd < 0)
			{
				*why = "no file descriptor";
				return LockResult::Unavailable;
			}
#ifdef _WIN32
			const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
			if (h == INVALID_HANDLE_VALUE)
			{
				*why = "no file handle";
				return LockResult::Unavailable;
			}
			// A byte far past any data, so the lock never stops another process reading the file.
			OVERLAPPED ov = {};
			ov.OffsetHigh = 0x7FFFFFFFu;
			if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
				return LockResult::Locked;
			const DWORD err = GetLastError();
			if (err == ERROR_LOCK_VIOLATION)
				return LockResult::Contended;
			*why = fmt::format("LockFileEx error {}", err);
			return LockResult::Unavailable;
#else
			// flock, not lockf: the lock belongs to this open file, so a second open of the same file in
			// this process (a renderer switch before the old cache is closed) is refused too.
			if (flock(fd, LOCK_EX | LOCK_NB) == 0)
				return LockResult::Locked;
			const int err = errno;
			if (err == EWOULDBLOCK || err == EAGAIN)
				return LockResult::Contended;
			*why = fmt::format("flock errno {}", err);
			return LockResult::Unavailable;
#endif
		}

		/// Writes and reads go straight to the file: after a failed write, a stdio buffer would still
		/// hold the bytes and put them back after the file was cut to its last good size.
		static std::FILE* OpenUnbuffered(const std::string& path, const char* mode)
		{
			std::FILE* fp = FileSystem::OpenCFile(path.c_str(), mode);
			if (fp)
				std::setvbuf(fp, nullptr, _IONBF, 0);
			return fp;
		}

		/// Live stores, so a clear can empty them in place instead of deleting files they have open.
		static std::mutex s_registry_mutex;
		static std::vector<BlobStore*> s_registry;
	} // namespace

	bool Digest::operator==(const Digest& rhs) const
	{
		return (std::memcmp(bytes, rhs.bytes, sizeof(bytes)) == 0);
	}

	std::string Digest::ToHex() const
	{
		std::string ret;
		for (const u8 b : bytes)
			ret += fmt::format("{:02x}", b);
		return ret;
	}

	Digest Hash128(const void* data, size_t size)
	{
		const XXH128_hash_t h = XXH3_128bits(data, size);
		Digest d;
		std::memcpy(&d.bytes[0], &h.low64, sizeof(u64));
		std::memcpy(&d.bytes[8], &h.high64, sizeof(u64));
		return d;
	}

	u64 Hash64(const void* data, size_t size)
	{
		return XXH3_64bits(data, size);
	}

	const std::string& GetBuildId()
	{
#ifdef GS_CACHE_BUILD_ID
		static const std::string id = GS_CACHE_BUILD_ID;
#else
		static const std::string id = std::string("git ") + BuildVersion::GitHash;
#endif
		return id;
	}

	u32 GetBuildVersionWord()
	{
		const std::string text = fmt::format("{} {}", SHADER_CACHE_VERSION, GetBuildId());
		return static_cast<u32>(Hash64(text.data(), text.size()));
	}

	Stamp& Stamp::Add(std::string_view name, std::string_view value)
	{
		// Length-prefixed, so no pair of values can run together into the same text.
		fmt::format_to(std::back_inserter(m_text), "{}[{}]={}\n", name, value.size(), value);
		return *this;
	}

	Stamp& Stamp::Add(std::string_view name, u64 value)
	{
		return Add(name, std::string_view(fmt::format("{}", value)));
	}

	Stamp& Stamp::AddHex(std::string_view name, const void* data, size_t size)
	{
		std::string hex;
		for (size_t i = 0; i < size; i++)
			hex += fmt::format("{:02x}", static_cast<const u8*>(data)[i]);
		return Add(name, std::string_view(hex));
	}

	Digest Stamp::GetDigest() const
	{
		return Hash128(m_text.data(), m_text.size());
	}

	const char* ReadResultString(ReadResult result)
	{
		switch (result)
		{
			case ReadResult::Ok: return "ok";
			case ReadResult::Missing: return "missing";
			case ReadResult::BadHeader: return "bad header";
			case ReadResult::StampMismatch: return "stamp mismatch";
			case ReadResult::Truncated: return "truncated";
			case ReadResult::ChecksumMismatch: return "checksum mismatch";
		}
		return "unknown";
	}

	bool WriteFileAtomic(const std::string& path, const void* data, size_t size)
	{
		// Named per process, so two processes writing the same cache cannot interleave in one file.
#ifdef _WIN32
		const std::string tmp_path = fmt::format("{}.tmp{}", path, GetCurrentProcessId());
#else
		const std::string tmp_path = fmt::format("{}.tmp{}", path, getpid());
#endif
		if (!FileSystem::WriteBinaryFile(tmp_path.c_str(), data, size) ||
			!FileSystem::RenamePath(tmp_path.c_str(), path.c_str()))
		{
			FileSystem::DeleteFilePath(tmp_path.c_str());
			return false;
		}
		return true;
	}

	bool WriteFramedFile(const std::string& path, Kind kind, const Stamp& stamp, const void* data, size_t size)
	{
		const FileHeader h = MakeHeader(FRAMED_MAGIC, kind, 0, stamp.GetDigest(), size, Hash64(data, size));
		std::vector<u8> out(sizeof(h) + size);
		std::memcpy(out.data(), &h, sizeof(h));
		if (size > 0)
			std::memcpy(out.data() + sizeof(h), data, size);
		return WriteFileAtomic(path, out.data(), out.size());
	}

	ReadResult ReadFramedFile(const std::string& path, Kind kind, const Stamp& stamp, std::vector<u8>* payload)
	{
		std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
		if (!data.has_value())
			return ReadResult::Missing;
		if (data->size() < sizeof(FileHeader))
			return ReadResult::Truncated;

		FileHeader h;
		std::memcpy(&h, data->data(), sizeof(h));
		if (const ReadResult res = CheckHeader(h, FRAMED_MAGIC, kind, 0, stamp.GetDigest()); res != ReadResult::Ok)
			return res;
		if (data->size() - sizeof(h) != h.size)
			return ReadResult::Truncated;
		if (Hash64(data->data() + sizeof(h), h.size) != h.payload_hash)
			return ReadResult::ChecksumMismatch;

		payload->assign(data->begin() + sizeof(h), data->end());
		return ReadResult::Ok;
	}

	std::vector<u8> MakeRecordFileHeader(Kind kind, const Stamp& stamp, u32 record_size, u64 user)
	{
		const FileHeader h = MakeHeader(RECORD_MAGIC, kind, record_size, stamp.GetDigest(), user, 0);
		std::vector<u8> out(sizeof(h));
		std::memcpy(out.data(), &h, sizeof(h));
		return out;
	}

	size_t GetRecordFileHeaderSize()
	{
		return sizeof(FileHeader);
	}

	void SealRecord(void* record, u32 record_size)
	{
		const u32 sum = static_cast<u32>(Hash64(record, record_size - sizeof(u32)));
		std::memcpy(static_cast<u8*>(record) + record_size - sizeof(u32), &sum, sizeof(sum));
	}

	bool IsRecordSealed(const void* record, u32 record_size)
	{
		u32 sum;
		std::memcpy(&sum, static_cast<const u8*>(record) + record_size - sizeof(u32), sizeof(sum));
		return (sum == static_cast<u32>(Hash64(record, record_size - sizeof(u32))));
	}

	ReadResult ParseRecordFile(const std::vector<u8>& data, Kind kind, const Stamp& stamp, u32 record_size,
		u64* user, std::vector<std::vector<u8>>* records, u32* dropped)
	{
		if (dropped)
			*dropped = 0;
		records->clear();
		if (data.size() < sizeof(FileHeader))
			return data.empty() ? ReadResult::Missing : ReadResult::Truncated;

		FileHeader h;
		std::memcpy(&h, data.data(), sizeof(h));
		if (const ReadResult res = CheckHeader(h, RECORD_MAGIC, kind, record_size, stamp.GetDigest()); res != ReadResult::Ok)
			return res;
		*user = h.size;

		// Whole records only: a partial one at the end is an append the app did not finish.
		const size_t count = (data.size() - sizeof(h)) / record_size;
		records->reserve(count);
		for (size_t i = 0; i < count; i++)
		{
			const u8* rec = data.data() + sizeof(h) + i * record_size;
			if (!IsRecordSealed(rec, record_size))
			{
				if (dropped)
					(*dropped)++;
				continue;
			}
			records->emplace_back(rec, rec + record_size);
		}
		return ReadResult::Ok;
	}

	BlobStore::BlobStore() = default;

	BlobStore::~BlobStore()
	{
		Close();
	}

	bool BlobStore::Open(const std::string& base_path, Kind kind, const Stamp& stamp, u32 key_size, u64 max_data_size)
	{
		Close();
		{
			std::unique_lock lock(m_mutex);
			m_base_path = base_path;
			m_kind = kind;
			m_stamp = stamp.GetDigest();
			m_stamp_text = stamp.GetText();
			m_key_size = key_size;
			m_max_data_size = max_data_size;
			m_open_info = {};
			m_bad_data = 0;
			m_write_failed = false;

			if (!ReadExisting() && !CreateNew())
				return false;
		}

		std::unique_lock reg(s_registry_mutex);
		s_registry.push_back(this);
		return true;
	}

	void BlobStore::CloseFiles()
	{
		if (m_index_file)
		{
			std::fclose(m_index_file);
			m_index_file = nullptr;
		}
		if (m_data_file)
		{
			std::fclose(m_data_file);
			m_data_file = nullptr;
		}
	}

	void BlobStore::Close()
	{
		{
			std::unique_lock reg(s_registry_mutex);
			s_registry.erase(std::remove(s_registry.begin(), s_registry.end(), this), s_registry.end());
		}
		std::unique_lock lock(m_mutex);
		CloseFiles();
		m_entries.clear();
		m_read_only = false;
	}

	bool BlobStore::IsWritable() const
	{
		std::unique_lock lock(m_mutex);
		return m_index_file && m_data_file && !m_read_only && !m_write_failed;
	}

	bool BlobStore::Recreate()
	{
		std::unique_lock lock(m_mutex);
		m_entries.clear();
		m_write_failed = false;
		if (m_read_only)
		{
			// Another process owns the files; stop using them rather than delete its work.
			CloseFiles();
			return false;
		}
		if (m_index_file && m_data_file)
			return ResetFiles();
		CloseFiles();
		return CreateNew();
	}

	size_t BlobStore::GetEntryCount() const
	{
		std::unique_lock lock(m_mutex);
		return m_entries.size();
	}

	bool BlobStore::ResetFiles()
	{
		// In place where the files allow it, so the lock on the index is never let go.
		const FileHeader h = MakeHeader(INDEX_MAGIC, m_kind, m_key_size, m_stamp, 0, 0);
		if (!TruncateFile(m_data_file, 0) || !TruncateFile(m_index_file, 0))
		{
			// No descriptor to truncate through: reopen both files empty. Nothing was locked.
			CloseFiles();
			m_index_file = OpenUnbuffered(m_base_path + ".idx", "w+b");
			m_data_file = OpenUnbuffered(m_base_path + ".bin", "w+b");
		}
		if (!m_index_file || !m_data_file || FileSystem::FSeek64(m_index_file, 0, SEEK_SET) != 0 ||
			std::fwrite(&h, sizeof(h), 1, m_index_file) != 1)
		{
			Console.Error("GS cache: cannot create '%s'", m_base_path.c_str());
			CloseFiles();
			return false;
		}
		return true;
	}

	bool BlobStore::CreateNew()
	{
		const std::string index_path = m_base_path + ".idx";

		// Opened and locked before anything is emptied, so a store another process holds is left alone.
		m_index_file = OpenUnbuffered(index_path, "a+b");
		// A libretro frontend's file system opens "a+" only on an existing file.
		if (!m_index_file)
			m_index_file = OpenUnbuffered(index_path, "w+b");
		if (!m_index_file)
		{
			Console.Error("GS cache: cannot open '%s'", index_path.c_str());
			return false;
		}
		std::string why;
		switch (TryLockFile(m_index_file, &why))
		{
			case LockResult::Locked:
				break;
			case LockResult::Contended:
				Console.Warning("GS cache: '%s' is in use by another process; running without it", index_path.c_str());
				CloseFiles();
				return false;
			case LockResult::Unavailable:
				Console.Warning("GS cache: cannot lock '%s' (%s); writing it unlocked", index_path.c_str(), why.c_str());
				break;
		}

		m_data_file = OpenUnbuffered(m_base_path + ".bin", "w+b");
		m_read_only = false;
		return ResetFiles();
	}

	bool BlobStore::ReadExisting()
	{
		const std::string index_path = m_base_path + ".idx";
		const std::string data_path = m_base_path + ".bin";

		m_index_file = OpenUnbuffered(index_path, "r+b");
		if (!m_index_file)
		{
			m_open_info.discarded = ReadResult::Missing;
			return false;
		}
		std::string why;
		switch (TryLockFile(m_index_file, &why))
		{
			case LockResult::Locked:
				break;
			case LockResult::Contended:
				// Read what the other process has written; write nothing.
				std::fclose(m_index_file);
				m_index_file = OpenUnbuffered(index_path, "rb");
				if (!m_index_file)
					return false;
				m_read_only = true;
				m_open_info.read_only = true;
				Console.Warning("GS cache: '%s' is in use by another process; reading it without adding to it",
					index_path.c_str());
				break;
			case LockResult::Unavailable:
				Console.Warning("GS cache: cannot lock '%s' (%s); using it unlocked", index_path.c_str(), why.c_str());
				break;
		}

		const auto fail = [this](ReadResult why) {
			m_open_info.discarded = why;
			CloseFiles();
			m_entries.clear();
			// A store another process holds is not ours to replace: leave its files alone and run
			// without a cache, with the files closed so CreateNew is not attempted on them.
			m_read_only = false;
			return false;
		};

		// The whole index in one read: the file is unbuffered.
		const s64 index_size = FileSystem::FSize64(m_index_file);
		if (index_size < static_cast<s64>(sizeof(FileHeader)))
			return fail(ReadResult::Truncated);
		std::vector<u8> index(static_cast<size_t>(index_size));
		if (FileSystem::FSeek64(m_index_file, 0, SEEK_SET) != 0 || std::fread(index.data(), index.size(), 1, m_index_file) != 1)
			return fail(ReadResult::Truncated);

		FileHeader h;
		std::memcpy(&h, index.data(), sizeof(h));
		if (const ReadResult res = CheckHeader(h, INDEX_MAGIC, m_kind, m_key_size, m_stamp); res != ReadResult::Ok)
		{
			Console.WriteLn("GS cache: discarding '%s': %s", index_path.c_str(), ReadResultString(res));
			return fail(res);
		}

		m_data_file = OpenUnbuffered(data_path, m_read_only ? "rb" : "r+b");
		if (!m_data_file)
			return fail(ReadResult::Missing);
		const s64 data_size_signed = FileSystem::FSize64(m_data_file);
		if (data_size_signed < 0)
			return fail(ReadResult::Truncated);
		const u64 data_size = static_cast<u64>(data_size_signed);
		if (data_size > m_max_data_size)
		{
			Console.WriteLn("GS cache: '%s' holds %llu bytes, over the limit; starting over", data_path.c_str(),
				static_cast<unsigned long long>(data_size));
			return fail(ReadResult::Ok);
		}

		const size_t entry_size = GetEntrySize();
		u64 good_end = sizeof(h);
		while (good_end + entry_size <= index.size())
		{
			const u8* rec = index.data() + good_end;
			Entry e;
			u64 entry_hash;
			std::memcpy(&e.offset, &rec[m_key_size], sizeof(u64));
			std::memcpy(&e.size, &rec[m_key_size + 8], sizeof(u32));
			std::memcpy(&e.aux, &rec[m_key_size + 12], sizeof(u32));
			std::memcpy(&e.data_hash, &rec[m_key_size + 16], sizeof(u64));
			std::memcpy(&entry_hash, &rec[m_key_size + 24], sizeof(u64));

			// The first entry that is damaged or points past the data ends the index. Every later
			// entry was appended after it, so it cannot be trusted to be whole either.
			if (entry_hash != Hash64(rec, entry_size - sizeof(u64)) || e.offset > data_size ||
				e.size > data_size - e.offset)
			{
				break;
			}

			m_entries.insert_or_assign(std::string(reinterpret_cast<const char*>(rec), m_key_size), e);
			good_end += entry_size;
		}

		if (index.size() > good_end)
		{
			m_open_info.truncated_bytes = index.size() - good_end;
			Console.Warning("GS cache: '%s' ends in %llu damaged or incomplete bytes; %s", index_path.c_str(),
				static_cast<unsigned long long>(m_open_info.truncated_bytes),
				m_read_only ? "ignoring them" : "cutting them off");
			if (!m_read_only && !TruncateFile(m_index_file, good_end))
			{
				// No descriptor: rewrite the good part instead.
				std::fclose(m_index_file);
				m_index_file = OpenUnbuffered(index_path, "w+b");
				if (!m_index_file || std::fwrite(index.data(), good_end, 1, m_index_file) != 1)
					return fail(ReadResult::Truncated);
			}
		}

		m_open_info.entries = static_cast<u32>(m_entries.size());
		Console.WriteLn("GS cache: read %zu entries from '%s'", m_entries.size(), index_path.c_str());
		return true;
	}

	bool BlobStore::Lookup(const void* key, std::vector<u8>* data, u32* aux)
	{
		std::unique_lock lock(m_mutex);
		const auto it = m_entries.find(std::string(static_cast<const char*>(key), m_key_size));
		if (it == m_entries.end() || !m_data_file)
			return false;

		const Entry& e = it->second;
		data->resize(e.size);
		if (FileSystem::FSeek64(m_data_file, static_cast<s64>(e.offset), SEEK_SET) != 0 ||
			(e.size > 0 && std::fread(data->data(), e.size, 1, m_data_file) != 1) ||
			Hash64(data->data(), data->size()) != e.data_hash)
		{
			std::clearerr(m_data_file);
			m_bad_data++;
			Console.Warning("GS cache: an entry in '%s.bin' failed its checksum; rebuilding it", m_base_path.c_str());
			m_entries.erase(it);
			data->clear();
			return false;
		}

		if (aux)
			*aux = e.aux;
		return true;
	}

	bool BlobStore::Insert(const void* key, const void* data, size_t size, u32 aux)
	{
		std::unique_lock lock(m_mutex);
		if (!m_index_file || !m_data_file || m_read_only || m_write_failed)
			return false;

		// Two threads that compiled the same source both get here; the second has nothing to add.
		if (m_entries.find(std::string(static_cast<const char*>(key), m_key_size)) != m_entries.end())
			return true;

		const s64 data_end = FileSystem::FSize64(m_data_file);
		const s64 index_end = FileSystem::FSize64(m_index_file);
		if (data_end < 0 || index_end < 0)
			return false;

		Entry e;
		e.offset = static_cast<u64>(data_end);
		e.size = static_cast<u32>(size);
		e.aux = aux;
		e.data_hash = Hash64(data, size);

		const size_t entry_size = GetEntrySize();
		std::vector<u8> rec(entry_size);
		std::memcpy(&rec[0], key, m_key_size);
		std::memcpy(&rec[m_key_size], &e.offset, sizeof(u64));
		std::memcpy(&rec[m_key_size + 8], &e.size, sizeof(u32));
		std::memcpy(&rec[m_key_size + 12], &e.aux, sizeof(u32));
		std::memcpy(&rec[m_key_size + 16], &e.data_hash, sizeof(u64));
		const u64 entry_hash = Hash64(rec.data(), entry_size - sizeof(u64));
		std::memcpy(&rec[m_key_size + 24], &entry_hash, sizeof(u64));

		// Data first, then the entry that points at it: a kill between the two leaves unreferenced
		// data, never an entry without its data.
		if (FileSystem::FSeek64(m_data_file, data_end, SEEK_SET) != 0 ||
			(size > 0 && std::fwrite(data, size, 1, m_data_file) != 1) ||
			FileSystem::FSeek64(m_index_file, index_end, SEEK_SET) != 0 ||
			std::fwrite(rec.data(), entry_size, 1, m_index_file) != 1)
		{
			// Usually a full disk. Cut both files back to where they were, so no part of this entry is
			// left for the next start to read, and add nothing more this session: another append would
			// most likely fail the same way.
			std::clearerr(m_data_file);
			std::clearerr(m_index_file);
			const bool cut = TruncateFile(m_index_file, static_cast<u64>(index_end)) &&
							 TruncateFile(m_data_file, static_cast<u64>(data_end));
			m_write_failed = true;
			Console.Error("GS cache: failed to append to '%s'%s; not adding to it again this session",
				m_base_path.c_str(), cut ? "" : " (and could not cut it back; the next start will)");
			return false;
		}

		m_entries.insert_or_assign(std::string(static_cast<const char*>(key), m_key_size), e);
		return true;
	}

	bool BlobStore::OwnsPath(const std::string& path) const
	{
		return (path == m_base_path + ".idx" || path == m_base_path + ".bin");
	}

	u32 DeleteAll(const std::string& cache_dir)
	{
		static constexpr const char* prefixes[] = {
			"vulkan_shaders", "vulkan_pipelines", "gl_programs", "d3d_shaders_", "d3d12_", "lsfg_spirv"};

		std::unique_lock reg(s_registry_mutex);

		u32 removed = 0;
		FileSystem::FindResultsArray files;
		if (FileSystem::FindFiles(cache_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				const std::string_view name = Path::GetFileName(fd.FileName);
				const bool is_cache = std::any_of(std::begin(prefixes), std::end(prefixes),
					[&name](const char* prefix) { return name.compare(0, std::strlen(prefix), prefix) == 0; });
				if (!is_cache)
					continue;

				// A live store is emptied in place, keeping its lock and its open files.
				const auto owner = std::find_if(s_registry.begin(), s_registry.end(),
					[&fd](const BlobStore* store) { return store->OwnsPath(fd.FileName); });
				if (owner != s_registry.end())
					continue;

				removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
			}
		}
		const std::string probe = Path::Combine(cache_dir, "x");
		for (BlobStore* store : s_registry)
		{
			if (Path::GetDirectory(store->GetBasePath()) == Path::GetDirectory(probe))
				removed += store->Recreate() ? 2 : 0;
		}

		const std::string keys_dir = Path::Combine(cache_dir, "vulkan_pipeline_keys");
		FileSystem::FindResultsArray key_files;
		if (FileSystem::FindFiles(keys_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &key_files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : key_files)
				removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
		}
		FileSystem::DeleteDirectory(keys_dir.c_str());

		return removed;
	}

	namespace
	{
		static bool ProcessIsAlive(u64 pid)
		{
#ifdef _WIN32
			const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
			if (!h)
				return false;
			DWORD code = 0;
			const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
			CloseHandle(h);
			return alive;
#else
			return (pid > 0 && (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM));
#endif
		}
	} // namespace

	u32 CleanStaleTempFiles(const std::string& dir)
	{
		static constexpr s64 DAY = 24 * 60 * 60;
		const s64 now = static_cast<s64>(std::time(nullptr));
		u32 removed = 0;
		FileSystem::FindResultsArray files;
		if (!FileSystem::FindFiles(dir.c_str(), "*.tmp*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
			return 0;
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			const std::string_view name = Path::GetFileName(fd.FileName);
			const size_t pos = name.rfind(".tmp");
			if (pos == std::string_view::npos || pos + 4 >= name.size())
				continue;
			const std::string_view digits = name.substr(pos + 4);
			if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
				continue;
			const u64 pid = std::strtoull(std::string(digits).c_str(), nullptr, 10);
			// A live pid may be a new process that reused the number, so age settles it too.
			if (ProcessIsAlive(pid) && now - static_cast<s64>(fd.ModificationTime) < DAY)
				continue;
			removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
		}
		return removed;
	}

	std::string ShortName(const Digest& digest)
	{
		return digest.ToHex().substr(0, 8);
	}

	u32 PruneOtherIdentities(const std::string& dir, std::string_view prefix, std::string_view current_stem, u32 keep)
	{
		// stem -> newest modification time of its files. A stem is the name up to its first '.'.
		std::vector<std::pair<std::string, s64>> stems;
		FileSystem::FindResultsArray files;
		if (!FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
			return 0;
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			const std::string_view name = Path::GetFileName(fd.FileName);
			if (name.compare(0, prefix.size(), prefix) != 0)
				continue;
			const std::string stem(name.substr(0, name.find('.')));
			auto it = std::find_if(stems.begin(), stems.end(), [&stem](const auto& p) { return p.first == stem; });
			if (it == stems.end())
				stems.emplace_back(stem, static_cast<s64>(fd.ModificationTime));
			else
				it->second = std::max(it->second, static_cast<s64>(fd.ModificationTime));
		}

		// The current identity always stays, then the most recently used others.
		std::sort(stems.begin(), stems.end(), [&current_stem](const auto& a, const auto& b) {
			if ((a.first == current_stem) != (b.first == current_stem))
				return a.first == current_stem;
			return a.second > b.second;
		});
		u32 removed = 0;
		for (size_t i = keep; i < stems.size(); i++)
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				const std::string_view name = Path::GetFileName(fd.FileName);
				if (name.substr(0, name.find('.')) == stems[i].first)
				{
					Console.WriteLn("GS cache: removing '%s', left by another build or driver", fd.FileName.c_str());
					removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
				}
			}
		}
		return removed;
	}
} // namespace GSCacheFile
