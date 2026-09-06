// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/StringUtil.h"

#include <optional>
#include <string_view>

// Parsing for pcsx2-gsrunner's -affinity flag. Lives in its own header rather than in
// Main.cpp so a test can call it; there is nothing else in here.
//
// The argument names one of VMManager::SetEmuThreadAffinities' thread-placement modes:
//
//   0  unpinned -- every emu thread gets an empty mask, which means "every processor"
//   1-6 explicit per-core placements, one row per EE/VU/GS priority order
//   7  Performance Cores -- confine the emu threads to the capacity-ranked big tier
//
// Out-of-range and malformed values are rejected rather than clamped. The mode decides
// which cores the whole VM runs on, so a value that was quietly turned into a different
// one would make a round claim a placement it never had -- which is the exact failure
// this flag exists to stop.
namespace GSRunnerAffinity
{
	inline constexpr int MODE_MIN = 0;
	inline constexpr int MODE_MAX = 7;

	inline std::optional<int> ParseMode(const std::string_view arg)
	{
		// Reject before parsing what from_chars would otherwise accept: a leading '+', and
		// surrounding whitespace, both of which would let two different spellings of the same
		// round appear in two different artifacts.
		if (arg.empty() || arg != StringUtil::StripWhitespace(arg))
			return std::nullopt;

		std::string_view rest;
		const std::optional<int> value = StringUtil::FromChars<int>(arg, 10, &rest);
		if (!value.has_value() || !rest.empty())
			return std::nullopt;

		if (value.value() < MODE_MIN || value.value() > MODE_MAX)
			return std::nullopt;

		return value;
	}
} // namespace GSRunnerAffinity
