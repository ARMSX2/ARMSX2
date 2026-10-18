// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Volumetric (GSVolumetric.h, ENABLE_VOLUMETRIC): the Vulkan device's multiview stereo and the depth,
// pre-HUD and right-eye images handed to the frontend with each libretro frame. GSDeviceVK.cpp only
// calls into these functions (one-line hooks), which do nothing in builds without ENABLE_VOLUMETRIC
// (GSDeviceVK.h).

#include "GS/GSPerfMon.h"
#include "GS/GSVolumetric.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKLibretro.h"

#include "libretro_vulkan.h"

#include "common/Console.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
	// The right eye's picture for each libretro backbuffer (which holds the left eye's), and the
	// ones displaced by a size change, kept until the host can no longer be reading them.
	constexpr int kBackbuffers = 3; // GSDeviceVK.cpp's kLibretroBackbuffers
	std::unique_ptr<GSTextureVK> s_bb_right[kBackbuffers];
	struct RetiredRight { std::unique_ptr<GSTextureVK> tex; u64 retired_at; };
	std::vector<RetiredRight> s_bb_right_retired;
	u64 s_right_presents = 0;
	constexpr u64 kRetireFrames = 6;
	constexpr VkClearValue s_clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};

	// The present into the frontend's images (VKLibretro::ArmPresentCopy): the targets this present
	// took, the wrappers through which it draws straight into those of its size (through an RGBA8
	// UNORM view: the GS writes sRGB-encoded values, as the frontend's sRGB images expect), and the
	// ids it reports. $PS2V_NO_DIRECT_PRESENT copies into them instead, for a measurement.
	VKLibretro::PresentCopyTarget s_present_target[2];
	std::unique_ptr<GSTextureVK> s_direct_tex[2];
	bool s_present_copy_taken = false;
	u64 s_present_copied[2] = {};
	bool DirectPresentAllowed()
	{
		static const bool allowed = [] {
			const bool a = !std::getenv("PS2V_NO_DIRECT_PRESENT");
			if (!a)
				Console.Warning("VK stereo: presenting into the frontend's images by copies ($PS2V_NO_DIRECT_PRESENT)");
			return a;
		}();
		return allowed;
	}
} // namespace

GSTextureVK* GSDeviceVK::TakePresentTargets(GSTextureVK* bb)
{
	// The last present's wrappers: its command buffer is submitted, and their views and framebuffers
	// are destroyed once the GPU is done with it. The images belong to the frontend.
	s_direct_tex[0].reset();
	s_direct_tex[1].reset();
	s_present_copied[0] = s_present_copied[1] = 0;
	s_present_target[0] = s_present_target[1] = VKLibretro::PresentCopyTarget();
	s_present_copy_taken =
		VKLibretro::Active && VKLibretro::TakePresentCopy(&s_present_target[0], &s_present_target[1]);
	if (!s_present_copy_taken || !DirectPresentAllowed())
		return bb;
	for (int eye = 0; eye < 2; eye++)
	{
		const VKLibretro::PresentCopyTarget& t = s_present_target[eye];
		if (t.image && t.drawable && t.width == static_cast<u32>(bb->GetWidth()) &&
			t.height == static_cast<u32>(bb->GetHeight()))
		{
			s_direct_tex[eye] = GSTextureVK::Adopt(t.image, GSTexture::RenderTarget, GSTexture::Format::Color,
				bb->GetWidth(), bb->GetHeight(), 1, VK_FORMAT_R8G8B8A8_UNORM);
		}
	}
	return PresentTarget(bb);
}

GSTextureVK* GSDeviceVK::PresentTarget(GSTextureVK* bb)
{
	return s_direct_tex[0] ? s_direct_tex[0].get() : bb;
}


