// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The burst memo's parts that need no renderer: the masked packet compare and the store (history,
// budget, eviction). The memo on the copy road itself is in gs_burst_memo_draw_tests.cpp.

#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace GSBurstMemo;

namespace
{
	// The masks read GSLocalMemory::m_psm, which the first GSLocalMemory fills.
	void EnsureFormatTables()
	{
		static const std::unique_ptr<GSLocalMemory> mem = std::make_unique<GSLocalMemory>();
	}

	// A copy-road-shaped draw: a textured sprite into a narrow 32-bit frame, no blending, no fog, no depth
	// test, no dither, UV coordinates. Each test changes what it needs.
	struct DrawState
	{
		GSDrawingEnvironment env;
		std::vector<GSVertex> v;
		std::vector<u16> idx;
		u64 flags = 0;

		DrawState()
		{
			EnsureFormatTables();
			std::memset(static_cast<void*>(&env), 0, sizeof(env));
			GSDrawingContext& c = env.CTXT[0];
			c.FRAME.FBP = 0x100;
			c.FRAME.FBW = 1;
			c.FRAME.PSM = PSMCT32;
			c.TEX0.TBP0 = 0x2000;
			c.TEX0.TBW = 1;
			c.TEX0.PSM = PSMCT32;
			c.TEX0.TW = 6;
			c.TEX0.TH = 6;
			c.TEX0.TFX = TFX_DECAL;
			c.SCISSOR.SCAX1 = 63;
			c.SCISSOR.SCAY1 = 63;
			c.TEST.ZTST = ZTST_ALWAYS;
			env.PRIM.PRIM = GS_SPRITE;
			env.PRIM.TME = 1;
			env.PRIM.FST = 1;
			env.COLCLAMP.CLAMP = 1;

			v.resize(2);
			std::memset(static_cast<void*>(v.data()), 0, v.size() * sizeof(GSVertex));
			v[0].XYZ.X = 0;
			v[0].XYZ.Y = 0;
			v[0].U = 0;
			v[0].V = 0;
			v[1].XYZ.X = 16 * 16;
			v[1].XYZ.Y = 16 * 16;
			v[1].U = 16 * 16;
			v[1].V = 16 * 16;
			for (GSVertex& x : v)
			{
				x.RGBAQ.R = 0x80;
				x.RGBAQ.G = 0x80;
				x.RGBAQ.B = 0x80;
				x.RGBAQ.A = 0x80;
				x.RGBAQ.Q = 1.0f;
			}
			idx = {0, 1};
		}

		GSDrawingContext& ctx() { return env.CTXT[env.PRIM.CTXT]; }

		Packet Take()
		{
			Packet p;
			p.Take(ctx(), env, flags, v.data(), static_cast<u32>(v.size()), idx.data(), static_cast<u32>(idx.size()));
			return p;
		}

		bool Matches(const Packet& p)
		{
			return p.Matches(ctx(), env, flags, v.data(), static_cast<u32>(v.size()), idx.data(), static_cast<u32>(idx.size()));
		}

		u64 Signature()
		{
			return Packet::Signature(ctx(), env, v.data(), static_cast<u32>(v.size()), static_cast<u32>(idx.size()));
		}
	};

	// Records the base draw, applies `change` and says whether the changed draw still matches the record.
	bool MatchesAfter(const std::function<void(DrawState&)>& setup, const std::function<void(DrawState&)>& change)
	{
		DrawState d;
		setup(d);
		const Packet p = d.Take();
		change(d);
		return d.Matches(p);
	}

	void Plain(DrawState&) {}
} // namespace

TEST(GSBurstMemoPacket, ADrawMatchesItsOwnRecord)
{
	DrawState d;
	const Packet p = d.Take();
	EXPECT_TRUE(d.Matches(p));
	EXPECT_EQ(p.Signature(), d.Signature());
}

