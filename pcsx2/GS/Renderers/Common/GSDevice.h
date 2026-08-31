// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/HashCombine.h"
#include "common/WindowInfo.h"
#include "GS/GS.h"
#include "GS/GSRegs.h" // GetAlphaTestPS speaks in ATST_* register values
#include "GS/Renderers/Common/GSFastList.h"
#include "GS/Renderers/Common/GSGPUProfile.h"
#include "GS/Renderers/Common/GSShaderEnums.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "GS/Renderers/Tile/GSPageBitmap.h" // the writeback batch's page-hazard set
#include "GS/GSAlignedClass.h"
#include "GS/GSExtra.h"
#include <array>
#include <span>
#include <string>
#include <vector>

enum class Filter
{
	Nearest = 0,
	Biln    = 1,
};

static inline constexpr Filter Nearest = Filter::Nearest;
static inline constexpr Filter Biln    = Filter::Biln;

static inline constexpr Filter BilnIf(bool biln)
{
	return biln ? Biln : Nearest;
}

struct GPUPipelineStatistics
{
	u64 vs_invocations;
	u64 ps_invocations;
};

enum class ShaderConvert
{
	COPY = 0,
	DEPTH_COPY,
	RGB5A1_TO_16_BITS,
	DATM_1,
	DATM_0,
	DATM_1_RTA_CORRECTION,
	DATM_0_RTA_CORRECTION,
	COLCLIP_INIT,
	COLCLIP_RESOLVE,
	RTA_CORRECTION,
	RTA_DECORRECTION,
	TRANSPARENCY_FILTER,
	DEPTH32_TO_16_BITS,
	DEPTH32_TO_32_BITS,
	DEPTH32_TO_RGBA8,
	DEPTH32_TO_RGB8,
	DEPTH16_TO_RGB5A1,
	RGBA8_TO_DEPTH32,
	RGBA8_TO_DEPTH24,
	RGBA8_TO_DEPTH16,
	RGB5A1_TO_DEPTH16,
	DEPTH32_TO_DEPTH24,
	DOWNSAMPLE_COPY,
	RGBA_TO_8I,
	RGB5A1_TO_8I,
	CLUT_4,
	CLUT_8,
	YUV,
	Count
};

enum class PresentShader
{
	COPY = 0,
	SCANLINE,
	DIAGONAL_FILTER,
	TRIANGULAR_FILTER,
	COMPLEX_FILTER,
	LOTTES_FILTER,
	SUPERSAMPLE_4xRGSS,
	SUPERSAMPLE_AUTO,
	Count
};

enum class SetDATM : u8
{
	DATM0 = 0U,
	DATM1,
	DATM0_RTA_CORRECTION,
	DATM1_RTA_CORRECTION
};

enum class ShaderInterlace
{
	WEAVE = 0,
	BOB = 1,
	BLEND = 2,
	MAD_BUFFER = 3,
	MAD_RECONSTRUCT = 4,
	Count
};

static inline constexpr bool HasVariableWriteMask(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::COPY:
		case ShaderConvert::RTA_CORRECTION:
			return true;
		default:
			return false;
	}
}

static inline constexpr bool HasColorOutput(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::COPY:
		case ShaderConvert::RTA_CORRECTION:
		case ShaderConvert::RTA_DECORRECTION:
		case ShaderConvert::TRANSPARENCY_FILTER:
		case ShaderConvert::DEPTH32_TO_RGBA8:
		case ShaderConvert::DEPTH32_TO_RGB8:
		case ShaderConvert::DEPTH16_TO_RGB5A1:
		case ShaderConvert::DOWNSAMPLE_COPY:
		case ShaderConvert::RGBA_TO_8I:
		case ShaderConvert::RGB5A1_TO_8I:
		case ShaderConvert::CLUT_4:
		case ShaderConvert::CLUT_8:
		case ShaderConvert::YUV:
		case ShaderConvert::COLCLIP_RESOLVE:
			return true;
		default:
			return false;
	}
}

static inline constexpr bool HasFloat32Output(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::RGBA8_TO_DEPTH32:
		case ShaderConvert::RGBA8_TO_DEPTH24:
		case ShaderConvert::RGBA8_TO_DEPTH16:
		case ShaderConvert::RGB5A1_TO_DEPTH16:
		case ShaderConvert::DEPTH_COPY:
		case ShaderConvert::DEPTH32_TO_DEPTH24:
			return true;
		default:
			return false;
	}
}

static inline constexpr bool HasFloat32Input(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::DEPTH_COPY:
		case ShaderConvert::DEPTH32_TO_16_BITS:
		case ShaderConvert::DEPTH32_TO_32_BITS:
		case ShaderConvert::DEPTH32_TO_RGBA8:
		case ShaderConvert::DEPTH32_TO_RGB8:
		case ShaderConvert::DEPTH16_TO_RGB5A1:
		case ShaderConvert::DEPTH32_TO_DEPTH24:
			return true;
		default:
			return false;
	}
}

static inline constexpr bool IsDATMConvertShader(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::DATM_0:
		case ShaderConvert::DATM_1:
		case ShaderConvert::DATM_0_RTA_CORRECTION:
		case ShaderConvert::DATM_1_RTA_CORRECTION:
			return true;
		default:
			return false;
	}
}

static inline constexpr bool HasStencilOutput(ShaderConvert shader)
{
	return IsDATMConvertShader(shader);
}

static inline constexpr int IntegerOutputBpp(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::DEPTH32_TO_32_BITS:
			return 32;
		case ShaderConvert::DEPTH32_TO_16_BITS:
		case ShaderConvert::RGB5A1_TO_16_BITS:
			return 16;
		default:
			return 0;
	}
}

static inline constexpr bool HasColorClipOutput(ShaderConvert shader)
{
	return (shader == ShaderConvert::COLCLIP_INIT);
}

static inline constexpr bool SupportsBilinear(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::RGBA8_TO_DEPTH32:
		case ShaderConvert::RGBA8_TO_DEPTH24:
		case ShaderConvert::RGBA8_TO_DEPTH16:
		case ShaderConvert::RGB5A1_TO_DEPTH16:
			return true;
		default:
			return false;
	}
}

static inline constexpr u32 ShaderConvertWriteMask(ShaderConvert shader)
{
	switch (shader)
	{
		case ShaderConvert::DEPTH32_TO_RGB8:
			return 0x7;
		default:
			return 0xf;
	}
}

static inline constexpr int GetShaderIndexForMask(ShaderConvert shader, int mask)
{
	pxAssert(HasVariableWriteMask(shader));
	int index = mask;
	if (shader == ShaderConvert::RTA_CORRECTION)
		index |= 1 << 4;
	return index;
}

static inline constexpr ShaderConvert SetDATMShader(SetDATM datm)
{
	switch (datm)
	{
	case SetDATM::DATM1_RTA_CORRECTION:
		return ShaderConvert::DATM_1_RTA_CORRECTION;
	case SetDATM::DATM0_RTA_CORRECTION:
		return ShaderConvert::DATM_0_RTA_CORRECTION;
	case SetDATM::DATM1:
		return ShaderConvert::DATM_1;
	case SetDATM::DATM0:
	default:
		return ShaderConvert::DATM_0;
	}
}

const char* ShaderEntryPoint(ShaderConvert value);
const char* ShaderEntryPoint(PresentShader value);
const char* ShaderConvertName(ShaderConvert shader);

class ShaderConvertSelector
{
	union
	{
		struct
		{
			u32 shader : 8; // Main shader
			u32 mask : 8; // Variable color mask
			u32 depth_out : 1; // Depth texture output
			u32 filter : 1; // Shader filter (HW filter is specified separately)
		};

		u32 key;
	} fields;

	static_assert(sizeof(fields) == 4);

public:
	constexpr ShaderConvertSelector(ShaderConvert shader = ShaderConvert::COPY, u8 mask = 0xf,
 		bool depth_out = false, Filter filter = Filter::Nearest)
		: fields { static_cast<u32>(shader) }
	{
		*this = SetMask(mask).SetDepthOutput(depth_out).SetFilter(filter);
	}

	constexpr ShaderConvert Shader() const
	{
		return static_cast<ShaderConvert>(fields.shader);
	}

	constexpr u8 Mask() const
	{
		return fields.mask;
	}

	constexpr u8 DefaultMask() const
	{
		return ShaderConvertWriteMask(Shader());
	}

	constexpr Filter GetFilter() const
	{
		return static_cast<Filter>(fields.filter);
	}

	constexpr bool Biln() const
	{
		return GetFilter() == Filter::Biln;
	}

	constexpr bool Nearest() const
	{
		return GetFilter() == Filter::Nearest;
	}

	constexpr bool SupportsBilinear() const
	{
		return ::SupportsBilinear(Shader());
	}

	constexpr bool ColorOutput() const
	{
		return HasColorOutput(Shader());
	}

	constexpr bool DepthOutput() const
	{
		return fields.depth_out;
	}

	constexpr bool StencilOutput() const
	{
		return HasStencilOutput(Shader());
	}

	constexpr bool DATMConvertShader() const
	{
		return IsDATMConvertShader(Shader());
	}

	constexpr bool Float32Output() const
	{
		return HasFloat32Output(Shader());
	}

	constexpr bool Float32Input() const
	{
		return HasFloat32Input(Shader());
	}

	constexpr int IntegerOutputBpp() const
	{
		return ::IntegerOutputBpp(Shader());
	}

	constexpr bool VariableWriteMask() const
	{
		return HasVariableWriteMask(Shader());
	}

	constexpr bool ColorClipOutput() const
	{
		return HasColorClipOutput(Shader());
	}

	const char* Name() const
	{
		return ShaderConvertName(Shader());
	}

	const char* EntryPoint() const
	{
		return ShaderEntryPoint(Shader());
	}

	constexpr ShaderConvertSelector SetMask(u8 mask = 0xf) const
	{
		ShaderConvertSelector tmp = *this;
		tmp.fields.mask = VariableWriteMask() ? (mask & 0xf) : DefaultMask();
		return tmp;
	}

	constexpr ShaderConvertSelector SetMask(bool wr, bool wg, bool wb, bool wa) const
	{
		return SetMask((wr ? 1 : 0) | (wg ? 2 : 0) | (wb ? 4 : 0) | (wa ? 8 : 0));
	}

	constexpr ShaderConvertSelector SetDepthOutput(bool depth_out) const
	{
		ShaderConvertSelector tmp = *this;
		tmp.fields.depth_out = Float32Output() && depth_out;
		return tmp;
	}

	constexpr ShaderConvertSelector SetFilter(Filter filter) const
	{
		ShaderConvertSelector tmp = *this;
		tmp.fields.filter = static_cast<u32>(SupportsBilinear() ? filter : Filter::Nearest);
		return tmp;
	}

	GSTexture::Format OutputFormat() const
	{
		if (DepthOutput())
			return GSTexture::Format::DepthStencil;
		else if (int bpp = IntegerOutputBpp())
			return bpp == 16 ? GSTexture::Format::UInt16 : GSTexture::Format::UInt32;
		else if (Float32Output())
			return GSTexture::Format::DepthColor;
		else if (ColorOutput())
			return GSTexture::Format::Color;
		else if (ColorClipOutput())
			return GSTexture::Format::ColorClip;
		else
			return GSTexture::Format::Invalid;
	}

private:
	// Helper variables for packing valid shaders into a contiguous range.
	static const std::span<const ShaderConvertSelector> SHADERS;
	static const std::array<u8, static_cast<u32>(ShaderConvert::Count) * 4> INDEX_REMAP;
	static const u32 NUM_REMAPPED_SHADERS;

public:
	static constexpr u32 NUM_VARIABLE_WRITE_MASK_SHADERS = 2;
	static const u32 NUM_TOTAL_SHADERS;

	u32 Index() const
	{
		if (VariableWriteMask() && !fields.depth_out && Nearest())
			return GetShaderIndexForMask(Shader(), fields.mask) + NUM_REMAPPED_SHADERS;
		u32 remapped = INDEX_REMAP[(fields.depth_out << 0) +
		                           (fields.filter    << 1) +
		                           (fields.shader    << 2)];
		pxAssert(remapped < NUM_REMAPPED_SHADERS);
		return remapped;
	}

	// Inverse of Index()
	static ShaderConvertSelector Get(u32 index)
	{
		return SHADERS[index];
	}
};

static inline ShaderConvertSelector GetConvertShader(GSTexture::Format src, GSTexture::Format dst,
	u32 src_bpp = 32, u32 dst_bpp = 32, u8 mask = 0xf)
{
	ShaderConvert shader = static_cast<ShaderConvert>(-1);
	switch (src)
	{
		case GSTexture::Format::Color:
			switch (dst)
			{
				case GSTexture::Format::Color:
					pxAssert(src_bpp == 32 && dst_bpp == 32);
					shader = ShaderConvert::COPY; // bpp is handled by mask
					break;
				case GSTexture::Format::DepthColor:
				case GSTexture::Format::DepthStencil:
					switch (dst_bpp)
					{
						case 32:
							shader = ShaderConvert::RGBA8_TO_DEPTH32;
							break;
						case 24:
							shader = ShaderConvert::RGBA8_TO_DEPTH24;
							break;
						case 16:
							pxAssert(src_bpp == 16 || src_bpp == 32);
							shader = src_bpp == 16 ? ShaderConvert::RGB5A1_TO_DEPTH16 :
							                         ShaderConvert::RGBA8_TO_DEPTH16;
							break;
						default:
							pxAssert(false);
							break;
					}
					break;
				default:
					pxAssert(false);
					break;
			}
			break;
		case GSTexture::Format::DepthColor:
		case GSTexture::Format::DepthStencil:
			switch (dst)
			{
				case GSTexture::Format::Color:
					switch (dst_bpp)
					{
						case 32:
							shader = ShaderConvert::DEPTH32_TO_RGBA8;
							break;
						case 24:
							shader = ShaderConvert::DEPTH32_TO_RGB8;
							break;
						case 16:
							pxAssert(src_bpp == 16);
							shader = ShaderConvert::DEPTH16_TO_RGB5A1;
							break;
						default:
							pxAssert(false);
							break;
					}
					break;
				case GSTexture::Format::DepthColor:
				case GSTexture::Format::DepthStencil:
					switch (dst_bpp)
					{
						case 32:
							pxAssert(src_bpp == 32);
							shader = ShaderConvert::DEPTH_COPY;
							break;
						case 24:
							pxAssert(src_bpp == 32);
							shader = ShaderConvert::DEPTH32_TO_DEPTH24;
							break;
						default:
							pxAssert(false);
							break;
					}
					break;
				default:
					pxAssert(false);
			}
			break;
		default:
			pxAssert(false);
			break;
	}

	return ShaderConvertSelector(shader, mask, dst == GSTexture::Format::DepthStencil);
}

static inline ShaderConvertSelector GetConvertShader(const GSTexture* src, const GSTexture* dst, u32 src_bpp, u32 dst_bpp, u8 mask = 0xf)
{
	return GetConvertShader(src->GetFormat(), dst->GetFormat(), src_bpp, dst_bpp, mask);
}

static inline ShaderConvertSelector GetConvertShaderMask(GSTexture::Format src, GSTexture::Format dst,
	u32 src_bpp, u32 dst_bpp, bool red = true, bool green = true, bool blue = true, bool alpha = true)
{
	const u8 mask = (red ? 1 : 0) | (green ? 2 : 0) | (blue ? 4 : 0) | (alpha ? 8 : 0);
	return GetConvertShader(src, dst, src_bpp, dst_bpp, mask);
}

static inline ShaderConvertSelector GetConvertShaderMask(const GSTexture* src, const GSTexture* dst,
	u32 src_bpp, u32 dst_bpp, bool red = true, bool green = true, bool blue = true, bool alpha = true)
{
	return GetConvertShaderMask(src->GetFormat(), dst->GetFormat(), src_bpp, dst_bpp, red, green, blue, alpha);
}

enum ChannelFetch
{
	ChannelFetch_NONE  = 0,
	ChannelFetch_RED   = 1,
	ChannelFetch_GREEN = 2,
	ChannelFetch_BLUE  = 3,
	ChannelFetch_ALPHA = 4,
	ChannelFetch_RGB   = 5,
	ChannelFetch_GXBY  = 6,
};

enum class HWBlendType
{
	SRC_ONE_DST_FACTOR      = 1, // Use the dest color as blend factor, Cs is set to 1.
	SRC_ALPHA_DST_FACTOR    = 2, // Use the dest color as blend factor, Cs is set to (Alpha - 1).
	SRC_DOUBLE              = 3, // Double source color.
	SRC_HALF_ONE_DST_FACTOR = 4, // Use the dest color as blend factor, Cs is set to 0.5, additionally divide As or Af by 2.
	SRC_INV_DST_BLEND_HALF  = 5, // Halve the alpha then double the final result.
	INV_SRC_DST_BLEND_HALF  = 6, // Halve the alpha then double the final result.

	BMIX1_ALPHA_HIGH_ONE    = 1, // Blend formula is replaced when alpha is higher than 1.
	BMIX1_SRC_HALF          = 2, // Impossible blend will always be wrong on hw, divide Cs by 2.
	BMIX2_OVERFLOW          = 3, // Blending Cs might overflow, try to compensate.
};

struct alignas(16) DisplayConstantBuffer
{
	GSVector4 SourceRect; // +0,xyzw
	GSVector4 TargetRect; // +16,xyzw
	GSVector2 SourceSize; // +32,xy
	GSVector2 TargetSize; // +40,zw
	GSVector2 TargetResolution; // +48,xy
	GSVector2 RcpTargetResolution; // +56,zw
	GSVector2 SourceResolution; // +64,xy
	GSVector2 RcpSourceResolution; // +72,zw
	GSVector4 TimeAndPad; // seconds since GS init +76,xyzw
	// +96

	// assumes that sRect is normalized
	void SetSource(const GSVector4& sRect, const GSVector2i& sSize)
	{
		SourceRect = sRect;
		SourceResolution = GSVector2(static_cast<float>(sSize.x), static_cast<float>(sSize.y));
		RcpSourceResolution = GSVector2(1.0f) / SourceResolution;
		SourceSize = GSVector2((sRect.z - sRect.x) * SourceResolution.x, (sRect.w - sRect.y) * SourceResolution.y);
	}
	void SetTarget(const GSVector4& dRect, const GSVector2i& dSize)
	{
		TargetRect = dRect;
		TargetResolution = GSVector2(static_cast<float>(dSize.x), static_cast<float>(dSize.y));
		RcpTargetResolution = GSVector2(1.0f) / TargetResolution;
		TargetSize = GSVector2(dRect.z - dRect.x, dRect.w - dRect.y);
	}
	void SetTime(float time)
	{
		TimeAndPad = GSVector4(time);
	}
};
static_assert(sizeof(DisplayConstantBuffer) == 96, "DisplayConstantBuffer is correct size");

struct alignas(16) MergeConstantBuffer
{
	GSVector4 BGColor;
	u32 EMODA;
	u32 EMODC;
	u32 DOFFSET;
	float ScaleFactor;
};
static_assert(sizeof(MergeConstantBuffer) == 32, "MergeConstantBuffer is correct size");

struct alignas(16) InterlaceConstantBuffer
{
	GSVector4 ZrH; // data passed to the shader
};
static_assert(sizeof(InterlaceConstantBuffer) == 16, "InterlaceConstantBuffer is correct size");

enum HWBlendFlags
{
	// Flags to determine blending behavior
	BLEND_CD     = 0x1,    // Output is Cd, hw blend can handle it
	BLEND_HW1    = 0x2,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW2    = 0x4,    // Clear color blending (use directly the destination color as blending factor)
	BLEND_HW3    = 0x8,    // Multiply Cs by (255/128) to compensate for wrong Ad/255 value, should be Ad/128
	BLEND_HW4    = 0x10,   // HW rendering is split in 2 passes
	BLEND_HW5    = 0x20,   // HW rendering is split in 2 passes
	BLEND_HW6    = 0x40,   // HW rendering is split in 2 passes
	BLEND_HW7    = 0x80,   // HW rendering is split in 2 passes
	BLEND_HW8    = 0x100,  // HW rendering is split in 2 passes
	BLEND_HW9    = 0x200,  // HW rendering is split in 2 passes
	BLEND_MIX1   = 0x400,  // Mix of hw and sw, do Cs*F or Cs*As in shader
	BLEND_MIX2   = 0x800,  // Mix of hw and sw, do Cs*(As + 1) or Cs*(F + 1) in shader
	BLEND_MIX3   = 0x1000, // Mix of hw and sw, do Cs*(1 - As) or Cs*(1 - F) in shader
	BLEND_ACCU   = 0x2000, // Allow to use a mix of SW and HW blending to keep the best of the 2 worlds
	BLEND_NO_REC = 0x4000, // Doesn't require sampling of the RT as a texture
	BLEND_A_MAX  = 0x8000, // Impossible blending uses coeff bigger than 1
};

// Determines the HW blend function for the video backend
struct HWBlend
{
	typedef u8 BlendOp; /*GSDevice::BlendOp*/
	typedef u8 BlendFactor; /*GSDevice::BlendFactor*/

	u16 flags;
	BlendOp op;
	BlendFactor src, dst;
};

struct alignas(16) GSHWDrawConfig
{
	enum class Topology: u8
	{
		Point,
		Line,
		Triangle,
	};
	using VSExpand = GSShader::VSExpand;
	using PS_ATST  = GSShader::PS_ATST;
	using PS_AFAIL = GSShader::PS_AFAIL;
	using PS_AA1   = GSShader::PS_AA1;
	using PS_ROV_DEPTH = GSShader::PS_ROV_DEPTH;
#pragma pack(push, 1)
	struct VSSelector
	{
		union
		{
			struct
			{
				u8 fst : 1;
				u8 tme : 1;
				u8 iip : 1;
				u8 point_size : 1;		///< Set when points need to be expanded without VS expanding.
				VSExpand expand : 3;
				u8 tile_prim_ord : 1; ///< Tile renderer: emit the flat per-primitive ordinal the fragment walks index their plane payload by (triangles must carry identity indices)
			};
			u8 key;
		};
		VSSelector(): key(0) {}
		VSSelector(u8 k): key(k) {}

		/// Returns true if the fixed index buffer should be used.
		__fi bool UseFixedExpandIndexBuffer() const { return (expand == VSExpand::Point || expand == VSExpand::Sprite); }
		
		/// Return true if the index buffer should be bound as a vertex shader resource.
		__fi bool UseVSExpandIndexBuffer() const { return (expand == VSExpand::TriangleAA1); }
	};
	static_assert(sizeof(VSSelector) == 1, "VSSelector is a single byte");

	struct PSSelector
	{
		// Performance note: there are too many shader combinations
		// It might hurt the performance due to frequent toggling worse it could consume
		// a lots of memory.
		union
		{
			struct
			{
				// Format
				u32 aem_fmt   : 2;
				u32 pal_fmt   : 2;
				u32 dst_fmt   : 2; // 0 → 32-bit, 1 → 24-bit, 2 → 16-bit
				u32 depth_fmt : 2; // 0 → None, 1 → 32-bit, 2 → 16-bit, 3 → RGBA
				// Alpha extension/Correction
				u32 aem : 1;
				u32 fba : 1;
				// Fog
				u32 fog : 1;
				// Flat/goround shading
				u32 iip : 1;
				// Pixel test
				u32 date : 3;
				PS_ATST atst : 3;
				PS_AFAIL afail : 3;
				u32 ztst : 2;
				// Color sampling
				u32 fst : 1; // Investigate to do it on the VS
				u32 tfx : 3;
				u32 tcc : 1;
				u32 wms : 2;
				u32 wmt : 2;
				u32 adjs : 1;
				u32 adjt : 1;
				u32 ltf : 1;
				u32 tile_ltf : 1; // Tile renderer: PS2-exact integer bilinear (1/16 snap, 4-bit nested truncating lerps) via texelFetch
				u32 tile_nn : 1; // Tile renderer: nearest through the in-shader coordinate walk (perspective STQ triangles)
				u32 tile_mip : 2; // Tile renderer: mip mode per the SW scanline selector — 0 off, 1 round, 2 trilinear
				u32 tile_lcm : 1; // Tile renderer: LOD is the constant packed in LODParams.w, not the per-pixel Q formula
				u32 tile_ltfx : 2; // Tile renderer: per-pixel MMAG/MMIN across the LOD crossing — 0 off, 1 linear where minifying, 2 linear where magnifying
				u32 tile_vcolor : 1; // Tile renderer: the SW scanline's vertex-colour arithmetic (seven fractional bits, truncating)
				u32 tile_fog : 1; // Tile renderer: integer fog blend at the console rule
				u32 tile_zwalk : 2; // Tile renderer depth realization — 0 off; 1 the SW scanline's float64 depth walk replayed in soft-float (byte-exact, ~1.5k ops); 2 the closed plane form (integer wraparound + f32 fraction carry, within one unit of the walk at comparator ties — the fast profile's plane-exact contract)
				u32 tile_twalk : 2; // Tile renderer: the texture coordinate from the SW scanline's walk, replayed per fragment off the per-primitive payload — 0 off, 1 triangle blocks, 2 sprite blocks
				u32 tile_twalk_fst : 1; // Tile renderer: that walk is the truncating integer DDA (the software renderer's effective sel.fst), else the accumulating float one with the truncated per-pixel reciprocal
				u32 tile_cwalk : 1; // Tile renderer: gouraud colour and fog from the SW scanline's blocked walk, replayed per fragment off the per-primitive edge payload — not from the interpolator
				u32 tile_tclag : 1; // Tile renderer: a non-sprite coordinate trails the exact plane by one 16.16 unit on each axis walking forward (gs-shade console rule)
				u32 tile_snap : 1; // Tile renderer: floor the texel-space coordinate to 1/16 before sampling — the console's coordinate quantisation kept when the filter's weights move to the hardware sampler (fast profile)
				u32 tile_blend_mix : 1; // Tile renderer: blend-mix offsets at the exact-floor constant (127/256) instead of Classic's reduced-precision-ROP compromise (124/256)
				u32 tile_blend : 1; // Tile renderer: the whole blend equation in the console's integer arithmetic over a read destination — (((A−B)·C)>>7)+D, the shift ARITHMETIC
				u32 tile_direct_idx : 3; // Tile renderer: the index texture IS a colour target, addressed through the GS swizzle per fetch — 0 off, else GSTileSwizzleForms::IndexFormat + 1
				u32 tile_direct_pal : 1; // Tile renderer: the palette IS a colour target holding the CLUT's source words, gathered per fetch through the loaders' word order

