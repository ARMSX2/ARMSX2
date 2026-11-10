// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The kick kernel's conversion pass (GSVertexKickKernel::ConvertJobs) against the
// pass it replaced, kept here as the oracle, on the same inputs.
//
// The pass it replaced tested every vertex for its place in the job and built each
// vertex with GSVertexKernels::ParsePackedRecord_Neon and FmmAccumVertex.
// ConvertJobs splits a job into stretches and has its own parse and fold. So the
// comparison is of everything the pass writes -- the vertex buffer, byte for byte,
// and the seven vectors of the bounds -- for every layout, both primitive
// classes, every depth-clamp mode, every texture and shading mode and both
// settings of keep_gif_q, over records chosen to sit where the two forms could
// part: a Q of +0, -0, either infinity, quiet and signalling NaNs of both signs
// and the denormals; S and T that make the divide produce a NaN or an infinity;
// UV words with the bits above fourteen set; acc_slot and colour_slot at every
// position against a job's slots; and the first and last record of a chunk
// against unreadable pages.

#include <gtest/gtest.h>

#include "GS/GS.h"
#include "GS/GSVertexKickKernel.h"

#ifdef ARCH_ARM64

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace
{
	using GSVertexKernels::FmmAcc;
	using GSVertexKernels::PackedLayout;
	using GSVertexKickKernel::ConvertArgs;
	using GSVertexKickKernel::ConvertJob;
	using GSVertexKickKernel::Invariants;

	// The oracle: the conversion pass as it stood before ConvertJobs was split
	// into stretches (ARMSX2 gs-runs 4810bf1ae9), verbatim but for its name.
	template <GSVertexKernels::PackedLayout layout, bool triangle, bool clamp>
	__forceinline_odr void OldConvertJobs(const ConvertArgs& args)
	{
		const Invariants& inv = *args.inv;
		const GIFPackedLayout off = inv.off;
		const u32 stride = GSVertexKernels::LayoutStride<layout>(off);
		const GIFPackedReg* RESTRICT rin = args.rin;
		GSVertex* RESTRICT vbuff = args.vbuff;
		GSVector4i carry = GSVector4i::zero();
		if constexpr (GSVertexKernels::LayoutCarriesM0(layout))
			carry = inv.carry_m0;
		const u64 uvfog = inv.uvfog;
		[[maybe_unused]] const GSVector4i clamp_keep = clamp ? inv.clamp_keep : GSVector4i::zero();
		[[maybe_unused]] const GSVector4i clamp_shifted = clamp ? inv.clamp_shifted : GSVector4i::zero();
		const GSVertexKernels::PackedParseConsts kc = GSVertexKernels::MakePackedParseConsts(inv.keep_gif_q);
#ifdef ARCH_ARM64
		[[maybe_unused]] const u32 shade = inv.shade;
		[[maybe_unused]] const bool tme = (shade & 1u) != 0;
		[[maybe_unused]] const bool fst = (shade & 2u) != 0;
		[[maybe_unused]] const bool iip = (shade & 4u) != 0;
		GSVertexKernels::FmmAcc a;
		if constexpr (triangle)
			a = *args.acc;
#endif

		GSVector4i m0, m1;
		for (const ConvertJob* RESTRICT q = args.jobs; q != args.jobs_end; q++)
		{
			const GIFPackedReg* RESTRICT rv = rin + q->from * stride;
			GSVertex* RESTRICT out = vbuff + q->slot;
			u32 left = q->to - q->from;
#ifdef ARCH_ARM64
			[[maybe_unused]] u32 s = q->slot;
			[[maybe_unused]] const u32 acc_slot = q->acc_slot;
			[[maybe_unused]] const u32 colour_slot = q->colour_slot;
#endif
			do
			{
				GSVertexKernels::ParsePackedRecord_Fast<layout>(rv, off, uvfog, carry, kc, m0, m1);
				if constexpr (clamp)
					m1 = (m1 & clamp_keep) | (m1.srl32<8>() & clamp_shifted);

				out->m[0] = m0;
				out->m[1] = m1;

#ifdef ARCH_ARM64
				if constexpr (triangle)
				{
					if (s >= acc_slot)
						GSVertexKernels::FmmAccumVertex(a, m0, m1, tme, fst, iip || s >= colour_slot);
					s++;
				}
#endif
				rv += stride;
				out++;
			} while (--left != 0);
		}

#ifdef ARCH_ARM64
		if constexpr (triangle)
			*args.acc = a;
#endif
	}

	using PassFn = void (*)(const ConvertArgs&);

	template <PackedLayout layout, bool triangle, bool clamp>
	__noinline void NewPass(const ConvertArgs& args)
	{
		GSVertexKickKernel::ConvertJobs<layout, triangle, clamp>(args);
	}

	template <PackedLayout layout, bool triangle, bool clamp>
	__noinline void OldPass(const ConvertArgs& args)
	{
		OldConvertJobs<layout, triangle, clamp>(args);
	}

#define LAYOUT_ROW(pass, layout) \
	{ \
		{&pass<layout, false, false>, &pass<layout, false, true>}, \
		{&pass<layout, true, false>, &pass<layout, true, true>}, \
	}
#define PASS_TABLE(pass) \
	{ \
		LAYOUT_ROW(pass, PackedLayout::TripleXYZF2), LAYOUT_ROW(pass, PackedLayout::TripleXYZ2), \
		LAYOUT_ROW(pass, PackedLayout::NopTripleXYZF2), LAYOUT_ROW(pass, PackedLayout::PairSTQXYZ2), \
		LAYOUT_ROW(pass, PackedLayout::PairUVXYZ2), LAYOUT_ROW(pass, PackedLayout::PairRGBAQXYZ2), \
		LAYOUT_ROW(pass, PackedLayout::UvTripleXYZF2), LAYOUT_ROW(pass, PackedLayout::PairUVXYZF2), \
	}

	// [layout][triangle][clamp]
	constexpr PassFn kNewPass[8][2][2] = PASS_TABLE(NewPass);
	constexpr PassFn kOldPass[8][2][2] = PASS_TABLE(OldPass);

	constexpr const char* kLayoutNames[8] = {"TripleXYZF2", "TripleXYZ2", "NopTripleXYZF2", "PairSTQXYZ2",
		"PairUVXYZ2", "PairRGBAQXYZ2", "UvTripleXYZF2", "PairUVXYZF2"};

	// std::mt19937 with a 32-bit result, which is what it produces.
	struct Rng
	{
		std::mt19937 gen;
		explicit Rng(u32 seed)
			: gen(seed)
		{
		}
		u32 operator()() { return static_cast<u32>(gen()); }
	};

	constexpr u32 kChunk = GSVertexKickKernel::kChunkVertices;
	constexpr u32 kSlots = 320;

	// Float bit patterns a Q, an S or a T can hold that the fix-ups, the divide
	// and the masked min and max each treat as a case of their own.
	constexpr u32 kSpecialFloats[] = {
		0x00000000u, 0x80000000u, // +0, -0
		0x7F800000u, 0xFF800000u, // +inf, -inf
		0x7FC00000u, 0xFFC00000u, // quiet NaN
		0x7F800001u, 0xFF800001u, // signalling NaN
		0x7FFFFFFFu, 0xFFFFFFFFu, // NaN, all mantissa bits
		0x00000001u, 0x007FFFFFu, // denormals
		0x00800000u, 0x7F7FFFFFu, // FLT_MIN, FLT_MAX
		0xFF7FFFFFu, 0x3F800000u, // -FLT_MAX, 1.0
	};

	// The chunk's records, with an unreadable page on each side: the first record
	// starts on the page after one, or the last ends on the page before the other.
	class RecordPages
	{
	public:
		RecordPages()
		{
			m_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
			m_data = ((kChunk * 8 * sizeof(GIFPackedReg)) + m_page - 1) / m_page * m_page;
			m_map = static_cast<u8*>(mmap(nullptr, m_data + 2 * m_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
			if (m_map == MAP_FAILED)
			{
				m_map = nullptr;
				return;
			}
			mprotect(m_map, m_page, PROT_NONE);
			mprotect(m_map + m_page + m_data, m_page, PROT_NONE);
		}
		~RecordPages()
		{
			if (m_map)
				munmap(m_map, m_data + 2 * m_page);
		}
		bool ok() const { return m_map != nullptr; }
		// `bytes` of records, against the page before them or the page after.
		GIFPackedReg* Place(size_t bytes, bool at_end) const
		{
			u8* const first = m_map + m_page;
			return reinterpret_cast<GIFPackedReg*>(at_end ? (first + m_data - bytes) : first);
		}

	private:
		size_t m_page = 0;
		size_t m_data = 0;
		u8* m_map = nullptr;
	};

	struct PassSetup
	{
		u32 layout;
		bool triangle;
		GSLimit24BitDepth clamp; // Disabled takes the pass's no-clamp arm
		u32 shade;               // bit 0 TME, bit 1 FST, bit 2 IIP
		bool keep_gif_q;
	};

	Invariants MakeInvariants(const PassSetup& su, Rng& rng)
	{
		Invariants inv = {};
		inv.shade = su.shade;
		inv.keep_gif_q = su.keep_gif_q;
		inv.clamp_enabled = (su.clamp != GSLimit24BitDepth::Disabled);
		GSVertexKickKernel::MakeDepthClampMasks(su.clamp, inv.clamp_keep, inv.clamp_shifted);
		inv.uvfog = (static_cast<u64>(rng()) << 32) | rng();
		for (u32 i = 0; i < 4; i++)
			inv.carry_m0.U32[i] = (rng() & 3) ? rng() : kSpecialFloats[rng() % std::size(kSpecialFloats)];

		// Where the layout's descriptors sit. The contiguous triples ignore this;
		// every other layout gets a stride of up to six with its descriptors at
		// distinct places in it, in any order.
		const PackedLayout l = static_cast<PackedLayout>(su.layout);
		if (GSVertexKernels::LayoutIsContiguousTriple(l))
		{
			inv.off = {3, 0, 1, 2};
		}
		else
		{
			const u32 used = GSVertexKernels::LayoutHasSeparateRgba(l) ? 3 : 2;
			const u32 stride = used + rng() % (7 - used);
			u32 place[6] = {0, 1, 2, 3, 4, 5};
			for (u32 i = 0; i < used; i++)
				std::swap(place[i], place[i + rng() % (stride - i)]);
			inv.off = {stride, place[0], (used == 3) ? place[2] : 0u, place[1]};
		}
		return inv;
	}

	u32 PlausibleFloat(Rng& rng)
	{
		const u32 r = rng();
		if ((r & 7) == 0)
			return kSpecialFloats[(r >> 3) % std::size(kSpecialFloats)];
		if ((r & 7) == 1)
			return rng();
		const float f = (static_cast<float>(static_cast<int>(rng() >> 8) - (1 << 23)) / static_cast<float>(1 << 12));
		u32 bits;
		std::memcpy(&bits, &f, sizeof(bits));
		return bits;
	}

	// Random bytes everywhere, then floats worth dividing in the first three
	// words of every descriptor: whichever of them the layout reads as {S, T, Q}
	// then holds numbers, zeros, infinities and NaNs rather than noise.
	void FillRecords(GIFPackedReg* r, u32 regs, Rng& rng)
	{
		for (u32 i = 0; i < regs; i++)
		{
			for (u32 w = 0; w < 4; w++)
				r[i].U32[w] = rng();
			if (rng() & 1)
			{
				for (u32 w = 0; w < 3; w++)
					r[i].U32[w] = PlausibleFloat(rng);
			}
		}
	}

	struct Result
	{
		std::vector<GSVertex> vbuff;
		FmmAcc acc;
	};

	Result RunPass(PassFn fn, const GIFPackedReg* rin, const std::vector<ConvertJob>& jobs, const Invariants& inv,
		const FmmAcc& acc0, u32 seed)
	{
		Result res;
		res.vbuff.resize(kSlots);
		std::mt19937 fill(seed);
		for (GSVertex& v : res.vbuff)
		{
			u32 words[8];
			for (u32& w : words)
				w = static_cast<u32>(fill());
			std::memcpy(&v, words, sizeof(words));
		}
		res.acc = acc0;
		const ConvertArgs args = {rin, res.vbuff.data(), jobs.data(), jobs.data() + jobs.size(), &res.acc, &inv};
		fn(args);
		return res;
	}

	FmmAcc MakeAcc(const PassSetup& su, Rng& rng)
	{
		FmmAcc a;
		GSVertexKernels::FmmAccReset(a, (su.shade & 1u) != 0, (su.shade & 2u) != 0);
		// Sometimes as a draw's earlier vertices would have left it.
		if (rng() & 1)
		{
			GSVector4i m0, m1;
			for (u32 i = 0; i < 4; i++)
			{
				m0.U32[i] = PlausibleFloat(rng);
				m1.U32[i] = rng();
			}
			GSVertexKernels::FmmAccumVertex(a, m0, m1, (su.shade & 1u) != 0, (su.shade & 2u) != 0, true);
		}
		return a;
	}

	::testing::AssertionResult SameResult(const Result& a, const Result& b)
	{
		for (u32 i = 0; i < kSlots; i++)
		{
			if (std::memcmp(&a.vbuff[i], &b.vbuff[i], sizeof(GSVertex)) != 0)
			{
				const u32* x = reinterpret_cast<const u32*>(&a.vbuff[i]);
				const u32* y = reinterpret_cast<const u32*>(&b.vbuff[i]);
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"slot %u: old %08x %08x %08x %08x | %08x %08x %08x %08x, new %08x %08x %08x %08x | %08x %08x %08x %08x", i,
					x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], y[0], y[1], y[2], y[3], y[4], y[5], y[6], y[7]);
				return ::testing::AssertionFailure() << buf;
			}
		}
		static const char* const names[7] = {"pmin", "pmax", "tmin", "tmax", "tnan", "cmin", "cmax"};
		const GSVector4i* const x = &a.acc.pmin;
		const GSVector4i* const y = &b.acc.pmin;
		static_assert(sizeof(FmmAcc) == 7 * sizeof(GSVector4i));
		for (u32 i = 0; i < 7; i++)
		{
			if (std::memcmp(&x[i], &y[i], sizeof(GSVector4i)) != 0)
			{
				char buf[160];
				std::snprintf(buf, sizeof(buf), "%s: old %08x %08x %08x %08x, new %08x %08x %08x %08x", names[i], x[i].U32[0],
					x[i].U32[1], x[i].U32[2], x[i].U32[3], y[i].U32[0], y[i].U32[1], y[i].U32[2], y[i].U32[3]);
				return ::testing::AssertionFailure() << buf;
			}
		}
		return ::testing::AssertionSuccess();
	}

	std::string Describe(const PassSetup& su)
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%s, %s, clamp %d, shade %u, keep_gif_q %d", kLayoutNames[su.layout],
			su.triangle ? "triangle" : "sprite", static_cast<int>(su.clamp), su.shade, su.keep_gif_q ? 1 : 0);
		return buf;
	}

	constexpr GSLimit24BitDepth kClampModes[3] = {GSLimit24BitDepth::Disabled, GSLimit24BitDepth::PrioritizeLower,
		GSLimit24BitDepth::PrioritizeUpper};

	template <typename F>
	void ForEverySetup(F&& f)
	{
		for (u32 layout = 0; layout < 8; layout++)
		{
			for (u32 triangle = 0; triangle < 2; triangle++)
			{
				for (const GSLimit24BitDepth clamp : kClampModes)
				{
					for (u32 shade = 0; shade < 8; shade++)
					{
						for (u32 keep = 0; keep < 2; keep++)
							f(PassSetup{layout, triangle != 0, clamp, shade, keep != 0});
					}
				}
			}
		}
	}

	// Both passes on one input; true if the results are the same.
	::testing::AssertionResult Compare(const PassSetup& su, const GIFPackedReg* rin, const std::vector<ConvertJob>& jobs,
		const Invariants& inv, const FmmAcc& acc0, u32 seed, Result* out = nullptr)
	{
		const bool clamp = inv.clamp_enabled;
		const Result o = RunPass(kOldPass[su.layout][su.triangle][clamp], rin, jobs, inv, acc0, seed);
		const Result n = RunPass(kNewPass[su.layout][su.triangle][clamp], rin, jobs, inv, acc0, seed);
		if (out)
			*out = n;
		return SameResult(o, n);
	}
} // namespace

