#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "fat32.h"

using namespace Fat32;

// 
// If there's a crash during write:
//
//   Crash Point	 			             Result
//
//   After allocation, before link           Unreferenced cluster (safe leak)
//   After link, before data write           Data cluster empty but chain valid
//   After data write, before size update    File shorter than actual chain
//   During size update                      Old size preserved
// 
// No cross-linking possible.
// 
//
// If crash during delete:
//
//   Crash Point                            Result
//
//   After dir delete, before FAT free      Orphan cluster chain (recoverable)
//   During FAT free                        Partial free, fsck cleans
// 
// Never causes another file to reference freed clusters.
//
// FAT Mirror behavior:
//
//   Condition             Mount         fsck scan         fsck repair
//
//   Mirrors identical      OK              OK                 OK
//   Mirrors diverge    Mount fails      reported           repaired
//   Mirror disabled    no compare   compare active only  repair others
//   3+ FATs           all compared    all compared        all synced
//

static const char* error_strings[] = {
    [SUCCESS]                 = "Success",
    [BDEV_READ_ERR]           = "Block device read error",
    [BDEV_WRITE_ERR]          = "Block device write error",
    [FAT_FULL]                = "FAT full, unable to allocate cluster",
    [ALREADY_EXISTS]          = "Already exists",
    [UNSUPPORTED_SECTOR_SIZE] = "Unsupported sector size",
    [MALFORMED_FILENAME]      = "Malformed filename",
    [FILE_NOT_FOUND]          = "File or path component not found",
    [DIRECTORY_FULL]          = "Directory full",
    [NEGATIVE_SEEK]           = "Seek to negative position",
    [DIR_NOT_EMPTY]           = "Directory not empty",
    [BDEV_FLUSH_ERR]          = "Block device write error during flush",
    [BDEV_INIT_ERR]           = "Block device failed to initialize",
    [FSCK_ALLOC_ERR]          = "Failed to allocate memory in fsck",
    [NOT_DIRECTORY]           = "Not a directory",
    [FS_NEEDS_REPAIR]         = "File system needs repair",
    [BAD_FAT_SIZE]            = "FAT size is 0",
};

static_assert((sizeof error_strings / sizeof error_strings[0]) == NUM_ERRORS,
    "some error is lacking a string");

static constexpr uint8_t dot[12] = ".          ";
static constexpr uint8_t dotdot[12] = "..         ";

    
const char* FileSys::strerror(Error err) const
{
    if (err >= NUM_ERRORS || error_strings[err] == NULL)
        return "Unknown error";

    return error_strings[err];
}


int FileSys::load_sector(uint32_t lba, uint8_t** sector)
{
    if (lba == _sec_lba) {
        *sector = _sector;
        return success();
    }

    if (_bdev.read_blocks(lba, 1, _sector))
        return with_error(Error::BDEV_READ_ERR);

    *sector = _sector;
    _sec_lba = lba;
    return success();
}


int FileSys::store_sector(uint32_t lba)
{
    if (_bdev.write_blocks(lba, 1, _sector))
        return with_error(Error::BDEV_WRITE_ERR);

    _sec_lba = lba;
    return success();
}


uint32_t FileSys::cluster_to_lba(uint32_t cluster)
{
    return _data_start_lba + ((cluster - 2) * _sectors_per_cluster);
}


int FileSys::fat_get(uint32_t cluster, uint32_t *val)
{
    uint32_t off = cluster * 4;
    uint32_t lba = _fat_start_lba + (off / _bytes_per_sector);
    uint32_t pos = off % _bytes_per_sector;

    uint8_t* sector;
    if (load_sector(lba, &sector))
        return -1;

    *val = (*(uint32_t*)&sector[pos]) & 0x0fffffff;  // XXX symbol
    return success();
}


int FileSys::fat_set_single(uint32_t cluster,
                            uint32_t val,
                            uint32_t fat_index)
{
    const uint32_t off = cluster * 4;
    const uint32_t base = _fat_start_lba + fat_index * _sectors_per_fat;

    const uint32_t lba = base + (off / _bytes_per_sector);
    const uint32_t pos = off % _bytes_per_sector;

    uint8_t* sector;
    if (load_sector(lba, &sector))
        return -1;

    uint32_t *entry = (uint32_t*)&sector[pos];
    *entry = (*entry & 0xf0000000) | (val & 0x0fffffff); // XXX make symbolic?

    if (store_sector(lba))
         return -1;

    return success();
}


int FileSys::fat_set(uint32_t cluster, uint32_t val)
{
    const int mirror_disabled = _ext_flags & MIRROR_DISABLED;

    uint32_t start_fat = 0;
    uint32_t end_fat   = _fat_count;

    if (mirror_disabled) {
        start_fat = _ext_flags & ACTIVE_FAT_MASK;
        end_fat   = start_fat + 1;
    }

    for (uint32_t fat = start_fat; fat < end_fat; fat++)
        if (fat_set_single(cluster, val, fat))
            return -1;

    return success();
}


int FileSys::fat_allocate(uint32_t *out)
{
    uint32_t start = _fsinfo_valid ? _next_free_cluster : 2;

    for (uint32_t pass = 0; pass < 2; pass++) {

        for (uint32_t c = start; c < _total_clusters; c++) {
            uint32_t val;

            if (fat_get(c, &val))
                return -1;

            if (val == 0) {
                if (fat_set(c, EOC))
                    return -1;

                *out = c;

                if (_fsinfo_valid) {
                    if (_free_cluster_count != 0xffffffff)
                        --_free_cluster_count;

                    _next_free_cluster = c + 1;
                    _fsinfo_dirty = true;
                }

                return success();
            }
        }

        /* wrap once */
        start = 2;
    }

    return with_error(Error::FAT_FULL);
}


