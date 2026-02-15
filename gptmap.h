#pragma once
#include <stdint.h>
#include "blockdev.h"


namespace GPTMap {

    enum : uint32_t { GPT_HEADER_LBA = 1 };
    enum : uint64_t { GPT_SIGNATURE = 0x5452415020494645ULL };  /* "EFI PART" */
    enum : uint16_t { GPT_MAX_PARTITIONS = 128 };
    enum : uint16_t { GPT_NAME_LEN = 36 }; /* UTF-16 code units */
    enum : int { MAX_PARTITIONS = 8 };
    enum : uint16_t { SECTOR_SIZE = 512 };

    enum GuidType: uint16_t {
        TYPE_UNKNOWN = 0,
        TYPE_EFI_SYSTEM,        // EFI System Partition
        TYPE_FAT32,             // FAT32
        TYPE_MS_BASIC_DATA,
        TYPE_LINUX_FILESYSTEM,
        TYPE_LINUX_SWAP,
        TYPE_BIOS_BOOT,
        NUM_TYPES
    };

    typedef struct {
        uint8_t  type_guid[16];
        uint8_t  guid[16];
        uint64_t first_lba;
        uint64_t last_lba;
        uint64_t attributes;
        char     name[38];      // In ASCII
        GuidType type;          // Type, e.g. FAT32
        GuidType otype;         // Original type, e.g. EFI System
        uint32_t entry_index;
    } gpt_partition_t;

    typedef struct {
        int             count;
        gpt_partition_t entries[MAX_PARTITIONS];
    } gpt_table_t;


    // Partition table
    class Table {
        BlockDev&   _bdev;
        uint8_t     _sector[512];
        gpt_table_t _table;
        
    public:
        Table(BlockDev& bdev)
            : _bdev(bdev)
        {
            _table.count = 0;
        }

        // Load table
        int load();

        // Number of partitions
        int count() const { return _table.count; }

        // Return partition data by index
        gpt_partition_t& get(int n) { return _table.entries[n]; }

        // Return name for partition type
        static const char* typestr(GuidType type) {
            return part_type_str[type];
        }

    private:
        bool partition_is_fat32(uint32_t first_lba);

        static const char* part_type_str[NUM_TYPES];
    };


    // Mapper
    class Mapper: public BlockDev {
        BlockDev&        _bdev;
        uint32_t         _first_lba;
        uint32_t         _last_lba;
        gpt_partition_t& _partition;
        
    public:
        Mapper(BlockDev& bdev, gpt_partition_t& part)
            : _bdev(bdev), _partition(part),
              _first_lba(part.first_lba), _last_lba(part.last_lba)
        { }

        int init() {
            if (_bdev.sector_size() != SECTOR_SIZE)
                return -1;

            // We're limited to 32-bit LBAs, or 2TB with 512-byte sectoring.
            if (_partition.last_lba >> 32)
                return -1;

            return 0;
        }

        int read_blocks(uint32_t lba, uint32_t count, void *buffer) {
            lba += _first_lba;
            if (lba < _first_lba || lba + count > _last_lba)  // < means it wrapped
                return -1;

            return _bdev.read_blocks(lba, count, buffer);
        }

        int write_blocks(uint32_t lba, uint32_t count, const void *buffer) {
            lba += _first_lba;
            if (lba < _first_lba || lba + count > _last_lba)
                return -1;

            return _bdev.write_blocks(lba, count, buffer);
        }

        int flush() { return _bdev.flush(); }

        uint32_t sector_size() const { return SECTOR_SIZE; }
    };


    // Header

#pragma pack(push,1)
    typedef struct {
        uint64_t signature;
        uint32_t revision;
        uint32_t header_size;
        uint32_t header_crc32;
        uint32_t reserved;
        uint64_t current_lba;
        uint64_t backup_lba;
        uint64_t first_usable_lba;
        uint64_t last_usable_lba;
        uint8_t  disk_guid[16];
        uint64_t partition_entry_lba;
        uint32_t num_partition_entries;
        uint32_t sizeof_partition_entry;
        uint32_t partition_array_crc32;
    } gpt_header_t;

    typedef struct {
        uint8_t  type_guid[16];
        uint8_t  guid[16];
        uint64_t first_lba;
        uint64_t last_lba;
        uint64_t attributes;
        uint16_t name[GPT_NAME_LEN];
    } gpt_entry_raw_t;
#pragma pack(pop)


    template <typename T1, typename T2>
    T1 min(const T1& a, const T2& b) {
        return a < (T1)b ? a : (T1)b;
    }
}; // ns GPTMap
