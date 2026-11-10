// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSRunsVK.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "GS/Renderers/Common/GSAlphaBitLogicOp.h"
#include "GS/Renderers/Common/GSMeasurementOverrides.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"
#include "GS/GSPerfMon.h"

#include "common/Console.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include <algorithm>
#include <cstring>

// The run's window in the table ring: a header (the run's vertex and texture constants), then one 32-byte row per
// draw. The window is reserved whole when the run opens, so the descriptor range bound for it always lies inside the
// ring, and the run's rows are committed one by one behind the header.
namespace
{
	struct RunHeader
	{
		float vertex_scale[2];
		float vertex_offset[2];
		float texture_offset[2];
		float st_scale[2];
		u32 max_depth;
		u32 pad[3];
	};
	static_assert(sizeof(RunHeader) == 48);

	constexpr u32 HEADER_RANGE = 64;
	constexpr u32 TABLE_SKIP = 256; // the rows start this far into the window (any device's offset alignment)
	constexpr u32 RUN_WINDOW = 256 * 1024;
	constexpr u32 MAX_DRAWS = (RUN_WINDOW - TABLE_SKIP) / 32;
	constexpr u32 RING_SIZE = 16 * 1024 * 1024;
	constexpr u32 MAX_TEXTURES = 1024;
	constexpr u32 POOL_SETS = 256;
	constexpr u32 POOL_DESCRIPTORS = 8192;
	u32 Variant(bool dual, bool rta, bool zfloor) { return (dual ? 1u : 0u) | (rta ? 2u : 0u) | (zfloor ? 4u : 0u); }
	u64 PipelineKey(u8 dss, u32 variant, u32 spec) { return static_cast<u64>(dss) | (static_cast<u64>(variant) << 8) | (static_cast<u64>(spec) << 16); }
} // namespace

std::unique_ptr<GSRunsVK> GSRunsVK::Create(GSDeviceVK* dev)
{
	if (g_gs_measurement_overrides.runs_off)
	{
		Console.WriteLn("VK: run machine off (-runs off)");
		return {};
	}
	// Device capability decides, not the vendor: the per-draw textures need non-uniform indexing into a runtime
	// array, and the run's blend ways need the second fragment output.
	if (!dev->m_optional_extensions.vk_ext_descriptor_indexing || !dev->m_features.dual_source_blend)
	{
		Console.WriteLn("VK: run machine off: %s", !dev->m_optional_extensions.vk_ext_descriptor_indexing ?
			"no descriptor indexing" : "no dual-source blending");
		return {};
	}
	std::unique_ptr<GSRunsVK> runs(new GSRunsVK(dev));
	if (!runs->Init())
	{
		Console.Error("VK: run machine failed to initialise; drawing every draw classically");
		return {};
	}
	return runs;
}

GSRunsVK::GSRunsVK(GSDeviceVK* dev)
	: m_dev(dev)
{
}

