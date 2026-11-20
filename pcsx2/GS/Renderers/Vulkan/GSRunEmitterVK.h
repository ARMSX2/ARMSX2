// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSCompileQueue.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "GS/Renderers/Vulkan/VKStreamBuffer.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class GSDeviceVK;
class GSTextureVK;

/// The row emitter's Vulkan backend (GSRowEmitter.h has the order). Rows are decided and written when they arrive, and
/// recorded later with raw Vulkan calls, never through the device's cached setters:
///
/// - A row on the bases' side that the run program can draw joins the open group (one host draw of the run program,
///   tfx_runs.glsl) or opens one once its fixed-state class has a long enough streak (GSRunsPolicy::Streaks); its
///   vertices, indices and 32-byte row go into the rings at once. Anything else is drawn classically by the device,
///   after the open group is recorded.
/// - A held twin joins or opens the twins' open group the same way, into rings of its own, or is written as a held
///   classic draw: its vertices, indices and constants go into the device's rings at once and its record keeps the
///   pipeline, textures and offsets to record it with.
///
/// A group or held draw belongs to the pass it was decided in. Every pass end records everything pending first
/// (GSDeviceVK::EndRenderPass), so that pass is the one open when it is recorded; a submit ends the pass first. After
/// recording, the device's cached state is marked dirty, so its next classic draw binds all of its own again.
///
/// Each row of a group samples its own texture view through its own sampler, from the group's descriptor array, with
/// the same sampling call its classic program makes, so a merged row reads exactly the texels its classic draw would.
class GSRunEmitterVK
{
public:
	enum Side : u32
	{
		BASES = 0,
		TWINS = 1,
	};

	/// What a held classic draw is recorded with.
	struct HeldClassic
	{
		VkPipeline pipeline;
		VkDescriptorSet ubo_set; ///< the device's constants set when written (a grown vertex ring replaces it)
		u32 ubo[2];
		GSTextureVK* tex;
		VkSampler sampler;
		GSTextureVK* pal;
		VkDescriptorSet set1; ///< allocated where the device does not push descriptors
		GSVector4i scissor;
		VkViewport viewport;
		bool blend_constant;
		u8 blend_constant_color;
		bool fixed_expand; ///< sprites expanded in the vertex program: the fixed index buffer, the base vertex pushed
		bool index_range; ///< the driver takes the draw's index range (vkCmdSetIndexRangeMALISX2)
		u16 index_min, index_max;
		VkBuffer vbuf;
		u32 vstart, istart, icount;
		const GSTexture* pass_rt;
		const GSTexture* pass_ds;
	};

	/// The emitter, with the run program where the device has its features (descriptor indexing with non-uniform
	/// sampled-image indexing, dual-source blending) and it is not switched off for an A/B (-runs off). Without it no
	/// group forms: rows are drawn classically and twins are still held.
	explicit GSRunEmitterVK(GSDeviceVK* dev);
	~GSRunEmitterVK();

	bool HasRunProgram() const { return m_run_program; }

	enum class Join : u8
	{
		Joined,    ///< the row is in a group
		Classic,   ///< the row is drawn (or held) classically
		Submitted, ///< a ring was full: the command buffer was submitted, everything pending was recorded first
	};
	/// A plain row on `side`: joins the side's open group or opens one, if the run program can draw it
	/// (GSDeviceVK::RowFitsRunProgram, asked only when a group is open or would open). On the bases' side an open group the
	/// row cannot join is recorded; on the twins' side it stays held, closed.
	Join JoinGroup(Side side, const GSHWDrawConfig& config);
	/// A row on `side` that cannot join any group ends the side's streak.
	void Break(Side side) { m_streaks.Break(side); }
	/// A draw that is not a row: rows after it are not consecutive with earlier ones.
	void BreakAll()
	{
		m_streaks.Break(BASES);
		m_streaks.Break(TWINS);
	}

	/// A held classic draw for the device to write: kept in arrival order with the twins' groups.
	HeldClassic* AddHeldClassic();

	/// Whether anything is pending: the bases' open group or a held twin.
	bool AnyPending() const { return m_bases.open || !m_held.empty(); }
	/// Records the bases' open group, if there is one.
	void RecordOpen();
	/// Records the bases' open group, then every held twin in arrival order.
	void RecordAll();
	/// Forgets everything pending without recording it.
	void Drop();

	/// Rows that joined groups, groups recorded, and held classic draws recorded, per side, since the process started.
	static u64 s_rows[2], s_groups[2], s_held_classic;

private:
	struct Texture
	{
		VkImageView view;
		VkSampler sampler;
		VkImageLayout layout;
	};