// Words the draw's own state says it does not read: changing them does not stop a match.
TEST(GSBurstMemoPacket, WordsTheDrawDoesNotReadAreMasked)
{
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().ALPHA.A = 2; })) << "ALPHA without blending";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.env.PABE.PABE = 1; })) << "PABE without blending";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().ZBUF.ZBP = 0x55; })) << "ZBUF without the depth test";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.env.FOGCOL.FCR = 9; })) << "FOGCOL without fog";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.env.DIMX.DM00 = 3; })) << "DIMX without dither";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEX0.CBP = 0x3000; })) << "palette fields of a texture without one";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEX0.CLD = 1; })) << "CLD of a texture without a palette";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.env.TEXCLUT.CBW = 4; })) << "TEXCLUT without a palette";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.env.TEXA.TA0 = 0x40; })) << "TEXA for a 32-bit texture";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.v[1].ST.S = 0.5f; })) << "ST with UV coordinates";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.v[1].RGBAQ.Q = 2.0f; })) << "Q with UV coordinates";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.v[0].XYZ.Z = 7; })) << "vertex Z without the depth test";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) { d.v[0].FOG = 0x11; })) << "vertex fog without fog";
	EXPECT_TRUE(MatchesAfter(Plain, [](DrawState& d) {
		d.env.CTXT[1] = d.env.CTXT[0];
		d.env.PRIM.CTXT = 1;
	})) << "the same registers in the other context";
}

// Words the draw reads: changing any one stops a match.
TEST(GSBurstMemoPacket, WordsTheDrawReadsAreCompared)
{
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().FRAME.FBP = 0x101; })) << "FRAME";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().FRAME.FBMSK = 0xFF000000; })) << "FBMSK";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEX0.TBP0 = 0x2020; })) << "TEX0 base";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEX0.TFX = TFX_MODULATE; })) << "TEX0 function";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEX1.MMIN = 1; })) << "TEX1";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().CLAMP.WMS = CLAMP_REPEAT + 1; })) << "CLAMP";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().SCISSOR.SCAX1 = 31; })) << "SCISSOR";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().TEST.ATE = 1; })) << "TEST";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().FBA.FBA = 1; })) << "FBA";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.ctx().XYOFFSET.OFX = 16; })) << "XYOFFSET";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.PRIM.AA1 = 1; })) << "PRIM.AA1";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.PRIM.ABE = 1; })) << "PRIM.ABE";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.DTHE.DTHE = 1; })) << "DTHE";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.COLCLAMP.CLAMP = 0; })) << "COLCLAMP";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.SCANMSK.MSK = 2; })) << "SCANMSK";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.env.PRMODECONT.AC = 1; })) << "PRMODECONT";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.flags = 1; })) << "the record's flags";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.v[1].XYZ.X += 16; })) << "vertex X";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.v[1].RGBAQ.A = 0x7F; })) << "vertex alpha";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.v[1].U += 16; })) << "vertex U";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) { d.idx = {1, 0}; })) << "index order";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) {
		d.v.push_back(d.v[1]);
		d.idx = {0, 1};
	})) << "vertex count";
	EXPECT_FALSE(MatchesAfter(Plain, [](DrawState& d) {
		d.v.push_back(d.v[0]);
		d.v.push_back(d.v[1]);
		d.idx = {0, 1, 2, 3};
	})) << "index count";
}

