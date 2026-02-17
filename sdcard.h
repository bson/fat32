//
//  Copyright 2026 Jan Brittenson
//  All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify it under
//  the terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version. 
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program, named LICENSE. If not, see <http://www.gnu.org/licenses/>
//
#pragma once
#include <stdint.h>
#include "sdio.h"
#include "blockdev.h"


class SDCard: public BlockDev {

    SDIO&     _sdio;            // SDIO 1 & 4 bit compatible interface
    uint32_t  _rca;             // Relative Card Address
    uint32_t  _size;            // Sector count
    bool      _high_capacity;   // SDHC/SDXC

    enum : uint16_t { BLOCK_SIZE = 512 };

    enum {
        SD_RESP_NONE = 0,
        SD_RESP_R1   = 1,
        SD_RESP_R2   = 2,
        SD_RESP_R3   = 3,
        SD_RESP_R6   = 6,
        SD_RESP_R7   = 7
    };

public:
    SDCard(SDIO& sdio)
        :  _sdio(sdio),
           _rca(0),
           _high_capacity(false)
    {
    }

    // BlockDev interface
    int init();
    int read_blocks(uint32_t lba, uint32_t count, void *buffer, bool);
    int write_blocks(uint32_t lba, uint32_t count, const void *buffer, bool);
    int flush() { return 0; }
    uint32_t sector_size() const { return BLOCK_SIZE; }
    uint32_t size() const { return _size; };

private:
    int read_csd();
    int wait_ready();
};
