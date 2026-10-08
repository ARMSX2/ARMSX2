// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <vector>

// A GS dump is several files made at different times: the dump itself, streamed while it records,
// the driver report, written when it opens, and the screenshot, written by a worker thread. What
// the user is handed is one zip of them, so there is one file to move off a device and attach to a
// report.
namespace GSDumpBundle
{
	/// The zip for a dump base name.
	std::string ZipPath(const std::string& dump_base);

	/// Writes `zip_path` holding each existing file in `parts`, named by its file name, then
	/// deletes those files. A part that does not exist is left out: the screenshot can fail on its
	/// own and the rest of the dump is still worth keeping.
	///
	/// Returns false, with every part left where it was, when no part exists or the zip cannot be
	/// written. A dump must never be lost to the packaging. The zip appears at `zip_path` whole or
	/// not at all.
	bool Pack(const std::string& zip_path, const std::vector<std::string>& parts);
} // namespace GSDumpBundle
