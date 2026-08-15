// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the GS local-to-local transfer engine
// (GSLocalMemory::Move).
//
// The gs-mem hardware capture (SCPH-30001, 2026-08-11) measured what the GS
// actually does with TRXDIR=2, and none of the textbook answers fit: not a
// plain forward walk, not snapshot semantics, not honouring the direction
// field conditionally. The measured mechanism, the only one that explains all
// 163,840 words of the capture's local-to-local section:
//
//   - The copy is staged through a 128-PIXEL buffer: read 128 pixels in walk
//     order, write them, then read the next 128. Overlap inside a chunk
//     behaves like a snapshot; overlap across a chunk boundary smears by
//     exactly one chunk. The unit is pixels at every depth -- 512 bytes at
//     32bpp, 256 at 16, 128 at 8 -- which the combs below separate.
//   - TRXPOS.DIR is honoured literally, in both axes, overlap or not.
//   - A cross-format copy moves the low min(src,dst) bits of each pixel and
//     leaves the rest of the destination pixel alone. A 16->32 copy keeps the
//     destination's top sixteen bits (measured; NOT a zero-extend).
//
// The comb expectations below are hard-coded from the capture's own numbers
// (destination row v receives source row v except at chunk-boundary rows,
// where it receives the row before). The harder overlap geometries are pinned
// against an in-test transcription of the capture decoder's winning model,
// which silicon validated on every overlap geometry, all four DIR settings,
// four widths and three depths. The two layers cross-check each other: the
// combs would catch a mistranscribed reference, the reference covers the
// geometries with no closed-form pattern.

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
GIFRegBITBLTBUF Blit(u32 sbp, u32 sbw, u32 spsm, u32 dbp, u32 dbw, u32 dpsm)
{
	GIFRegBITBLTBUF b = {};
	b.SBP = sbp;
	b.SBW = sbw;
	b.SPSM = spsm;
	b.DBP = dbp;
	b.DBW = dbw;
	b.DPSM = dpsm;
	return b;
}

GIFRegTRXPOS Pos(u32 ssax, u32 ssay, u32 dsax, u32 dsay, u32 dirx, u32 diry)
{
	GIFRegTRXPOS p = {};
	p.SSAX = ssax;
	p.SSAY = ssay;
	p.DSAX = dsax;
	p.DSAY = dsay;
	p.DIRX = dirx;
	p.DIRY = diry;
	return p;
}

GIFRegTRXREG Reg(u32 w, u32 h)
{
	GIFRegTRXREG r = {};
	r.RRW = w;
	r.RRH = h;
	return r;
}

// Per-format seed, injective in y for any fixed x over the row spans used here,
// so a copy that lands one row wrong always reads as a different value. The
// 32-bit seed also varies its top byte, so the 24-bit-preserving paths are
// exercised on non-constant data.
u32 SeedValue(u32 psm, int x, int y)
{
	switch (GSLocalMemory::m_psm[psm].trbpp)
	{
		case 32:
			return ((0x80u + ((x + y) & 0x7f)) << 24) | ((y & 0xfff) << 12) | (x & 0xfff);
		case 24:
			return ((y & 0xfff) << 12) | (x & 0xfff);
		case 16:
			return (y * 29 + x * 3 + 7) & 0xffff;
		case 8:
			return (y * 7 + x + 3) & 0xff;
		default:
			return (y * 5 + x + 1) & 0xf;
	}
}

void SeedRect(GSLocalMemory* mem, u32 psm, u32 bp, u32 bw, const GSVector4i& r)
{
	const GSLocalMemory::psm_t& fmt = GSLocalMemory::m_psm[psm];
	for (int y = r.top; y < r.bottom; y++)
		for (int x = r.left; x < r.right; x++)
			(mem->*fmt.wp)(x, y, SeedValue(psm, x, y), bp, bw);
}

u32 Pixel(GSLocalMemory* mem, u32 psm, u32 bp, u32 bw, int x, int y)
{
	return (mem->*GSLocalMemory::m_psm[psm].rp)(x, y, bp, bw);
}

