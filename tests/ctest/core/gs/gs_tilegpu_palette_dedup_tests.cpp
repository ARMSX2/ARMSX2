// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu plan's palette stream, deduplicated (GSTileClutStreamDedup), and the palette cache
// lookup that stops re-hashing words the caller already hashed.
//
// The road being pinned: a paletted draw the CLUT gather cannot serve expands its CLUT, copies the
// expansion into the plan's palette stream and hashes it for a content id. On the R&C UYA effects
// scene that ran 1,535 times a frame to carry 85 distinct palettes -- 380,480 words, 94.5% of them
// duplicates -- and the source road then hashed the same words a second time.
//
// Two keys decide a duplicate and this file is mostly about why BOTH are needed:
//
//   - the REGISTER key, (CLUT RAM generation x the registers Read32 selects and widens with),
//     which costs no hash. It catches a draw re-reading a CLUT nothing has touched.
//   - the CONTENT key, the hash of the expanded words, verified with a memcmp. It catches the
//     population the register key cannot see: a game that RELOADS its CLUT from local memory
//     before every draw moves the RAM generation every time, so identical bytes arrive under a
//     fresh register key. That is not a hypothetical -- it is what rcuya-effects does 1,354 times
//     a frame, and the register key alone removed 23.9% of its stream against the content key's
//     94.3%.
//
// The soundness claim under all of it is one sentence: EQUAL KEY IMPLIES BYTE-IDENTICAL
// EXPANSION. It is checked here against the real GSClut rather than against a model of it, because
// a key that misses a CLUT rewrite is a wrong-colour bug and nothing downstream would catch it.

#include "GS/GSClut.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTilePaletteCache.h"
#include "GS/Renderers/TileGpu/GSTileClutStreamDedup.h"

#include "GSTileStubDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

namespace
{
	using Dedup = GSTileClutStreamDedup;
	using Key = Dedup::Key;

	// Distinct and nonzero per texel, so a palette entry sourced from the wrong word always reads
	// as a different value and unwritten memory (zero) never masquerades as a valid entry.
	u32 Seed32(int x, int y, u32 salt)
	{
		return 0x40000000u | (salt << 24) | ((u32)(y & 0xff) << 16) | ((u32)(x & 0xff) << 4) | 1u;
	}

	GIFRegTEX0 ClutTEX0(u32 cbp, u32 cpsm, u32 csa, u32 psm)
	{
		GIFRegTEX0 t = {};
		t.CBP = cbp;
		t.CPSM = cpsm;
		t.CSM = 0;
		t.CSA = csa;
		t.CLD = 1;
		t.PSM = psm;
		return t;
	}

	Key MakeKey(u32 gen, u32 load_csm, const GIFRegTEX0& t, const GIFRegTEXA& a, u32 entries)
	{
		return Dedup::MakeKey(gen, load_csm, t, a, entries);
	}

	// A baseline set of key inputs every "does this field move the key" case perturbs one of.
	struct KeyInputs
	{
		u32 gen = 7;
		u32 load_csm = 0;
		GIFRegTEX0 tex0 = ClutTEX0(0x40, PSMCT32, 3, PSMT8);
		GIFRegTEXA texa = {};
		u32 entries = 256;

		Key Build() const { return MakeKey(gen, load_csm, tex0, texa, entries); }
	};
} // namespace

// ---------------------------------------------------------------------------------------------
// The register key: what moves it, and what must not.
// ---------------------------------------------------------------------------------------------

TEST(GSTileClutStreamDedupKey, TheSameStateIsTheSameKey)
{
	const KeyInputs in;
	EXPECT_TRUE(in.Build() == in.Build());
	EXPECT_EQ(in.Build().Hash(), in.Build().Hash());
}

