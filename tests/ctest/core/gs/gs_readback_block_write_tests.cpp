// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The block readback write-back must leave GS memory exactly as the old per-pixel loop did.

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

namespace
{
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMZ32, PSMZ24};
	constexpr u32 kPsms16[] = {PSMCT16, PSMCT16S, PSMZ16, PSMZ16S};
	constexpr u32 kMasks[] = {0xffffffffu, 0x00ffffffu, 0x0fffffffu, 0xf0ffffffu, 0xff000000u};

	class GSReadbackBlockWrite : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_got = std::make_unique<GSLocalMemory>();
			m_want = std::make_unique<GSLocalMemory>();
			m_rng.seed(2026);
		}

		u32 Random(u32 n) { return static_cast<u32>(m_rng() % n); }

		void FillBoth()
		{
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 4)
			{
				const u32 v = static_cast<u32>(m_rng());
				std::memcpy(m_got->m_vm8 + i, &v, 4);
			}
			std::memcpy(m_want->m_vm8, m_got->m_vm8, GSLocalMemory::m_vmsize);
		}

		/// One readback of `r` into a buffer at `bp` with width `bw`. `skew` bytes put the source off 16-byte alignment and
		/// `slack` bytes are added to each row's pitch. `mask` 0 means the overload without a mask.
		void Check(u32 psm, u32 bp, u32 bw, const GSVector4i& r, u32 mask, u32 skew, u32 slack)
		{
			const u32 pitch = static_cast<u32>(r.width()) * 4 + slack;
			std::vector<u8> buffer(skew + static_cast<size_t>(pitch) * r.height() + 16);
			for (u8& b : buffer)
				b = static_cast<u8>(m_rng());
			u8* const src = buffer.data() + skew;

			const GSOffset got_off = m_got->GetOffset(bp, bw, psm);
			if (mask == 0)
				m_got->WritePixel32(src, pitch, got_off, r);
			else
				m_got->WritePixel32(src, pitch, got_off, r, mask);

			const u32 m = mask ? mask : 0xffffffffu;
			m_want->GetOffset(bp, bw, psm).loopPixels(r, m_want->vm32(), reinterpret_cast<u32*>(src), static_cast<int>(pitch),
				[&](u32* d, u32* s) { *d = (*d & ~m) | (*s & m); });

			const u32* const got = m_got->vm32();
			const u32* const want = m_want->vm32();
			for (u32 i = 0; i < GSLocalMemory::m_vmsize / 4; i++)
			{
				ASSERT_EQ(got[i], want[i]) << "word " << i << " psm 0x" << std::hex << psm << " mask 0x" << mask << std::dec
										   << " bp " << bp << " bw " << bw << " rect " << r.left << "," << r.top << " "
										   << r.right << "," << r.bottom << " skew " << skew << " slack " << slack;
			}
		}

		/// The same for WritePixel16, which has no mask.
		void Check16(u32 psm, u32 bp, u32 bw, const GSVector4i& r, u32 skew, u32 slack)
		{
			const u32 pitch = static_cast<u32>(r.width()) * 2 + slack;
			std::vector<u8> buffer(skew + static_cast<size_t>(pitch) * r.height() + 16);
			for (u8& b : buffer)
				b = static_cast<u8>(m_rng());
			u8* const src = buffer.data() + skew;

			m_got->WritePixel16(src, pitch, m_got->GetOffset(bp, bw, psm), r);
			m_want->GetOffset(bp, bw, psm).loopPixels(r, m_want->vm16(), reinterpret_cast<u16*>(src), static_cast<int>(pitch),
				[](u16* d, u16* s) { *d = *s; });

			const u32* const got = m_got->vm32();
			const u32* const want = m_want->vm32();
			for (u32 i = 0; i < GSLocalMemory::m_vmsize / 4; i++)
			{
				ASSERT_EQ(got[i], want[i]) << "word " << i << " psm 0x" << std::hex << psm << std::dec << " bp " << bp
										   << " bw " << bw << " rect " << r.left << "," << r.top << " " << r.right << ","
										   << r.bottom << " skew " << skew << " slack " << slack;
			}
		}

		std::unique_ptr<GSLocalMemory> m_got;
		std::unique_ptr<GSLocalMemory> m_want;
		std::mt19937 m_rng;
	};

	TEST_F(GSReadbackBlockWrite, AWholeAlignedTargetMatchesThePixelLoop)
	{
		FillBoth();
		for (const u32 psm : kPsms)
		{
			for (const u32 mask : kMasks)
				Check(psm, 0, 10, GSVector4i(0, 0, 640, 448), mask, 0, 0);
			Check(psm, 0x1a40, 10, GSVector4i(0, 0, 640, 448), 0, 0, 0);
		}
		for (const u32 psm : kPsms16)
			Check16(psm, 0, 10, GSVector4i(0, 0, 640, 448), 0, 0);
	}

	TEST_F(GSReadbackBlockWrite, UnalignedAndSubBlockRectanglesMatchThePixelLoop)
	{
		FillBoth();
		const GSVector4i rects[] = {
			GSVector4i(3, 5, 6, 7),       // inside one block
			GSVector4i(1, 1, 15, 15),     // no whole block inside
			GSVector4i(0, 0, 8, 8),       // exactly one block
			GSVector4i(7, 7, 17, 17),     // one whole block, edges all round
			GSVector4i(8, 3, 64, 12),     // one row of blocks, rows above and below
			GSVector4i(5, 16, 13, 80),    // one column of blocks, columns either side
			GSVector4i(61, 29, 131, 99),  // across page edges
			GSVector4i(0, 0, 1, 448),     // one column of pixels
			GSVector4i(0, 447, 640, 448), // one row of pixels
		};
		for (const u32 psm : kPsms)
			for (const GSVector4i& r : rects)
				for (const u32 mask : {0xffffffffu, 0x00ffffffu, 0u})
					Check(psm, 0x340, 10, r, mask, 4, 12);
		for (const u32 psm : kPsms16)
			for (const GSVector4i& r : rects)
				Check16(psm, 0x340, 10, r, 2, 6);
		// 16-bit blocks are 16 pixels wide: one that is 8-aligned but not 16-aligned has no whole block inside.
		for (const u32 psm : kPsms16)
			Check16(psm, 0x340, 10, GSVector4i(8, 8, 40, 24), 0, 0);
	}

	TEST_F(GSReadbackBlockWrite, RandomRectanglesMatchThePixelLoop)
	{
		FillBoth();
		for (int round = 0; round < 600; round++)
		{
			const bool sixteen = (round % 4) == 3;
			const u32 psm = sixteen ? kPsms16[Random(std::size(kPsms16))] : kPsms[Random(std::size(kPsms))];
			// A buffer is bw * 64 pixels wide in the 16-bit formats too.
			const u32 bw = 1 + Random(16);
			const u32 width = bw * 64;
			const int left = static_cast<int>(Random(width));
			const int top = static_cast<int>(Random(512));
			const int right = left + 1 + static_cast<int>(Random(width - left));
			const int bottom = top + 1 + static_cast<int>(Random(160));
			const u32 mask = (round % 3 == 0) ? 0xffffffffu : (round % 3 == 1) ? 0x00ffffffu : kMasks[Random(std::size(kMasks))];
			const u32 bp = (round & 1) ? Random(0x4000) : (Random(0x4000) & ~0x1fu);
			if (sixteen)
				Check16(psm, bp, bw, GSVector4i(left, top, right, bottom), 2 * Random(8), 2 * Random(5));
			else
				Check(psm, bp, bw, GSVector4i(left, top, right, bottom), mask, 4 * Random(4), 4 * Random(5));
			if (::testing::Test::HasFatalFailure())
				return;
		}
	}

	TEST_F(GSReadbackBlockWrite, ARectangleWiderThanTheBufferKeepsRowOrder)
	{
		FillBoth();
		// Past the page row width, pixel (64 + k, y) is the same memory as (k, y + page height), so which
		// value survives depends on the order the rows are written in.
		const GSVector4i r(3, 0, 70, 80);
		for (const u32 psm : kPsms)
			for (const u32 mask : {0xffffffffu, 0x00ffffffu})
				Check(psm, 0, 1, r, mask, 0, 0);
		for (const u32 psm : kPsms16)
			Check16(psm, 0, 1, r, 0, 0);
	}

	TEST_F(GSReadbackBlockWrite, ABufferThatWrapsTheEndOfMemoryMatchesThePixelLoop)
	{
		FillBoth();
		// The last few pages, then on round to the start of GS memory.
		for (const u32 psm : kPsms)
			for (const u32 mask : {0xffffffffu, 0x00ffffffu})
				Check(psm, 0x3fe0 - 32 * 4, 10, GSVector4i(4, 0, 636, 256), mask, 0, 0);
		for (const u32 psm : kPsms16)
			Check16(psm, 0x3fe0 - 32 * 2, 10, GSVector4i(4, 0, 636, 256), 0, 0);
	}
} // namespace
