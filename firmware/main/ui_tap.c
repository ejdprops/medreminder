#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_tap.h"

#include <esp_log.h>

static const char *TAG = "ui_tap";

typedef struct {
    ui_tap_cb_t cb;
    void *user_data;
} tap_t;

/* One touch point, so one press time is enough */
static uint32_t s_pressed_at;

static void tap_event_cb(lv_event_t *e)
{
    tap_t *t = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_pressed_at = lv_tick_get();
        break;
    case LV_EVENT_CLICKED: {
        uint32_t held = lv_tick_elaps(s_pressed_at);
        if (held < UI_TAP_MIN_MS) {
            ESP_LOGI(TAG, "tap ignored (%u ms)", (unsigned)held);
            break;
        }
        t->cb(lv_event_get_current_target_obj(e), t->user_data);
        break;
    }
    case LV_EVENT_DELETE:
        lv_free(t);
        break;
    default:
        break;
    }
}

void ui_on_tap(lv_obj_t *obj, ui_tap_cb_t cb, void *user_data)
{
    tap_t *t = lv_malloc(sizeof(*t));
    LV_ASSERT_MALLOC(t);
    t->cb = cb;
    t->user_data = user_data;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, tap_event_cb, LV_EVENT_ALL, t);
}

#endif
