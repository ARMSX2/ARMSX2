// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE FPR word <-> host double relocation (EeFpuFormat.h).
//
// Checked over all 2^32 words: one threaded pass fills the counters and each
// test reads one property out of it. The value law's reference is arithmetic,
// ldexp of the significand, rather than a second bit assembly.

#include "EeFpuFormat.h"

#include "common/Pcsx2Defs.h"

#if defined(__aarch64__)
#include "arm64/AsmHelpers.h"
#include "Config.h"
#include "Arm64JitBuffer.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
	// The EE value of a normal word, built by arithmetic.
	double EeValueOf(u32 word)
	{
		const u32 exp = (word >> 23) & 0xFF;
		const u32 man = word & 0x007FFFFF;
		const double v = std::ldexp(1.0 + static_cast<double>(man) / 8388608.0, static_cast<int>(exp) - 127);
		return (word & 0x80000000u) ? -v : v;
	}

	u32 DoubleExponentField(u64 bits) { return static_cast<u32>((bits >> 52) & 0x7FF); }
	u64 DoubleMantissaField(u64 bits) { return bits & 0x000FFFFFFFFFFFFFull; }

	// What one contiguous slice of the word space says about the format.
	struct RelocScan
	{
		u64 round_trip_failures = 0;
		u32 first_round_trip_failure = 0;
		u64 value_failures = 0;
		u32 first_value_failure = 0;
		u64 normals_checked = 0;
		u32 max_exponent_field = 0;
		u32 max_exponent_witness = 0;
		u64 denormal_mismatches = 0;
		u32 first_denormal_mismatch = 0;
		u64 denormals = 0;

		// Slices are merged in ascending word order, so the earlier slice's
		// first failure is the whole scan's.
		void Absorb(const RelocScan& later)
		{
			if (!round_trip_failures)
				first_round_trip_failure = later.first_round_trip_failure;
			if (!value_failures)
				first_value_failure = later.first_value_failure;
			if (!denormal_mismatches)
				first_denormal_mismatch = later.first_denormal_mismatch;
			round_trip_failures += later.round_trip_failures;
			value_failures += later.value_failures;
			denormal_mismatches += later.denormal_mismatches;
			normals_checked += later.normals_checked;
			denormals += later.denormals;
			if (later.max_exponent_field > max_exponent_field)
			{
				max_exponent_field = later.max_exponent_field;
				max_exponent_witness = later.max_exponent_witness;
			}
		}
	};

	RelocScan ScanWords(u64 begin, u64 end)
	{
		// 2^(e-127) for every EE exponent field, so the reference side of the
		// value law costs a multiply rather than a libm call.
		double pow2[256] = {};
		for (int e = 1; e <= 255; e++)
			pow2[e] = std::ldexp(1.0, e - 127);
		const double scale = std::ldexp(1.0, kEeFprScaleExp);

		RelocScan scan;
		for (u64 i = begin; i < end; i++)
		{
			const u32 word = static_cast<u32>(i);
			const u64 bits = eeFprWidenBits(word);

			if (eeFprNarrowBits(bits) != word)
			{
				if (!scan.round_trip_failures++)
					scan.first_round_trip_failure = word;
			}

			const u32 dexp = DoubleExponentField(bits);
			if (dexp > scan.max_exponent_field)
			{
				scan.max_exponent_field = dexp;
				scan.max_exponent_witness = word;
			}

			const u32 eexp = (word >> 23) & 0xFF;
			const u32 eman = word & 0x007FFFFF;
			const bool ee_denormal = (eexp == 0) && (eman != 0);
			const bool host_denormal = (dexp == 0) && (DoubleMantissaField(bits) != 0);
			if (ee_denormal != host_denormal)
			{
				if (!scan.denormal_mismatches++)
					scan.first_denormal_mismatch = word;
			}
			scan.denormals += ee_denormal ? 1 : 0;

			if (eexp == 0)
				continue; // no normal value to compare against

			scan.normals_checked++;
			double stored;
			std::memcpy(&stored, &bits, sizeof(stored));
			const double magnitude = pow2[eexp] * (1.0 + static_cast<double>(eman) / 8388608.0);
			const double reference = (word & 0x80000000u) ? -magnitude : magnitude;
			if (stored * scale != reference)
			{
				if (!scan.value_failures++)
					scan.first_value_failure = word;
			}
		}
		return scan;
	}
} // namespace

