#ifndef HW_LCD_H
#define HW_LCD_H

#include <stdint.h>
#include <stdbool.h>

#define LCD_W 320
#define LCD_H 320

// RGB565 framebuffer. Draw into it, then call lcd_flush().
extern uint16_t lcd_fb[LCD_W * LCD_H];

void lcd_init(void);
// Sends rows that changed since the previous flush to the panel.
void lcd_flush(void);
// Forces the next flush to resend the whole screen.
void lcd_invalidate(void);
// Re-run the panel init sequence (e.g. after the PicoCalc power was cycled
// while the Pico stayed powered over USB) and redraw the whole frame.
void lcd_reinit(void);
// Debug: read one row of panel memory back as RGB565.
void lcd_readback_row(int y, uint16_t *out);

#endif
