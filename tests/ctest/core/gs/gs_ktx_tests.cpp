// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSTextureKTX.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	void PutU32(std::vector<u8>& data, size_t offset, u32 value)
	{
		ASSERT_LE(offset + sizeof(value), data.size());
		data[offset + 0] = static_cast<u8>(value);
		data[offset + 1] = static_cast<u8>(value >> 8);
		data[offset + 2] = static_cast<u8>(value >> 16);
		data[offset + 3] = static_cast<u8>(value >> 24);
	}

	void AppendU32(std::vector<u8>& data, u32 value)
	{
		const size_t offset = data.size();
		data.resize(offset + sizeof(value));
		PutU32(data, offset, value);
	}

	void AppendKeyValue(std::vector<u8>& data, const char* key, const char* value)
	{
		const u32 entry_size = static_cast<u32>(std::strlen(key) + 1 + std::strlen(value) + 1);
		AppendU32(data, entry_size);
		data.insert(data.end(), key, key + std::strlen(key) + 1);
		data.insert(data.end(), value, value + std::strlen(value) + 1);
		data.resize((data.size() + 3) & ~size_t(3), 0);
	}

	std::vector<u8> MakeKTX(u32 internal_format, u32 width, u32 height, u32 mip_count,
		const char* orientation = "S=r,T=d")
	{
		std::vector<u8> metadata;
		if (orientation)
			AppendKeyValue(metadata, "KTXorientation", orientation);

		std::vector<u8> data(KTX::HEADER_SIZE, 0);
		std::memcpy(data.data(), KTX::IDENTIFIER, KTX::IDENTIFIER_SIZE);
		PutU32(data, 12, KTX::ENDIANNESS_MARKER);
		PutU32(data, 16, 0); // glType: compressed
		PutU32(data, 20, 1); // glTypeSize
		PutU32(data, 24, 0); // glFormat: compressed
		PutU32(data, 28, internal_format);
		PutU32(data, 32, KTX::GL_RGBA);
		PutU32(data, 36, width);
		PutU32(data, 40, height);
		PutU32(data, 44, 0); // depth
		PutU32(data, 48, 0); // array elements
		PutU32(data, 52, 1); // faces
		PutU32(data, 56, mip_count);
		PutU32(data, 60, static_cast<u32>(metadata.size()));
		data.insert(data.end(), metadata.begin(), metadata.end());

		GSTexture::Format format = GSTexture::Format::Invalid;
		if (!KTX::LookupInternalFormat(internal_format, &format))
			return data;

		KTX::ContainerInfo info{format, width, height, mip_count, GSTexture::GetBlockInfo(format),
			static_cast<u64>(data.size()), static_cast<u32>(metadata.size())};
		for (u32 level = 0; level < mip_count; level++)
		{
			KTX::LevelInfo level_info{};
			if (!KTX::CalculateLevelGeometry(info, level, &level_info))
				break;
			AppendU32(data, level_info.payload_size);
			data.resize(data.size() + level_info.payload_size, static_cast<u8>(level + 1));
		}
		return data;
	}

	KTX::ParseResult ParseFixture(const std::vector<u8>& data, KTX::ContainerInfo* info,
		std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS>* levels)
	{
		KTX::ParseResult result = KTX::ParseHeader(data.data(), data.size(), info);
		if (result != KTX::ParseResult::Ok)
			return result;
		result = KTX::ValidateKeyValueData(data.data() + KTX::HEADER_SIZE, info->key_value_bytes);
		if (result != KTX::ParseResult::Ok)
			return result;
		result = KTX::BuildLevelLayout(*info, static_cast<s64>(data.size()), levels);
		if (result != KTX::ParseResult::Ok)
			return result;
		for (u32 level = 0; level < info->mip_count; level++)
		{
			result = KTX::ValidateLevelSize(
				data.data() + (*levels)[level].data_offset - sizeof(u32), sizeof(u32), (*levels)[level]);
			if (result != KTX::ParseResult::Ok)
				return result;
		}
		return KTX::ParseResult::Ok;
	}

	class TempKTXFile
	{
	public:
		explicit TempKTXFile(const std::vector<u8>& data)
		{
			static std::atomic<u32> counter{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = std::filesystem::temp_directory_path() /
				("armsx2-ktx-test-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".ktx");

			std::ofstream file(m_path, std::ios::binary);
			file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
			m_valid = file.good();
		}

		~TempKTXFile()
		{
			std::error_code error;
			std::filesystem::remove(m_path, error);
		}

		bool IsValid() const { return m_valid; }
		std::string Path() const { return m_path.string(); }

	private:
		std::filesystem::path m_path;
		bool m_valid = false;
	};
} // namespace

TEST(GSKtxParser, AcceptsAllLinearAstcFootprints)
{
	const std::pair<u32, GSTexture::Format> cases[] = {
		{KTX::FORMAT_ASTC_4X4, GSTexture::Format::ASTC4x4},
		{KTX::FORMAT_ASTC_5X4, GSTexture::Format::ASTC5x4},
		{KTX::FORMAT_ASTC_5X5, GSTexture::Format::ASTC5x5},
		{KTX::FORMAT_ASTC_6X5, GSTexture::Format::ASTC6x5},
		{KTX::FORMAT_ASTC_6X6, GSTexture::Format::ASTC6x6},
		{KTX::FORMAT_ASTC_8X5, GSTexture::Format::ASTC8x5},
		{KTX::FORMAT_ASTC_8X6, GSTexture::Format::ASTC8x6},
		{KTX::FORMAT_ASTC_8X8, GSTexture::Format::ASTC8x8},
		{KTX::FORMAT_ASTC_10X5, GSTexture::Format::ASTC10x5},
		{KTX::FORMAT_ASTC_10X6, GSTexture::Format::ASTC10x6},
		{KTX::FORMAT_ASTC_10X8, GSTexture::Format::ASTC10x8},
		{KTX::FORMAT_ASTC_10X10, GSTexture::Format::ASTC10x10},
		{KTX::FORMAT_ASTC_12X10, GSTexture::Format::ASTC12x10},
		{KTX::FORMAT_ASTC_12X12, GSTexture::Format::ASTC12x12},
	};

	for (const auto& [internal_format, expected_format] : cases)
	{
		const std::vector<u8> data = MakeKTX(internal_format, 13, 11, 1);
		KTX::ContainerInfo info{};
		std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS> levels{};
		ASSERT_EQ(ParseFixture(data, &info, &levels), KTX::ParseResult::Ok) << internal_format;
		EXPECT_EQ(info.format, expected_format);
		EXPECT_EQ(levels[0].width, 13u);
		EXPECT_EQ(levels[0].height, 11u);
		EXPECT_EQ(KTX::ValidateLevelSize(data.data() + levels[0].data_offset - sizeof(u32), sizeof(u32), levels[0]),
			KTX::ParseResult::Ok);
	}
}

TEST(GSKtxParser, PinsSevenLevelOddChain)
{
	const std::vector<u8> data = MakeKTX(KTX::FORMAT_ASTC_6X5, 64, 33, 7);
	KTX::ContainerInfo info{};
	std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS> levels{};
	ASSERT_EQ(ParseFixture(data, &info, &levels), KTX::ParseResult::Ok);
	EXPECT_EQ(info.mip_count, 7u);
	EXPECT_EQ(levels[0].pitch, 176u); // ceil(64/6) * 16
	EXPECT_EQ(levels[0].payload_size, 1232u); // seven 5-texel block rows
	EXPECT_EQ(levels[6].width, 1u);
	EXPECT_EQ(levels[6].height, 1u);
	EXPECT_EQ(levels[6].pitch, 16u);
	EXPECT_EQ(levels[6].payload_size, 16u);
}

TEST(GSKtxParser, RejectsMalformedHeaders)
{
	KTX::ContainerInfo info{};
	auto data = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1);
	EXPECT_EQ(KTX::ParseHeader(data.data(), KTX::HEADER_SIZE - 1, &info), KTX::ParseResult::TruncatedHeader);

	auto check = [&data, &info](size_t offset, u32 value, KTX::ParseResult expected) {
		auto modified = data;
		PutU32(modified, offset, value);
		EXPECT_EQ(KTX::ParseHeader(modified.data(), modified.size(), &info), expected);
	};

	auto bad_identifier = data;
	bad_identifier[0] ^= 1;
	EXPECT_EQ(KTX::ParseHeader(bad_identifier.data(), bad_identifier.size(), &info), KTX::ParseResult::BadIdentifier);
	check(12, 0x01020304, KTX::ParseResult::BadEndianness);
	check(16, 1, KTX::ParseResult::BadTypeFields);
	check(20, 4, KTX::ParseResult::BadTypeFields);
	check(24, KTX::GL_RGBA, KTX::ParseResult::BadTypeFields);
	check(32, 0, KTX::ParseResult::BadBaseFormat);
	check(28, 0x93D4, KTX::ParseResult::BadInternalFormat); // sRGB ASTC 6x6
	check(44, 1, KTX::ParseResult::BadTextureType);
	check(48, 1, KTX::ParseResult::BadTextureType);
	check(52, 6, KTX::ParseResult::BadTextureType);
	check(36, 0, KTX::ParseResult::BadDimensions);
	check(40, 0, KTX::ParseResult::BadDimensions);
	check(56, 0, KTX::ParseResult::BadMipCount);
	check(56, 8, KTX::ParseResult::BadMipCount);
	check(60, KTX::MAX_KEY_VALUE_BYTES + 1, KTX::ParseResult::BadKeyValueData);

	auto impossible_mips = MakeKTX(KTX::FORMAT_ASTC_6X6, 4, 4, 4);
	EXPECT_EQ(KTX::ParseHeader(impossible_mips.data(), impossible_mips.size(), &info), KTX::ParseResult::BadMipCount);

	PutU32(data, 36, 9000);
	EXPECT_EQ(KTX::ParseHeader(data.data(), data.size(), &info, 8192), KTX::ParseResult::TooLarge);
}