// Transcription of the capture decoder's winning model (simulate_banded with
// chunk_px=128), deliberately built on the x/y pixel accessors rather than the
// GSOffset machinery the engine uses, so the two computations share nothing
// but the swizzle truth the capture certified.
void ReferenceBandedMove(GSLocalMemory* mem, const GIFRegBITBLTBUF& BITBLTBUF, const GIFRegTRXPOS& TRXPOS, const GIFRegTRXREG& TRXREG)
{
	const GSLocalMemory::psm_t& spsm = GSLocalMemory::m_psm[BITBLTBUF.SPSM];
	const GSLocalMemory::psm_t& dpsm = GSLocalMemory::m_psm[BITBLTBUF.DPSM];
	const u32 move_bits = std::min(spsm.trbpp, dpsm.trbpp);
	const u32 mask = (move_bits >= 32) ? 0xffffffffu : ((1u << move_bits) - 1);
	const int w = TRXREG.RRW;
	const int h = TRXREG.RRH;

	std::vector<std::pair<int, int>> order;
	order.reserve(static_cast<size_t>(w) * h);
	for (int vy = 0; vy < h; vy++)
	{
		const int v = TRXPOS.DIRY ? (h - 1 - vy) : vy;
		for (int ux = 0; ux < w; ux++)
		{
			const int u = TRXPOS.DIRX ? (w - 1 - ux) : ux;
			order.emplace_back(u, v);
		}
	}

	constexpr size_t kChunk = 128;
	u32 held[kChunk];
	for (size_t start = 0; start < order.size(); start += kChunk)
	{
		const size_t n = std::min(kChunk, order.size() - start);
		for (size_t i = 0; i < n; i++)
		{
			const auto [u, v] = order[start + i];
			held[i] = (mem->*spsm.rp)(TRXPOS.SSAX + u, TRXPOS.SSAY + v, BITBLTBUF.SBP, BITBLTBUF.SBW) & mask;
		}
		for (size_t i = 0; i < n; i++)
		{
			const auto [u, v] = order[start + i];
			const u32 keep = (mem->*dpsm.rp)(TRXPOS.DSAX + u, TRXPOS.DSAY + v, BITBLTBUF.DBP, BITBLTBUF.DBW) & ~mask;
			(mem->*dpsm.wp)(TRXPOS.DSAX + u, TRXPOS.DSAY + v, keep | held[i], BITBLTBUF.DBP, BITBLTBUF.DBW);
		}
	}
}

class LocalMemoryMoveTest : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
	{
		s_mem = new GSLocalMemory();
		s_ref = new GSLocalMemory();
	}

	static void TearDownTestSuite()
	{
		delete s_ref;
		delete s_mem;
		s_mem = nullptr;
		s_ref = nullptr;
	}

	void SetUp() override
	{
		std::memset(s_mem->m_vm8, 0, GSLocalMemory::m_vmsize);
		std::memset(s_ref->m_vm8, 0, GSLocalMemory::m_vmsize);
	}

	// Runs the engine and the reference model on identically seeded twins and
	// requires the whole 4 MB to agree, so stray writes anywhere are caught.
	static void MoveBothAndCompare(const GIFRegBITBLTBUF& b, const GIFRegTRXPOS& p, const GIFRegTRXREG& r)
	{
		s_mem->Move(b, p, r);
		ReferenceBandedMove(s_ref, b, p, r);
		EXPECT_EQ(std::memcmp(s_mem->m_vm8, s_ref->m_vm8, GSLocalMemory::m_vmsize), 0)
			<< "engine and console-validated banded model disagree";
	}

	// The comb: copy w x 32 one row DOWN over itself. The console's buffered
	// copy makes destination row v receive source row v everywhere except at
	// v = chunk_rows, 2*chunk_rows, ..., which receive row v-1 -- where
	// chunk_rows = 128 / w. Hard-coded from the capture's RESULT.
	static void RunDownComb(u32 psm, u32 bw, int w, int chunk_rows)
	{
		constexpr int h = 32;
		const GSVector4i seeded(0, 0, w, h + 4);
		SeedRect(s_mem, psm, 0, bw, seeded);
		SeedRect(s_ref, psm, 0, bw, seeded);

		const GIFRegBITBLTBUF b = Blit(0, bw, psm, 0, bw, psm);
		const GIFRegTRXPOS p = Pos(0, 0, 0, 1, 0, 0);
		const GIFRegTRXREG r = Reg(w, h);

		s_mem->Move(b, p, r);

		for (int x = 0; x < w; x++)
			ASSERT_EQ(Pixel(s_mem, psm, 0, bw, x, 0), SeedValue(psm, x, 0)) << "row 0 is source-only and must not change, x=" << x;

		for (int row = 1; row <= h; row++)
		{
			const int v = row - 1;
			const int src_row = (v > 0 && (v % chunk_rows) == 0) ? (v - 1) : v;
			for (int x = 0; x < w; x++)
				ASSERT_EQ(Pixel(s_mem, psm, 0, bw, x, row), SeedValue(psm, x, src_row))
					<< "dest row " << row << " x=" << x << " should hold source row " << src_row;
		}

		for (int row = h + 1; row < h + 4; row++)
			for (int x = 0; x < w; x++)
				ASSERT_EQ(Pixel(s_mem, psm, 0, bw, x, row), SeedValue(psm, x, row)) << "guard row " << row << " must not change";

		// The same op through the reference model must agree with the
		// hard-coded pattern too -- this is what keeps the reference honest.
		ReferenceBandedMove(s_ref, b, p, r);
		EXPECT_EQ(std::memcmp(s_mem->m_vm8, s_ref->m_vm8, GSLocalMemory::m_vmsize), 0)
			<< "reference model disagrees with the hard-coded comb pattern";
	}

	static GSLocalMemory* s_mem;
	static GSLocalMemory* s_ref;
};

