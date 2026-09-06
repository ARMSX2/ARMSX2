// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string>

// Files from bin/resources compiled into the binary by
// cmake/EmbedResources.cmake. The readers fall back to these when the resources
// directory has no copy - the normal case for the libretro core, where the
// frontend gives us a system directory and nothing is guaranteed to be in it.
namespace EmbeddedResources
{
	// Named relative to the resources directory, the way the readers ask for
	// them: "shaders/vulkan/tfx.glsl", "GameIndex.yaml". Returns nothing if the
	// file was not embedded.
	std::optional<std::string> Read(const char* filename);
} // namespace EmbeddedResources
