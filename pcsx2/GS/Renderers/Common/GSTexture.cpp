// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSPng.h"

#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/StringUtil.h"

#include <bit>
#include <bitset>

GSTexture::GSTexture() = default;

GSTexture::~GSTexture() = default;

bool GSTexture::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	// An upload only conflicts with a queued draw that reads or writes this very texture -
	// and the overwhelming majority are into a source the queue has never seen, since the
	// texture cache uploads into freshly pooled surfaces. Flushing for all of them was 15%
	// of every flush on Dirge of Cerberus.
	g_gs_device->FlushDeferredDrawsFor(this);
	return DoUpdate(r, data, pitch, layer);
}

bool GSTexture::Map(GSMap& m, const GSVector4i* r, int layer)
{
	g_gs_device->FlushDeferredDrawsFor(this);
	return DoMap(m, r, layer);
}

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)

void GSTexture::AssertNoQueuedObserver(const char* what) const
{
	// g_gs_device is null while the device itself is being torn down, which is also when
	// the last textures are destroyed - there is no queue to violate at that point.
	pxAssertMsg(!g_gs_device || !g_gs_device->DeferredDrawsWouldObserve(this), what);
}

#endif

bool GSTexture::ValidateUsageAndFormat(Usage usage, Format format)
{
	if (IsDepthStencil(usage) && (usage & (Usage::ShaderWrite | Usage::RenderTarget)))
		return false; // DS is not compatible with Write or RT
	if (IsFeedback(usage) && !(usage & (Usage::DepthStencil | Usage::RenderTarget)))
		return false; // Feedback requires RT or DS
	if (usage == (Usage::ShaderWrite | Usage::RenderTarget))
		return false; // We always include Feedback for RT+Write
	if (format == Format::UNorm8 && !IsTexture(usage)) // Unorm8 only used for sampling
		return false;
	if (IsFeedback(usage) && !IsFeedbackFormat(format)) // Only some formats used with feedback
		return false;
	if (IsShaderWrite(usage) && !IsShaderWriteFormat(format)) // Only some formats used with shader write
		return false;
	if (IsDepthStencil(usage) && (format != Format::DepthStencil)) // Only use DepthStencil format with DepthStencil usage
		return false;
	if (!g_gs_device->Features().depth_feedback)
	{
		if (IsDepthStencil(usage) && IsFeedback(usage))
			return false;
	}
	return true;
}

bool GSTexture::Save(const std::string& fn)
{
	// Depth textures need special treatment - we have a stencil component.
	// Just re-use the existing conversion shader instead.
	if (m_format == Format::DepthStencil || m_format == Format::DepthColor)
	{
		GSTexture* temp = g_gs_device->CreateRenderTarget(GetWidth(), GetHeight(), Format::Color, false);
		if (!temp)
		{
			Console.Error("Failed to allocate %dx%d texture for depth conversion", GetWidth(), GetHeight());
			return false;
		}

		g_gs_device->StretchRectAuto(this, temp, Nearest);
		const bool res = temp->Save(fn);
		g_gs_device->Recycle(temp);
		return res;
	}

	GSPng::Format format = (IsDevBuild || GSConfig.SaveAlpha) ? GSPng::RGB_A_PNG : GSPng::RGB_PNG;

	switch (m_format)
	{
		case Format::UNorm8:
			format = GSPng::R8I_PNG;
			break;
		case Format::Color:
			break;
		default:
			Console.Error("Format %d not saved to image", static_cast<int>(m_format));
			return false;
	}

	const GSVector4i rc(0, 0, m_size.x, m_size.y);
	std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(m_size.x, m_size.y, m_format));
	if (!dl || (dl->CopyFromTexture(rc, this, rc, 0), dl->Flush(), !dl->Map(rc)))
	{
		Console.Error("(GSTexture) DownloadTexture() failed.");
		return false;
	}

	const int compression = GSConfig.PNGCompressionLevel;
	return GSPng::Save(format, fn, dl->GetMapPointer(), m_size.x, m_size.y, dl->GetMapPitch(), compression, false);
}

