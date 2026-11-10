// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The kick kernel's decision pass (GSVertexKickKernel::DecideVector) against the
// pass it replaced, kept here as the oracle, on the same inputs.
//
// DecideVector decides on the raw 16-bit coordinates, both axes in one lane; the
// pass it replaced worked on offset-subtracted 32-bit ones, X and Y apart. So the
// comparison is of everything the pass writes -- both accept words, the "any"
// flag and the box -- over inputs chosen to sit on every edge where the two
// forms could part: each scissor edge moved into raw space at 0, 1, 0xFFFF and
// past it; boxes as wide as the coordinate space; band remainders at every
// alignment; ADC, repeated positions, and every count and record step the
// kernel can pass. DecideVectorWide, the one-vertex-at-a-time form DecideVector
// hands the calls it cannot state to, is held to the same oracle on every input,
// not only on the ones that reach it.

#include <gtest/gtest.h>

#include "GS/GS.h"
#include "GS/GSVertexKickKernel.h"

#ifdef ARCH_ARM64

#include <cstdio>
#include <cstring>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace
{
	using GSVertexKickKernel::DecideBounds;
	using GSVertexKickKernel::VectorDecided;

	struct ClassDesc
	{
		u32 n;
		bool keyed;
		bool every;
		const char* name;
	};

	constexpr ClassDesc kClasses[] = {
		{3, false, true, "strip, grid"},
		{3, true, true, "strip, no grid"},
		{3, false, false, "list, grid"},
		{3, true, false, "list, no grid"},
		{2, false, false, "sprite, grid"},
		{2, true, false, "sprite, no grid"},
	};

	using DecideFn = void (*)(const u8*, size_t, u32, u32, const DecideBounds&, u64*, VectorDecided&);

	constexpr u32 DecideClass(u32 n, bool keyed, bool every)
	{
		return ((n == 2) ? 1u : 0u) | (keyed ? 2u : 0u) | (every ? 4u : 0u);
	}

	using GSVertexKickKernel::kDecideLaneMask;

	// The oracle: the decision pass as it stood before DecideVector moved to raw
	// coordinates (ARMSX2 gs-runs 43f5c61992), verbatim but for its name.
	template <u32 n, bool keyed, bool every>
	__noinline void OldDecideVector(const u8* RESTRICT pxyz, size_t step_bytes, u32 count, u32 period,
		const DecideBounds& kb, u64* RESTRICT accept, VectorDecided& out)
	{
		const int32x4_t ofx = vdupq_n_s32(kb.ofx);
		const int32x4_t ofy = vdupq_n_s32(kb.ofy);
		const int32x4_t bl = vdupq_n_s32(kb.l);
		const int32x4_t bt = vdupq_n_s32(kb.t);
		const int32x4_t br = vdupq_n_s32(kb.r);
		const int32x4_t bb = vdupq_n_s32(kb.b);
		const uint32x4_t step = vdupq_n_u32(kb.step);
		const uint32x4_t m16 = vdupq_n_u32(0xFFFFu);
		const int32x4_t imax = vdupq_n_s32(std::numeric_limits<int>::max());
		const int32x4_t imin = vdupq_n_s32(std::numeric_limits<int>::min());
		alignas(16) static constexpr u32 lane_bits[4] = {1, 2, 4, 8};
		const uint32x4_t bits = vld1q_u32(lane_bits);

		// The validity of the three quads a list's pattern cycles through.
		uint32x4_t valid0 = vdupq_n_u32(0), valid1 = valid0, valid2 = valid0;
		if constexpr (!every)
		{
			valid0 = vld1q_u32(kDecideLaneMask[period & 15]);
			valid1 = vld1q_u32(kDecideLaneMask[(period >> 4) & 15]);
			valid2 = vld1q_u32(kDecideLaneMask[(period >> 8) & 15]);
		}

		const u32* RESTRICT first = reinterpret_cast<const u32*>(pxyz);
		int32x4_t px = vdupq_n_s32(static_cast<int>(first[0] & 0xFFFFu) - kb.ofx);
		int32x4_t py = vdupq_n_s32(static_cast<int>(first[1] & 0xFFFFu) - kb.ofy);
		uint32x4_t peq = vdupq_n_u32(0);
		int32x4_t amnx = imax, amny = imax, amxx = imin, amxy = imin;

		// Returns the quad's four accept bits.
		const auto quad = [&](uint32x4_t v0, uint32x4_t v1, uint32x4_t v2, uint32x4_t v3) __attribute__((always_inline))
		{
			// The even half-words of a packed position are X, Y, the low half of Z
			// and the half of the last word that carries ADC in its top bit. Two
			// unzips leave X in the low half of each lane of one vector, and Y under
			// that ADC half in the other, so ADC is the lane's sign.
			const uint16x8_t t01 = vuzp1q_u16(vreinterpretq_u16_u32(v0), vreinterpretq_u16_u32(v1));
			const uint16x8_t t23 = vuzp1q_u16(vreinterpretq_u16_u32(v2), vreinterpretq_u16_u32(v3));
			const uint32x4_t X = vreinterpretq_u32_u16(vuzp1q_u16(t01, t23));
			const uint32x4_t Y = vreinterpretq_u32_u16(vuzp2q_u16(t01, t23));

			const int32x4_t wx = vsubq_s32(vreinterpretq_s32_u32(vandq_u32(X, m16)), ofx);
			const int32x4_t wy = vsubq_s32(vreinterpretq_s32_u32(vandq_u32(Y, m16)), ofy);

			// The vertex before each lane's, and for a triangle the one before that.
			const int32x4_t x1 = vextq_s32(px, wx, 3);
			const int32x4_t y1 = vextq_s32(py, wy, 3);
			int32x4_t mnx = vminq_s32(wx, x1);
			int32x4_t mxx = vmaxq_s32(wx, x1);
			int32x4_t mny = vminq_s32(wy, y1);
			int32x4_t mxy = vmaxq_s32(wy, y1);

			uint32x4_t rej = vcltzq_s32(vreinterpretq_s32_u32(Y));

			if constexpr (n == 3)
			{
				const int32x4_t x2 = vextq_s32(px, wx, 2);
				const int32x4_t y2 = vextq_s32(py, wy, 2);
				mnx = vminq_s32(mnx, x2);
				mxx = vmaxq_s32(mxx, x2);
				mny = vminq_s32(mny, y2);
				mxy = vmaxq_s32(mxy, y2);

				// Two positions equal: this vertex and the last, this and the one
				// before, and the last two, which is the first of these one lane on.
				const uint32x4_t eq1 = vandq_u32(vceqq_s32(wx, x1), vceqq_s32(wy, y1));
				const uint32x4_t eq2 = vandq_u32(vceqq_s32(wx, x2), vceqq_s32(wy, y2));
				rej = vorrq_u32(rej, vorrq_u32(vorrq_u32(eq1, eq2), vextq_u32(peq, eq1, 3)));
				peq = eq1;
			}

			rej = vorrq_u32(rej, vorrq_u32(vcltq_s32(mxx, bl), vcgeq_s32(mnx, br)));
			rej = vorrq_u32(rej, vorrq_u32(vcltq_s32(mxy, bt), vcgeq_s32(mny, bb)));

			if constexpr (keyed)
			{
				const int32x4_t m15 = vdupq_n_s32(15);
				const auto key = [&](int32x4_t v) __attribute__((always_inline))
				{
					return vsubq_s32(v, vreinterpretq_s32_u32(vceqq_s32(vandq_s32(v, m15), m15)));
				};
				rej = vorrq_u32(rej, vorrq_u32(vceqq_s32(key(mnx), key(mxx)), vceqq_s32(key(mny), key(mxy))));
			}
			else
			{
				const uint32x4_t dx = vreinterpretq_u32_s32(veorq_s32(mnx, mxx));
				const uint32x4_t dy = vreinterpretq_u32_s32(veorq_s32(mny, mxy));
				rej = vorrq_u32(rej, vorrq_u32(vcltq_u32(dx, step), vcltq_u32(dy, step)));
			}

			uint32x4_t acc;
			if constexpr (every)
			{
				acc = vmvnq_u32(rej);
			}
			else
			{
				acc = vbicq_u32(valid0, rej);
				const uint32x4_t t = valid0;
				valid0 = valid1;
				valid1 = valid2;
				valid2 = t;
			}

			amnx = vminq_s32(amnx, vbslq_s32(acc, mnx, imax));
			amny = vminq_s32(amny, vbslq_s32(acc, mny, imax));
			amxx = vmaxq_s32(amxx, vbslq_s32(acc, mxx, imin));
			amxy = vmaxq_s32(amxy, vbslq_s32(acc, mxy, imin));

			px = wx;
			py = wy;
			return static_cast<u64>(vaddvq_u32(vandq_u32(acc, bits)));
		};

		const auto ld = [](const u8* p) __attribute__((always_inline)) { return vld1q_u32(reinterpret_cast<const u32*>(p)); };

		// Sixty-four vertices to an accept word, so the word being built stays in
		// a register.
		const u8* RESTRICT p = pxyz;
		u32 left = count;
		for (u32 w = 0; w < 2; w++)
		{
			u64 word = 0;
			u32 block = std::min<u32>(left, 64);
			left -= block;
			u32 shift = 0;
			for (; block >= 4; block -= 4, shift += 4, p += 4 * step_bytes)
				word |= quad(ld(p), ld(p + step_bytes), ld(p + 2 * step_bytes), ld(p + 3 * step_bytes)) << shift;
			if (block != 0)
			{
				// The last one to three vertices of the chunk; nothing may be read
				// past the packet.
				const u8* const p1 = p + ((block > 1) ? step_bytes : 0);
				const u8* const p2 = p1 + ((block > 2) ? step_bytes : 0);
				word |= quad(ld(p), ld(p1), ld(p2), ld(p2)) << shift;
			}
			accept[w] = word;
		}

		out.any = (accept[0] | accept[1]) != 0;
		if (out.any)
		{
			out.minx = vminvq_s32(amnx) + kb.bias_x;
			out.miny = vminvq_s32(amny) + kb.bias_y;
			out.maxx = vmaxvq_s32(amxx) + kb.bias_x;
			out.maxy = vmaxvq_s32(amxy) + kb.bias_y;
		}
	}

	template <u32 n, bool keyed, bool every>
	void OldThunk(const u8* p, size_t s, u32 c, u32 per, const DecideBounds& kb, u64* a, VectorDecided& o)
	{
		OldDecideVector<n, keyed, every>(p, s, c, per, kb, a, o);
	}
	template <u32 n, bool keyed, bool every>
	void NewThunk(const u8* p, size_t s, u32 c, u32 per, const DecideBounds& kb, u64* a, VectorDecided& o)
	{
		GSVertexKickKernel::DecideVector<n, keyed, every>(p, s, c, per, kb, a, o);
	}
	template <u32 n, bool keyed, bool every>
	void WideThunk(const u8* p, size_t s, u32 c, u32 per, const DecideBounds& kb, u64* a, VectorDecided& o)
	{
		GSVertexKickKernel::DecideVectorWide<n, keyed, every>(p, s, c, per, kb, a, o);
	}

	constexpr DecideFn kOld[8] = {&OldThunk<3, false, false>, &OldThunk<2, false, false>, &OldThunk<3, true, false>,
		&OldThunk<2, true, false>, &OldThunk<3, false, true>, nullptr, &OldThunk<3, true, true>, nullptr};
	constexpr DecideFn kNew[8] = {&NewThunk<3, false, false>, &NewThunk<2, false, false>, &NewThunk<3, true, false>,
		&NewThunk<2, true, false>, &NewThunk<3, false, true>, nullptr, &NewThunk<3, true, true>, nullptr};
	constexpr DecideFn kWide[8] = {&WideThunk<3, false, false>, &WideThunk<2, false, false>, &WideThunk<3, true, false>,
		&WideThunk<2, true, false>, &WideThunk<3, false, true>, nullptr, &WideThunk<3, true, true>, nullptr};

	DecideFn Old(const ClassDesc& c) { return kOld[DecideClass(c.n, c.keyed, c.every)]; }

	struct Pos
	{
		u16 x, y;
		bool adc;
	};

	// Records whose last position ends on the last byte before a page that
	// cannot be read, so a load past the last record faults.
	class GuardedRecords
	{
	public:
		GuardedRecords()
		{
			m_page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
			m_span = 2 * m_page;
			while (m_span < 128 * 64 + m_page)
				m_span += m_page;
			m_base = static_cast<u8*>(mmap(nullptr, m_span + m_page, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
			if (m_base == MAP_FAILED)
				m_base = nullptr;
			else
				mprotect(m_base + m_span, m_page, PROT_NONE);
		}
		~GuardedRecords()
		{
			if (m_base)
				munmap(m_base, m_span + m_page);
		}

		bool ok() const { return m_base != nullptr; }

		// Lays the positions out `step` bytes apart; everything between them is
		// noise, as the other registers of a record are.
		const u8* Fill(const std::vector<Pos>& v, size_t step, std::mt19937& rng)
		{
			const size_t bytes = (v.size() - 1) * step + 16;
			u8* const first = m_base + m_span - bytes;
			for (size_t i = 0; i < bytes; i++)
				first[i] = static_cast<u8>(rng());
			for (size_t i = 0; i < v.size(); i++)
			{
				u32 w[4];
				w[0] = v[i].x | (rng() << 16);
				w[1] = v[i].y | (rng() << 16);
				w[2] = rng();
				w[3] = (rng() & ~0x8000u) | (v[i].adc ? 0x8000u : 0u);
				std::memcpy(first + i * step, w, 16);
			}
			return first;
		}

	private:
		size_t m_page = 0;
		size_t m_span = 0;
		u8* m_base = nullptr;
	};

	struct Result
	{
		u64 accept[2];
		VectorDecided out;
	};

	Result RunDecide(DecideFn fn, const u8* p, size_t step, u32 count, u32 period, const DecideBounds& kb)
	{
		Result r;
		r.accept[0] = r.accept[1] = 0x5A5A5A5A5A5A5A5Aull;
		r.out.minx = 0x11111111;
		r.out.miny = 0x22222222;
		r.out.maxx = 0x33333333;
		r.out.maxy = 0x44444444;
		r.out.any = true;
		fn(p, step, count, period, kb, r.accept, r.out);
		return r;
	}

	::testing::AssertionResult Same(const ClassDesc& c, const u8* p, size_t step, u32 count, u32 period,
		const DecideBounds& kb)
	{
		const u32 cls = DecideClass(c.n, c.keyed, c.every);
		const Result ref = RunDecide(kOld[cls], p, step, count, period, kb);
		for (int which = 0; which < 2; which++)
		{
			const Result got = RunDecide(which ? kWide[cls] : kNew[cls], p, step, count, period, kb);
			if (ref.accept[0] == got.accept[0] && ref.accept[1] == got.accept[1] && ref.out.any == got.out.any &&
				ref.out.minx == got.out.minx && ref.out.miny == got.out.miny && ref.out.maxx == got.out.maxx &&
				ref.out.maxy == got.out.maxy)
			{
				continue;
			}
			return ::testing::AssertionFailure()
				   << (which ? "DecideVectorWide, " : "DecideVector, ") << c.name << " count " << count << " step " << step
				   << " period " << period << " of (" << kb.ofx << "," << kb.ofy << ") ltrb (" << kb.l << "," << kb.t << ","
				   << kb.r << "," << kb.b << ") bias (" << kb.bias_x << "," << kb.bias_y << ") step " << kb.step << std::hex
				   << ": accept " << ref.accept[0] << ":" << ref.accept[1] << " against " << got.accept[0] << ":"
				   << got.accept[1] << std::dec << ", any " << ref.out.any << " against " << got.out.any << ", box ("
				   << ref.out.minx << "," << ref.out.miny << "," << ref.out.maxx << "," << ref.out.maxy << ") against ("
				   << got.out.minx << "," << got.out.miny << "," << got.out.maxx << "," << got.out.maxy << ")";
		}
		return ::testing::AssertionSuccess();
	}

	// The periods RunChunk passes for the class, plus patterns it never passes.
	std::vector<u32> Periods(const ClassDesc& c)
	{
		if (c.every)
			return {0};
		if (c.n == 3)
			return {0x249u, 0x492u, 0x924u, 0xFFFu, 0x000u, 0x5A3u};
		return {0x555u, 0xAAAu, 0xFFFu, 0x000u, 0x249u, 0x5A3u};
	}

	DecideBounds Bounds(int xyofx, int xyofy, int bias, u32 step, int l, int t, int r, int b)
	{
		DecideBounds kb = {};
		kb.bias_x = kb.bias_y = bias;
		kb.ofx = xyofx + bias;
		kb.ofy = xyofy + bias;
		kb.l = l;
		kb.t = t;
		kb.r = r;
		kb.b = b;
		kb.step = step;
		return kb;
	}

	// The bias and step a class is given: none and 1 where it has no grid.
	DecideBounds ClassBounds(const ClassDesc& c, int xyofx, int xyofy, int bias, u32 step, int l, int t, int r, int b)
	{
		return c.keyed ? Bounds(xyofx, xyofy, 0, 1, l, t, r, b) : Bounds(xyofx, xyofy, bias, step, l, t, r, b);
	}

	std::vector<Pos> RandomPositions(u32 count, std::mt19937& rng, int mode, u16 cx, u16 cy)
	{
		std::vector<Pos> v(count);
		for (u32 i = 0; i < count; i++)
		{
			Pos p;
			switch (mode)
			{
				case 0: // anywhere
					p.x = static_cast<u16>(rng());
					p.y = static_cast<u16>(rng());
					break;
				case 1: // a few pixels around a point: one-band boxes and repeats
					p.x = static_cast<u16>(cx + (rng() % 40) - 20);
					p.y = static_cast<u16>(cy + (rng() % 40) - 20);
					break;
				case 2: // a screenful around a point
					p.x = static_cast<u16>(cx + (rng() % 12000) - 6000);
					p.y = static_cast<u16>(cy + (rng() % 9000) - 4500);
					break;
				default: // the ends of the coordinate space
					p.x = static_cast<u16>((rng() & 1) ? (rng() % 48) : (0xFFFF - (rng() % 48)));
					p.y = static_cast<u16>((rng() & 1) ? (rng() % 48) : (0xFFFF - (rng() % 48)));
					break;
			}
			p.adc = (rng() % 5) == 0;
			if (i > 0 && (rng() % 7) == 0)
			{
				p.x = v[i - 1].x;
				p.y = v[i - 1].y;
			}
			else if (i > 1 && (rng() % 11) == 0)
			{
				p.x = v[i - 2].x;
				p.y = v[i - 2].y;
			}
			v[i] = p;
		}
		return v;
	}
} // namespace

// Every count the kernel can pass, at every record step, with the last record
// against an unreadable page.
TEST(GsKickDecide, MatchesAtEveryCountAndStep)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());
	for (const ClassDesc& c : kClasses)
	{
		std::mt19937 rng(1000 + DecideClass(c.n, c.keyed, c.every));
		for (u32 count = 1; count <= 128; count++)
		{
			for (size_t step : {16u, 32u, 48u, 64u})
			{
				const int mode = static_cast<int>((count + step / 16) % 3);
				const std::vector<Pos> v = RandomPositions(count, rng, mode, 27648 + 5120, 27648 + 3584);
				const u8* const p = rec.Fill(v, step, rng);
				const DecideBounds kb = ClassBounds(c, 27648, 27648, 1, 16, 0, 0, 640 * 16, 448 * 16);
				for (u32 period : Periods(c))
					ASSERT_TRUE(Same(c, p, step, count, period, kb));
			}
		}
	}
}

