// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#include "ACATA.h"
#include "ACATAPI.h"
#include "ACCORE.h"
#include "common/Console.h"
#include "common/FileSystem.h"

#include "ACMACROS.h"

#include <sys/stat.h>
#include <cstring>

#include "ACATA_CMD_RESPONSES"

static u8 ata_pio_buf[131072];
static u32 ata_pio_len = 0;
static u32 ata_pio_pos = 0;
static u16 ata_identify_buf[256];

static u32 ata_total_sectors = 0;

static void ata_build_identify()
{
	memset(ata_identify_buf, 0, sizeof(ata_identify_buf));
	ata_identify_buf[0] = 0x0040;
	for (int i = 10; i <= 19; i++) ata_identify_buf[i] = 0x2020;
	ata_identify_buf[23] = 0x312E; ata_identify_buf[24] = 0x3030;
	ata_identify_buf[25] = 0x2020; ata_identify_buf[26] = 0x2020;
	ata_identify_buf[27] = 0x4E41; ata_identify_buf[28] = 0x4D43;
	ata_identify_buf[29] = 0x4F20; ata_identify_buf[30] = 0x4844;
	ata_identify_buf[31] = 0x4420; ata_identify_buf[32] = 0x4452;
	ata_identify_buf[33] = 0x4956; ata_identify_buf[34] = 0x4520;
	for (int i = 35; i <= 46; i++) ata_identify_buf[i] = 0x2020;
	ata_identify_buf[47] = 0x8010;
	ata_identify_buf[49] = 0x0300;
	ata_identify_buf[53] = 0x0007;
	if (ACATA::TH::IMAGE) {
		if (ACATA::TH::IMAGESIZE > 0) {
			ata_total_sectors = (u32)(ACATA::TH::IMAGESIZE / ATA_SECTORSIZE);
			ata_identify_buf[60] = ata_total_sectors & 0xFFFF;
			ata_identify_buf[61] = (ata_total_sectors >> 16) & 0xFFFF;
		}
	}
	ata_identify_buf[63] = 0x0407;
	ata_identify_buf[80] = 0x001E;
	ata_identify_buf[82] = 0x0068;
	ata_identify_buf[83] = 0x4000;
	ata_identify_buf[84] = 0x4000;
	ata_identify_buf[85] = 0x0068;
	ata_identify_buf[86] = 0x0000;
	ata_identify_buf[87] = 0x4000;
	ata_identify_buf[88] = 0x001F;
	ata_identify_buf[128] = 0x0001;
}

std::string ACATA::imgpath = "";
ACMEDIATYPE ACATA::MediaType;

u32 ACATA::last_read = 0;
u32 ACATA::last_write = 0;
u32 atacmd_responding = -1;
u32 atacmd_response_traverse = 0;


u16 ACATA::read16(u32 addr) {
    last_read = addr;
    switch (addr)
    {
    case ACATA_R_DATA:
        return ACATA::handle_dataR(addr);
    case ACATA_R_STATUS_ALT:
        ACATAPI::chunk_poll();
        return R_STATUS;
    case ACATA_R_NSECTOR:
        return R_NSECTOR;
    case ACATA_R_SECTOR:
        return R_SECTOR;
    case ACATA_R_ERROR:
        return R_ERROR;
    case ACATA_R_SELECT:
        return R_SELECT;
    case ACATA_R_LCYL:
        return R_LCYL;
    case ACATA_R_HCYL:
        return R_HCYL;
    case ACATA_R_STATUS:
        ACATAPI::chunk_poll();
        // reading R_STATUS after writing zero to it? this is the ACATA probe. we have to respond BUSY at least once for the driver to keep going
        // FIXME
        if (ACATA::last_write == ACATA_R_STATUS && ACATA::cmd_handled == 0 && ((R_SELECT & ACATA_UNIT1) == 0)) {
            if (R_STATUS & ATA_STAT_BUSY)
                CLRB(R_STATUS, ATA_STAT_BUSY);
            else
                R_STATUS |= ATA_STAT_BUSY;
        }
        return R_STATUS;
    
    default:
    	Console.Error("%-16s %08X: ", "ACATA::read16", addr);
        break;
    }
    return 0;
}