const char* GSTexture::GetFormatName(Format format)
{
	switch (format)
	{
		default:
			pxFailRel("Invalid texture format");
			[[fallthrough]];
		case Format::Invalid:      return "Invalid";
		case Format::Color:        return "Color";
		case Format::ColorHQ:      return "ColorHQ";
		case Format::ColorHDR:     return "ColorHDR";
		case Format::ColorClip:    return "ColorClip";
		case Format::DepthStencil: return "DepthStencil";
		case Format::DepthColor:   return "DepthColor";
		case Format::UNorm8:       return "UNorm8";
		case Format::UInt16:       return "UInt16";
		case Format::UInt32:       return "UInt32";
		case Format::PrimID:       return "PrimID";
		case Format::BC1:          return "BC1";
		case Format::BC2:          return "BC2";
		case Format::BC3:          return "BC3";
		case Format::BC7:          return "BC7";
		case Format::ASTC4x4:      return "ASTC4x4";
		case Format::ASTC5x4:      return "ASTC5x4";
		case Format::ASTC5x5:      return "ASTC5x5";
		case Format::ASTC6x5:      return "ASTC6x5";
		case Format::ASTC6x6:      return "ASTC6x6";
		case Format::ASTC8x5:      return "ASTC8x5";
		case Format::ASTC8x6:      return "ASTC8x6";
		case Format::ASTC8x8:      return "ASTC8x8";
		case Format::ASTC10x5:     return "ASTC10x5";
		case Format::ASTC10x6:     return "ASTC10x6";
		case Format::ASTC10x8:     return "ASTC10x8";
		case Format::ASTC10x10:    return "ASTC10x10";
		case Format::ASTC12x10:    return "ASTC12x10";
		case Format::ASTC12x12:    return "ASTC12x12";
	}
}

GSTexture::BlockInfo GSTexture::GetBlockInfo(Format format)
{
	switch (format)
	{
		default:
			pxFailRel("Invalid texture format");
			[[fallthrough]];
		case Format::Invalid:      return {1, 1, 1};   // Invalid
		case Format::Color:        return {1, 1, 4};   // Color/RGBA8
		case Format::ColorHQ:      return {1, 1, 4};   // ColorHQ/RGB10A2
		case Format::ColorHDR:     return {1, 1, 8};   // ColorHDR/RGBA16F
		case Format::ColorClip:    return {1, 1, 8};   // ColorClip/RGBA16
		case Format::DepthStencil: return {1, 1, 4};   // DepthStencil
		case Format::DepthColor:   return {1, 1, 4};   // DepthColor/R32
		case Format::UNorm8:       return {1, 1, 1};   // UNorm8/R8
		case Format::UInt16:       return {1, 1, 2};   // UInt16/R16UI
		case Format::UInt32:       return {1, 1, 4};   // UInt32/R32UI
		case Format::PrimID:       return {1, 1, 4};   // PrimID/R32
		case Format::BC1:          return {4, 4, 8};   // BC1 - 16 pixels in 64 bits
		case Format::BC2:          return {4, 4, 16};  // BC2 - 16 pixels in 128 bits
		case Format::BC3:          return {4, 4, 16};  // BC3 - 16 pixels in 128 bits
		case Format::BC7:          return {4, 4, 16};  // BC7 - 16 pixels in 128 bits
		case Format::ASTC4x4:      return {4, 4, 16};
		case Format::ASTC5x4:      return {5, 4, 16};
		case Format::ASTC5x5:      return {5, 5, 16};
		case Format::ASTC6x5:      return {6, 5, 16};
		case Format::ASTC6x6:      return {6, 6, 16};
		case Format::ASTC8x5:      return {8, 5, 16};
		case Format::ASTC8x6:      return {8, 6, 16};
		case Format::ASTC8x8:      return {8, 8, 16};
		case Format::ASTC10x5:     return {10, 5, 16};
		case Format::ASTC10x6:     return {10, 6, 16};
		case Format::ASTC10x8:     return {10, 8, 16};
		case Format::ASTC10x10:    return {10, 10, 16};
		case Format::ASTC12x10:    return {12, 10, 16};
		case Format::ASTC12x12:    return {12, 12, 16};
	}
}

