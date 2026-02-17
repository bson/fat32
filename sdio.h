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


// Abstract interface for SDIO hardware implementation

class SDIO {
public:
    virtual void set_clock(uint32_t hz) = 0;
    virtual void set_bus_width_4bit() = 0;
    virtual void set_bus_width_1bit() = 0;
    virtual int send_cmd(uint8_t cmd,
                         uint32_t arg,
                         uint32_t resp_type,
                         uint32_t *response) = 0;
    virtual int data_read(void *buf, uint32_t bytes) = 0;
    virtual int data_write(const void *buf, uint32_t bytes) = 0;
    virtual int wait_data_done() = 0;

    virtual int get_data_crc_error() = 0;
    virtual int get_data_timeout() = 0;
    virtual void reset_datapath() = 0;
};

