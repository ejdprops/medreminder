/* Panel speaker (CrowPanel 7" V3.0 schematic): NS4168 I2S class-D amp, BCLK IO42, LRCLK IO18,
 * DATA IO17; CTRL pulled to 3V3 (always on), powered from 3V3; output to J5 (JST PH 2.0) through
 * 10 uF series caps, so lows are weak: tones of 1-4 kHz carry best.
 * The I2S channel stays enabled and sends zeros between sounds (auto clear), so the amp never
 * starts or stops (no pops). Sounds are rendered into PSRAM at start; playing one scales and
 * copies it into the DMA buffers from the audio task. */
#include "audio.h"

#include <math.h>
#include <string.h>

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

static const char *TAG = "audio";

#define PIN_BCLK 42
#define PIN_WS 18
#define PIN_DOUT 17
#define SAMPLE_RATE 16000
#define DMA_DESC 4
#define DMA_FRAMES 256
#define CHUNK 256        /* frames per write */
#define TONE_AMP 0.7f    /* of full scale, before volume */
#define VOLUME_DB 40.0f  /* volume 1 is this far below 100 */
#define QUEUE_LEN 4

typedef struct {
    int16_t *data;
    int len, cap; /* samples, mono */
} pcm_t;

typedef struct {
    audio_sound_t sound;
    int volume;
} req_t;

static pcm_t s_pcm[AUDIO_SOUND_COUNT];
static QueueHandle_t s_queue;
static i2s_chan_handle_t s_tx;

/* Sine tone with linear fades (ms) so the start and end don't click */
static void pcm_tone(pcm_t *p, float freq, int ms, float amp, int fade_ms)
{
    const int total = SAMPLE_RATE * ms / 1000;
    const int fade = SAMPLE_RATE * fade_ms / 1000;
    for (int k = 0; k < total && p->len < p->cap; k++) {
        float env = k < fade ? (float)k / fade : (total - k < fade ? (float)(total - k) / fade : 1.0f);
        p->data[p->len++] = (int16_t)(sinf(2.0f * (float)M_PI * freq * k / SAMPLE_RATE) * env * amp * 32767.0f);
    }
}

static void pcm_silence(pcm_t *p, int ms)
{
    for (int k = SAMPLE_RATE * ms / 1000; k > 0 && p->len < p->cap; k--) {
        p->data[p->len++] = 0;
    }
}

static void render(audio_sound_t sound)
{
    pcm_t *p = &s_pcm[sound];
    p->cap = SAMPLE_RATE; /* 1 s, more than any sound */
    p->data = heap_caps_malloc(p->cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!p->data) {
        ESP_LOGE(TAG, "no PSRAM for sound %d", sound);
        return;
    }
    switch (sound) {
    case AUDIO_ALERT:
        pcm_tone(p, 1047.0f, 180, TONE_AMP, 8); /* C6 */
        pcm_silence(p, 60);
        pcm_tone(p, 1319.0f, 180, TONE_AMP, 8); /* E6 */
        pcm_silence(p, 60);
        pcm_tone(p, 1568.0f, 260, TONE_AMP, 8); /* G6 */
        break;
    default:
        break;
    }
}

/* 100 = full scale, 1 = -VOLUME_DB, 0 = silent */
static float gain_of(int volume)
{
    if (volume <= 0) {
        return 0.0f;
    }
    if (volume > 100) {
        volume = 100;
    }
    return powf(10.0f, -VOLUME_DB * (100 - volume) / 99.0f / 20.0f);
}

static void play(const req_t *r)
{
    const pcm_t *p = &s_pcm[r->sound];
    float gain = gain_of(r->volume);
    if (!p->data || gain <= 0.0f) {
        return;
    }
    ESP_LOGI(TAG, "sound %d at %d (gain %.3f)", r->sound, r->volume, gain);
    static int16_t *buf; /* stereo: the amp gets the same on both; PSRAM (internal RAM is short) */
    if (!buf && !(buf = heap_caps_malloc(CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM))) {
        return;
    }
    for (int k = 0; k < p->len; k += CHUNK) {
        int n = p->len - k < CHUNK ? p->len - k : CHUNK;
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(p->data[k + i] * gain);
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
        }
        size_t done;
        i2s_channel_write(s_tx, buf, n * 2 * sizeof(int16_t), &done, portMAX_DELAY);
    }
}

static void task(void *arg)
{
    (void)arg;
    for (int i = 0; i < AUDIO_SOUND_COUNT; i++) {
        render((audio_sound_t)i);
    }
    req_t r;
    for (;;) {
        if (xQueueReceive(s_queue, &r, portMAX_DELAY)) {
            play(&r);
        }
    }
}

void audio_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESC;
    cc.dma_frame_num = DMA_FRAMES;
    cc.auto_clear = true; /* zeros between sounds */
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT,
                     .din = I2S_GPIO_UNUSED},
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(req_t));
    /* Stack in PSRAM: internal RAM is short (Wi-Fi, TLS and the RGB panel need it) */
    xTaskCreateWithCaps(task, "audio", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "started");
}

void audio_play(audio_sound_t sound, int volume)
{
    if (!s_queue || sound >= AUDIO_SOUND_COUNT) {
        return;
    }
    req_t r = {sound, volume};
    xQueueSend(s_queue, &r, 0);
}
