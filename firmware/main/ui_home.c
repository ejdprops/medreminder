/* Home screen, 320x240 landscape. Date + time on top; at the bottom node sync status and the
 * firmware version centered, settings gear right, dev reset button left. Doses per
 * CONFIG_MEDREMINDER_UI_LAYOUT:
 *   RECT (C5 board): the next dose large in the center (tap to mark taken), four small pills around
 *     it following the day clockwise: UL = taken two ago, UR = most recently taken, LR = after
 *     next, LL = after that. Taking a dose rotates every pill one step.
 *   ROUND: date + progress on top and the day's pills listed in a 168px circle. */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_home.h"

#include <string.h>
#include <time.h>

#include <esp_log.h>

#include "audio.h"
#include "doses.h"
#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "time_sync.h"
#include "ui_tap.h"
#include "ui_settings.h"

static const char *TAG = "ui_home";

static lv_obj_t *s_date_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_dose_area; /* holds the pills; rebuilt when a dose is taken */
static int s_last_minute = -1; /* clock labels last drawn for this minute */

/* bg, name text, slot text per dose_state_t */
static const uint32_t s_pill_colors[][3] = {
    [DOSE_TAKEN] = {0x15803d, 0xf0fdf4, 0xbbf7d0},
    [DOSE_NEXT] = {0xb45309, 0xfffbeb, 0xfde68a},
    [DOSE_LATER] = {0x1d4ed8, 0xeff6ff, 0xbfdbfe},
    [DOSE_OVERDUE] = {0xb91c1c, 0xffffff, 0xfecaca},
};

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

static void flex_center(lv_obj_t *o, lv_flex_flow_t flow, int32_t gap)
{
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(o, gap, 0);
}

static void settings_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    ESP_LOGI(TAG, "settings tap");
    ui_settings_open();
    audio_play(AUDIO_CLICK);
}

static void build_top_line(lv_obj_t *scr)
{
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, 320, 30);
    lv_obj_set_pos(row, 0, 0);
    flex_center(row, LV_FLEX_FLOW_ROW, 8);

#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
    const lv_font_t *top_font = &inter_16_bold;
#else
    const lv_font_t *top_font = &inter_12_bold;
#endif
    s_date_label = label(row, "--- -- ---", top_font, 0x9ca3af);
    label(row, "\u00b7", top_font, 0x4b5563);
#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
    s_time_label = label(row, "--:--", top_font, 0xe5e7eb);
#else
    label(row, "3/5 taken", &inter_12_bold, 0xe5e7eb);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 44, 4);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x23262e), 0);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x22c55e), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 5);
    lv_bar_set_value(bar, 3, LV_ANIM_OFF);
#endif
}

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

/* "AM ✓" for taken, "MID · next" for next, "MID · late" for the overdue next dose, else the slot */
static const char *slot_text(int i, bool next, char *buf, size_t len)
{
    switch (dose_state(i)) {
    case DOSE_TAKEN:
        lv_snprintf(buf, len, "%s \u2713", dose_slot(i));
        return buf;
    case DOSE_NEXT:
        lv_snprintf(buf, len, "%s \u00b7 next", dose_slot(i));
        return buf;
    case DOSE_OVERDUE:
        if (next) {
            lv_snprintf(buf, len, "%s \u00b7 late", dose_slot(i));
            return buf;
        }
        return dose_slot(i);
    default:
        return dose_slot(i);
    }
}

static lv_obj_t *pill_base(lv_obj_t *parent, int i, int32_t w, int32_t h)
{
    dose_state_t st = dose_state(i);
    lv_obj_t *pill = plain_obj(parent);
    lv_obj_set_size(pill, w, h);
    lv_obj_set_style_radius(pill, h / 2, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(s_pill_colors[st][0]), 0);
    if (st == DOSE_OVERDUE) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, pill);
        lv_anim_set_exec_cb(&a, pulse_exec);
        lv_anim_set_values(&a, LV_OPA_COVER, 184); /* 1 -> 0.72 opacity, 1.4s cycle */
        lv_anim_set_duration(&a, 700);
        lv_anim_set_reverse_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    return pill;
}