void GSDeviceVK::EnableStereoMultiview(VkDeviceCreateInfo& device_info, VkPhysicalDeviceMultiviewFeatures& multiview_feature)
{
	// The host's settings as the GS starts.
	GSVolumetric::ApplyStereo();
	m_volumetric_shaders = GSVolumetric::GetStereo().enabled || GSVolumetric::GetStereo().multiview;
	m_stereo_layers = 1;
	m_multiview_feature = false;
	// A host that may show 3D asks for multiview even while it starts flat: the feature can only be
	// turned on here, and SetStereoLayers switches between 1 and 2 layers while the GS runs.
	if (GSVolumetric::GetStereo().multiview)
	{
		VkPhysicalDeviceMultiviewFeatures probe_mv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
		VkPhysicalDeviceFeatures2 probe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		Vulkan::AddPointerToChain(&probe, &probe_mv);
		vkGetPhysicalDeviceFeatures2(m_physical_device, &probe);
		if (probe_mv.multiview)
		{
			multiview_feature.multiview = VK_TRUE;
			Vulkan::AddPointerToChain(&device_info, &multiview_feature);
			m_multiview_feature = true;
			m_stereo_layers = GSVolumetric::GetStereo().enabled ? 2 : 1;
			Console.WriteLn("VK: multiview available; starting with %u layer(s) per render target.", m_stereo_layers);
		}
		else
		{
			Console.Warning("VK: multiview stereo requested, but the device has no multiview support.");
		}
	}
}

bool GSDeviceVK::SupportsStereoLayers(u32 layers) const
{
	return layers == 1 || (layers == 2 && m_multiview_feature);
}

// Flat (1 layer) draws as upstream does: single-view TFX passes and shaders, no layer loops, no
// multiview pass restarts. Volumetric (2 layers) draws both eyes. What bakes the layer count in is
// rebuilt here: the TFX render passes (cached per multiview-ness, so switching back reuses them),
// the TFX shaders and pipelines (recompiled as draws need them, from the shader cache once seen),
// and the convert pipelines with MULTIVIEW variants (colour clip, DATE prim-ID setup).
bool GSDeviceVK::SetStereoLayers(u32 layers)
{
	if (layers == m_stereo_layers)
		return true;
	if (!SupportsStereoLayers(layers))
		return false;

	// Workers build pipelines for the old layer count; the caller restarts them (SetGameIdentity).
	StopPipelinePrecompile();
	EndRenderPass();
	ExecuteCommandBuffer(false);
	// vkDeviceWaitIdle needs every queue to itself: the frontend submits to the shared one.
	auto* const hw_render = static_cast<retro_hw_render_interface_vulkan*>(VKLibretro::GetHWRenderInterface());
	if (hw_render)
		hw_render->lock_queue(hw_render->handle);
	WaitForGPUIdle();
	if (hw_render)
		hw_render->unlock_queue(hw_render->handle);

	{
		std::unique_lock lock(m_tfx_shader_mutex);
		for (auto& it : m_tfx_pipelines)
			vkDestroyPipeline(m_device, it.second, nullptr);
		m_tfx_pipelines.clear();
		for (auto& it : m_tfx_fragment_shaders)
			vkDestroyShaderModule(m_device, it.second, nullptr);
		m_tfx_fragment_shaders.clear();
		for (auto& it : m_tfx_vertex_shaders)
			vkDestroyShaderModule(m_device, it.second, nullptr);
		m_tfx_vertex_shaders.clear();
	}
	for (VkPipeline& pipe : m_convert)
	{
		if (pipe != VK_NULL_HANDLE)
			vkDestroyPipeline(m_device, pipe, nullptr);
		pipe = VK_NULL_HANDLE;
	}
	for (auto* arr : {&m_colclip_setup_pipelines, &m_colclip_finish_pipelines})
	{
		for (auto& row : *arr)
		{
			for (VkPipeline& pipe : row)
			{
				if (pipe != VK_NULL_HANDLE)
					vkDestroyPipeline(m_device, pipe, nullptr);
				pipe = VK_NULL_HANDLE;
			}
		}
	}
	for (auto& row : m_primid_image_setup_pipelines)
	{
		for (VkPipeline& pipe : row)
		{
			if (pipe != VK_NULL_HANDLE)
				vkDestroyPipeline(m_device, pipe, nullptr);
			pipe = VK_NULL_HANDLE;
		}
	}

	m_stereo_layers = layers;
	m_current_pipeline = VK_NULL_HANDLE;
	m_date_copy = {};
	InvalidateCachedState();
	if (!CreateRenderPasses() || !CompileConvertPipelines())
	{
		Console.Error("VK: rebuilding the render passes and pipelines for %u layer(s) failed.", layers);
		return false;
	}
	return true;
}

