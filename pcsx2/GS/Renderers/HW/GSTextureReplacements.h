// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/HW/GSTextureCache.h"

#include <functional>
#include <utility>

namespace GSTextureReplacements
{
	struct ReplacementTexture
	{
		u32 width;
		u32 height;
		GSTexture::Format format;
		std::pair<u8, u8> alpha_minmax;

		u32 pitch;
		std::vector<u8> data;

		struct MipData
		{
			u32 width;
			u32 height;
			u32 pitch;
			std::vector<u8> data;
		};
		std::vector<MipData> mips;

		/// Produced by the texture upscaler rather than loaded from a pack. Its mip chain is
		/// complete, so the GPU must not regenerate it from the base level.
		bool generated = false;
	};

	void Initialize();
	void GameChanged();
	void ReloadReplacementMap();
	void UpdateConfig(Pcsx2Config::GSOptions& old_config);
	void Shutdown();

	u32 CalcMipmapLevelsForReplacement(u32 width, u32 height);

	bool HasAnyReplacementTextures();
	bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash);
	GSTexture* LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap, bool* pending, std::pair<u8, u8>* alpha_minmax);
	GSTexture* CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap);
	void ProcessAsyncLoadedTextures();

	/// Texture upscaling. A generated texture is looked up, uploaded and injected like a pack
	/// texture, and a pack texture for the same name always wins.
	///
	/// Everything here runs on the texture cache's thread except the workers, which only see data
	/// they were handed.

	/// True while a texture upscale mode is set and its filters loaded.
	bool IsUpscaleActive();

	/// Whether a texture of this size is worth upscaling: at least 8 pixels on a side, at most 1024.
	bool CanUpscaleTexture(int width, int height);

	/// What LookupUpscaledTexture needs to read the guest levels of one texture.
	struct UpscaleRequest
	{
		GSTextureCache::HashCacheKey key;
		GIFRegTEXA TEXA;
		GSTextureCache::SourceRegion region;

		/// TEX0 of each guest level to read; level_tex0[0] is the base. The native path uploads
		/// guest mips only when the lod range is set and HWMipmap is on.
		GIFRegTEX0 level_tex0[7];
		u32 guest_levels;

		/// The lod range is set, so the native texture has mips (the same value LookupReplacementTexture
		/// takes as `mipmap`).
		bool mipmap;

		/// The lod range is set but HWMipmap is off, so the native texture gets driver-generated
		/// mips. The upscaled texture gets a CPU box filtered chain from its own base level.
		bool cpu_mips;
	};

	/// Returns the upscaled texture if it is already in the replacement cache. Otherwise reads the
	/// guest levels now, queues an upscale job, and sets *pending (the caller keeps the native
	/// texture meanwhile, and the result is injected later). Returns null without *pending when
	/// nothing could be queued.
	GSTexture* LookupUpscaledTexture(const UpscaleRequest& request, GSLocalMemory& mem, bool* pending, std::pair<u8, u8>* alpha_minmax);

	struct UpscaleStats
	{
		u64 queued;
		u64 queued_4x; ///< Of those, the jobs that upscale by 4 (two 2x passes).
		u64 upscaled; ///< Jobs that finished and were kept.
		u64 injected; ///< Results swapped into the hash cache after the native texture was drawn.
		u64 cache_hits; ///< Lookups answered from the replacement cache.
		u64 dropped; ///< Jobs discarded: queue overflow, or a stale generation.
		u64 failed; ///< Jobs that threw, or whose result could not be cached.
		u64 skipped_size; ///< Lookups of textures outside the size limits.
		u64 guest_mip_jobs; ///< Queued jobs that carry guest mip levels (more than the base level).
		u64 cpu_mip_jobs; ///< Queued jobs that get a CPU built mip chain.
		u64 cpu_ns; ///< Time spent upscaling, summed over the workers.
		u64 native_draws; ///< Draws that read an upscaled texture's original texels instead (GSTexelAddressedDraw.h).
	};
	UpscaleStats GetUpscaleStats();

	/// Counts a draw that used the original texels of a texture it would otherwise have read upscaled.
	void NoteUpscaleNativeDraw();

	/// Returns once every queued upscale job and the ones running have finished. For tests.
	void SyncUpscaleWorkers();

	void DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level);
	void ClearDumpedTextureList();

	/// Get the number of textures that have been dumped.
	u32 GetDumpedTextureCount();

	/// Get the number of replacement textures that have been loaded/cached.
	u32 GetLoadedTextureCount();

	/// Loader will take a filename and interpret the format (e.g. DDS, PNG, etc).
	using ReplacementTextureLoader = bool (*)(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
	ReplacementTextureLoader GetLoader(const std::string_view filename);

	/// Load a validated KTX1 ASTC chain. Device capability is checked by the registered loader.
	bool LoadKTXTexture(const std::string& filename, ReplacementTexture* tex, u32 max_texture_size);

	/// Saves an image buffer to a PNG file (for dumping).
	bool SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch);

	/// The loader/dumper thread. Exposed for tests.
	void StartWorkerThread();
	void StopWorkerThread();
	void QueueWorkerThreadItem(std::function<void()> fn, bool high_priority);

	/// Returns once every queued job, and the one the worker is running, has finished.
	void SyncWorkerThread();
} // namespace GSTextureReplacements
