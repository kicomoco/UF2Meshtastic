// Meshtastic client for the ClockworkPi PicoCalc (Raspberry Pi Pico 2 W).
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio_usb.h"
#include "pico/bootrom.h"

#include "hw/lcd.h"
#include "hw/keyboard.h"
#include "hw/sound.h"
#include "storage.h"
#include "log.h"
#include "mesh/mesh.h"
#include "mesh/ble_link.h"
#include "ui/ui.h"

// Debug aid: sending 'P' over USB serial dumps the framebuffer as hex rows.
static void usb_commands(void) {
    int c = getchar_timeout_us(0);
    if (c == 'P') {
        printf("\nFB-BEGIN %d %d\n", LCD_W, LCD_H);
        for (int y = 0; y < LCD_H; y++) {
            for (int x = 0; x < LCD_W; x++) printf("%04x", lcd_fb[y * LCD_W + x]);
            printf("\n");
        }
        printf("FB-END\n");
    } else if (c == 'Q') {
        static uint16_t row[LCD_W];
        printf("\nFB-BEGIN %d %d\n", LCD_W, LCD_H);
        for (int y = 0; y < LCD_H; y++) {
            lcd_readback_row(y, row);
            for (int x = 0; x < LCD_W; x++) printf("%04x", row[x]);
            printf("\n");
        }
        printf("FB-END\n");
    } else if (c == 'T') {
        // Stress partial updates, then compare panel memory with the framebuffer.
        static uint16_t row[LCD_W];
        uint32_t seed = 12345;
        for (int i = 0; i < 300; i++) {
            seed = seed * 1103515245 + 12345;
            int y0 = (seed >> 8) % LCD_H, h = 1 + (seed >> 20) % 40;
            for (int y = y0; y < y0 + h && y < LCD_H; y++)
                for (int x = 0; x < LCD_W; x++) lcd_fb[y * LCD_W + x] = (uint16_t)(seed + y * 7 + x);
            lcd_flush();
        }
        int bad = 0, first = -1;
        for (int y = 0; y < LCD_H; y++) {
            lcd_readback_row(y, row);
            for (int x = 0; x < LCD_W; x++) {
                if ((row[x] & 0xF7DE) != (lcd_fb[y * LCD_W + x] & 0xF7DE)) {
                    if (first < 0) {
                        first = y;
                        for (int k = 0; k < 6; k++) printf("x%d fb=%04x panel=%04x\n", x + k, lcd_fb[y * LCD_W + x + k], row[x + k]);
                    }
                    bad++;
                    break;
                }
            }
        }
        printf("STRESS: %d bad rows (first %d)\n", bad, first);
        ui_redraw();
        lcd_invalidate();
    } else if (c == 'B') {
        // Reboot to BOOTSEL from the main loop, i.e. never in the middle of a
        // keyboard I2C transfer (which can hang the keyboard controller).
        printf("rebooting to BOOTSEL\n");
        sleep_ms(50);
        reset_usb_boot(0, 0);
    } else if (c == 'W') {
        // W<reg><val>: raw keyboard-controller register write (debug)
        int r = getchar_timeout_us(100000), v = getchar_timeout_us(100000);
        if (r >= 0 && v >= 0) kbd_debug_raw_write((uint8_t)r, (uint8_t)v);
    } else if (c == 'I') {
        kbd_debug_scan();
    } else if (c == 'K') {
        uint32_t ok, fail, evs;
        kbd_debug_stats(&ok, &fail, &evs);
        printf("KBD: i2c ok=%lu fail=%lu events=%lu modal=%d\n", (unsigned long)ok, (unsigned long)fail,
               (unsigned long)evs, ui_modal_active());
    } else if (c == '!') {
        // Inject the next character as a key press ('\r' = Enter).
        int k = getchar_timeout_us(100000);
        if (k >= 0) kbd_inject(k == '\r' || k == '\n' ? KEY_ENTER : (uint8_t)k);
    } else if (c == 'R') {
        lcd_invalidate();
        lcd_flush();
        printf("full refresh done\n");
    }
}

static void splash(const char *msg) {
    gfx_clear(C_BG);
    gfx_text_center(LCD_W / 2, 130, "Meshtastic", FONT_BIG, C_ACCENT, TRANSPARENT);
    gfx_text_center(LCD_W / 2, 152, msg, FONT, C_DIM, TRANSPARENT);
    lcd_flush();
}

int main(void) {
    stdio_init_all();
    lcd_init();
    splash("Starting...");
    kbd_init();
    sound_init();

    app_log("Meshtastic for PicoCalc " APP_VERSION);
    if (!storage_init()) app_log("SD card not available - settings will not be saved");
    kbd_set_lcd_backlight(g_settings.lcd_backlight);
    kbd_set_kbd_backlight(g_settings.kbd_backlight);

    mesh_init();
    app_log("Mesh state loaded");

    splash("Starting Bluetooth...");
    if (cyw43_arch_init()) {
        app_log("CYW43 init failed");
        splash("Wireless chip init failed");
    } else {
        app_log("CYW43 ready, starting BLE");
        ble_link_init();
    }

    ui_init();
    lcd_invalidate();
    app_log("UI ready");

    bool usb_was_connected = false;
    absolute_time_t next_alive = make_timeout_time_ms(60000);
    while (true) {
        // Replay the log when a USB terminal attaches so early messages are not lost.
        bool usb_connected = stdio_usb_connected();
        if (usb_connected && !usb_was_connected) {
            printf("\n--- log replay ---\n");
            for (int i = 0; i < app_log_count(); i++) printf("%s\n", app_log_line(i));
            printf("--- live ---\n");
        }
        usb_was_connected = usb_connected;
        if (usb_connected) usb_commands();
        if (time_reached(next_alive)) {
            next_alive = make_timeout_time_ms(60000);
            printf("[alive] link=%d\n", (int)ble_link_state());
        }

        cyw43_arch_poll();
        kbd_poll();
        if (kbd_take_power_cycle()) {
            app_log("PicoCalc power cycled, reinitialising display");
            lcd_reinit();
            kbd_set_lcd_backlight(g_settings.lcd_backlight);
            kbd_set_kbd_backlight(g_settings.kbd_backlight);
            lcd_invalidate();
        }
        key_event_t ev;
        while (kbd_get(&ev)) ui_key(&ev);
        ble_link_poll();
        mesh_poll();
        sound_poll();
        if (ui_render()) lcd_flush();
        else cyw43_arch_wait_for_work_until(make_timeout_time_ms(2));
    }
}