class EeFpuReloc : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
	{
		unsigned slices = std::thread::hardware_concurrency();
		slices = std::clamp(slices, 1u, 16u);
		const u64 per_slice = 0x100000000ull / slices;

		std::vector<RelocScan> results(slices);
		std::vector<std::thread> workers;
		for (unsigned i = 0; i < slices; i++)
		{
			const u64 begin = i * per_slice;
			const u64 end = (i + 1 == slices) ? 0x100000000ull : begin + per_slice;
			workers.emplace_back([i, begin, end, &results] { results[i] = ScanWords(begin, end); });
		}
		for (std::thread& worker : workers)
			worker.join();
		for (const RelocScan& slice : results)
			s_scan.Absorb(slice);
	}

	static inline RelocScan s_scan;
};

// CVT.W.S parks a raw int32 in an FPR slot, so the relocation has to be
// lossless on bit patterns that are not sensible floats too.
TEST_F(EeFpuReloc, EveryWordSurvivesTheRoundTrip)
{
	EXPECT_EQ(s_scan.round_trip_failures, 0ull)
		<< "first failing word 0x" << std::hex << s_scan.first_round_trip_failure;
}

// stored * 2^896 is the EE value the word denotes, uniformly.
TEST_F(EeFpuReloc, EveryNormalWordScalesToItsEeValue)
{
	EXPECT_EQ(s_scan.normals_checked, 4278190080ull) << "2^32 words less the 2^24 whose exponent field is 0";
	EXPECT_EQ(s_scan.value_failures, 0ull)
		<< "first failing word 0x" << std::hex << s_scan.first_value_failure;

	// Both ends of the range, readable without running the loop.
	EXPECT_EQ(std::ldexp(eeFprWiden(0x00800000), kEeFprScaleExp), EeValueOf(0x00800000));
	EXPECT_EQ(std::ldexp(eeFprWiden(0x7FFFFFFF), kEeFprScaleExp), EeValueOf(0x7FFFFFFF));
	EXPECT_EQ(std::ldexp(eeFprWiden(0xFFFFFFFF), kEeFprScaleExp), EeValueOf(0xFFFFFFFF));
}

// The EE's smallest normal has to land on the host's, or FPCR.FZ flushes on a
// different boundary than the EE does.
TEST_F(EeFpuReloc, TheDenormalBoundaryIsTheHostOne)
{
	EXPECT_EQ(eeFprWidenBits(0x00800000), 0x0010000000000000ull) << "2^-126 must map to 0x1p-1022";
	EXPECT_EQ(eeFprWidenBits(0x80800000), 0x8010000000000000ull);
	EXPECT_EQ(s_scan.denormal_mismatches, 0ull)
		<< "first mismatching word 0x" << std::hex << s_scan.first_denormal_mismatch;
	EXPECT_EQ(s_scan.denormals, 16777214ull) << "2 * (2^23 - 1) words are EE denormals; the class must not be empty";
}

// Exponent 255 is an ordinary binade on the EE, so the host's 2047 has to be
// out of reach by construction rather than by clamping.
TEST_F(EeFpuReloc, NoWordReachesTheHostInfinityExponent)
{
	EXPECT_EQ(s_scan.max_exponent_field, 255u);
	EXPECT_EQ(DoubleExponentField(eeFprWidenBits(s_scan.max_exponent_witness)), 255u)
		<< "witness 0x" << std::hex << s_scan.max_exponent_witness;
	EXPECT_EQ(DoubleExponentField(eeFprWidenBits(0x7F800000)), 255u);
	EXPECT_EQ(DoubleExponentField(eeFprWidenBits(0x7FFFFFFF)), 255u);
}

#if defined(__aarch64__)

using namespace vixl::aarch64;

namespace
{
	// Sign x exponent x a handful of mantissas, the interesting words, and a
	// pseudo-random tail. Every exponent field including 0 and 255 appears with
	// both signs.
	std::vector<u32> RelocSampleWords()
	{
		std::vector<u32> words = {
			0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,
			0x00800000, 0x80800000, 0x00800001, 0x3F800000, 0xBF800000, 0x3E800000,
			0x40490FDB, 0x3F490FDA, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000,
			0x7FC00000, 0xFFC00000, 0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFFFE, 0x00000002};

		static constexpr u32 kMantissas[] = {0, 1, 2, 0x2AA, 0x555, 0x400000, 0x7FFFFE, 0x7FFFFF};
		for (u32 sign = 0; sign < 2; sign++)
			for (u32 exp = 0; exp < 256; exp++)
				for (u32 man : kMantissas)
					words.push_back((sign << 31) | (exp << 23) | man);

		u32 state = 0x9E3779B9;
		for (int i = 0; i < 200000; i++)
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			words.push_back(state);
		}
		return words;
	}
} // namespace

