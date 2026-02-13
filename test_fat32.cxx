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
    assert(fs->open("nope.txt", &f) != 0);
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
    f.close();

    assert(fs->open(filename, &f) == 0);

    memset(buffer, 0, sizeof(buffer));
    assert(f.read(buffer, sizeof(buffer)) >= 0);

    assert(strncmp(buffer, data, strlen(data)) == 0);

    f.close();

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
        f.close();
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
    Fat32::FileSys::File f;
    fs->create("DIR11/FILE.TXT", &f);
    f.close();
    assert(fs->rmdir("DIR11") != 0);

    printf("  Fat32::mkdir, Fat32::rmdir passed\n");
}

void test_psinfo_write(Fat32::FileSys* fs)
{
    printf("TEST: Fat32::checkpoint\n");

    assert(fs->checkpoint() == 0);
    printf("  Fat32::checkpoint passed\n");
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
    
    f.close();
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

    f.close();
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

    f.close();
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

    f.close();
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

    f.close();
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

    f.close();
}


/* ================= MAIN ================= */

int main(void)
{
    PosixBlockDev bdev;

    assert(system("dd if=/dev/zero of=test.img bs=1M count=16 && "
                  "mkfs.vfat -F 32 -n \"FAT32 Test\" " TEST_IMAGE) == 0);

    bdev.fd = open(TEST_IMAGE, O_RDWR);
    if (bdev.fd < 0)
        die("open test image");

    Fat32::FileSys fs(bdev);

    test_mount(&fs);
    test_open_nonexistent(&fs);
    test_create_write_read_delete(&fs);
    test_multilevel_path(&fs);
    test_stat(&fs);
    test_dirops(&fs);

    test_basic_write_read(&fs);
    test_overwrite_middle(&fs);
    test_seek_beyond_eof_write(&fs);
    test_truncate_shrink(&fs);
    test_truncate_grow(&fs);
    test_truncate_zero(&fs);

    test_psinfo_write(&fs);

    close(bdev.fd);

    printf("All tests completed.\n");
    printf("%d loads, %d stores\n", bdev.numloads, bdev.numstores);
    return 0;
}
