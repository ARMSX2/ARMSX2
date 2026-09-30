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
			GSConfig.BackThread = true;
			GSConfig.BackThreadResolved = true;
			m_device_api = RenderAPI::Vulkan;
			BringUp();
			ASSERT_TRUE(m_gs->IsBackThreadRunning());
			g_gs_front = std::make_unique<GSFrontState>(m_gs);
			g_gs_front->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
			g_gs_front->ResetPCRTC();
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

		// A local-to-local move of a 64x32 CT32 block, started by the TRXDIR write.
		static void LocalMove(GSState& gs)
		{
			Packet p;
			GIFReg r = {};

			r.U64 = 0;
			r.BITBLTBUF.SBP = 0x0;
			r.BITBLTBUF.SBW = 1;
			r.BITBLTBUF.SPSM = PSMCT32;
			r.BITBLTBUF.DBP = 0x1000;
			r.BITBLTBUF.DBW = 1;
			r.BITBLTBUF.DPSM = PSMCT32;
			p.Reg(GIF_A_D_REG_BITBLTBUF, r);

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_TRXPOS, r);

			r.U64 = 0;
			r.TRXREG.RRW = 64;
			r.TRXREG.RRH = 32;
			p.Reg(GIF_A_D_REG_TRXREG, r);

			r.U64 = 0;
			r.TRXDIR.XDIR = 2;
			p.Reg(GIF_A_D_REG_TRXDIR, r);

			p.Send(gs, GIFRegPRIM{});
		}

		// A GIF IMAGE tag with `qwords` of data behind it.
		static void Image(GSState& gs, u32 qwords)
		{
			std::vector<GIFPackedReg> buf(qwords + 1);
			GIFTag tag = {};
			tag.NLOOP = qwords;
			tag.EOP = 1;
			tag.FLG = GIF_FLG_IMAGE;
			std::memcpy(&buf[0], &tag, sizeof(tag));
			gs.Transfer<0>(reinterpret_cast<const u8*>(buf.data()), static_cast<u32>(buf.size()));
		}
	};

	/// A renderer whose back thread can be held at a known point: the first palette-source
	/// invalidation of a palette load waits until the test opens the gate. Records queued behind
	/// that load stay unexecuted until then. VSync only records its idle-frame argument.
	class StallRenderer final : public GSRendererHW
	{
	public:
		void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false) override
		{
			if (clut)
			{
				m_stalled.store(true, std::memory_order_release);
				while (!m_gate.load(std::memory_order_acquire))
					std::this_thread::yield();
			}
			GSRendererHW::InvalidateLocalMem(BITBLTBUF, r, clut);
		}

		void VSync(u32 field, bool registers_written, bool idle_frame) override
		{
			m_vsyncs++;
			m_idle_frame = idle_frame;
		}

		std::atomic<bool> m_gate{true};
		std::atomic<bool> m_stalled{false};
		int m_vsyncs = 0;
		bool m_idle_frame = false;
	};

	class GSSplitSeamStalled : public GSSplitSeam
	{
	protected:
		void BringUpStalled()
		{
			GSConfig.BackThread = true;
			GSConfig.BackThreadResolved = true;

			auto device = std::make_unique<CaptureDevice>();
			device->m_api = RenderAPI::Vulkan;
			g_gs_device = std::move(device);
			ASSERT_TRUE(g_gs_device->Create(GSVSyncMode::Disabled, false));

			m_priv_regs = std::make_unique<GSPrivRegSet>();
			std::memset(m_priv_regs.get(), 0, sizeof(GSPrivRegSet));

			auto renderer = std::make_unique<StallRenderer>();
			m_stall = renderer.get();
			g_gs_renderer = std::move(renderer);
			g_gs_renderer->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
			g_gs_renderer->ResetPCRTC();
			ASSERT_TRUE(m_stall->IsBackThreadRunning());

			g_gs_front = std::make_unique<GSFrontState>(m_stall);
			g_gs_front->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
			g_gs_front->ResetPCRTC();
		}

		// A TEX0 write that loads a palette, which the front ships as a ClutLoad record. With the
		// gate closed the back thread stops inside that record.
		void StallTheBack()
		{
			m_stall->m_gate.store(false, std::memory_order_release);

			Packet p;
			GIFReg r = {};
			r.U64 = 0;
			r.TEX0.TBP0 = 0x1000;
			r.TEX0.TBW = 1;
			r.TEX0.PSM = PSMT8;
			r.TEX0.TW = 6;
			r.TEX0.TH = 6;
			r.TEX0.CBP = 0x2000;
			r.TEX0.CPSM = PSMCT32;
			r.TEX0.CLD = 1;
			p.Reg(GIF_A_D_REG_TEX0_1, r);
			p.Send(*g_gs_front, GIFRegPRIM{});

			while (!m_stall->m_stalled.load(std::memory_order_acquire))
				std::this_thread::yield();
		}

		// Opens the gate from another thread after a delay, for a call that has to be made while
		// the back is still stalled and may itself wait for the back.
		std::thread OpenGateLater()
		{
			return std::thread([this]() {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				m_stall->m_gate.store(true, std::memory_order_release);
			});
		}

		StallRenderer* m_stall = nullptr;
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

// Move() ends a local-to-local transfer by setting TRXDIR to 3 (off), so a later IMAGE tag, a FIFO
// read or a savestate sees no transfer in progress. This is the single-object reference.
TEST_F(GSSplitSeam, MoveLeavesTransferDirectionOffOnASingleObject)
{
	BringUp();

	LocalMove(*m_gs);

	EXPECT_EQ(m_gs->m_env.TRXDIR.XDIR, 3u);
}

// On the split the move runs on the back. The front must still end with TRXDIR off.
TEST_F(GSSplitSeam, MoveLeavesTransferDirectionOffOnTheFront)
{
	BringUpSplit();

	LocalMove(*g_gs_front);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_gs_front->m_env.TRXDIR.XDIR, 3u);
}