// The emitted sequences have to agree with the C++ helpers word for word: the
// two sides meet at every boundary the architectural word is observable at.
TEST(EeFpuRelocEmit, BothDirectionsAgreeWithTheHelpers)
{
	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";
	u8* const base = static_cast<u8*>(buf.ptr());
	size_t used = 0;

	// u64 widen_reg(u32 word)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const widen_reg = armStartBlock();
	const ptrdiff_t widen_reg_start = armAsm->GetCursorOffset();
	armEmitEeFprWiden(d0, w0, x8);
	const ptrdiff_t widen_reg_bytes = armAsm->GetCursorOffset() - widen_reg_start;
	armAsm->Fmov(x0, d0);
	armAsm->Ret();
	used = static_cast<size_t>(armEndBlock() - base);

	// u64 widen_mem(const u32* word)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const widen_mem = armStartBlock();
	const ptrdiff_t widen_mem_start = armAsm->GetCursorOffset();
	armEmitEeFprWidenFromMem(d0, MemOperand(x0), x8);
	const ptrdiff_t widen_mem_bytes = armAsm->GetCursorOffset() - widen_mem_start;
	armAsm->Fmov(x0, d0);
	armAsm->Ret();
	used = static_cast<size_t>(armEndBlock() - base);

	// u64 narrow(u64 stored_bits)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const narrow = armStartBlock();
	armAsm->Fmov(d0, x0);
	const ptrdiff_t narrow_start = armAsm->GetCursorOffset();
	armEmitEeFprNarrow(x0, d0, x8);
	const ptrdiff_t narrow_bytes = armAsm->GetCursorOffset() - narrow_start;
	armAsm->Ret();
	used = static_cast<size_t>(armEndBlock() - base);

	// void narrow_mem(u64 stored_bits, u32* out)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const narrow_mem = armStartBlock();
	armAsm->Fmov(d0, x0);
	const ptrdiff_t narrow_mem_start = armAsm->GetCursorOffset();
	armEmitEeFprNarrowToMem(MemOperand(x1), d0, x2, x8);
	const ptrdiff_t narrow_mem_bytes = armAsm->GetCursorOffset() - narrow_mem_start;
	armAsm->Ret();
	armEndBlock();

	// A mask that stopped encoding as a logical immediate would be silently
	// materialized instead, so pin the widths.
	EXPECT_EQ(widen_reg_bytes, 3 * 4);
	EXPECT_EQ(widen_mem_bytes, 4 * 4);
	EXPECT_EQ(narrow_bytes, 3 * 4);
	EXPECT_EQ(narrow_mem_bytes, 4 * 4);

	const auto widen_reg_fn = reinterpret_cast<u64 (*)(u32)>(widen_reg);
	const auto widen_mem_fn = reinterpret_cast<u64 (*)(const u32*)>(widen_mem);
	const auto narrow_fn = reinterpret_cast<u64 (*)(u64)>(narrow);
	const auto narrow_mem_fn = reinterpret_cast<void (*)(u64, u32*)>(narrow_mem);

	const std::vector<u32> words = RelocSampleWords();
	u64 failures = 0;
	u32 first_failure = 0;
	for (const u32 word : words)
	{
		const u64 wide = eeFprWidenBits(word);
		u32 out = 0;
		narrow_mem_fn(wide, &out);
		const bool ok = widen_reg_fn(word) == wide && widen_mem_fn(&word) == wide &&
						narrow_fn(wide) == static_cast<u64>(word) && out == word;
		if (!ok && !failures++)
			first_failure = word;
	}
	EXPECT_EQ(failures, 0ull) << "first failing word 0x" << std::hex << first_failure
							  << std::dec << " of " << words.size();
}

