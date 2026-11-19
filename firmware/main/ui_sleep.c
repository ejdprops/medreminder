#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_sleep.h"

#include <time.h>

#include <esp_log.h>
#include <lvgl.h>
#include <nvs.h>

#include "doses.h"

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#define HAVE_BACKLIGHT 1
#endif

static const char *TAG = "ui_sleep";

#define DEFAULT_MINUTES 5

static int s_minutes = DEFAULT_MINUTES;
static bool s_asleep;
static int s_brightness;  /* level to restore on wake */
static lv_obj_t *s_shield; /* while asleep: catches the waking tap */

static void backlight(int percent)
{
#ifdef HAVE_BACKLIGHT
    bsp_display_brightness_set(percent);
#endif
}

static void shield_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        ui_sleep_wake("tap");
        return;
    }
    /* Released: the tap is over, let the next one through */
    lv_obj_delete_async(s_shield);
    s_shield = NULL;
}

static void sleep_now(void)
{
#ifdef HAVE_BACKLIGHT
    s_brightness = bsp_display_brightness_get();
#endif
    backlight(0);
    s_asleep = true;
    s_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_shield);
    lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESS_LOST, NULL);
    ESP_LOGI(TAG, "screen off after %d min idle", s_minutes);
}

void ui_sleep_wake(const char *why)
{
    lv_display_trigger_activity(NULL);
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    backlight(s_brightness > 0 ? s_brightness : 100);
    if (s_shield && !lv_obj_has_state(s_shield, LV_STATE_PRESSED)) {
        lv_obj_delete(s_shield); /* woken without a touch: nothing to swallow */
        s_shield = NULL;
    }
    ESP_LOGI(TAG, "screen on (%s)", why);
}

static void timer_cb(lv_timer_t *t)
{
    bool due = doses_any_due(time(NULL));
    if (s_asleep) {
        if (due) {
            ui_sleep_wake("dose due");
        }
        return;
    }
    if (s_minutes > 0 && !due && lv_display_get_inactive_time(NULL) >= (uint32_t)s_minutes * 60000) {
        sleep_now();
    }
}

void ui_sleep_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "sleep", &v) == ESP_OK) {
            s_minutes = v;
        }
        nvs_close(nvs);
    }
    lv_timer_create(timer_cb, 1000, NULL);
}

int ui_sleep_minutes(void)
{
    return s_minutes;
}

void ui_sleep_set_minutes(int minutes)
{
    s_minutes = minutes;
    nvs_handle_t nvs;
    if (nvs_open("ui", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "sleep", (uint8_t)minutes);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "screen off: %d min (0 = never)", minutes);
}

#endif
