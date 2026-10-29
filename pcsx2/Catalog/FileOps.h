// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/FileSystem.h"

#include <string>

namespace Catalog
{
	/// The few file operations that multi-step, undoable changes on disk are built from (rotating
	/// autosaves, swapping a restored memory card into place). They sit behind an interface so a
	/// test can make step N fail and check that everything was put back.
	class FileOps
	{
	public:
		virtual ~FileOps() = default;

		/// True for a file or a directory.
		virtual bool Exists(const std::string& path) = 0;
		/// Moves `from` to `to`, replacing a file at `to`. Works on directories too, but a directory
		/// can only replace an empty one.
		virtual bool Rename(const std::string& from, const std::string& to) = 0;
		/// Deletes a file, or a directory with everything in it. A missing path counts as done.
		virtual bool Remove(const std::string& path) = 0;
	};

	class RealFileOps final : public FileOps
	{
	public:
		bool Exists(const std::string& path) override
		{
			return FileSystem::FileExists(path.c_str()) || FileSystem::DirectoryExists(path.c_str());
		}

		bool Rename(const std::string& from, const std::string& to) override
		{
			return FileSystem::RenamePath(from.c_str(), to.c_str());
		}

		bool Remove(const std::string& path) override
		{
			if (FileSystem::DirectoryExists(path.c_str()))
				return FileSystem::RecursiveDeleteDirectory(path.c_str());
			if (FileSystem::FileExists(path.c_str()))
				return FileSystem::DeleteFilePath(path.c_str());
			return true;
		}
	};

	inline FileOps& GetRealFileOps()
	{
		static RealFileOps ops;
		return ops;
	}
} // namespace Catalog
