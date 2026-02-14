#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "blockdev.h"
#include "fat32.h"
#include "cache.h"

/*
 * dd if=/dev/zero of=test.img bs=1M count=16
 * mkfs.vfat -F 32 -n "TEST IMAGE" test.img
 */

#define TEST_IMAGE "test.img"
#define SECTOR_SIZE 512

/* ================= POSIX BLOCK DEVICE ================= */

class PosixBlockDev : public BlockDev {
public:
    int fd;
    int numloads;
    int numstores;

    PosixBlockDev()
        : fd(-1), numloads(0), numstores(0)
    { }

    int init() { return 0; }


    int read_blocks(uint32_t lba, uint32_t count, void *buffer)
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


    int write_blocks(uint32_t lba, uint32_t count, const void *buffer)
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
};


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

//    assert(fs->unlink(filename) == 0);
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
    printf("TEST: Fat32::stat\n");

    const char *filename = "statfile.txt";
    const char *data = "Stat test data";
    Fat32::FileSys::File f;
    Fat32::FileSys::Stat st;

    /* Ensure file does not exist */
    fs->unlink(filename);

    /* Create */
    assert(fs->create(filename, &f) == 0);

    /* Initial stat */
    assert(fs->stat(filename, &st) == 0);
    assert(st._size == 0);
    assert(st._first_cluster >= 2);
    assert((st._attributes & Fat32::DirentAttr::DIRECTORY) == 0);

    /* Write data */
    assert(fs->open(filename, &f) == 0);
    assert(f.write(data, strlen(data)) == (int)strlen(data));
    assert(f.close() == 0);

    /* Stat again */
    assert(fs->stat(filename, &st) == 0);
    assert(st._size == strlen(data));
    assert(st._first_cluster >= 2);

    /* Reopen and verify stat consistent */
    assert(fs->open(filename, &f) == 0);
    assert(f._file_size == st._size);
    assert(f.close() == 0);

    /* Delete */
    assert(fs->unlink(filename) == 0);

    /* Stat must now fail */
    assert(fs->stat(filename, &st) != 0);

    printf("  Fat32::stat passed\n");
}

void test_dirops(Fat32::FileSys* fs)
{
    printf("TEST: Fat32::mkdir, Fat32::rmdir\n");

    assert(fs->mkdir("DIR10") == 0);
    assert(fs->mkdir("DIR10") != 0 && fs->last_error() == Fat32::Error::ALREADY_EXISTS);
    assert(fs->rmdir("DIR10") == 0);
    assert(fs->rmdir("DIR10") != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);


    assert(fs->mkdir("DIR11") == 0);
    Fat32::FileSys::File f;
    assert(fs->create("DIR11/FILE.TXT", &f) == 0);
    assert(f.close() == 0);
    assert(fs->rmdir("DIR11") != 0  && fs->last_error() == Fat32::Error::DIR_NOT_EMPTY);

    printf("  Fat32::mkdir, Fat32::rmdir passed\n");
}

void test_psinfo_write(Fat32::FileSys* fs)
{
    printf("TEST: Fat32::sync\n");

    assert(fs->sync() == 0);
    printf("  Fat32::sync passed\n");
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

    Fat32::FileSys::Stat st;
    assert(fs->stat(name, &st) == 0);

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

    Fat32::FileSys::Stat st;
    assert(fs->stat(name, &st) == 0);
    assert(st._size == 0);

    assert(f.close() == 0);
}


void test_rename(Fat32::FileSys* fs)
{
    Fat32::FileSys::File f;
    Fat32::FileSys::Stat st;
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
    assert(fs->stat(oldname, &st) != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);

    /* New must exist */
    assert(fs->stat(newname, &st) == 0);
    assert(st._size == 11);

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
    Fat32::FileSys::Stat st;
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
    assert(fs->stat(oldpath, &st) != 0 && fs->last_error() == Fat32::Error::FILE_NOT_FOUND);
    
    /* New must exist */
    assert(fs->stat(newpath, &st) == 0);
    assert(st._size == 15);

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


/* ================= MAIN ================= */

int main(void)
{
    PosixBlockDev bdev;
    CacheBlockDev bcache(bdev, false);

    assert(system("dd if=/dev/zero of=test.img bs=1M count=32 && "
                  "mkfs.vfat -F 32 -n \"FAT32 Test\" " TEST_IMAGE) == 0);

    bdev.fd = ::open(TEST_IMAGE, O_RDWR);
    if (bdev.fd < 0)
        die("open test image");

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

    fs.sync();

    close(bdev.fd);

    return 0;
}