// A single-view utility pass writes a layered texture one layer proxy at a time, and the proxies
// keep their own load state. Hand them the texture's (a pending clear is committed to the whole
// image; an invalidated texture's layers may be discarded by their first pass, as the texture's
// would be upstream) and mark the texture written: else it stays Invalidated, and the next pass
// on it discards what the proxies drew (DONT_CARE), as do texture cache checks of its state.
void GSDeviceVK::BeginStereoLayerWrite(GSTexture* tex)
{
	tex->GetLayerProxyTexture(0); // commits a pending clear
	const GSTexture::State state =
		(tex->GetState() == GSTexture::State::Invalidated) ? GSTexture::State::Invalidated : GSTexture::State::Dirty;
	for (u32 layer = 0; layer < tex->GetArrayLayers(); layer++)
	{
		GSTexture* proxy = tex->GetLayerProxyTexture(layer);
		if (proxy != tex)
			proxy->SetState(state);
	}
	tex->SetState(GSTexture::State::Dirty);
}

void GSDeviceVK::ReleaseStereoBackbuffers()
{
	for (std::unique_ptr<GSTextureVK>& bb : s_bb_right)
		bb.reset();
	s_bb_right_retired.clear();
	s_right_presents = 0;
	// The wrappers of the frontend's images, before the device goes (not at static destruction).
	s_direct_tex[0].reset();
	s_direct_tex[1].reset();
}

