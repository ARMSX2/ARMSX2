// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once

/**
 * @file ACATAPI.h
 * ATAPI related code for the ACATA interface
 */

#include "MemoryTypes.h"
#include "common/Pcsx2Types.h"
#include "common/Pcsx2Defs.h"

#define ATAPI_PKT_GETLBA(P) ((P.raw8[2] << 24) | (P.raw8[3] << 16) | (P.raw8[4] << 8) | (P.raw8[5]))
#define ATAPI_PKT_GETLEN(P) ((P.raw8[7] << 8) | P.raw8[8])
typedef union {
    u8  raw8[12];
    u16 raw[6];
    u32 raw32[3];
    struct {
        u8 opcode;
        u8 control0;
        u16 lba_high; // use ATAPI_PKT_GETLBA to get us
        u16 lba_lo;   // use ATAPI_PKT_GETLBA to get us
        u8 resv;
        u16 transf_len; // use ATAPI_PKT_GETLEN to get me
        u8 control1;
        u8 control2;
        u8 unknown;
    }pkt;
}atapi_packet_t; // helper for atapi packets

namespace ACATAPI
{
    void handle_cmd(atapi_packet_t P);
    void Reset();
    void ResetTransfers(); // no PIO or DMA transfer left half done (the MODE SENSE page stays)
    void Setup(); // change ACATA stuff to handle CDROM instead of HDD
    u16 pio_read_word();
    bool has_pio_data();
    void chunk_poll();
    void pio_write_word(u16 val);
    bool has_pio_write();
    bool dma_read(u32* pMem, int size);

    enum CONSTANTS {
        DVD_SECTORSIZE = 0x800,
    };
}


enum ATAPICMD {
    TEST_UNIT_READY = 0x00,
    REQUEST_SENSE = 0x03,
    FORMAT_UNIT = 0x04,
    INQUIRY = 0x12,
    START_STOP_UNIT_EJECT_DEVICE = 0x1B,
    PREVENT_ALLOW_MEDIUM_REMOVAL = 0x1E,
    READ_FORMAT_CAPACITIES = 0x23,
    READ_CAPACITY = 0x25,
    READ_10 = 0x28, // Standard read of logical blocks (512 bytes or 2048 bytes).
    WRITE_10 = 0x2A,
    SEEK_10 = 0x2B,
    WRITE_AND_VERIFY_10 = 0x2E,
    VERIFY_10 = 0x2F,
    SYNCHRONIZE_CACHE = 0x35,
    WRITE_BUFFER = 0x3B,
    READ_BUFFER = 0x3C,
    READ_TOC__PMA__ATIP = 0x43,
    GET_CONFIGURATION = 0x46,
    GET_EVENT_STATUS_NOTIFICATION = 0x4A,
    READ_DISC_INFORMATION = 0x51,
    READ_TRACK_INFORMATION = 0x52,
    RESERVE_TRACK = 0x53,
    SEND_OPC_INFORMATION = 0x54,
    MODE_SELECT = 0x55,
    REPAIR_TRACK = 0x58,
    MODE_SENSE = 0x5A,
    CLOSE_TRACK_SESSION = 0x5B,
    READ_BUFFER_CAPACITY = 0x5C,
    SEND_CUE_SHEET = 0x5D,
    REPORT_LUNS = 0xA0,
    BLANK = 0xA1,
    SECURITY_PROTOCOL_IN = 0xA2,
    SEND_KEY = 0xA3,
    REPORT_KEY = 0xA4,
    LOAD_UNLOAD_MEDIUM = 0xA6,
    SET_READ_AHEAD = 0xA7,
    READ_12 = 0xA8, // Extended read for larger capacities.
    WRITE_12 = 0xAA,
    //READ MEDIA SERIAL NUMBER / SERVICE ACTION IN (12) =	0xAB / 0x01,
    GET_PERFORMANCE = 0xAC,
    READ_DISC_STRUCTURE = 0xAD,
    SECURITY_PROTOCOL_OUT = 0xB5,
    SET_STREAMING = 0xB6,
    READ_CD_MSF = 0xB9,
    SET_CD_SPEED = 0xBB,
    MECHANISM_STATUS = 0xBD,
    READ_CD = 0xBE,
    SEND_DISC_STRUCTURE = 0xBF,
};
