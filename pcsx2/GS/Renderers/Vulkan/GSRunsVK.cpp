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
	// Every run's texture set is the whole array, whatever the run holds: no variable descriptor count, which Qualcomm's
	// 8xx-generation drivers refuse below the binding's size. Over the runs corpus and the report dumps the median run
	// holds 1 texture and the 99th percentile 79 (Spider-Man 3); an array of 32 cuts 2% of runs and adds 2 host draws to
	// the worst frame, and its pool of 256 sets is the 8,192 descriptors the variable sets had
	// (umbrella campaigns/qcom8xx-lowmem-2026-11-11/census).
	constexpr u32 MAX_TEXTURES = 32;
	constexpr u32 POOL_SETS = 256;
	// A slot's pools serve one command buffer's runs. Past this many, the allocations fail for another reason than full
	// pools, and the machine stops rather than grow without bound.
	constexpr u32 MAX_SLOT_POOLS = 64;
	// The pipeline variant: dual-source blending, RTA correction, the depth floor (all three in the program), and a pass
	// without a depth attachment (the pipeline only).
	u32 Variant(bool dual, bool rta, bool zfloor, bool nods)
	{
		return (dual ? 1u : 0u) | (rta ? 2u : 0u) | (zfloor ? 4u : 0u) | (nods ? 8u : 0u);
	}
	constexpr u32 VARIANT_NO_DEPTH = 8;
	// The vertex selector bits a run's draws may differ in: texture mapping and shading (the run's vertex program does
	// both always) and the expansion (a sprite is expanded on the CPU).
	constexpr u32 VS_RUN_FREE = 0x06u | 0x70u;
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
	if (m_set0_pool)
		vkDestroyDescriptorPool(device, m_set0_pool, nullptr);
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

	Vulkan::DescriptorSetLayoutBuilder dslb;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_set0_layout = dslb.Create(device)) == VK_NULL_HANDLE)
		return false;

	{
		// Partially bound: a run writes only the textures it holds.
		const VkDescriptorBindingFlagsEXT flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;
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
	m_frame_pools.resize(GSDeviceVK::NUM_COMMAND_BUFFERS);
	m_textures.reserve(m_max_textures);
	m_image_infos.reserve(m_max_textures);

	m_vs = g_vulkan_shader_cache->GetVertexShader(m_dev->RunsShaderSource(true, std::string(), m_source));
	if (!m_vs)
	{
		Console.Error("VK: the run vertex program did not compile");
		return false;
	}


	m_class = &GSRunsPolicy::DeviceClassFor(m_dev->m_device_properties.deviceName);
	Console.WriteLn("VK: run machine on, up to %u textures a run, split rule of class %s (%s)", m_max_textures, m_class->name,
		m_class->source);
	return true;
}

bool GSRunsVK::CreateRings()
{
	const VkDevice device = m_dev->m_device;
	if (!m_index_stream.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, RING_SIZE, "runs-index") ||
		!m_table_stream.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, RING_SIZE, "runs-table"))
	{
		return false;
	}
	// The header and table set comes from a pool of its own, sized for exactly its descriptors: the device's global pool
	// counts only the types the classic path uses, and a driver that enforces per-type counts (malisx2) refuses the rest.
	const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
	const VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, sizes};
	if (vkCreateDescriptorPool(device, &pci, nullptr, &m_set0_pool) != VK_SUCCESS)
		return false;
	const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_set0_pool, 1, &m_set0_layout};
	if (vkAllocateDescriptorSets(device, &ai, &m_set0) != VK_SUCCESS)
	{
		m_set0 = VK_NULL_HANDLE;
		return false;
	}
	Vulkan::DescriptorSetUpdateBuilder dsub;
	dsub.AddBufferDescriptorWrite(m_set0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, m_table_stream.GetBuffer(), 0, HEADER_RANGE);
	dsub.AddBufferDescriptorWrite(m_set0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, m_table_stream.GetBuffer(), 0,
		RUN_WINDOW - TABLE_SKIP);
	dsub.Update(device);
	return true;
}