// An IMAGE tag after a finished move does nothing on a single object. On the front it must not
// run a move of its own against the front's unused local memory.
TEST_F(GSSplitSeam, ImageTagAfterAMoveDoesNotMoveOnTheFront)
{
	BringUpSplit();

	LocalMove(*g_gs_front);
	Image(*g_gs_front, 4);
	g_gs_front->DrainBackQueue();

	EXPECT_TRUE(g_gs_front->m_draw_transfers.empty());
	EXPECT_EQ(g_gs_front->m_env.TRXDIR.XDIR, 3u);
}

// With a move hook armed the front cannot know whether the hook took the move (which leaves
// TRXDIR at 2) or declined it. MV_Ico declines a CT32 to CT32 move, so the answer is 3.
TEST_F(GSSplitSeam, MoveHookThatDeclinesLeavesTransferDirectionOffOnTheFront)
{
	GSConfig.MoveHandlerFunctionId = GSLookupMoveHandlerFunctionId("MV_Ico");
	BringUpSplit();
	m_gs->UpdateRenderFixes();

	LocalMove(*g_gs_front);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_gs_front->m_env.TRXDIR.XDIR, 3u);
}

// Unsynchronized downloads read live GS memory from the EE thread, which the back thread makes
// unsafe, so the policy turns the split off for them when a renderer opens. Switching to that
// mode in game must reach the policy too.
TEST_F(GSSplitSeam, SwitchingToUnsynchronizedDownloadsTurnsTheSplitOff)
{
	BringUpSplit();

	Pcsx2Config::GSOptions changed = GSConfig;
	changed.HWDownloadMode = GSHardwareDownloadMode::Unsynchronized;
	GSUpdateConfig(changed);
	m_gs = nullptr; // a reopen replaces the renderer

	EXPECT_FALSE(g_gs_front);
	ASSERT_TRUE(g_gs_renderer);
	EXPECT_FALSE(g_gs_renderer->IsBackThreadRunning());
	EXPECT_FALSE(GSConfig.BackThreadResolved);
}

// And back: leaving Unsynchronized with the split requested turns it on again.
TEST_F(GSSplitSeam, LeavingUnsynchronizedDownloadsTurnsTheSplitOn)
{
	BringUpSplit();

	Pcsx2Config::GSOptions changed = GSConfig;
	changed.HWDownloadMode = GSHardwareDownloadMode::Unsynchronized;
	GSUpdateConfig(changed);
	changed = GSConfig;
	changed.HWDownloadMode = GSHardwareDownloadMode::Enabled;
	GSUpdateConfig(changed);
	m_gs = nullptr;

	EXPECT_TRUE(g_gs_front);
	ASSERT_TRUE(g_gs_renderer);
	EXPECT_TRUE(g_gs_renderer->IsBackThreadRunning());
}

// SubmitVsync decides whether the frame was idle from the back's draw and transfer serials. On
// a single object every draw of the frame has executed by then. On the split the answer is only
// right after the back has run everything the frame queued.
TEST_F(GSSplitSeamStalled, IdleFrameIsDecidedAfterTheBackCatchesUp)
{
	BringUpStalled();
	ASSERT_TRUE(m_stall->IsIdleFrame());

	StallTheBack();
	Sprite(*g_gs_front, 0, 0);

	std::thread opener = OpenGateLater();
	m_stall->SubmitVsync(0, false);
	opener.join();

	EXPECT_EQ(m_stall->m_vsyncs, 1);
	EXPECT_FALSE(m_stall->m_idle_frame);
}

