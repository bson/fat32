#include <stdint.h>
#include "stm32_sdio.h"

// This is just an outline


// Note: device needs to be enabled before use

static uint32_t sdmmc_get_ker_clk(oid)
{
    /* Example: STM32F7 using PLL48CLK */
    return 48000000;  /* Adjust to your clock tree */
}


void Stm32SDIO::set_clock(uint32_t hz)
{
    const uint32_t ker_clk = sdmmc_get_ker_clk();
    uint32_t clkdiv = (ker_clk / hz);

    if (clkdiv > 0)
        clkdiv -= 2;

    if (clkdiv > 0x3ff)
        clkdiv = 0x3ff;

    SDMMC->CLKCR &= ~SDMMC_CLKCR_CLKEN;
    SDMMC->CLKCR &= ~SDMMC_CLKCR_CLKDIV;
    SDMMC->CLKCR |= clkdiv;
    SDMMC->CLKCR |= SDMMC_CLKCR_CLKEN;
}


void Stm32SDIO::set_bus_width_1bit()
{
    SDMMC->CLKCR &= ~SDMMC_CLKCR_WIDBUS;
}


void Stm32SDIO::set_bus_width_4bit()
{
    SDMMC->CLKCR &= ~SDMMC_CLKCR_WIDBUS;
    SDMMC->CLKCR |= (1 << 11);  /* WIDBUS = 01 = 4-bit */
}


int Stm32SDIO::send_cmd(uint8_t cmd,
                        uint32_t arg,
                        uint32_t resp_type,
                        uint32_t *response)
{
    /* Clear flags */
    SDMMC->ICR = 0xffffffff;

    SDMMC->ARG = arg;

    uint32_t cmdreg = 0;
    cmdreg |= cmd & 0x3f;
    cmdreg |= SDMMC_CMD_CPSMEN;

    switch (resp_type) {

        case SD_RESP_NONE:
            break;

        case SD_RESP_R2:
            cmdreg |= SDMMC_CMD_WAITRESP_1;
            break;

        default:
            cmdreg |= SDMMC_CMD_WAITRESP_0;
            break;
    }

    SDMMC->CMD = cmdreg;

    /* Wait for completion */
    while (!(SDMMC->STA &
             (SDMMC_STA_CMDSENT |
              SDMMC_STA_CMDREND |
              SDMMC_STA_CTIMEOUT |
              SDMMC_STA_CCRCFAIL)))
        ;

    if (SDMMC->STA & SDMMC_STA_CTIMEOUT)
        return -1;

    if (SDMMC->STA & SDMMC_STA_CCRCFAIL &&
        resp_type != SD_RESP_R3)
        return -1;

    if (response) {
        if (resp_type == SD_RESP_R2) {
            response[0] = SDMMC->RESP1;
            response[1] = SDMMC->RESP2;
            response[2] = SDMMC->RESP3;
            response[3] = SDMMC->RESP4;
        } else {
            response[0] = SDMMC->RESP1;
        }
    }

    return 0;
}


int Stm32SDIO::data_read(void *buf, uint32_t bytes)
{
    uint32_t *dst = buf;

    SDMMC->DTIMER = 0xFFFFFFFF;
    SDMMC->DLEN   = bytes;

    SDMMC->DCTRL =
        SDMMC_DCTRL_DTEN |
        SDMMC_DCTRL_DTDIR |
        SDMMC_DCTRL_DBLOCKSIZE_3; /* 512 bytes */

    uint32_t words = bytes / 4;

    while (words > 0) {

        if (SDMMC->STA & SDMMC_STA_RXFIFOHF) {

            for (int i = 0; i < 8 && words > 0; i++) {
                *dst++ = SDMMC->FIFO;
                words--;
            }
        }

        if (SDMMC->STA &
            (SDMMC_STA_DTIMEOUT |
             SDMMC_STA_DCRCFAIL))
            return -1;
    }

    while (!(SDMMC->STA & SDMMC_STA_DATAEND));

    return 0;
}


int Stm32SDIO::data_write(const void *buf, uint32_t bytes)
{
    const uint32_t *src = buf;

    SDMMC->DTIMER = 0xffffffff;
    SDMMC->DLEN   = bytes;

    SDMMC->DCTRL =
        SDMMC_DCTRL_DTEN |
        SDMMC_DCTRL_DBLOCKSIZE_3; /* 512 bytes */

    uint32_t words = bytes / 4;

    while (words > 0) {
        if (SDMMC->STA & SDMMC_STA_TXFIFOHE) {
            for (int i = 0; i < 8 && words > 0; i++) {
                SDMMC->FIFO = *src++;
                words--;
            }
        }

        if (SDMMC->STA &
            (SDMMC_STA_DTIMEOUT |
             SDMMC_STA_DCRCFAIL))
            return -1;
    }

    while (!(SDMMC->STA & SDMMC_STA_DATAEND))
        ;

    return 0;
}


int Stm32SDIO::wait_data_done()
{
    while (!(SDMMC->STA &
             (SDMMC_STA_DATAEND |
              SDMMC_STA_DTIMEOUT |
              SDMMC_STA_DCRCFAIL)))
        ;

    if (SDMMC->STA &
        (SDMMC_STA_DTIMEOUT |
         SDMMC_STA_DCRCFAIL))
        return -1;

    SDMMC->ICR = 0xffffffff;
    return 0;
}


int Stm32SDIO::get_data_crc_error()
{
    return (SDMMC->STA & SDMMC_STA_DCRCFAIL) != 0;
}


int Stm32SDIO::get_data_timeout()
{
    return (SDMMC->STA & SDMMC_STA_DTIMEOUT) != 0;
}


void Stm32SDIO::reset_datapath()
{
    SDMMC->DCTRL = 0;
    SDMMC->ICR = 0xffffffff;
}