bool GSTexture::IsASTCFormat(Format format)
{
	return format >= Format::ASTC4x4 && format <= Format::ASTC12x12;
}

bool GSTexture::IsBlockCompressedFormat(Format format)
{
	const BlockInfo bi = GetBlockInfo(format);
	return bi.width != 1 || bi.height != 1;
}

u32 GSTexture::GetCompressedBytesPerBlock() const
{
	return GetCompressedBytesPerBlock(m_format);
}

u32 GSTexture::GetCompressedBytesPerBlock(Format format)
{
	switch (format)
	{
		default:
			pxFailRel("Invalid texture format");
			[[fallthrough]];
		case Format::Invalid:      return 1;  // Invalid
		case Format::Color:        return 4;  // Color/RGBA8
		case Format::ColorHQ:      return 4;  // ColorHQ/RGB10A2
		case Format::ColorHDR:     return 8;  // ColorHDR/RGBA16F
		case Format::ColorClip:    return 8;  // ColorClip/RGBA16
		case Format::DepthStencil: return 4;  // DepthStencil
		case Format::DepthColor:   return 4;  // DepthColor/R32
		case Format::UNorm8:       return 1;  // UNorm8/R8
		case Format::UInt16:       return 2;  // UInt16/R16UI
		case Format::UInt32:       return 4;  // UInt32/R32UI
		case Format::PrimID:       return 4;  // PrimID/R32
		case Format::BC1:          return 8;  // BC1 - 16 pixels in 64 bits
		case Format::BC2:          return 16; // BC2 - 16 pixels in 128 bits
		case Format::BC3:          return 16; // BC3 - 16 pixels in 128 bits
		case Format::BC7:          return 16; // BC7 - 16 pixels in 128 bits
	}
}

u32 GSTexture::GetCompressedBlockSize() const
{
	return GetCompressedBlockSize(m_format);
}

u32 GSTexture::GetCompressedBlockSize(Format format)
{
	// Scalar form: only meaningful for square blocks. Rectangular ASTC footprints must
	// go through GetBlockInfo(); callers below are shared with ASTC and use that.
	const BlockInfo bi = GetBlockInfo(format);
	return (bi.width == bi.height) ? bi.width : 0;
}

u32 GSTexture::CalcUploadPitch(Format format, u32 width)
{
	const BlockInfo bi = GetBlockInfo(format);
	// Ordinary ceiling division: ASTC block dimensions (5, 6, 10, 12) are not powers of
	// two, so AlignUpPow2-style rounding would corrupt the pitch.
	return ((width + bi.width - 1) / bi.width) * bi.bytes;
}

u32 GSTexture::CalcUploadPitch(u32 width) const
{
	return CalcUploadPitch(m_format, width);
}

u32 GSTexture::CalcUploadRowLengthFromPitch(u32 pitch) const
{
	return CalcUploadRowLengthFromPitch(m_format, pitch);
}

u32 GSTexture::CalcUploadRowLengthFromPitch(Format format, u32 pitch)
{
	const BlockInfo bi = GetBlockInfo(format);
	return ((pitch + bi.bytes - 1) / bi.bytes) * bi.width;
}

u32 GSTexture::CalcUploadSize(u32 height, u32 pitch) const
{
	return CalcUploadSize(m_format, height, pitch);
}

u32 GSTexture::CalcUploadSize(Format format, u32 height, u32 pitch)
{
	const BlockInfo bi = GetBlockInfo(format);
	return pitch * ((static_cast<u32>(height) + bi.height - 1) / bi.height);
}