// Every input GSClut::Read32 reads. A field missing from the key is a stale palette served to a
// draw whose CLUT moved under it, which is exactly the failure mode that has no other guard.
TEST(GSTileClutStreamDedupKey, EveryExpansionInputMovesTheKey)
{
	const KeyInputs base;
	const Key k = base.Build();

	{ // the CLUT RAM's own identity
		KeyInputs v = base;
		v.gen = base.gen + 1;
		EXPECT_TRUE(v.Build() != k) << "a CLUT RAM change must invalidate";
	}
	{ // the load's CSM -- Read32's four-bit path un-swizzles on CSM1-read-after-CSM0-load
		KeyInputs v = base;
		v.load_csm = 1;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.tex0.CBP = base.tex0.CBP + 1;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.tex0.CSA = base.tex0.CSA + 1;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.tex0.CPSM = PSMCT16;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.tex0.CSM = 1;
		EXPECT_TRUE(v.Build() != k);
	}
	{ // a sixteen-entry palette is not a 256-entry one however identical its first sixteen words
		KeyInputs v = base;
		v.entries = 16;
		EXPECT_TRUE(v.Build() != k);
	}
	{ // TEXA, which the 16-bit expansion widens through
		KeyInputs v = base;
		v.texa.AEM = 1;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.texa.TA0 = 0x33;
		EXPECT_TRUE(v.Build() != k);
	}
	{
		KeyInputs v = base;
		v.texa.TA1 = 0xCC;
		EXPECT_TRUE(v.Build() != k);
	}
}

// The complement, and it is not cosmetic: TEX0's texture-side fields change constantly while the
// palette does not, and a key that moved with them would refuse the whole population it exists to
// catch. Only what Read32 reads may be in the key.
TEST(GSTileClutStreamDedupKey, FieldsTheExpansionDoesNotReadDoNotMoveTheKey)
{
	const KeyInputs base;
	const Key k = base.Build();

	KeyInputs v = base;
	v.tex0.TBP0 = 0x1234;
	v.tex0.TBW = 8;
	v.tex0.TW = 9;
	v.tex0.TH = 9;
	v.tex0.TCC = 1;
	v.tex0.TFX = 2;
	v.tex0.CLD = 4;
	EXPECT_TRUE(v.Build() == k);
}

// ---------------------------------------------------------------------------------------------
// The map: hits, misses, and the invalidation the RAM generation is there for.
// ---------------------------------------------------------------------------------------------

TEST(GSTileClutStreamDedup, FindsWhatInsertPutAndMissesTheRest)
{
	const KeyInputs in;
	Dedup d;
	EXPECT_EQ(d.FindByRegisters(in.Build()), Dedup::kNone);

	d.Insert(in.Build(), 0xABCDEF01ull, 512, 256);
	const u32 i = d.FindByRegisters(in.Build());
	ASSERT_NE(i, Dedup::kNone);
	EXPECT_EQ(d.At(i).offset, 512u);
	EXPECT_EQ(d.At(i).entries, 256u);
	EXPECT_EQ(d.At(i).content_id, 0xABCDEF01ull);

	KeyInputs other = in;
	other.tex0.CSA = in.tex0.CSA + 1;
	EXPECT_EQ(d.FindByRegisters(other.Build()), Dedup::kNone);
}

// Open addressing with a generation stamp: more entries than the table started with must grow it
// and keep every entry findable, and a plan boundary must retire them all in one act.
TEST(GSTileClutStreamDedup, SurvivesMoreEntriesThanItStartedWith)
{
	Dedup d;
	std::vector<Dedup::Key> keys;
	for (u32 n = 0; n < 4096; n++)
	{
		KeyInputs in;
		in.gen = n;
		in.tex0.CBP = n & 0x3FFFu;
		keys.push_back(in.Build());
		d.Insert(keys.back(), 0x1000ull + n, n * 16, 16);
	}
	EXPECT_EQ(d.Size(), 4096u);
	for (u32 n = 0; n < 4096; n++)
	{
		const u32 i = d.FindByRegisters(keys[n]);
		ASSERT_NE(i, Dedup::kNone) << "entry " << n;
		EXPECT_EQ(d.At(i).offset, n * 16);
		EXPECT_EQ(d.At(i).content_id, 0x1000ull + n);
	}
	d.Clear();
	EXPECT_EQ(d.Size(), 0u);
	for (u32 n = 0; n < 4096; n++)
		EXPECT_EQ(d.FindByRegisters(keys[n]), Dedup::kNone) << "entry " << n;
}

