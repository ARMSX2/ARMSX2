// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the rows and the state table (GS/Renderers/HW/GSDrawRow.h): the record layouts, the two-level table's
// lookups, growth and flush, the sequence-number window, and the half-pixel mode-5 push written into positions
// against the vertex offset it replaces.

#include "GS/Renderers/HW/GSDrawRow.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

using namespace GSDrawRows;

namespace
{
	RegKey RandomReg(std::mt19937_64& rng)
	{
		RegKey k = {};
		for (u32 i = 0; i < RegKey::WORDS; i++)
			k.w[i] = rng() & ((i & 1) ? 0xFFull : ~0ull); // some words take few values, as register fields do
		return k;
	}

	VarKey RandomVar(std::mt19937_64& rng)
	{
		VarKey v = {};
		for (u32 i = 0; i < VarKey::WORDS; i++)
			v.w[i] = rng() & ((i & 1) ? 0xFFFull : ~0ull);
		return v;
	}
} // namespace

TEST(GSDrawRow, RecordLayouts)
{
	EXPECT_EQ(sizeof(GSDrawRow), 32u);
	EXPECT_EQ(sizeof(GSDrawFacts), 64u);
	EXPECT_EQ(offsetof(GSDrawRow, state), 0u);
	EXPECT_EQ(offsetof(GSDrawRow, tex), 2u);
	EXPECT_EQ(offsetof(GSDrawRow, flags), 3u);
	EXPECT_EQ(offsetof(GSDrawRow, a_min), 4u);
	EXPECT_EQ(offsetof(GSDrawRow, lod), 6u);
	EXPECT_EQ(offsetof(GSDrawRow, box), 8u);
	EXPECT_EQ(offsetof(GSDrawRow, ifirst), 20u);
	EXPECT_EQ(offsetof(GSDrawRow, nidx), 24u);
	EXPECT_EQ(offsetof(GSDrawFacts, memo), 56u);
	EXPECT_EQ(sizeof(StateHead), 96u);
	EXPECT_EQ(sizeof(RegState), 96u);
	EXPECT_EQ(CONF_BYTES % 16, 0u);
	EXPECT_EQ(CB_BYTES % 16, 0u);
	std::printf("state entry: configuration %u bytes, constant buffers %u bytes, body %zu bytes, head %zu bytes\n",
		CONF_BYTES, CB_BYTES, sizeof(StateBody), sizeof(StateHead));
}

TEST(GSDrawRow, SequenceWindowWrapsAt24Bits)
{
	EXPECT_TRUE(SeqInRun(100, 100));
	EXPECT_TRUE(SeqInRun(100, 100 + MAX_RUN_ROWS - 1));
	EXPECT_FALSE(SeqInRun(100, 100 + MAX_RUN_ROWS));
	// A run that starts just before the wrap continues across it.
	const u32 first = SEQ_WRAP - 10;
	EXPECT_TRUE(SeqInRun(first, (first + 20) & (SEQ_WRAP - 1)));
	EXPECT_FALSE(SeqInRun(first, (first + MAX_RUN_ROWS + 5) & (SEQ_WRAP - 1)));
	// A sequence number before the run's first is far outside it.
	EXPECT_FALSE(SeqInRun(100, 99));
}

TEST(GSStateTable, RegisterStatesFindOnlyTheirOwnKey)
{
	StateTable table;
	std::mt19937_64 rng(7);
	std::vector<RegKey> keys;
	for (u32 i = 0; i < 3000; i++)
	{
		const RegKey k = RandomReg(rng);
		if (table.FindReg(k) != StateTable::NONE)
			continue; // a repeat
		ASSERT_EQ(table.InsertReg(k), keys.size());
		keys.push_back(k);
	}
	EXPECT_EQ(table.RegCount(), keys.size());
	for (u32 i = 0; i < keys.size(); i++)
	{
		const u16 r = table.FindReg(keys[i]);
		ASSERT_EQ(r, i);
		EXPECT_TRUE(table.Reg(r).key == keys[i]);
	}
	// A key that differs in one bit of any word is another key.
	for (u32 word = 0; word < RegKey::WORDS; word++)
	{
		RegKey k = keys[word];
		k.w[word] ^= 1ull << (word * 7 % 64);
		const u16 r = table.FindReg(k);
		EXPECT_TRUE(r == StateTable::NONE || table.Reg(r).key == k);
	}
}

TEST(GSStateTable, LastAndOneBeforeAreFoundWithoutTheIndex)
{
	// Two register states drawn alternately, the common pattern, keep answering from the two-entry memory.
	StateTable table;
	std::mt19937_64 rng(5);
	const RegKey a = RandomReg(rng), b = RandomReg(rng), c = RandomReg(rng);
	const u16 ia = table.InsertReg(a);
	const u16 ib = table.InsertReg(b);
	const u16 ic = table.InsertReg(c);
	for (u32 i = 0; i < 10; i++)
	{
		EXPECT_EQ(table.FindReg(a), ia);
		EXPECT_EQ(table.FindReg(b), ib);
	}
	EXPECT_EQ(table.FindReg(c), ic);
	EXPECT_EQ(table.FindReg(a), ia);
	EXPECT_EQ(table.FindReg(c), ic);
}

