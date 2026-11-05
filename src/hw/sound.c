// Square-wave beeps on the PicoCalc audio PWM pins (GP26 left, GP27 right).
#include "hw/sound.h"

#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#define AUDIO_L 26
#define AUDIO_R 27

static const uint16_t *cur;
static absolute_time_t note_end;
static uint slice_l, slice_r;

static void set_tone(uint32_t freq) {
    if (!freq) {
        pwm_set_enabled(slice_l, false);
        pwm_set_enabled(slice_r, false);
        pwm_set_gpio_level(AUDIO_L, 0);
        pwm_set_gpio_level(AUDIO_R, 0);
        return;
    }
    uint32_t sys = clock_get_hz(clk_sys);
    // Pick a divider so the wrap fits in 16 bits.
    float div = (float)sys / (freq * 65535.0f);
    if (div < 1.0f) div = 1.0f;
    uint32_t wrap = (uint32_t)(sys / (div * freq));
    if (wrap > 65535) wrap = 65535;
    pwm_set_clkdiv(slice_l, div);
    pwm_set_clkdiv(slice_r, div);
    pwm_set_wrap(slice_l, wrap);
    pwm_set_wrap(slice_r, wrap);
    // Low duty cycle keeps the volume modest.
    pwm_set_gpio_level(AUDIO_L, wrap / 8);
    pwm_set_gpio_level(AUDIO_R, wrap / 8);
    pwm_set_enabled(slice_l, true);
    pwm_set_enabled(slice_r, true);
}

void sound_init(void) {
    gpio_set_function(AUDIO_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_R, GPIO_FUNC_PWM);
    slice_l = pwm_gpio_to_slice_num(AUDIO_L);
    slice_r = pwm_gpio_to_slice_num(AUDIO_R);
    set_tone(0);
}

static void start_note(void) {
    if (!cur || cur[1] == 0) {
        cur = NULL;
        set_tone(0);
        return;
    }
    set_tone(cur[0]);
    note_end = make_timeout_time_ms(cur[1]);
}

void sound_play(const uint16_t *notes) {
    cur = notes;
    start_note();
}

void sound_poll(void) {
    if (cur && absolute_time_diff_us(get_absolute_time(), note_end) <= 0) {
        cur += 2;
        start_note();
    }
}

void sound_message(void) {
    static const uint16_t notes[] = {1568, 70, 0, 40, 2093, 90, 0, 0};
    sound_play(notes);
}

void sound_alert(void) {
    static const uint16_t notes[] = {880, 120, 0, 60, 880, 120, 0, 0};
    sound_play(notes);
}
