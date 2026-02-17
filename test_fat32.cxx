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

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "blockdev.h"
#include "fat32.h"
#include "cache.h"
#include "gptmap.h"

#define TEST_IMAGE "test.img"
#define SECTOR_SIZE 512

/* ================= POSIX BLOCK DEVICE ================= */

class PosixBlockDev : public BlockDev {
public:
    int fd;
    int numloads;
    int numstores;
    struct stat st;

    PosixBlockDev()
        : fd(-1), numloads(0), numstores(0)
    { }

    int init() {
        if (::fstat(fd, &st) == -1)
            return -1;

        return 0;
    }


    int read_blocks(uint32_t lba, uint32_t count, void *buffer, bool)
    {
        //printf("Load LBA: %d\n", lba);
        numloads++;

        const off_t offset = (off_t)lba * SECTOR_SIZE;

        if (lseek(fd, offset, SEEK_SET) < 0)
            return -1;

        const ssize_t bytes = ::read(fd, buffer, count * SECTOR_SIZE);

        if (bytes != (ssize_t)(count * SECTOR_SIZE))
            return -1;

        return 0;
    }


    int write_blocks(uint32_t lba, uint32_t count, const void *buffer, bool)
    {
        //printf("Write LBA: %d\n", lba);
        numstores++;

        const off_t offset = (off_t)lba * SECTOR_SIZE;

        if (lseek(fd, offset, SEEK_SET) < 0)
            return -1;

        const ssize_t bytes = ::write(fd, buffer, count * SECTOR_SIZE);

        if (bytes != (ssize_t)(count * SECTOR_SIZE))
            return -1;

        ::fsync(fd);
        return 0;
    }

    int flush() { ::fsync(fd); return 0; }

    uint32_t sector_size() const { return SECTOR_SIZE; }

    uint32_t size() const { return st.st_size / SECTOR_SIZE; }
};


// FAT date and time conversion

