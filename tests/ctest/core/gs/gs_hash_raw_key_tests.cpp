// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the raw-block key of GSTextureCache::HashCacheKey::Create. A paletted window (T8, T4, T8H, T4HL,
// T4HH) of at least one block in each direction is keyed by the bytes of its blocks as they sit in local
// memory, in the order the unswizzle reads them, plus where the window starts inside its first block.
//
// A cache key must guarantee that two windows with the same key have the same texels (what PSM::rtxP hands
// PreloadTexture). The key may be stricter than the texels (it reads the whole of the window's edge blocks,
// so a byte outside the window changes it), never looser.
//
// The second half of the file pins the per-block hash cache, which keys such a window by a hash of the
// hashes of its blocks instead of by their bytes: the same equivalence of windows, from far fewer bytes read.
//
// Only windows that the texel hash unswizzles (a region, or a format that shares its words with other data)
// get a new key. All others keep the texel hash: a window smaller than a block, a format that is not
// paletted, an unregioned 8 or 4-bit window (its hash names its texture pack files), and every lookup that
// asks for the texel hash (dump, replace, upscale).

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

	struct Window
	{
		u32 psm = PSMT8;
		u32 tbp0 = 0; ///< In blocks.
		u32 tbw = 2; ///< In units of 64 texels.
		u32 tw = 8, th = 8; ///< Log2 of the size of the texture.
		bool has_x = false, has_y = false;
		int x0 = 0, y0 = 0, w = 0, h = 0;
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

		/// The texel hash of the window: of its unswizzled texels, or, for a window the texel hash does not
		/// unswizzle, of its blocks. Built here from the bytes.
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

		/// Whether a raw lookup hashes the window differently from a texel lookup: paletted, a block or more each
		/// way (the random windows all are), and one the texel hash unswizzles (a region, or a format that shares
		/// its words with other data).
		static bool ChangesWithRawKeys(const Window& win)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[win.psm];
			return psm.pal > 0 && (win.has_x || (win.has_y && win.y0 > 0) || psm.fmsk != 0xFFFFFFFFu);
		}

		/// The key of a texel-hash lookup, from the fields it is made of.
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

		/// A window of the given format with random position, size and region, at least a block in each direction.
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
			win.w = 100; // a region, so the window is hashed from raw blocks

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
		// the bytes.
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
		// A texture pack file of an 8 or 4-bit texture is named by the hash of its blocks, so raw lookups and
		// dump, replace and upscale lookups must all leave that key unchanged.
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

	// With the cache a raw level is keyed by a hash of the 64-bit hashes of its blocks, and a block's hash is
	// kept until a mark of the block. The key must equal the one a from-scratch computation gives (so a write is
	// seen), and two windows share a key with the cache exactly when they do without it.

	class GSBlockHashCache : public GSHashRawKey
	{
	protected:
		void SetUp() override
		{
			GSHashRawKey::SetUp();
			GSTextureCache::SetBlockHashCacheEnabled(true);
			GSTextureCache::SetBlockHashVerify(false);
		}

		void TearDown() override
		{
			GSTextureCache::SetBlockHashCacheEnabled(true);
			GSTextureCache::SetBlockHashVerify(false);
			GSHashRawKey::TearDown();
		}

		/// The key of the window with the cache, or without it (one hash of the raw blocks).
		HashCacheKey KeyWith(const Window& win, bool cache, const GSVector2i* lod = nullptr) const
		{
			GSTextureCache::SetBlockHashCacheEnabled(cache);
			const HashCacheKey key = Key(win, true, lod);
			GSTextureCache::SetBlockHashCacheEnabled(true);
			return key;
		}

		/// The hash a single-level window of a paletted format must have with the cache, built here from the
		/// bytes of local memory with no cache: the 64-bit XXH3 of each block (of its masked words, for the H
		/// formats), in the order the unswizzle reads them, then where the window starts inside its first
		/// block if that is not the corner, all hashed once more.
		u64 CachedHashFromScratch(const Window& win)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[win.psm];
			std::vector<u8> bytes;
			for (const u32 bn : Blocks(win))
			{
				u8 block[GS_BLOCK_SIZE];
				std::memcpy(block, m_gs->m_mem.BlockPtr(bn), GS_BLOCK_SIZE);
				if (psm.fmsk != 0xFFFFFFFFu)
				{
					u32 words[GS_BLOCK_SIZE / sizeof(u32)];
					std::memcpy(words, block, GS_BLOCK_SIZE);
					for (u32& w : words)
						w &= psm.fmsk;
					std::memcpy(block, words, GS_BLOCK_SIZE);
				}
				const u64 h = XXH3_64bits(block, GS_BLOCK_SIZE);
				const u8* hp = reinterpret_cast<const u8*>(&h);
				bytes.insert(bytes.end(), hp, hp + sizeof(h));
			}

			const GSVector4i rect = Rect(win);
			const GSVector4i block_rect = BlockRect(win);
			const u64 offset = static_cast<u64>(rect.left - block_rect.left) | (static_cast<u64>(rect.top - block_rect.top) << 8);
			if (offset)
			{
				const u8* op = reinterpret_cast<const u8*>(&offset);
				bytes.insert(bytes.end(), op, op + sizeof(offset));
			}
			return XXH3_64bits(bytes.data(), bytes.size());
		}

		/// A write of random bytes into a random part of the window's own blocks, or somewhere else, through the
		/// host-to-local writer and marked the way a transfer marks it (the rectangle of the transfer).
		void RandomMarkedWrite(std::mt19937& rng, const std::vector<Window>& wins)
		{
			Window target = wins[rng() % wins.size()];
			if (rng() % 4 == 0)
			{
				target = RandomWindow(rng, target.psm);
			}
			const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[target.psm];
			const GSVector4i rect = Rect(target);
			const int x0 = rect.left + static_cast<int>(rng() % rect.width());
			const int y0 = rect.top + static_cast<int>(rng() % rect.height());
			const int w = 1 + static_cast<int>(rng() % (rect.right - x0));
			const int h = 1 + static_cast<int>(rng() % (rect.bottom - y0));

			GIFRegBITBLTBUF blit = {};
			blit.DBP = target.tbp0;
			blit.DBW = target.tbw;
			blit.DPSM = target.psm;
			GIFRegTRXPOS pos = {};
			pos.DSAX = x0;
			pos.DSAY = y0;
			GIFRegTRXREG reg = {};
			reg.RRW = w;
			reg.RRH = h;
			const size_t len = (static_cast<size_t>(w) * h * p.trbpp + 7) / 8;
			std::vector<u8> data(len);
			for (u8& b : data)
				b = static_cast<u8>(rng());

			int tx = x0, ty = y0;
			p.wi(m_gs->m_mem, tx, ty, data.data(), static_cast<int>(len), blit, pos, reg);
			m_gs->m_mem.MarkPagesWritten(Offset(target), GSVector4i(x0, y0, x0 + w, y0 + h));
		}

		static GSTextureCache::BlockHashStats Stats3() { return GSTextureCache::GetBlockHashStats(); }
	};

	TEST_F(GSBlockHashCache, TheKeyIsTheHashOfTheBlockHashesWhateverHasBeenWritten)
	{
		// After random marked writes, the key the cache gives is the key built from scratch from the bytes
		// of local memory, so a block hash that was kept past a write would show.
		BringUp();
		Randomise();
		std::mt19937 rng(31);

		u32 checked = 0, changed = 0;
		for (const u32 psm : kPaletted)
		{
			// Raw windows only: an unregioned 8 or 4-bit window is not one.
			std::vector<Window> wins;
			while (wins.size() < 6)
			{
				const Window win = RandomWindow(rng, psm);
				if (ChangesWithRawKeys(win))
					wins.push_back(win);
			}

			std::vector<u64> last(wins.size(), 0);
			for (int round = 0; round < 40; round++)
			{
				RandomMarkedWrite(rng, wins);
				for (size_t i = 0; i < wins.size(); i++)
				{
					const HashCacheKey key = Key(wins[i], true);
					ASSERT_EQ(key.TEX0.U64 & 1, 1u) << "the window is raw";
					ASSERT_EQ(key.TEX0Hash, CachedHashFromScratch(wins[i])) << "psm " << psm << " round " << round << " window " << i;
					changed += (key.TEX0Hash != last[i]);
					last[i] = key.TEX0Hash;
					checked++;
				}
			}
		}
		EXPECT_EQ(checked, 5u * 40u * 6u);
		EXPECT_GT(changed, checked / 10) << "the writes did not reach the windows";
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, TwoWindowsHaveOneKeyWithTheCacheExactlyWhenTheyHaveOneWithout)
	{
		// The cache changes the number a window hashes to and nothing about which windows are one.
		BringUp();
		std::mt19937 rng(32);

		u32 equal = 0, different = 0;
		const auto compare = [&](const Window& a, const Window& b, const char* what, u32 psm) {
			const bool with_cache = KeyWith(a, true) == KeyWith(b, true);
			const bool without = KeyWith(a, false) == KeyWith(b, false);
			EXPECT_EQ(with_cache, without) << what << " psm " << psm;
			(with_cache ? equal : different)++;
		};

		for (const u32 psm : kPaletted)
		{
			const u32 fmsk = GSLocalMemory::m_psm[psm].fmsk;
			Randomise();
			for (int trial = 0; trial < 120; trial++)
			{
				const Window a = RandomWindow(rng, psm);
				Window b;

				// A copy of the window elsewhere: one key, unless the H format's other bits differ.
				if (CopyElsewhere(rng, a, b))
				{
					compare(a, b, "copy", psm);
					if (fmsk != 0xFFFFFFFFu)
					{
						for (const u32 bn : Blocks(b))
						{
							u32* words = reinterpret_cast<u32*>(m_gs->m_mem.BlockPtr(bn));
							for (u32 i = 0; i < GS_BLOCK_SIZE / sizeof(u32); i++)
								words[i] = (words[i] & fmsk) | (static_cast<u32>(rng()) & ~fmsk);
						}
						m_gs->m_mem.MarkAllPagesWritten();
						compare(a, b, "copy with junk in the other bits", psm);
					}

					const std::vector<u32> blocks = Blocks(b);
					m_gs->m_mem.BlockPtr(blocks[rng() % blocks.size()])[rng() % GS_BLOCK_SIZE] ^= static_cast<u8>(1u << (rng() % 8));
					m_gs->m_mem.MarkAllPagesWritten();
					compare(a, b, "copy with one bit changed", psm);
				}

				// Another window of the same format: a different size, a different place, a different offset.
				compare(a, RandomWindow(rng, psm), "unrelated window", psm);
			}

			// Blocks that are all alike, so that only the shape and the offset tell the windows apart.
			FillUniform();
			for (int trial = 0; trial < 60; trial++)
				compare(RandomWindow(rng, psm), RandomWindow(rng, psm), "windows over uniform memory", psm);
		}

		EXPECT_GT(equal, 100u);
		EXPECT_GT(different, 100u);
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, AMipmapChainIsKeyedThroughTheCacheToo)
	{
		// Two levels, both raw: each is keyed through the cache, with the same equivalence of windows. On
		// memory of one repeated block, windows differ only by where they start inside a block at each level.
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

		u32 equal = 0, different = 0;
		for (const int xa : {4, 20, 36, 52, 3})
		{
			for (const int xb : {4, 20, 36, 52, 3})
			{
				const bool with_cache = KeyWith(at(xa), true, &lod) == KeyWith(at(xb), true, &lod);
				const bool without = KeyWith(at(xa), false, &lod) == KeyWith(at(xb), false, &lod);
				EXPECT_EQ(with_cache, without) << xa << " and " << xb;
				(with_cache ? equal : different)++;
			}
		}
		EXPECT_GT(equal, 5u);
		EXPECT_GT(different, 5u);

		const GSTextureCache::BlockHashStats before = Stats3();
		Key(at(4), true, &lod);
		EXPECT_EQ(Stats3().levels - before.levels, 2u);

		// A byte of the second level's blocks moves the key, and the key is the one built from scratch.
		const HashCacheKey one = Key(at(4), true, &lod);
		Window level1;
		level1.psm = PSMT8;
		level1.tbp0 = 1024;
		level1.tbw = 4;
		level1.has_x = level1.has_y = true;
		level1.x0 = 2;
		level1.w = 16;
		level1.h = 16;
		m_gs->m_mem.BlockPtr(Blocks(level1)[0])[100] ^= 0x08;
		m_gs->m_mem.MarkAllPagesWritten();
		EXPECT_FALSE(Key(at(4), true, &lod) == one);
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, OnlyTheBlocksThatWereMarkedAreHashedAgain)
	{
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8;
		win.tbp0 = 64;
		win.tbw = 2;
		win.tw = 7;
		win.th = 6;
		win.has_x = true;
		win.x0 = 0;
		win.w = 100; // a region, so a raw window
		const std::vector<u32> blocks = Blocks(win);
		const GSVector4i block_rect = BlockRect(win);
		const GSVector2i bs = GSLocalMemory::m_psm[PSMT8].bs;
		const int per_row = block_rect.width() / bs.x;
		ASSERT_GT(blocks.size(), 16u);

		// The first lookup hashes every block, which no hash has been kept for.
		GSTextureCache::BlockHashStats before = Stats3();
		const HashCacheKey first = Key(win, true);
		GSTextureCache::BlockHashStats after = Stats3();
		EXPECT_EQ(after.levels - before.levels, 1u);
		EXPECT_EQ(after.blocks - before.blocks, blocks.size());
		EXPECT_EQ(after.rehashed - before.rehashed, blocks.size());

		// The second hashes none.
		before = after;
		EXPECT_TRUE(Key(win, true) == first);
		after = Stats3();
		EXPECT_EQ(after.blocks - before.blocks, blocks.size());
		EXPECT_EQ(after.rehashed - before.rehashed, 0u);

		// Three blocks of the window are written and marked, each by a rectangle of exactly that block; the next
		// lookup hashes those and no other, and its key moves.
		for (const size_t k : {size_t(2), size_t(5), blocks.size() - 1})
		{
			const int bx = static_cast<int>(k) % per_row, by = static_cast<int>(k) / per_row;
			m_gs->m_mem.BlockPtr(blocks[k])[40] ^= 0x20;
			const int x = block_rect.left + bx * bs.x, y = block_rect.top + by * bs.y;
			m_gs->m_mem.MarkPagesWritten(Offset(win), GSVector4i(x, y, x + bs.x, y + bs.y));
		}
		before = Stats3();
		const HashCacheKey moved = Key(win, true);
		after = Stats3();
		EXPECT_EQ(after.rehashed - before.rehashed, 3u);
		EXPECT_FALSE(moved == first);
		EXPECT_EQ(moved.TEX0Hash, CachedHashFromScratch(win));

		// A mark that changed no byte costs a rehash and moves nothing.
		const int x = block_rect.left, y = block_rect.top;
		m_gs->m_mem.MarkPagesWritten(Offset(win), GSVector4i(x, y, x + bs.x, y + bs.y));
		before = Stats3();
		EXPECT_TRUE(Key(win, true) == moved);
		after = Stats3();
		EXPECT_EQ(after.rehashed - before.rehashed, 1u);

		// Another window over the same blocks is served by the same hashes.
		Window other = win;
		other.w = 90;
		before = Stats3();
		Key(other, true);
		after = Stats3();
		EXPECT_EQ(after.rehashed - before.rehashed, 0u);
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, AnHFormatIsKeyedFromItsMaskedBitsEveryTime)
	{
		BringUp();
		Randomise();
		for (const u32 psm : {PSMT8H, PSMT4HL, PSMT4HH})
		{
			Window win;
			win.psm = psm;
			win.tbp0 = 160;
			win.tbw = 1;
			win.tw = 5;
			win.th = 5;
			const GSTextureCache::BlockHashStats before = Stats3();
			const HashCacheKey first = Key(win, true);
			Key(win, true);
			const GSTextureCache::BlockHashStats after = Stats3();
			EXPECT_EQ(after.levels - before.levels, 2u) << psm;
			EXPECT_EQ(after.rehashed - before.rehashed, 32u) << psm << ": no block hash is kept for a masked block";
			EXPECT_EQ(first.TEX0Hash, CachedHashFromScratch(win)) << psm;
		}
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, WithTheCacheOffNoBlockIsCachedAndTheMarksAreLeftAlone)
	{
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8;
		win.tbp0 = 64;
		win.tbw = 2;
		win.tw = 7;
		win.th = 6;
		win.has_x = true;
		win.w = 100;

		const GSTextureCache::BlockHashStats before = Stats3();
		const HashCacheKey off_key = KeyWith(win, false);
		const GSTextureCache::BlockHashStats after = Stats3();
		EXPECT_EQ(after.levels, before.levels);
		for (const u32 bn : Blocks(win))
			EXPECT_TRUE(m_gs->m_mem.HashDirty(bn)) << "block " << bn << ": the uncached key cleared a bit it does not own";

		EXPECT_FALSE(KeyWith(win, true) == off_key);
		ExpectVerified();
	}

	TEST_F(GSBlockHashCache, TheCacheIsBoundToTheMemoryItDescribes)
	{
		// A cache that outlives the memory it described, or is made over a memory that exists already, must
		// not trust a bit that some other history cleared.
		BringUp();
		Randomise();
		GSLocalMemory other;
		std::mt19937 rng(34);
		for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 4)
		{
			const u32 v = static_cast<u32>(rng());
			std::memcpy(other.m_vm8 + i, &v, 4);
		}
		ASSERT_NE(other.Id(), m_gs->m_mem.Id());

		GSLocalMemory& mine = m_gs->m_mem;
		const u32 block = 5;
		const u64 first = g_texture_cache->BlockHash(mine, block);
		EXPECT_FALSE(mine.HashDirty(block)) << "the cache cleared the bit it keeps the hash by";

		// A store with no mark, then the cache is asked about another memory and back. Binding to the other
		// memory marked this one's every hash invalid, so the store is seen.
		mine.BlockPtr(block)[7] ^= 0x01;
		const u64 theirs = g_texture_cache->BlockHash(other, block);
		EXPECT_EQ(theirs, XXH3_64bits(other.BlockPtr(block), GS_BLOCK_SIZE));
		const u64 again = g_texture_cache->BlockHash(mine, block);
		EXPECT_EQ(again, XXH3_64bits(mine.BlockPtr(block), GS_BLOCK_SIZE));
		EXPECT_NE(again, first);

		// While bound, a block is not hashed twice.
		mine.BlockPtr(block)[9] ^= 0x01;
		EXPECT_EQ(g_texture_cache->BlockHash(mine, block), again);
	}

	TEST_F(GSBlockHashCache, VerifyModeFindsAStoreThatWasNotMarked)
	{
		BringUp();
		Randomise();
		Window win;
		win.psm = PSMT8;
		win.tbp0 = 64;
		win.tbw = 2;
		win.tw = 7;
		win.th = 6;
		win.has_x = true;
		win.w = 100;
		const std::vector<u32> blocks = Blocks(win);

		GSTextureCache::SetBlockHashVerify(true);
		const HashCacheKey before = Key(win, true);
		GSTextureCache::BlockHashVerifyReport report = GSTextureCache::GetBlockHashVerifyReport();
		EXPECT_EQ(report.levels, 1u);
		EXPECT_EQ(report.blocks, blocks.size());
		EXPECT_EQ(report.stale_blocks, 0u);
		EXPECT_EQ(report.lookups, 1u);

		// A store that is marked is seen.
		m_gs->m_mem.BlockPtr(blocks[1])[3] ^= 0x01;
		m_gs->m_mem.MarkPagesWritten(Offset(win), GSVector4i(0, 0, 100, 64));
		EXPECT_FALSE(Key(win, true) == before);
		report = GSTextureCache::GetBlockHashVerifyReport();
		EXPECT_EQ(report.stale_blocks, 0u);
		EXPECT_EQ(report.extra_misses + report.false_hits, 0u);

		// One that is not is found, and counted once.
		const HashCacheKey marked = Key(win, true);
		m_gs->m_mem.BlockPtr(blocks[4])[3] ^= 0x01;
		EXPECT_TRUE(Key(win, true) == marked) << "the key could not know";
		report = GSTextureCache::GetBlockHashVerifyReport();
		EXPECT_EQ(report.stale_blocks, 1u);
		Key(win, true);
		report = GSTextureCache::GetBlockHashVerifyReport();
		EXPECT_EQ(report.stale_blocks, 1u) << "the cached hash was put right";

		// The two forms of that key now disagree on which windows are one: the stale key still equals the key of
		// the window as it was, which the uncached form says is another.
		EXPECT_GT(report.false_hits, 0u);
		GSTextureCache::SetBlockHashVerify(false);
	}
} // namespace
