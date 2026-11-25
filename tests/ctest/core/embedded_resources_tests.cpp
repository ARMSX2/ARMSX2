// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// In a build that compiles the shaders in (ARMSX2_EMBED_RESOURCES), the compiled-in copy is the one
// the renderer reads, whatever the resources folder holds. A copy on disk can be stale or empty: on
// Android, after an app update, some phones read the rewritten shader files back empty until a
// reboot, and every game then failed to create the GS device.

#include <gtest/gtest.h>

#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "common/FileSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	class EmbeddedResources : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_dir = fs::temp_directory_path() /
					("armsx2-embedded-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(m_dir / "shaders" / "vulkan");
			m_previous = EmuFolders::Resources;
			EmuFolders::Resources = m_dir.string();
		}

		void TearDown() override
		{
			EmuFolders::Resources = m_previous;
			std::error_code ignored;
			fs::remove_all(m_dir, ignored);
		}

		void WriteResource(const char* name, const std::string& text)
		{
			std::ofstream(m_dir / name, std::ios::binary) << text;
		}

		fs::path m_dir;
		std::string m_previous;
	};
} // namespace

TEST_F(EmbeddedResources, ShaderComesFromTheBinaryNotTheResourcesFolder)
{
#ifndef ARMSX2_EMBEDDED_RESOURCES
	GTEST_SKIP() << "this build reads shaders from the resources folder only";
#else
	const std::optional<std::string> shipped = FileSystem::ReadFileToString(ARMSX2_SOURCE_CONVERT_GLSL);
	ASSERT_TRUE(shipped.has_value());
	ASSERT_FALSE(shipped->empty());

	WriteResource("shaders/vulkan/convert.glsl", "");
	EXPECT_EQ(GSDevice::ReadShaderSource("shaders/vulkan/convert.glsl"), shipped);

	WriteResource("shaders/vulkan/convert.glsl", "// an older build's copy\n");
	EXPECT_EQ(GSDevice::ReadShaderSource("shaders/vulkan/convert.glsl"), shipped);
#endif
}
