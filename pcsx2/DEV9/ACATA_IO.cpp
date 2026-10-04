// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

///////////////// I/O THREAD CODE BELOW ONLY

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "fmt/format.h"

#include "ACATA.h"
#include "ACATA_IO_CHD.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if __POSIX__
#define INVALID_HANDLE_VALUE -1
#include <unistd.h>
#include <fcntl.h>
#endif

std::mutex ACATA::TH::ioMutex;
std::string ACATA::TH::open_error;
bool ACATA::TH::b_isIdle,
    ACATA::TH::ioWrite,
    ACATA::TH::ioRead,
    ACATA::TH::isCHD;
std::condition_variable ACATA::TH::Idle_cv, ACATA::TH::ioReady;
FILE* ACATA::TH::IMAGE;
s64 ACATA::TH::IMAGESIZE;
u32 ACATA::TH::sectorsize = ACATAPI::CONSTANTS::DVD_SECTORSIZE; //TODO: remove hardcode before testing HDD/CD games !
u32 ACATA::TH::unitbytes;
u32 ACATA::TH::unitdataoff;
u32 ACATA::TH::nsector;
s64 ACATA::TH::LBA;

ChdImage CHD;


// ARMSX2: the read is bounded by the DMA's transfer, not only by the sector count the game put in the
// command: a command asking for more than its DMA moves read straight on past the DMA's buffer, over the
// rest of IOP memory. And a read the image cannot supply (a truncated dump, a CHD read error) reports a
// drive error instead of abort()ing the whole app.
bool ACATA::TH::IO_Read(u32* addr, u32 size) {
	const u64 wanted = (u64)sectorsize * nsector;
	if (size != wanted)
		Console.Error("ACATA:IO_Read> the DMA moves %u bytes, the command asked for %llu (%u sectors of %u)",
			size, static_cast<unsigned long long>(wanted), nsector, sectorsize);
	const u32 bytes = static_cast<u32>(std::min<u64>(size, wanted));
	const u32 whole = sectorsize ? bytes / sectorsize : 0; // whole sectors that fit
	const u32 part = sectorsize ? bytes % sectorsize : 0;  // and the start of one more, where the DMA ends inside it
	u8* dst = reinterpret_cast<u8*>(addr);
	bool ok = true;

	if (isCHD) {
		const u32 scale = sectorsize / CHD.GetSectorSize();
		const u64 chd_lba = LBA * scale;
		if (whole && !CHD.ReadSectors(chd_lba, whole * scale, dst)) {
			Console.ErrorFmt("ACATA:IO_ReadCHD: lba:{} nsector:{} failed", chd_lba, whole * scale);
			ok = false;
		}
		if (ok && part) {
			std::vector<u8> sector(sectorsize);
			ok = CHD.ReadSectors(chd_lba + (u64)whole * scale, scale, sector.data());
			if (ok)
				std::memcpy(dst + (size_t)whole * sectorsize, sector.data(), part);
			else
				Console.ErrorFmt("ACATA:IO_ReadCHD: lba:{} failed", chd_lba + (u64)whole * scale);
		}
	} else {
		const u64 pos = (u64)LBA * sectorsize;
		if (FileSystem::FSeek64(IMAGE, pos, SEEK_SET) != 0) {
			Console.ErrorFmt("ACATA:IO_Read: failed to seek pos:{}", pos);
			ok = false;
		} else if (std::fread(dst, 1, bytes, IMAGE) != bytes) {
			Console.ErrorFmt("ACATA:IO_Read: size:{} at:{} failed (the image ends early?)", bytes, pos);
			ok = false;
		}
	}
	if (!ok)
		std::memset(dst, 0, bytes);
	{
		std::lock_guard ioSignallock(ioMutex);
		ioRead = false;
	}
	return ok;
}