int FileSys::fat_free_chain(uint32_t start)
{
    uint32_t cluster = start;

    while (cluster < EOC) {
        uint32_t next;
        if (fat_get(cluster, &next))
            return -1;
        if (fat_set(cluster, 0))
            return -1;
        cluster = next;
    }

    if (_fsinfo_valid) {
        if (_free_cluster_count != 0xFFFFFFFF)
            ++_free_cluster_count;

        if (cluster < _next_free_cluster)
            _next_free_cluster = cluster;

        _fsinfo_dirty = true;
    }

    return success();
}


int FileSys::fat_recompute_free_clusters(uint32_t* free_count, uint32_t* next_free) 
{
    uint32_t free = 0;
    uint32_t first = 0;

    for (uint32_t c = 2; c < _total_clusters; c++) {
        uint32_t val;

        if (fat_get(c, &val))
            return -1;

        if (val == 0) {
            ++free;
            if (first == 0)
                first = c;
        }
    }

    *free_count = free;
    *next_free = first ? first : 2;

    return success();
}

int FileSys::dir_load_volume_label_from_root()
{
    uint32_t cluster = _root_cluster;

    while (cluster < EOC) {
        const uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            dirent_t *ent;
            if (load_sector(lba + s, (uint8_t**)&ent))
                return -1;

            for (int i = 0; i < _bytes_per_sector / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00)
                    goto no_label;

                if (ent[i].name[0] == 0xe5)
                    continue;

                if (ent[i].attr == DirentAttr::VOLUME_ID) {
                    ::memcpy(_volume_label, ent[i].name, 11);
                    _volume_label[11] = 0;

                    for (int end = 10; end >= 0 && _volume_label[end] == ' '; --end)
                        _volume_label[end] = 0;

                    return success();
                }
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

no_label:
    // Volume has no label, simply use "UNKNOWN"
    ::memcpy(_volume_label, "UNKNOWN\0\0\0\0", 12);  // Including trailing 0
    return success();
}


int FileSys::mount()
{
    if (_bdev.init())
        return with_error(Error::BDEV_INIT_ERR);

    bpb_t *bpb;
    if (load_sector(0, (uint8_t**)&bpb))
        return -1;

    _reserved_sectors   = bpb->reserved_sector_count;
    _bytes_per_sector   = bpb->bytes_per_sector;
    _sectors_per_cluster= bpb->sectors_per_cluster;
    _sectors_per_fat    = bpb->fat_size_32;
    _fat_count          = bpb->num_fats;
    _root_cluster       = bpb->root_cluster;
    _ext_flags          = bpb->ext_flags;

    if (_bytes_per_sector > MAX_SECTOR_SIZE)
        return with_error(Error::UNSUPPORTED_SECTOR_SIZE);

    if (_sectors_per_fat == 0)
        return with_error(Error::BAD_FAT_SIZE);

    _fat_start_lba  = _reserved_sectors;
    _data_start_lba = _reserved_sectors + _fat_count * _sectors_per_fat;

    const uint32_t total_sectors =
        bpb->total_sectors_32 ?
        bpb->total_sectors_32 :
        bpb->total_sectors_16;

    const uint32_t data_sectors = total_sectors
        - (_reserved_sectors + _fat_count * _sectors_per_fat);

    _total_clusters = data_sectors / _sectors_per_cluster;

    // Load PSINFO
    _fsinfo_lba = _fat_start_lba - 1; // From BPB
    _fsinfo_valid = false;
    _fsinfo_dirty = false;

    fsinfo_t *fsi;
    if (!load_sector(_fsinfo_lba, (uint8_t**)&fsi)) {

        if (fsi->lead_sig  == fsinfo_t::SIG1 &&
            fsi->struct_sig == fsinfo_t::SIG2 &&
            fsi->trail_sig == fsinfo_t::SIG3) {

            _free_cluster_count = fsi->free_count;
            _next_free_cluster =
                (fsi->next_free >= 2 &&
                 fsi->next_free < _total_clusters)
                ? fsi->next_free
                : 2;

            _fsinfo_valid = true;
        }
    }

    // If invalid or missing: rebuild and (re)initialize
    if (!_fsinfo_valid) {
        uint32_t free_count;
        uint32_t next_free;

        if (fat_recompute_free_clusters(&free_count, &next_free))
            return -1;

        fsi->lead_sig   = fsinfo_t::SIG1;
        fsi->struct_sig = fsinfo_t::SIG2;
        fsi->free_count = free_count;
        fsi->next_free  = next_free;
        fsi->trail_sig  = fsinfo_t::SIG3;

        if (store_sector(_fsinfo_lba))
            return -1;

        _free_cluster_count = free_count;
        _next_free_cluster  = next_free;
        _fsinfo_valid       = true;
    }

    if (dir_load_volume_label_from_root())
        return -1;


    // Strict checks; this is last so the FS is still usable if we
    // fail here.
#ifdef FAT32_STRICT_MOUNT
    // Check FAT mirror consistency
    if (!(_ext_flags & MIRROR_DISABLED)) {
        for (uint32_t i = 1; i < _fat_count; i++) {
            const int cmp = fat_compare(0, i);
            if (cmp < 0)
                return -1;

            if (cmp > 0)
                return with_error(Error::FS_NEEDS_REPAIR);
        }
    }

    // Run fsck check
    fsck_report_t report;
    if (fsck(false, &report))
        return with_error(Error::FS_NEEDS_REPAIR);

    if (report.lost_clusters || report.cross_links || report.size_mismatches
        || report.invalid_entries || report.invalid_references)
        return with_error(Error::FS_NEEDS_REPAIR);
#endif
    return success();
}


int FileSys::sync()
{
    if (!_fsinfo_valid || !_fsinfo_dirty)
        return success();

    fsinfo_t *fsi;
    if (load_sector(_fsinfo_lba, (uint8_t**)&fsi))
        return -1;

    fsi->free_count = _free_cluster_count;
    fsi->next_free  = _next_free_cluster;

    if (store_sector(_fsinfo_lba))
        return -1;

    _fsinfo_dirty = false;

    // Flush any cache
    if (_bdev.flush()) 
        return with_error(Error::BDEV_FLUSH_ERR);

    return success();
}


void FileSys::build_83_name(const uint8_t *entry, char *out)
{
    char name[9] = {0};
    char ext[4]  = {0};

    ::memcpy(name, entry, 8);
    ::memcpy(ext, entry + 8, 3);

    /* Trim trailing spaces */
    for (int i = 7; i >= 0 && name[i] == ' '; i--)
        name[i] = 0;

    for (int i = 2; i >= 0 && ext[i] == ' '; i--)
        ext[i] = 0;

    if (ext[0])
        ::snprintf(out, 13, "%s.%s", name, ext);
    else
        ::snprintf(out, 13, "%s", name);
}


int FileSys::make_sfn(const char *name, uint8_t out[11])
{
    ::memset(out, ' ', 11);

    const char *dot = ::strchr(name, '.');
    const int base_len = dot ? (dot - name) : ::strlen(name);

    if (base_len < 1 || base_len > 8)
        return with_error(Error::MALFORMED_FILENAME);

    for (int i = 0; i < base_len; i++)
        out[i] = ::toupper(name[i]);

    if (dot) {
        const int ext_len = ::strlen(dot + 1);
        if (ext_len > 3)
            return with_error(Error::MALFORMED_FILENAME);

        for (int i = 0; i < ext_len; i++)
            out[8 + i] = ::toupper(dot[1 + i]);
    }

    return success();
}


int FileSys::dir_find(uint32_t cluster,
                      const char *name,
                      FileSys::File *file)
{
    uint8_t sname[11];
    if (make_sfn(name, sname))
        return -1;

    while (cluster < EOC) {
        const uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            uint8_t* sector;
            if (load_sector(lba + s, &sector))
                return -1;

            dirent_t *ent = (dirent_t*)sector;

            for (int i = 0; i < _bytes_per_sector / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00)
                    return with_error(Error::FILE_NOT_FOUND);

                if (ent[i].name[0] == 0xe5)
                    continue;

                if (!(ent[i].attr & DirentAttr::LFN)) {
                    if (!::memcmp(sname, ent[i].name, sizeof sname)) {

                        file->_first_cluster =
                            ((uint32_t)ent[i].first_cluster_hi << 16)
                            | ent[i].first_cluster_lo;

                        file->_current_cluster = file->_first_cluster;

                        file->_file_size  = ent[i].file_size;
                        file->_file_pos   = 0;
                        file->_dir_lba    = lba + s;
                        file->_dir_offset = i * sizeof(*ent);
                        file->_fs = this;
                        return success();
                    }
                }
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

    return with_error(Error::FILE_NOT_FOUND);
}


// For a path, find the parent dir cluster.  Path is clobbered.
int FileSys::dir_find_parent(char* path_buffer, bool tail, uint32_t* cluster)
{
    char *const slash = ::strrchr(path_buffer, '/');
    uint32_t c;
    if (!slash) {
        if (tail) {
            *cluster = _root_cluster;
            return success();
        }

        File f;
        if (dir_find(_root_cluster, path_buffer, &f))
            return -1;

        *cluster = f._first_cluster;

        return success();
    }

    const char* name = slash + 1;
    *slash = 0;

    uint32_t d;
    if (dir_find_parent(path_buffer, false, &d))
        return -1;
    
    if (tail) {
        *cluster = d;
        return 0;
    }

    File f;
    if (dir_find(d, name, &f))
        return -1;

    *cluster = f._first_cluster;

    return success();
}


int FileSys::dir_find_free_slot(uint32_t dir_cluster,
                                       uint32_t *out_lba,
                                       uint32_t *out_offset)
{
    while (true) {
        const uint32_t lba = cluster_to_lba(dir_cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            uint8_t* sector;
            if (load_sector(lba + s, &sector))
                return -1;

            dirent_t *ent = (dirent_t*)sector;

            for (int i = 0; i < _bytes_per_sector / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00
                    || ent[i].name[0] == 0xe5) {
                    *out_lba = lba + s;
                    *out_offset = i * sizeof(*ent);
                    return success();
                }
            }
        }

        uint32_t next;
        if (fat_get(dir_cluster, &next))
            return -1;

        if (next == EOC) {
            // Directory is full - grow it
            if (fat_allocate(&next))
                return -1;
            if (fat_set(dir_cluster, next))
                return -1;
            
            const uint32_t new_lba = cluster_to_lba(next);
            for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
                ::memset(_sector, 0, sizeof _sector);
                _sec_lba = new_lba + s;
                if (store_sector(new_lba+s))
                    return -1;
            }
        }

        dir_cluster = next;
    }
}


