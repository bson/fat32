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

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "cache.h"


void CacheBlockDev::lru_move_to_front(CacheEntry *e)
{
    if (_lru_head == e)
        return;

    /* unlink */
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;

    if (_lru_tail == e)
        _lru_tail = e->prev;

    /* insert at head */
    e->prev = NULL;
    e->next = _lru_head;

    if (_lru_head)
        _lru_head->prev = e;

    _lru_head = e;

    if (!_lru_tail)
        _lru_tail = e;
}


CacheBlockDev::CacheEntry* CacheBlockDev::cache_lookup(uint32_t lba)
{
    for (uint32_t i = 0; i < CACHE_SECTORS; i++) {
        if (_entries[i].valid && _entries[i].lba == lba)
            return _entries + i;
    }

    return NULL;
}


CacheBlockDev::CacheEntry* CacheBlockDev::cache_evict()
{
    CacheEntry* victim = _lru_tail;

    assert(victim);

    if (victim->dirty) {
        if (_bdev.write_blocks(victim->lba, 1, victim->data))
            return NULL;
        victim->dirty = 0;
    }

    victim->valid = 0;

    return victim;
}


CacheBlockDev::CacheEntry* CacheBlockDev::cache_alloc_entry()
{
    for (uint32_t i = 0; i < CACHE_SECTORS; i++) {
        if (!_entries[i].valid)
            return _entries + i;
    }

    return cache_evict();
}


int CacheBlockDev::read_blocks(uint32_t lba, uint32_t count, void *buffer, bool bypass)
{
    uint8_t *out = (uint8_t*)buffer;

    _nreads += count;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t cur = lba + i;

        CacheEntry *e = cache_lookup(cur);

        if (!e) {
            if (bypass) {
                if (_bdev.read_blocks(cur, 1, out, bypass))
                    return -1;
                out += SECTOR_SIZE;
                continue;
            } else {
                e = cache_alloc_entry();
                if (!e)
                    return -1;

                if (_bdev.read_blocks(cur, 1, e->data) != 0)
                    return -1;

                e->lba = cur;
                e->valid = 1;
                e->dirty = 0;
            }
        } else {
            ++_nread_hits;
        }

        ::memcpy(out, e->data, SECTOR_SIZE);
        out += SECTOR_SIZE;

        lru_move_to_front(e);
    }

    return 0;
}


int CacheBlockDev::write_blocks(uint32_t lba, uint32_t count, const void *buffer, bool bypass)
{
    const uint8_t *in = (const uint8_t*)buffer;

    _nwrites += count;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t cur = lba + i;

        CacheEntry *e = cache_lookup(cur);

        if (!e) {
            e = cache_alloc_entry();
            if (!e)
                return -1;

            e->lba = cur;
            e->valid = 1;
            e->dirty = 0;
        } else {
            e->dirty = false;
            ++_nwrite_hits;
        }

        if (!bypass)
            ::memcpy(e->data, in, SECTOR_SIZE);

        if (_write_through || bypass) {
            if (_bdev.write_blocks(cur, 1, bypass ? in : e->data))
                return -1;
        } else
            e->dirty = 1;

        if (!bypass)
            lru_move_to_front(e);

        in += SECTOR_SIZE;
    }

    return 0;
}


int CacheBlockDev::flush()
{
    int status = 0;

    if (!_write_through) {
        for (uint32_t i = 0; i < CACHE_SECTORS; i++) {
            if (_entries[i].valid && _entries[i].dirty) {
                if (_bdev.write_blocks(_entries[i].lba, 1, _entries[i].data) != 0) {
                    // Keep flushing the rest
                    status = -1;
                }

                _entries[i].dirty = 0;
            }
        }
    }

    // Flush any underlying caching or buffering
    if (_bdev.flush())
        status = -1;

    return status;
}
