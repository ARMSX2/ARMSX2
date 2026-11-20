// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSRunEmitterVK.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "GS/Renderers/Common/GSMeasurementOverrides.h"
#include "GS/Renderers/Common/GSRowEmitter.h"
#include "GS/GSPerfMon.h"

#include "common/Console.h"

#include <algorithm>
#include <cstring>

// A group's window in its side's table ring: a header (the group's vertex and texture constants), then one 32-byte row
// per row. The window is reserved whole when the group opens, so the descriptor range bound for it always lies inside
// the ring, and the group's rows are committed one by one behind the header.
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
	// The bases' rings take every group of a title without twins; the twins' take Sly 3's held twins (about 1,200 rows a
	// frame at 1x) many times over.
	constexpr u32 RING_SIZE[2] = {16 * 1024 * 1024, 4 * 1024 * 1024};
	// Every group's texture set is the whole array, whatever the group holds: no variable descriptor count, which
	// Qualcomm's 8xx-generation drivers refuse below the binding's size. Over the runs corpus and the report dumps the
	// median run holds 1 texture and the 99th percentile 79 (Spider-Man 3); an array of 32 cuts 2% of runs and adds 2
	// host draws to the worst frame, and its pool of 256 sets is the 8,192 descriptors the variable sets had.
	constexpr u32 MAX_TEXTURES = 32;
	static_assert(MAX_TEXTURES <= 64, "Group::sampler_slots holds one bit a slot");
	constexpr u32 POOL_SETS = 256;
	// A slot's pools serve one command buffer's groups. Past this many, the allocations fail for another reason than
	// full pools, and the run program stops rather than grow without bound.
	constexpr u32 MAX_SLOT_POOLS = 64;
	// The pipeline variant: dual-source blending (any way but 2 alone), RTA correction, the depth floor (all three in the
	// program), and, the pipeline only, a pass without a depth attachment and the blend unit's own equation.
	constexpr u32 VARIANT_NO_DEPTH = 8;
	constexpr u32 VARIANT_UNIT = 16;
	u32 Variant(u32 ways, bool rta, bool zfloor, bool nods)
	{
		return ((ways & ~(1u << 2)) != 0 ? 1u : 0u) | (rta ? 2u : 0u) | (zfloor ? 4u : 0u) | (nods ? VARIANT_NO_DEPTH : 0u) |
		       (GSRunsPolicy::UnitEquation(ways) ? VARIANT_UNIT : 0u);
	}
	// The vertex selector bits a group's rows may differ in: texture mapping and shading (the run's vertex program does
	// both always) and the expansion (a sprite is expanded on the CPU).
	constexpr u32 VS_RUN_FREE = 0x06u | 0x70u;
	u64 PipelineKey(u8 dss, u32 variant, u32 spec) { return static_cast<u64>(dss) | (static_cast<u64>(variant) << 8) | (static_cast<u64>(spec) << 16); }

	u64 DrawArea(const GSHWDrawConfig& config)
	{
		return static_cast<u64>(std::max(0, config.drawarea.width())) * static_cast<u64>(std::max(0, config.drawarea.height()));
	}

	/// The row's fixed state against a group's: what one host draw of the run program holds one value of.
	/// strict ~0u: the strict key is not compared.
	bool SameFixed(const GSHWDrawConfig& config, const GSHWDrawConfig::VSConstantBuffer& b, const GSTexture* rt,
		const GSTexture* ds, u8 dss, u16 vs, const GSVector4i& scissor, const GSVector2& st_scale, u8 rta, u8 zfloor)
	{
		const GSHWDrawConfig::VSConstantBuffer& a = config.cb_vs;
		return config.rt == rt && config.ds == ds && config.depth.key == dss && (config.vs.key & ~VS_RUN_FREE) == vs &&
		       config.scissor.eq(scissor) && std::memcmp(&a.vertex_scale, &b.vertex_scale, sizeof(GSVector2)) == 0 &&
		       std::memcmp(&a.vertex_offset, &b.vertex_offset, sizeof(GSVector2)) == 0 &&
		       std::memcmp(&a.texture_offset, &b.texture_offset, sizeof(GSVector2)) == 0 && a.max_depth == b.max_depth &&
		       std::memcmp(&config.cb_ps.STScale, &st_scale, sizeof(GSVector2)) == 0 && config.ps.rta_correction == rta &&
		       config.ps.zfloor == zfloor;
	}

	/// The streak's key (GSRunsPolicy::StreakClass).
	u64 FixedClass(const GSHWDrawConfig& config)
	{
		return GSRunsPolicy::StreakClass(config, static_cast<u16>(config.vs.key & ~VS_RUN_FREE),
			config.rt ? config.rt->GetSize() : GSVector2i(0, 0), config.ds ? config.ds->GetSize() : GSVector2i(0, 0));
	}
} // namespace