void GSRunsVK::Classify(const GSHWDrawConfig& config)
{
	Draw& d = m_draw;
	d.cand = false;
	m_draws++;
	// Dormant: the draw takes the classic path, nothing of it is remembered. No run is open (runs end with their pass,
	// and the machine sleeps only between frames).
	if (!m_dormancy.OpensRuns() || m_sets_failed)
		return;
	m_frame_draws++;
	if (GSRunsPolicy::OutsidePS(config.ps) || GSRunsPolicy::OutsideVS(config.vs))
	{
		if (m_run.open)
			Flush(END_UNMERGEABLE);
		return;
	}
	// A draw that cannot open a run (no run open, and the draw before it did not share its fixed state) is drawn
	// classically and remembered, before it costs a classification.
	if (!m_run.open && !FollowsSame(config, ~0u))
	{
		Prime(config);
		return;
	}
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
		Flush(END_UNMERGEABLE);
		return;
	}

	d.cand = true;
	d.flags = pd.flags;
	d.way = pd.way;

	if (m_run.open && !Matches(config))
		Flush(EndReason(config));
}

namespace
{
	/// The draw's fixed state against a run's or the primed draw's (both hold the same fields).
	/// strict ~0u: the draw is not classified yet, so its strict key is not compared.
	template <typename Fixed>
	bool SameFixed(const GSHWDrawConfig& config, const Fixed& f, u32 strict)
	{
		const GSHWDrawConfig::VSConstantBuffer& a = config.cb_vs;
		const GSHWDrawConfig::VSConstantBuffer& b = f.cb_vs;
		return config.rt == f.rt && config.ds == f.ds && config.depth.key == f.dss && (config.vs.key & ~VS_RUN_FREE) == f.vs &&
		       config.scissor.eq(f.scissor) && std::memcmp(&a.vertex_scale, &b.vertex_scale, sizeof(GSVector2)) == 0 &&
		       std::memcmp(&a.vertex_offset, &b.vertex_offset, sizeof(GSVector2)) == 0 &&
		       std::memcmp(&a.texture_offset, &b.texture_offset, sizeof(GSVector2)) == 0 && a.max_depth == b.max_depth &&
		       std::memcmp(&config.cb_ps.STScale, &f.st_scale, sizeof(GSVector2)) == 0 && config.ps.rta_correction == f.rta &&
		       config.ps.zfloor == f.zfloor && (strict == ~0u || strict == f.strict);
	}
} // namespace

bool GSRunsVK::Matches(const GSHWDrawConfig& config) const
{
	const Run& r = m_run;
	return SameFixed(config, r, GSRunsPolicy::StrictKey(m_draw.flags, r.rta)) && r.ndraws < MAX_DRAWS && !SplitAtDear(config);
}

bool GSRunsVK::FollowsSame(const GSHWDrawConfig& config, u32 strict) const
{
	// The draw before this one was primed, or was in the run that just ended (a split, a ring or the texture array): the
	// run that follows may open at once if the fixed state is the same. A primed draw's strict key is not compared: it
	// was primed before it was classified.
	const Primed& p = m_primed;
	return p.draw + 1 == m_draws && (p.in_run ? SameFixed(config, m_run, strict) : SameFixed(config, p, ~0u));
}

void GSRunsVK::Prime(const GSHWDrawConfig& config)
{
	Primed& p = m_primed;
	p.draw = m_draws;
	p.in_run = false;
	p.rt = config.rt;
	p.ds = config.ds;
	p.dss = config.depth.key;
	p.vs = config.vs.key & ~VS_RUN_FREE;
	p.rta = config.ps.rta_correction;
	p.zfloor = config.ps.zfloor;
	p.scissor = config.scissor;
	p.cb_vs = config.cb_vs;
	p.st_scale = config.cb_ps.STScale;
}

static u64 DrawArea(const GSHWDrawConfig& config)
{
	return static_cast<u64>(std::max(0, config.drawarea.width())) * static_cast<u64>(std::max(0, config.drawarea.height()));
}

bool GSRunsVK::SplitAtDear(const GSHWDrawConfig& config) const
{
	const Run& r = m_run;
	if (r.dear_mixed || GSRunsPolicy::Dear(m_draw.flags) == r.dear)
		return false;
	return GSRunsPolicy::SplitAtDear(*m_class, r.area_total, DrawArea(config));
}

