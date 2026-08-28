// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pins the raw .astc container parser (GS/Renderers/HW/GSTextureASTC.h) and the shared
// compressed-block geometry on GSTexture. Header-only helpers, so these ride the
// gs_vertex_tests target without extra linkage. The reference for every constant here is
// astcenc's astc_header/ASTC_MAGIC_ID and Arm's Docs/FileFormat.md in the upstream repo.

#include "GS/Renderers/HW/GSTextureASTC.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace
{
	// Builds a minimal valid 16-byte header for the given geometry.
	std::array<u8, ASTC::HEADER_SIZE> MakeHeader(u32 bw, u32 bh, u32 w, u32 h)
	{
		std::array<u8, ASTC::HEADER_SIZE> hdr = {};
		hdr[0] = 0x13;
		hdr[1] = 0xAB;
		hdr[2] = 0xA1;
		hdr[3] = 0x5C;
		hdr[4] = static_cast<u8>(bw);
		hdr[5] = static_cast<u8>(bh);
		hdr[6] = 1;
		hdr[7] = static_cast<u8>(w & 0xFF);
		hdr[8] = static_cast<u8>((w >> 8) & 0xFF);
		hdr[9] = static_cast<u8>((w >> 16) & 0xFF);
		hdr[10] = static_cast<u8>(h & 0xFF);
		hdr[11] = static_cast<u8>((h >> 8) & 0xFF);
		hdr[12] = static_cast<u8>((h >> 16) & 0xFF);
		hdr[13] = 1; // dim_z, little-endian 24-bit == 1
		return hdr;
	}
} // namespace

TEST(GSAstcParser, AcceptsAllFourteenStandardFootprints)
{
	const std::pair<GSTexture::Format, std::pair<u32, u32>> cases[] = {
		{GSTexture::Format::ASTC4x4, {4, 4}},
		{GSTexture::Format::ASTC5x4, {5, 4}},
		{GSTexture::Format::ASTC5x5, {5, 5}},
		{GSTexture::Format::ASTC6x5, {6, 5}},
		{GSTexture::Format::ASTC6x6, {6, 6}},
		{GSTexture::Format::ASTC8x5, {8, 5}},
		{GSTexture::Format::ASTC8x6, {8, 6}},
		{GSTexture::Format::ASTC8x8, {8, 8}},
		{GSTexture::Format::ASTC10x5, {10, 5}},
		{GSTexture::Format::ASTC10x6, {10, 6}},
		{GSTexture::Format::ASTC10x8, {10, 8}},
		{GSTexture::Format::ASTC10x10, {10, 10}},
		{GSTexture::Format::ASTC12x10, {12, 10}},
		{GSTexture::Format::ASTC12x12, {12, 12}},
	};

	for (const auto& [format, footprint] : cases)
	{
		const auto hdr = MakeHeader(footprint.first, footprint.second, 64, 64);
		ASTC::HeaderInfo info{};
		ASSERT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok) << footprint.first << 'x' << footprint.second;
		EXPECT_EQ(info.format, format);
		EXPECT_EQ(info.width, 64u);
		EXPECT_EQ(info.height, 64u);
		EXPECT_EQ(info.block_width, footprint.first);
		EXPECT_EQ(info.block_height, footprint.second);
	}
}

TEST(GSAstcParser, SmallWidthsAndHeights)
{
	for (const u32 dim : {1u, 3u, 4u, 5u})
	{
		const auto hdr = MakeHeader(4, 4, dim, dim);
		ASTC::HeaderInfo info{};
		ASSERT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok);

		u32 pitch = 0;
		u32 payload = 0;
		ASSERT_TRUE(ASTC::CalculatePayloadSize(info, &pitch, &payload));
		// Even a single texel occupies one full block row/column; 5 texels spans two.
		const u32 expected_blocks = (dim + 3) / 4;
		EXPECT_EQ(pitch, 16u * expected_blocks);
		EXPECT_EQ(payload, 16u * expected_blocks * expected_blocks);
	}
}

TEST(GSAstcParser, PinsOddNonSquareDimensions)
{
	// The canonical edge case from the work plan: not a multiple of either block axis.
	const auto hdr = MakeHeader(6, 5, 13, 11);
	ASTC::HeaderInfo info{};
	ASSERT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok);

	u32 pitch = 0;
	u32 payload = 0;
	ASSERT_TRUE(ASTC::CalculatePayloadSize(info, &pitch, &payload));
	EXPECT_EQ(pitch, 48u); // ceil(13/6) = 3 blocks * 16 bytes
	EXPECT_EQ(payload, 144u); // 3 * ceil(11/5) = 9 blocks total
	EXPECT_TRUE(ASTC::ValidateFileSize(info, 16 + 144));
}

TEST(GSAstcParser, RejectsBadMagic)
{
	auto hdr = MakeHeader(4, 4, 16, 16);
	hdr[3] = 0x5D; // flip one magic byte
	ASTC::HeaderInfo info{};
	EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::BadMagic);
}

