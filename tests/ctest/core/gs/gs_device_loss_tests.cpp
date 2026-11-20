// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// After a device loss, GSreopen destroys the device and creates a new one under the same renderer. The
// renderer must hand back everything it holds from the old device first: a texture or readback buffer
// kept across the swap is later used with the new device, which does not own it. These tests run the
// renderer's part of that path (GSRenderer::ReleaseDeviceObjects), then the device part as GSreopen does
// it, and check that afterwards the new device is only ever handed objects it created.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/Common/GSFieldShiftDetector.h"
#include "GS/Renderers/Common/GSFieldShiftPolicy.h"

#include <unordered_set>

using namespace GSHWDrawHarness;

namespace
{
	class TrackingDevice;

	/// A readback buffer that knows which device made it, and counts a copy recorded while another
	/// device is current.
	class TrackedDownload final : public GSDownloadTexture
	{
	public:
		TrackedDownload(TrackingDevice* owner, u32 width, u32 height, GSTexture::Format format)
			: GSDownloadTexture(width, height, format)
			, m_owner(owner)
		{
			m_buffer.resize(GetBufferSize(width, height, format));
		}

		bool Map(const GSVector4i& read_rc) override
		{
			m_map_pointer = m_buffer.data();
			return true;
		}
		void Unmap() override { m_map_pointer = nullptr; }
		void Flush() override {}
#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view name) override {}
#endif

	protected:
		void DoCopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level,
			bool use_transfer_pitch) override;

	private:
		TrackingDevice* m_owner;
		std::vector<u8> m_buffer;
	};

	/// The None backend, remembering every surface it creates and counting textures and readback
	/// buffers it is handed that it did not create.
	class TrackingDevice final : public CaptureDevice
	{
	public:
		std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override
		{
			m_downloads_made++;
			return std::make_unique<TrackedDownload>(this, width, height, format);
		}

		void Use(const GSTexture* t)
		{
			if (t && !m_made.count(t))
				m_foreign_uses++;
		}

		std::unordered_set<const GSTexture*> m_made;
		u32 m_downloads_made = 0;
		u32 m_stretches = 0;
		u32 m_foreign_uses = 0;

	protected:
		using GSDeviceNone::DoStretchRect;

		GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override
		{
			GSTexture* t = CaptureDevice::CreateSurface(usage, width, height, levels, format);
			m_made.insert(t);
			return t;
		}

		void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
			ShaderConvertSelector shader, Filter filter) override
		{
			m_stretches++;
			Use(sTex);
			Use(dTex);
			CaptureDevice::DoStretchRect(sTex, sRect, dTex, dRect, shader, filter);
		}
	};

	void TrackedDownload::DoCopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
	{
		TrackingDevice* const current = static_cast<TrackingDevice*>(g_gs_device.get());
		if (current != m_owner)
			current->m_foreign_uses++;
		current->Use(stex);
		m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, 1);
		m_needs_flush = false;
	}

	class DeviceLoss : public Fixture
	{
	protected:
		std::unique_ptr<CaptureDevice> NewDevice() override { return std::make_unique<TrackingDevice>(); }

		void TearDown() override
		{
			Fixture::TearDown();
			// Kept until now so no object of the new device can take the old device's address.
			m_old_device.reset();
		}

		TrackingDevice* Device() const { return static_cast<TrackingDevice*>(g_gs_device.get()); }

		/// What GSreopen(true, false, ...) does: the renderer releases what it holds, the old device is
		/// cleared, purged and destroyed, and a new device takes its place.
		void LoseDevice()
		{
			g_gs_renderer->ReleaseDeviceObjects();
			g_gs_device->ClearCurrent();
			g_gs_device->PurgePool();
			g_gs_device->Destroy();
			m_old_device = std::move(g_gs_device);

			g_gs_device = std::make_unique<TrackingDevice>();
			ASSERT_TRUE(g_gs_device->Create(GSVSyncMode::Disabled, false));
			m_device = Device();
		}

		void FeedFields(GSTexture* merge, int fields)
		{
			GSFieldShiftDetector& detector = m_gs->GetFieldShiftDetector();
			for (int i = 0; i < fields; i++, m_field++)
				detector.Update(merge, GSVector2i(1280, 896), 2, 0, m_field & 1);
		}

		// An untextured sprite over a 640x448 frame at block 0, so the texture cache holds a colour target
		// drawn since its last read.
		void DrawFrame()
		{
			Packet p;
			Environment(p, 0, PSMCT32);
			p.Vertex(0, 0, 0, 0, 0);
			p.Vertex(640 << 4, 448 << 4, 0, 0, 0);
			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		std::unique_ptr<GSDevice> m_old_device;
		int m_field = 0;
	};
} // namespace

// The field-shift detector keeps its probe target between rounds. A round that starts after the swap
// must not draw into the old device's probe.
TEST_F(DeviceLoss, TheFieldShiftDetectorProbesOnlyWithTheNewDevicesTextures)
{
	BringUp();

	GSTexture* merge = g_gs_device->CreateRenderTarget(1280, 896, GSTexture::Format::Color, false);
	ASSERT_NE(merge, nullptr);
	for (int i = 0; i < 64 && m_gs->GetFieldShiftDetector().IsProbing(); i++)
		FeedFields(merge, 1);
	ASSERT_FALSE(m_gs->GetFieldShiftDetector().IsProbing()) << "no decision within 64 fields";
	ASSERT_GT(Device()->m_stretches, 0u) << "the detector never probed";
	g_gs_device->Recycle(merge);

	LoseDevice();

	merge = g_gs_device->CreateRenderTarget(1280, 896, GSTexture::Format::Color, false);
	ASSERT_NE(merge, nullptr);
	FeedFields(merge, GS_FIELD_SHIFT_RECHECK_FIELDS + 64);
	g_gs_device->Recycle(merge);

	EXPECT_GT(Device()->m_stretches, 0u) << "no probe round ran on the new device";
	EXPECT_EQ(Device()->m_foreign_uses, 0u) << "the new device was handed the old device's objects";
}

// The texture cache keeps its readback buffers between reads. A read after the swap must not copy into
// a buffer of the old device.
TEST_F(DeviceLoss, ATextureCacheReadUsesOnlyTheNewDevicesReadbackBuffers)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	GSConfig.HWDownloadMode = GSHardwareDownloadMode::Enabled;
	BringUp();

	DrawFrame();
	g_gs_renderer->ReadbackTextureCache();
	ASSERT_GT(Device()->m_downloads_made, 0u) << "the read did not reach a readback buffer";

	LoseDevice();

	DrawFrame();
	g_gs_renderer->ReadbackTextureCache();

	EXPECT_EQ(Device()->m_foreign_uses, 0u) << "the new device was handed the old device's objects";
}
