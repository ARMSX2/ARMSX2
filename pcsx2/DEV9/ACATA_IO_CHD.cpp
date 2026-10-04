// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "ACATA_IO_CHD.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"
#include "common/Console.h"
#include <cstdio>
#include <cstring>

// ARMSX2: the image is opened through FileSystem, which also reaches the content:// documents an Android
// library hands out, and given to libchdr as a core_file. From chd_open_core_file on, libchdr owns it
// and closes it (ChdCoreFile::Close), on failure as well.
namespace
{
	struct ChdCoreFile
	{
		core_file core;
		std::FILE* fp;

		static std::FILE* Fp(core_file* file) { return static_cast<ChdCoreFile*>(file->argp)->fp; }
		static u64 Size(core_file* file) { return static_cast<u64>(FileSystem::FSize64(Fp(file))); }
		static size_t Read(void* buffer, size_t size, size_t count, core_file* file) { return std::fread(buffer, size, count, Fp(file)); }
		static int Seek(core_file* file, int64_t offset, int whence) { return FileSystem::FSeek64(Fp(file), offset, whence); }
		static int Close(core_file* file)
		{
			ChdCoreFile* self = static_cast<ChdCoreFile*>(file->argp);
			std::fclose(self->fp);
			delete self;
			return 0;
		}
	};
} // namespace

ChdImage::ChdImage() = default;

ChdImage::~ChdImage()
{
    Close();
}

bool ChdImage::Open(const std::string& path, std::string* why)
{
    Close();

    Error error;
    std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb", &error);
    if (!fp) {
        Console.ErrorFmt("{} failed to open '{}': {}", __FUNCTION__, path, error.GetDescription());
        if (why)
            *why = error.GetDescription();
        return false;
    }

    ChdCoreFile* file = new ChdCoreFile{{}, fp};
    file->core.argp = file;
    file->core.fsize = &ChdCoreFile::Size;
    file->core.fread = &ChdCoreFile::Read;
    file->core.fclose = &ChdCoreFile::Close;
    file->core.fseek = &ChdCoreFile::Seek;

    chd_error err = chd_open_core_file(&file->core, CHD_OPEN_READ, nullptr, &m_chd);

    if (err != CHDERR_NONE) {
        m_chd = nullptr;
        Console.ErrorFmt("{} failed to open CHD: {}", __FUNCTION__, chd_error_string(err));
        if (why)
            *why = chd_error_string(err);
        return false;
    }

    const chd_header* hdr = chd_get_header(m_chd);

    m_hunkSize = hdr->hunkbytes;

    switch (hdr->unitbytes)
    {
        case 2048:
            m_type = ACMEDIATYPE::ACDVD;
            break;

        case 512:
            m_type = ACMEDIATYPE::ACHDD;
            break;

        default:
            m_type = ACMEDIATYPE::ACUNK;
            break;
    }

    m_unitBytes = hdr->unitbytes;
    m_totalUnits = hdr->logicalbytes / hdr->unitbytes;

    // CD units are raw frames; find where the 2048-byte payload sits (DVD/HDD keep it at 0).
    if (m_unitBytes == 2352 || m_unitBytes == 2448)
        m_frameDataOffset = DetectCdDataOffset();

    m_hunkBuffer.resize(m_hunkSize);

    DevCon.WriteLnFmt("{}: opened ok (unit {}, data offset {})", __FUNCTION__, m_unitBytes, m_frameDataOffset);
    return true;
}

void ChdImage::Close()
{
    if (m_chd)
    {
        chd_close(m_chd);
        m_chd = nullptr;
    }

    m_hunkBuffer.clear();

    m_cachedHunk = UINT32_MAX;

    m_writes.Close();

    m_hunkSize = 0;
    m_unitBytes = 0;
    m_frameDataOffset = 0;
    m_totalUnits = 0;

    m_type = ACMEDIATYPE::ACUNK;
}

bool ChdImage::IsOpen() const
{
    return m_chd != nullptr;
}

ACMEDIATYPE ChdImage::GetType() const
{
    return m_type;
}