void ACATA::write16(u32 addr, u16 val) {
    last_write = addr;
    switch (addr) {
    case ACATA_R_NSECTOR: R_NSECTOR = val & 0xFF; break;
    case ACATA_R_SECTOR:  R_SECTOR  = val & 0xFF; break;
    case ACATA_R_FEATURE: R_FEATURE = val & 0xFF; break;
    case ACATA_R_CONTROL: R_CONTROL = val & 0xFF; break;
    case ACATA_R_LCYL:    R_LCYL    = val & 0xFF; break;
    case ACATA_R_HCYL:    R_HCYL    = val & 0xFF; break;
    case ACATA_R_SELECT:  R_SELECT  = val & 0xFF; break;
    case ACATA_R_COMMAND:
        // uyjulian says: "Yeah it should be masked and high byte is command type"
        ACATA::handle_cmd(val & 0xFF); return;
    
    case ACATA_R_DATA:
        ACATA::handle_dataW(addr, val);
    break;

    default:
    	Console.Error("%-16s %08X = %04X", "ACATA::write16", addr, val);
        break;
    }
}

u32 ACATA::cmd_handled;
u32 ACATA::cmd_handledc;
atapi_packet_t ACATA::ata_c_packet;

void ACATA::handle_dataW(u32 addr, u16 val) { // writes at R_DATA
    switch (ACATA::cmd_handled) {
    case ATA_C_PACKET:
        if (ACATA::cmd_handledc < 6) { // packet is followed by a 6 word PIO
            ACATA::ata_c_packet.raw[ACATA::cmd_handledc++] = val;
            if (ACATA::cmd_handledc == 6) {
                CLRB(R_STATUS, ATA_STAT_DRQ); // keep up DRQ only while the packet comes in?
                ACATAPI::handle_cmd(ACATA::ata_c_packet);
            }
        } else if (ACATAPI::has_pio_write()) {
            ACATAPI::pio_write_word(val);
        }
        break;

    default:
        Console.Error("ACATA: writing from %X while no pending CMD", addr);
        break;
    }
}

u16 ACATA::handle_dataR(u32 addr) { // PIO read at R_DATA
    switch (ACATA::cmd_handled) {
    case ATA_C_IDENTIFY_PACKET_DEVICE:
        if (ACATA::cmd_handledc < 256) {
            u16 word = ATA_R_IDENTIFY_PACKET_DEVICE[ACATA::cmd_handledc++];
            if (ACATA::cmd_handledc >= 256) {
                ACATA::cmd_handled = -1;
                CLRB(R_STATUS, ATA_STAT_DRQ);
                R_STATUS |= ATA_STAT_READY;
                // Tell the guest the read finished, or it waits here forever and never
                // sends its first ATAPI command.
                ACCORE::intr(ACCORE::INTRN_ATA);
            }
            return word;
        }
        break;
    case ATA_C_IDENTIFY_DEVICE:
        if (ACATA::cmd_handledc < 256) {
            u16 word = ata_identify_buf[ACATA::cmd_handledc++];
            if (ACATA::cmd_handledc >= 256) {
                ACATA::cmd_handled = -1;
                CLRB(R_STATUS, ATA_STAT_DRQ);
                R_STATUS |= ATA_STAT_READY;
                ACCORE::intr(ACCORE::INTRN_ATA);
            }
            return word;
        }
        break;
    case ATA_C_READ_SECTOR:
        if (ata_pio_pos * 2 < ata_pio_len) {
            u32 off = ata_pio_pos * 2;
            u16 val = ata_pio_buf[off] | ((u16)ata_pio_buf[off + 1] << 8);
            ata_pio_pos++;
            if (ata_pio_pos * 2 >= ata_pio_len) {
                ACATA::cmd_handled = -1;
                CLRB(R_STATUS, ATA_STAT_DRQ);
                R_STATUS |= ATA_STAT_READY;
                ACCORE::intr(ACCORE::INTRN_ATA);
            }
            return val;
        }
        break;
    case ATA_C_SET_FEATURES:
    break;
    case ATA_C_SCE_SECURITY_CONTROL:
        if (ACATA::cmd_handledc < 256) {
            u32 off = ACATA::cmd_handledc * 2;
            u16 word = ata_pio_buf[off] | ((u16)ata_pio_buf[off + 1] << 8);
            ACATA::cmd_handledc++;
            if (ACATA::cmd_handledc >= 256) {
                ACATA::cmd_handled = -1;
                CLRB(R_STATUS, ATA_STAT_DRQ);
                R_STATUS |= ATA_STAT_READY;
                ACCORE::intr(ACCORE::INTRN_ATA);
            }
            return word;
        }
        break;
    case ATA_C_PACKET:
        if (ACATAPI::has_pio_data())
            return ACATAPI::pio_read_word();
        break;
    
    default:
        Console.Error("ACATA: reading from %X while no pending CMD", ACATA_R_DATA);
    }
    return R_DATA;
}

