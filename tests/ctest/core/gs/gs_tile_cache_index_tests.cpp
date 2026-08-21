// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The two Tile texture caches' SLOT BOOKKEEPING — which slot holds a key, which slot is free, and
// which occupied slot gets evicted — against a naive model of the same rules.
//
// Both caches used to answer all three questions with one linear walk of their 512 entries, and now
// answer them off a hash index and an intrusive recency list (GSTileSlotIndex). That change is
// supposed to be invisible: same entry served on every hit, same victim on every eviction, same
// counters moving at the same times. "Supposed to be invisible" is exactly the class of change that
// hides a defect for months, because the cheap check — does it still render — passes on a cache
// that quietly evicts the wrong entry and rebuilds it a frame later.
//
// So the property is pinned directly. A seeded random driver runs thousands of mixed operations
// (lookups, probes, pin-frame advances, content invalidation, occasional Clear) over a key pool
// deliberately larger than the cache, so hits, misses, admissions, evictions, pin refusals and
// capacity refusals all occur in one run, and every step is compared against a reference model that
// shares no data structure with the cache: identity through a std::unordered_map, recency through an
// explicit ordered vector, the free slot and the victim through plain scans.
//
// The reference derives its victim TWICE — once from the recency ordering and once from the
// last_use stamps — and asserts the two agree. That is the mirroring property the intrusive list
// has to preserve, stated in the model rather than assumed by it: if a `last_use` write were ever
// added to the cache without its matching Touch, the two orders would drift and the run would fail
// on the first eviction that straddled the drift.