TEST(GSKtxParser, RejectsMalformedOrientationMetadata)
{
	KTX::ContainerInfo info{};

	auto missing = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1, nullptr);
	ASSERT_EQ(KTX::ParseHeader(missing.data(), missing.size(), &info), KTX::ParseResult::Ok);
	EXPECT_EQ(KTX::ValidateKeyValueData(missing.data() + KTX::HEADER_SIZE, info.key_value_bytes),
		KTX::ParseResult::BadOrientation);

	auto flipped = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1, "S=r,T=u");
	ASSERT_EQ(KTX::ParseHeader(flipped.data(), flipped.size(), &info), KTX::ParseResult::Ok);
	EXPECT_EQ(KTX::ValidateKeyValueData(flipped.data() + KTX::HEADER_SIZE, info.key_value_bytes),
		KTX::ParseResult::BadOrientation);

	auto duplicate = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1);
	std::vector<u8> second;
	AppendKeyValue(second, "KTXorientation", "S=r,T=d");
	duplicate.insert(duplicate.begin() + KTX::HEADER_SIZE + KTX::ReadU32(duplicate.data() + 60), second.begin(), second.end());
	PutU32(duplicate, 60, KTX::ReadU32(duplicate.data() + 60) + static_cast<u32>(second.size()));
	ASSERT_EQ(KTX::ParseHeader(duplicate.data(), duplicate.size(), &info), KTX::ParseResult::Ok);
	EXPECT_EQ(KTX::ValidateKeyValueData(duplicate.data() + KTX::HEADER_SIZE, info.key_value_bytes),
		KTX::ParseResult::BadOrientation);

	auto bad_length = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1);
	PutU32(bad_length, KTX::HEADER_SIZE, 0xFFFFFFFF);
	ASSERT_EQ(KTX::ParseHeader(bad_length.data(), bad_length.size(), &info), KTX::ParseResult::Ok);
	EXPECT_EQ(KTX::ValidateKeyValueData(bad_length.data() + KTX::HEADER_SIZE, info.key_value_bytes),
		KTX::ParseResult::BadKeyValueData);

	auto bad_padding = MakeKTX(KTX::FORMAT_ASTC_6X6, 16, 16, 1);
	ASSERT_EQ(KTX::ReadU32(bad_padding.data() + 60), 28u);
	bad_padding[KTX::HEADER_SIZE + 27] = 1;
	ASSERT_EQ(KTX::ParseHeader(bad_padding.data(), bad_padding.size(), &info), KTX::ParseResult::Ok);
	EXPECT_EQ(KTX::ValidateKeyValueData(bad_padding.data() + KTX::HEADER_SIZE, info.key_value_bytes),
		KTX::ParseResult::BadKeyValueData);

	std::vector<u8> empty_key_metadata;
	AppendU32(empty_key_metadata, 2);
	empty_key_metadata.push_back(0);
	empty_key_metadata.push_back(0);
	empty_key_metadata.resize(8, 0);
	EXPECT_EQ(KTX::ValidateKeyValueData(empty_key_metadata.data(), empty_key_metadata.size()),
		KTX::ParseResult::BadKeyValueData);
}