GSRunEmitterVK::GSRunEmitterVK(GSDeviceVK* dev)
	: m_dev(dev)
{
	m_frame_pools.resize(GSDeviceVK::NUM_COMMAND_BUFFERS);
	if (g_gs_measurement_overrides.runs_off)
	{
		Console.WriteLn("VK: run program off (-runs off)");
		return;
	}
	// Device capability decides, not the vendor: the per-row textures need non-uniform indexing into a runtime array,
	// and the groups' blend ways need the second fragment output.
	if (!dev->m_optional_extensions.vk_ext_descriptor_indexing || !dev->m_features.dual_source_blend)
	{
		Console.WriteLn("VK: run program off: %s", !dev->m_optional_extensions.vk_ext_descriptor_indexing ?
			"no descriptor indexing" : "no dual-source blending");
		return;
	}
	m_run_program = Init();
	if (!m_run_program)
		Console.Error("VK: run program failed to initialise; drawing every row classically");
}

GSRunEmitterVK::~GSRunEmitterVK()
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
	for (Rings& r : m_rings)
	{
		if (r.set0_pool)
			vkDestroyDescriptorPool(device, r.set0_pool, nullptr);
		r.index.Destroy(false);
		r.table.Destroy(false);
	}
	if (m_pipeline_layout)
		vkDestroyPipelineLayout(device, m_pipeline_layout, nullptr);
	if (m_set0_layout)
		vkDestroyDescriptorSetLayout(device, m_set0_layout, nullptr);
	if (m_set1_layout)
		vkDestroyDescriptorSetLayout(device, m_set1_layout, nullptr);
}

bool GSRunEmitterVK::Init()
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

	// A group's texture array is bounded by what one stage may sample from one set.
	m_max_textures = std::min({MAX_TEXTURES, limits.maxPerStageDescriptorSamplers, limits.maxPerStageDescriptorSampledImages,
		limits.maxDescriptorSetSamplers, limits.maxDescriptorSetSampledImages, limits.maxPerStageResources - 8});
	if (m_max_textures < 8)
	{
		Console.WriteLn("VK: run program off: %u samplers per stage", m_max_textures);
		return false;
	}

	Vulkan::DescriptorSetLayoutBuilder dslb;
	dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT);
	if ((m_set0_layout = dslb.Create(device)) == VK_NULL_HANDLE)
		return false;

	{
		// Partially bound: a group writes only the textures it holds.
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
	m_bases.textures.reserve(m_max_textures);
	m_image_infos.reserve(m_max_textures);

	m_vs = g_vulkan_shader_cache->GetVertexShader(m_dev->RunsShaderSource(true, std::string(), m_source));
	if (!m_vs)
	{
		Console.Error("VK: the run vertex program did not compile");
		return false;
	}

	m_class = &GSRunsPolicy::DeviceClassFor(m_dev->m_device_properties.deviceName);
	m_one_call = m_dev->GetMobileDriverProfile().HasBug(DriverBug::BrokenMixedSamplingDerivatives);
	m_fetch = m_dev->GetMobileDriverProfile().HasBug(DriverBug::StallsOnFilteredRunSampling);
	Console.WriteLn("VK: run program on, up to %u textures a group, groups open at %u rows, split rule of class %s (%s), %s%s",
		m_max_textures, m_open_rows, m_class->name, m_class->source,
		m_one_call ? "one sampling call a group" : "mixed sampling calls",
		m_fetch ? ", a group's sampler bounded, small rows past it fetched" : "");
	return true;
}

