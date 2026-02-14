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

    int init() { return _bdev.init(); }
    int read_blocks(uint32_t lba, uint32_t count, void *buffer);
    int write_blocks(uint32_t lba, uint32_t count, const void *buffer);
    int flush();
    uint32_t sector_size() const { return SECTOR_SIZE; }

private:
    void lru_move_to_front(CacheEntry *e);
    CacheEntry* cache_lookup(uint32_t lba);
    CacheEntry* cache_evict();
    CacheEntry *cache_alloc_entry();

    CacheBlockDev(CacheBlockDev&) = delete;
};