TEST(GSKtxParser, RejectsMalformedLevelLayout)
{
	const auto valid = MakeKTX(KTX::FORMAT_ASTC_6X6, 17, 13, 3);
	KTX::ContainerInfo info{};
	std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS> levels{};
	ASSERT_EQ(ParseFixture(valid, &info, &levels), KTX::ParseResult::Ok);

	auto bad_size = valid;
	PutU32(bad_size, levels[1].data_offset - sizeof(u32), levels[1].payload_size + 16);
	EXPECT_EQ(KTX::ValidateLevelSize(bad_size.data() + levels[1].data_offset - sizeof(u32), sizeof(u32), levels[1]),
		KTX::ParseResult::BadImageSize);

	EXPECT_EQ(KTX::BuildLevelLayout(info, static_cast<s64>(valid.size() - 1), &levels),
		KTX::ParseResult::TruncatedLevel);
	EXPECT_EQ(KTX::BuildLevelLayout(info, static_cast<s64>(valid.size() + 1), &levels),
		KTX::ParseResult::TrailingBytes);
	EXPECT_EQ(KTX::BuildLevelLayout(info, -1, &levels), KTX::ParseResult::TruncatedLevel);
}

TEST(GSKtxParser, RejectsGeometryOverflow)
{
	const auto data = MakeKTX(KTX::FORMAT_ASTC_4X4, 0xFFFFFFFFu, 0xFFFFFFFFu, 1);
	KTX::ContainerInfo info{};
	ASSERT_EQ(KTX::ParseHeader(data.data(), data.size(), &info), KTX::ParseResult::Ok);
	std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS> levels{};
	EXPECT_EQ(KTX::BuildLevelLayout(info, static_cast<s64>(data.size()), &levels), KTX::ParseResult::BadImageSize);
}

