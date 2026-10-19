// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The host-to-local image writers of the 4-bit formats (PSMT4, PSMT4HL and PSMT4HH; GSLocalMemory::m_psm[psm].wi,
// which GSState::ExecTransferRecord calls) against the console.
//
// The console (SCPH-30001, 2026-10-19; 459 cases per format) treats the bytes of a transfer into any of the three
// formats as one stream of nibbles, the low nibble of each byte first. Nibble n goes to pixel
// (DSAX + n mod RRW, DSAY + n div RRW). A row is not padded to a byte: on an odd width the last pixel of a row
// takes the low nibble of a byte and the first pixel of the next row takes its high nibble. The transfer ends when
// the bytes do or when the rectangle is full, so an odd pixel count drops its spare nibble, and nothing is stored
// outside the rectangle, nor in any other nibble of the same word. A transfer that arrives in pieces carries on
// where the last piece stopped. The console does this for every width and start x it was given, including odd widths
// and start x values that are not a multiple of 4, and it does not reject, stall or fault on any of them.
//
// A second capture (SCPH-30001, 2026-10-19; 521 transfers) asked the same of an odd start x with a rectangle wide enough
// for the PSMT4 writer's block-aligned path (a whole 32-pixel block column inside it), the one combination the first
// capture left open: the console stores it by the same rule, whole or in pieces, at any start x.
//
// Four layers:
//   - the console's own rows, 30 cases cut from the first capture, on all three formats, whole and in pieces;
//   - 14 more rows cut from the second capture, PSMT4 at an odd and an even start x, wide, whole and in pieces;
//   - the rule above as a property: random geometry, any width and start x, whole and cut into pieces of whole quadwords;
//   - a pin on the even widths, which the writers have always stored right: a hash over random transfers.

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{
	constexpr u32 kFourBitPsms[] = {PSMT4, PSMT4HL, PSMT4HH};

	const char* PsmName(u32 psm)
	{
		switch (psm)
		{
			case PSMT4:
				return "PSMT4";
			case PSMT4HL:
				return "PSMT4HL";
			default:
				return "PSMT4HH";
		}
	}

	/// Bw counts 64-pixel columns; PSMT4 pages are 128 wide and take it in pairs. The console probe used 2 and 1.
	u32 BwFor(u32 psm, u32 columns) { return psm == PSMT4 ? columns * 2 : columns; }

	/// A row of the table in gs_four_bit_stream_console_rows.inc.
	struct ConsoleRow
	{
		const char* name;
		int w, h, x0, y0;
		const int* pieces; ///< bytes in each piece of the transfer, in arrival order
		int npieces;
		const short* cells; ///< w * h entries: the source nibble the console stored in each pixel, or -1 for none
		int bw = 0; ///< the buffer width (DBW) the console used, or 0 for the narrowest buffer the format has
	};

#include "gs_four_bit_stream_console_rows.inc"
#include "gs_four_bit_stream_odd_x_console_rows.inc"

	struct Geometry
	{
		u32 psm, bp, bw;
		int x0, y0, w, h;

		/// The bytes the transfer expects: GSState::GSTransferBuffer::Update cuts what arrives past this.
		size_t Total() const { return (static_cast<size_t>(w) * h + 1) / 2; }

		std::string Describe() const
		{
			return std::string(PsmName(psm)) + " bp " + std::to_string(bp) + " bw " + std::to_string(bw) + " rect " + std::to_string(x0) + "," +
			       std::to_string(y0) + " " + std::to_string(w) + "x" + std::to_string(h);
		}
	};

	void WriteNibble(GSLocalMemory& mem, const Geometry& g, int x, int y, u32 v)
	{
		switch (g.psm)
		{
			case PSMT4: mem.WritePixel4(x, y, v, g.bp, g.bw); break;
			case PSMT4HL: mem.WritePixel4HL(x, y, v, g.bp, g.bw); break;
			case PSMT4HH: mem.WritePixel4HH(x, y, v, g.bp, g.bw); break;
			default: ADD_FAILURE() << "not a 4-bit format"; break;
		}
	}

	u32 ReadNibble(const GSLocalMemory& mem, const Geometry& g, int x, int y)
	{
		switch (g.psm)
		{
			case PSMT4: return mem.ReadPixel4(x, y, g.bp, g.bw);
			case PSMT4HL: return mem.ReadPixel4HL(x, y, g.bp, g.bw);
			default: return mem.ReadPixel4HH(x, y, g.bp, g.bw);
		}
	}

	/// The rule: nibble n of the bytes delivered so far goes to (x0 + n mod w, y0 + n div w), low nibble of each
	/// byte first, until the bytes or the rectangle run out.
	void ApplyRule(GSLocalMemory& mem, const Geometry& g, const u8* stream, size_t delivered)
	{
		const size_t nibbles = std::min(delivered * 2, static_cast<size_t>(g.w) * g.h);
		for (size_t n = 0; n < nibbles; n++)
			WriteNibble(mem, g, g.x0 + static_cast<int>(n % g.w), g.y0 + static_cast<int>(n / g.w), (stream[n >> 1] >> ((n & 1) << 2)) & 0xF);
	}

	/// Where the rule leaves the cursor after `delivered` bytes: at the next pixel to write.
	void RuleCursor(const Geometry& g, size_t delivered, int& tx, int& ty)
	{
		const size_t n = std::min(delivered * 2, static_cast<size_t>(g.w) * g.h);
		tx = g.x0 + static_cast<int>(n % g.w);
		ty = g.y0 + static_cast<int>(n / g.w);
	}

	u32 Mix(u32 x)
	{
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}

	/// Local memory in which no nibble is 0, as the console probe's ground was: a cell holds 0 after a transfer of
	/// zeros exactly if the transfer wrote it.
	const std::vector<u8>& Ground()
	{
		static const std::vector<u8> ground = [] {
			std::vector<u8> g(GSLocalMemory::m_vmsize);
			for (u32 i = 0; i < GSLocalMemory::m_vmsize / 4; i++)
			{
				const u32 h = Mix(i ^ 0x0badf00du);
				u32 word = 0;
				for (int j = 0; j < 8; j++)
					word |= (1u + ((h >> (4 * j)) & 15u) % 15u) << (4 * j);
				std::memcpy(&g[i * 4], &word, 4);
			}
			return g;
		}();
		return ground;
	}

	void LayGround(GSLocalMemory& mem) { std::memcpy(mem.m_vm8, Ground().data(), GSLocalMemory::m_vmsize); }

	/// Empty when the two memories are equal, else where they first differ.
	std::string FirstDifference(const GSLocalMemory& a, const GSLocalMemory& b)
	{
		if (std::memcmp(a.m_vm8, b.m_vm8, GSLocalMemory::m_vmsize) == 0)
			return {};
		u32 i = 0;
		while (a.m_vm8[i] == b.m_vm8[i])
			i++;
		u32 diff = 0;
		for (u32 j = i; j < GSLocalMemory::m_vmsize; j++)
			diff += a.m_vm8[j] != b.m_vm8[j];
		return "memory differs at byte " + std::to_string(i) + " (" + std::to_string(a.m_vm8[i]) + " against " + std::to_string(b.m_vm8[i]) + "), " +
		       std::to_string(diff) + " bytes in all";
	}

	/// One host-to-local transfer, delivered the way GSState delivers it: the writer is handed each piece in turn and
	/// the cursor it leaves is the cursor the next piece starts from.
	class Transfer
	{
	public:
		Transfer(GSLocalMemory& mem, const Geometry& g)
			: m_mem(mem)
			, m_g(g)
			, m_tx(g.x0)
			, m_ty(g.y0)
		{
		}

		/// A piece that would run past the end of the image is cut to what the transfer still expects (GSTransferBuffer::Update).
		/// The piece is copied to a buffer of exactly its size, so a read past its end is a read past an allocation.
		void Deliver(const u8* data, size_t len)
		{
			len = std::min(len, m_g.Total() - m_delivered);
			if (len == 0)
				return;
			const std::vector<u8> piece(data, data + len);

			GIFRegBITBLTBUF blit = {};
			blit.DBP = m_g.bp;
			blit.DBW = m_g.bw;
			blit.DPSM = m_g.psm;
			GIFRegTRXPOS pos = {};
			pos.DSAX = m_g.x0;
			pos.DSAY = m_g.y0;
			GIFRegTRXREG reg = {};
			reg.RRW = m_g.w;
			reg.RRH = m_g.h;
			GSLocalMemory::m_psm[m_g.psm].wi(m_mem, m_tx, m_ty, piece.data(), static_cast<int>(len), blit, pos, reg);
			m_delivered += len;
		}

		size_t Delivered() const { return m_delivered; }
		int Tx() const { return m_tx; }
		int Ty() const { return m_ty; }

	private:
		GSLocalMemory& m_mem;
		Geometry m_g;
		int m_tx, m_ty;
		size_t m_delivered = 0;
	};

	/// The console's stand-in for source nibble k in each of its runs: 0, k & 15, (k >> 4) & 15 and (k >> 8) & 15, so
	/// a cell names the nibble it holds up to 4095. The first capture ran the first three, the second all four.
	constexpr int kRuns = 4;
	u32 Stamp(int run, int k)
	{
		if (run == 0)
			return 0;
		return static_cast<u32>((k >> (4 * (run - 1))) & 15);
	}

	class GSLocalMemoryFourBitStream : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_mem = std::make_unique<GSLocalMemory>();
			m_other = std::make_unique<GSLocalMemory>();
			m_model = std::make_unique<GSLocalMemory>();
			m_rng.seed(20261019);
			LayGround(*m_mem);
			LayGround(*m_other);
			LayGround(*m_model);
		}

		u32 Random(u32 n) { return static_cast<u32>(m_rng() % n); }

		std::vector<u8> RandomBytes(size_t n)
		{
			std::vector<u8> v(n);
			for (u8& b : v)
				b = static_cast<u8>(m_rng());
			return v;
		}

		std::unique_ptr<GSLocalMemory> m_mem, m_other, m_model;
		std::mt19937_64 m_rng;
	};

	// ---------------------------------------------------------------------------------------------------------------
	// The console's rows.
	// ---------------------------------------------------------------------------------------------------------------

	// Each row is run once per stamp, as on the console: with every source nibble 0 (which cells were written) and with
	// nibble k = k & 15, (k >> 4) & 15 and (k >> 8) & 15 (which source nibble each written cell holds). The whole
	// of local memory must then equal the ground with exactly the console's cells replaced, so a cell the console
	// left alone, a nibble of the same word that the format does not own, and a cell filled from past the source all
	// show. Each row is delivered both ways: in the pieces the console received, one call each (a flush between two
	// pieces of a transfer), and as one call (GSState buffers a transfer unless something forces a flush).
	void RunConsoleRow(GSLocalMemory& mem, GSLocalMemory& model, const ConsoleRow& row, u32 psm)
	{
		const Geometry g{psm, 0x400, row.bw ? static_cast<u32>(row.bw) : BwFor(psm, 1), row.x0, row.y0, row.w, row.h};

		for (int run = 0; run < kRuns; run++)
		{
			std::vector<u8> stream;
			for (int p = 0; p < row.npieces; p++)
				stream.resize(stream.size() + row.pieces[p]);
			for (size_t j = 0; j < stream.size(); j++)
				stream[j] = static_cast<u8>(Stamp(run, static_cast<int>(2 * j)) | (Stamp(run, static_cast<int>(2 * j + 1)) << 4));

			LayGround(model);
			for (int y = 0; y < row.h; y++)
			{
				for (int x = 0; x < row.w; x++)
				{
					const int k = row.cells[y * row.w + x];
					if (k >= 0)
						WriteNibble(model, g, row.x0 + x, row.y0 + y, Stamp(run, k));
				}
			}

			for (const bool in_pieces : {true, false})
			{
				LayGround(mem);
				Transfer t(mem, g);
				if (in_pieces)
				{
					size_t at = 0;
					for (int p = 0; p < row.npieces; p++)
					{
						t.Deliver(stream.data() + at, static_cast<size_t>(row.pieces[p]));
						at += static_cast<size_t>(row.pieces[p]);
					}
				}
				else
				{
					t.Deliver(stream.data(), stream.size());
				}

				const std::string diff = FirstDifference(mem, model);
				ASSERT_TRUE(diff.empty()) << PsmName(psm) << " " << row.name << ", run " << run << (in_pieces ? ", in pieces" : ", in one call") << ": " << diff;
			}
		}
	}

	TEST_F(GSLocalMemoryFourBitStream, EveryCaseTheConsoleRanLandsWhereTheConsoleStoredIt)
	{
		for (const ConsoleRow& row : kConsoleRows)
			for (const u32 psm : kFourBitPsms)
				ASSERT_NO_FATAL_FAILURE(RunConsoleRow(*m_mem, *m_model, row, psm));
	}

	// The second capture: PSMT4 rectangles wide enough for the block-aligned path, at a start x that is odd (the
	// one combination the writer stored one nibble off, with the first pixel read from the byte before the source and
	// the last nibble dropped) and, as a control, at an even one. The buffer is two pages wide, so a rectangle can cross
	// x = 128.
	TEST_F(GSLocalMemoryFourBitStream, EveryWidePSMT4CaseTheConsoleRanFromAnOddStartXLandsWhereTheConsoleStoredIt)
	{
		for (const ConsoleRow& row : kOddXConsoleRows)
			ASSERT_NO_FATAL_FAILURE(RunConsoleRow(*m_mem, *m_model, row, PSMT4));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// By hand: the rule on the smallest transfers that show it.
	// ---------------------------------------------------------------------------------------------------------------

	TEST_F(GSLocalMemoryFourBitStream, AnOddWidthRowHandsTheOtherHalfOfItsLastByteToTheNextRow)
	{
		for (const u32 psm : kFourBitPsms)
		{
			const Geometry g{psm, 0x80, BwFor(psm, 2), 5, 3, 3, 2};
			const u8 bytes[] = {0x10, 0x32, 0x54}; // nibbles 0 1 2 3 4 5
			LayGround(*m_mem);
			Transfer t(*m_mem, g);
			t.Deliver(bytes, sizeof(bytes));

			// Three pixels a row: row 0 takes nibbles 0 1 2, row 1 takes 3 4 5. The last pixel of row 0 takes the low
			// half of the second byte and the first pixel of row 1 its high half.
			for (int i = 0; i < 6; i++)
				EXPECT_EQ(ReadNibble(*m_mem, g, 5 + i % 3, 3 + i / 3), static_cast<u32>(i)) << PsmName(psm) << " nibble " << i;
			EXPECT_EQ(t.Tx(), 5);
			EXPECT_EQ(t.Ty(), 5);
		}
	}

	TEST_F(GSLocalMemoryFourBitStream, AnOddPixelCountDropsItsSpareNibbleAndStoresNothingOutsideTheRectangle)
	{
		for (const u32 psm : kFourBitPsms)
		{
			// Seven pixels, so four bytes, and the high nibble of the last is spare.
			const Geometry g{psm, 0x80, BwFor(psm, 2), 5, 3, 7, 1};
			const u8 bytes[] = {0x10, 0x32, 0x54, 0xE6};
			LayGround(*m_mem);
			Transfer t(*m_mem, g);
			t.Deliver(bytes, sizeof(bytes));

			for (int i = 0; i < 7; i++)
				EXPECT_EQ(ReadNibble(*m_mem, g, 5 + i, 3), static_cast<u32>(i)) << PsmName(psm) << " nibble " << i;

			// The spare nibble (0xE) is stored nowhere: not past the right edge, not in the row below.
			LayGround(*m_model);
			for (int i = 0; i < 7; i++)
				WriteNibble(*m_model, g, 5 + i, 3, static_cast<u32>(i));
			EXPECT_EQ(FirstDifference(*m_mem, *m_model), "") << PsmName(psm);
			EXPECT_EQ(t.Tx(), 5);
			EXPECT_EQ(t.Ty(), 4);
		}
	}

	TEST_F(GSLocalMemoryFourBitStream, ATransferGivenMoreBytesThanItsRectangleHoldsStopsAtTheRectangle)
	{
		// GSState never hands a writer more than the transfer expects, but the writer stops at the rectangle all the same.
		// Only odd widths: the even widths take their rows from the length they are handed on the block-aligned paths.
		for (const u32 psm : kFourBitPsms)
		{
			const Geometry g{psm, 0x80, BwFor(psm, 2), 5, 3, 9, 3};
			const std::vector<u8> bytes = RandomBytes(64);
			LayGround(*m_mem);
			LayGround(*m_model);

			// Not through Transfer, which cuts the piece itself.
			GIFRegBITBLTBUF blit = {};
			blit.DBP = g.bp;
			blit.DBW = g.bw;
			blit.DPSM = psm;
			GIFRegTRXPOS pos = {};
			pos.DSAX = g.x0;
			pos.DSAY = g.y0;
			GIFRegTRXREG reg = {};
			reg.RRW = g.w;
			reg.RRH = g.h;
			int tx = g.x0, ty = g.y0;
			GSLocalMemory::m_psm[psm].wi(*m_mem, tx, ty, bytes.data(), static_cast<int>(bytes.size()), blit, pos, reg);

			ApplyRule(*m_model, g, bytes.data(), bytes.size());
			EXPECT_EQ(FirstDifference(*m_mem, *m_model), "") << PsmName(psm);
			EXPECT_EQ(tx, g.x0);
			EXPECT_EQ(ty, g.y0 + g.h);
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The rule as a property.
	// ---------------------------------------------------------------------------------------------------------------

	enum class Widths
	{
		Any,
		Odd,
		Even,
		WideFromOddX, ///< an even width of 34 to 160 at an odd start x: PSMT4's block-aligned path usually has a whole block column
	};

	Geometry RandomGeometry(std::mt19937_64& rng, u32 psm, Widths widths)
	{
		const auto random = [&](u32 n) { return static_cast<u32>(rng() % n); };
		Geometry g;
		g.psm = psm;
		g.bp = random(GS_MAX_BLOCKS);
		g.bw = BwFor(psm, 1 + random(6));
		g.x0 = static_cast<int>(random(100));
		g.y0 = static_cast<int>(random(60));
		// Mostly narrow, where the rows are short enough for the straddle to happen often; sometimes wide.
		g.w = 1 + static_cast<int>(random(4) ? random(24) : random(150));
		if (widths == Widths::Odd)
			g.w |= 1;
		else if (widths == Widths::Even)
			g.w = (g.w + 1) & ~1;
		else if (widths == Widths::WideFromOddX)
		{
			g.w = 34 + 2 * static_cast<int>(random(64));
			g.x0 |= 1;
		}
		g.h = 1 + static_cast<int>(random(3) ? random(12) : random(70));
		return g;
	}

	/// Cuts `total` bytes into pieces that are whole quadwords but for the last, as the GIF delivers them (an IMAGE tag
	/// is quadwords and a HWREG write is 64 bits, so every piece boundary is a whole number of bytes, and a multiple of
	/// 8 at that).
	std::vector<size_t> RandomPieces(std::mt19937_64& rng, size_t total)
	{
		std::vector<size_t> pieces;
		size_t at = 0;
		for (int more = static_cast<int>(rng() % 5); more > 0 && total - at >= 8; more--)
		{
			const size_t n = 8 * (1 + rng() % ((total - at) / 8));
			pieces.push_back(n);
			at += n;
		}
		if (at < total)
			pieces.push_back(total - at);
		return pieces;
	}

	// Whole in one call, in pieces, and the rule, for every width: all three must end with the same local memory,
	// byte for byte, and the cursor after each piece must be the next pixel of the stream. The memories carry on from
	// one case to the next, so a stray store of one case shows as a difference in a later one at the latest.
	void RunProperty(std::mt19937_64& rng, GSLocalMemory& whole_mem, GSLocalMemory& cut_mem, GSLocalMemory& model_mem, u32 psm, Widths widths, int cases)
	{
		LayGround(whole_mem);
		LayGround(cut_mem);
		LayGround(model_mem);

		for (int i = 0; i < cases; i++)
		{
			const Geometry g = RandomGeometry(rng, psm, widths);
			std::vector<u8> bytes(g.Total());
			for (u8& b : bytes)
				b = static_cast<u8>(rng());
			const std::vector<size_t> pieces = RandomPieces(rng, bytes.size());

			ApplyRule(model_mem, g, bytes.data(), bytes.size());

			Transfer whole(whole_mem, g);
			whole.Deliver(bytes.data(), bytes.size());

			Transfer cut(cut_mem, g);
			size_t at = 0;
			for (const size_t n : pieces)
			{
				cut.Deliver(bytes.data() + at, n);
				at += n;

				int want_x, want_y;
				RuleCursor(g, at, want_x, want_y);
				ASSERT_EQ(cut.Tx(), want_x) << g.Describe() << ": the cursor after " << at << " of " << bytes.size() << " bytes";
				ASSERT_EQ(cut.Ty(), want_y) << g.Describe() << ": the cursor after " << at << " of " << bytes.size() << " bytes";
			}

			int want_x, want_y;
			RuleCursor(g, bytes.size(), want_x, want_y);
			ASSERT_EQ(whole.Tx(), want_x) << g.Describe();
			ASSERT_EQ(whole.Ty(), want_y) << g.Describe();

			std::string pieces_text;
			for (const size_t n : pieces)
				pieces_text += " " + std::to_string(n);

			std::string diff = FirstDifference(whole_mem, model_mem);
			ASSERT_TRUE(diff.empty()) << g.Describe() << ", in one call: " << diff;
			diff = FirstDifference(cut_mem, model_mem);
			ASSERT_TRUE(diff.empty()) << g.Describe() << ", in pieces of" << pieces_text << " bytes: " << diff;
		}
	}

	TEST_F(GSLocalMemoryFourBitStream, ATransferOfAnyWidthIntoPSMT4IsOneStreamOfNibblesWholeOrInPieces)
	{
		RunProperty(m_rng, *m_mem, *m_other, *m_model, PSMT4, Widths::Any, 300);
	}

	TEST_F(GSLocalMemoryFourBitStream, ATransferOfAnyWidthIntoPSMT4HLIsOneStreamOfNibblesWholeOrInPieces)
	{
		RunProperty(m_rng, *m_mem, *m_other, *m_model, PSMT4HL, Widths::Any, 300);
	}

	TEST_F(GSLocalMemoryFourBitStream, ATransferOfAnyWidthIntoPSMT4HHIsOneStreamOfNibblesWholeOrInPieces)
	{
		RunProperty(m_rng, *m_mem, *m_other, *m_model, PSMT4HH, Widths::Any, 300);
	}

	// Odd widths only, for more of the interesting ones in the same time.
	TEST_F(GSLocalMemoryFourBitStream, AnOddWidthTransferIntoEachFourBitFormatIsOneStreamOfNibblesWholeOrInPieces)
	{
		for (const u32 psm : kFourBitPsms)
			RunProperty(m_rng, *m_mem, *m_other, *m_model, psm, Widths::Odd, 300);
	}

	// The even widths were always stored right from an even start x, so this one holds for the writers as they were
	// before the odd widths and the odd start x were repaired: it is what shows the rule and the way the tests above
	// apply it are the writers' own for every shape that never had a fault.
	TEST_F(GSLocalMemoryFourBitStream, AnEvenWidthTransferIntoEachFourBitFormatIsOneStreamOfNibblesWholeOrInPieces)
	{
		for (const u32 psm : kFourBitPsms)
			RunProperty(m_rng, *m_mem, *m_other, *m_model, psm, Widths::Even, 300);
	}

	// A wide even-width rectangle from an odd start x: PSMT4's block-aligned path used to read its source at byte x / 2
	// and store every pixel one nibble late. The console stores it by the same rule as every other transfer.
	TEST_F(GSLocalMemoryFourBitStream, AWideTransferFromAnOddStartXIntoEachFourBitFormatIsOneStreamOfNibblesWholeOrInPieces)
	{
		for (const u32 psm : kFourBitPsms)
			RunProperty(m_rng, *m_mem, *m_other, *m_model, psm, Widths::WideFromOddX, 300);
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The even widths: pinned to what they stored before the odd widths and the odd start x were repaired.
	// ---------------------------------------------------------------------------------------------------------------

	// A hash of local memory and of the cursors after 1500 random transfers of even width into each format, over
	// memory that is not zero, with the rectangle in one piece and in up to three pieces of whole quadwords. Some of
	// them take the block-aligned paths (a width and height that are multiples of 8, at a start that is). The values
	// were recorded from the writers as they were before the odd-width repair (master at 6799cf5c0a), so this fails if a
	// change to the 4-bit writers moves one even-width pixel or cursor.
	//
	// `even_start_only` clears the low bit of the start x of a PSMT4 rectangle 32 or more wide. That is the one
	// combination those writers stored wrong (every pixel one nibble late), so with it set the hash is still the value
	// recorded then; without it, the transfers that start at an odd x are in the hash and are stored as the console
	// stores them. The draws from the generator are the same either way: only the start x of those transfers moves.
	u64 EvenWidthHash(u32 psm, bool even_start_only = false)
	{
		std::mt19937_64 rng(psm * 7919 + 1);
		const auto random = [&](u32 n) { return static_cast<u32>(rng() % n); };

		auto mem = std::make_unique<GSLocalMemory>();
		LayGround(*mem);
		u64 hash = 0xcbf29ce484222325ull;
		const auto mix = [&](u64 v) { hash = (hash ^ v) * 0x100000001b3ull; };

		for (int i = 0; i < 1500; i++)
		{
			Geometry g;
			g.psm = psm;
			g.bp = random(2) ? random(GS_MAX_BLOCKS) : 32 * random(GS_MAX_BLOCKS / 32);
			g.bw = BwFor(psm, 1 + random(8));
			switch (random(4))
			{
				case 0: g.w = 2 * (1 + static_cast<int>(random(40))); break;
				case 1: g.w = 8 * (1 + static_cast<int>(random(12))); break;
				case 2: g.w = 32 * (1 + static_cast<int>(random(3))); break;
				default: g.w = 2 * (1 + static_cast<int>(random(6))); break;
			}
			g.h = random(3) ? 1 + static_cast<int>(random(48)) : 8 * (1 + static_cast<int>(random(8)));
			g.x0 = random(3) ? static_cast<int>(random(100)) : 8 * static_cast<int>(random(8));
			g.y0 = random(3) ? static_cast<int>(random(100)) : 8 * static_cast<int>(random(6));
			if (even_start_only && psm == PSMT4 && g.w >= 32)
				g.x0 &= ~1;

			std::vector<u8> bytes(g.Total());
			for (u8& b : bytes)
				b = static_cast<u8>(rng());

			Transfer t(*mem, g);
			size_t at = 0;
			const int pieces = 1 + static_cast<int>(random(3));
			for (int p = 0; p + 1 < pieces && at + 16 < bytes.size(); p++)
			{
				const size_t room = (bytes.size() - at) / 16;
				const size_t n = std::min<size_t>(16 * (1 + random(static_cast<u32>(std::max<size_t>(room, 1)))), bytes.size() - at);
				t.Deliver(bytes.data() + at, n);
				at += n;
				mix(static_cast<u64>(t.Tx()) << 32 | static_cast<u32>(t.Ty()));
			}
			t.Deliver(bytes.data() + at, bytes.size() - at);
			mix(static_cast<u64>(t.Tx()) << 32 | static_cast<u32>(t.Ty()));
		}

		for (u32 i = 0; i < GSLocalMemory::m_vmsize / 8; i++)
		{
			u64 word;
			std::memcpy(&word, mem->m_vm8 + i * 8, 8);
			mix(word);
		}
		return hash;
	}

	TEST_F(GSLocalMemoryFourBitStream, EveryEvenWidthTransferIsStoredAsItWasBeforeTheOddWidthsWereRepaired)
	{
		// The value the PSMT4 hash had when it left out the odd start x of the wide rectangles: unchanged by the repair of
		// that start x, which is the proof that no other transfer in the hash moved.
		EXPECT_EQ(EvenWidthHash(PSMT4, true), 5168402120249559249ull) << "PSMT4, wide rectangles from an even start x";
		EXPECT_EQ(EvenWidthHash(PSMT4HL), 17258870135605540776ull) << "PSMT4HL";
		EXPECT_EQ(EvenWidthHash(PSMT4HH), 10041492323775641200ull) << "PSMT4HH";
	}

	// The same transfers with the odd start x of the wide PSMT4 rectangles left in. They differ from the hash above only in
	// those transfers, and the value is recorded from the writers after the repair, which the console rows and the property
	// tests above show to be the console's rule.
	TEST_F(GSLocalMemoryFourBitStream, EveryEvenWidthPSMT4TransferIncludingTheOddStartXOfTheWideOnesIsPinned)
	{
		EXPECT_EQ(EvenWidthHash(PSMT4), 934949934108528768ull) << "PSMT4";
	}
} // namespace