// Job lists as the kernel's plan could never make them and as it does: any
// number of jobs, any stretch of the chunk, slots that overlap an earlier job's,
// acc_slot and colour_slot below, inside and above the job or absent.
TEST(GsKickConvert, MatchesOnRandomJobs)
{
	RecordPages pages;
	ASSERT_TRUE(pages.ok());
	Rng rng(0x5EED0001u);
	u64 bounds_moved = 0, nan_recorded = 0, cases = 0;

	ForEverySetup([&](const PassSetup& su) {
		for (u32 round = 0; round < 40; round++)
		{
			const Invariants inv = MakeInvariants(su, rng);
			const u32 stride = GSVertexKernels::LayoutIsContiguousTriple(static_cast<PackedLayout>(su.layout)) ? 3 : inv.off.stride;
			const u32 count = 1 + rng() % kChunk;
			GIFPackedReg* const rin = pages.Place(count * stride * sizeof(GIFPackedReg), (round & 1) != 0);
			FillRecords(rin, count * stride, rng);

			std::vector<ConvertJob> jobs;
			const u32 njobs = 1 + rng() % 6;
			for (u32 j = 0; j < njobs; j++)
			{
				const u32 from = rng() % count;
				const u32 to = from + 1 + rng() % (count - from);
				const u32 len = to - from;
				const u32 slot = rng() % (kSlots - len + 1);
				const auto mark = [&]() -> u32 {
					switch (rng() % 5)
					{
						case 0:
							return ~0u;
						case 1:
							return slot - std::min(slot, rng() % 4);
						case 2:
							return slot + len + rng() % 3;
						default:
							return slot + rng() % len;
					}
				};
				const u32 acc_slot = mark();
				const u32 colour_slot = mark();
				jobs.push_back({from, to, slot, acc_slot, colour_slot});
			}

			const FmmAcc acc0 = MakeAcc(su, rng);
			Result res;
			const u32 seed = rng();
			ASSERT_TRUE(Compare(su, rin, jobs, inv, acc0, seed, &res)) << Describe(su) << ", round " << round;
			cases++;
			if (std::memcmp(&res.acc, &acc0, sizeof(FmmAcc)) != 0)
				bounds_moved++;
			if ((res.acc.tnan.U64[0] | res.acc.tnan.U64[1]) != 0)
				nan_recorded++;
		}
	});

	// The comparison is not of two passes that both did nothing.
	EXPECT_EQ(cases, 8u * 2 * 3 * 8 * 2 * 40);
	EXPECT_GT(bounds_moved, cases / 4);
	EXPECT_GT(nan_recorded, cases / 50);
}

