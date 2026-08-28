// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#include <cstdint>
#include <limits>

/// Raw astcenc `.astc` file parsing, kept free of device policy: no g_gs_device access,
/// no logging, no allocation, so it can be pinned directly by host tests.
///
/// The pack contract is linear LDR RGBA with straight alpha (astcenc `-cl`). The raw
/// format records neither transfer function nor profile, so nothing else can be inferred.
namespace ASTC
{
	static constexpr u32 HEADER_SIZE = 16;
	static constexpr u32 BLOCK_BYTES = 16;
	// Magic bytes for the astcenc raw container.
	static constexpr u8 MAGIC[4] = {0x13, 0xAB, 0xA1, 0x5C};

	enum class ParseResult
	{
		Ok,
		BadMagic,
		BadBlockFootprint, // block_depth != 1 or (bw,bh) not a standard LDR footprint
		NotTwoDimensional, // image_depth != 1
		BadImageDimensions, // zero width or height
		TruncatedHeader, // fewer than HEADER_SIZE bytes available
		TooLarge, // dimension above the supplied device limit
	};

	struct HeaderInfo
	{
		GSTexture::Format format;
		u32 width;
		u32 height;
		u32 block_width;
		u32 block_height;
	};

	/// Parses and validates the fixed 16-byte header. [max_dimension] comes from the active
	/// device's texture-size limit; the parser itself has no device dependency.
	inline ParseResult ParseHeader(const u8* data, size_t data_size, HeaderInfo* out_info,
		u32 max_dimension = std::numeric_limits<u32>::max())
	{
		if (data_size < HEADER_SIZE)
			return ParseResult::TruncatedHeader;

		for (u32 i = 0; i < 4; i++)
		{
			if (data[i] != MAGIC[i])
				return ParseResult::BadMagic;
		}

		const u32 block_width = data[4];
		const u32 block_height = data[5];
		const u32 block_depth = data[6];

		// Explicit byte reads; never cast the header onto a packed struct.
		const u32 width = data[7] | (static_cast<u32>(data[8]) << 8) | (static_cast<u32>(data[9]) << 16);
		const u32 height = data[10] | (static_cast<u32>(data[11]) << 8) | (static_cast<u32>(data[12]) << 16);
		const u32 depth = data[13] | (static_cast<u32>(data[14]) << 8) | (static_cast<u32>(data[15]) << 16);

		if (block_depth != 1 || depth != 1)
			return ParseResult::NotTwoDimensional;

		GSTexture::Format format;
		switch ((block_width << 8) | block_height)
		{
			case (4u << 8) | 4u:
				format = GSTexture::Format::ASTC4x4;
				break;
			case (5u << 8) | 4u:
				format = GSTexture::Format::ASTC5x4;
				break;
			case (5u << 8) | 5u:
				format = GSTexture::Format::ASTC5x5;
				break;
			case (6u << 8) | 5u:
				format = GSTexture::Format::ASTC6x5;
				break;
			case (6u << 8) | 6u:
				format = GSTexture::Format::ASTC6x6;
				break;
			case (8u << 8) | 5u:
				format = GSTexture::Format::ASTC8x5;
				break;
			case (8u << 8) | 6u:
				format = GSTexture::Format::ASTC8x6;
				break;
			case (8u << 8) | 8u:
				format = GSTexture::Format::ASTC8x8;
				break;
			case (10u << 8) | 5u:
				format = GSTexture::Format::ASTC10x5;
				break;
			case (10u << 8) | 6u:
				format = GSTexture::Format::ASTC10x6;
				break;
			case (10u << 8) | 8u:
				format = GSTexture::Format::ASTC10x8;
				break;
			case (10u << 8) | 10u:
				format = GSTexture::Format::ASTC10x10;
				break;
			case (12u << 8) | 10u:
				format = GSTexture::Format::ASTC12x10;
				break;
			case (12u << 8) | 12u:
				format = GSTexture::Format::ASTC12x12;
				break;
			default:
				return ParseResult::BadBlockFootprint;
		}

		if (width == 0 || height == 0)
			return ParseResult::BadImageDimensions;

		if (width > max_dimension || height > max_dimension)
			return ParseResult::TooLarge;

		out_info->format = format;
		out_info->width = width;
		out_info->height = height;
		out_info->block_width = block_width;
		out_info->block_height = block_height;
		return ParseResult::Ok;
	}

	/// Checked geometry for the payload following the header. Returns false when the size
	/// overflows u32 rather than wrapping around into a bogus allocation.
	inline bool CalculatePayloadSize(const HeaderInfo& info, u32* out_pitch, u32* out_payload_size)
	{
		constexpr u64 MAX_U32 = std::numeric_limits<u32>::max();

		// Ordinary ceiling division: ASTC block dimensions are not all powers of two.
		const u64 blocks_x = (static_cast<u64>(info.width) + info.block_width - 1) / info.block_width;
		const u64 blocks_y = (static_cast<u64>(info.height) + info.block_height - 1) / info.block_height;

		const u64 pitch = blocks_x * BLOCK_BYTES;
		const u64 payload_size = pitch * blocks_y;
		if (pitch > MAX_U32 || payload_size > MAX_U32)
			return false;

		*out_pitch = static_cast<u32>(pitch);
		*out_payload_size = static_cast<u32>(payload_size);
		return true;
	}

	/// The raw container carries no size field, so the only defense against a truncated
	/// payload or trailing bytes is comparing the file size against header-derived geometry.
	inline bool ValidateFileSize(const HeaderInfo& info, s64 file_size)
	{
		u32 pitch = 0;
		u32 payload_size = 0;
		if (!CalculatePayloadSize(info, &pitch, &payload_size))
			return false;

		return file_size == static_cast<s64>(HEADER_SIZE) + static_cast<s64>(payload_size);
	}
} // namespace ASTC