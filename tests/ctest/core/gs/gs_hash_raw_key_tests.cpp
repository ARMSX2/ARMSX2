// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the raw-block key of GSTextureCache::HashCacheKey::Create. A paletted window (T8, T4, T8H, T4HL,
// T4HH) of at least one block in each direction is keyed by the bytes of its blocks as they sit in local
// memory, in the order the unswizzle reads them, plus where the window starts inside its first block. The
// property a cache key needs, and the one these tests pin, is that two windows with the same key have the
// same texels: the texture cached under the key is the right one for both.
//
// The texels are what PSM::rtxP hands PreloadTexture. The key is allowed to be stricter than the texels (it
// reads the whole of the window's edge blocks, so a byte outside the window changes it), never looser.
//
// Only a window that used to be unswizzled to be hashed (it has a region, or the format shares its words with
// other data) changes. Everything else keeps the key it had, bit for bit: a window smaller than a block, a
// format that is not paletted, an unregioned 8 or 4-bit window (always hashed from its blocks, and the names
// of its texture pack files are made from that hash), and every lookup that asks for the texel hash (dump,
// replace, upscale).

#include "gs_write_stamp_drivers.h"

#include "GS/GSXXH.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <set>

using namespace GSWriteStamps;

namespace
{
	using HashCacheKey = GSTextureCache::HashCacheKey;
	using SourceRegion = GSTextureCache::SourceRegion;

	constexpr u32 kPaletted[] = {PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH};

	/// A texture window: the registers of the texture, and its region when it has one.
	struct Window
	{
		u32 psm = PSMT8;
		u32 tbp0 = 0; ///< In blocks.
		u32 tbw = 2; ///< In units of 64 texels.
		u32 tw = 8, th = 8; ///< Log2 of the size of the texture.
		bool has_x = false, has_y = false;
		int x0 = 0, y0 = 0, w = 0, h = 0; ///< The region.
	};

	int Log2(int v)
	{
		int n = 0;
		while ((1 << (n + 1)) <= v)
			n++;
		return n;
	}

	class GSHashRawKey : public SwPrimFixture
	{
	protected:
		void SetUp() override
		{
			SwPrimFixture::SetUp();
			GSTextureCache::SetHashMemoVerify(true);
		}

		void TearDown() override
		{
			GSTextureCache::SetHashMemoVerify(false);
			SwPrimFixture::TearDown();
		}

		static GIFRegTEX0 Tex0(const Window& win)
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = win.tbp0;
			tex0.TBW = win.tbw;
			tex0.PSM = win.psm;
			tex0.TW = win.tw;
			tex0.TH = win.th;
			return tex0;
		}

		static SourceRegion Region(const Window& win)
		{
			SourceRegion region = {};
			if (win.has_x)
				region.SetX(win.x0, win.x0 + win.w);
			if (win.has_y)
				region.SetY(win.y0, win.y0 + win.h);
			return region;
		}

		/// The texels of the window, relative to the base pointer.
		static GSVector4i Rect(const Window& win)
		{
			const SourceRegion region = Region(win);
			const int tw = region.HasX() ? region.GetWidth() : (1 << win.tw);
			const int th = region.HasY() ? region.GetHeight() : (1 << win.th);
			return region.GetRect(tw, th);
		}

		static GSVector4i BlockRect(const Window& win) { return Rect(win).ralign<Align_Outside>(GSLocalMemory::m_psm[win.psm].bs); }

		GSOffset Offset(const Window& win) const { return m_gs->m_mem.GetOffset(win.tbp0, win.tbw, win.psm); }

		/// The block numbers of the window, in the order the unswizzle reads them.
		std::vector<u32> Blocks(const Window& win) const
		{
			std::vector<u32> blocks;
			Offset(win).loopBlocks(BlockRect(win), [&](u32 bn) { blocks.push_back(bn); });
			return blocks;
		}

