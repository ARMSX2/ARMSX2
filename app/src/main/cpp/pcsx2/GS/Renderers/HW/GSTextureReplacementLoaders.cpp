// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/ScopedGuard.h"

#include "GS/Renderers/HW/GSTextureReplacements.h"

#include <algorithm>
#include <csetjmp>
#include <limits>
#if !TARGET_OS_IPHONE
#include <png.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

struct LoaderDefinition
{
	const char* extension;
	GSTextureReplacements::ReplacementTextureLoader loader;
};

static bool PNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
static bool DDSLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);

static constexpr LoaderDefinition s_loaders[] = {
	{"png", PNGLoader},
	{"dds", DDSLoader},
};


GSTextureReplacements::ReplacementTextureLoader GSTextureReplacements::GetLoader(const std::string_view filename)
{
	const std::string_view extension(Path::GetExtension(filename));
	if (extension.empty())
		return nullptr;

	for (const LoaderDefinition& defn : s_loaders)
	{
		if (StringUtil::Strncasecmp(extension.data(), defn.extension, extension.size()) == 0)
			return defn.loader;
	}

	return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Helper routines
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static u32 GetBlockCount(u32 extent, u32 block_size)
{
	return std::max(Common::AlignUp(extent, block_size) / block_size, 1u);
}

static void CalcBlockMipmapSize(u32 block_size, u32 bytes_per_block, u32 base_width, u32 base_height, u32 mip, u32& width, u32& height, u32& pitch, u32& size)
{
	width = std::max<u32>(base_width >> mip, 1u);
	height = std::max<u32>(base_height >> mip, 1u);

	const u32 blocks_wide = GetBlockCount(width, block_size);
	const u32 blocks_high = GetBlockCount(height, block_size);

	// Pitch can't be specified with each mip level, so we have to calculate it ourselves.
	pitch = blocks_wide * bytes_per_block;
	size = blocks_high * pitch;
}

static void ConvertTexture_X8B8G8R8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Set alpha channel to full intensity.
			data_ptr[3] = 0x80;
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_A8R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Byte swap ABGR -> RGBA
			u32 val;
			std::memcpy(&val, data_ptr, sizeof(val));
			val = ((val & 0xFF00FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000));
			std::memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_X8R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* data_ptr = data.data() + row * pitch;

		for (u32 x = 0; x < width; x++)
		{
			// Byte swap XBGR -> RGBX, and set alpha to full intensity.
			u32 val;
			std::memcpy(&val, data_ptr, sizeof(val));
			val = ((val & 0x0000FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000)) | 0xFF000000;
			std::memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
		}
	}
}

static void ConvertTexture_R8G8B8(u32 width, u32 height, std::vector<u8>& data, u32& pitch)
{
	const u32 new_pitch = width * sizeof(u32);
	std::vector<u8> new_data(new_pitch * height);

	for (u32 row = 0; row < height; row++)
	{
		const u8* rgb_data_ptr = data.data() + row * pitch;
		u8* data_ptr = new_data.data() + row * new_pitch;

		for (u32 x = 0; x < width; x++)
		{
			// This is BGR in memory.
			u32 val;
			std::memcpy(&val, rgb_data_ptr, sizeof(val));
			val = ((val & 0x0000FF00) | ((val >> 16) & 0xFF) | ((val << 16) & 0xFF0000)) | 0xFF000000;
			std::memcpy(data_ptr, &val, sizeof(u32));
			data_ptr += sizeof(u32);
			rgb_data_ptr += 3;
		}
	}

	data = std::move(new_data);
	pitch = new_pitch;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// PNG Handlers
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if !TARGET_OS_IPHONE
bool PNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
    // ... (existing code)
}
#else
static void UnpremultiplyAlpha(u32 width, u32 height, std::vector<u8>& data, u32 pitch)
{
	for (u32 row = 0; row < height; row++)
	{
		u8* pixel = data.data() + (static_cast<size_t>(row) * pitch);

		for (u32 col = 0; col < width; col++)
		{
			const u8 alpha = pixel[3];
			if (alpha != 0 && alpha != 255)
			{
				for (u32 channel = 0; channel < 3; channel++)
				{
					const u32 straight = (static_cast<u32>(pixel[channel]) * 255u + (alpha / 2u)) / alpha;
					pixel[channel] = static_cast<u8>((straight > 255u) ? 255u : straight);
				}
			}

			pixel += sizeof(u32);
		}
	}
}

