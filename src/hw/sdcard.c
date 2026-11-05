// SD card over SPI0 on the PicoCalc, implementing the FatFs disk I/O layer.
#include "ff.h"
#include "diskio.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#define SD_SPI spi0
#define SD_MISO 16
#define SD_CS 17
#define SD_SCK 18
#define SD_MOSI 19
#define SD_BAUD_INIT 400000
#define SD_BAUD 20000000

#define CMD0 0
#define CMD1 1
#define ACMD41 (0x80 | 41)
#define CMD8 8
#define CMD9 9
#define CMD12 12
#define CMD16 16
#define CMD17 17
#define CMD18 18
#define CMD24 24
#define CMD25 25
#define CMD55 55
#define CMD58 58

#define CT_MMC 0x01
#define CT_SD1 0x02
#define CT_SD2 0x04
#define CT_BLOCK 0x08

static volatile DSTATUS stat = STA_NOINIT;
static uint8_t card_type;

static inline void cs_high(void) { gpio_put(SD_CS, 1); }
static inline void cs_low(void) { gpio_put(SD_CS, 0); }

static uint8_t xchg(uint8_t d) {
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &d, &r, 1);
    return r;
}

static void rcvr_multi(uint8_t *buf, UINT n) { spi_read_blocking(SD_SPI, 0xFF, buf, n); }
static void xmit_multi(const uint8_t *buf, UINT n) { spi_write_blocking(SD_SPI, buf, n); }

static bool wait_ready(uint32_t timeout_ms) {
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    do {
        if (xchg(0xFF) == 0xFF) return true;
    } while (absolute_time_diff_us(get_absolute_time(), end) > 0);
    return false;
}

static void deselect(void) {
    cs_high();
    xchg(0xFF);
}

static bool select_card(void) {
    cs_low();
    xchg(0xFF);
    if (wait_ready(500)) return true;
    deselect();
    return false;
}

static bool rcvr_datablock(uint8_t *buf, UINT btr) {
    uint8_t token;
    absolute_time_t end = make_timeout_time_ms(200);
    do {
        token = xchg(0xFF);
    } while (token == 0xFF && absolute_time_diff_us(get_absolute_time(), end) > 0);
    if (token != 0xFE) return false;
    rcvr_multi(buf, btr);
    xchg(0xFF);
    xchg(0xFF); // discard CRC
    return true;
}

static bool xmit_datablock(const uint8_t *buf, uint8_t token) {
    if (!wait_ready(500)) return false;
    xchg(token);
    if (token != 0xFD) {
        xmit_multi(buf, 512);
        xchg(0xFF);
        xchg(0xFF); // dummy CRC
        uint8_t resp = xchg(0xFF);
        if ((resp & 0x1F) != 0x05) return false;
    }
    return true;
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t res;
    if (cmd & 0x80) {
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }
    if (cmd != CMD12) {
        deselect();
        if (!select_card()) return 0xFF;
    }
    uint8_t pkt[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0x01};
    if (cmd == CMD0) pkt[5] = 0x95;
    if (cmd == CMD8) pkt[5] = 0x87;
    xmit_multi(pkt, 6);
    if (cmd == CMD12) xchg(0xFF);
    int n = 10;
    do {
        res = xchg(0xFF);
    } while ((res & 0x80) && --n);
    return res;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv) return STA_NOINIT;
    return stat;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv) return STA_NOINIT;

    spi_init(SD_SPI, SD_BAUD_INIT);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO);
    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    cs_high();

    for (int i = 0; i < 10; i++) xchg(0xFF);

    uint8_t ty = 0, ocr[4];
    if (send_cmd(CMD0, 0) == 1) {
        absolute_time_t end = make_timeout_time_ms(1000);
        if (send_cmd(CMD8, 0x1AA) == 1) {
            rcvr_multi(ocr, 4);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                while (absolute_time_diff_us(get_absolute_time(), end) > 0 && send_cmd(ACMD41, 1UL << 30)) {
                }
                if (absolute_time_diff_us(get_absolute_time(), end) > 0 && send_cmd(CMD58, 0) == 0) {
                    rcvr_multi(ocr, 4);
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
                }
            }
        } else {
            uint8_t cmd;
            if (send_cmd(ACMD41, 0) <= 1) {
                ty = CT_SD1;
                cmd = ACMD41;
            } else {
                ty = CT_MMC;
                cmd = CMD1;
            }
            while (absolute_time_diff_us(get_absolute_time(), end) > 0 && send_cmd(cmd, 0)) {
            }
            if (absolute_time_diff_us(get_absolute_time(), end) <= 0 || send_cmd(CMD16, 512) != 0) ty = 0;
        }
    }
    card_type = ty;
    deselect();

    if (ty) {
        spi_set_baudrate(SD_SPI, SD_BAUD);
        stat &= ~STA_NOINIT;
    } else {
        stat = STA_NOINIT;
    }
    return stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !count) return RES_PARERR;
    if (stat & STA_NOINIT) return RES_NOTRDY;
    if (!(card_type & CT_BLOCK)) sector *= 512;

    if (count == 1) {
        if (send_cmd(CMD17, sector) == 0 && rcvr_datablock(buff, 512)) count = 0;
    } else {
        if (send_cmd(CMD18, sector) == 0) {
            do {
                if (!rcvr_datablock(buff, 512)) break;
                buff += 512;
            } while (--count);
            send_cmd(CMD12, 0);
        }
    }
    deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !count) return RES_PARERR;
    if (stat & STA_NOINIT) return RES_NOTRDY;
    if (stat & STA_PROTECT) return RES_WRPRT;
    if (!(card_type & CT_BLOCK)) sector *= 512;

    if (count == 1) {
        if (send_cmd(CMD24, sector) == 0 && xmit_datablock(buff, 0xFE)) count = 0;
    } else {
        if (card_type & (CT_SD1 | CT_SD2)) send_cmd((0x80 | 23), count);
        if (send_cmd(CMD25, sector) == 0) {
            do {
                if (!xmit_datablock(buff, 0xFC)) break;
                buff += 512;
            } while (--count);
            if (!xmit_datablock(0, 0xFD)) count = 1;
        }
    }
    deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv) return RES_PARERR;
    if (stat & STA_NOINIT) return RES_NOTRDY;

    DRESULT res = RES_ERROR;
    uint8_t csd[16];
    switch (cmd) {
    case CTRL_SYNC:
        if (select_card()) res = RES_OK;
        break;
    case GET_SECTOR_COUNT:
        if (send_cmd(CMD9, 0) == 0 && rcvr_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) { // SDC v2
                uint32_t csize = csd[9] + ((uint32_t)csd[8] << 8) + ((uint32_t)(csd[7] & 63) << 16) + 1;
                *(LBA_t *)buff = (LBA_t)csize << 10;
            } else {
                uint8_t n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                uint32_t csize = (csd[8] >> 6) + ((uint32_t)csd[7] << 2) + ((uint32_t)(csd[6] & 3) << 10) + 1;
                *(LBA_t *)buff = (LBA_t)csize << (n - 9);
            }
            res = RES_OK;
        }
        break;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 128;
        res = RES_OK;
        break;
    default:
        res = RES_PARERR;
    }
    deselect();
    return res;
}