bool GSTexture::IsFeedbackFormat(Format format)
{
	return format == Format::Color || format == Format::ColorClip ||
		format == Format::DepthColor || format == Format::DepthStencil;
}

bool GSTexture::IsShaderWriteFormat(Format format)
{
	return format == Format::Color || format == Format::DepthColor;
}

void GSTexture::GenerateMipmapsIfNeeded()
{
	if (!m_needs_mipmaps_generated || m_mipmap_levels <= 1 || IsCompressedFormat())
		return;

	m_needs_mipmaps_generated = false;

	// Reads every level of the texture and writes the smaller ones, so it is an observation
	// point like any other. Guarding the single non-virtual caller keeps the six backend
	// GenerateMipmap() overrides untouched.
	g_gs_device->FlushDeferredDrawsFor(this);
	GenerateMipmap();
}

GSDownloadTexture::GSDownloadTexture(u32 width, u32 height, GSTexture::Format format)
	: m_width(width)
	, m_height(height)
	, m_format(format)
{
}

GSDownloadTexture::~GSDownloadTexture() = default;

void GSDownloadTexture::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	g_gs_device->FlushDeferredDraws();
	DoCopyFromTexture(drc, stex, src, src_level, use_transfer_pitch);
}

u32 GSDownloadTexture::GetBufferSize(u32 width, u32 height, GSTexture::Format format, u32 pitch_align /* = 1 */)
{
	const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(format);
	const u32 bw = (width + bi.width - 1) / bi.width;
	const u32 bh = (height + bi.height - 1) / bi.height;

	pxAssert(std::has_single_bit(pitch_align));
	const u32 pitch = Common::AlignUpPow2(bw * bi.bytes, pitch_align);
	return (pitch * bh);
}

u32 GSDownloadTexture::GetTransferPitch(u32 width, u32 pitch_align) const
{
	const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(m_format);
	const u32 bw = (width + bi.width - 1) / bi.width;

	pxAssert(std::has_single_bit(pitch_align));
	return Common::AlignUpPow2(bw * bi.bytes, pitch_align);
}

void GSDownloadTexture::GetTransferSize(
	const GSVector4i& rc, u32* copy_offset, u32* copy_row_bytes, u32* copy_rows) const
{
	const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(m_format);
	const u32 tw = static_cast<u32>(rc.width());
	const u32 tb = ((tw + bi.width - 1) / bi.width);

	*copy_offset = (((static_cast<u32>(rc.y) + bi.height - 1) / bi.height) * m_current_pitch) +
				   (((static_cast<u32>(rc.x) + bi.width - 1) / bi.width) * bi.bytes);
	// One row's worth. Rows are m_current_pitch apart, and for a block-compressed format a "row"
	// is a row of blocks -- both of which GetTransferRegionSize takes care of for the callers that
	// need the whole region rather than a row of it.
	*copy_row_bytes = tb * bi.bytes;
	*copy_rows = ((static_cast<u32>(rc.height()) + bi.height - 1) / bi.height);
}

bool GSDownloadTexture::ReadTexels(const GSVector4i& rc, void* out_ptr, u32 out_stride)
{
	if (m_needs_flush)
		Flush();

	if (!Map(rc))
		return false;

	const GSTexture::BlockInfo bi = GSTexture::GetBlockInfo(m_format);
	const u32 tw = static_cast<u32>(rc.width());
	const u32 tb = ((tw + bi.width - 1) / bi.width);

	const u32 copy_offset = (((static_cast<u32>(rc.y) + bi.height - 1) / bi.height) * m_current_pitch) +
							(((static_cast<u32>(rc.x) + bi.width - 1) / bi.width) * bi.bytes);
	const u32 copy_size = tb * bi.bytes;
	const u32 copy_rows = ((static_cast<u32>(rc.height()) + bi.height - 1) / bi.height);

	StringUtil::StrideMemCpy(out_ptr, out_stride, m_map_pointer + copy_offset, m_current_pitch, copy_size, copy_rows);
	return true;
}