// The generation bump is the register key's entire invalidation mechanism: the registers are
// unchanged and the RAM is not, and the entry must not be served.
TEST(GSTileClutStreamDedup, ACLUTRamChangeInvalidatesTheRegisterKey)
{
	KeyInputs in;
	Dedup d;
	d.Insert(in.Build(), 1, 0, 256);
	ASSERT_NE(d.FindByRegisters(in.Build()), Dedup::kNone);

	in.gen++;
	EXPECT_EQ(d.FindByRegisters(in.Build()), Dedup::kNone);
}

TEST(GSTileClutStreamDedup, ClearEmptiesBothKeys)
{
	const KeyInputs in;
	Dedup d;
	std::array<u32, 256> words{};
	words.fill(0x11223344u);
	d.Insert(in.Build(), 99, 0, 256);
	EXPECT_EQ(d.Size(), 1u);
	ASSERT_NE(d.FindByContent(99, words.data(), words.data(), 256), Dedup::kNone);

	d.Clear();
	EXPECT_EQ(d.Size(), 0u);
	EXPECT_EQ(d.FindByRegisters(in.Build()), Dedup::kNone);
	EXPECT_EQ(d.FindByContent(99, words.data(), words.data(), 256), Dedup::kNone);
}

// ---------------------------------------------------------------------------------------------
// The content key, and the compare behind it.
// ---------------------------------------------------------------------------------------------

TEST(GSTileClutStreamDedup, AContentHitIsDecidedByTheWordsAndNotTheHash)
{
	std::vector<u32> stream(256);
	for (u32 i = 0; i < 256; i++)
		stream[i] = 0x1000u + i;

	std::array<u32, 256> same{};
	std::copy(stream.begin(), stream.end(), same.begin());
	std::array<u32, 256> different = same;
	different[137] ^= 1u; // one bit, in the middle, where a length or endpoint check would miss it

	const u64 id = GSTilePaletteCache::ContentId(stream.data(), 256);
	Dedup d;
	d.Insert(KeyInputs{}.Build(), id, 0, 256);

	ASSERT_NE(d.FindByContent(id, stream.data(), same.data(), 256), Dedup::kNone);
	// The same id offered for different words is what a 64-bit hash collision looks like from
	// here. It must cost a compare and an append, never a wrong palette.
	EXPECT_EQ(d.FindByContent(id, stream.data(), different.data(), 256), Dedup::kNone);
}

TEST(GSTileClutStreamDedup, AContentHitRefusesADifferentEntryCount)
{
	std::vector<u32> stream(256, 0x77u);
	const u64 id = GSTilePaletteCache::ContentId(stream.data(), 256);
	Dedup d;
	d.Insert(KeyInputs{}.Build(), id, 0, 256);
	EXPECT_EQ(d.FindByContent(id, stream.data(), stream.data(), 16), Dedup::kNone);
}

// The alias the second level installs: words already in the stream get the new register key
// pointed at them, so the NEXT draw at that register state costs no hash either.
TEST(GSTileClutStreamDedup, InsertRegistersAliasesAnExistingStreamEntry)
{
	// The palette sits 64 words into the stream, as a second one in a real plan would, so the
	// content compare has to honour the offset rather than always starting at word zero.
	constexpr u32 kAt = 64;
	std::vector<u32> stream(kAt + 256, 0u);
	std::array<u32, 256> words{};
	for (u32 e = 0; e < 256; e++)
		words[e] = 0x55u + e;
	std::copy(words.begin(), words.end(), stream.begin() + kAt);

	const u64 id = GSTilePaletteCache::ContentId(words.data(), 256);
	KeyInputs first;
	Dedup d;
	d.Insert(first.Build(), id, kAt, 256);

	KeyInputs reloaded = first;
	reloaded.gen++; // the same bytes, loaded again
	ASSERT_EQ(d.FindByRegisters(reloaded.Build()), Dedup::kNone);
	const u32 by_content = d.FindByContent(id, stream.data(), words.data(), 256);
	ASSERT_NE(by_content, Dedup::kNone);
	d.AliasRegisters(reloaded.Build(), by_content);

	const u32 i = d.FindByRegisters(reloaded.Build());
	ASSERT_NE(i, Dedup::kNone);
	EXPECT_EQ(d.At(i).offset, kAt);
	EXPECT_EQ(d.Size(), 1u) << "aliasing must not add a second copy of the palette";
}

