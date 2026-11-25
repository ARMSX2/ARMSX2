// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string>

// Files from bin/resources compiled into the binary by
// cmake/EmbedResources.cmake. Where a file is compiled in, the readers use it and
// ignore the resources directory's copy, which can be missing (the libretro core
// gets a system directory with nothing guaranteed in it), stale or empty.
namespace EmbeddedResources
{
	// Named relative to the resources directory, the way the readers ask for
	// them: "shaders/vulkan/tfx.glsl", "GameIndex.yaml". Returns nothing if the
	// file was not embedded.
	std::optional<std::string> Read(const char* filename);
} // namespace EmbeddedResources