TEST(GSAstcParser, RejectsTruncatedHeader)
{
	auto hdr = MakeHeader(4, 4, 16, 16);
	ASTC::HeaderInfo info{};
	EXPECT_EQ(ASTC::ParseHeader(hdr.data(), ASTC::HEADER_SIZE - 1, &info),
		ASTC::ParseResult::TruncatedHeader);
	EXPECT_EQ(ASTC::ParseHeader(nullptr, 0, &info), ASTC::ParseResult::TruncatedHeader);
}

TEST(GSAstcParser, RejectsZeroDimensions)
{
	ASTC::HeaderInfo info{};
	{
		const auto hdr = MakeHeader(4, 4, 0, 16);
		EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::BadImageDimensions);
	}
	{
		const auto hdr = MakeHeader(4, 4, 16, 0);
		EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::BadImageDimensions);
	}
}

TEST(GSAstcParser, RejectsNonStandardFootprints)
{
	ASTC::HeaderInfo info{};
	for (const auto [bw, bh] : {std::pair{3u, 3u}, std::pair{7u, 7u}, std::pair{12u, 12u - 1}, std::pair{4u, 8u}})
	{
		const auto hdr = MakeHeader(bw, bh, 16, 16);
		EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::BadBlockFootprint)
			<< bw << 'x' << bh;
	}
}

TEST(GSAstcParser, RejectsThreeDimensionalBlocksAndImages)
{
	ASTC::HeaderInfo info{};
	{
		auto hdr = MakeHeader(4, 4, 16, 16);
		hdr[6] = 2; // block_depth
		EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::NotTwoDimensional);
	}
	{
		auto hdr = MakeHeader(4, 4, 16, 16);
		hdr[13] = 2; // image_depth, low byte of the 24-bit field
		EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::NotTwoDimensional);
	}
}

TEST(GSAstcParser, RejectsDimensionsAboveDeviceLimit)
{
	const auto hdr = MakeHeader(4, 4, 9000, 10);
	ASTC::HeaderInfo info{};
	EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info, 8192), ASTC::ParseResult::TooLarge);
	// Without a limit (default) the same image parses.
	EXPECT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok);
}

TEST(GSAstcParser, RejectsOverflowingPayloadSizes)
{
	// Maximal legal 24-bit dimensions at the smallest footprint overflow a u32 payload.
	const auto hdr = MakeHeader(4, 4, 0xFFFFFF, 0xFFFFFF);
	ASTC::HeaderInfo info{};
	ASSERT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok);

	u32 pitch = 0;
	u32 payload = 0;
	EXPECT_FALSE(ASTC::CalculatePayloadSize(info, &pitch, &payload));
}

TEST(GSAstcParser, RejectsWrongFileSizes)
{
	const auto hdr = MakeHeader(4, 4, 16, 16);
	ASTC::HeaderInfo info{};
	ASSERT_EQ(ASTC::ParseHeader(hdr.data(), hdr.size(), &info), ASTC::ParseResult::Ok);

	// Exact size is 16 (header) + ceil(16/4)^2 = 16 blocks * 16 bytes = 272.
	EXPECT_TRUE(ASTC::ValidateFileSize(info, 272));
	EXPECT_FALSE(ASTC::ValidateFileSize(info, 271)); // truncated payload
	EXPECT_FALSE(ASTC::ValidateFileSize(info, 273)); // trailing payload bytes
	EXPECT_FALSE(ASTC::ValidateFileSize(info, -1)); // stat failure sentinel
}

TEST(GSAstcLoaders, ExtensionMatchIsExactAndCaseInsensitive)
{
	EXPECT_NE(GSTextureReplacements::GetLoader("texture.astc"), nullptr);
	EXPECT_NE(GSTextureReplacements::GetLoader("TEXTURE.ASTC"), nullptr);
	EXPECT_NE(GSTextureReplacements::GetLoader("a/b/c/texture.AsTc"), nullptr);

	// Regression pin: a prefix shorter than the registered extension used to match too,
	// so ".pngfoo" resolved to the PNG loader.
	EXPECT_EQ(GSTextureReplacements::GetLoader("texture.pngfoo"), nullptr);
	EXPECT_EQ(GSTextureReplacements::GetLoader("texture.ddsx"), nullptr);
	EXPECT_NE(GSTextureReplacements::GetLoader("texture.png"), nullptr);
	EXPECT_NE(GSTextureReplacements::GetLoader("texture.dds"), nullptr);
	EXPECT_EQ(GSTextureReplacements::GetLoader("texture.txt"), nullptr);
	EXPECT_EQ(GSTextureReplacements::GetLoader("no_extension"), nullptr);
}

