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
	static constexpr u32 EXPAND_BUFFER_SIZE = sizeof(u16) * 16383 * 6;

	WindowInfo m_window_info;
	GSVSyncMode m_vsync_mode = GSVSyncMode::Disabled;
	bool m_allow_present_throttle = false;
	u64 m_last_frame_displayed_time = 0;

	GSTexture* m_merge = nullptr;
	GSTexture* m_weavebob = nullptr;
	GSTexture* m_blend = nullptr;
	GSTexture* m_mad = nullptr;
	GSTexture* m_target_tmp = nullptr;
	GSTexture* m_current = nullptr;
	GSTexture* m_cas = nullptr;
	GSTexture* m_mfx_output = nullptr; ///< MetalFX spatial upscale destination (Metal backend).
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
		return DoBeginPresent(frame_skip);
	}

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

	/// A snapshot copy: clone src_rect of the pass's colour target into a scratch surface the
	/// pass's draws sample, so a draw reads a pre-pass version of pixels the pass also writes
	/// without a raster-order hazard. Taken before the pass it feeds opens; one per pass. Today
	/// it serves the destination-alpha test (DATE: a draw's state row asks the shader to discard
	/// on the snapshot's alpha bit 7 against DATM) -- the planner opens a new pass whenever a
	/// DATE draw's rect intersects what the pass already wrote, so the snapshot is exact for it.
	/// The in-pass declared read (ROAA) replaces this road where the device has it.
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

	/// One page of a prep op's page list: which page, and which of its 32 physical blocks the op
	/// touches (a writeback writes only the blocks the surface holds newest; a seed reads whole pages
	/// and carries kFullBlockMask).
	struct GSTileGpuPageEntry
	{
		u16 page;
		u16 pad_;
		u32 block_mask;
	};

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
		Materialise = 2,
	};

	/// One prep dispatch. `target` indexes the plan's target list — or, for a Materialise, its
	/// `prep_textures` list. bp/bw/psm is the layout that pixel space realises (guest layout =
	/// pixel space, page-aligned base); for a Materialise it is the texture window's TBP0, its
	/// pages per texture row and its TEX0.PSM. The page list is `page_entry_count` rows of the
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
		/// cache's key already accounts for (two TEXA settings are two entries).
		u32 texa;
	};

	/// The pass's uniform depth configuration, which selects the depth pipeline variant. Every
	/// draw in a pass shares it — the planner breaks a pass whenever the GS z-write/z-test
	/// changes, so a ZTST=ALWAYS write and a ZTST=GEQUAL test are never forced onto one pipeline.
	/// GS depth grows towards the viewer, so the test is GREATER_OR_EQUAL when the draw tests and
	/// ALWAYS when it only writes; the write follows ZMSK independently of the test.
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
		/// are all untextured. It selects the fragment shader variant: the module compiles in these
		/// roads and no others, so a pass never pays program size for a road it does not take. Like
		/// the depth mode it is uniform across the pass by construction (it is a union, not a
		/// per-draw choice), so it splits nothing.
		u32 road_mask;
		bool declares_self_read; ///< ROAA: the pass reads its own colour target in raster order
		GSTileGpuDepthMode depth_mode; ///< uniform across the pass; None iff zbuf_target is kNoTarget
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

		std::span<const GSTileGpuPass> passes;
		std::span<const GSTileGpuIndirectDraw> draws;
		std::span<const GSTileGpuTopology> topologies; ///< one per draw, parallel to `draws`
		/// One per draw, parallel to `draws`: the fixed-function blend the draw takes, or 0 for
		/// none. Bit 31 set = blend enabled; bits 0-6 = the GS ALPHA (A,B,C,D) index into
		/// GSDevice::m_blendMap (A*27 + B*9 + C*3 + D); bits 8-15 = ALPHA.FIX when C selects the
		/// fixed factor (it rides as the blend constant, dynamic state, not pipeline state). Like
		/// the topology it is pipeline state, so the constant-cost submission splits its indirect
		/// runs on it -- a split, never a pass break.
		std::span<const u32> blend_keys;
		static constexpr u32 kBlendEnable = 0x80000000u;
		/// Bit 30: the draw writes no colour (a depth-only draw); the pipeline masks every channel.
		static constexpr u32 kNoColorWrite = 0x40000000u;
		/// Bit 29: the draw writes RGB but not alpha (the GS AFAIL RGB_ONLY mode); the pipeline
		/// masks the alpha channel alone.
		static constexpr u32 kNoAlphaWrite = 0x20000000u;
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
		std::span<const GSTileGpuTargetPair> target_pairs;
		std::span<const GSTileGpuSnapshotCopy> snapshots;
		std::span<const GSTileGpuPrepOp> prep_ops;
		std::span<const GSTileGpuPageEntry> page_entries;

		const void* state_table = nullptr; ///< state_count rows of state_stride bytes
		u32 state_stride = 0;
		u32 state_count = 0;

		std::span<const u8> vertices; ///< raw vertex bytes; vertex_stride is the row size
		u32 vertex_stride = 0;
		std::span<const u16> indices;

		std::span<GSTexture* const> targets; ///< the *_target fields index this list

		/// The images the frame's Materialise prep ops build into — rule 3's materialised texture
		/// sources. A separate list from `targets` because a source is not one: no pass renders
		/// into it, no target pair names it, and the page model does not own it. A Materialise op's
		/// `target` field indexes here.
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

	/// Submit one frame's pass plan through the executor. Returns false when the device does
	/// not serve it, so the renderer can refuse to construct rather than drop frames silently.
	virtual bool ExecuteTileGpuPassPlan(const GSTileGpuPassPlan& plan) { return false; }

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