// The masks follow the recorded draw's own state.
TEST(GSBurstMemoPacket, MasksFollowTheDrawsState)
{
	const auto blend = [](DrawState& d) { d.env.PRIM.ABE = 1; };
	EXPECT_FALSE(MatchesAfter(blend, [](DrawState& d) { d.ctx().ALPHA.A = 2; })) << "ALPHA with blending";
	EXPECT_FALSE(MatchesAfter(blend, [](DrawState& d) { d.env.PABE.PABE = 1; })) << "PABE with blending";

	const auto depth = [](DrawState& d) { d.ctx().TEST.ZTE = 1; };
	EXPECT_FALSE(MatchesAfter(depth, [](DrawState& d) { d.ctx().ZBUF.ZBP = 0x55; })) << "ZBUF with the depth test";
	EXPECT_FALSE(MatchesAfter(depth, [](DrawState& d) { d.v[0].XYZ.Z = 7; })) << "vertex Z with the depth test";

	const auto stq = [](DrawState& d) { d.env.PRIM.FST = 0; };
	EXPECT_FALSE(MatchesAfter(stq, [](DrawState& d) { d.v[1].ST.S = 0.5f; })) << "ST with STQ coordinates";
	EXPECT_FALSE(MatchesAfter(stq, [](DrawState& d) { d.v[1].RGBAQ.Q = 2.0f; })) << "Q with STQ coordinates";
	EXPECT_TRUE(MatchesAfter(stq, [](DrawState& d) { d.v[1].U += 16; })) << "UV with STQ coordinates";

	const auto untextured = [](DrawState& d) { d.env.PRIM.TME = 0; };
	EXPECT_TRUE(MatchesAfter(untextured, [](DrawState& d) { d.ctx().TEX0.TBP0 = 0x2020; })) << "TEX0 without a texture";
	EXPECT_TRUE(MatchesAfter(untextured, [](DrawState& d) { d.ctx().CLAMP.WMS = 1; })) << "CLAMP without a texture";
	EXPECT_TRUE(MatchesAfter(untextured, [](DrawState& d) { d.v[1].U += 16; })) << "UV without a texture";

	const auto paletted = [](DrawState& d) { d.ctx().TEX0.PSM = PSMT8; };
	EXPECT_FALSE(MatchesAfter(paletted, [](DrawState& d) { d.ctx().TEX0.CBP = 0x3000; })) << "CBP of a paletted texture";
	EXPECT_FALSE(MatchesAfter(paletted, [](DrawState& d) { d.ctx().TEX0.CSA = 1; })) << "CSA of a paletted texture";
	EXPECT_TRUE(MatchesAfter(paletted, [](DrawState& d) { d.env.TEXCLUT.CBW = 4; })) << "TEXCLUT with CSM1";

	const auto csm2 = [](DrawState& d) {
		d.ctx().TEX0.PSM = PSMT8;
		d.ctx().TEX0.CSM = 1;
	};
	EXPECT_FALSE(MatchesAfter(csm2, [](DrawState& d) { d.env.TEXCLUT.CBW = 4; })) << "TEXCLUT with CSM2";

	const auto texa16 = [](DrawState& d) { d.ctx().TEX0.PSM = PSMCT16; };
	EXPECT_FALSE(MatchesAfter(texa16, [](DrawState& d) { d.env.TEXA.TA0 = 0x40; })) << "TEXA for a 16-bit texture";
	const auto texa24 = [](DrawState& d) { d.ctx().TEX0.PSM = PSMCT24; };
	EXPECT_FALSE(MatchesAfter(texa24, [](DrawState& d) { d.env.TEXA.AEM = 1; })) << "TEXA for a 24-bit texture";
	const auto pal16 = [](DrawState& d) {
		d.ctx().TEX0.PSM = PSMT4;
		d.ctx().TEX0.CPSM = PSMCT16;
	};
	EXPECT_FALSE(MatchesAfter(pal16, [](DrawState& d) { d.env.TEXA.TA1 = 0x40; })) << "TEXA for a 16-bit palette";

	const auto fog = [](DrawState& d) { d.env.PRIM.FGE = 1; };
	EXPECT_FALSE(MatchesAfter(fog, [](DrawState& d) { d.env.FOGCOL.FCR = 9; })) << "FOGCOL with fog";
	EXPECT_FALSE(MatchesAfter(fog, [](DrawState& d) { d.v[0].FOG = 0x11; })) << "vertex fog with fog";

	const auto dither = [](DrawState& d) { d.env.DTHE.DTHE = 1; };
	EXPECT_FALSE(MatchesAfter(dither, [](DrawState& d) { d.env.DIMX.DM00 = 3; })) << "DIMX with dither";
}

TEST(GSBurstMemoPacket, MatchingDrawsShareASignature)
{
	DrawState d;
	const Packet p = d.Take();
	d.ctx().ALPHA.A = 2;
	d.v[1].ST.S = 0.25f;
	ASSERT_TRUE(d.Matches(p));
	EXPECT_EQ(d.Signature(), p.Signature());

	DrawState other;
	other.ctx().FRAME.FBP = 0x140;
	EXPECT_NE(other.Signature(), p.Signature());
}

