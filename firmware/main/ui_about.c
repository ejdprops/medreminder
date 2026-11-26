/* About screen, 320x240, opened from Settings: Averak Labs logo on the left in a dark tile (the
 * logo's own background, #0b0d14, so the image edges don't show); product, firmware, Matter,
 * hardware, uptime and licenses on the right. */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_about.h"

#include <stdio.h>

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_log.h>
#include <esp_timer.h>
#if __has_include("esp_psram.h")
#include <esp_psram.h>
#endif

#include "audio.h"
#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "images/images.h"
#include "matter_info.h"
#include "ui_tap.h"
#include "ui_theme.h"

static const char *TAG = "ui_about";

#define BG_COLOR (UI_T->bg)
#define LOGO_BG 0x0b0d14 /* the logo's background */

static lv_obj_t *s_screen;
static lv_obj_t *s_return_to;
static lv_obj_t *s_uptime_label;

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/* Caption + value stacked in the right column; returns the value label */
static lv_obj_t *field(lv_obj_t *col, const char *caption, const char *value)
{
    lv_obj_t *c = label(col, caption, &inter_8, UI_T->caption);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_style_margin_top(c, 3, 0);
    lv_obj_t *v = label(col, value, &inter_10_semibold, UI_T->text2);
    lv_obj_set_width(v, LV_PCT(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    return v;
}

static void update_uptime(void)
{
    int64_t s = esp_timer_get_time() / 1000000;
    int d = (int)(s / 86400), h = (int)(s / 3600 % 24), m = (int)(s / 60 % 60);
    if (d > 0) {
        lv_label_set_text_fmt(s_uptime_label, "%dd %dh %dm", d, h, m);
    } else {
        lv_label_set_text_fmt(s_uptime_label, "%dh %dm", h, m);
    }
}

static void back_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    lv_screen_load(s_return_to);
    audio_play(AUDIO_CLICK);
    ESP_LOGI(TAG, "back");
}

static void build(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Header: back + title (same as Settings) */
    lv_obj_t *back = plain_obj(s_screen);
    lv_obj_set_size(back, 36, 36);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_T->btn_bg), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_T->btn_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_set_ext_click_area(back, 4);
    ui_on_tap(back, back_cb, NULL);
    lv_obj_t *chev = lv_image_create(back);
    lv_image_set_src(chev, &icon_chevron_left);
    lv_obj_set_style_image_recolor(chev, lv_color_hex(UI_T->text2), 0);
    lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
    lv_obj_center(chev);

    lv_obj_t *title = label(s_screen, "About", &inter_12_bold, UI_T->text);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 54, 17);

    /* Left: the brand mark (logo + pill, square, as the app icon) in a tile, centered where the facts are */
    lv_obj_t *tile = plain_obj(s_screen);
    lv_obj_set_size(tile, 124, 124);
    lv_obj_set_pos(tile, 4, 78);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(LOGO_BG), 0);
    lv_obj_t *logo = lv_image_create(tile);
    lv_image_set_src(logo, &img_averak_logo_small);
    lv_obj_center(logo);

    /* Right: facts */
    lv_obj_t *col = plain_obj(s_screen);
    lv_obj_set_size(col, 176, 196);
    lv_obj_set_pos(col, 136, 44);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    matter_device_info_t info = {0};
    if (!matter_get_device_info(&info)) {
        ESP_LOGW(TAG, "Matter device info unavailable");
    }
    label(col, info.product_name[0] ? info.product_name : "Pill Reminder", &inter_16_bold, UI_T->text);
    label(col, info.vendor_name[0] ? info.vendor_name : "Averak Labs", &inter_11_semibold, UI_T->muted);

    char buf[64];
    field(col, "FIRMWARE", fw_version_short());
    label(col, fw_version_git(), &inter_9, UI_T->caption);

    snprintf(buf, sizeof(buf), "SW %u · VID %04X · PID %04X", fw_version_number(), info.vendor_id,
             info.product_id);
    field(col, "MATTER", buf);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    size_t psram = 0;
#if __has_include("esp_psram.h")
    psram = esp_psram_get_size();
#endif
#if CONFIG_IDF_TARGET_ESP32C5
    const char *target = "ESP32-C5";
#else
    const char *target = CONFIG_IDF_TARGET;
#endif
    snprintf(buf, sizeof(buf), "%s r%d.%d · %u+%uMB", target, chip.revision / 100, chip.revision % 100,
             (unsigned)(psram >> 20), (unsigned)(flash >> 20));
    field(col, "HARDWARE \u00b7 PSRAM+FLASH", buf);

    s_uptime_label = field(col, "UPTIME", "");

    lv_obj_t *lic = field(col, "LICENSES", "Inter OFL-1.1 · Waveshare Apache-2.0");
    lv_obj_set_style_text_font(lic, &inter_9, 0);
    lv_obj_set_style_text_color(lic, lv_color_hex(UI_T->muted), 0);
}

void ui_about_invalidate(void)
{
    if (s_screen && s_screen != lv_screen_active()) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
}

void ui_about_open(void)
{
    if (!s_screen) {
        build();
    }
    s_return_to = lv_screen_active();
    update_uptime();
    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "open");
}

#endif