				// Shuffle and fbmask effect
				u32 shuffle  : 1;
				u32 shuffle_same : 1;
				u32 real16src: 1;
				u32 process_ba : 2;
				u32 process_rg : 2;
				u32 shuffle_across : 1;
				u32 write_rg : 1;
				u32 fbmask   : 1;

				// Blend and Colclip
				u32 blend_a        : 2;
				u32 blend_b        : 2;
				u32 blend_c        : 2;
				u32 blend_d        : 2;
				u32 fixed_one_a    : 1;
				u32 blend_hw       : 3; /*HWBlendType*/
				u32 a_masked       : 1;
				u32 colclip_hw     : 1; // colclip (COLCLAMP off) emulation through HQ textures
				u32 rta_correction : 1;
				u32 rta_source_correction : 1;
				u32 colclip        : 1; // COLCLAMP off (color blend outputs wrap around 0-255)
				u32 blend_mix      : 2;
				u32 round_inv      : 1; // Blending will invert the value, so rounding needs to go the other way
				u32 pabe           : 1;
				u32 no_color       : 1; // disables color output entirely (depth only)
				u32 no_color1      : 1; // disables second color output (when unnecessary)
				u32 blend_factor_in_alpha : 1; // writes the blend factor to the first output's alpha instead of the second output (no dual-source blend)

				// Others ways to fetch the texture
				u32 channel : 3;

				// Dithering
				u32 dither : 2;
				u32 dither_adjust : 1;

				// Depth writing
				u32 zclamp : 1;
				u32 zfloor : 1;

				// Hack
				u32 tcoffsethack : 1;
				u32 urban_chaos_hle : 1;
				u32 tales_of_abyss_hle : 1;
				u32 tex_is_fb : 1; // Jak Shadows
				u32 automatic_lod : 1;
				u32 manual_lod : 1;
				u32 point_sampler : 1;
				u32 region_rect : 1;

				// Scan mask
				u32 scanmsk : 2;

				// AA1
				PS_AA1 aa1 : 2; // Pixel shader AA1 primitive. Must be used in conjunction with VS AA1 expand.
				u32 abe : 1; // Alpha blend enabled. Currently only used for emulating AA1/ABE interaction.

				// Anisotropic filtering
				u32 sw_aniso : 5;
				
				// ROVs
				u32 rov_color : 1;
				PS_ROV_DEPTH rov_depth : 2;
			};

			struct
			{
				u64 key_lo;
				u64 key_hi;
			};
		};
		__fi PSSelector() : key_lo(0), key_hi(0) {}

		__fi bool operator==(const PSSelector& rhs) const { return (key_lo == rhs.key_lo && key_hi == rhs.key_hi); }
		__fi bool operator!=(const PSSelector& rhs) const { return (key_lo != rhs.key_lo || key_hi != rhs.key_hi); }
		__fi bool operator<(const PSSelector& rhs) const { return (key_lo < rhs.key_lo || key_hi < rhs.key_hi); }

		__fi bool IsSWBlending() const
		{
			return blend_a || blend_b || blend_d;
		}

		__fi bool IsZTesting() const
		{
			return ztst == ZTST_GEQUAL || ztst == ZTST_GREATER;
		}

		__fi bool IsAlphaTesting() const
		{
			return atst != PS_ATST::NONE;
		}

		__fi bool IsFeedbackLoopRT() const
		{
			const u32 sw_blend_bits = blend_a | blend_b | blend_d;
			const bool sw_blend_needs_rt = (sw_blend_bits != 0 && ((sw_blend_bits | blend_c) & 1u)) || ((a_masked & blend_c) != 0);
			const bool afail_needs_rt = afail == PS_AFAIL::ZB_ONLY || afail == PS_AFAIL::RGB_ONLY || afail == PS_AFAIL::RGB_ONLY_SW_Z;
			return tex_is_fb || fbmask || (date >= 5) || sw_blend_needs_rt || afail_needs_rt;
		}

		__fi bool IsFeedbackLoopDepth() const
		{
			const bool afail_needs_depth = afail == PS_AFAIL::FB_ONLY || afail == PS_AFAIL::RGB_ONLY_SW_Z;
			const bool ztst_needs_depth = ztst == ZTST_GEQUAL || ztst == ZTST_GREATER;
			const bool aa1_needs_depth = aa1 == PS_AA1::TRIANGLE_SW_Z;
			return afail_needs_depth || ztst_needs_depth || aa1_needs_depth;
		}

		__fi bool HasShaderDiscard() const
		{
			return (IsAlphaTesting() && afail == PS_AFAIL::KEEP) || scanmsk || date || IsZTesting();
		}

		/// Disables color output from the pixel shader, this is done when all channels are masked.
		__fi void DisableColorOutput()
		{
			// remove software blending, since this will cause the color to be declared inout with fbfetch.
			blend_a = blend_b = blend_c = blend_d = 0;

			// TEX_IS_FB relies on us having a color output to begin with.
			tex_is_fb = 0;

			// no point having fbmask, since we're not writing. DATE has to stay.
			fbmask = 0;

			// disable both outputs.
			no_color = no_color1 = 1;
		}

		/// Disables depth output from the pixel shader.
		__fi void DisableDepthOutput()
		{
			if (afail == PS_AFAIL::RGB_ONLY_SW_Z)
			{
				afail = PS_AFAIL::RGB_ONLY;
			}

			if (aa1 == PS_AA1::TRIANGLE_SW_Z)
			{
				aa1 = PS_AA1::TRIANGLE;
			}

			if (rov_depth == PS_ROV_DEPTH::READ_WRITE)
			{
				rov_depth = PS_ROV_DEPTH::READ_ONLY;
			}
		}

		__fi bool HasColorOutput() const
		{
			return !no_color;
		}

		__fi bool HasDepthOutput() const
		{
			return zfloor || zclamp || IsFeedbackLoopDepth() || (rov_depth == PS_ROV_DEPTH::READ_WRITE);
		}

		__fi bool HasColorROV() const
		{
			return rov_color != 0;
		}

		__fi bool HasDepthROV() const
		{
			return rov_depth != PS_ROV_DEPTH::NONE;
		}

		__fi bool HasDepthROVWrite() const
		{
			return rov_depth == PS_ROV_DEPTH::READ_WRITE;
		}
	};
	static_assert(sizeof(PSSelector) == 16, "PSSelector is 12 bytes");
#pragma pack(pop)
	struct PSSelectorHash
	{
		std::size_t operator()(const PSSelector& p) const
		{
			std::size_t h = 0;
			HashCombine(h, p.key_lo, p.key_hi);
			return h;
		}
	};
#pragma pack(push, 1)
	struct SamplerSelector
	{
		union
		{
			struct
			{
				u8 tau      : 1;
				u8 tav      : 1;
				u8 biln     : 1;
				u8 triln    : 3;
				u8 lodclamp : 1;
			};
			u8 key;
		};
		SamplerSelector(): key(0) {}
		SamplerSelector(u8 k): key(k) {}
		static SamplerSelector Point() { return SamplerSelector(); }
		static SamplerSelector Linear()
		{
			SamplerSelector out;
			out.biln = 1;
			return out;
		}

		/// Returns true if the effective minification filter is linear.
		__fi bool IsMinFilterLinear() const
		{
			if (triln < static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest))
			{
				// use the same filter as mag when mipmapping is off
				return biln;
			}
			else
			{
				// Linear_Mipmap_Nearest or Linear_Mipmap_Linear
				return (triln >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest));
			}
		}

		/// Returns true if the effective magnification filter is linear.
		__fi bool IsMagFilterLinear() const
		{
			// magnification uses biln regardless of mip mode (they're only used for minification)
			return biln;
		}

		/// Returns true if the effective mipmap filter is linear.
		__fi bool IsMipFilterLinear() const
		{
			return (triln == static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Linear) ||
					triln == static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear));
		}

		/// Returns true if mipmaps should be used when filtering (i.e. LOD not clamped to zero).
		__fi bool UseMipmapFiltering() const
		{
			return (triln >= static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest));
		}
	};
	struct DepthStencilSelector
	{
		union
		{
			struct
			{
				u8 ztst : 2;
				u8 zwe  : 1;
				u8 date : 1;
				u8 date_one : 1;

				u8 _free : 3;
			};
			u8 key;
		};
		constexpr DepthStencilSelector(): key(0) {}
		constexpr DepthStencilSelector(u8 k): key(k) {}
		static constexpr DepthStencilSelector NoDepth()
		{
			DepthStencilSelector out;
			out.ztst = ZTST_ALWAYS;
			return out;
		}
	};
	struct ColorMaskSelector
	{
		union
		{
			struct
			{
				u8 wr : 1;
				u8 wg : 1;
				u8 wb : 1;
				u8 wa : 1;

				u8 _free : 4;
			};
			struct
			{
				u8 wrgba : 4;
			};
			u8 key;
		};
		constexpr ColorMaskSelector(): key(0xF) {}
		constexpr ColorMaskSelector(u8 c): key(0) { wrgba = c; }
	};

#pragma pack(pop)
	struct alignas(16) VSConstantBuffer
	{
		GSVector2 vertex_scale;
		GSVector2 vertex_offset;
		GSVector2 texture_scale;
		GSVector2 texture_offset;
		GSVector2 point_size;
		u32 max_depth;
		float line_aa1_width;
		__fi VSConstantBuffer()
		{
			memset(static_cast<void*>(this), 0, sizeof(*this));
		}
		__fi VSConstantBuffer(const VSConstantBuffer& other)
		{
			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
		}
		__fi VSConstantBuffer& operator=(const VSConstantBuffer& other)
		{
			new (this) VSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const VSConstantBuffer& other) const
		{
			return BitEqual(*this, other);
		}
		__fi bool operator!=(const VSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const VSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
			return true;
		}
	};

	struct alignas(16) VSPushConstants
	{
		u32 base_vertex;
		u32 base_index;
		u32 _pad0;
		u32 _pad1;

		__fi VSPushConstants()
		{
			memset(static_cast<void*>(this), 0, sizeof(*this));
		}
		__fi VSPushConstants(const VSPushConstants& other)
		{
			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
		}
		__fi VSPushConstants& operator=(const VSPushConstants& other)
		{
			new (this) VSPushConstants(other);
			return *this;
		}
		__fi bool operator==(const VSPushConstants& other) const
		{
			return BitEqual(*this, other);
		}
		__fi bool operator!=(const VSPushConstants& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const VSPushConstants& other)
		{
			if (*this == other)
				return false;

			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
			return true;
		}
	};
	static_assert(sizeof(VSPushConstants) == 16, "VSPushConstants wrong size");

	struct alignas(16) PSConstantBuffer
	{
		GSVector4 FogColor_AREF;
		GSVector4 WH;
		GSVector4 TA_MaxDepth_Af;
		GSVector4i FbMask;

		GSVector4 HalfTexel;
		GSVector4 MinMax;
		GSVector4 LODParams;
		GSVector4 STRange;
		GSVector4i ChannelShuffle;
		GSVector2 ChannelShuffleOffset;
		GSVector2 TCOffsetHack;
		GSVector2 STScale;

		GSVector4 DitherMatrix[4];

		GSVector4 ScaleFactor;
		float LineCovScale;
		// Tile renderer. TileLtfxQ is the Q at which the level of detail crosses
		// zero, for the per-pixel MMAG/MMIN choice; it is read only when tile_ltfx
		// is set. TileZBase, TileTWBase and TileCBase are the uvec4 element indices
		// of this draw's depth-walk, texture-coordinate-walk and colour-walk blocks
		// in the vertex-stream storage buffer (bit-cast into the float slot the way
		// MinMax carries its region bounds), stamped by the device at upload time;
		// each is read only when its own selector bit is set.
		float TileLtfxQ;
		float TileZBase;
		float TileTWBase;
		float TileCBase;
		float _pad_tile[3];
		// Tile renderer, direct sampling of a colour target through the GS swizzle
		// (PS_TILE_DIRECT_IDX / PS_TILE_DIRECT_PAL): the index window's TBP0 and
		// pages-per-row and the owner's base and pages-per-row; the palette's CBP,
		// its owner's base and pages-per-row, and (entry-kind << 8) | first entry.
		GSVector4i TileDirectIdx;
		GSVector4i TileDirectPal;

		__fi PSConstantBuffer()
		{
			memset(static_cast<void*>(this), 0, sizeof(*this));
		}
		__fi PSConstantBuffer(const PSConstantBuffer& other)
		{
			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
		}
		__fi PSConstantBuffer& operator=(const PSConstantBuffer& other)
		{
			new (this) PSConstantBuffer(other);
			return *this;
		}
		__fi bool operator==(const PSConstantBuffer& other) const
		{
			return BitEqual(*this, other);
		}
		__fi bool operator!=(const PSConstantBuffer& other) const
		{
			return !(*this == other);
		}
		__fi bool Update(const PSConstantBuffer& other)
		{
			if (*this == other)
				return false;

			memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(*this));
			return true;
		}
	};
	// For hardware rendering backends
	struct BlendState
	{
		typedef u8 BlendOp; /*GSDevice::BlendOp*/
		typedef u8 BlendFactor; /*GSDevice::BlendFactor*/

		union
		{
			struct
			{
				bool enable : 1;
				bool constant_enable : 1;
				BlendOp op : 6;
				BlendFactor src_factor : 4;
				BlendFactor dst_factor : 4;
				BlendFactor src_factor_alpha : 4;
				BlendFactor dst_factor_alpha : 4;
				u8 constant;
			};
			u32 key;
		};
		constexpr BlendState(): key(0) {}
		constexpr BlendState(bool enable_, BlendFactor src_factor_, BlendFactor dst_factor_, BlendOp op_,
			BlendFactor src_alpha_factor_, BlendFactor dst_alpha_factor_, bool constant_enable_, u8 constant_)
			: key(0)
		{
			enable = enable_;
			constant_enable = constant_enable_;
			src_factor = src_factor_;
			dst_factor = dst_factor_;
			op = op_;
			src_factor_alpha = src_alpha_factor_;
			dst_factor_alpha = dst_alpha_factor_;
			constant = constant_;
		}

		// Blending has no effect if RGB is masked.
		bool IsEffective(ColorMaskSelector colormask) const;
	};

	enum class AlphaTestMode
	{
		NONE,
		KEEP,
		FEEDBACK,
		SIMPLE_FB_ONLY,
		SIMPLE_RGB_ONLY,
		SPLIT_RGB_ONLY,
		PASS_THEN_FAIL,
		NEVER,
		ABORT_DRAW
	};

	static bool HasAlphaTestSecondPass(AlphaTestMode method)
	{
		return method == AlphaTestMode::SIMPLE_FB_ONLY ||
		       method == AlphaTestMode::SIMPLE_RGB_ONLY ||
		       method == AlphaTestMode::SPLIT_RGB_ONLY ||
		       method == AlphaTestMode::PASS_THEN_FAIL ||
		       method == AlphaTestMode::NEVER;
	}

	enum class DestinationAlphaMode : u8
	{
		Off,            ///< No destination alpha test
		Stencil,        ///< Emulate using read-only stencil
		StencilOne,     ///< Emulate using read-write stencil (first write wins)
		PrimIDTracking, ///< Emulate by tracking the primitive ID of the last pixel allowed through
		Full,           ///< Full emulation (using barriers / ROV)
	};

	enum class ColClipMode : u8
	{
		NoModify = 0,
		ConvertOnly = 1,
		ResolveOnly = 2,
		ConvertAndResolve = 3,
		EarlyResolve = 4
	};

	GSTexture* rt;         ///< Render target
	GSTexture* ds;         ///< Depth stencil
	GSTexture* tex;        ///< Source texture
	GSTexture* pal;        ///< Palette texture
	const GSVertex* verts; ///< Vertices to draw
	const u16* indices;    ///< Indices to draw
	u32 nverts;            ///< Number of vertices
	u32 nindices;          ///< Number of indices
	u32 indices_per_prim;  ///< Number of indices that make up one primitive
	/// Tile per-primitive plane payload: the depth walk's blocks and the texture
	/// coordinate walk's blocks and the colour walk's blocks concatenated into ONE upload, because
	/// two reservations against the streaming buffer can hit a mid-draw flush between
	/// them and strand the first one's offset. The `*_at` fields are uvec4 element offsets INTO the payload;
	/// the device adds the upload's own base and stamps the results into the PS
	/// constants. 0xFFFFFFFF means that walk has no blocks in this draw.
	const u32* tile_payload;
	u32 tile_payload_size; ///< Size of the whole payload in uvec4 elements (0 = none)
	u32 tile_zwalk_at;
	u32 tile_twalk_at;
	u32 tile_cwalk_at;
	const std::vector<size_t>* drawlist;          ///< For reducing barriers on sprites
	const std::vector<GSVector4i>* drawlist_bbox; ///< For RT copy when barriers not available.
	GSVector4i scissor; ///< Scissor rect
	GSVector4i drawarea; ///< Area in the framebuffer which will be modified.
	GSVector4i samplearea; ///< Area in the texture which will be sampled.
	Topology topology;  ///< Draw topology

	alignas(8) PSSelector ps;
	VSSelector vs;

	BlendState blend;
	SamplerSelector sampler;
	ColorMaskSelector colormask;
	DepthStencilSelector depth;

	bool require_one_barrier;  ///< Require texture barrier before draw (also used to requst an rt copy if texture barrier isn't supported)
	bool require_full_barrier; ///< Require texture barrier between all prims

	enum : u32
	{
		TEX_HAZARD_NONE,
		TEX_HAZARD_RT,
		TEX_HAZARD_DEPTH,
	} tex_hazard;

	AlphaTestMode alpha_test;

	DestinationAlphaMode destination_alpha;
	SetDATM datm;
	bool line_expand;

	struct AlphaPass
	{
		alignas(8) PSSelector ps;
		bool enable : 1;
		bool require_one_barrier : 1;
		bool require_full_barrier : 1;
		ColorMaskSelector colormask;
		DepthStencilSelector depth;
		float ps_aref;
	};
	static_assert(sizeof(AlphaPass) == 24, "alpha pass is 24 bytes");

	AlphaPass alpha_second_pass;

	struct BlendMultiPass
	{
		BlendState blend;
		bool enable : 1;
		u8 no_color1 : 1;
		u8 blend_hw : 3; // HWBlendType
		u8 dither : 2;
	};
	static_assert(sizeof(BlendMultiPass) == 8, "blend multi pass is 8 bytes");

	BlendMultiPass blend_multi_pass;

	VSConstantBuffer cb_vs;
	PSConstantBuffer cb_ps;
	
	// These are here as they need to be preserved between draws, and the state clear only does up to the constant buffers.
	ColClipMode colclip_mode;
	GIFRegFRAME colclip_frame;
	GSVector4i colclip_update_area; ///< Area in the framebuffer which colclip will modify;

	__fi bool IsFeedbackLoopRT(const PSSelector& ps) const
	{
		return ps.IsFeedbackLoopRT() || (tex_hazard == TEX_HAZARD_RT);
	}

	__fi bool IsFeedbackLoopDepth(const PSSelector& ps) const
	{
		return ps.IsFeedbackLoopDepth() || (tex_hazard == TEX_HAZARD_DEPTH);
	}
	
	bool IsBlending()
	{
		return blend.enable || blend_multi_pass.enable || ps.IsSWBlending();
	}

	/// Maps a PS2 alpha test onto the four comparisons the shader implements.
	///
	/// The GS compares eight-bit integers, so the four missing comparisons come from
	/// nudging AREF by half a step: LESS is LEQUAL against a reference a hair below,
	/// GREATER is GEQUAL against one a hair below the next integer. The nudge is two
	/// ULP at the top of the range, so it can never land between two representable
	/// alphas. invert_test asks for the complement instead, which is what a second
	/// pass over the failing fragments needs.
	///
	/// Shared by both hardware renderers: Classic reaches it from EmulateAlphaTest,
	/// Tile from its draw lowering's pass split. One definition, because a drift
	/// between them would be a silent one-level difference at the test boundary.
	static void GetAlphaTestPS(u32 atst, u8 aref, bool invert_test, PS_ATST& ps_atst_out, float& aref_out)
	{
		static constexpr u32 inverted_atst[] = {
			ATST_ALWAYS, ATST_NEVER, ATST_GEQUAL, ATST_GREATER, ATST_NOTEQUAL, ATST_LESS, ATST_LEQUAL, ATST_EQUAL};

		constexpr float small_val = 0x100p-23f;

		switch (invert_test ? inverted_atst[atst & 7] : atst)
		{
			case ATST_LESS:
				aref_out = static_cast<float>(aref) - small_val;
				ps_atst_out = PS_ATST::LEQUAL;
				break;
			case ATST_LEQUAL:
				aref_out = static_cast<float>(aref) - small_val + 1.0f;
				ps_atst_out = PS_ATST::LEQUAL;
				break;
			case ATST_GEQUAL:
				aref_out = static_cast<float>(aref) - small_val;
				ps_atst_out = PS_ATST::GEQUAL;
				break;
			case ATST_GREATER:
				aref_out = static_cast<float>(aref) - small_val + 1.0f;
				ps_atst_out = PS_ATST::GEQUAL;
				break;
			case ATST_EQUAL:
				aref_out = static_cast<float>(aref);
				ps_atst_out = PS_ATST::EQUAL;
				break;
			case ATST_NOTEQUAL:
				aref_out = static_cast<float>(aref);
				ps_atst_out = PS_ATST::NOTEQUAL;
				break;
			case ATST_NEVER:
			case ATST_ALWAYS:
			default:
				ps_atst_out = PS_ATST::NONE;
				break;
		}
	}

	// Dumping
	static void DumpConfig(const std::string& path, const GSHWDrawConfig& conf,
		bool ps = true, bool vs = true, bool bs = true, bool dss = true, bool ss = true, bool asp = true, bool bmp = true,
		bool cbvs = true, bool cbps = true);
};

static inline u32 GetExpansionFactor(GSHWDrawConfig::VSExpand expand)
{
	switch (expand)
	{
		case GSHWDrawConfig::VSExpand::Point:
		case GSHWDrawConfig::VSExpand::Line:
		case GSHWDrawConfig::VSExpand::LineAA1:
			return 4;
		case GSHWDrawConfig::VSExpand::Sprite:
			return 2;
		case GSHWDrawConfig::VSExpand::TriangleAA1:
			return 13;
		default:
			return 1;
	}
}

static inline u32 GetVertexAlignment(GSHWDrawConfig::VSExpand expand)
{
	switch (expand)
	{
		case GSHWDrawConfig::VSExpand::Sprite:
			// Sprite expand does a 2-4 expansion, and relies on the low bit of the vertex ID to figure out if it's the first or second coordinate.
			return 2;
		default:
			return 1;
	}
}

/// The TileGpu source-descriptor ring: how many sets the device keeps for rule 3's frame-wide
/// materialised sources. One set is written per PLAN, and a set may not be rewritten while a
/// submission that reads it is still in flight — so when the ring comes round early the GS thread
/// blocks on that set's fence, mid-plan (GSDeviceVK::GpuWaitCause::SourceSet).
///
/// Depth is therefore a wait-count lever and nothing else. Every set carries identical descriptors;
/// which physical index a plan lands on cannot reach a pixel.
///
/// ⚠️ The default is 32 rather than the 8 it shipped as, decided 2026-08-24 against a measured
/// write rate. A plan flush writes a set, and the corpus's plan-flush rate spans two orders of
/// magnitude: Katamari 1.5–2 writes a frame, SotC 5.9, Ace Combat 5 6.8, Yu-Gi-Oh 12.5, OutRun 42,
/// GT4 52. At depth 8 the ring reached back four frames on Katamari and two thirds of ONE frame on
/// Yu-Gi-Oh — and the SD865's steady-state blocking waits ranked in exactly that order (Katamari
/// 0.00 a frame, SotC and AC5 0.97, Yu-Gi-Oh 2.43). The cost of depth is descriptor memory and
/// nothing else: a set is kMaxSources = 128 combined image samplers, and Turnip gives a combined
/// image sampler 2 × 16 dwords = 128 bytes (`descriptor_size`, tu_descriptor_set.cc), so a set is
/// 16 KB on Adreno and 8 → 32 buys four times the reach for 384 KB. The ceiling of 64 is 1 MB.
constexpr u32 kGSTileGpuSourceSetRingDefault = 32;
constexpr u32 kGSTileGpuSourceSetRingMin = 2;
constexpr u32 kGSTileGpuSourceSetRingMax = 64;

/// The effective ring depth: what EmuCore/GS/TileGpuSourceSetRingDepth says, or the built-in
/// default where it says nothing. Two states, not three — a negative has no meaning here (there is
/// no "uncapped" ring) and reads as the default rather than as an error, because the alternative is
/// a run that refuses to start over a typo in a dev-only key.
constexpr u32 gsTileGpuSourceSetRingDepth(int setting, u32 builtin_default = kGSTileGpuSourceSetRingDefault)
{
	const u32 want = (setting > 0) ? static_cast<u32>(setting) : builtin_default;
	if (want < kGSTileGpuSourceSetRingMin)
		return kGSTileGpuSourceSetRingMin;
	if (want > kGSTileGpuSourceSetRingMax)
		return kGSTileGpuSourceSetRingMax;
	return want;
}

/// The TileGpu pass-tail kick: how often the executor OFFERS to submit the work it has recorded so
/// far, at a pass boundary, instead of holding the whole frame for one submission at the end.
///
/// The kick landed (92f0e375d8) gated on being near a READBACK, because that is the case it was
/// ported for: a pull fence-waits on everything already recorded, so submitting early lets the GPU
/// have that backlog done before the pull asks. That gate is exactly right for what it was measured
/// on and it leaves the larger case unserved. TileGpu submits its whole frame in one act, so on a
/// title with no readbacks at all the GPU cannot start until the GS thread has finished recording,
/// and the frame is a strict alternation: the CPU records against an empty queue, submits, blocks;
/// the GPU runs against an idle CPU. Measured on the SD865, Stuntman is idle on the GPU for 35.99 ms
/// of a 90.50 ms drawn frame, and the GS thread records for 39.20 -- idle/record 0.91, on three
/// independent arms. The kick is the machinery that fills that bubble and it fired ZERO times a
/// frame there, because Stuntman reads nothing back.
///
/// So the cadence below is a SECOND trigger for the same kick, not a replacement for the first:
/// a fixed number of render passes since the last submit, on every frame, readback or not.
///
/// PIXEL-INERT by construction, and this is the property the whole road rests on. What a kick
/// changes is WHEN recorded work is submitted, never what is recorded: queue submissions execute in
/// order, the plan's stream reservations are retained across the boundary, and every per-pass bind
/// is re-established inside its pass. A byte difference between two cadences is a pre-existing
/// ordering defect, not a trade.
constexpr u32 kGSTileGpuKickPassCadenceDefault = 32;

