#include <stdlib.h>
#include <string.h>
#include "gptmap.h"


using namespace GPTMap;


static const uint8_t GUID_EFI_SYSTEM[16] =
    {0xC1,0x2A,0x73,0x28,0xF8,0x1F,0x11,0xD2,
     0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};

static const uint8_t GUID_MS_BASIC_DATA[16] =
    {0xEB,0xD0,0xA0,0xA2,0xB9,0xE5,0x44,0x33,
     0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};

static const uint8_t GUID_LINUX_FS[16] =
    {0x0F,0xC6,0x3D,0xAF,0x84,0x83,0x47,0x72,
     0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};

static const uint8_t GUID_LINUX_SWAP[16] =
    {0x06,0x57,0xFD,0x6D,0xA4,0xAB,0x43,0xC4,
     0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F};

static const uint8_t GUID_BIOS_BOOT[16] =
    {0x21,0x68,0x61,0x48,0x64,0x49,0x6E,0x6F,
     0x74,0x4E,0x65,0x65,0x64,0x45,0x46,0x49};

const char* Table::part_type_str[NUM_TYPES] = {
    [TYPE_UNKNOWN]            = "Unknown",
    [TYPE_EFI_SYSTEM]         = "EFI System",
    [TYPE_FAT32]              = "FAT32",
    [TYPE_MS_BASIC_DATA]      = "Microsoft Basic Data",
    [TYPE_LINUX_FILESYSTEM]   = "Linux Filesystem",
    [TYPE_LINUX_SWAP]         = "Linux Swap",
    [TYPE_BIOS_BOOT]          = "BIOS Boot",
};

static void gpt_guid_normalize(uint8_t out[16], const uint8_t in[16])
{
    out[0] = in[3];
    out[1] = in[2];
    out[2] = in[1];
    out[3] = in[0];

    out[4] = in[5];
    out[5] = in[4];

    out[6] = in[7];
    out[7] = in[6];

    for (int i = 8; i < 16; i++)
        out[i] = in[i];
}


static GuidType
identify_partition_type(const uint8_t raw_guid[16])
{
    uint8_t guid[16];
    gpt_guid_normalize(guid, raw_guid);

    if (!::memcmp(guid, GUID_EFI_SYSTEM, 16))
        return TYPE_EFI_SYSTEM;

    if (!::memcmp(guid, GUID_MS_BASIC_DATA, 16))
        return TYPE_MS_BASIC_DATA;

    if (!::memcmp(guid, GUID_LINUX_FS, 16))
        return TYPE_LINUX_FILESYSTEM;

    if (!::memcmp(guid, GUID_LINUX_SWAP, 16))
        return TYPE_LINUX_SWAP;

    if (!::memcmp(guid, GUID_BIOS_BOOT, 16))
        return TYPE_BIOS_BOOT;

    return TYPE_UNKNOWN;
}


bool Table::partition_is_fat32(uint32_t first_lba)
{
    if (_bdev.read_blocks(first_lba, 1, _sector) != 0)
        return false;

    return (::memcmp(_sector+82, "FAT32", 5) == 0);
}


static uint32_t crc32(const void *data, size_t len)
{
    uint32_t crc = 0xffffffff;
    const uint8_t *p = (const uint8_t*)data;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }

    return ~crc;
}


static int utf16le_to_ascii(char *out,
                            size_t out_size,
                            const uint16_t *in,
                            size_t in_len,
                            char replacement)
{
    if (!out || out_size == 0)
        return 0;

    ::memset(out, 0, out_size);

    size_t o = 0;

    for (size_t i = 0; i < in_len; i++) {

        if (o >= out_size - 1)
            break;

        const uint16_t wc = in[i];

        if (wc == 0)
            break;  /* stop at UTF-16 null */

        /* Surrogate range: 0xD800–0xDFFF */
        if (wc >= 0xd800 && wc <= 0xdfff) {
            out[o++] = replacement;
            continue;
        }

        if (wc <= 0x7f) {
            out[o++] = (char)wc;
        } else {
            out[o++] = replacement;
        }
    }

    out[o] = 0;
    return o;
}


int Table::load()
{
    if (_bdev.read_blocks(GPT_HEADER_LBA, 1, _sector) != 0)
        return -1;

    gpt_header_t hdr = *(gpt_header_t *)_sector;

    if (hdr.signature != GPT_SIGNATURE)
        return -2;

    uint32_t saved_crc = hdr.header_crc32;
    hdr.header_crc32 = 0;

    if (crc32(&hdr, hdr.header_size) != saved_crc)
        return -3;

    if (hdr.sizeof_partition_entry < sizeof(gpt_entry_raw_t))
        return -4;

    // Scan and collect partitions
    if (hdr.partition_entry_lba >> 32) // 32-bit LBA limit
        return -5;

    const uint32_t lba = hdr.partition_entry_lba;
    int entry_count = min(hdr.num_partition_entries, GPT_MAX_PARTITIONS);
    int sector = 0;
    _table.count = 0;
    int i = 0;

    while (entry_count-- > 0) {
        if (_bdev.read_blocks(lba + sector++, 1, _sector))
            return -6;

        const gpt_entry_raw_t* raw = (const gpt_entry_raw_t*)_sector;

        while ((uint8_t*)raw < _sector + sizeof _sector) {
            if (raw->type_guid[0] || raw->type_guid[1] || raw->type_guid[2] || raw->type_guid[3]) {
                gpt_partition_t *p = &_table.entries[_table.count++];

                ::memcpy(p->type_guid, raw->type_guid, 16);
                ::memcpy(p->guid, raw->guid, 16);

                p->first_lba   = raw->first_lba;
                p->last_lba    = raw->last_lba;
                p->attributes  = raw->attributes;
                p->entry_index = i;
                p->type        = identify_partition_type(p->type_guid);
                p->otype       = p->type;

                utf16le_to_ascii(p->name, sizeof p->name, raw->name, GPT_NAME_LEN, '=');
            }
            ++raw;
            ++i;
        }
    }

    // Autodetect FAT32 by probing
    // Do this outside the loop above as it clobbers _sector.
    for (int i = 0; i < _table.count; i++) {
        switch (_table.entries[i].type) {
        case TYPE_EFI_SYSTEM:
        case TYPE_MS_BASIC_DATA:

            if (partition_is_fat32(_table.entries[i].first_lba))
                _table.entries[i].type = TYPE_FAT32;
            break;

        default:
            ;
        }
    }

    return 0;
}