// ---------------------------------------------------------------------------------------------
// The soundness property, against the real GSClut: equal key implies byte-identical expansion.
// ---------------------------------------------------------------------------------------------

namespace
{
	class ClutDedupTest : public ::testing::Test
	{
	protected:
		void SetUp() override { m_mem = std::make_unique<GSLocalMemory>(); }

		// A 32x32 texel rect of 32-bit words at cbp, covering every block a CSM1 load of any index
		// width reads. `salt` distinguishes one palette's bytes from another's.
		void SeedPalette(u32 cbp, u32 salt)
		{
			const GSLocalMemory::psm_t& fmt = GSLocalMemory::m_psm[PSMCT32];
			for (int y = 0; y < 32; y++)
			{
				for (int x = 0; x < 32; x++)
					(m_mem.get()->*fmt.wp)(x, y, Seed32(x, y, salt), cbp, 1);
			}
		}

		void Load(const GIFRegTEX0& t)
		{
			const GIFRegTEXCLUT tc = {};
			m_mem->m_clut.WriteDecision(t, tc);
			m_mem->m_clut.WriteLoad(t, tc);
		}

		// One "draw": expand the CLUT the way AccumulateDraw does, and return the key beside the
		// words, so a test can assert the two agree.
		struct Observation
		{
			Key key;
			std::vector<u32> words;
			u64 content_id;
		};

		Observation Observe(const GIFRegTEX0& t, const GIFRegTEXA& a)
		{
			const u32 entries = GSLocalMemory::m_psm[t.PSM].pal;
			m_mem->m_clut.Read32(t, a);
			const u32* expanded = m_mem->m_clut;
			Observation o;
			o.key = Dedup::MakeKey(m_mem->m_clut, t, a, entries);
			o.words.assign(expanded, expanded + entries);
			o.content_id = GSTilePaletteCache::ContentId(expanded, entries);
			return o;
		}

		std::unique_ptr<GSLocalMemory> m_mem;
	};
} // namespace

