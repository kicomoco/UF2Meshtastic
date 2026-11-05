// ILI9488 320x320 panel on the PicoCalc (SPI1). The panel is driven in
// 18-bit mode (3 bytes per pixel) as SPI does not support RGB565 on ILI9488.
#include "hw/lcd.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#define LCD_SPI spi1
#define LCD_SCK 10
#define LCD_MOSI 11
#define LCD_MISO 12
#define LCD_CS 13
#define LCD_DC 14
#define LCD_RST 15
#define LCD_BAUD 25000000

uint16_t lcd_fb[LCD_W * LCD_H];

static uint32_t row_hash[LCD_H];
static bool force_full = true;
static int dma_chan = -1;
static uint8_t line_buf[2][LCD_W * 3];

static void cs_low(void);
static void cs_high(void);

static void cmd_data(uint8_t c, const uint8_t *d, size_t n) {
    cs_low();
    gpio_put(LCD_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_DC, 1);
    if (n) spi_write_blocking(LCD_SPI, d, n);
    cs_high();
}

static void cmd(uint8_t c) { cmd_data(c, NULL, 0); }

#define CMDN(c, ...)                                     \
    do {                                                 \
        static const uint8_t _d[] = {__VA_ARGS__};       \
        cmd_data((c), _d, sizeof(_d));                   \
    } while (0)

static void cs_low(void) {
    gpio_put(LCD_CS, 0);
    busy_wait_at_least_cycles(30);
}

static void cs_high(void) {
    busy_wait_at_least_cycles(30);
    gpio_put(LCD_CS, 1);
    busy_wait_at_least_cycles(30);
}

static void send_cmd_in_txn(uint8_t c) {
    gpio_put(LCD_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_DC, 1);
}

// Starts a memory-write transaction; CS stays low until the caller raises it.
static void begin_window(int x0, int y0, int x1, int y1) {
    uint8_t d[4];
    cs_low();
    send_cmd_in_txn(0x2A);
    d[0] = x0 >> 8; d[1] = x0; d[2] = x1 >> 8; d[3] = x1;
    spi_write_blocking(LCD_SPI, d, 4);
    send_cmd_in_txn(0x2B);
    d[0] = y0 >> 8; d[1] = y0; d[2] = y1 >> 8; d[3] = y1;
    spi_write_blocking(LCD_SPI, d, 4);
    send_cmd_in_txn(0x2C);
}

static void panel_setup(void) {
    gpio_put(LCD_RST, 1);
    sleep_ms(10);
    gpio_put(LCD_RST, 0);
    sleep_ms(10);
    gpio_put(LCD_RST, 1);
    sleep_ms(200);

    CMDN(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F);
    CMDN(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F);
    CMDN(0xC0, 0x17, 0x15);       // Power control 1
    CMDN(0xC1, 0x41);             // Power control 2
    CMDN(0xC5, 0x00, 0x12, 0x80); // VCOM
    CMDN(0x36, 0x48);             // MADCTL: MX | BGR
    CMDN(0x3A, 0x66);             // 18-bit pixels
    CMDN(0xB0, 0x00);             // Interface mode
    CMDN(0xB1, 0xA0);             // Frame rate
    cmd(0x21);                    // Inversion on (PicoCalc panel requires it)
    CMDN(0xB4, 0x02);
    CMDN(0xB6, 0x02, 0x02, 0x3B);
    CMDN(0xB7, 0xC6);
    CMDN(0xE9, 0x00);
    CMDN(0xF7, 0xA9, 0x51, 0x2C, 0x82);
    cmd(0x11); // Sleep out
    sleep_ms(120);
}