#include "GSTileStubDevice.h"

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"
#include "GS/Renderers/Tile/GSTileExpandedCache.h"
#include "GS/Renderers/Tile/GSTileTextureSource.h"
#include "GS/Renderers/Tile/GSVramModel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	using GSTileTest::StubDevice;

	// Both caches hold this many entries. The reference has to agree on the number, because the
	// interesting behaviour (admission, eviction, the capacity refusal) only starts at the bound.
	constexpr int kCapacity = 512;
	constexpr int kNone = -1;

	// The slot rules, modelled the obvious way. Nothing here shares a structure with
	// GSTileSlotIndex — that is the point of having it.
	template <typename Key, typename KeyHash>
	class RefSlots
	{
	public:
		int Find(const Key& k) const
		{
			const auto it = m_by_key.find(k);
			return (it == m_by_key.end()) ? kNone : it->second;
		}

		int FreeSlot() const
		{
			for (int i = 0; i < kCapacity; i++)
			{
				if (!m_alive[i])
					return i;
			}
			return kNone;
		}

		/// The least recently used slot `ok` accepts. Derived from the explicit ordering AND from
		/// the last_use stamps, with the two required to agree — see the file comment.
		template <typename Ok>
		int Victim(Ok&& ok) const
		{
			int by_order = kNone;
			for (auto it = m_order.rbegin(); it != m_order.rend(); ++it)
			{
				if (ok(*it))
				{
					by_order = *it;
					break;
				}
			}
			int by_stamp = kNone;
			for (int i = 0; i < kCapacity; i++)
			{
				if (m_alive[i] && ok(i) && (by_stamp == kNone || m_use[i] < m_use[by_stamp]))
					by_stamp = i;
			}
			EXPECT_EQ(by_order, by_stamp) << "the reference model's own two recency orders disagree";
			return by_order;
		}

		void Occupy(int slot, const Key& k)
		{
			m_alive[slot] = true;
			m_key[slot] = k;
			m_by_key[k] = slot;
			Touch(slot);
		}

		void Release(int slot)
		{
			m_alive[slot] = false;
			m_by_key.erase(m_key[slot]);
			const auto at = std::find(m_order.begin(), m_order.end(), slot);
			if (at != m_order.end())
				m_order.erase(at);
			m_use[slot] = 0;
		}

		void Touch(int slot)
		{
			const auto at = std::find(m_order.begin(), m_order.end(), slot);
			if (at != m_order.end())
				m_order.erase(at);
			m_order.insert(m_order.begin(), slot);
			m_use[slot] = ++m_use_counter;
		}

		void Clear()
		{
			m_alive.fill(false);
			m_use.fill(0);
			m_by_key.clear();
			m_order.clear();
			m_use_counter = 0;
		}

		bool Alive(int slot) const { return m_alive[slot]; }

	private:
		std::array<bool, kCapacity> m_alive{};
		std::array<Key, kCapacity> m_key{};
		std::array<u64, kCapacity> m_use{};
		std::vector<int> m_order; ///< front = most recently used
		std::unordered_map<Key, int, KeyHash> m_by_key;
		u64 m_use_counter = 0;
	};

	// ---------------------------------------------------------------------------------------
	// GSTileExpandedCache
	// ---------------------------------------------------------------------------------------

	struct PairKey
	{
		u64 index_id = 0;
		u64 palette_id = 0;
		bool operator==(const PairKey& o) const { return index_id == o.index_id && palette_id == o.palette_id; }
	};
	struct PairKeyHash
	{
		size_t operator()(const PairKey& k) const
		{
			return std::hash<u64>()(k.index_id) * 1099511628211ull ^ std::hash<u64>()(k.palette_id);
		}
	};

	class RefExpanded
	{
	public:
		using Outcome = GSTileExpandedCache::BuiltOutcome;
		using Admission = GSTileExpandedCache::Admission;

		u64 hits = 0, builds = 0, passes = 0, deferrals = 0, cap_refusals = 0, evictions = 0;

		void NextFrame() { m_frame++; }
		void SetPinFrame(u64 f) { m_pin = f; }

		void Clear()
		{
			m_slots.Clear();
			m_has_tex.fill(false);
			m_seen.fill(0);
			m_pinned.fill(0);
		}

		/// Lookup and LookupBuilt take exactly the same decisions; they differ in how the expansion
		/// is produced and in how many passes one build costs.
		/// `admit_now` is the FirstSight admission the donor road asks for: the second-sight wait is
		/// waived and a fresh pair builds straight away. Modelled here rather than left to the live
		/// class, because a rule that only one caller uses is exactly the rule an equivalence test is
		/// for -- everything else about the entry (its slot, its recency, the pin) has to be
		/// identical to what the ordinary road would have produced.
		Outcome Serve(const PairKey& k, bool build_ok, u32 levels, int* served_slot, bool admit_now = false)
		{
			*served_slot = kNone;
			int s = m_slots.Find(k);
			if (s != kNone && m_has_tex[s])
			{
				m_slots.Touch(s);
				m_pinned[s] = m_pin;
				hits++;
				*served_slot = s;
				return Outcome::Hit;
			}
			if (s == kNone)
			{
				s = Admit(k);
				if (s == kNone)
					return Outcome::RefusedCapacity;
				if (!admit_now)
				{
					deferrals++;
					return Outcome::Deferred;
				}
			}
			else if (m_seen[s] == m_frame && !admit_now)
			{
				m_slots.Touch(s);
				deferrals++;
				return Outcome::Deferred;
			}
			if (!build_ok)
				return Outcome::Failed;
			passes += std::max<u32>(levels, 1);
			m_has_tex[s] = true;
			m_slots.Touch(s);
			m_pinned[s] = m_pin;
			builds++;
			*served_slot = s;
			return Outcome::Built;
		}

		Admission ProbeAdmit(const PairKey& k, bool admit_now = false)
		{
			const int s = m_slots.Find(k);
			if (s == kNone)
			{
				if (Admit(k) == kNone)
					return Admission::Deferred;
				return admit_now ? Admission::Served : Admission::Deferred;
			}
			m_slots.Touch(s);
			if (m_has_tex[s])
				m_pinned[s] = m_pin;
			if (!m_has_tex[s] && m_seen[s] == m_frame)
				return admit_now ? Admission::Served : Admission::Deferred;
			return Admission::Served;
		}

		/// Which key a slot currently holds a texture for, so the test can check the cache handed
		/// back the texture that belongs to the key it asked about.
		bool HasTexture(int slot) const { return slot != kNone && m_has_tex[slot]; }

	private:
		int Admit(const PairKey& k)
		{
			int slot = m_slots.FreeSlot();
			const bool from_free = (slot != kNone);
			if (!from_free)
				slot = m_slots.Victim([this](int i) { return !Pinned(i); });
			if (slot == kNone)
			{
				cap_refusals++;
				return kNone;
			}
			if (!from_free)
			{
				evictions++;
				m_slots.Release(slot);
			}
			m_has_tex[slot] = false;
			m_slots.Occupy(slot, k);
			m_pinned[slot] = 0;
			m_seen[slot] = m_frame;
			return slot;
		}

		bool Pinned(int i) const { return m_pin != 0 && m_pinned[i] == m_pin; }

		RefSlots<PairKey, PairKeyHash> m_slots;
		std::array<bool, kCapacity> m_has_tex{};
		std::array<u64, kCapacity> m_seen{};
		std::array<u64, kCapacity> m_pinned{};
		u64 m_frame = 0;
		u64 m_pin = 0;
	};

	// ---------------------------------------------------------------------------------------
	// GSTileTextureSource
	// ---------------------------------------------------------------------------------------

	struct WinKey
	{
		u64 w[4] = {0, 0, 0, 0};
		bool operator==(const WinKey& o) const
		{
			return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2] && w[3] == o.w[3];
		}
	};
	struct WinKeyHash
	{
		size_t operator()(const WinKey& k) const
		{
			size_t h = 0;
			for (u64 x : k.w)
				h = h * 1099511628211ull ^ std::hash<u64>()(x);
			return h;
		}
	};

	WinKey KeyOf(const GSTileTextureSource::WindowKey& k)
	{
		return WinKey{{k.reg, k.aux, k.mip[0], k.mip[1]}};
	}

	class RefSource
	{
	public:
		using Outcome = GSTileTextureSource::BuiltOutcome;
		using Token = GSTileTextureSource::ContentToken;

		u64 hits = 0, builds = 0, same_pages = 0, pin_refusals = 0, cap_refusals = 0, evictions = 0;

		void SetFrame(u64 f) { m_frame = f; }

		void Clear()
		{
			m_slots.Clear();
			m_stamp.fill(0);
			m_build_id.fill(0);
			m_content.fill(Token{});
			m_pinned.fill(0);
			// The cache's build counter survives Clear (only Reset drops the whole class), so the
			// model's must too.
		}

		u32 PinnedEntries() const
		{
			u32 n = 0;
			for (int i = 0; i < kCapacity; i++)
				n += (m_slots.Alive(i) && Pinned(i)) ? 1 : 0;
			return n;
		}

		/// The device build road. `build_ok` is what the caller's builder will answer.
		Outcome LookupBuilt(const WinKey& k, u64 stamp, const Token& content, bool build_ok, u64* build_id)
		{
			*build_id = 0;
			const int s = m_slots.Find(k);
			if (s != kNone)
			{
				m_slots.Touch(s);
				const bool pinned = Pinned(s);
				m_pinned[s] = m_frame;
				if (m_stamp[s] == stamp)
				{
					hits++;
					*build_id = m_build_id[s];
					return Outcome::Hit;
				}
				if (content == m_content[s])
				{
					m_stamp[s] = stamp;
					same_pages++;
					*build_id = m_build_id[s];
					return Outcome::Rescued;
				}
				if (pinned)
				{
					pin_refusals++;
					return Outcome::RefusedPinned;
				}
				if (!build_ok)
				{
					m_slots.Release(s);
					m_pinned[s] = 0;
					return Outcome::Failed;
				}
				m_build_id[s] = ++m_build_counter;
				builds++;
				m_stamp[s] = stamp;
				m_content[s] = content;
				*build_id = m_build_id[s];
				return Outcome::Built;
			}

			int slot = m_slots.FreeSlot();
			const bool from_free = (slot != kNone);
			if (!from_free)
				slot = m_slots.Victim([this](int i) { return !Pinned(i); });
			if (slot == kNone)
			{
				cap_refusals++;
				return Outcome::RefusedCapacity;
			}
			if (!from_free)
			{
				evictions++;
				m_slots.Release(slot);
			}
			m_slots.Occupy(slot, k);
			m_pinned[slot] = m_frame;
			m_content[slot] = Token{};
			m_build_id[slot] = 0;
			if (!build_ok)
			{
				m_slots.Release(slot);
				m_pinned[slot] = 0;
				return Outcome::Failed;
			}
			m_build_id[slot] = ++m_build_counter;
			builds++;
			m_stamp[slot] = stamp;
			m_content[slot] = content;
			*build_id = m_build_id[slot];
			return Outcome::Built;
		}

		/// The CPU build road, driven with a frozen gen stamp so the content tiers never engage and
		/// what is under test is exactly the identity-and-recency layer.
		Outcome LookupFrozen(const WinKey& k, u64 stamp, u64* build_id)
		{
			*build_id = 0;
			const int s = m_slots.Find(k);
			if (s != kNone)
			{
				m_slots.Touch(s);
				m_pinned[s] = m_frame;
				EXPECT_EQ(m_stamp[s], stamp) << "the frozen-stamp driver moved a stamp";
				hits++;
				*build_id = m_build_id[s];
				return Outcome::Hit;
			}
			int slot = m_slots.FreeSlot();
			const bool from_free = (slot != kNone);
			if (!from_free)
				slot = m_slots.Victim([this](int i) { return !Pinned(i); });
			if (slot == kNone)
			{
				cap_refusals++;
				return Outcome::RefusedCapacity;
			}
			if (!from_free)
			{
				evictions++;
				m_slots.Release(slot);
			}
			m_slots.Occupy(slot, k);
			m_pinned[slot] = m_frame;
			m_build_id[slot] = ++m_build_counter;
			builds++;
			m_stamp[slot] = stamp;
			*build_id = m_build_id[slot];
			return Outcome::Built;
		}

		/// Probe's verdict: hit, would-rebuild, or nothing at all. Every entry on the roads driven
		/// here is built over the whole window, so the valid-rect leg is always satisfied.
		void Probe(const WinKey& k, u64 stamp, bool* hit, bool* would_rebuild, u64* build_id) const
		{
			*hit = false;
			*would_rebuild = false;
			*build_id = 0;
			const int s = m_slots.Find(k);
			if (s == kNone)
				return;
			if (m_stamp[s] == stamp)
			{
				*hit = true;
				*build_id = m_build_id[s];
			}
			else
			{
				*would_rebuild = true;
			}
		}

	private:
		bool Pinned(int i) const { return m_frame != 0 && m_pinned[i] == m_frame; }

		RefSlots<WinKey, WinKeyHash> m_slots;
		std::array<u64, kCapacity> m_stamp{};
		std::array<u64, kCapacity> m_build_id{};
		std::array<GSTileTextureSource::ContentToken, kCapacity> m_content{};
		std::array<u64, kCapacity> m_pinned{};
		u64 m_frame = 0;
		u64 m_build_counter = 0;
	};

	// ---------------------------------------------------------------------------------------

	class ExpandBuilder final : public GSTileExpandBuilder
	{
	public:
		bool refuse = false;
		bool BuildTileExpansion(GSTexture*, GSTexture*, GSTexture*) override { return !refuse; }
	};

	class SourceBuilder final : public GSTileSourceBuilder
	{
	public:
		bool refuse = false;
		bool BuildTileSource(GSTexture*, const GIFRegTEX0&, const GIFRegTEXA&) override { return !refuse; }
	};

	GSPageBitmap PagesOf(u32 first, u32 count)
	{
		GSPageBitmap b;
		for (u32 i = 0; i < count; i++)
			b.set((first + i) % GS_MAX_PAGES);
		return b;
	}

	class TileCacheIndexTest : public ::testing::Test
	{
	protected:
		void SetUp() override { g_gs_device = std::make_unique<StubDevice>(); }
		void TearDown() override { g_gs_device.reset(); }

		StubDevice& Device() { return static_cast<StubDevice&>(*g_gs_device); }
	};

	// The seed is fixed so a failure is reproducible, and printed so a bisect can re-run it.
	constexpr u32 kSeed = 0x5C5EED01u;
} // namespace