/// The near-readback trigger's own threshold, unchanged since the kick landed. Named here rather
/// than left a literal at the call site because the cadence has to be read against it: the two
/// triggers are OR'd, so the effective threshold on a frame near a readback is the smaller of the
/// two, and at the shipped 32 that is still this 8.
constexpr u32 kGSTileGpuKickReadbackThreshold = 8;

/// The effective cadence: what EmuCore/GS/TileGpuKickPassCadence says. ZERO is off and means the
/// near-readback trigger stands alone, which is the arm this shipped as and the control arm of the
/// device A/B. Anything below zero is off as well -- a negative cadence has no second meaning to
/// carry, and a dev-only key should fall back to the previously shipped behaviour on a typo rather
/// than refuse the run. There is no upper clamp: a cadence larger than any frame's pass count is
/// simply a cadence that never fires, and rewriting it to some ceiling would change what a device
/// record's arm meant.
constexpr u32 gsTileGpuKickPassCadence(int setting)
{
	if (setting <= 0)
		return 0;
	return static_cast<u32>(setting);
}

/// Does the pass-tail kick want to offer a submit here? The whole composition, in one place, so a
/// unit test can hold it: the two triggers are independent and OR'd.
///
///   - the NEAR-READBACK trigger, unchanged: on a frame within the readback window, either
///     `readback_threshold` passes have been recorded since the last submit, or this pass wrote a
///     texture a recent pull read -- which almost certainly means it is producing the data for the
///     next pull, so it kicks whatever the count says (but not at zero passes, which would submit
///     nothing).
///   - the CADENCE trigger, additive: `cadence` passes since the last submit, on any frame.
///
/// Additive rather than a replacement because the two answer different questions. The near-readback
/// trigger is worth +14.6% on GT4 Online Public Beta on the SD865 and its threshold of 8 was fitted
/// against that; the cadence is fitted at 32 against the bubble, and pushing the readback trigger
/// out to 32 with it would retune a landed win nothing in this round measured.
///
/// This decides only whether to OFFER. What actually goes is decided by the caller's never-block
/// guard -- the kick fires only when the NEXT command buffer has verifiably retired -- and on the
/// shipped ring that guard declines the large majority of offers.
constexpr bool gsTileGpuKickWantsSubmit(u32 passes_since_submit, u32 cadence, bool near_readback,
	bool produces_readback_data, u32 readback_threshold = kGSTileGpuKickReadbackThreshold)
{
	if (near_readback && (passes_since_submit >= readback_threshold ||
							 (produces_readback_data && passes_since_submit > 0)))
		return true;
	return cadence != 0 && passes_since_submit >= cadence;
}

/// The kick PREDICTOR's constants (EmuCore/GS/TileGpuAdaptiveKick).
///
/// Everything here is integer arithmetic on nanoseconds and counts. A frame's verdict must not
/// depend on how a float rounded on one architecture.
enum : u32
{
	/// The latched bubble decays by 1/256 of itself per DRAWN frame. Two numbers meet here.
	///
	/// It has to decay at all: the cadence, when it works, HIDES the bubble it is filling, so a
	/// peak measured while the cadence was off can never be refreshed while the cadence is on. A
	/// peak that never decayed would latch the cadence on for the rest of the run off one scene.
	///
	/// It has to decay SLOWLY: the only way to re-measure an undistorted bubble is to spend a frame
	/// with the cadence off, and that frame costs whatever the cadence was worth. 1/256 halves the
	/// peak in 177 drawn frames (~3 s at 60 Hz), so the re-measurement costs at most the confirm
	/// window -- four frames in ~180, under 2.5% -- and a scene that genuinely quietens still
	/// re-prices within seconds.
	kGSTileGpuKickBubbleDecayShift = 8,
	/// Entering the ON state needs the bubble to be worth TWICE the price; staying needs it worth
	/// the price once. The 2x band is the hysteresis, the same shape and the same reason as the
	/// depth predictor's 1/16-on-1/32-off: the metric is a per-frame quantity and a title sitting
	/// on the threshold would otherwise re-cut its submission cadence every frame.
	kGSTileGpuKickEnterMultiplier = 2,
	/// Consecutive frames that must ask for the other state before it is taken. Two, for the
	/// depth predictor's reason: the band above answers a metric sitting on the threshold, this
	/// only removes a single anomalous frame (a load screen, a full-screen wipe).
	kGSTileGpuKickConfirmFrames = 2,
};

/// The per-frame submission-cadence policy, and its census.
///
/// ⚠️ THE PROBLEM THIS SHAPE EXISTS TO SOLVE, because every simpler shape was tried against the
/// 22-title x 2-device grid first and every one of them oscillates. The cadence's whole job is to
/// remove GPU-blocking wait. So on a title where it WORKS, the wait it was justified by is gone,
/// and any predictor that re-reads the wait each frame reads "no bubble here" and switches itself
/// off -- on Stuntman, the -33% case the cadence exists for. You cannot observe a bubble you have
/// filled.
///
/// So the credit is LATCHED, not re-measured: `bubble_ns` is the PEAK blocking wait seen, decayed
/// slowly, exactly the shape the declaring budget's per-class peak uses and for the same reason.
/// While the cadence is off the frame is undistorted and the peak is the truth; while it is on the
/// frame's own wait is only a FLOOR under the peak, and the decay is what eventually forces a
/// re-measurement rather than letting one scene latch the lever for a run.
///
/// THE DECISION, in one line: the cadence stays on while the bubble it is fighting is worth more
/// than the cadence costs to run, both measured in nanoseconds on THIS device.
///
///   credit   `bubble_ns`, the latched peak of the frame's GPU-blocking wait -- sync +
///            out-of-band + source-set wrap, the GSPerfMon::GpuBlockingWaits population exactly.
///            Ring backpressure is deliberately NOT in it: the census calls it "not a drain" for a
///            reason, and it is a symptom of submitting more, so it belongs on neither side rather
///            than on the credit side.
///   debit    what the cadence's own machinery cost the GS thread. MEASURED, not assumed, and this
///            is what makes the predictor per-device with no vendor gate anywhere in it: on the
///            SD865 (Turnip) a mid-frame submit costs the recording thread ~0.11 ms and on the
///            RG477V ~0.02 ms, and Flatout 2 -- same title, same ~500 render passes, same ~7
///            submits a frame -- is +17.2% on the first device and -8.7% on the second. Nothing
///            structural separates those two runs. The price of a submit does.
///
/// When the cadence is OFF there is no measured cost to read, so the debit is the counterfactual:
/// this frame's render passes at the rate the cadence has already been seen to charge PER RENDER
/// PASS on this device.
///
/// ⚠️ PER PASS, not per submit, and the first cut of this got it wrong. The cadence's bill is not
/// the submits: it is the OFFERS, one at every pass tail past the cadence, each costing a
/// ScanForCommandBufferCompletion whether or not the fence gate lets the submit go -- and the gate
/// declines the large majority (Spider-Man 3 on the M2: 380 offers a frame, 19 taken). Priced per
/// submit, that whole offer bill lands on the few that went, inflating the unit price ~20x; then
/// the OFF state multiplies that inflated price by `render_passes / cadence`, which over-counts the
/// submits by the same ~20x the gate declined. Two errors compounding, and they do not cancel: the
/// two states end up pricing the same frame differently and the predictor flips between them. In
/// the M2 census that cost Spider-Man 3 244 switches in 1,548 frames. Per pass, both states price
/// "what the cadence costs on a frame this size" and agree by construction.
///
/// ⚠️ A ZERO debit keeps the cadence ON, and that is not a fallback, it is the rule: a device that
/// has not yet priced a submit has produced no evidence, and a lever that costs nothing measurable
/// has nothing to back off from. The predictor may only ever move off the shipped arrangement
/// (cadence on) after it has seen a price.
///
/// Starts ON for the same reason.
struct GSTileGpuKickPolicyPicker
{
	bool on = true;        ///< the cadence's state -- the value the executor reads
	u32 confirming = 0;    ///< consecutive frames that have asked for the other state
	u32 switches = 0;      ///< times the state actually changed -- the churn column
	u32 frames = 0;        ///< frames observed
	u32 frames_on = 0;     ///< ...of which ran with the cadence on
	u64 bubble_ns = 0;     ///< the latched blocking-wait peak
	u64 submits_taken = 0; ///< mid-frame submits the cadence was the marginal trigger for
	u64 tax_total_ns = 0;  ///< the cadence's own GS-thread cost, summed over the frames it RAN in
	u64 tax_passes = 0;    ///< ...over this many render passes

	/// What the cadence costs this device per render pass, in nanoseconds. Zero until it has run
	/// somewhere, which is the whole of "unpriced, so no evidence, so leave the shipped arrangement
	/// alone". Integer division: a rate under 1 ns a pass is a rate this decision cannot use.
	u64 TaxNs() const { return tax_passes ? (tax_total_ns / tax_passes) : 0; }

	/// Feed one finished frame; returns the state the NEXT frame runs under.
	///
	/// ⚠️ ONE CALL PER VIDEO FRAME, at the frame boundary (GSDevice::TileGpuFrameBoundary), and not
	/// per plan. Every quantity in here is per CALL: the peak decays by 1/256 a call, the
	/// confirmation counts consecutive calls, and the debit is one call's cost against one call's
	/// passes. Called per plan instead, all three run at the plan rate -- so a title that
	/// mid-frame-flushes 190 times a frame decays its latched bubble 190x faster than one that
	/// flushes once, for the same second of wall clock, and plan count becomes a hidden input to
	/// the submission cadence. That is what this was doing until 2026-08-31.
	///
	/// `wait_ns`      the frame's GPU-blocking wait (sync + out-of-band + source-set wrap)
	/// `submits`      mid-frame submits the CADENCE was the marginal trigger for -- census only; a
	///                kick the near-readback trigger would have taken anyway is neither the
	///                cadence's to be charged for nor this predictor's to govern
	/// `cadence_ns`   what the cadence's offers and submits cost the GS thread, wall time
	/// `render_passes` the frame's render passes -- the cadence's unit of work, and what it is
	///                priced by
	bool Observe(u64 wait_ns, u32 submits, u64 cadence_ns, u32 render_passes)
	{
		// ⚠️ AN EMPTY FRAME IS NOT EVIDENCE. A frame that opened no render pass gave the cadence no
		// opportunity, so it says nothing about either state: not a vote, not agreement (agreement
		// clears the confirmation count, and a 30 Hz title presents an empty frame every second
		// vsync -- it would never reach two consecutive votes), and above all not a decay step. The
		// declaring budget learned this one the expensive way: its peak decayed on empty frames,
		// fell under its own re-admission line in a single step, and re-admitted a class that had
		// been refused, with the counter reading 100% refused throughout.
		if (render_passes == 0)
			return on;
		frames++;
		frames_on += on ? 1u : 0u;
		submits_taken += submits;
		// The rate is measured only on the frames the cadence RAN in. An off frame records no cost
		// because there was none to record, and putting its passes in the denominator would price
		// the lever cheaper the longer it stays off -- which is exactly the feedback that would
		// switch it back on for no reason.
		if (on)
		{
			tax_total_ns += cadence_ns;
			tax_passes += render_passes;
		}

		// The peak, decayed first so a frame's own wait can re-establish it in the same step.
		bubble_ns -= bubble_ns >> kGSTileGpuKickBubbleDecayShift;
		if (wait_ns > bubble_ns)
			bubble_ns = wait_ns;

		// The price of the state we are IN. Measured while on, counterfactual while off, and the
		// two are the same quantity: what the cadence costs on a frame with this many passes.
		const u64 debit = on ? cadence_ns : (static_cast<u64>(render_passes) * TaxNs());
		const u64 need = on ? debit : (debit * kGSTileGpuKickEnterMultiplier);
		const bool want = (debit == 0) || (bubble_ns >= need);
		if (want == on)
		{
			confirming = 0;
			return on;
		}
		if (++confirming < kGSTileGpuKickConfirmFrames)
			return on;
		on = want;
		confirming = 0;
		switches++;
		return on;
	}
};

/// The strongest ordering grade the TileGpu executor knows how to force.
constexpr u32 kGSTileGpuSerializeMax = 3;

/// The effective serialization grade: what EmuCore/GS/TileGpuSerializeOps says, clamped to the
/// ladder the executor implements. Out of range clamps rather than refuses — a diagnostic key
/// must not be able to stop a run over a typo — and anything at or below zero is off, which is
/// the shipped position and the one that records nothing at all.
constexpr u32 gsTileGpuSerializeOps(int setting)
{
	if (setting <= 0)
		return 0;
	if (static_cast<u32>(setting) > kGSTileGpuSerializeMax)
		return kGSTileGpuSerializeMax;
	return static_cast<u32>(setting);
}

/// The executor's op boundaries, one bit each, for EmuCore/GS/TileGpuSerializeMask.
///
/// The blanket grade above answers "is this an ordering problem at all". It cannot say WHICH edge,
/// and re-running blankets cannot either: seven boundaries take seven device rounds one at a time,
/// or three by bisection, and only if the bit numbering is the same in every run record. So the
/// numbering lives here rather than at the call sites, and it is APPEND-ONLY -- a mask value quoted
/// in a device record has to keep meaning what it meant when it was written, so a boundary that
/// goes away leaves its bit behind rather than renumbering the ones above it.
///
/// Each name is the op whose TAIL the boundary sits at, in the order the executor records them.
enum GSTileGpuSerializeSite : u32
{
	/// A palette expand: the index and palette images sampled into a colour image.
	kGSTileGpuSerializeSiteExpand = 0,
	/// A donor build or a CLUT gather: a resident target read through the GS swizzle into a source.
	kGSTileGpuSerializeSiteDonor = 1,
	/// A CLUT block copy: a palette's blocks copied image-to-buffer into the frame's byte road.
	kGSTileGpuSerializeSiteClutCopy = 2,
	/// A source materialise: a texture window composed out of the byte road into a source image.
	kGSTileGpuSerializeSiteMaterialise = 3,
	/// The byte road proper -- a writeback compute dispatch, or a seed pass into a target.
	kGSTileGpuSerializeSiteByteRoad = 4,
	/// The pass snapshot: a copy of the colour target taken for the draws that read their own
	/// destination through the scratch rather than in-pass.
	kGSTileGpuSerializeSiteSnapshot = 5,
	/// The tail of a geometry pass, between it and the next one.
	kGSTileGpuSerializeSitePassTail = 6,
	kGSTileGpuSerializeSiteCount = 7,
};

/// Every site engaged, which is what the blanket grade asks for.
constexpr u32 kGSTileGpuSerializeMaskAll = (1u << kGSTileGpuSerializeSiteCount) - 1u;

/// The effective site mask: what EmuCore/GS/TileGpuSerializeMask says, with the bits above the
/// sites that exist dropped. Clamps rather than refuses for the same reason the grade does -- a
/// diagnostic key must not be able to stop a run over a typo -- and a negative is off, not "all",
/// because a bisection arm that silently became the blanket would read as the blanket's result.
constexpr u32 gsTileGpuSerializeMask(int setting)
{
	if (setting <= 0)
		return 0;
	return static_cast<u32>(setting) & kGSTileGpuSerializeMaskAll;
}

class GSPassScheduler;

class GSDevice : public GSAlignedClass<32>
{
	/// Emits queued draws through DoRenderHW, which is protected.
	friend class GSPassScheduler;

public:
	enum class PresentResult
	{
		OK,
		FrameSkipped,
		DeviceLost
	};

	enum class DebugMessageCategory
	{
		Cache,
		Reg,
		Debug,
		Message,
		Performance
	};

	// clang-format off
	struct FeatureSupport
	{
		bool broken_point_sampler : 1; ///< Issue with AMD cards, see tfx shader for details
		bool vs_expand            : 1; ///< Supports expanding points/lines/sprites in the vertex shader
		bool primitive_id         : 1; ///< Supports primitive ID for use with prim tracking destination alpha algorithm
		bool texture_barrier      : 1; ///< Supports sampling rt and hopefully texture barrier
		bool multidraw_fb_copy    : 1; ///< Replacement for texture barrier.
		bool cheap_rt_feedback_read : 1; ///< A feedback read costs nothing structural — no render-pass break, no tile flush — so the renderer may take one on a draw that did not need it. ⚠️ `!texture_barrier` is NOT a substitute: it is equally true of every driver on the RT-copy feedback workaround, where the read is the most expensive one we have.
		bool provoking_vertex_last: 1; ///< Supports using the last vertex in a primitive as the value for flat shading.
		bool point_expand         : 1; ///< Supports point expansion in hardware.
		bool line_expand          : 1; ///< Supports line expansion in hardware.
		bool prefer_new_textures  : 1; ///< Allocate textures up to the pool size before reusing them, to avoid render pass restarts.
		bool dxt_textures         : 1; ///< Supports DXTn texture compression, i.e. S3TC and BC1-3.
		bool bptc_textures        : 1; ///< Supports BC6/7 texture compression.
		bool astc_textures        : 1; ///< Can create and sample every standard 2D ASTC LDR UNORM format used by the replacement loader.
		bool framebuffer_fetch    : 1; ///< Can sample from the framebuffer without texture barriers.
		bool framebuffer_fetch_orders_overlap : 1; ///< Framebuffer fetch also orders overlapping primitives *within* a single draw, so a full barrier is redundant. Vulkan's rasterization-order attachment access, Metal's programmable blending and GL's ARM_shader_framebuffer_fetch all guarantee this by spec; GL's EXT_shader_framebuffer_fetch does not deliver it in practice.
		bool stencil_buffer       : 1; ///< Supports stencil buffer, and can use for DATE.
		bool cas_sharpening       : 1; ///< Supports sufficient functionality for contrast adaptive sharpening.
		bool test_and_sample_depth: 1; ///< Supports concurrently binding the depth-stencil buffer for sampling and depth testing.
		bool no_ps2_z_quantization: 1; ///< Skip PS2 32-bit-fixed Z floor (saves SPIR-V DepthReplacing → re-enables early-ZS on tilers).
		bool depth_feedback       : 1; ///< Depth feedback loops can be done with DS directly (otherwise need to copy to separate RT).  Implies `feedback_loops`.
		bool aa1                  : 1; ///< Supports the GS AA1 feature.
		bool rov                  : 1; ///< Supports rasterizer ordered views for both depth and color.
		bool metalfx_spatial      : 1; ///< Supports Apple MetalFX spatial upscaling (Metal backend, macOS 13+).
		bool fsr1                 : 1; ///< Supports AMD FidelityFX Super Resolution 1 (two compute passes).
		bool sgsr                 : 1; ///< Supports Qualcomm Snapdragon Game Super Resolution 1 (one compute pass).
		bool dual_source_blend    : 1; ///< Supports a second fragment output (SRC1) as a hardware blend factor.
		bool broken_mad_deinterlace : 1; ///< Driver can't reliably preserve/read the two-bank FastMAD history target.
		FeatureSupport()
		{
			memset(this, 0, sizeof(*this));
			// Desktop backends (GL 3.3+, Metal, DX) always support this. GLES and Vulkan override it
			// after querying the device, because mobile GPUs (notably Mali) may omit dual-source
			// blending. When absent, GSRendererHW emulates SRC1 blend equations in-shader per-draw
			// instead of forcing a global high blending-accuracy level. Ported from sashkinbro/EmuCoreX.
			dual_source_blend = true;
		}
		/// Supports feedback loops through either texture barriers or rt copies.
		bool feedback_loops() const { return texture_barrier || multidraw_fb_copy; }
	};

	struct MultiStretchRect
	{
		GSVector4 src_rect;
		GSVector4 dst_rect;
		GSTexture* src;
		Filter filter;
		GSHWDrawConfig::ColorMaskSelector wmask; // 0xf for all channels by default
	};

	struct TextureRecycleDeleter
	{
		void operator()(GSTexture* const tex);
	};
	using RecycledTexture = std::unique_ptr<GSTexture, TextureRecycleDeleter>;

	enum BlendFactor : u8
	{
		// HW blend factors
		SRC_COLOR,   INV_SRC_COLOR,   DST_COLOR,  INV_DST_COLOR,
		SRC1_COLOR,  INV_SRC1_COLOR,  SRC_ALPHA,  INV_SRC_ALPHA,
		DST_ALPHA,   INV_DST_ALPHA,   SRC1_ALPHA, INV_SRC1_ALPHA,
		CONST_COLOR, INV_CONST_COLOR, CONST_ONE,  CONST_ZERO,
	};
	enum BlendOp : u8
	{
		// HW blend operations
		OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT
	};
	// clang-format on

protected:
	std::string m_name = "Unknown";
	FeatureSupport m_features;
	u32 m_max_texture_size = 0;
	// ★ Unknown, NOT Adreno. Defaulting to a real vendor meant every backend that never calls
	// SetRuntimeGPUProfile (Vulkan, Metal, DX12 — none of them did) silently identified as Adreno,
	// and so did desktop OpenGL on anything not-Mali. That made IsAdrenoGPUProfile() fire
	// Adreno-only workarounds on Apple Silicon, and made IsMaliGPUProfile() permanently false under
	// Vulkan — which quietly disabled the Tekken 5 MediaTek-Mali GameDB fix on our default renderer.
	// Unknown means "no vendor quirks", which is the only safe thing to assume before detection.
	RuntimeGpuProfile m_runtime_gpu_profile = RuntimeGpuProfile::Unknown;
	// Per-vendor mobile GPU identity + GS tuning (pool sizes / ages / constrained), resolved from the
	// GPU-profile system (sashkinbro/EmuCoreX). Drives texture/target pool sizing on Android below.
	MobileGpuIdentity m_mobile_gpu_identity;
	MobileGsTuning m_mobile_gs_tuning;
	// Resolved driver identity + the workarounds the driver-bug database says this exact
	// driver needs. Kept beside the GPU identity because the two answer different questions:
	// the identity is "which silicon", this is "which blob", and the blob is what actually
	// miscompiles shaders (see GSGPUDriverProfile.cpp). Empty/conservative until a backend
	// resolves it, so a device with no matching rule behaves exactly as it did before.
	MobileDriverProfile m_mobile_driver_profile;
	// Android: true when the SoC is MediaTek (Dimensity/Helio). Hoisted from GSDeviceVK
	// so both backends + GS.cpp Android GameDB overrides can read it. Set during device
	// open from the resolved GPU profile.
	bool m_is_mediatek_soc = false;

	struct
	{
		u32 start, count;
	} m_vertex = {};
	struct
	{
		u32 start, count;
	} m_index = {};

	u32 m_frame = 0; // for ageing the pool
	u32 m_frames_since_pool_cleanup = 0;

private:
	std::array<FastList<GSTexture*>, 2> m_pool; // [texture, target]
	u64 m_pool_memory_usage = 0;

	static const std::array<HWBlend, 3*3*3*3> m_blendMap;

protected:
	static constexpr int NUM_INTERLACE_SHADERS = 5;
	static constexpr float MAD_SENSITIVITY = 0.08f;
	static constexpr u32 MAX_POOLED_TARGETS = 300;
	static constexpr u32 MAX_TARGET_AGE = 20;
	static constexpr u32 MAX_POOLED_TEXTURES = 300;
	static constexpr u32 MAX_TEXTURE_AGE = 10;
	static constexpr u32 NUM_CAS_CONSTANTS = 12; // 8 plus src offset x/y, 16 byte alignment
	// Five uvec4s: EASU's con0..con3 plus FSR's "Sample" vector. RCAS only reads con0 and
	// Sample, but Sample still decorates to byte offset 64, so both passes push all 80 bytes
	// - a short push leaves Sample undefined and the shader squares the whole image.
	static constexpr u32 NUM_FSR1_CONSTANTS = 20;
	/// dstSize(2) + uvOffset(2) + uvScale(2) + srcSize(2) + invSrcSize(2) + edgeSharpness(1),
	/// as u32 words. Mixed uint/float, so the host packs it rather than the type saying so.
	static constexpr u32 NUM_SGSR_CONSTANTS = 11;
	/// Plain and edge-direction. Two modules from one file, like FSR1's two passes: the variant
	/// is a preprocessor gate, so it cannot be a specialization constant.
	static constexpr u32 NUM_SGSR_PIPELINES = 2;
	static constexpr u32 EXPAND_BUFFER_SIZE = sizeof(u16) * 16383 * 6;

	WindowInfo m_window_info;
	GSVSyncMode m_vsync_mode = GSVSyncMode::Disabled;
	bool m_allow_present_throttle = false;
	bool m_present_has_new_frame = false;
	u64 m_last_frame_displayed_time = 0;

	GSTexture* m_merge = nullptr;
	GSTexture* m_weavebob = nullptr;
	GSTexture* m_blend = nullptr;
	GSTexture* m_mad = nullptr;
	GSTexture* m_target_tmp = nullptr;
	GSTexture* m_current = nullptr;
	/// Whether a chain is loaded in the backend, so ApplyShaderChain can free it on the
	/// frame the player turns shaders off rather than polling for it.
	bool m_shader_chain_loaded = false;
	GSTexture* m_cas = nullptr;
	GSTexture* m_mfx_output = nullptr; ///< MetalFX spatial upscale destination (Metal backend).
	GSTexture* m_fsr1_easu = nullptr; ///< FSR1 EASU output, at display size; RCAS reads it back.
	GSTexture* m_fsr1_output = nullptr; ///< FSR1 RCAS output, the texture actually presented.
	GSTexture* m_sgsr_output = nullptr; ///< SGSR output. One pass, so one target, unlike FSR1.
	GSTexture* m_colclip_rt = nullptr; ///< Temp hw colclip texture
	GSTexture* m_ds_as_rt = nullptr; ///< Depth as color

	/// Render passes are coalesced by holding draws here until something needs to observe
	/// the target. See GSPassScheduler and FlushDeferredDraws().
	std::unique_ptr<GSPassScheduler> m_pass_scheduler;
	u32 m_deferred_draw_count = 0;
	bool m_flushing = false;

	/// Textures the texture cache has finished with, but which queued draws still read or
	/// write. They go back into the pool once those draws have run - returning them any
	/// earlier would let FetchSurface hand out a texture that is about to be sampled.
	std::vector<GSTexture*> m_deferred_recycle;

	void FlushDeferredDrawsImpl();
	bool DeferredDrawsReference(const GSTexture* tex) const;

	bool AcquireWindow(bool recreate_window);