static void build_doses(lv_obj_t *area);

static void rebuild_doses(void *arg)
{
    (void)arg;
    lv_obj_clean(s_dose_area);
    build_doses(s_dose_area);
    s_last_minute = -1; /* the round layout's time label was recreated */
}

static void take_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    if (!doses_take((int)(intptr_t)user_data)) {
        return;
    }
    audio_play(AUDIO_CLICK);
    lv_async_call(rebuild_doses, NULL); /* not from inside the pill's own event */
}

/* The next dose's pill marks it taken on tap */
static void make_takeable(lv_obj_t *pill, int i)
{
    lv_obj_set_style_bg_color(pill, lv_color_lighten(lv_color_hex(s_pill_colors[dose_state(i)][0]), LV_OPA_20),
                              LV_STATE_PRESSED);
    ui_on_tap(pill, take_cb, (void *)(intptr_t)i);
}

#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
/* Next dose large in the center (153x58); the others small (89x33) around it, between the top
 * line (y 0-30) and the status row (y 200-240). */
typedef struct {
    int32_t w, h;
    const lv_font_t *name_font, *slot_font;
} pill_size_t;

static const pill_size_t s_center_size = {153, 58, &inter_23_semibold, &inter_16_semibold};
static const pill_size_t s_corner_size = {89, 33, &inter_12_semibold, &inter_9_semibold};

/* Pill position by offset from the next dose (-2..+2), clockwise from upper left */
static const struct {
    int offset;
    int32_t x, y;
} s_positions[] = {
    {0, 83, 86},    /* center: next */
    {-1, 204, 45},  /* upper right: most recently taken */
    {1, 204, 152},  /* lower right: after next */
    {2, 27, 152},   /* lower left */
    {-2, 27, 45},   /* upper left: taken two ago */
};

static lv_obj_t *build_pill(lv_obj_t *area, int i, bool next, const pill_size_t *sz, int32_t x, int32_t y)
{
    const uint32_t *c = s_pill_colors[dose_state(i)];
    lv_obj_t *pill = pill_base(area, i, sz->w, sz->h);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_pad_hor(pill, sz->h / 3, 0);
    flex_center(pill, LV_FLEX_FLOW_COLUMN, 0);

    lv_obj_t *name = label(pill, dose_name(i), sz->name_font, c[1]);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    char buf[24];
    label(pill, slot_text(i, next, buf, sizeof(buf)), sz->slot_font, c[2]);
    return pill;
}

static void build_doses(lv_obj_t *area)
{
    /* Next = first dose not yet taken; if all are taken, the cycle starts over at the first */
    int next = doses_next();
    int count = doses_count();
    /* Fill center, then the nearest offsets, so fewer than five doses never repeat a pill */
    for (int p = 0; p < (int)(sizeof(s_positions) / sizeof(s_positions[0])) && p < count; p++) {
        int i = ((next + s_positions[p].offset) % count + count) % count;
        bool center = s_positions[p].offset == 0;
        lv_obj_t *pill = build_pill(area, i, center, center ? &s_center_size : &s_corner_size,
                                    s_positions[p].x, s_positions[p].y);
        if (center) {
            make_takeable(pill, i);
        }
    }
}
#else
/* Round screens: the day's pills inside a 168px circle with a TODAY/time header */
static void build_doses(lv_obj_t *area)
{
    lv_obj_t *circle = plain_obj(area);
    lv_obj_set_size(circle, 168, 168);
    lv_obj_set_pos(circle, 76, 32);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x101217), 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(0x1c1f26), 0);
    flex_center(circle, LV_FLEX_FLOW_COLUMN, 4);

    s_time_label = label(circle, "TODAY · --:--", &inter_8, 0x6b7280);
    lv_obj_set_style_text_letter_space(s_time_label, 1, 0);
    bool next_found = false;
    for (int i = 0; i < doses_count(); i++) {
        dose_state_t st = dose_state(i);
        lv_obj_t *pill = pill_base(circle, i, 104, 20);
        bool next = !next_found && st != DOSE_TAKEN;
        if (next) {
            next_found = true;
            make_takeable(pill, i);
        }
        lv_obj_set_style_pad_hor(pill, 9, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *name = label(pill, dose_name(i), &inter_10_semibold, s_pill_colors[st][1]);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char buf[24];
        label(pill, slot_text(i, next, buf, sizeof(buf)), &inter_9, s_pill_colors[st][2]);
    }
}
#endif