// The palette-expanded cache: admission markers, hits, deferrals inside a frame, evictions, and the
// capacity refusal every road shares. The key pool is 720 pairs against 512 slots, so the LRU is
// load-bearing rather than decorative — a cache that picked a different victim would start missing
// on keys the model still holds within a few hundred operations. The random traffic is followed by
// a directed storm, because the two refusal legs need the whole cache pinned at once and random
// traffic does not reliably get there.
TEST_F(TileCacheIndexTest, ExpandedCacheMatchesReferenceUnderRandomTraffic)
{
	SCOPED_TRACE("seed=" + std::to_string(kSeed));

	GSTileExpandedCache cache;
	RefExpanded ref;
	ExpandBuilder builder;

	std::unique_ptr<GSTexture> index_tex(
		g_gs_device->CreateRenderTarget(64, 64, GSTexture::Format::Color, false, true));
	std::unique_ptr<GSTexture> palette_tex(
		g_gs_device->CreateRenderTarget(16, 16, GSTexture::Format::Color, false, true));
	ASSERT_NE(index_tex, nullptr);
	ASSERT_NE(palette_tex, nullptr);

	// What the cache handed back for a pair the last time it built one. A wrong victim shows up
	// here as a stale pointer coming back for a key the model says was evicted.
	std::unordered_map<PairKey, GSTexture*, PairKeyHash> served;

	const auto check_counters = [&]() {
		ASSERT_EQ(cache.Hits(), ref.hits) << "hits";
		ASSERT_EQ(cache.Builds(), ref.builds) << "builds";
		ASSERT_EQ(cache.Passes(), ref.passes) << "passes";
		ASSERT_EQ(cache.Deferrals(), ref.deferrals) << "deferrals";
		ASSERT_EQ(cache.CapacityRefusals(), ref.cap_refusals) << "capacity refusals";
		ASSERT_TRUE(cache.Serves()) << "the stub device never refuses an expansion";
	};

	const auto check_texture = [&](const PairKey& key, GSTexture* tex,
								   GSTileExpandedCache::BuiltOutcome want) {
		if (want == GSTileExpandedCache::BuiltOutcome::Hit)
		{
			ASSERT_NE(tex, nullptr);
			ASSERT_EQ(tex, served[key]) << "a hit served a texture that is not this pair's";
		}
		else if (want == GSTileExpandedCache::BuiltOutcome::Built)
		{
			ASSERT_NE(tex, nullptr);
			served[key] = tex;
		}
		else
		{
			ASSERT_EQ(tex, nullptr);
		}
	};

	const auto do_probe = [&](const PairKey& key, bool admit_now = false) {
		const auto when = admit_now ? GSTileExpandedCache::AdmitWhen::FirstSight :
									  GSTileExpandedCache::AdmitWhen::SecondSight;
		const GSTileExpandedCache::Admission got = cache.ProbeAdmit(key.index_id, key.palette_id, when);
		const GSTileExpandedCache::Admission want = ref.ProbeAdmit(key, admit_now);
		ASSERT_EQ(static_cast<int>(got), static_cast<int>(want)) << "ProbeAdmit verdict";
		check_counters();
	};

	const auto do_lookup_built = [&](const PairKey& key, bool admit_now = false) {
		GSTileExpandedCache::BuiltOutcome got{};
		const auto when = admit_now ? GSTileExpandedCache::AdmitWhen::FirstSight :
									  GSTileExpandedCache::AdmitWhen::SecondSight;
		GSTexture* tex = cache.LookupBuilt(index_tex.get(), key.index_id, palette_tex.get(), key.palette_id,
			builder, when, &got);
		int slot = kNone;
		const GSTileExpandedCache::BuiltOutcome want = ref.Serve(key, !builder.refuse, 1, &slot, admit_now);
		ASSERT_EQ(static_cast<int>(got), static_cast<int>(want)) << "LookupBuilt outcome";
		check_texture(key, tex, want);
		check_counters();
	};

	const auto do_lookup = [&](const PairKey& key, u32 levels) {
		GSTexture* tex = cache.Lookup(index_tex.get(), key.index_id, palette_tex.get(), key.palette_id, levels);
		int slot = kNone;
		const GSTileExpandedCache::BuiltOutcome want = ref.Serve(key, true, levels, &slot);
		check_texture(key, tex, want);
		check_counters();
	};

	std::mt19937 rng(kSeed);
	u64 pin_frame = 1;
	cache.SetFrame(pin_frame);
	ref.SetPinFrame(pin_frame);

	for (int step = 0; step < 20000; step++)
	{
		SCOPED_TRACE("random step=" + std::to_string(step));
		if (step == 7000 || step == 14000)
		{
			cache.Clear();
			ref.Clear();
			served.clear();
			ASSERT_EQ(cache.CapacityRefusals(), ref.cap_refusals);
			continue;
		}
		const u32 roll = rng() % 100;

		if (roll < 3)
		{
			// The admission filter's clock. Its whole job is to make a pair prove itself across a
			// frame boundary, so the boundary has to happen often enough to matter.
			cache.NextFrame();
			ref.NextFrame();
			continue;
		}
		if (roll < 6)
		{
			// The PIN clock, which is the plan-executed clock and not the video one. Advancing it
			// frees the working set for eviction again.
			pin_frame++;
			cache.SetFrame(pin_frame);
			ref.SetPinFrame(pin_frame);
			continue;
		}
		const PairKey key{static_cast<u64>(rng() % 60) + 1, static_cast<u64>(rng() % 12) + 1};
		builder.refuse = (rng() % 64) == 0;
		// A minority of the traffic asks for the FirstSight admission the donor road uses, mixed in
		// with the ordinary road rather than run as a separate pass: the two rules share one cache,
		// and what could go wrong is one of them leaving an entry the other then misreads.
		const bool admit_now = (rng() % 5) == 0;

		if (roll < 40)
			ASSERT_NO_FATAL_FAILURE(do_probe(key, admit_now));
		else if (roll < 70)
			ASSERT_NO_FATAL_FAILURE(do_lookup_built(key, admit_now));
		else
			ASSERT_NO_FATAL_FAILURE(do_lookup(key, (rng() % 4 == 0) ? 2u : 1u));
	}

	// The directed storm. Every slot carries a real expansion and every one of them is named by a
	// draw this plan frame, so the cache has nothing it is allowed to take — which is the leg that
	// fails closed, and the one an LRU that walked the wrong end would silently break instead.
	cache.Clear();
	ref.Clear();
	served.clear();
	builder.refuse = false;
	cache.NextFrame();
	ref.NextFrame();
	pin_frame++;
	cache.SetFrame(pin_frame);
	ref.SetPinFrame(pin_frame);

	const auto storm_key = [](int i) {
		return PairKey{static_cast<u64>(0x100000 + i), 7};
	};
	for (int i = 0; i < kCapacity; i++)
	{
		SCOPED_TRACE("storm marker=" + std::to_string(i));
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(storm_key(i)));
	}
	cache.NextFrame();
	ref.NextFrame();
	for (int i = 0; i < kCapacity; i++)
	{
		SCOPED_TRACE("storm build=" + std::to_string(i));
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(storm_key(i)));
	}
	for (int i = 0; i < 8; i++)
	{
		SCOPED_TRACE("storm overflow=" + std::to_string(i));
		const PairKey key{static_cast<u64>(0x200000 + i), 9};
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(key));
		ASSERT_NO_FATAL_FAILURE(do_probe(key));
		// FirstSight has to fail closed against a full cache too: waiving the second-sight wait
		// waives the WAIT, never the pin, and a pair that cannot get a slot still refuses.
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(key, true));
		ASSERT_NO_FATAL_FAILURE(do_probe(key, true));
	}

	// Not a vacuous run: every road the gate cares about was actually reached.
	EXPECT_GT(cache.Hits(), 0u);
	EXPECT_GT(cache.Builds(), 0u);
	EXPECT_GT(cache.Deferrals(), 0u);
	EXPECT_GE(cache.CapacityRefusals(), 16u) << "the pin never held the whole cache down";
	EXPECT_GT(ref.evictions, 100u) << "the cache never filled, so the LRU was never asked anything";
}

