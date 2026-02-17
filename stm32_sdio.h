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


class Stm32SDIO: public SDIO {

     enum {
        SD_RESP_NONE = 0,
        SD_RESP_R1   = 1,
        SD_RESP_R2   = 2,
        SD_RESP_R3   = 3,
        SD_RESP_R6   = 6,
        SD_RESP_R7   = 7
    };

public:
    void set_clock(uint32_t hz);
    void set_bus_width_1bit();
    void set_bus_width_4bit();
    int send_cmd(uint8_t cmd, uint32_t arg, uint32_t resp_type, uint32_t *response);
    int data_read(void *buf, uint32_t bytes);
    int data_write(const void *buf, uint32_t bytes);
    int wait_data_done();
    int get_data_crc_error();
    int get_data_timeout();
    void reset_datapath();
}
