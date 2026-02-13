#pragma once
#include "blockdev.h"
#include <stdint.h>
#include <stddef.h>

namespace Fat32 {

    enum : uint16_t { MAX_SECTOR_SIZE = 512 };
    enum : uint32_t { EOC = 0x0ffffff8 };

    enum DirentAttr : uint8_t {
        NONE      = 0x00,
        READ_ONLY = 0x01,
        HIDDEN    = 0x02,
        SYSTEM    = 0x04,
        VOLUME_ID = 0x08,           // Root dir only
        LFN       = 0x0f,
        DIRECTORY = 0x10,
        ARCHIVE   = 0x20
    };

    // Naming here is to match canonical use
    enum SeekOp : int {
        SET = 0,
        CUR = 1, 
        END = 2
    };


    class FileSys {
        BlockDev& _bdev;

        // Single sector buffer.
        uint32_t _sec_lba;          // Sector currently buffered
        uint8_t  _sector[MAX_SECTOR_SIZE];

        uint32_t _fat_start_lba;
        uint32_t _data_start_lba;
        uint32_t _root_cluster;

        uint32_t _sectors_per_cluster;
        uint32_t _bytes_per_sector;
        uint32_t _reserved_sectors;
        uint32_t _sectors_per_fat;
        uint32_t _fat_count;
        uint32_t _total_clusters;

        uint32_t _fsinfo_lba;
        uint32_t _free_cluster_count;
        uint32_t _next_free_cluster;
        bool     _fsinfo_valid; // fsinfo was found or was initialized
        bool     _fsinfo_dirty; // fsinfo needs to be rewritten

        char     _volume_label[12];

        FileSys() = delete;
        FileSys(FileSys&) = delete;

    public:
        class File;
        class Stat;

        FileSys(BlockDev& bdev)
            : _bdev(bdev), _sec_lba(~uint32_t(0))
        { }

        int mount();

        // Checkpoint FS state (PSINFO) if dirty
        int checkpoint();

        int stat(const char *path, Stat *st);

        // Must exist: will not create
        int open(const char *path, File *file);

        // Must not exist: create and open
        int create(const char *path, File *file);
        int unlink(const char *path);

        int mkdir(const char *path);
        int rmdir(const char *path);

        const char* volume_label() const { return _volume_label; }


        class File {
        public:
            FileSys *_fs;

            uint32_t _first_cluster;
            uint32_t _current_cluster;

            uint32_t _file_size;
            uint32_t _file_pos;

            uint32_t _dir_lba;
            uint32_t _dir_offset;

            // Test if anything more can be read
            int available() const { return _file_size - _file_pos; }
            bool eof() const { return available() <= 0; }

            // These return bytes read/written, or -1 on error
            int read(void *buffer, size_t len);
            int write(const void *buffer, size_t len);

            int lseek(int32_t offset, SeekOp whence);
            int truncate(uint32_t new_size);

            int close() { return sync(); }
            int sync();

        private:
            // Make sure a specific cluster exists, extending the file if necessary
            int ensure_cluster_index(uint32_t needed_index, uint32_t *out_cluster);

            // Update directory entry size field
            int update_dirent_size();
        };


        class Stat {
        public:
            uint32_t _size;
            uint32_t _first_cluster;
            DirentAttr _attributes;
        };


    protected:
        friend class Fat32;

        int load_sector(uint32_t lba, uint8_t** sector); // Load sector, if needed
        int store_sector(uint32_t lba); // Write sector buffer

    private:
        uint32_t cluster_size();
        uint32_t cluster_to_lba(uint32_t cluster);
        int fat_get(uint32_t cluster, uint32_t *val);
        int fat_set_single(uint32_t cluster, uint32_t val, uint32_t fat_index);
        int fat_set(uint32_t cluster, uint32_t val);
        int fat_allocate(uint32_t *out);
        int fat_free_chain(uint32_t start);
        int fat_recompute_free_clusters(uint32_t* free_count, uint32_t* next_free);
        int fat_cluster_at(uint32_t start_cluster, uint32_t index, uint32_t* cluster); // FAT Walk

        int cluster_for_offset(uint32_t first_cluster,
                               uint32_t offset,
                               uint32_t *out_cluster,
                               uint32_t *cluster_index);

        int dir_load_volume_label_from_root();
        int dir_find(uint32_t cluster, const char *name, File *file);
        int dir_find_free_slot(uint32_t dir_cluster, uint32_t *out_lba,
                               uint32_t *out_offset);
        int dir_find_parent(char* path_buffer, bool tail, uint32_t* cluster);
        int dir_is_empty(uint32_t cluster);

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
            enum : uint32_t {
                SIG1 = 0x41615252,
                SIG2 = 0x61417272,
                SIG3 = 0xaa550000
            };

            uint32_t lead_sig;        /* 0x41615252 SIG1 */
            uint8_t  reserved1[480];
            uint32_t struct_sig;      /* 0x61417272 SIG2 */
            uint32_t free_count;
            uint32_t next_free;
            uint8_t  reserved2[12];
            uint32_t trail_sig;       /* 0xAA550000 SIG3 */
        } fsinfo_t;


        typedef struct {
            uint8_t  name[11];
            DirentAttr attr;
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
    };

    // Misc utility functions

    // Returns pointer to last component of path
    static const char* basename(const char* path);
    static int make_sfn(const char *name, uint8_t out[11]);

    template <typename T1, typename T2>
    T1 min(const T1& a, const T2& b) {
        return a < (T1)b ? a : (T1)b;
    }

}; // ns Fat32