// Each scissor edge in turn below zero in raw space, at its sixteen-bit limit
// and past it, and grid steps up to and past the largest DecideVector states
// itself. Past the limit the call is DecideVectorWide's; either way the result
// is the oracle's.
TEST(GsKickDecide, MatchesWhereTheBoundsLeaveSixteenBits)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());
	std::mt19937 rng(77);

	for (const ClassDesc& c : kClasses)
	{
		const std::vector<u32> periods = Periods(c);
		const DecideBounds probe = ClassBounds(c, 27648, 27648, 1, 16, 0, 0, 0, 0);
		const DecideBounds usual = ClassBounds(c, 27648, 27648, 1, 16, 0, 0, 640 * 16, 448 * 16);
		for (int mode = 0; mode < 4; mode++)
		{
			const std::vector<Pos> v = RandomPositions(40, rng, mode, 27648 + 5120, 27648 + 3584);
			const u8* const p = rec.Fill(v, 48, rng);
			for (u32 period : periods)
			{
				for (int which = 0; which < 4; which++)
				{
					const int of = (which & 1) ? probe.ofy : probe.ofx;
					for (int raw : {-70000, -1, 0, 1, 65535, 65536, 65537, 131072, 0x7FFF0000})
					{
						DecideBounds kb = usual;
						((which == 0) ? kb.l : (which == 1) ? kb.t : (which == 2) ? kb.r : kb.b) = raw - of;
						ASSERT_TRUE(Same(c, p, 48, 40, period, kb)) << "edge " << which << " at " << raw;
					}
				}
				if (!c.keyed)
				{
					for (u32 step : {1u, 2u, 16u, 32u, 128u, 0x8000u, 0x10000u, 0x10001u, 0x20000u, 0x80000000u})
						ASSERT_TRUE(Same(c, p, 48, 40, period, Bounds(27648, 27648, 1, step, 0, 0, 640 * 16, 448 * 16)));
				}
			}
		}
	}
}