void lcd_init(void) {
    spi_init(LCD_SPI, LCD_BAUD);
    gpio_set_function(LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MISO, GPIO_FUNC_SPI);

    gpio_init(LCD_CS);
    gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_put(LCD_CS, 1);
    gpio_init(LCD_DC);
    gpio_set_dir(LCD_DC, GPIO_OUT);
    gpio_init(LCD_RST);
    gpio_set_dir(LCD_RST, GPIO_OUT);

    panel_setup();
    dma_chan = dma_claim_unused_channel(true);

    memset(lcd_fb, 0, sizeof(lcd_fb));
    force_full = true;
    lcd_flush();
    cmd(0x29); // Display on
    sleep_ms(20);
}

void lcd_invalidate(void) { force_full = true; }

void lcd_reinit(void) {
    panel_setup();
    force_full = true;
    lcd_flush();
    cmd(0x29);
    sleep_ms(20);
}

void lcd_readback_row(int y, uint16_t *out) {
    static uint8_t raw[LCD_W * 3 + 1];
    cs_low();
    gpio_put(LCD_DC, 0);
    uint8_t c = 0x2A;
    spi_write_blocking(LCD_SPI, &c, 1);
    uint8_t d[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
    gpio_put(LCD_DC, 1);
    spi_write_blocking(LCD_SPI, d, 4);
    gpio_put(LCD_DC, 0);
    c = 0x2B;
    spi_write_blocking(LCD_SPI, &c, 1);
    d[0] = y >> 8; d[1] = y; d[2] = y >> 8; d[3] = y;
    gpio_put(LCD_DC, 1);
    spi_write_blocking(LCD_SPI, d, 4);
    gpio_put(LCD_DC, 0);
    c = 0x2E;
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_DC, 1);
    spi_set_baudrate(LCD_SPI, 6000000);
    spi_read_blocking(LCD_SPI, 0, raw, 1); // dummy byte
    spi_read_blocking(LCD_SPI, 0, raw, LCD_W * 3);
    gpio_put(LCD_DC, 0);
    cs_high();
    spi_set_baudrate(LCD_SPI, LCD_BAUD);
    for (int x = 0; x < LCD_W; x++) {
        uint8_t r = raw[x * 3], g = raw[x * 3 + 1], b = raw[x * 3 + 2];
        out[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

static uint32_t hash_row(const uint16_t *row) {
    const uint32_t *w = (const uint32_t *)row;
    uint32_t h = 2166136261u;
    for (int i = 0; i < LCD_W / 2; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static void convert_row(const uint16_t *src, uint8_t *dst) {
    for (int x = 0; x < LCD_W; x++) {
        uint16_t p = src[x];
        uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        *dst++ = (r << 3) | (r >> 2);
        *dst++ = (g << 2) | (g >> 4);
        *dst++ = (b << 3) | (b >> 2);
    }
}

static void send_rows(int y0, int y1) {
    begin_window(0, y0, LCD_W - 1, y1);

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);

    int buf = 0;
    for (int y = y0; y <= y1; y++) {
        convert_row(&lcd_fb[y * LCD_W], line_buf[buf]);
        dma_channel_wait_for_finish_blocking(dma_chan);
        dma_channel_configure(dma_chan, &c, &spi_get_hw(LCD_SPI)->dr, line_buf[buf], LCD_W * 3, true);
        buf ^= 1;
    }
    dma_channel_wait_for_finish_blocking(dma_chan);
    while (spi_is_busy(LCD_SPI)) tight_loop_contents();
    // Drain RX FIFO / clear overrun from the TX-only DMA transfer.
    while (spi_is_readable(LCD_SPI)) (void)spi_get_hw(LCD_SPI)->dr;
    spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
    cs_high();
}

void lcd_flush(void) {
    int run_start = -1;
    for (int y = 0; y < LCD_H; y++) {
        uint32_t h = hash_row(&lcd_fb[y * LCD_W]);
        bool changed = force_full || h != row_hash[y];
        row_hash[y] = h;
        if (changed && run_start < 0) run_start = y;
        if (!changed && run_start >= 0) {
            send_rows(run_start, y - 1);
            run_start = -1;
        }
    }
    if (run_start >= 0) send_rows(run_start, LCD_H - 1);
    force_full = false;
}
