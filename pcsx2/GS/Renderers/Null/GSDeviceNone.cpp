// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNone.h"
#include "GS/GS.h"
#include "GS/GSPerfMon.h"

#include "common/Console.h"

// -------------------------------------------------------------------------
// GSTextureNone
// -------------------------------------------------------------------------

GSTextureNone::GSTextureNone(Usage usage, int width, int height, int levels, Format format)
{
	m_usage = usage;
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;
	m_format = format;
}

void* GSTextureNone::GetNativeHandle() const
{
	return nullptr;
}

bool GSTextureNone::DoUpdate(const GSVector4i& r, const void* data, int pitch, int layer)
{
	return true;
}

bool GSTextureNone::DoMap(GSMap& m, const GSVector4i* r, int layer)
{
	// 8 bytes/texel covers the widest uncompressed format (RGBA16); callers get
	// scratch memory they can safely write through, contents are discarded.
	const GSVector4i rc = r ? *r : GSVector4i::loadh(m_size);
	const u32 pitch = static_cast<u32>(m_size.x) * 8;
	m_map_buffer.resize(static_cast<size_t>(pitch) * static_cast<size_t>(m_size.y));
	m.bits = m_map_buffer.data() + static_cast<size_t>(rc.top) * pitch + static_cast<size_t>(rc.left) * 8;
	m.pitch = static_cast<int>(pitch);
	return true;
}

void GSTextureNone::Unmap()
{
}

void GSTextureNone::GenerateMipmap()
{
}

#ifdef PCSX2_DEVBUILD
void GSTextureNone::SetDebugName(std::string_view name)
{
}
#endif

// -------------------------------------------------------------------------
// GSDownloadTextureNone
// -------------------------------------------------------------------------

GSDownloadTextureNone::GSDownloadTextureNone(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	m_buffer.resize(GetBufferSize(width, height, format));
}

void GSDownloadTextureNone::DoCopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, 1);
	m_needs_flush = false;
}

bool GSDownloadTextureNone::Map(const GSVector4i& read_rc)
{
	m_map_pointer = m_buffer.data();
	return true;
}

void GSDownloadTextureNone::Unmap()
{
	m_map_pointer = nullptr;
}

void GSDownloadTextureNone::Flush()
{
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureNone::SetDebugName(std::string_view name)
{
}
#endif

// -------------------------------------------------------------------------
// GSDeviceNone
// -------------------------------------------------------------------------

bool GSDeviceNone::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	m_name = "None";
	m_max_texture_size = 8192;

	// The features a real device would report. Left at FeatureSupport's defaults this device is
	// not any device, and GSRendererHW's CPU decisions follow the difference -- see
	// GSNullDeviceProfile.h. Printed in full because a count taken here is a count ABOUT some
	// device, and the reader has to be able to see which one without reading the source.
	//
	// Only for NullHW, the measurement renderer. The plain Null renderer shares this device but
	// draws nothing, and eerunner --renderer null is a shipped way to run a VM with no GPU at all;
	// neither is measuring a device, and giving them a device's features would change behaviour
	// nobody asked to change. GSCurrentRenderer is not set until OpenGSRenderer, which runs after
	// this, so the requested renderer is read from the config.
	if (GSConfig.Renderer == GSRendererType::NullHW)
	{
		m_features = GSNullDeviceProfile::Features(s_feature_profile);

		Console.WriteLn("Null (HW) device: feature profile '%s' -- %s", GSNullDeviceProfile::Name(s_feature_profile),
			GSNullDeviceProfile::Description(s_feature_profile));
		Console.WriteLn("Null (HW) features: texbarrier=%s fbfetch=%s dualSrc=%s fastShadow=%s blendConst=%s "
						"testSampleDepth=%s stencil=%s vs_expand=%s primID=%s provokingLast=%s "
						"point/lineExpand=%s/%s preferNewTex=%s aa1=%s depthFeedback=%s rov=%s "
						"cheapRtRead=%s multidrawFbCopy=%s fbfetchOrdersOverlap=%s feedbackLoopLayout=%s "
						"noZQuant=%s astc=%s dxt/bptc=%s/%s brokenPointSampler=%s brokenMadDeint=%s",
			m_features.texture_barrier ? "on" : "off", m_features.framebuffer_fetch ? "yes(in-tile)" : "NO",
			m_features.dual_source_blend ? "yes" : "NO(sw-blend-fallback)",
			m_features.fast_stencil_shadow ? "yes(blend)" : "NO(rt-read)",
			m_features.broken_blend_constant ? "BROKEN(afix-via-src1)" : "ok",
			m_features.test_and_sample_depth ? "on" : "off", m_features.stencil_buffer ? "yes" : "no",
			m_features.vs_expand ? "yes" : "no", m_features.primitive_id ? "yes" : "no",
			m_features.provoking_vertex_last ? "yes" : "no", m_features.point_expand ? "yes" : "no",
			m_features.line_expand ? "yes" : "no", m_features.prefer_new_textures ? "yes" : "no",
			m_features.aa1 ? "yes" : "no", m_features.depth_feedback ? "yes" : "no",
			m_features.rov ? "yes" : "no", m_features.cheap_rt_feedback_read ? "yes" : "no",
			m_features.multidraw_fb_copy ? "yes" : "no",
			m_features.framebuffer_fetch_orders_overlap ? "yes" : "no",
			m_features.feedback_loop_layout ? "yes" : "no", m_features.no_ps2_z_quantization ? "yes" : "no",
			m_features.astc_textures ? "yes" : "no", m_features.dxt_textures ? "yes" : "no",
			m_features.bptc_textures ? "yes" : "no", m_features.broken_point_sampler ? "yes" : "no",
			m_features.broken_mad_deinterlace ? "yes" : "no");
	}

	// Nominal surfaceless "window" so layout consumers (ImGui display size, OSD
	// scale) see sane nonzero dimensions.
	m_window_info = WindowInfo();
	m_window_info.surface_width = 640;
	m_window_info.surface_height = 480;
	m_window_info.surface_scale = 1.0f;
	return true;
}

