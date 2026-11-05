#ifndef HW_KEYBOARD_H
#define HW_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// PicoCalc keyboard key codes (from the STM32 keyboard firmware).
#define KEY_BACKSPACE 0x08
#define KEY_TAB 0x09
#define KEY_ENTER 0x0A
#define KEY_MOD_ALT 0xA1
#define KEY_MOD_SHL 0xA2
#define KEY_MOD_SHR 0xA3
#define KEY_MOD_SYM 0xA4
#define KEY_MOD_CTRL 0xA5
#define KEY_ESC 0xB1
#define KEY_LEFT 0xB4
#define KEY_UP 0xB5
#define KEY_DOWN 0xB6
#define KEY_RIGHT 0xB7
#define KEY_CAPS_LOCK 0xC1
#define KEY_BREAK 0xD0
#define KEY_INSERT 0xD1
#define KEY_HOME 0xD2
#define KEY_DEL 0xD4
#define KEY_END 0xD5
#define KEY_PAGE_UP 0xD6
#define KEY_PAGE_DOWN 0xD7
#define KEY_F1 0x81
#define KEY_F2 0x82
#define KEY_F3 0x83
#define KEY_F4 0x84
#define KEY_F5 0x85
#define KEY_F6 0x86
#define KEY_F7 0x87
#define KEY_F8 0x88
#define KEY_F9 0x89
#define KEY_F10 0x90
#define KEY_POWER 0x91

// Modifier flags reported with each key event.
#define MOD_CTRL 0x01
#define MOD_ALT 0x02
#define MOD_SHIFT 0x04

typedef struct {
    uint8_t key;
    uint8_t mods;
} key_event_t;

void kbd_init(void);
// Non-blocking. Call frequently from the main loop.
void kbd_poll(void);
// Returns true and fills *ev if a key event is available.
bool kbd_get(key_event_t *ev);

// Battery percentage (0-100) as last reported by the keyboard controller,
// or -1 if unknown. *charging is set if the charger is active.
int kbd_battery_percent(bool *charging);
// True once after the keyboard controller reappears following an outage
// (PicoCalc switched off and on while the Pico stayed powered over USB).
bool kbd_take_power_cycle(void);
// Debug helpers (USB serial commands).
void kbd_inject(uint8_t key);
bool kbd_has_kbd_backlight(void);
void kbd_debug_scan(void);
void kbd_debug_raw_write(uint8_t reg, uint8_t val);
void kbd_debug_stats(uint32_t *ok, uint32_t *fail, uint32_t *events);
void kbd_set_lcd_backlight(uint8_t level);
void kbd_set_kbd_backlight(uint8_t level);

#endif
