// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "UpdaterBranding.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/ZipHelpers.h"

#include "fmt/format.h"

#if defined(_WIN32)
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "SZErrors.h"
#endif

#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static inline bool ExtractUpdater(const char* archive_path, const char* destination_path, std::string* error)
{
#if defined(_WIN32)
	if (StringUtil::EndsWithNoCase(archive_path, ".zip"))
	{
		zip_error_t ze;
		zip_error_init(&ze);
		std::unique_ptr<zip_t, void (*)(zip_t*)> zip = zip_open_managed(archive_path, ZIP_RDONLY, &ze);
		if (!zip)
		{
			*error = fmt::format("Failed to open '{}': {}", archive_path, zip_error_strerror(&ze));
			zip_error_fini(&ze);
			return false;
		}
		zip_error_fini(&ze);

		const zip_int64_t count = zip_get_num_entries(zip.get(), 0);
		if (count < 0)
		{
			*error = fmt::format("Failed to read zip directory: {}", zip_strerror(zip.get()));
			return false;
		}

		zip_uint64_t updater_file_index = static_cast<zip_uint64_t>(count);
		for (zip_uint64_t file_index = 0; file_index < static_cast<zip_uint64_t>(count); file_index++)
		{
			const char* filename = zip_get_name(zip.get(), file_index, ZIP_FL_ENC_GUESS);
			if (!filename)
				continue;

			std::string normalized_filename(filename);
			for (char& ch : normalized_filename)
			{
				if (ch == '/' || ch == '\\')
					ch = FS_OSPATH_SEPARATOR_CHARACTER;
			}

			const std::string filename_only(Path::GetFileName(normalized_filename));
			if (StringUtil::Strcasecmp(filename_only.c_str(), UpdaterBranding::UPDATER_EXECUTABLE) == 0)
			{
				updater_file_index = file_index;
				break;
			}
		}

		if (updater_file_index == static_cast<zip_uint64_t>(count))
		{
			*error = fmt::format("Updater executable ({}) not found in archive.", UpdaterBranding::UPDATER_EXECUTABLE);
			return false;
		}

		zip_stat_t zs;
		if (zip_stat_index(zip.get(), updater_file_index, ZIP_FL_ENC_GUESS, &zs) != 0)
		{
			*error = fmt::format("Failed to stat {} in archive: {}", UpdaterBranding::UPDATER_EXECUTABLE, zip_strerror(zip.get()));
			return false;
		}
		if (zs.size > static_cast<zip_uint64_t>((std::numeric_limits<size_t>::max)()))
		{
			*error = fmt::format("{} is too large to extract.", UpdaterBranding::UPDATER_EXECUTABLE);
			return false;
		}

		std::unique_ptr<zip_file_t, int (*)(zip_file_t*)> zf =
			zip_fopen_index_managed(zip.get(), updater_file_index, ZIP_FL_ENC_GUESS);
		if (!zf)
		{
			*error = fmt::format("Failed to open {} in archive: {}", UpdaterBranding::UPDATER_EXECUTABLE, zip_strerror(zip.get()));
			return false;
		}

		std::vector<u8> data(static_cast<size_t>(zs.size));
		if (!data.empty() && zip_fread(zf.get(), data.data(), data.size()) != static_cast<zip_int64_t>(data.size()))
		{
			*error = fmt::format("Failed to read {} from archive.", UpdaterBranding::UPDATER_EXECUTABLE);
			return false;
		}

		std::FILE* fp = FileSystem::OpenCFile(destination_path, "wb");
		if (!fp)
		{
			*error = fmt::format("Failed to open '{0}' for writing.", destination_path);
			return false;
		}

		const bool wrote_completely = (data.empty() || std::fwrite(data.data(), data.size(), 1, fp) == 1) && std::fflush(fp) == 0;
		if (std::fclose(fp) != 0 || !wrote_completely)
		{
			*error = fmt::format("Failed to write output file '{}'", destination_path);
			FileSystem::DeleteFilePath(destination_path);
			return false;
		}

		error->clear();
		return true;
	}

	static constexpr size_t kInputBufSize = ((size_t)1 << 18);
	static constexpr ISzAlloc g_Alloc = {SzAlloc, SzFree};

	CFileInStream instream = {};
	CLookToRead2 lookstream = {};
	CSzArEx archive = {};

	FileInStream_CreateVTable(&instream);
	LookToRead2_CreateVTable(&lookstream, False);
	CrcGenerateTable();

	lookstream.buf = (Byte*)ISzAlloc_Alloc(&g_Alloc, kInputBufSize);
	if (!lookstream.buf)
	{
		*error = "Failed to allocate input buffer?!";
		return false;
	}

	lookstream.bufSize = kInputBufSize;
	lookstream.realStream = &instream.vt;
	LookToRead2_INIT(&lookstream);
	ScopedGuard buffer_guard([&lookstream]() {
		ISzAlloc_Free(&g_Alloc, lookstream.buf);
	});

#ifdef _WIN32
	WRes wres = InFile_OpenW(&instream.file, FileSystem::GetWin32Path(archive_path).c_str());
#else
	WRes wres = InFile_Open(&instream.file, archive_path);
#endif
	if (wres != 0)
	{
		*error = fmt::format("Failed to open '{0}': {1}", archive_path, wres);
		return false;
	}

	ScopedGuard file_guard([&instream]() {
		File_Close(&instream.file);
	});

	SzArEx_Init(&archive);

	SRes res = SzArEx_Open(&archive, &lookstream.vt, &g_Alloc, &g_Alloc);
	if (res != SZ_OK)
	{
		*error = fmt::format("SzArEx_Open() failed: {0} [{1}]", SZErrorToString(res), res);
		return false;
	}
	ScopedGuard archive_guard([&archive]() {
		SzArEx_Free(&archive, &g_Alloc);
	});

	std::vector<UInt16> filename_buffer;
	u32 updater_file_index = archive.NumFiles;
	for (u32 file_index = 0; file_index < archive.NumFiles; file_index++)
	{
		if (SzArEx_IsDir(&archive, file_index))
			continue;

		size_t filename_len = SzArEx_GetFileNameUtf16(&archive, file_index, nullptr);
		if (filename_len <= 1)
			continue;

		filename_buffer.resize(filename_len);
		filename_len = SzArEx_GetFileNameUtf16(&archive, file_index, filename_buffer.data());

		// TODO: This won't work on Linux (4-byte wchar_t).
		const std::string filename(StringUtil::WideStringToUTF8String(reinterpret_cast<wchar_t*>(filename_buffer.data())));
		if (filename != UpdaterBranding::UPDATER_EXECUTABLE)
			continue;

		updater_file_index = file_index;
		break;
	}

	if (updater_file_index == archive.NumFiles)
	{
		*error = fmt::format("Updater executable ({}) not found in archive.", UpdaterBranding::UPDATER_EXECUTABLE);
		return false;
	}

	UInt32 block_index = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
	Byte* out_buffer = 0; /* it must be 0 before first call for each new archive. */
	size_t out_buffer_size = 0; /* it can have any value before first call (if outBuffer = 0) */
	ScopedGuard out_buffer_guard([&out_buffer]() {
		if (out_buffer)
			ISzAlloc_Free(&g_Alloc, out_buffer);
	});

	size_t out_offset = 0;
	size_t extracted_size = 0;
	res = SzArEx_Extract(&archive, &lookstream.vt, updater_file_index,
		&block_index, &out_buffer, &out_buffer_size, &out_offset, &extracted_size, &g_Alloc, &g_Alloc);
	if (res != SZ_OK)
	{
		*error = fmt::format("Failed to decompress {0} from 7z (file index=%u, error=%s)",
			UpdaterBranding::UPDATER_EXECUTABLE, updater_file_index, SZErrorToString(res));
		return false;
	}

	std::FILE* fp = FileSystem::OpenCFile(destination_path, "wb");
	if (!fp)
	{
		*error = fmt::format("Failed to open '{0}' for writing.", destination_path);
		return false;
	}

	const bool wrote_completely = std::fwrite(out_buffer + out_offset, extracted_size, 1, fp) == 1 && std::fflush(fp) == 0;
	if (std::fclose(fp) != 0 || !wrote_completely)
	{
		*error = fmt::format("Failed to write output file '{}'", destination_path);
		FileSystem::DeleteFilePath(destination_path);
		return false;
	}

	error->clear();
	return true;
#else
	*error = "Not supported on this platform";
	return false;
#endif
}
