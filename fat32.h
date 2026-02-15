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
#include <stddef.h>
#include "blockdev.h"

namespace Fat32 {

    // In reality only 512-byte sector sizes are commonly used by
    // storage devices.  When a FAT32 FS is created the sector size
    // becomes part of its formatting.
    enum : uint16_t { MAX_SECTOR_SIZE = 512 };
    enum : uint32_t { EOC = 0x0ffffff8 };
    enum : uint16_t { MIRROR_DISABLED = 0x0080 };
    enum : uint16_t { ACTIVE_FAT_MASK = 0x000f };

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

    enum Error : uint8_t {
        SUCCESS                   = 0,
        BDEV_READ_ERR             = 1, // Block device read error
        BDEV_WRITE_ERR            = 2, // Block device write error
        FAT_FULL                  = 3, // FAT full, unable to allocate cluster
        ALREADY_EXISTS            = 4, // Already exists
        UNSUPPORTED_SECTOR_SIZE   = 5, // Unable to mount due to unsupported sector size
        MALFORMED_FILENAME        = 6, // Malformed 8.3 SFN
        FILE_NOT_FOUND            = 7, // File or path component not found
        DIRECTORY_FULL            = 8, // No free entry in directory
        NEGATIVE_SEEK             = 9, // Seek to negative position
        DIR_NOT_EMPTY             =10, // Directory not empty
        BDEV_FLUSH_ERR            =11, // Block device flush error
        BDEV_INIT_ERR             =12, // Block device init failed (e.g. bad partition)
        FSCK_ALLOC_ERR            =13, // fsck calloc() failed
        NOT_DIRECTORY             =14, // opendir or other dir op on a non-directory
        FS_NEEDS_REPAIR           =15, // Consistency check failed
        BAD_FAT_SIZE              =16, // FAT size is 0
        NUM_ERRORS
    };

    class FileSys {
        BlockDev& _bdev;

        // Single sector buffer.
        uint32_t _sec_lba;      // Sector currently being staged
        uint8_t  _sector[MAX_SECTOR_SIZE];
        char     _tmp[256];     // For path wrangling

        uint32_t _sectors_per_cluster;
        uint32_t _bytes_per_sector;
        uint32_t _reserved_sectors;

        uint32_t _fat_start_lba;
        uint32_t _fat_size_sectors;
        uint32_t _fat_count;

        uint32_t _data_start_lba;
        uint32_t _root_cluster;
        uint32_t _total_clusters;

        uint32_t _fsinfo_lba;
        uint32_t _free_cluster_count;
        uint32_t _next_free_cluster;

        uint16_t _ext_flags;
        bool     _fsinfo_valid; // fsinfo was found or was initialized
        bool     _fsinfo_dirty; // fsinfo needs to be rewritten

        char     _volume_label[12];

        Error    _last_error;

        FileSys() = delete;
        FileSys(FileSys&) = delete;

    public:
        class File;

        FileSys(BlockDev& bdev)
            : _bdev(bdev), _sec_lba(~uint32_t(0))
        { }

        // If compiled with -DFAT32_STRICT_MOUNT then this includes
        // integrity checks.  If it fails with FS_NEEDS_REPAIR then
        // the FS is still mounted and usable.  It can be repaired
        // with fsck() or used anyway.
        int mount();

        // Checkpoint FS state (currently only PSINFO) if dirty
        int sync();

        // Must exist: will not create
        int open(const char *path, File *file);

        // Must not exist: create and open
        int create(const char *path, File *file);
        int rename(const char *from_path, const char* to_path);
        int unlink(const char *path);

        int mkdir(const char *path);
        int rmdir(const char *path);

        typedef struct {
            uint32_t files;
            uint32_t directories;
            uint32_t total_clusters;
            uint32_t free_clusters;
            uint32_t referenced_clusters;
            uint32_t lost_clusters;
            uint32_t cross_links;
            uint32_t size_mismatches;
            uint32_t invalid_entries;
            uint32_t invalid_references;
            uint32_t repairs;
        } fsck_report_t;

        // Requires heap (calloc)
        int fsck(bool fix, fsck_report_t* report);

        const char* volume_label() const { return _volume_label; }