bool GSRunEmitterVK::CreateRings(Side side)
{
	Rings& r = m_rings[side];
	const VkDevice device = m_dev->m_device;
	static constexpr const char* names[2][2] = {{"runs-index", "runs-table"}, {"runs-index-twins", "runs-table-twins"}};
	if (!r.index.Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, RING_SIZE[side], names[side][0]) ||
		!r.table.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, RING_SIZE[side], names[side][1]))
	{
		return false;
	}
	// The header and table set comes from a pool of its own, sized for exactly its descriptors: the device's global pool
	// counts only the types the classic path uses, and a driver that enforces per-type counts (malisx2) refuses the rest.
	const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
	const VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, sizes};
	if (vkCreateDescriptorPool(device, &pci, nullptr, &r.set0_pool) != VK_SUCCESS)
		return false;
	const VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, r.set0_pool, 1, &m_set0_layout};
	if (vkAllocateDescriptorSets(device, &ai, &r.set0) != VK_SUCCESS)
	{
		r.set0 = VK_NULL_HANDLE;
		return false;
	}
	Vulkan::DescriptorSetUpdateBuilder dsub;
	dsub.AddBufferDescriptorWrite(r.set0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, r.table.GetBuffer(), 0, HEADER_RANGE);
	dsub.AddBufferDescriptorWrite(r.set0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, r.table.GetBuffer(), 0,
		RUN_WINDOW - TABLE_SKIP);
	dsub.Update(device);
	return true;
}

GSRunEmitterVK::Group* GSRunEmitterVK::OpenGroup(Side side)
{
	if (side == BASES)
		return m_bases.open ? &m_bases : nullptr;
	return m_twin_open != ~0u ? &m_twin_groups[m_twin_open] : nullptr;
}

void GSRunEmitterVK::Close(Side side)
{
	if (side == BASES)
	{
		if (m_bases.open)
			Record(BASES, m_bases);
		return;
	}
	m_twin_open = ~0u;
}

bool GSRunEmitterVK::SplitAtDear(const Group& g, const GSHWDrawConfig& config, const GSRunsPolicy::Draw& d) const
{
	if (g.dear_mixed || GSRunsPolicy::Dear(d.flags) == g.dear)
		return false;
	return GSRunsPolicy::SplitAtDear(*m_class, g.area_total, DrawArea(config));
}

bool GSRunEmitterVK::Fits(const Group& g, const GSHWDrawConfig& config, const GSRunsPolicy::Draw& d) const
{
	return SameFixed(config, g.cb_vs, g.rt, g.ds, g.dss, g.vs, g.scissor, g.st_scale, g.rta, g.zfloor) &&
	       GSRunsPolicy::StrictKey(d.flags, g.rta) == g.strict && g.ndraws < MAX_DRAWS &&
	       GSRunsPolicy::WaysShareAnEquation(g.ways | (1u << d.way)) && GSRunsPolicy::LodJoins(m_one_call, g.lod, d.flags) &&
	       !SplitAtDear(g, config, d);
}