GSLocalMemory* LocalMemoryMoveTest::s_mem = nullptr;
GSLocalMemory* LocalMemoryMoveTest::s_ref = nullptr;

// --- The combs: the 128-pixel chunk, and that it is pixels, not bytes ---------

TEST_F(LocalMemoryMoveTest, Comb32Width32ChunksEveryFourRows)
{
	// 128 px / 32 px per row = 4 rows per chunk (capture: v = 4, 8, ..., 28).
	RunDownComb(PSMCT32, 1, 32, 4);
}

TEST_F(LocalMemoryMoveTest, Comb32Width64ChunksEveryTwoRows)
{
	RunDownComb(PSMCT32, 1, 64, 2);
}

TEST_F(LocalMemoryMoveTest, Comb16Width32ChunkIsPixelsNotBytes)
{
	// At 16bpp a 512-BYTE buffer would be 256 px = 8 rows; silicon says 4.
	RunDownComb(PSMCT16, 1, 32, 4);
}

TEST_F(LocalMemoryMoveTest, Comb8Width64ChunkIsPixelsNotBytes)
{
	// At 8bpp a 512-BYTE buffer would be 512 px = 8 rows; silicon says 2.
	RunDownComb(PSMT8, 2, 64, 2);
}

// --- The direction field is honoured literally --------------------------------

TEST_F(LocalMemoryMoveTest, ShiftUpWithReversedWalkSmearsFromTheBottom)
{
	// Copy one row UP with DIRY=1. Walking bottom-up makes the reads trail the
	// writes, so each chunk's first read (except the very first chunk's) lands
	// on a row an earlier chunk already stored: destination row v receives
	// source row v+2 instead of v+1 at v = 27, 23, ..., 3. Our old engine
	// refused the reversal here ("only reverse if the destination is behind
	// the source") and produced a clean copy -- the console does not.
	constexpr int w = 32, h = 32;
	SeedRect(s_mem, PSMCT32, 0, 1, GSVector4i(0, 0, w, h + 4));

	s_mem->Move(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), Pos(0, 1, 0, 0, 0, 1), Reg(w, h));

	for (int v = 0; v < h; v++)
	{
		const int src_row = ((v % 4) == 3 && v != h - 1) ? (v + 2) : (v + 1);
		for (int x = 0; x < w; x++)
			ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, x, v), SeedValue(PSMCT32, x, src_row))
				<< "dest row " << v << " x=" << x << " should hold source row " << src_row;
	}
	for (int x = 0; x < w; x++)
		ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, x, h), SeedValue(PSMCT32, x, h)) << "row " << h << " is source-only and must not change";
}