bool PNGLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
	if (filename.empty())
		return false;

	CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
		reinterpret_cast<const UInt8*>(filename.c_str()), filename.length(), false);
	if (!url)
		return false;

	CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
	CFRelease(url);
	if (!source)
		return false;

	CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
	CFRelease(source);
	if (!image)
		return false;

	const size_t width = CGImageGetWidth(image);
	const size_t height = CGImageGetHeight(image);
	if (width == 0 || height == 0 || width > std::numeric_limits<u32>::max() || height > std::numeric_limits<u32>::max())
	{
		CGImageRelease(image);
		return false;
	}

	const size_t bytes_per_row = width * sizeof(u32);
	std::vector<u8> data(bytes_per_row * height);
	CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
	const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast);
	CGContextRef context = CGBitmapContextCreate(data.data(), width, height, 8, bytes_per_row, color_space, bitmap_info);
	if (color_space)
		CGColorSpaceRelease(color_space);
	if (!context)
	{
		CGImageRelease(image);
		return false;
	}

	CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
	CGContextRelease(context);
	CGImageRelease(image);

	// CoreGraphics requires premultiplied alpha for this context; texture
	// replacement rendering expects straight RGBA.
	UnpremultiplyAlpha(static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(bytes_per_row));

	tex->width = static_cast<u32>(width);
	tex->height = static_cast<u32>(height);
	tex->format = GSTexture::Format::Color;
	tex->pitch = static_cast<u32>(bytes_per_row);
	tex->data = std::move(data);
	tex->mips.clear();
	return true;
}
#endif

#if !TARGET_OS_IPHONE
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
    // ... (existing code)
}
#else
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
	if (filename.empty() || width == 0 || height == 0 || !buffer)
		return false;

	const size_t bytes_per_row = static_cast<size_t>(width) * sizeof(u32);
	const size_t total_bytes = bytes_per_row * height;
	const u8* data_ptr = buffer;
	std::vector<u8> compact_data;

	if (pitch != bytes_per_row)
	{
		compact_data.resize(total_bytes);
		for (u32 y = 0; y < height; y++)
			std::memcpy(compact_data.data() + (static_cast<size_t>(y) * bytes_per_row), buffer + (static_cast<size_t>(y) * pitch), bytes_per_row);
		data_ptr = compact_data.data();
	}

	CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
	if (!color_space)
		return false;

	CFDataRef data = CFDataCreate(kCFAllocatorDefault, data_ptr, total_bytes);
	if (!data)
	{
		CGColorSpaceRelease(color_space);
		return false;
	}

	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CFRelease(data);
	if (!provider)
	{
		CGColorSpaceRelease(color_space);
		return false;
	}

	const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big | kCGImageAlphaLast);
	CGImageRef image = CGImageCreate(width, height, 8, 32, bytes_per_row, color_space, bitmap_info,
		provider, nullptr, false, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(color_space);
	if (!image)
		return false;

	CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
		reinterpret_cast<const UInt8*>(filename.c_str()), filename.length(), false);
	if (!url)
	{
		CGImageRelease(image);
		return false;
	}

	CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
	CFRelease(url);
	if (!destination)
	{
		CGImageRelease(image);
		return false;
	}

	CGImageDestinationAddImage(destination, image, nullptr);
	const bool success = CGImageDestinationFinalize(destination);
	CFRelease(destination);
	CGImageRelease(image);
	return success;
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DDS Handler
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// From https://raw.githubusercontent.com/Microsoft/DirectXTex/master/DirectXTex/DDS.h
//
// This header defines constants and structures that are useful when parsing
// DDS files.  DDS files were originally designed to use several structures
// and constants that are native to DirectDraw and are defined in ddraw.h,
// such as DDSURFACEDESC2 and DDSCAPS2.  This file defines similar
// (compatible) constants and structures so that one can use DDS files
// without needing to include ddraw.h.
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=248926

#pragma pack(push, 1)

static constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT
{
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwFourCC;
	uint32_t dwRGBBitCount;
	uint32_t dwRBitMask;
	uint32_t dwGBitMask;
	uint32_t dwBBitMask;
	uint32_t dwABitMask;
};

