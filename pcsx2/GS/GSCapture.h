// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <string>
#include <utility>
#include <vector>

#include "common/SmallString.h"
#include "GSVector.h"

#if defined(__ANDROID__)
// The stubs below hold a ThreadHandle by value, so the forward declaration is not enough.
#include "common/Threading.h"
#endif

namespace Threading
{
class ThreadHandle;
}

class GSTexture;
class GSDownloadTexture;

namespace GSCapture
{
#if defined(__ANDROID__)
	// Video capture is not built on Android. Android records the screen itself, far better than
	// we could from in here, and for the thing capture actually gets used for -- looking at what
	// the GS did -- a gsdump opened in RenderDoc beats a video outright.
	//
	// Stubbed here rather than by touching the ~30 call sites, so this stays two files of
	// divergence from upstream instead of thirty. Every one of those sites is already guarded by
	// IsCapturing()/IsCapturingVideo()/SPU2::IsAudioCaptureActive(), so returning false makes the
	// whole path unreachable; the handful that are not guarded are teardown calls, where a no-op
	// is what they want anyway.
	//
	// This is also what keeps ffmpeg out of the build. GSCapture.cpp is the only thing that needs
	// its headers, and upstream has since deleted the in-tree copy of them (PCSX2 2.9.x), which
	// would otherwise have meant vendoring ffmpeg per-ABI for the NDK to keep compiling a feature
	// nothing on Android can reach.
	inline bool BeginCapture(float, GSVector2i, float, std::string) { return false; }
	inline bool DeliverVideoFrame(GSTexture*) { return false; }
	inline void DeliverAudioPacket(const float*) {}
	inline void EndCapture() {}
	inline bool IsCapturing() { return false; }
	inline bool IsCapturingVideo() { return false; }
	inline bool IsCapturingAudio() { return false; }
	inline TinyString GetElapsedTime() { return TinyString(); }
	// A reference, so it needs something to refer to. Default-constructed: the only caller reads
	// GetCPUTime() off it, and it never gets there because IsCapturing() is false.
	inline const Threading::ThreadHandle& GetEncoderThreadHandle()
	{
		static const Threading::ThreadHandle s_null_handle;
		return s_null_handle;
	}
	inline GSVector2i GetSize() { return GSVector2i(0, 0); }
	inline std::string GetNextCaptureFileName() { return std::string(); }
	inline void Flush() {}

	using CodecName = std::pair<std::string, std::string>;
	using CodecList = std::vector<CodecName>;
	inline CodecList GetVideoCodecList(const char*) { return CodecList(); }
	inline CodecList GetAudioCodecList(const char*) { return CodecList(); }
	using FormatName = std::pair<int, std::string>;
	using FormatList = std::vector<FormatName>;
	inline FormatList GetVideoFormatList(const char*) { return FormatList(); }
#else
	bool BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename);
	bool DeliverVideoFrame(GSTexture* stex);
	void DeliverAudioPacket(const float* frames); // AudioStream::CHUNK_SIZE
	void EndCapture();

	bool IsCapturing();
	bool IsCapturingVideo();
	bool IsCapturingAudio();
	TinyString GetElapsedTime();
	const Threading::ThreadHandle& GetEncoderThreadHandle();
	GSVector2i GetSize();
	std::string GetNextCaptureFileName();
	void Flush();

	using CodecName = std::pair<std::string, std::string>; // shortname,longname
	using CodecList = std::vector<CodecName>;
	CodecList GetVideoCodecList(const char* container);
	CodecList GetAudioCodecList(const char* container);

	using FormatName = std::pair<int, std::string>; // id,name
	using FormatList = std::vector<FormatName>;
	FormatList GetVideoFormatList(const char* codec);
#endif
}; // namespace GSCapture
