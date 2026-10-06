// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the write stamps of GSLocalMemory: the sequence, the page sets a mark names, and that the
// writers inside GSLocalMemory (the host-to-local image writers behind GSState::ExecTransferRecord's
// mark, GSLocalMemory::Move and the four rectangle readback writers) leave no changed page unmarked.
//
// The texture hash memo reuses a hash while no page it read has been written since, so the one thing
// that must hold is that a store into local memory is never missed. The page sets are therefore checked
// against what the writers really store, not against the page looper's own idea of a rectangle.

#include "gs_write_stamp_watch.h"

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

using GSWriteStamps::Watch;

namespace
{
	// Every format a surface can have in local memory.
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	class GSLocalMemoryWriteStamps : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_mem = std::make_unique<GSLocalMemory>();
			m_rng.seed(1234);
		}

		u32 Random(u32 n) { return static_cast<u32>(m_rng() % n); }

		void FillRandom()
		{
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 4)
			{
				const u32 v = static_cast<u32>(m_rng());
				std::memcpy(m_mem->m_vm8 + i, &v, 4);
			}
		}

		static std::vector<u32> Range(u32 first, u32 last)
		{
			std::vector<u32> v;
			for (u32 i = first; i <= last; i++)
				v.push_back(i);
			return v;
		}

		/// The pages that carry the stamp of the latest mark, in order.
		std::vector<u32> LatestMark() const
		{
			std::vector<u32> pages;
			for (u32 page = 0; page < GS_MAX_PAGES; page++)
			{
				if (m_mem->PageStamp(page) == m_mem->WriteSeq())
					pages.push_back(page);
			}
			return pages;
		}

		// bw counts 64-pixel columns; 8 and 4-bit pages are 128 wide and take it in pairs.
		static u32 RandomBw(std::mt19937_64& rng, u32 psm)
		{
			const u32 n = 1 + static_cast<u32>(rng() % 16);
			return GSLocalMemory::m_psm[psm].pgs.x == 128 ? n * 2 : n;
		}

		std::unique_ptr<GSLocalMemory> m_mem;
		std::mt19937_64 m_rng;
	};

	TEST_F(GSLocalMemoryWriteStamps, AFreshMemoryHasNoStampsAndASequenceThatIsNotBelowThem)
	{
		EXPECT_EQ(m_mem->WriteSeq(), 1u);
		for (u32 page = 0; page < GS_MAX_PAGES; page++)
			ASSERT_EQ(m_mem->PageStamp(page), 0u) << "page " << page;
	}

	TEST_F(GSLocalMemoryWriteStamps, EveryMarkIsOneStepAndItsPagesShareTheStamp)
	{
		const u64 seq0 = m_mem->WriteSeq();

		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 10, PSMCT32), GSVector4i(0, 0, 640, 448));
		EXPECT_EQ(m_mem->WriteSeq(), seq0 + 1);
		EXPECT_EQ(LatestMark().size(), 140u); // 10 x 14 pages, however many pages that is

		m_mem->MarkPageRangeWritten(200, 3);
		EXPECT_EQ(m_mem->WriteSeq(), seq0 + 2);
		EXPECT_EQ(LatestMark(), Range(200, 202));

		m_mem->MarkAllPagesWritten();
		EXPECT_EQ(m_mem->WriteSeq(), seq0 + 3);
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
	}

	TEST_F(GSLocalMemoryWriteStamps, ALaterMarkLeavesEarlierStampsAlone)
	{
		m_mem->MarkPageRangeWritten(10, 4);
		const u64 first = m_mem->WriteSeq();
		m_mem->MarkPageRangeWritten(12, 4);
		const u64 second = m_mem->WriteSeq();

		EXPECT_EQ(m_mem->PageStamp(10), first);
		EXPECT_EQ(m_mem->PageStamp(11), first);
		EXPECT_EQ(m_mem->PageStamp(12), second);
		EXPECT_EQ(m_mem->PageStamp(15), second);
		EXPECT_EQ(m_mem->PageStamp(16), 0u);
		EXPECT_EQ(m_mem->PageStamp(9), 0u);
	}

	TEST_F(GSLocalMemoryWriteStamps, AStampIsNotBelowTheSequenceNotedBeforeTheWrite)
	{
		// What a reader relies on: it notes the sequence, reads, and later asks whether any page it read
		// has a stamp past the note. A write after the note is past it; a write before is not.
		m_mem->MarkPageRangeWritten(5, 1);
		const u64 noted = m_mem->WriteSeq();
		EXPECT_LE(m_mem->PageStamp(5), noted);

		m_mem->MarkPageRangeWritten(5, 1);
		EXPECT_GT(m_mem->PageStamp(5), noted);
	}

	TEST_F(GSLocalMemoryWriteStamps, ARangeWrapsAtTheEndOfLocalMemory)
	{
		m_mem->MarkPageRangeWritten(510, 5);
		EXPECT_EQ(LatestMark(), (std::vector<u32>{0, 1, 2, 510, 511}));

		m_mem->MarkPageRangeWritten(GS_MAX_PAGES + 7, 2); // a first page past the end wraps too
		EXPECT_EQ(LatestMark(), (std::vector<u32>{7, 8}));
	}

	TEST_F(GSLocalMemoryWriteStamps, AnEmptyRangeMarksNothingAndTakesNoStep)
	{
		const u64 seq = m_mem->WriteSeq();
		m_mem->MarkPageRangeWritten(100, 0);
		EXPECT_EQ(m_mem->WriteSeq(), seq);
	}

	TEST_F(GSLocalMemoryWriteStamps, ARangeOfEveryPageOrMoreMarksAllOfThem)
	{
		m_mem->MarkPageRangeWritten(37, GS_MAX_PAGES);
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
		m_mem->MarkPageRangeWritten(0, GS_MAX_PAGES + 100);
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
	}

	TEST_F(GSLocalMemoryWriteStamps, ARectangleMarksTheOnePageItIsIn)
	{
		// 32-bit pages are 64 x 32 pixels, and page n starts at block 32 * n.
		m_mem->MarkPagesWritten(m_mem->GetOffset(3 * 32, 1, PSMCT32), GSVector4i(0, 0, 64, 32));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{3}));

		m_mem->MarkPagesWritten(m_mem->GetOffset(3 * 32, 1, PSMCT32), GSVector4i(10, 5, 20, 9));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{3}));
	}

	TEST_F(GSLocalMemoryWriteStamps, ARectangleMarksItsPagesAlongTheRowAndDownTheBufferWidth)
	{
		// A 640 x 448 frame at block 0 is 10 pages wide and 14 high, and a row of pages is bw pages.
		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 10, PSMCT32), GSVector4i(0, 0, 640, 448));
		EXPECT_EQ(LatestMark(), Range(0, 139));

		// Two pages in, one page row down, three pages wide, two high.
		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 10, PSMCT32), GSVector4i(128, 32, 320, 96));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{12, 13, 14, 22, 23, 24}));
	}

	TEST_F(GSLocalMemoryWriteStamps, ARectangleOfEachFormatMarksItsOwnPageSize)
	{
		// 16-bit pages are 64 x 64, 8 and 4-bit pages are 128 x 64 and 128 x 128.
		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 1, PSMCT16), GSVector4i(0, 0, 64, 64));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{0}));

		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 2, PSMT8), GSVector4i(0, 0, 128, 128));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{0, 1}));

		m_mem->MarkPagesWritten(m_mem->GetOffset(0, 2, PSMT4), GSVector4i(0, 0, 128, 128));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{0}));
	}

	TEST_F(GSLocalMemoryWriteStamps, ARectangleThatRunsPastTheEndOfLocalMemoryWrapsToTheStart)
	{
		// Page 511 and the page below it in a buffer one page wide: the second is page 512, which is page 0.
		m_mem->MarkPagesWritten(m_mem->GetOffset(511 * 32, 1, PSMCT32), GSVector4i(0, 0, 64, 64));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{0, 511}));
	}

	TEST_F(GSLocalMemoryWriteStamps, ARectangleThatReaches2048MarksEveryPage)
	{
		const GSOffset off = m_mem->GetOffset(0, 32, PSMCT32);

		// Right up to 2048 is an ordinary rectangle...
		m_mem->MarkPagesWritten(off, GSVector4i(1984, 0, 2048, 32));
		EXPECT_EQ(LatestMark(), (std::vector<u32>{31}));
		m_mem->MarkPagesWritten(off, GSVector4i(0, 2016, 64, 2048));
		EXPECT_LT(LatestMark().size(), 4u);

		// ...one pixel more is not, in either direction. (Move wraps x there and the other writers do not.)
		m_mem->MarkPagesWritten(off, GSVector4i(1984, 0, 2049, 32));
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
		m_mem->MarkPagesWritten(off, GSVector4i(0, 0, 64, 2049));
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
		m_mem->MarkPagesWritten(off, GSVector4i(2040, 100, 4000, 200));
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
	}

	TEST_F(GSLocalMemoryWriteStamps, AnEmptyInvertedOrNegativeRectangleMarksEveryPage)
	{
		const GSOffset off = m_mem->GetOffset(0, 10, PSMCT32);
		const GSVector4i bad[] = {
			GSVector4i(10, 10, 10, 20), // no width
			GSVector4i(10, 10, 20, 10), // no height
			GSVector4i(30, 10, 20, 20), // inverted in x
			GSVector4i(10, 30, 20, 20), // inverted in y
			GSVector4i(-1, 0, 64, 32),
			GSVector4i(0, -1, 64, 32),
		};
		for (const GSVector4i& r : bad)
		{
			const u64 seq = m_mem->WriteSeq();
			m_mem->MarkPagesWritten(off, r);
			EXPECT_EQ(m_mem->WriteSeq(), seq + 1);
			EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES)) << r.x << "," << r.y << " " << r.z << "," << r.w;
		}
	}

	// The page set against the addresses of the pixels: the pages a rectangle's pixels live in, taken pixel by
	// pixel from the same address arithmetic every writer uses, are all in the set a mark covers.
	TEST_F(GSLocalMemoryWriteStamps, ThePagesOfARectangleHoldEveryPixelOfIt)
	{
		u32 exact = 0, larger = 0;
		for (int i = 0; i < 1500; i++)
		{
			const u32 psm = kPsms[Random(std::size(kPsms))];
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
			const u32 bp = Random(GS_MAX_BLOCKS);
			const u32 bw = RandomBw(m_rng, psm);
			const int x0 = static_cast<int>(Random(300)), y0 = static_cast<int>(Random(300));
			const GSVector4i r(x0, y0, x0 + 1 + static_cast<int>(Random(200)), y0 + 1 + static_cast<int>(Random(200)));
			const GSOffset off = m_mem->GetOffset(bp, bw, psm);

			// The address is in pixels of the format, and 4-bit pixels are addressed by nibble.
			const u32 pixels_per_page = static_cast<u32>(p.pgs.x * p.pgs.y);
			std::vector<bool> holds(GS_MAX_PAGES, false);
			for (int y = r.top; y < r.bottom; y++)
			{
				const GSOffset::PAHelper pa = off.paMulti(0, y);
				for (int x = r.left; x < r.right; x++)
					holds[(pa.value(x) / pixels_per_page) % GS_MAX_PAGES] = true;
			}

			std::vector<bool> marked(GS_MAX_PAGES, false);
			off.loopPages(r, [&](u32 page) { marked[page] = true; });

			bool same = true;
			for (u32 page = 0; page < GS_MAX_PAGES; page++)
			{
				ASSERT_TRUE(!holds[page] || marked[page])
					<< "psm " << psm << " bp " << bp << " bw " << bw << " rect " << r.x << "," << r.y << " " << r.z << "," << r.w
					<< ": a pixel is in page " << page << ", which the page set leaves out";
				same &= (holds[page] == marked[page]);
			}
			exact += same;
			larger += !same;
		}
		// Not asserted: how often the set has pages that hold no pixel of the rectangle.
		RecordProperty("exact_sets", static_cast<int>(exact));
		RecordProperty("larger_sets", static_cast<int>(larger));
	}

	// The page set of a rectangle against what the host-to-local writers really store: whatever bytes
	// change, a mark of the rectangle (one pixel larger on the right and bottom, as ExecTransferRecord
	// marks it) covers them. Random buffers at any block address, including ones that are not page
	// aligned and ones whose rows run off the end of local memory.
	TEST_F(GSLocalMemoryWriteStamps, TheRectangleOfAnImageWriteCoversEveryPageItChanges)
	{
		u32 cases = 0;
		for (int i = 0; i < 1500; i++)
		{
			const u32 psm = kPsms[Random(std::size(kPsms))];
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
			const u32 bp = Random(GS_MAX_BLOCKS);
			const u32 bw = RandomBw(m_rng, psm);
			const int x0 = static_cast<int>(Random(300)), y0 = static_cast<int>(Random(300));
			const int w = 1 + static_cast<int>(Random(200)), h = 1 + static_cast<int>(Random(200));

			GIFRegBITBLTBUF blit = {};
			blit.DBP = bp;
			blit.DBW = bw;
			blit.DPSM = psm;
			GIFRegTRXPOS pos = {};
			pos.DSAX = x0;
			pos.DSAY = y0;
			GIFRegTRXREG reg = {};
			reg.RRW = w;
			reg.RRH = h;

			const size_t len = (static_cast<size_t>(w) * h * p.trbpp + 7) / 8;
			std::vector<u8> data(len, 0xFF);

			std::memset(m_mem->m_vm8, 0, GSLocalMemory::m_vmsize);
			const Watch watch(*m_mem);
			int tx = x0, ty = y0;
			p.wi(*m_mem, tx, ty, data.data(), static_cast<int>(len), blit, pos, reg);

			m_mem->MarkPagesWritten(m_mem->GetOffset(bp, bw, psm), GSVector4i(x0, y0, x0 + w + 1, y0 + h + 1));

			ASSERT_TRUE(watch.UnmarkedChangedPages().empty())
				<< "psm " << psm << " bp " << bp << " bw " << bw << " rect " << x0 << "," << y0 << " " << w << "x" << h
				<< ": unmarked " << Watch::Describe(watch.UnmarkedChangedPages());
			cases += !watch.ChangedPages().empty();
		}
		EXPECT_GT(cases, 1400u) << "the cases did not write";
	}

	// Why the rectangle is grown by a pixel: a 4-bit write of odd width stores the second pixel of its last
	// pair of a row one pixel past the right edge. Here that pixel is the first column of the next page.
	TEST_F(GSLocalMemoryWriteStamps, AnOddWidthFourBitImageWriteStoresOnePixelPastItsRightEdge)
	{
		constexpr u32 psm = PSMT4HL, bp = 10249, bw = 12;
		constexpr int x0 = 287, y0 = 268, w = 153, h = 164; // right edge at 440, which starts a page
		const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];

		GIFRegBITBLTBUF blit = {};
		blit.DBP = bp;
		blit.DBW = bw;
		blit.DPSM = psm;
		GIFRegTRXPOS pos = {};
		pos.DSAX = x0;
		pos.DSAY = y0;
		GIFRegTRXREG reg = {};
		reg.RRW = w;
		reg.RRH = h;
		const size_t len = (static_cast<size_t>(w) * h * p.trbpp + 7) / 8;
		std::vector<u8> data(len, 0xFF);

		const Watch watch(*m_mem);
		int tx = x0, ty = y0;
		p.wi(*m_mem, tx, ty, data.data(), static_cast<int>(len), blit, pos, reg);

		// The page set of the exact rectangle leaves out a page the write changed...
		const GSOffset off = m_mem->GetOffset(bp, bw, psm);
		m_mem->MarkPagesWritten(off, GSVector4i(x0, y0, x0 + w, y0 + h));
		EXPECT_FALSE(watch.UnmarkedChangedPages().empty());

		// ...and the grown one does not.
		m_mem->MarkPagesWritten(off, GSVector4i(x0, y0, x0 + w + 1, y0 + h + 1));
		EXPECT_TRUE(watch.UnmarkedChangedPages().empty());
	}

	// The same question for a transfer that arrives in slices. A slice after the first starts below the
	// top of the rectangle, so only the rectangle of the whole transfer covers it.
	TEST_F(GSLocalMemoryWriteStamps, TheRectangleOfASlicedImageWriteCoversEverySlice)
	{
		for (int i = 0; i < 300; i++)
		{
			const u32 psm = kPsms[Random(std::size(kPsms))];
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
			const u32 bp = Random(GS_MAX_BLOCKS);
			const u32 bw = RandomBw(m_rng, psm);
			const int x0 = static_cast<int>(Random(200)), y0 = static_cast<int>(Random(200));
			const int w = 1 + static_cast<int>(Random(150)), h = 8 + static_cast<int>(Random(200));

			GIFRegBITBLTBUF blit = {};
			blit.DBP = bp;
			blit.DBW = bw;
			blit.DPSM = psm;
			GIFRegTRXPOS pos = {};
			pos.DSAX = x0;
			pos.DSAY = y0;
			GIFRegTRXREG reg = {};
			reg.RRW = w;
			reg.RRH = h;

			const size_t total = (static_cast<size_t>(w) * h * p.trbpp + 7) / 8;
			std::vector<u8> data(total, 0xFF);

			std::memset(m_mem->m_vm8, 0, GSLocalMemory::m_vmsize);
			const Watch watch(*m_mem);

			// Three slices, the first two a whole number of quadwords as the GIF delivers them.
			int tx = x0, ty = y0;
			size_t at = 0;
			for (int slice = 0; slice < 3; slice++)
			{
				const size_t n = slice == 2 ? total - at : std::min(total - at, 16 * (1 + static_cast<size_t>(Random(static_cast<u32>(total / 32 + 1)))));
				GIFRegBITBLTBUF b = blit;
				GIFRegTRXPOS ps = pos;
				GIFRegTRXREG rg = reg;
				p.wi(*m_mem, tx, ty, data.data() + at, static_cast<int>(n), b, ps, rg);
				at += n;
			}

			m_mem->MarkPagesWritten(m_mem->GetOffset(bp, bw, psm), GSVector4i(x0, y0, x0 + w + 1, y0 + h + 1));

			ASSERT_TRUE(watch.UnmarkedChangedPages().empty())
				<< "psm " << psm << " bp " << bp << " bw " << bw << " rect " << x0 << "," << y0 << " " << w << "x" << h
				<< ": unmarked " << Watch::Describe(watch.UnmarkedChangedPages());
		}
	}

	// Move marks its own destination: nothing calls it with a rectangle to mark.
	TEST_F(GSLocalMemoryWriteStamps, MoveMarksEveryPageItChanges)
	{
		FillRandom();
		u32 cases = 0;
		for (int i = 0; i < 600; i++)
		{
			const u32 spsm = kPsms[Random(std::size(kPsms))];
			// Mostly the same format both sides, sometimes a cross-format copy.
			const u32 dpsm = Random(4) ? spsm : kPsms[Random(std::size(kPsms))];

			GIFRegBITBLTBUF blit = {};
			blit.SBP = Random(GS_MAX_BLOCKS);
			blit.SBW = RandomBw(m_rng, spsm);
			blit.SPSM = spsm;
			blit.DBP = Random(GS_MAX_BLOCKS);
			blit.DBW = RandomBw(m_rng, dpsm);
			blit.DPSM = dpsm;

			GIFRegTRXPOS pos = {};
			pos.SSAX = Random(300);
			pos.SSAY = Random(300);
			// Now and then a destination whose x wraps at 2048, which no rectangle describes.
			pos.DSAX = Random(8) ? Random(300) : 2048 - Random(40);
			pos.DSAY = Random(300);
			pos.DIRX = Random(2);
			pos.DIRY = Random(2);

			GIFRegTRXREG reg = {};
			reg.RRW = 1 + Random(150);
			reg.RRH = 1 + Random(100);

			const Watch watch(*m_mem);
			m_mem->Move(blit, pos, reg);

			ASSERT_TRUE(watch.UnmarkedChangedPages().empty())
				<< "psm " << spsm << "->" << dpsm << " dbp " << blit.DBP << " dbw " << blit.DBW << " dest " << pos.DSAX << "," << pos.DSAY
				<< " " << reg.RRW << "x" << reg.RRH << ": unmarked " << Watch::Describe(watch.UnmarkedChangedPages());
			cases += !watch.ChangedPages().empty();
		}
		EXPECT_GT(cases, 500u) << "the cases did not write";
	}

	TEST_F(GSLocalMemoryWriteStamps, MoveOfNothingMarksNothing)
	{
		GIFRegBITBLTBUF blit = {};
		blit.SBW = blit.DBW = 1;
		GIFRegTRXPOS pos = {};
		GIFRegTRXREG reg = {};
		reg.RRW = 0;
		reg.RRH = 10;

		const u64 seq = m_mem->WriteSeq();
		m_mem->Move(blit, pos, reg);
		EXPECT_EQ(m_mem->WriteSeq(), seq);
	}

	TEST_F(GSLocalMemoryWriteStamps, MoveThatWrapsAt2048MarksEveryPage)
	{
		FillRandom();
		GIFRegBITBLTBUF blit = {};
		blit.SBW = blit.DBW = 32;
		blit.DBP = 0x2000;
		GIFRegTRXPOS pos = {};
		pos.DSAX = 2040;
		GIFRegTRXREG reg = {};
		reg.RRW = 16;
		reg.RRH = 4;

		m_mem->Move(blit, pos, reg);
		EXPECT_EQ(m_mem->WriteSeq(), 2u);
		EXPECT_EQ(LatestMark().size(), static_cast<size_t>(GS_MAX_PAGES));
	}

	// The readback writers take the surface and the rectangle themselves.
	TEST_F(GSLocalMemoryWriteStamps, TheRectangleReadbackWritersMarkEveryPageTheyChange)
	{
		FillRandom();
		std::vector<u32> src(300 * 300);
		for (u32& v : src)
			v = static_cast<u32>(m_rng());
		u8* const bits = reinterpret_cast<u8*>(src.data());
		constexpr u32 pitch = 300 * 4;

		u32 cases = 0;
		for (int i = 0; i < 800; i++)
		{
			const u32 kind = Random(5);
			static constexpr u32 psm32[] = {PSMCT32, PSMZ32};
			static constexpr u32 psm24[] = {PSMCT24, PSMZ24};
			static constexpr u32 psm16[] = {PSMCT16, PSMCT16S, PSMZ16, PSMZ16S};
			u32 psm = psm16[Random(4)];
			if (kind < 2)
				psm = psm32[Random(2)];
			else if (kind == 2)
				psm = psm24[Random(2)];
			const u32 bp = Random(GS_MAX_BLOCKS);
			const u32 bw = 1 + Random(16);
			const int x0 = static_cast<int>(Random(150)), y0 = static_cast<int>(Random(150));
			const GSVector4i r(x0, y0, x0 + 1 + static_cast<int>(Random(140)), y0 + 1 + static_cast<int>(Random(140)));
			const GSOffset off = m_mem->GetOffset(bp, bw, psm);

			const Watch watch(*m_mem);
			switch (kind)
			{
				case 0:
					m_mem->WritePixel32(bits, pitch, off, r);
					break;
				case 1:
					m_mem->WritePixel32(bits, pitch, off, r, 0x00ff00ffu);
					break;
				case 2:
					m_mem->WritePixel24(bits, pitch, off, r);
					break;
				case 3:
					m_mem->WritePixel16(bits, pitch, off, r);
					break;
				default:
					m_mem->WriteFrame16(bits, pitch, off, r);
					break;
			}

			ASSERT_TRUE(watch.UnmarkedChangedPages().empty())
				<< "writer " << kind << " psm " << psm << " bp " << bp << " bw " << bw << " rect " << r.x << "," << r.y << " " << r.z << "," << r.w
				<< ": unmarked " << Watch::Describe(watch.UnmarkedChangedPages());
			cases += !watch.ChangedPages().empty();
		}
		EXPECT_GT(cases, 700u) << "the cases did not write";
	}

	// A raw store that nothing marks shows up as unmarked: the watch itself has to be able to fail.
	TEST_F(GSLocalMemoryWriteStamps, AStoreThatIsNotMarkedIsSeenByTheWatch)
	{
		const Watch watch(*m_mem);
		m_mem->m_vm8[5 * GS_PAGE_SIZE + 100] = 0x5a;
		m_mem->m_vm8[9 * GS_PAGE_SIZE] = 0x5a;
		m_mem->MarkPageRangeWritten(9, 1);
		EXPECT_EQ(watch.UnmarkedChangedPages(), (std::vector<u32>{5}));
	}
} // namespace
