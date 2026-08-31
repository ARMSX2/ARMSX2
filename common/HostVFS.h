// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstdio>
#include <string>
#include <vector>

/// A file system supplied by the host application rather than by the OS.
///
/// The libretro core installs one of these when the frontend offers its VFS
/// interface: on Android that is the only way to reach content the app has no
/// path to (Storage Access Framework), and it is also what makes network
/// shares work. The table is plain function pointers so that common/ does not
/// have to know what libretro is - pcsx2-libretro/Main.cpp fills it in.
///
/// When nothing is installed every FileSystem call behaves exactly as before,
/// which is the case for the Qt and SDL frontends.
namespace HostVFS
{
	/// Mirrors the subset of the frontend's interface that FileSystem needs.
	/// Anything the frontend does not implement is left null, and the caller
	/// falls back to the OS - so an old frontend that offers files but not
	/// directory iteration still works for everything but browsing.
	struct Ops
	{
		void* (*open)(const char* path, unsigned mode, unsigned hints);
		int (*close)(void* handle);
		s64 (*size)(void* handle);
		s64 (*tell)(void* handle);
		s64 (*seek)(void* handle, s64 offset, int seek_position);
		s64 (*read)(void* handle, void* buffer, u64 length);
		s64 (*write)(void* handle, const void* buffer, u64 length);
		int (*flush)(void* handle);
		int (*remove)(const char* path);
		int (*rename)(const char* old_path, const char* new_path);
		int (*truncate)(void* handle, s64 length);

		/// Returns a bitmask: 1 = exists, 2 = directory, 4 = character device.
		/// size receives the file size, in bytes.
		int (*stat)(const char* path, s32* size);
		int (*mkdir)(const char* dir);

		void* (*opendir)(const char* dir, bool include_hidden);
		bool (*readdir)(void* dir_handle);
		const char* (*dirent_get_name)(void* dir_handle);
		bool (*dirent_is_dir)(void* dir_handle);
		int (*closedir)(void* dir_handle);
	};

	/// The frontend's stat() bits, as libretro defines them.
	enum : int
	{
		STAT_IS_VALID = (1 << 0),
		STAT_IS_DIRECTORY = (1 << 1),
		STAT_IS_CHARACTER_SPECIAL = (1 << 2),
	};

	/// Installs the host's file system. Call once, before anything opens a file.
	void Install(const Ops& ops);

	/// True when a host file system is in use; false means plain OS calls.
	bool IsInstalled();

	/// The installed table, or nullptr. Callers check the individual members:
	/// a frontend may implement files but not directories.
	const Ops* GetOps();

	/// Opens a file through the host and wraps it in a std::FILE*, so every
	/// existing caller keeps working unchanged. Returns nullptr if the host
	/// cannot open it, or if this platform has no way to build such a wrapper.
	std::FILE* OpenAsCFile(const char* path, const char* mode);

	/// stat(), for the callers that only want the answers FileSystem asks for.
	bool StatPath(const char* path, bool* is_directory, s64* size);
} // namespace HostVFS
