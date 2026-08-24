// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/Common/GSInterlaceModePolicy.h"
#include "GS/Renderers/Common/GSPresentationPolicy.h"
#include "GS/Renderers/Common/GSSnapshotPolicy.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSFeDecode.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GSDumpReplayer.h"
#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKLibretro.h"
#endif
#include "Host.h"
#include "PerformanceMetrics.h"
#include "common/HostSys.h" // GetCPUTicks — present-cap pacer
#include "pcsx2/Config.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "fmt/format.h"
#include "IconsFontAwesome.h"

#include <algorithm>
#include <array>
#include <deque>
#include <thread>
#include <mutex>

static void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename);

static constexpr std::array<PresentShader, 8> s_tv_shader_indices = {
	PresentShader::COPY, PresentShader::SCANLINE,
	PresentShader::DIAGONAL_FILTER, PresentShader::TRIANGULAR_FILTER,
	PresentShader::COMPLEX_FILTER, PresentShader::LOTTES_FILTER,
	PresentShader::SUPERSAMPLE_4xRGSS, PresentShader::SUPERSAMPLE_AUTO};

static std::deque<std::thread> s_screenshot_threads;
static std::mutex s_screenshot_threads_mutex;

std::unique_ptr<GSRenderer> g_gs_renderer;

// Since we read this on the EE thread, we can't put it in the renderer, because
// we might be switching while the other thread reads it.
static GSVector4 s_last_draw_rect;

// Last time we reset the renderer due to a GPU crash, if any.
static Common::Timer::Value s_last_gpu_reset_time;

// Screen alignment
static GSDisplayAlignment s_display_alignment = GSDisplayAlignment::Center;
// Android portrait (GitHub #375): top-align the render instead of vertically centering it,
// so the bottom of a tall screen is free for touch controls. Applied only when the window
// is portrait (height > width); read live per-present. Default on; user can switch to Center.
static bool s_portrait_render_top = true;
// Pixels to leave clear at the top of a portrait window when top-aligning. Supplied by the Android
// side from the display cutout, so a punch-hole or notch camera does not sit on top of the image.
// Zero everywhere else; only consulted on the top-align path, which by definition has spare room
// below it (that space is what the touch controls occupy).
static int s_portrait_render_top_inset = 0;
// Android landscape: top-align the render instead of vertically centering it. Foldables and
// clamshell controllers (Backbone and friends) open the screen DOWNWARD, so a centred image sits
// awkwardly low and the letterbox lands where the hinge/controller is — reported as the one thing
// keeping those users on another emulator. Default off (centre), so nothing changes unless asked.
static bool s_landscape_render_top = false;

// Defined further down alongside the present path. Forward-declared because Merge() needs the
// frame's on-screen rect to size the RetroArch shader chain, and it runs before them.
static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size);
static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect,
	const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive);

GSRenderer::GSRenderer(bool allow_back_records)
	: GSState(nullptr, false, allow_back_records)
	, m_shader_time_start(Common::Timer::GetCurrentValue())
{
	s_last_draw_rect = GSVector4::zero();
}

GSRenderer::~GSRenderer() = default;

void GSRenderer::Reset(bool hardware_reset)
{
	// Clear the current display texture.
	if (hardware_reset)
		g_gs_device->ClearCurrent();

	GSState::Reset(hardware_reset);
}

void GSRenderer::Destroy()
{
	GSCapture::EndCapture();
}

void GSRenderer::UpdateRenderFixes()
{
}

template <GSRenderer::MergeMode merge_mode>
bool GSRenderer::Merge(int field)
{
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	if (!PCRTCDisplays.PCRTCDisplays[0].enabled && !PCRTCDisplays.PCRTCDisplays[1].enabled)
	{
		m_real_size = GSVector2i(0, 0);
		return false;
	}

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y) !=
	                                (PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y != PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y);

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled &&                    // RC1 enabled.
				(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) ||                             // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0));           // Use RC1 for feedback merge.

		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);

		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled &&                // RC2 enabled.
				((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) ||                         // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1));       // Use RC2 for feedback merge.

		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
	{
		m_real_size = GSVector2i(0, 0);

		// Clear out the MAD buffer as some remnants of the previously shown frame came be left over, causing a flash for one frame.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic || GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF)
		{
			GSTexture* mad_tex = g_gs_device->GetMAD();

			if (mad_tex)
			{
				g_gs_device->ClearRenderTarget(mad_tex, 0);
				mad_tex = nullptr;
			}
		}
		return false;
	}

	s_n++;

	// Progressive frames have no temporal deinterlacing state to preserve. Once the active
	// outputs have been resolved, a frame which will not be presented can therefore omit the
	// display merge and all following post-processing without affecting emulated GS memory.
	if constexpr (merge_mode == MergeMode::SkipFinalComposition)
	{
		if (m_scanmask_used)
			m_scanmask_used--;
		return true;
	}

	GSVector4 src_gs_read[2];
	GSVector4 dst[3];

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	// Centralised in GSInterlaceModePolicy.h so the progressive pass-through case (shader_mode -1)
	// is pinned by static_assert and unit tests rather than resting on the sign behaviour of a
	// shift. Ported from sashkinbro/EmuCoreX.
	const GSInterlaceModeSelection interlace_selection = SelectGSInterlaceMode(
		static_cast<int>(GSConfig.InterlaceMode),
		GSConfig.InterlaceMode == GSInterlaceMode::Automatic,
		game_deinterlacing,
		m_regs->SMODE2.FFMD,
		scanmask_frame);
	const int field2 = interlace_selection.field_offset;
	int mode = interlace_selection.shader_mode;
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FastMAD (mode 3) stores four fields in a two-bank history target. Older Mali-G57 Vulkan drivers
	// can expose stale/alternating banks during reconstruction; Bob isn't a safe fallback (its
	// alternating field phase makes the whole picture move vertically). Use weave+blend (mode 2)
	// instead, and suppress the FFMD merge offset so both passes keep a fixed coordinate system.
	// Ported from sashkinbro/EmuCoreX.
	const bool stable_mad_fallback = (mode == 3 && g_gs_device->Features().broken_mad_deinterlace);
	if (stable_mad_fallback)
		mode = 2;

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		const GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();

		float interlace_offset = 0.0f;
		if (isReallyInterlaced() && m_regs->SMODE2.FFMD && !is_bob && !stable_mad_fallback && !GSConfig.DisableInterlaceOffset && GSConfig.InterlaceMode != GSInterlaceMode::Off)
		{
			interlace_offset = (scale.y) * static_cast<float>(field ^ field2);
		}
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;

			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		const GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / ((m_regs->EXTDATA.SMPH - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGH) + 1));
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / ((m_regs->EXTDATA.SMPV - m_regs->DISP[m_regs->EXTBUF.FBIN].DISPLAY.MAGV) + 1);

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if (isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset);
	}

	// Adaptive deinterlacing consumes prior fields. A skipped interlaced frame must update that
	// history, but it does not need optional visual filters or output-size shader work.
	if constexpr (merge_mode == MergeMode::InterlaceHistoryOnly)
	{
		if (m_scanmask_used)
			m_scanmask_used--;
		return true;
	}

	if (GSConfig.ShadeBoost)
		g_gs_device->ShadeBoost();

	if (GSConfig.FXAA)
		g_gs_device->FXAA();

	// RetroArch (.slangp) shader chain runs last in the post-process chain, so it sees the
	// finished frame the way the user actually sees it (ShadeBoost/FXAA included).
	//
	// It renders at the frame's ON-SCREEN size, not the internal one. Shaders that generate
	// detail per output pixel — CRT scanlines above all — must run at display pixel density:
	// generated at 640x448 and then upscaled to a 1080p window, one scanline lands on ~2.4
	// screen pixels, so the presenter's filtering smears them into the uneven, wrong-looking
	// pattern reported on an AYN Thor. RetroArch itself renders the chain into the viewport
	// for exactly this reason.
	//
	// The target MUST be the aspect-corrected draw rect, not the raw window. librashader maps
	// the whole input to the whole viewport, so a 16:9 target for a 4:3 frame stretches the
	// picture — and CalculateDrawDstRect derives its rect from the aspect-ratio SETTING, not
	// from the texture, so it would then letterbox the already-stretched result instead of
	// correcting it. Matching the draw rect keeps the final present a 1:1 blit.
	//
	// m_real_size is deliberately left alone: in CalculateDrawSrcRect it only scales user Crop
	// values (with no crop the src rect is the whole texture either way), so holding it at the
	// internal size keeps crop proportional to the frame rather than to the shaded target.
	if (GSConfig.ShaderChainEnabled && !GSConfig.ShaderChainPreset.empty())
	{
		if (GSTexture* const pre_chain = g_gs_device->GetCurrent())
		{
			const GSVector4i pre_src(CalculateDrawSrcRect(pre_chain, m_real_size));
			const GSVector4 pre_dst(CalculateDrawDstRect(g_gs_device->GetWindowWidth(),
				g_gs_device->GetWindowHeight(), pre_src, pre_chain->GetSize(), s_display_alignment,
				g_gs_device->UsesLowerLeftOrigin(), GetVideoMode() == GSVideoMode::SDTV_480P));
			const GSVector2i on_screen(
				static_cast<int>(std::floor((pre_dst.z - pre_dst.x) + 0.5f)),
				static_cast<int>(std::floor((pre_dst.w - pre_dst.y) + 0.5f)));
			g_gs_device->ApplyShaderChain(on_screen);
		}
	}

	// Sharpens biinear at lower resolutions, almost nearest but with more uniform pixels.
	if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y))
	{
		g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

GSVector2i GSRenderer::GetInternalResolution()
{
	return m_real_size;
}

float GSRenderer::GetModXYOffset()
{
	if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Normal)
	{
		float mod_xy = GetUpscaleMultiplier();
		const int rounded_mod_xy = static_cast<int>(std::round(mod_xy));
		if (rounded_mod_xy > 1)
		{
			if (!(rounded_mod_xy & 1))
				return mod_xy += 0.2f;
			else if (!(rounded_mod_xy & 2))
				return mod_xy += 0.3f;
			else
				return mod_xy += 0.1f;
		}
	}

	return 0.0f;
}

