// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The device double the Tile cache tests stand up. Everything below CreateSurface is GSDevice's
// own and non-virtual — the pool, the LRU, Recycle and FetchSurface all run for real — so a test
// exercises the same Recycle-to-pool path the renderer does. Only surface creation is stubbed,
// plus the two Tile device entry points a cache calls into (they are pure device work and have
// nothing to say about the cache's decisions, which is what these tests are about).

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#include <array>
#include <memory>
#include <string>

namespace GSTileTest
{
	class StubTexture final : public GSTexture
	{
	public:
		StubTexture(Usage usage, int w, int h, int levels, Format format)
		{
			m_size = GSVector2i(w, h);
			m_mipmap_levels = levels;
			m_usage = usage;
			m_format = format;
		}

		void* GetNativeHandle() const override { return nullptr; }
		void Unmap() override {}
		void GenerateMipmap() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view) override {}
#endif

	protected:
		bool DoUpdate(const GSVector4i&, const void*, int, int) override { return true; }
		bool DoMap(GSMap&, const GSVector4i*, int) override { return false; }
	};

	class StubDevice final : public GSDevice
	{
	public:
		StubDevice() { m_max_texture_size = 16384; }
		~StubDevice() override { PurgePool(); }

		/// Flip to false to make the palette expansion pass refuse, which is how a test reaches
		/// GSTileExpandedCache's permanent Serves() shutdown.
		bool expand_serves = true;

		bool TileExpandPalette(GSTexture*, GSTexture*, GSTexture*, u32, u32) override
		{
			return expand_serves;
		}

	protected:
		GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels,
			GSTexture::Format format) override
		{
			return new StubTexture(usage, width, height, levels, format);
		}

		void DoMerge(GSTexture**, GSVector4*, GSTexture*, GSVector4*, const GSRegPMODE&, const GSRegEXTBUF&, u32,
			const Filter) override
		{
		}
		void DoInterlace(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&, ShaderInterlace, Filter,
			const InterlaceConstantBuffer&) override
		{
		}
		void DoFXAA(GSTexture*, GSTexture*) override {}
		void DoShadeBoost(GSTexture*, GSTexture*, const float[4]) override {}
		bool DoCAS(GSTexture*, GSTexture*, bool, const std::array<u32, NUM_CAS_CONSTANTS>&) override { return false; }
		void DoCopyRect(GSTexture*, GSTexture*, const GSVector4i&, u32, u32) override {}
		void DoUpdateCLUTTexture(GSTexture*, float, u32, u32, GSTexture*, u32, u32) override {}
		void DoConvertToIndexedTexture(GSTexture*, float, u32, u32, u32, u32, GSTexture*, u32, u32) override {}
		void DoFilteredDownsampleTexture(GSTexture*, GSTexture*, u32, const GSVector2i&, const GSVector4&) override {}
		void DoRenderHW(GSHWDrawConfig&) override {}
		void DoStretchRect(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&, ShaderConvertSelector,
			Filter) override
		{
		}
		PresentResult DoBeginPresent(bool) override { return PresentResult::FrameSkipped; }

	public:
		RenderAPI GetRenderAPI() const override { return RenderAPI::None; }
		bool HasSurface() const override { return false; }
		void DestroySurface() override {}
		bool UpdateWindow() override { return false; }
		void ResizeWindow(u32, u32, float) override {}
		bool SupportsExclusiveFullscreen() const override { return false; }
		void EndPresent() override {}
		void SetVSyncMode(GSVSyncMode, bool) override {}
		std::string GetDriverInfo() const override { return {}; }
		bool SetGPUTimingEnabled(bool) override { return false; }
		float GetAndResetAccumulatedGPUTime() override { return 0.0f; }
		bool SetGPUPipelineStatisticsEnabled(bool) override { return false; }
		GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override { return {}; }
		void PushDebugGroup(const char*, ...) override {}
		void PopDebugGroup() override {}
		void InsertDebugMessage(DebugMessageCategory, const char*, ...) override {}
		std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32, u32, GSTexture::Format) override
		{
			return nullptr;
		}
		void PresentRect(GSTexture*, const GSVector4&, GSTexture*, const GSVector4&, PresentShader, float,
			Filter) override
		{
		}
		void ClearSamplerCache() override {}
	};
} // namespace GSTileTest
