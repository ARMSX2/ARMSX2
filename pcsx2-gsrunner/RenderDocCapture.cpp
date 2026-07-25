// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "RenderDocCapture.h"

#include "common/Console.h"

#include "fmt/format.h"

#include "renderdoc_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace
{
#if defined(_WIN32)
	static constexpr const char* RDOC_LIBRARY_NAME = "renderdoc.dll";
#elif defined(__APPLE__)
	static constexpr const char* RDOC_LIBRARY_NAME = "librenderdoc.dylib";
#elif defined(__ANDROID__)
	static constexpr const char* RDOC_LIBRARY_NAME = "libVkLayer_GLES_RenderDoc.so";
#else
	static constexpr const char* RDOC_LIBRARY_NAME = "librenderdoc.so";
#endif

	RENDERDOC_API_1_4_2* s_api = nullptr;

	// Armed dump frame range, [s_start_frame, s_end_frame).
	u32 s_start_frame = 0;
	u32 s_end_frame = 0;

	bool s_capturing = false;
	bool s_finished = false;

	// Initialize() runs while command line arguments are still being applied, and
	// gsrunner's Console output does not reach the terminal until later in startup.
	// Setup diagnostics therefore go straight to stderr, as the usage text does.
	template <typename... T>
	void ReportSetup(fmt::format_string<T...> fmt, T&&... args)
	{
		const std::string msg = fmt::format(fmt, std::forward<T>(args)...);
		std::fprintf(stderr, "(RenderDoc) %s\n", msg.c_str());
	}

	pRENDERDOC_GetAPI LoadGetAPI()
	{
#ifdef _WIN32
		// Injected by the RenderDoc UI is the common case.
		HMODULE mod = GetModuleHandleA(RDOC_LIBRARY_NAME);
		if (!mod)
			mod = LoadLibraryA(RDOC_LIBRARY_NAME);
		if (!mod)
		{
			ReportSetup("Failed to load {}.", RDOC_LIBRARY_NAME);
			return nullptr;
		}

		return reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
		// Launched from qrenderdoc / renderdoccmd: already in the process, with its
		// graphics-API hooks installed.
		void* mod = dlopen(RDOC_LIBRARY_NAME, RTLD_NOW | RTLD_NOLOAD);
		if (!mod)
		{
			// RenderDoc installs its graphics-API hooks from the library constructor
			// as the process starts. dlopen()ing it now would hand back a working API
			// object whose hooks were never installed -- StartFrameCapture() then
			// succeeds and captures nothing. Refuse, and say how to preload it.
			//
			// Distributions drop librenderdoc.so in a private directory that is not on
			// the loader search path, so locate it to give a copy-pasteable command.
			static constexpr const char* SEARCH_PATHS[] = {
				"/usr/lib64/renderdoc/librenderdoc.so",
				"/usr/lib/renderdoc/librenderdoc.so",
				"/usr/lib/x86_64-linux-gnu/renderdoc/librenderdoc.so",
				"/usr/lib/aarch64-linux-gnu/renderdoc/librenderdoc.so",
				"/usr/local/lib/renderdoc/librenderdoc.so",
			};

			const char* found = nullptr;
			for (const char* path : SEARCH_PATHS)
			{
				if (access(path, R_OK) == 0)
				{
					found = path;
					break;
				}
			}

			if (found)
			{
				ReportSetup("RenderDoc is not loaded into this process, and loading it now would be too late for "
							"its hooks. Re-run with:\n    LD_PRELOAD={} <same command>\nor launch gsrunner from "
							"qrenderdoc / renderdoccmd.",
					found);
			}
			else
			{
				ReportSetup("RenderDoc is not loaded into this process and {} was not found. Install RenderDoc, then "
							"re-run with LD_PRELOAD pointing at it, or launch gsrunner from qrenderdoc / renderdoccmd.",
					RDOC_LIBRARY_NAME);
			}

			return nullptr;
		}

		return reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
#endif
	}

	void LogNewestCapture(u32 frame_number)
	{
		const u32 count = s_api->GetNumCaptures();
		if (count == 0)
		{
			Console.WarningFmt("(RenderDoc) Closed the capture for dump frame {}, but RenderDoc reported no capture file.",
				frame_number);
			return;
		}

		u32 path_length = 0;
		if (!s_api->GetCapture(count - 1, nullptr, &path_length, nullptr) || path_length == 0)
			return;

		// path_length counts the terminating null.
		std::string path(path_length, '\0');
		if (!s_api->GetCapture(count - 1, path.data(), &path_length, nullptr))
			return;

		path.resize(std::strlen(path.c_str()));
		Console.WriteLnFmt("(RenderDoc) Captured dump frame {} to {}", frame_number, path);
		// Console output is easy to lose in a dump replay's log; this is the one line
		// the caller actually came for.
		std::fprintf(stderr, "(RenderDoc) Captured dump frame %u to %s\n", frame_number, path.c_str());
	}
} // namespace