// The whole claim, over a script that exercises every road the key has to survive: two palettes at
// two CBPs, two CSAs of each, reloads, revisits, and a device-side sync in the middle.
TEST_F(ClutDedupTest, EqualKeyImpliesEqualExpansion)
{
	SeedPalette(0x00, 1);
	SeedPalette(0x40, 2);
	GIFRegTEXA texa = {};

	std::vector<Observation> obs;
	const auto load_and_observe = [&](u32 cbp, u32 salt, u32 csa, u32 psm, bool reload) {
		if (salt != 0)
			SeedPalette(cbp, salt);
		const GIFRegTEX0 t = ClutTEX0(cbp, PSMCT32, csa, psm);
		if (reload)
			Load(t);
		obs.push_back(Observe(t, texa));
	};

	load_and_observe(0x00, 0, 0, PSMT8, true);   // palette A
	load_and_observe(0x40, 0, 0, PSMT8, true);   // palette B
	load_and_observe(0x00, 0, 0, PSMT8, true);   // back to A -- a reload, so a fresh key
	load_and_observe(0x00, 0, 0, PSMT8, false);  // A again with nothing touched -- key must hold
	load_and_observe(0x00, 0, 3, PSMT4, false);  // a four-bit read of slot 3 of the same load
	load_and_observe(0x00, 0, 0, PSMT4, false);  // ...and of slot 0
	load_and_observe(0x00, 3, 0, PSMT8, true);   // A's memory REWRITTEN, then reloaded
	{
		// The device-sync road: a palette the CLUT gather loaded off a target, written back into
		// the CLUT RAM. It dirties the read side without any load from local memory, and until the
		// write generation covered it a key built here matched the one before it.
		std::array<u32, 256> synced{};
		for (u32 e = 0; e < 256; e++)
			synced[e] = 0xFF000000u | (e * 7u + 1u);
		m_mem->m_clut.SetEntries32(0, 256, synced.data());
		obs.push_back(Observe(ClutTEX0(0x00, PSMCT32, 0, PSMT8), texa));
	}

	ASSERT_GE(obs.size(), 8u);
	bool any_equal_pair = false;
	for (size_t i = 0; i < obs.size(); i++)
	{
		for (size_t j = i + 1; j < obs.size(); j++)
		{
			if (obs[i].key != obs[j].key)
				continue;
			any_equal_pair = true;
			EXPECT_EQ(obs[i].words, obs[j].words) << "observations " << i << " and " << j
												  << " share a key but not their expansion";
		}
	}
	EXPECT_TRUE(any_equal_pair) << "the script must contain a genuine key repeat or it proves nothing";
}

// The register key's own population, stated as a positive: a re-read of a CLUT nothing has touched
// costs no hash and no append.
TEST_F(ClutDedupTest, ARereadWithNothingTouchedHoldsTheKey)
{
	SeedPalette(0x00, 1);
	const GIFRegTEX0 t = ClutTEX0(0x00, PSMCT32, 0, PSMT8);
	Load(t);
	const Observation first = Observe(t, GIFRegTEXA{});
	const Observation again = Observe(t, GIFRegTEXA{});
	EXPECT_TRUE(first.key == again.key);
	EXPECT_EQ(first.words, again.words);
}

// The measured shape of rcuya-effects, and the reason the content key exists. Reloading the SAME
// bytes moves the RAM generation, so the register key cannot see the repeat -- while the content
// id is unchanged, so the content key can.
TEST_F(ClutDedupTest, AReloadOfIdenticalBytesMovesTheRegisterKeyButNotTheContentId)
{
	SeedPalette(0x00, 1);
	const GIFRegTEX0 t = ClutTEX0(0x00, PSMCT32, 0, PSMT8);
	Load(t);
	const Observation first = Observe(t, GIFRegTEXA{});
	Load(t); // the game reloads its CLUT before the next draw; nothing in memory changed
	const Observation second = Observe(t, GIFRegTEXA{});

	EXPECT_TRUE(first.key != second.key) << "a reload moves the RAM generation, by design";
	EXPECT_EQ(first.words, second.words);
	EXPECT_EQ(first.content_id, second.content_id);

	// ...and the dedup therefore serves the second draw out of the first's stream words.
	Dedup d;
	d.Insert(first.key, first.content_id, 0, 256);
	EXPECT_EQ(d.FindByRegisters(second.key), Dedup::kNone);
	EXPECT_NE(d.FindByContent(second.content_id, first.words.data(), second.words.data(), 256), Dedup::kNone);
}

// A palette written back from the device dirties the read side with no load at all. The write
// generation used to miss it outright; a stream dedup keyed on it would have served stale words.
TEST_F(ClutDedupTest, ADeviceSyncMovesTheKey)
{
	SeedPalette(0x00, 1);
	const GIFRegTEX0 t = ClutTEX0(0x00, PSMCT32, 0, PSMT8);
	Load(t);
	const Observation before = Observe(t, GIFRegTEXA{});

	std::array<u32, 256> synced{};
	for (u32 e = 0; e < 256; e++)
		synced[e] = 0xFF000000u | (e * 13u + 5u);
	m_mem->m_clut.SetEntries32(0, 256, synced.data());
	const Observation after = Observe(t, GIFRegTEXA{});

	EXPECT_NE(before.words, after.words) << "the sync must actually have changed the expansion";
	EXPECT_TRUE(before.key != after.key);
}