// One job over the whole chunk at every count the kernel can pass, with the
// chunk's first record on the page after an unreadable one and then with its
// last record ending against an unreadable one: a load that leaves the chunk
// on either side faults.
TEST(GsKickConvert, MatchesAtEveryCountAgainstUnreadablePages)
{
	RecordPages pages;
	ASSERT_TRUE(pages.ok());
	Rng rng(0x5EED0002u);

	for (u32 layout = 0; layout < 8; layout++)
	{
		for (u32 triangle = 0; triangle < 2; triangle++)
		{
			for (u32 count = 1; count <= kChunk; count++)
			{
				for (u32 at_end = 0; at_end < 2; at_end++)
				{
					const PassSetup su = {layout, triangle != 0, kClampModes[count % 3], rng() & 7u, (rng() & 7u) == 0};
					const Invariants inv = MakeInvariants(su, rng);
					const u32 stride = GSVertexKernels::LayoutIsContiguousTriple(static_cast<PackedLayout>(layout)) ? 3 : inv.off.stride;
					GIFPackedReg* const rin = pages.Place(count * stride * sizeof(GIFPackedReg), at_end != 0);
					FillRecords(rin, count * stride, rng);

					// A convert-only stretch, then the bounds, as a drawn run leaves it.
					const u32 slot = rng() % (kSlots - count + 1);
					const u32 acc_slot = slot + rng() % (count + 1);
					const std::vector<ConvertJob> jobs = {{0, count, slot, acc_slot, acc_slot + rng() % 3}};
					ASSERT_TRUE(Compare(su, rin, jobs, inv, MakeAcc(su, rng), rng()))
						<< Describe(su) << ", count " << count << ", at_end " << at_end;
				}
			}
		}
	}
}