static float GetCurrentAspectRatioFloat(bool is_progressive)
{
	switch (GSConfig.AspectRatio)
	{
		default:
		// We don't know the AR of the display here, nor we care about it
		case AspectRatioType::Stretch:
		case AspectRatioType::RAuto4_3_3_2:
			if (EmuConfig.CurrentCustomAspectRatio > 0.f)
				return EmuConfig.CurrentCustomAspectRatio;
			else if (is_progressive)
				return 3.0f / 2.0f;
			else
				return 4.0f / 3.0f;
		case AspectRatioType::R4_3:
			return 4.0f / 3.0f;
		case AspectRatioType::R16_9:
			return 16.0f / 9.0f;
		case AspectRatioType::R10_7:
			return 10.0f / 7.0f;
		case AspectRatioType::R21_9:
			return 21.0f / 9.0f;
		case AspectRatioType::R20_9:
			return 20.0f / 9.0f;
		case AspectRatioType::R19_5_9:
			return 19.5f / 9.0f;
		case AspectRatioType::Custom:
			// Clamped, not trusted: a 0 or negative would divide by zero downstream.
			return std::clamp(GSConfig.CustomAspectRatio, 0.5f, 5.0f);
	}
}

static GSVector4 CalculateDrawDstRect(s32 window_width, s32 window_height, const GSVector4i& src_rect, const GSVector2i& src_size, GSDisplayAlignment alignment, bool flip_y, bool is_progressive)
{
	const float f_width = static_cast<float>(window_width);
	const float f_height = static_cast<float>(window_height);
	const float clientAr = f_width / f_height;

	float targetAr = clientAr;
	if (EmuConfig.CurrentAspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		if (is_progressive)
			targetAr = 3.0f / 2.0f;
		else
			targetAr = 4.0f / 3.0f;
		// Fall back on the custom aspect ratio set by patches (e.g. 16:9, 21:9)
		if (EmuConfig.CurrentCustomAspectRatio > 0.f)
			targetAr = EmuConfig.CurrentCustomAspectRatio;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R4_3)
	{
		targetAr = 4.0f / 3.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R16_9)
	{
		targetAr = 16.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R10_7)
	{
		targetAr = 10.0f / 7.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R21_9)
	{
		targetAr = 21.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R20_9)
	{
		targetAr = 20.0f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::R19_5_9)
	{
		targetAr = 19.5f / 9.0f;
	}
	else if (EmuConfig.CurrentAspectRatio == AspectRatioType::Custom)
	{
		targetAr = std::clamp(GSConfig.CustomAspectRatio, 0.5f, 5.0f);
	}

	const float crop_adjust = (static_cast<float>(src_rect.width()) / static_cast<float>(src_size.x)) /
		(static_cast<float>(src_rect.height()) / static_cast<float>(src_size.y));

	const double arr = (targetAr * crop_adjust) / clientAr;
	float target_width = f_width;
	float target_height = f_height;
	if (arr < 1)
		target_width = std::floor(f_width * arr + 0.5f);
	else if (arr > 1)
		target_height = std::floor(f_height / arr + 0.5f);

	target_height *= GSConfig.StretchY / 100.0f;

	if (GSConfig.IntegerScaling)
	{
		// make target width/height an integer multiple of the texture width/height
		float t_width = static_cast<double>(src_rect.width());
		float t_height = static_cast<double>(src_rect.height());

		// If using Bilinear (Shape) the image will be prescaled to larger than the window, so we need to unscale it.
		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp && src_rect.width() > 0 && src_rect.height() > 0)
		{
			const GSVector2i resolution = g_gs_renderer->PCRTCDisplays.GetResolution();
			const GSVector2i fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * g_gs_renderer->GetUpscaleMultiplier()),
				static_cast<int>(static_cast<float>(resolution.y) * g_gs_renderer->GetUpscaleMultiplier()));

			if (g_gs_device->GetWindowWidth() > fs.x || g_gs_device->GetWindowHeight() > fs.y)
			{
				t_width *= static_cast<float>(fs.x) / src_rect.width();
				t_height *= static_cast<float>(fs.y) / src_rect.height();
			}
		}

		float scale;
		if ((t_width / t_height) >= 1.0)
			scale = target_width / t_width;
		else
			scale = target_height / t_height;

		if (scale > 1.0)
		{
			const float adjust = std::floor(scale) / scale;
			target_width = target_width * adjust;
			target_height = target_height * adjust;
		}
	}

	float target_x, target_y;
	if (target_width >= f_width)
	{
		target_x = -((target_width - f_width) * 0.5f);
	}
	else
	{
		switch (alignment)
		{
			case GSDisplayAlignment::Center:
				target_x = (f_width - target_width) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_x = (f_width - target_width);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				target_x = 0.0f;
				break;
		}
	}
	const bool is_portrait_window_outer = window_height > window_width;
	if (target_height >= f_height)
	{
		// The render is TALLER than the window, so the overflow is normally split evenly and the
		// image is cropped at both edges. Top-align instead when asked: anchor the top edge and let
		// the crop fall entirely at the bottom.
		//
		// ★ This branch is the one landscape actually takes. A 4:3 game on a wide phone fills the
		// height and pillarboxes the sides, so there is no vertical slack and the `else` below never
		// runs — which is exactly why the landscape setting appeared to do nothing at first.
		target_y = (s_landscape_render_top && !is_portrait_window_outer)
			? 0.0f
			: -((target_height - f_height) * 0.5f);
	}
	else
	{
		// Android #375: top-align the render in a PORTRAIT window (bottom stays free for
		// touch controls). Vertical only — horizontal alignment (target_x) is unchanged.
		const bool is_portrait_window = is_portrait_window_outer;
		GSDisplayAlignment v_align = alignment;
		if ((s_portrait_render_top && is_portrait_window) ||
			(s_landscape_render_top && !is_portrait_window))
			v_align = GSDisplayAlignment::LeftOrTop;
		switch (v_align)
		{
			case GSDisplayAlignment::Center:
				target_y = (f_height - target_height) * 0.5f;
				break;
			case GSDisplayAlignment::RightOrBottom:
				target_y = (f_height - target_height);
				break;
			case GSDisplayAlignment::LeftOrTop:
			default:
				// Push clear of a notch/punch-hole camera when the host asked for it. Only applies
				// to the top-align case; this branch already knows the render is shorter than the
				// window, so shifting it down cannot clip the bottom.
				target_y = (s_portrait_render_top && window_height > window_width)
					? static_cast<float>(s_portrait_render_top_inset)
					: 0.0f;
				break;
		}
	}

	GSVector4 ret(target_x, target_y, target_x + target_width, target_y + target_height);

	if (flip_y)
	{
		const float height = ret.w - ret.y;
		ret.y = static_cast<float>(window_height) - ret.w;
		ret.w = ret.y + height;
	}

	return ret;
}

