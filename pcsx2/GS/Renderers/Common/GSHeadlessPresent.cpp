// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSHeadlessPresent.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <memory>

namespace GSHeadless
{
	namespace
	{
		std::atomic<bool> s_enabled{false};
		std::atomic<u64> s_present_count{0};

		// GS thread only. Not owned.
		GSTexture* s_presented = nullptr;
	} // namespace

	void SetEnabled(bool enabled)
	{
		s_enabled.store(enabled, std::memory_order_relaxed);
	}

	bool IsEnabled()
	{
		return s_enabled.load(std::memory_order_relaxed);
	}

	void NotePresented(GSTexture* backbuffer)
	{
		s_presented = backbuffer;
		s_present_count.fetch_add(1, std::memory_order_release);
	}

	void ForgetPresented()
	{
		s_presented = nullptr;
	}

	u64 PresentCount()
	{
		return s_present_count.load(std::memory_order_acquire);
	}

	bool CaptureFreshFrame(Frame* out, Error* error)
	{
		if (!IsEnabled())
		{
			Error::SetStringView(error, "headless present is not enabled (start the host with --headless WxH)");
			return false;
		}
		if (!g_gs_device)
		{
			Error::SetStringView(error, "the GS is not open");
			return false;
		}

		// A present of our own, so the texture below is the one this call just drew. The idle
		// loop would have kept it current anyway, but between its last frame and now the window
		// may have been resized (which retires the old backbuffer) or a page opened.
		const u64 before = PresentCount();
		s_presented = nullptr;
		GSPresentCurrentFrame();
		if (PresentCount() == before || !s_presented)
		{
			Error::SetStringView(error,
				"the renderer did not present an offscreen frame (only the Vulkan and OpenGL backends support headless present)");
			return false;
		}

		GSTexture* const tex = s_presented;
		const int width = tex->GetWidth();
		const int height = tex->GetHeight();
		std::unique_ptr<GSDownloadTexture> dl(
			g_gs_device->CreateDownloadTexture(width, height, GSTexture::Format::Color));
		if (!dl)
		{
			Error::SetStringView(error, "could not create a download texture for the readback");
			return false;
		}

		const GSVector4i rc(0, 0, width, height);
		dl->CopyFromTexture(rc, tex, rc, 0);
		dl->Flush();

		out->width = static_cast<u32>(width);
		out->height = static_cast<u32>(height);
		out->pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
		if (!dl->ReadTexels(rc, out->pixels.data(), static_cast<u32>(width) * sizeof(u32)))
		{
			Error::SetStringView(error, "could not map the readback texture");
			return false;
		}

		// OpenGL stores a texture with row 0 at the bottom of the picture, the same convention
		// that makes it present with a lower-left origin.
		if (g_gs_device->UsesLowerLeftOrigin())
		{
			const size_t w = static_cast<size_t>(width);
			for (size_t y = 0; y < static_cast<size_t>(height) / 2; y++)
				std::swap_ranges(out->pixels.begin() + y * w, out->pixels.begin() + (y + 1) * w,
					out->pixels.begin() + (static_cast<size_t>(height) - 1 - y) * w);
		}

		// The present clears to alpha 1 but blending the UI over it leaves whatever the blend
		// equation produced; a screenshot is opaque.
		for (u32& px : out->pixels)
			px |= 0xFF000000u;

		return true;
	}

	bool CaptureFreshFrameToPNG(const std::string& path, u32* width, u32* height, Error* error)
	{
		if (!StringUtil::EndsWithNoCase(path, ".png"))
		{
			Error::SetStringView(error, "the output path must end in .png");
			return false;
		}

		Frame frame;
		if (!CaptureFreshFrame(&frame, error))
			return false;

		const std::string dir(Path::GetDirectory(path));
		if (!dir.empty() && !FileSystem::EnsureDirectoryExists(dir.c_str(), true, error))
			return false;

		// ".part.png" so SaveToFile still sees a png extension.
		const std::string partial = path.substr(0, path.size() - 4) + ".part.png";
		*width = frame.width;
		*height = frame.height;
		RGBA8Image image;
		image.SetPixels(frame.width, frame.height, std::move(frame.pixels));
		if (!image.SaveToFile(partial.c_str()))
		{
			Error::SetStringFmt(error, "could not write '{}'", partial);
			return false;
		}
		if (!FileSystem::RenamePath(partial.c_str(), path.c_str(), error))
		{
			FileSystem::DeleteFilePath(partial.c_str());
			return false;
		}
		return true;
	}
} // namespace GSHeadless