GSRunsVK::~GSRunsVK()
{
	const VkDevice device = m_dev->m_device;
	// The device stopped its compile queue first: a job still queued never runs, one that ran left its pipeline.
	for (auto& [key, job] : m_pending)
	{
		if (!m_dev->m_compile_queue.Cancel(job.ticket) && m_dev->m_compile_queue.IsDone(job.ticket) && job.result->pipeline)
			vkDestroyPipeline(device, job.result->pipeline, nullptr);
	}
	if (m_log)
	{
		// The refusal census: each refusal's detail and how many draws it kept off the machine, largest first.
		std::vector<std::pair<std::pair<u32, u64>, u64>> rs(m_refusals.begin(), m_refusals.end());
		std::sort(rs.begin(), rs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		for (const auto& [k, n] : rs)
			std::fprintf(m_log, "R %u %016llx %llu\n", k.first, static_cast<unsigned long long>(k.second), static_cast<unsigned long long>(n));
		std::fclose(m_log);
	}
	for (auto& it : m_pipelines)
		vkDestroyPipeline(device, it.second, nullptr);
	for (auto& it : m_fs)
		vkDestroyShaderModule(device, it.second, nullptr);
	if (m_vs)
		vkDestroyShaderModule(device, m_vs, nullptr);
	for (FramePools& fp : m_frame_pools)
	{
		for (VkDescriptorPool pool : fp.pools)
			vkDestroyDescriptorPool(device, pool, nullptr);
	}
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(device, m_pipeline_layout, nullptr);
	if (m_set0_layout)
		vkDestroyDescriptorSetLayout(device, m_set0_layout, nullptr);
	if (m_set1_layout)
		vkDestroyDescriptorSetLayout(device, m_set1_layout, nullptr);
	m_index_stream.Destroy(false);
	m_table_stream.Destroy(false);
}

bool GSRunsVK::Init()
{
	const VkDevice device = m_dev->m_device;
	const VkPhysicalDeviceLimits& limits = m_dev->m_device_properties.limits;

	std::optional<std::string> src = GSDevice::ReadShaderSource("shaders/vulkan/tfx_runs.glsl");
	if (!src.has_value())
	{
		Console.Error("VK: cannot read shaders/vulkan/tfx_runs.glsl");
		return false;
	}
	m_source = std::move(*src);

	// The run's texture array is bounded by what one stage may sample from one set.
	m_max_textures = std::min({MAX_TEXTURES, limits.maxPerStageDescriptorSamplers, limits.maxPerStageDescriptorSampledImages,
		limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages, limits.maxPerStageResources - 8});
	if (m_max_textures < 8)
	{
		Console.WriteLn("VK: run machine off: %u samplers per stage", m_max_textures);
		return false;
	}

	if (!m_index_stream.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, RING_SIZE, "runs-index") ||
		!m_table_stream.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, RING_SIZE, "runs-table"))
	{
		Console.Error("VK: run machine rings failed");
		return false;
	}

	Vulkan::DescriptorSetLayoutBuilder dslb;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_set0_layout = dslb.Create(device)) == VK_NULL_HANDLE)
		return false;

	{
		const VkDescriptorBindingFlagsEXT flags =
			VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;
		const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bfci = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT, nullptr, 1, &flags};
		const VkDescriptorSetLayoutBinding binding = {
			0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_max_textures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		const VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfci, 0, 1, &binding};
		if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_set1_layout) != VK_SUCCESS)
			return false;
	}

	Vulkan::PipelineLayoutBuilder plb;
	plb.AddDescriptorSet(m_set0_layout);
	plb.AddDescriptorSet(m_set1_layout);
	if ((m_pipeline_layout = plb.Create(device)) == VK_NULL_HANDLE)
		return false;
	if ((m_set0 = m_dev->AllocatePersistentDescriptorSet(m_set0_layout)) == VK_NULL_HANDLE)
		return false;
	Vulkan::DescriptorSetUpdateBuilder dsub;
	dsub.AddBufferDescriptorWrite(m_set0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, m_table_stream.GetBuffer(), 0, HEADER_RANGE);
	dsub.AddBufferDescriptorWrite(m_set0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, m_table_stream.GetBuffer(), 0,
		RUN_WINDOW - TABLE_SKIP);
	dsub.Update(device);

	m_max_draws = g_gs_measurement_overrides.runs_max_draws ? std::min(g_gs_measurement_overrides.runs_max_draws, MAX_DRAWS) : MAX_DRAWS;
	m_frame_pools.resize(GSDeviceVK::NUM_COMMAND_BUFFERS);
	m_textures.reserve(m_max_textures);
	m_image_infos.reserve(m_max_textures);

	m_vs = g_vulkan_shader_cache->GetVertexShader(m_dev->RunsShaderSource(true, std::string(), m_source));
	if (!m_vs)
	{
		Console.Error("VK: the run vertex program did not compile");
		return false;
	}

	if (!g_gs_measurement_overrides.runs_log.empty())
	{
		m_log = std::fopen(g_gs_measurement_overrides.runs_log.c_str(), "w");
		if (m_log)
		{
			static char s_buf[1 << 16];
			std::setvbuf(m_log, s_buf, _IOFBF, sizeof(s_buf));
			std::fprintf(m_log, "# runs split=%s\n# K pass tfx merged runs runs_multi longest | road topo ps blend tex const not_in_pass compiling | "
			                    "end_state end_target end_split end_pass end_other end_ring end_unmergeable end_textures end_compiling\n"
			                    "# S pass spec variant draws textures\n# C spec variant dss ms [sync] (a run program's pipeline, as the GS "
			                    "thread takes it)\n",
				g_gs_measurement_overrides.runs_split_dear ? "dear" : "none");
		}
	}

	Console.WriteLn("VK: run machine on, up to %u textures a run", m_max_textures);
	return true;
}