static GSVector4i CalculateDrawSrcRect(const GSTexture* src, const GSVector2i real_size)
{
	const GSVector2i size(src->GetSize());
	const GSVector2 scale = GSVector2(size.x, size.y) / GSVector2(real_size.x, real_size.y).max(GSVector2(0.1f, 0.1f));
	const float upscale = GSIsHardwareRenderer() ? GSConfig.UpscaleMultiplier : 1;
	const int left = static_cast<int>(static_cast<float>(GSConfig.Crop[0] * scale.x) * upscale);
	const int top = static_cast<int>(static_cast<float>(GSConfig.Crop[1] * scale.y) * upscale);
	const int right =  size.x - static_cast<int>(static_cast<float>(GSConfig.Crop[2] * scale.x) * upscale);
	const int bottom = size.y - static_cast<int>(static_cast<float>(GSConfig.Crop[3] * scale.y) * upscale);
	return GSVector4i(left, top, right, bottom);
}

static const char* GetScreenshotSuffix()
{
	static constexpr const char* suffixes[static_cast<u8>(GSScreenshotFormat::Count)] = {
		"png", "jpg", "webp"};
	return suffixes[static_cast<u8>(GSConfig.ScreenshotFormat)];
}

static void CompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels)
{
	RGBA8Image image;
	image.SetPixels(width, height, std::move(pixels));

	std::string key(fmt::format("GSScreenshot_{}", filename));

	if (!GSDumpReplayer::IsRunner())
	{
		Host::AddIconOSDMessage(key, ICON_FA_CAMERA,
			fmt::format(TRANSLATE_FS("GS", "Saving screenshot to '{}'."), Path::GetFileName(filename)), 60.0f);
	}

	// maybe std::async would be better here.. but it's definitely worth threading, large screenshots take a while to compress.
	std::unique_lock lock(s_screenshot_threads_mutex);
	s_screenshot_threads.emplace_back([key = std::move(key), filename = std::move(filename), image = std::move(image),
										  quality = GSConfig.ScreenshotQuality]() {
		if (image.SaveToFile(filename.c_str(), quality))
		{
			if (!GSDumpReplayer::IsRunner())
			{
				Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
					fmt::format(TRANSLATE_FS("GS", "Saved screenshot to '{}'."), Path::GetFileName(filename)),
					Host::OSD_INFO_DURATION);
			}
		}
		else
		{
			Host::AddIconOSDMessage(std::move(key), ICON_FA_CAMERA,
				fmt::format(TRANSLATE_FS("GS", "Failed to save screenshot to '{}'."), Path::GetFileName(filename),
					Host::OSD_ERROR_DURATION));
		}

		// remove ourselves from the list, if the GS thread is waiting for us, we won't be in there
		const auto this_id = std::this_thread::get_id();
		std::unique_lock lock(s_screenshot_threads_mutex);
		for (auto it = s_screenshot_threads.begin(); it != s_screenshot_threads.end(); ++it)
		{
			if (it->get_id() == this_id)
			{
				it->detach();
				s_screenshot_threads.erase(it);
				break;
			}
		}
	});
}

