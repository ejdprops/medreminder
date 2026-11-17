#include "ui.h"

#include <esp_log.h>

static const char *TAG = "ui";

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"

static lv_obj_t *s_touch_dot;
static lv_obj_t *s_touch_label;

static void screen_touch_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_set_pos(s_touch_dot, p.x - 8, p.y - 8);
    lv_obj_remove_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_touch_label, "touch %d, %d", (int)p.x, (int)p.y);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "touch x=%d y=%d", (int)p.x, (int)p.y);
    }
}

static void build_static_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MedReminder");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Display test (7a)\nMatter running");
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9AA5B1), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 90);

    /* Color bars: check RGB order / inversion at a glance (red, green, blue, left to right) */
    static const uint32_t bars[] = {0xE53935, 0x43A047, 0x1E88E5};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 60, 40);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i]), 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i - 1) * 70, 20);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    s_touch_label = lv_label_create(scr);
    lv_label_set_text(s_touch_label, "touch the screen");
    lv_obj_set_style_text_color(s_touch_label, lv_color_hex(0xFFB300), 0);
    lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    s_touch_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_touch_dot);
    lv_obj_set_size(s_touch_dot, 16, 16);
    lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_touch_dot, lv_color_hex(0xFFB300), 0);
    lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_touch_dot, LV_OBJ_FLAG_CLICKABLE);
}

void ui_start(void)
{
    /* LVGL task stack in PSRAM: internal RAM is reserved for Matter and the SPI DMA buffers */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    cfg.lv_adapter_cfg.stack_in_psram = true;
    if (!bsp_display_start_with_config(&cfg)) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    if (bsp_display_lock(UINT32_MAX)) {
        build_static_screen();
        bsp_display_unlock();
    }
    esp_err_t err = bsp_display_show();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display show failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "UI started");
}

#else

void ui_start(void)
{
    ESP_LOGI(TAG, "no display on this board");
}

#endif