void ACATA::handle_cmd(u16 val) {
    CLRB(R_STATUS, ATA_STAT_DRQ);
    switch (val) {
    case ATA_C_NOP:
        ACATA::cmd_handled = val;
        // the only situation when we will recieve a 0 (twice) to this reg, is during ACATA init.
        // here, the ata busy flag must be present at least once, on the following checks to this addr
        break;
    case ATA_C_IDENTIFY_PACKET_DEVICE:
        ACATA_LOG("ATA_C_IDENTIFY_PACKET_DEVICE");
        ACATA::cmd_handled = val;
        ACATA::cmd_handledc = 0;
        R_STATUS |= ATA_STAT_DRQ;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_PACKET:
        ACATA::cmd_handled = val;
        ACATA::cmd_handledc = 0;
        R_NSECTOR = 0x01; // ATAPI: Command/Data=1, IO=0 -> ready for command packet
        R_STATUS |= ATA_STAT_DRQ;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS_ALT, ATA_STAT_ERR); // when packet is sent, ACCDVD fails if this bit is set or timeout consumed
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_IDENTIFY_DEVICE:
        ACATA_LOG("ATA_C_IDENTIFY_DEVICE");
        ata_build_identify();
        ACATA::cmd_handled = val;
        ACATA::cmd_handledc = 0;
        R_STATUS |= ATA_STAT_DRQ;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_READ_SECTOR: {
        u32 lba = R_SECTOR | (R_LCYL << 8) | (R_HCYL << 16) | ((R_SELECT & 0x0F) << 24);
        u32 count = R_NSECTOR ? R_NSECTOR : 256;
        u32 total = count * ATA_SECTORSIZE;
        ACATA_LOG("CMD:ATA_C_READ_SECTOR lba:%08X sectors:%02X", lba, count);
        if (!ACATA::TH::IMAGE || total > sizeof(ata_pio_buf)) {
            R_STATUS |= ATA_STAT_ERR;
            R_ERROR = ATA_ERR_ABORT;
            ACCORE::intr(ACCORE::INTRN_ATA);
            break;
        }
        FileSystem::FSeek64(ACATA::TH::IMAGE, (s64)lba * ATA_SECTORSIZE, SEEK_SET);
        size_t rd = fread(ata_pio_buf, 1, total, ACATA::TH::IMAGE);
        if (rd != total) {
            Console.Error("ATA_C_READ_SECTOR: short read (%zu/%u) at LBA %u", rd, total, lba);
            R_STATUS |= ATA_STAT_ERR;
            R_ERROR = ATA_ERR_ABORT;
            ACCORE::intr(ACCORE::INTRN_ATA);
            break;
        }
        ACATA::cmd_handled = val;
        ACATA::cmd_handledc = 0;
        ata_pio_len = total;
        ata_pio_pos = 0;
        R_STATUS |= ATA_STAT_DRQ;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        CLRB(R_STATUS, ATA_STAT_ERR);
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    }
    case ATA_C_READ_DMA:
    case ATA_C_READ_DMA_WITHOUT_RETRIES: {
        u32 lba = R_SECTOR | (R_LCYL << 8) | (R_HCYL << 16) | ((R_SELECT & 0x0F) << 24);
        u32 count = R_NSECTOR ? R_NSECTOR : 256;
        ACATA::TH::LBA = lba;
        ACATA::TH::nsector = count;
        ACATA::TH::sectorsize = ATA_SECTORSIZE;
        ACCORE::DMA::PendTrasnfType = ACCORE::DMA::ATA;
        ACATA::cmd_handled = val;
        R_STATUS |= ATA_STAT_BUSY;
        CLRB(R_STATUS, ATA_STAT_DRQ);
        ACATA_LOG("CMD:%s lba:%08X sectors:%02X", 
            (val == ATA_C_READ_DMA) ? "ATA_C_READ_DMA" : "ATA_C_READ_DMA_WITHOUT_RETRIES", lba, count);
        break;
    }
    case ATA_C_SET_FEATURES:
        ACATA_LOG("ATA_C_SET_FEATURES: FEATURE:%04X, R_NSECTOR:%04X, R_SECTOR:%04X, R_LCYL:%04X, R_HCYL:%04X",
            R_FEATURE, R_NSECTOR, R_SECTOR, R_LCYL, R_HCYL);
        CLRB(R_STATUS, ATA_STAT_ERR);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_WRITE_DMA:
    case ATA_C_WRITE_DMA_WITHOUT_RETRIES: {
        u32 lba = R_SECTOR | (R_LCYL << 8) | (R_HCYL << 16) | ((R_SELECT & 0x0F) << 24);
        u32 count = R_NSECTOR ? R_NSECTOR : 256;
        ACATA::TH::LBA = lba;
        ACATA::TH::nsector = count;
        ACATA::TH::sectorsize = ATA_SECTORSIZE;
        ACCORE::DMA::PendTrasnfType = ACCORE::DMA::ATA_WRITE;
        ACATA::cmd_handled = val;
        R_STATUS |= ATA_STAT_BUSY;
        CLRB(R_STATUS, ATA_STAT_DRQ);
        ACATA_LOG("CMD:%s lba:%08X sectors:%02X", 
            (val == ATA_C_WRITE_DMA) ? "ATA_C_WRITE_DMA" : "ATA_C_WRITE_DMA_WITHOUT_RETRIES", lba, count);
        break;
    }
    case ATA_C_SMART:
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    case ATA_C_IDLE:
    case ATA_C_IDLE_IMMEDIATE:
    case ATA_C_STANDBY:
    case ATA_C_STANDBY_IMMEDIATE:
    case ATA_C_SLEEP:
    case ATA_C_CHECK_POWER_MODE:
        R_NSECTOR = 0xFF;
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    case ATA_C_SCE_SECURITY_CONTROL:
        if (R_FEATURE == 0xEC) {
            memset(ata_pio_buf, 0, 512);
            ata_pio_len = 512;
            ata_pio_pos = 0;
            ACATA::cmd_handled = val;
            ACATA::cmd_handledc = 0;
            R_STATUS |= ATA_STAT_DRQ;
            CLRB(R_STATUS, ATA_STAT_BUSY);
            CLRB(R_STATUS, ATA_STAT_ERR);
            ACCORE::intr(ACCORE::INTRN_ATA);
        } else {
            CLRB(R_STATUS, ATA_STAT_ERR);
            CLRB(R_STATUS, ATA_STAT_BUSY);
            R_STATUS |= ATA_STAT_READY;
            ACCORE::intr(ACCORE::INTRN_ATA);
        }
        break;
    case ATA_C_READ_NATIVE_MAX_ADDRESS: {
        u32 max_lba = ata_total_sectors ? ata_total_sectors - 1 : 0;
        R_SECTOR = max_lba & 0xFF;
        R_LCYL = (max_lba >> 8) & 0xFF;
        R_HCYL = (max_lba >> 16) & 0xFF;
        R_SELECT = (R_SELECT & 0xF0) | ((max_lba >> 24) & 0x0F);
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    }
    case ATA_C_SET_MAX_ADDRESS:
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    case ATA_C_SECURITY_SET_PASSWORD:
    case ATA_C_SECURITY_UNLOCK:
    case ATA_C_SECURITY_ERASE_PREPARE:
    case ATA_C_SECURITY_ERASE_UNIT:
    case ATA_C_SECURITY_FREEZE_LOCK:
    case ATA_C_SECURITY_DISABLE_PASSWORD:
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    case ATA_C_FLUSH_CACHE:
    case ATA_C_FLUSH_CACHE_EXT:
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_EXECUTE_DEVICE_DIAGNOSTIC:
        R_ERROR = 0x01;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
    break;
    case ATA_C_INITIALIZE_DEVICE_PARAMETERS:
        // Need to rework this hack.
        // BG3: 0x91 completes immediately, so mark the device idle (-1); else cmd_handled stays 0 and the BUSY
        // probe-hack wedges it on -> HDD driver hangs (black screen).
        ACATA::cmd_handled = -1;
        CLRB(R_STATUS, ATA_STAT_ERR);
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        ACCORE::intr(ACCORE::INTRN_ATA);
        break;
    case ATA_C_DEVICE_RESET:
        ACATA_LOG("ATA_C_DEVICE_RESET");
        R_ERROR = 0x01;
        R_NSECTOR = 0x01;
        R_SECTOR = 0x01;
        R_LCYL = 0x00;
        R_HCYL = 0x00;
        R_SELECT = 0x00;
        CLRB(R_STATUS, ATA_STAT_BUSY);
        R_STATUS |= ATA_STAT_READY;
        break;

    default:
        Console.Error("unhandled ATACMD %X", val);
        break;
    }
}