int FileSys::cluster_for_offset(uint32_t first_cluster,
                                uint32_t offset,
                                uint32_t *out_cluster,
                                uint32_t *cluster_index) 
{
    uint32_t cluster_size = _sectors_per_cluster * _bytes_per_sector;
    uint32_t index = offset / cluster_size;
    uint32_t cluster = first_cluster;

    for (uint32_t i = 0; i < index; i++) {
        if (fat_get(cluster, &cluster))
            return -1;

        if (cluster >= EOC)
            return -1;
    }

    *out_cluster = cluster;
    *cluster_index = index;
    return success();
}


int FileSys::File::ensure_cluster_index(uint32_t needed_index, uint32_t *out_cluster)
{
    uint32_t cluster = _first_cluster;
    uint32_t index = 0;

    while (index < needed_index) {
        uint32_t next;
        if (_fs->fat_get(cluster, &next))
            return -1;

        if (next >= EOC) {
            uint32_t new_cluster;
            if (_fs->fat_allocate(&new_cluster))
                return -1;

            if (_fs->fat_set(cluster, new_cluster))
                return -1;

            cluster = new_cluster;
        } else {
            cluster = next;
        }

        index++;
    }

    *out_cluster = cluster;
    return _fs->success();
}


int FileSys::File::update_dirent_size() 
{
    uint8_t* sector;
    if (_fs->load_sector(_dir_lba, &sector))
        return -1;

    dirent_t *ent = (dirent_t*)&sector[_dir_offset];

    ent->file_size = _file_size;

    return _fs->store_sector(_dir_lba);
}


