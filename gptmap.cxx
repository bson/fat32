#include <stdlib.h>
#include <string.h>
#include "gptmap.h"


using namespace GPTMap;

const uint8_t fat32_guid[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

// EFI System partition type GUID
const uint8_t efi_system_guid[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x0, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

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


static void utf16le_to_utf8(char *out,
                            const uint16_t *in,
                            size_t max_chars)
{
    size_t o = 0;

    for (size_t i = 0; i < max_chars; i++) {
        const uint16_t c = in[i];
        if (c == 0)
            break;

        if (c < 0x80) {
            out[o++] = c;
        } else if (c < 0x800) {
            out[o++] = 0xc0 | (c >> 6);
            out[o++] = 0x80 | (c & 0x3f);
        } else {
            out[o++] = 0xe0 | (c >> 12);
            out[o++] = 0x80 | ((c >> 6) & 0x3f);
            out[o++] = 0x80 | (c & 0x3f);
        }
    }

    out[o] = 0;
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
    const uint64_t lba = hdr.partition_entry_lba;
    int entry_count = min(hdr.num_partition_entries, GPT_MAX_PARTITIONS);
    int sector = 0;
    _table.count = 0;
    int i = 0;

    while (entry_count-- > 0) {
        if (_bdev.read_blocks(lba + sector++, 1, _sector))
            return -5;

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

                utf16le_to_ascii(p->name, sizeof p->name, raw->name, GPT_NAME_LEN, '=');
            }
            ++raw;
            ++i;
        }
    }
    return 0;
}