void GSRunsVK::Classify(const GSHWDrawConfig& config)
{
	Draw& d = m_draw;
	d = {};
	m_pass.tfx++;
	GSRunsPolicy::TextureFacts tex;
	if (config.tex)
	{
		tex.present = true;
		tex.is_target = config.tex->IsRenderTargetOrDepthStencil();
		tex.rgba8 = config.tex->GetFormat() == GSTexture::Format::Color;
	}
	const GSRunsPolicy::Draw pd = GSRunsPolicy::Classify(config, tex, m_dev->GetColorClipTexture() != nullptr);
	if (pd.why != GSRunsPolicy::Refusal::None)
	{
		static constexpr Why why[] = {WHY_ROAD, WHY_ROAD, WHY_TOPO, WHY_PS, WHY_BLEND, WHY_TEX, WHY_CONST};
		m_pass.why[why[static_cast<u32>(pd.why)]]++;
		if (m_log)
			NoteRefusal(config, pd.why);
		Flush(END_UNMERGEABLE);
		return;
	}

	const u64 seq = m_draw_seq++;
	if (g_gs_measurement_overrides.runs_draw_hi != 0)
	{
		if (seq < g_gs_measurement_overrides.runs_draw_lo || seq >= g_gs_measurement_overrides.runs_draw_hi)
		{
			m_pass.why[WHY_ROAD]++;
			Flush(END_UNMERGEABLE);
			return;
		}
		if (m_log)
		{
			std::fprintf(m_log, "D %llu flags=%x way=%u ps=%016llx:%016llx vs=%02x dss=%02x bs=%016llx cms=%02x tex=%dx%d/%d sampler=%llx\n",
				static_cast<unsigned long long>(seq), pd.flags, pd.way, static_cast<unsigned long long>(config.ps.key_hi),
				static_cast<unsigned long long>(config.ps.key_lo), config.vs.key, config.depth.key,
				static_cast<unsigned long long>(config.blend.key), config.colormask.key, config.tex ? config.tex->GetWidth() : 0,
				config.tex ? config.tex->GetHeight() : 0, config.tex ? config.tex->GetMipmapLevels() : 0,
				static_cast<unsigned long long>(config.sampler.key));
		}
	}
	d.cand = true;
	d.flags = pd.flags;
	d.way = pd.way;

	if (m_run.open && !Matches(config))
		Flush(EndReason(config));
}

void GSRunsVK::NoteRefusal(const GSHWDrawConfig& config, GSRunsPolicy::Refusal why)
{
	// Detail per refusal kind (-runs-log only): road, the conditions that hold as bits; topology, the topology, expansion
	// and VS bits; PS, the selector bits outside the run program's set (lo word, then hi word as a second record);
	// blend, the blend state's key with the colour mask and the PS blend fields; texture, its format; constant, 0.
	u64 detail = 0;
	const GSHWDrawConfig::PSSelector& ps = config.ps;
	switch (why)
	{
		case GSRunsPolicy::Refusal::Road:
		{
			const bool c[] = {config.stencil_counter.enable, config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off,
				config.require_one_barrier, config.require_full_barrier, config.blend_multi_pass.enable, config.alpha_second_pass.enable,
				config.colormask.logic_op != GSAlphaBitLogicOp::Off, ps.colclip_hw || m_dev->GetColorClipTexture() != nullptr,
				config.road.clone_rt, config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE, config.line_expand,
				ps.HasColorROV() || ps.HasDepthROV(), config.road.rt_loop, config.road.depth_loop, config.road.depth_read, config.raster_order,
				config.date_copy != GSAlphaBitLogicOp::NoDateCopy, config.colclip_native_scale > 1.0f, !config.rt, !config.ds};
			for (u32 i = 0; i < std::size(c); i++)
				detail |= static_cast<u64>(c[i]) << i;
			break;
		}
		case GSRunsPolicy::Refusal::Topo:
			detail = static_cast<u64>(config.topology) | (static_cast<u64>(config.vs.key) << 8) | (static_cast<u64>(config.indices_per_prim) << 16);
			break;
		case GSRunsPolicy::Refusal::PS:
		{
			const GSHWDrawConfig::PSSelector& allowed = GSRunsPolicy::AllowedPS();
			detail = ps.key_lo & ~allowed.key_lo;
			if (ps.key_hi & ~allowed.key_hi)
				m_refusals[{static_cast<u32>(why) | 0x100u, ps.key_hi & ~allowed.key_hi}]++;
			if (ps.wms > 1 || ps.wmt > 1)
				m_refusals[{static_cast<u32>(why) | 0x200u, (ps.wms << 2) | ps.wmt}]++;
			if (static_cast<u32>(ps.atst) > 4 || config.pal)
				m_refusals[{static_cast<u32>(why) | 0x300u, static_cast<u64>(ps.atst) | (config.pal ? 0x100 : 0)}]++;
			break;
		}
		case GSRunsPolicy::Refusal::Blend:
			detail = (config.blend.key & 0xFFFFFFFFull) | (static_cast<u64>(config.colormask.wrgba) << 32) |
			         (static_cast<u64>(ps.blend_a | (ps.blend_b << 2) | (ps.blend_c << 4) | (ps.blend_d << 6) | (ps.blend_mix << 8) |
			                           (ps.af_in_src1 << 10) | (ps.no_color1 << 11)) << 36);
			break;
		case GSRunsPolicy::Refusal::Tex:
			detail = config.tex ? (static_cast<u64>(config.tex->GetFormat()) | (config.tex->IsRenderTargetOrDepthStencil() ? 0x100 : 0)) : 0xFFFF;
			break;
		default:
			break;
	}
	m_refusals[{static_cast<u32>(why), detail}]++;
}

