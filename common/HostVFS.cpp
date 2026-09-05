// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/HostVFS.h"
#include "common/Console.h"

#include <cstring>
#include <mutex>
#include <vector>

// The libretro VFS access flags, kept here rather than pulled from libretro.h:
// common/ does not depend on the core.
enum : unsigned
{
	VFS_ACCESS_READ = (1 << 0),
	VFS_ACCESS_WRITE = (1 << 1),
	VFS_ACCESS_READ_WRITE = (VFS_ACCESS_READ | VFS_ACCESS_WRITE),
	VFS_ACCESS_UPDATE_EXISTING = (1 << 2),
};

namespace
{
	HostVFS::Ops s_ops = {};
	bool s_installed = false;
} // namespace

void HostVFS::Install(const Ops& ops)
{
	s_ops = ops;
	s_installed = (ops.open != nullptr && ops.close != nullptr && ops.read != nullptr);
	if (!s_installed)
		Console.Warning("HostVFS: refusing an interface without open/close/read");
}

bool HostVFS::IsInstalled()
{
	return s_installed;
}

const HostVFS::Ops* HostVFS::GetOps()
{
	return s_installed ? &s_ops : nullptr;
}

bool HostVFS::StatPath(const char* path, bool* is_directory, s64* approx_size)
{
	if (!s_installed || !s_ops.stat)
		return false;

	s32 truncated_size = 0;
	const int flags = s_ops.stat(path, &truncated_size);
	if (!(flags & STAT_IS_VALID))
		return false;

	const bool directory = (flags & STAT_IS_DIRECTORY) != 0;
	if (is_directory)
		*is_directory = directory;
	// Wraps at 2 GB, and named so the caller has to notice. See the header.
	if (approx_size)
		*approx_size = directory ? 0 : static_cast<s64>(truncated_size);
	return true;
}

s64 HostVFS::ExactSizeOfPath(const char* path)
{
	if (!s_installed || !s_ops.size || !s_ops.open || !s_ops.close)
		return -1;

	void* handle = s_ops.open(path, VFS_ACCESS_READ, 0 /* RETRO_VFS_FILE_ACCESS_HINT_NONE */);
	if (!handle)
		return -1;

	const s64 size = s_ops.size(handle);
	s_ops.close(handle);
	return (size < 0) ? -1 : size;
}

namespace
{
	// A std::FILE* has to come out of this, because every caller in the tree
	// takes one. Both of the two ways to build one from callbacks are here:
	// fopencookie on glibc, funopen elsewhere (bionic, the BSDs, macOS).
#if defined(__GLIBC__) || defined(__BIONIC__) || defined(__APPLE__) || defined(__FreeBSD__)
#define HOSTVFS_HAVE_FILE_WRAPPER 1
#endif

#if defined(HOSTVFS_HAVE_FILE_WRAPPER)

	unsigned TranslateMode(const char* mode, bool* append)
	{
		*append = false;

		const bool plus = (std::strchr(mode, '+') != nullptr);
		switch (mode[0])
		{
			case 'r':
				// r+ must not truncate, and must fail when the file is absent:
				// that is what UPDATE_EXISTING means to the frontend.
				return plus ? (VFS_ACCESS_READ_WRITE | VFS_ACCESS_UPDATE_EXISTING) : VFS_ACCESS_READ;

			case 'w':
				return plus ? VFS_ACCESS_READ_WRITE : VFS_ACCESS_WRITE;

			case 'a':
				// No append flag in the interface; open for update and seek to
				// the end once, which is what append means for our callers.
				*append = true;
				return plus ? (VFS_ACCESS_READ_WRITE | VFS_ACCESS_UPDATE_EXISTING) :
							  (VFS_ACCESS_WRITE | VFS_ACCESS_UPDATE_EXISTING);

			default:
				return VFS_ACCESS_READ;
		}
	}

	// The wrapped streams. A cookie stream has no file descriptor - fileno()
	// on one returns -1 - so the few callers in FileSystem that would fstat()
	// a std::FILE* have to ask the host about the handle behind it instead,
	// and that is the only thing this maps. A handful of entries at most, and
	// only touched on open and close.
	std::mutex s_streams_lock;
	std::vector<std::pair<std::FILE*, void*>> s_streams;

	void RememberStream(std::FILE* fp, void* handle)
	{
		std::unique_lock lock(s_streams_lock);
		s_streams.emplace_back(fp, handle);
	}

	void ForgetStream(void* handle)
	{
		std::unique_lock lock(s_streams_lock);
		for (auto it = s_streams.begin(); it != s_streams.end(); ++it)
		{
			if (it->second == handle)
			{
				s_streams.erase(it);
				return;
			}
		}
	}

	int CookieClose(void* cookie)
	{
		ForgetStream(cookie);
		const HostVFS::Ops* ops = HostVFS::GetOps();
		return (ops && ops->close) ? ops->close(cookie) : 0;
	}

#if defined(__GLIBC__)

