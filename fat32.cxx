#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "fat32.h"

using namespace Fat32;

// 
// *** Crash Behavior Analysis ***
//
// If crash during write:
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

int FileSys::load_sector(uint32_t lba, uint8_t** sector)
{
    if (lba == _sec_lba) {
        *sector = _sector;
        return 0;
    }

    if (_bdev.read_blocks(lba, 1, _sector))
        return -1;

    *sector = _sector;
    _sec_lba = lba;
    return 0;
}


int FileSys::store_sector(uint32_t lba)
{
    if (_bdev.write_blocks(lba, 1, _sector))
        return -1;

    _sec_lba = lba;
    return 0;
}


uint32_t FileSys::cluster_to_lba(uint32_t cluster)
{
    return _data_start_lba + ((cluster - 2) * _sectors_per_cluster);
}


int FileSys::fat_get(uint32_t cluster, uint32_t *val)
{
    uint32_t off = cluster * 4;
    uint32_t lba = _fat_start_lba + (off / SECTOR_SIZE);
    uint32_t pos = off % SECTOR_SIZE;

    uint8_t* sector;
    if (load_sector(lba, &sector))
        return -1;

    *val = (*(uint32_t*)&sector[pos]) & 0x0fffffff;  // XXX symbol
    return 0;
}


int FileSys::fat_set_single(uint32_t cluster,
                                   uint32_t val,
                                   uint32_t fat_index)
{
    const uint32_t off = cluster * 4;
    const uint32_t base = _fat_start_lba + fat_index * _sectors_per_fat;

    const uint32_t lba = base + (off / SECTOR_SIZE);
    const uint32_t pos = off % SECTOR_SIZE;

    uint8_t* sector;
    if (load_sector(lba, &sector))
        return -1;

    uint32_t *entry = (uint32_t*)&sector[pos];
    *entry = (*entry & 0xf0000000) | (val & 0x0fffffff); // XXX make symbolic

    if (store_sector(lba))
         return -1;

    return 0;
}


int FileSys::fat_set(uint32_t cluster, uint32_t val)
{
    for (uint32_t i = 0; i < _fat_count; i++)
        if (fat_set_single(cluster, val, i))
            return -1;
    return 0;
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

                return 0;
            }
        }

        /* wrap once */
        start = 2;
    }

    return -1; /* full */
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

    return 0;
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

    return 0;
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

            for (int i = 0; i < SECTOR_SIZE / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00)
                    return -1;

                if (ent[i].name[0] == 0xe5)
                    continue;

                if (ent[i].attr == DirentAttr::VOLUME_ID) {
                    memcpy(_volume_label, ent[i].name, 11);
                    _volume_label[11] = 0;

                    for (int end = 10; end >= 0 && _volume_label[end] == ' '; --end)
                        _volume_label[end] = 0;

                    return 0;
                }
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

    return -1;
}


int FileSys::mount()
{
    bpb_t *bpb;
    if (load_sector(0, (uint8_t**)&bpb))
        return -1;

    _reserved_sectors   = bpb->reserved_sector_count;
    _sectors_per_cluster= bpb->sectors_per_cluster;
    _sectors_per_fat    = bpb->fat_size_32;
    _fat_count          = bpb->num_fats;
    _root_cluster       = bpb->root_cluster;

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

    return dir_load_volume_label_from_root();
}


int FileSys::checkpoint()
{
    if (!_fsinfo_valid || !_fsinfo_dirty)
        return 0;

    fsinfo_t *fsi;
    if (load_sector(_fsinfo_lba, (uint8_t**)&fsi))
        return -1;

    fsi->free_count = _free_cluster_count;
    fsi->next_free  = _next_free_cluster;

    if (store_sector(_fsinfo_lba))
        return -1;

    _fsinfo_dirty = false;
    return 0;
}