int FileSys::open(const char *path, FileSys::File *file)
{
    uint32_t dir_cluster;

    const char* p = *path == '/' ? path + 1 : path;

    ::strncpy(_tmp, p, sizeof _tmp);
    _tmp[255] = 0;
    if (dir_find_parent(_tmp, true, &dir_cluster))
        return -1;

    return dir_find(dir_cluster, basename(path), file);
}


uint32_t FileSys::cluster_size()
{
    return _bytes_per_sector * _sectors_per_cluster;
}


int FileSys::fat_cluster_at(uint32_t start_cluster, uint32_t index, uint32_t* cluster)
{
    uint32_t c;
    for (c = start_cluster; index-- && c >= 2 && c < EOC; )
        if (fat_get(c, &c))
            return -1;
    
    *cluster = c;
    return success();
}


int FileSys::File::read(void *buffer, size_t len) 
{
    if (_file_pos >= _file_size)
        return _fs->success();

    if (_file_pos + len > _file_size)
        len = _file_size - _file_pos;

    uint8_t *out = (uint8_t*)buffer;
    uint32_t remaining = len;
    uint32_t total_read = 0;

    const uint32_t cluster_size = _fs->_sectors_per_cluster * _fs->_bytes_per_sector;

    while (remaining > 0) {
        uint32_t cluster;
        uint32_t cluster_index;

        if (_fs->cluster_for_offset(_first_cluster, _file_pos, &cluster, &cluster_index))
            return -1;

        const uint32_t cluster_offset = _file_pos % cluster_size;
        const uint32_t lba = _fs->cluster_to_lba(cluster)
            + (cluster_offset / _fs->_bytes_per_sector);

        uint8_t* sector;
        if (_fs->load_sector(lba, &sector))
            return -1;

        const uint32_t sector_offset = cluster_offset % _fs->_bytes_per_sector;
        const uint32_t to_copy = min(_fs->_bytes_per_sector - sector_offset, remaining);
        ::memcpy(out, sector + sector_offset, to_copy);

        out += to_copy;
        remaining -= to_copy;
        total_read += to_copy;
        _file_pos = min(_file_pos + to_copy, _file_size);
    }

    (void)_fs->success();
    return total_read;
}


int FileSys::File::write(const void *buffer, size_t len)
{
    const uint8_t *in = (const uint8_t*)buffer;

    uint32_t remaining = len;
    uint32_t total_written = 0;

    const uint32_t cluster_size = _fs->_sectors_per_cluster * _fs->_bytes_per_sector;

    while (remaining > 0) {
        const uint32_t needed_index = _file_pos / cluster_size;

        uint32_t cluster;
        if (ensure_cluster_index(needed_index, &cluster))
            return -1;

        const uint32_t cluster_offset = _file_pos % cluster_size;
        const uint32_t lba = _fs->cluster_to_lba(cluster)
            + (cluster_offset / _fs->_bytes_per_sector);

        uint8_t* sector;
        if (_fs->load_sector(lba, &sector))
            return -1;

        const uint32_t sector_offset = cluster_offset % _fs->_bytes_per_sector;
        const uint32_t to_copy = min(_fs->_bytes_per_sector - sector_offset, remaining);

        ::memcpy(sector + sector_offset, in, to_copy);

        /* DATA FIRST */
        if (_fs->store_sector(lba))
            return -1;

        in += to_copy;
        remaining -= to_copy;
        total_written += to_copy;
        _file_pos += to_copy;

        if (_file_pos > _file_size)
            _file_size = _file_pos;
    }

#if 0 // Update on close or sync
    /* After data + FAT updates, update size */
    if (update_dirent_size())
        return -1;
#endif
    (void)_fs->success();
    return total_written;
}

