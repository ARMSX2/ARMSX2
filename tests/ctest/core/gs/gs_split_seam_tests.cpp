// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins state that has to cross the seam of the GS multi-threading split.
//
// With the split on, a front object parses GIF data on the MTGS thread and the renderer object
// executes the resulting records on the GS back thread. Each object has a full GSState, so a
// value that one side needs and the other side owns does not fail loudly: the reader gets its own
// stale copy. Each test below builds the split (front + running back thread), drives the front
// the way MTGS does, and checks the value a single object would have produced.

#include "gs_hw_draw_harness.h"

#include "GS/GSPerfMon.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace GSHWDrawHarness;

namespace
{
	class GSSplitSeam : public Fixture
	{
	protected:
		void TearDown() override
		{
			g_gs_front.reset();
			Fixture::TearDown();
		}

		// The renderer with its back thread running, and a front parser object on top of it.
		void BringUpSplit()
		{
			GSConfig.BackThreadResolved = true;
			m_device_api = RenderAPI::Vulkan;
			BringUp();
			ASSERT_TRUE(m_gs->IsBackThreadRunning());
			g_gs_front = std::make_unique<GSFrontState>(m_gs);
		}

		// One untextured 64x64 sprite into a 32-bit frame at `fbp`, with `scanmsk` written first.
		static void Sprite(GSState& gs, u32 fbp, u32 scanmsk)
		{
			Packet p;
			Environment(p, fbp, PSMCT32);

			GIFReg r = {};
			r.U64 = 0;
			r.SCANMSK.MSK = scanmsk;
			p.Reg(GIF_A_D_REG_SCANMSK, r);

			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(64 << 4, 64 << 4, 0, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(gs, prim);
		}
	};
} // namespace

// GSC_IRem clears SCANMSK in the parse environment from inside a draw. On a single object that
// environment is the live one, so later draws are built with the mask off until the game writes
// SCANMSK again. This is the reference the split test below must match.
TEST_F(GSSplitSeam, IRemScanMaskClearPersistsOnASingleObject)
{
	GSConfig.GetSkipCountFunctionId = GSLookupGetSkipCountFunctionId("GSC_IRem");
	BringUp();
	m_gs->UpdateRenderFixes();

	Sprite(*m_gs, 0, 2);

	EXPECT_EQ(m_gs->m_env.SCANMSK.MSK, 0u);
	EXPECT_EQ(m_gs->m_prev_env.SCANMSK.MSK, 0u);
}

// With the split on, the hook clears the back's installed copy, which the next draw record
// overwrites. The front has to apply the clear itself or every later draw carries the mask.
TEST_F(GSSplitSeam, IRemScanMaskClearReachesTheFront)
{
	GSConfig.GetSkipCountFunctionId = GSLookupGetSkipCountFunctionId("GSC_IRem");
	BringUpSplit();
	m_gs->UpdateRenderFixes();

	Sprite(*g_gs_front, 0, 2);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_gs_front->m_env.SCANMSK.MSK, 0u);
	EXPECT_EQ(g_gs_front->m_prev_env.SCANMSK.MSK, 0u);
}

// Without the hook nothing clears the mask, on either object.
TEST_F(GSSplitSeam, ScanMaskStaysWithoutTheIRemHook)
{
	BringUpSplit();
	m_gs->UpdateRenderFixes();

	Sprite(*g_gs_front, 0, 2);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_gs_front->m_env.SCANMSK.MSK, 2u);
}
