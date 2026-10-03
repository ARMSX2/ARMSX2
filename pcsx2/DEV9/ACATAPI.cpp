// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "ACCORE.h"
#include "ACATAPI.h"
#include "ACATA.h"
#include "ACATA_IO_CHD.h"
#include "common/Console.h"
#include "common/FileSystem.h"

#include "ACMACROS.h"

#include <sys/stat.h>
#include <cstring>

static u8 atapi_pio_buf[65536];
static u32 atapi_pio_len = 0;
static u32 atapi_pio_pos = 0;
static u32 atapi_pio_chunk = 0;      // max bytes sent per chunk (the byte-count register is only 16-bit)
static int atapi_pio_chunk_busy = 0; // >0 while we set up the next chunk

static u8 atapi_pio_write_buf[65536];
static u32 atapi_pio_write_len = 0;
static u32 atapi_pio_write_pos = 0;

// Instant DMA buffer: read sectors into memory immediately instead of deferring to
// the DMA callback. Avoids a race where SPU2 DMA completes before disc data is ready.
static u8 atapi_dma_buf[2048 * 128];
static u32 atapi_dma_len = 0;

static u8 mode_page_01[12] = {};
static bool mode_page_01_set = false;

// One READ_10 fetch, whatever the backing image: CHD, plain cooked file or raw-frame file.
static bool atapi_read_sectors(u32 lba, u32 nsec, u8* dst) {
    const u32 secsize = ACATAPI::CONSTANTS::DVD_SECTORSIZE;
    if (ACATA::TH::isCHD) {
        u32 scale = secsize / CHD.GetSectorSize();
        return CHD.ReadSectors((u64)lba * scale, nsec * scale, dst);
    }
    if (ACATA::TH::unitbytes == 0) {
        s64 offset = (s64)lba * secsize;
        size_t expected = (size_t)nsec * secsize;
        FileSystem::FSeek64(ACATA::TH::IMAGE, offset, SEEK_SET);
        return fread(dst, 1, expected, ACATA::TH::IMAGE) == expected;
    }
    for (u32 i = 0; i < nsec; i++) {
        s64 offset = (s64)(lba + i) * ACATA::TH::unitbytes + ACATA::TH::unitdataoff;
        FileSystem::FSeek64(ACATA::TH::IMAGE, offset, SEEK_SET);
        if (fread(dst + (size_t)i * secsize, 1, secsize, ACATA::TH::IMAGE) != secsize)
            return false;
    }
    return true;
}

void ACATAPI::Reset() { mode_page_01_set = false; }

void ACATAPI::ResetTransfers() {
    atapi_pio_len = 0;
    atapi_pio_pos = 0;
    atapi_pio_chunk = 0;
    atapi_pio_chunk_busy = 0;
    atapi_pio_write_len = 0;
    atapi_pio_write_pos = 0;
    atapi_dma_len = 0;
}

static void atapi_pio_write_setup(u32 len) {
    atapi_pio_write_len = len;
    atapi_pio_write_pos = 0;
    memset(atapi_pio_write_buf, 0, std::min(len, (u32)sizeof(atapi_pio_write_buf)));
    ACATA::R_LCYL = len & 0xFF;
    ACATA::R_HCYL = (len >> 8) & 0xFF;
    ACATA::R_NSECTOR = 0x00;
    ACATA::R_STATUS |= ATA_STAT_DRQ;
    CLRB(ACATA::R_STATUS, ATA_STAT_BUSY);
    CLRB(ACATA::R_STATUS, ATA_STAT_ERR);
    ACCORE::intr(ACCORE::INTRN_ATA);
}

static void atapi_pio_setup(u32 len) {
    atapi_pio_len = len;
    atapi_pio_pos = 0;
    u32 limit = (ACATA::R_LCYL | (ACATA::R_HCYL << 8)) & 0xFFFE; // chunk size the driver asked for
    if (limit == 0)
        limit = 0xFFFE;
    atapi_pio_chunk = limit;
    u32 chunk = std::min(len, atapi_pio_chunk);
    ACATA::R_LCYL = chunk & 0xFF;
    ACATA::R_HCYL = (chunk >> 8) & 0xFF;
    ACATA::R_NSECTOR = 0x02;
    ACATA::R_STATUS |= ATA_STAT_DRQ;
    CLRB(ACATA::R_STATUS, ATA_STAT_BUSY);
    CLRB(ACATA::R_STATUS, ATA_STAT_ERR);
    ACCORE::intr(ACCORE::INTRN_ATA);
}