// acc_slot and colour_slot at every position from below a job's first slot to
// above its last, for every texture and shading mode: the three stretches at
// every length, including none.
TEST(GsKickConvert, MatchesAtEveryStretchBoundary)
{
	RecordPages pages;
	ASSERT_TRUE(pages.ok());
	Rng rng(0x5EED0003u);
	constexpr u32 n = 7;
	constexpr u32 slot = 40;

	for (u32 layout = 0; layout < 8; layout++)
	{
		for (u32 shade = 0; shade < 8; shade++)
		{
			const PassSetup su = {layout, true, GSLimit24BitDepth::Disabled, shade, false};
			const Invariants inv = MakeInvariants(su, rng);
			const u32 stride = GSVertexKernels::LayoutIsContiguousTriple(static_cast<PackedLayout>(layout)) ? 3 : inv.off.stride;
			GIFPackedReg* const rin = pages.Place(n * stride * sizeof(GIFPackedReg), true);
			FillRecords(rin, n * stride, rng);
			const FmmAcc acc0 = MakeAcc(su, rng);

			for (u32 a = slot - 2; a <= slot + n + 2; a++)
			{
				for (u32 c = slot - 2; c <= slot + n + 2; c++)
				{
					const std::vector<ConvertJob> jobs = {{0, n, slot, a, c}};
					ASSERT_TRUE(Compare(su, rin, jobs, inv, acc0, 7)) << Describe(su) << ", acc_slot " << a << ", colour_slot " << c;
				}
				const std::vector<ConvertJob> none = {{0, n, slot, a, ~0u}};
				ASSERT_TRUE(Compare(su, rin, none, inv, acc0, 7)) << Describe(su) << ", acc_slot " << a << ", no colour_slot";
			}
			const std::vector<ConvertJob> neither = {{0, n, slot, ~0u, ~0u}};
			ASSERT_TRUE(Compare(su, rin, neither, inv, acc0, 7)) << Describe(su) << ", no acc_slot";
		}
	}
}

