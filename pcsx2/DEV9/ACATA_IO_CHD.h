// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once

#include "common/ARCADE.h"
#include "ACATA.h"
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include <libchdr/chd.h>
}

class ChdImage
{
public:

    ChdImage();
    ~ChdImage();

    ChdImage(const ChdImage&) = delete;
    ChdImage& operator=(const ChdImage&) = delete;

    bool Open(const std::string& path);
    void Close();

    bool IsOpen() const;

    ACMEDIATYPE GetType() const;
    u32 GetSectorSize() const;
    u64 GetSectorCount() const;

    bool ReadSector(u64 lba, void* buffer);
    bool ReadSectors(u64 lba, u32 count, void* buffer);
    
    static bool IsChdFileName(const std::string& path);
    static bool IsChdImage(const std::string& path); // by name, or failing that by its header

private:
    bool ReadHunk(u32 hunk);
    u32 DetectCdDataOffset();

    chd_file* m_chd = nullptr;

    ACMEDIATYPE m_type = ACMEDIATYPE::ACUNK;

    u32 m_hunkSize = 0;
    u32 m_unitBytes = 0;
    u32 m_frameDataOffset = 0;  // where the 2048-byte payload sits inside a raw CD frame

    u64 m_totalUnits = 0;

    std::vector<u8> m_hunkBuffer;

    u32 m_cachedHunk = UINT32_MAX;
};

extern ChdImage CHD;
