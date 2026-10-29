// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "ACSRAM.h"
#include "IopMem.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "ACMACROS.h"

u8 ACSRAM::buffer[ACSRAM_MAX_SIZE];
u8 compbuf[ACSRAM_MAX_SIZE] = {0};
std::string ACSRAM::filepath = "";
bool LockSave = false; // set when an existing SRAM image could not be read; blocks saving so it is not overwritten


// ARMSX2: reads through FileSystem (whose create path works on Android's emulated storage) and
// closes the file after a good read too.
// LockSave is decided afresh on every call: it is set only when an existing SRAM file could not be
// read in full or opened, so WriteFile() never overwrites it with the blank buffer. Saving is allowed
// over a whole SRAM read, or where there is no file yet.
int ACSRAM::ReadFile() {
    Error error;
    LockSave = false;
    FILE* FD = FileSystem::OpenCFile(ACSRAM::filepath.c_str(), "rb", &error);
    if (FD) {
        // Unlike PS2 NVRAM, there is no generic damage/validity check for ACSRAM: it is per-game, and
        // not every game checks that its data is valid. Real hardware has the same problem: such games
        // can misbehave on a machine whose ACSRAM holds another game's payload.
        if (std::fread(compbuf, sizeof(compbuf), 1, FD) == 1) {
            std::memcpy(ACSRAM::buffer, compbuf, ACSRAM_MAX_SIZE);
            Console.WriteLn(Color_StrongCyan, "%-16s OK", __FUNCTION__);
            std::fclose(FD);
            return 1;
        } else {
            Console.ErrorFmt("ACSRAM: Could not read all the data. locking save");
            LockSave = true;
        }
	    std::fclose(FD);
    } else {
        Console.ErrorFmt("ACSRAM: Could not open input image file '{}': {}", ACSRAM::filepath, error.GetDescription());
        // A file that is there but would not open is kept as it is too.
        LockSave = FileSystem::FileExists(ACSRAM::filepath.c_str());
    }
    Console.Warning("ACSRAM: failed to read SRAM. Preparing blank buffer");
    ACSRAM::Clear(0x0);
    return 0;
}

int ACSRAM::WriteFile() {
    if (LockSave) {
        Console.Warning("ACSRAM: skipping sram save to preserve existing image");
        return 0;
    }
	Error error;
    if (ACSRAM::filepath.empty())
        return 0;
    FILE* FD = FileSystem::OpenCFile(ACSRAM::filepath.c_str(), "r+b");
    if (!FD) {
        FileSystem::CreateDirectoryPath(std::string(Path::GetDirectory(ACSRAM::filepath)).c_str(), true);
        FD = FileSystem::OpenCFile(ACSRAM::filepath.c_str(), "w+b", &error);
        if (!FD) {
            Console.ErrorFmt("ACSRAM: Could not open output image file '{}': {}", ACSRAM::filepath, error.GetDescription());
            return EIO;
        }
    }

	if (std::fread(compbuf, sizeof(compbuf), 1, FD) == 1 &&
		(std::memcmp(compbuf, ACSRAM::buffer, ACSRAM_MAX_SIZE) == 0)) {
		DEV_LOG("ACSRAM has not changed, skip writing to disk.");
	    std::fclose(FD);
		return 0;
	}

	if (FileSystem::FSeek64(FD, 0, SEEK_SET) == 0 &&
		std::fwrite(ACSRAM::buffer, ACSRAM_MAX_SIZE, 1, FD) == 1) {
		INFO_LOG("ACSRAM saved to {}.", Path::GetFileName(ACSRAM::filepath));
	}
	else
	{
	    std::fclose(FD);
		ERROR_LOG("Failed to save ACSRAM to {}: {}", Path::GetFileName(ACSRAM::filepath), strerror(errno));
        return EIO;
    }
	std::fclose(FD);
    return 0;
}

#define OOB_REPORT(T) Console.Error("%s: out of bound index: %08X", __FUNCTION__, T);
#define GET_SRAM_OFF(t) ((t - ACSRAM_ADDR_BASE)/2) // u8 buffer on u16 MMIO, halve the address to get real offset

void ACSRAM::Clear(u8 fillerbyte) {
    std::memset(ACSRAM::buffer, fillerbyte, sizeof(ACSRAM::buffer));
}

u8 ACSRAM::Read8(u32 addr) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("read8  [%04X]:%02X", T, ACSRAM::buffer[T]);
        return ACSRAM::buffer[T];
    } else OOB_REPORT(T);
    return 0;
}

u16 ACSRAM::Read16(u32 addr) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("read16 [%04X]:%02X", T, ACSRAM::buffer[T]);
        return ACSRAM::buffer[T];
    } else OOB_REPORT(T);
    return 0;
}

void ACSRAM::Write16(u32 addr, u16 val) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("write16 [%04X]=%02X", T, val);
        ACSRAM::buffer[T] = val;
    } else OOB_REPORT(T);
}