/* Placeholder until node sync is real */
static const bool s_nodes_synced = true;

/* Bottom: node sync status centered (faint green/red outline instead of a dot), version centered
 * below it, settings gear far right (dev reset button far left). */
static void build_bottom_row(lv_obj_t *scr)
{
    uint32_t tint = s_nodes_synced ? 0x22c55e : 0xef4444;
    lv_obj_t *status = plain_obj(scr);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(status, 10, 0);
    lv_obj_set_style_pad_ver(status, 3, 0);
    lv_obj_set_style_radius(status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_10, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(tint), 0);
    lv_obj_set_style_border_width(status, 1, 0);
    lv_obj_set_style_border_opa(status, LV_OPA_40, 0);
    lv_obj_set_style_border_color(status, lv_color_hex(tint), 0);
    label(status, s_nodes_synced ? "3 nodes synced" : "Nodes not synced", &inter_11_semibold, 0x9ca3af);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_t *ver = label(scr, fw_version_short(), &inter_8, 0x6b7280);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *gear = plain_obj(scr);
    lv_obj_set_size(gear, 36, 36);
    lv_obj_set_pos(gear, 276, 202);
    lv_obj_set_style_radius(gear, 10, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(0x2e333d), 0);
    flex_center(gear, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_ext_click_area(gear, 4); /* 36px drawn, 44px touch target */
    ui_on_tap(gear, settings_cb, NULL);

    lv_obj_t *img = lv_image_create(gear);
    lv_image_set_src(img, &icon_settings);
    lv_obj_set_style_image_recolor(img, lv_color_hex(0xd1d5db), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

#if CONFIG_MEDREMINDER_DEV_TOOLS
/* Dev: mark every dose not taken (and restart the test schedule) */
static void reset_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    doses_reset();
    audio_play(AUDIO_CLICK);
    rebuild_doses(NULL);
}

static void build_reset_button(lv_obj_t *scr)
{
    lv_obj_t *btn = plain_obj(scr);
    lv_obj_set_size(btn, 36, 36);
    lv_obj_set_pos(btn, 8, 202);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x7c2d12), 0); /* dev-only: orange edge */
    flex_center(btn, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_ext_click_area(btn, 4);
    ui_on_tap(btn, reset_cb, NULL);

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, &icon_reset);
    lv_obj_set_style_image_recolor(img, lv_color_hex(0xfdba74), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}
#endif

/* Every second: dose states and alerts, then the date and time labels (dashes until the first
 * SNTP sync) */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!time_is_valid()) {
        return;
    }
    time_t now = time(NULL);
    if (doses_tick(now)) {
        rebuild_doses(NULL);
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
    int hour12 = tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12;
#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
    lv_label_set_text_fmt(s_time_label, "%d:%02d %s", hour12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
#else
    lv_label_set_text_fmt(s_time_label, "TODAY · %d:%02d %s", hour12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
#endif
}

void ui_home_create(lv_obj_t *scr)
{
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0c0f), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    doses_init();
    build_top_line(scr);
    /* Transparent, non-clickable area in screen coordinates, so pill positions stay absolute */
    s_dose_area = plain_obj(scr);
    lv_obj_set_size(s_dose_area, 320, 200);
    build_doses(s_dose_area);
    build_bottom_row(scr);
#if CONFIG_MEDREMINDER_DEV_TOOLS
    build_reset_button(scr);
#endif

    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

#endif