void GSJoinSnapshotThreads()
{
	std::unique_lock lock(s_screenshot_threads_mutex);
	while (!s_screenshot_threads.empty())
	{
		std::thread save_thread(std::move(s_screenshot_threads.front()));
		s_screenshot_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

// What was live when the GPU died, as one greppable block for the emulog.
//
// A lost device is almost always the driver refusing something we asked it to do, and the ask is
// visible in the feature set rather than in the crash. The Mali r44p1 blob is the worked example:
// it loses the Vulkan device under attachment-feedback-loop and mishandles in-tile framebuffer
// fetch on GL, both of which are the accurate-blending destination read. Neither is deducible from
// "host GPU lost" -- and nothing else in the log says which blend path the device had picked,
// because that decision is made from driver strings at startup and never restated.
//
// So this exists to make the report actionable rather than to change behaviour: a user who says
// "it crashed on my handheld" now hands us the driver build and the exact blend configuration that
// killed it. Keep it to facts we can act on -- identity, the destination-read path, and the
// settings that steer it.
static std::string DescribeDeviceForLossReport()
{
	static constexpr const char* blend_level_names[] = {
		"Minimum", "Basic", "Medium", "High", "Full", "Maximum"};

	const GSDevice::FeatureSupport& f = g_gs_device->Features();

	// The destination read is the thing most likely to have killed us, so name the path in words
	// rather than making a reader reconstruct it from three booleans.
	const char* blend_path;
	if (f.framebuffer_fetch)
		blend_path = f.framebuffer_fetch_orders_overlap ? "in-tile framebuffer fetch (orders overlap)" :
														 "in-tile framebuffer fetch (barrier still taken on overlap)";
	else if (f.texture_barrier)
		blend_path = "texture barrier";
	else if (f.multidraw_fb_copy)
		blend_path = "render-target copy per primitive group";
	else
		blend_path = "render-target copy per draw";

	const u8 blend_level = static_cast<u8>(GSConfig.AccurateBlendingUnit);

	// Every backend's driver info is several lines (the GL version string, the Vulkan driver and
	// conformance versions, the device name), and the driver build is the single most useful field
	// in here -- it is what identifies r44p1. Indent its continuation lines so the block stays one
	// visually contiguous report instead of three lines that look like unrelated log output.
	const std::string driver_info = StringUtil::ReplaceAll(g_gs_device->GetDriverInfo(), "\n", "\n  ");

	return fmt::format(
		"  Renderer: {}\n"
		"  {}\n"
		"  Destination read: {}\n"
		"  framebuffer_fetch={} texture_barrier={} multidraw_fb_copy={} depth_feedback={}\n"
		"  Blending accuracy: {}, DisableFramebufferFetch={}, OverrideTextureBarriers={}",
		Pcsx2Config::GSOptions::GetRendererName(GSGetCurrentRenderer()), driver_info, blend_path,
		f.framebuffer_fetch, f.texture_barrier, f.multidraw_fb_copy, f.depth_feedback,
		(blend_level < std::size(blend_level_names)) ? blend_level_names[blend_level] : "?",
		static_cast<bool>(GSConfig.DisableFramebufferFetch), static_cast<int>(GSConfig.OverrideTextureBarriers));
}

bool GSRenderer::BeginPresentFrame(bool frame_skip)
{
	Host::BeginPresentFrame();

	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
	{
		// If we're skipping a frame, we need to reset imgui's state, since
		// we won't be calling EndPresentFrame().
		ImGuiManager::SkipFrame();
		return false;
	}
	else if (res == GSDevice::PresentResult::OK)
	{
		// All good!
		return true;
	}

	// Describe the device BEFORE anything below touches it. The abort path never returns and the
	// recovery path destroys the device, so this is the last point at which the driver identity and
	// the feature set that caused the loss can still be read.
	const std::string device_description = DescribeDeviceForLossReport();
	Console.Error(fmt::format("Host GPU device lost. Configuration in use at the time:\n{}", device_description));

	// If we're constantly crashing on something in particular, we don't want to end up in an
	// endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
	// reasons. So just abort in such case.
	//
	// A configuration the driver cannot survive reaches this deterministically: the recovery below
	// rebuilds the identical device, so the second loss follows the first within a frame or two.
	// That makes this the normal end state for such a device rather than the rare one, and it fires
	// before the OSD warning further down -- so from the user's side it is an unexplained crash and
	// this message is the whole bug report. Say what died.
	const Common::Timer::Value current_time = Common::Timer::GetCurrentValue();
	if (s_last_gpu_reset_time != 0 &&
		Common::Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < 15.0f)
	{
		pxFailRel(fmt::format("Host GPU lost too many times, device is probably completely wedged.\n{}",
			device_description)
					  .c_str());
	}
	s_last_gpu_reset_time = current_time;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSGetCurrentRenderer(), std::nullopt))
	{
		pxFailRel(
			fmt::format("Failed to recreate GS device after loss.\n{}", device_description).c_str());
		return false;
	}

	// First frame after reopening is definitely going to be trash, so skip it.
	Host::AddIconOSDMessage("GSDeviceLost", ICON_FA_TRIANGLE_EXCLAMATION,
		TRANSLATE_SV("GS", "Host GPU device encountered an error and was recovered. This may have broken rendering."),
		Host::OSD_CRITICAL_ERROR_DURATION);
	return false;
}

void GSRenderer::EndPresentFrame()
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	g_gs_device->EndPresent();
	ImGuiManager::NewFrame();
}

// The device accumulates GPU timing when a command buffer RETIRES, which has nothing to do with
// whether the frame was presented -- the timestamps are written at command-buffer begin and at
// submit. So the accumulator has to be drained once per VSync on every path, or the time it holds
// is silently carried into whichever later frame happens to drain it, and never drained at all in
// a configuration that never presents. That was the state of things: surfaceless gsrunner runs
// computed a GPU time on-device every frame and threw it away, which read as "the GPU is not
// measured here" and left the renderer A/Bs with no GPU term at all.
void GSRenderer::SampleGPUTiming()
{
	const float gpu_time = g_gs_device->GetAndResetAccumulatedGPUTime();
	const GPUPipelineStatistics gpu_stats = g_gs_device->GetAndResetAccumulatedGPUPipelineStatistics();
	PerformanceMetrics::OnGPUTimingSampled(gpu_time, gpu_stats.vs_invocations, gpu_stats.ps_invocations);
}

void GSRenderer::SubmitVsync(u32 field, bool registers_written)
{
	GSBackQueue::VsyncRecord rec;
	rec.field = field;
	rec.registers_written = registers_written;
	rec.idle_frame = IsIdleFrame(); // front-computable: compares serials against the last frame's

	// Front-end decode tap: the frame boundary, so a divergence can be reported
	// as "frame 3, draw 812" and not only as an event index.
	if (GSFeDecode::IsActive()) [[unlikely]]
		FeDecodeOnVsync(rec.field, rec.registers_written, rec.idle_frame);

	// VSYNC is never queued: present runs on the MTGS thread behind a drain, so
	// the back thread stays off the GSDevice on present paths entirely (which
	// is also what keeps SW + GL-present devices legal in queued modes).
	DrainBackQueue();
	ExecVsyncRecord(rec);
}

void GSRenderer::ExecVsyncRecord(const GSBackQueue::VsyncRecord& rec)
{
	VSync(rec.field, rec.registers_written, rec.idle_frame);
}