int FileSys::File::truncate(uint32_t new_size)
{
    const uint32_t cl_size = _fs->cluster_size();

    /* No-op */
    if (new_size == _file_size)
        return _fs->success();

    /* ---------------- SHRINK ---------------- */
    if (new_size < _file_size) {
        if (new_size == 0) {
            if (_first_cluster >= 2)
                _fs->fat_free_chain(_first_cluster);

            _first_cluster = 0;
        } else {
            const uint32_t last_cluster_index = (new_size - 1) / cl_size;
            uint32_t last_cluster;
            if (_fs->fat_cluster_at(_first_cluster, last_cluster_index, &last_cluster))
                return -1;

            uint32_t next;
            if (_fs->fat_get(last_cluster, &next))
                return -1;

            if (next >= 2 && next < EOC)
                if (_fs->fat_free_chain(next))
                    return -1;

            if (_fs->fat_set(last_cluster, EOC))
                return -1;
        }

        _file_size = new_size;
        _file_pos = min(_file_pos, new_size);

        return update_dirent_size();
    }

    /* ---------------- GROW ---------------- */

    const uint32_t needed_clusters = (new_size + cl_size - 1) / cl_size;

    uint32_t current_clusters = 0;

    if (_first_cluster >= 2) {
        uint32_t c = _first_cluster;
        while (c >= 2 && c < EOC) {
            ++current_clusters;
            if (_fs->fat_get(c, &c))
                return -1;
        }
    }

    while (current_clusters < needed_clusters) {
        uint32_t newc;
        if (_fs->fat_allocate(&newc))
            return -1;

        if (_first_cluster == 0) {
            _first_cluster = newc;
        } else {
            uint32_t last;
            if(_fs->fat_cluster_at(_first_cluster, current_clusters - 1, &last))
                return -1;

            if (_fs->fat_set(last, newc))
                return -1;
        }

        if (_fs->fat_set(newc, EOC))
            return -1;

        ++current_clusters;
    }

    _file_size = new_size;
    return update_dirent_size();
}


int FileSys::File::lseek(int32_t offset, SeekOp whence)
{
    uint32_t new_pos;

    switch (whence) {
    case SeekOp::SET:
        new_pos = offset;
        break;

    case SeekOp::CUR:
        new_pos = _file_pos + offset;
        break;

    case SeekOp::END:
        new_pos = _file_size + offset;
        break;
    }

    if ((int32_t)new_pos < 0)
        return _fs->with_error(Error::NEGATIVE_SEEK);

    _file_pos = new_pos;
    return _fs->success();
}


int FileSys::unlink(const char *path)
{
    File file;

    if (open(path, &file))
        return -1;

    /* 1. mark directory entry deleted */
    uint8_t* sector;
    if (load_sector(file._dir_lba, &sector))
        return -1;

    sector[file._dir_offset] = 0xe5;

    if (store_sector(file._dir_lba))
        return -1;

    /* 2. free cluster chain */
    return fat_free_chain(file._first_cluster);
}



int FileSys::File::sync()
{
    return update_dirent_size();
}


int FileSys::create(const char *path, FileSys::File *file)
{
    File f;

    /* Fail if exists */
    if (open(path, &f) == 0)
        return with_error(Error::ALREADY_EXISTS);

    const char* p = *path == '/' ? path + 1 : path;
    ::strncpy(_tmp, p, sizeof _tmp);
    _tmp[255] = 0;
    uint32_t dir_cluster;
    if (dir_find_parent(_tmp, true, &dir_cluster))
        return -1;

    uint32_t lba;
    uint32_t off;
    if (dir_find_free_slot(dir_cluster, &lba, &off))
        return -1;

    uint32_t cluster;
    if (fat_allocate(&cluster))
        return -1;

    uint8_t* sector;
    if (load_sector(lba, &sector))
        return -1;

    dirent_t *ent = (dirent_t*)&sector[off];

    ::memset(ent, 0, sizeof(*ent));

    if (make_sfn(basename(path), ent->name))
        return -1;

    ent->attr = DirentAttr::NONE;
    ent->first_cluster_lo = cluster & 0xffff;
    ent->first_cluster_hi = cluster >> 16;
    ent->file_size = 0;

    /* crash ordering:
       cluster already marked allocated
       now write directory entry */

    if (store_sector(lba))
        return -1;

    return open(path, file);
}


int FileSys::rename(const char *from_path, const char* to_path)
{
    // First form dest filename to make sure it's sensible
    const char* name = ::strrchr(to_path, '/');
    if (name)
        ++name;
    else
        name = to_path;

    uint8_t new_sfn[11];
    if (make_sfn(name, new_sfn))
        return -1;

    File old;
    if (open(from_path, &old))
        return -1;

    File f;
    if (open(to_path, &f) == 0)
        return with_error(Error::ALREADY_EXISTS);

    /* Extract parent directory cluster of new_path */
    const char* tp = *to_path == '/' ? to_path + 1 : to_path;
    ::strncpy(_tmp, tp, sizeof _tmp);
    _tmp[255] = 0;
    uint32_t to_dir_cluster;
    if (dir_find_parent(_tmp, true, &to_dir_cluster))
        return -1;

    uint8_t* sector;
    if (load_sector(old._dir_lba, &sector))
        return -1;

    dirent_t* entry = (dirent_t*)&sector[old._dir_offset];
    dirent_t old_entry = *entry;
    entry->name[0] = 0xe5;      // Mark old deleted

    if (store_sector(old._dir_lba))
        return -1;

    // Add to dest dir. A crash at this point means the file is
    // orphaned; it can never appear in two directories.

    uint32_t lba;
    uint32_t off;
    if (dir_find_free_slot(to_dir_cluster, &lba, &off))
        return -1;

    if (load_sector(lba, &sector))
        return -1;

    ::memcpy(old_entry.name, new_sfn, sizeof old_entry.name);
    *(dirent_t*)&sector[off] = old_entry;
    if (store_sector(lba))
        return -1;

    return success();
}


