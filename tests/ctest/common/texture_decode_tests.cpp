// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// BC7 is pinned to the scalar decoder's output hash; JPEG is a round trip through the RGBA path.

#include "common/Image.h"
#include "common/TextureDecompress.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <random>

namespace
{
	TEST(TextureDecodeBC7, EveryModeDecodesToThePinnedScalarResult)
	{
		std::mt19937 rng(7);
		u64 hash = 0xcbf29ce484222325ull; // FNV-1a
		const auto mix = [&hash](u8 b) { hash = (hash ^ b) * 0x100000001b3ull; };
		for (u32 mode = 0; mode <= 8; mode++)
		{
			for (int i = 0; i < 4000; i++)
			{
				u8 block[16];
				for (u8& b : block)
					b = static_cast<u8>(rng() & 0xff);
				// Mode m is m zero bits then a one, from the bottom of the first byte; eight zeros is no mode.
				if (mode < 8)
					block[0] = static_cast<u8>((block[0] & ~((2u << mode) - 1)) | (1u << mode));
				else
					block[0] = 0;

				bc7decomp::color_rgba pixels[16];
				std::memset(pixels, 0xcd, sizeof(pixels));
				const bool ok = bc7decomp::unpack_bc7(block, pixels);
				mix(ok ? 1 : 0);
				const u8* const p = reinterpret_cast<const u8*>(pixels);
				for (size_t k = 0; k < sizeof(pixels); k++)
					mix(p[k]);
			}
		}
		EXPECT_EQ(hash, 0x28b8276f14ded103ull);
	}

	u32 Rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

	TEST(TextureDecodeJpeg, ARoundTripKeepsSizeChannelOrderAndOpaqueAlpha)
	{
		// Red on the left, blue on the right, a green ramp down the rows, and an alpha the JPEG cannot keep.
		constexpr u32 w = 96, h = 64;
		RGBA8Image source(w, h);
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++)
				source.GetRowPixels(y)[x] = (x < w / 2) ? Rgba(220, y * 3, 20, x) : Rgba(20, y * 3, 220, 255 - x);

		const auto jpeg = source.SaveToBuffer("round-trip.jpg", 95);
		ASSERT_TRUE(jpeg.has_value());
		ASSERT_GT(jpeg->size(), 0u);

		RGBA8Image decoded;
		ASSERT_TRUE(decoded.LoadFromBuffer("round-trip.jpg", jpeg->data(), jpeg->size()));
		ASSERT_EQ(decoded.GetWidth(), w);
		ASSERT_EQ(decoded.GetHeight(), h);

		for (u32 y = 0; y < h; y++)
		{
			for (u32 x = 0; x < w; x++)
			{
				const u32 got = decoded.GetRowPixels(y)[x];
				ASSERT_EQ(got >> 24, 0xffu) << "alpha at " << x << "," << y;
				// Away from the red/blue edge, which chroma subsampling blurs, every channel is close to the source.
				if (x + 4 >= w / 2 && x < w / 2 + 4)
					continue;
				const u32 want = source.GetRowPixels(y)[x];
				for (u32 c = 0; c < 24; c += 8)
				{
					const int d = static_cast<int>((got >> c) & 0xff) - static_cast<int>((want >> c) & 0xff);
					ASSERT_LE(std::abs(d), 12) << "channel " << c / 8 << " at " << x << "," << y << " got 0x" << std::hex
											   << got << " want 0x" << want;
				}
			}
		}
	}
} // namespace