// Offsets, grids and scissors, including the ones whose edges leave the sixteen
// bits the routine works in (it must hand those calls over) and the ones that
// land exactly on its limits.
TEST(GsKickDecide, MatchesAcrossBoundsAndGrids)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());

	const int offsets[] = {0, 1, 15, 16, 27648, 32768, 49152, 65520, 65535};
	const u32 steps[] = {16, 32, 128, 2, 1, 0x8000, 0x10000, 0x20000};
	const int biases[] = {0, 1, 8, 15, 31};
	// Scissor edges in the biased window space: raw edge = edge + offset + bias.
	const int edges[] = {-70000, -32768, -17, -1, 0, 1, 16, 2000, 10240, 37887, 65535, 65536, 70000};

	for (const ClassDesc& c : kClasses)
	{
		std::mt19937 rng(2000 + DecideClass(c.n, c.keyed, c.every));
		const std::vector<u32> periods = Periods(c);
		for (u32 iter = 0; iter < 6000; iter++)
		{
			const int ofx = offsets[rng() % std::size(offsets)];
			const int ofy = offsets[rng() % std::size(offsets)];
			const int bias = biases[rng() % std::size(biases)];
			const u32 step = steps[rng() % std::size(steps)];
			int l = edges[rng() % std::size(edges)];
			int t = edges[rng() % std::size(edges)];
			int r = edges[rng() % std::size(edges)];
			int b = edges[rng() % std::size(edges)];
			if ((rng() & 3) != 0)
			{
				// Mostly a window something can be inside of.
				l = static_cast<int>(rng() % 4000) - 2000;
				t = static_cast<int>(rng() % 4000) - 2000;
				r = l + 1 + static_cast<int>(rng() % 30000);
				b = t + 1 + static_cast<int>(rng() % 30000);
			}
			if ((rng() & 7) == 0)
			{
				// An edge exactly at each limit of raw space.
				const int limit[] = {0, 1, 65535, 65536};
				const DecideBounds probe = ClassBounds(c, ofx, ofy, bias, step, 0, 0, 0, 0);
				l = limit[rng() & 3] - probe.ofx;
				r = limit[rng() & 3] - probe.ofx;
				t = limit[rng() & 3] - probe.ofy;
				b = limit[rng() & 3] - probe.ofy;
			}
			const DecideBounds kb = ClassBounds(c, ofx, ofy, bias, step, l, t, r, b);

			const u32 count = 1 + rng() % 128;
			const size_t stride = 16 * (1 + rng() % 4);
			const int mode = static_cast<int>(rng() % 4);
			const u16 cx = static_cast<u16>(kb.ofx + kb.l + static_cast<int>(rng() % 64) - 32);
			const u16 cy = static_cast<u16>(kb.ofy + kb.t + static_cast<int>(rng() % 64) - 32);
			const std::vector<Pos> v = RandomPositions(count, rng, mode, (rng() & 1) ? cx : static_cast<u16>(kb.ofx + kb.r),
				(rng() & 1) ? cy : static_cast<u16>(kb.ofy + kb.b));
			const u8* const p = rec.Fill(v, stride, rng);
			ASSERT_TRUE(Same(c, p, stride, count, periods[rng() % periods.size()], kb)) << "iteration " << iter;
		}
	}
}