GSTextureVK* GSDeviceVK::PresentRightEye(VkCommandBuffer cmdbuffer, GSTextureVK* bb, int bb_index)
{
	// GSVolumetric multiview stereo: the present drew the left eye (a layered texture is sampled as
	// its layer 0) into bb. The right eye is layer 1 of the same frame, scaled into a backbuffer of
	// its own through the same rectangles. Pause-menu repaints record no geometry: reuse the last.
	static PresentGeometry s_geometry;
	if (!m_present_geometry.src_rect.rempty())
		s_geometry = m_present_geometry;

	GSTextureVK* const current = static_cast<GSTextureVK*>(m_current);
	if (!current || current->GetArrayLayers() < 2 || s_geometry.src_rect.rempty())
		return nullptr;

	// Like the libretro backbuffers, a replaced one may still be read by the host for a few frames.
	s_right_presents++;
	std::erase_if(s_bb_right_retired, [](const RetiredRight& r) { return s_right_presents - r.retired_at > kRetireFrames; });

	if (bb_index < 0 || bb_index >= kBackbuffers)
		return nullptr;
	// Straight into the frontend's image when it armed one of this size, else a backbuffer of our own.
	GSTextureVK* right = s_direct_tex[1].get();
	const bool direct = right && right->GetWidth() == bb->GetWidth() && right->GetHeight() == bb->GetHeight();
	if (!direct)
	{
		std::unique_ptr<GSTextureVK>& own = s_bb_right[bb_index];
		if (!own || own->GetWidth() != bb->GetWidth() || own->GetHeight() != bb->GetHeight())
		{
			if (own)
				s_bb_right_retired.push_back({std::move(own), s_right_presents});
			own = GSTextureVK::Create(GSTexture::RenderTarget, GSTexture::Format::Color, bb->GetWidth(), bb->GetHeight(), 1);
			if (!own)
				return nullptr;
		}
		right = own.get();
	}

	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferDst);
	const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
	vkCmdClearColorImage(cmdbuffer, right->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&s_clear_color.color, 1, &range);

	const GSVector4i& src = s_geometry.src_rect;
	const GSVector4& dst = s_geometry.draw_rect;
	if (direct)
	{
		// The frontend's image is sRGB: a blit would encode the GS's sRGB values a second time, so
		// draw through its UNORM view instead.
		if (dst.z > dst.x && dst.w > dst.y)
		{
			GSTexture* layer = current->GetLayerProxyTexture(1);
			StretchRect(layer, GSVector4(src) / GSVector4(layer->GetSize()).xyxy(), right, dst, ShaderConvert::COPY,
				s_geometry.linear ? Filter::Biln : Filter::Nearest);
			// StretchRect leaves its pass open, as GS draws do; the overlay below begins its own.
			EndRenderPass();
		}
	}
	else
	{
		// The draw rectangle can reach outside the backbuffer (zoom, overscan); a blit cannot, so
		// clip it and move the source edges with it.
		const float cx0 = std::max(dst.x, 0.0f), cy0 = std::max(dst.y, 0.0f);
		const float cx1 = std::min(dst.z, static_cast<float>(bb->GetWidth()));
		const float cy1 = std::min(dst.w, static_cast<float>(bb->GetHeight()));
		if (cx1 > cx0 && cy1 > cy0 && dst.z > dst.x && dst.w > dst.y)
		{
			const float sx = static_cast<float>(src.z - src.x) / (dst.z - dst.x);
			const float sy = static_cast<float>(src.w - src.y) / (dst.w - dst.y);
			const GSVector4i s = GSVector4i(static_cast<int>(src.x + (cx0 - dst.x) * sx),
				static_cast<int>(src.y + (cy0 - dst.y) * sy), static_cast<int>(src.z - (dst.z - cx1) * sx),
				static_cast<int>(src.w - (dst.w - cy1) * sy)).rintersect(current->GetRect());
			const GSVector4i d(static_cast<int>(cx0), static_cast<int>(cy0), static_cast<int>(cx1), static_cast<int>(cy1));
			if (!s.rempty() && !d.rempty())
			{
				current->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::TransferSrc);
				const VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 1u}, {{s.x, s.y, 0}, {s.z, s.w, 1}},
					{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u}, {{d.x, d.y, 0}, {d.z, d.w, 1}}};
				vkCmdBlitImage(cmdbuffer, current->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, right->GetImage(),
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, s_geometry.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
			}
		}
	}

	// The ImGui overlay (OSD messages) the present drew over the left eye, drawn again over the
	// right one, at the same place: it stays flat on the screen.
	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ColorAttachment);
	if (const VkFramebuffer fb = right->GetFramebuffer(false); fb != VK_NULL_HANDLE)
	{
		const u32 w = static_cast<u32>(right->GetWidth()), h = static_cast<u32>(right->GetHeight());
		const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr,
			GetRenderPass(right->GetVkFormat(), VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
				VK_ATTACHMENT_STORE_OP_STORE),
			fb, {{0, 0}, {w, h}}, 0u, nullptr};
		vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
		const VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
		const VkRect2D scissor{{0, 0}, {w, h}};
		vkCmdSetViewport(cmdbuffer, 0, 1, &vp);
		vkCmdSetScissor(cmdbuffer, 0, 1, &scissor);
		m_scissor = GSVector4i(0, 0, static_cast<s32>(w), static_cast<s32>(h));
		m_dirty_flags |= DIRTY_UTILITY_STATE;
		DrawImGui();
		vkCmdEndRenderPass(cmdbuffer);
	}

	right->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	return right;
}

GSTextureVK* GSDeviceVK::PrepareVolumetricPresent(VkCommandBuffer cmdbuffer, GSTextureVK* bb, int bb_index)
{
	// The host samples these images while the GS goes on: hand them over ready to be read.
	const GSVolumetricFrame& volumetric = GSVolumetric::CurrentFrame();
	GSTextureVK* depth = VKLibretro::Active ? static_cast<GSTextureVK*>(volumetric.depth) : nullptr;
	GSTextureVK* prehud = depth ? static_cast<GSTextureVK*>(volumetric.prehud) : nullptr;
	if (depth)
		depth->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	if (prehud)
		prehud->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	GSTextureVK* live_depth = depth ? static_cast<GSTextureVK*>(volumetric.live_depth) : nullptr;
	if (live_depth)
		live_depth->TransitionToLayout(cmdbuffer, GSTextureVK::Layout::ShaderReadOnly);
	GSTextureVK* bb_right = (VKLibretro::Active && IsMultiview()) ? PresentRightEye(cmdbuffer, bb, bb_index) : nullptr;
	LogMultiviewRestartStats();
	RecordPresentCopies(cmdbuffer, bb, bb_right);
	return bb_right;
}