void GSRenderer::VSync(u32 field, bool registers_written, bool idle_frame)
{
	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		if (GSConfig.SaveInfo)
		{
			DumpGSPrivRegs(*m_regs, GetDrawDumpPath("%05lld_f%05lld_vsync_gs_reg.txt", s_n, g_perfmon.GetFrame()));

			DumpDrawInfo(false, false, true);
		}

		if (GSConfig.SaveTransferImages)
			DumpTransferImages();

		if (GSConfig.SaveFrameStats)
		{
			m_perfmon_frame = g_perfmon - m_perfmon_frame;
			m_perfmon_frame.Dump(GetDrawDumpPath("%05lld_f%05lld_frame_stats.txt", s_n, g_perfmon.GetFrame()), GSIsHardwareRenderer());
			m_perfmon_frame = g_perfmon;
		}
	}

	const int fb_sprite_blits = g_perfmon.GetDisplayFramebufferSpriteBlits();
	const bool fb_sprite_frame = (fb_sprite_blits > 0);

	bool skip_frame = false;
	if (GSConfig.SkipDuplicateFrames && !GSCapture::IsCapturingVideo())
	{
		bool is_unique_frame;
		switch (PerformanceMetrics::GetInternalFPSMethod())
		{
		case PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister:
			is_unique_frame = registers_written;
			break;
		case PerformanceMetrics::InternalFPSMethod::DISPFBBlit:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
		{
			m_skipped_duplicate_frames = 0;
		}
	}

	// ★ Manual frame skip and the max-presented-FPS cap. Both were fully implemented in GS.cpp with
	// JNI setters wired to live UI controls, and both had ZERO readers — GSGetManualFrameSkip() and
	// GSGetMaxPresentInterval() were never called, so the in-game "Frame Skip" picker (0..5) and the
	// FPS cap silently did nothing. The GS.cpp comments named this exact function as the reader, so
	// the consumer was lost rather than never written. Restored here.
	//
	// Manual skipping omits presentation only. Platform-opted FPS caps may additionally omit final
	// composition after Merge() has verified the current outputs; emulation and GS writes still run.
	bool fps_cap_present_skip = false;
	{
		const u32 manual_skip = GSGetManualFrameSkip();
		if (manual_skip > 0)
		{
			// Present 1 frame in every (manual_skip + 1).
			m_manual_frameskip_phase = (m_manual_frameskip_phase + 1) % (manual_skip + 1);
			if (m_manual_frameskip_phase != 0)
				skip_frame = true;
		}
		else
		{
			m_manual_frameskip_phase = 0;
		}
	}
	if (!skip_frame)
	{
		if (!GSGetPresentCapSuspended())
		{
			// Accumulator pacer, not a simple "too soon?" test: advancing the deadline by exactly one
			// interval holds the requested AVERAGE rate even when it isn't a whole division of the
			// source (47 or 55 fps work, not just 30/20/15). Resynchronise when we fall more than one
			// interval behind, so a hitch can't bank credit and then burst.
			const u64 interval = GSGetMaxPresentInterval();
			if (interval > 0)
			{
				const u64 now = GetCPUTicks();
				if (m_next_present_deadline == 0 || now + interval < m_next_present_deadline)
					m_next_present_deadline = now; // first frame, or the clock jumped backwards
				if (now < m_next_present_deadline)
				{
					skip_frame = true;
					fps_cap_present_skip = true;
				}
				else if ((now - m_next_present_deadline) > interval)
					m_next_present_deadline = now + interval; // far behind: restart the cadence
				else
					m_next_present_deadline += interval;
			}
			else
			{
				m_next_present_deadline = 0;
			}
		}
		else
		{
			// Turbo owns presentation cadence while a custom cap is active. Re-prime
			// from the next normal frame instead of carrying a stale deadline forward.
			m_next_present_deadline = 0;
		}
	}

	// The GS has already processed draw commands and framebuffer writes before VSync. A cap-skipped
	// frame can omit display-only work when no image consumer is active.
	// Interlaced frames take a separate history-only path below so temporal deinterlacing remains
	// correct. Requiring an actual cap-created skip keeps duplicate/manual skips on master's
	// original full-render path when the default 60 FPS mode is selected.
	const bool request_skipped_final_render =
		fps_cap_present_skip && GSGetPresentCapRenderSkip() &&
		GSIsHardwareRenderer() &&
		m_regs->EXTWRITE.WRITE == 0 &&
		m_snapshot.empty() && !m_dump && m_dump_frames == 0 && !GSCapture::IsCapturingVideo() &&
		!GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && g_gs_device->GetCurrent() != nullptr;

	bool merged_frame;
	if (!request_skipped_final_render)
	{
		// Compile-time specialization leaves the default 60 FPS path with the same
		// full Merge() work and no per-frame merge-mode checks.
		merged_frame = Merge<MergeMode::Full>(field);
	}
	else if (isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		merged_frame = Merge<MergeMode::InterlaceHistoryOnly>(field);
	}
	else
	{
		merged_frame = Merge<MergeMode::SkipFinalComposition>(field);
	}
	const bool skipped_final_render = request_skipped_final_render && merged_frame;
	const bool blank_frame = !merged_frame;
	// Run length, not just "was blank": the policy below distinguishes a single alternating blank
	// (an interlaced-field artefact, safe to drop) from a run of them (a fade the game is actually
	// drawing, which must be presented).
	if (!skipped_final_render)
		m_consecutive_blank_frames = blank_frame ? (m_consecutive_blank_frames + 1) : 0;

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	// Only cap-created skips may defer the maintenance scan. Native 60 FPS,
	// duplicate-frame skips, and manual skips retain master's AgePool() behavior.
	if (!idle_frame)
	{
		if (fps_cap_present_skip && GSGetPresentCapRenderSkip())
			g_gs_device->AgePoolAfterPresentCapSkip();
		else
			g_gs_device->AgePool();
	}

#ifdef __ANDROID__
	// Suppress only startup blanks, before the GS has produced any output. Mid-game blank/fade
	// frames must take the normal present path: explicit APIs such as Vulkan need that path to
	// submit the recorded command buffer and finalize texture state for the following frame.
	// See GSPresentationPolicy.h. Ported from sashkinbro/EmuCoreX.
	//
	// ...but never suppress a blank that has an OSD message or a toast on top of it. With Skip BIOS
	// on there is no boot animation, so the game shows a black screen with no GS output for a while,
	// and the RetroAchievements "achievements loaded" toast posts into exactly that window. Skipping
	// the present means EndPresentFrame() — and with it the OSD/notification draw — never runs, so
	// the toast is queued but invisible until something forces a real present (opening the pause
	// menu, which is why it appears there and vanishes on back-out). Presenting a blank with content
	// on it is precisely what the pause menu already does here, and is safe: the swapchain image is
	// acquired and ImGui draws over black.
	const bool skip_blank = ShouldSkipAndroidBlankFrame(
		blank_frame,
		g_gs_device->GetCurrent() != nullptr,
		g_gs_device->GetRenderAPI() == RenderAPI::Vulkan,
		m_consecutive_blank_frames) &&
		!ImGuiManager::HasPresentableOverlayContent();
#else
	constexpr bool skip_blank = false;
#endif

	// Skip presentation when running uncapped while vsync is on. ShouldSkipPresentingFrame()
	// consumes the present-throttle credit when it answers "present", so it belongs last in
	// this disjunction and nowhere else in the function — see its declaration.
	if (skip_frame || skip_blank || g_gs_device->ShouldSkipPresentingFrame())
	{
		if (BeginPresentFrame(true))
		{
			EndPresentFrame();
			PerformanceMetrics::OnPresent();
		}

		SampleGPUTiming();
		PerformanceMetrics::Update(registers_written, fb_sprite_frame, skip_frame);
	}
	else
	{

		g_perfmon.EndFrame(idle_frame);

		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();

		// Little bit ugly, but we can't do CAS inside the render pass.
		GSVector4i src_rect;
		GSVector4 src_uv, draw_rect;
		GSTexture* current = g_gs_device->GetCurrent();
		if (current && !blank_frame)
		{
#ifdef ENABLE_VULKAN
			// Libretro: the output canvas is a backbuffer this side sizes, not
			// a real window, so track the merged frame — expanded to the target
			// aspect ratio (the internal-resolution screenshot rule) and
			// clamped to the geometry advertised to the frontend — so internal
			// upscale survives the present pass. Resize before the draw rect
			// below is computed so the whole frame stays consistent.
			if (VKLibretro::Active)
			{
				const float aspect = GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P);
				float fwidth = static_cast<float>(current->GetWidth());
				float fheight = static_cast<float>(current->GetHeight());
				if (fwidth / fheight >= aspect)
					fheight = fwidth / aspect;
				else
					fwidth = fheight * aspect;
				const float clamp_scale = std::min({1.0f,
					static_cast<float>(VKLibretro::kMaxCanvasWidth) / fwidth,
					static_cast<float>(VKLibretro::kMaxCanvasHeight) / fheight});
				const u32 canvas_width = std::max(1, static_cast<int>(std::lround(fwidth * clamp_scale)));
				const u32 canvas_height = std::max(1, static_cast<int>(std::lround(fheight * clamp_scale)));
				if (canvas_width != static_cast<u32>(g_gs_device->GetWindowWidth()) ||
					canvas_height != static_cast<u32>(g_gs_device->GetWindowHeight()))
				{
					g_gs_device->ResizeWindow(canvas_width, canvas_height, g_gs_device->GetWindowScale());
					ImGuiManager::WindowResized();
				}
			}
#endif

			src_rect = CalculateDrawSrcRect(current, m_real_size);
			src_uv = GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy();
			const GSVector2i pres_size = g_gs_device->GetPresentationSize();
			draw_rect = CalculateDrawDstRect(pres_size.x, pres_size.y,
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P);
			s_last_draw_rect = draw_rect;

			// MetalFX spatial upscale runs before CAS/present, and only when actually upscaling
			// (source smaller than the on-screen draw rect). CAS can still sharpen afterward.
			if (GSConfig.Upscaler == GSUpscaler::MetalFXSpatial)
			{
				static bool mfx_log_once = false;
				if (g_gs_device->Features().metalfx_spatial)
				{
					const int draw_w = static_cast<int>(std::ceil(draw_rect.z - draw_rect.x));
					const int draw_h = static_cast<int>(std::ceil(draw_rect.w - draw_rect.y));
					if (current->GetWidth() < draw_w && current->GetHeight() < draw_h)
						g_gs_device->MetalFXUpscale(current, src_rect, src_uv, draw_rect);
				}
				else if (!mfx_log_once)
				{
					Host::AddIconOSDMessage("MetalFXUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "MetalFX upscaling is not available on this system (requires a Metal GPU on macOS 13 or newer)."),
						10.0f);
					mfx_log_once = true;
				}
			}

			if (GSConfig.CASMode != GSCASMode::Disabled)
			{
				static bool cas_log_once = false;
				if (g_gs_device->Features().cas_sharpening)
				{
					// sharpen only if the IR is higher than the display resolution
					const bool sharpen_only = (GSConfig.CASMode == GSCASMode::SharpenOnly ||
					                           (current->GetWidth() > g_gs_device->GetWindowWidth() &&
					                            current->GetHeight() > g_gs_device->GetWindowHeight()));
					g_gs_device->CAS(current, src_rect, src_uv, draw_rect, sharpen_only);
				}
				else if (!cas_log_once)
				{
					Host::AddIconOSDMessage("CASUnsupported", ICON_FA_TRIANGLE_EXCLAMATION,
						TRANSLATE_SV("GS", "CAS is not available, your graphics driver does not support the required functionality."),
						10.0f);
					cas_log_once = true;
				}
			}
		}

		if (BeginPresentFrame(false))
		{
			if (current && !blank_frame)
			{
				const u64 current_time = Common::Timer::GetCurrentValue();
				const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

				g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
					s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
			}

			EndPresentFrame();
			PerformanceMetrics::OnPresent();
		}

		SampleGPUTiming();
		PerformanceMetrics::Update(registers_written, fb_sprite_frame, false);
	}

	// snapshot
	const GSSnapshotAction snapshot_action =
		SelectGSSnapshotAction(!m_snapshot.empty(), m_snapshot_dump_frames, static_cast<bool>(m_dump), m_dump_frames);

	if (!m_snapshot.empty())
	{
		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;

		if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp)
		{
			const GSTexture* current = g_gs_device->GetCurrent();
			const GSVector2i internal_res = GetInternalResolution();

			if (current && (current->GetWidth() > internal_res.x || current->GetHeight() > internal_res.y))
				g_gs_device->Resize(internal_res.x, internal_res.y);
		}

		if (snapshot_action.refuse_dump)
		{
			// The screenshot below still gets written -- it is the part of the request that can
			// be served. Saying so beats leaving the caller to wonder where the dump went.
			Host::AddKeyedOSDMessage("GSDump",
				TRANSLATE_STR("GS", "A GS dump is already recording; only a screenshot was saved."),
				Host::OSD_WARNING_DURATION);
		}

		if (snapshot_action.open_dump)
		{
			m_dump_frames = m_snapshot_dump_frames;

			if (GSConfig.UserHacks_ReadTCOnClose)
				ReadbackTextureCache();

			// The dump replays from this state forward, so it has to be the state a
			// savestate would record here: parse registers from the front object under
			// the split, local memory from the back. m_parse_target->Freeze() is the
			// same call GSfreeze makes, and it drains before serializing.
			freezeData fd = {0, nullptr};
			m_parse_target->Freeze(&fd, true);
			fd.data = new u8[fd.size];
			m_parse_target->Freeze(&fd, false);

			// keep the screenshot relatively small so we don't bloat the dump
			static constexpr u32 DUMP_SCREENSHOT_WIDTH = 640;
			static constexpr u32 DUMP_SCREENSHOT_HEIGHT = 480;
			SaveSnapshotToMemory(DUMP_SCREENSHOT_WIDTH, DUMP_SCREENSHOT_HEIGHT, true, false,
				&screenshot_width, &screenshot_height, &screenshot_pixels);

			std::string_view compression_str;
			if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::Uncompressed)
			{
				m_dump = GSDumpBase::CreateUncompressedDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with no compression");
			}
			else if (GSConfig.GSDumpCompression == GSDumpCompressionMethod::LZMA)
			{
				m_dump = GSDumpBase::CreateXzDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with LZMA compression");
			}
			else
			{
				m_dump = GSDumpBase::CreateZstDump(m_snapshot, VMManager::GetDiscSerial(),
					VMManager::GetDiscCRC(), screenshot_width, screenshot_height,
					screenshot_pixels.empty() ? nullptr : screenshot_pixels.data(), fd, m_regs);
				compression_str = TRANSLATE_SV("GS", "with Zstandard compression");
			}

			delete[] fd.data;

			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saving {0} GS dump {1} to '{2}'"),
					(m_dump_frames == 1) ? TRANSLATE_SV("GS", "single frame") : TRANSLATE_SV("GS", "multi-frame"), compression_str,
					Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
		}

		const bool internal_resolution = (GSConfig.ScreenshotSize >= GSScreenshotSize::InternalResolution);
		const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);

		if (g_gs_device->GetCurrent() && SaveSnapshotToMemory(
			internal_resolution ? 0 : g_gs_device->GetWindowWidth(),
			internal_resolution ? 0 : g_gs_device->GetWindowHeight(),
			aspect_correct, true,
			&screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			CompressAndWriteScreenshot(fmt::format("{}.{}", m_snapshot, GetScreenshotSuffix()),
				screenshot_width, screenshot_height, std::move(screenshot_pixels));
		}
		else
		{
			Host::AddIconOSDMessage("GSScreenshot", ICON_FA_CAMERA,
				TRANSLATE_SV("GS", "Failed to render/download screenshot."), Host::OSD_ERROR_DURATION);
		}

		m_snapshot = {};
		m_snapshot_dump_frames = 0;
	}

	// Independent of the request above: a recording takes this frame whether or not a snapshot
	// landed on it. Making these two alternatives is what dropped the frame boundary.
	if (snapshot_action.record_vsync)
	{
		if (m_dump->VSync(field, snapshot_action.dump_is_last, m_regs))
		{
			Host::AddKeyedOSDMessage("GSDump",
				fmt::format(TRANSLATE_FS("GS", "Saved GS dump to '{}'."), Path::GetFileName(m_dump->GetPath())),
				Host::OSD_INFO_DURATION);
			m_dump.reset();
		}
		else if (!snapshot_action.dump_is_last)
		{
			m_dump_frames--;
		}
	}

	// capture
	if (GSCapture::IsCapturingVideo())
	{
		const GSVector2i size = GSCapture::GetSize();
		if (GSTexture* current = g_gs_device->GetCurrent())
		{
			// TODO: Maybe avoid this copy in the future? We can use swscale to fix it up on the dumping thread..
			if (current->GetSize() != size)
			{
				GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, false);
				if (temp)
				{
					g_gs_device->StretchRect(current, temp, GSVector4(0, 0, size.x, size.y), ShaderConvert::COPY, Biln);
					GSCapture::DeliverVideoFrame(temp);
					g_gs_device->Recycle(temp);
				}
			}
			else
			{
				GSCapture::DeliverVideoFrame(current);
			}
		}
		else
		{
			// Bit janky, but unless we want to make variable frame rate files, we need to deliver *a* frame to
			// the video file, so just grab a blank RT.
			GSTexture* temp = g_gs_device->CreateRenderTarget(size.x, size.y, GSTexture::Format::Color, true);
			if (temp)
			{
				GSCapture::DeliverVideoFrame(temp);
				g_gs_device->Recycle(temp);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()) && GSConfig.SaveTransferImages)
		DumpTransferImages();
}