        Error last_error() const { return _last_error; }
        const char* strerror(Error err) const;

        // readdir

        class DIR {
        protected:
            friend class FileSys;

            FileSys *_fs;

            uint32_t _start_cluster;
            uint32_t _current_cluster;

            uint32_t _sector_index;
            uint32_t _entry_offset;

            uint8_t  _sector[MAX_SECTOR_SIZE];

        public:
            typedef struct {
                char name[13];        /* 8.3 name, null-terminated */
                DirentAttr attr;
                uint32_t size;
                uint32_t first_cluster;
            } entry_t;

            int readdir(entry_t *out);
            void closedir();
        };

        int opendir(const char *path, DIR *dir);
        void closedir();

        // Open files

        class File {
        public:
            FileSys *_fs;

            DirentAttr _attr;

#ifdef FAT32_DATE_AND_TIME
            uint8_t  _creation_time_tenth;
            uint16_t _creation_time;
            uint16_t _creation_date;
            uint16_t _last_access_date;
            uint16_t _write_time;
            uint16_t _write_date;
#endif

            uint32_t _first_cluster;
            uint32_t _current_cluster;

            uint32_t _file_size;
            uint32_t _file_pos;

            uint32_t _dir_lba;
            uint32_t _dir_offset;

            // Test if anything more can be read.  Available can be
            // negative after a seek past EOF.
            int32_t available() const { return int32_t(_file_size - _file_pos); }
            bool eof() const { return available() <= 0; }
            uint32_t filepos() const { return _file_pos; }

            DirentAttr attributes() const { return _attr; }

            // These return bytes read/written, or -1 on error
            int read(void *buffer, size_t len);
            int write(const void *buffer, size_t len);

            int lseek(int32_t offset, SeekOp whence);
            int truncate(uint32_t new_size);

            int close() { return sync(); }
            int sync();

            Error last_error() const { return _fs->last_error(); }
            const char* strerror(Error err) const { return _fs->strerror(err);  }

        private:
            // Make sure a specific cluster exists, extending the file if necessary
            int ensure_cluster_index(uint32_t needed_index, uint32_t* out_cluster);

            // Update directory entry size and timestamp fields
            int update_dirent_size_time();
        };


    protected:
        friend class Fat32;
        friend class DIR;

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

        int cluster_for_offset(uint32_t first_cluster, uint32_t offset,
                               uint32_t *out_cluster, uint32_t *cluster_index);

        int dir_load_volume_label_from_root();
        int dir_find(uint32_t cluster, const char *name, File *file);
        int dir_find_free_slot(uint32_t dir_cluster, uint32_t *out_lba,
                               uint32_t *out_offset);
        int dir_find_parent(char* path_buffer, bool tail, uint32_t* cluster);
        int dir_is_empty(uint32_t cluster);

        int make_sfn(const char *name, uint8_t out[11]);
        void build_83_name(const uint8_t *entry, char *out);

        int success() { _last_error = Error::SUCCESS; return 0; } // Good return
        int with_error(Error err) {                               // Error return
            if (_last_error == Error::SUCCESS)
                _last_error = err;
            return -1;
        } 

    private:
        typedef struct {
            uint8_t *cluster_refcount;
            uint32_t total_clusters;
        } fsck_ctx_t;

        int fsck_mark_chain(fsck_ctx_t *ctx, uint32_t start_cluster, fsck_report_t *report);
        int fsck_chain_length(uint32_t start, uint32_t* len);
        void fsck_verify_mirrors(fsck_ctx_t *ctx, fsck_report_t* report, bool fix);
        int fsck_scan_directory(fsck_ctx_t *ctx, uint32_t dir_cluster, bool fix,
                                fsck_report_t *rep);
        int fat_compare(uint32_t fat_a, uint32_t fat_b); // 0=same, 1=differ, 1=error
        int fat_copy(uint32_t src, uint32_t dst);



        // Disk structures

#pragma pack(push,1)

        // BIOS Parameter Block (previously BOOT Sector)
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

    template <typename T1, typename T2>
    T1 min(const T1& a, const T2& b) {
        return a < (T1)b ? a : (T1)b;
    }

}; // ns Fat32