// Copies src into the target at the end of the present: after everything this frame drew, ahead of
// whatever the GS submits next. The target is left in COLOR_ATTACHMENT_OPTIMAL, as an XR swapchain
// image is handed back to the runtime.
static bool CopyToPresentTarget(VkCommandBuffer cmd, GSTextureVK* src, const VKLibretro::PresentCopyTarget& target)
{
	if (!src || !target.image || target.width != static_cast<u32>(src->GetWidth()) ||
		target.height != static_cast<u32>(src->GetHeight()))
		return false;
	src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	to_dst.srcAccessMask = 0;
	to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.image = target.image;
	to_dst.subresourceRange = range;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
		1, &to_dst);
	VkImageCopy region{};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent = {target.width, target.height, 1};
	vkCmdCopyImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	VkImageMemoryBarrier to_runtime = to_dst;
	to_runtime.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_runtime.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	to_runtime.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_runtime.newLayout =
		target.readable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 1, &to_runtime);
	src->TransitionToLayout(cmd, GSTextureVK::Layout::ShaderReadOnly);
	return true;
}

void GSDeviceVK::RecordPresentCopies(VkCommandBuffer cmdbuffer, GSTextureVK* bb, GSTextureVK* bb_right)
{
	if (!s_present_copy_taken)
		return;
	for (int eye = 0; eye < 2; eye++)
	{
		const VKLibretro::PresentCopyTarget& target = s_present_target[eye];
		GSTextureVK* const src = eye ? bb_right : bb;
		if (!target.image || !src)
			continue;
		if (src == s_direct_tex[eye].get())
		{
			// Drawn straight into it: only the layout the frontend gets it in.
			src->TransitionToLayout(cmdbuffer,
				target.readable ? GSTextureVK::Layout::ComputeReadOnly : GSTextureVK::Layout::ColorAttachment);
			s_present_copied[eye] = target.id;
		}
		else if (CopyToPresentTarget(cmdbuffer, src, target))
		{
			s_present_copied[eye] = target.id;
		}
	}
}

void GSDeviceVK::FinishPresentCopies()
{
	// The command buffer with the copies is submitted: the frontend may release the images.
	if (std::exchange(s_present_copy_taken, false))
		VKLibretro::FinishPresentCopy();
}