bool GSRunsVK::Matches(const GSHWDrawConfig& config) const
{
	const Run& r = m_run;
	const GSHWDrawConfig::VSConstantBuffer& a = config.cb_vs;
	const GSHWDrawConfig::VSConstantBuffer& b = r.cb_vs;
	return config.rt == r.rt && config.ds == r.ds && config.depth.key == r.dss && (config.vs.key & ~0x06u) == r.vs &&
	       config.scissor.eq(r.scissor) && r.ndraws < m_max_draws && std::memcmp(&a.vertex_scale, &b.vertex_scale, sizeof(GSVector2)) == 0 &&
	       std::memcmp(&a.vertex_offset, &b.vertex_offset, sizeof(GSVector2)) == 0 &&
	       std::memcmp(&a.texture_offset, &b.texture_offset, sizeof(GSVector2)) == 0 && a.max_depth == b.max_depth &&
	       std::memcmp(&config.cb_ps.STScale, &r.st_scale, sizeof(GSVector2)) == 0 && config.ps.rta_correction == r.rta &&
	       config.ps.zfloor == r.zfloor && GSRunsPolicy::StrictKey(m_draw.flags, r.rta) == r.strict &&
	       (!g_gs_measurement_overrides.runs_split_dear || GSRunsPolicy::Dear(m_draw.flags) == r.dear);
}

GSRunsVK::Why GSRunsVK::EndReason(const GSHWDrawConfig& config) const
{
	if (config.rt != m_run.rt || config.ds != m_run.ds)
		return END_TARGET;
	if (GSRunsPolicy::StrictKey(m_draw.flags, m_run.rta) != m_run.strict)
		return END_SPLIT;
	if (g_gs_measurement_overrides.runs_split_dear && GSRunsPolicy::Dear(m_draw.flags) != m_run.dear)
		return END_SPLIT;
	return END_STATE;
}

bool GSRunsVK::Continue(const GSHWDrawConfig& config)
{
	GSDeviceVK* dev = m_dev;
	if (!m_run.open || !dev->InRenderPass() || dev->m_current_render_target != config.rt || dev->m_current_depth_target != config.ds ||
		dev->m_current_framebuffer_feedback_loop != GSDeviceVK::FeedbackLoopFlag_None)
		return false;
	if (config.ps.tfx != 4)
	{
		// PSSetShaderResource would commit a clear or transition the texture, both outside the pass.
		const GSTextureVK* tex = static_cast<const GSTextureVK*>(config.tex);
		if (tex->GetState() != GSTexture::State::Dirty || tex->GetLayout() != GSTextureVK::Layout::ShaderReadOnly)
			return false;
	}
	if (config.sampler.key != m_sampler_key)
	{
		m_sampler_key = config.sampler.key;
		m_sampler = dev->GetSampler(config.sampler);
	}
	return Append(config, 0, false, m_sampler);
}