// The source cache's DEVICE road (TileGpu's): identity over the full four-word window key, the
// content-token rescue, the pin refusal, eviction and the capacity refusal. Build ids are checked
// on every serve, which is the strongest single assertion available here — an id is stamped per
// build from one counter, so a divergence in which entry was built, kept or evicted moves it.
TEST_F(TileCacheIndexTest, SourceCacheDeviceRoadMatchesReferenceUnderRandomTraffic)
{
	SCOPED_TRACE("seed=" + std::to_string(kSeed));

	GSTileTextureSource cache;
	RefSource ref;
	SourceBuilder builder;
	GSVramModel model;

	// 640 distinct windows against 512 slots. TBP0 is 14 bits in the key, so the bases stay inside
	// it or two "distinct" windows would alias onto one entry.
	struct Window
	{
		GIFRegTEX0 tex0 = {};
		GIFRegTEXA texa = {};
		GSPageBitmap pages;
		WinKey key;
		u64 content = 0;
	};
	std::vector<Window> windows;
	for (u32 i = 0; i < 640; i++)
	{
		Window w;
		w.tex0.TBP0 = (i * 8) & 0x3FFF;
		w.tex0.TBW = 1 + (i % 3);
		w.tex0.PSM = (i % 3 == 0) ? PSMCT16 : PSMCT32;
		w.tex0.TW = 3;
		w.tex0.TH = 3;
		w.texa.TA0 = static_cast<u8>(i & 0xFF);
		w.texa.TA1 = static_cast<u8>((i >> 3) & 0xFF);
		w.texa.AEM = (i & 1);
		w.pages = PagesOf(i % 400, 2);
		w.key = KeyOf(GSTileTextureSource::KeyFor(w.tex0, w.texa));
		w.content = i + 1;
		windows.push_back(w);
	}
	// The keys have to be distinct or the model and the cache would legitimately disagree about
	// how many entries exist.
	{
		std::unordered_map<WinKey, int, WinKeyHash> seen;
		for (size_t i = 0; i < windows.size(); i++)
			ASSERT_TRUE(seen.emplace(windows[i].key, static_cast<int>(i)).second) << "window " << i;
	}

	std::unordered_map<WinKey, GSTexture*, WinKeyHash> served;

	const auto do_lookup_built = [&](Window& w, bool unknown_token) {
		const u64 stamp = GSTileTextureSource::GenStamp(model, w.pages);
		const GSTileTextureSource::ContentToken token =
			unknown_token ? GSTileTextureSource::ContentToken{} :
							GSTileTextureSource::ContentToken{
								GSTileTextureSource::ContentToken::Source::Pages, w.content};

		GSTileTextureSource::BuiltOutcome got{};
		u64 got_id = 0;
		GSTexture* tex = cache.LookupBuilt(model, w.tex0, w.texa, w.pages, builder, token, &got_id, &got);

		u64 want_id = 0;
		const GSTileTextureSource::BuiltOutcome want =
			ref.LookupBuilt(w.key, stamp, token, !builder.refuse, &want_id);

		ASSERT_EQ(static_cast<int>(got), static_cast<int>(want)) << "LookupBuilt outcome";
		ASSERT_EQ(got_id, want_id) << "build id";
		switch (want)
		{
			case GSTileTextureSource::BuiltOutcome::Hit:
			case GSTileTextureSource::BuiltOutcome::Rescued:
				ASSERT_NE(tex, nullptr);
				ASSERT_EQ(tex, served[w.key]) << "served a texture that is not this window's";
				break;
			case GSTileTextureSource::BuiltOutcome::Built:
				ASSERT_NE(tex, nullptr);
				served[w.key] = tex;
				break;
			default:
				ASSERT_EQ(tex, nullptr);
				break;
		}

		ASSERT_EQ(cache.Hits(), ref.hits) << "hits";
		ASSERT_EQ(cache.Builds(), ref.builds) << "builds";
		ASSERT_EQ(cache.RebuildsSamePages(), ref.same_pages) << "same-page rescues";
		ASSERT_EQ(cache.PinRefusals(), ref.pin_refusals) << "pin refusals";
		ASSERT_EQ(cache.CapacityRefusals(), ref.cap_refusals) << "capacity refusals";
		ASSERT_EQ(cache.PinnedEntries(), ref.PinnedEntries()) << "pinned entries";
	};

	const auto do_probe = [&](const Window& w) {
		const u64 stamp = GSTileTextureSource::GenStamp(model, w.pages);
		const GSTileTextureSource::ProbeResult got = cache.Probe(model, w.tex0, w.texa, w.pages);
		bool hit = false, would_rebuild = false;
		u64 build_id = 0;
		ref.Probe(w.key, stamp, &hit, &would_rebuild, &build_id);
		ASSERT_EQ(got.hit, hit) << "Probe hit";
		ASSERT_EQ(got.would_rebuild, would_rebuild) << "Probe would_rebuild";
		ASSERT_EQ(got.build_id, build_id) << "Probe build id";
		ASSERT_EQ(got.gen_stamp, stamp) << "Probe gen stamp";
	};

	std::mt19937 rng(kSeed);
	u64 frame = 1;
	cache.SetFrame(frame);
	ref.SetFrame(frame);

	for (int step = 0; step < 20000; step++)
	{
		SCOPED_TRACE("random step=" + std::to_string(step));
		if (step == 7000 || step == 14000)
		{
			cache.Clear();
			ref.Clear();
			served.clear();
			ASSERT_EQ(cache.PinnedEntries(), ref.PinnedEntries());
			continue;
		}
		const u32 roll = rng() % 100;

		if (roll < 4)
		{
			// The plan has executed: the working set is free again.
			frame++;
			cache.SetFrame(frame);
			ref.SetFrame(frame);
			continue;
		}
		if (roll < 8)
		{
			// Somebody wrote guest memory under a window. Its gen stamp moves, which is what puts
			// the rescue and rebuild legs in play.
			Window& w = windows[rng() % windows.size()];
			model.OnCpuWrite(w.pages, kGSTilePlanesColor);
			w.content++;
			continue;
		}
		Window& w = windows[rng() % windows.size()];
		if (roll < 35)
		{
			ASSERT_NO_FATAL_FAILURE(do_probe(w));
			continue;
		}
		// A quarter of the lookups arrive with a token the caller could not prove, which is never
		// equal to anything — including another unknown one — so it must never rescue.
		builder.refuse = (rng() % 64) == 0;
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(w, (rng() % 4) == 0));
	}

	// The directed storm: one frame that names every slot. The overflow has to refuse rather than
	// evict, and a window whose bytes moved under a recorded draw has to refuse rather than rebuild
	// in place — the two legs that only exist at the capacity bound.
	cache.Clear();
	ref.Clear();
	served.clear();
	builder.refuse = false;
	frame++;
	cache.SetFrame(frame);
	ref.SetFrame(frame);

	for (int i = 0; i < kCapacity; i++)
	{
		SCOPED_TRACE("storm build=" + std::to_string(i));
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(windows[i], false));
	}
	ASSERT_EQ(cache.PinnedEntries(), static_cast<u32>(kCapacity));
	for (int i = kCapacity; i < kCapacity + 8; i++)
	{
		SCOPED_TRACE("storm overflow=" + std::to_string(i));
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(windows[i], false));
		ASSERT_NO_FATAL_FAILURE(do_probe(windows[i]));
	}
	for (int i = 0; i < 8; i++)
	{
		SCOPED_TRACE("storm pinned rebuild=" + std::to_string(i));
		Window& w = windows[i];
		model.OnCpuWrite(w.pages, kGSTilePlanesColor);
		w.content++;
		ASSERT_NO_FATAL_FAILURE(do_lookup_built(w, false));
		ASSERT_NO_FATAL_FAILURE(do_probe(w));
	}

	EXPECT_GT(cache.Hits(), 0u);
	EXPECT_GT(cache.Builds(), 0u);
	EXPECT_GT(cache.RebuildsSamePages(), 0u) << "the content-token rescue never fired";
	EXPECT_GE(cache.PinRefusals(), 8u) << "the pin never refused a rebuild";
	EXPECT_GE(cache.CapacityRefusals(), 8u) << "the pin never held the whole cache down";
	EXPECT_GT(ref.evictions, 100u) << "the cache never filled, so the LRU was never asked anything";
}