#define DDS_FOURCC 0x00000004 // DDPF_FOURCC
#define DDS_RGB 0x00000040 // DDPF_RGB
#define DDS_RGBA 0x00000041 // DDPF_RGB | DDPF_ALPHAPIXELS
#define DDS_LUMINANCE 0x00020000 // DDPF_LUMINANCE
#define DDS_LUMINANCEA 0x00020001 // DDPF_LUMINANCE | DDPF_ALPHAPIXELS
#define DDS_ALPHA 0x00000002 // DDPF_ALPHA
#define DDS_PAL8 0x00000020 // DDPF_PALETTEINDEXED8
#define DDS_PAL8A 0x00000021 // DDPF_PALETTEINDEXED8 | DDPF_ALPHAPIXELS
#define DDS_BUMPDUDV 0x00080000 // DDPF_BUMPDUDV

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
	((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | ((uint32_t)(uint8_t)(ch2) << 16) | \
		((uint32_t)(uint8_t)(ch3) << 24))
#endif /* defined(MAKEFOURCC) */

#define DDS_HEADER_FLAGS_TEXTURE \
	0x00001007 // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
#define DDS_HEADER_FLAGS_MIPMAP 0x00020000 // DDSD_MIPMAPCOUNT
#define DDS_HEADER_FLAGS_VOLUME 0x00800000 // DDSD_DEPTH
#define DDS_HEADER_FLAGS_PITCH 0x00000008 // DDSD_PITCH
#define DDS_HEADER_FLAGS_LINEARSIZE 0x00080000 // DDSD_LINEARSIZE
#define DDS_MAX_TEXTURE_SIZE 32768

// Subset here matches D3D10_RESOURCE_DIMENSION and D3D11_RESOURCE_DIMENSION
enum DDS_RESOURCE_DIMENSION
{
	DDS_DIMENSION_TEXTURE1D = 2,
	DDS_DIMENSION_TEXTURE2D = 3,
	DDS_DIMENSION_TEXTURE3D = 4,
};

struct DDS_HEADER
{
	uint32_t dwSize;
	uint32_t dwFlags;
	uint32_t dwHeight;
	uint32_t dwWidth;
	uint32_t dwPitchOrLinearSize;
	uint32_t dwDepth; // only if DDS_HEADER_FLAGS_VOLUME is set in dwFlags
	uint32_t dwMipMapCount;
	uint32_t dwReserved1[11];
	DDS_PIXELFORMAT ddspf;
	uint32_t dwCaps;
	uint32_t dwCaps2;
	uint32_t dwCaps3;
	uint32_t dwCaps4;
	uint32_t dwReserved2;
};

struct DDS_HEADER_DXT10
{
	uint32_t dxgiFormat;
	uint32_t resourceDimension;
	uint32_t miscFlag; // see DDS_RESOURCE_MISC_FLAG
	uint32_t arraySize;
	uint32_t miscFlags2; // see DDS_MISC_FLAGS2
};

#pragma pack(pop)

static_assert(sizeof(DDS_HEADER) == 124, "DDS Header size mismatch");
static_assert(sizeof(DDS_HEADER_DXT10) == 20, "DDS DX10 Extended Header size mismatch");

constexpr DDS_PIXELFORMAT DDSPF_A8R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_A8B8G8R8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGBA, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000};
constexpr DDS_PIXELFORMAT DDSPF_X8B8G8R8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0x00000000};
constexpr DDS_PIXELFORMAT DDSPF_R8G8B8 = {
	sizeof(DDS_PIXELFORMAT), DDS_RGB, 0, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000};

// End of Microsoft code from DDS.h.

static bool DDSPixelFormatMatches(const DDS_PIXELFORMAT& pf1, const DDS_PIXELFORMAT& pf2)
{
	return std::tie(pf1.dwSize, pf1.dwFlags, pf1.dwFourCC, pf1.dwRGBBitCount, pf1.dwRBitMask, pf1.dwGBitMask, pf1.dwGBitMask, pf1.dwBBitMask, pf1.dwABitMask) ==
	       std::tie(pf2.dwSize, pf2.dwFlags, pf2.dwFourCC, pf2.dwRGBBitCount, pf2.dwRBitMask, pf2.dwGBitMask, pf2.dwGBitMask, pf2.dwBBitMask, pf2.dwABitMask);
}

struct DDSLoadInfo
{
	u32 block_size = 1;
	u32 bytes_per_block = 4;
	u32 width = 0;
	u32 height = 0;
	u32 mip_count = 0;
	GSTexture::Format format = GSTexture::Format::Color;
	s64 base_image_offset = 0;
	u32 base_image_size = 0;
	u32 base_image_pitch = 0;