TEST(GSKtxLoader, LoadsCompleteChain)
{
	const TempKTXFile file(MakeKTX(KTX::FORMAT_ASTC_6X6, 17, 13, 3));
	ASSERT_TRUE(file.IsValid());

	GSTextureReplacements::ReplacementTexture texture;
	ASSERT_TRUE(GSTextureReplacements::LoadKTXTexture(file.Path(), &texture, 16384));
	EXPECT_EQ(texture.format, GSTexture::Format::ASTC6x6);
	EXPECT_EQ(texture.width, 17u);
	EXPECT_EQ(texture.height, 13u);
	EXPECT_EQ(texture.pitch, 48u);
	ASSERT_EQ(texture.data.size(), 144u);
	EXPECT_EQ(texture.data.front(), 1u);

	ASSERT_EQ(texture.mips.size(), 2u);
	EXPECT_EQ(texture.mips[0].width, 8u);
	EXPECT_EQ(texture.mips[0].height, 6u);
	EXPECT_EQ(texture.mips[0].pitch, 32u);
	ASSERT_EQ(texture.mips[0].data.size(), 32u);
	EXPECT_EQ(texture.mips[0].data.front(), 2u);
	EXPECT_EQ(texture.mips[1].width, 4u);
	EXPECT_EQ(texture.mips[1].height, 3u);
	EXPECT_EQ(texture.mips[1].pitch, 16u);
	ASSERT_EQ(texture.mips[1].data.size(), 16u);
	EXPECT_EQ(texture.mips[1].data.front(), 3u);
}

TEST(GSKtxLoader, LaterInvalidLevelLeavesOutputUntouched)
{
	auto data = MakeKTX(KTX::FORMAT_ASTC_6X6, 17, 13, 3);
	KTX::ContainerInfo info{};
	std::array<KTX::LevelInfo, KTX::MAX_MIP_LEVELS> levels{};
	ASSERT_EQ(ParseFixture(data, &info, &levels), KTX::ParseResult::Ok);
	PutU32(data, levels[2].data_offset - sizeof(u32), levels[2].payload_size + 16);

	const TempKTXFile file(data);
	ASSERT_TRUE(file.IsValid());
	GSTextureReplacements::ReplacementTexture texture;
	texture.width = 99;
	texture.height = 77;
	texture.data = {4, 5, 6};

	EXPECT_FALSE(GSTextureReplacements::LoadKTXTexture(file.Path(), &texture, 16384));
	EXPECT_EQ(texture.width, 99u);
	EXPECT_EQ(texture.height, 77u);
	EXPECT_EQ(texture.data, (std::vector<u8>{4, 5, 6}));
	EXPECT_TRUE(texture.mips.empty());
}
