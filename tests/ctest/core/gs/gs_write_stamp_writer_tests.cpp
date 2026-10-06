// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Every path through which the hardware renderer changes GS local memory leaves the write stamps of
// the pages it changed ahead of where they were: no store goes unmarked.
//
// The texture hash memo (HashCacheKey::Create) reuses a hash while no page it read has been marked
// since. Several of these writers store through a raw pointer and bypass GSLocalMemory's own
// primitives, and the texture cache invalidation around them is conditional or covers a smaller
// rectangle than the store, so each is driven here through the renderer the way a game reaches it, with
// local memory compared page by page before and after.
//
// Each case also checks that the writer really ran (that pages changed), so a draw that takes another
// road does not pass for nothing.

#include "gs_write_stamp_drivers.h"

#include "SaveState.h"

using namespace GSWriteStamps;

namespace
{
	class GSWriteStampWriters : public WriterFixture
	{
	};

	class GSWriteStampSwPrimRender : public SwPrimFixture
	{
	};

	// --- Host to local transfers: GSState::ExecTransferRecord ----------------------------------------

	TEST_F(GSWriteStampWriters, ATransferInOnePiece)
	{
		BringUp();
		const Watch watch(m_gs->m_mem);

		Upload up(*m_gs, 0x100, 1, PSMCT32, 0, 0, 64, 64);
		up.Data(static_cast<u32>(Upload::Size(PSMCT32, 64, 64) / 16));

		EXPECT_EQ(watch.ChangedPages().size(), 2u); // 64 x 64 of 32 bits is two pages
		ExpectCovered(watch, "one-piece transfer");
	}

	TEST_F(GSWriteStampWriters, ATransferOfEachFormatAtAnUnalignedBlockAddress)
	{
		BringUp();
		static constexpr u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
		for (const u32 psm : psms)
		{
			Fill(0);
			const Watch watch(m_gs->m_mem);

			const int w = 133, h = 71; // odd, so the 4-bit writers meet their awkward case
			Upload up(*m_gs, 0x123, 4, psm, 5, 3, w, h);
			up.Data(static_cast<u32>(Upload::Size(psm, w, h) / 16));

			EXPECT_FALSE(watch.ChangedPages().empty()) << "psm " << psm << ": the transfer did not write";
			ExpectCovered(watch, ("transfer, psm " + std::to_string(psm)).c_str());
		}
	}

	// The rectangle the transfer reports to the texture cache is cut down for a slice that is not the last
	// to the rows the slice's own bytes would fill from the top of the rectangle, so a middle slice, which
	// writes rows further down, is reported short. The stamps use the whole rectangle.
	TEST_F(GSWriteStampWriters, ATransferInSlicesMarksTheRowsOfEverySlice)
	{
		BringUp();
		const Watch watch(m_gs->m_mem);

		// 64 x 128 of 32 bits is 32 KiB, four pages one above the other, cut as 8 KiB, 8 KiB and the rest.
		Upload up(*m_gs, 0, 1, PSMCT32, 0, 0, 64, 128);
		up.Data(512);
		up.CutSlice();
		EXPECT_EQ(watch.ChangedPages().size(), 1u);
		ExpectCovered(watch, "first slice");

		up.Data(512);
		up.CutSlice();
		EXPECT_EQ(watch.ChangedPages().size(), 2u);
		ExpectCovered(watch, "middle slice");

		up.Data(1024);
		EXPECT_EQ(watch.ChangedPages().size(), 4u);
		ExpectCovered(watch, "last slice");
	}

	TEST_F(GSWriteStampWriters, ATransferOnTheBackThreadMarksTheRenderersMemory)
	{
		GSConfig.BackThread = true;
		GSConfig.BackThreadResolved = true;
		m_device_api = RenderAPI::Vulkan;
		BringUp();
		ASSERT_TRUE(m_gs->IsBackThreadRunning());
		g_gs_front = std::make_unique<GSFrontState>(m_gs);
		g_gs_front->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
		g_gs_front->ResetPCRTC();

		const Watch watch(m_gs->m_mem);

		Upload up(*g_gs_front, 0x200, 2, PSMCT32, 0, 0, 128, 64);
		up.Data(static_cast<u32>(Upload::Size(PSMCT32, 128, 64) / 16));
		g_gs_front->DrainBackQueue();

		EXPECT_FALSE(watch.ChangedPages().empty());
		ExpectCovered(watch, "transfer through the pipelined split");
	}

	// --- Whole memory: GSState::Defrost --------------------------------------------------------------

	TEST_F(GSWriteStampWriters, LoadingASavedStateMarksEveryPage)
	{
		BringUp();
		Fill(0x11);

		freezeData fd = {};
		ASSERT_EQ(m_gs->Freeze(&fd, true), 0);
		std::vector<u8> state(static_cast<size_t>(fd.size));
		fd.data = state.data();
		ASSERT_EQ(m_gs->Freeze(&fd, false), 0);

		Fill(0x22); // as if the game had gone on and written, unmarked
		const Watch watch(m_gs->m_mem);

		ASSERT_EQ(m_gs->Defrost(&fd), 0);

		EXPECT_EQ(watch.ChangedPages().size(), static_cast<size_t>(GS_MAX_PAGES));
		EXPECT_EQ(watch.MarkedPages().size(), static_cast<size_t>(GS_MAX_PAGES));
		ExpectCovered(watch, "state load");
	}