	ssize_t CookieRead(void* cookie, char* buffer, size_t size)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		const s64 res = ops->read(cookie, buffer, static_cast<u64>(size));
		// A short read is not an error, but a negative one is, and stdio wants
		// -1 rather than whatever the frontend returned.
		return (res < 0) ? -1 : static_cast<ssize_t>(res);
	}

	ssize_t CookieWrite(void* cookie, const char* buffer, size_t size)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		if (!ops->write)
			return -1;
		const s64 res = ops->write(cookie, buffer, static_cast<u64>(size));
		return (res < 0) ? -1 : static_cast<ssize_t>(res);
	}

	int CookieSeek(void* cookie, off64_t* offset, int whence)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		if (ops->seek(cookie, static_cast<s64>(*offset), whence) < 0)
			return -1;

		// RetroArch's seek() returns 0 on success rather than the new position,
		// so the position has to be read back - reporting 0 here would make
		// stdio believe every seek landed at the start of the file.
		const s64 pos = ops->tell ? ops->tell(cookie) : -1;
		if (pos < 0)
			return -1;

		*offset = static_cast<off64_t>(pos);
		return 0;
	}

	std::FILE* WrapHandle(void* handle, const char* mode)
	{
		const cookie_io_functions_t funcs = {CookieRead, CookieWrite, CookieSeek, CookieClose};
		return fopencookie(handle, mode, funcs);
	}

#else // funopen flavour

	int CookieReadF(void* cookie, char* buffer, int size)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		const s64 res = ops->read(cookie, buffer, static_cast<u64>(size));
		return (res < 0) ? -1 : static_cast<int>(res);
	}

	int CookieWriteF(void* cookie, const char* buffer, int size)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		if (!ops->write)
			return -1;
		const s64 res = ops->write(cookie, buffer, static_cast<u64>(size));
		return (res < 0) ? -1 : static_cast<int>(res);
	}

	// bionic has funopen64 and 64-bit off64_t callbacks; Apple and the BSDs
	// have only funopen, whose callbacks take fpos_t (64-bit on both).
#if defined(__BIONIC__)
	using FunopenOffset = off64_t;
#else
	using FunopenOffset = fpos_t;
#endif

	FunopenOffset CookieSeekF(void* cookie, FunopenOffset offset, int whence)
	{
		const HostVFS::Ops* ops = HostVFS::GetOps();
		if (ops->seek(cookie, static_cast<s64>(offset), whence) < 0)
			return -1;

		// Same as the glibc path: the position comes from tell(), not from the
		// return value of seek().
		const s64 pos = ops->tell ? ops->tell(cookie) : -1;
		return (pos < 0) ? -1 : static_cast<FunopenOffset>(pos);
	}

	std::FILE* WrapHandle(void* handle, const char* mode)
	{
		const bool writable = (std::strchr(mode, 'w') || std::strchr(mode, 'a') || std::strchr(mode, '+'));
#if defined(__BIONIC__)
		return funopen64(handle, CookieReadF, writable ? CookieWriteF : nullptr, CookieSeekF, CookieClose);
#else
		return funopen(handle, CookieReadF, writable ? CookieWriteF : nullptr, CookieSeekF, CookieClose);
#endif
	}

#endif // glibc / funopen

#endif // HOSTVFS_HAVE_FILE_WRAPPER
} // namespace

std::FILE* HostVFS::OpenAsCFile(const char* path, const char* mode)
{
#if defined(HOSTVFS_HAVE_FILE_WRAPPER)
	if (!s_installed)
		return nullptr;

	bool append = false;
	const unsigned access = TranslateMode(mode, &append);

	void* handle = s_ops.open(path, access, 0 /* RETRO_VFS_FILE_ACCESS_HINT_NONE */);
	if (!handle)
		return nullptr;

	if (append && s_ops.seek)
		s_ops.seek(handle, 0, 2 /* SEEK_END */);

	std::FILE* fp = WrapHandle(handle, mode);
	if (!fp)
	{
		s_ops.close(handle);
		return nullptr;
	}

	RememberStream(fp, handle);
	return fp;
#else
	// No fopencookie and no funopen: nothing to build a std::FILE* out of, so
	// the caller falls back to the OS.
	return nullptr;
#endif
}

bool HostVFS::SizeOfCFile(std::FILE* fp, s64* size)
{
#if defined(HOSTVFS_HAVE_FILE_WRAPPER)
	if (!s_installed || !s_ops.size || !fp)
		return false;

	void* handle = nullptr;
	{
		std::unique_lock lock(s_streams_lock);
		for (const auto& [stream, stream_handle] : s_streams)
		{
			if (stream == fp)
			{
				handle = stream_handle;
				break;
			}
		}
	}

	if (!handle)
		return false;

	const s64 res = s_ops.size(handle);
	if (res < 0)
		return false;

	*size = res;
	return true;
#else
	return false;
#endif
}
