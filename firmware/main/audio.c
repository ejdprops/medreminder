#include "audio.h"

#include <esp_log.h>

static const char *TAG = "audio";

#if __has_include("bsp/esp-bsp.h")
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <nvs.h>

#include "bsp/esp-bsp.h"

#define SAMPLE_RATE 16000
#define AMP_SETTLE_MS 150    /* amp start-up: sound sent sooner than this is lost */
#define CODEC_LINGER_MS 3000 /* keep codec + amp up this long after a sound, for quick follow-ups */
/* esp_codec_dev_write returns once samples are queued in the I2S DMA buffers, not once they are
 * played: before shutting down, push this much silence so the whole sound has reached the DAC */
#define DMA_FLUSH_MS (BSP_AUDIO_DMA_DESCRIPTORS * BSP_AUDIO_DMA_FRAMES * 1000 / SAMPLE_RATE + 20)
#define DEFAULT_VOLUME 60
#define NVS_NAMESPACE "audio"
#define NVS_KEY_VOLUME "volume"

static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_queue;
static volatile int s_volume = DEFAULT_VOLUME;

/* Sine tone, amplitude 0-1, with linear fades (ms) so the start and end don't click */
static void play_tone(float freq, int ms, float amp, int fade_ms)
{
    int16_t buf[256];
    const int total = SAMPLE_RATE * ms / 1000;
    const int fade = SAMPLE_RATE * fade_ms / 1000;
    for (int done = 0; done < total;) {
        int n = total - done < 256 ? total - done : 256;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            float env = k < fade ? (float)k / fade : (total - k < fade ? (float)(total - k) / fade : 1.0f);
            buf[i] = (int16_t)(sinf(2.0f * (float)M_PI * freq * k / SAMPLE_RATE) * env * amp * 20000.0f);
        }
        esp_codec_dev_write(s_codec, buf, n * sizeof(int16_t));
        done += n;
    }
}

/* One tick: 1ms attack, then exponential decay (time constant 4ms) over 25ms. A held tone with
 * hard edges sounded like two ticks (onset and cutoff). */
static void play_click(void)
{
    int16_t buf[400];
    const int total = SAMPLE_RATE * 25 / 1000;
    const int attack = SAMPLE_RATE / 1000;
    for (int k = 0; k < total; k++) {
        float t = (float)k / SAMPLE_RATE;
        float env = k < attack ? (float)k / attack : expf(-(t - 0.001f) / 0.004f);
        buf[k] = (int16_t)(sinf(2.0f * (float)M_PI * 2000.0f * t) * env * 26000.0f);
    }
    esp_codec_dev_write(s_codec, buf, total * sizeof(int16_t));
}

static void silence(int ms)
{
    int16_t buf[256] = {0};
    for (int left = SAMPLE_RATE * ms / 1000; left > 0; left -= 256) {
        esp_codec_dev_write(s_codec, buf, (left < 256 ? left : 256) * sizeof(int16_t));
    }
}

/* Codec, I2S and amp are up only around sounds: idle internal RAM stays with Matter, and the amp
 * (which hisses when idle) is off. The amp is switched with the codec, not per sound, because it
 * needs AMP_SETTLE_MS to start: a short click sent right after switching it on is never heard. */
static bool codec_up(void)
{
    if (s_codec) {
        return true;
    }
    s_codec = bsp_audio_codec_speaker_init();
    if (!s_codec) {
        ESP_LOGE(TAG, "ES8311 init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = SAMPLE_RATE,
    };
    int err = esp_codec_dev_open(s_codec, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed: %d", err);
        bsp_audio_deinit();
        s_codec = NULL;
        return false;
    }
    bsp_board_pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(AMP_SETTLE_MS));
    return true;
}

static void codec_down(void)
{
    if (s_codec) {
        silence(DMA_FLUSH_MS);
        bsp_board_pa_set(false);
        bsp_audio_deinit(); /* deletes (and closes) the codec, then the I2S channel */
        s_codec = NULL;
    }
}

static void play(audio_sound_t sound)
{
    if (s_volume == 0 || !codec_up()) {
        return;
    }
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    ESP_LOGI(TAG, "play %s at volume %d", sound == AUDIO_CLICK ? "click" : "preview", s_volume);
    switch (sound) {
    case AUDIO_CLICK:
        play_click();
        break;
    case AUDIO_PREVIEW:
        play_tone(880.0f, 150, 1.0f, 5);
        silence(100);
        play_tone(880.0f, 150, 1.0f, 5);
        break;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        audio_sound_t sound;
        TickType_t wait = s_codec ? pdMS_TO_TICKS(CODEC_LINGER_MS) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &sound, wait) == pdTRUE) {
            play(sound);
        } else {
            codec_down();
        }
    }
}

void audio_start(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, NVS_KEY_VOLUME, &v) == ESP_OK && v <= 100) {
            s_volume = v;
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "volume %d", s_volume);

    s_queue = xQueueCreate(4, sizeof(audio_sound_t));
    if (!s_queue || xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", 4096, NULL, 5, NULL, tskNO_AFFINITY,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "audio task create failed");
    }
}

void audio_play(audio_sound_t sound)
{
    if (s_queue) {
        xQueueSend(s_queue, &sound, 0);
    }
}

int audio_get_volume(void)
{
    return s_volume;
}

void audio_set_volume(int volume)
{
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    s_volume = volume;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_VOLUME, (uint8_t)volume);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "volume %d", volume);
}

#else

void audio_start(void)
{
    ESP_LOGI(TAG, "no audio on this board");
}

void audio_play(audio_sound_t sound)
{
    (void)sound;
}

int audio_get_volume(void)
{
    return 0;
}

void audio_set_volume(int volume)
{
    (void)volume;
}

#endif