// The Q fix-ups by their definition, not only by the oracle: a Q of +0 becomes
// FLT_MIN on the three layouts that read Q from the record, a NaN Q becomes
// FLT_MAX on the NOP-padded one alone, every other Q passes, and under
// keep_gif_q every Q passes.
TEST(GsKickConvert, QFixUpsAreTheDefinition)
{
	RecordPages pages;
	ASSERT_TRUE(pages.ok());
	Rng rng(0x5EED0004u);
	const u32 triples[3] = {static_cast<u32>(PackedLayout::TripleXYZF2), static_cast<u32>(PackedLayout::TripleXYZ2),
		static_cast<u32>(PackedLayout::NopTripleXYZF2)};
	u32 zeros = 0, nans = 0;

	for (const u32 layout : triples)
	{
		for (u32 keep = 0; keep < 2; keep++)
		{
			for (u32 triangle = 0; triangle < 2; triangle++)
			{
				const PassSetup su = {layout, triangle != 0, GSLimit24BitDepth::Disabled, 5, keep != 0};
				const Invariants inv = MakeInvariants(su, rng);
				const u32 stride = (layout == static_cast<u32>(PackedLayout::NopTripleXYZF2)) ? inv.off.stride : 3;
				const u32 off_a = (stride == 3 && layout != static_cast<u32>(PackedLayout::NopTripleXYZF2)) ? 0 : inv.off.off_a;
				const u32 count = static_cast<u32>(std::size(kSpecialFloats));
				GIFPackedReg* const rin = pages.Place(count * stride * sizeof(GIFPackedReg), true);
				FillRecords(rin, count * stride, rng);
				for (u32 i = 0; i < count; i++)
				{
					// S, T and the colour's low bytes zero as well: only Q may be touched.
					rin[i * stride + off_a].U32[0] = (i & 1) ? 0u : kSpecialFloats[(i + 5) % count];
					rin[i * stride + off_a].U32[1] = (i & 2) ? 0u : kSpecialFloats[(i + 9) % count];
					rin[i * stride + off_a].U32[2] = kSpecialFloats[i];
				}

				const std::vector<ConvertJob> jobs = {{0, count, 3, 5, 5}};
				Result res;
				ASSERT_TRUE(Compare(su, rin, jobs, inv, MakeAcc(su, rng), 11, &res)) << Describe(su);
				for (u32 i = 0; i < count; i++)
				{
					const u32 q = kSpecialFloats[i];
					const bool is_nan = (q & 0x7FFFFFFFu) > 0x7F800000u;
					u32 want = q;
					if (!keep && q == 0)
					{
						want = 0x00800000u;
						zeros++;
					}
					if (!keep && is_nan && layout == static_cast<u32>(PackedLayout::NopTripleXYZF2))
					{
						want = 0x7F7FFFFFu;
						nans++;
					}
					const GSVertex& v = res.vbuff[3 + i];
					u32 got[4];
					std::memcpy(got, &v.m[0], sizeof(got));
					EXPECT_EQ(got[3], want) << Describe(su) << ", Q " << std::hex << q;
					EXPECT_EQ(got[0], rin[i * stride + off_a].U32[0]) << Describe(su) << ", S with Q " << std::hex << q;
					EXPECT_EQ(got[1], rin[i * stride + off_a].U32[1]) << Describe(su) << ", T with Q " << std::hex << q;
				}
			}
		}
	}
	EXPECT_EQ(zeros, 3u * 2);
	EXPECT_EQ(nans, 6u * 2);
}