// A draw into the displayed buffer counts as a display blit (internal frame-rate detection and
// skip-duplicate-frames). A single object tests it against DISPFB as it stands when the draw is
// flushed. MTGS rewrites the privileged registers at the next vsync packet, before that vsync
// drains the back, so the back must not read them live.
TEST_F(GSSplitSeamStalled, DisplayBlitIsCountedAgainstTheRegistersAtFlush)
{
	BringUpStalled();
	m_priv_regs->PMODE.EN1 = 1;
	m_priv_regs->DISP[0].DISPFB.FBP = 0x10;
	g_perfmon.GetDisplayFramebufferSpriteBlits(); // reset the counter

	StallTheBack();
	Sprite(*g_gs_front, 0x10, 0);

	// The next frame's registers arrive while the draw is still queued: the game flipped buffers.
	m_priv_regs->DISP[0].DISPFB.FBP = 0x80;

	m_stall->m_gate.store(true, std::memory_order_release);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_perfmon.GetDisplayFramebufferSpriteBlits(), 1);
}

// The same flip in the other direction: a draw into what was not the displayed buffer when it was
// flushed does not count, whatever DISPFB says by the time the back runs it.
TEST_F(GSSplitSeamStalled, DisplayBlitIsNotCountedForABufferDisplayedLater)
{
	BringUpStalled();
	m_priv_regs->PMODE.EN1 = 1;
	m_priv_regs->DISP[0].DISPFB.FBP = 0x80;
	g_perfmon.GetDisplayFramebufferSpriteBlits();

	StallTheBack();
	Sprite(*g_gs_front, 0x10, 0);

	m_priv_regs->DISP[0].DISPFB.FBP = 0x10;

	m_stall->m_gate.store(true, std::memory_order_release);
	g_gs_front->DrainBackQueue();

	EXPECT_EQ(g_perfmon.GetDisplayFramebufferSpriteBlits(), 0);
}

namespace
{
	/// A bare parser object, to set up draw-buffer slots directly.
	class DrawBufferProbe final : public GSState
	{
	public:
		void Draw() override {}

		// Grows slot `i` once past the size every slot starts at.
		void GrowSlot(int i)
		{
			m_vertex = &m_vertex_buffers[i];
			m_index = &m_index_buffers[i];
			GrowVertexBuffer();
		}

		// Slot 0 empty, slot 1 current and holding `count` vertices and indices.
		void PendInSecondSlot(u32 count)
		{
			m_used_buffers_idx = 2;
			m_current_buffer_idx = 1;
			m_vertex_buffers[0].head = m_vertex_buffers[0].tail = m_vertex_buffers[0].next = 0;
			m_index_buffers[0].tail = 0;

			GSVertexBuff& vb = m_vertex_buffers[1];
			GSIndexBuff& ib = m_index_buffers[1];
			for (u32 n = 0; n < count; n++)
			{
				std::memset(&vb.buff[n], 0, sizeof(GSVertex));
				vb.buff[n].XYZ.Z = n;
				ib.buff[n] = static_cast<u16>(n);
			}
			vb.head = 0;
			vb.tail = count;
			vb.next = count;
			ib.tail = count;
		}

		const GSVertexBuff& VertexSlot(int i) const { return m_vertex_buffers[i]; }
		const GSIndexBuff& IndexSlot(int i) const { return m_index_buffers[i]; }
	};
} // namespace

// With draw buffering on, ResetDrawBufferIdx moves a pending draw from a later slot down into an
// empty earlier one. The earlier slot's arrays can be smaller than what is pending (slots grow on
// their own, and the split's flush trades a slot's arrays for a pool node's), so the move must
// not copy into them. After the move the first slot holds the draw in arrays large enough for it.
TEST(GSDrawBufferCompaction, MovesAPendingDrawLargerThanTheEmptySlot)
{
	DrawBufferProbe probe;
	probe.GrowSlot(1);
	const u32 small = probe.VertexSlot(0).maxcount;
	ASSERT_GT(probe.VertexSlot(1).maxcount, small);

	const u32 count = small + 100;
	probe.PendInSecondSlot(count);
	probe.ResetDrawBufferIdx();

	const auto& vb = probe.VertexSlot(0);
	ASSERT_EQ(vb.tail, count);
	EXPECT_GE(vb.maxcount, count);
	EXPECT_EQ(probe.IndexSlot(0).tail, count);
	EXPECT_EQ(vb.buff[count - 1].XYZ.Z, count - 1);
	EXPECT_EQ(probe.IndexSlot(0).buff[count - 1], static_cast<u16>(count - 1));
}
