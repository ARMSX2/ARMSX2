// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSDumpBundle.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "zip.h"

namespace GSDumpBundle
{
	std::string ZipPath(const std::string& dump_base)
	{
		return dump_base + ".gs.zip";
	}

	// The dump and the screenshot are compressed already; deflating them again costs time on a
	// handheld and gains nothing.
	static bool IsAlreadyCompressed(const std::string_view name)
	{
		for (const std::string_view suffix : {".zst", ".xz", ".png", ".jpg", ".webp"})
		{
			if (StringUtil::EndsWithNoCase(name, suffix))
				return true;
		}
		return false;
	}

	bool Pack(const std::string& zip_path, const std::vector<std::string>& parts)
	{
		std::vector<const std::string*> present;
		for (const std::string& part : parts)
		{
			if (FileSystem::FileExists(part.c_str()))
				present.push_back(&part);
			else
				Console.WarningFmt("GSDump: '{}' was not written, so it is not in '{}'.", Path::GetFileName(part),
					Path::GetFileName(zip_path));
		}
		if (present.empty())
			return false;

		zip_error_t ze;
		zip_error_init(&ze);

		zip_source_t* zip_source = zip_source_file_create(zip_path.c_str(), 0, 0, &ze);
		zip_t* zf = nullptr;
		if (!zip_source || !(zf = zip_open_from_source(zip_source, ZIP_CREATE | ZIP_TRUNCATE, &ze)))
		{
			Console.ErrorFmt("GSDump: cannot create '{}': {}", zip_path, zip_error_strerror(&ze));
			if (zip_source)
				zip_source_free(zip_source);
			zip_error_fini(&ze);
			return false;
		}

		for (const std::string* part : present)
		{
			zip_source_t* source = zip_source_file_create(part->c_str(), 0, 0, &ze);
			const std::string name(Path::GetFileName(*part));
			const zip_int64_t index = source ? zip_file_add(zf, name.c_str(), source, ZIP_FL_ENC_UTF_8) : -1;
			if (index < 0)
			{
				Console.ErrorFmt("GSDump: cannot add '{}' to '{}': {}", name, zip_path,
					source ? zip_strerror(zf) : zip_error_strerror(&ze));
				if (source)
					zip_source_free(source);
				zip_discard(zf);
				zip_error_fini(&ze);
				return false;
			}

			if (IsAlreadyCompressed(name))
				zip_set_file_compression(zf, index, ZIP_CM_STORE, 0);
		}

		// The parts are read here, so this is where a full disk shows up.
		if (zip_close(zf) != 0)
		{
			Console.ErrorFmt("GSDump: cannot write '{}': {}", zip_path, zip_strerror(zf));
			zip_discard(zf);
			zip_error_fini(&ze);
			return false;
		}
		zip_error_fini(&ze);

		for (const std::string* part : present)
			FileSystem::DeleteFilePath(part->c_str());
		return true;
	}
} // namespace GSDumpBundle