		HashCacheKey Key(const Window& win, bool raw_blocks, const GSVector2i* lod = nullptr) const
		{
			return HashCacheKey::Create(Tex0(win), GIFRegTEXA{}, nullptr, lod, Region(win), raw_blocks);
		}

		/// The texel indices of the window, one byte each: what PreloadTexture uploads.
		std::vector<u8> Texels(const Window& win)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[win.psm];
			const GSVector4i rect = Rect(win);
			const GSVector4i block_rect = BlockRect(win);
			const u32 pitch = VectorAlign(static_cast<u32>(block_rect.width()));

			std::vector<u8> storage(static_cast<size_t>(pitch) * block_rect.height() + 64);
			u8* const dst = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(storage.data()) + 63) & ~static_cast<uintptr_t>(63));
			psm.rtxP(m_gs->m_mem, Offset(win), block_rect, dst, pitch, GIFRegTEXA{});

			std::vector<u8> texels;
			for (int y = rect.top; y < rect.bottom; y++)
			{
				const u8* row = dst + static_cast<size_t>(pitch) * (y - block_rect.top) + (rect.left - block_rect.left);
				texels.insert(texels.end(), row, row + rect.width());
			}
			return texels;
		}

		/// The hash of the window as it was taken before raw keys: of its unswizzled texels, or, for a window
		/// that was never unswizzled to hash, of its blocks. Built here from the bytes.
		u64 TexelHash(const Window& win)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[win.psm];
			const SourceRegion region = Region(win);
			const GSVector4i rect = Rect(win);

			std::vector<u8> bytes;
			if (rect.width() < psm.bs.x || rect.height() < psm.bs.y || psm.fmsk != 0xFFFFFFFFu || region.GetMaxX() > 0 || region.GetMinY() > 0)
			{
				bytes = Texels(win);
			}
			else
			{
				for (const u32 bn : Blocks(win))
				{
					const u8* block = m_gs->m_mem.BlockPtr(bn);
					bytes.insert(bytes.end(), block, block + GS_BLOCK_SIZE);
				}
			}
			return XXH3_64bits(bytes.data(), bytes.size());
		}

		/// Whether the window is hashed differently by a raw lookup: paletted, a block or more each way (the
		/// random windows all are), and one that used to be unswizzled to hash.
		static bool ChangesWithRawKeys(const Window& win)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[win.psm];
			return psm.pal > 0 && (win.has_x || (win.has_y && win.y0 > 0) || psm.fmsk != 0xFFFFFFFFu);
		}

		/// The key a lookup got before raw keys existed, from the fields it is made of.
		bool MasterKeyMatches(const Window& win, const HashCacheKey& key)
		{
			HashCacheKey expected;
			expected.TEX0Hash = TexelHash(win);
			expected.CLUTHash = 0;
			expected.TEX0.U64 = Tex0(win).U64 & 0x00000003FFF00000ULL;
			expected.TEXA.U64 = 0;
			expected.region_width = static_cast<u32>(Region(win).GetWidth());
			expected.region_height = static_cast<u32>(Region(win).GetHeight());
			return expected == key;
		}

		static u32 TbwFor(std::mt19937& rng, const Window& win)
		{
			// The window fits in the buffer width, as a game's texture does, so no block is read twice.
			const int right = win.has_x ? win.x0 + win.w : (1 << win.tw);
			u32 tbw = static_cast<u32>((right + 63) / 64) + rng() % 3;
			if (GSLocalMemory::m_psm[win.psm].bpp != 32)
				tbw = (tbw + 1) & ~1u; // the page of an 8 or 4-bit format is 128 texels wide
			return tbw;
		}

		/// A window of the format of random position, size and region, at least a block in each direction.
		static Window RandomWindow(std::mt19937& rng, u32 psm)
		{
			const GSVector2i bs = GSLocalMemory::m_psm[psm].bs;
			Window win;
			win.psm = psm;
			win.tbp0 = (rng() % 512) * 32;
			win.tw = Log2(bs.x) + rng() % (9 - Log2(bs.x));
			win.th = Log2(bs.y) + rng() % (9 - Log2(bs.y));
			if (rng() & 1)
			{
				win.has_x = true;
				win.w = bs.x + static_cast<int>(rng() % (256 - bs.x));
				win.x0 = static_cast<int>(rng() % 300);
			}
			if (rng() & 1)
			{
				win.has_y = true;
				win.h = bs.y + static_cast<int>(rng() % (256 - bs.y));
				win.y0 = static_cast<int>(rng() % 300);
			}
			win.tbw = TbwFor(rng, win);
			return win;
		}

		/// Copies the blocks of `from` to a window of the same shape at another address and buffer width, so that
		/// the two read the same bytes in the same order from different places. False if the windows overlap or
		/// read a block twice, which a copy cannot be exact for.
		bool CopyElsewhere(std::mt19937& rng, const Window& from, Window& to)
		{
			to = from;
			to.tbp0 = (rng() % 512) * 32;
			to.tbw = TbwFor(rng, to);

			const std::vector<u32> src = Blocks(from);
			const std::vector<u32> dst = Blocks(to);
			if (src.size() != dst.size())
				return false;

			const std::set<u32> src_set(src.begin(), src.end());
			const std::set<u32> dst_set(dst.begin(), dst.end());
			if (src_set.size() != src.size() || dst_set.size() != dst.size())
				return false;
			for (const u32 bn : dst)
			{
				if (src_set.count(bn))
					return false;
			}

			for (size_t i = 0; i < src.size(); i++)
				std::memcpy(m_gs->m_mem.BlockPtr(dst[i]), m_gs->m_mem.BlockPtr(src[i]), GS_BLOCK_SIZE);
			m_gs->m_mem.MarkAllPagesWritten();
			return true;
		}

		/// Local memory made of one block, over and over, so that every window reads the same bytes in the same
		/// order wherever it is and whatever its buffer width.
		void FillUniform()
		{
			std::mt19937 rng(11);
			u8 block[GS_BLOCK_SIZE];
			for (u8& b : block)
				b = static_cast<u8>(rng());
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += GS_BLOCK_SIZE)
				std::memcpy(m_gs->m_mem.m_vm8 + i, block, GS_BLOCK_SIZE);
			m_gs->m_mem.MarkAllPagesWritten();
		}

		void Randomise()
		{
			FillRandom();
			m_gs->m_mem.MarkAllPagesWritten();
		}

		const GSTextureCache::HashMemoStats& Stats() const { return g_texture_cache->GetHashMemoStats(); }

		static void ExpectVerified()
		{
			const GSTextureCache::HashMemoVerifyReport report = GSTextureCache::GetHashMemoVerifyReport();
			EXPECT_EQ(report.mismatches, 0u);
			EXPECT_EQ(report.missed_writers, 0u);
		}
	};

	// --- The property ----------------------------------------------------------------------------------

	TEST_F(GSHashRawKey, AWindowCopiedToAnotherAddressHasTheSameKeyAndTexels)
	{
		BringUp();
		Randomise();
		std::mt19937 rng(1);

		u32 compared = 0;
		for (const u32 psm : kPaletted)
		{
			for (int trial = 0; trial < 200; trial++)
			{
				const Window a = RandomWindow(rng, psm);
				Window b;
				if (!CopyElsewhere(rng, a, b))
					continue;

				// Where the format shares its words with other data, the bits that are not its own differ.
				const u32 fmsk = GSLocalMemory::m_psm[psm].fmsk;
				if (fmsk != 0xFFFFFFFFu)
				{
					for (const u32 bn : Blocks(b))
					{
						u32* words = reinterpret_cast<u32*>(m_gs->m_mem.BlockPtr(bn));
						for (u32 i = 0; i < GS_BLOCK_SIZE / sizeof(u32); i++)
							words[i] = (words[i] & fmsk) | (static_cast<u32>(rng()) & ~fmsk);
					}
					m_gs->m_mem.MarkAllPagesWritten();
				}

				EXPECT_TRUE(Key(a, true) == Key(b, true)) << "psm " << psm << " trial " << trial;
				EXPECT_EQ(Texels(a), Texels(b)) << "psm " << psm << " trial " << trial;
				compared++;
			}
		}
		EXPECT_GT(compared, 400u);
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, EqualKeysHaveEqualTexelsWhateverIsChanged)
	{
		BringUp();
		Randomise();
		std::mt19937 rng(2);

		u32 same_key = 0, new_key = 0;
		for (const u32 psm : kPaletted)
		{
			const u32 fmsk = GSLocalMemory::m_psm[psm].fmsk;
			for (int trial = 0; trial < 200; trial++)
			{
				const Window a = RandomWindow(rng, psm);
				Window b;
				if (!CopyElsewhere(rng, a, b))
					continue;
				const HashCacheKey key_a = Key(a, true);
				const std::vector<u8> texels_a = Texels(a);

				// Change one byte of one block of the copy.
				const std::vector<u32> blocks = Blocks(b);
				const u32 bn = blocks[rng() % blocks.size()];
				const u32 byte = rng() % GS_BLOCK_SIZE;
				const u8 flip = static_cast<u8>(1 + rng() % 255);
				m_gs->m_mem.BlockPtr(bn)[byte] ^= flip;
				m_gs->m_mem.MarkAllPagesWritten();

				const bool same = Key(b, true) == key_a;
				if (same)
				{
					same_key++;
					EXPECT_EQ(Texels(b), texels_a) << "psm " << psm << " trial " << trial << ": same key, different texels";
				}
				else
				{
					new_key++;
				}

				// Every byte of a block counts, except the bits that are not the format's own.
				const u32 flipped_bits = static_cast<u32>(flip) << ((byte & 3) * 8);
				EXPECT_EQ(same, (flipped_bits & fmsk) == 0) << "psm " << psm << " trial " << trial << " byte " << byte << " flip " << static_cast<u32>(flip);
			}
		}

		// Both outcomes were reached, so neither half of the check was empty.
		EXPECT_GT(same_key, 0u);
		EXPECT_GT(new_key, 0u);
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, TheKeyReadsTheBlocksOfTheWindowAndNoOthers)
	{
		BringUp();
		Randomise();
		std::mt19937 rng(3);

		for (const u32 psm : kPaletted)
		{
			for (int trial = 0; trial < 50; trial++)
			{
				const Window win = RandomWindow(rng, psm);
				const std::vector<u32> blocks = Blocks(win);
				const std::set<u32> in_window(blocks.begin(), blocks.end());
				if (in_window.size() != blocks.size())
					continue;

				const HashCacheKey before = Key(win, true);
				u32 other;
				do
				{
					other = rng() % GS_MAX_BLOCKS;
				} while (in_window.count(other));

				u32* word = reinterpret_cast<u32*>(m_gs->m_mem.BlockPtr(other));
				word[rng() % (GS_BLOCK_SIZE / sizeof(u32))] ^= 0xFFFFFFFFu;
				m_gs->m_mem.MarkAllPagesWritten();
				EXPECT_TRUE(Key(win, true) == before) << "psm " << psm << " trial " << trial;
			}
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, ATexelOutsideTheWindowInAnEdgeBlockIsAMiss)
	{
		// The key is stricter than the texels: the window starts at x = 4, so the texel at x = 0 of the same
		// block is not in it, and the texels do not move when that one is written. The key does.
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8;
		win.tbp0 = 64;
		win.tbw = 4;
		win.tw = 8;
		win.th = 8;
		win.has_x = true;
		win.x0 = 4;
		win.w = 24;

		const HashCacheKey before = Key(win, true);
		const std::vector<u8> texels = Texels(win);

		m_gs->m_mem.m_vm8[Offset(win).pa(0, 0)] ^= 0x40;
		m_gs->m_mem.MarkAllPagesWritten();

		EXPECT_EQ(Texels(win), texels);
		EXPECT_FALSE(Key(win, true) == before);
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, WindowsThatDifferInOnlyOneBlockOfAnHFormatHaveOtherKeys)
	{
		// The H formats mask each block before it is hashed. A block that is lost or hashed twice makes
		// windows that differ in that block alone share a key, so every position gets a turn.
		BringUp();
		Randomise();
		for (const u32 psm : {PSMT8H, PSMT4HL, PSMT4HH})
		{
			Window win;
			win.psm = psm;
			win.tbp0 = 160;
			win.tbw = 1;
			win.tw = 6;
			win.th = 6;

			const std::vector<u32> blocks = Blocks(win);
			ASSERT_EQ(blocks.size(), 64u);
			const u32 fmsk = GSLocalMemory::m_psm[psm].fmsk;
			const HashCacheKey before = Key(win, true);
			ASSERT_EQ(before.TEX0.U64 & 1, 1u);

			for (size_t k = 0; k < blocks.size(); k++)
			{
				u32* words = reinterpret_cast<u32*>(m_gs->m_mem.BlockPtr(blocks[k]));
				const u32 bit = fmsk & (0u - fmsk);

				// A bit of the format, in the first word of the block and in the last.
				for (const u32 word : {0u, static_cast<u32>(GS_BLOCK_SIZE / sizeof(u32) - 1)})
				{
					words[word] ^= bit;
					m_gs->m_mem.MarkAllPagesWritten();
					EXPECT_FALSE(Key(win, true) == before) << "psm " << psm << " block " << k << " word " << word;
					words[word] ^= bit;
					m_gs->m_mem.MarkAllPagesWritten();
				}

				// A bit that is not the format's own.
				words[1] ^= ~fmsk & (0u - ~fmsk);
				m_gs->m_mem.MarkAllPagesWritten();
				EXPECT_TRUE(Key(win, true) == before) << "psm " << psm << " block " << k << ": a bit outside the format";
				words[1] ^= ~fmsk & (0u - ~fmsk);
				m_gs->m_mem.MarkAllPagesWritten();
			}
			EXPECT_TRUE(Key(win, true) == before) << "psm " << psm << ": the memory was not put back";
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, WindowsThatDifferInOnlyOneBlockOfAnEightBitFormatHaveOtherKeys)
	{
		BringUp();
		Randomise();
		for (const u32 psm : {PSMT8, PSMT4})
		{
			Window win;
			win.psm = psm;
			win.tbp0 = 160;
			win.tbw = 2;
			win.tw = 7;
			win.th = 6;
			win.has_x = true;
			win.x0 = 0;
			win.w = 100; // a region, so the window is hashed from raw blocks, unlike the same window without one

			const std::vector<u32> blocks = Blocks(win);
			ASSERT_GT(blocks.size(), 8u);
			const HashCacheKey before = Key(win, true);
			ASSERT_EQ(before.TEX0.U64 & 1, 1u);
			for (size_t k = 0; k < blocks.size(); k++)
			{
				u8* block = m_gs->m_mem.BlockPtr(blocks[k]);
				block[17] ^= 0x01;
				m_gs->m_mem.MarkAllPagesWritten();
				EXPECT_FALSE(Key(win, true) == before) << "psm " << psm << " block " << k;
				block[17] ^= 0x01;
				m_gs->m_mem.MarkAllPagesWritten();
			}
		}
		ExpectVerified();
	}

	// --- Where the window starts ----------------------------------------------------------------------

	TEST_F(GSHashRawKey, TheOffsetInTheFirstBlockIsInTheKey)
	{
		// Every block holds the same bytes, so windows of one shape read the same stream of blocks wherever
		// they are. Which texels they are is told by where they start inside the first block alone.
		BringUp();
		FillUniform();

		for (const u32 psm : kPaletted)
		{
			const GSVector2i bs = GSLocalMemory::m_psm[psm].bs;

			// A window one texel wider and taller than a block, which is two blocks each way from any offset.
			const auto at = [&](int x, int y) {
				Window win;
				win.psm = psm;
				win.tbp0 = 96;
				win.tbw = 8;
				win.tw = 8;
				win.th = 8;
				win.has_x = win.has_y = true;
				win.x0 = x;
				win.y0 = y;
				win.w = bs.x + 1;
				win.h = bs.y + 1;
				return win;
			};

			const int offsets[][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {bs.x - 1, 0}, {0, bs.y - 1}, {bs.x - 1, bs.y - 1}};
			for (size_t i = 0; i < std::size(offsets); i++)
			{
				for (size_t j = i + 1; j < std::size(offsets); j++)
				{
					const Window a = at(offsets[i][0], offsets[i][1]);
					const Window b = at(offsets[j][0], offsets[j][1]);
					ASSERT_EQ(Blocks(a).size(), Blocks(b).size());
					EXPECT_FALSE(Key(a, true) == Key(b, true))
						<< "psm " << psm << " offsets (" << offsets[i][0] << "," << offsets[i][1] << ") and ("
						<< offsets[j][0] << "," << offsets[j][1] << ") share a key";
				}

				// The same offset a whole block over, on the same bytes, is the same window: the key follows
				// the offset and the bytes, not the address.
				const Window a = at(offsets[i][0], offsets[i][1]);
				const Window moved = at(offsets[i][0] + bs.x, offsets[i][1] + bs.y);
				EXPECT_TRUE(Key(a, true) == Key(moved, true)) << "psm " << psm << " offset " << i;
				EXPECT_EQ(Texels(a), Texels(moved)) << "psm " << psm << " offset " << i;
			}
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, TheOffsetOfALevelOfAMipmapChainIsInTheKey)
	{
		// A 32 x 32 window at x = 4 or at x = 20 starts at the same place in its first block (4), over the
		// same blocks. Its level 1, half the size, starts at x = 2 or at x = 10: another place, which the key
		// has to carry although the first level agrees.
		BringUp();
		FillUniform();

		Packet p;
		GIFReg r = {};
		r.U64 = 0;
		r.TEX0.TBP0 = 96;
		r.TEX0.TBW = 8;
		r.TEX0.PSM = PSMT8;
		r.TEX0.TW = 8;
		r.TEX0.TH = 8;
		p.Reg(GIF_A_D_REG_TEX0_1, r);
		r.U64 = 0;
		r.MIPTBP1.TBP1 = 1024;
		r.MIPTBP1.TBW1 = 4;
		p.Reg(GIF_A_D_REG_MIPTBP1_1, r);
		p.Send(*m_gs, GIFRegPRIM{});

		const GSVector2i lod(0, 1);
		const auto at = [&](int x) {
			Window win;
			win.psm = PSMT8;
			win.tbp0 = 96;
			win.tbw = 8;
			win.has_x = win.has_y = true;
			win.x0 = x;
			win.w = 32;
			win.h = 32;
			return win;
		};

		const HashCacheKey a = Key(at(4), true, &lod);
		const HashCacheKey b = Key(at(20), true, &lod);
		const HashCacheKey a_again = Key(at(4), true, &lod);
		EXPECT_TRUE(a == a_again);
		EXPECT_FALSE(a == b);

		// With the first level alone, the two are one window.
		EXPECT_TRUE(Key(at(4), true) == Key(at(20), true));

		// And the level that tells them apart does read other texels: sharing a key would be wrong.
		const auto level1 = [&](int x) {
			Window win;
			win.psm = PSMT8;
			win.tbp0 = 1024;
			win.tbw = 4;
			win.has_x = win.has_y = true;
			win.x0 = x;
			win.w = 16;
			win.h = 16;
			return win;
		};
		EXPECT_NE(Texels(level1(2)), Texels(level1(10)));
		ExpectVerified();
	}

	// --- What is not raw ------------------------------------------------------------------------------

	TEST_F(GSHashRawKey, AWindowSmallerThanABlockKeepsTheTexelHash)
	{
		BringUp();
		Randomise();
		for (const u32 psm : kPaletted)
		{
			const GSVector2i bs = GSLocalMemory::m_psm[psm].bs;
			for (const bool wide : {false, true})
			{
				Window win;
				win.psm = psm;
				win.tbp0 = 128;
				win.tbw = 4;
				win.tw = 8;
				win.th = 8;
				win.has_x = win.has_y = true;
				win.x0 = 3;
				win.y0 = 2;
				win.w = wide ? bs.x : bs.x - 1; // one of the two is a block wide, the other a texel short
				win.h = wide ? bs.y - 1 : bs.y;

				const HashCacheKey raw = Key(win, true);
				const HashCacheKey texel = Key(win, false);
				EXPECT_TRUE(raw == texel) << "psm " << psm;
				EXPECT_EQ(raw.TEX0Hash, TexelHash(win)) << "psm " << psm;
				EXPECT_EQ(raw.TEX0.U64 & 1, 0u) << "psm " << psm;
			}
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, AFormatThatIsNotPalettedIsNotRaw)
	{
		BringUp();
		Randomise();
		for (const u32 psm : {PSMCT32, PSMCT24, PSMCT16, PSMZ32})
		{
			Window win;
			win.psm = psm;
			win.tbp0 = 128;
			win.tbw = 4;
			win.tw = 8;
			win.th = 7;
			EXPECT_TRUE(Key(win, true) == Key(win, false)) << "psm " << psm;

			win.has_x = true;
			win.x0 = 5;
			win.w = 40;
			EXPECT_TRUE(Key(win, true) == Key(win, false)) << "psm " << psm << " with a region";
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, TheTexelKeyIsTheOneItWas)
	{
		// What a lookup for a dump, a pack texture or an upscale asks for: the texel hash, built here from
		// the bytes as it was before raw keys.
		BringUp();
		Randomise();
		std::mt19937 rng(4);
		for (const u32 psm : kPaletted)
		{
			for (int trial = 0; trial < 100; trial++)
			{
				const Window win = RandomWindow(rng, psm);
				const HashCacheKey key = Key(win, false);
				EXPECT_EQ(key.TEX0Hash, TexelHash(win)) << "psm " << psm << " trial " << trial;
				EXPECT_EQ(key.TEX0.U64 & 1, 0u);
			}
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, ARawKeyIsTaggedWhereItIsAnotherHashAndNowhereElse)
	{
		BringUp();
		Randomise();
		std::mt19937 rng(5);
		u32 changed = 0, unchanged = 0;
		for (const u32 psm : kPaletted)
		{
			for (int trial = 0; trial < 100; trial++)
			{
				const Window win = RandomWindow(rng, psm);
				const HashCacheKey raw = Key(win, true);
				const HashCacheKey texel = Key(win, false);
				if (ChangesWithRawKeys(win))
				{
					changed++;
					EXPECT_EQ(raw.TEX0.U64 & 1, 1u) << "psm " << psm << " trial " << trial;
					EXPECT_FALSE(raw == texel) << "psm " << psm << " trial " << trial;
				}
				else
				{
					unchanged++;
					EXPECT_TRUE(raw == texel) << "psm " << psm << " trial " << trial;
				}
				EXPECT_EQ(texel.TEX0.U64 & 1, 0u);
			}
		}
		EXPECT_GT(changed, 0u);
		EXPECT_GT(unchanged, 0u);
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, AnUnregionedPalettedWindowKeepsItsKeyInEveryMode)
	{
		// What a texture pack file of an 8 or 4-bit texture is named by: the hash of its blocks, from the key
		// master builds. Raw keys change nothing for it, and neither do dump, replace and upscale lookups.
		BringUp();
		Randomise();
		for (const u32 psm : {PSMT8, PSMT4})
		{
			for (const bool y_region : {false, true})
			{
				Window win;
				win.psm = psm;
				win.tbp0 = 64;
				win.tbw = 4;
				win.tw = 7;
				win.th = 6;
				if (y_region)
				{
					// A region that starts at the top is still hashed from its blocks.
					win.has_y = true;
					win.y0 = 0;
					win.h = 48;
				}

				EXPECT_TRUE(MasterKeyMatches(win, Key(win, false))) << "psm " << psm << " y_region " << y_region;
				EXPECT_TRUE(MasterKeyMatches(win, Key(win, true))) << "psm " << psm << " y_region " << y_region;
				EXPECT_EQ(Key(win, true).TEX0.U64 & 1, 0u);
			}
		}
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, TheKeyOfALookupForTexelsIsMasterKey)
	{
		// dump, replace and upscale: whatever the window, the key is the one master builds.
		BringUp();
		Randomise();
		std::mt19937 rng(6);
		for (const u32 psm : kPaletted)
		{
			for (int trial = 0; trial < 100; trial++)
			{
				const Window win = RandomWindow(rng, psm);
				EXPECT_TRUE(MasterKeyMatches(win, Key(win, false))) << "psm " << psm << " trial " << trial;
			}
		}
		ExpectVerified();
	}

	// --- The memo -------------------------------------------------------------------------------------

	TEST_F(GSHashRawKey, RawAndTexelHashesDoNotShareAMemoEntry)
	{
		// 512 x 256 of 8 bits is 512 blocks, the smallest texture the memo takes.
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8;
		win.tbp0 = 128;
		win.tbw = 8;
		win.tw = 9;
		win.th = 8;
		win.has_x = true;
		win.x0 = 0;
		win.w = 512;

		const HashCacheKey raw = Key(win, true);
		const HashCacheKey texel = Key(win, false);
		ASSERT_FALSE(raw == texel);
		EXPECT_EQ(Stats().lookups, 2u) << "the window was too small for the memo, which makes the checks below empty";

		// The memo holds the texel hash now, and a lookup for the raw one is not answered with it.
		const u64 hits = Stats().hits;
		EXPECT_TRUE(Key(win, true) == raw);
		EXPECT_EQ(Stats().hits, hits);

		// Each kind is answered by the memo once it holds it.
		EXPECT_TRUE(Key(win, true) == raw);
		EXPECT_EQ(Stats().hits, hits + 1);
		EXPECT_TRUE(Key(win, false) == texel);
		EXPECT_EQ(Stats().hits, hits + 1);
		EXPECT_TRUE(Key(win, false) == texel);
		EXPECT_EQ(Stats().hits, hits + 2);

		// A write to the window is seen through the memo like any other texture's.
		m_gs->m_mem.m_vm8[win.tbp0 * GS_BLOCK_SIZE + 9] ^= 0x10;
		m_gs->m_mem.MarkPageRangeWritten(win.tbp0 / 32, 1);
		EXPECT_FALSE(Key(win, false) == texel);
		EXPECT_FALSE(Key(win, true) == raw);
		ExpectVerified();
	}

	TEST_F(GSHashRawKey, ARawKeyIsMemoisedAndSeesAWrite)
	{
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8H;
		win.tbp0 = 128;
		win.tbw = 8;
		win.tw = 9;
		win.th = 8;
		const HashCacheKey first = Key(win, true);
		EXPECT_TRUE(Key(win, true) == first);
		EXPECT_EQ(Stats().hits, 1u);

		// A write to the bits of the format moves the key. A write to the others is a mark that changed
		// no texel: the memo recomputes, and the key is the same.
		m_gs->m_mem.m_vm8[win.tbp0 * GS_BLOCK_SIZE + 1] ^= 0xFF; // not the top byte, which is the format's own
		m_gs->m_mem.MarkPageRangeWritten(win.tbp0 / 32, 1);
		EXPECT_TRUE(Key(win, true) == first);
		EXPECT_EQ(Stats().stale, 1u);

		m_gs->m_mem.m_vm8[win.tbp0 * GS_BLOCK_SIZE + 3] ^= 0x01;
		m_gs->m_mem.MarkPageRangeWritten(win.tbp0 / 32, 1);
		EXPECT_FALSE(Key(win, true) == first);
		EXPECT_EQ(Stats().stale, 2u);
		ExpectVerified();
	}
} // namespace
