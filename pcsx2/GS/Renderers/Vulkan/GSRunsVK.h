// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSCompileQueue.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "GS/Renderers/Vulkan/VKStreamBuffer.h"

#include <array>
#include <mutex>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GSDeviceVK;
class GSTextureVK;

/// The run machine. Inside a Vulkan render pass, consecutive TFX draws whose classic state differs only in what a row
/// of data can carry (texture and sampler, texture function, fog, shading, alpha test, lod source, and how the classic
/// path chose to blend inside one blend family) are drawn as one host draw under one pipeline and a run program
/// (tfx_runs.glsl) specialised on the states the run holds one value of.
///
/// It sits entirely below the hand-off from GSRendererHW: DoRenderHW asks Classify() at its top whether the draw can
/// join a run (and the open run ends there if not, before anything is recorded), and Append() at its draw point instead
/// of binding and drawing. Everything else that records into the command buffer inside a render pass ends the open run
/// first (Flush()).
///
/// Each draw samples its own texture view through its own sampler, from the run's descriptor array, with the same
/// sampling call its classic program makes, so a merged draw reads exactly the texels its classic draw would have.
class GSRunsVK
{
public:
	/// Why a run ended (each Flush says, for the reader of its call site).
	enum Why : u32
	{
		END_STATE, END_TARGET, END_SPLIT, END_PASS, END_OTHER, END_RING, END_UNMERGEABLE, END_TEXTURES,
		END_COMPILING, ///< the run's program with the next draw in it is still being built: that draw is drawn classically
	};

	/// The machine on a device with the features it needs (descriptor indexing with non-uniform sampled-image
	/// indexing, dual-source blending), nullptr elsewhere or when it is switched off for an A/B (-runs off).
	static std::unique_ptr<GSRunsVK> Create(GSDeviceVK* dev);
	~GSRunsVK();

	/// At the top of DoRenderHW: decides whether this draw can join a run, and ends the open run when it cannot or
	/// would not continue it.
	void Classify(const GSHWDrawConfig& config);
	/// The draw in hand was found able to join a run.
	bool Candidate() const { return m_draw.cand; }
	/// At DoRenderHW's draw point, for a candidate: writes the draw's vertices, indices and row, and adds it to the
	/// open run (opening one). False: the draw takes the classic path after all, nothing of it written.
	bool Append(const GSHWDrawConfig& config, u32 pipeline_feedback_flags, bool pipeline_colclip, VkSampler sampler);
	/// At the top of DoRenderHW, for a candidate: a draw that continues the open run in the open pass, whose texture
	/// needs no transition and no clear, is appended without the classic path's per-draw device work (constant
	/// buffers, bindings, pipeline selection), none of which a merged draw uses. False: the draw takes DoRenderHW's
	/// ordinary road, which may still append it.
	bool Continue(const GSHWDrawConfig& config);
	/// Records the open run's one host draw. A no-op without an open run, and while a flush is recording.
	void Flush(Why why);
	/// At every vsync, outside a render pass (so no run is open): the frame's share of draws that joined runs decides
	/// whether the next frame may open any (GSRunsPolicy::Dormancy).
	void EndFrame();

private:
	explicit GSRunsVK(GSDeviceVK* dev);
	bool Init();
	/// The index and table rings and the set naming them; made by the first draw that would join a run.
	bool CreateRings();

	struct Draw // what Classify decided for the draw in hand
	{
		bool cand = false;
		u32 flags = 0;
		u32 way = 0;
	};

	struct Texture
	{
		VkImageView view;
		VkSampler sampler;
		VkImageLayout layout;
	};

	struct Run
	{
		bool open = false;
		const GSTexture* rt = nullptr;
		const GSTexture* ds = nullptr;
		u8 dss = 0;
		u8 vs = 0;
		u8 rta = 0;
		u8 zfloor = 0;
		GSVector4i scissor;
		VkViewport viewport;
		GSHWDrawConfig::VSConstantBuffer cb_vs;
		GSVector2 st_scale;
		VkBuffer vbuf = VK_NULL_HANDLE;
		u32 header_off = 0, table_off = 0, idx_first = 0, idx_count = 0, ndraws = 0;
		u32 ways = 0;
		u32 dear = 0;
		bool nods = false; ///< the pass has no depth attachment
		u32 strict = 0; ///< GSRunsPolicy::StrictKey of the run's draws
		u64 area_total = 0; ///< the draws' draw-rectangle area in target pixels
		bool dear_mixed = false; ///< the run holds draws of more than one dear class
		GSRunsPolicy::StateSet states;
		u32 last_slot = 0;
	};


