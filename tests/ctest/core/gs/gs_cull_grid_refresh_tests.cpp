// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins that a settings change re-derives the per-primitive cull grid on every parser.
//
// The vertex kick drops a primitive that spans no device sample point, and which points exist
// depends on the upscale AND the half-pixel-offset mode: Native puts them half a device step off
// the plain grid (GSState::CullGridFor). A grid left over from before a change culls against
// points the renderer no longer samples -- a thin primitive that now lights a pixel is dropped.
// The grid used to be refreshed only from UpdateRenderFixes, which GSUpdateConfig runs for an
// upscale or render-fix change but not for a half-pixel-offset change, and never on the front
// parser of the pipelined back thread, which does its own culling. A GameDB halfPixelOffset
// reaches the renderer through the same settings apply after boot, so it was lost too.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	bool SameGrid(const GSVertexKernels::CullGrid& a, const GSVertexKernels::CullGrid& b)
	{
		return a.shift == b.shift && a.sprite_shift == b.sprite_shift && a.band_bias_x == b.band_bias_x &&
		       a.band_bias_y == b.band_bias_y;
	}

	class GSCullGridRefresh : public Fixture
	{
	protected:
		void TearDown() override
		{
			g_gs_front.reset();
			Fixture::TearDown();
		}
	};
} // namespace

// Precondition for everything below: at 2x the two modes ask for different grids.
TEST_F(GSCullGridRefresh, OffAndNativeAskForDifferentGrids)
{
	EXPECT_FALSE(SameGrid(GSState::CullGridFor(2.0f, GSHalfPixelOffset::Off),
		GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native)));
}

TEST_F(GSCullGridRefresh, AHalfPixelOffsetChangeReachesTheRenderer)
{
	BringUp();
	ASSERT_TRUE(SameGrid(m_gs->m_cull_grid, GSState::CullGridFor(2.0f, GSHalfPixelOffset::Off)));

	Pcsx2Config::GSOptions changed = GSConfig;
	changed.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Native;
	GSUpdateConfig(changed);

	EXPECT_TRUE(SameGrid(m_gs->m_cull_grid, GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native)));
}

TEST_F(GSCullGridRefresh, EveryChangeReachesTheFrontParser)
{
	GSConfig.BackThreadModeResolved = GSBackThreadMode::Pipelined;
	m_device_api = RenderAPI::Vulkan;
	BringUp();
	ASSERT_TRUE(m_gs->IsBackThreadRunning());
	g_gs_front = std::make_unique<GSFrontState>(m_gs);
	ASSERT_TRUE(SameGrid(g_gs_front->m_cull_grid, GSState::CullGridFor(2.0f, GSHalfPixelOffset::Off)));

	Pcsx2Config::GSOptions changed = GSConfig;
	changed.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Native;
	GSUpdateConfig(changed);
	EXPECT_TRUE(SameGrid(g_gs_front->m_cull_grid, GSState::CullGridFor(2.0f, GSHalfPixelOffset::Native)));

	changed = GSConfig;
	changed.UpscaleMultiplier = 4.0f;
	GSUpdateConfig(changed);
	EXPECT_TRUE(SameGrid(g_gs_front->m_cull_grid, GSState::CullGridFor(4.0f, GSHalfPixelOffset::Native)));
	EXPECT_TRUE(SameGrid(m_gs->m_cull_grid, GSState::CullGridFor(4.0f, GSHalfPixelOffset::Native)));
}