GSRunEmitterVK::Join GSRunEmitterVK::JoinGroup(Side side, const GSHWDrawConfig& config)
{
	pxAssert(m_run_program && !m_recording);
	if (m_sets_failed)
	{
		m_streaks.Break(side);
		return Join::Classic;
	}
	GSDeviceVK* dev = m_dev;
	Group* g = OpenGroup(side);
	GSRunsPolicy::Draw d;
	u64 cls;
	if (g)
	{
		if (!dev->RowFitsRunProgram(config, d))
		{
			Close(side);
			m_streaks.Break(side);
			return Join::Classic;
		}
		if (Fits(*g, config, d))
		{
			cls = g->cls; // the same fixed state as the group's, so its class
		}
		else
		{
			// A row of the group's own fixed state that cannot join it is a split: the next group opens at once.
			cls = FixedClass(config);
			const bool same = g->cls == cls &&
				SameFixed(config, g->cb_vs, g->rt, g->ds, g->dss, g->vs, g->scissor, g->st_scale, g->rta, g->zfloor);
			Close(side);
			g = nullptr;
			if (same)
				m_streaks.Split(side);
		}
		if (!m_streaks.Note(side, cls, m_open_rows) && !g)
			return Join::Classic;
	}
	else
	{
		cls = FixedClass(config);
		// Rows are counted by their fixed state before they are classified: the run program is asked about a row only
		// once its class's streak would open a group (most rows of a title with few groups never are).
		if (!m_streaks.Note(side, cls, m_open_rows))
			return Join::Classic;
		if (!dev->RowFitsRunProgram(config, d))
		{
			m_streaks.Break(side);
			return Join::Classic;
		}
	}
	const VkSampler sampler = dev->RowSampler(config);

	// The group's program with this row in it must be built. If it is not, it is asked for, the group ends, and the row
	// is drawn classically; the next row of the class opens a group at once if its program is built by then.
	const bool nods = dev->m_current_depth_target == nullptr;
	const auto ready = [&](const Group* with) {
		GSRunsPolicy::StateSet states = with ? with->states : GSRunsPolicy::StateSet();
		states.Note(d.flags);
		const u32 ways = (with ? with->ways : 0u) | (1u << d.way);
		return ProgramReady(config.depth.key, Variant(ways, config.ps.rta_correction, config.ps.zfloor, nods), states.Key());
	};
	// A row whose state flags and way the group already holds changes neither its program nor its state set.
	const bool same_program = g && d.flags == g->last_flags && (g->ways & (1u << d.way));
	if (!same_program && !ready(g))
	{
		if (g)
			Close(side);
		m_streaks.Split(side);
		return Join::Classic;
	}
	// A group that ends below (ring, texture cap) is replaced by one holding this row alone, whose program is another.
	bool alone_ready = g == nullptr;

	Rings& rings = m_rings[side];
	if (rings.set0 == VK_NULL_HANDLE)
	{
		if (rings.failed)
			return Join::Classic;
		if (!CreateRings(side))
		{
			Console.Error("VK: run program rings failed; drawing those rows classically");
			rings.failed = true;
			return Join::Classic;
		}
	}

	const VkPhysicalDeviceLimits& limits = dev->m_device_properties.limits;
	const u32 align = std::max<u32>(64, static_cast<u32>(std::max(limits.minUniformBufferOffsetAlignment, limits.minStorageBufferOffsetAlignment)));
	// A sprite (two vertices, the classic path expands it in its vertex program) becomes four vertices and two triangles
	// here, the corners and index order the classic expansion makes (tfx.glsl VS_EXPAND_SPRITE, the expand index
	// buffer): each corner is the right-bottom vertex with X, S, U from the left-top one on the right and Y, T, V on the
	// bottom. Every value is a raw vertex field, so the vertex program computes from it what the classic expansion did.
	const bool sprites = config.vs.expand == GSHWDrawConfig::VSExpand::Sprite;
	if (sprites && (config.nverts & 1 || config.nindices != config.nverts * 3))
	{
		if (g)
			Close(side);
		m_streaks.Break(side);
		return Join::Classic;
	}
	const u32 nidx = config.nindices, nvtx = sprites ? config.nverts * 2 : config.nverts;
	const u32 vsize = nvtx * sizeof(GSVertex);
	VKStreamBuffer& vring = dev->m_vertex_stream_buffer;
	const u64 fence = dev->GetCurrentFenceCounter();
	// A ring that cannot take the row is a submit: everything pending is recorded first (the pass ends), and the row is
	// drawn or held classically by the caller.
	const auto submit = [this, dev, side](const char* why) {
		dev->ExecuteCommandBufferAndRestartRenderPass(false, why);
		pxAssert(!OpenGroup(side) || OpenGroup(side)->ndraws == 0);
		if (OpenGroup(side))
			Close(side);
		return Join::Submitted;
	};

	for (u32 attempt = 0;; attempt++)
	{
		if (attempt == 4)
			pxFailRel("VK: the run emitter could not place a row");
		if (!g)
		{
			if (!alone_ready)
			{
				if (!ready(nullptr))
				{
					m_streaks.Split(side);
					return Join::Classic;
				}
				alone_ready = true;
			}
			if (!rings.table.ReserveMemory(RUN_WINDOW, align))
				return submit("runs table ring");
			Group* ng;
			if (side == BASES)
				ng = &m_bases;
			else
			{
				if (m_twin_groups_used == m_twin_groups.size())
					m_twin_groups.emplace_back().textures.reserve(m_max_textures);
				m_twin_open = m_twin_groups_used++;
				m_held.push_back({true, m_twin_open});
				ng = &m_twin_groups[m_twin_open];
			}
			std::vector<Texture> textures = std::move(ng->textures);
			*ng = Group();
			ng->textures = std::move(textures);
			ng->textures.clear();
			g = ng;
			g->header_off = rings.table.GetCurrentOffset();
			g->table_off = g->header_off + TABLE_SKIP;
			RunHeader hdr = {};
			std::memcpy(hdr.vertex_scale, &config.cb_vs.vertex_scale, 8);
			std::memcpy(hdr.vertex_offset, &config.cb_vs.vertex_offset, 8);
			std::memcpy(hdr.texture_offset, &config.cb_vs.texture_offset, 8);
			std::memcpy(hdr.st_scale, &config.cb_ps.STScale, 8);
			hdr.max_depth = config.cb_vs.max_depth;
			std::memcpy(rings.table.GetCurrentHostPointer(), &hdr, sizeof(hdr));
			rings.table.CommitMemory(TABLE_SKIP);
			g->open = true;
			g->cls = cls;
			g->rt = config.rt;
			g->ds = config.ds;
			g->pass_rt = dev->m_current_render_target;
			g->pass_ds = dev->m_current_depth_target;
			g->nods = nods;
			g->dss = config.depth.key;
			g->vs = config.vs.key & ~VS_RUN_FREE;
			g->scissor = config.scissor;
			const GSVector2i size = (config.rt ? config.rt : config.ds)->GetSize();
			g->viewport = {0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, 1.0f};
			g->cb_vs = config.cb_vs;
			g->st_scale = config.cb_ps.STScale;
			g->rta = config.ps.rta_correction;
			g->zfloor = config.ps.zfloor;
			g->dear = GSRunsPolicy::Dear(d.flags);
			g->strict = GSRunsPolicy::StrictKey(d.flags, g->rta);
			g->idx_first = ~0u;
		}
		// The row: contiguous with the group's other rows.
		if (!rings.table.ReserveMemory(32, 16))
			return submit("runs table ring");
		if (rings.table.GetCurrentOffset() != g->table_off + 32 * g->ndraws)
		{
			Close(side);
			g = nullptr;
			continue;
		}
		u8* row = rings.table.GetCurrentHostPointer();
		// The indices: contiguous with the group's.
		if (!rings.index.ReserveMemory(nidx * 4, 4))
			return submit("runs index ring");
		const u32 ioff = rings.index.GetCurrentOffset() / 4;
		if (g->ndraws != 0 && ioff != g->idx_first + g->idx_count)
		{
			Close(side);
			g = nullptr;
			continue;
		}
		u32* idx = reinterpret_cast<u32*>(rings.index.GetCurrentHostPointer());
		// The vertices: the vertex ring as IASetVertexBuffer reserves it.
		if (!vring.ReserveMemory(vsize, sizeof(GSVertex) * GetVertexAlignment(GSHWDrawConfig::VSExpand::None)))
			return submit("runs vertex ring");
		if (g->ndraws != 0 && vring.GetBuffer() != g->vbuf)
		{
			// The vertex ring grew under the group: its rows are in the old buffer.
			Close(side);
			g = nullptr;
			continue;
		}
		pxAssert(fence == dev->GetCurrentFenceCounter());

		// The row's texture: its slot in the group's array.
		u32 slot = 0;
		u32 fetch_bits = 0;
		if (config.ps.tfx != 4)
		{
			const GSTextureVK* tex = static_cast<const GSTextureVK*>(config.tex);
			const Texture t = {tex->GetView(), sampler, tex->GetVkLayout()};
			const auto same = [&t](const Texture& o) { return o.view == t.view && o.sampler == t.sampler; };
			bool known = true;
			if (g->last_slot < g->textures.size() && same(g->textures[g->last_slot]))
				slot = g->last_slot;
			else
			{
				const auto it = std::find_if(g->textures.begin(), g->textures.end(), same);
				slot = static_cast<u32>(it - g->textures.begin());
				known = it != g->textures.end();
				if (!known && g->textures.size() == m_max_textures)
				{
					Close(side);
					g = nullptr;
					continue;
				}
			}
			if (m_fetch)
			{
				// At most SAMPLER_TEXTURES of the group's textures through the sampler; past them a small row fetches and a
				// large one, or one that cannot fetch, opens the next group (GSRunsPolicy::PlaceRow).
				const u32 bits = GSRunsPolicy::FetchBits(d.flags, config.sampler, config.tex->GetMipmapLevels());
				const bool in_set = (g->sampler_slots >> slot) & 1;
				switch (GSRunsPolicy::PlaceRow(bits != 0, in_set, g->sampler_count, DrawArea(config)))
				{
					case GSRunsPolicy::Placement::Sampler:
						if (!in_set)
						{
							g->sampler_slots |= u64{1} << slot;
							g->sampler_count++;
						}
						break;
					case GSRunsPolicy::Placement::Fetch:
						fetch_bits = bits;
						g->fetched++;
						break;
					case GSRunsPolicy::Placement::Close:
						Close(side);
						g = nullptr;
						continue;
				}
			}
			if (!known)
				g->textures.push_back(t);
			g->last_slot = slot;
		}

		const u32 k = g->ndraws;
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
		}
		else
		{
			for (u32 i = 0; i < nvtx; i++)
			{
				GSVertex v = config.verts[i];
				v.FOG = (v.FOG & 0xFFu) | (k << 8);
				dst[i] = v;
			}
			for (u32 i = 0; i < nidx; i++)
				idx[i] = vstart + config.indices[i];
		}
		vring.CommitMemory(vsize);
		rings.index.CommitMemory(nidx * 4);

		u32 s[8];
		s[0] = d.flags;
		s[1] = slot | fetch_bits;
		const GSVector4 fog = config.cb_ps.FogColor_AREF;
		s[2] = (config.ps.fog ? (static_cast<u32>(fog.x) | (static_cast<u32>(fog.y) << 8) | (static_cast<u32>(fog.z) << 16)) : 0u) |
		       (GSRunsPolicy::WayReadsAf(d.way) ? (static_cast<u32>(config.cb_ps.TA_MaxDepth_Af.w * 128.0f) << 24) : 0u);
		std::memcpy(&s[3], &fog.w, 4);
		std::memcpy(&s[4], &config.cb_ps.LODParams, 16);
		std::memcpy(row, s, 32);
		rings.table.CommitMemory(32);

		if (g->ndraws == 0)
		{
			g->idx_first = ioff;
			g->idx_count = 0;
			g->vbuf = vring.GetBuffer();
		}
		// The row's area (the renderer's draw rectangle, clipped to the scissor, in target pixels), a ceiling on its
		// fill: the split rule weighs the group's against the price of a host draw.
		g->area_total += DrawArea(config);
		if (g->ndraws != 0 && GSRunsPolicy::Dear(d.flags) != g->dear)
			g->dear_mixed = true;
		g->ndraws++;
		g->idx_count += nidx;
		g->ways |= 1u << d.way;
		if (GSRunsPolicy::Textured(d.flags))
			g->lod = GSRunsPolicy::Field(d.flags, GSRunsPolicy::STATE_LOD);
		if (d.flags != g->last_flags)
		{
			g->states.Note(d.flags); // which states the group holds one value of: those become constants of its program
			g->last_flags = d.flags;
		}
		if (config.tex)
			static_cast<GSTextureVK*>(config.tex)->SetUseFenceCounter(fence);
		return Join::Joined;
	}
}