bool GSRunsVK::Append(const GSHWDrawConfig& config, u32 pipeline_feedback_flags, bool pipeline_colclip, VkSampler sampler)
{
	Draw& d = m_draw;
	d.cand = false;
	GSDeviceVK* dev = m_dev;
	if (!dev->InRenderPass() || dev->m_current_render_target != config.rt || dev->m_current_depth_target != config.ds ||
		dev->m_current_framebuffer_feedback_loop != GSDeviceVK::FeedbackLoopFlag_None || pipeline_feedback_flags != 0 || pipeline_colclip)
	{
		m_pass.why[WHY_NOT_IN_PASS]++;
		Flush(END_UNMERGEABLE);
		return false;
	}
	if (m_run.open && !Matches(config))
		Flush(EndReason(config));

	// The run's program with this draw in it must be built. If it is not, it is asked for, the run ends, and the draw
	// takes the classic path. Asked again below if the run ends for another reason before the draw is in it, since the
	// draw then opens a run of its own.
	const auto ready = [&]() {
		const bool open = m_run.open;
		GSRunsPolicy::StateSet states = open ? m_run.states : GSRunsPolicy::StateSet();
		states.Note(d.flags);
		const u32 ways = (open ? m_run.ways : 0u) | (1u << d.way);
		const bool rta = config.ps.rta_correction;
		const u32 variant = Variant((ways & ~(1u << 2)) != 0, rta, config.ps.zfloor);
		if (ProgramReady(config.depth.key, variant, SpecKeyOf(states, GSRunsPolicy::StrictKey(d.flags, rta))))
			return true;
		m_pass.why[WHY_COMPILING]++;
		Flush(END_COMPILING);
		return false;
	};
	if (!ready())
		return false;

	const VkPhysicalDeviceLimits& limits = dev->m_device_properties.limits;
	const u32 align = std::max<u32>(64, static_cast<u32>(std::max(limits.minUniformBufferOffsetAlignment, limits.minStorageBufferOffsetAlignment)));
	const u32 nidx = config.nindices, nvtx = config.nverts;
	VKStreamBuffer& vring = dev->m_vertex_stream_buffer;
	for (u32 attempt = 0;; attempt++)
	{
		if (attempt == 4)
			pxFailRel("VK: the run machine could not place a draw");
		const u64 fence = dev->GetCurrentFenceCounter();
		Run& r = m_run;
		if (!r.open)
		{
			if (attempt != 0 && !ready())
				return false;
			if (!m_table_stream.ReserveMemory(RUN_WINDOW, align))
			{
				dev->ExecuteCommandBufferAndRestartRenderPass(false, "runs table ring");
				continue;
			}
			r = {};
			r.header_off = m_table_stream.GetCurrentOffset();
			r.table_off = r.header_off + TABLE_SKIP;
			RunHeader hdr = {};
			std::memcpy(hdr.vertex_scale, &config.cb_vs.vertex_scale, 8);
			std::memcpy(hdr.vertex_offset, &config.cb_vs.vertex_offset, 8);
			std::memcpy(hdr.texture_offset, &config.cb_vs.texture_offset, 8);
			std::memcpy(hdr.st_scale, &config.cb_ps.STScale, 8);
			hdr.max_depth = config.cb_vs.max_depth;
			std::memcpy(m_table_stream.GetCurrentHostPointer(), &hdr, sizeof(hdr));
			m_table_stream.CommitMemory(TABLE_SKIP);
			r.open = true;
			r.rt = config.rt;
			r.ds = config.ds;
			r.dss = config.depth.key;
			r.vs = config.vs.key & ~0x06u;
			r.scissor = config.scissor;
			r.viewport = dev->m_viewport;
			r.cb_vs = config.cb_vs;
			r.st_scale = config.cb_ps.STScale;
			r.rta = config.ps.rta_correction;
			r.zfloor = config.ps.zfloor;
			r.dear = GSRunsPolicy::Dear(d.flags);
			r.strict = GSRunsPolicy::StrictKey(d.flags, r.rta);
			r.idx_first = ~0u;
			m_textures.clear();
		}
		// The row: contiguous with the run's other rows.
		if (!m_table_stream.ReserveMemory(32, 16) || m_table_stream.GetCurrentOffset() != r.table_off + 32 * r.ndraws)
		{
			Flush(END_RING);
			continue;
		}
		u8* row = m_table_stream.GetCurrentHostPointer();
		// The indices: contiguous with the run's.
		if (!m_index_stream.ReserveMemory(nidx * 4, 4))
		{
			dev->ExecuteCommandBufferAndRestartRenderPass(false, "runs index ring");
			continue;
		}
		const u32 ioff = m_index_stream.GetCurrentOffset() / 4;
		if (r.ndraws != 0 && ioff != r.idx_first + r.idx_count)
		{
			Flush(END_RING);
			continue;
		}
		u32* idx = reinterpret_cast<u32*>(m_index_stream.GetCurrentHostPointer());
		// The vertices: the vertex ring as IASetVertexBuffer reserves it.
		const u32 vsize = nvtx * sizeof(GSVertex);
		if (!vring.ReserveMemory(vsize, sizeof(GSVertex) * GetVertexAlignment(GSHWDrawConfig::VSExpand::None)))
		{
			dev->ExecuteCommandBufferAndRestartRenderPass(false, "runs vertex ring");
			continue;
		}
		if (r.ndraws != 0 && vring.GetBuffer() != r.vbuf)
		{
			Flush(END_RING); // the vertex ring grew under the run
			continue;
		}
		if (fence != dev->GetCurrentFenceCounter())
			continue;

		// The draw's texture: its slot in the run's array.
		u32 slot = 0;
		if (config.ps.tfx != 4)
		{
			const GSTextureVK* tex = static_cast<const GSTextureVK*>(config.tex);
			const Texture t = {tex->GetView(), sampler, tex->GetVkLayout()};
			const auto same = [&t](const Texture& o) { return o.view == t.view && o.sampler == t.sampler; };
			if (r.last_slot < m_textures.size() && same(m_textures[r.last_slot]))
				slot = r.last_slot;
			else
			{
				const auto it = std::find_if(m_textures.begin(), m_textures.end(), same);
				if (it != m_textures.end())
					slot = static_cast<u32>(it - m_textures.begin());
				else if (m_textures.size() == m_max_textures)
				{
					Flush(END_TEXTURES);
					continue;
				}
				else
				{
					slot = static_cast<u32>(m_textures.size());
					m_textures.push_back(t);
				}
			}
			r.last_slot = slot;
		}

		const u32 k = r.ndraws;
		const u32 vstart = vring.GetCurrentOffset() / sizeof(GSVertex);
		GSVertex* dst = reinterpret_cast<GSVertex*>(vring.GetCurrentHostPointer());
		for (u32 i = 0; i < nvtx; i++)
		{
			GSVertex v = config.verts[i];
			v.FOG = (v.FOG & 0xFFu) | (k << 8);
			dst[i] = v;
		}
		vring.CommitMemory(vsize);
		for (u32 i = 0; i < nidx; i++)
			idx[i] = vstart + config.indices[i];
		m_index_stream.CommitMemory(nidx * 4);

		u32 s[8];
		s[0] = d.flags;
		s[1] = slot;
		const GSVector4 fog = config.cb_ps.FogColor_AREF;
		s[2] = (config.ps.fog ? (static_cast<u32>(fog.x) | (static_cast<u32>(fog.y) << 8) | (static_cast<u32>(fog.z) << 16)) : 0u) |
		       ((d.way == 1) ? (static_cast<u32>(config.cb_ps.TA_MaxDepth_Af.w * 128.0f) << 24) : 0u);
		std::memcpy(&s[3], &fog.w, 4);
		std::memcpy(&s[4], &config.cb_ps.LODParams, 16);
		std::memcpy(row, s, 32);
		m_table_stream.CommitMemory(32);

		if (r.ndraws == 0)
		{
			r.idx_first = ioff;
			r.idx_count = 0;
			r.vbuf = vring.GetBuffer();
		}
		r.ndraws++;
		r.idx_count += nidx;
		r.ways |= 1u << d.way;
		r.states.Note(d.flags); // which states the run holds one value of: those become constants of its program
		if (config.tex)
			static_cast<GSTextureVK*>(config.tex)->SetUseFenceCounter(fence);
		return true;
	}
}

