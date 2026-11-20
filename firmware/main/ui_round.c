/* Home screen for round 240x240 displays without touch (CONFIG_MEDREMINDER_UI_LAYOUT_ROUND; the
 * C6 node). Made to be read across the room:
 *   - a thick ring around the edge in the next dose's color (red and pulsing when overdue, green
 *     when the day is done)
 *   - date and clock at the top; the next dose's time and name large in the middle, with "in 25 min"
 *     / "Due now" / "Late 42 min" under it
 *   - today's doses as dots along the bottom (filled when taken), the version under them
 * Not commissioned yet: the Matter QR and manual code instead (no touch, so no Settings page to
 * find them on); a static "Pairing" screen while Home sets it up (nothing animates: the C6 has one
 * core for Matter and LVGL), then "Paired" for 10 s. */
#include <sdkconfig.h>
#if __has_include("lvgl.h") && CONFIG_MEDREMINDER_UI_LAYOUT_ROUND
#include "ui_home.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>

#include "doses.h"
#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "matter_info.h"
#include "time_sync.h"
#include "ui_theme.h"

static const char *TAG = "ui_round";

#define SIZE 240
#define RING_W 14
#define OVERDUE_RED 0xef4444
#define LATE_TEXT 0xdc2626
#define DOT 12
#define DOT_GAP 5
#define DOTS_MAX 9 /* what fits across the circle at the dots' height; more: "3/12 taken" */
#define DOTS_Y 186
#define QR_SIZE 136
#define PAIRED_MS 10000

typedef enum { MODE_SETUP_CODE, MODE_PAIRING, MODE_PAIRED, MODE_DOSES } screen_mode_t;

static lv_obj_t *s_screen;
static lv_obj_t *s_date_label, *s_time_label, *s_when_label;
static int s_next = -1;      /* dose in the middle; -1 = none left today */
static int s_last_minute = -1; /* clock labels last drawn for this minute */
static screen_mode_t s_mode;
static unsigned s_completions;  /* matter_pairing_completions() last seen */
static uint32_t s_paired_at;     /* tick when the last pairing completed */

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

/* Centered label at a fixed height */
static lv_obj_t *label_at(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color, int32_t y)
{
    lv_obj_t *l = label(parent, text, font, color);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

/* Overdue: the ring blinks between full and faint. Not a smooth fade: redrawing the whole ring every
 * frame took the C6's one core from Matter (priority 1, below LVGL) and pairing timed out. */
static void ring_blink_cb(lv_timer_t *t)
{
    lv_obj_t *ring = lv_timer_get_user_data(t);
    bool full = lv_obj_get_style_border_opa(ring, 0) == LV_OPA_COVER;
    lv_obj_set_style_border_opa(ring, full ? LV_OPA_30 : LV_OPA_COVER, 0);
}

static void ring_delete_cb(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

static void build_ring(lv_obj_t *scr, uint32_t color, bool blink)
{
    lv_obj_t *ring = plain_obj(scr);
    lv_obj_set_size(ring, SIZE, SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, RING_W, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    if (blink) {
        lv_timer_t *t = lv_timer_create(ring_blink_cb, 700, ring);
        lv_obj_add_event_cb(ring, ring_delete_cb, LV_EVENT_DELETE, t);
    }
}

/* "in 25 min", "in 2h 05m", "Due now", "Late 42 min" for the middle dose */
static void update_when_label(void)
{
    if (!s_when_label || s_next < 0) {
        return;
    }
    if (!time_is_valid()) {
        lv_label_set_text(s_when_label, "");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int diff = dose_def(s_next)->minute - (tm.tm_hour * 60 + tm.tm_min);
    bool late = diff < 0;
    int m = late ? -diff : diff;
    char buf[24];
    if (dose_state(s_next) == DOSE_OVERDUE) {
        if (m < 60) {
            snprintf(buf, sizeof(buf), "Late %d min", m);
        } else {
            snprintf(buf, sizeof(buf), "Late %dh %02dm", m / 60, m % 60);
        }
    } else if (diff <= 0) {
        snprintf(buf, sizeof(buf), "Due now");
    } else if (m < 60) {
        snprintf(buf, sizeof(buf), "in %d min", m);
    } else {
        snprintf(buf, sizeof(buf), "in %dh %02dm", m / 60, m % 60);
    }
    lv_label_set_text(s_when_label, buf);
}

/* Today's doses along the bottom: filled when taken, outlined in the dose color otherwise (red for
 * overdue), the middle dose a size larger */
static void build_dots(lv_obj_t *scr)
{
    int today = 0, taken = 0;
    for (int i = 0; i < doses_count(); i++) {
        if (dose_due_today(i)) {
            today++;
            taken += dose_taken(i);
        }
    }
    if (today == 0) {
        return;
    }
    if (today > DOTS_MAX) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d taken", taken, today);
        label_at(scr, buf, &inter_12_bold, UI_T->muted, DOTS_Y);
        return;
    }
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, LV_SIZE_CONTENT, DOT + 4);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, DOTS_Y - 2);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, DOT_GAP, 0);
    for (int i = 0; i < doses_count(); i++) {
        if (!dose_due_today(i)) {
            continue;
        }
        uint32_t color = dose_colors[dose_def(i)->color];
        dose_state_t st = dose_state(i);
        int32_t d = i == s_next ? DOT + 4 : DOT;
        lv_obj_t *dot = plain_obj(row);
        lv_obj_set_size(dot, d, d);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        if (st == DOSE_TAKEN) {
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        } else {
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_border_color(dot, lv_color_hex(st == DOSE_OVERDUE ? OVERDUE_RED : color), 0);
        }
    }
}