TEST(GSStateTable, VariantsAreFoundByTheirFacts)
{
	StateTable table;
	std::mt19937_64 rng(9);
	const u16 r = table.InsertReg(RandomReg(rng));
	std::vector<VarKey> vars;
	std::vector<u16> ids;
	for (u32 i = 0; i < 20; i++)
	{
		vars.push_back(RandomVar(rng));
		ids.push_back(table.InsertVariant(r, vars.back()));
		table.Body(ids.back()).test = i;
	}
	// In any order, each finds its own, and the last hit is tried first.
	for (u32 pass = 0; pass < 2; pass++)
	{
		for (u32 i = 0; i < vars.size(); i++)
		{
			const u32 j = pass ? (vars.size() - 1 - i) : i;
			const u16 e = table.FindVariant(r, vars[j]);
			ASSERT_EQ(e, ids[j]);
			EXPECT_EQ(table.Body(e).test, j);
			EXPECT_EQ(table.Reg(r).last_hit, e);
			EXPECT_EQ(table.Head(e).reg, r);
		}
	}
	// A variant found in the list moves to its front.
	EXPECT_EQ(table.FindVariant(r, vars[5]), ids[5]);
	EXPECT_EQ(table.Reg(r).first, ids[5]);
	// One differing bit in any word is a miss.
	for (u32 word = 0; word < VarKey::WORDS; word++)
	{
		VarKey v = vars[3];
		v.w[word] ^= 1ull << 40;
		EXPECT_EQ(table.FindVariant(r, v), StateTable::NONE);
	}
	// Another register state does not see these variants.
	const u16 r2 = table.InsertReg(RandomReg(rng));
	EXPECT_EQ(table.FindVariant(r2, vars[0]), StateTable::NONE);
}

TEST(GSStateTable, GrowthKeepsIndicesValid)
{
	StateTable table;
	std::mt19937_64 rng(13);
	std::vector<RegKey> regs;
	std::vector<VarKey> vars;
	std::vector<u16> ids;
	for (u32 i = 0; i < StateTable::CHUNK * 3 + 17; i++)
	{
		regs.push_back(RandomReg(rng));
		const u16 r = table.InsertReg(regs.back());
		vars.push_back(RandomVar(rng));
		ids.push_back(table.InsertVariant(r, vars.back()));
		table.Body(ids.back()).test = i;
	}
	EXPECT_EQ(table.Chunks(), 4u);
	for (u32 i = 0; i < regs.size(); i++)
	{
		const u16 r = table.FindReg(regs[i]);
		ASSERT_NE(r, StateTable::NONE);
		const u16 e = table.FindVariant(r, vars[i]);
		ASSERT_EQ(e, ids[i]);
		EXPECT_EQ(table.Body(e).test, i);
	}
}

TEST(GSStateTable, FullTableRefusesAndFlushForgets)
{
	StateTable table;
	std::mt19937_64 rng(17);
	const u16 r = table.InsertReg(RandomReg(rng));
	for (u32 i = 0; i < StateTable::CAPACITY; i++)
		ASSERT_NE(table.InsertVariant(r, RandomVar(rng)), StateTable::NONE);
	EXPECT_EQ(table.InsertVariant(r, RandomVar(rng)), StateTable::NONE);
	EXPECT_EQ(table.Chunks(), StateTable::MAX_CHUNKS);

	const RegKey k = RandomReg(rng);
	const VarKey v = RandomVar(rng);
	table.Flush();
	EXPECT_EQ(table.RegCount(), 0u);
	EXPECT_EQ(table.EntryCount(), 0u);
	EXPECT_EQ(table.Flushes(), 1u);
	EXPECT_EQ(table.FindReg(k), StateTable::NONE);
	const u16 r2 = table.InsertReg(k);
	EXPECT_EQ(r2, 0u);
	EXPECT_EQ(table.FindVariant(r2, v), StateTable::NONE);
	const u16 e = table.InsertVariant(r2, v);
	EXPECT_EQ(e, 0u);
	EXPECT_EQ(table.FindVariant(table.FindReg(k), v), e);
}

