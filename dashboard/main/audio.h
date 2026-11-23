/* Panel speaker: NS4168 I2S class-D amp (always on, no enable pin) on J5. */
#pragma once

typedef enum {
    AUDIO_ALERT, /* overdue dose: three rising notes (C6 E6 G6), as on the C5 */
    AUDIO_SOUND_COUNT
} audio_sound_t;

/* I2S up and the sounds rendered (PSRAM); call once */
void audio_start(void);

/* Queue a sound at this volume (0-100, 0 = silent; a dB scale: each step is heard about the
 * same). Never blocks; dropped if the queue is full. */
void audio_play(audio_sound_t sound, int volume);