	virtual GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) = 0;

	virtual void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) = 0;
	virtual void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) = 0;
	virtual void DoFXAA(GSTexture* sTex, GSTexture* dTex) = 0;
	virtual void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) = 0;
	/// Run the librashader filter chain from sTex into dTex. Returns false when the
	/// backend has no chain support, the preset failed to load, or the frame was
	/// skipped — the caller then leaves m_current alone, so an unsupported backend or
	/// a bad preset degrades to "no shader" instead of a black screen. NOT pure: only
	/// the Vulkan/OpenGL devices override it, everything else keeps the no-op.
	virtual bool DoApplyShaderChain(GSTexture* sTex, GSTexture* dTex) { return false; }

	/// Free whatever the chain is holding. A loaded chain owns a render target and a
	/// pipeline per pass and the collection runs to forty of them, so leaving it resident
	/// after the player turns shaders off is the memory that matters on a handheld. Same
	/// override rule as above: only the librashader-capable backends implement it.
	virtual void ReleaseShaderChain() {}

	/// Generation of the parameter-override store, bumped on every SetShaderChainParams.
	/// A backend compares this against its own last-applied generation to decide whether
	/// there is anything to do — it is an atomic load, so the per-frame check costs no
	/// lock. Zero means nothing has ever been pushed.
	static u64 GetShaderChainParamGeneration();

	/// Copies the queued overrides into [out] when they belong to [preset], returning false
	/// (and leaving [out] alone) when the store holds another preset's values or none at
	/// all. Takes the lock, so call it only once the generation says something changed.
	static bool GetShaderChainParams(const std::string& preset, std::vector<std::pair<std::string, float>>* out);

	/// Resolves CAS shader includes for the specified source.
	static bool GetCASShaderSource(std::string* source);

	/// Applies CAS and writes to the destination texture, which should be a shader writeable texture.
	virtual bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants) = 0;

	/// Upscales sTex into dTex using a backend-specific spatial upscaler (MetalFX on Metal).
	/// Base implementation is a no-op; only the Metal backend overrides it.
	virtual bool DoMetalFXSpatial(GSTexture* sTex, GSTexture* dTex) { return false; }

	/// Resolves FSR1 shader includes for the specified source, and prepends the #version line
	/// plus the FSR_PASS_EASU gate. The pass cannot be a specialization constant: it decides
	/// which function bodies ffx_fsr1.h emits at all, which is a preprocessor-time question.
	static bool GetFSR1ShaderSource(std::string* source, bool easu_pass);

	/// FSR1 pass 1 (EASU): edge-adaptive spatial upsample from sTex to dTex's size.
	/// FSR1 pass 2 (RCAS): robust contrast-adaptive sharpen, same size in and out.
	/// Both no-op in the base class so only the backends that gate Features().fsr1 on need them.
	virtual bool DoFSR1EASU(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants) { return false; }
	virtual bool DoFSR1RCAS(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_FSR1_CONSTANTS>& constants) { return false; }

	/// SGSR: edge-directed spatial upsample from sTex to dTex's size, in a single dispatch.
	/// No-op in the base class, like the FSR1 pair above.
	virtual bool DoSGSR(GSTexture* sTex, GSTexture* dTex, const std::array<u32, NUM_SGSR_CONSTANTS>& constants,
		bool edge_direction) { return false; }

	/// Perform texture operations for ImGui
	void UpdateImGuiTextures();

protected:
	// Entry point to the renderer-specific StretchRect code.
	virtual void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) = 0;
	virtual void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
		PresentShader shader, Filter filter)
	{
		pxFailRel("Not implemented");
	}
	void DoStretchRectWithAssertions(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter);

	/// Serves a StretchRect through CopyRect when the two are equivalent, returning whether it did.
	/// A stretch is a draw, so it needs a render pass of its own and the pass it interrupted has to
	/// be restarted afterwards -- two pass boundaries where an image copy costs one.
	bool TryStretchRectAsCopy(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader);

	/// Backend entry points for work that reads or writes texture contents. These are
	/// reached only through the public non-virtual wrappers of the same name, which run
	/// FlushDeferredDraws() first — see that function for why the indirection exists.
	/// Backends may call them directly on themselves to skip the (re-entrant) flush.
	virtual void DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) = 0;
	virtual void DoDrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader);
	virtual void DoUpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) = 0;
	virtual void DoConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) = 0;
	virtual void DoFilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) = 0;
	virtual void DoRenderHW(GSHWDrawConfig& config) = 0;
	virtual void DoBeginDSAsRT(GSTexture* ds, const GSVector4i& drawarea);
	virtual void DoHintReadbackSource(GSTexture* tex);
	virtual PresentResult DoBeginPresent(bool frame_skip) = 0;