RenderAPI GSDeviceNone::GetRenderAPI() const
{
	return RenderAPI::None;
}

bool GSDeviceNone::HasSurface() const
{
	return true;
}

void GSDeviceNone::DestroySurface()
{
}

bool GSDeviceNone::UpdateWindow()
{
	return true;
}

void GSDeviceNone::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_window_info.surface_width = new_window_width;
	m_window_info.surface_height = new_window_height;
	m_window_info.surface_scale = new_window_scale;
}

bool GSDeviceNone::SupportsExclusiveFullscreen() const
{
	return false;
}

// Always FrameSkipped: the caller skips the whole present/ImGui draw path, so
// none of the drawing stubs below are ever reached with real work.
GSDevice::PresentResult GSDeviceNone::DoBeginPresent(bool frame_skip)
{
	return PresentResult::FrameSkipped;
}

void GSDeviceNone::EndPresent()
{
}

void GSDeviceNone::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNone::GetDriverInfo() const
{
	return "Null host device (no graphics API)";
}

bool GSDeviceNone::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceNone::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

void GSDeviceNone::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceNone::PopDebugGroup()
{
}

void GSDeviceNone::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

std::unique_ptr<GSDownloadTexture> GSDeviceNone::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return std::make_unique<GSDownloadTextureNone>(width, height, format);
}

void GSDeviceNone::DoCopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
}

void GSDeviceNone::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
}

void GSDeviceNone::DoUpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
}

void GSDeviceNone::DoConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
	GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceNone::DoFilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceNone::DoRenderHW(GSHWDrawConfig& config)
{
	// The only backend-independent fact about a submission: GSRendererHW called RenderHW()
	// once for this internal draw (exactly one of DrawPrims/EndHLEHardwareDraw/the channel-
	// shuffle completion calls it per draw, never more than one). Real backends additionally
	// split this into several submissions for reasons that only exist with a GPU behind them
	// -- a DATE primitive-ID prepass, a per-primitive texture-barrier loop, a colclip
	// encode/resolve pass -- none of which this stub performs, so this count is a floor on a
	// real backend's Draw Calls, not an equal.
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
}

void GSDeviceNone::ClearSamplerCache()
{
}

GSTexture* GSDeviceNone::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNone(usage, width, height, levels, format);
}

void GSDeviceNone::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const MergeTopBand* top_band, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
}

void GSDeviceNone::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
}

void GSDeviceNone::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceNone::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceNone::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return true;
}

void GSDeviceNone::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
}

void GSDeviceNone::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
	PresentShader shader, Filter filter)
{
}