void ACATA::TH::IO_Write(u32* addr, u32 size) {
	if (!isCHD) {
		const s64 lba = ACATA::TH::LBA;
		const u64 pos = lba * ACATA::TH::sectorsize;
		u64 size2 = (u64)sectorsize * nsector;
		if (size != size2)
			Console.Error("%s> mismatch %ld vs %ld", __FUNCTION__, size, size2);
		if (FileSystem::FSeek64(IMAGE, pos, SEEK_SET) != 0) {
			Console.ErrorFmt("ACATA:IO_Write: failed to seek pos:{}", pos);
			return;
		}
		if (std::fwrite(addr, sectorsize, nsector, IMAGE) != static_cast<size_t>(nsector)) {
			Console.ErrorFmt("ACATA:IO_Write: size:{} at:{} failed", size2, pos);
			return;
		}
		std::fflush(IMAGE);
	} else {
		// ARMSX2: a CHD cannot be written, and these writes used to be dropped. They are kept beside the
		// game's SRAM and read back over the CHD (ChdWrites), in this session and the next; never more
		// than the DMA brought.
		const u32 unit = CHD.GetSectorSize();
		const u32 scale = unit ? sectorsize / unit : 0;
		const u32 whole = sectorsize ? static_cast<u32>(std::min<u64>(size, static_cast<u64>(sectorsize) * nsector) / sectorsize) : 0;
		if (scale == 0 || whole == 0 || !CHD.WriteSectors(static_cast<u64>(LBA) * scale, whole * scale, addr))
			Console.ErrorFmt("{}: lba:{} sectors:{} of a CHD drive not kept", __FUNCTION__, LBA, nsector);
	}
}

static bool probe_at(FILE* f, s64 pos, u8* dst, u32 len) {
	if (FileSystem::FSeek64(f, pos, SEEK_SET) != 0)
		return false;
	return std::fread(dst, 1, len, f) == len;
}

static bool pvd_at(FILE* f, s64 pos) { //ISO9660 primary volume descriptor magic
	u8 b[6];
	return probe_at(f, pos, b, 6) && std::memcmp(b, "\x01" "CD001", 6) == 0;
}

