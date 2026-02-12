#pragma once
#include <stdint.h>

class BlockDev {
public:
    virtual int read_blocks(uint32_t lba,
                            uint32_t count,
                            void *buffer) = 0;

    virtual int write_blocks(uint32_t lba,
                             uint32_t count,
                             const void *buffer) = 0;
};