u32 ChdImage::GetSectorSize() const
{
    // MAME CD CHDs: libchdr CD codecs already extract user data (2048) from raw sectors
    if (m_unitBytes == 2448 || m_unitBytes == 2352)
        return 2048;
    return m_unitBytes;
}

u64 ChdImage::GetSectorCount() const
{
    return m_totalUnits;
}

// CHD metadata gives the CD track format. Raw sectors (MODE1_RAW/MODE2_RAW) keep the
// sync + header before the 2048-byte data (offset 16/24); plain MODE1/MODE2 store just
// the 2048 data at offset 0 (like an ISO).
u32 ChdImage::DetectCdDataOffset()
{
    char meta[256] = {};
    u32 len = 0;
    if (chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, 0,
                         meta, sizeof(meta), &len, nullptr, nullptr) != CHDERR_NONE &&
        chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, 0,
                         meta, sizeof(meta), &len, nullptr, nullptr) != CHDERR_NONE)
        return 0;

    // Match the track type (" TYPE:"), not the pregap type ("PGTYPE:").
    if (std::strstr(meta, " TYPE:MODE2_RAW"))
        return 24;
    if (std::strstr(meta, " TYPE:MODE1_RAW"))
        return 16;
    return 0;
}

bool ChdImage::ReadHunk(u32 hunk)
{
    if (hunk == m_cachedHunk)
        return true;

    chd_error err =
        chd_read(m_chd,
                 hunk,
                 m_hunkBuffer.data());

    if (err != CHDERR_NONE)
        return false;

    m_cachedHunk = hunk;
    return true;
}

bool ChdImage::ReadSector(u64 lba, void* buffer)
{
    if (!m_chd)
        return false;

    if (lba >= m_totalUnits)
        return false;

    const u64 byteOffset = lba * m_unitBytes;

    const u32 hunk =
        static_cast<u32>(byteOffset / m_hunkSize);

    const u32 offset =
        static_cast<u32>(byteOffset % m_hunkSize);

    if ((offset + m_unitBytes) > m_hunkSize)
        return false;

    if (!ReadHunk(hunk))
        return false;

    if (m_unitBytes == 2448 || m_unitBytes == 2352)
    {
        std::memcpy(buffer, m_hunkBuffer.data() + offset + m_frameDataOffset, 2048);
    }
    else
    {
        std::memcpy(buffer, m_hunkBuffer.data() + offset, m_unitBytes);
        // ARMSX2: the sector as the game last wrote it, when it has (ChdWrites).
        m_writes.Apply(lba, 1, static_cast<u8*>(buffer));
    }

    return true;
}

void ChdImage::OpenWrites(const std::string& path)
{
    const chd_header* hdr = m_chd ? chd_get_header(m_chd) : nullptr;
    if (!hdr || path.empty() || m_type != ACMEDIATYPE::ACHDD)
        return;
    m_writes.Open(path, hdr->sha1, hdr->logicalbytes, m_unitBytes);
}

bool ChdImage::WriteSectors(u64 lba, u32 count, const void* buffer)
{
    return m_chd && m_writes.Write(lba, count, static_cast<const u8*>(buffer));
}

bool ChdImage::ReadSectors(u64 lba,
                           u32 count,
                           void* buffer)
{
    u8* dst = static_cast<u8*>(buffer);
    const u32 outBytes = (m_unitBytes == 2448 || m_unitBytes == 2352) ? 2048 : m_unitBytes;

    for (u32 i = 0; i < count; i++)
    {
        if (!ReadSector(lba + i,
                        dst + (i * outBytes)))
        {
            return false;
        }
    }

    return true;
}

bool ChdImage::IsChdFileName(const std::string& path)
{
	return StringUtil::compareNoCase(Path::GetExtension(path), "chd");
}

bool ChdImage::IsChdImage(const std::string& path)
{
	if (IsChdFileName(path))
		return true;

	// ARMSX2: a content:// URI does not always carry the file's name, but a CHD names itself in its
	// first bytes.
	const auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	char magic[8];
	return fp && std::fread(magic, sizeof(magic), 1, fp.get()) == 1 && std::memcmp(magic, "MComprHD", sizeof(magic)) == 0;
}