// One box at a time, with its two sides at every alignment against the grid and
// as far apart as the coordinate space allows. A sixteen-bit test that compared
// the two sides' low bits alone would call a box 65,521 units wide narrow.
TEST(GsKickDecide, MatchesOnBoxesOfEveryWidthAndAlignment)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());

	std::vector<u16> lo, hi;
	for (u32 i = 0; i < 40; i++)
	{
		lo.push_back(static_cast<u16>(i));
		lo.push_back(static_cast<u16>(4096 + i));
		hi.push_back(static_cast<u16>(0xFFFF - i));
		hi.push_back(static_cast<u16>(4096 + i));
		hi.push_back(static_cast<u16>(4096 + 100 + i));
	}

	for (const ClassDesc& c : kClasses)
	{
		std::mt19937 rng(3000 + DecideClass(c.n, c.keyed, c.every));
		const u32 period = c.every ? 0u : ((c.n == 3) ? 0x924u : 0xAAAu);
		for (int of : {0, 1, 7, 16, 4090})
		{
			for (u32 step : {16u, 32u, 128u})
			{
				// The scissor is everything, so only the band test decides.
				const DecideBounds kb = ClassBounds(c, of, 5, 1, step, -70000, -70000, 70000, 70000);
				for (u16 a : lo)
				{
					for (u16 b : hi)
					{
						// A primitive whose X spans [a, b] and whose Y spans 1,000 units,
						// repeated: every vertex of a strip completes one like it.
						std::vector<Pos> v;
						for (u32 i = 0; i < 12; i++)
						{
							const u32 k = i % 3;
							v.push_back({(k == 0) ? a : ((k == 1) ? b : static_cast<u16>(a + (b - a) / 2)),
								static_cast<u16>(3000 + ((c.n == 2) ? (i & 1) : k) * 500), false});
						}
						if (c.n == 2)
						{
							for (u32 i = 0; i < 12; i++)
								v[i].x = (i & 1) ? b : a;
						}
						const u8* const p = rec.Fill(v, 16, rng);
						ASSERT_TRUE(Same(c, p, 16, 12, period, kb)) << "sides " << a << " and " << b;
					}
				}
			}
		}
	}
}