u32 GSRunsVK::SpecKey() const
{
	return SpecKeyOf(m_run.states, m_run.strict);
}

u32 GSRunsVK::SpecKeyOf(const GSRunsPolicy::StateSet& states, u32 strict)
{
	// A strict run keeps its colour states constants even under the read mask (GSRunsPolicy::Passthrough).
	const u32 colour = (1u << GSRunsPolicy::STATE_TFX) | (1u << GSRunsPolicy::STATE_TCC) | (1u << GSRunsPolicy::STATE_FOG) |
	                   (1u << GSRunsPolicy::STATE_IIP) | (1u << GSRunsPolicy::STATE_WAY);
	return states.Key(g_gs_measurement_overrides.runs_read_mask & (strict ? ~colour : ~0u));
}

VkShaderModule GSRunsVK::GetFragmentShader(u32 variant, u32 spec)
{
	const u64 key = spec | (static_cast<u64>(variant) << 32);
	{
		std::unique_lock lock(m_fs_mutex);
		const auto it = m_fs.find(key);
		if (it != m_fs.end())
			return it->second;
	}
	std::string defines;
	defines += "#define RUN_DUAL " + std::to_string(variant & 1) + "\n";
	defines += "#define RUN_RTA_CORRECTION " + std::to_string((variant >> 1) & 1) + "\n";
	defines += "#define RUN_ZFLOOR " + std::to_string(variant >> 2) + "\n";
	for (u32 i = 0; i < GSRunsPolicy::NUM_STATES; i++)
	{
		const u32 v = (spec >> (4 * i)) & 0xF;
		if (v != 0xF)
			defines += "#define RUN_C_" + std::string(GSRunsPolicy::STATE_NAME[i]) + " " + std::to_string(v) + "\n";
	}
	VkShaderModule mod = g_vulkan_shader_cache->GetFragmentShader(m_dev->RunsShaderSource(false, defines, m_source));
	if (!mod)
		pxFailRel("VK: a run program did not compile");
	std::unique_lock lock(m_fs_mutex);
	const auto [it, added] = m_fs.emplace(key, mod);
	if (!added)
	{
		vkDestroyShaderModule(m_dev->m_device, mod, nullptr); // built twice at once, by two compile-queue jobs
		mod = it->second;
	}
	return mod;
}

