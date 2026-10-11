// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <optional>
#include <string>
#include <string_view>

class Error;

namespace Catalog
{
	/// Replaces `path` with the given bytes so that a reader, or a crash or power cut, sees either
	/// the old file or the complete new one, never a half-written one. The data goes to a uniquely
	/// named sibling file, is flushed to disk, then renamed over `path`. Missing parent
	/// directories are created. On failure the temporary file is removed, `path` is untouched and
	/// `error` (if given) says why.
	bool WriteFileAtomic(const std::string& path, const void* data, size_t size, Error* error = nullptr);

	inline bool WriteFileAtomic(const std::string& path, std::string_view data, Error* error = nullptr)
	{
		return WriteFileAtomic(path, data.data(), data.size(), error);
	}

	/// Bytes available to this user on the filesystem that holds `path`, or nullopt when the system
	/// cannot say (the path does not exist, a filesystem that reports nothing).
	std::optional<u64> FreeSpaceOn(const std::string& path);
} // namespace Catalog