// Half-pixel mode 5 above 1x. Master moves the vertex offset of a textured FST sprite by (16 - frac1) / 8 * scale
// times its unpushed value on an axis where the push applies; the rows add 16 - frac1 sixteenths to the coordinates
// instead and keep the unpushed offset. The vertex program computes (p - 0.05) * scale - offset. The two forms round
// differently: near the target's left edge, where the clip position is the difference of two numbers close to -1,
// they are several units in the last place apart. Both must light the same pixels, and differ by far less than the
// program's 0.05-sixteenth bias that keeps edges off pixel centres.
namespace
{
	struct Axis
	{
		float clip_master;
		float clip_pushed;
	};

	Axis ClipX(int x, int x0, int x1, int ofx, int width, float rtscale)
	{
		// DetermineVSConfig's NativeWTexOffset arm, one axis.
		const float sx = 2.0f / static_cast<float>(width << 4);
		const float ox = static_cast<float>(ofx);
		float ox2 = -1.0f / (static_cast<float>(width) * rtscale);
		const float ox2_unpushed = ox2;
		const int frac0 = (x0 - ofx) & 0xf;
		const int frac1 = (x1 - ofx) & 0xf;
		if (GSSpriteEdgeSnap::NativeSpritePushApplies(frac0, frac1, x0 <= x1))
			ox2 *= 1.0f + ((static_cast<float>(16 - frac1) / 8.0f) * rtscale);
		const float offset_master = ox * sx + ox2 + 1;
		const float offset_unpushed = ox * sx + ox2_unpushed + 1;

		const u32 push = NativeSpritePush(x0, x1, ofx);
		const float p_master = static_cast<float>(x);
		const float p_pushed = static_cast<float>(x + static_cast<int>(push));
		// One operation per statement, as the vertex program evaluates it.
		const float a_master = p_master - 0.05f;
		const float b_master = a_master * sx;
		const float a_pushed = p_pushed - 0.05f;
		const float b_pushed = a_pushed * sx;
		return {b_master - offset_master, b_pushed - offset_unpushed};
	}

	/// The first pixel column whose centre is at or past the edge, in device pixels.
	int FirstPixel(float clip, int width, float rtscale)
	{
		const float win = (clip + 1.0f) * 0.5f * (static_cast<float>(width) * rtscale);
		return static_cast<int>(std::ceil(win - 0.5f));
	}

	int UlpDistance(float a, float b)
	{
		s32 ia, ib;
		std::memcpy(&ia, &a, 4);
		std::memcpy(&ib, &b, 4);
		if (ia < 0)
			ia = static_cast<s32>(0x80000000u) - ia;
		if (ib < 0)
			ib = static_cast<s32>(0x80000000u) - ib;
		return std::abs(ia - ib);
	}
} // namespace

TEST(GSDrawRowHalfPixel, PushAmountFollowsTheSnapRule)
{
	for (int frac0 = 0; frac0 < 16; frac0++)
	{
		for (int frac1 = 0; frac1 < 16; frac1++)
		{
			for (int dir = 0; dir < 2; dir++)
			{
				const int ofx = 0x7000;
				const int x0 = ofx + 100 * 16 + frac0 + (dir ? 64 * 16 : 0);
				const int x1 = ofx + 132 * 16 + frac1;
				const bool applies = GSSpriteEdgeSnap::NativeSpritePushApplies(frac0, frac1, x0 <= x1);
				EXPECT_EQ(NativeSpritePush(x0, x1, ofx), applies ? static_cast<u32>(16 - frac1) : 0u);
				if (applies)
				{
					EXPECT_GE(NativeSpritePush(x0, x1, ofx), 1u);
					EXPECT_LE(NativeSpritePush(x0, x1, ofx), 16u);
				}
			}
		}
	}
}

