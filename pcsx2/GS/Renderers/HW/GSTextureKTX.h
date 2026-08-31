// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <limits>

/// Strict KTX1 parsing for two-dimensional ASTC LDR UNORM replacement textures, kept
/// free of device policy: no g_gs_device access, no logging, and no allocation from
/// untrusted metadata, so it can be pinned directly by host tests.
///
/// The accepted contract is intentionally narrow: little-endian KTX1, one 2D
/// face, no arrays or depth, any of the 14 standard linear `GL_COMPRESSED_RGBA_ASTC_*_KHR`
/// internal formats, one through seven explicit mip levels, and `KTXorientation` equal
/// to `S=r,T=d`. Anything else is rejected.
///
/// This intentionally mirrors the raw `.astc` parser in GSTextureASTC.h: explicit byte
/// reads, never a cast onto a packed structure.
namespace KTX
{
	static constexpr u32 IDENTIFIER_SIZE = 12;
	static constexpr u32 HEADER_FIELD_COUNT = 13; // u32 fields after the identifier
	static constexpr u32 HEADER_SIZE = IDENTIFIER_SIZE + HEADER_FIELD_COUNT * 4;
	static constexpr u32 MAX_MIP_LEVELS = 7; // PS2 exposes base plus six mips
	static constexpr u32 GL_RGBA = 0x1908;
	static constexpr u32 ENDIANNESS_MARKER = 0x04030201;
	// Defense in depth for the untrusted key/value blob: real packs carry a few
	// dozen bytes of orientation/channels metadata.
	static constexpr u32 MAX_KEY_VALUE_BYTES = 1024 * 1024;

	static constexpr u8 IDENTIFIER[IDENTIFIER_SIZE] = {
		0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};

	// GL_COMPRESSED_RGBA_ASTC_*_KHR internal format enums. The sRGB twins (0x93D0..)
	// are deliberately absent: the replacement pipeline samples linear UNORM.
	constexpr u32 FORMAT_ASTC_4X4 = 0x93B0;
	constexpr u32 FORMAT_ASTC_5X4 = 0x93B1;
	constexpr u32 FORMAT_ASTC_5X5 = 0x93B2;
	constexpr u32 FORMAT_ASTC_6X5 = 0x93B3;
	constexpr u32 FORMAT_ASTC_6X6 = 0x93B4;
	constexpr u32 FORMAT_ASTC_8X5 = 0x93B5;
	constexpr u32 FORMAT_ASTC_8X6 = 0x93B6;
	constexpr u32 FORMAT_ASTC_8X8 = 0x93B7;
	constexpr u32 FORMAT_ASTC_10X5 = 0x93B8;
	constexpr u32 FORMAT_ASTC_10X6 = 0x93B9;
	constexpr u32 FORMAT_ASTC_10X8 = 0x93BA;
	constexpr u32 FORMAT_ASTC_10X10 = 0x93BB;
	constexpr u32 FORMAT_ASTC_12X10 = 0x93BC;
	constexpr u32 FORMAT_ASTC_12X12 = 0x93BD;

	enum class ParseResult
	{
		Ok,
		BadIdentifier,
		BadEndianness, // big-endian container
		BadTypeFields, // glType/glFormat/glTypeSize not a compressed texture
		BadBaseFormat, // glBaseInternalFormat != GL_RGBA
		BadInternalFormat, // not one of the 14 linear ASTC formats
		BadTextureType, // 3D, array, or cube
		BadDimensions, // zero width or height
		BadMipCount, // zero (generate sentinel), above the geometric chain, or above 7
		BadKeyValueData, // malformed or oversized metadata
		BadOrientation, // missing, duplicated, or incompatible KTXorientation
		TruncatedHeader, // fewer than the fixed header bytes available
		TruncatedLevel, // level record runs past EOF
		BadImageSize, // imageSize disagrees with ASTC block geometry
		TrailingBytes, // data past the last mip record
		TooLarge, // dimension above the supplied device limit
	};

	struct LevelInfo
	{
		u32 width;
		u32 height;
		u32 pitch;
		u32 payload_size;
		/// Offset of the mip payload from the start of the container.
		u64 data_offset;
	};

	struct ContainerInfo
	{
		GSTexture::Format format;
		u32 width;
		u32 height;
		u32 mip_count;
		GSTexture::BlockInfo block_info;
		/// Offset of mip level 0's record (after the header and key/value data).
		u64 mips_offset;
		u32 key_value_bytes;
	};

	/// Dimensions of mip [level] of a [base]-sized image edge.
	inline u32 MipDimension(u32 base, u32 level)
	{
		return std::max<u32>(1u, base >> level);
	}