int FileSys::stat(const char *path, FileSys::Stat *st)
{
    File f;

    if (open(path, &f))
        return -1;

    st->_size = f._file_size;
    st->_first_cluster = f._first_cluster;
    st->_attributes = DirentAttr::NONE;

    /* reload directory entry */
    uint8_t* sector;
    _sec_lba = ~uint32_t(0);
    if (load_sector(f._dir_lba, &sector))
        return -1;
    
    const dirent_t *ent = (dirent_t*)(sector + f._dir_offset);

    st->_attributes = ent->attr;

    return success();
}


int FileSys::dir_is_empty(uint32_t cluster)
{
    while (cluster < EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            dirent_t* ent;
            if (load_sector(lba + s, (uint8_t**)&ent))
                return -1;

            for (int i = 0; i < _bytes_per_sector / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00) {
                    success();
                    return 1; /* end */
                }

                if (ent[i].name[0] == 0xe5)
                    continue;

                /* Skip "." and ".." */
                if ((ent[i].attr & DirentAttr::DIRECTORY) &&
                    (!::memcmp(ent[i].name, dot, 11)
                     || !::memcmp(ent[i].name, dotdot, 11)))
                    continue;

                return success(); /* 0 - not empty */
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

    success();
    return 1;
}


int FileSys::mkdir(const char *path)
{
    File f;

    /* Fail if exists */
    if (open(path, &f) == 0)
        return with_error(Error::ALREADY_EXISTS);

    const char* p = *path == '/' ? path + 1 : path;
    ::strncpy(_tmp, p, sizeof _tmp);
    _tmp[255] = 0;

    uint32_t parent_cluster;
    if (dir_find_parent(_tmp, true, &parent_cluster))
        return -1;

    uint32_t slot_lba;
    uint32_t slot_off;
    if (dir_find_free_slot(parent_cluster, &slot_lba, &slot_off))
        return -1;

    uint32_t new_cluster;
    if (fat_allocate(&new_cluster))
        return -1;

    /* Initialize new directory cluster */
    uint32_t lba = cluster_to_lba(new_cluster);

    ::memset(_sector, 0, _bytes_per_sector);

    dirent_t *ent = (dirent_t*)_sector;

    /* "." entry */
    ::memset(&ent[0], 0, sizeof(*ent));
    ::memcpy(ent[0].name, dot, 11);
    ent[0].attr = DirentAttr::DIRECTORY;
    ent[0].first_cluster_lo = new_cluster & 0xffff;
    ent[0].first_cluster_hi = new_cluster >> 16;

    /* ".." entry */
    ::memset(&ent[1], 0, sizeof(*ent));
    ::memcpy(ent[1].name, dotdot, 11);
    ent[1].attr = DirentAttr::DIRECTORY;
    ent[1].first_cluster_lo = parent_cluster & 0xffff;
    ent[1].first_cluster_hi = parent_cluster >> 16;

    if (store_sector(lba))
        return -1;

    /* Now write parent directory entry */
    uint8_t* sector;
    if (load_sector(slot_lba, &sector))
        return -1;

    dirent_t *slot = (dirent_t*)&sector[slot_off];

    ::memset(slot, 0, sizeof(*slot));

    if (make_sfn(basename(path), slot->name))
        return -1;

    slot->attr = DirentAttr::DIRECTORY;
    slot->first_cluster_lo = new_cluster & 0xffff;
    slot->first_cluster_hi = new_cluster >> 16;
    slot->file_size = 0;

    if (store_sector(slot_lba))
        return -1;

    return success();
}


int FileSys::rmdir(const char *path)
{
    File f;

    if (open(path, &f))
        return -1;

    /* Check empty */
    if (dir_is_empty(f._first_cluster) != 1)
        return with_error(Error::DIR_NOT_EMPTY);

    /* Free cluster */
    if (fat_set(f._first_cluster, 0))
        return -1;

    /* Mark directory entry deleted */
    uint8_t* sector;
    if (load_sector(f._dir_lba, &sector))
        return -1;

    dirent_t *ent = (dirent_t*)&sector[f._dir_offset];

    ent->name[0] = 0xe5;

    return store_sector(f._dir_lba);
}


// * static
const char* Fat32::basename(const char* path) 
{
    const char* slash = ::strrchr(path, '/');

    if (slash)
        return slash+1;

    return path;
}

// Tracks/Checks
//
//  *  Files
//  *  Directories
//  *  Cross-links
//  *  Invalid cluster references
//  *  Duplicate cluster use
//  *  Directory cluster usage
//  *  Read failures
//  *  LFN-safe traversal
//  *  Sector-based memory usage

int FileSys::fsck(bool fix, fsck_report_t* report)
{
    memset(report, 0, sizeof(*report));

    fsck_ctx_t ctx;
    ctx.total_clusters = _total_clusters;
    ctx.cluster_refcount = (uint8_t*)calloc(_total_clusters, sizeof(uint8_t));

    if (!ctx.cluster_refcount)
        return with_error(Error::FSCK_ALLOC_ERR);

    report->total_clusters = _total_clusters;

    // Check FAT mirrors
    fsck_verify_mirrors(&ctx, report, fix);

    /* Scan directory tree starting at root */
    report->directories = 1;        // root is implicit
    (void)fsck_scan_directory(&ctx, _root_cluster, fix, report);

    /* Detect lost clusters */
    for (uint32_t c = 2; c < _total_clusters; c++) {
        uint32_t val;
        if (!fat_get(c, &val)) {
            if (val != 0 && ctx.cluster_refcount[c] == 0) {
                ++report->lost_clusters;
#ifdef FAT32_FSCK_REPAIR
                if (fix)
                    fat_set(c, 0);
#endif
            }

            if (val == 0)
                report->free_clusters++;
            else
                report->referenced_clusters++;
        } // else log something? add to report?
    }

    free(ctx.cluster_refcount);

#ifdef FAT32_FSCK_REPAIR
    if (fix)
        sync();
#endif

    return success();
}


int FileSys::fsck_mark_chain(fsck_ctx_t *ctx, uint32_t start_cluster, fsck_report_t *report)
{
    for (uint32_t c = start_cluster; c >= 2 && c < EOC; ) {
        if (c >= ctx->total_clusters)
            return -1;

        ++ctx->cluster_refcount[c];

        if (ctx->cluster_refcount[c] > 1) {
            ++report->cross_links;
            return 1;
        }

        uint32_t next;
        if (fat_get(c, &next))
            return -1;

        if (next == c) {   /* self-loop */
            ++report->cross_links;
            return 1;
        }

        c = next;
    }

    return 0;
}


int FileSys::fsck_chain_length(uint32_t start, uint32_t* len)
{
    uint32_t n = 0;
    uint32_t c = start;

    while (c >= 2 && c < EOC) {
        ++n;
        if (fat_get(c, &c))
            return -1;
    }

    *len = n;
    return 0;
}


void FileSys::fsck_verify_mirrors(FileSys::fsck_ctx_t *ctx,
                                  FileSys::fsck_report_t* report,
                                  bool fix)
{
    if (_fat_count < 2)
        return;

    uint32_t primary = 0;

    if (_ext_flags & MIRROR_DISABLED)
        primary = _ext_flags & ACTIVE_FAT_MASK;

    for (uint32_t i = 0; i < _fat_count; i++) {
        if (i == primary)
            continue;
        
        const int cmp = fat_compare(primary, i);

        if (cmp < 0) {
            ++report->invalid_references;
            continue;
        }

        if (cmp > 0) {
            ++report->cross_links; /* reuse counter */

#ifdef FAT32_FSCK_REPAIR
            if (fix) {
                if (fat_copy(primary, i) == 0)
                    ++report->repairs;
            }
#endif
        }
    }
}


int FileSys::fsck_scan_directory(fsck_ctx_t *ctx,
                                 uint32_t dir_cluster,
                                 bool fix,
                                 fsck_report_t *report)
{
    uint32_t cluster = dir_cluster;

    if (cluster < 2 || cluster >= _total_clusters) {
        ++report->invalid_references;
        return -1;
    }

    while (cluster >= 2 && cluster < EOC) {

        /* Mark directory cluster itself */
        if (cluster >= _total_clusters) {
            ++report->invalid_references;
            return -1;
        }

        if (ctx->cluster_refcount[cluster]) {
            ++report->cross_links;
            return -1;
        }

        ctx->cluster_refcount[cluster] = 1;

        const uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            uint8_t* sector;
            if (load_sector(lba+s, &sector)) {
                ++report->invalid_references;
                return -1;
            }

            bool dirty = false;
            for (uint32_t off = 0; off < _bytes_per_sector; off += 32) {
                dirent_t *entry = (dirent_t*)&sector[off];

                if (entry->name[0] == 0x00)
                    return 0;

                if (entry->name[0] == 0xe5)
                    continue;

                const uint8_t attr = entry->attr;

                if (attr == DirentAttr::LFN || (attr & DirentAttr::VOLUME_ID))
                    continue;

                /* Skip "." and ".." */
                if (!::memcmp(entry->name, dot, 11) || !::memcmp(entry->name, dotdot, 11))
                    continue;

                /* Extract cluster */
                const uint32_t start_cluster = (entry->first_cluster_hi << 16) |
                    entry->first_cluster_lo;

                if (start_cluster >= _total_clusters) {
                    ++report->invalid_entries;
#ifdef FAT32_FSCK_REPAIR
                    if (fix) {
                        entry->name[0] = 0xe5;
                        dirty = true;
                    }
#endif
                    continue;
                }

                if (attr & DirentAttr::DIRECTORY) {
                    /* Directory */
                    ++report->directories;

                    if (start_cluster >= 2 && start_cluster < _total_clusters) {
                        if (dirty) {
                            if (store_sector(lba+s))
                                return -1;
                            dirty = false;
                        }
                        (void)fsck_scan_directory(ctx, start_cluster, fix, report);
                        if (load_sector(lba+s, &sector))
                            return -1;

                    } else {
                        ++report->invalid_references;
                    }
                } else {
                    /* File */
                    ++report->files;

                    if (start_cluster >= 2 && start_cluster < _total_clusters) {
                        if (dirty) {
                            if (store_sector(lba+s))
                                return -1;
                            dirty = false;
                        }
                        if (fsck_mark_chain(ctx, start_cluster, report)) {
                            ++report->invalid_references;
                            if (load_sector(lba+s, &sector))
                                return -1;
                            dirty = false;
#ifdef FAT32_FSCK_REPAIR
                            if (fix) {
                                entry->name[0] = 0xe5;
                                dirty = true;
                            }
#endif
                        } else {
                            if (load_sector(lba+s, &sector))
                                return -1;
                            dirty = false;
                        }

                        uint32_t chain_len;
                        if (fsck_chain_length(start_cluster, &chain_len))
                            return -1;

                        const uint32_t cluster_bytes =
                            chain_len * _sectors_per_cluster * _bytes_per_sector;

                        if (load_sector(lba+s, &sector))
                            return -1;
                        dirty = false;

                        if (cluster_bytes < entry->file_size) {
                            ++report->size_mismatches;
#ifdef FAT32_FSCK_REPAIR
                            if (fix) {
                                entry->file_size = cluster_bytes;
                                dirty = true;
                            }
#endif
                        }

                    } else if (start_cluster != 0) {
                        ++report->invalid_references;
                    }
                }
            }

            if (dirty) {
                if (store_sector(lba+s))
                    return -1;
                dirty = false;
            }
        }

        if (fat_get(cluster, &cluster)) {
            ++report->invalid_references;
            return -1;
        }
    }

    return 0;
}


