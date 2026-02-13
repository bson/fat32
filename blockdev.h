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