TEST_F(LocalMemoryMoveTest, ShiftDownWithReversedWalkIsClean)
{
	// Copy one row DOWN with DIRY=1: walking bottom-up, every read happens
	// before its row is overwritten, so the copy is exact everywhere. Pins the
	// reversal's sign -- an engine that applied DIR backwards would smear.
	constexpr int w = 32, h = 32;
	SeedRect(s_mem, PSMCT32, 0, 1, GSVector4i(0, 0, w, h + 4));

	s_mem->Move(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), Pos(0, 0, 0, 1, 0, 1), Reg(w, h));

	for (int x = 0; x < w; x++)
		ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, x, 0), SeedValue(PSMCT32, x, 0));
	for (int row = 1; row <= h; row++)
		for (int x = 0; x < w; x++)
			ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, x, row), SeedValue(PSMCT32, x, row - 1))
				<< "dest row " << row << " x=" << x;
}

TEST_F(LocalMemoryMoveTest, DisjointCopyIsExactUnderEveryDirection)
{
	// The capture's control: with no overlap the four DIR settings must be
	// indistinguishable, and the copy exact.
	constexpr int w = 32, h = 32;
	for (u32 d = 0; d < 4; d++)
	{
		SetUp();
		SeedRect(s_mem, PSMCT32, 0, 1, GSVector4i(0, 0, 64, h));

		s_mem->Move(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), Pos(0, 0, 32, 0, d & 1, (d >> 1) & 1), Reg(w, h));

		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, x, y), SeedValue(PSMCT32, x, y)) << "source changed, DIR=" << d;
				ASSERT_EQ(Pixel(s_mem, PSMCT32, 0, 1, 32 + x, y), SeedValue(PSMCT32, x, y)) << "bad copy, DIR=" << d;
			}
		}
	}
}

// --- Overlap geometries with no closed form: engine vs validated model --------

TEST_F(LocalMemoryMoveTest, OverlapDestAfterSourceBothAxesMatchesBandedModel)
{
	// The capture's (0,0)->(8,8) geometry, forward walk: the destination chases
	// the source through every chunk, so the smear compounds -- no closed-form
	// row pattern exists. 576 of 4096 words differed under the old engine.
	const GSVector4i seeded(0, 0, 48, 48);
	SeedRect(s_mem, PSMCT32, 0, 1, seeded);
	SeedRect(s_ref, PSMCT32, 0, 1, seeded);
	MoveBothAndCompare(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), Pos(0, 0, 8, 8, 0, 0), Reg(32, 32));
}

TEST_F(LocalMemoryMoveTest, OverlapDestBeforeSourceReversedMatchesBandedModel)
{
	// (8,8)->(0,0) with each reversed-walk setting the capture separated.
	for (const u32 dir : {1u, 2u, 3u})
	{
		SetUp();
		const GSVector4i seeded(0, 0, 48, 48);
		SeedRect(s_mem, PSMCT32, 0, 1, seeded);
		SeedRect(s_ref, PSMCT32, 0, 1, seeded);
		MoveBothAndCompare(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), Pos(8, 8, 0, 0, dir & 1, (dir >> 1) & 1), Reg(32, 32));
	}
}

TEST_F(LocalMemoryMoveTest, ColumnCombsMatchBandedModel)
{
	// One column left/right: chunks span whole rows here, so the banded copy
	// comes out clean -- but a naive forward walk would smear the column, so
	// the case still separates models.
	for (const bool right : {true, false})
	{
		SetUp();
		const GSVector4i seeded(0, 0, 48, 48);
		SeedRect(s_mem, PSMCT32, 0, 1, seeded);
		SeedRect(s_ref, PSMCT32, 0, 1, seeded);
		const GIFRegTRXPOS p = right ? Pos(0, 0, 1, 0, 0, 0) : Pos(1, 0, 0, 0, 0, 0);
		MoveBothAndCompare(Blit(0, 1, PSMCT32, 0, 1, PSMCT32), p, Reg(32, 32));
	}
}

// --- Cross-format copies move min(src,dst) bits -------------------------------