int FileSys::fat_compare(uint32_t fat_a, uint32_t fat_b)
{
    static uint8_t sec_a[MAX_SECTOR_SIZE];

    success();                  // Clear any error

    uint32_t lba_a = _fat_start_lba + fat_a * _sectors_per_fat;
    uint32_t lba_b = _fat_start_lba + fat_b * _sectors_per_fat;

    for (uint32_t s = 0; s < _sectors_per_fat; s++) {
        uint8_t* sec_a_in;
        if (load_sector(lba_a, &sec_a_in))
            return -1;

        ::memcpy(sec_a, sec_a_in, sizeof sec_a);

        uint8_t* sec_b;
        if (load_sector(lba_b, &sec_b))
            return -1;

        if (::memcmp(sec_a, sec_b, _bytes_per_sector) != 0)
            return 1; /* mismatch */

        ++lba_a;
        ++lba_b;
    }

    return success(); /* identical */
}


int FileSys::fat_copy(uint32_t src, uint32_t dst)
{
    uint32_t lba_src = _fat_start_lba + src * _sectors_per_fat;
    uint32_t lba_dst = _fat_start_lba + dst * _sectors_per_fat;

    for (uint32_t s = 0; s < _sectors_per_fat; s++) {
        uint8_t* sector;
        if (load_sector(lba_src, &sector))
            return -1;

        if (store_sector(lba_dst))
            return -1;
    }

    return success();
}



