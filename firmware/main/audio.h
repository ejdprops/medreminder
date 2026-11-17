#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_CLICK,   /* UI feedback when a tap changes something */
    AUDIO_PREVIEW, /* volume preview: the alert chime */
    AUDIO_ALERT,   /* overdue dose: three rising notes (C6 E6 G6) */
    AUDIO_SOUND_COUNT
} audio_sound_t;

/* Start the audio task and load the saved volume. Call once the BSP's I2C bus and IO expander are
 * up (the BSP must be initialized from one task). No-op on boards without the audio BSP. */
void audio_start(void);

/* Queue a sound; never blocks (dropped if the queue is full). Silent at volume 0. */
void audio_play(audio_sound_t sound);

/* Output volume 0-100, saved in NVS */
int audio_get_volume(void);
void audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
