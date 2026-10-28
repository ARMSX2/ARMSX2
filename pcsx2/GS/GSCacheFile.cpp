// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCacheFile.h"

#include "BuildVersion.h"
#include "ShaderCacheVersion.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "common/HostVFS.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
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
			u32 param; // 0
			Digest stamp;
			u64 size; // framed: payload size; pack: 0
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


		static constexpr u32 PACK_MAGIC = 0x50435347; // "GSCP"
		static constexpr u32 PACK_RECORD_MAGIC = 0x52505347; // "GSPR"
		/// data_size of a record that erases its key.
		static constexpr u32 ERASED = 0xFFFFFFFFu;
		static constexpr u32 MAX_RECORD_DATA = 256u * 1024 * 1024;
		/// A pack this large and more than half garbage is written whole again.
		static constexpr u64 COMPACT_MIN_BYTES = 1024 * 1024;
		static constexpr size_t IO_BUFFER_SIZE = 1024 * 1024;

#pragma pack(push, 4)
		struct RecordHeader
		{
			u32 magic;
			u16 type;
			u16 key_size;
			u32 data_size;
			u32 aux;
			/// Of this header with the field zero, then the key and the payload.
			u64 checksum;
		};
#pragma pack(pop)
		static_assert(sizeof(RecordHeader) == 24);

		static u64 RecordChecksum(RecordHeader h, const void* payload, size_t size)
		{
			h.checksum = 0;
			return XXH3_64bits_withSeed(payload, size, Hash64(&h, sizeof(h)));
		}

		/// Open packs, so a clear can empty them in place and the cleanup pass never deletes one.
		static std::mutex s_registry_mutex;
		static std::vector<Pack*> s_registry;

		static std::string NormalizePath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			return path;
		}

		/// Callers hold s_registry_mutex.
		static bool IsOpenPackPath(const std::string& path)
		{
			const std::string p = NormalizePath(path);
			return std::any_of(s_registry.begin(), s_registry.end(),
				[&p](const Pack* pack) { return NormalizePath(pack->GetPath()) == p; });
		}

		/// The modification time is when a pack was last used, for the cleanup pass. A host file
		/// system (the libretro frontend's) cannot set it, and the pass leaves packs alone there.
		static void MarkUsed(const std::string& path)
		{
			if (HostVFS::IsInstalled())
				return;
			std::error_code ec;
			// (UTF-8, as FileSystem takes it: a plain std::string is the ANSI code page on Windows)
			const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
			std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec);
		}

		static std::string TempPathFor(const std::string& path)
		{
			// Named per process, so two processes writing the same cache cannot interleave in one file.
#ifdef _WIN32
			return fmt::format("{}.tmp{}", path, GetCurrentProcessId());
#else
			return fmt::format("{}.tmp{}", path, getpid());
#endif
		}

		static bool WriteChunked(std::FILE* fp, const u8* data, size_t size, Clock::time_point deadline)
		{
			static constexpr size_t CHUNK = 1024 * 1024;
			for (size_t pos = 0; pos < size; pos += CHUNK)
			{
				if (Clock::now() >= deadline)
					return false;
				const size_t n = std::min(CHUNK, size - pos);
				if (std::fwrite(data + pos, n, 1, fp) != 1)
					return false;
			}
			return true;
		}
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

	bool WriteFileAtomic(const std::string& path, const void* data, size_t size, Clock::time_point deadline)
	{
		const std::string tmp_path = TempPathFor(path);
		std::FILE* fp = FileSystem::OpenCFile(tmp_path.c_str(), "wb");
		if (!fp)
			return false;
		const bool written = WriteChunked(fp, static_cast<const u8*>(data), size, deadline);
		if (std::fclose(fp) != 0 || !written || !FileSystem::RenamePath(tmp_path.c_str(), path.c_str()))
		{
			FileSystem::DeleteFilePath(tmp_path.c_str());
			return false;
		}
		return true;
	}

	bool WriteFramedFile(const std::string& path, Kind kind, const Stamp& stamp, const void* data, size_t size,
		Clock::time_point deadline)
	{
		const FileHeader h = MakeHeader(FRAMED_MAGIC, kind, 0, stamp.GetDigest(), size, Hash64(data, size));
		std::vector<u8> out(sizeof(h) + size);
		std::memcpy(out.data(), &h, sizeof(h));
		if (size > 0)
			std::memcpy(out.data() + sizeof(h), data, size);
		return WriteFileAtomic(path, out.data(), out.size(), deadline);
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

	std::string GetPackDirectory(const std::string& cache_dir, const std::string& serial, u32 crc)
	{
		const std::string title = serial.empty() ? std::string("_") : Path::SanitizeFileName(fmt::format("{}_{:08X}", serial, crc));
		return Path::Combine(Path::Combine(cache_dir, "gs_packs"), title);
	}

	u64 GetDefaultPackRamCap()
	{
		static constexpr u64 MIN_CAP = 32ull * 1024 * 1024;
		return std::max<u64>(GetPhysicalMemory() / 32, MIN_CAP);
	}

	Pack::Pack() = default;

	Pack::~Pack()
	{
		Close();
	}

	std::string Pack::MapKey(u16 type, const void* key, u32 key_size)
	{
		std::string k(sizeof(type) + key_size, '\0');
		std::memcpy(k.data(), &type, sizeof(type));
		if (key_size > 0)
			std::memcpy(k.data() + sizeof(type), key, key_size);
		return k;
	}

	void Pack::Open(const std::string& path, Kind kind, const Stamp& stamp, u64 ram_cap)
	{
		Close();
		{
			std::unique_lock io(m_io_mutex);
			std::unique_lock lock(m_mutex);
			m_path = path;
			m_kind = kind;
			m_stamp = stamp.GetDigest();
			m_ram_cap = ram_cap;
			Load();
			m_open = true;
		}
		std::unique_lock reg(s_registry_mutex);
		s_registry.push_back(this);
	}

	void Pack::Close()
	{
		{
			std::unique_lock reg(s_registry_mutex);
			s_registry.erase(std::remove(s_registry.begin(), s_registry.end(), this), s_registry.end());
		}
		std::unique_lock io(m_io_mutex);
		std::unique_lock lock(m_mutex);
		m_open = false;
		m_load_info = {};
		m_entries.clear();
		m_next_seq = 0;
		m_ram_bytes = 0;
		m_file_bytes = 0;
		m_live_bytes = 0;
		m_replace = false;
		m_partial = false;
		m_torn = false;
		m_write_failed = false;
	}

	void Pack::Load()
	{
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		m_load_info = {};

		std::FILE* fp = FileSystem::OpenCFile(m_path.c_str(), "rb");
		if (!fp)
		{
			m_load_info.result = ReadResult::Missing;
			return;
		}
		// Record by record through a large buffer: one pass over the file, never held twice in RAM.
		std::vector<char> io_buffer(IO_BUFFER_SIZE);
		std::setvbuf(fp, io_buffer.data(), _IOFBF, io_buffer.size());
		const s64 size_signed = FileSystem::FSize64(fp);
		const u64 file_size = (size_signed > 0) ? static_cast<u64>(size_signed) : 0;

		FileHeader h;
		ReadResult res = ReadResult::Truncated;
		if (file_size >= sizeof(h) && std::fread(&h, sizeof(h), 1, fp) == 1)
			res = CheckHeader(h, PACK_MAGIC, m_kind, 0, m_stamp);
		if (res != ReadResult::Ok)
		{
			std::fclose(fp);
			m_load_info.result = res;
			m_replace = true;
			Console.WriteLn("GS cache: not using '%s': %s", m_path.c_str(), ReadResultString(res));
			return;
		}

		u64 pos = sizeof(h);
		std::vector<u8> payload;
		while (file_size - pos >= sizeof(RecordHeader))
		{
			RecordHeader rh;
			if (std::fread(&rh, sizeof(rh), 1, fp) != 1 || rh.magic != PACK_RECORD_MAGIC)
				break;
			const bool erase = (rh.data_size == ERASED);
			if (!erase && rh.data_size > MAX_RECORD_DATA)
				break;
			const u64 payload_size = rh.key_size + (erase ? 0u : rh.data_size);
			if (payload_size > file_size - pos - sizeof(rh))
				break;
			payload.resize(static_cast<size_t>(payload_size));
			if ((payload_size > 0 && std::fread(payload.data(), payload.size(), 1, fp) != 1) ||
				RecordChecksum(rh, payload.data(), payload.size()) != rh.checksum)
			{
				break;
			}

			const u64 record_bytes = sizeof(rh) + payload_size;
			const std::string map_key = MapKey(rh.type, payload.data(), rh.key_size);
			auto it = m_entries.find(map_key);
			const auto drop = [this](decltype(it) where) {
				m_live_bytes -= where->second.disk_bytes;
				if (where->second.data)
					m_ram_bytes -= where->second.data->size();
				m_entries.erase(where);
			};
			if (erase)
			{
				if (it != m_entries.end())
					drop(it);
			}
			else if (m_partial || m_ram_bytes + rh.data_size > m_ram_cap)
			{
				// The newest record of this key stays on disk, so an older one must not be served.
				m_partial = true;
				m_load_info.ram_cap_hit = true;
				if (it != m_entries.end())
					drop(it);
			}
			else
			{
				if (it == m_entries.end())
				{
					it = m_entries.emplace(map_key, Entry()).first;
					it->second.seq = m_next_seq++;
				}
				else
				{
					m_live_bytes -= it->second.disk_bytes;
					m_ram_bytes -= it->second.data->size();
				}
				Entry& e = it->second;
				e.data = std::make_shared<const std::vector<u8>>(payload.begin() + rh.key_size, payload.end());
				e.aux = rh.aux;
				e.gen = e.written_gen = 1;
				e.disk_bytes = record_bytes;
				m_live_bytes += record_bytes;
				m_ram_bytes += rh.data_size;
			}
			pos += record_bytes;
			m_load_info.records++;
		}
		std::fclose(fp);

		m_file_bytes = pos;
		m_load_info.result = ReadResult::Ok;
		m_load_info.bytes = file_size;
		m_load_info.torn_bytes = file_size - pos;
		m_load_info.ns = static_cast<u64>(Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - start));
		if (m_load_info.torn_bytes > 0)
		{
			m_torn = true;
			// Written whole when it can be, which cuts the tail without truncating (a host file
			// system cannot); a pack only partly in RAM is cut in place instead.
			m_replace = !m_partial;
			Console.Warning("GS cache: '%s' ends in %llu damaged or incomplete bytes; cutting them off", m_path.c_str(),
				static_cast<unsigned long long>(m_load_info.torn_bytes));
		}
		Console.WriteLn("GS cache: read %zu entries (%llu KB) from '%s' in %.1f ms%s", m_entries.size(),
			static_cast<unsigned long long>(file_size / 1024), m_path.c_str(), static_cast<double>(m_load_info.ns) / 1e6,
			m_partial ? ", stopped at the RAM cap" : "");
		MarkUsed(m_path);
	}

	bool Pack::IsOpen() const
	{
		std::unique_lock lock(m_mutex);
		return m_open;
	}

	std::string Pack::GetPath() const
	{
		std::unique_lock lock(m_mutex);
		return m_path;
	}

	Pack::LoadInfo Pack::GetLoadInfo() const
	{
		std::unique_lock lock(m_mutex);
		return m_load_info;
	}

	size_t Pack::GetEntryCount() const
	{
		std::unique_lock lock(m_mutex);
		return static_cast<size_t>(std::count_if(
			m_entries.begin(), m_entries.end(), [](const auto& it) { return !it.second.erased; }));
	}

	u64 Pack::GetRamBytes() const
	{
		std::unique_lock lock(m_mutex);
		return m_ram_bytes;
	}

	u64 Pack::GetFileBytes() const
	{
		std::unique_lock lock(m_mutex);
		return m_file_bytes;
	}

	bool Pack::Get(u16 type, const void* key, u32 key_size, std::vector<u8>* data, u32* aux) const
	{
		std::unique_lock lock(m_mutex);
		const auto it = m_entries.find(MapKey(type, key, key_size));
		if (it == m_entries.end() || it->second.erased)
			return false;
		data->assign(it->second.data->begin(), it->second.data->end());
		if (aux)
			*aux = it->second.aux;
		return true;
	}

	bool Pack::Put(u16 type, const void* key, u32 key_size, const void* data, size_t size, u32 aux, bool replace)
	{
		pxAssert(key_size <= 0xFFFF && size < MAX_RECORD_DATA);
		std::unique_lock lock(m_mutex);
		if (!m_open)
			return false;
		const std::string map_key = MapKey(type, key, key_size);
		auto it = m_entries.find(map_key);
		if (it != m_entries.end() && !it->second.erased)
		{
			const std::vector<u8>& old = *it->second.data;
			if (!replace || (it->second.aux == aux && old.size() == size && (size == 0 || std::memcmp(old.data(), data, size) == 0)))
				return false;
			m_ram_bytes -= old.size();
		}
		if (it == m_entries.end())
		{
			it = m_entries.emplace(map_key, Entry()).first;
			it->second.seq = m_next_seq++;
		}
		Entry& e = it->second;
		const u8* bytes = static_cast<const u8*>(data);
		e.data = std::make_shared<const std::vector<u8>>(bytes, bytes + size);
		e.aux = aux;
		e.erased = false;
		e.gen++;
		m_ram_bytes += size;
		return true;
	}

	void Pack::Erase(u16 type, const void* key, u32 key_size)
	{
		std::unique_lock lock(m_mutex);
		const auto it = m_entries.find(MapKey(type, key, key_size));
		if (it == m_entries.end() || it->second.erased)
			return;
		// Kept as a pending erase until Write puts it on disk: a record of the key may be there.
		m_ram_bytes -= it->second.data->size();
		it->second.data.reset();
		it->second.erased = true;
		it->second.gen++;
	}

	void Pack::ForEach(u16 type, const Visitor& fn) const
	{
		std::unique_lock lock(m_mutex);
		std::vector<const std::pair<const std::string, Entry>*> found;
		for (const auto& it : m_entries)
		{
			u16 t;
			std::memcpy(&t, it.first.data(), sizeof(t));
			if (t == type && !it.second.erased)
				found.push_back(&it);
		}
		std::sort(found.begin(), found.end(), [](const auto* a, const auto* b) { return a->second.seq < b->second.seq; });
		for (const auto* it : found)
		{
			fn(reinterpret_cast<const u8*>(it->first.data()) + sizeof(u16), static_cast<u32>(it->first.size() - sizeof(u16)),
				*it->second.data, it->second.aux);
		}
	}

	void Pack::Clear()
	{
		std::unique_lock lock(m_mutex);
		m_entries.clear();
		m_ram_bytes = 0;
		m_live_bytes = 0;
		m_replace = true;
		m_partial = false;
		m_epoch++;
	}

	bool Pack::HasPendingWrites() const
	{
		std::unique_lock lock(m_mutex);
		if (!m_open || m_write_failed)
			return false;
		return m_replace || m_torn || std::any_of(m_entries.begin(), m_entries.end(),
			[](const auto& it) { return it.second.gen != it.second.written_gen; });
	}

	void Pack::WriteRecords(std::FILE* fp, const std::vector<Pending>& pending, bool whole, Clock::time_point deadline,
		WriteResult* res, std::vector<std::pair<size_t, u64>>* written)
	{
		std::vector<u8> rec;
		for (size_t i = 0; i < pending.size(); i++)
		{
			const Pending& p = pending[i];
			// A pack written whole holds no erases: the key is simply absent.
			if (whole && p.erased)
			{
				written->emplace_back(i, 0);
				continue;
			}
			if (Clock::now() >= deadline)
			{
				res->deadline_hit = true;
				return;
			}

			RecordHeader rh = {};
			rh.magic = PACK_RECORD_MAGIC;
			std::memcpy(&rh.type, p.map_key.data(), sizeof(rh.type));
			rh.key_size = static_cast<u16>(p.map_key.size() - sizeof(u16));
			rh.data_size = p.erased ? ERASED : static_cast<u32>(p.data->size());
			rh.aux = p.aux;
			const size_t data_size = p.erased ? 0 : p.data->size();
			rec.resize(sizeof(rh) + rh.key_size + data_size);
			std::memcpy(rec.data() + sizeof(rh), p.map_key.data() + sizeof(u16), rh.key_size);
			if (data_size > 0)
				std::memcpy(rec.data() + sizeof(rh) + rh.key_size, p.data->data(), data_size);
			rh.checksum = RecordChecksum(rh, rec.data() + sizeof(rh), rec.size() - sizeof(rh));
			std::memcpy(rec.data(), &rh, sizeof(rh));

			if (std::fwrite(rec.data(), rec.size(), 1, fp) != 1)
			{
				res->failed = true;
				return;
			}
			written->emplace_back(i, rec.size());
			res->records++;
			res->bytes += rec.size();
		}
	}

	Pack::WriteResult Pack::Write(Clock::time_point deadline)
	{
		std::unique_lock io(m_io_mutex);
		WriteResult res;

		const bool exists = FileSystem::FileExists(m_path.c_str());
		bool whole;
		bool torn;
		u64 good_end;
		u64 epoch;
		std::vector<Pending> pending;
		{
			std::unique_lock lock(m_mutex);
			if (!m_open || m_write_failed)
				return res;
			// A compaction is skipped in a pass with a deadline (a close): appends are what must land.
			const bool compact = !m_partial && m_file_bytes >= COMPACT_MIN_BYTES && m_file_bytes > 2 * m_live_bytes &&
								 deadline == Clock::time_point::max();
			// The file is gone (deleted from outside) while the table still holds what was in it.
			const bool lost = !exists && m_file_bytes > 0 && !m_partial;
			whole = m_replace || compact || lost;
			torn = m_torn && !whole;
			good_end = m_file_bytes;
			epoch = m_epoch;
			for (const auto& [key, e] : m_entries)
			{
				if (whole || e.gen != e.written_gen)
					pending.push_back({key, e.data, e.aux, e.erased, e.gen, e.seq});
			}
			if (!whole && !torn && pending.empty())
				return res;
		}
		std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.seq < b.seq; });

		const std::string tmp_path = TempPathFor(m_path);
		std::FILE* fp = exists ? FileSystem::OpenCFile(m_path.c_str(), "r+b") : nullptr;
		if (fp)
		{
			std::string why;
			if (TryLockFile(fp, &why) == LockResult::Contended)
			{
				std::fclose(fp);
				res.contended = true;
				return res;
			}
		}
		if (whole)
		{
			// The lock is let go before the rename: Windows will not rename over an open file.
			if (fp)
				std::fclose(fp);
			fp = FileSystem::OpenCFile(tmp_path.c_str(), "wb");
		}
		else if (!exists)
		{
			fp = FileSystem::OpenCFile(m_path.c_str(), "w+b");
		}
		std::vector<char> io_buffer(IO_BUFFER_SIZE);
		bool ok = (fp != nullptr);
		if (ok)
		{
			std::setvbuf(fp, io_buffer.data(), _IOFBF, io_buffer.size());
			if (torn && !TruncateFile(fp, good_end))
				ok = false;
		}
		if (ok && FileSystem::FSeek64(fp, 0, SEEK_END) == 0)
		{
			const s64 end = FileSystem::FTell64(fp);
			if (end == 0)
			{
				const FileHeader h = MakeHeader(PACK_MAGIC, m_kind, 0, m_stamp, 0, 0);
				ok = (std::fwrite(&h, sizeof(h), 1, fp) == 1);
				res.bytes += sizeof(h);
			}
			else if (end < 0)
			{
				ok = false;
			}
		}
		else
		{
			ok = false;
		}

		std::vector<std::pair<size_t, u64>> written;
		if (ok)
		{
			WriteRecords(fp, pending, whole, deadline, &res, &written);
			ok = !res.failed;
		}
		if (fp && std::fclose(fp) != 0)
			ok = false;
		if (whole)
		{
			res.rewrote = ok && !res.deadline_hit && FileSystem::RenamePath(tmp_path.c_str(), m_path.c_str());
			if (!res.rewrote)
			{
				// All or nothing: the pack on disk stays as it was. Running out of time is not a failure.
				FileSystem::DeleteFilePath(tmp_path.c_str());
				ok = ok && res.deadline_hit;
				written.clear();
				res.records = 0;
				res.bytes = 0;
			}
		}
		res.failed = !ok;

		std::unique_lock lock(m_mutex);
		if (!ok)
		{
			// Usually a full disk. A torn append is cut by the next start; another attempt would most
			// likely fail the same way.
			m_write_failed = true;
			Console.Error("GS cache: failed to write '%s'; not writing it again this session", m_path.c_str());
			return res;
		}
		if (m_epoch != epoch)
		{
			// Cleared while this ran: what was written belongs to the old contents.
			m_replace = true;
			return res;
		}
		if (res.rewrote)
		{
			m_file_bytes = 0;
			m_live_bytes = 0;
			m_replace = false;
			m_torn = false;
		}
		else if (whole)
		{
			// Abandoned at the deadline.
			return res;
		}
		if (torn)
		{
			m_torn = false;
			m_file_bytes = good_end;
		}
		m_file_bytes += res.bytes;
		for (const auto& [index, record_bytes] : written)
		{
			const Pending& p = pending[index];
			const auto it = m_entries.find(p.map_key);
			if (it == m_entries.end())
				continue;
			Entry& e = it->second;
			if (!res.rewrote)
				m_live_bytes -= e.disk_bytes;
			e.disk_bytes = p.erased ? 0 : record_bytes;
			m_live_bytes += e.disk_bytes;
			if (e.gen == p.gen)
			{
				e.written_gen = e.gen;
				if (e.erased)
					m_entries.erase(it);
			}
		}
		return res;
	}

	std::shared_ptr<Pack> OpenPack(const std::string& dir, std::string_view prefix, const std::string& stem, Kind kind,
		const Stamp& stamp, u64 ram_cap)
	{
		static constexpr u32 KEEP_IDENTITIES = 3;
		if (FileSystem::EnsureDirectoryExists(dir.c_str(), true))
			PruneOtherIdentities(dir, prefix, stem, KEEP_IDENTITIES);
		std::shared_ptr<Pack> pack = std::make_shared<Pack>();
		pack->Open(Path::Combine(dir, stem + ".pack"), kind, stamp, ram_cap);
		return pack;
	}

	PackWriter::PackWriter() = default;

	PackWriter::~PackWriter()
	{
		Stop(Clock::now() + CLOSE_DEADLINE);
	}

	void PackWriter::Start(const std::string& cache_dir, Extra extra, Clock::duration interval)
	{
		std::unique_lock lock(m_mutex);
		if (m_running)
			return;
		m_cache_dir = cache_dir;
		m_extra = std::move(extra);
		m_interval = interval;
		m_stop = false;
		m_running = true;
		m_thread = std::thread(&PackWriter::Run, this);
	}

	void PackWriter::Stop(Clock::time_point deadline)
	{
		{
			std::unique_lock lock(m_mutex);
			if (!m_running)
				return;
			m_stop = true;
			m_stop_deadline = deadline;
			m_cv.notify_all();
		}
		m_thread.join();
		std::unique_lock lock(m_mutex);
		m_running = false;
		m_released.clear();
		m_extra = {};
		m_done_cv.notify_all();
	}

	bool PackWriter::IsRunning() const
	{
		std::unique_lock lock(m_mutex);
		return m_running;
	}

	void PackWriter::Add(std::shared_ptr<Pack> pack)
	{
		std::unique_lock lock(m_mutex);
		if (std::find(m_packs.begin(), m_packs.end(), pack) == m_packs.end())
			m_packs.push_back(std::move(pack));
	}

	void PackWriter::Release(std::shared_ptr<Pack> pack)
	{
		if (!pack)
			return;
		std::unique_lock lock(m_mutex);
		m_packs.erase(std::remove(m_packs.begin(), m_packs.end(), pack), m_packs.end());
		if (m_running)
			m_released.push_back(std::move(pack));
	}

	void PackWriter::Flush(bool wait)
	{
		std::unique_lock lock(m_mutex);
		if (!m_running)
			return;
		const u64 ticket = ++m_asked;
		m_cv.notify_all();
		if (wait)
			m_done_cv.wait(lock, [this, ticket]() { return m_finished >= ticket || !m_running; });
	}

	void PackWriter::Run()
	{
		Threading::SetNameOfCurrentThread("GS cache writer");
		Threading::ThreadHandle::GetForCallingThread().SetNicePriority(5);

		static std::once_flag s_leftovers_once;
		std::call_once(s_leftovers_once, [this]() { CleanLeftovers(m_cache_dir); });

		std::unique_lock lock(m_mutex);
		for (;;)
		{
			const Clock::time_point next = Clock::now() + m_interval;
			m_cv.wait_until(lock, next, [this]() { return m_stop || m_asked > m_finished; });
			if (m_stop)
				break;
			const u64 ticket = m_asked;
			const bool asked = (ticket > m_finished);
			lock.unlock();
			Pass(Clock::time_point::max(), asked);
			lock.lock();
			m_finished = ticket;
			m_done_cv.notify_all();
		}
		const Clock::time_point deadline = m_stop_deadline;
		const u64 ticket = m_asked;
		lock.unlock();
		Pass(deadline, true);
		lock.lock();
		m_finished = ticket;
		m_done_cv.notify_all();
	}

	void PackWriter::Pass(Clock::time_point deadline, bool asked)
	{
		std::vector<std::shared_ptr<Pack>> packs;
		{
			std::unique_lock lock(m_mutex);
			packs = m_packs;
			packs.insert(packs.end(), m_released.begin(), m_released.end());
			m_released.clear();
		}
		for (const std::shared_ptr<Pack>& pack : packs)
		{
			if (!pack->HasPendingWrites())
				continue;
			const Pack::WriteResult res = pack->Write(deadline);
			if (res.records > 0 || res.deadline_hit)
			{
				DevCon.WriteLn("GS cache: wrote %u records (%llu KB) to '%s'%s%s", res.records,
					static_cast<unsigned long long>(res.bytes / 1024), pack->GetPath().c_str(),
					res.rewrote ? ", whole" : "", res.deadline_hit ? ", stopped at the deadline" : "");
			}
		}
		if (m_extra)
			m_extra(deadline, asked);
	}

	u32 CleanLeftovers(const std::string& cache_dir)
	{
		static constexpr s64 DAY = 24 * 60 * 60;
		static constexpr s64 LEGACY_AGE = 7 * DAY;
		static constexpr s64 UNUSED_PACK_AGE = 60 * DAY;
		const s64 now = static_cast<s64>(std::time(nullptr));
		// A host file system may not report modification times at all.
		const auto older_than = [now](const FILESYSTEM_FIND_DATA& fd, s64 age) {
			return fd.ModificationTime > 0 && now - static_cast<s64>(fd.ModificationTime) > age;
		};

		u32 removed = 0;
		FileSystem::FindResultsArray files;
		if (FileSystem::FindFiles(cache_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				const std::string_view name = Path::GetFileName(fd.FileName);
				if ((name.compare(0, 14, "vulkan_shaders") == 0 || name.compare(0, 11, "gl_programs") == 0) &&
					older_than(fd, LEGACY_AGE))
				{
					removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
				}
			}
		}
		const std::string keys_dir = Path::Combine(cache_dir, "vulkan_pipeline_keys");
		FileSystem::FindResultsArray key_files;
		if (FileSystem::FindFiles(keys_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &key_files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : key_files)
			{
				if (older_than(fd, LEGACY_AGE))
					removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
			}
			FileSystem::DeleteDirectory(keys_dir.c_str());
		}

		const std::string packs_dir = Path::Combine(cache_dir, "gs_packs");
		FileSystem::FindResultsArray titles;
		if (!FileSystem::FindFiles(packs_dir.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &titles))
			return removed;
		for (const FILESYSTEM_FIND_DATA& title : titles)
		{
			removed += CleanStaleTempFiles(title.FileName);
			FileSystem::FindResultsArray packs;
			if (FileSystem::FindFiles(title.FileName.c_str(), "*.pack", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &packs))
			{
				std::unique_lock reg(s_registry_mutex);
				for (const FILESYSTEM_FIND_DATA& fd : packs)
				{
					if (older_than(fd, UNUSED_PACK_AGE) && !IsOpenPackPath(fd.FileName))
					{
						Console.WriteLn("GS cache: removing '%s', unused for 60 days", fd.FileName.c_str());
						removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
					}
				}
			}
			// Only goes when empty.
			FileSystem::DeleteDirectory(title.FileName.c_str());
		}
		return removed;
	}

	u32 DeleteAll(const std::string& cache_dir)
	{
		// The shared stores and key lists of older builds, and the caches that are still one file each.
		static constexpr const char* prefixes[] = {
			"vulkan_shaders", "vulkan_pipelines", "gl_programs", "d3d_shaders_", "d3d12_", "lsfg_spirv"};

		u32 removed = 0;
		FileSystem::FindResultsArray files;
		if (FileSystem::FindFiles(cache_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				const std::string_view name = Path::GetFileName(fd.FileName);
				const bool is_cache = std::any_of(std::begin(prefixes), std::end(prefixes),
					[&name](const char* prefix) { return name.compare(0, std::strlen(prefix), prefix) == 0; });
				if (is_cache)
					removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
			}
		}

		const std::string keys_dir = Path::Combine(cache_dir, "vulkan_pipeline_keys");
		FileSystem::FindResultsArray key_files;
		if (FileSystem::FindFiles(keys_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &key_files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : key_files)
				removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
		}
		FileSystem::DeleteDirectory(keys_dir.c_str());

		const std::string packs_dir = Path::Combine(cache_dir, "gs_packs");
		std::unique_lock reg(s_registry_mutex);
		FileSystem::FindResultsArray pack_files;
		if (FileSystem::FindFiles(packs_dir.c_str(), "*",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &pack_files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : pack_files)
			{
				// An open pack is emptied in place below, so its owner never writes to a deleted file.
				if (!IsOpenPackPath(fd.FileName))
					removed += FileSystem::DeleteFilePath(fd.FileName.c_str()) ? 1 : 0;
			}
		}
		const std::string packs_prefix = NormalizePath(packs_dir) + "/";
		for (Pack* pack : s_registry)
		{
			if (NormalizePath(pack->GetPath()).compare(0, packs_prefix.size(), packs_prefix) != 0)
				continue;
			pack->Clear();
			removed += pack->Write().failed ? 0 : 1;
		}
		FileSystem::FindResultsArray titles;
		if (FileSystem::FindFiles(packs_dir.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &titles))
		{
			for (const FILESYSTEM_FIND_DATA& title : titles)
				FileSystem::DeleteDirectory(title.FileName.c_str());
		}

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