	/// Maps the 14 linear ASTC internal formats. The sRGB twins are rejected here so
	/// the replacement pipeline keeps its linear-UNORM sampling semantics.
	inline bool LookupInternalFormat(u32 gl_internal_format, GSTexture::Format* out_format)
	{
		GSTexture::Format format;
		switch (gl_internal_format)
		{
			case FORMAT_ASTC_4X4: format = GSTexture::Format::ASTC4x4; break;
			case FORMAT_ASTC_5X4: format = GSTexture::Format::ASTC5x4; break;
			case FORMAT_ASTC_5X5: format = GSTexture::Format::ASTC5x5; break;
			case FORMAT_ASTC_6X5: format = GSTexture::Format::ASTC6x5; break;
			case FORMAT_ASTC_6X6: format = GSTexture::Format::ASTC6x6; break;
			case FORMAT_ASTC_8X5: format = GSTexture::Format::ASTC8x5; break;
			case FORMAT_ASTC_8X6: format = GSTexture::Format::ASTC8x6; break;
			case FORMAT_ASTC_8X8: format = GSTexture::Format::ASTC8x8; break;
			case FORMAT_ASTC_10X5: format = GSTexture::Format::ASTC10x5; break;
			case FORMAT_ASTC_10X6: format = GSTexture::Format::ASTC10x6; break;
			case FORMAT_ASTC_10X8: format = GSTexture::Format::ASTC10x8; break;
			case FORMAT_ASTC_10X10: format = GSTexture::Format::ASTC10x10; break;
			case FORMAT_ASTC_12X10: format = GSTexture::Format::ASTC12x10; break;
			case FORMAT_ASTC_12X12: format = GSTexture::Format::ASTC12x12; break;
			default: return false;
		}

		if (out_format)
			*out_format = format;
		return true;
	}

	/// Checked ASTC geometry for mip [level] of [container]. Returns false on u32
	/// overflow rather than wrapping into a bogus allocation size.
	inline bool CalculateLevelGeometry(const ContainerInfo& container, u32 level, LevelInfo* out)
	{
		constexpr u64 MAX_U32 = std::numeric_limits<u32>::max();

		const u32 width = MipDimension(container.width, level);
		const u32 height = MipDimension(container.height, level);

		// Ordinary ceiling division: ASTC block dimensions are not all powers of two.
		const u64 blocks_x = (static_cast<u64>(width) + container.block_info.width - 1) / container.block_info.width;
		const u64 blocks_y = (static_cast<u64>(height) + container.block_info.height - 1) / container.block_info.height;

		const u64 pitch = blocks_x * container.block_info.bytes;
		const u64 payload_size = pitch * blocks_y;
		if (pitch > MAX_U32 || payload_size > MAX_U32)
			return false;

		out->width = width;
		out->height = height;
		out->pitch = static_cast<u32>(pitch);
		out->payload_size = static_cast<u32>(payload_size);
		return true;
	}