	// --- The render target clear on the CPU: GSRendererHW::ClearGSLocalMemory ------------------------

	TEST_F(GSWriteStampWriters, ClearingAWholeFrameWritesWholePages)
	{
		BringUp();
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		ClearDraw(*m_gs, PSMCT32, 0, 0, 640, 448, false);

		EXPECT_EQ(watch.ChangedPages().size(), 140u) << "the draw did not clear the frame in local memory";
		ExpectCovered(watch, "page-aligned clear, 32 bits");
	}

	TEST_F(GSWriteStampWriters, ClearingAWholeSixteenBitFrameWritesWholePages)
	{
		BringUp();
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		ClearDraw(*m_gs, PSMCT16, 0, 0, 640, 448, false);

		EXPECT_FALSE(watch.ChangedPages().empty()) << "the draw did not clear the frame in local memory";
		ExpectCovered(watch, "page-aligned clear, 16 bits");
	}

	TEST_F(GSWriteStampWriters, ClearingRowsThatEndShortOfAPageWritesThemPixelByPixel)
	{
		BringUp();
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		// 447 rows: the page fast path covers the 13 pages of rows above 416, and the last 31 rows are written pixel by pixel.
		ClearDraw(*m_gs, PSMCT32, 0, 0, 640, 447, false);

		EXPECT_FALSE(watch.ChangedPages().empty()) << "the draw did not clear the frame in local memory";
		ExpectCovered(watch, "clear with a pixel-by-pixel tail");
	}

	TEST_F(GSWriteStampWriters, ClearingTheDepthBufferWritesWholePages)
	{
		BringUp();
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		ClearDraw(*m_gs, PSMCT32, 0, 0, 640, 448, true);

		EXPECT_FALSE(watch.ChangedPages().empty()) << "the draw did not clear in local memory";
		ExpectCovered(watch, "depth clear");
	}

	// --- The hacks that draw on the CPU: SwSpriteRender and OI_PointListPalette ----------------------

	TEST_F(GSWriteStampWriters, ASpriteDrawnOnTheCpu)
	{
		BringUp();
		UseHack("OI_DBZBTGames"); // draws its 16 x 16 and 64 x 64 sprites at the origin in software
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		SpriteDraw(*m_gs, 0, 0, 16, 16, 0);

		EXPECT_FALSE(watch.ChangedPages().empty()) << "the sprite was not drawn on the CPU";
		ExpectCovered(watch, "SwSpriteRender");
	}

	TEST_F(GSWriteStampWriters, APointListDrawnOnTheCpu)
	{
		BringUp();
		UseHack("OI_PointListPalette");
		Fill(0xAA);
		const Watch watch(m_gs->m_mem);

		Packet p;
		Environment(p, 0, PSMCT32);
		// The last points sit on the right and bottom edge of the draw, and in the next page. The draw
		// takes x == right and y == bottom, so they are written.
		p.Vertex(10 << 4, 5 << 4, 0, 0, 0);
		p.Vertex(30 << 4, 9 << 4, 0, 0, 0);
		p.Vertex(64 << 4, 31 << 4, 0, 0, 0);
		p.Vertex(63 << 4, 32 << 4, 0, 0, 0);
		GIFRegPRIM prim = {};
		prim.PRIM = GS_POINTLIST;
		p.Send(*m_gs, prim);

		EXPECT_FALSE(watch.ChangedPages().empty()) << "the points were not drawn on the CPU";
		ExpectCovered(watch, "OI_PointListPalette");
	}

	// --- The software scanline core: GSSwPrimRenderFunctions::Run ------------------------------------

	TEST_F(GSWriteStampSwPrimRender, ATileThroughTheRasterizer)
	{
		BringUp();
		m_gs->GetSwPrimState().palette_block_copy = false;
		FillRandom();
		const Watch watch(m_gs->m_mem);
		const double copies = g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies);

		Tile(0x40);

		EXPECT_EQ(g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies), copies);
		EXPECT_FALSE(watch.ChangedPages().empty()) << "the draw did not go through the rasterizer";
		ExpectCovered(watch, "rasterizer");
	}

	TEST_F(GSWriteStampSwPrimRender, ATileThroughThePaletteBlockCopy)
	{
		BringUp();
		m_gs->GetSwPrimState().palette_block_copy = true;
		FillRandom();
		const Watch watch(m_gs->m_mem);
		const double copies = g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies);

		Tile(0x40);

		EXPECT_GT(g_perfmon.GetCounter(GSPerfMon::SwPaletteBlockCopies), copies) << "the draw did not take the block copy";
		EXPECT_FALSE(watch.ChangedPages().empty());
		ExpectCovered(watch, "palette block copy");
	}
} // namespace