// ARMSX2: VMManager resolves where the image is (a path, or an Android content:// URI the frontend found
// next to the .acgame) and hands it over whole; PCSX2x6's SetEnv built it from a directory and a name.
void ACATA::SetImage(std::string path, ACMEDIATYPE media) {
	ACATA::imgpath = std::move(path);
	ACATA::MediaType = media;
	Console.WriteLnFmt("ACATA: image '{}', media type {}", ACATA::imgpath, static_cast<int>(ACATA::MediaType));
}


u16 ACATA::R_DATA;
u16 ACATA::R_FEATURE;
u16 ACATA::R_ERROR;
u16 ACATA::R_NSECTOR;
u16 ACATA::R_SECTOR;
u16 ACATA::R_LCYL;
u16 ACATA::R_HCYL;
u16 ACATA::R_SELECT;
u16 ACATA::R_STATUS;
u16 ACATA::R_COMMAND;
u16 ACATA::R_STATUS_ALT;
u16 ACATA::R_CONTROL;

void ACATA::Reset() {
	ata_pio_len = 0;
	ata_pio_pos = 0;
	atacmd_responding = -1;
	atacmd_response_traverse = 0;
	ACATA::last_read = 0;
	ACATA::last_write = 0;
	R_DATA = R_FEATURE = R_ERROR = R_NSECTOR = R_SECTOR = R_LCYL = R_HCYL = R_SELECT = 0;
	R_STATUS = R_COMMAND = R_STATUS_ALT = R_CONTROL = 0;
	TH::nsector = 0;
	TH::LBA = 0;
	ACATAPI::ResetTransfers();
}