/* Nothing left today: green ring, check, tomorrow's first dose */
static void build_done(lv_obj_t *scr, bool any_today)
{
    build_ring(scr, UI_T->ok, false);
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &icon_check_circle);
    lv_obj_set_style_image_recolor(img, lv_color_hex(UI_T->ok), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_image_set_scale(img, 512); /* 20px icon at 40px */
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 84);
    label_at(scr, any_today ? "All done today" : "No doses today", &inter_23_semibold, UI_T->text, 120);

    if (!time_is_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int first = doses_first_on((tm.tm_wday + 1) % 7);
    if (first >= 0) {
        char text[40], t[8];
        dose_format_time(dose_def(first)->minute, t, sizeof(t));
        snprintf(text, sizeof(text), "Tomorrow %s %s", t, dose_name(first));
        lv_obj_t *l = label_at(scr, text, &inter_14_semibold, UI_T->muted, 152);
        lv_obj_set_width(l, 180);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
}

static void build_next(lv_obj_t *scr, int i)
{
    bool overdue = dose_state(i) == DOSE_OVERDUE;
    build_ring(scr, overdue ? OVERDUE_RED : dose_colors[dose_def(i)->color], overdue);

    char t[8];
    dose_format_time(dose_def(i)->minute, t, sizeof(t));
    label_at(scr, t, &inter_48_bold, UI_T->text, 70);
    lv_obj_t *name = label_at(scr, dose_name(i), &inter_30_semibold, UI_T->text, 122);
    lv_obj_set_width(name, 190);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    s_when_label = label_at(scr, "", &inter_16_bold, overdue ? LATE_TEXT : UI_T->muted, 158);
    update_when_label();
}

/* Not commissioned: "Add in Apple Home", the QR, the manual code */
static void build_pairing(lv_obj_t *scr)
{
    label_at(scr, "Add in Apple Home", &inter_12_bold, UI_T->text, 22);
    char qr[64];
    if (matter_get_qr_code(qr, sizeof(qr))) {
        lv_obj_t *q = lv_qrcode_create(scr);
        lv_qrcode_set_size(q, QR_SIZE);
        lv_qrcode_set_dark_color(q, lv_color_black());
        lv_qrcode_set_light_color(q, lv_color_white());
        lv_qrcode_update(q, qr, strlen(qr));
        lv_obj_set_style_border_width(q, 4, 0); /* quiet zone */
        lv_obj_set_style_border_color(q, lv_color_white(), 0);
        lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 42);
    }
    char code[16];
    if (matter_get_manual_code(code, sizeof(code)) && strlen(code) == 11) {
        char dashed[16];
        snprintf(dashed, sizeof(dashed), "%.4s-%.3s-%.4s", code, code + 4, code + 7);
        label_at(scr, dashed, &inter_16_bold, UI_T->text, 190);
    }
}