// Positions on each side of each scissor edge, one sub-texel at a time.
TEST(GsKickDecide, MatchesAtEveryScissorEdge)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());
	for (const ClassDesc& c : kClasses)
	{
		std::mt19937 rng(4000 + DecideClass(c.n, c.keyed, c.every));
		const u32 period = c.every ? 0u : ((c.n == 3) ? 0x924u : 0xAAAu);
		const DecideBounds kb = ClassBounds(c, 27648, 27648, 1, 16, 160, 320, 640 * 16, 448 * 16);
		for (int axis = 0; axis < 2; axis++)
		{
			const int near_edge = (axis == 0) ? (kb.ofx + kb.l) : (kb.ofy + kb.t);
			const int far_edge = (axis == 0) ? (kb.ofx + kb.r) : (kb.ofy + kb.b);
			for (int edge : {near_edge, far_edge})
			{
				for (int d = -40; d <= 40; d++)
				{
					// Every vertex within a few pixels of the edge on this axis and
					// spread over the window on the other.
					std::vector<Pos> v;
					for (u32 i = 0; i < 24; i++)
					{
						const u16 on = static_cast<u16>(edge + d - static_cast<int>(rng() % 48));
						const u16 across = static_cast<u16>(27648 + 1000 + (i % 3) * 700 + rng() % 16);
						v.push_back({(axis == 0) ? on : across, (axis == 0) ? across : on, false});
					}
					const u8* const p = rec.Fill(v, 48, rng);
					ASSERT_TRUE(Same(c, p, 48, 24, period, kb)) << "axis " << axis << " edge " << edge << " d " << d;
				}
			}
		}
	}
}