namespace
{
	EntryPtr MakeEntry(u32 fbp, u32 draws, size_t out_blocks_per_draw = 1)
	{
		DrawState d;
		d.ctx().FRAME.FBP = fbp;
		auto e = std::make_shared<Entry>();
		for (u32 i = 0; i < draws; i++)
		{
			Draw dr;
			dr.packet = d.Take();
			for (size_t b = 0; b < out_blocks_per_draw; b++)
				dr.out_blocks.push_back(static_cast<u32>(b));
			dr.out_bytes.assign(out_blocks_per_draw * 256, 0xAB);
			e->Append(dr);
			d.v[0].XYZ.X += 16;
			d.v[1].XYZ.X += 16;
		}
		e->Recount();
		return e;
	}

	u64 FirstSignature(const Entry& e) { return e.first_signature; }

	bool Holds(const Store& s, const EntryPtr& e)
	{
		const u64 sig = FirstSignature(*e);
		if (!s.MayHave(sig))
			return false;
		const std::vector<EntryPtr>* list = s.Find(sig);
		if (!list)
			return false;
		for (const EntryPtr& x : *list)
			if (x == e)
				return true;
		return false;
	}
} // namespace

// A draw laid out in an entry reads back as it was recorded, and its record matches exactly the draws its packet
// matches.
TEST(GSBurstMemoEntry, ARecordReadsBackAndMatchesAsItsPacket)
{
	auto e = std::make_shared<Entry>();
	std::vector<DrawState> states(3);
	states[1].ctx().TEST.ZTE = 1; // vertex Z compared
	states[2].env.PRIM.FST = 0; // ST and Q compared, palette fields masked away from TEX0
	for (u32 i = 0; i < states.size(); i++)
	{
		Draw dr;
		dr.packet = states[i].Take();
		dr.src = {{0x1000 + i, 0x1111 * (i + 1)}, {0x1001 + i, 0x2222}};
		dr.pre = {{0x800 + i, 0x3333}};
		dr.out_blocks = {0x800 + i, 0x801 + i};
		dr.out_bytes.resize(2 * 256);
		for (size_t b = 0; b < dr.out_bytes.size(); b++)
			dr.out_bytes[b] = static_cast<u8>(b * 7 + i);
		dr.palette_entries = i ? 256 : 0;
		dr.palette_hash = 0xABCDu + i;
		dr.frame_target_checked = i == 1;
		dr.copied = i != 1;
		dr.bbox = GSVector4i(1, 2, 3 + i, 4);
		dr.left.fbmsk = 0xFF000000u + i;
		dr.left.r = GSVector4i(5, 6, 7, 8 + i);
		e->Append(dr);

		const Record r = e->At(i);
		EXPECT_EQ(r.h->nsrc, 2u);
		EXPECT_EQ(r.src_blocks[1], 0x1001 + i);
		EXPECT_EQ(r.src_hashes[0], 0x1111u * (i + 1));
		EXPECT_EQ(r.pre_blocks[0], 0x800 + i);
		EXPECT_EQ(r.pre_hashes[0], 0x3333u);
		EXPECT_EQ(r.h->nout, 2u);
		EXPECT_EQ(r.out_blocks[1], 0x801 + i);
		EXPECT_EQ(std::memcmp(r.out_bytes, dr.out_bytes.data(), dr.out_bytes.size()), 0);
		EXPECT_EQ(r.h->palette_entries, dr.palette_entries);
		EXPECT_EQ(r.h->palette_hash, dr.palette_hash);
		EXPECT_EQ((r.h->flags & REC_FRAME_TARGET_CHECKED) != 0, dr.frame_target_checked);
		EXPECT_EQ((r.h->flags & REC_COPIED) != 0, dr.copied);
		EXPECT_TRUE(r.h->bbox.eq(dr.bbox));
		EXPECT_EQ(r.h->left.fbmsk, dr.left.fbmsk);
		EXPECT_TRUE(r.h->left.r.eq(dr.left.r));
	}
	EXPECT_EQ(e->Count(), 3u);
	EXPECT_EQ(e->first_signature, states[0].Take().Signature());

	// The record and the packet give the same answer, for the draw itself and for changes in and out of its mask.
	const std::vector<std::function<void(DrawState&)>> changes = {
		[](DrawState&) {},
		[](DrawState& d) { d.ctx().ALPHA.A = 2; },
		[](DrawState& d) { d.v[0].XYZ.Z = 9; },
		[](DrawState& d) { d.v[1].ST.S = 0.75f; },
		[](DrawState& d) { d.v[1].U += 16; },
		[](DrawState& d) { d.ctx().TEX0.CBP = 0x3000; },
		[](DrawState& d) { d.ctx().FRAME.FBP = 0x101; },
		[](DrawState& d) {
			d.env.CTXT[1] = d.env.CTXT[0];
			d.env.PRIM.CTXT = 1;
		},
	};
	for (u32 i = 0; i < states.size(); i++)
	{
		const Packet p = states[i].Take();
		const Record r = e->At(i);
		for (size_t c = 0; c < changes.size(); c++)
		{
			DrawState d = states[i];
			changes[c](d);
			const bool by_packet = d.Matches(p);
			const bool by_record = r.Matches(d.ctx(), d.env, d.flags, d.v.data(), static_cast<u32>(d.v.size()), d.idx.data(),
				static_cast<u32>(d.idx.size()));
			EXPECT_EQ(by_record, by_packet) << "record " << i << ", change " << c;
		}
	}
}