void GSDeviceVK::FillVolumetricFrame(VKLibretro::Frame& frame, GSTextureVK* bb, GSTextureVK* bb_right)
{
	const GSVolumetricFrame& volumetric = GSVolumetric::CurrentFrame();
	GSTextureVK* depth = VKLibretro::Active ? static_cast<GSTextureVK*>(volumetric.depth) : nullptr;
	GSTextureVK* prehud = depth ? static_cast<GSTextureVK*>(volumetric.prehud) : nullptr;
	GSTextureVK* live_depth = depth ? static_cast<GSTextureVK*>(volumetric.live_depth) : nullptr;
	frame.stereo_eye = volumetric.stereo_eye;
	frame.copied_left = std::exchange(s_present_copied[0], 0);
	frame.copied_right = std::exchange(s_present_copied[1], 0);
	frame.draw_rect[0] = volumetric.draw_rect.x, frame.draw_rect[1] = volumetric.draw_rect.y;
	frame.draw_rect[2] = volumetric.draw_rect.z, frame.draw_rect[3] = volumetric.draw_rect.w;
	if (bb_right)
	{
		frame.right_image = bb_right->GetImage();
		frame.right_view = bb_right->GetView();
	}
	if (depth)
	{
		frame.depth_image = depth->GetImage();
		frame.depth_view = depth->GetView();
		frame.depth_width = static_cast<u32>(depth->GetWidth());
		frame.depth_height = static_cast<u32>(depth->GetHeight());
		frame.depth_scale = volumetric.depth_scale;
		const GSVector4& uv = volumetric.src_uv;
		const GSVector4& rect = volumetric.draw_rect;
		frame.depth_src_uv[0] = uv.x, frame.depth_src_uv[1] = uv.y, frame.depth_src_uv[2] = uv.z, frame.depth_src_uv[3] = uv.w;
		frame.depth_draw_rect[0] = rect.x, frame.depth_draw_rect[1] = rect.y;
		frame.depth_draw_rect[2] = rect.z, frame.depth_draw_rect[3] = rect.w;
		if (prehud)
		{
			frame.prehud_image = prehud->GetImage();
			frame.prehud_view = prehud->GetView();
		}
		if (live_depth)
		{
			frame.live_depth_image = live_depth->GetImage();
			frame.live_depth_view = live_depth->GetView();
		}
	}
	if (const std::string prefix = GSVolumetric::TakePresentDumpRequest(); !prefix.empty())
	{
		bb->Save(prefix + "-left.png");
		if (bb_right)
			bb_right->Save(prefix + "-right.png");
		if (GSTexture* current = m_current)
		{
			current->GetLayerProxyTexture(0)->Save(prefix + "-layer0.png");
			if (current->GetArrayLayers() > 1)
				current->GetLayerProxyTexture(1)->Save(prefix + "-layer1.png");
		}
		Console.WriteLn("Debug: present dump %s (current %dx%d, %u layers)", prefix.c_str(),
			m_current ? m_current->GetWidth() : 0, m_current ? m_current->GetHeight() : 0,
			m_current ? m_current->GetArrayLayers() : 0u);
	}
}

void GSDeviceVK::CheckStereoRenderPass(VkRenderPass rp)
{
	// GSVolumetric multiview stereo: a per-eye target must be drawn by a multiview pass (its
	// framebuffer was made for one) and a single-layer one by a single-view pass. A mismatch is a
	// path that still needs a per-layer loop.
	if (!IsMultiview())
		return;
	const GSTextureVK* target = m_current_render_target ? m_current_render_target : m_current_depth_target;
	if (!target)
		return;
	const bool multiview_pass = m_multiview_render_passes.count(rp) != 0;
	const bool layered_target = target->GetArrayLayers() > 1;
	if (multiview_pass != layered_target)
	{
		static u32 s_reports = 0;
		if (s_reports++ < 20)
		{
			Console.Error("VK stereo: %s render pass on a %u-layer %dx%d target (format %u)",
				multiview_pass ? "multiview" : "single-view", target->GetArrayLayers(), target->GetWidth(),
				target->GetHeight(), static_cast<u32>(target->GetFormat()));
		}
	}
}

// Profiling: how many passes the frames have, how many restarts the VS and PS constants cause, and
// $PS2V_MV_RESTART to drop restarts for a measurement ("vs": only for VS constants, "none": never;
// either brings the corruption back where the driver bug strikes).
namespace
{
	struct RestartStats
	{
		u32 vs = 0, ps = 0;
		u64 passes_at_last_log = 0;
		u32 presents = 0;
	};
	RestartStats s_restart_stats;
	int RestartMode() // 2 both, 1 VS only, 0 none
	{
		static const int mode = [] {
			const char* env = std::getenv("PS2V_MV_RESTART");
			const int m = !env ? 2 : !std::strcmp(env, "vs") ? 1 : !std::strcmp(env, "none") ? 0 : 2;
			if (m != 2)
				Console.Warning("VK stereo: multiview pass restarts %s ($PS2V_MV_RESTART)", m ? "for VS constants only" : "off");
			return m;
		}();
		return mode;
	}
} // namespace