TEST(GSTextureBlockGeometry, BlockInfoDescriptors)
{
	struct Case
	{
		GSTexture::Format format;
		GSTexture::BlockInfo bi;
	};
	const Case cases[] = {
		{GSTexture::Format::Color, {1, 1, 4}},
		{GSTexture::Format::UNorm8, {1, 1, 1}},
		{GSTexture::Format::BC1, {4, 4, 8}},
		{GSTexture::Format::BC7, {4, 4, 16}},
		{GSTexture::Format::ASTC4x4, {4, 4, 16}},
		{GSTexture::Format::ASTC5x4, {5, 4, 16}},
		{GSTexture::Format::ASTC6x5, {6, 5, 16}},
		{GSTexture::Format::ASTC8x6, {8, 6, 16}},
		{GSTexture::Format::ASTC10x10, {10, 10, 16}},
		{GSTexture::Format::ASTC12x10, {12, 10, 16}},
		{GSTexture::Format::ASTC12x12, {12, 12, 16}},
	};

	for (const Case& c : cases)
	{
		const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(c.format);
		EXPECT_EQ(bi.width, c.bi.width) << GSTexture::GetFormatName(c.format);
		EXPECT_EQ(bi.height, c.bi.height) << GSTexture::GetFormatName(c.format);
		EXPECT_EQ(bi.bytes, c.bi.bytes) << GSTexture::GetFormatName(c.format);
	}
}

TEST(GSTextureBlockGeometry, FormatClassificationHelpers)
{
	EXPECT_TRUE(GSTexture::IsCompressedFormat(GSTexture::Format::BC1));
	EXPECT_TRUE(GSTexture::IsCompressedFormat(GSTexture::Format::BC7));
	EXPECT_TRUE(GSTexture::IsCompressedFormat(GSTexture::Format::ASTC4x4));
	EXPECT_TRUE(GSTexture::IsCompressedFormat(GSTexture::Format::ASTC12x12));
	EXPECT_FALSE(GSTexture::IsCompressedFormat(GSTexture::Format::Color));
	EXPECT_FALSE(GSTexture::IsCompressedFormat(GSTexture::Format::Invalid));

	EXPECT_TRUE(GSTexture::IsASTCFormat(GSTexture::Format::ASTC4x4));
	EXPECT_TRUE(GSTexture::IsASTCFormat(GSTexture::Format::ASTC12x12));
	EXPECT_FALSE(GSTexture::IsASTCFormat(GSTexture::Format::BC7));
	EXPECT_FALSE(GSTexture::IsASTCFormat(GSTexture::Format::Color));

	EXPECT_TRUE(GSTexture::IsBlockCompressedFormat(GSTexture::Format::BC1));
	EXPECT_TRUE(GSTexture::IsBlockCompressedFormat(GSTexture::Format::ASTC6x5));
	EXPECT_FALSE(GSTexture::IsBlockCompressedFormat(GSTexture::Format::Color));
	EXPECT_FALSE(GSTexture::IsBlockCompressedFormat(GSTexture::Format::Invalid));
}

TEST(GSTextureBlockGeometry, UploadGeometryPinnedForBCAndAstc)
{
	// BC values must stay byte-for-byte identical to the pre-refactor calculations.
	EXPECT_EQ(GSTexture::CalcUploadPitch(GSTexture::Format::BC1, 13), 32u); // ceil(13/4)*8
	EXPECT_EQ(GSTexture::CalcUploadPitch(GSTexture::Format::BC7, 16), 64u); // exactly 4 blocks * 16
	EXPECT_EQ(GSTexture::CalcUploadSize(GSTexture::Format::BC1, 13, 32), 128u);
	EXPECT_EQ(GSTexture::CalcUploadRowLengthFromPitch(GSTexture::Format::BC1, 96), 48u);

	// Uncompressed passthrough.
	EXPECT_EQ(GSTexture::CalcUploadPitch(GSTexture::Format::Color, 10), 40u);
	EXPECT_EQ(GSTexture::CalcUploadSize(GSTexture::Format::Color, 10, 40), 400u);

	// Rectangular ASTC ceiling division (not AlignUpPow2): 5, 6, 10, 12 are not powers of two.
	EXPECT_EQ(GSTexture::CalcUploadPitch(GSTexture::Format::ASTC6x5, 13), 48u); // ceil(13/6)=3 * 16
	EXPECT_EQ(GSTexture::CalcUploadPitch(GSTexture::Format::ASTC6x5, 12), 32u); // exactly 2 blocks
	EXPECT_EQ(GSTexture::CalcUploadSize(GSTexture::Format::ASTC6x5, 11, 48), 144u);
	EXPECT_EQ(GSTexture::CalcUploadSize(GSTexture::Format::ASTC6x5, 10, 48), 96u); // 2 full block rows
	EXPECT_EQ(GSTexture::CalcUploadRowLengthFromPitch(GSTexture::Format::ASTC6x5, 48), 18u); // 3 blocks * 6 texels
}