	/// One run-program host draw.
	struct Group
	{
		bool open = false;
		u64 cls = 0; ///< the fixed-state class (the streak's key)
		const GSTexture* rt = nullptr;
		const GSTexture* ds = nullptr;
		const GSTexture* pass_rt = nullptr; ///< the pass it was decided in
		const GSTexture* pass_ds = nullptr;
		u8 dss = 0;
		u16 vs = 0;
		u8 rta = 0;
		u8 zfloor = 0;
		bool nods = false; ///< the pass has no depth attachment
		GSVector4i scissor;
		VkViewport viewport;
		GSHWDrawConfig::VSConstantBuffer cb_vs;
		GSVector2 st_scale;
		VkBuffer vbuf = VK_NULL_HANDLE;
		u32 header_off = 0, table_off = 0, idx_first = 0, idx_count = 0, ndraws = 0;
		u32 ways = 0;
		u32 dear = 0;
		bool dear_mixed = false; ///< the group holds rows of more than one dear class
		u32 strict = 0; ///< GSRunsPolicy::StrictKey of its rows
		u32 lod = GSRunsPolicy::LOD_ANY; ///< the lod field of its textured rows, where they must share one (LodJoins)
		u64 area_total = 0; ///< its rows' draw-rectangle area in target pixels
		GSRunsPolicy::StateSet states;
		u32 last_slot = 0;
		u64 sampler_slots = 0; ///< where the run program fetches: the slots it samples through the sampler (PlaceRow)
		u32 sampler_count = 0;
		u32 fetched = 0; ///< rows of the group that fetch
		u32 last_flags = ~0u; ///< the state flags of the last row joined: a row with the same ones changes no program
		std::vector<Texture> textures; ///< one per distinct (view, sampler) pair, in first-use order
	};

	/// A side's index and table rings, and the set naming its table ring. The twins' groups stay open while bases are
	/// written, so each side keeps its own rings to keep each group's ranges unbroken.
	struct Rings
	{
		VKStreamBuffer index;
		VKStreamBuffer table;
		VkDescriptorPool set0_pool = VK_NULL_HANDLE;
		VkDescriptorSet set0 = VK_NULL_HANDLE;
		bool failed = false;
	};

	struct FramePools // the groups' texture sets of one command buffer slot, reset when the slot is reused
	{
		std::vector<VkDescriptorPool> pools;
		u32 cursor = 0;
	};

	/// A held twin, in arrival order: a group of the twins' side or a held classic draw.
	struct Held
	{
		bool group;
		u32 index;
	};

	bool Init();
	/// The side's rings and the set that names them; made by the first row that would join a group there.
	bool CreateRings(Side side);
	Group* OpenGroup(Side side);
	/// Ends the side's open group: the bases' is recorded, the twins' stays held.
	void Close(Side side);
	void Record(Side side, Group& g);
	bool Fits(const Group& g, const GSHWDrawConfig& config, const GSRunsPolicy::Draw& d) const;
	bool SplitAtDear(const Group& g, const GSHWDrawConfig& config, const GSRunsPolicy::Draw& d) const;
	bool ProgramReady(u8 dss, u32 variant, u32 spec);
	/// Any thread.
	VkShaderModule GetFragmentShader(u32 variant, u32 spec);
	/// Any thread.
	VkPipeline CreatePipeline(u8 dss, u32 variant, u32 spec);
	VkPipeline GetPipeline(const Group& g);
	VkDescriptorSet WriteTextureSet(std::vector<Texture>& textures);

	GSDeviceVK* m_dev;
	bool m_run_program = false;
	GSRunsPolicy::Streaks m_streaks;
	u32 m_open_rows = GSRunsPolicy::INTERIM_OPENING_ROWS; ///< B
	bool m_recording = false;

	Group m_bases;
	std::vector<Group> m_twin_groups; ///< the twins' groups in use, held; m_twin_open the open one
	u32 m_twin_groups_used = 0;
	u32 m_twin_open = ~0u;
	std::vector<HeldClassic> m_classic;
	u32 m_classic_used = 0;
	std::vector<Held> m_held;

	std::vector<VkDescriptorImageInfo> m_image_infos;
	u32 m_max_textures = 0;

	std::string m_source;
	Rings m_rings[2];
	VkDescriptorSetLayout m_set0_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set1_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkShaderModule m_vs = VK_NULL_HANDLE;
	std::mutex m_fs_mutex; ///< guards m_fs, which compile-queue jobs fill
	std::unordered_map<u64, VkShaderModule> m_fs;
	std::unordered_map<u64, VkPipeline> m_pipelines; ///< GS thread only

	// Run programs being built on the device's compile queue, by pipeline key. GS thread only; the job writes only its
	// result, which the GS thread reads after the queue says the job ran.
	struct Result
	{
		VkPipeline pipeline = VK_NULL_HANDLE;
	};
	struct Pending
	{
		GSCompileQueue::Ticket ticket;
		std::shared_ptr<Result> result;
	};
	std::unordered_map<u64, Pending> m_pending;
	std::vector<FramePools> m_frame_pools;
	u64 m_pool_fence = 0;
	bool m_sets_failed = false; ///< the driver refuses the groups' texture sets: no group forms from here on
	const GSRunsPolicy::DeviceClass* m_class = &GSRunsPolicy::DEVICE_CLASS_DEFAULT;
	bool m_one_call = false; ///< the driver has DriverBug::BrokenMixedSamplingDerivatives (GSRunsPolicy::LodJoins)
	bool m_fetch = false; ///< the driver has DriverBug::StallsOnFilteredRunSampling (GSRunsPolicy::FetchBits)
};
