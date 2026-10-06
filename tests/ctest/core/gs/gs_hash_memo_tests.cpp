// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the memo of texture hashes in GSTextureCache::HashCacheKey::Create: a texture whose pages have
// not been written since it was hashed is not hashed again, and one whose pages have been, by any of
// the paths that store into local memory, is.
//
// The memo may only ever return what a fresh hash would, so every case here runs with verify mode on:
// each memo hit is also hashed afresh and compared, and the page sweep looks for stores that were not
// marked. A case passes only if the memo hit when it should, missed when it should, and verify mode saw
// no mismatch and no missed writer. Where the case can, the hash is also compared with one built here
// from the bytes of the texture, independent of the renderer.
//
// The textures are 256 x 128 of 32 bits, 512 blocks and 16 pages, the smallest the memo takes.

#include "gs_write_stamp_drivers.h"

#include "GS/GSXXH.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "SaveState.h"

#include <string>

using namespace GSWriteStamps;

namespace
{
	constexpr u32 kTW = 8;
	constexpr u32 kTH = 7;
	constexpr u32 kTBW = 4;
	constexpr u32 kPages = 16; // 4 pages wide and 4 high, one after another from the base pointer

	class GSHashMemo : public SwPrimFixture
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

		/// A texture of 16 pages that starts at `page`.
		static GIFRegTEX0 Tex0(u32 page, u32 psm = PSMCT32, u32 tw = kTW, u32 th = kTH, u32 tbw = kTBW)
		{
			GIFRegTEX0 tex0 = {};
			tex0.TBP0 = page * 32;
			tex0.TBW = tbw;
			tex0.PSM = psm;
			tex0.TW = tw;
			tex0.TH = th;
			return tex0;
		}

		static u64 Hash(const GIFRegTEX0& tex0, const GIFRegTEXA& texa = {}, const GSVector2i* lod = nullptr, GSTextureCache::SourceRegion region = {})
		{
			return GSTextureCache::HashCacheKey::Create(tex0, texa, nullptr, lod, region).TEX0Hash;
		}

		/// The hash of a block-aligned 32-bit texture, built from its bytes: XXH3 of the blocks in the order
		/// the texture is walked, laid end to end.
		u64 Reference(const GIFRegTEX0& tex0) const
		{
			std::vector<u8> bytes;
			AppendBlocks(bytes, tex0);
			return XXH3_64bits(bytes.data(), bytes.size());
		}

		void AppendBlocks(std::vector<u8>& bytes, const GIFRegTEX0& tex0) const
		{
			const GSLocalMemory& mem = m_gs->m_mem;
			mem.GetOffset(tex0.TBP0, tex0.TBW, tex0.PSM).loopBlocks(GSVector4i(0, 0, 1 << tex0.TW, 1 << tex0.TH), [&](u32 bn) {
				const u8* block = mem.BlockPtr(bn);
				bytes.insert(bytes.end(), block, block + GS_BLOCK_SIZE);
			});
		}

		const GSTextureCache::HashMemoStats& Stats() const { return g_texture_cache->GetHashMemoStats(); }

		/// Verify mode found nothing, and saw `hits` memo hits.
		static void ExpectVerified(u64 hits)
		{
			const GSTextureCache::HashMemoVerifyReport report = GSTextureCache::GetHashMemoVerifyReport();
			EXPECT_EQ(report.hits, hits);
			EXPECT_EQ(report.mismatches, 0u);
			EXPECT_EQ(report.missed_writers, 0u);
		}

		// Local memory with every word different, marked as written.
		void Randomise()
		{
			FillRandom();
			m_gs->m_mem.MarkAllPagesWritten();
		}

		/// A 32-bit rectangle of one value at the start of `page`, through the readback writer.
		void WriteRect(u32 page, u32 bw, int w, int h, u32 value)
		{
			std::vector<u32> pixels(static_cast<size_t>(w) * h, value);
			m_gs->m_mem.WritePixel32(reinterpret_cast<u8*>(pixels.data()), w * 4, m_gs->m_mem.GetOffset(page * 32, bw, PSMCT32), GSVector4i(0, 0, w, h));
		}