GSRunEmitterVK::HeldClassic* GSRunEmitterVK::AddHeldClassic()
{
	// The twins' open group ends: a held classic draw comes after it.
	m_twin_open = ~0u;
	if (m_classic_used == m_classic.size())
		m_classic.emplace_back();
	m_held.push_back({false, m_classic_used});
	return &m_classic[m_classic_used++];
}

void GSRunEmitterVK::RecordOpen()
{
	if (!m_bases.open || m_recording)
		return;
	// The group ended at a recording, not at a row that could not join it: a split.
	m_streaks.Split(BASES);
	Record(BASES, m_bases);
	m_dev->UpdateDeferredCount();
}

void GSRunEmitterVK::RecordAll()
{
	if (m_recording)
		return;
	m_dev->m_raw_binds.Reset();
	if (m_bases.open)
	{
		m_streaks.Split(BASES);
		Record(BASES, m_bases);
	}
	for (const Held& h : m_held)
	{
		if (h.group)
			Record(TWINS, m_twin_groups[h.index]);
		else
		{
			m_recording = true;
			m_dev->RecordHeldClassic(m_classic[h.index]);
			m_recording = false;
		}
	}
	// The last twins' group ended at a recording, not at a row that could not join it: a split.
	if (m_twin_open != ~0u)
		m_streaks.Split(TWINS);
	m_held.clear();
	m_twin_groups_used = 0;
	m_twin_open = ~0u;
	m_classic_used = 0;
}