static void atapi_complete_nodata() {
    atapi_pio_len = 0;
    atapi_pio_pos = 0;
    ACATA::R_NSECTOR = 0x03;
    CLRB(ACATA::R_STATUS, ATA_STAT_DRQ);
    CLRB(ACATA::R_STATUS, ATA_STAT_ERR);
    CLRB(ACATA::R_STATUS, ATA_STAT_BUSY);
    ACATA::R_STATUS |= ATA_STAT_READY;
    ACATA::R_LCYL = 0;
    ACATA::R_HCYL = 0;
    ACCORE::intr(ACCORE::INTRN_ATA);
}

// Signal command error: clears transfer state and sets R_NSECTOR=0x03 (I/O=1, C/D=1)
// to indicate the device is ready for a new command after an error.
static void atapi_complete_error() {
    atapi_pio_len = 0;
    atapi_pio_pos = 0;
    ACATA::R_NSECTOR = 0x03;
    CLRB(ACATA::R_STATUS, ATA_STAT_DRQ);
    CLRB(ACATA::R_STATUS, ATA_STAT_BUSY);
    ACATA::R_STATUS |= ATA_STAT_READY;
    ACATA::R_LCYL = 0;
    ACATA::R_HCYL = 0;
    ACCORE::intr(ACCORE::INTRN_ATA);
}

u16 ACATAPI::pio_read_word() {
    if (atapi_pio_pos * 2 < atapi_pio_len) {
        u32 offset = atapi_pio_pos * 2;
        u16 val = atapi_pio_buf[offset];
        if (offset + 1 < atapi_pio_len)
            val |= (u16)atapi_pio_buf[offset + 1] << 8;
        atapi_pio_pos++;
        u32 bytepos = atapi_pio_pos * 2;
        if (bytepos >= atapi_pio_len) {
            ACATA::R_NSECTOR = 0x03;
            CLRB(ACATA::R_STATUS, ATA_STAT_DRQ);
            ACATA::R_STATUS |= ATA_STAT_READY;
            ACCORE::intr(ACCORE::INTRN_ATA);
        } else if (atapi_pio_chunk && (bytepos % atapi_pio_chunk) == 0) { // end of a chunk: set up the next one
            CLRB(ACATA::R_STATUS, ATA_STAT_DRQ);
            ACATA::R_STATUS |= ATA_STAT_BUSY;
            atapi_pio_chunk_busy = 1;
            ACCORE::intr(ACCORE::INTRN_ATA);
        }
        return val;
    }
    return 0;
}

bool ACATAPI::has_pio_data() {
    return atapi_pio_chunk_busy == 0 && atapi_pio_len > 0 && atapi_pio_pos * 2 < atapi_pio_len;
}

// Between chunks: stay busy for a few status polls, then offer the next chunk.
void ACATAPI::chunk_poll() {
    if (atapi_pio_chunk_busy == 0)
        return;
    if (atapi_pio_chunk_busy++ < 3) // wait a few polls before offering the next chunk
        return;
    atapi_pio_chunk_busy = 0;
    u32 next = std::min(atapi_pio_len - atapi_pio_pos * 2, atapi_pio_chunk);
    ACATA::R_LCYL = next & 0xFF;
    ACATA::R_HCYL = (next >> 8) & 0xFF;
    ACATA::R_NSECTOR = 0x02;
    CLRB(ACATA::R_STATUS, ATA_STAT_BUSY);
    ACATA::R_STATUS |= ATA_STAT_DRQ;
    ACCORE::intr(ACCORE::INTRN_ATA);
}

void ACATAPI::pio_write_word(u16 val) {
    if (atapi_pio_write_pos * 2 >= atapi_pio_write_len)
        return;
    u32 offset = atapi_pio_write_pos * 2;
    atapi_pio_write_buf[offset] = val & 0xFF;
    if (offset + 1 < atapi_pio_write_len)
        atapi_pio_write_buf[offset + 1] = (val >> 8) & 0xFF;
    atapi_pio_write_pos++;
    if (atapi_pio_write_pos * 2 >= atapi_pio_write_len) {
        if (atapi_pio_write_len >= 12 && atapi_pio_write_buf[8] == 0x01) {
            memcpy(mode_page_01, atapi_pio_write_buf, 12);
            mode_page_01_set = true;
        }
        atapi_pio_write_len = 0;
        atapi_complete_nodata();
    }
}

