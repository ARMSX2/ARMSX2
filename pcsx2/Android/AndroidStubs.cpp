// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Stubs for functionality not available/needed on Android.

#include "PrecompiledHeader.h"

#include "CDVD/CDVDdiscReader.h"

#include "common/FileSystem.h"
#include "common/HostSys.h"

// g_host_hotkeys / Host::SetMouseLock moved to AndroidHostStubs.cpp: they are
// FRONTEND definitions (emucore's JNI frontend has none; pcsx2-gsrunner has its
// own), and keeping them in this member made every Android frontend that links
// libpcsx2.a collide with them when this object was pulled for the disc stubs.

#ifdef ENABLE_LIBRETRO

// The APK's JNI layer (platforms/android/.../native-lib.cpp) implements these,
// and the libretro core is linked without it, so the Android core build ended
// on undefined symbols. Each bridges to something the app owns and the core
// does not have: the Storage Access Framework file it was handed, the app's own
// directories, the notification sound. The core reaches its files through the
// frontend's VFS instead, and the frontend owns audio.
//
// Nothing here can be reached with a frontend VFS installed - FileSystem tries
// that first in every one of these paths - and without one, each returned
// failure lands in the ordinary errno path rather than a hard stop.
//
// onPadRumble WAS stubbed here too. It is gone: InputManager now takes the
// libretro core down its own rumble path (Host::SetPadVibration, wired to the
// frontend's retro_rumble_interface) instead of the JNI one, so nothing
// declares it in this build and a stub would only be a symbol nobody names.

int FileSystem::OpenFDFileContent(const char* filename)
{
	return -1;
}

bool FileSystem::CreateDirectoryViaJava(const char* path)
{
	return false;
}

bool FileSystem::CreateFileViaJava(const char* path)
{
	return false;
}

bool Common::PlaySoundAsync(const char* path)
{
	return false;
}

#endif

// HTTPDownloader::Create is now provided by common/HTTPDownloaderAndroid.cpp,
// which bridges to java.net.HttpURLConnection via JNI. The stub that
// returned null lived here previously — RA login + cover downloads
// silently failed and the cleanup path crashed when the unique_ptr was
// dereferenced. See HTTPDownloaderAndroid.{h,cpp}.

// Optical drive / disc reader stubs - no physical disc on Android
std::vector<std::string> GetOpticalDriveList()
{
	return {};
}

void GetValidDrive(std::string& drive)
{
}

// IOCtlSrc stubs
IOCtlSrc::IOCtlSrc(std::string filename)
{
}

IOCtlSrc::~IOCtlSrc()
{
}

bool IOCtlSrc::Reopen(Error* error)
{
	return false;
}

bool IOCtlSrc::DiscReady()
{
	return false;
}

u32 IOCtlSrc::GetSectorCount() const
{
	return 0;
}

s32 IOCtlSrc::GetMediaType() const
{
	return 0;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	static const std::vector<toc_entry> empty;
	return empty;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	return false;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
	return false;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return 0;
}
