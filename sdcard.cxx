#include <stdint.h>
#include "sdcard.h"


int SDCard::read_csd()
{
    uint32_t resp[4];

    if (_sdio.send_cmd(9, _rca, SD_RESP_R2, resp))
        return -1;

    /* resp[] holds 128-bit CSD */

    uint8_t csd[16];

    for (int i = 0; i < 4; i++) {
        csd[i*4+0] = resp[3-i] >> 24;
        csd[i*4+1] = resp[3-i] >> 16;
        csd[i*4+2] = resp[3-i] >> 8;
        csd[i*4+3] = resp[3-i];
    }

    const uint8_t csd_structure = (csd[0] >> 6) & 0x3;

    if (csd_structure == 1) {
        /* SDHC/SDXC */
        const uint32_t c_size =
            ((uint32_t)(csd[7] & 0x3F) << 16) |
            ((uint32_t)csd[8] << 8) |
            csd[9];

        _size = (c_size + 1) * 1024;
    } else {
        /* SDSC */
        const uint32_t c_size =
            ((uint32_t)(csd[6] & 0x3) << 10) |
            ((uint32_t)csd[7] << 2) |
            ((csd[8] >> 6) & 0x3);

        const uint8_t c_size_mult =
            ((csd[9] & 0x3) << 1) |
            ((csd[10] >> 7) & 0x1);

        const uint8_t read_bl_len = csd[5] & 0xF;
        const uint32_t block_len = 1 << read_bl_len;
        const uint32_t mult = 1 << (c_size_mult + 2);
        const uint32_t blocknr = (c_size + 1) * mult;

        const uint32_t capacity_bytes = blocknr * block_len;
        _size = capacity_bytes / BLOCK_SIZE;
    }

    return 0;
}


int SDCard::init(void)
{
    uint32_t resp[4];

    _sdio.set_bus_width_1bit();
    _sdio.set_clock(400000); /* 400 kHz init */

    /* CMD0: GO_IDLE */
    if (_sdio.send_cmd(0, 0, SD_RESP_NONE, nullptr))
        return -1;

    /* CMD8: SEND_IF_COND */
    if (_sdio.send_cmd(8, 0x1aa, SD_RESP_R7, resp))
        return -1;

    if ((resp[0] & 0xfff) != 0x1aa)
        return -1; /* voltage mismatch */

    /* ACMD41 loop */
    while (1) {

        /* CMD55 */
        if (_sdio.send_cmd(55, 0, SD_RESP_R1, resp))
            return -1;

        /* ACMD41 */
        if (_sdio.send_cmd(41,
                           0x40300000, /* HCS + 3.2–3.4V */
                           SD_RESP_R3,
                           resp))
            return -1;

        if (resp[0] & (1UL << 31))
            break; /* card ready */
    }

    if (resp[0] & (1UL << 30))
        _high_capacity = true;

    /* CMD2: ALL_SEND_CID */
    if (_sdio.send_cmd(2, 0, SD_RESP_R2, resp))
        return -1;

    /* CMD3: SEND_REL_ADDR */
    if (_sdio.send_cmd(3, 0, SD_RESP_R6, resp))
        return -1;

    _rca = resp[0] & 0xffff0000;

    /* CMD7: SELECT_CARD */
    if (_sdio.send_cmd(7, _rca, SD_RESP_R1, resp))
        return -1;

    /* Set block length (not required for SDHC but harmless) */
    _sdio.send_cmd(16, BLOCK_SIZE, SD_RESP_R1, resp);

    /* Switch to 4-bit mode */

    /* CMD55 */
    if (_sdio.send_cmd(55, _rca, SD_RESP_R1, resp))
        return -1;

    /* ACMD6: set 4-bit */
    if (_sdio.send_cmd(6, 2, SD_RESP_R1, resp))
        return -1;

    _sdio.set_bus_width_4bit();
    _sdio.set_clock(25000000); /* 25 MHz normal */

    read_csd();

    return 0;
}


int SDCard::read_blocks(uint32_t lba, uint32_t count, void *buffer, bool)
{
    if (count == 0)
        return 0;

    const uint32_t addr = _high_capacity ? lba : lba * BLOCK_SIZE;

    uint32_t resp[4];

    if (count == 1) {
        if (_sdio.send_cmd(17, addr, SD_RESP_R1, resp))
            return -1;

        if (_sdio.data_read(buffer, BLOCK_SIZE))
            return -1;

        return _sdio.wait_data_done();
    }

    /* Multiple block read */

    if (_sdio.send_cmd(18, addr, SD_RESP_R1, resp))
        return -1;

    if (_sdio.data_read(buffer, count * BLOCK_SIZE))
        return -1;

    if (_sdio.wait_data_done())
        return -1;

    /* CMD12: STOP_TRANSMISSION */
    if (_sdio.send_cmd(12, 0, SD_RESP_R1, resp))
        return -1;

    return 0;
}


int SDCard::write_blocks(uint32_t lba, uint32_t count, const void *buffer, bool)
{
    if (count == 0)
        return 0;

    const uint32_t addr = _high_capacity ? lba : lba * BLOCK_SIZE;

    uint32_t resp[4];

    if (count == 1) {
        if (_sdio.send_cmd(24, addr, SD_RESP_R1, resp))
            return -1;

        if (_sdio.data_write(buffer, BLOCK_SIZE))
            return -1;

        return _sdio.wait_data_done();
    }

    /* Multi-block write */

    if (_sdio.send_cmd(25, addr, SD_RESP_R1, resp))
        return -1;

    if (_sdio.data_write(buffer, count * BLOCK_SIZE))
        return -1;

    if (_sdio.wait_data_done())
        return -1;

    /* STOP */
    if (_sdio.send_cmd(12, 0, SD_RESP_R1, resp))
        return -1;

    return wait_ready();
}


int SDCard::wait_ready()
{
    uint32_t resp[4];

    for (int i = 0; i < 100000; i++) {

        if (_sdio.send_cmd(13, _rca, SD_RESP_R1, resp))
            return -1;

        if (!(resp[0] & (1 << 8))) /* READY_FOR_DATA */
            continue;

        if (((resp[0] >> 9) & 0xF) == 4) /* TRAN state */
            return 0;
    }

    return -1;
}
