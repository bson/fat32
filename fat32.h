#pragma once
#include "blockdev.h"
#include <stdint.h>
#include <stddef.h>

template <typename T1, typename T2>
T1 min(const T1& a, const T2& b) {
    return a < (T1)b ? a : (T1)b;
}

class Fat32 {
public:
    enum : uint16_t { SECTOR_SIZE = 512 };
    enum : uint32_t { EOC = 0x0ffffff8 };

    enum DirEntAttr : uint8_t {
        UNUSED    = 0x00,           // Unused entry
        READ_ONLY = 0x01,
        HIDDEN    = 0x02,
        SYSTEM    = 0x04,
        VOLUME_ID = 0x08,           // Root dir only
        LFN       = 0x0f,
        DIRECTORY = 0x10,
        ARCHIVE   = 0x20
    };


    class Stat;
    class File;

    class FileSys {
    public:
        BlockDev *_bdev;

        uint32_t _fat_start_lba;
        uint32_t _data_start_lba;
        uint32_t _root_cluster;

        uint32_t _sectors_per_cluster;
        uint32_t _reserved_sectors;
        uint32_t _sectors_per_fat;
        uint32_t _fat_count;
        uint32_t _total_clusters;


        int stat(const char *path, Stat *st);

        // Must exist: will not create
        int open(const char *path, File *file);

        // Must not exist: create and open
        int create(const char *path, File *file);
        int unlink(const char *path);

        int mkdir(const char *path);
        int rmdir(const char *path);

        friend class File;
    protected:
        uint32_t cluster_to_lba(uint32_t cluster);
        int fat_get(uint32_t cluster, uint32_t *val);
        int fat_set_single(uint32_t cluster, uint32_t val, uint32_t fat_index);
        int fat_set(uint32_t cluster, uint32_t val);
        int fat_allocate(uint32_t *out);
        int fat_free_chain(uint32_t start);
        int dir_find(uint32_t cluster, const char *name, File *file);
        int dir_find_free_slot(uint32_t dir_cluster, uint32_t *out_lba,
                               uint32_t *out_offset);
        int dir_find_parent(char* path_buffer, bool tail, uint32_t* cluster);
        int dir_is_empty(uint32_t cluster);

    };


    class File {
    public:
        FileSys *_fs;

        uint32_t _first_cluster;
        uint32_t _current_cluster;

        uint32_t _file_size;
        uint32_t _file_pos;

        uint32_t _dir_lba;
        uint32_t _dir_offset;


        int read(void *buffer, size_t len);

        int write(const void *buffer, size_t len);

        int close();
    };


    class Stat {
    public:
        uint32_t _size;
        uint32_t _first_cluster;
        DirEntAttr _attributes;
    };


    static int mount(BlockDev *bdev, FileSys *fs);

    // Misc utility functions

    // Returns pointer to last component of path
    static const char* basename(const char* path);

private:

    // Disk structures

#pragma pack(push,1)

    typedef struct {
        uint8_t  jump[3];
        uint8_t  oem[8];
        uint16_t bytes_per_sector;
        uint8_t  sectors_per_cluster;
        uint16_t reserved_sector_count;
        uint8_t  num_fats;
        uint16_t root_entry_count;
        uint16_t total_sectors_16;
        uint8_t  media;
        uint16_t fat_size_16;
        uint16_t sectors_per_track;
        uint16_t num_heads;
        uint32_t hidden_sectors;
        uint32_t total_sectors_32;

        uint32_t fat_size_32;
        uint16_t ext_flags;
        uint16_t fs_version;
        uint32_t root_cluster;
        uint16_t fs_info;
        uint16_t backup_boot_sector;
        uint8_t  reserved[12];
    } bpb_t;

    typedef struct {
        uint8_t  name[11];
        enum DirEntAttr attr;
        uint8_t  nt_reserved;
        uint8_t  creation_time_tenth;
        uint16_t creation_time;
        uint16_t creation_date;
        uint16_t last_access_date;
        uint16_t first_cluster_hi;
        uint16_t write_time;
        uint16_t write_date;
        uint16_t first_cluster_lo;
        uint32_t file_size;
    } dirent_t;

#pragma pack(pop)

    static int make_sfn(const char *name, uint8_t out[11]);
};
