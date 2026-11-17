/* Home screen, 320x240 landscape. Layout follows the "Living room — C5 2.8in (320×240 landscape)"
 * artboard in the "MedReminder Round Display UI" design: date + progress on top, dose pills in a
 * 168px circle, four 56px corner tiles, node status + settings gear at the bottom. */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_home.h"

#include <esp_log.h>

#include "fonts/fonts.h"
#include "icons/icons.h"

static const char *TAG = "ui_home";

typedef enum { DOSE_TAKEN, DOSE_NEXT, DOSE_LATER, DOSE_OVERDUE } dose_state_t;

typedef struct {
    const char *name;
    const char *slot;
    dose_state_t state;
} dose_t;

typedef struct {
    const char *id;
    const lv_image_dsc_t *icon;
    const char *state;
    uint32_t bg, border, icon_color, text_color;
    int32_t x, y;
} tile_t;

/* Placeholder data from the design artboard */
static const dose_t s_doses[] = {
    {"Evan", "AM", DOSE_TAKEN},
    {"Karen", "AM", DOSE_TAKEN},
    {"Karen", "MID", DOSE_NEXT},
    {"Karen", "PM", DOSE_LATER},
    {"Karen", "BED", DOSE_LATER},
};

static const tile_t s_tiles[] = {
    {"packages", &icon_package, "Pkgs", 0x6b21a8, 0x86198f, 0xe9d5ff, 0xffffff, 10, 40},
    {"garage", &icon_garage, "Open", 0x3a1a0a, 0x9a3412, 0xfdba74, 0xfb923c, 254, 40},
    {"couch_lights", &icon_bulb, "1 on", 0x1c1a10, 0xa16207, 0xfbbf24, 0xfbbf24, 10, 136},
    {"front_door", &icon_lock, "Locked", 0x052e1b, 0x15803d, 0x86efac, 0x4ade80, 254, 136},
};

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

static void tap_cb(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    ESP_LOGI(TAG, "%s %s", id, lv_event_get_code(e) == LV_EVENT_LONG_PRESSED ? "hold" : "tap");
}

static void make_tappable(lv_obj_t *o, const char *id, uint32_t bg)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, lv_color_lighten(lv_color_hex(bg), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_add_event_cb(o, tap_cb, LV_EVENT_CLICKED, (void *)id);
    lv_obj_add_event_cb(o, tap_cb, LV_EVENT_LONG_PRESSED, (void *)id);
}

static void build_top_line(lv_obj_t *scr)
{
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, 320, 30);
    lv_obj_set_pos(row, 0, 0);
    flex_center(row, LV_FLEX_FLOW_ROW, 8);

    label(row, "WED 11 NOV", &inter_12_bold, 0x9ca3af);
    label(row, "\u00b7", &inter_12_bold, 0x4b5563);
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
}

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void build_pill(lv_obj_t *parent, const dose_t *d)
{
    const uint32_t *c = s_pill_colors[d->state];
    lv_obj_t *pill = plain_obj(parent);
    lv_obj_set_size(pill, 104, 20);
    lv_obj_set_style_radius(pill, 10, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(c[0]), 0);
    lv_obj_set_style_pad_hor(pill, 9, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = label(pill, d->name, &inter_10_semibold, c[1]);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    const char *slot = d->slot;
    char buf[24];
    if (d->state == DOSE_TAKEN) {
        lv_snprintf(buf, sizeof(buf), "%s \u2713", d->slot);
        slot = buf;
    } else if (d->state == DOSE_NEXT) {
        lv_snprintf(buf, sizeof(buf), "%s \u00b7 next", d->slot);
        slot = buf;
    }
    label(pill, slot, &inter_9, c[2]);

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
}

static void build_dose_circle(lv_obj_t *scr)
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

    lv_obj_t *when = label(circle, "TODAY \u00b7 2:15 PM", &inter_8, 0x6b7280);
    lv_obj_set_style_text_letter_space(when, 1, 0);
    for (size_t i = 0; i < sizeof(s_doses) / sizeof(s_doses[0]); i++) {
        build_pill(circle, &s_doses[i]);
    }
}

static void build_tile(lv_obj_t *scr, const tile_t *t)
{
    lv_obj_t *tile = plain_obj(scr);
    lv_obj_set_size(tile, 56, 56);
    lv_obj_set_pos(tile, t->x, t->y);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(t->bg), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(t->border), 0);
    flex_center(tile, LV_FLEX_FLOW_COLUMN, 4);
    make_tappable(tile, t->id, t->bg);

    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, t->icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(t->icon_color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    label(tile, t->state, &inter_10_bold, t->text_color);
}

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
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(0x2e333d), 0);
    flex_center(gear, LV_FLEX_FLOW_COLUMN, 0);
    make_tappable(gear, "settings", 0x171920);
    lv_obj_set_ext_click_area(gear, 4); /* 36px drawn, 44px touch target */

    lv_obj_t *img = lv_image_create(gear);
    lv_image_set_src(img, &icon_settings);
    lv_obj_set_style_image_recolor(img, lv_color_hex(0xd1d5db), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

void ui_home_create(lv_obj_t *scr)
{
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0c0f), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_top_line(scr);
    build_dose_circle(scr);
    for (size_t i = 0; i < sizeof(s_tiles) / sizeof(s_tiles[0]); i++) {
        build_tile(scr, &s_tiles[i]);
    }
    build_status_row(scr);
}

#endif