bool GSRenderer::QueueSnapshot(const std::string& path, const u32 gsdump_frames)
{
	if (!m_snapshot.empty())
		return false;

	// Allows for providing a complete path
	if (path.size() > 4 && StringUtil::EndsWithNoCase(path, ".png"))
		m_snapshot = path.substr(0, path.size() - 4);
	else
		m_snapshot = GSGetBaseSnapshotFilename();

	// this is really gross, but wx we get the snapshot request after shift...
	m_snapshot_dump_frames = gsdump_frames;
	return true;
}

static std::string GSGetBaseFilename()
{
	std::string filename;

	// append the game serial and title
	if (std::string name(VMManager::GetTitle(true)); !name.empty())
	{
		Path::SanitizeFileName(&name);
		if (name.length() > 219)
			name.resize(219);
		filename += name;
	}
	if (std::string serial = VMManager::GetDiscSerial(); !serial.empty())
	{
		Path::SanitizeFileName(&serial);
		filename += '_';
		filename += serial;
	}

	const time_t cur_time = time(nullptr);
	char local_time[16];

	if (strftime(local_time, sizeof(local_time), "%Y%m%d%H%M%S", localtime(&cur_time)))
	{
		static time_t prev_snap;
		// The variable 'n' is used for labelling the screenshots when multiple screenshots are taken in
		// a single second, we'll start using this variable for naming when a second screenshot request is detected
		// at the same time as the first one. Hence, we're initially setting this counter to 2 to imply that
		// the captured image is the 2nd image captured at this specific time.
		static int n = 2;

		filename += '_';

		if (cur_time == prev_snap)
			filename += fmt::format("{0}_({1})", local_time, n++);
		else
		{
			n = 2;
			filename += fmt::format("{}", local_time);
		}
		prev_snap = cur_time;
	}

	return filename;
}

std::string GSGetBaseSnapshotFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeSnapshotsByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Snapshots, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}

	return Path::Combine(EmuFolders::Snapshots, GSGetBaseFilename());
}

std::string GSGetBaseVideoFilename()
{
	// If organize by game is enabled, use or create a game-specific folder.
	if (GSConfig.OrganizeVideoCaptureByGame)
	{
		const bool prefer_english = Host::GetBaseBoolSettingValue("UI", "PreferEnglishGameList", false);
		std::string game_name = VMManager::GetTitle(prefer_english);
		if (!game_name.empty())
		{
			Path::SanitizeFileName(&game_name);
			const std::string game_dir = Path::Combine(EmuFolders::Videos, game_name);

			// Make sure the per-game directory exists or that we can successfully create it.
			if (FileSystem::DirectoryExists(game_dir.c_str()) || FileSystem::CreateDirectoryPath(game_dir.c_str(), false))
				return Path::Combine(game_dir, GSGetBaseFilename());
		}
	}
	// prepend video directory
	return Path::Combine(EmuFolders::Videos, GSGetBaseFilename());
}

void GSRenderer::StopGSDump()
{
	m_snapshot = {};
	m_snapshot_dump_frames = 0;
	m_dump_frames = 0;
}

void GSRenderer::PresentCurrentFrame()
{
	if (BeginPresentFrame(false))
	{
		GSTexture* current = g_gs_device->GetCurrent();
		if (current)
		{
			const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
			const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());
			const GSVector2i pres_size = g_gs_device->GetPresentationSize();
			const GSVector4 draw_rect(CalculateDrawDstRect(pres_size.x, pres_size.y,
				src_rect, current->GetSize(), s_display_alignment, g_gs_device->UsesLowerLeftOrigin(),
				GetVideoMode() == GSVideoMode::SDTV_480P));
			s_last_draw_rect = draw_rect;

			const u64 current_time = Common::Timer::GetCurrentValue();
			const float shader_time = static_cast<float>(Common::Timer::ConvertValueToSeconds(current_time - m_shader_time_start));

			g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect,
				s_tv_shader_indices[GSConfig.TVShader], shader_time, BilnIf(GSConfig.LinearPresent != GSPostBilinearMode::Off));
		}

		EndPresentFrame();
	}
}

void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y)
{
	const float draw_width = s_last_draw_rect.z - s_last_draw_rect.x;
	const float draw_height = s_last_draw_rect.w - s_last_draw_rect.y;
	const float rel_x = window_x - s_last_draw_rect.x;
	const float rel_y = window_y - s_last_draw_rect.y;
	if (rel_x < 0 || rel_x > draw_width || rel_y < 0 || rel_y > draw_height)
	{
		*display_x = -1.0f;
		*display_y = -1.0f;
		return;
	}

	*display_x = rel_x / draw_width;
	*display_y = rel_y / draw_height;
}

void GSSetDisplayAlignment(GSDisplayAlignment alignment)
{
	s_display_alignment = alignment;
}

void GSSetPortraitRenderTopInset(int pixels)
{
	s_portrait_render_top_inset = (pixels > 0) ? pixels : 0;
}

void GSSetPortraitRenderTopAlign(bool enabled)
{
	s_portrait_render_top = enabled;
}

void GSSetLandscapeRenderTopAlign(bool enabled)
{
	s_landscape_render_top = enabled;
}

bool GSRenderer::BeginCapture(std::string filename, const GSVector2i& size)
{
	// GV7-2: capture start/stop can run mid-frame on the MTGS thread; teardown
	// frees download textures on the device the back thread may be drawing on.
	DrainBackQueue();
	const GSVector2i capture_resolution = (size.x != 0 && size.y != 0) ?
											  size :
											  (GSConfig.VideoCaptureAutoResolution ?
													  GetInternalResolution() :
													  GSVector2i(GSConfig.VideoCaptureWidth, GSConfig.VideoCaptureHeight));

	return GSCapture::BeginCapture(GetTvRefreshRate(), capture_resolution,
		GetCurrentAspectRatioFloat(GetVideoMode() == GSVideoMode::SDTV_480P),
		std::move(filename));
}

void GSRenderer::EndCapture()
{
	DrainBackQueue(); // see BeginCapture
	GSCapture::EndCapture();
}

GSTexture* GSRenderer::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return nullptr;
}

bool GSRenderer::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}