// The slot pair is the same relocation with memory and register swapped, so it
// has its own encodings to pin and its own composition to check.
TEST(EeFpuRelocEmit, TheSlotPairIsTheIdentityThroughMemory)
{
	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";
	u8* const base = static_cast<u8*>(buf.ptr());
	size_t used = 0;

	// void store_slot(u32 word, u64* slot)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const store = armStartBlock();
	const ptrdiff_t store_start = armAsm->GetCursorOffset();
	armEmitEeFprStoreSlotWord(MemOperand(x1), w0, x8);
	const ptrdiff_t store_bytes = armAsm->GetCursorOffset() - store_start;
	armAsm->Ret();
	used = static_cast<size_t>(armEndBlock() - base);

	// u32 load_slot(const u64* slot)
	armSetAsmPtr(base + used, buf.size() - used, nullptr);
	u8* const load = armStartBlock();
	const ptrdiff_t load_start = armAsm->GetCursorOffset();
	armEmitEeFprLoadSlotWord(x0, MemOperand(x0), x8);
	const ptrdiff_t load_bytes = armAsm->GetCursorOffset() - load_start;
	armAsm->Ret();
	armEndBlock();

	EXPECT_EQ(store_bytes, 3 * 4);
	EXPECT_EQ(load_bytes, 3 * 4);

	const auto store_fn = reinterpret_cast<void (*)(u32, u64*)>(store);
	const auto load_fn = reinterpret_cast<u64 (*)(const u64*)>(load);

	const std::vector<u32> words = RelocSampleWords();
	u64 failures = 0;
	u32 first_failure = 0;
	for (const u32 word : words)
	{
		u64 slot = 0;
		store_fn(word, &slot);
		if ((slot != eeFprWidenBits(word) || load_fn(&slot) != static_cast<u64>(word)) && !failures++)
			first_failure = word;
	}
	EXPECT_EQ(failures, 0ull) << "first failing word 0x" << std::hex << first_failure
							  << std::dec << " of " << words.size();
}

// Underflow costs nothing here: FPCR.FZ flushes on the EE's own boundary as
// part of each op. Tested in emitted code, because a compiler folds constant
// arithmetic in the mode it was built with and not the one the msr installed.
TEST(EeFpuRelocEmit, FlushToZeroLandsOnTheEeDenormalBoundary)
{
	const u64 fpcr = Pcsx2Config::CpuOptions().FPUFPCR.bitmask;

	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";

	// u64 mul(u64 a_bits, u64 b_bits). Multiplying two stored values squares
	// the scale, so one operand is unscaled by 2^896 first.
	armSetAsmPtr(buf.ptr(), buf.size(), nullptr);
	u8* const code = armStartBlock();
	armAsm->Fmov(d0, x0);
	armAsm->Fmov(d1, x1);
	armAsm->Mov(x11, 0x77F0000000000000ull); // 2^896
	armAsm->Fmov(d2, x11);
	armAsm->Mrs(x9, FPCR);
	armAsm->Mov(x10, fpcr);
	armAsm->Msr(FPCR, x10);
	armAsm->Fmul(d0, d0, d2);
	armAsm->Fmul(d0, d0, d1);
	armAsm->Msr(FPCR, x9);
	armAsm->Fmov(x0, d0);
	armAsm->Ret();
	armEndBlock();

	const auto raw = reinterpret_cast<u64 (*)(u64, u64)>(code);
	const auto mul = [raw](u32 a, u32 b) {
		return eeFprNarrowBits(raw(eeFprWidenBits(a), eeFprWidenBits(b)));
	};

	// A denormal operand is gone before the multiply, sign kept.
	EXPECT_EQ(mul(0x00000001, 0x3F800000), 0x00000000u);
	EXPECT_EQ(mul(0x807FFFFF, 0x3F800000), 0x80000000u);

	// A product that underflows out of the EE's range goes the same way.
	EXPECT_EQ(mul(0x00800000, 0x3F000000), 0x00000000u); // 2^-126 * 0.5
	EXPECT_EQ(mul(0x80800000, 0x3F000000), 0x80000000u);

	// 2^-126 itself is a normal double here: the half of the boundary a flush
	// one binade too high would break.
	EXPECT_EQ(mul(0x00800000, 0x3F800000), 0x00800000u);
	EXPECT_EQ(mul(0x80800000, 0x3F800000), 0x80800000u);

	// The top of the EE's range multiplies as an ordinary number, where a host
	// single returns an infinity the fast path's clamp folds back to FLT_MAX.
	EXPECT_EQ(mul(0x7F7FFFFF, 0x40000000), 0x7FFFFFFFu);
	EXPECT_EQ(mul(0x7FFFFFFF, 0x3F800000), 0x7FFFFFFFu);
	EXPECT_EQ(mul(0x7F800000, 0x3F000000), 0x7F000000u); // 2^128 * 0.5
}

#endif // __aarch64__