// A record's own UV keeps fourteen bits a half whatever else the descriptor
// holds, and the fields the tag does not write come from the carried state.
TEST(GsKickConvert, RecordUVIsMaskedAndTheRestIsCarried)
{
	RecordPages pages;
	ASSERT_TRUE(pages.ok());
	Rng rng(0x5EED0005u);

	for (u32 layout = 0; layout < 8; layout++)
	{
		const PackedLayout l = static_cast<PackedLayout>(layout);
		const PassSetup su = {layout, false, GSLimit24BitDepth::Disabled, 3, false};
		Invariants inv = MakeInvariants(su, rng);
		inv.uvfog = 0xA5A5A5A5C3C3C3C3ull;
		const u32 stride = GSVertexKernels::LayoutIsContiguousTriple(l) ? 3 : inv.off.stride;
		const u32 off_a = GSVertexKernels::LayoutIsContiguousTriple(l) ? 0 : inv.off.off_a;
		constexpr u32 count = 4;
		GIFPackedReg* const rin = pages.Place(count * stride * sizeof(GIFPackedReg), true);
		FillRecords(rin, count * stride, rng);
		if (GSVertexKernels::LayoutHasUV(l))
		{
			for (u32 i = 0; i < count; i++)
			{
				rin[i * stride + off_a].U32[0] = 0xFFFF0000u | (0xC000u + 0x1234u * (i + 1));
				rin[i * stride + off_a].U32[1] = 0xFFFF0000u | (0x8000u + 0x0421u * (i + 1));
			}
		}

		const std::vector<ConvertJob> jobs = {{0, count, 0, ~0u, ~0u}};
		Result res;
		ASSERT_TRUE(Compare(su, rin, jobs, inv, MakeAcc(su, rng), 13, &res)) << Describe(su);
		for (u32 i = 0; i < count; i++)
		{
			const GSVertex& v = res.vbuff[i];
			if (GSVertexKernels::LayoutHasUV(l))
			{
				EXPECT_EQ(v.U, (0xC000u + 0x1234u * (i + 1)) & 0x3FFFu) << kLayoutNames[layout];
				EXPECT_EQ(v.V, (0x8000u + 0x0421u * (i + 1)) & 0x3FFFu) << kLayoutNames[layout];
			}
			else
			{
				EXPECT_EQ(v.UV, 0xC3C3C3C3u) << kLayoutNames[layout];
			}
			if (!GSVertexKernels::LayoutIsXYZF2(l))
			{
				EXPECT_EQ(v.FOG, 0xA5A5A5A5u) << kLayoutNames[layout];
			}
			if (l == PackedLayout::PairUVXYZ2 || l == PackedLayout::PairUVXYZF2)
			{
				EXPECT_EQ(std::memcmp(&v.m[0], &inv.carry_m0, 16), 0) << kLayoutNames[layout];
			}
		}
	}
}

#endif // ARCH_ARM64