GSRunsVK::Why GSRunsVK::EndReason(const GSHWDrawConfig& config)
{
	if (config.rt != m_run.rt || config.ds != m_run.ds)
		return END_TARGET;
	if (GSRunsPolicy::StrictKey(m_draw.flags, m_run.rta) != m_run.strict)
		return END_SPLIT;
	if (SplitAtDear(config))
		return END_SPLIT;
	return END_STATE;
}

bool GSRunsVK::Continue(const GSHWDrawConfig& config)
{
	GSDeviceVK* dev = m_dev;
	// A draw without a depth buffer continues a run in a pass with one as the classic path draws it: depth untouched.
	const bool ds_ok = config.ds ? (dev->m_current_depth_target == config.ds) : ((dev->m_current_depth_target == nullptr) == m_run.nods);
	if (!m_run.open || !dev->InRenderPass() || dev->m_current_render_target != config.rt || !ds_ok ||
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
	// A draw without a depth buffer is drawn in the open pass's: with it if the classic path kept it attached (its
	// pipeline then has the depth attachment and the draw's depth state, which neither tests nor writes), else without.
	if (!dev->InRenderPass() || dev->m_current_render_target != config.rt || (config.ds && dev->m_current_depth_target != config.ds) ||
		dev->m_current_framebuffer_feedback_loop != GSDeviceVK::FeedbackLoopFlag_None || pipeline_feedback_flags != 0 || pipeline_colclip)
	{
		Flush(END_UNMERGEABLE);
		return false;
	}
	if (m_run.open && !Matches(config))
		Flush(EndReason(config));
	if (!m_run.open && !FollowsSame(config, GSRunsPolicy::StrictKey(d.flags, config.ps.rta_correction)))
	{
		Prime(config);
		return false;
	}
	// The rings and the set that names them are made by the first draw that would join a run, so a title that forms
	// none allocates nothing: its memory is laid out as with the machine off.
	if (m_set0 == VK_NULL_HANDLE)
	{
		if (m_rings_failed)
			return false;
		if (!CreateRings())
		{
			Console.Error("VK: run machine rings failed; drawing every draw classically");
			m_rings_failed = true;
			return false;
		}
	}

	// The run's program with this draw in it must be built. If it is not, it is asked for, the run ends, and the draw
	// takes the classic path. Asked again below if the run ends for another reason before the draw is in it, since the
	// draw then opens a run of its own.
	const auto ready = [&]() {
		const bool open = m_run.open;
		GSRunsPolicy::StateSet states = open ? m_run.states : GSRunsPolicy::StateSet();
		states.Note(d.flags);
		const u32 ways = (open ? m_run.ways : 0u) | (1u << d.way);
		const bool rta = config.ps.rta_correction;
		const u32 variant = Variant((ways & ~(1u << 2)) != 0, rta, config.ps.zfloor, dev->m_current_depth_target == nullptr);
		if (ProgramReady(config.depth.key, variant, states.Key()))
			return true;
		m_frame_joined++; // it would have joined: counted, so a title does not fall asleep while its programs build
		Flush(END_COMPILING);
		return false;
	};
	if (!ready())
		return false;

	const VkPhysicalDeviceLimits& limits = dev->m_device_properties.limits;
	const u32 align = std::max<u32>(64, static_cast<u32>(std::max(limits.minUniformBufferOffsetAlignment, limits.minStorageBufferOffsetAlignment)));
	// A sprite (two vertices, the classic path expands it in its vertex program) becomes four vertices and two
	// triangles here, the corners and index order the classic expansion makes (tfx.glsl VS_EXPAND_SPRITE, the expand
	// index buffer): each corner is the right-bottom vertex with X, S, U from the left-top one on the right and Y, T, V
	// on the bottom. Every value is a raw vertex field, so the vertex program computes from it what the classic
	// expansion did.
	const bool sprites = config.vs.expand == GSHWDrawConfig::VSExpand::Sprite;
	if (sprites && (config.nverts & 1 || config.nindices != config.nverts * 3))
	{
		Flush(END_UNMERGEABLE);
		return false;
	}
	const u32 nidx = config.nindices, nvtx = sprites ? config.nverts * 2 : config.nverts;
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
			r.nods = dev->m_current_depth_target == nullptr;
			r.dss = config.depth.key;
			r.vs = config.vs.key & ~VS_RUN_FREE;
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
		if (sprites)
		{
			for (u32 s = 0; s < config.nverts; s += 2)
			{
				const GSVertex& lt = config.verts[s];
				GSVertex rb = config.verts[s + 1];
				rb.FOG = (rb.FOG & 0xFFu) | (k << 8);
				for (u32 c = 0; c < 4; c++)
				{
					GSVertex v = rb;
					if (c & 1)
					{
						v.XYZ.X = lt.XYZ.X;
						v.ST.S = lt.ST.S;
						v.U = lt.U;
					}
					if (c & 2)
					{
						v.XYZ.Y = lt.XYZ.Y;
						v.ST.T = lt.ST.T;
						v.V = lt.V;
					}
					dst[s * 2 + c] = v;
				}
				const u32 b = vstart + s * 2;
				u32* q = idx + s * 3;
				q[0] = b;
				q[1] = b + 1;
				q[2] = b + 2;
				q[3] = b + 1;
				q[4] = b + 2;
				q[5] = b + 3;
			}
			vring.CommitMemory(vsize);
		}
		else
		{
			for (u32 i = 0; i < nvtx; i++)
			{
				GSVertex v = config.verts[i];
				v.FOG = (v.FOG & 0xFFu) | (k << 8);
				dst[i] = v;
			}
			vring.CommitMemory(vsize);
			for (u32 i = 0; i < nidx; i++)
				idx[i] = vstart + config.indices[i];
		}
		m_index_stream.CommitMemory(nidx * 4);

		u32 s[8];
		s[0] = d.flags;
		s[1] = slot;
		const GSVector4 fog = config.cb_ps.FogColor_AREF;
		s[2] = (config.ps.fog ? (static_cast<u32>(fog.x) | (static_cast<u32>(fog.y) << 8) | (static_cast<u32>(fog.z) << 16)) : 0u) |
		       ((d.way == 1 || d.way == 5) ? (static_cast<u32>(config.cb_ps.TA_MaxDepth_Af.w * 128.0f) << 24) : 0u);
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
		// The draw's area (the renderer's draw rectangle, clipped to the scissor, in target pixels), a ceiling on its
		// fill: the split rule weighs the run's against the price of a host draw.
		const u64 area = DrawArea(config);
		r.area_total += area;
		if (r.ndraws != 0 && GSRunsPolicy::Dear(d.flags) != r.dear)
			r.dear_mixed = true;
		r.ndraws++;
		m_primed.draw = m_draws;
		m_primed.in_run = true;
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
	return m_run.states.Key();
}

VkShaderModule GSRunsVK::GetFragmentShader(u32 variant, u32 spec)
{
	variant &= ~VARIANT_NO_DEPTH; // the pipeline's, not the program's
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
	defines += "#define RUN_ZFLOOR " + std::to_string((variant >> 2) & 1) + "\n";
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
	const bool ds = !(variant & VARIANT_NO_DEPTH);
	gpb.SetRenderPass(m_dev->GetTFXRenderPass(true, ds, false, false, false, false, VK_ATTACHMENT_LOAD_OP_LOAD,
						  ds ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE),
		0);
	gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	gpb.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	GSDeviceVK::AddCachedDynamicState(gpb);
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
	if (ds)
		gpb.SetDepthState((dss.ztst != ZTST_ALWAYS || dss.zwe), dss.zwe, ztst[dss.ztst]);
	else
		gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
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
	if (!queue.IsRunning())
	{
		// Built here and now, on the GS thread, only while the device has no compile queue.
		m_pipelines.emplace(key, CreatePipeline(dss, variant, spec));
		m_compiles++;
		return true;
	}

	auto it = m_pending.find(key);
	if (it == m_pending.end())
	{
		// Built on the device's compile queue, ahead of need; the draws that need it meanwhile are drawn classically.
		Pending job;
		job.result = std::make_shared<Result>();
		job.ticket = queue.Submit(GSCompileQueue::Priority::Ahead, [this, dss, variant, spec, result = job.result]() {
			result->pipeline = CreatePipeline(dss, variant, spec);
		});
		m_pending.emplace(key, std::move(job));
		return false;
	}
	if (!queue.IsDone(it->second.ticket))
		return false;
	m_pipelines.emplace(key, it->second.result->pipeline);
	m_compiles++;
	m_pending.erase(it);
	return true;
}

VkPipeline GSRunsVK::GetPipeline(bool dual)
{
	const Run& r = m_run;
	// Append let the run grow only while its program was built.
	const auto it = m_pipelines.find(PipelineKey(r.dss, Variant(dual, r.rta, r.zfloor, r.nods), SpecKey()));
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
	for (;;)
	{
		bool fresh = false;
		if (fp.cursor == fp.pools.size())
		{
			if (fp.pools.size() == MAX_SLOT_POOLS)
			{
				Console.Error("VK: run machine stopped: %u descriptor pools in one command buffer slot are full (this run "
							  "%u textures); drawing every draw classically",
					MAX_SLOT_POOLS, count);
				m_sets_failed = true;
				return VK_NULL_HANDLE;
			}
			const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SETS * m_max_textures};
			const VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, POOL_SETS, 1, &size};
			VkDescriptorPool pool;
			if (vkCreateDescriptorPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
				pxFailRel("VK: run descriptor pool creation failed");
			fp.pools.push_back(pool);
			fresh = true;
		}
		const VkDescriptorSetAllocateInfo ai = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, fp.pools[fp.cursor], 1, &m_set1_layout};
		const VkResult res = vkAllocateDescriptorSets(device, &ai, &set);
		if (res == VK_SUCCESS)
			break;
		if (fresh)
		{
			// An empty pool refused the set, so no new pool will take it (Qualcomm's 8xx-generation drivers did, for
			// the variable-count sets this machine once used). The pool was never used and goes now.
			Console.Error("VK: run machine stopped: an empty descriptor pool refused a set of %u textures (%s); drawing "
						  "every draw classically",
				count, Vulkan::VkResultToString(res));
			vkDestroyDescriptorPool(device, fp.pools.back(), nullptr);
			fp.pools.pop_back();
			m_sets_failed = true;
			return VK_NULL_HANDLE;
		}
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

void GSRunsVK::Flush(Why)
{
	Run& r = m_run;
	if (!r.open || m_flushing)
		return;
	r.open = false;
	if (r.ndraws == 0)
		return;
	if (r.ndraws > 1)
		m_frame_joined += r.ndraws;
	m_flushing = true;

	GSDeviceVK* dev = m_dev;
	const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
	// Way 2 alone needs no blending; any other way needs the dual-source blend.
	const bool dual = (r.ways & ~(1u << 2)) != 0;
	const VkPipeline pipeline = GetPipeline(dual);
	const VkDescriptorSet set1 = WriteTextureSet();
	if (set1 == VK_NULL_HANDLE)
	{
		// No set could be had and the machine has stopped: this run's draws are lost, once.
		m_textures.clear();
		m_flushing = false;
		return;
	}
	g_perfmon.Put(GSPerfMon::PipelineSwitches, 1);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const u32 offsets[2] = {r.header_off, r.table_off};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &m_set0, 2, offsets);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 1, 1, &set1, 0, nullptr);
	const VkRect2D scissor = {{r.scissor.x, r.scissor.y}, {static_cast<u32>(r.scissor.width()), static_cast<u32>(r.scissor.height())}};
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

	// The classic path rebinds all of its state for its next draw. m_current_pipeline is left alone: it is the pipeline
	// the classic path wants bound, and a flush from inside ApplyTFXState or ApplyUtilityState comes after SetPipeline
	// chose it; the dirty flag makes the apply bind it again.
	dev->m_current_pipeline_layout = GSDeviceVK::PipelineLayout::Undefined;
	dev->m_dirty_flags |= GSDeviceVK::DIRTY_FLAG_PIPELINE | GSDeviceVK::DIRTY_FLAG_TFX_UBO | GSDeviceVK::DIRTY_FLAG_TFX_TEXTURES |
	                      GSDeviceVK::DIRTY_FLAG_VS_PUSH_CONSTANTS | GSDeviceVK::DIRTY_FLAG_VIEWPORT | GSDeviceVK::DIRTY_FLAG_SCISSOR |
	                      GSDeviceVK::DIRTY_FLAG_INDEX_BUFFER;
	m_textures.clear();
	m_flushing = false;
}

void GSRunsVK::EndFrame()
{
	m_dormancy.EndFrame(m_frame_draws, m_frame_joined);
	m_frame_draws = 0;
	m_frame_joined = 0;
}
