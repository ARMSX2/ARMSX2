// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Reading and writing the text of .pnach files for the online patch browser (PatchRepo.h).
//
// This is not the core's pnach loader (Patch.cpp): that one understands patch lines and applies
// them. The browser only needs to split a downloaded file into its `[Section]` blocks, show a
// name and a one-line description for each, and copy chosen blocks into the player's own file
// unchanged, so this works on lines of text and never interprets a `patch=` line.

#include <string>
#include <string_view>
#include <vector>

namespace Catalog
{
	struct PnachSection
	{
		/// The text between the brackets, trimmed. This is what goes in `[Patches] Enable` /
		/// `[Cheats] Enable` and what the core matches on, so it is never empty here.
		std::string name;

		/// The first `comment=` value in the block, else the first `description=` value, else
		/// empty. (The patch database mostly writes `description=`; community cheat files use
		/// either.)
		std::string description;

		/// The block exactly as it appeared, header line included, with trailing whitespace
		/// removed and line endings normalised to "\n".
		std::string body;
	};

	struct PnachFile
	{
		/// The first `gametitle=` value anywhere in the file, trimmed; empty if none.
		std::string game_title;

		/// Blocks in file order. Duplicated names are all kept: choosing between them is the
		/// caller's policy. Lines before the first `[Section]` (the old ungrouped `patch=` style)
		/// belong to no block and are dropped, so a file with no headers has no sections.
		std::vector<PnachSection> sections;
	};

	/// Splits .pnach text into sections. Accepts "\n", "\r\n" and "\r" line endings and a leading
	/// UTF-8 byte order mark. Never fails: text that is not a pnach file just has no sections.
	PnachFile ParsePnach(std::string_view text);

	/// The text of a .pnach file after adding the given section bodies.
	///
	/// With empty `existing` this is a new file: `gametitle=<title>`, a blank line, then each body
	/// followed by a blank line (no `gametitle=` line when `game_title` is empty). Otherwise
	/// `existing` is kept byte for byte, separated from the new bodies by one blank line.
	std::string ComposePnach(std::string_view existing, std::string_view game_title, const std::vector<std::string>& bodies);

	/// The key two cheat names are compared by when looking for duplicates: trimmed, ASCII
	/// lowercased, every run of whitespace collapsed to one space. The same cheat is often spelled
	/// "Infinite Health" in one file and "infinite  health" in another.
	std::string CheatNameKey(std::string_view name);
} // namespace Catalog