// * static
int Fat32::make_sfn(const char *name, uint8_t out[11])
{
    memset(out, ' ', 11);

    const char *dot = strchr(name, '.');
    const int base_len = dot ? (dot - name) : strlen(name);

    if (base_len < 1 || base_len > 8)
        return -1;

    for (int i = 0; i < base_len; i++)
        out[i] = toupper(name[i]);

    if (dot) {
        const int ext_len = strlen(dot + 1);
        if (ext_len > 3)
            return -1;

        for (int i = 0; i < ext_len; i++)
            out[8 + i] =
                toupper(dot[1 + i]);
    }

    return 0;
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

            for (int i = 0; i < SECTOR_SIZE / sizeof(*ent); i++)
            {
                if (ent[i].name[0] == 0x00)
                    return -1;

                if (ent[i].name[0] == 0xe5)
                    continue;

                if (!(ent[i].attr & DirentAttr::LFN)) {
                    if (!memcmp(sname, ent[i].name, sizeof sname)) {

                        file->_first_cluster =
                            ((uint32_t)ent[i].first_cluster_hi << 16)
                            | ent[i].first_cluster_lo;

                        file->_current_cluster = file->_first_cluster;

                        file->_file_size  = ent[i].file_size;
                        file->_file_pos   = 0;
                        file->_dir_lba    = lba + s;
                        file->_dir_offset = i * sizeof(*ent);
                        file->_fs = this;
                        return 0;
                    }
                }
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

    return -1;
}


// For a path, find the parent dir cluster.  Path is clobbered.
int FileSys::dir_find_parent(char* path_buffer, bool tail, uint32_t* cluster)
{
    char *const slash = strrchr(path_buffer, '/');
    uint32_t c;
    if (!slash) {
        if (tail) {
            *cluster = _root_cluster;
            return 0;
        }

        File f;
        if (dir_find(_root_cluster, path_buffer, &f))
            return -1;

        *cluster = f._first_cluster;

        return 0;
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

    return 0;
}


int FileSys::dir_find_free_slot(uint32_t dir_cluster,
                                       uint32_t *out_lba,
                                       uint32_t *out_offset)
{
    while (dir_cluster < EOC) {
        const uint32_t lba = cluster_to_lba(dir_cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            uint8_t* sector;
            if (load_sector(lba + s, &sector))
                return -1;

            dirent_t *ent = (dirent_t*)sector;

            for (int i = 0; i < SECTOR_SIZE / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00
                    || ent[i].name[0] == 0xe5) {
                    *out_lba = lba + s;
                    *out_offset = i * sizeof(*ent);
                    return 0;
                }
            }
        }

        if (fat_get(dir_cluster, &dir_cluster))
            return -1;
    }

    return -1;
}


int FileSys::open(const char *path, FileSys::File *file)
{
    uint32_t dir_cluster;

    char tmp[256];
    strncpy(tmp, path, sizeof tmp);
    tmp[255] = 0;
    if (dir_find_parent(tmp, true, &dir_cluster))
        return -1;

    return dir_find(dir_cluster, basename(path), file);
}


int FileSys::File::read(void *buffer, size_t len) 
{
    uint8_t *out = (uint8_t*)buffer;

    if (_file_pos + len > _file_size)
        len = _file_size - _file_pos;

    size_t remaining = len;

    while (remaining > 0 && _current_cluster < EOC) {
        const uint32_t lba = _fs->cluster_to_lba(_current_cluster);

        for (uint32_t s = 0; s < _fs->_sectors_per_cluster && remaining > 0; s++) {
            uint8_t* sector;
            if (_fs->load_sector(lba + s, &sector))
                return -1;

            const size_t copy = min<size_t>(remaining, SECTOR_SIZE);
            memcpy(out, sector, copy);

            out += copy;
            remaining -= copy;
            _file_pos += copy;

            //assert(remaining >= 0);
        }

        if (_fs->fat_get(_current_cluster, &_current_cluster))
            return -1;
    }

    return -(remaining != 0);
}


int FileSys::File::write(const void *buffer, size_t len)
{
    const uint8_t *in = (const uint8_t*)buffer;
    size_t remaining = len;

    while (remaining > 0) {
        const uint32_t lba = _fs->cluster_to_lba(_current_cluster);

        for (uint32_t s = 0; s < _fs->_sectors_per_cluster && remaining; s++) {
            memset(_fs->_sector, 0, SECTOR_SIZE);

            const size_t copy = min<size_t>(remaining, SECTOR_SIZE);
            memcpy(_fs->_sector, in, copy);
            if (_fs->store_sector(lba + s))
                return -1;

            in += copy;
            remaining -= copy;
            _file_pos += copy;
            _file_size += copy;
        }

        if (remaining > 0) {
            uint32_t new_cluster;
            if (_fs->fat_allocate(&new_cluster))
                return -1;

            /* Safe ordering:
               1. new cluster already marked EOC
               2. link previous -> new
            */
            if (_fs->fat_set(_current_cluster, new_cluster))
                return -1;

            _current_cluster = new_cluster;
        }
    }

    /* update directory size AFTER data write */
    uint8_t* sector;
    if (_fs->load_sector(_dir_lba, &sector))
        return -1;

    dirent_t *ent = (dirent_t*)&sector[_dir_offset];

    ent->file_size = _file_size;

    if (_fs->store_sector(_dir_lba))
        return -1;

    return len;
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


int FileSys::File::close()
{
    return 0;
}


int FileSys::create(const char *path, FileSys::File *file)
{
    File f;

    /* Fail if exists */
    if (open(path, &f) == 0)
        return -1;

    uint32_t dir_cluster;
    char tmp[256];
    strncpy(tmp, path, sizeof tmp);
    tmp[255] = 0;
    if (dir_find_parent(tmp, true, &dir_cluster))
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

    memset(ent, 0, sizeof(*ent));

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
    
    dirent_t *ent = (dirent_t*)(sector + f._dir_offset);

    st->_attributes = ent->attr;

    return 0;
}


int FileSys::dir_is_empty(uint32_t cluster)
{
    static const uint8_t dot[12] = ".          ";
    static const uint8_t dotdot[12] = "..         ";
    
    while (cluster < EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < _sectors_per_cluster; s++) {
            uint8_t* sector;
            if (load_sector(lba + s, &sector))
                return -1;

            dirent_t *ent = (dirent_t*)sector;

            for (int i = 0; i < SECTOR_SIZE / sizeof(*ent); i++) {
                if (ent[i].name[0] == 0x00)
                    return 1; /* end */

                if (ent[i].name[0] == 0xe5)
                    continue;

                /* Skip "." and ".." */
                if ((ent[i].attr & DirentAttr::DIRECTORY) &&
                    (!memcmp(ent[i].name, dot, 11)
                     || !memcmp(ent[i].name, dotdot, 11)))
                    continue;

                return 0; /* not empty */
            }
        }

        if (fat_get(cluster, &cluster))
            return -1;
    }

    return 1;
}


int FileSys::mkdir(const char *path)
{
    File f;

    /* Fail if exists */
    if (open(path, &f) == 0)
        return -1;

    uint32_t parent_cluster;
    char tmp[256];
    strncpy(tmp, path, sizeof tmp);
    tmp[255] = 0;
    if (dir_find_parent(tmp, true, &parent_cluster))
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

    memset(_sector, 0, SECTOR_SIZE);

    dirent_t *ent = (dirent_t*)_sector;

    /* "." entry */
    memset(&ent[0], 0, sizeof(*ent));
    memset(ent[0].name, ' ', 11);
    ent[0].name[0] = '.';
    ent[0].attr = DirentAttr::DIRECTORY;
    ent[0].first_cluster_lo = new_cluster & 0xffff;
    ent[0].first_cluster_hi = new_cluster >> 16;

    /* ".." entry */
    memset(&ent[1], 0, sizeof(*ent));
    memset(ent[1].name, ' ', 11);
    ent[1].name[0] = '.';
    ent[1].name[1] = '.';
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

    memset(slot, 0, sizeof(*slot));

    if (make_sfn(basename(path), slot->name))
        return -1;

    slot->attr = DirentAttr::DIRECTORY;
    slot->first_cluster_lo = new_cluster & 0xffff;
    slot->first_cluster_hi = new_cluster >> 16;
    slot->file_size = 0;

    if (store_sector(slot_lba))
        return -1;

    return 0;
}


int FileSys::rmdir(const char *path)
{
    File f;

    if (open(path, &f))
        return -1;

    /* Check empty */
    if (dir_is_empty(f._first_cluster) != 1)
        return -1;

    /* Free cluster */
    if (fat_set(f._first_cluster, 0))
        return -1;

    /* Mark directory entry deleted */
    uint8_t* sector;
    if (load_sector(f._dir_lba, &sector))
        return -1;

    dirent_t *ent = (dirent_t*)&sector[f._dir_offset];

    ent->name[0] = 0xe5;

    if (store_sector(f._dir_lba))
        return -1;

    return 0;
}


// * static
const char* Fat32::basename(const char* path) 
{
    const char* slash = strrchr(path, '/');

    if (slash)
        return slash+1;

    return path;
}