	struct FramePools // the runs' texture sets of one command buffer slot, reset when the slot is reused
	{
		std::vector<VkDescriptorPool> pools;
		u32 cursor = 0;
	};

	// The fixed state of the last draw that would have opened a run, drawn classically instead: a run opens only at the
	// second of two consecutive draws that share it. A run of one draw costs the device more than its classic draw (the
	// run's set and binds, then the classic path binding all of its state again), and a title whose fixed state changes
	// at every draw (Sly 3's depth state) would otherwise pay that at every draw.
	struct Primed
	{
		u64 draw = ~0ull; ///< m_draws when primed, or when a draw last joined a run
		bool in_run = false; ///< that draw joined a run: compare with the run's fixed state, which outlives its flush
		const GSTexture* rt = nullptr;
		const GSTexture* ds = nullptr;
		u8 dss = 0, vs = 0, rta = 0, zfloor = 0;
		u32 strict = 0; ///< never compared: a draw is primed before it is classified
		GSVector4i scissor;
		GSHWDrawConfig::VSConstantBuffer cb_vs;
		GSVector2 st_scale;
	};

	bool Matches(const GSHWDrawConfig& config) const;
	/// Whether the draw follows a primed draw or a run's last draw and shares its fixed state (strict ~0u: not compared).
	bool FollowsSame(const GSHWDrawConfig& config, u32 strict) const;
	/// Remembers the draw's fixed state: the next draw may open a run with it.
	void Prime(const GSHWDrawConfig& config);
	/// Whether the draw in hand ends the open run at a change of dear class (the device class's cost model).
	bool SplitAtDear(const GSHWDrawConfig& config) const;
	Why EndReason(const GSHWDrawConfig& config);
	u32 SpecKey() const;
	/// Any thread.
	VkShaderModule GetFragmentShader(u32 variant, u32 spec);
	/// Any thread.
	VkPipeline CreatePipeline(u8 dss, u32 variant, u32 spec);
	/// Whether the pipeline for this program is built; if it is not, asks the device's compile queue for it.
	bool ProgramReady(u8 dss, u32 variant, u32 spec);
	/// The flushing run's pipeline, built (Append grows a run only while it is).
	VkPipeline GetPipeline(bool dual);
	VkDescriptorSet WriteTextureSet();

	GSDeviceVK* m_dev;
	Draw m_draw;
	Run m_run;
	Primed m_primed;
	u64 m_draws = 0; ///< TFX draws classified
	GSRunsPolicy::Dormancy m_dormancy;
	u32 m_frame_draws = 0; ///< TFX draws of this frame seen while runs could open
	u32 m_frame_joined = 0; ///< of those, draws in runs of two or more, or held back while their run's program builds
	bool m_flushing = false;

	// The run's textures, one descriptor per distinct (view, sampler) pair, in first-use order.
	std::vector<Texture> m_textures;
	std::vector<VkDescriptorImageInfo> m_image_infos;
	u32 m_max_textures = 0;
	u64 m_sampler_key = ~0ull; ///< Continue's last sampler, so a run of draws on one sampler looks it up once
	VkSampler m_sampler = VK_NULL_HANDLE;

	std::string m_source;
	VKStreamBuffer m_index_stream;
	VKStreamBuffer m_table_stream;
	VkDescriptorSetLayout m_set0_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_set1_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorPool m_set0_pool = VK_NULL_HANDLE;
	VkDescriptorSet m_set0 = VK_NULL_HANDLE;
	bool m_rings_failed = false;
	VkShaderModule m_vs = VK_NULL_HANDLE;
	std::mutex m_fs_mutex; ///< guards m_fs, which compile-queue jobs fill
	std::unordered_map<u64, VkShaderModule> m_fs;
	std::unordered_map<u64, VkPipeline> m_pipelines; ///< GS thread only
	u32 m_compiles = 0;

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
	bool m_sets_failed = false; ///< the driver refuses the run's texture sets: every draw is drawn classically from here on
	const GSRunsPolicy::DeviceClass* m_class = &GSRunsPolicy::DEVICE_CLASS_DEFAULT;
};
