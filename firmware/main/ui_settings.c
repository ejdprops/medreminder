/* Settings screen, 320x240: Matter setup code (QR + manual), pairing status, brightness, alert volume.
 * Same dark style as the home screen (no design artboard for this page yet). */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_settings.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>

#include "audio.h"
#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "matter_info.h"
#include "ui_about.h"
#include "ui_doses.h"
#include "ui_tap.h"

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#define HAVE_BACKLIGHT 1
#endif

static const char *TAG = "ui_settings";

#define VENDOR_APPLE_HOME 0x1349

static lv_obj_t *s_screen;
static lv_obj_t *s_return_to;
static lv_obj_t *s_qr_card;
static lv_obj_t *s_paired_card;
static lv_obj_t *s_code_label;
static lv_obj_t *s_status_label;
static lv_timer_t *s_refresh_timer;
static int s_last_fabrics = -1;
static bool s_code_revealed;

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

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *l = label(parent, text, &inter_8, 0x6b7280);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent, uint32_t bg, uint32_t border)
{
    lv_obj_t *c = plain_obj(parent);
    lv_obj_set_size(c, 132, 132);
    lv_obj_set_pos(c, 12, 50);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(border), 0);
    return c;
}

static void update_code_visibility(bool paired)
{
    bool show_qr = !paired || s_code_revealed;
    lv_obj_set_flag(s_qr_card, LV_OBJ_FLAG_HIDDEN, !show_qr);
    lv_obj_set_flag(s_paired_card, LV_OBJ_FLAG_HIDDEN, show_qr);
}

static void refresh_status(void)
{
    uint16_t vendors[5];
    int n = matter_fabric_vendors(vendors, 5);
    if (n == s_last_fabrics) {
        return;
    }
    s_last_fabrics = n;
    bool apple = false;
    for (int i = 0; i < n && i < 5; i++) {
        apple |= vendors[i] == VENDOR_APPLE_HOME;
    }
    if (n == 0) {
        lv_label_set_text(s_status_label, "Not paired");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xfbbf24), 0);
        s_code_revealed = false;
    } else {
        lv_label_set_text_fmt(s_status_label, "%s · %d fabric%s", apple ? "Apple Home" : "Paired", n,
                              n == 1 ? "" : "s");
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x4ade80), 0);
    }
    update_code_visibility(n > 0);
    ESP_LOGI(TAG, "pairing: %d fabric(s)%s", n, apple ? ", Apple Home" : "");
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
}

static void back_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    lv_screen_load(s_return_to);
    audio_play(AUDIO_CLICK);
    ESP_LOGI(TAG, "back");
}

static void doses_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    ui_doses_open();
    audio_play(AUDIO_CLICK);
}

static void about_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    ui_about_open();
    audio_play(AUDIO_CLICK);
}

static void reveal_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    s_code_revealed = true;
    update_code_visibility(true);
    audio_play(AUDIO_CLICK);
    ESP_LOGI(TAG, "setup code revealed");
}

static void brightness_cb(lv_event_t *e)
{
    int32_t v = lv_slider_get_value(lv_event_get_target_obj(e));
#ifdef HAVE_BACKLIGHT
    bsp_display_brightness_set((int)v);
#endif
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        ESP_LOGI(TAG, "brightness %d%%", (int)v);
    }
}

/* Saved and previewed on release, so dragging doesn't write NVS or queue sounds */
static void volume_cb(lv_event_t *e)
{
    audio_set_volume((int)lv_slider_get_value(lv_event_get_target_obj(e)));
    audio_play(AUDIO_PREVIEW);
}

static lv_obj_t *slider(lv_obj_t *parent, int32_t y, int32_t min, int32_t max, int32_t value)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 136, 6);
    lv_obj_set_pos(s, 166, y);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(s, 3, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x23262e), 0);
    lv_obj_set_style_radius(s, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x9ca3af), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xe5e7eb), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB); /* 18px knob */
    lv_obj_set_style_opa(s, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(s, 14); /* 6px track, ~34px touch band */
    return s;
}

static void build_qr_card(lv_obj_t *scr)
{
    s_qr_card = card(scr, 0xffffff, 0xffffff);
    char qr[128] = "";
    if (!matter_get_qr_code(qr, sizeof(qr))) {
        ESP_LOGE(TAG, "QR code unavailable");
    }
    lv_obj_t *q = lv_qrcode_create(s_qr_card);
    lv_qrcode_set_size(q, 116);
    lv_qrcode_set_dark_color(q, lv_color_black());
    lv_qrcode_set_light_color(q, lv_color_white());
    lv_qrcode_update(q, qr, strlen(qr));
    lv_obj_center(q);
    ESP_LOGI(TAG, "QR: %s", qr);
}