// The cases above are not all rejections or all acceptances: each class sees
// drawn primitives, each kind of rejection, and at least one hand-over.
TEST(GsKickDecide, CasesAreNotVacuous)
{
	GuardedRecords rec;
	ASSERT_TRUE(rec.ok());
	for (const ClassDesc& c : kClasses)
	{
		std::mt19937 rng(5000);
		const u32 period = c.every ? 0u : ((c.n == 3) ? 0x924u : 0xAAAu);
		const DecideBounds kb = ClassBounds(c, 27648, 27648, 1, 16, 0, 0, 640 * 16, 448 * 16);
		const DecideFn ref = Old(c);

		// A zigzag inside the window: drawn.
		std::vector<Pos> v;
		for (u32 i = 0; i < 24; i++)
			v.push_back({static_cast<u16>(27648 + 800 + i * 90 + ((i & 1) ? 400 : 0)), static_cast<u16>(27648 + 900 + ((i & 2) ? 500 : 0) + i * 30), false});
		const u8* p = rec.Fill(v, 16, rng);
		const Result drawn = RunDecide(ref, p, 16, 24, period, kb);
		EXPECT_NE(drawn.accept[0], 0ull) << c.name;
		EXPECT_TRUE(Same(c, p, 16, 24, period, kb));

		// The same with ADC everywhere: nothing.
		for (Pos& q : v)
			q.adc = true;
		p = rec.Fill(v, 16, rng);
		EXPECT_EQ(RunDecide(ref, p, 16, 24, period, kb).accept[0], 0ull) << c.name;
		EXPECT_TRUE(Same(c, p, 16, 24, period, kb));

		// A right edge past raw space: the routine cannot state it and must still agree.
		const DecideBounds wide = ClassBounds(c, 60000, 60000, 1, 16, 0, 0, 640 * 16, 448 * 16);
		for (Pos& q : v)
			q.adc = false;
		for (Pos& q : v)
		{
			q.x = static_cast<u16>(q.x + 60000 - 27648);
			q.y = static_cast<u16>(q.y + 60000 - 27648);
		}
		p = rec.Fill(v, 16, rng);
		EXPECT_NE(RunDecide(ref, p, 16, 24, period, wide).accept[0], 0ull) << c.name;
		EXPECT_TRUE(Same(c, p, 16, 24, period, wide));
	}
}

#endif // ARCH_ARM64