/* Home is setting the device up: static, nothing to redraw */
static void build_pairing_progress(lv_obj_t *scr)
{
    build_ring(scr, UI_T->date, false);
    label_at(scr, "Pairing", &inter_30_semibold, UI_T->text, 84);
    lv_obj_t *l = label_at(scr, "Finish on your phone", &inter_14_semibold, UI_T->muted, 126);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_paired(lv_obj_t *scr)
{
    build_ring(scr, UI_T->ok, false);
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &icon_check_circle);
    lv_obj_set_style_image_recolor(img, lv_color_hex(UI_T->ok), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_image_set_scale(img, 512); /* 20px icon at 40px */
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 70);
    label_at(scr, "Pairing", &inter_23_semibold, UI_T->text, 112);
    label_at(scr, "successful", &inter_23_semibold, UI_T->text, 140);
}

static screen_mode_t current_mode(void)
{
    unsigned c = matter_pairing_completions();
    if (c != s_completions) {
        s_completions = c;
        s_paired_at = lv_tick_get();
        return MODE_PAIRED;
    }
    if (matter_pairing_active()) {
        return MODE_PAIRING;
    }
    if (s_mode == MODE_PAIRED && lv_tick_elaps(s_paired_at) < PAIRED_MS) {
        return MODE_PAIRED;
    }
    return matter_fabric_vendors(NULL, 0) > 0 ? MODE_DOSES : MODE_SETUP_CODE;
}

static void build(void)
{
    lv_obj_clean(s_screen);
    s_date_label = s_time_label = s_when_label = NULL;
    s_last_minute = -1;
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_T->bg), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    switch (s_mode) {
    case MODE_SETUP_CODE:
        build_pairing(s_screen);
        ESP_LOGI(TAG, "setup code");
        return;
    case MODE_PAIRING:
        build_pairing_progress(s_screen);
        ESP_LOGI(TAG, "pairing");
        return;
    case MODE_PAIRED:
        build_paired(s_screen);
        ESP_LOGI(TAG, "paired");
        return;
    case MODE_DOSES:
        break;
    }

    s_date_label = label_at(s_screen, "--- -- ---", &inter_12_bold, UI_T->date, 24);
    s_time_label = label_at(s_screen, "---- hrs", &inter_14_semibold, UI_T->time, 40);
    s_next = doses_next();
    bool any_today = false;
    for (int i = 0; i < doses_count(); i++) {
        any_today |= dose_due_today(i);
    }
    if (s_next >= 0) {
        build_next(s_screen, s_next);
    } else {
        build_done(s_screen, any_today);
    }
    build_dots(s_screen);
    label_at(s_screen, fw_version_short(), &inter_8, UI_T->caption, 210);
}

/* Every second: pairing and dose states (rebuild on change), then the clock labels once a minute */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    bool changed = doses_tick(now);
    screen_mode_t mode = current_mode();
    if (mode != s_mode || (mode == MODE_DOSES && (changed || doses_next() != s_next))) {
        s_mode = mode;
        build();
    }
    if (!s_time_label || !time_is_valid()) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    int minute = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    if (minute == s_last_minute) {
        return;
    }
    s_last_minute = minute;
    static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    lv_label_set_text_fmt(s_date_label, "%s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
    lv_label_set_text_fmt(s_time_label, "%02d%02d hrs", tm.tm_hour, tm.tm_min);
    update_when_label();
}

void ui_home_create(lv_obj_t *scr)
{
    s_screen = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    doses_init();
    s_completions = matter_pairing_completions();
    s_mode = current_mode();
    build();
    lv_timer_create(tick_cb, 1000, NULL);
    tick_cb(NULL);
}

void ui_home_restyle(void)
{
    build();
    tick_cb(NULL);
}

#endif