int FileSys::opendir(const char *path, FileSys::DIR *dir)
{
    memset(dir, 0, sizeof(*dir));

    dir->_fs = this;

    // Special case root directory; it doesn't have a dirent anywhere,
    // so has no usual properties like modification times etc.
    if (!*path || !strcmp(path, "/")) { 
        dir->_start_cluster = _root_cluster;
        dir->_current_cluster = _root_cluster;
    } else {
        uint32_t cluster;
        uint8_t attr;

        File d;

        if (open(path, &d))
            return -1;

        Stat st;
        if (stat(path, &st))
            return -1;

        attr = st._attributes;

        if (!(attr & DirentAttr::DIRECTORY))  /* must be directory */
            return with_error(Error::NOT_DIRECTORY);

        dir->_start_cluster = d._first_cluster;
        dir->_current_cluster = d._first_cluster;
    }

    dir->_sector_index = 0;
    dir->_entry_offset = 0;

    return success();
}


int FileSys::DIR::readdir(entry_t *out)
{
    while (_current_cluster >= 2 && _current_cluster < EOC) {
        const uint32_t lba = _fs->cluster_to_lba(_current_cluster);

        while (_sector_index < _fs->_sectors_per_cluster) {

            if (_entry_offset == 0) {
                uint8_t* buf;
                if (_fs->load_sector(lba + _sector_index, &buf))
                    return -1;

                ::memcpy(_sector, buf, sizeof _sector);
            }

            while (_entry_offset < _fs->_bytes_per_sector) {
                const dirent_t *entry = (dirent_t*)(_sector + _entry_offset);

                _entry_offset += 32;

                /* End of directory */
                if (entry->name[0] == 0x00)
                    return 0;

                /* Deleted */
                if (entry->name[0] == 0xe5)
                    continue;

                const DirentAttr attr = entry->attr;

                /* Skip LFN entries */
                if (attr == DirentAttr::LFN)
                    continue;

                /* Skip "." and ".." */
                if (!::memcmp(entry->name, dot, 11) || !::memcmp(entry->name, dotdot, 11))
                    continue;

                ::memset(out, 0, sizeof(*out));

                _fs->build_83_name(entry->name, out->name);

                out->attr = attr;

                out->size = entry->file_size;
                out->first_cluster = (entry->first_cluster_hi << 16) | entry->first_cluster_lo;

                return 1; /* entry returned */
            }

            _entry_offset = 0;
            ++_sector_index;
        }

        _sector_index = 0;
        if (_fs->fat_get(_current_cluster, &_current_cluster))
            return -1;
    }

    return 0;
}


void FileSys::DIR::closedir()
{
    (void)this;
}