static void build_paired_card(lv_obj_t *scr)
{
    s_paired_card = card(scr, 0x101217, 0x1c1f26);
    lv_obj_set_flex_flow(s_paired_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_paired_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_paired_card, 6, 0);

    label(s_paired_card, "✓ Paired", &inter_12_bold, 0x4ade80);
    lv_obj_t *hint = label(s_paired_card, "Setup code hidden", &inter_9, 0x6b7280);
    lv_obj_set_style_margin_bottom(hint, 6, 0);

    lv_obj_t *btn = plain_obj(s_paired_card);
    lv_obj_set_size(btn, 112, 30);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2e333d), 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 7); /* 44px tall touch target */
    ui_on_tap(btn, reveal_cb, NULL);
    lv_obj_t *t = label(btn, "Show setup code", &inter_10_semibold, 0xd1d5db);
    lv_obj_center(t);
}

static void build(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0b0c0f), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Header: back + title */
    lv_obj_t *back = plain_obj(s_screen);
    lv_obj_set_size(back, 36, 36);
    lv_obj_set_pos(back, 8, 6);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x2e333d), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 4);
    ui_on_tap(back, back_cb, NULL);
    lv_obj_t *chev = lv_image_create(back);
    lv_image_set_src(chev, &icon_chevron_left);
    lv_obj_set_style_image_recolor(chev, lv_color_hex(0xd1d5db), 0);
    lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
    lv_obj_center(chev);

    lv_obj_t *title = label(s_screen, "Settings", &inter_12_bold, 0xe5e7eb);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 54, 17);

    /* Header right: Doses, About */
    lv_obj_t *doses = plain_obj(s_screen);
    lv_obj_set_size(doses, 72, 36);
    lv_obj_set_pos(doses, 160, 6);
    lv_obj_set_style_radius(doses, 10, 0);
    lv_obj_set_style_bg_opa(doses, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(doses, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(doses, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(doses, 1, 0);
    lv_obj_set_style_border_color(doses, lv_color_hex(0x2e333d), 0);
    lv_obj_set_ext_click_area(doses, 4);
    ui_on_tap(doses, doses_cb, NULL);
    lv_obj_center(label(doses, "Doses", &inter_11_semibold, 0xd1d5db));

    lv_obj_t *about = plain_obj(s_screen);
    lv_obj_set_size(about, 72, 36);
    lv_obj_set_pos(about, 240, 6);
    lv_obj_set_style_radius(about, 10, 0);
    lv_obj_set_style_bg_opa(about, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(about, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(about, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(about, 1, 0);
    lv_obj_set_style_border_color(about, lv_color_hex(0x2e333d), 0);
    lv_obj_set_ext_click_area(about, 4);
    ui_on_tap(about, about_cb, NULL);
    lv_obj_center(label(about, "About", &inter_11_semibold, 0xd1d5db));

    /* Left: setup code */
    build_qr_card(s_screen);
    build_paired_card(s_screen);
    char manual[24] = "";
    if (matter_get_manual_code(manual, sizeof(manual)) && strlen(manual) == 11) {
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%.4s-%.3s-%.4s", manual, manual + 4, manual + 7);
        s_code_label = label(s_screen, fmt, &inter_12_bold, 0xe5e7eb);
    } else {
        s_code_label = label(s_screen, manual, &inter_12_bold, 0xe5e7eb);
    }
    lv_obj_set_pos(s_code_label, 0, 190);
    lv_obj_set_width(s_code_label, 156);
    lv_obj_set_style_text_align(s_code_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *code_cap = label(s_screen, "Matter setup code", &inter_9, 0x6b7280);
    lv_obj_set_pos(code_cap, 0, 208);
    lv_obj_set_width(code_cap, 156);
    lv_obj_set_style_text_align(code_cap, LV_TEXT_ALIGN_CENTER, 0);

    /* Right: status and sliders */
    caption(s_screen, "PAIRING", 166, 52);
    s_status_label = label(s_screen, "", &inter_11_semibold, 0x9ca3af);
    lv_obj_set_pos(s_status_label, 166, 66);

    caption(s_screen, "BRIGHTNESS", 166, 104);
#ifdef HAVE_BACKLIGHT
    int brightness = bsp_display_brightness_get();
#else
    int brightness = 100;
#endif
    lv_obj_t *b = slider(s_screen, 126, 10, 100, brightness);
    lv_obj_add_event_cb(b, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(b, brightness_cb, LV_EVENT_RELEASED, NULL);

    caption(s_screen, "ALERT VOLUME", 166, 160);
    lv_obj_t *v = slider(s_screen, 182, 0, 100, audio_get_volume());
    lv_obj_add_event_cb(v, volume_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *ver = label(s_screen, fw_version_short(), &inter_8, 0x6b7280);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
}

void ui_settings_open(void)
{
    if (!s_screen) {
        build();
    }
    s_return_to = lv_screen_active();
    s_code_revealed = false; /* when paired, hide the setup code again on every visit */
    s_last_fabrics = -1;
    refresh_status();
    if (!s_refresh_timer) {
        s_refresh_timer = lv_timer_create(refresh_timer_cb, 1000, NULL);
    }
    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "open");
}

#endif
