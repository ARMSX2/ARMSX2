// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Vulkan pixel shader (bin/resources/shaders/vulkan/tfx.glsl) floors the interpolated
// depth to the PS2's 32-bit integer grid, floor(z * 2^32) * 2^-32, and declares its depth
// write layout(depth_less): the value it writes is never above gl_FragCoord.z. The driver
// may then reject a fragment by testing depth before the shader runs. If the floor could
// round above its input, that early test would drop fragments the late test passes.
//
// These tests check every float32 depth from 0 to 1 with the same float32 operations the
// shader uses. They prove the arithmetic, not a GPU: whether a GPU's early test reads the
// same depth as gl_FragCoord.z is a device question.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
	float ZFloor(float z)
	{
		return std::floor(z * 0x1p32f) * 0x1p-32f;
	}

	float FromBits(uint32_t bits)
	{
		float f;
		std::memcpy(&f, &bits, sizeof(f));
		return f;
	}
} // namespace

// The floored depth is never above its input, and it is exactly the integer floor of
// z * 2^32 scaled back: no float32 rounding anywhere in the expression.
TEST(GSZFloor, EveryDepthFloorsExactlyAndNeverRises)
{
	constexpr uint32_t one_bits = 0x3f800000u;
	uint32_t above = 0;
	uint32_t inexact = 0;
	for (uint32_t bits = 0; bits <= one_bits; bits++)
	{
		const float z = FromBits(bits);
		const float f = ZFloor(z);
		above += f > z;
		inexact += static_cast<double>(f) * 0x1p32 != std::floor(static_cast<double>(z) * 0x1p32);
	}
	EXPECT_EQ(above, 0u);
	EXPECT_EQ(inexact, 0u);
}

TEST(GSZFloor, GridPointsAreFixed)
{
	// Depths already on the grid, as the GS writes them, come back unchanged.
	for (const uint32_t zi : {0u, 1u, 2u, 0xFFFFu, 0x10000u, 0xFFFFFFu, 0x1000000u, 0x7FFFFFFFu, 0xFFFFFF00u})
	{
		const float z = static_cast<float>(zi) * 0x1p-32f;
		if (static_cast<double>(z) * 0x1p32 != static_cast<double>(zi))
			continue; // not representable in float32; the shader never sees it either
		EXPECT_EQ(ZFloor(z), z) << zi;
	}
}
