// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Small helpers shared by the catalog_tests sources.

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifndef CATALOG_FIXTURE_DIR
#error "catalog_tests must be built with CATALOG_FIXTURE_DIR (see tests/ctest/core/catalog/CMakeLists.txt)"
#endif

namespace CatalogTest
{
	/// Absolute path of a file in tests/ctest/core/catalog/fixtures/.
	inline std::string FixturePath(std::string_view name)
	{
		return Path::Combine(CATALOG_FIXTURE_DIR, name);
	}

	/// Contents of a fixture; fails the calling test if it is missing.
	inline std::string ReadFixture(std::string_view name)
	{
		const std::optional<std::string> data = FileSystem::ReadFileToString(FixturePath(name).c_str());
		if (!data.has_value())
		{
			ADD_FAILURE() << "missing fixture " << FixturePath(name);
			return {};
		}
		return *data;
	}

	/// A fresh empty directory, deleted with everything in it when the object goes away.
	class ScopedTempDir
	{
	public:
		ScopedTempDir()
		{
			static std::atomic<unsigned> counter{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = (std::filesystem::temp_directory_path() /
					  ("armsx2-catalog-test-" + std::to_string(stamp) + "-" + std::to_string(counter++)))
			             .string();
			std::filesystem::create_directories(m_path);
		}

		~ScopedTempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(m_path, ec);
		}

		ScopedTempDir(const ScopedTempDir&) = delete;
		ScopedTempDir& operator=(const ScopedTempDir&) = delete;

		const std::string& Dir() const { return m_path; }
		std::string File(std::string_view name) const { return Path::Combine(m_path, name); }

	private:
		std::string m_path;
	};
} // namespace CatalogTest
