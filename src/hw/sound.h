#ifndef HW_SOUND_H
#define HW_SOUND_H

#include <stdint.h>

void sound_init(void);
// Queue a short melody: pairs of {freq_hz, duration_ms}, terminated by {0,0}.
void sound_play(const uint16_t *notes);
void sound_poll(void);

void sound_message(void);
void sound_alert(void);

#endif