TEST(GSBurstMemoStore, FindsWhatItHoldsByTheFirstDrawsSignature)
{
	Store s;
	const EntryPtr a = MakeEntry(0x100, 3);
	const EntryPtr b = MakeEntry(0x140, 2);
	s.Insert(a);
	s.Insert(b);
	EXPECT_TRUE(Holds(s, a));
	EXPECT_TRUE(Holds(s, b));
	EXPECT_EQ(s.Entries(), 2u);
	EXPECT_EQ(s.Bytes(), a->bytes + b->bytes);
	EXPECT_TRUE(a->stored);

	const EntryPtr absent = MakeEntry(0x180, 1);
	EXPECT_FALSE(Holds(s, absent));

	s.Clear();
	EXPECT_FALSE(Holds(s, a));
	EXPECT_EQ(s.Entries(), 0u);
	EXPECT_EQ(s.Bytes(), 0u);
	EXPECT_FALSE(a->stored);
}

// An entry stays HISTORY drawn frames after its install or its last hit, and goes after that.
TEST(GSBurstMemoStore, EntriesLeaveAfterTheHistoryUnlessUsed)
{
	Store s;
	const EntryPtr used = MakeEntry(0x100, 2);
	const EntryPtr idle = MakeEntry(0x140, 2);
	s.Insert(used);
	s.Insert(idle);
	for (u64 f = 0; f < Store::HISTORY; f++)
	{
		s.EndFrame();
		s.Touch(*used);
		EXPECT_TRUE(Holds(s, idle)) << "after " << (f + 1) << " frames";
	}
	s.EndFrame();
	EXPECT_FALSE(Holds(s, idle));
	EXPECT_FALSE(idle->stored);
	EXPECT_TRUE(Holds(s, used));
	EXPECT_EQ(s.Evictions(), 1u);
	EXPECT_EQ(s.Bytes(), used->bytes);
}

// Over budget, the least recently used entries go first, until the store fits.
TEST(GSBurstMemoStore, TheBudgetEvictsTheLeastRecentlyUsed)
{
	const EntryPtr probe = MakeEntry(0x100, 4, 4);
	Store s;
	s.SetBudget(probe->bytes * 3);

	const EntryPtr a = MakeEntry(0x100, 4, 4);
	const EntryPtr b = MakeEntry(0x140, 4, 4);
	const EntryPtr c = MakeEntry(0x180, 4, 4);
	s.Insert(a);
	s.EndFrame();
	s.Insert(b);
	s.EndFrame();
	s.Insert(c);
	s.Touch(*a); // a is now the most recently used
	EXPECT_EQ(s.Entries(), 3u);

	const EntryPtr d = MakeEntry(0x1C0, 4, 4);
	s.Insert(d);
	EXPECT_FALSE(Holds(s, b)) << "b was the least recently used";
	EXPECT_TRUE(Holds(s, a));
	EXPECT_TRUE(Holds(s, c));
	EXPECT_TRUE(Holds(s, d));
	EXPECT_LE(s.Bytes(), s.Budget());
	EXPECT_EQ(s.BudgetEvictions(), 1u);
	EXPECT_GE(s.PeakBytes(), s.Bytes());
}