bool ACATAPI::has_pio_write() {
    return atapi_pio_write_len > 0 && atapi_pio_write_pos * 2 < atapi_pio_write_len;
}

bool ACATAPI::dma_read(u32* pMem, int size) {
    if (atapi_dma_len == 0)
        return false;
    u32 copy = std::min(atapi_dma_len, (u32)size);
    memcpy(pMem, atapi_dma_buf, copy);
    atapi_dma_len = 0;
    return true;
}

void ACATAPI::handle_cmd(atapi_packet_t P) {
    u32 transf_lba = ATAPI_PKT_GETLBA(P);
    u32 nsec = ATAPI_PKT_GETLEN(P);
    switch (P.pkt.opcode) {
    case ATAPICMD::TEST_UNIT_READY:
        atapi_complete_nodata();
        break;

    case ATAPICMD::INQUIRY: {
        ACATA_LOG("ATAPI INQUIRY");
        // The disc driver sends INQUIRY to check the drive is a CD/DVD-ROM before it
        // mounts "cdrom:". Answer as a DVD-ROM drive or the mount never happens.
        u8 alloc_len = P.raw8[4];
        memset(atapi_pio_buf, 0, 36);
        atapi_pio_buf[0] = 0x05; // peripheral device type: CD/DVD-ROM
        atapi_pio_buf[1] = 0x80; // removable medium
        atapi_pio_buf[3] = 0x21; // response data format
        atapi_pio_buf[4] = 0x1F; // additional length: 31 more bytes -> 36 total
        memcpy(&atapi_pio_buf[8],  "LECTORA ", 8);
        memcpy(&atapi_pio_buf[16], "DE MIERDA       ", 16);
        memcpy(&atapi_pio_buf[32], "1.00", 4);
        u32 resp_len = 36;
        if (alloc_len > 0 && alloc_len < resp_len)
            resp_len = alloc_len;
        atapi_pio_setup(resp_len);
        break;
    }

    case ATAPICMD::READ_CAPACITY: {
        if ((ACATA::TH::IMAGE || ACATA::TH::isCHD) && ACATA::TH::IMAGESIZE > 0) {
            u32 last_lba = (u32)(ACATA::TH::IMAGESIZE / ACATAPI::CONSTANTS::DVD_SECTORSIZE) - 1;
            u32 block_len = ACATAPI::CONSTANTS::DVD_SECTORSIZE;
            atapi_pio_buf[0] = (last_lba >> 24) & 0xFF;
            atapi_pio_buf[1] = (last_lba >> 16) & 0xFF;
            atapi_pio_buf[2] = (last_lba >>  8) & 0xFF;
            atapi_pio_buf[3] = last_lba & 0xFF;
            atapi_pio_buf[4] = (block_len >> 24) & 0xFF;
            atapi_pio_buf[5] = (block_len >> 16) & 0xFF;
            atapi_pio_buf[6] = (block_len >>  8) & 0xFF;
            atapi_pio_buf[7] = block_len & 0xFF;
            atapi_pio_setup(8);
        } else {
            Console.Error("ACATAPI:READ_CAPACITY: image not open or size invalid");
            ACATA::R_STATUS |= ATA_STAT_ERR;
            ACATA::R_ERROR = ATA_ERR_ABORT;
        }
        break;
    }

    case ATAPICMD::MODE_SENSE: {
        u8 page_code = P.raw8[2] & 0x3F;
        u16 alloc_len = (P.raw8[7] << 8) | P.raw8[8];
        memset(atapi_pio_buf, 0, 64);
        u32 resp_len = 0;
        if (page_code == 0x01) {
            resp_len = 20;
            if (mode_page_01_set) {
                memcpy(atapi_pio_buf, mode_page_01, 12);
            } else {
                atapi_pio_buf[0] = 0x00; atapi_pio_buf[1] = 0x12;
                atapi_pio_buf[2] = (ACATA::MediaType == ACMEDIATYPE::ACDVD) ? 0x41 :
                                   (ACATA::MediaType == ACMEDIATYPE::ACCD)  ? 0x01 : 0x00;
                atapi_pio_buf[8] = 0x01; atapi_pio_buf[9] = 0x0A;
                atapi_pio_buf[11] = 0x05;
            }
        } else if (page_code == 0x2A) {
            resp_len = 28;
            atapi_pio_buf[0] = 0x00; atapi_pio_buf[1] = 0x1A;
            atapi_pio_buf[8] = 0x2A; atapi_pio_buf[9] = 0x12;
            atapi_pio_buf[10] = 0x03;
            atapi_pio_buf[16] = 0x15; atapi_pio_buf[17] = 0xA4;
            atapi_pio_buf[22] = 0x15; atapi_pio_buf[23] = 0xA4;
        } else {
            resp_len = 8;
            atapi_pio_buf[0] = 0x00; atapi_pio_buf[1] = 0x06;
        }
        if (alloc_len > 0 && alloc_len < resp_len)
            resp_len = alloc_len;
        atapi_pio_setup(resp_len);
        break;
    }

    case ATAPICMD::READ_10: {
        if ((!ACATA::TH::IMAGE && !ACATA::TH::isCHD) || nsec == 0) {
            if (nsec == 0) {
                // Not an error per the ATAPI spec; CD games spam it while streaming, so log only the first few.
                static int nsec0_count = 0;
                if (nsec0_count++ < 3)
                    Console.Warning("ACATAPI:READ_10: zero sectors requested (LBA:%X)", transf_lba);
            } else {
                Console.Error("ACATAPI:READ_10: no image open");
                ACATA::R_STATUS |= ATA_STAT_ERR;
                ACATA::R_ERROR = ATA_ERR_ABORT;
            }
            atapi_complete_nodata();
            break;
        }
        ACATA_LOG("ATAPI READ_10 lba:%08X sectors:%02X dma:%d", transf_lba, nsec, ACATA_ISDMA);
        if (ACATA_ISDMA) {
            u32 total = nsec * ACATAPI::CONSTANTS::DVD_SECTORSIZE;
            bool ok = (total <= sizeof(atapi_dma_buf)) && atapi_read_sectors(transf_lba, nsec, atapi_dma_buf);
            if (ok) {
                atapi_dma_len = total;
                atapi_complete_nodata();
            } else {
                Console.Error("ACATAPI:READ_10 DMA: read failed lba %u, %u sectors", transf_lba, nsec);
                ACATA::R_STATUS |= ATA_STAT_ERR;
                ACATA::R_ERROR = ATA_ERR_ABORT;
                atapi_complete_error();
            }
        } else {
            u32 total = nsec * ACATAPI::CONSTANTS::DVD_SECTORSIZE;
            if (total > sizeof(atapi_pio_buf)) {
                Console.Error("ACATAPI:READ_10: transfer too large (%u bytes)", total);
                ACATA::R_STATUS |= ATA_STAT_ERR;
                ACATA::R_ERROR = ATA_ERR_ABORT;
                atapi_complete_error();
                break;
            }
            bool ok = atapi_read_sectors(transf_lba, nsec, atapi_pio_buf);
            if (ok) {
                atapi_pio_setup(total);
            } else {
                Console.Error("ACATAPI:READ_10: read failed at lba %u, %u sectors", transf_lba, nsec);
                ACATA::R_STATUS |= ATA_STAT_ERR;
                ACATA::R_ERROR = ATA_ERR_ABORT;
                atapi_complete_error();
            }
        }
        break;
    }

    case ATAPICMD::MODE_SELECT: {
        u16 param_len = ATAPI_PKT_GETLEN(P);
        if (param_len > 0) {
            atapi_pio_write_setup(param_len);
        } else {
            atapi_complete_nodata();
        }
        break;
    }

    case ATAPICMD::SET_CD_SPEED:
        atapi_complete_nodata();
        break;

    case ATAPICMD::SET_STREAMING: {
        u16 param_len = (P.raw8[9] << 8) | P.raw8[10];
        if (param_len > 0) {
            atapi_pio_write_setup(param_len);
        } else {
            atapi_complete_nodata();
        }
        break;
    }

    default:
        Console.Error("ACATAPI: UNK_CMD %02X, lba:%08X, nsec:%04X", P.raw8[0], transf_lba, nsec);
        atapi_complete_nodata();
        break;
    }
}

void ACATAPI::Setup() {
    u32 SectorSizes[3] = {/*TODO: CHECK CD SECTOR SIZE*/0, ACATAPI::CONSTANTS::DVD_SECTORSIZE, 512};
    ACATA::TH::sectorsize = SectorSizes[ACATA::MediaType];
}