// ---------------------------------------------------------------------------------------------
// The palette cache, handed the id instead of re-deriving it.
// ---------------------------------------------------------------------------------------------

namespace
{
	class PaletteCacheIdTest : public ::testing::Test
	{
	protected:
		void SetUp() override { g_gs_device = std::make_unique<GSTileTest::StubDevice>(); }
		void TearDown() override
		{
			m_cache.Clear();
			g_gs_device.reset();
		}

		static std::array<u32, 256> Palette(u32 salt)
		{
			std::array<u32, 256> w{};
			for (u32 e = 0; e < 256; e++)
				w[e] = 0xFF000000u | (e * 2654435761u) ^ (salt << 8);
			return w;
		}

		GSTilePaletteCache m_cache;
	};
} // namespace

// The overload's whole contract: same texture, same id, same cache state. The read generations are
// stepped so neither call can be answered by the last-answer memo -- the point is the CONTENT
// path, and a memo hit would prove nothing about it.
TEST_F(PaletteCacheIdTest, ThePrehashedLookupIsTheHashingLookup)
{
	const auto words = Palette(1);
	const u64 id = GSTilePaletteCache::ContentId(words.data(), 256);

	u64 hashed_id = 0;
	GSTexture* const built = m_cache.Lookup(words.data(), 256, 1, &hashed_id);
	ASSERT_NE(built, nullptr);
	EXPECT_EQ(hashed_id, id);
	EXPECT_EQ(m_cache.Builds(), 1u);

	u64 carried_id = 0;
	GSTexture* const same = m_cache.Lookup(words.data(), 256, 2, id, &carried_id);
	EXPECT_EQ(same, built) << "the carried id must find the entry the hash built";
	EXPECT_EQ(carried_id, id);
	EXPECT_EQ(m_cache.Builds(), 1u) << "no rebuild";
	EXPECT_EQ(m_cache.Hits(), 1u);

	// ...and the hashing form still finds what the carried one built, which is what keeps the two
	// callers of this cache sharing entries rather than each holding their own.
	const auto other = Palette(2);
	const u64 other_id = GSTilePaletteCache::ContentId(other.data(), 256);
	u64 out = 0;
	GSTexture* const carried_build = m_cache.Lookup(other.data(), 256, 3, other_id, &out);
	ASSERT_NE(carried_build, nullptr);
	EXPECT_EQ(m_cache.Builds(), 2u);
	u64 out2 = 0;
	EXPECT_EQ(m_cache.Lookup(other.data(), 256, 4, &out2), carried_build);
	EXPECT_EQ(out2, other_id);
	EXPECT_EQ(m_cache.Builds(), 2u);
}

// The memo in front of both forms is shared, and it must stay in front of the carried-id form too:
// it is where last_use and pinned_frame are refreshed, so skipping it would move eviction order
// even though it hands back the same texture.
TEST_F(PaletteCacheIdTest, TheReadGenerationMemoStillRunsForTheCarriedIdForm)
{
	const auto words = Palette(3);
	const u64 id = GSTilePaletteCache::ContentId(words.data(), 256);

	u64 out = 0;
	GSTexture* const built = m_cache.Lookup(words.data(), 256, 9, id, &out);
	ASSERT_NE(built, nullptr);
	const u64 hits_before = m_cache.Hits();

	// Same read generation: the memo answers, and the words are not even looked at.
	std::array<u32, 256> lies{};
	lies.fill(0xDEADBEEFu);
	u64 memo_id = 0;
	EXPECT_EQ(m_cache.Lookup(lies.data(), 256, 9, 0ull, &memo_id), built);
	EXPECT_EQ(memo_id, id);
	EXPECT_EQ(m_cache.Hits(), hits_before + 1);
	EXPECT_EQ(m_cache.Builds(), 1u);
}