// A burst larger than the whole budget is not kept.
TEST(GSBurstMemoStore, ABurstLargerThanTheBudgetIsNotKept)
{
	Store s;
	const EntryPtr big = MakeEntry(0x100, 8, 8);
	s.SetBudget(big->bytes - 1);
	s.Insert(big);
	EXPECT_FALSE(Holds(s, big));
	EXPECT_EQ(s.Bytes(), 0u);
}

// A burst whose records run past one slab reads back record for record, a record never straddling two slabs.
TEST(GSBurstMemoEntry, RecordsPastOneSlabReadBack)
{
	const auto pool = std::make_shared<SlabPool>();
	Entry e(pool);
	// About 5 KiB a record: well over SLAB_BYTES in all.
	const u32 draws = static_cast<u32>(3 * SlabPool::SLAB_BYTES / (5u << 10));
	for (u32 i = 0; i < draws; i++)
	{
		Draw dr;
		DrawState d;
		d.v[0].XYZ.X = static_cast<u16>(16 * i);
		dr.packet = d.Take();
		dr.out_blocks.resize(20);
		dr.out_bytes.resize(20 * 256);
		for (size_t b = 0; b < dr.out_blocks.size(); b++)
			dr.out_blocks[b] = static_cast<u32>(i * 20 + b);
		for (size_t b = 0; b < dr.out_bytes.size(); b++)
			dr.out_bytes[b] = static_cast<u8>(b * 13 + i);
		e.Append(dr);
	}
	ASSERT_EQ(e.Count(), draws);
	EXPECT_GE(e.slabs.size(), 3u);
	for (u32 i = 0; i < draws; i++)
	{
		const Record r = e.At(i);
		ASSERT_EQ(r.h->nout, 20u) << "record " << i;
		EXPECT_EQ(r.out_blocks[19], i * 20 + 19) << "record " << i;
		EXPECT_EQ(r.out_bytes[20 * 256 - 1], static_cast<u8>((20 * 256 - 1) * 13 + i)) << "record " << i;
		const u8* start = static_cast<const u8*>(e.Address(i));
		const bool inside = std::any_of(e.slabs.begin(), e.slabs.end(), [&](const SlabPool::Slab* s) {
			return start >= s->data && start + r.h->bytes <= s->data + s->size;
		});
		EXPECT_TRUE(inside) << "record " << i << " straddles a slab";
	}
}

// Once the store has evicted bursts, new bursts record into the slabs those left: no memory from the allocator.
TEST(GSBurstMemoStore, RecordingAfterEvictionsReusesSlabs)
{
	Store s;
	const auto record = [&](u32 fbp) {
		auto e = std::make_shared<Entry>(s.Pool());
		DrawState d;
		d.ctx().FRAME.FBP = fbp;
		for (u32 i = 0; i < 40; i++)
		{
			Draw dr;
			dr.packet = d.Take();
			dr.out_blocks.resize(8);
			dr.out_bytes.assign(8 * 256, 0x5A);
			e->Append(dr);
			d.v[0].XYZ.X += 16;
		}
		e->Recount();
		s.Insert(e);
	};
	for (u32 i = 0; i < 8; i++)
		record(0x100 + i);
	for (u64 f = 0; f <= Store::HISTORY; f++)
		s.EndFrame();
	ASSERT_EQ(s.Entries(), 0u);
	const u64 allocations = s.Pool()->Allocations();
	EXPECT_GT(allocations, 0u);
	for (u32 i = 0; i < 8; i++)
		record(0x200 + i);
	EXPECT_EQ(s.Pool()->Allocations(), allocations);
}