TEST(GSDrawRowHalfPixel, PushedPositionsMatchThePushedOffsetOverAll256Pairs)
{
	static constexpr int widths[] = {256, 320, 512, 640};
	static constexpr float scales[] = {2.0f, 3.0f, 4.0f};
	u32 checked = 0, pushed = 0, off_by_one_ulp = 0;
	int worst_ulp = 0;
	double worst_ratio = 0.0;
	for (const int width : widths)
	{
		for (const float rtscale : scales)
		{
			const int ofx = (2048 - width / 2) * 16;
			for (int frac0 = 0; frac0 < 16; frac0++)
			{
				for (int frac1 = 0; frac1 < 16; frac1++)
				{
					for (int dir = 0; dir < 2; dir++)
					{
						// A sprite 1 to 200 native pixels wide anywhere in the target, both vertices of it.
						for (int px = 0; px < width - 1; px += 7)
						{
							const int len = 1 + (px * 13) % 200;
							const int lo = ofx + px * 16;
							const int x0 = dir ? lo + len * 16 + frac0 : lo + frac0;
							const int x1 = dir ? lo + frac1 : lo + len * 16 + frac1;
							for (const int x : {x0, x1})
							{
								const Axis a = ClipX(x, x0, x1, ofx, width, rtscale);
								const int ulp = UlpDistance(a.clip_master, a.clip_pushed);
								worst_ulp = std::max(worst_ulp, ulp);
								off_by_one_ulp += ulp == 1;
								// The difference in device pixels against the vertex program's own bias of 0.05
								// sixteenths of a native pixel: it must be a small fraction of it.
								const double err_px = std::fabs(static_cast<double>(a.clip_master) - static_cast<double>(a.clip_pushed)) *
								                      0.5 * width * rtscale;
								const double bias_px = 0.05 / 16.0 * rtscale;
								worst_ratio = std::max(worst_ratio, err_px / bias_px);
								ASSERT_LT(err_px, bias_px / 16.0) << "width " << width << " scale " << rtscale << " frac " << frac0
																  << "," << frac1 << " x " << x << " ulp " << ulp;
								ASSERT_EQ(FirstPixel(a.clip_master, width, rtscale), FirstPixel(a.clip_pushed, width, rtscale))
									<< "width " << width << " scale " << rtscale << " frac " << frac0 << "," << frac1 << " x " << x;
								checked++;
								pushed += NativeSpritePush(x0, x1, ofx) != 0;
							}
						}
					}
				}
			}
		}
	}
	EXPECT_GT(pushed, 0u);
	RecordProperty("checked", static_cast<int>(checked));
	RecordProperty("pushed", static_cast<int>(pushed));
	RecordProperty("off_by_one_ulp", static_cast<int>(off_by_one_ulp));
	RecordProperty("worst_ulp", worst_ulp);
	std::printf("half-pixel push: %u positions, %u pushed, %u one ulp apart, worst %d ulp, worst error %.5f of the bias\n",
		checked, pushed, off_by_one_ulp, worst_ulp, worst_ratio);
}

// The alpha bounds: the class answers every comparison with 0 and 128, and any other read is recorded.
TEST(GSDrawRowAlpha, TheClassAnswersEveryComparisonWithZeroAnd128)
{
	for (int a = 0; a <= 256; a++)
	{
		for (int b = 0; b <= 256; b++)
		{
			if (AlphaClass(a) != AlphaClass(b))
				continue;
			for (const int k : {0, 128})
			{
				ASSERT_EQ(a == k, b == k) << a << " " << b << " " << k;
				ASSERT_EQ(a < k, b < k) << a << " " << b << " " << k;
				ASSERT_EQ(a <= k, b <= k) << a << " " << b << " " << k;
				ASSERT_EQ(a > k, b > k) << a << " " << b << " " << k;
				ASSERT_EQ(a >= k, b >= k) << a << " " << b << " " << k;
			}
		}
	}
}

TEST(GSDrawRowAlpha, ReadsTheClassCannotAnswerAreRecorded)
{
	u32 asked = 0;
	const AlphaBound b(77, BLEND_MAX, &asked);
	EXPECT_TRUE(b > 0);
	EXPECT_TRUE(b < 128);
	EXPECT_FALSE(b == 128);
	EXPECT_TRUE(b != 0);
	EXPECT_EQ(asked, 0u);
	EXPECT_EQ(b.ForLog(), 77);
	EXPECT_EQ(asked, 0u);
	EXPECT_TRUE(b > 64);
	EXPECT_EQ(asked, 1u << BLEND_MAX);

	asked = 0;
	const AlphaRange r{{40, DRAW_MIN, &asked}, {40, DRAW_MAX, &asked}};
	EXPECT_TRUE(r.OneValue());
	EXPECT_EQ(asked, 0u);
	EXPECT_EQ(r.max.Value(), 40);
	EXPECT_EQ(asked, 1u << DRAW_MAX);
	EXPECT_EQ(AlphaValueCare(asked), 0x1FFull << 9);
}

TEST(GSStateTable, AVariantMatchesOnlyTheValuesItsDerivationRead)
{
	StateTable table;
	std::mt19937_64 rng(23);
	const u16 r = table.InsertReg(RandomReg(rng));
	VarKey v = RandomVar(rng);
	v.values = AlphaValues(64, 64, 10, 200, 128);
	// Derived without reading a value: any values of the same classes match.
	const u16 e = table.InsertVariant(r, v, 0);
	VarKey w = v;
	w.values = AlphaValues(65, 70, 11, 201, 128);
	EXPECT_EQ(table.FindVariant(r, w), e);
	// A variant that read the draw's maximum matches only that maximum.
	VarKey x = RandomVar(rng);
	x.values = AlphaValues(64, 64, 10, 200, 128);
	const u16 f = table.InsertVariant(r, x, AlphaValueCare(1u << DRAW_MAX));
	VarKey y = x;
	y.values = AlphaValues(1, 64, 99, 99, 0);
	EXPECT_EQ(table.FindVariant(r, y), f);
	y.values = AlphaValues(64, 65, 10, 200, 128);
	EXPECT_EQ(table.FindVariant(r, y), StateTable::NONE);
}