VkPipeline GSRunsVK::CreatePipeline(u8 dss_key, u32 variant, u32 spec)
{
	const bool dual = (variant & 1) != 0;
	const GSHWDrawConfig::DepthStencilSelector dss(dss_key);
	Vulkan::GraphicsPipelineBuilder gpb;
	if (m_dev->m_features.provoking_vertex_last)
		gpb.SetProvokingVertex(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
	gpb.SetPipelineLayout(m_pipeline_layout);
	gpb.SetRenderPass(m_dev->GetTFXRenderPass(true, true, false, false, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
						  VK_ATTACHMENT_LOAD_OP_LOAD),
		0);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	gpb.SetDynamicViewportAndScissorState();
	// The classic pipelines' dynamic state: a pipeline with these static would overwrite the values the classic path
	// set, and it re-emits them only when they change.
	gpb.AddDynamicState(VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	gpb.AddDynamicState(VK_DYNAMIC_STATE_LINE_WIDTH);
	gpb.SetVertexShader(m_vs);
	gpb.SetFragmentShader(GetFragmentShader(variant, spec));
	gpb.AddVertexBuffer(0, sizeof(GSVertex));
	gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32_SFLOAT, 0); // ST
	gpb.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UINT, 8); // RGBA
	gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32_SFLOAT, 12); // Q
	gpb.AddVertexAttribute(3, 0, VK_FORMAT_R16G16_UINT, 16); // XY
	gpb.AddVertexAttribute(4, 0, VK_FORMAT_R32_UINT, 20); // Z
	gpb.AddVertexAttribute(5, 0, VK_FORMAT_R16G16_UINT, 24); // UV
	gpb.AddVertexAttribute(6, 0, VK_FORMAT_R8G8B8A8_UNORM, 28); // FOG
	gpb.AddVertexAttribute(7, 0, VK_FORMAT_R32_UINT, 28); // FOG word: the draw's row in its high bytes
	static constexpr VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	gpb.SetDepthState((dss.ztst != ZTST_ALWAYS || dss.zwe), dss.zwe, ztst[dss.ztst]);
	if (dual)
	{
		gpb.SetBlendAttachment(0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF);
	}
	else
	{
		gpb.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF);
	}
	VkPipeline pipeline = gpb.Create(m_dev->m_device, g_vulkan_shader_cache->GetPipelineCache(false));
	g_vulkan_shader_cache->GetPipelineCache(true);
	if (!pipeline)
		pxFailRel("VK: run pipeline creation failed");
	return pipeline;
}

bool GSRunsVK::ProgramReady(u8 dss, u32 variant, u32 spec)
{
	const u64 key = PipelineKey(dss, variant, spec);
	if (m_pipelines.find(key) != m_pipelines.end())
		return true;

	GSCompileQueue& queue = m_dev->m_compile_queue;
	if (g_gs_measurement_overrides.runs_sync_compile || !queue.IsRunning())
	{
		// Built here and now, on the GS thread (-runs-sync-compile prices this).
		const Common::Timer timer;
		m_pipelines.emplace(key, CreatePipeline(dss, variant, spec));
		m_compiles++;
		if (m_log)
			std::fprintf(m_log, "C %08x %u %u %.2f sync\n", spec, variant, dss, timer.GetTimeMilliseconds());
		return true;
	}

	auto it = m_pending.find(key);
	if (it == m_pending.end())
	{
		// Built on the device's compile queue, ahead of need; the draws that need it meanwhile are drawn classically.
		Pending job;
		job.result = std::make_shared<Result>();
		job.ticket = queue.Submit(GSCompileQueue::Priority::Ahead, [this, dss, variant, spec, result = job.result]() {
			const Common::Timer timer;
			result->pipeline = CreatePipeline(dss, variant, spec);
			result->ms = static_cast<float>(timer.GetTimeMilliseconds());
		});
		m_pending.emplace(key, std::move(job));
		return false;
	}
	if (!queue.IsDone(it->second.ticket))
		return false;
	m_pipelines.emplace(key, it->second.result->pipeline);
	m_compiles++;
	if (m_log)
		std::fprintf(m_log, "C %08x %u %u %.2f\n", spec, variant, dss, it->second.result->ms);
	m_pending.erase(it);
	return true;
}

VkPipeline GSRunsVK::GetPipeline(bool dual)
{
	const Run& r = m_run;
	// Append let the run grow only while its program was built.
	const auto it = m_pipelines.find(PipelineKey(r.dss, Variant(dual, r.rta, r.zfloor), SpecKey()));
	if (it == m_pipelines.end())
		pxFailRel("VK: a run grew past the program built for it");
	return it->second;
}