bool RenderDocCapture::Initialize(const std::string& path_template, u32 start_frame, u32 num_frames)
{
	if (num_frames == 0)
	{
		ReportSetup("Frame count must be at least 1.");
		return false;
	}

	pRENDERDOC_GetAPI get_api = LoadGetAPI();
	if (!get_api)
		return false;

	if (!get_api(eRENDERDOC_API_Version_1_4_2, reinterpret_cast<void**>(&s_api)) || !s_api)
	{
		ReportSetup("RENDERDOC_GetAPI() rejected API version 1.4.2.");
		s_api = nullptr;
		return false;
	}

	// A capture opens at the present boundary *before* the frame it covers, so frame
	// 0 has no boundary to open on.
	if (start_frame == 0)
	{
		ReportSetup("Dump frame 0 has no preceding present to open a capture on; starting at frame 1.");
		start_frame = 1;
	}

	s_start_frame = start_frame;
	s_end_frame = start_frame + num_frames;
	s_capturing = false;
	s_finished = false;

	if (!path_template.empty())
		s_api->SetCaptureFilePathTemplate(path_template.c_str());

	// gsrunner decides when to capture. Leaving the key trigger armed would let a
	// stray keypress inject an extra capture, and it does nothing on Wayland anyway.
	s_api->SetCaptureKeys(nullptr, 0);

	const char* tmpl = path_template.empty() ? s_api->GetCaptureFilePathTemplate() : path_template.c_str();
	if (num_frames > 1)
		ReportSetup("Capturing dump frames {}-{} to {}_frameN.rdc", s_start_frame, s_end_frame - 1, tmpl);
	else
		ReportSetup("Capturing dump frame {} to {}_frameN.rdc", s_start_frame, tmpl);

	return true;
}

void RenderDocCapture::OnPresentFrame(u32 frame_number)
{
	if (!s_api || s_finished)
		return;

	if (s_capturing)
	{
		// The work this capture was opened for is now submitted. The frame's present
		// itself falls outside the capture; the draws are what matter, and closing
		// here keeps it to one .rdc per dump frame.
		s_capturing = false;
		if (s_api->EndFrameCapture(nullptr, nullptr))
			LogNewestCapture(frame_number);
		else
			Console.ErrorFmt("(RenderDoc) EndFrameCapture() failed for dump frame {}.", frame_number);

		if ((frame_number + 1) >= s_end_frame)
		{
			s_finished = true;
			return;
		}
	}

	const u32 next_frame = frame_number + 1;
	if (next_frame >= s_start_frame && next_frame < s_end_frame)
	{
		s_api->StartFrameCapture(nullptr, nullptr);
		s_capturing = true;
	}
}

void RenderDocCapture::Shutdown()
{
	if (s_api && !s_finished)
	{
		std::fprintf(stderr,
			"(RenderDoc) Playback ended before dump frame %u was reached; no capture was written. "
			"Pick an earlier frame, or raise -loop.\n",
			s_capturing ? (s_end_frame - 1) : s_start_frame);
	}

	s_api = nullptr;
	s_capturing = false;
}