	std::function<void(u32 width, u32 height, std::vector<u8>& data, u32& pitch)> conversion_function;
};

static bool ParseDDSHeader(std::FILE* fp, DDSLoadInfo* info)
{
	u32 magic;
	if (std::fread(&magic, sizeof(magic), 1, fp) != 1 || magic != DDS_MAGIC)
		return false;

	DDS_HEADER header;
	u32 header_size = sizeof(header);
	if (std::fread(&header, header_size, 1, fp) != 1 || header.dwSize < header_size)
		return false;

	// We should check for DDS_HEADER_FLAGS_TEXTURE here, but some tools don't seem
	// to set it (e.g. compressonator). But we can still validate the size.
	if (header.dwWidth == 0 || header.dwWidth >= DDS_MAX_TEXTURE_SIZE ||
		header.dwHeight == 0 || header.dwHeight >= DDS_MAX_TEXTURE_SIZE)
	{
		return false;
	}

	// Image should be 2D.
	if (header.dwFlags & DDS_HEADER_FLAGS_VOLUME)
		return false;

	// Presence of width/height fields is already tested by DDS_HEADER_FLAGS_TEXTURE.
	info->width = header.dwWidth;
	info->height = header.dwHeight;
	if (info->width == 0 || info->height == 0)
		return false;

	// Check for mip levels.
	if (header.dwFlags & DDS_HEADER_FLAGS_MIPMAP)
	{
		info->mip_count = header.dwMipMapCount;
		if (header.dwMipMapCount != 0)
			info->mip_count = header.dwMipMapCount;
		else
			info->mip_count = GSTextureReplacements::CalcMipmapLevelsForReplacement(info->width, info->height);
	}
	else
	{
		info->mip_count = 1;
	}

	// Handle fourcc formats vs uncompressed formats.
	const bool has_fourcc = (header.ddspf.dwFlags & DDS_FOURCC) != 0;
	if (has_fourcc)
	{
		// Handle DX10 extension header.
		u32 dxt10_format = 0;
		if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', '1', '0'))
		{
			DDS_HEADER_DXT10 dxt10_header;
			if (std::fread(&dxt10_header, sizeof(dxt10_header), 1, fp) != 1)
				return false;

			// Can't handle array textures here. Doesn't make sense to use them, anyway.
			if (dxt10_header.resourceDimension != DDS_DIMENSION_TEXTURE2D || dxt10_header.arraySize != 1)
				return false;

			header_size += sizeof(dxt10_header);
			dxt10_format = dxt10_header.dxgiFormat;
		}

		const GSDevice::FeatureSupport features(g_gs_device->Features());
		if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '1') || dxt10_format == 71 /*DXGI_FORMAT_BC1_UNORM*/)
		{
			info->format = GSTexture::Format::BC1;
			info->block_size = 4;
			info->bytes_per_block = 8;
			if (!features.dxt_textures)
				return false;
		}
		else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '2') || header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '3') || dxt10_format == 74 /*DXGI_FORMAT_BC2_UNORM*/)
		{
			info->format = GSTexture::Format::BC2;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.dxt_textures)
				return false;
		}
		else if (header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '4') || header.ddspf.dwFourCC == MAKEFOURCC('D', 'X', 'T', '5') || dxt10_format == 77 /*DXGI_FORMAT_BC3_UNORM*/)
		{
			info->format = GSTexture::Format::BC3;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.dxt_textures)
				return false;
		}
		else if (dxt10_format == 98 /*DXGI_FORMAT_BC7_UNORM*/)
		{
			info->format = GSTexture::Format::BC7;
			info->block_size = 4;
			info->bytes_per_block = 16;
			if (!features.bptc_textures)
				return false;
		}
		else
		{
			// Leave all remaining formats to SOIL.
			return false;
		}
	}
	else
	{
		if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8R8G8B8))
		{
			info->conversion_function = ConvertTexture_A8R8G8B8;
		}
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8R8G8B8))
		{
			info->conversion_function = ConvertTexture_X8R8G8B8;
		}
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_X8B8G8R8))
		{
			info->conversion_function = ConvertTexture_X8B8G8R8;
		}
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_R8G8B8))
		{
			info->conversion_function = ConvertTexture_R8G8B8;
		}
		else if (DDSPixelFormatMatches(header.ddspf, DDSPF_A8B8G8R8))
		{
			// This format is already in RGBA order, so no conversion necessary.
		}
		else
		{
			return false;
		}

		// All these formats are RGBA, just with byte swapping.
		info->format = GSTexture::Format::Color;
		info->block_size = 1;
		info->bytes_per_block = header.ddspf.dwRGBBitCount / 8;
	}

	// Mip levels smaller than the block size are padded to multiples of the block size.
	const u32 blocks_wide = GetBlockCount(info->width, info->block_size);
	const u32 blocks_high = GetBlockCount(info->height, info->block_size);

	// Pitch can be specified in the header, otherwise we can derive it from the dimensions. For
	// compressed formats, both DDS_HEADER_FLAGS_LINEARSIZE and DDS_HEADER_FLAGS_PITCH should be
	// set. See https://msdn.microsoft.com/en-us/library/windows/desktop/bb943982(v=vs.85).aspx
	if (header.dwFlags & DDS_HEADER_FLAGS_PITCH && header.dwFlags & DDS_HEADER_FLAGS_LINEARSIZE)
	{
		// Convert pitch (in bytes) to texels/row length.
		if (header.dwPitchOrLinearSize < info->bytes_per_block)
		{
			// Likely a corrupted or invalid file.
			return false;
		}

		info->base_image_pitch = header.dwPitchOrLinearSize;
		info->base_image_size = info->base_image_pitch * blocks_high;
	}
	else
	{
		// Assume no padding between rows of blocks.
		info->base_image_pitch = blocks_wide * info->bytes_per_block;
		info->base_image_size = info->base_image_pitch * blocks_high;
	}

	// Check for truncated or corrupted files.
	info->base_image_offset = sizeof(magic) + header_size;
	if (info->base_image_offset >= FileSystem::FSize64(fp))
		return false;

	return true;
}