static const u8 CD_SYNC[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

static void apply_disc_layout(u32 stride, u32 dataoff) { //IMAGESIZE becomes logical bytes, like the CHD branch
	ACATA::TH::unitbytes = stride;
	ACATA::TH::unitdataoff = dataoff;
	ACATA::TH::IMAGESIZE = (ACATA::TH::IMAGESIZE / stride) * ACATAPI::CONSTANTS::DVD_SECTORSIZE;
	Console.WriteLnFmt("ACATA: disc image has {}-byte sector units, payload at +{}", stride, dataoff);
}

// Find where the 2048-byte payload sits in a CD/DVD image; false = corrupted dump.
static bool DetectDiscImageLayout() {
	FILE* f = ACATA::TH::IMAGE;
	u8 hdr[16];
	if (!probe_at(f, 0, hdr, 16))
		return true;
	if (std::memcmp(hdr, CD_SYNC, 12) == 0) { //raw CD frames; MODE2 has an 8-byte subheader before the payload
		for (u32 stride : {2352u, 2448u}) {
			u8 next_sync[12];
			if (probe_at(f, stride, next_sync, 12) && std::memcmp(next_sync, CD_SYNC, 12) == 0) {
				apply_disc_layout(stride, (hdr[15] == 2) ? 24 : 16);
				return true;
			}
		}
	}
	if (pvd_at(f, 16 * 2048)) //plain cooked 2048 iso: the default path already fits
		return true;
	if ((ACATA::TH::IMAGESIZE % 2336) == 0 && pvd_at(f, 16 * 2336 + 8)) { //2336 Mode2 CD dump: subheader kept
		apply_disc_layout(2336, 8);
		return true;
	}
	if ((ACATA::TH::IMAGESIZE % 2064) == 0 && pvd_at(f, 16 * 2064 + 12)) { //raw DVD sectors: 12-byte ID/IED/CPR header
		apply_disc_layout(2064, 12);
		return true;
	}
	if (pvd_at(f, 16 * 2048 + 8)) {
		ACATA::TH::open_error = "This disc image is a corrupted dump (truncated sectors). A clean dump of this disc is required.";
		Console.Error("ACATA: this disc image is a corrupted dump (truncated sectors), a clean dump of this disc is required.");
		return false;
	}
	Console.Warning("ACATA: unknown disc image layout, assuming plain 2048-byte sectors");
	return true;
}

int ACATA::TH::IO_OpenImage() {
	open_error.clear();
	isCHD = ChdImage::IsChdImage(ACATA::imgpath);
	unitbytes = 0;
	unitdataoff = 0;
	if (isCHD) {
		// ARMSX2: with the reason, so a report tells a damaged file from one libchdr refuses.
		std::string why;
		if (CHD.Open(ACATA::imgpath, &why)) {
			u32 secsize = CHD.GetSectorSize();
			if (secsize != sectorsize) 
				Console.ErrorFmt("ACATA: CHD sectorsize mismatches declaration {} vs {}", secsize, sectorsize);
			sectorsize = secsize;
			ACATA::TH::IMAGESIZE = (CHD.GetSectorCount() * sectorsize);
			if (ACATA::MediaType == ACMEDIATYPE::ACHDD)
				CHD.OpenWrites(ACATA::writespath);
		} else {
			open_error = fmt::format("Cannot open the arcade game's media image '{}': {}.", ACATA::imgpath, why);
			return EIO;
		}
		Console.WriteLn("%s: CHD image opened ok", __FUNCTION__);
	} else {
		// ARMSX2: through FileSystem, which also opens an Android content:// document (read-only: a hard
		// drive image there takes no writes, see IO_Write). The size comes from the open file, which a
		// content:// URI has where a path stat() does not.
		Error error;
		const bool content_uri = ACATA::imgpath.starts_with("content://");
		ACATA::TH::IMAGE = content_uri ? nullptr : FileSystem::OpenCFile(ACATA::imgpath.c_str(), "r+b");

		if (!ACATA::TH::IMAGE)
			ACATA::TH::IMAGE = FileSystem::OpenCFile(ACATA::imgpath.c_str(), "rb", &error);

		if (!ACATA::TH::IMAGE) {
			Console.ErrorFmt("{}> fail to open '{}': {}", __FUNCTION__, ACATA::imgpath, error.GetDescription());
			return EIO;
		}
		s64 t;
		if ((t = FileSystem::FSize64(ACATA::TH::IMAGE)) > 0)
			ACATA::TH::IMAGESIZE = t;
		else {
			Console.ErrorFmt("{}> fail to get filesize: {}", __FUNCTION__, t);
			std::fclose(ACATA::TH::IMAGE);
			ACATA::TH::IMAGE = nullptr;
			return EINVAL;
		}
		if ((ACATA::MediaType == ACMEDIATYPE::ACCD || ACATA::MediaType == ACMEDIATYPE::ACDVD) && !DetectDiscImageLayout()) {
			std::fclose(ACATA::TH::IMAGE);
			ACATA::TH::IMAGE = nullptr;
			return EIO;
		}
		Console.WriteLn("%s: image opened ok", __FUNCTION__);
	}
	return 0;
}

int ACATA::TH::IO_CloseImage() {
	if (isCHD) {
		Console.WriteLn("%s CHD", __FUNCTION__);
		CHD.Close();
		isCHD = false;
		return 0;
	} else if (ACATA::TH::IMAGE) {
		Console.WriteLn("%s", __FUNCTION__);
		// ARMSX2: forget the handle, so a second close (the next session's) cannot fclose it again.
		const int ret = std::fclose(ACATA::TH::IMAGE);
		ACATA::TH::IMAGE = nullptr;
		return ret;
	}
	return EINVAL;
}