void GSDeviceVK::LogMultiviewRestartStats()
{
	// Every 600 presents (10 s at 60 fps).
	if (!IsMultiview() || ++s_restart_stats.presents < 600)
		return;
	const u64 passes = m_render_pass_serial - s_restart_stats.passes_at_last_log;
	Console.WriteLn("VK stereo: per present %.1f render passes, %.1f restarts for VS constants, %.1f for PS constants",
		static_cast<float>(passes) / 600.0f, s_restart_stats.vs / 600.0f, s_restart_stats.ps / 600.0f);
	// What else the frames cost the GPU (GSPerfMon, per frame over its last period).
	Console.WriteLn("VK stats: per frame %.0f draw calls, %.1f Mpx pass area, %.0f barriers, %.1f texture copies, "
					"%.1f uploads, %.1f readbacks, %.2f blocking GPU waits, %.1f hash cache misses, %.0f pipeline switches",
		g_perfmon.Get(GSPerfMon::DrawCalls), g_perfmon.Get(GSPerfMon::RenderPassAreaPixels) / 1e6,
		g_perfmon.Get(GSPerfMon::Barriers), g_perfmon.Get(GSPerfMon::TextureCopies), g_perfmon.Get(GSPerfMon::TextureUploads),
		g_perfmon.Get(GSPerfMon::Readbacks), g_perfmon.Get(GSPerfMon::GpuBlockingWaits),
		g_perfmon.Get(GSPerfMon::HashCacheMiss), g_perfmon.Get(GSPerfMon::PipelineSwitches));
	s_restart_stats = {};
	s_restart_stats.passes_at_last_log = m_render_pass_serial;
}

void GSDeviceVK::RestartMultiviewPassForNewConstants(bool vertex_constants)
{
	// GSVolumetric multiview stereo: inside a multiview render pass, Turnip (Adreno 750, Mesa 26.3)
	// does not reliably apply a new dynamic offset of the constant buffers to the following draws;
	// they keep an earlier draw's constants. With the vertex transform taken from the wrong draw,
	// Time Crisis II's first shooting room came out with its walls missing and its furniture drawn
	// several times over (docs/m3-multiview.md). A pass begun after the change reads the right
	// offset, so restart the open pass in place: same framebuffer, contents loaded.
	if (!IsMultiview() || !InRenderPass())
		return;
	if (RestartMode() < (vertex_constants ? 1 : 2))
		return;
	(vertex_constants ? s_restart_stats.vs : s_restart_stats.ps)++;
	const VkRenderPass pass = m_current_render_pass;
	const GSVector4i area = m_current_render_pass_area;
	EndRenderPass();
	BeginRenderPass(GetRenderPassForRestarting(pass), area);
}

// Debugging: the $PS2V_FLUSH_FILE draw range (see DebugFlushAfterDraw).
static u32 s_debug_flush_first = 0;
static u32 s_debug_flush_last = 0;

void GSDeviceVK::DebugReadFlushRange()
{
	m_debug_draw_index = 0;
	static const char* s_file = std::getenv("PS2V_FLUSH_FILE");
	if (!s_file)
		return;
	u32 first = 0, last = 0;
	if (std::FILE* fp = std::fopen(s_file, "r"))
	{
		if (std::fscanf(fp, "%u %u", &first, &last) != 2)
			first = last = 0;
		std::fclose(fp);
	}
	if (first != s_debug_flush_first || last != s_debug_flush_last)
		Console.WriteLn("Debug: flushing after draws [%u, %u)", first, last);
	s_debug_flush_first = first;
	s_debug_flush_last = last;
}

void GSDeviceVK::DebugFlushAfterDraw()
{
	// Debugging (bisecting GPU ordering bugs, see docs/m3-multiview.md): after the hardware draws of a frame whose index is
	// in [first, last), end the pass and wait for the GPU. The range comes from the file
	// $PS2V_FLUSH_FILE ("first last"), read once per frame.
	if (s_debug_flush_first < s_debug_flush_last && m_debug_draw_index >= s_debug_flush_first &&
		m_debug_draw_index < s_debug_flush_last)
	{
		EndRenderPass();
		ExecuteCommandBuffer(WaitType::Sleep);
	}
	m_debug_draw_index++;
}