bool GSRenderer::SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	// GV7-2: mid-frame screenshot issues device calls (CreateRenderTarget /
	// StretchRect) on the MTGS thread; the back thread may be mid-draw on the
	// same device. The vsync-path callers are already post-drain (no-op there).
	DrainBackQueue();

	GSTexture* const current = g_gs_device->GetCurrent();
	if (!current)
	{
		*width = 0;
		*height = 0;
		pixels->clear();
		return false;
	}

	const GSVector4i src_rect(CalculateDrawSrcRect(current, m_real_size));
	const GSVector4 src_uv(GSVector4(src_rect) / GSVector4(current->GetSize()).xyxy());

	const bool is_progressive = (GetVideoMode() == GSVideoMode::SDTV_480P);
	GSVector4 draw_rect;
	if (window_width == 0 || window_height == 0)
	{
		if (apply_aspect)
		{
			// use internal resolution of the texture
			const float aspect = GetCurrentAspectRatioFloat(is_progressive);
			const int tex_width = current->GetWidth();
			const int tex_height = current->GetHeight();

			// expand to the larger dimension
			const float tex_aspect = static_cast<float>(tex_width) / static_cast<float>(tex_height);
			if (tex_aspect >= aspect)
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_width), static_cast<float>(tex_width) / aspect);
			else
				draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(tex_height) * aspect, static_cast<float>(tex_height));
		}
		else
		{
			// uncorrected aspect is only available at internal resolution
			draw_rect = GSVector4(0.0f, 0.0f, static_cast<float>(current->GetWidth()), static_cast<float>(current->GetHeight()));
		}
	}
	else
	{
		draw_rect = CalculateDrawDstRect(window_width, window_height, src_rect, current->GetSize(),
			GSDisplayAlignment::LeftOrTop, false, is_progressive);
	}
	const u32 draw_width = static_cast<u32>(draw_rect.z - draw_rect.x);
	const u32 draw_height = static_cast<u32>(draw_rect.w - draw_rect.y);
	const u32 image_width = crop_borders ? draw_width : std::max(draw_width, window_width);
	const u32 image_height = crop_borders ? draw_height : std::max(draw_height, window_height);

	// We're not expecting screenshots to be fast, so just allocate a download texture on demand.
	GSTexture* rt = g_gs_device->CreateRenderTarget(draw_width, draw_height, GSTexture::Format::Color, false);
	if (rt)
	{
		std::unique_ptr<GSDownloadTexture> dl(g_gs_device->CreateDownloadTexture(draw_width, draw_height, GSTexture::Format::Color));
		if (dl)
		{
			const GSVector4i rc(0, 0, draw_width, draw_height);
			g_gs_device->StretchRect(current, src_uv, rt, GSVector4(rc), ShaderConvert::TRANSPARENCY_FILTER, Biln);
			dl->CopyFromTexture(rc, rt, rc, 0);
			dl->Flush();

			if (dl->Map(rc))
			{
				const u32 pad_x = (image_width - draw_width) / 2;
				const u32 pad_y = (image_height - draw_height) / 2;
				pixels->clear();
				pixels->resize(image_width * image_height, 0);
				*width = image_width;
				*height = image_height;
				StringUtil::StrideMemCpy(pixels->data() + pad_y * image_width + pad_x, image_width * sizeof(u32), dl->GetMapPointer(),
					dl->GetMapPitch(), draw_width * sizeof(u32), draw_height);

				g_gs_device->Recycle(rt);
				return true;
			}
		}

		g_gs_device->Recycle(rt);
	}

	*width = 0;
	*height = 0;
	pixels->clear();
	return false;
}

void DumpGSPrivRegs(const GSPrivRegSet& r, const std::string& filename)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wt");
	if (!fp)
		return;

	for (int i = 0; i < 2; i++)
	{
		if (i == 0 && !r.PMODE.EN1)
			continue;
		if (i == 1 && !r.PMODE.EN2)
			continue;

		std::fprintf(fp.get(), "DISPFB%d: { BP: 0x%05x, BW: %u, PSM: %u, DBX: %u, DBY: %u }\n",
			i,
			r.DISP[i].DISPFB.Block(),
			r.DISP[i].DISPFB.FBW,
			r.DISP[i].DISPFB.PSM,
			r.DISP[i].DISPFB.DBX,
			r.DISP[i].DISPFB.DBY);

		std::fprintf(fp.get(), "DISPLAY%d: { DX: %u, DY: %u, DW: %u, DH: %u, MAGH: %u, MAGV: %u }\n",
			i,
			r.DISP[i].DISPLAY.DX,
			r.DISP[i].DISPLAY.DY,
			r.DISP[i].DISPLAY.DW,
			r.DISP[i].DISPLAY.DH,
			r.DISP[i].DISPLAY.MAGH,
			r.DISP[i].DISPLAY.MAGV);
	}

	std::fprintf(fp.get(), "PMODE: { EN1: %u, EN2: %u, CRTMD: %u, MMOD: %u, AMOD: %u, SLBG: %u, ALP: %u }\n",
		r.PMODE.EN1,
		r.PMODE.EN2,
		r.PMODE.CRTMD,
		r.PMODE.MMOD,
		r.PMODE.AMOD,
		r.PMODE.SLBG,
		r.PMODE.ALP);

	std::fprintf(fp.get(),
		"SMODE1: { CLKSEL: %u, CMOD: %u, EX: %u, GCONT: %u, LC: %u, NVCK: %u, PCK2: %u, PEHS: %u, PEVS: %u, PHS: %u, PRST: %u, PVS: %u, RC: %u, SINT: %u, SLCK: %u, SLCK2: %u, SPML: %u, T1248: %u, VCKSEL: %u, VHP: %u, XPCK: %u }\n",
		r.SMODE1.CLKSEL,
		r.SMODE1.CMOD,
		r.SMODE1.EX,
		r.SMODE1.GCONT,
		r.SMODE1.LC,
		r.SMODE1.NVCK,
		r.SMODE1.PCK2,
		r.SMODE1.PEHS,
		r.SMODE1.PEVS,
		r.SMODE1.PHS,
		r.SMODE1.PRST,
		r.SMODE1.PVS,
		r.SMODE1.RC,
		r.SMODE1.SINT,
		r.SMODE1.SLCK,
		r.SMODE1.SLCK2,
		r.SMODE1.SPML,
		r.SMODE1.T1248,
		r.SMODE1.VCKSEL,
		r.SMODE1.VHP,
		r.SMODE1.XPCK);

	std::fprintf(fp.get(), "SMODE2: { INT: %u, FFMD: %u, DPMS: %u }\n",
		r.SMODE2.INT,
		r.SMODE2.FFMD,
		r.SMODE2.DPMS);

	std::fprintf(fp.get(), "SRFSH: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SRFSH.U32[0],
		r.SRFSH.U32[1]);

	std::fprintf(fp.get(), "SYNCH1: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH1.U32[0],
		r.SYNCH1.U32[1]);

	std::fprintf(fp.get(), "SYNCH2: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.SYNCH2.U32[0],
		r.SYNCH2.U32[1]);

	std::fprintf(fp.get(), "SYNCV: { VBP: %u, VBPE: %u, VDP: %u, VFP: %u, VFPE: %u, VS: %u }\n",
		r.SYNCV.VBP,
		r.SYNCV.VBPE,
		r.SYNCV.VDP,
		r.SYNCV.VFP,
		r.SYNCV.VFPE,
		r.SYNCV.VS);

	std::fprintf(fp.get(), "CSR: { U32_0: 0x%08x, U32_1: 0x%08x }\n",
		r.CSR.U32[0],
		r.CSR.U32[1]);

	std::fprintf(fp.get(), "BGCOLOR: { B: %u, G: %u, R: %u }\n",
		r.BGCOLOR.B,
		r.BGCOLOR.G,
		r.BGCOLOR.R);

	std::fprintf(fp.get(), "EXTBUF: { BP: 0x%05x, BW: %u, FBIN: %u, WFFMD: %u, EMODA: %u, EMODC: %u, WDX: %u, WDY: %u }\n",
		r.EXTBUF.EXBP, r.EXTBUF.EXBW, r.EXTBUF.FBIN, r.EXTBUF.WFFMD,
		r.EXTBUF.EMODA, r.EXTBUF.EMODC, r.EXTBUF.WDX, r.EXTBUF.WDY);

	std::fprintf(fp.get(), "EXTDATA: { SX: %u, SY: %u, SMPH: %u, SMPV: %u, WW: %u, WH: %u }\n",
		r.EXTDATA.SX, r.EXTDATA.SY, r.EXTDATA.SMPH, r.EXTDATA.SMPV, r.EXTDATA.WW, r.EXTDATA.WH);

	std::fprintf(fp.get(), "EXTWRITE: { EN: %u }\n", r.EXTWRITE.WRITE);
}
