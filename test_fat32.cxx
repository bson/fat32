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

/*
 * dd if=/dev/zero of=test.img bs=1M count=16
 * mkfs.vfat -F 32 test.img
 */

#define TEST_IMAGE "test.img"
#define SECTOR_SIZE Fat32::SECTOR_SIZE

/* ================= POSIX BLOCK DEVICE ================= */

class PosixBlockDev : public BlockDev {
public:
    int fd;
    int numloads;
    int numstores;

    PosixBlockDev()
        : fd(-1), numloads(0), numstores(0)
    { }


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
};


/* ================= TEST HELPERS ================= */

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void test_mount(BlockDev* bdev, Fat32::FileSys *fs)
{
    printf("TEST: mount\n");
    assert(Fat32::mount(bdev, fs) == 0);
}

static void test_open_nonexistent(Fat32::FileSys *fs)
{
    printf("TEST: open nonexistent file\n");
    Fat32::File f;
    assert(fs->open("nope.txt", &f) != 0);
}

static void test_create_write_read_delete(Fat32::FileSys *fs)
{
    printf("TEST: write existing file\n");

    const char *filename = "testfile.txt";
    const char *data = "Hello FAT32 test suite!";
    char buffer[128];

    Fat32::File f;

    assert(fs->create(filename, &f) == 0);

    assert(f.write(data, strlen(data)) == (int)strlen(data));
    f.close();

    assert(fs->open(filename, &f) == 0);

    memset(buffer, 0, sizeof(buffer));
    assert(f.read(buffer, sizeof(buffer)) >= 0);

    assert(strncmp(buffer, data, strlen(data)) == 0);

    f.close();

//    assert(fs->unlink(filename) == 0);
}

static void test_multilevel_path(Fat32::FileSys *fs)
{
    printf("TEST: multi-level path\n");

    Fat32::File f;

    assert(fs->mkdir("DIR1") == 0);
    assert(fs->create("DIR1/FILE1.TXT", &f) == 0);

    if (fs->open("DIR1/FILE1.TXT", &f) == 0) {
        printf("  Found DIR1/FILE1.TXT\n");
        f.close();
    } else {
        printf("  Skipping (directory not present)\n");
    }
}

static void test_lfn_lookup(Fat32::FileSys *fs)
{
    printf("TEST: long filename lookup\n");

    Fat32::File f;

    if (fs->open("Long File Name.txt", &f) == 0) {
        printf("  LFN opened successfully\n");
        f.close();
    } else {
        printf("  Skipping (LFN not present)\n");
    }
}

static void test_stat(Fat32::FileSys *fs)
{
    printf("TEST: Fat32::stat\n");

    const char *filename = "statfile.txt";
    const char *data = "Stat test data";
    Fat32::File f;
    Fat32::Stat st;

    /* Ensure file does not exist */
    fs->unlink(filename);

    /* Create */
    assert(fs->create(filename, &f) == 0);

    /* Initial stat */
    assert(fs->stat(filename, &st) == 0);
    assert(st._size == 0);
    assert(st._first_cluster >= 2);
    assert((st._attributes & Fat32::DirEntAttr::DIRECTORY) == 0);

    /* Write data */
    assert(fs->open(filename, &f) == 0);
    assert(f.write(data, strlen(data)) == (int)strlen(data));
    f.close();

    /* Stat again */
    assert(fs->stat(filename, &st) == 0);
    assert(st._size == strlen(data));
    assert(st._first_cluster >= 2);

    /* Reopen and verify stat consistent */
    assert(fs->open(filename, &f) == 0);
    assert(f._file_size == st._size);
    f.close();

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
    assert(fs->mkdir("DIR10") != 0);
    assert(fs->rmdir("DIR10") == 0);
    assert(fs->rmdir("DIR10") != 0);


    fs->mkdir("DIR11");
    Fat32::File f;
    fs->create("DIR11/FILE.TXT", &f);
    f.close();
    assert(fs->rmdir("DIR11") != 0);

    printf("  Fat32::mkdir, Fat32::rmdir passed\n");
}

/* ================= MAIN ================= */

int main(void)
{
    PosixBlockDev bdev;

    bdev.fd = open(TEST_IMAGE, O_RDWR);
    if (bdev.fd < 0)
        die("open test image");

    Fat32::FileSys fs;

    test_mount(&bdev, &fs);
    test_open_nonexistent(&fs);
    test_create_write_read_delete(&fs);
    test_multilevel_path(&fs);
    test_lfn_lookup(&fs);
    test_stat(&fs);
    test_dirops(&fs);

    close(bdev.fd);

    printf("All tests completed.\n");
    printf("%d loads, %d stores\n", bdev.numloads, bdev.numstores);
    return 0;
}
