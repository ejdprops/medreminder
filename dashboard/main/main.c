/* MedReminder dashboard: CrowPanel 7" next to the pills, fed by a XIAO C6 node over one wire
 * (link.c). The screen is ui.c; the panel hardware board.c; Wi-Fi (wifi.c) feeds the weather and
 * calendar tiles; audio.c drives the speaker. */
#include <stdlib.h>
#include <time.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>

#include "audio.h"
#include "board.h"
#include "calendar.h"
#include "link.h"
#include "ui.h"
#include "weather.h"
#include "wifi.h"

static const char *TAG = "dash";

/* Every HEAP_LOG_S: internal RAM (Wi-Fi, TLS, DMA need it) and PSRAM */
#define HEAP_LOG_S 10
static void heap_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "heap: internal free %u KB, min %u KB, largest %u KB; psram free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

#define TZ "CST6CDT,M3.2.0,M11.1.0" /* the C5's CONFIG_MEDREMINDER_TZ */

void app_main(void)
{
    setenv("TZ", TZ, 1);
    tzset();
    link_start();
    if (!board_start()) {
        return;
    }
    if (board_lock()) {
        ui_start();
        board_unlock();
    }
    board_backlight(100);
    audio_start();
    /* After the display: Wi-Fi only feeds the weather/calendar tiles */
    wifi_start();
    weather_start();
    calendar_start();
    ESP_LOGI(TAG, "started");
    /* From the esp_timer task: app_main returns and frees its (internal) stack */
    const esp_timer_create_args_t ta = {.callback = heap_timer_cb, .name = "heap"};
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) {
        esp_timer_start_periodic(t, HEAP_LOG_S * 1000000ULL);
    }
}