int fat_time_from_posix(time_t t,
                        uint16_t *fat_date,
                        uint16_t *fat_time)
{
    if (!fat_date || !fat_time)
        return -1;

    struct tm tm;
    if (localtime_r(&t, &tm) == NULL)
        return -1;

    int year = tm.tm_year + 1900;

    if (year < 1980)
        year = 1980;
    else if (year > 2107)
        year = 2107;

    *fat_date = ((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;

    /* 2-second resolution */
    *fat_time = (tm.tm_hour << 11) | (tm.tm_min  << 5)  | ((tm.tm_sec / 2) & 0x1f);

    return 0;
}


int fat_time_to_posix(uint16_t fat_date,
                      uint16_t fat_time,
                      time_t *out)
{
    if (!out)
        return -1;

    if (fat_date == 0)
        return -1;  /* strict: treat zero as invalid */

    int year  = ((fat_date >> 9) & 0x7F) + 1980;
    int month = (fat_date >> 5) & 0x0F;
    int day   = fat_date & 0x1F;

    int hour  = (fat_time >> 11) & 0x1F;
    int min   = (fat_time >> 5)  & 0x3F;
    int sec   = (fat_time & 0x1F) * 2;

    if (year < 1980 || year > 2107)
        return -1;
    if (month < 1 || month > 12)
        return -1;
    if (day < 1 || day > 31)
        return -1;
    if (hour > 23 || min > 59 || sec > 59)
        return -1;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    tm.tm_year = year - 1900;
    tm.tm_mon  = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;
    tm.tm_isdst = -1;  /* let libc determine DST */

    time_t t = mktime(&tm);
    if (t == (time_t)-1)
        return -1;

    /* Re-validate: ensure mktime didn’t normalize invalid date */
    struct tm verify;
    if (localtime_r(&t, &verify) == NULL)
        return -1;

    if (verify.tm_year != tm.tm_year ||
        verify.tm_mon  != tm.tm_mon  ||
        verify.tm_mday != tm.tm_mday ||
        verify.tm_hour != tm.tm_hour ||
        verify.tm_min  != tm.tm_min  ||
        verify.tm_sec  != tm.tm_sec)
    {
        return -1;  /* invalid calendar value (e.g., Feb 30) */
    }

    *out = t;
    return 0;
}


// Global used by Fat32 for the current timestamp

void fat32_now(uint16_t* fat_date, uint16_t* fat_time)
{
    // Yesterday
    fat_time_from_posix(time(NULL)-24*60*60, fat_date, fat_time);
}


/* ================= TEST HELPERS ================= */

void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

void test_mount(Fat32::FileSys *fs)
{
    printf("TEST: mount\n");
    assert(fs->mount() == 0);
    printf("  Volume Label: \"%s\"\n", fs->volume_label());
}

void test_open_nonexistent(Fat32::FileSys *fs)
{
    printf("TEST: open nonexistent file\n");
    Fat32::FileSys::File f;
    assert(fs->open("nope.txt", &f) != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);
}

void test_create_write_read_delete(Fat32::FileSys *fs)
{
    printf("TEST: write existing file\n");

    const char *filename = "testfile.txt";
    const char *data = "Hello FAT32 test suite!";
    char buffer[128];

    Fat32::FileSys::File f;

    assert(fs->create(filename, &f) == 0);

    assert(f.write(data, strlen(data)) == (int)strlen(data));
    assert(f.close() == 0);

    assert(fs->open(filename, &f) == 0);

    memset(buffer, 0, sizeof(buffer));
    assert(f.read(buffer, sizeof(buffer)) >= 0);

    assert(strncmp(buffer, data, strlen(data)) == 0);

    assert(f.close() == 0);
}

void test_multilevel_path(Fat32::FileSys *fs)
{
    printf("TEST: multi-level path\n");

    Fat32::FileSys::File f;

    assert(fs->mkdir("DIR1") == 0);
    assert(fs->create("DIR1/FILE1.TXT", &f) == 0);

    if (fs->open("DIR1/FILE1.TXT", &f) == 0) {
        printf("  Found DIR1/FILE1.TXT\n");
        assert(f.close() == 0);
    } else {
        printf("  Skipping (directory not present)\n");
    }
}

void test_stat(Fat32::FileSys *fs)
{
    printf("TEST: open-as-stat\n");

    const char *filename = "statfile.txt";
    const char *data = "Stat test data";
    Fat32::FileSys::File f;

    /* Ensure file does not exist */
    fs->unlink(filename);

    /* Create */
    assert(fs->create(filename, &f) == 0);

    /* Initial stat */
    Fat32::FileSys::File st;
    assert(fs->open(filename, &st) == 0);
    assert(st._file_size == 0);
    assert(st._first_cluster == 0);
    assert((st.attributes() & Fat32::DirentAttr::DIRECTORY) == 0);

    /* Write data */
    assert(fs->open(filename, &f) == 0);
    assert(f.write(data, strlen(data)) == (int)strlen(data));
    assert(f.close() == 0);

    /* Stat again */
    assert(fs->open(filename, &st) == 0);
    assert(st._file_size == strlen(data));
    assert(st._first_cluster >= 2);

    /* Reopen and verify stat consistent */
    assert(fs->open(filename, &f) == 0);
    assert(f._file_size == st._file_size);
    assert(f.close() == 0);

    /* Delete */
    assert(fs->unlink(filename) == 0);

    /* open must now fail */
    assert(fs->open(filename, &st) != 0);

    printf("  open-as-stat passed\n");
}

void test_dirops(Fat32::FileSys* fs)
{
    printf("TEST: mkdir, rmdir\n");

    assert(fs->mkdir("DIR10") == 0);
    assert(fs->mkdir("DIR10") != 0 && fs->last_error() == Fat32::Error::ALREADY_EXISTS);
    assert(fs->rmdir("DIR10") == 0);
    assert(fs->rmdir("DIR10") != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);


    assert(fs->mkdir("DIR11") == 0);
    Fat32::FileSys::File f;
    assert(fs->create("DIR11/FILE.TXT", &f) == 0);
    assert(f.close() == 0);
    assert(fs->rmdir("DIR11") != 0  && fs->last_error() == Fat32::Error::DIR_NOT_EMPTY);

    printf("  mkdir, rmdir passed\n");
}

void test_psinfo_write(Fat32::FileSys* fs)
{
    printf("TEST: FS sync\n");

    assert(fs->sync() == 0);
    printf("  FS sync passed\n");
}


// Basic Write / Read Roundtrip
void test_basic_write_read(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "basic.txt";
    const char *data = "hello fat32";
    char buf[64];

    assert(fs->create(name, &f) == 0);
    assert(f.write(data, strlen(data)) == (int)strlen(data));

    assert(f.lseek(0, Fat32::SeekOp::SET) == 0);

    memset(buf, 0, sizeof(buf));
    assert(f.read(buf, sizeof(buf)) == (int)strlen(data));
    assert(strcmp(buf, data) == 0);
    
    assert(f.close() == 0);
}


// Overwrite In Middle (No Append)
void test_overwrite_middle(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "overwrt.txt";
    char buf[32];

    assert(fs->create(name, &f) == 0);
    assert(f.write("abcdef", 6) == 6);

    assert(f.lseek(2, Fat32::SeekOp::SET) == 0);
    assert(f.write("ZZ", 2) == 2);

    assert(f.lseek(0, Fat32::SeekOp::SET) == 0);
    assert(f.read(buf, 6) == 6);

    assert(memcmp(buf, "abZZef", 6) == 0);

    assert(f.close() == 0);
}


// Seek Beyond EOF Then Write (File Growth)
void test_seek_beyond_eof_write(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "grow.txt";
    char buf[32];

    assert(fs->create(name, &f) == 0);
    assert(f.write("abc", 3) == 3);

    assert(f.lseek(10, Fat32::SeekOp::SET) == 0);
    assert(f.write("X", 1) == 1);

    Fat32::FileSys::File st;
    assert(fs->open(name, &st) == 0);

    assert(f.lseek(0, Fat32::SeekOp::SET) == 0);
    assert(f.read(buf, 11) == 11);

    assert(buf[0] == 'a');
    assert(buf[1] == 'b');
    assert(buf[2] == 'c');
    assert(buf[10] == 'X');

    assert(f.close() == 0);
}


// Truncate Shrink
void test_truncate_shrink(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "shrink.txt";
    char buf[32];

    assert(fs->create(name, &f) == 0);
    assert(f.write("0123456789", 10) == 10);

    assert(f.truncate(4) == 0);

    assert(f.lseek(0, Fat32::SeekOp::SET) == 0);
    assert(f.read(buf, 16) == 4);
    assert(memcmp(buf, "0123", 4) == 0);

    assert(f.close() == 0);
}


// Truncate Grow
void test_truncate_grow(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "grow_tr.txt";
    char buf[64];

    assert(fs->create(name, &f) == 0);
    assert(f.write("abc", 3) == 3);

    assert(f.truncate(20) == 0);

    assert(f.lseek(0, Fat32::SeekOp::SET) == 0);
    assert(f.read(buf, 20) == 20);

    assert(memcmp(buf, "abc", 3) == 0);

    assert(f.close() == 0);
}


// Truncate To Zero (Cluster Free)
void test_truncate_zero(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    const char *name = "zero.txt";

    assert(fs->create(name, &f) == 0);
    assert(f.write("long long data", 14) == 14);
    assert(f.truncate(0) == 0);
    assert(f.close() == 0);

    Fat32::FileSys::File st;
    assert(fs->open(name, &st) == 0);
    assert(st._file_size == 0);
}


void test_rename(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    Fat32::FileSys::File st;
    char buf[32];

    const char *oldname = "oldname.txt";
    const char *newname = "newname.txt";

    printf("TEST: rename\n");

    /* Ensure clean */
    fs->unlink(oldname);
    fs->unlink(newname);

    /* Create and write */
    assert(fs->create(oldname, &f) == 0);
    assert(f.write("rename-test", 11) == 11);
    assert(f.close() == 0);

    /* Rename */
    assert(fs->rename(oldname, newname) == 0);

    /* Old must not exist */
    assert(fs->open(oldname, &st) != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);

    /* New must exist */
    assert(fs->open(newname, &st) == 0);
    assert(st._file_size == 11);

    /* Verify contents */
    assert(fs->open(newname, &f) == 0);
    assert(f.read(buf, 11) == 11);
    assert(memcmp(buf, "rename-test", 11) == 0);
    assert(f.close() == 0);

    printf("  rename passed\n");
}

void test_cross_directory_rename(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    Fat32::FileSys::File st;
    char buf[64];

    const char *dir1 = "dirA";
    const char *dir2 = "dirB";
    const char *oldpath = "dirA/file.txt";
    const char *newpath = "dirB/file.txt";

    printf("TEST: cross-directory rename\n");

    /* Cleanup in case previous run failed */
    fs->unlink(oldpath);
    fs->unlink(newpath);
    fs->rmdir(dir1);
    fs->rmdir(dir2);

    /* Create directories */
    assert(fs->mkdir(dir1) == 0);
    assert(fs->mkdir(dir2) == 0);

    /* Create file in dirA */
    assert(fs->create(oldpath, &f) == 0);
    assert(f.write("cross-move-test", 15) == 15);
    assert(f.close() == 0);

    /* Rename (move) into dirB */
    assert(fs->rename(oldpath, newpath) == 0);

    /* Old must not exist */
    assert(fs->open(oldpath, &st) != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);
    
    /* New must exist */
    assert(fs->open(newpath, &st) == 0);
    assert(st._file_size == 15);

    /* Verify content */
    assert(fs->open(newpath, &f) == 0);
    assert(f.read(buf, 15) == 15);
    assert(memcmp(buf, "cross-move-test", 15) == 0);
    assert(f.close() == 0);

    printf("  cross-directory rename passed\n");
}


void test_cross_directory_rename_dest_exists(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;

    const char *dir1 = "dirC";
    const char *dir2 = "dirD";
    const char *oldpath = "dirC/file.txt";
    const char *newpath = "dirD/file.txt";

    printf("TEST: cross-directory rename fails if dest exists\n");

    fs->unlink(oldpath);
    fs->unlink(newpath);
    fs->rmdir(dir1);
    fs->rmdir(dir2);

    assert(fs->mkdir(dir1) == 0);
    assert(fs->mkdir(dir2) == 0);

    /* Create source */
    assert(fs->create(oldpath, &f) == 0);
    assert(f.close() == 0);

    /* Create destination */
    assert(fs->create(newpath, &f) == 0);
    assert(f.close() == 0);

    /* Rename must fail */
    assert(fs->rename(oldpath, newpath) != 0 && fs->last_error() == Fat32::Error::ALREADY_EXISTS);

    printf("  rename-dest-exists passed\n");
}


/* ================= READDIR ================= */

/* Helper: collect directory names into array */
static int collect_names(Fat32::FileSys* fs,
                         const char *path,
                         char names[][13],
                         int max)
{
    Fat32::FileSys::DIR dir;
    Fat32::FileSys::DIR::entry_t ent;
    int count = 0;

    assert(fs->opendir(path, &dir) == 0);

    while (dir.readdir(&ent) > 0) {
        if (count < max) {
            ::strncpy(names[count], ent.name, 13);
            count++;
        }
    }

    dir.closedir();
    return count;
}


static void test_empty_directory(Fat32::FileSys* fs)
{
    assert(fs->mkdir("EMPTY") == 0);

    char names[16][13];
    int count = collect_names(fs, "EMPTY", names, 16);

    assert(count == 0);
}


static void test_single_file(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    assert(fs->create("FILE100.TXT", &f) == 0);

    char names[50][13];
    int count = collect_names(fs, "", names, 50);

    int found = 0;
    for (int i = 0; i < count; i++)
        if (::strcmp(names[i], "FILE100.TXT") == 0)
            found = 1;

    assert(found);
}


static void test_multiple_files(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    assert(fs->create("A.TXT", &f) == 0);
    assert(fs->create("B.TXT", &f) == 0);
    assert(fs->create("C.TXT", &f) == 0);

    char names[50][13];
    int count = collect_names(fs, "", names, 50);

    int a=0,b=0,c=0;

    for (int i=0;i<count;i++) {
        if (!strcmp(names[i],"A.TXT")) a=1;
        if (!strcmp(names[i],"B.TXT")) b=1;
        if (!strcmp(names[i],"C.TXT")) c=1;
    }

    assert(a && b && c);
}


static void test_subdirectory(Fat32::FileSys* fs)
{
    assert(fs->mkdir("DIR9") == 0);
    Fat32::FileSys::File f;
    assert(fs->create("DIR9/INNER.TXT", &f) == 0);

    char names[50][13];
    int count = collect_names(fs, "DIR9", names, 50);

    assert(count == 1);
    assert(strcmp(names[0], "INNER.TXT") == 0);
}


static void test_deleted_entries(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    assert(fs->create("DELME.TXT", &f) == 0);
    assert(fs->unlink("DELME.TXT") == 0);

    char names[16][13];
    int count = collect_names(fs, "", names, 16);

    for (int i = 0; i < count; i++)
        assert(strcmp(names[i], "DELME.TXT") != 0);
}


static void test_large_directory(Fat32::FileSys* fs)
{
    char name[20];

    Fat32::FileSys::File f;
    for (int i = 0; i < 200; i++) {
        ::snprintf(name, sizeof(name), "F%03d.TXT", i);
        assert(fs->create(name, &f) == 0);
    }

    Fat32::FileSys::DIR dir;
    Fat32::FileSys::DIR::entry_t ent;

    assert(fs->opendir("", &dir) == 0);

    int count = 0;
    while (dir.readdir(&ent) > 0)
        count++;

    dir.closedir();

    assert(count >= 200);
}


static void test_readdir_eof(Fat32::FileSys* fs)
{
    Fat32::FileSys::DIR dir;
    Fat32::FileSys::DIR::entry_t ent;

    assert(fs->opendir("", &dir) == 0);

    while (dir.readdir(&ent) > 0)
        ;

    /* Multiple calls after EOF must return 0 */
    assert(dir.readdir(&ent) == 0);
    assert(dir.readdir(&ent) == 0);

    dir.closedir();
}


void test_readdir(Fat32::FileSys* fs)
{
    printf("TEST: readdir\n");
    test_empty_directory(fs);
    test_single_file(fs);
    test_multiple_files(fs);
    test_subdirectory(fs);
    test_deleted_entries(fs);
    test_large_directory(fs);
    test_readdir_eof(fs);

    printf("   readdir tests passed\n");
}


#define PATTERN_TEST_FILE "test_1m1.bin"
#define PATTERN_TEST_FILE2 "test_1m2.bin"
#define PATTERN_TOTAL_SIZE (1024 * 1024)      /* 1 MiB */
#define PATTERN_BLOCK_SIZE 512
#define PATTERN_IO_SIZE (10 * 1024)           /* 10 KiB */

static void fill_block(uint8_t *buf, uint32_t block_number)
{
    for (uint32_t i = 0; i < PATTERN_BLOCK_SIZE; i++)
        buf[i] = (uint8_t)((block_number ^ i) & 0xFF);
}

static int generate_pattern(uint8_t *buf,
                            size_t offset,
                            size_t len)
{
    size_t remaining = len;
    size_t pos = offset;

    while (remaining > 0) {

        uint32_t block = pos / PATTERN_BLOCK_SIZE;
        uint32_t block_offset = pos % PATTERN_BLOCK_SIZE;

        uint8_t temp[PATTERN_BLOCK_SIZE];
        fill_block(temp, block);

        size_t copy = PATTERN_BLOCK_SIZE - block_offset;
        if (copy > remaining)
            copy = remaining;

        memcpy(buf + (len - remaining),
               temp + block_offset,
               copy);

        remaining -= copy;
        pos += copy;
    }

    return 0;
}


void test_large_write_read(Fat32::FileSys* fs)
{
    printf("TEST: large write-read (without cache bypass)\n");

    Fat32::FileSys::File f;
    assert(fs->create(PATTERN_TEST_FILE, &f) == 0);

    uint8_t buffer[PATTERN_IO_SIZE];

    /* Write phase */
    for (size_t offset = 0; offset < PATTERN_TOTAL_SIZE; offset += PATTERN_IO_SIZE) {
        size_t chunk = PATTERN_IO_SIZE;
        if (offset + chunk > PATTERN_TOTAL_SIZE)
            chunk = PATTERN_TOTAL_SIZE - offset;

        generate_pattern(buffer, offset, chunk);

        assert(f.write(buffer, chunk) == chunk);
    }

    assert(f.close() == 0);

    assert(fs->open(PATTERN_TEST_FILE, &f) == 0);

    uint8_t verify[PATTERN_IO_SIZE];

    for (size_t offset = 0; offset < PATTERN_TOTAL_SIZE; offset += PATTERN_IO_SIZE) {
        size_t chunk = PATTERN_IO_SIZE;
        if (offset + chunk > PATTERN_TOTAL_SIZE)
            chunk = PATTERN_TOTAL_SIZE - offset;

        assert(f.read(buffer, chunk) == chunk);

        generate_pattern(verify, offset, chunk);

        assert(::memcmp(buffer, verify, chunk) == 0);
    }

    assert(f.close() == 0);

    printf("    1 MiB write/read verification passed\n");
}


void test_large_write_read_bypass(Fat32::FileSys* fs)
{
    printf("TEST: large write-read (cache bypass for data)\n");

    Fat32::FileSys::File f;
    assert(fs->create(PATTERN_TEST_FILE2, &f) == 0);

    uint8_t buffer[PATTERN_IO_SIZE];

    /* Write phase */
    for (size_t offset = 0; offset < PATTERN_TOTAL_SIZE; offset += PATTERN_IO_SIZE) {
        size_t chunk = PATTERN_IO_SIZE;
        if (offset + chunk > PATTERN_TOTAL_SIZE)
            chunk = PATTERN_TOTAL_SIZE - offset;

        generate_pattern(buffer, offset, chunk);

        assert(f.write(buffer, chunk, true) == chunk);
    }

    assert(f.close() == 0);

    assert(fs->open(PATTERN_TEST_FILE2, &f) == 0);

    uint8_t verify[PATTERN_IO_SIZE];

    for (size_t offset = 0; offset < PATTERN_TOTAL_SIZE; offset += PATTERN_IO_SIZE) {
        size_t chunk = PATTERN_IO_SIZE;
        if (offset + chunk > PATTERN_TOTAL_SIZE)
            chunk = PATTERN_TOTAL_SIZE - offset;

        assert(f.read(buffer, chunk, true) == chunk);

        generate_pattern(verify, offset, chunk);

        assert(::memcmp(buffer, verify, chunk) == 0);
    }

    assert(f.close() == 0);

    printf("    1 MiB write/read verification passed\n");
}


/* ================= MAIN ================= */

int main(void)
{
    // Prep 64MB test image
    assert(system("./dir2fat32-esp " TEST_IMAGE " 64") == 0);

    PosixBlockDev bdev;

    bdev.fd = ::open(TEST_IMAGE, O_RDWR);
    if (bdev.fd < 0)
        die("open test image");

    GPTMap::Table gpt(bdev);
    
    assert(gpt.load() == 0);
    assert(gpt.count() >= 1);

    int fatpart = -1;
    for (int i = 0;  i < gpt.count(); i++) {
        printf("\n--- partition #%d ---\n", i+1);
        printf("Name: \"%s\"\n", gpt.get(i).name);
        printf("Type: %s (as %s)\n",
               GPTMap::Table::typestr(gpt.get(i).type),
               GPTMap::Table::typestr(gpt.get(i).otype)
            );
        printf("Index: %d\n", gpt.get(i).entry_index);
        printf("First LBA: %llu\n", gpt.get(i).first_lba);
        printf("Last LBA: %llu\n", gpt.get(i).last_lba);

        const uint64_t lba_count = gpt.get(i).last_lba - gpt.get(i).first_lba;
        printf("LBA size: %llu (%lluMB)\n",  lba_count, (lba_count * SECTOR_SIZE) >> 20);
        printf("Attributes: 0x%llx\n\n", gpt.get(i).attributes);

        if (gpt.get(i).type == GPTMap::TYPE_FAT32)
            fatpart = i;
    }

    assert(fatpart != -1);

    GPTMap::Mapper mapper(bdev, gpt.get(fatpart));
    CacheBlockDev bcache(mapper, false);
    Fat32::FileSys fs(bcache);

    test_mount(&fs);
    test_open_nonexistent(&fs);
    test_create_write_read_delete(&fs);
    test_multilevel_path(&fs);
    test_stat(&fs);
    test_dirops(&fs);
    test_rename(&fs);
    test_cross_directory_rename(&fs);
    test_cross_directory_rename_dest_exists(&fs);
    test_basic_write_read(&fs);
    test_overwrite_middle(&fs);
    test_seek_beyond_eof_write(&fs);
    test_truncate_shrink(&fs);
    test_truncate_grow(&fs);
    test_truncate_zero(&fs);
    test_readdir(&fs);
    test_large_write_read_bypass(&fs);
    test_large_write_read(&fs);
    test_psinfo_write(&fs);

    printf("All tests completed.\n");
    printf("\nSector I/O: %d loads, %d stores\n", bdev.numloads, bdev.numstores);
    printf("\nCache: %d reads (%d hits, %d%%), %d writes (%d hits, %d%%)\n",
           bcache._nreads, bcache._nread_hits, (bcache._nread_hits*100)/bcache._nreads,
           bcache._nwrites, bcache._nwrite_hits, (bcache._nwrite_hits*100)/bcache._nwrites);

    printf("\n--- fsck ---\n");
    Fat32::FileSys::fsck_report_t report;
    assert(fs.fsck(false, &report) == 0);

    printf("Files: %u\n", report.files);
    printf("Directories: %u\n", report.directories);
    printf("Free clusters: %u\n", report.free_clusters);
    printf("Referenced clusters: %u\n", report.referenced_clusters);
    printf("Cross-links: %u\n", report.cross_links);
    printf("Invalid refs: %u\n", report.invalid_references);
    printf("Lost clusters: %u\n", report.lost_clusters);
    printf("File size mismatches: %u\n", report.size_mismatches);
    printf("Invalid directory entries: %u\n", report.invalid_entries);
    printf("Repairs: %u\n", report.repairs);

    fs.sync();

    close(bdev.fd);

    printf("\nRunning fsck.vfat, corrections disabled...\n\n");
    system("sudo kpartx -v -a " TEST_IMAGE
           " && (sudo fsck.vfat -nv -F 0 /dev/mapper/loop0p1"
               " ; sudo kpartx -v -d " TEST_IMAGE ")");

    printf("\nDone!\n");

    return 0;
}