void GSRunEmitterVK::Drop()
{
	m_bases.open = false;
	m_bases.textures.clear();
	m_held.clear();
	m_twin_groups_used = 0;
	m_twin_open = ~0u;
	m_classic_used = 0;
}

VkShaderModule GSRunEmitterVK::GetFragmentShader(u32 variant, u32 spec)
{
	variant &= ~(VARIANT_NO_DEPTH | VARIANT_UNIT); // the pipeline's, not the program's
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
	if (m_fetch)
		defines += "#define RUN_FETCH 1\n";
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

VkPipeline GSRunEmitterVK::CreatePipeline(u8 dss_key, u32 variant, u32 spec)
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
	gpb.AddVertexAttribute(7, 0, VK_FORMAT_R32_UINT, 28); // FOG word: the row's index in its group in its high bytes
	static constexpr VkCompareOp ztst[] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_GREATER};
	if (ds)
		gpb.SetDepthState((dss.ztst != ZTST_ALWAYS || dss.zwe), dss.zwe, ztst[dss.ztst]);
	else
		gpb.SetDepthState(false, false, VK_COMPARE_OP_ALWAYS);
	if (dual)
	{
		gpb.SetBlendAttachment(0, true, (variant & VARIANT_UNIT) ? VK_BLEND_FACTOR_SRC1_COLOR : VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF);
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

bool GSRunEmitterVK::ProgramReady(u8 dss, u32 variant, u32 spec)
{
	const u64 key = PipelineKey(dss, variant, spec);
	if (m_pipelines.find(key) != m_pipelines.end())
		return true;

	GSCompileQueue& queue = m_dev->m_compile_queue;
	if (!queue.IsRunning())
	{
		// Built here and now, on the GS thread, only while the device has no compile queue.
		m_pipelines.emplace(key, CreatePipeline(dss, variant, spec));
		return true;
	}

	auto it = m_pending.find(key);
	if (it == m_pending.end())
	{
		// Built on the device's compile queue, ahead of need; the rows that need it meanwhile are drawn classically.
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
	m_pending.erase(it);
	return true;
}

VkPipeline GSRunEmitterVK::GetPipeline(const Group& g)
{
	// JoinGroup let the group grow only while its program was built.
	const auto it = m_pipelines.find(PipelineKey(g.dss, Variant(g.ways, g.rta, g.zfloor, g.nods), g.states.Key()));
	if (it == m_pipelines.end())
		pxFailRel("VK: a group grew past the program built for it");
	return it->second;
}

VkDescriptorSet GSRunEmitterVK::WriteTextureSet(std::vector<Texture>& textures)
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

	// A group of untextured rows still binds an array: one null texture.
	if (textures.empty())
		textures.push_back({dev->m_null_texture->GetView(), dev->m_point_sampler, dev->m_null_texture->GetVkLayout()});
	const u32 count = static_cast<u32>(textures.size());

	VkDescriptorSet set = VK_NULL_HANDLE;
	for (;;)
	{
		bool fresh = false;
		if (fp.cursor == fp.pools.size())
		{
			if (fp.pools.size() == MAX_SLOT_POOLS)
			{
				Console.Error("VK: run program stopped: %u descriptor pools in one command buffer slot are full (this group "
							  "%u textures); drawing every row classically",
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
			// the variable-count sets this program once used). The pool was never used and goes now.
			Console.Error("VK: run program stopped: an empty descriptor pool refused a set of %u textures (%s); drawing "
						  "every row classically",
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
		m_image_infos[i] = {textures[i].sampler, textures[i].view, textures[i].layout};
	const VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, count,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_image_infos.data(), nullptr, nullptr};
	vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
	return set;
}

void GSRunEmitterVK::Record(Side side, Group& g)
{
	if (!g.open)
		return;
	g.open = false;
	if (g.ndraws == 0)
		return;

	GSDeviceVK* dev = m_dev;
	// The pass the group was decided in is open: every pass end records what is pending first.
	pxAssertMsg(dev->InRenderPass() && dev->m_current_render_target == g.pass_rt && dev->m_current_depth_target == g.pass_ds &&
					dev->m_current_framebuffer_feedback_loop == GSDeviceVK::FeedbackLoopFlag_None,
		"A run group is recorded outside the pass it was decided in");
	m_recording = true;
	const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
	const VkPipeline pipeline = GetPipeline(g);
	const VkDescriptorSet set1 = WriteTextureSet(g.textures);
	if (set1 == VK_NULL_HANDLE)
	{
		// No set could be had and the run program has stopped: this group's rows are lost, once.
		g.textures.clear();
		m_recording = false;
		return;
	}
	Rings& rings = m_rings[side];
	g_perfmon.Put(GSPerfMon::PipelineSwitches, 1);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	const u32 offsets[2] = {g.header_off, g.table_off};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &rings.set0, 2, offsets);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 1, 1, &set1, 0, nullptr);
	const VkRect2D scissor = {{g.scissor.x, g.scissor.y}, {static_cast<u32>(g.scissor.width()), static_cast<u32>(g.scissor.height())}};
	vkCmdSetViewport(cmd, 0, 1, &g.viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	const bool other_vbuf = g.vbuf != dev->m_vertex_stream_buffer.GetBuffer();
	if (other_vbuf)
	{
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &g.vbuf, &zero);
	}
	vkCmdBindIndexBuffer(cmd, rings.index.GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	vkCmdDrawIndexed(cmd, g.idx_count, 1, g.idx_first, 0, 0);
	if (other_vbuf)
		dev->SetInitialState(cmd);
	dev->DirtyAfterRawDraws();
	dev->m_raw_binds.Reset(); // a held twin after the group binds everything again
	g.textures.clear();
	if (side == BASES)
		dev->m_rows->SetGroupTargets(nullptr, nullptr);
	m_recording = false;
}