VkDescriptorSet GSRunsVK::WriteTextureSet()
{
	GSDeviceVK* dev = m_dev;
	const VkDevice device = dev->m_device;
	// The pools of this command buffer slot were last used by its previous submission, which has completed by the
	// time the slot records again.
	FramePools& fp = m_frame_pools[dev->m_current_frame];
	const u64 fence = dev->GetCurrentFenceCounter();
	if (m_pool_fence != fence)
	{
		m_pool_fence = fence;
		for (VkDescriptorPool pool : fp.pools)
			vkResetDescriptorPool(device, pool, 0);
		fp.cursor = 0;
	}

	// A run of untextured draws still binds an array: one null texture.
	if (m_textures.empty())
		m_textures.push_back({dev->m_null_texture->GetView(), dev->m_point_sampler, dev->m_null_texture->GetVkLayout()});
	const u32 count = static_cast<u32>(m_textures.size());

	VkDescriptorSet set = VK_NULL_HANDLE;
	const VkDescriptorSetVariableDescriptorCountAllocateInfoEXT vci = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT, nullptr, 1, &count};
	for (;;)
	{
		if (fp.cursor == fp.pools.size())
		{
			const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(POOL_DESCRIPTORS, m_max_textures)};
			const VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, POOL_SETS, 1, &size};
			VkDescriptorPool pool;
			if (vkCreateDescriptorPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
				pxFailRel("VK: run descriptor pool creation failed");
			fp.pools.push_back(pool);
		}
		const VkDescriptorSetAllocateInfo ai = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vci, fp.pools[fp.cursor], 1, &m_set1_layout};
		if (vkAllocateDescriptorSets(device, &ai, &set) == VK_SUCCESS)
			break;
		fp.cursor++;
	}

	m_image_infos.resize(count);
	for (u32 i = 0; i < count; i++)
		m_image_infos[i] = {m_textures[i].sampler, m_textures[i].view, m_textures[i].layout};
	const VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, count,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_image_infos.data(), nullptr, nullptr};
	vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
	return set;
}

void GSRunsVK::Flush(Why why)
{
	Run& r = m_run;
	if (!r.open || m_flushing)
		return;
	r.open = false;
	if (r.ndraws == 0)
		return;
	m_flushing = true;
	m_pass.why[why]++;
	m_pass.runs++;
	m_pass.merged += r.ndraws;
	m_pass.runs_multi += (r.ndraws > 1) ? 1 : 0;
	m_pass.longest = std::max(m_pass.longest, r.ndraws);

	GSDeviceVK* dev = m_dev;
	const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
	// Way 2 alone needs no blending; any other way needs the dual-source blend.
	const bool dual = (r.ways & ~(1u << 2)) != 0;
	const u32 variant = Variant(dual, r.rta, r.zfloor);
	const VkPipeline pipeline = GetPipeline(dual);
	const VkDescriptorSet set1 = WriteTextureSet();
	if (m_log)
	{
		std::fprintf(m_log, "S %llu %08x %u %u %zu\n", static_cast<unsigned long long>(dev->m_render_pass_serial), SpecKey(),
			variant, r.ndraws, m_textures.size());
	}
	g_perfmon.Put(GSPerfMon::PipelineSwitches, 1);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const u32 offsets[2] = {r.header_off, r.table_off};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &m_set0, 2, offsets);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 1, 1, &set1, 0, nullptr);
	VkRect2D scissor = {{r.scissor.x, r.scissor.y}, {static_cast<u32>(r.scissor.width()), static_cast<u32>(r.scissor.height())}};
	if (g_gs_measurement_overrides.runs_nofill)
		scissor.extent = {1, 1};
	vkCmdSetViewport(cmd, 0, 1, &r.viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	const bool other_vbuf = r.vbuf != dev->m_vertex_stream_buffer.GetBuffer();
	if (other_vbuf)
	{
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &r.vbuf, &zero);
	}
	vkCmdBindIndexBuffer(cmd, m_index_stream.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDrawIndexed(cmd, r.idx_count, 1, r.idx_first, 0, 0);
	if (other_vbuf)
		dev->SetInitialState(cmd);

	// The classic path rebinds all of its state for its next draw.
	dev->m_current_pipeline = VK_NULL_HANDLE;
	dev->m_current_pipeline_layout = GSDeviceVK::PipelineLayout::Undefined;
	dev->m_dirty_flags |= GSDeviceVK::DIRTY_FLAG_PIPELINE | GSDeviceVK::DIRTY_FLAG_TFX_UBO | GSDeviceVK::DIRTY_FLAG_TFX_TEXTURES |
	                      GSDeviceVK::DIRTY_FLAG_VS_PUSH_CONSTANTS | GSDeviceVK::DIRTY_FLAG_VIEWPORT | GSDeviceVK::DIRTY_FLAG_SCISSOR |
	                      GSDeviceVK::DIRTY_FLAG_INDEX_BUFFER;
	m_textures.clear();
	m_flushing = false;
}

void GSRunsVK::EndPass(u64 pass_serial)
{
	if (m_log && (m_pass.tfx || m_pass.runs))
	{
		std::fprintf(m_log, "K %llu %u %u %u %u %u |", static_cast<unsigned long long>(pass_serial), m_pass.tfx, m_pass.merged,
			m_pass.runs, m_pass.runs_multi, m_pass.longest);
		for (u32 i = 0; i < WHY_COUNT; i++)
			std::fprintf(m_log, (i == END_STATE) ? " | %u" : " %u", m_pass.why[i]);
		std::fputc('\n', m_log);
	}
	m_pass = {};
}