// The source cache's CPU road (the Tile renderer's), which the TileGpu corpus never touches. Driven
// with a frozen gen stamp — nothing writes guest memory — so the content tiers stay out of the way
// and what is exercised is the layer this change actually replaced: find the window, take a free
// slot in the same order, evict the same entry. Mip pyramids are in the mix because the level
// register words are two of the four key words and no other road varies them.
TEST_F(TileCacheIndexTest, SourceCacheCpuRoadMatchesReferenceUnderRandomTraffic)
{
	SCOPED_TRACE("seed=" + std::to_string(kSeed));

	auto mem = std::make_unique<GSLocalMemory>();
	GSTileTextureSource cache;
	RefSource ref;
	GSVramModel model;

	struct Window
	{
		std::array<GIFRegTEX0, 2> levels_tex0 = {};
		u32 levels = 1;
		GIFRegTEXA texa = {};
		GSPageBitmap pages;
		WinKey key;
	};
	std::vector<Window> windows;
	for (u32 i = 0; i < 640; i++)
	{
		Window w;
		w.levels = (i % 5 == 0) ? 2u : 1u;
		w.levels_tex0[0].TBP0 = (i * 8) & 0x3FFF;
		w.levels_tex0[0].TBW = 1 + (i % 3);
		w.levels_tex0[0].PSM = PSMCT32;
		w.levels_tex0[0].TW = 3;
		w.levels_tex0[0].TH = 3;
		w.levels_tex0[1] = w.levels_tex0[0];
		w.levels_tex0[1].TBP0 = ((i * 8) + 4) & 0x3FFF;
		w.levels_tex0[1].TW = 2;
		w.levels_tex0[1].TH = 2;
		w.pages = PagesOf(i % 400, 2);
		w.key = KeyOf(GSTileTextureSource::KeyFor(w.levels_tex0[0], w.texa, w.levels_tex0.data(), w.levels));
		windows.push_back(w);
	}
	{
		std::unordered_map<WinKey, int, WinKeyHash> seen;
		for (size_t i = 0; i < windows.size(); i++)
			ASSERT_TRUE(seen.emplace(windows[i].key, static_cast<int>(i)).second) << "window " << i;
	}

	// The Tile renderer draws immediately, so the pin discipline is off — which is also what makes
	// every entry an eviction candidate and the recency order the only thing choosing between them.
	cache.SetFrame(0);
	ref.SetFrame(0);

	std::mt19937 rng(kSeed ^ 0x1234u);
	std::unordered_map<WinKey, GSTexture*, WinKeyHash> served;

	for (int step = 0; step < 12000; step++)
	{
		SCOPED_TRACE("step=" + std::to_string(step));
		if (step == 4000 || step == 8000)
		{
			cache.Clear();
			ref.Clear();
			served.clear();
			continue;
		}

		Window& w = windows[rng() % windows.size()];
		const u64 stamp = GSTileTextureSource::GenStamp(model, w.pages);
		ASSERT_EQ(stamp, 0u) << "nothing in this test may move a gen stamp";

		u64 got_id = 0;
		GSTexture* tex = cache.Lookup(*mem, model, w.levels_tex0[0], w.texa, w.pages, w.levels_tex0.data(),
			w.levels, nullptr, &got_id);

		u64 want_id = 0;
		const GSTileTextureSource::BuiltOutcome want = ref.LookupFrozen(w.key, stamp, &want_id);

		ASSERT_NE(tex, nullptr);
		ASSERT_EQ(got_id, want_id) << "build id";
		if (want == GSTileTextureSource::BuiltOutcome::Hit)
			ASSERT_EQ(tex, served[w.key]) << "served a texture that is not this window's";
		else
			served[w.key] = tex;

		ASSERT_EQ(cache.Hits(), ref.hits) << "hits";
		ASSERT_EQ(cache.Builds(), ref.builds) << "builds";
		ASSERT_EQ(cache.CapacityRefusals(), ref.cap_refusals) << "capacity refusals";
	}

	EXPECT_GT(cache.Hits(), 0u);
	EXPECT_GT(ref.evictions, 100u) << "the cache never filled, so the LRU was never asked anything";
}