	inline u32 ReadU32(const u8* data)
	{
		return data[0] | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) |
			   (static_cast<u32>(data[3]) << 24);
	}

	/// Validates the fixed KTX1 header. The key/value blob and mip records are
	/// validated separately so the file loader can reject the expected file size
	/// before allocating payload storage.
	inline ParseResult ParseHeader(const u8* data, size_t data_size, ContainerInfo* out_info,
		u32 max_dimension = std::numeric_limits<u32>::max())
	{
		if (data_size < HEADER_SIZE)
			return ParseResult::TruncatedHeader;

		if (std::memcmp(data, IDENTIFIER, IDENTIFIER_SIZE) != 0)
			return ParseResult::BadIdentifier;

		if (ReadU32(data + 12) != ENDIANNESS_MARKER)
			return ParseResult::BadEndianness;

		const u32 gl_type = ReadU32(data + 16);
		const u32 gl_type_size = ReadU32(data + 20);
		const u32 gl_format = ReadU32(data + 24);
		const u32 gl_internal_format = ReadU32(data + 28);
		const u32 gl_base_internal_format = ReadU32(data + 32);
		const u32 width = ReadU32(data + 36);
		const u32 height = ReadU32(data + 40);
		const u32 depth = ReadU32(data + 44);
		const u32 array_elements = ReadU32(data + 48);
		const u32 faces = ReadU32(data + 52);
		const u32 mip_count = ReadU32(data + 56);
		const u32 key_value_bytes = ReadU32(data + 60);

		if (gl_type != 0 || gl_type_size != 1 || gl_format != 0)
			return ParseResult::BadTypeFields;
		if (gl_base_internal_format != GL_RGBA)
			return ParseResult::BadBaseFormat;

		GSTexture::Format format;
		if (!LookupInternalFormat(gl_internal_format, &format))
			return ParseResult::BadInternalFormat;
		if (depth != 0 || array_elements != 0 || faces != 1)
			return ParseResult::BadTextureType;
		if (width == 0 || height == 0)
			return ParseResult::BadDimensions;
		if (width > max_dimension || height > max_dimension)
			return ParseResult::TooLarge;

		u32 geometric_mip_count = 1;
		for (u32 mip_width = width, mip_height = height; mip_width > 1 || mip_height > 1; geometric_mip_count++)
		{
			mip_width = std::max<u32>(1, mip_width >> 1);
			mip_height = std::max<u32>(1, mip_height >> 1);
		}
		if (mip_count == 0 || mip_count > geometric_mip_count || mip_count > MAX_MIP_LEVELS)
			return ParseResult::BadMipCount;
		if (key_value_bytes > MAX_KEY_VALUE_BYTES)
			return ParseResult::BadKeyValueData;

		out_info->format = format;
		out_info->width = width;
		out_info->height = height;
		out_info->mip_count = mip_count;
		out_info->block_info = GSTexture::GetBlockInfo(format);
		out_info->mips_offset = static_cast<u64>(HEADER_SIZE) + key_value_bytes;
		out_info->key_value_bytes = key_value_bytes;
		return ParseResult::Ok;
	}

	/// Validates the KTX1 key/value blob. Unknown entries are accepted, but all
	/// entries and padding must be well-formed and exactly one direct orientation
	/// entry is required.
	inline ParseResult ValidateKeyValueData(const u8* data, size_t data_size)
	{
		size_t offset = 0;
		bool found_orientation = false;
		constexpr char ORIENTATION_KEY[] = "KTXorientation";
		constexpr char ORIENTATION_VALUE[] = "S=r,T=d";

		while (offset < data_size)
		{
			if (data_size - offset < sizeof(u32))
				return ParseResult::BadKeyValueData;

			const u32 entry_size = ReadU32(data + offset);
			offset += sizeof(u32);
			if (entry_size < 2 || entry_size > data_size - offset)
				return ParseResult::BadKeyValueData;

			const u8* const entry = data + offset;
			const u8* const key_end = static_cast<const u8*>(std::memchr(entry, 0, entry_size));
			if (!key_end)
				return ParseResult::BadKeyValueData;

			const size_t key_size = static_cast<size_t>(key_end - entry);
			if (key_size == 0)
				return ParseResult::BadKeyValueData;
			const u8* const value = key_end + 1;
			const size_t value_size = entry_size - key_size - 1;
			if (key_size == sizeof(ORIENTATION_KEY) - 1 &&
				std::memcmp(entry, ORIENTATION_KEY, sizeof(ORIENTATION_KEY) - 1) == 0)
			{
				if (found_orientation)
					return ParseResult::BadOrientation;
				found_orientation = true;

				const size_t expected_size = sizeof(ORIENTATION_VALUE) - 1;
				if (value_size != expected_size && value_size != expected_size + 1)
					return ParseResult::BadOrientation;
				if (std::memcmp(value, ORIENTATION_VALUE, expected_size) != 0 ||
					(value_size == expected_size + 1 && value[expected_size] != 0))
				{
					return ParseResult::BadOrientation;
				}
			}

			offset += entry_size;
			const size_t padding = (4 - (entry_size & 3)) & 3;
			if (padding > data_size - offset)
				return ParseResult::BadKeyValueData;
			for (size_t i = 0; i < padding; i++)
			{
				if (data[offset + i] != 0)
					return ParseResult::BadKeyValueData;
			}
			offset += padding;
		}

		return found_orientation ? ParseResult::Ok : ParseResult::BadOrientation;
	}

	/// Calculates the exact mip record offsets and rejects truncated or trailing
	/// container bytes before any payload allocation occurs.
	inline ParseResult BuildLevelLayout(const ContainerInfo& container, s64 file_size,
		std::array<LevelInfo, MAX_MIP_LEVELS>* out_levels)
	{
		if (file_size < 0)
			return ParseResult::TruncatedLevel;

		u64 offset = container.mips_offset;
		const u64 size = static_cast<u64>(file_size);
		for (u32 level = 0; level < container.mip_count; level++)
		{
			LevelInfo info{};
			if (!CalculateLevelGeometry(container, level, &info))
				return ParseResult::BadImageSize;
			if (offset > size || sizeof(u32) > size - offset)
				return ParseResult::TruncatedLevel;

			info.data_offset = offset + sizeof(u32);
			if (info.payload_size > size - info.data_offset)
				return ParseResult::TruncatedLevel;

			const u32 padding = (4 - (info.payload_size & 3)) & 3;
			offset = info.data_offset + info.payload_size + padding;
			(*out_levels)[level] = info;
		}

		if (offset != size)
			return (offset < size) ? ParseResult::TrailingBytes : ParseResult::TruncatedLevel;
		return ParseResult::Ok;
	}

	/// Validates the four-byte imageSize field that precedes one mip payload.
	inline ParseResult ValidateLevelSize(const u8* data, size_t data_size, const LevelInfo& level)
	{
		if (data_size < sizeof(u32))
			return ParseResult::TruncatedLevel;
		return (ReadU32(data) == level.payload_size) ? ParseResult::Ok : ParseResult::BadImageSize;
	}
} // namespace KTX
