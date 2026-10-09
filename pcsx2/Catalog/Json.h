// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "rapidjson/document.h"

#include <string>
#include <string_view>

class Error;

namespace Catalog
{
	/// Parses UTF-8 JSON (a leading byte order mark is accepted) that need not be NUL terminated.
	/// On failure returns false and, if `error` is given, sets it to "offset N: <what is wrong>".
	bool ParseJson(std::string_view text, rapidjson::Document* document, std::string* error = nullptr);

	std::string JsonToString(const rapidjson::Value& value, bool pretty = false);

	/// JsonToString() written with WriteFileAtomic(): the state files services keep (installed
	/// packs, driver metadata) are never seen half-written.
	bool WriteJsonFileAtomic(const std::string& path, const rapidjson::Value& value, Error* error = nullptr);
} // namespace Catalog
