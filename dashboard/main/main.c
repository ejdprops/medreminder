/* MedReminder dashboard, step 1: bring-up screen. Checks the panel (color order: the swatches
 * must read red/green/blue/white), touch (a ring follows the finger, coordinates shown and logged)
 * and the one-wire link to the XIAO node (status and today's doses from the C5). */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <esp_log.h>

#include "board.h"
#include "link.h"

static const char *TAG = "dash";

#define TZ "CST6CDT,M3.2.0,M11.1.0" /* the C5's CONFIG_MEDREMINDER_TZ */

static lv_obj_t *s_link_label, *s_doses_label, *s_touch_label, *s_ring;

static void touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(s_ring, p.x - 20, p.y - 20);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_touch_label, "touch %d, %d", (int)p.x, (int)p.y);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "touch %d, %d", (int)p.x, (int)p.y);
    }
}

static void update_cb(lv_timer_t *t)
{
    (void)t;
    static link_view_t v; /* static: off the LVGL task stack */
    bool changed = link_get(&v);
    char when[40] = "clock not set";
    if (v.status.time) {
        time_t now = v.status.time;
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(when, sizeof(when), "%a %d %b  %H:%M:%S", &tm);
    }
    lv_label_set_text_fmt(s_link_label, "Link %s  |  %lu ok, %lu failed\nC5 %s  |  %s", v.up ? "UP" : "DOWN",
                          (unsigned long)v.ok, (unsigned long)v.failed,
                          v.status.flags & LINK_FLAG_MAIN_CONNECTED ? "connected" : "NOT connected", when);
    if (!changed) {
        return;
    }
    static char text[DOSE_MAX * 40];
    int len = snprintf(text, sizeof(text), "Doses (%u):\n", v.doses.count);
    for (int i = 0; i < v.doses.count && len < (int)sizeof(text) - 40; i++) {
        const dose_def_t *d = &v.doses.defs[i];
        len += snprintf(text + len, sizeof(text) - len, "%02d%02d  %s%s\n", d->minute / 60, d->minute % 60, d->name,
                        (v.doses.taken >> i) & 1 ? "  - taken" : "");
    }
    lv_label_set_text(s_doses_label, text);
}

static lv_obj_t *swatch(lv_obj_t *parent, uint32_t color, const char *name, int x)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 110, 60);
    lv_obj_set_pos(box, x, 70);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, x, 136);
    return box;
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "MedReminder dashboard - bring-up");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 20, 20);

    swatch(scr, 0xff0000, "red", 20);
    swatch(scr, 0x00ff00, "green", 150);
    swatch(scr, 0x0000ff, "blue", 280);
    swatch(scr, 0xffffff, "white", 410);

    s_link_label = lv_label_create(scr);
    lv_obj_set_pos(s_link_label, 20, 180);
    lv_obj_set_style_text_font(s_link_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_link_label, "Link: starting");

    s_doses_label = lv_label_create(scr);
    lv_obj_set_pos(s_doses_label, 20, 250);
    lv_label_set_text(s_doses_label, "Doses: waiting for the node");

    s_touch_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_touch_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_touch_label, "touch anywhere");
    lv_obj_align(s_touch_label, LV_ALIGN_TOP_RIGHT, -20, 20);

    /* Corner marks: a touch on each should put the ring on it */
    static const lv_align_t corners[] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
                                         LV_ALIGN_BOTTOM_RIGHT};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *m = lv_obj_create(scr);
        lv_obj_remove_style_all(m);
        lv_obj_set_size(m, 12, 12);
        lv_obj_set_style_bg_color(m, lv_color_hex(0xfacc15), 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
        lv_obj_align(m, corners[i], 0, 0);
        lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    }

    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, 40, 40);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(0x22c55e), 0);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_timer_create(update_cb, 500, NULL);
}

void app_main(void)
{
    setenv("TZ", TZ, 1);
    tzset();
    link_start();
    if (!board_start()) {
        return;
    }
    if (board_lock()) {
        build();
        board_unlock();
    }
    board_backlight(100);
    ESP_LOGI(TAG, "started");
}