		/// Hashes the texture, runs `write`, hashes it again, and requires the second hash to have been
		/// computed afresh (no hit, one stale entry) and to be the hash of the new bytes.
		template <typename Write>
		void ExpectWriteIsSeen(const GIFRegTEX0& tex0, const char* what, Write&& write)
		{
			const u64 before = Hash(tex0);
			ASSERT_EQ(before, Reference(tex0)) << what;
			const u64 hits = Stats().hits;
			ASSERT_EQ(Hash(tex0), before);
			ASSERT_EQ(Stats().hits, hits + 1) << what << ": the unwritten texture was not answered by the memo";

			const u64 stale = Stats().stale;
			write();

			const u64 after = Hash(tex0);
			EXPECT_EQ(Stats().hits, hits + 1) << what << ": a hit on a texture that was written";
			EXPECT_EQ(Stats().stale, stale + 1) << what << ": the memo entry was not found stale";
			EXPECT_NE(after, before) << what << ": the writer did not change the texture";
			EXPECT_EQ(after, Reference(tex0)) << what;
		}
	};

	// --- The memo itself -----------------------------------------------------------------------------

	TEST_F(GSHashMemo, AnUnwrittenTextureIsHashedOnce)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);

		const u64 first = Hash(tex0);
		EXPECT_EQ(first, Reference(tex0));
		for (int i = 0; i < 5; i++)
			EXPECT_EQ(Hash(tex0), first);

		EXPECT_EQ(Stats().lookups, 6u);
		EXPECT_EQ(Stats().hits, 5u);
		EXPECT_EQ(Stats().stale, 0u);
		ExpectVerified(5);
	}

	TEST_F(GSHashMemo, AWriteToAnotherPageLeavesTheMemo)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4); // pages 4 to 19
		const u64 hash = Hash(tex0);

		WriteRect(20, 1, 64, 32, 0x12345678u); // the page after the texture
		m_gs->m_mem.MarkPageRangeWritten(3, 1); // the page before it
		m_gs->m_mem.MarkPageRangeWritten(100, 20);

		EXPECT_EQ(Hash(tex0), hash);
		EXPECT_EQ(Stats().hits, 1u);
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, AWriteToAnyPageOfTheTextureMakesTheHashFresh)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);
		u64 hash = Hash(tex0);

		for (u32 page = 4; page < 4 + kPages; page++)
		{
			EXPECT_EQ(Hash(tex0), hash);
			const u64 stale = Stats().stale;

			m_gs->m_mem.m_vm8[page * GS_PAGE_SIZE + 17] ^= 0x40;
			m_gs->m_mem.MarkPageRangeWritten(page, 1);

			const u64 next = Hash(tex0);
			EXPECT_NE(next, hash) << "page " << page;
			EXPECT_EQ(Stats().stale, stale + 1) << "page " << page;
			hash = next;
		}
		ExpectVerified(kPages);
	}

	TEST_F(GSHashMemo, AMarkNeedNotHaveChangedAByte)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);

		const u64 hash = Hash(tex0);
		m_gs->m_mem.MarkPageRangeWritten(4, 1);
		EXPECT_EQ(Hash(tex0), hash);
		EXPECT_EQ(Stats().hits, 0u);
		EXPECT_EQ(Stats().stale, 1u);

		// Now it is memoised again, against the new sequence.
		EXPECT_EQ(Hash(tex0), hash);
		EXPECT_EQ(Stats().hits, 1u);

		m_gs->m_mem.MarkAllPagesWritten();
		EXPECT_EQ(Hash(tex0), hash);
		EXPECT_EQ(Stats().stale, 2u);
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, TheKeyIsEverythingTheHashReads)
	{
		BringUp();
		Randomise();

		// 512 x 128, so that each of the variants below is still a texture the memo takes.
		const GIFRegTEX0 tex0 = Tex0(4, PSMCT32, 9, 7, 8);
		const u64 base = Hash(tex0);
		const u64 hits = Stats().hits;

		// Other texture sizes, formats, buffer widths and addresses are other textures.
		GIFRegTEX0 other = tex0;
		other.TW = 8;
		EXPECT_NE(Hash(other), base);
		other = tex0;
		other.TH = 6;
		EXPECT_NE(Hash(other), base);
		other = tex0;
		other.PSM = PSMCT16;
		EXPECT_NE(Hash(other), base);
		other = tex0;
		other.TBW = 9;
		EXPECT_NE(Hash(other), base);
		other = tex0;
		other.TBP0 += 32;
		EXPECT_NE(Hash(other), base);

		// A region of the texture is a different set of texels.
		GSTextureCache::SourceRegion region = {};
		region.SetX(0, 256);
		EXPECT_NE(Hash(tex0, {}, nullptr, region), base);

		EXPECT_EQ(Stats().hits, hits) << "a different key was answered with the hash of this one";
		EXPECT_EQ(Stats().lookups, 7u) << "a variant was too small for the memo, which makes the checks above empty";

		// Nothing was lost: the same key gives the same hash.
		EXPECT_EQ(Hash(tex0), base);
		ExpectVerified(Stats().hits);
	}

	TEST_F(GSHashMemo, TexaMattersOnlyWhereTheTextureReadsIt)
	{
		BringUp();
		Randomise();

		// A 24-bit texture takes its alpha from TA0 and AEM, so it is hashed with them.
		GIFRegTEXA texa = {};
		texa.TA0 = 0x80;
		const GIFRegTEX0 ct24 = Tex0(4, PSMCT24);
		const u64 a = Hash(ct24, texa);
		EXPECT_EQ(Hash(ct24, texa), a);
		EXPECT_EQ(Stats().hits, 1u);

		texa.TA0 = 0x40;
		EXPECT_NE(Hash(ct24, texa), a);
		EXPECT_EQ(Stats().hits, 1u);

		// The bits of TEXA that are not TA0, TA1 and AEM are not read, and do not keep a hit away.
		texa.U64 |= 0xFFFFFF00FFFF7F00ULL;
		const u64 b = Hash(ct24, {});
		texa.TA0 = 0;
		texa.AEM = 0;
		texa.TA1 = 0;
		EXPECT_EQ(Hash(ct24, texa), b);
		EXPECT_EQ(Stats().hits, 2u);
		ExpectVerified(Stats().hits);
	}

	TEST_F(GSHashMemo, ATextureHashedFromExpandedTexels)
	{
		// The 8-bit-in-32-bit formats, the palette formats and the 16 and 24-bit formats are hashed from
		// the texels they expand to, a different read of the same pages.
		BringUp();
		Randomise();
		struct Format
		{
			u32 psm, tw, th, tbw;
		};
		// Each is 512 blocks, so 16 pages.
		static constexpr Format formats[] = {
			{PSMT8H, 8, 7, 4},
			{PSMT4HL, 8, 7, 4},
			{PSMT4HH, 8, 7, 4},
			{PSMT8, 9, 8, 8},
			{PSMT4, 9, 9, 8},
			{PSMCT16, 8, 8, 4},
			{PSMCT24, 8, 7, 4},
		};
		for (const Format& f : formats)
		{
			const GIFRegTEX0 tex0 = Tex0(4, f.psm, f.tw, f.th, f.tbw);
			const u64 hash = Hash(tex0);
			EXPECT_EQ(Hash(tex0), hash) << "psm " << f.psm;
			m_gs->m_mem.MarkPageRangeWritten(4, 1);
			EXPECT_EQ(Hash(tex0), hash) << "psm " << f.psm;
		}
		EXPECT_EQ(Stats().hits, std::size(formats));
		EXPECT_EQ(Stats().stale, std::size(formats));
		ExpectVerified(std::size(formats));
	}

	TEST_F(GSHashMemo, ARegionOfTheTexture)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4, PSMCT32, 9, 7, 8);
		GSTextureCache::SourceRegion region = {};
		region.SetX(8, 264);
		region.SetY(0, 128);

		const u64 hash = Hash(tex0, {}, nullptr, region);
		EXPECT_EQ(Hash(tex0, {}, nullptr, region), hash);
		EXPECT_EQ(Stats().hits, 1u);

		m_gs->m_mem.MarkPageRangeWritten(4, 1);
		EXPECT_EQ(Hash(tex0, {}, nullptr, region), hash);
		EXPECT_EQ(Stats().stale, 1u);
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ATextureWhoseRegionReachesPixel2048IsNeverMemoised)
	{
		// The rectangle of such a texture has no page set to compare, so every hash of it is fresh.
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);
		GSTextureCache::SourceRegion region = {};
		region.SetX(0, 2100);
		region.SetY(0, 32);

		const u64 hash = Hash(tex0, {}, nullptr, region);
		EXPECT_EQ(Hash(tex0, {}, nullptr, region), hash);
		EXPECT_EQ(Stats().lookups, 2u);
		EXPECT_EQ(Stats().hits, 0u);
		EXPECT_EQ(Stats().stale, 0u);
		ExpectVerified(0);
	}

	TEST_F(GSHashMemo, ASmallTextureIsNotMemoised)
	{
		// 64 x 32 is one page and 32 blocks: hashed every time, and never in the memo's way.
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4, PSMCT32, 6, 5, 1);
		const u64 hash = Hash(tex0);
		EXPECT_EQ(hash, Reference(tex0));
		EXPECT_EQ(Hash(tex0), hash);
		EXPECT_EQ(Stats().lookups, 0u);
		EXPECT_EQ(Stats().hits, 0u);
		ExpectVerified(0);
	}

	TEST_F(GSHashMemo, ManySmallTexturesDoNotEvictALargeOne)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 big = Tex0(4);
		const u64 hash = Hash(big);

		for (u32 page = 100; page < 400; page++)
			Hash(Tex0(page, PSMCT32, 6, 5, 1));

		EXPECT_EQ(Hash(big), hash);
		EXPECT_EQ(Stats().hits, 1u);
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, AMipmapChainIsMemoisedWithEveryLevelsPages)
	{
		BringUp();
		Randomise();

		// A 256 x 128 base in pages 4 to 19, then levels of 128 x 64 in pages 20 to 23 and 64 x 32 in page 24.
		Packet p;
		GIFReg r = {};
		r.U64 = 0;
		r.TEX0 = Tex0(4);
		p.Reg(GIF_A_D_REG_TEX0_1, r);
		r.U64 = 0;
		r.MIPTBP1.TBP1 = 20 * 32;
		r.MIPTBP1.TBW1 = 2;
		r.MIPTBP1.TBP2 = 24 * 32;
		r.MIPTBP1.TBW2 = 1;
		p.Reg(GIF_A_D_REG_MIPTBP1_1, r);
		p.Send(*m_gs, GIFRegPRIM{});

		const GSVector2i lod(0, 2);
		const GIFRegTEX0 tex0 = Tex0(4);
		const u64 chain = Hash(tex0, {}, &lod);
		EXPECT_NE(chain, Hash(tex0)) << "the chain hashes like its base level alone";
		const u64 hits = Stats().hits;
		EXPECT_EQ(Hash(tex0, {}, &lod), chain);
		EXPECT_EQ(Stats().hits, hits + 1);

		// A store into the last level's page only.
		WriteRect(24, 1, 64, 32, 0x0a0b0c0du);

		const u64 stale = Stats().stale;
		const u64 after = Hash(tex0, {}, &lod);
		EXPECT_NE(after, chain);
		EXPECT_EQ(Stats().stale, stale + 1);

		// The same chain with a level moved is a different texture.
		r.U64 = 0;
		r.MIPTBP1.TBP1 = 30 * 32;
		r.MIPTBP1.TBW1 = 2;
		r.MIPTBP1.TBP2 = 24 * 32;
		r.MIPTBP1.TBW2 = 1;
		Packet q;
		q.Reg(GIF_A_D_REG_MIPTBP1_1, r);
		q.Send(*m_gs, GIFRegPRIM{});
		const u64 moved_hits = Stats().hits;
		EXPECT_NE(Hash(tex0, {}, &lod), after);
		EXPECT_EQ(Stats().hits, moved_hits);
		ExpectVerified(Stats().hits);
	}

	TEST_F(GSHashMemo, ManyTexturesSharingTheMemoStayCorrect)
	{
		// Textures hashed in turn, some of which share an entry of the memo, with writes in between.
		// Whatever the memo keeps, what it answers is right.
		BringUp();
		Randomise();
		constexpr u32 kTextures = 24;
		std::mt19937 rng(5);
		for (int round = 0; round < 8; round++)
		{
			for (u32 t = 0; t < kTextures; t++)
			{
				const GIFRegTEX0 tex0 = Tex0(t * kPages);
				ASSERT_EQ(Hash(tex0), Reference(tex0)) << "round " << round << " texture " << t;
			}
			// Rewrite a few pages.
			for (int i = 0; i < 5; i++)
			{
				const u32 page = rng() % (kTextures * kPages);
				m_gs->m_mem.m_vm8[page * GS_PAGE_SIZE + rng() % GS_PAGE_SIZE] ^= 0x5a;
				m_gs->m_mem.MarkPageRangeWritten(page, 1);
			}
		}
		EXPECT_GT(Stats().hits, 0u);
		ExpectVerified(Stats().hits);
	}

	TEST_F(GSHashMemo, TheMemoIsPerRenderer)
	{
		BringUp();
		Randomise();
		Hash(Tex0(4));
		Hash(Tex0(4));
		EXPECT_EQ(Stats().hits, 1u);

		// A renderer made anew has an empty memo and local memory of its own.
		m_gs = nullptr;
		g_gs_renderer->Destroy();
		g_gs_renderer.reset();
		auto renderer = std::make_unique<Renderer>();
		m_gs = renderer.get();
		g_gs_renderer = std::move(renderer);
		g_gs_renderer->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));

		EXPECT_EQ(Stats().lookups, 0u);
		EXPECT_EQ(m_gs->m_mem.WriteSeq(), 1u);
		Hash(Tex0(4));
		EXPECT_EQ(Stats().hits, 0u);
	}

	// --- Every writer, one at a time: the texture sits where the writer writes -----------------------

	TEST_F(GSHashMemo, AnImageTransfer)
	{
		BringUp();
		Randomise();
		ExpectWriteIsSeen(Tex0(4), "transfer", [&] {
			// 0xFF over the first page of the texture, which the random bytes are not.
			Upload up(*m_gs, Tex0(4).TBP0, kTBW, PSMCT32, 0, 0, 64, 32);
			up.Data(static_cast<u32>(Upload::Size(PSMCT32, 64, 32) / 16));
		});
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ASliceInTheMiddleOfAnImageTransfer)
	{
		// 64 x 128 in three slices from page 0, which is four pages one above the other: the second slice
		// writes page 1, below where the first slice stopped, and is the one the invalidation of the
		// texture cache is told too little about. The texture is pages 0 to 15.
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(0);

		Upload up(*m_gs, 0, 1, PSMCT32, 0, 0, 64, 128);
		up.Data(512);
		up.CutSlice();

		const u64 first = Hash(tex0);
		EXPECT_EQ(Hash(tex0), first);
		EXPECT_EQ(Stats().hits, 1u);

		// The page the middle slice writes alone: the memo is for the texture as the first slice left it.
		up.Data(512);
		up.CutSlice();
		const u64 stale = Stats().stale;
		EXPECT_NE(Hash(tex0), first);
		EXPECT_EQ(Stats().stale, stale + 1) << "the memo kept a hash of a page the middle slice wrote";
		EXPECT_EQ(Hash(tex0), Reference(tex0));

		up.Data(1024);
		ExpectVerified(Stats().hits);
	}

	TEST_F(GSHashMemo, AnImageTransferOnTheBackThread)
	{
		GSConfig.BackThread = true;
		GSConfig.BackThreadResolved = true;
		m_device_api = RenderAPI::Vulkan;
		BringUp();
		ASSERT_TRUE(m_gs->IsBackThreadRunning());
		g_gs_front = std::make_unique<GSFrontState>(m_gs);
		g_gs_front->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
		g_gs_front->ResetPCRTC();
		Randomise();

		ExpectWriteIsSeen(Tex0(4), "transfer on the back thread", [&] {
			Upload up(*g_gs_front, Tex0(4).TBP0, kTBW, PSMCT32, 0, 0, 64, 32);
			up.Data(static_cast<u32>(Upload::Size(PSMCT32, 64, 32) / 16));
			g_gs_front->DrainBackQueue();
		});
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ALocalToLocalMove)
	{
		BringUp();
		Randomise();
		ExpectWriteIsSeen(Tex0(4), "move", [&] {
			GIFRegBITBLTBUF blit = {};
			blit.SBP = 40 * 32;
			blit.SBW = 1;
			blit.SPSM = PSMCT32;
			blit.DBP = Tex0(4).TBP0;
			blit.DBW = kTBW;
			blit.DPSM = PSMCT32;
			GIFRegTRXREG reg = {};
			reg.RRW = 64;
			reg.RRH = 32;
			m_gs->m_mem.Move(blit, GIFRegTRXPOS{}, reg);
		});
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ARenderTargetReadback)
	{
		BringUp();
		Randomise();
		ExpectWriteIsSeen(Tex0(4), "readback", [&] { WriteRect(4, kTBW, 64, 32, 0x01020304u); });
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, AClearOfGSMemoryOnTheCpu)
	{
		BringUp();
		Randomise();
		ExpectWriteIsSeen(Tex0(4), "clear", [&] { ClearDraw(*m_gs, PSMCT32, 0, 0, 640, 448, false); });
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ASpriteDrawnOnTheCpu)
	{
		BringUp();
		UseHack("OI_DBZBTGames");
		Randomise();
		ExpectWriteIsSeen(Tex0(0), "sprite", [&] { SpriteDraw(*m_gs, 0, 0, 16, 16, 0); });
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, APointListDrawnOnTheCpu)
	{
		BringUp();
		UseHack("OI_PointListPalette");
		Randomise();
		ExpectWriteIsSeen(Tex0(0), "point list", [&] {
			Packet p;
			Fixture::Environment(p, 0, PSMCT32);
			p.Vertex(10 << 4, 5 << 4, 0, 0, 0);
			p.Vertex(30 << 4, 9 << 4, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_POINTLIST;
			p.Send(*m_gs, prim);
		});
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, APaletteSpriteDrawnOnTheScanlineCore)
	{
		BringUp();
		Randomise();
		// The frame of the tile is at block 0x40 << 5, which is page 64.
		ExpectWriteIsSeen(Tex0(64), "palette sprite", [&] { Tile(0x40); });
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, ALoadedState)
	{
		BringUp();
		Randomise();

		freezeData fd = {};
		ASSERT_EQ(m_gs->Freeze(&fd, true), 0);
		std::vector<u8> state(static_cast<size_t>(fd.size));
		fd.data = state.data();
		ASSERT_EQ(m_gs->Freeze(&fd, false), 0);

		// The game goes on and changes local memory...
		std::memset(m_gs->m_mem.m_vm8, 0x33, GSLocalMemory::m_vmsize);
		m_gs->m_mem.MarkAllPagesWritten();

		// ...and the saved state is loaded over it.
		ExpectWriteIsSeen(Tex0(4), "state load", [&] { ASSERT_EQ(m_gs->Defrost(&fd), 0); });
		ExpectVerified(1);
	}

	TEST_F(GSHashMemo, AnEightBitTextureWhoseMemoryIsPartlyWritten)
	{
		// A 32-bit upload over a page that the high byte of a texture of 8 bits per texel lives in.
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4, PSMT8H);
		const u64 hash = Hash(tex0);
		EXPECT_EQ(Hash(tex0), hash);

		Upload up(*m_gs, Tex0(4).TBP0, kTBW, PSMCT32, 0, 0, 64, 32);
		up.Data(static_cast<u32>(Upload::Size(PSMCT32, 64, 32) / 16));

		EXPECT_NE(Hash(tex0), hash);
		EXPECT_EQ(Stats().stale, 1u);
		ExpectVerified(1);
	}

	// --- Verify mode ---------------------------------------------------------------------------------

	TEST_F(GSHashMemo, AStoreThatIsNotMarkedIsAMissedWriter)
	{
		BringUp();
		Randomise();

		g_texture_cache->VerifyWriteStamps(m_gs->m_mem); // the first sweep only looks
		m_gs->m_mem.m_vm8[7 * GS_PAGE_SIZE + 100] ^= 0xFF;
		g_texture_cache->VerifyWriteStamps(m_gs->m_mem);
		EXPECT_EQ(GSTextureCache::GetHashMemoVerifyReport().missed_writers, 1u);
		EXPECT_EQ(GSTextureCache::GetHashMemoVerifyReport().sweeps, 2u);

		// A store that is marked is not one, and neither is a mark of bytes that did not change.
		m_gs->m_mem.m_vm8[9 * GS_PAGE_SIZE + 100] ^= 0xFF;
		m_gs->m_mem.MarkPageRangeWritten(9, 1);
		m_gs->m_mem.MarkPageRangeWritten(11, 2);
		g_texture_cache->VerifyWriteStamps(m_gs->m_mem);
		EXPECT_EQ(GSTextureCache::GetHashMemoVerifyReport().missed_writers, 1u);
	}

	TEST_F(GSHashMemo, AMemoHitOnBytesThatChangedUnmarkedIsAMismatch)
	{
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);
		const u64 before = Hash(tex0);

		// A store the memo cannot know about. Verify mode computes the hash afresh and gives it back.
		m_gs->m_mem.m_vm8[4 * GS_PAGE_SIZE + 5] ^= 0xFF;
		const u64 after = Hash(tex0);

		EXPECT_EQ(GSTextureCache::GetHashMemoVerifyReport().hits, 1u);
		EXPECT_EQ(GSTextureCache::GetHashMemoVerifyReport().mismatches, 1u);
		EXPECT_NE(after, before);
		EXPECT_EQ(after, Reference(tex0));
	}

	TEST_F(GSHashMemo, VerifyModeIsOffUnlessAsked)
	{
		GSTextureCache::SetHashMemoVerify(false);
		BringUp();
		Randomise();
		const GIFRegTEX0 tex0 = Tex0(4);
		Hash(tex0);

		// With it off the memo is trusted: nothing is computed afresh, nothing is counted.
		m_gs->m_mem.m_vm8[4 * GS_PAGE_SIZE + 5] ^= 0xFF;
		Hash(tex0);
		const GSTextureCache::HashMemoVerifyReport report = GSTextureCache::GetHashMemoVerifyReport();
		EXPECT_EQ(report.hits, 0u);
		EXPECT_EQ(Stats().hits, 1u);
	}
} // namespace
