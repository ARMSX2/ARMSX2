// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/FileUtil.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <atomic>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Catalog
{
	static unsigned long CurrentProcessId()
	{
#ifdef _WIN32
		return GetCurrentProcessId();
#else
		return static_cast<unsigned long>(getpid());
#endif
	}

	static bool SyncToDisk(std::FILE* fp)
	{
#ifdef _WIN32
		return _commit(_fileno(fp)) == 0;
#else
		return fsync(fileno(fp)) == 0;
#endif
	}

	bool WriteFileAtomic(const std::string& path, const void* data, size_t size, Error* error)
	{
		const std::string directory(Path::GetDirectory(path));
		if (!directory.empty() && !FileSystem::EnsureDirectoryExists(directory.c_str(), true, error))
			return false;

		// Unique per process and per call, so two writers of the same file never share a temp file.
		static std::atomic<unsigned> counter{0};
		const std::string temp_path = fmt::format("{}.{}.{}.tmp", path, CurrentProcessId(), counter.fetch_add(1));

		std::FILE* fp = FileSystem::OpenCFile(temp_path.c_str(), "wb", error);
		if (!fp)
			return false;

		bool ok = (size == 0) || (std::fwrite(data, 1, size, fp) == size);
		ok = ok && (std::fflush(fp) == 0) && SyncToDisk(fp);
		ok = (std::fclose(fp) == 0) && ok;
		if (!ok)
		{
			Error::SetStringView(error, "Writing the file failed (disk full?).");
			FileSystem::DeleteFilePath(temp_path.c_str());
			return false;
		}

		if (!FileSystem::RenamePath(temp_path.c_str(), path.c_str(), error))
		{
			FileSystem::DeleteFilePath(temp_path.c_str());
			return false;
		}

		return true;
	}

	std::optional<u64> FreeSpaceOn(const std::string& path)
	{
		std::error_code ec;
		const std::filesystem::space_info info = std::filesystem::space(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())), ec);
		if (ec)
			return std::nullopt;
		return static_cast<u64>(info.available);
	}
} // namespace Catalog
