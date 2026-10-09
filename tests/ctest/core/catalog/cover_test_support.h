// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Shared by the cover tests: the live-capture URLs and the GameIndex.yaml excerpt.

#include "catalog_test_support.h"

#include "Catalog/CoverSources.h"

#include <memory>
#include <string>

namespace CoverTest
{
	inline constexpr const char* PS2_REPO = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers";
	inline constexpr const char* PSX_REPO = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers";
	inline constexpr const char* ARCADE_ROOT = "https://arcade.ps2ktxpak.net/arcade-covers";

	inline std::string Ps2Jpg(const std::string& serial) { return std::string(PS2_REPO) + "/default/" + serial + ".jpg"; }
	inline std::string Ps2Png(const std::string& serial) { return std::string(PS2_REPO) + "/3d/" + serial + ".png"; }

	/// Real entries from bin/resources/GameIndex.yaml: Kuon (SCAJ/SLES/SLPS/SLUS), Madden NFL 2001,
	/// Resident Evil 4, two Guilty Gear XX releases, and titles with brackets and comments.
	inline std::shared_ptr<const Catalog::CoverRegionIndex> ExcerptIndex()
	{
		return std::make_shared<const Catalog::CoverRegionIndex>(
			Catalog::CoverRegionIndex::FromGameIndexYaml(CatalogTest::ReadFixture("covers/GameIndex.excerpt.yaml")));
	}
} // namespace CoverTest