public:
	GSDevice();
	virtual ~GSDevice();

	/// Returns a string containing current adapter in use.
	const std::string& GetName() const { return m_name; }

	GSTexture* GetColorClipTexture() const { return m_colclip_rt; }
		
	void SetColorClipTexture(GSTexture* tex) { m_colclip_rt = tex; }

	bool IsDSInRTActive() const { return m_ds_as_rt; }
	/// Create a temporary color clone of depth for depth feedback
	void BeginDSAsRT(GSTexture* ds, const GSVector4i& drawarea)
	{
		FlushDeferredDraws();
		DoBeginDSAsRT(ds, drawarea);
	}
	void EndDSAsRT();

	/// Returns a string representing the specified API.
	static const char* RenderAPIToString(RenderAPI api);

	/// Queues parameter overrides for the RetroArch shader chain. Set from the UI thread,
	/// consumed on the GS thread: a librashader chain is single-threaded, so the UI must
	/// NEVER call libra_*_filter_chain_set_param itself — it leaves the values here and
	/// DoApplyShaderChain applies them just before the frame call.
	///
	/// [params] is a list of "assign this value to this parameter" instructions, NOT a
	/// complete description of the chain's state. Anything absent keeps whatever the chain
	/// already has, which for a freshly created chain is the preset's own initial value.
	/// That is what makes reset work: the UI pushes the initial value explicitly rather
	/// than dropping the entry, because a live chain has no "unset" for us to ask for.
	///
	/// [preset] is the preset the values were read off. It stops a stale push from landing
	/// on the wrong chain — values queued for preset A are ignored once the device has
	/// moved on to preset B, which would otherwise silently apply A's values to B's
	/// same-named parameters.
	static void SetShaderChainParams(std::string preset, std::vector<std::pair<std::string, float>> params);

	/// Parses the configured fullscreen mode into its components (width * height @ refresh Hz)
	static bool GetRequestedExclusiveFullscreenMode(u32* width, u32* height, float* refresh_rate);

	/// Converts a fullscreen mode to a string.
	static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

	/// Generates a fixed index buffer for expanding points and sprites. Buffer is assumed to be at least EXPAND_BUFFER_SIZE in size.
	static void GenerateExpansionIndexBuffer(void* buffer);

	// Process copy area for sw blend copies.
	GSVector4i ProcessCopyArea(const GSVector4i& rtsize, const GSVector4i& drawarea);

	/// Reads the specified shader source file.
	static std::optional<std::string> ReadShaderSource(const char* filename);

	/// Returns the maximum number of mipmap levels for a given texture size.
	static int GetMipmapLevelsForSize(int width, int height);

	__fi u64 GetPoolMemoryUsage() const { return m_pool_memory_usage; }

	__fi FeatureSupport Features() const { return m_features; }
	__fi u32 GetMaxTextureSize() const { return m_max_texture_size; }
	__fi void SetRuntimeGPUProfile(RuntimeGpuProfile p) { m_runtime_gpu_profile = p; }
	__fi void SetMobileGPUIdentity(const MobileGpuIdentity& identity) { m_mobile_gpu_identity = identity; }
	__fi void SetMobileGSTuning(const MobileGsTuning& tuning) { m_mobile_gs_tuning = tuning; }
	__fi const MobileGpuIdentity& GetMobileGPUIdentity() const { return m_mobile_gpu_identity; }
	__fi const MobileGsTuning& GetMobileGSTuning() const { return m_mobile_gs_tuning; }
	__fi void SetMobileDriverProfile(const MobileDriverProfile& profile) { m_mobile_driver_profile = profile; }
	__fi const MobileDriverProfile& GetMobileDriverProfile() const { return m_mobile_driver_profile; }
	/// The driver is *known* to have this defect. Diagnostics only — never gate rendering on it,
	/// because a recorded bug whose mitigation is not integrated yet still has no workaround bit.
	__fi bool HasMobileDriverBug(DriverBug bug) const { return m_mobile_driver_profile.HasBug(bug); }
	/// The one call sites should use: "is this mitigation active for this driver". False for every
	/// device the database has no rule for, so untouched hardware keeps its existing behaviour.
	__fi bool UsesMobileDriverWorkaround(DriverWorkaround workaround) const
	{
		return m_mobile_driver_profile.UsesWorkaround(workaround);
	}
	__fi bool IsConstrainedMobileGPUProfile() const { return m_mobile_gs_tuning.constrained; }
	__fi RuntimeGpuProfile GetRuntimeGPUProfile() const { return m_runtime_gpu_profile; }
	__fi void SetMediaTekSoC(bool v) { m_is_mediatek_soc = v; }
	__fi bool IsMediaTekSoC() const { return m_is_mediatek_soc; }
	__fi bool IsMaliGPUProfile() const { return (m_runtime_gpu_profile == RuntimeGpuProfile::Mali); }
	__fi bool IsAdrenoGPUProfile() const { return (m_runtime_gpu_profile == RuntimeGpuProfile::Adreno); }
	__fi bool IsPowerVRGPUProfile() const { return (m_runtime_gpu_profile == RuntimeGpuProfile::PowerVR); }

	__fi const WindowInfo& GetWindowInfo() const { return m_window_info; }
	__fi s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
	__fi s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
	__fi GSVector2i GetWindowSize() const { return GSVector2i(static_cast<s32>(m_window_info.surface_width), static_cast<s32>(m_window_info.surface_height)); }
	// Logical window dimensions for layout: same as GetWindowSize for
	// Rot0/Rot180, swapped for Rot90/Rot270 so callers compute the present
	// rect against a portrait box that the rotation transform then maps onto
	// the landscape swapchain. Use this in callers that produce coordinates
	// later consumed by the rotation-aware Vulkan present path (game draw_rect,
	// ImGui DisplaySize). Other callers (GS Resize, viewport setup) want the
	// raw physical dims and should keep using GetWindowWidth/Height.
	GSVector2i GetPresentationSize() const;
	__fi s32 GetPresentationWidth() const { return GetPresentationSize().x; }
	__fi s32 GetPresentationHeight() const { return GetPresentationSize().y; }
	__fi float GetWindowScale() const { return m_window_info.surface_scale; }
	__fi GSVSyncMode GetVSyncMode() const { return m_vsync_mode; }
	__fi bool IsPresentThrottleAllowed() const { return m_allow_present_throttle; }

	__fi GSTexture* GetCurrent() const { return m_current; }
	__fi GSTexture* GetMAD() const { return m_mad; }
	
	void Recycle(GSTexture* t);

	/// Returns true if it's an OpenGL-based renderer.
	bool UsesLowerLeftOrigin() const;

	/// Texture/target pool budget. Per-GPU from the mobile GS tuning on Android, the
	/// MAX_POOLED_*/MAX_*_AGE constants elsewhere. From sashkinbro/EmuCoreX.
	u32 GetPoolLimit(bool texture) const;
	u32 GetPoolMaxAge(bool texture) const;

	/// Free ImGui textures before shutdown
	void DestroyImGuiTextures();

	virtual bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle);
	virtual void Destroy();

	/// Returns the graphics API used by this device.
	virtual RenderAPI GetRenderAPI() const = 0;

	/// Returns true if we have a window we're rendering into.
	virtual bool HasSurface() const = 0;

	/// Destroys the surface we're currently drawing to.
	virtual void DestroySurface() = 0;

	/// Switches to a new window/surface.
	virtual bool UpdateWindow() = 0;

	/// Call when the window size changes externally to recreate any resources.
	virtual void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) = 0;

	/// Returns true if exclusive fullscreen is supported.
	virtual bool SupportsExclusiveFullscreen() const = 0;

	/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
	/// displayed, but the GPU command queue will still be flushed.
	PresentResult BeginPresent(bool frame_skip)
	{
		FlushDeferredDraws();
		// Assume nothing until the frame is actually composited. Several presents legitimately
		// carry no new game output — see NotePresentHasNewFrame.
		m_present_has_new_frame = false;
		return DoBeginPresent(frame_skip);
	}

	/// Record that the present being built carries a game frame the GS has just produced. Called
	/// from the one place that knows — GSRenderer::VSync, where "there is something to draw" is
	/// already decided as `current && !blank_frame`. Read by the Vulkan backend, which must not
	/// hand frame generation a frame the game never drew.
	///
	/// A pause-menu repaint re-presents the PREVIOUS frame and deliberately does NOT set this: it
	/// goes through PresentCurrentFrame, which draws the same image again. Neither does a blank,
	/// a skipped duplicate, or a boot screen with no GS output yet.
	void NotePresentHasNewFrame() { m_present_has_new_frame = true; }

	/// Presents the frame to the display.
	virtual void EndPresent() = 0;

	/// Changes vsync mode for this display.
	virtual void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) = 0;

	/// Returns a string of information about the graphics driver being used.
	virtual std::string GetDriverInfo() const = 0;

	/// Submission epochs, for callers that track which GPU work a resource waits on.
	/// GetSubmitEpoch() names the command buffer being recorded now — work recorded
	/// after this call lands in it (or later); GetCompletedSubmitEpoch() is the newest
	/// epoch the GPU is known to have finished, polled non-blockingly. A resource whose
	/// epoch is <= completed can be read back without submitting or waiting on anything
	/// recorded since; one whose epoch == the current epoch cannot be read back without
	/// submitting the buffer being recorded. Backends without a submission timeline
	/// return 0 for both, which reads as "everything is pending" — the conservative
	/// answer, never a wrong one.
	virtual u64 GetSubmitEpoch() const { return 0; }
	virtual u64 GetCompletedSubmitEpoch() { return 0; }

	/// Tile renderer: a render target read as an indexed texture. Writes `dst` — an
	/// RGBA8 render target of the index window's size — with the indices of that window
	/// read out of `owner`, a page-aligned CT32/CT24 surface texture, through the GS
	/// swizzle in a fragment shader (GSTileSwizzleForms carries the arithmetic and the
	/// meaning of the parameters). Returns false when the device does not serve it — every
	/// backend but Vulkan — and the caller takes the CPU route.
	struct TileReinterpretParams
	{
		u32 src_bp; ///< the index window's TBP0 (blocks)
		u32 src_bwpg; ///< the window's width in pages, as GSOffset computes it
		u32 dst_bp; ///< the owner's base (page-aligned blocks)
		u32 dst_bwpg; ///< the owner's width in pages
		u32 fmt; ///< GSTileSwizzleForms::IndexFormat
	};
	virtual bool TileReinterpretIndex(GSTexture* owner, GSTexture* dst, const TileReinterpretParams& p) { return false; }

	/// Wall time and count of out-of-band readback fence waits — the wait class
	/// that sits OUTSIDE the drain accounting by design, and therefore needs its
	/// own row in any census that wants the whole wait bill.
	virtual u64 GetOobWaitNs() const { return 0; }
	virtual u64 GetOobWaitCalls() const { return 0; }

	/// Host waits on GPU completion, split by who is to blame. `Sync` is the GS thread blocking
	/// OUT OF TURN — a readback's submit-and-wait, an explicit sync — and together with the
	/// out-of-band waits above it is the whole population that serializes the frame. `Ring` is the
	/// command-buffer ring's own recycle wait, which is backpressure and is reported separately so
	/// it cannot be mistaken for a drain.
	virtual u64 GetSyncWaitNs() const { return 0; }
	virtual u64 GetSyncWaitCalls() const { return 0; }
	virtual u64 GetRingWaitNs() const { return 0; }
	virtual u64 GetRingWaitCalls() const { return 0; }

	/// The TileGpu source-descriptor ring wrapping onto a set an in-flight submission still reads.
	/// Reported separately from `Sync` and, unlike `Ring`, still counted in
	/// GSPerfMon::GpuBlockingWaits — it stalls the GS thread mid-frame, and the acceptance metric
	/// must not improve by reclassification. See GSDeviceVK::GpuWaitCause for the argument in full.
	virtual u64 GetSourceSetWaitNs() const { return 0; }
	virtual u64 GetSourceSetWaitCalls() const { return 0; }

	/// The TileGpu executor's mid-frame kick (EmuCore/GS/TileGpuKickReadbackFrames, and the cadence
	/// EmuCore/GS/TileGpuKickPassCadence sets): how often the gate opened, and how often the next
	/// command buffer had also retired so the submit could actually go. Read as a PAIR —
	/// offered-minus-taken is the fence gate declining, which is the pipeline being full rather than
	/// anything being wrong, and only NUM_COMMAND_BUFFERS-1 submissions are ever in flight. Both zero
	/// means the gate never opened, which is a different statement from the lever being off.
	virtual u64 GetTileGpuKicksOffered() const { return 0; }
	virtual u64 GetTileGpuKicksTaken() const { return 0; }

	/// The kick PREDICTOR's census (EmuCore/GS/TileGpuAdaptiveKick): frames observed, of which ran
	/// with the cadence on, how many times the state actually changed, the latched bubble the
	/// decision is made against, and what this device has been measured to charge for one mid-frame
	/// render pass. The last one is the whole reason the predictor is per-device -- read it beside a
	/// device record's arm, because it is the number that decides.
	virtual u64 GetTileGpuKickPredictorFrames() const { return 0; }
	virtual u64 GetTileGpuKickPredictorFramesOn() const { return 0; }
	virtual u64 GetTileGpuKickPredictorSwitches() const { return 0; }
	virtual u64 GetTileGpuKickPredictorBubbleNs() const { return 0; }
	virtual u64 GetTileGpuKickPredictorTaxNs() const { return 0; }
	virtual u64 GetTileGpuKickPredictorSubmits() const { return 0; }
	/// Render passes the executor opened for a Seed or SeedDepth op, cumulative over the run, and
	/// how many of those were the upload merge's. Counted HERE because nothing else can see them:
	/// the renderer's pass census counts PLAN passes (`m_frame.passes += m_plan_passes.size()`), and
	/// a seed is a render pass the executor opens at a pass HEAD, outside every plan pass. The
	/// merge's seed bill — the largest single thing the widened merge added to a GPU-bound frame —
	/// was invisible to every number this campaign took until these two existed.
	virtual u64 GetTileGpuSeedRenderPasses() const { return 0; }
	virtual u64 GetTileGpuMergeSeedRenderPasses() const { return 0; }

	/// Tile renderer: a palette loaded off a render target. Writes `dst` — an RGBA8
	/// render target of `entries` × 1 — with the CSM1 32-bit palette whose source words
	/// begin at block `cbp` of `owner`, a page-aligned CT32/CT24 surface texture, in
	/// entry order (256 for an eight-bit palette at CSA 0, 16 for a four-bit one).
	/// False when the device does not serve it.
	struct TileClutGatherParams
	{
		u32 cbp; ///< the palette's CBP (blocks)
		u32 dst_bp; ///< the owner's base (page-aligned blocks)
		u32 dst_bwpg; ///< the owner's width in pages
		u32 entries; ///< 16 or 256
	};
	virtual bool TileClutFromTarget(GSTexture* owner, GSTexture* dst, const TileClutGatherParams& p) { return false; }

	/// Tile renderer: whether this device can address a colour target through the GS
	/// swizzle in a fragment shader at all (the closed forms fitted, the shader legs
	/// exist) — the precondition for PS_TILE_DIRECT_IDX / PS_TILE_DIRECT_PAL. `clut_ok`
	/// says the same for the palette's word order. False on every backend but Vulkan.
	virtual bool TileSwizzleFormsFit(bool& clut_ok)
	{
		clut_ok = false;
		return false;
	}

	/// Tile renderer: expand an index texture through its palette on the device.
	/// Writes level `dst_level` of `dst` (RGBA8, same texel geometry as `index` level
	/// `src_level`) with palette[index] at every texel — index read from the alpha
	/// channel exactly as the draw shader would read it (an R8 source's view replicates
	/// the byte into every channel; a reinterpreted RGBA source carries it in .a), so
	/// the expanded texel is bit-identical to what the in-shader expansion produces.
	/// A render-target `dst` at dst_level 0 is written by the expansion draw directly;
	/// any other dst (a plain mipmapped texture, or a level above 0) is filled through
	/// a device scratch surface. False when the device does not serve it — every
	/// backend but Vulkan — and the caller keeps the in-shader palette path.
	virtual bool TileExpandPalette(GSTexture* index, GSTexture* palette, GSTexture* dst, u32 src_level, u32 dst_level)
	{
		return false;
	}

	/// TileGpu executor (GSHWRendererVariant::TileGpu) — the parallel device road to
	/// RenderHW. Where RenderHW submits one draw at a time behind the render-pass
	/// scheduler's coalescing, the executor takes a whole frame's pass plan the renderer
	/// has already structured and submits it as indirect draws whose per-draw state is
	/// pulled from an indexed table in a storage buffer. It does not touch the deferred-draw
	/// machinery: the pass planner already owns the pass structure the scheduler used to
	/// decide. Served only by a device whose TileGpuExecutorAvailable() is true — Vulkan
	/// with the descriptor-indexing + indirect-draw contract negotiated at device creation.

	/// Which primitive a draw's indices describe. A topology is pipeline state, not a per-draw
	/// field, so it cannot ride in GSTileGpuIndirectDraw (that struct is byte-identical to
	/// VkDrawIndexedIndirectCommand). It travels in the plan's parallel `topologies` array
	/// instead: the executor picks a pipeline per draw by it, and the constant-cost indirect
	/// submission groups draws by it (one indirect range per topology, since one
	/// vkCmdDrawIndexedIndirect covers a single pipeline). Values index the executor's
	/// per-topology pipeline table; keep them contiguous from zero.
	enum class GSTileGpuTopology : u8
	{
		Triangle = 0, ///< triangle list — triangles copied straight, sprites synthesised to quads
		Line = 1,     ///< line list — two indices per line, copied straight
		Point = 2,    ///< point list — one index per point, copied straight
	};

	/// One indexed indirect draw. Field order and size match VkDrawIndexedIndirectCommand,
	/// so the executor uploads the array straight into an indirect buffer; the draw's row in
	/// the state table rides in first_instance (which is why drawIndirectFirstInstance sits
	/// in the executor's capability gate), selecting state with no descriptor rebind.
	struct GSTileGpuIndirectDraw
	{
		u32 index_count;
		u32 instance_count; ///< 1 for a plain draw
		u32 first_index;
		s32 vertex_offset;
		u32 state_index; ///< -> first_instance; row into GSTileGpuPassPlan::state_table
	};

	/// A FRAME/ZBUF surface pair a pass renders into, named as indices into the plan's
	/// target list (resolved to GSTextures by the executor). kNoTarget marks an absent
	/// slot — a colour-only or depth-only pass.
	struct GSTileGpuTargetPair
	{
		u32 frame_target;
		u32 zbuf_target;
	};

	/// Where a TileGpu pass may write, and the mapping its vertices arrive through. Two
	/// questions, and one number cannot answer both.
	struct GSTileGpuPassGeometry
	{
		/// The viewport: NDC -> device pixels. It is the other half of the vertex transform the
		/// renderer built into each state row, and that transform is written against the COLOUR
		/// target's OWN size -- guest row y is colour-image row y, because a surface's texture
		/// row 0 is its base pointer's row 0. So the viewport spans exactly that.
		int viewport_width;
		int viewport_height;
		/// The render area, and the scissor that matches it: what the pass is allowed to touch.
		/// A framebuffer may not be larger than the smallest attachment it carries, so a pass
		/// pairing a tall colour target with a shorter depth one renders into the intersection.
		int area_width;
		int area_height;
	};

	/// The geometry of a pass rendering into this attachment pair.
	///
	/// The two answers differ exactly when the pair's sizes do, which this planner does produce:
	/// a colour surface grows to whatever page set a texture read of it spans, while its depth
	/// partner keeps the height its own draws asked for. FlatOut 2 renders its world into a
	/// 640x448 PSMCT32 buffer and then composites it to the display buffer through a TW=1024
	/// TH=512 texture read, so the colour surface becomes 640x512 and the depth stays 640x448.
	/// Serving the viewport from the clamped size rescaled every guest row by 448/512: the world
	/// sat 12.5% up the frame and ran out at row 392 with a black band under it, while the HUD --
	/// sprites that need no depth, so they land in colour-only passes with nothing to clamp
	/// against -- stayed exactly where it belonged.
	///
	/// Clipping at the clamped height loses nothing: both surfaces grew to cover every draw's
	/// footprint, so no draw's bottom is below min(colour, depth). The planner asserts it.
	static constexpr GSTileGpuPassGeometry gsTileGpuPassGeometry(bool has_color, int color_width,
		int color_height, bool has_depth, int depth_width, int depth_height)
	{
		const int vw = has_color ? color_width : depth_width;
		const int vh = has_color ? color_height : depth_height;
		int aw = vw;
		int ah = vh;
		if (has_color && has_depth)
		{
			aw = (depth_width < aw) ? depth_width : aw;
			ah = (depth_height < ah) ? depth_height : ah;
		}
		return GSTileGpuPassGeometry{vw, vh, aw, ah};
	}

	/// Whether TileGpu hands the As blend factor to a second fragment output on this device: what
	/// EmuCore/GS/TileGpuDualSrcRoad says, or the device's own answer where it says nothing.
	///
	/// Three states, the shape the pass cap already has: zero asks the device, a positive value
	/// forces the feature-free roads anywhere (the arm an A/B needs on a device that would not
	/// otherwise take them), a negative one asks for the second output. A negative value cannot
	/// conjure the feature, so a device without dualSrcBlend keeps the feature-free roads whatever
	/// the setting says.
	static constexpr bool gsTileGpuDualSourceRoad(int setting, bool has_dual_source)
	{
		if (setting > 0)
			return false;
		return has_dual_source;
	}

	/// Which of a blend row's two factors the GS's own SOURCE ALPHA supplies.
	///
	/// It is the one GS blend term Vulkan can express only as a dual-source (index 1) fragment
	/// output: the GS's As is the fragment's alpha byte read in the 0x80 = 1.0 convention, so it
	/// reaches 1.99 where the alpha the draw STORES reaches 1.0, and the two are therefore
	/// different numbers that cannot share o_color.a while the draw writes both.
	enum GSTileGpuDualSrcTerm : u32
	{
		kGSTileGpuDualSrcSource = 1u << 0, ///< the row's SOURCE factor is As
		kGSTileGpuDualSrcDest = 1u << 1,   ///< the row's DESTINATION factor is As
	};

	/// The dual-source terms a GS ALPHA index's blend row names, read off the row the executor
	/// really builds the pipeline from (GSDevice::m_blendMap) rather than off the register
	/// selectors -- the map approximates several equations, and only the row says which of those
	/// approximations still reaches for the second source.
	static u32 gsTileGpuDualSrcTerms(u32 blend_index);

	/// How one draw's As blend factor reaches the blend unit.
	enum class GSTileGpuDualSrcRoad : u8
	{
		/// Its row names no As factor at all: nothing to carry.
		None = 0,
		/// The second colour output at index 1, which needs dualSrcBlend.
		DualSource = 1,
		/// o_color.a carries the factor and the pipeline reads it back as SRC_ALPHA -- exactly
		/// equal, because the index-1 output was a broadcast of that same scalar. This draw does
		/// not write the target's alpha byte, so the channel write mask throws the carrier away
		/// after the blend has used it and nothing has to give the alpha back.
		Carrier = 2,
		/// ...and this draw DOES write it, but its factor never reaches the min(x, 1) clamp, so the
		/// carrier is exactly As * 255/128 and the alpha blend equation gives the alpha back by
		/// multiplying it by the constant 128/255.
		CarrierRestore = 3,
		/// ...and neither of those holds: the carrier still takes o_color.a, and a companion draw
		/// over the same geometry writes the alpha channel alone with nothing borrowed.
		CarrierCompanion = 4,
	};

	/// The alpha blend constant the CarrierRestore road divides the carrier back down by. The
	/// carrier is As * 255/128 and the stored alpha is As, so this is 128/255 exactly.
	static constexpr float kGSTileGpuAlphaRestore = 128.0f / 255.0f;

	/// Which road one draw's As factor takes.
	///
	/// It does not matter WHICH of the row's two factors is As -- both become SRC_ALPHA off the same
	/// carrier -- so the only question is what happens to the alpha byte the carrier displaces.
	/// Cheapest first: the write mask already drops it, or the alpha blend equation gives it back,
	/// or a second draw writes it. Every road is exact and none needs a device feature, which is the
	/// point: the device this exists for has neither dual-source blending nor a guaranteed way back
	/// into the fragment stage.
	///
	/// ⚠️ There WAS a fourth road and it is deliberately gone. Where As multiplies the source alone
	/// the fragment stage can fold the factor into its own colour and leave the alpha channel
	/// untouched, which costs nothing and needs no companion -- and over the corpus it moved five
	/// pixels by one level, because the shader's fp32 multiply and the blend unit's are not the same
	/// multiply. Proven by substituting the companion road for the fold alone, which took the
	/// divergence to zero pixels while substituting it for the restore left all five. Deleting it
	/// costs 156 companion draws a frame corpus-wide (0.74% of plan draws; OutRun 2006 pays the most
	/// at 9.25%) and buys a road that is byte-identical to the second output on all nineteen dumps.
	///
	/// `alpha_is_the_shaders` covers the two ways the fragment stage owns the alpha byte it writes:
	/// an AFAIL that keeps the destination's alpha per fragment, and an FBMSK that masks alpha in
	/// PART. Both put a value in o_color.a that the carrier would overwrite, so both take the
	/// companion, which writes that alpha with the carrier bit off.
	static constexpr GSTileGpuDualSrcRoad gsTileGpuDualSrcRoad(
		u32 terms, bool has_dual_source, bool writes_alpha, bool factor_unclamped, bool alpha_is_the_shaders)
	{
		if (terms == 0)
			return GSTileGpuDualSrcRoad::None;
		if (has_dual_source)
			return GSTileGpuDualSrcRoad::DualSource;
		if (!writes_alpha)
			return GSTileGpuDualSrcRoad::Carrier;
		if (factor_unclamped && !alpha_is_the_shaders)
			return GSTileGpuDualSrcRoad::CarrierRestore;
		return GSTileGpuDualSrcRoad::CarrierCompanion;
	}

	/// A VkRect2D's worth of scissor: the GS scissor in target pixels, clamped into the pass's
	/// render area.
	struct GSTileGpuScissorRect
	{
		int x, y;
		int width, height;
	};

	/// The GS scissor [x0, x1) x [y0, y1) as the pass's scissor rectangle.
	///
	/// Vulkan tests the pixel's integer coordinate against [offset, offset + extent), which is the
	/// GS's own test: an exact integer rectangle over rasterized pixels, with nothing said about
	/// the primitive's interpolation. So this is a transcription of the register, not a model of
	/// it -- the columns kept are exactly x0 .. x1-1.
	///
	/// The clamping is not cosmetic: a scissor offset must be non-negative and the rectangle has to
	/// stay inside the area the pass declared, and a GS scissor with its high edge below its low one
	/// is legal and means "nothing", which arrives here as a zero extent rather than a negative one.
	static constexpr GSTileGpuScissorRect gsTileGpuScissorRect(
		int x0, int y0, int x1, int y1, int area_width, int area_height)
	{
		const int cx0 = (x0 < 0) ? 0 : ((x0 > area_width) ? area_width : x0);
		const int cy0 = (y0 < 0) ? 0 : ((y0 > area_height) ? area_height : y0);
		const int cx1 = (x1 < cx0) ? cx0 : ((x1 > area_width) ? area_width : x1);
		const int cy1 = (y1 < cy0) ? cy0 : ((y1 > area_height) ? area_height : y1);
		return GSTileGpuScissorRect{cx0, cy0, cx1 - cx0, cy1 - cy0};
	}

	/// Whether a pass may render into less than its whole attachment pair.
	///
	/// Only where every attachment it carries LOADs. Outside the render area an attachment is neither
	/// loaded nor stored, so a LOAD/STORE one keeps exactly the bytes it held and shrinking the area
	/// moves nothing. The other two load ops INITIALIZE the attachment, and they only initialize
	/// inside the render area: a clamped CLEAR leaves the region outside it uninitialized -- which the
	/// next pass to LOAD there reads as garbage -- and a clamped DONT_CARE leaves that region holding
	/// whatever was in memory where the full-area form left it undefined. Both are an attachment's
	/// FIRST bind after allocation, a handful of passes a frame, so the rule costs nothing and the
	/// argument stays a proof rather than a probability.
	static constexpr bool gsTileGpuMayClampPassArea(
		bool has_color, bool color_loads, bool has_depth, bool depth_loads)
	{
		return (!has_color || color_loads) && (!has_depth || depth_loads);
	}

	/// The RENDER AREA of a pass: the union of its draws' scissors, rounded out to the device's
	/// render-area granularity and clamped to the attachment pair.
	///
	/// A render pass on a tiler pays a tile load and a tile store for every pixel of its render area,
	/// drawn or not -- so a pass's cost is its AREA, not its draw count. The pass-structure census
	/// measured what taking the whole attachment costs: Dirge of Cerberus alternates draw by draw
	/// between a 35x35 scratch surface and its 640x448 framebuffer and moves 134.8 Mpx of tile traffic
	/// a frame, Gran Turismo 4's paletted road 50.3, against a few percent of it actually touched.
	///
	/// It is the union of the SAME rectangles the executor sets before each indirect call, so every
	/// draw is contained in it by construction rather than by two sides agreeing. Rounding OUT is what
	/// makes the area one the driver need not split a tile over; it is content-preserving only because
	/// the caller restricts this to a pass whose attachments all LOAD -- outside the render area such
	/// an attachment is neither loaded nor stored, and inside the round-out margin it is loaded and
	/// stored back unchanged, because no draw's scissor reaches there.
	///
	/// An empty union (a pass whose draws all scissor away, or one the executor will issue no geometry
	/// for) comes back as one granularity tile at the origin rather than a zero extent: still a
	/// load-and-store of its own value, and it asks nothing of the driver about a degenerate area.
	static GSTileGpuScissorRect gsTileGpuPassArea(std::span<const GSVector4i> scissors, int area_width,
		int area_height, int granularity_width, int granularity_height)
	{
		const int gw = (granularity_width > 0) ? granularity_width : 1;
		const int gh = (granularity_height > 0) ? granularity_height : 1;

		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool any = false;
		for (const GSVector4i& s : scissors)
		{
			// Through the executor's own transcription, so a scissor that admits nothing arrives as a
			// zero extent and contributes nothing, and one reaching past the pair is already clamped.
			const GSTileGpuScissorRect r = gsTileGpuScissorRect(s.x, s.y, s.z, s.w, area_width, area_height);
			if (r.width <= 0 || r.height <= 0)
				continue;
			if (!any)
			{
				x0 = r.x;
				y0 = r.y;
				x1 = r.x + r.width;
				y1 = r.y + r.height;
				any = true;
				continue;
			}
			x0 = (r.x < x0) ? r.x : x0;
			y0 = (r.y < y0) ? r.y : y0;
			x1 = ((r.x + r.width) > x1) ? (r.x + r.width) : x1;
			y1 = ((r.y + r.height) > y1) ? (r.y + r.height) : y1;
		}
		if (!any)
		{
			x0 = y0 = 0;
			x1 = y1 = 1;
		}

		const int lx = x0 - (x0 % gw);
		const int ly = y0 - (y0 % gh);
		int hx = ((x1 + gw - 1) / gw) * gw;
		int hy = ((y1 + gh - 1) / gh) * gh;
		hx = (hx > area_width) ? area_width : hx;
		hy = (hy > area_height) ? area_height : hy;
		return GSTileGpuScissorRect{lx, ly, (hx > lx) ? (hx - lx) : 0, (hy > ly) ? (hy - ly) : 0};
	}

	/// The GS page grid in PIXELS of a 32-bit colour target: a PSMCT32 page is 64x32 pixels, and
	/// 64 * 32 * 4 = 8192 bytes is the 8 KB page the whole byte road is counted in. Used to round a
	/// snapshot copy out, and to count what one costs in the units every other TileGpu census uses.
	static constexpr int kGSTileGpuSnapshotPageWidth = 64;
	static constexpr int kGSTileGpuSnapshotPageHeight = 32;

	/// The rectangle a pass's snapshot copy has to cover: the union of the rectangles its DATE draws
	/// read, rounded out to the page grid and clamped to the target.
	///
	/// The snapshot road copies the colour target into a scratch surface the pass then samples, and
	/// it copied the WHOLE target -- 140 pages, 1.147 MB, for a target of 640x448 -- once per pass
	/// with a DATE draw in it, whatever those draws actually looked at. Stuntman takes 1,179 of them
	/// a drawn frame on the SD865: 2.70 GB of image copy, which at that device's 44 GB/s DRAM peak
	/// cannot cost less than ~61 ms of an 86 ms GPU frame.
	///
	/// WHAT BOUNDS THE NARROWING, and it is a single shader line. The only consumer of a snapshot is
	/// `texelFetch(u_snapshot, ivec2(gl_FragCoord.xy), 0).a` in tilegpu.glsl, under the draw's own
	/// `date != 0`: an unfiltered fetch at the fragment's OWN coordinate, so a DATE draw reads
	/// exactly the pixels it rasterizes and nothing beside them -- no filter footprint, no gather, no
	/// derivative. The copy already lands at the target's own coordinates, so the fetch does not move
	/// with the rect and no shader changes. Every draw of a pass shares its colour surface (it is in
	/// the pass key), and a draw's `rect` is its scissor-clipped bbox in that surface's pixel space,
	/// which is the SAME rectangle the planner already trusts to bound the draw's writes when it
	/// decides whether a DATE draw needs a fresh snapshot. So the union over the pass's DATE draws
	/// covers every read any consumer of that snapshot can make. It is Classic's own rule: its DATE
	/// stencil pre-pass is `SetupDATE(rt, ds, datm, config.drawarea)`, per-draw coverage, and has
	/// been since long before this renderer.
	///
	/// NOTHING REQUIRES THE ROUND-OUT. The copy is a vkCmdCopyImage over an uncompressed colour
	/// format, whose block is 1x1, and the reader is a texelFetch -- both are exact at single-pixel
	/// granularity. It is rounded out anyway so that a copy costs a whole number of the pages the
	/// census reports, and so that its rows stay 256-byte multiples for whatever the driver does
	/// underneath. The margin is at most one page in each direction.
	///
	/// An EMPTY union comes back as the WHOLE target, not as nothing. That is the fail-safe and it
	/// is the point of putting the rule here: outside the copied rect the scratch holds its previous
	/// tenant's pixels, so a caller that under-states its reads gets garbage rather than a stale
	/// pixel. A caller that hands this function nothing gets a slow pass instead.
	///
	/// 2026-08-30.
	static GSVector4i gsTileGpuSnapshotRect(
		std::span<const GSVector4i> reads, int target_width, int target_height)
	{
		const GSVector4i full(0, 0, target_width, target_height);
		if (target_width <= 0 || target_height <= 0)
			return full;

		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		bool any = false;
		for (const GSVector4i& rd : reads)
		{
			// Clamped through the executor's own scissor transcription, so a rectangle reaching past
			// the target arrives clipped and one that admits nothing arrives as a zero extent.
			const GSTileGpuScissorRect r =
				gsTileGpuScissorRect(rd.x, rd.y, rd.z, rd.w, target_width, target_height);
			if (r.width <= 0 || r.height <= 0)
				continue;
			if (!any)
			{
				x0 = r.x;
				y0 = r.y;
				x1 = r.x + r.width;
				y1 = r.y + r.height;
				any = true;
				continue;
			}
			x0 = (r.x < x0) ? r.x : x0;
			y0 = (r.y < y0) ? r.y : y0;
			x1 = ((r.x + r.width) > x1) ? (r.x + r.width) : x1;
			y1 = ((r.y + r.height) > y1) ? (r.y + r.height) : y1;
		}
		if (!any)
			return full;

		constexpr int gw = kGSTileGpuSnapshotPageWidth;
		constexpr int gh = kGSTileGpuSnapshotPageHeight;
		const int lx = x0 - (x0 % gw);
		const int ly = y0 - (y0 % gh);
		int hx = ((x1 + gw - 1) / gw) * gw;
		int hy = ((y1 + gh - 1) / gh) * gh;
		hx = (hx > target_width) ? target_width : hx;
		hy = (hy > target_height) ? target_height : hy;
		return GSVector4i(lx, ly, hx, hy);
	}

	/// What a snapshot copy of `r` costs, in the 8 KB pages the rest of the TileGpu census counts.
	/// Rounded UP on both axes, so a target whose height is not a whole number of page rows -- 448 is
	/// fourteen, but 224 is seven and a half -- is charged for the partial row it copies.
	static constexpr u32 gsTileGpuSnapshotPages(const GSVector4i& r)
	{
		const int w = (r.z > r.x) ? (r.z - r.x) : 0;
		const int h = (r.w > r.y) ? (r.w - r.y) : 0;
		if (w <= 0 || h <= 0)
			return 0;
		const int cols = (w + kGSTileGpuSnapshotPageWidth - 1) / kGSTileGpuSnapshotPageWidth;
		const int rows = (h + kGSTileGpuSnapshotPageHeight - 1) / kGSTileGpuSnapshotPageHeight;
		return static_cast<u32>(cols) * static_cast<u32>(rows);
	}

	/// The census bucket a snapshot copy's page count falls in. Doubling bands, because the question
	/// the distribution answers is "are these copies small" and the answer is an order of magnitude,
	/// not a page: 140 is the whole target on the corpus's 640x448 dumps and 1 is one page.
	static constexpr u32 kGSTileGpuSnapshotPageBuckets = 8;
	static constexpr u32 gsTileGpuSnapshotPageBucket(u32 pages)
	{
		if (pages <= 1)
			return 0;
		if (pages <= 2)
			return 1;
		if (pages <= 4)
			return 2;
		if (pages <= 8)
			return 3;
		if (pages <= 16)
			return 4;
		if (pages <= 32)
			return 5;
		if (pages <= 64)
			return 6;
		return 7;
	}

	/// A snapshot copy: clone src_rect of the pass's colour target into a scratch surface the
	/// pass's draws sample, so a draw reads a pre-pass version of pixels the pass also writes
	/// without a raster-order hazard. Taken before the pass it feeds opens; one per pass. Today
	/// it serves the destination-alpha test (DATE: a draw's state row asks the shader to discard
	/// on the snapshot's alpha bit 7 against DATM) -- the planner opens a new pass whenever a
	/// DATE draw's rect intersects what the pass already wrote, so the snapshot is exact for it.
	/// The in-pass declared read (ROAA) replaces this road where the device has it.
	///
	/// `src_rect` is the pass's own DATE-read coverage, not the whole target: see
	/// gsTileGpuSnapshotRect, which is what builds it and what states why the narrowing is sound.
	/// Outside it the scratch holds its previous tenant, so nothing may sample there.
	struct GSTileGpuSnapshotCopy
	{
		u32 src_target;
		GSVector4i src_rect;
	};

	/// One GS page (8 KB of guest memory) the frame's byte road needs, and where its bytes start
	/// from. The executor gives every entry an 8 KB slot in its ring buffer, prefilled by memcpy from
	/// `src` when that is non-null (the CPU shadow's bytes for the page — or a version copy the
	/// renderer took before an upload overwrote them) and left for the GPU to compose otherwise
	/// (a page whose every byte comes from a target writeback). The slot is then named by the frame's
	/// page table at [epoch][page], which the flat-road shader, the writeback and the seed all
	/// address bytes through: table[epoch * 512 + page] is the word offset of the slot in the ring.
	/// A page may appear once per epoch range: (page, epoch_first..epoch_last) rows are disjoint.
	struct GSTileGpuRingPage
	{
		u16 page;        ///< 0..GS_MAX_PAGES-1
		u16 epoch_first; ///< first page-table epoch this slot serves
		u16 epoch_last;  ///< last (inclusive)
		u16 pad_;
		const void* src; ///< 8 KB to prefill the slot with, or nullptr
	};

	/// A page entry's `keep_mask_words` when nothing on the page is somebody else's at byte
	/// granularity — the op writes every byte of every block its `block_mask` names.
	static constexpr u32 kGSTileGpuNoKeepMask = 0xFFFFFFFFu;
	/// Words in one page's keep-mask table: 32 blocks x 8 words, one BIT per byte of the block's
	/// 256. A block is 256 bytes in every format, so this table is format-independent.
	static constexpr u32 kGSTileGpuKeepMaskWordsPerPage = 32 * 8;
	/// How GSTileGpuPageEntry::rowcol packs the page's position in its surface: column in the low
	/// bits, row above this shift. Six bits of column because `bw` is a 6-bit GS field, so a column
	/// is 0..62; nine bits of row because the pool wraps at 512 pages and bw = 1 makes every page its
	/// own row. Fifteen bits in all, which is why the field is a u16.
	/// tilegpu_writeback.glsl's TILEGPU_WB_ROW_SHIFT is the same number and
	/// GSDeviceVK::CheckTileGpuShaderContracts holds the two to each other.
	static constexpr u32 kGSTileGpuPageEntryRowShift = 6;

	/// One page of a prep op's page list: which page, and which of its 32 physical blocks the op
	/// touches (a writeback writes only the blocks the surface holds newest; an ordinary seed reads
	/// whole pages and carries kFullBlockMask). A seed reads this field only when its op sets
	/// `seed_blocks_per_page`, which is the upload merge's batched road and nothing else — see
	/// GSTileGpuPrepOp::seed_blocks.
	///
	/// `keep_mask_words` is the WRITEBACK's byte-granular exception, and it exists for one caller:
	/// the CPU->GPU upload merge. A host->local transfer that covers only PART of a block leaves
	/// that block's bytes split — some the CPU's new ones, the rest still the surface's — and the
	/// block mask cannot say so, because it is per block. So the merge stages the page from the CPU
	/// shadow (which carries the transfer's bytes) and then runs a writeback that must fill in the
	/// surface's half WITHOUT overwriting the CPU's. This names the CPU's bytes: an index into the
	/// plan's `writeback_keep_masks` of a kGSTileGpuKeepMaskWordsPerPage table, block-major, 8 words
	/// a block, bit (word*4 + byte) set for a byte the writeback must leave alone. kGSTileGpuNoKeepMask
	/// on every other page entry, which is every entry any other road emits.
	///
	/// `slot` is the page's 8 KB ring slot, as a WORD offset into the ring -- the same value the
	/// frame's epoch page table holds at [op.epoch][page], which is where the writeback shader used
	/// to read it from. The EXECUTOR fills it, not the renderer: only the executor knows where the
	/// ring landed. The renderer leaves it zero and nothing reads it until the executor has written
	/// it. Seed ops carry page entries too and this field means nothing on theirs -- the shader that
	/// reads entries is the writeback and only the writeback.
	///
	/// WHY IT IS CARRIED HERE rather than looked up. The writeback runs 2048 invocations per page
	/// and every one of them read the same four SSBO words: the entry's three, and then the page
	/// table's slot -- a DEPENDENT load, since the table index needs the page the first read
	/// produced. Four loads x 2048 lanes is 8192 of the page's ~12300 memory instructions, all of
	/// them re-reads of four workgroup-uniform values. Carrying the slot makes the entry four words,
	/// which is one 16-byte load through the ring's uvec4 view: one memory instruction per lane
	/// instead of four, and the dependent load gone outright.
	///
	/// WHY IT IS THE SAME WORD THE SHADER WOULD HAVE READ. The epoch page tables are authored on the
	/// CPU, into mapped memory, before the frame records its first command, and nothing on the GPU
	/// timeline writes them: a writeback's stores land inside a page slot (`slot + bib*64 + wib`,
	/// under 2048 words by construction), a CLUT copy lands in the palette region, and the poison
	/// fill lands in slots. So the table is immutable for the frame and resolving it at record time
	/// is the same read moved earlier, not a different one.
	///
	/// WHY ONE SLOT PER ENTRY IS ENOUGH under batching. Entry ranges are disjoint -- an op takes its
	/// range off the end of the array -- so an entry belongs to exactly one op and therefore to
	/// exactly one epoch. GSTileGpuWritebackBatch merges ops only when their `epoch` agrees, so a
	/// merged dispatch cannot ask one entry for two epochs' slots.
	///
	/// `rowcol` is the page's POSITION in the surface's pixel space -- its column and row of pages
	/// off the base page -- packed at kGSTileGpuPageEntryRowShift. The writeback derived it for
	/// itself out of `page`, `bp` and `bw`:
	///
	///     rel = (page - bp/32) mod 512;  col = rel % bw;  row = rel / bw
	///
	/// which is an integer DIVIDE and an integer MODULO, and mobile GPUs have neither. Both lower to
	/// a reciprocal-and-correct sequence tens of instructions long, and this shader ran it in all
	/// 2048 invocations of every page it wrote back, for one answer that is the same in all 2048 --
	/// `bw` is a push constant and `page` is the workgroup's own entry, so nothing in the expression
	/// varies across the workgroup at all. It is not a divide the shader needed; it is a divide it
	/// was repeating.
	///
	/// So the renderer does it once per page, on the CPU, where a divide is a divide, and the answer
	/// rides in the sixteen bits the entry was padding with. The shader reads it out of `page`'s own
	/// word with a shift and a mask -- no extra load, because that word was already in a register:
	/// the entry arrives as one uvec4 (see above) and `rowcol` is the high half of its x. `bp` and
	/// `bw` then have no reader left in the writeback, joining `table_base` and `epoch` as push
	/// fields it declares to keep the shared block's positions and does not use.
	///
	/// WHY NOT A BAKED RECIPROCAL instead, the usual answer to a uniform divisor: it would still cost
	/// a wide multiply, a shift and a multiply-subtract per invocation, and a tenth push-constant
	/// word in a nine-word block four shaders read positionally. This costs two ALU ops and no new
	/// word. And why not broadcast one lane's answer through shared memory: there is nothing to
	/// broadcast -- the value is workgroup-uniform, so every lane already computes the same thing --
	/// and the barrier would reach the dim-8 arm, which has no shared memory at all.
	///
	/// `bw` IS NEVER ZERO on a surface that reaches here. It is the surface's own stride and the
	/// target pool already divides by it unguarded for the same surfaces, in
	/// GSTileTargetPool::HeightForPages, to decide how tall to make the texture this writeback reads.
	/// A zero-stride layout claims the whole page space (PagesForTargetRect) but never gets a
	/// texture, so it never gets a prep op either.
	struct GSTileGpuPageEntry
	{
		u16 page;
		u16 rowcol;
		u32 block_mask;
		u32 keep_mask_words;
		u32 slot;
	};
	/// The array is memcpy'd into the ring verbatim and indexed by the writeback shader as raw
	/// words, so the shader's TILEGPU_WB_ENTRY_WORDS and this size are one fact spelled twice.
	/// Four words, and the shader reads all four as one uvec4, so the size is also an ALIGNMENT
	/// claim: the entry array's base must be four-word aligned, which the executor asserts.
	static_assert(sizeof(GSTileGpuPageEntry) == 4 * sizeof(u32),
		"tilegpu_writeback.glsl walks page entries at TILEGPU_WB_ENTRY_WORDS words each");

	/// The dispatches run at the head of a pass, before its render pass opens: the two
	/// reconciliations between the byte store and a resident target, and the build of a
	/// materialised texture source. All three are keyed through the epoch page table.
	enum class GSTileGpuPrepKind : u8
	{
		/// Target -> bytes: reswizzle a colour target's listed pages into the ring slots the table
		/// names for `epoch`, block-masked, read-modify-write under `byte_mask` (0x00FFFFFF for a
		/// CT24 surface whose alpha byte belongs to someone else). Compute, run before the pass whose
		/// draws sample those pages through the flat road.
		Writeback = 0,
		/// Bytes -> target: unswizzle the listed pages out of the ring slots into a colour target's
		/// pixel space (a fragment pass over the target, discarding fragments outside the page set),
		/// so a draw about to render into pages the surface does not yet hold newest finds them
		/// current. Run before the pass whose draws render into them.
		Seed = 1,
		/// Bytes -> a texture source: unswizzle a whole texture window out of the ring slots into
		/// an ordinary RGBA8 image, one fragment per texel, so a draw can sample it with the
		/// hardware sampler instead of decoding four swizzled loads per bilinear tap. A fragment
		/// pass over the source image; `target` indexes `prep_textures`, NOT the frame's targets —
		/// a source is nothing's render target and never appears in a target pair. The page list is
		/// unused (the window's addresses come from bp/bw and the epoch page table). Always safe to
		/// hoist to the pass head: the ring is staged whole-frame, and the renderer emits this op
		/// after the composition ops for the same window, so array order puts it behind them.
		/// A PALETTED window (PSMT8/PSMT4) materialises to an INDEX image — the index replicated
		/// into all four channels — and takes an Expand after it, below.
		Materialise = 2,
		/// Indices + palette -> colour: one texel per index, `palette[index]`, into an RGBA8 image
		/// the draws actually sample. The (index, palette) split is the whole point of the two-stage
		/// paletted road — an index build serves every palette a game cycles through it, because the
		/// source cache's key deliberately excludes the palette — so this is a second pass rather
		/// than a bake inside the Materialise. `target`, `index_texture` and `palette_texture` all
		/// index `prep_textures`; bp/bw/psm/epoch and the page list are unused. It MUST be emitted
		/// after the Materialise that fills its index (array order is execution order at the pass
		/// head), which the renderer gets for free by queueing them in that order for the same draw.
		Expand = 3,
		/// A resident target -> a texture source, with no byte store in between: the window's texels
		/// are read straight out of the owner surface's texture through the GS swizzle, one fragment
		/// per texel. The road for a window whose pages ONE live target solely owns, which is the
		/// case the byte road serves by writing the target back into the ring and unswizzling it
		/// again — two passes and a megabyte of ring traffic for bytes that never leave the GPU.
		/// `target` indexes `prep_textures` (the destination index image, as a Materialise does),
		/// `donor_target` indexes the frame's `targets` (the owner), `bp`/`bw` are the WINDOW's
		/// TBP0 and pages-per-row, `psm` its TEX0.PSM, and `owner_bp`/`owner_bwpg` the owner's
		/// layout — the four numbers GSDevice::TileReinterpretParams carries. The page list, the
		/// epoch and texa are unused: this road reads no bytes, so it goes through no page table.
		/// Like a Materialise it produces an INDEX image and takes an Expand after it; unlike one it
		/// reads an image an earlier draw may still be writing, so the renderer breaks the pass when
		/// the owner is under the open pass's brush (the WritebackHoistCollides precedent).
		Donor = 4,
		/// A resident target -> a PALETTE. The GS loads its CLUT out of local memory at TEX0-write
		/// time, and when the words it loads were rendered by a native draw they are in the owner
		/// surface's texture and nowhere on the CPU -- so the load is a full GPU drain, seventy times
		/// a frame on GT4 and twelve hundred on GT4-OPB. This gathers them on the device instead: one
		/// fragment per entry, entry -> source word through the CSM1 loaders' own order, word ->
		/// owner texel through the same inverse arithmetic the Donor build uses. `target` indexes
		/// `prep_textures` (the N x 1 palette), `donor_target` the frame's `targets` (the owner),
		/// `bp` is the load's CBP and `owner_bp`/`owner_bwpg` the owner's layout. The entry count is
		/// the destination's width, so it needs no field of its own. Same hoist hazard as a Donor and
		/// the same test.
		ClutGather = 5,
		/// A resident target -> the frame's PALETTE STREAM, as a plain image-to-buffer copy. The
		/// same palette the ClutGather above produces, for the consumer that cannot sample a
		/// texture: a draw on the BYTE road reads its palette words out of the ring buffer, and at
		/// six hundred to twelve hundred CLUT loads a frame a gather PASS each would double the
		/// frame's pass count. A copy costs no pass at all -- it runs at a pass head beside the
		/// writebacks -- so the palette's blocks are copied verbatim and the CSM1 entry order is
		/// applied by the fragment shader at fetch time instead (tilegpu.glsl's
		/// palette-from-copied-block mode). `donor_target` indexes the frame's `targets` (the
		/// owner), `bp` is the DESTINATION word offset within the frame's palette stream,
		/// `copy_x`/`copy_y` are `copy_count` source rects of `copy_w` x `copy_h` texels, copied in
		/// order and each row-major. Same hoist hazard as a Donor and the same test.
		ClutBlockCopy = 6,
		/// Bytes -> a DEPTH target: the Seed above with the depth attachment as its destination. A
		/// full-target triangle whose fragments discard outside the op's page set and otherwise write
		/// `gl_FragDepth` — depth compare ALWAYS, depth write on, no colour attachment at all — with
		/// the guest word taken through the same swizzle the colour seed uses and mapped to the depth
		/// range exactly as the draw road's vertex stage maps a vertex Z.
		///
		/// A separate kind rather than a flag on Seed because everything about the destination
		/// differs: a different pipeline, a different render pass, a different attachment slot. `psm`
		/// selects the program through `gsTileDepthRoadFormat`, NOT `gsTileByteRoadFormat` — a Z
		/// buffer reaches formats the colour road does not carry.
		///
		/// There is no depth WRITEBACK to pair with it: a depth attachment still cannot be turned
		/// back into guest bytes, so truth a depth surface holds is still counted lossy when
		/// something else needs it as bytes.
		SeedDepth = 7,
	};

	/// One prep dispatch. `target` indexes the plan's target list — or, for a Materialise or an
	/// Expand, its `prep_textures` list. bp/bw/psm is the layout that pixel space realises (guest
	/// layout = pixel space, page-aligned base); for a Materialise it is the texture window's TBP0,
	/// its pages per texture row and its TEX0.PSM. The page list is `page_entry_count` rows of the
	/// plan's page_entries from `first_page_entry`. Formats served: PSMCT32/PSMCT24 (Format::Color
	/// targets); the renderer keeps every other surface off this road.
	struct GSTileGpuPrepOp
	{
		GSTileGpuPrepKind kind;
		u32 target;
		u32 bp;
		u32 bw;
		u32 psm;
		u32 byte_mask;
		u32 epoch;
		u32 first_page_entry;
		u32 page_entry_count;
		/// Materialise only: TEXA as the fragment shader packs it (bit 0 = apply, bit 1 = AEM,
		/// bits 8-15 = TA0). A 24-bit window's alpha byte is not its own, so the expansion is baked
		/// into the image at build time — which is what the CPU deswizzlers do, and what the source
		/// cache's key already accounts for (two TEXA settings are two entries). Zero for a paletted
		/// window: TEXA rides in the CLUT expansion there, which GSClut::Read32 has already applied
		/// to the palette words.
		u32 texa;
		/// Expand only: the index image and the N x 1 palette, both indices into `prep_textures`.
		u32 index_texture;
		u32 palette_texture;
		/// Donor only: the OWNER, as an index into the frame's `targets` list. It is a target rather
		/// than a prep texture because it is one -- a surface the page model owns and passes render
		/// into -- and it is a separate field from `target` for exactly that reason: the two index
		/// different lists and sharing the field would read a source as a surface.
		u32 donor_target;
		/// Donor only: the owner's own layout, which is the pixel space its texture realises
		/// (page-aligned base in blocks, width in pages). With bp/bw above these are the four
		/// numbers the reinterpretation needs; nothing else about the owner reaches the shader.
		u32 owner_bp;
		u32 owner_bwpg;
		/// Seed / SeedDepth only: the in-page BLOCKS the seed may write, one bit each, the same form
		/// the writeback's page entries carry — and uniform over the op, unlike theirs. kFullBlockMask
		/// on every ordinary seed, because a seed exists to make a whole page current.
		///
		/// The upload merge is the exception it is here for: that seed is repairing the blocks ONE
		/// surface holds after a CPU transfer landed in them, and the rest of the page belongs to the
		/// CPU — whose bytes are perfectly correct in the byte store but are not this surface's
		/// texels, and writing them over what the texture holds is a change nothing asked for. So a
		/// merge seed names blocks, and the blocks differ from page to page.
		u32 seed_blocks;
		/// ...and when that is set, the mask above is not the answer: each page entry carries its own
		/// `block_mask` and the op names them all in ONE render pass instead of one pass per page.
		/// The executor stages a per-page table beside the op's page mask and the seed shader reads
		/// it; `seed_blocks` is kFullBlockMask and unread. EmuCore/GS/TileGpuMergeSeedBatch, and the
		/// only road that sets it is GSRendererTileGpu's upload merge.
		///
		/// ⚠️ It is a SEPARATE field rather than a sentinel value of `seed_blocks` because
		/// kFullBlockMask is a legitimate answer for a merged page — an owner that holds all 32
		/// blocks of a partially-overwritten page — and a sentinel would silently seed the whole
		/// page from the CPU's bytes there.
		u32 seed_blocks_per_page;
		/// Seed only: this seed is the upload merge's. Carried for the census alone — the executor
		/// counts the render passes it opens for one, which is the only place they can be counted:
		/// the plan-pass counter cannot see a seed pass, and the merge's pass bill was invisible to
		/// every number this campaign took until it was.
		u32 seed_from_merge;
		/// ClutBlockCopy only: the source rects in the owner's texture, copied in this order into
		/// consecutive `copy_w` x `copy_h` word runs of the destination. Four 8x8 blocks for a
		/// 256-entry palette, one 8x2 for a 16-entry one (GSTileSwizzleForms::LocateClutBlocks).
		u32 copy_count;
		u32 copy_w, copy_h;
		u32 copy_x[4], copy_y[4];
		/// ClutBlockCopy only: the destination as a TILE rather than as consecutive runs — its row
		/// pitch in words, and where each region's first word sits inside it.
		///
		/// Zero stride is the derivation the road has always used: the destination rows are `copy_w`
		/// wide and region b starts at `b * copy_w * copy_h`, which is what "the regions follow one
		/// another" means. Every 32-bit copy produces that and is byte-identical under either
		/// spelling. A non-zero stride says the regions are placed instead, and `copy_off[b]` is
		/// region b's word offset from the op's own base — the four blocks of a 16-bit owner's
		/// 256-entry palette land as the four quadrants of one 32x16 tile whatever CBP's alignment,
		/// so their consumer reads one word order rather than two.
		u32 copy_stride;
		u32 copy_off[4];

		/// The destination row pitch this op's copy asks for, in words (VkBufferImageCopy's
		/// bufferRowLength). Both derivations in one place because the executor and the suite must
		/// not spell them separately.
		constexpr u32 CopyRowLength() const { return (copy_stride != 0) ? copy_stride : copy_w; }
		/// ...and region b's word offset from the op's base (bufferOffset, less the stream's own
		/// base). Placed where the op says so, and tiled where it does not.
		constexpr u32 CopyRegionOffset(u32 b) const
		{
			return (copy_stride != 0) ? copy_off[b] : (b * copy_w * copy_h);
		}
	};

	/// The blocks a seed may write on the page one of its entries names. The op's uniform
	/// `seed_blocks` is the answer for every seed but the upload merge's batched one, which carries
	/// the mask per page in the entry.
	///
	/// It is stated here because tilegpu_seed.glsl spells the same rule and the two must not drift:
	/// a seed that read the wrong mask does not fail, it writes the CPU's bytes over a surface's
	/// texels (or drops the repair the merge exists to make) and the frame looks nearly right.
	static constexpr u32 SeedBlocksOnPage(u32 seed_blocks, u32 seed_blocks_per_page, u32 entry_block_mask)
	{
		return (seed_blocks_per_page != 0) ? entry_block_mask : seed_blocks;
	}

	/// THE WRITEBACK BATCH -- how a run of Writeback ops becomes fewer commands than it has ops.
	///
	/// Each writeback op is one compute dispatch with a whole-buffer ring barrier on each side, and
	/// on a tiler that bracket is a cache flush plus an invalidate plus a drain: the CLUT copy run
	/// measured the same bracket at 14.3 us around a 1 KB copy against 1.4 us outside it. The
	/// campaign's fit prices a writeback op at 3.67 us of fixed cost plus 5.74 us a page, and
	/// Spider-Man 3 records 1,101 ops a frame -- 4.0 ms that buys no swizzling. The pages are
	/// already minimal (the compose road is demand-driven at page granularity and 98% consumed);
	/// the OPS are not.
	///
	/// So consecutive writeback ops are batched two ways at once, and both rest on one clause:
	///
	///   THE BRACKET. A maximal run of writeback ops gets ONE barrier pair instead of one each.
	///   Nothing between them reads or writes the ring -- every other op kind closes the run before
	///   it records anything, exactly as the CLUT copy run does -- so the interior pairs order
	///   nothing.
	///
	///   THE DISPATCH. Inside a run, ops that name the SAME source image with the SAME layout, byte
	///   mask and epoch, and whose page entries are contiguous in the plan's array, become one
	///   dispatch with the entry range concatenated. The shader takes its entry as
	///   gl_WorkGroupID.z off first_entry, so a longer range is a taller dispatch and nothing else;
	///   no shader, no push constant and no descriptor changes.
	///
	/// THE LOAD-BEARING CLAUSE IS PAGE DISJOINTNESS, and it is checked rather than argued. Two ops
	/// that name one page write disjoint BYTE lanes of the same words -- a PSMCT24 surface owning
	/// bytes 0-2 while another surface holds the alpha plane -- which is a read-modify-write race
	/// once the barrier between them is gone, and a race between z groups of one dispatch even with
	/// it. Any op whose pages the run has already claimed closes the run and opens a new one, so
	/// the case costs a bracket and never correctness. It is rare (a compose marks its pages synced,
	/// so a second op reaches the same page only through the other plane's owner) and the check is
	/// eight words of OR per op.
	///
	/// The class is a pure state machine over the op array -- no device, no Vulkan -- so the
	/// executor and the renderer's census walk the same decisions, and both are pinned in
	/// gs_tilegpu_writeback_batch_tests.cpp.
	class GSTileGpuWritebackBatch
	{
	public:
		/// One dispatch to record: an op's parameters, with `page_entry_count` covering every op
		/// that folded into it.
		struct Dispatch
		{
			GSTileGpuPrepOp op;
			u32 op_count; ///< ops merged into it -- the census's unit and the drop count's
		};

		/// What the caller must record for the op just offered, IN THIS ORDER. `flush_dispatch`
		/// first (the accumulated dispatch is complete and `Flushed()` names it), then `close_run`
		/// (the run's publishing barrier), then `open_run` (end the render pass and take the new
		/// run's acquiring barrier). All three false means the op folded into what is already
		/// accumulating and there is nothing to record at all.
		struct Step
		{
			bool flush_dispatch = false;
			bool close_run = false;
			bool open_run = false;
		};

		void Reset()
		{
			m_run_open = false;
			m_have_pending = false;
			m_pages.clear();
		}

		/// Offer the next WRITEBACK op. `entry_base` is the plan's whole page-entry array; the op's
		/// rows are the `page_entry_count` from `first_page_entry`.
		Step Offer(const GSTileGpuPrepOp& op, const GSTileGpuPageEntry* entry_base)
		{
			pxAssert(op.kind == GSTileGpuPrepKind::Writeback && op.page_entry_count > 0);
			Step s;
			if (!m_run_open || Collides(op, entry_base))
			{
				// A hazard, or nothing open: publish what the run has, close it, start another.
				s.flush_dispatch = TakePending();
				s.close_run = m_run_open;
				s.open_run = true;
				m_run_open = true;
				m_pages.clear();
			}
			else if (Merges(op))
			{
				m_pending.op.page_entry_count += op.page_entry_count;
				m_pending.op_count++;
				AddPages(op, entry_base);
				return s;
			}
			else
			{
				// Same run -- no barrier -- but a different image or layout, so its own dispatch.
				s.flush_dispatch = TakePending();
			}
			m_pending.op = op;
			m_pending.op_count = 1;
			m_have_pending = true;
			AddPages(op, entry_base);
			return s;
		}

		/// End of the op range, or the first op of any other kind: publish and close.
		Step Finish()
		{
			Step s;
			s.flush_dispatch = TakePending();
			s.close_run = m_run_open;
			m_run_open = false;
			m_pages.clear();
			return s;
		}

		/// Valid only immediately after a Step with `flush_dispatch` set.
		const Dispatch& Flushed() const { return m_flushed; }

		/// The dispatches a range of prep ops collapses to, by the same walk the executor makes.
		/// The renderer's census and the tests share it so the counter cannot drift from the
		/// commands.
		static u32 CountDispatches(const GSTileGpuPrepOp* ops, u32 op_count, const GSTileGpuPageEntry* entry_base)
		{
			GSTileGpuWritebackBatch b;
			u32 dispatches = 0;
			for (u32 i = 0; i < op_count; i++)
			{
				if (ops[i].kind != GSTileGpuPrepKind::Writeback)
				{
					dispatches += b.Finish().flush_dispatch ? 1u : 0u;
					continue;
				}
				// An op with no pages is not an op -- EmitPrepOp files none, and the executor skips
				// it ahead of the arm, so it must not cost the run its bracket here either.
				if (ops[i].page_entry_count == 0)
					continue;
				dispatches += b.Offer(ops[i], entry_base).flush_dispatch ? 1u : 0u;
			}
			return dispatches + (b.Finish().flush_dispatch ? 1u : 0u);
		}

		/// The barrier BRACKETS the same range opens, which is the other half of what a batch saves.
		/// `hazards_out` takes the brackets a PAGE COLLISION forced rather than an op of another
		/// kind -- the census that says whether the disjointness clause is costing anything.
		static u32 CountRuns(const GSTileGpuPrepOp* ops, u32 op_count, const GSTileGpuPageEntry* entry_base,
			u32* hazards_out = nullptr)
		{
			GSTileGpuWritebackBatch b;
			u32 runs = 0;
			for (u32 i = 0; i < op_count; i++)
			{
				if (ops[i].kind != GSTileGpuPrepKind::Writeback)
				{
					b.Finish();
					continue;
				}
				if (ops[i].page_entry_count == 0)
					continue;
				// A bracket that also CLOSED one is a hazard split: an op of any other kind would have
				// closed the run through Finish above and left nothing open to close here.
				const Step s = b.Offer(ops[i], entry_base);
				runs += s.open_run ? 1u : 0u;
				if (hazards_out && s.open_run && s.close_run)
					(*hazards_out)++;
			}
			b.Finish();
			return runs;
		}

	private:
		bool Merges(const GSTileGpuPrepOp& op) const
		{
			// Everything the dispatch fixes: the descriptor (target), the program and page geometry
			// (psm), the three push constants that address the surface (bp, bw, byte_mask), the page
			// table the slots come out of (epoch) -- and the entry range, which the shader walks
			// linearly from first_entry and cannot be given a hole in.
			return m_have_pending && m_pending.op.target == op.target && m_pending.op.psm == op.psm &&
				   m_pending.op.bp == op.bp && m_pending.op.bw == op.bw &&
				   m_pending.op.byte_mask == op.byte_mask && m_pending.op.epoch == op.epoch &&
				   (m_pending.op.first_page_entry + m_pending.op.page_entry_count) == op.first_page_entry;
		}

		bool Collides(const GSTileGpuPrepOp& op, const GSTileGpuPageEntry* entry_base) const
		{
			for (u32 i = 0; i < op.page_entry_count; i++)
			{
				if (m_pages.test(entry_base[op.first_page_entry + i].page))
					return true;
			}
			return false;
		}

		void AddPages(const GSTileGpuPrepOp& op, const GSTileGpuPageEntry* entry_base)
		{
			for (u32 i = 0; i < op.page_entry_count; i++)
				m_pages.set(entry_base[op.first_page_entry + i].page);
		}

		bool TakePending()
		{
			if (!m_have_pending)
				return false;
			m_flushed = m_pending;
			m_have_pending = false;
			return true;
		}

		GSPageBitmap m_pages;    ///< every page the OPEN run has claimed
		Dispatch m_pending = {}; ///< the dispatch still accumulating
		Dispatch m_flushed = {}; ///< the last one Take'd, for the caller to record
		bool m_run_open = false;
		bool m_have_pending = false;
	};

	/// A draw's depth configuration, which selects the depth pipeline variant. GS depth grows
	/// towards the viewer, so the test is GREATER_OR_EQUAL when the draw tests and ALWAYS when it
	/// only writes; the write follows ZMSK independently of the test.
	///
	/// PER DRAW, not per pass: it is two bits of VkPipelineDepthStencilStateCreateInfo, so the
	/// executor binds it per indirect run exactly as it binds the topology and the blend. Whether a
	/// pass is nonetheless kept depth-UNIFORM is the device's call — see
	/// TileGpuPrefersDepthUniformPasses and GSTileGpuPassPlan::depth_modes.
	enum class GSTileGpuDepthMode : u8
	{
		None = 0,        ///< no depth attachment (ZTE off, or neither test nor write)
		TestWrite = 1,   ///< GEQUAL, depth write on  (ZTST tests, ZMSK clear)
		TestNoWrite = 2, ///< GEQUAL, depth write off (ZTST tests, ZMSK set)
		WriteAlways = 3, ///< ALWAYS, depth write on  (ZTST ALWAYS, ZMSK clear)
	};
	static constexpr u32 kGSTileGpuDepthModes = 4;

	/// The texel roads a textured draw can take, as a bit per road. A pass's road_mask is the OR
	/// over its draws, and the fragment shader compiles in exactly the roads the mask names — dead
	/// code costs program size on the tiler targets, so a pass that never samples a resident target
	/// must not carry that road's instructions. An untextured draw contributes no bit.
	static constexpr u32 kGSTileGpuRoadByte = 1u << 0;   ///< decode guest bytes out of the frame's ring
	static constexpr u32 kGSTileGpuRoadTarget = 1u << 1; ///< sample a resident target directly (rule 2)
	static constexpr u32 kGSTileGpuRoadSource = 1u << 2; ///< sample a materialised texture source (rule 3)
	static constexpr u32 kGSTileGpuRoadMaskAll =
		kGSTileGpuRoadByte | kGSTileGpuRoadTarget | kGSTileGpuRoadSource;

	/// The BYTE road's texel-decode ARMS, as a bit per arm. The byte road is not one decoder, it is
	/// five address geometries behind a shared wrapper, and the per-texel tap is inlined five times
	/// over by bilinear sampling — so a pass sampling nothing but PSMT8 was paying program size for
	/// four geometries it never executes, times five. A pass's texel_mask is the OR over its
	/// byte-road draws, exactly as road_mask is the OR over its draws' roads.
	///
	/// Formats sharing an address geometry share an arm, deliberately: the three alpha-byte views
	/// differ by one bitfield extract and the four 16-bit families by two selects, so splitting
	/// those buys a handful of instructions and multiplies the variant population for nothing.
	static constexpr u32 kGSTileGpuTexelDirect32 = 1u << 0; ///< PSMCT32 / PSMCT24 / PSMZ32 / PSMZ24
	static constexpr u32 kGSTileGpuTexelIndex8 = 1u << 1;   ///< PSMT8
	static constexpr u32 kGSTileGpuTexelIndex4 = 1u << 2;   ///< PSMT4
	static constexpr u32 kGSTileGpuTexelIndexHi = 1u << 3;  ///< PSMT8H / PSMT4HL / PSMT4HH
	static constexpr u32 kGSTileGpuTexelDirect16 = 1u << 4; ///< PSMCT16 / PSMCT16S / PSMZ16 / PSMZ16S
	/// Not an address geometry: the CSM1 entry order a GATHERED palette needs. A palette the CPU
	/// expanded is one indexed load per entry; one copied out of the target a native draw rendered it
	/// into lands texel row-major, so three more closed forms have to run per tap to find an entry.
	/// Two of the eighteen corpus dumps ever gather a palette, and this bit is what keeps the other
	/// sixteen from carrying the machinery — which is the commit that crossed the a650 cliff.
	static constexpr u32 kGSTileGpuTexelPalGather = 1u << 5;
	/// The same statement for a palette gathered off a SIXTEEN-BIT owner, which is a different entry
	/// order and a different fetch: a 16-bit surface stores two cells per guest word, so a palette
	/// word is the pack of two texels eight apart in x rather than one texel read.
	///
	/// Its OWN arm rather than a widening of the one above, and the reason is the a650 budget: the two
	/// orders are alternatives, never a union. A pass gathering a 16-bit palette compiles this arm
	/// INSTEAD of the 32-bit one, so the widest paletted variant carries one gather's arithmetic
	/// either way. A shared bit would have made it carry both.
	static constexpr u32 kGSTileGpuTexelPalGather16 = 1u << 6;
	/// Just the address geometries: the population "does this pass mix formats" is asked about.
	static constexpr u32 kGSTileGpuTexelGeometryMask = kGSTileGpuTexelDirect32 | kGSTileGpuTexelIndex8 |
													   kGSTileGpuTexelIndex4 | kGSTileGpuTexelIndexHi |
													   kGSTileGpuTexelDirect16;
	/// The paletted geometries: the ones the gather arms mean anything for.
	static constexpr u32 kGSTileGpuTexelPalettedMask =
		kGSTileGpuTexelIndex8 | kGSTileGpuTexelIndex4 | kGSTileGpuTexelIndexHi;
	/// Both gather orders. Never both set on one pass -- see the note on the 16-bit arm.
	static constexpr u32 kGSTileGpuTexelPalGatherMask = kGSTileGpuTexelPalGather | kGSTileGpuTexelPalGather16;
	static constexpr u32 kGSTileGpuTexelMaskAll = kGSTileGpuTexelGeometryMask | kGSTileGpuTexelPalGatherMask;
	static constexpr u32 kGSTileGpuTexelArms = 7;

	/// What the in-pass destination read is FOR, as a bit per use. A pass's self_mask is the OR over
	/// the draws it admitted to the read, and it selects the fragment variant beside road_mask and
	/// texel_mask, for the same reason those exist: a pass that only needs the destination-alpha bit
	/// must not carry the blend equation's arithmetic. Zero = the pass reads nothing and is compiled,
	/// bound and submitted exactly as it was before this road existed.
	static constexpr u32 kGSTileGpuSelfDate = 1u << 0;  ///< the destination-alpha test reads the live pixel, not a snapshot
	static constexpr u32 kGSTileGpuSelfBlend = 1u << 1; ///< the GS blend equation is evaluated in the shader, in integer
	static constexpr u32 kGSTileGpuSelfMask = 1u << 2;  ///< FBMSK is honoured at BIT granularity by merging the destination
	static constexpr u32 kGSTileGpuSelfMaskAll =
		kGSTileGpuSelfDate | kGSTileGpuSelfBlend | kGSTileGpuSelfMask;
	static constexpr u32 kGSTileGpuSelfUses = 3;

	/// The arm a state row's index_format decodes through. The numbering is the renderer's (0 =
	/// direct 32-bit in the CT32 block space, 1-5 = GSTileSwizzleForms::IndexFormatFor + 1,
	/// 6-9 = Direct16FormatFor + 6, 10 = direct 32-bit in the DEPTH block space) and the fragment
	/// shader switches on the same value, so this mapping is the third party that has to agree with
	/// both — pinned in the gs suite rather than left to inspection.
	///
	/// ⚠️ 10 lands on the SAME arm as 0. The depth pair is the colour pair's address geometry under
	/// one constant block XOR, which is a select inside the arm rather than a second arm — the same
	/// call the 16-bit families' two depth members already take.
	static constexpr u32 GSTileGpuTexelArm(u32 index_format)
	{
		if (index_format == 0)
			return kGSTileGpuTexelDirect32;
		if (index_format == 1)
			return kGSTileGpuTexelIndex8;
		if (index_format == 2)
			return kGSTileGpuTexelIndex4;
		if (index_format < 6)
			return kGSTileGpuTexelIndexHi;
		if (index_format < 10)
			return kGSTileGpuTexelDirect16;
		return kGSTileGpuTexelDirect32;
	}

	/// The per-draw GS state a fragment program may take as a COMPILE-TIME CONSTANT rather than
	/// reading it out of the draw's state row. Every field here is one the fragment stage otherwise
	/// loads and branches on, and freezing one pays three times over on the Adreno 650: the load goes
	/// (a state-row read compiles to a cat5 `isam` with its own address arithmetic, and there are
	/// seventeen of them, because `v_row` is a flat per-primitive varying that nothing can hoist into
	/// a preamble); the compare/select chain the field fed goes; and the live state that held the
	/// textured roads at twelve to sixteen registers goes with it. That last one is a THRESHOLD, not
	/// a gradient — mesa's `regs * 2 <= reg_size_vec4 / 4` with a650's `reg_size_vec4 = 64` means a
	/// program at eight full registers or fewer runs wave128 (1280 fragments in flight) and one at
	/// nine runs wave64 (640). Measured offline against the device's own Turnip (26.1.2, a650): the
	/// materialised-source road goes 390 instructions / 12 registers / wave64 to 89 / 4 / wave128,
	/// the resident-target road 663 / 12 / wave64 to 172 / 6 / wave128, and the untextured road 192
	/// / 4 to 16 / 3.
	///
	/// `valid` false means every field is read from the row, which is what the PASS-UNION fallback
	/// must always do: a program standing in for many draws cannot freeze what they disagree about.
	///
	/// ⚠️ A specialized program must not move a pixel, and that holds only because each axis SELECTS
	/// an arm and never rewrites one. `tex_enable` is deliberately absent: a draw's road mask is
	/// non-zero exactly when it samples, so the textured block's own gate is already answered by the
	/// road, and spending a key bit on it would only let the two disagree.
	struct GSTileGpuFragmentSpec
	{
		bool valid = false;
		u8 fst = 0;  ///< StateRow::fst — 0 = STQ coordinates, 1 = UV
		u8 ltf = 0;  ///< StateRow::ltf — 0 = NEAREST, 1 = LINEAR
		u8 tfx = 0;  ///< TEX0.TFX — 0 MODULATE, 1 DECAL, 2 HIGHLIGHT, 3 HIGHLIGHT2
		u8 tcc = 0;  ///< TEX0.TCC — 1 = the texel carries alpha
		u8 atst = 0; ///< StateRow::atst — 0 = no test, else TEST.ATST + 1 (3 LESS .. 8 NOTEQUAL)
		u8 fge = 0;  ///< PRIM.FGE — 1 = fogged
		u8 date = 0; ///< StateRow::date — 0 off, 1 = DATM 0, 2 = DATM 1
		u8 wms = 0;  ///< CLAMP.WMS 0..3
		u8 wmt = 0;  ///< CLAMP.WMT 0..3
		u8 texa = 0; ///< the low two bits of StateRow::texa — bit 0 apply, bit 1 AEM
		/// Whether TEXA is frozen at all. False only where NarrowToDriver has taken it back off this
		/// driver; it is a DEVICE-side narrowing and never rides the plan's variant key, so a plan
		/// always arrives with it set. That costs no key ambiguity, because it is constant for a
		/// session: on a driver that freezes, `texa == 0` means "frozen to no TEXA", and on one that
		/// does not, it means "read the row" — never both in the same process.
		bool texa_frozen = true;

		/// Zero every field the program THIS road mask compiles cannot read, so two draws whose
		/// programs would come out character-identical do not become two keys, two modules, two
		/// pipelines and two indirect calls.
		///
		/// Two rules, both structural (tilegpu.glsl's own `#if`s, not a guess about what pays):
		/// the texture function, the coordinate kind, the filter and the wrap modes live inside
		/// TILEGPU_TEXTURED, which no road at all takes out; and TEXA lives inside TILEGPU_TAP_ANY,
		/// the two roads that go through the per-texel tap — a materialised source (rule 3) has its
		/// TEXA baked into the image at build time and the shader never applies it again.
		constexpr void NarrowToRoad(u32 road_mask)
		{
			if (road_mask == 0)
			{
				fst = ltf = tfx = tcc = wms = wmt = texa = 0;
				return;
			}
			if ((road_mask & (kGSTileGpuRoadByte | kGSTileGpuRoadTarget)) == 0)
				texa = 0;
		}

		/// The DEVICE half of the same narrowing: an axis this driver may not be trusted to freeze.
		///
		/// `freeze_texa` is false on Honeykrisp only. Freezing TEXA there moves six pixels of one
		/// corpus frame — green channel, |delta| <= 8, on two 2x2 quads of a bilinear byte-road draw
		/// — and it is not a source defect: the frozen constants provably equal the row's (a probe
		/// that paints on disagreement never fires), every contractible float op already carries
		/// NoContraction, and the same binary is byte-identical on all eighteen corpus dumps under
		/// lavapipe. It takes the interaction of five frozen axes to appear and any one of them read
		/// back off the row hides it again, which makes it a code-shape effect. Same driver, same
		/// signature and same class as the dynamic byte-extract miscompile the shader's
		/// TILEGPU_STATIC_BYTE_SEL form already works around; this is that bug's second face, and
		/// the workaround is the same shape — keyed on the driver, costing every other driver
		/// nothing. Adreno and Mali freeze all ten axes.
		///
		/// It runs before the spec enters ANY key, so the module cache, the pipeline cache and the
		/// #define block on that driver all agree that TEXA is not frozen. Nothing compiles twice.
		constexpr void NarrowToDriver(bool freeze_texa)
		{
			if (freeze_texa)
				return;
			texa = 0;
			texa_frozen = false;
		}

		constexpr bool operator==(const GSTileGpuFragmentSpec& o) const
		{
			return valid == o.valid && fst == o.fst && ltf == o.ltf && tfx == o.tfx && tcc == o.tcc &&
				   atst == o.atst && fge == o.fge && date == o.date && wms == o.wms && wmt == o.wmt &&
				   texa == o.texa && texa_frozen == o.texa_frozen;
		}
		constexpr bool operator!=(const GSTileGpuFragmentSpec& o) const { return !(*this == o); }
	};

	/// One GS-semantic minimum pass: a contiguous run of draws sharing a set of FRAME/ZBUF
	/// target pairs (up to the pass model's per-pass budget, GSTilePassSim::kMaxTargetPairs),
	/// optionally declaring the raster-order self-read the blend and same-pixel feedback
	/// design needs. The ranges index the like-named plan arrays.
	struct GSTileGpuPass
	{
		u32 first_draw;
		u32 draw_count;
		u32 first_target_pair;
		u32 target_pair_count;
		u32 first_snapshot; ///< snapshot copies taken before this pass opens
		u32 snapshot_count;
		u32 first_prep_op; ///< reconciliation dispatches (writebacks, seeds) run before this pass opens, in order
		u32 prep_op_count;
		/// The resident targets this pass's draws sample directly (the VRAM model's rule 2), as a
		/// range of the plan's tex_sources. The executor binds them as this pass's sampled-target
		/// array, in this order: a draw's state row names the slot by its position in the range.
		/// None of them may be this pass's own colour or depth attachment -- a draw sampling what
		/// its pass writes is the in-pass feedback road, and the planner breaks the pass instead.
		u32 first_tex_source;
		u32 tex_source_count;
		/// The OR of this pass's draws' texel roads (kGSTileGpuRoad*), zero for a pass whose draws
		/// are all untextured.
		///
		/// ⚠️ This is the pass's UNION and it is the FALLBACK, not the fragment variant a draw runs.
		/// The variant is per DRAW and rides the run key (GSTileGpuPassPlan::variant_keys); a plan
		/// carrying none leaves every run on these masks, which is what the executor did before the
		/// per-run key existed. The union still has a job: it is what the pass's descriptor binding
		/// and the size gate's variant census are about.
		u32 road_mask;
		/// The OR of the decode arms this pass's BYTE-road draws need (kGSTileGpuTexel*) — one of the
		/// five address geometries per draw, plus the palette-order arm where a draw's palette was
		/// gathered off a target. Zero for a pass that takes no byte road at all. Union and fallback,
		/// exactly as road_mask above. A pass whose road_mask carries the byte bit always names at
		/// least one geometry — a byte-road draw is textured by definition — and the device treats an
		/// empty one as the full set, because a superset is slow and a subset is wrong.
		u32 texel_mask;
		/// The OR of what this pass's draws need the in-pass destination read FOR (kGSTileGpuSelf*),
		/// zero for a pass no draw of which reads. Union and fallback, as above. Non-zero is exactly
		/// `declares_self_read`, and THAT half stays a pass property whatever the run key says: the
		/// declaration is an input-attachment reference in the render pass, so every pipeline the pass
		/// binds has to be built against it, reader or not.
		u32 self_mask;
		/// This pass renders into a frame format that stores fewer bits than the target's RGBA8 image
		/// holds (CT16 / CT16S), so its draws have to say what the console would have stored. Union
		/// and fallback: a pass has ONE colour surface and therefore one format, but a draw whose
		/// blend the executor's blend unit still runs is quantised by the console AFTER that blend
		/// and takes nothing here — so the arithmetic is per draw even though the format is not.
		bool quantises_frame;
		bool declares_self_read; ///< ROAA: the pass reads its own colour target in raster order
	};

	/// One frame, structured. The streams are CPU-side views the executor stages into its own
	/// device buffers (the shared-SSBO idiom: one whole-buffer descriptor plus a per-frame
	/// base offset). state_stride is the byte size of one state row and is opaque here — its
	/// layout is the TFX-on-storage-buffer backend's, not this contract's.
	struct GSTileGpuPassPlan
	{
		static constexpr u32 kNoTarget = 0xFFFFFFFFu;
		/// A state row's tex_target when the draw takes the byte road (it decodes guest bytes out
		/// of the ring) rather than sampling a resident target.
		static constexpr u32 kNoTexSlot = 0xFFFFFFFFu;
		/// How many distinct resident targets one pass may sample. The executor binds exactly this
		/// many descriptors per pass and the fragment shader declares an array of this size, so it
		/// is a contract constant, not a tunable either side can pick alone. A draw whose source
		/// would be the ninth opens a new pass.
		static constexpr u32 kMaxTexSourcesPerPass = 8;
		/// How many distinct MATERIALISED sources (rule 3) one frame may bind. Unlike the per-pass
		/// target array this one is frame-scoped — a source belongs to a texture window, not to a
		/// target pair, so the same image serves draws in any number of passes and the executor writes
		/// the array once per plan. A draw whose source would be the (n+1)th takes the byte road and is
		/// counted; 128 rather than 64 because the corpus census found GT4 and OutRun pinned at 64
		/// distinct windows per frame with draws refused behind them.
		static constexpr u32 kMaxSources = 128;
		/// A state row's tex_source when the draw does not take rule 3.
		static constexpr u32 kNoSourceSlot = 0xFFFFFFFFu;
		/// How many 32-bit words one state row is. THREE things have to agree on this number and none
		/// of them can check the others at compile time: the renderer's C++ StateRow, tilegpu.glsl's
		/// std430 StateRow, and this executor gate. Two sides on different strides read every row but
		/// the first from the wrong place -- silently, because a row is untyped bytes and every value
		/// in it is in range for something. So it is one constant with three readers rather than three
		/// literals, and the shader's own copy is parsed and compared at load (GSTileGpuShaderVariant::
		/// StateRowWordsIn), which is the only check that can catch a shader tree from another revision.
		static constexpr u32 kStateRowWords = 36;

		std::span<const GSTileGpuPass> passes;
		std::span<const GSTileGpuIndirectDraw> draws;
		std::span<const GSTileGpuTopology> topologies; ///< one per draw, parallel to `draws`
		/// One per draw, parallel to `draws`: the GS scissor in target pixels, [x0, x1) x [y0, y1).
		/// It both cuts the indirect call and supplies the rectangle the executor sets before it.
		/// Required, like the topology: the scissor is not in the state row and there is no pass-wide
		/// rectangle to fall back on, so a draw whose scissor the executor guessed would write pixels
		/// the GS rejects.
		std::span<const GSVector4i> scissors;
		/// One per draw, parallel to `draws`: the draw's depth pipeline variant. Required, like the
		/// topology and unlike the blend key — there is no per-pass depth mode to fall back to, and a
		/// draw whose depth state the executor guessed would write depth it must not write.
		///
		/// A pass's depth ATTACHMENT is still uniform (a render pass cannot gain or lose one), so
		/// None appears here exactly in the passes whose target pair carries no zbuf_target — the
		/// executor asserts that per run. Whether the three depth-carrying modes are also uniform
		/// within a pass depends on the device: see TileGpuPrefersDepthUniformPasses.
		std::span<const GSTileGpuDepthMode> depth_modes;
		/// One per draw, parallel to `draws`: the fixed-function blend the draw takes, or 0 for
		/// none. Bit 31 set = blend enabled; bits 0-6 = the GS ALPHA (A,B,C,D) index into
		/// GSDevice::m_blendMap (A*27 + B*9 + C*3 + D); bits 8-15 = ALPHA.FIX when C selects the
		/// fixed factor (it rides as the blend constant, dynamic state, not pipeline state). Like
		/// the topology it is pipeline state, so the constant-cost submission splits its indirect
		/// runs on it -- a split, never a pass break.
		std::span<const u32> blend_keys;
		static constexpr u32 kBlendEnable = 0x80000000u;
		/// Bits 16-19: the colour channels the draw does NOT write — R at bit 16, G at 17, B at
		/// 18, A at 19 — which the pipeline realizes as its per-channel colour write mask. It is
		/// the PRESERVE sense rather than the write sense so that zero means "write all four":
		/// that is both the overwhelming majority of draws and what a plan carrying no blend keys
		/// at all falls back to. 0xF is a depth-only draw (the GS AFAIL ZB_ONLY fold, or an FBMSK
		/// that keeps every bit the frame format stores); 0x8 is RGB without alpha (AFAIL
		/// RGB_ONLY, and FBMSK=0xFF000000 — Ace Combat 5's "write colour, keep alpha" repaint,
		/// see gsTileFrameColorWriteMask); and the partial-FBMSK population lives in between —
		/// OutRun 2006's world-erasing post sprites and Beyond Good & Evil's alpha-mask
		/// silhouettes are both 0x7, alpha alone.
		static constexpr u32 kNoWriteShift = 16;
		static constexpr u32 kNoWriteMask = 0xFu << kNoWriteShift;
		/// Pack a 4-bit rgba WRITE mask (the sense GSTileTypes.h's gsTileFrameColorWriteMask
		/// returns) into the blend key's preserve field.
		static constexpr u32 PackNoWrite(u32 write_mask)
		{
			return ((~write_mask) & 0xFu) << kNoWriteShift;
		}
		/// Bit 30: this draw reads its own destination pixel in rasterization order, and does in the
		/// fragment stage whatever the fixed-function state could not express for it -- the blend
		/// equation, the bit-granular write mask, the destination-alpha test, or several at once.
		/// It rides HERE rather than in a stream of its own because the blend key is already the run
		/// key, the pipeline key and the pass's per-draw pipeline state: putting the flag in it makes
		/// the indirect run cut, the pipeline pick and the pass's declaration fall out of one bit.
		/// A draw carrying it gets fixed-function blending disabled (bits 0-15 are then the equation
		/// the SHADER evaluates, out of the state row, and the executor must not also apply it) and
		/// keeps its channel write mask, which stays exact for every channel the mask covers whole.
		static constexpr u32 kSelfRead = 1u << 30;
		/// Bit 29: ...and of those, this draw's BLEND is one of the things the shader does itself, so
		/// the fixed-function blend must be off for it. Separate from kSelfRead because reading is not
		/// blending: a draw admitted only for its destination-alpha test still wants the executor's
		/// blend, and turning it off there silently renders every composite sprite unblended.
		static constexpr u32 kSelfBlend = 1u << 29;
		/// Bits 20-21: which GSTileGpuDualSrcRoad this draw's As blend factor takes, as 0 for
		/// "the index-1 output, or no As factor at all" and the three feature-free roads above it.
		/// It is pipeline state -- it decides which Vulkan factors the row's SRC1_* become and
		/// whether the alpha channel is blended -- so it belongs in the key that already cuts the
		/// indirect run on pipeline state, and the executor's run cut needs no new stream.
		static constexpr u32 kDualSrcRoadShift = 20;
		static constexpr u32 kDualSrcRoadMask = 3u << kDualSrcRoadShift;
		static constexpr u32 kDualSrcCarrier = 1u << kDualSrcRoadShift; ///< SRC1_* become SRC_ALPHA
		/// ...and the carrier with the alpha blend equation put to work giving the stored alpha back.
		static constexpr u32 kDualSrcCarrierRestore = 2u << kDualSrcRoadShift;
		/// One per draw, parallel to `draws`: the draw's SAMPLED BINDING KEY — its slot in its pass's
		/// sampled-target array in the low 16 bits and its slot in the frame's materialised-source
		/// array in the high 16 (kNoTexSlot / kNoSourceSlot truncate to 0xFFFF in their half, so a
		/// draw taking neither is 0xFFFFFFFF). Both halves are also in the draw's state row; they are
		/// repeated here because the state table's layout is the backend's business and this is not:
		/// the fragment shader indexes both arrays by them, and a descriptor array index must be
		/// dynamically uniform, so the executor may not put two different keys inside one indirect
		/// call. The key therefore ENDS an indirect run the way the topology and the blend key do --
		/// but unlike them it is not pipeline state, so the split reuses the bound pipeline and costs
		/// one more vkCmdDrawIndexedIndirect, nothing else. It is ONE key rather than two because the
		/// split is on the pair: two draws agreeing on the target and differing on the source still
		/// have to be separate calls. Empty = no draw samples an image and nothing splits.
		std::span<const u32> bind_keys;
		static constexpr u32 PackBindKey(u32 tex_slot, u32 source_slot)
		{
			return (tex_slot & 0xFFFFu) | ((source_slot & 0xFFFFu) << 16);
		}

		/// One per draw, parallel to `draws`: the draw's FRAGMENT VARIANT — the texel road it takes,
		/// the byte road's decode arm it decodes through, what it needs the in-pass destination read
		/// for, whether its own output is what a 16-bit frame stores, and the per-draw GS state the
		/// program freezes as a compile-time constant instead of reading out of the state row
		/// (GSTileGpuFragmentSpec). Packed by PackVariantKey.
		///
		/// It is part of the RUN key, and that is the whole point of it existing. A pass is one
		/// attachment configuration, so a pass had to carry the UNION of its draws' variants and every
		/// draw of it executed the union program. The fragment program is not attachment state — it is
		/// pipeline state, like the topology, the blend and the depth mode — so it belongs where those
		/// are, and keying it here costs a pipeline bind rather than a pass break.
		///
		/// What the union cost, from the corpus census: 99.6% of Bloodrayne 2's source-road draws, 99.4%
		/// of Ratchet & Clank's gameplay draws and 91.9% of Gran Turismo 4's sat in a pass that also
		/// compiled the byte road, and therefore ran a 1039-1748 instruction program where their own
		/// road is 390. And merging moved untextured draws off the 4-register program that runs wave128
		/// on an Adreno 650 onto a 12-register one that runs wave64 — 63 draws a frame on Shadow of the
		/// Colossus, 1390 on Xenosaga — halving the fragments in flight for a third of the painted area.
		/// Priced the other way, the extra indirect calls are 30 a frame worst case in the corpus,
		/// 0.007 ms at the measured 237 ns a call.
		///
		/// Empty leaves every run on its PASS's union masks, which is exactly what the executor did
		/// before this stream existed.
		std::span<const u32> variant_keys;
		/// The variant's packing: road mask at 0 (3 bits), texel-arm mask at 3 (7), self-read mask at
		/// 10 (3), the 16-bit quantise at 13, and the frozen per-draw GS state
		/// (GSDevice::GSTileGpuFragmentSpec) from 14 up — its presence flag at 14 and its ten fields
		/// in bits 15-31. One packer so the renderer, the executor and the run key cannot disagree
		/// about which bit is which.
		///
		/// ⚠️ THE WORD IS NOW FULL. The seventh texel arm spent the spare bit the layout carried at 31;
		/// everything above the arms moved up by one when it landed. An eighth arm, a fourth
		/// destination-read use or an eleventh frozen field has nowhere to go and needs a wider key or
		/// a narrower field, not a shuffle. Nothing persists this key -- the Vulkan module and pipeline
		/// caches are keyed by the shader SOURCE TEXT and by the driver's own pipeline blob, so a
		/// repack costs nothing on disk and nothing across sessions.
		///
		/// The alpha test's field is `atst - 2`, not `atst`, so "no test" plus the six comparisons
		/// that can reach the fragment stage fit three bits instead of four. NEVER and ALWAYS are
		/// folded into the write flags by the renderer and never arrive here.
		static constexpr u32 kVariantSpecValid = 1u << 14;
		static constexpr u32 kVariantSpecMask = 0xFFFFC000u; ///< bits 14-31: the whole frozen-state half
		static constexpr u32 PackVariantKey(u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise)
		{
			return (road_mask & kGSTileGpuRoadMaskAll) | ((texel_mask & kGSTileGpuTexelMaskAll) << 3) |
			       ((self_mask & kGSTileGpuSelfMaskAll) << 10) | (quantise ? (1u << 13) : 0u);
		}
		static constexpr u32 PackVariantKey(
			u32 road_mask, u32 texel_mask, u32 self_mask, bool quantise, const GSTileGpuFragmentSpec& spec)
		{
			const u32 key = PackVariantKey(road_mask, texel_mask, self_mask, quantise);
			if (!spec.valid)
				return key;
			return key | kVariantSpecValid | ((spec.fst & 1u) << 15) | ((spec.ltf & 1u) << 16) |
			       ((spec.tfx & 3u) << 17) | ((spec.tcc & 1u) << 19) |
			       (((spec.atst != 0) ? (spec.atst - 2u) : 0u) << 20) | ((spec.fge & 1u) << 23) |
			       ((spec.date & 3u) << 24) | ((spec.wms & 3u) << 26) | ((spec.wmt & 3u) << 28) |
			       ((spec.texa & 3u) << 30);
		}
		static constexpr u32 VariantRoadMask(u32 key) { return key & kGSTileGpuRoadMaskAll; }
		static constexpr u32 VariantTexelMask(u32 key) { return (key >> 3) & kGSTileGpuTexelMaskAll; }
		static constexpr u32 VariantSelfMask(u32 key) { return (key >> 10) & kGSTileGpuSelfMaskAll; }
		static constexpr bool VariantQuantises(u32 key) { return (key & (1u << 13)) != 0; }
		static constexpr GSTileGpuFragmentSpec VariantSpec(u32 key)
		{
			GSTileGpuFragmentSpec spec;
			if ((key & kVariantSpecValid) == 0)
				return spec;
			spec.valid = true;
			spec.fst = static_cast<u8>((key >> 15) & 1u);
			spec.ltf = static_cast<u8>((key >> 16) & 1u);
			spec.tfx = static_cast<u8>((key >> 17) & 3u);
			spec.tcc = static_cast<u8>((key >> 19) & 1u);
			const u32 at = (key >> 20) & 7u;
			spec.atst = static_cast<u8>((at != 0) ? (at + 2u) : 0u);
			spec.fge = static_cast<u8>((key >> 23) & 1u);
			spec.date = static_cast<u8>((key >> 24) & 3u);
			spec.wms = static_cast<u8>((key >> 26) & 3u);
			spec.wmt = static_cast<u8>((key >> 28) & 3u);
			spec.texa = static_cast<u8>((key >> 30) & 3u);
			return spec;
		}

		/// The per-draw PIPELINE state an indirect run has to be uniform in. A run is a maximal
		/// stretch of a pass's draws sharing it: the executor binds one pipeline and issues the
		/// stretch as one vkCmdDrawIndexedIndirect, so a change here cuts the run and costs a
		/// pipeline bind — never a pass break, because none of it is attachment state.
		///
		/// (The sampled-binding key cuts the CALL inside a run without changing the pipeline; see
		/// bind_keys. It is not part of this key for that reason.)
		struct GSTileGpuRunKey
		{
			GSTileGpuTopology topology = GSTileGpuTopology::Triangle;
			u32 blend_key = 0;
			GSTileGpuDepthMode depth_mode = GSTileGpuDepthMode::None;
			/// The draw's packed fragment variant (PackVariantKey), or 0 for a plan carrying no
			/// variant stream — where every run falls back to its pass's union masks and reads all
			/// of its per-draw GS state out of the state row.
			u32 variant = 0;

			constexpr bool operator==(const GSTileGpuRunKey& o) const
			{
				return topology == o.topology && blend_key == o.blend_key && depth_mode == o.depth_mode &&
					   variant == o.variant;
			}
			constexpr bool operator!=(const GSTileGpuRunKey& o) const { return !(*this == o); }
		};

		/// Draw `d`'s run key. One reader for the whole rule, so the executor's run loop and anything
		/// that wants to predict its cuts cannot disagree about what a run is.
		GSTileGpuRunKey RunKeyAt(u32 d) const
		{
			return GSTileGpuRunKey{topologies[d], (blend_keys.size() == draws.size()) ? blend_keys[d] : 0u,
				depth_modes[d], (variant_keys.size() == draws.size()) ? variant_keys[d] : 0u};
		}

		/// One past the last draw of the maximal run starting at `first`, bounded by `end`.
		///
		/// ⚠️ MAXIMAL and CONSECUTIVE, in submission order. Runs PARTITION a pass's draws — walking
		/// this from the pass's first draw visits every draw exactly once, in the order the GS
		/// issued them. Nothing here may ever sort or bucket by run key, however tempting it looks
		/// when a pass interleaves two depth modes or two topologies: a later draw has to see an
		/// earlier draw's output (that is the whole of GS draw order), and on hardware whose
		/// depth rejection depends on the order fragments arrive, resequencing is also a large and
		/// silent fragment-cost regression. Collapsing the call count is the submission's job;
		/// reordering is not on the table.
		u32 RunEndAt(u32 first, u32 end) const
		{
			const GSTileGpuRunKey key = RunKeyAt(first);
			u32 run_end = first + 1;
			while (run_end < end && RunKeyAt(run_end) == key)
				run_end++;
			return run_end;
		}

		std::span<const GSTileGpuTargetPair> target_pairs;
		std::span<const GSTileGpuSnapshotCopy> snapshots;
		std::span<const GSTileGpuPrepOp> prep_ops;
		std::span<const GSTileGpuPageEntry> page_entries;
		/// The keep-mask tables a page entry's `keep_mask_words` indexes: one
		/// kGSTileGpuKeepMaskWordsPerPage run per page entry that has one, appended in the order the
		/// entries were built. Empty on a frame with no upload merge, which is most of them.
		std::span<const u32> writeback_keep_masks;

		const void* state_table = nullptr; ///< state_count rows of state_stride bytes
		u32 state_stride = 0;
		u32 state_count = 0;

		std::span<const u8> vertices; ///< raw vertex bytes; vertex_stride is the row size
		u32 vertex_stride = 0;
		std::span<const u16> indices;

		std::span<GSTexture* const> targets; ///< the *_target fields index this list

		/// The images the frame's Materialise and Expand prep ops NAME — rule 3's materialised
		/// texture sources, the index images behind the paletted ones, and the palettes those
		/// expansions read. A separate list from `targets` because none of them is one: no pass
		/// renders into them, no target pair names them, and the page model does not own them. The
		/// `target`, `index_texture` and `palette_texture` fields of a prep op index here.
		std::span<GSTexture* const> prep_textures;

		/// Rule 3's frame-wide bind table: the materialised sources this frame's draws sample, in the
		/// slot order a state row's tex_source names. A slot is one (image, sampler) PAIR — the
		/// filtering and the wrap ride in the descriptor, so the shader needs one index and one fetch
		/// site, and two draws sampling the same image through different TEX1/CLAMP take two slots.
		/// Frame-scoped rather than per-pass: a source belongs to a window, so the executor writes the
		/// whole array once per plan, before any pass opens.
		///
		/// Every image here is a Format::Color render target the size of its window (tw x th), built
		/// by a Materialise prep op this frame or by one in an earlier frame that the source cache
		/// still holds. It is never one of the frame's targets, and never a pass attachment.
		struct SourceBind
		{
			GSTexture* texture;
			/// The sampler this slot pairs the image with, as a GSHWDrawConfig::SamplerSelector key:
			/// bit 0 = REPEAT on U (clear = CLAMP), bit 1 = REPEAT on V, bit 2 = bilinear. Rule 3
			/// admits only those two wrap modes, and mipmapping is off, so eight keys cover it and the
			/// backend's sampler cache pins max LOD at level 0.
			u8 sampler;
		};
		std::span<const SourceBind> sources;

		/// The rule-2 sampled targets, as indices into `targets`, grouped by pass
		/// (GSTileGpuPass::first_tex_source / tex_source_count). A textured draw either names one
		/// of its pass's entries in its state row -- the fragment stage then fetches the texel out
		/// of that image instead of decoding ring bytes -- or carries kNoTexSlot and takes the byte
		/// road. The renderer only puts a target here when the page model proves it is the sole
		/// owner of the whole read window at a 1:1 matching layout, so the two roads read the same
		/// texel; the image road just reads the live one.
		std::span<const u32> tex_sources;

		/// The frame's byte road: exactly the guest pages the plan reads through the flat-road
		/// shader or reconciles between the byte store and a target, each with the epoch range it
		/// serves (see GSTileGpuRingPage). The executor stages one 8 KB ring slot per entry and
		/// builds epoch_count page tables of GS_MAX_PAGES words naming them; a page no entry covers
		/// in some epoch reads as zeros there. Empty leaves every draw on the vertex-colour path.
		std::span<const GSTileGpuRingPage> ring_pages;
		u32 epoch_count = 0;

		/// The frame's expanded CLUTs, concatenated as 32-bit RGBA words: for each paletted draw
		/// the N-entry palette (N = 16 for PSMT4, 256 for PSMT8) that GSClut::Read32 produced,
		/// CSA/CPSM/TEXA already applied. The executor stages this into the same storage buffer as
		/// the ring, behind its own base; a state row's pal_offset indexes an entry within it. Empty
		/// leaves every draw on the direct/vertex-colour path.
		std::span<const u32> palettes;
	};

	/// Whether this device serves the TileGpu executor road. False on every backend but
	/// Vulkan, and on a Vulkan device that failed the descriptor-indexing + indirect-draw
	/// capability probe at device creation. The renderer consults this before constructing.
	virtual bool TileGpuExecutorAvailable() { return false; }

	/// Whether the executor can bind resident targets as sampled images for the VRAM model's
	/// rule 2. Asked ONCE, before any draw is accumulated, because the renderer answers it by
	/// *not doing work*: a rule-2 draw's read window is never composed into the byte ring, so a
	/// device that then failed to bind the target would sample stale bytes. False keeps every
	/// draw on the byte road.
	virtual bool TileGpuBindlessTargets() { return false; }

	/// Whether the executor can serve a pass that reads its own colour attachment in rasterization
	/// order -- the fragment-side read-modify-write road. Asked ONCE, before any target is
	/// allocated, because the read is an input attachment and that usage has to be on the image
	/// from creation. False keeps every draw on the fixed-function blend, keeps the destination-alpha
	/// test on its pre-pass snapshot copy, and declares no pass.
	virtual bool TileGpuSelfRead() { return false; }

	/// Whether this device's TileGpu fragment module declares a second colour output at index 1 and
	/// its pipelines may name SRC1_* blend factors. Asked ONCE, before the first draw is planned,
	/// because it decides which module was compiled -- and because the planner has to route every
	/// As-factor draw down one of the feature-free roads when it is false, which changes the draw's
	/// state row, its blend key and sometimes the number of draws in the plan.
	virtual bool TileGpuDualSourceBlend() { return true; }

	/// Whether this device would rather render MORE passes than see the depth write-enable or
	/// depth compare-enable change inside one.
	///
	/// The depth mode is pipeline state, so the planner is free to merge draws that differ only in
	/// it into a single pass and let the executor rebind per indirect run. Whether that is a win is
	/// a hardware property and nothing the planner can work out:
	///
	///  - On a tiler a pass boundary is a tile-buffer resolve and reload. Merging is worth 3-7x on
	///    the scenes where it bites (measured on an M2 under Honeykrisp: Shadow of the Colossus
	///    574.95 passes a frame down to 76.95, 35.0 ms down to 10.4).
	///  - On Adreno the same merge is a net LOSS on every dump measured, even though the pass-count
	///    deltas come out exactly as the model predicts. Something there charges more for mixed
	///    depth state inside a pass than the boundaries cost. This is an empirical result and no
	///    mechanism is claimed for it — see GSDeviceVK's answer for the measurement.
	///
	/// So: true keeps every pass depth-uniform, false merges. False is the default, including for
	/// vendors nobody has measured — merging is the behaviour that is right on the two architectures
	/// the design targets, and a device that needs uniformity says so.
	virtual bool TileGpuPrefersDepthUniformPasses() { return false; }

	/// The most draws this device wants inside ONE render pass, or 0 for no limit.
	///
	/// Unlike the depth question above this is not a choice between two arrangements — it is a
	/// ceiling. A pass that reaches it is closed and another with the SAME key opens, so the same
	/// draws run in the same order over more passes and not a pixel moves. The planner pays a pass
	/// boundary per split and buys back whatever the architecture charges for a long pass.
	///
	/// It exists because Adreno charges for one, measured twice with different instruments:
	///
	///  - A device pass-size sweep on an SD865 (Turnip, 2026-08-24) found the driver's per-pass
	///    occlusion-autotune term is concentrated entirely in a title's few giant passes, and a cap
	///    at 64 deletes all of it — Armored Core 3 27.26 ms to 18.48, MGS3 29.35 to 26.58.
	///  - A CPU-side census of the depth-policy A/B, on a different machine with no device access,
	///    found the fraction of a frame's fill relocated into passes of 64 or more draws is what
	///    ranks that A/B's per-title GPU cost, with the correlation peaking at the same 64.
	///
	/// The mechanism behind the second is NOT established, and this comment claims none. What is
	/// established is that a conservative ceiling deletes the term on the dumps measured while
	/// aggressive caps (below 8 draws) HARM two of three — so the answer is a ceiling well above
	/// the knee, never "as small as possible".
	///
	/// Zero is the default, including for vendors nobody has measured: a cap is a cost (one pass
	/// boundary per split) that only pays where an architecture charges for pass length, and no
	/// tiler measurement asks for one. EmuCore/GS/TileGpuMaxPassDraws overrides this per run.
	virtual u32 TileGpuMaxPassDraws() { return 0; }

	/// The most extra pipeline binds one plan may pay for fragment SPECIALIZATION, or 0 for no limit.
	///
	/// Freezing a draw's own GS state into its fragment program (GSTileGpuFragmentSpec) buys a much
	/// smaller program — on an Adreno 650, 247 of the corpus's 251 distinct variants at eight full
	/// registers or fewer, so wave128, against two of seventeen unspecialized. It costs pipeline
	/// binds, because the frozen state is part of the indirect-run key: two consecutive draws that
	/// differ only in their alpha comparison used to be one run and are now two.
	///
	/// The SD865 round of 2026-08-24 says that trade is not uniform. Shadow of the Colossus (-20.5%
	/// frame) and Gran Turismo 4 (-7.6%) win with GPU time falling; both Ratchet & Clank scenes lose
	/// (+9.2%, +8.4%) with GPU time RISING, +59% on the gameplay scene. What separates them is not
	/// the bind total — Shadow of the Colossus binds 763 a frame and wins where the gameplay scene
	/// binds 905 and loses — but how many of those binds the freezing ADDED over what the per-draw
	/// road narrowing already required: 141 and 150 on the winners against 525 and 859 on the losers,
	/// a 3.5x gap with the rest of the eighteen-dump corpus at 72 or below.
	///
	/// ⚠️ It is NOT an instruction-cache capacity effect, which was the first hypothesis and is
	/// refuted by its own arithmetic: compiled offline against the device's own Turnip, the Ratchet
	/// gameplay scene's busiest pass shape alternates among five programs totalling 32 instrlen units
	/// against an a650 instruction cache of 127, while Shadow of the Colossus — which WINS — runs
	/// 110-182-unit working sets and streams 2.6x more program bytes a frame. The cost tracks the
	/// number of switches, not the size of what is switched to, so the budget is counted in binds.
	///
	/// Zero everywhere nobody has measured, including every tiler: withholding the frozen state where
	/// a bind is cheap is pure loss — it gives up the wave size and saves nothing.
	/// EmuCore/GS/TileGpuMaxSpecializationBinds overrides this per run.
	virtual u32 TileGpuMaxSpecializationBinds() { return 0; }

	/// Whether declaring the in-pass destination read is charged to EVERY draw of the pass, reader
	/// or not — so that a pass which declares must contain only the draws that need it.
	///
	/// ⚠️ One bit, two consequences, and the name says only the first. It is deliberately one bit
	/// and not two: both answers come from the same silicon fact, so a second virtual would be a
	/// second chance to answer it inconsistently, and there is no device that could sensibly say
	/// yes to one and no to the other.
	///
	///  1. SEGREGATION, the pass key. Declaring is a property of the pass, so the cheap arrangement
	///     is to let a reader join whatever pass is open and have the pass declare on its behalf —
	///     the non-readers simply do not read. That is free on hardware where an in-pass read is
	///     free, and it is the wrong shape where declaring changes how the whole pass rasterizes:
	///     there the readers have to be alone, however many passes that costs.
	///  2. ADMISSION, which draws read at all. Segregation confines the toll to the readers and can
	///     do nothing for a title whose passes are reader-SATURATED, because there the readers
	///     already are the passes. So under this bit the planner declines the one admission class
	///     that saturates whole titles rather than costing them tens of draws a frame — the blend
	///     whose only inexpressible part is that a 16-bit frame quantises its result. Everything
	///     else stays admitted everywhere. See gsTileGpuAdmitsQuantisedBlend for the measurement
	///     and for what the refused draws keep.
	///
	/// False is the default, including for vendors nobody has measured: it is the arrangement with
	/// no pass-count cost and the more accurate one, and a device that charges for declaring says so.
	virtual bool TileGpuSegregatesSelfRead() { return false; }

	/// TileGpu: whether this session's fragment modules were compiled WITH the merged palette arm
	/// (tilegpu.glsl's pal_mode 3, behind TILEGPU_CLUT_MERGE).
	///
	/// It is a module define rather than a per-draw branch alone because dead code is not free on
	/// Adreno: the arm is a whole unit of instruction length in the widest paletted variants, and a
	/// lever that ships OFF must not enlarge the program every device runs. So the renderer cannot
	/// read TileGpuClutMergeRegions and assume the shader agrees -- the setting can move after the
	/// modules were assembled, and a mode-3 state row in front of a module with no mode-3 arm reads
	/// the palette in the wrong order. It asks this instead.
	virtual bool TileGpuClutMergeCompiled() { return false; }

	/// ...and whether they also carry the per-draw STRIDE that arm needs to read a palette out of a
	/// whole copied owner page (TileGpuClutMergePages). Separate from the above because the two
	/// levers compile separately: without the page merge the merged arm's stride is a constant 16,
	/// which is 72 SPIR-V words cheaper, and a module built that way would read a page-merged
	/// palette at 16 instead of 64 -- a plausible-looking palette made of the wrong words.
	virtual bool TileGpuClutMergePagesCompiled() { return false; }

	/// Submit one frame's pass plan through the executor. Returns false when the device does
	/// not serve it, so the renderer can refuse to construct rather than drop frames silently.
	virtual bool ExecuteTileGpuPassPlan(const GSTileGpuPassPlan& plan) { return false; }

	/// The video frame has ended: every plan it built has been executed and the next frame's first
	/// draw has not been accumulated.
	///
	/// A frame is NOT a plan. A mid-frame flush ends a plan and starts another, so a title that
	/// flushes a lot builds hundreds of plans in one video frame -- and anything the device decides
	/// once per frame has to be told where the frame ends, because the executor cannot tell the
	/// last plan of a frame from the first plan of the next one. The kick predictor
	/// (GSTileGpuKickPolicyPicker) is the one thing that needs it: its credit is a peak decayed
	/// per observation and its confirmation counts consecutive observations, so observing per plan
	/// runs both clocks at the plan rate rather than the frame rate.
	virtual void TileGpuFrameBoundary() {}

	/// Enables/disables GPU frame timing.
	virtual bool SetGPUTimingEnabled(bool enabled) = 0;

	/// Returns the amount of GPU time utilized since the last time this method was called.
	virtual float GetAndResetAccumulatedGPUTime() = 0;

	/// Enables/disables GPU pipeline statistics.
	virtual bool SetGPUPipelineStatisticsEnabled(bool enabled) = 0;

	/// Get the pipeline statistics for the last frame.
	virtual GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() = 0;

	/// Enables backend-specific diagnostic counters (e.g. Vulkan acquire/present timing).
	/// Off by default to surface WSI-layer timing in diagnostic tools without paying
	/// the cost on the normal present hot path.
	virtual void EnableExtendedStats(bool enabled) {}

	/// Returns backend-specific diagnostic lines (swapchain config, present/acquire timing, etc).
	/// Each line is a fully-formatted string, ready to print as-is. Default: empty.
	virtual std::vector<std::string> GetExtendedStats() const { return {}; }

	/// Returns true if not enough time has passed for present to not block.
	/// ⚠️ Not a pure query: answering "present" books this frame as the one that was displayed,
	/// so the next call within the same throttle period answers "skip". Ask exactly once per
	/// vsync, at the point the decision is acted on. A second caller — even a diagnostic that
	/// only reads the result — spends the credit and freezes the picture for as long as the
	/// throttle is armed.
	bool ShouldSkipPresentingFrame();

	/// Sleeps to the time the next frame can be displayed.
	void ThrottlePresentation();

	void ClearRenderTarget(GSTexture* t, u32 c);
	void ClearDepth(GSTexture* t, float d);
	bool ProcessClearsBeforeCopy(GSTexture* sTex, GSTexture* dTex, const bool full_copy);
	void InvalidateRenderTarget(GSTexture* t);

	virtual void PushDebugGroup(const char* fmt, ...) = 0;
	virtual void PopDebugGroup() = 0;
	virtual void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) = 0;

	/// Per-draw graphics-debugger label, compiled into every build and gated at runtime
	/// on GSConfig.DebugLabels.
	///
	/// Distinct from PushDebugGroup, which is compiled out unless ENABLE_OGL_DEBUG and
	/// additionally requires UseDebugDevice -- and therefore the validation layer, which
	/// makes a capture useless for timing. The GL_* macro family also evaluates its
	/// format arguments at every call site before the emitter can bail, so un-gating it
	/// wholesale would be far too expensive for a release build. This takes a
	/// pre-formatted string and is only reached when labelling is actually on.
	virtual void PushDrawLabel(const std::string_view label) {}
	virtual void PopDrawLabel() {}

	GSTexture::Usage GetDepthStencilUsage() const;

	GSTexture* FetchSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format, bool clear, bool prefer_reuse);
	GSTexture* FetchSurface(GSTexture::Usage usage, const GSVector2i& size, int levels, GSTexture::Format format, bool clear, bool prefer_reuse);
	GSTexture* CreateRenderTarget(int w, int h, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateRenderTarget(const GSVector2i& size, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateFeedbackTarget(int w, int h, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateFeedbackTarget(const GSVector2i& size, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateShaderWriteTarget(int w, int h, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateShaderWriteTarget(const GSVector2i& size, GSTexture::Format format, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateDepthStencil(int w, int h, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateDepthStencil(const GSVector2i& size, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateTexture(int w, int h, int mipmap_levels, GSTexture::Format format, bool prefer_reuse = false);
	GSTexture* CreateTexture(const GSVector2i& size, int mipmap_levels, GSTexture::Format format, bool prefer_reuse = false);
	GSTexture* CreateCompatible(GSTexture* tex, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateCompatible(GSTexture* tex, const GSVector2i& size, bool clear = true, bool prefer_reuse = true);
	GSTexture* CreateCompatible(GSTexture* tex, int w, int h, bool clear = true, bool prefer_reuse = true);

	virtual std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) = 0;

	/// Hints that a synchronous CPU readback of `tex` is being performed. Games that read
	/// back every frame (e.g. small occlusion-test targets) will typically draw into the
	/// same texture again shortly before the next readback; backends can use this to
	/// schedule command submission so that readback has minimal GPU backlog to wait on.
	void HintReadbackSource(GSTexture* tex)
	{
		FlushDeferredDraws();
		DoHintReadbackSource(tex);
	}

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
	{
		FlushDeferredDraws();
		DoCopyRect(sTex, dTex, r, destX, destY);
	}

	// StretchRect - all options
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvertSelector shader, Filter filter);
	void StretchRect(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvertSelector shader, Filter filter);
	void StretchRect(GSTexture* sTex, GSTexture* dTex, ShaderConvertSelector shader, Filter filter);
	
	// StretchRect - infer shader based on formats
	void StretchRectAuto(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, Filter filter,
		u32 src_bpp = 32, u32 dst_bpp = 32);
	void StretchRectAuto(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, Filter filter,
		u32 src_bpp = 32, u32 dst_bpp = 32);
	void StretchRectAuto(GSTexture* sTex, GSTexture* dTex, Filter filter, u32 src_bpp = 32, u32 dst_bpp = 32);

	// StretchRect - nearest filter, infer shader based on formats, specify channel mask
	void StretchRectAutoMask(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, u32 src_bpp = 32, u32 dst_bpp = 32);
	void StretchRectAutoMask(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha, u32 src_bpp = 32, u32 dst_bpp = 32);
	void StretchRectAutoMask(GSTexture* sTex, GSTexture* dTex, bool red, bool green, bool blue, bool alpha, u32 src_bpp = 32, u32 dst_bpp = 32);

	/// Performs a screen blit for display. If dTex is null, it assumes you are writing to the system framebuffer/swap chain.
	virtual void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, PresentShader shader, float shaderTime, Filter filter) = 0;

	/// Same as doing StretchRect for each item, except tries to batch together rectangles in as few draws as possible.
	/// The provided list should be sorted by texture, the implementations only check if it's the same as the last.
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader = ShaderConvert::COPY)
	{
		FlushDeferredDraws();
		DoDrawMultiStretchRects(rects, num_rects, dTex, shader);
	}

	/// Sorts a MultiStretchRect list for optimal batching.
	static void SortMultiStretchRects(MultiStretchRect* rects, u32 num_rects);

	/// Updates a GPU CLUT texture from a source texture.
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
	{
		FlushDeferredDraws();
		DoUpdateCLUTTexture(sTex, sScale, offsetX, offsetY, dTex, dOffset, dSize);
	}

	/// Converts a colour format to an indexed format texture.
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
	{
		FlushDeferredDraws();
		DoConvertToIndexedTexture(sTex, sScale, offsetX, offsetY, SBW, SPSM, dTex, DBW, DPSM);
	}

	/// Uses box downsampling to resize a texture.
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
	{
		FlushDeferredDraws();
		DoFilteredDownsampleTexture(sTex, dTex, downsample_factor, clamp_min, dRect);
	}

	/// Submits a hardware draw. With the render-pass scheduler active this may hold the
	/// draw back and emit it later, coalesced with other draws to the same target — see
	/// GSPassScheduler. Any device entry point that could observe the result flushes
	/// first, so the deferral is invisible.
	void RenderHW(GSHWDrawConfig& config);

	/// Emits any draws the render-pass scheduler is holding back, in an order that
	/// preserves what every deferred draw would have seen had it run immediately.
	///
	/// This is the load-bearing half of the coalescing design. Deferral is safe only
	/// because *observing* a render target — copying it, sampling it, downsampling it,
	/// presenting it, reading it back — has to go through a GSDevice entry point, and
	/// every one of those entry points calls this first. The virtuals behind them are
	/// protected and named Do*, so a caller cannot reach the backend without passing
	/// through the flush.
	///
	/// Re-entrant by design: emitting a deferred draw calls DoRenderHW, and several
	/// backends call CopyRect on themselves from inside it to clone an RT for a feedback
	/// loop. Re-entry is a no-op rather than recursion.
	///
	/// The queue depth is mirrored here rather than read from the scheduler so that the
	/// overwhelmingly common "nothing queued" case stays one load and one branch, and so
	/// that GSDevice.h - which most of the GS tree includes - does not have to pull in
	/// GSPassScheduler.h.
	__fi void FlushDeferredDraws()
	{
		if (m_deferred_draw_count != 0 && !m_flushing)
			FlushDeferredDrawsImpl();
	}

	/// Narrower form for the entry points that only touch one texture: flushes just when a
	/// queued draw can actually observe it. Texture pooling and deferred-clear bookkeeping
	/// run several times a frame on textures the queue has never heard of, and flushing for
	/// those throws away most of the coalescing.
	__fi void FlushDeferredDrawsFor(const GSTexture* tex)
	{
		if (m_deferred_draw_count != 0 && !m_flushing && DeferredDrawsReference(tex))
			FlushDeferredDrawsImpl();
	}

#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)
	/// True when a queued draw can still observe [tex] — i.e. when reaching a backend path
	/// that touches it, without having gone through FlushDeferredDraws() first, would be a
	/// bug. Backends assert on this at their lowest-level "about to touch this texture"
	/// chokepoint; see GSTextureVK::TransitionToLayout for the reasoning.
	__fi bool DeferredDrawsWouldObserve(const GSTexture* tex) const
	{
		return (m_deferred_draw_count != 0 && !m_flushing && DeferredDrawsReference(tex));
	}
#endif

	virtual void ClearSamplerCache() = 0;

	void ClearCurrent();
	void Merge(GSTexture* sTex[3], GSVector4* sRect, GSVector4* dRect, const GSVector2i& fs, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c);
	void Interlace(const GSVector2i& ds, int field, int mode, float yoffset);
	void FXAA();
	void ShadeBoost();
	/// Runs the configured RetroArch (.slangp) shader chain over m_current, after
	/// ShadeBoost/FXAA, rendering at `output_size` — the frame's ON-SCREEN size, so shaders
	/// that generate detail per output pixel (CRT scanlines above all) run at display pixel
	/// density instead of being generated at internal res and smeared by the presenter's
	/// upscale. Pass the aspect-corrected draw rect, NOT the raw window: librashader maps the
	/// whole input to the whole viewport, so a mismatched aspect stretches the picture.
	/// Returns true if the chain ran and m_current now points at the shaded target. The chain
	/// itself lives in DoApplyShaderChain, which only librashader-capable backends override.
	bool ApplyShaderChain(const GSVector2i& output_size);
	void Resize(int width, int height);

	void CAS(GSTexture*& tex, GSVector4i& src_rect, GSVector4& src_uv, const GSVector4& draw_rect, bool sharpen_only);

	/// Spatially upscales the merged display texture (MetalFX) to the draw-rect size, rewriting
	/// tex/src_rect/src_uv to point at the upscaled result, mirroring CAS().
	void MetalFXUpscale(GSTexture*& tex, GSVector4i& src_rect, GSVector4& src_uv, const GSVector4& draw_rect);

	/// Same contract as MetalFXUpscale(), via FSR1's two compute passes.
	void FSR1Upscale(GSTexture*& tex, GSVector4i& src_rect, GSVector4& src_uv, const GSVector4& draw_rect);

	/// Same contract again, via SGSR's single compute pass. Cheaper than FSR1 on mobile, which is
	/// the point of having it: SGSR was designed for Adreno, FSR1's two passes were not.
	void SGSRUpscale(GSTexture*& tex, GSVector4i& src_rect, GSVector4& src_uv, const GSVector4& draw_rect,
		bool edge_direction);

	bool ResizeRenderTarget(GSTexture** t, int w, int h, bool preserve_contents, bool recycle);

	void AgePool();
	void AgePoolAfterPresentCapSkip();
	void PurgePool();

	__fi static constexpr bool IsDualSourceBlendFactor(u8 factor)
	{
		return (factor == SRC1_ALPHA || factor == INV_SRC1_ALPHA || factor == SRC1_COLOR || factor == INV_SRC1_COLOR);
	}
	__fi static constexpr bool IsConstantBlendFactor(u16 factor)
	{
		return (factor == CONST_COLOR || factor == INV_CONST_COLOR);
	}

	// Convert the GS blend equations to HW blend factors/ops
	// Index is computed as ((((A * 3 + B) * 3) + C) * 3) + D. A, B, C, D taken from ALPHA register.
	__ri static HWBlend GetBlend(u32 index) { return m_blendMap[index]; }
	__ri static u16 GetBlendFlags(u32 index) { return m_blendMap[index].flags; }
};

template <>
struct std::hash<GSHWDrawConfig::PSSelector> : public GSHWDrawConfig::PSSelectorHash {};

extern std::unique_ptr<GSDevice> g_gs_device;

// Draw-config stringifiers, shared between the per-draw config dump and the draw log.
const char* GSGetTopologyName(GSHWDrawConfig::Topology topology);
const char* GSGetVSExpandName(GSHWDrawConfig::VSExpand vsexpand);
const char* GSGetTexHazardName(u32 tex_hazard);
const char* GSGetDestinationAlphaModeName(GSHWDrawConfig::DestinationAlphaMode datm);
