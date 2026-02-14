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

class BlockDev {
public:
    // Initialize, if needed
    virtual int init() = 0;

    virtual int read_blocks(uint32_t lba, uint32_t count, void *buffer) = 0;

    virtual int write_blocks(uint32_t lba, uint32_t count, const void *buffer) = 0;

    // Flush any caches or buffers
    virtual int flush() = 0;
    
    // Return device sector size
    virtual uint32_t sector_size() const = 0;
};