static bool ReadDDSMipLevel(std::FILE* fp, const std::string& filename, u32 mip_level, const DDSLoadInfo& info, u32 width, u32 height, std::vector<u8>& data, u32& pitch, u32 size)
{
	// D3D11 cannot handle block compressed textures where the first mip level is
	// not a multiple of the block size.
	if (mip_level == 0 && info.block_size > 1 &&
		((width % info.block_size) != 0 || (height % info.block_size) != 0))
	{
		Console.Error(
			"Invalid dimensions for DDS texture %s. For compressed textures of this format, "
			"the width/height of the first mip level must be a multiple of %u.",
			filename.c_str(), info.block_size);
		return false;
	}

	data.resize(size);
	if (std::fread(data.data(), size, 1, fp) != 1)
		return false;

	// Apply conversion function for uncompressed textures.
	if (info.conversion_function)
		info.conversion_function(width, height, data, pitch);

	return true;
}

bool DDSLoader(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
	if (!fp)
		return false;

	DDSLoadInfo info;
	if (!ParseDDSHeader(fp.get(), &info))
		return false;

	// always load the base image
	if (FileSystem::FSeek64(fp.get(), info.base_image_offset, SEEK_SET) != 0)
		return false;

	tex->format = info.format;
	tex->width = info.width;
	tex->height = info.height;
	tex->pitch = info.base_image_pitch;
	if (!ReadDDSMipLevel(fp.get(), filename, 0, info, tex->width, tex->height, tex->data, tex->pitch, info.base_image_size))
		return false;

	// Read in any remaining mip levels in the file.
	if (!only_base_image)
	{
		// dwMipMapCount includes the base image (level 0); over-declared counts or
		// trailing padding made the old bound load a phantom level and trip the
		// CreateTexture mip assert. Cap at what the dimensions support.
		const u32 max_levels = static_cast<u32>(
			GSDevice::GetMipmapLevelsForSize(static_cast<int>(info.width), static_cast<int>(info.height)));
		const u32 mip_levels = std::min(info.mip_count, max_levels);
		for (u32 level = 1; level < mip_levels; level++)
		{
			GSTextureReplacements::ReplacementTexture::MipData md;
			u32 mip_size;
			CalcBlockMipmapSize(info.block_size, info.bytes_per_block, info.width, info.height, level, md.width, md.height, md.pitch, mip_size);
			if (!ReadDDSMipLevel(fp.get(), filename, level, info, md.width, md.height, md.data, md.pitch, mip_size))
				break;

			tex->mips.push_back(std::move(md));
		}
	}

	return true;
}
