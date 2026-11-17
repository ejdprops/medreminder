/* Home screen, 320x240 landscape. Date + time on top, node status + settings gear (and the firmware
 * version, bottom-right) at the bottom. Doses per CONFIG_MEDREMINDER_UI_LAYOUT:
 *   RECT (C5 board): five equal 148x50 pills, one per corner and one in the center. The center is
 *     the next dose; the rest follow the day clockwise: UL = taken two ago, UR = most recently
 *     taken, LR = after next, LL = after that. Taking a dose rotates every pill one step.
 *   ROUND: date + progress on top and the day's pills listed in a 168px circle. */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_home.h"

#include <string.h>
#include <time.h>

#include <esp_log.h>

#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "time_sync.h"
#include "ui_settings.h"

static const char *TAG = "ui_home";

static lv_obj_t *s_date_label;
static lv_obj_t *s_time_label;

typedef enum { DOSE_TAKEN, DOSE_NEXT, DOSE_LATER, DOSE_OVERDUE } dose_state_t;

typedef struct {
    const char *name;
    const char *slot;
    dose_state_t state;
} dose_t;

/* Placeholder data, in daily order */
static const dose_t s_doses[] = {
    {"Evan", "AM", DOSE_TAKEN},
    {"Karen", "AM", DOSE_TAKEN},
    {"Karen", "MID", DOSE_NEXT},
    {"Karen", "PM", DOSE_LATER},
    {"Karen", "BED", DOSE_LATER},
};
#define DOSE_COUNT ((int)(sizeof(s_doses) / sizeof(s_doses[0])))

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

static void dot(lv_obj_t *parent, int32_t size, uint32_t color)
{
    lv_obj_t *d = plain_obj(parent);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "settings tap");
    ui_settings_open();
}

static void build_top_line(lv_obj_t *scr)
{
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, 320, 30);
    lv_obj_set_pos(row, 0, 0);
    flex_center(row, LV_FLEX_FLOW_ROW, 8);

    s_date_label = label(row, "--- -- ---", &inter_12_bold, 0x9ca3af);
    label(row, "·", &inter_12_bold, 0x4b5563);
#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
    s_time_label = label(row, "--:--", &inter_12_bold, 0xe5e7eb);
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

/* "AM ✓" for taken, "MID · next" for next, else the slot */
static const char *slot_text(const dose_t *d, char *buf, size_t len)
{
    if (d->state == DOSE_TAKEN) {
        lv_snprintf(buf, len, "%s ✓", d->slot);
        return buf;
    }
    if (d->state == DOSE_NEXT) {
        lv_snprintf(buf, len, "%s · next", d->slot);
        return buf;
    }
    return d->slot;
}

static lv_obj_t *pill_base(lv_obj_t *parent, const dose_t *d, int32_t w, int32_t h)
{
    lv_obj_t *pill = plain_obj(parent);
    lv_obj_set_size(pill, w, h);
    lv_obj_set_style_radius(pill, h / 2, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(s_pill_colors[d->state][0]), 0);
    if (d->state == DOSE_OVERDUE) {
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

#if CONFIG_MEDREMINDER_UI_LAYOUT_RECT
/* Two columns of 148 (8px margins and gap) and three rows of 50 (8px gaps) fill the 320x166 area
 * between the top line (y 0-30) and the status row (y 200-240). */
#define PILL_W 148
#define PILL_H 50

/* Pill position by offset from the next dose (-2..+2), clockwise from upper left */
static const struct {
    int offset;
    int32_t x, y;
} s_positions[] = {
    {0, 86, 90},    /* center: next */
    {-1, 164, 32},  /* upper right: most recently taken */
    {1, 164, 148},  /* lower right: after next */
    {2, 8, 148},    /* lower left */
    {-2, 8, 32},    /* upper left: taken two ago */
};

static void build_pill(lv_obj_t *scr, const dose_t *d, int32_t x, int32_t y)
{
    const uint32_t *c = s_pill_colors[d->state];
    lv_obj_t *pill = pill_base(scr, d, PILL_W, PILL_H);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_pad_hor(pill, 18, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *name = label(pill, d->name, &inter_20_semibold, c[1]);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    char buf[24];
    label(pill, slot_text(d, buf, sizeof(buf)), &inter_14_semibold, c[2]);
}

static void build_doses(lv_obj_t *scr)
{
    /* Next = first dose not yet taken; if all are taken, the cycle starts over at the first */
    int next = 0;
    for (int i = 0; i < DOSE_COUNT; i++) {
        if (s_doses[i].state != DOSE_TAKEN) {
            next = i;
            break;
        }
    }
    /* Fill center, then the nearest offsets, so fewer than five doses never repeat a pill */
    for (int p = 0; p < (int)(sizeof(s_positions) / sizeof(s_positions[0])) && p < DOSE_COUNT; p++) {
        int i = ((next + s_positions[p].offset) % DOSE_COUNT + DOSE_COUNT) % DOSE_COUNT;
        build_pill(scr, &s_doses[i], s_positions[p].x, s_positions[p].y);
    }
}
#else
/* Round screens: the day's pills inside a 168px circle with a TODAY/time header */
static void build_doses(lv_obj_t *scr)
{
    lv_obj_t *circle = plain_obj(scr);
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
    for (int i = 0; i < DOSE_COUNT; i++) {
        const dose_t *d = &s_doses[i];
        lv_obj_t *pill = pill_base(circle, d, 104, 20);
        lv_obj_set_style_pad_hor(pill, 9, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *name = label(pill, d->name, &inter_10_semibold, s_pill_colors[d->state][1]);
        lv_obj_set_flex_grow(name, 1);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char buf[24];
        label(pill, slot_text(d, buf, sizeof(buf)), &inter_9, s_pill_colors[d->state][2]);
    }
}
#endif

static void build_status_row(lv_obj_t *scr)
{
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, 320, 40);
    lv_obj_set_pos(row, 0, 200);
    flex_center(row, LV_FLEX_FLOW_ROW, 10);

    dot(row, 8, 0x22c55e);
    label(row, "3 nodes synced", &inter_11_semibold, 0x9ca3af);

    lv_obj_t *gear = plain_obj(row);
    lv_obj_set_size(gear, 36, 36);
    lv_obj_set_style_radius(gear, 10, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x171920), 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x2a2e38), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(0x2e333d), 0);
    flex_center(gear, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 4); /* 36px drawn, 44px touch target */
    lv_obj_add_event_cb(gear, settings_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *img = lv_image_create(gear);
    lv_image_set_src(img, &icon_settings);
    lv_obj_set_style_image_recolor(img, lv_color_hex(0xd1d5db), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

/* Date and time labels from the system clock; dashes until the first SNTP sync. */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int last_minute = -1;
    if (!time_is_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int minute = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
    if (minute == last_minute) {
        return;
    }
    last_minute = minute;

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

    build_top_line(scr);
    build_doses(scr);
    build_status_row(scr);

    lv_obj_t *ver = label(scr, fw_version_short(), &inter_8, 0x6b7280);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -8, -6);

    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

#endif