TEST_F(LocalMemoryMoveTest, CrossFormat16To32KeepsDestinationHighBits)
{
	// The capture's PSMCT16->PSMCT32 op: the console writes the sixteen source
	// bits into the low half of the destination pixel and leaves the top half
	// alone. Zero-extending -- what we shipped -- was 1,024 words wrong.
	constexpr int w = 32, h = 32;
	SeedRect(s_mem, PSMCT16, 0, 1, GSVector4i(0, 0, w, h));
	SeedRect(s_mem, PSMCT32, 64, 1, GSVector4i(0, 0, w, h));

	s_mem->Move(Blit(0, 1, PSMCT16, 64, 1, PSMCT32), Pos(0, 0, 0, 0, 0, 0), Reg(w, h));

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			const u32 expected = (SeedValue(PSMCT32, x, y) & 0xffff0000) | SeedValue(PSMCT16, x, y);
			ASSERT_EQ(Pixel(s_mem, PSMCT32, 64, 1, x, y), expected) << "x=" << x << " y=" << y;
		}
	}
}

TEST_F(LocalMemoryMoveTest, CrossFormat32To16MovesTheLowHalf)
{
	constexpr int w = 32, h = 32;
	SeedRect(s_mem, PSMCT32, 0, 1, GSVector4i(0, 0, w, h));

	s_mem->Move(Blit(0, 1, PSMCT32, 64, 1, PSMCT16), Pos(0, 0, 0, 0, 0, 0), Reg(w, h));

	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			ASSERT_EQ(Pixel(s_mem, PSMCT16, 64, 1, x, y), SeedValue(PSMCT32, x, y) & 0xffff) << "x=" << x << " y=" << y;
}

TEST_F(LocalMemoryMoveTest, CrossFormat32To24KeepsDestinationTopByte)
{
	constexpr int w = 32, h = 32;
	SeedRect(s_mem, PSMCT32, 0, 1, GSVector4i(0, 0, w, h));
	SeedRect(s_mem, PSMCT32, 64, 1, GSVector4i(0, 0, w, h)); // seed the raw words under the 24-bit destination

	s_mem->Move(Blit(0, 1, PSMCT32, 64, 1, PSMCT24), Pos(0, 0, 0, 0, 0, 0), Reg(w, h));

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			const u32 expected = (SeedValue(PSMCT32, x, y) & 0xff000000) | (SeedValue(PSMCT32, x, y) & 0x00ffffff);
			// Read the raw 32-bit word so the preserved top byte is visible.
			ASSERT_EQ(Pixel(s_mem, PSMCT32, 64, 1, x, y), expected) << "x=" << x << " y=" << y;
		}
	}
}
} // namespace

// A draw and a transfer are ordered against each other by the set of pages each
// one claims, so a claim that is short of what the operation actually writes is
// a race -- and gs-clip measured the software renderer producing at least four
// different outputs, run to run, on the one stage whose draws reach past the
// frame buffer's own stride. The catalogue's suspected mechanism was exactly
// that: a footprint clamped at the buffer width, under-claiming the pages a wide
// rectangle reaches into.
//
// It is not that. This walks every pixel of rectangles up to sixteen times their
// buffer's stride and checks the claimed set against the written set directly,
// and the claim covers the writes everywhere. Kept as a pin rather than deleted:
// the refutation is the useful part, and a future clamp added for tidiness would
// silently reintroduce the mechanism this rules out.
//
// What it does NOT cover, and where the defect therefore still lives: the
// ordering DECISIONS taken over those page sets, the texture-page side, and
// anything above GS_MAX_PAGES where the looper switches to its de-duplicating
// slow path.
TEST(PageClaim, CoversWhatAWideRectangleWrites)
{
	for (int bw : {1, 2, 4, 10})
	{
		for (int right : {64, 200, 640, 1024, 2047})
		{
			const GSOffset off = GSOffset::fromKnownPSM(0, bw, PSMCT32);
			const GSVector4i r(0, 0, right, 40);

			std::vector<bool> claimed(GS_MAX_PAGES, false);
			off.pageLooperForRect(r).loopPages([&](u32 p) { claimed[p] = true; });

			std::vector<bool> written(GS_MAX_PAGES, false);
			for (int y = r.top; y < r.bottom; y++)
				for (int x = r.left; x < r.right; x++)
					written[((off.pa(x, y) * sizeof(u32)) / GS_PAGE_SIZE) % GS_MAX_PAGES] = true;

			int missing = 0;
			for (u32 p = 0; p < GS_MAX_PAGES; p++)
				missing += (written[p] && !claimed[p]);

			EXPECT_EQ(missing, 0) << "buffer width " << (bw * 64) << " px, rectangle " << right << " px wide";
		}
	}
}
