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
#include <string.h>
#include "blockdev.h"


// Hard-wire the size
enum : int { CACHE_SECTORS = 16 };
enum : int { SECTOR_SIZE = 512 };


class CacheBlockDev: public BlockDev 
{
    struct CacheEntry {
        uint32_t  lba;
        uint8_t   data[SECTOR_SIZE];
        bool      valid;
        bool      dirty;

        CacheEntry *prev;
        CacheEntry *next;
    };

    // Underlying storage
    BlockDev&      _bdev;

    CacheEntry     _entries[CACHE_SECTORS];
    CacheEntry*    _lru_head;
    CacheEntry*    _lru_tail;

    const bool     _write_through;

public:
    uint32_t       _nreads;
    uint32_t       _nread_hits;
    uint32_t       _nwrites;
    uint32_t       _nwrite_hits;

    CacheBlockDev(BlockDev& bdev, bool write_through)
        : _bdev(bdev),
          _write_through(write_through)
    { 
        ::memset(_entries, 0, sizeof _entries);

        _lru_head = NULL;
        _lru_tail = NULL;

       _nreads = 0;
       _nread_hits = 0;
       _nwrites = 0;
       _nwrite_hits = 0;
    }

    int init() {
        if (_bdev.init())
            return -1;

        if (_bdev.sector_size() != SECTOR_SIZE)
            return -1;

        return 0;
    }
    int read_blocks(uint32_t lba, uint32_t count, void *buffer, bool bypass);
    int write_blocks(uint32_t lba, uint32_t count, const void *buffer, bool bypass);
    int flush();
    uint32_t sector_size() const { return SECTOR_SIZE; }
    uint32_t size() const { return _bdev.size(); }

private:
    void lru_move_to_front(CacheEntry *e);
    CacheEntry* cache_lookup(uint32_t lba);
    void cache_invalidate(uint32_t lba);
    CacheEntry* cache_evict();
    CacheEntry *cache_alloc_entry();

    CacheBlockDev(CacheBlockDev&) = delete;
};
