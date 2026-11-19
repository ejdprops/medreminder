#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_dose_row.h"

#include "doses.h"
#include "fonts/fonts.h"

#define OVERDUE_BORDER_W 2
#define OVERDUE_BORDER 0xef4444
#define NAME_TEXT 0xffffff /* on dark colors */
#define SLOT_TEXT 0xe5e7eb
#define DARK_NAME_TEXT 0x111827 /* on light colors (yellow, pale sky) */
#define DARK_SLOT_TEXT 0x374151

static uint32_t dose_color(int i)
{
    return dose_colors[dose_def(i)->color];
}

static bool dark_text(int i)
{
    uint32_t c = dose_color(i);
    int luma = (299 * ((c >> 16) & 0xff) + 587 * ((c >> 8) & 0xff) + 114 * (c & 0xff)) / 1000;
    return luma > 150;
}

uint32_t ui_dose_name_color(int i)
{
    return dark_text(i) ? DARK_NAME_TEXT : NAME_TEXT;
}

uint32_t ui_dose_slot_color(int i)
{
    return dark_text(i) ? DARK_SLOT_TEXT : SLOT_TEXT;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

/* Period, plus the weekday for a weekly dose ("PM · TUE"); live: "· next" / "· late" for the next
 * dose, "✓" for a taken one */
const char *ui_dose_slot_text(int i, bool live, bool next, char *buf, size_t len)
{
    static const char *const wdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    uint8_t days = dose_def(i)->days;
    int n = lv_snprintf(buf, len, "%s", dose_slot(i));
    for (int k = 0; k < 7; k++) {
        if (days == (1u << k)) {
            n += lv_snprintf(buf + n, len - n, " · %s", wdays[k]);
        }
    }
    if (!live) {
        return buf;
    }
    dose_state_t st = dose_state(i);
    if (st == DOSE_TAKEN) {
        lv_snprintf(buf + n, len - n, " ✓");
    } else if (st == DOSE_NEXT) {
        lv_snprintf(buf + n, len - n, " · next");
    } else if (st == DOSE_OVERDUE && next) {
        lv_snprintf(buf + n, len - n, " · late");
    }
    return buf;
}

void ui_dose_pulse(lv_obj_t *obj)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, pulse_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, 184); /* 1 -> 0.72 opacity, 1.4s cycle */
    lv_anim_set_duration(&a, 700);
    lv_anim_set_reverse_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

lv_obj_t *ui_dose_pill(lv_obj_t *parent, int i, int32_t w, int32_t h, bool live)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pill, w, h);
    lv_obj_set_style_radius(pill, h / 2, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(dose_color(i)), 0);
    lv_obj_set_style_bg_color(pill, lv_color_lighten(lv_color_hex(dose_color(i)), LV_OPA_20), LV_STATE_PRESSED);
    if (live && dose_state(i) == DOSE_OVERDUE) {
        lv_obj_set_style_border_width(pill, OVERDUE_BORDER_W, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(OVERDUE_BORDER), 0);
        ui_dose_pulse(pill);
    }
    return pill;
}

/* Doses list row: time, name, weekday for a weekly dose; 14px */
#define MAIN_FONT (&inter_14_semibold)
#define ROW_PAD_H 14
#define ROW_GAP 10

static lv_obj_t *one_line(lv_obj_t *l)
{
    lv_obj_set_width(l, LV_SIZE_CONTENT); /* never wrap */
    return l;
}

lv_obj_t *ui_dose_row(lv_obj_t *parent, int i)
{
    static const char *const wdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    lv_obj_t *row = ui_dose_pill(parent, i, LV_PCT(100), UI_DOSE_ROW_H, false);
    lv_obj_set_style_pad_hor(row, ROW_PAD_H, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ROW_GAP, 0);

    char buf[8];
    dose_format_time(dose_def(i)->minute, buf, sizeof(buf));
    one_line(label(row, buf, MAIN_FONT, ui_dose_name_color(i)));
    /* The name takes what's left and ends in "..." if it doesn't fit; the fixed one-line height
     * keeps it from wrapping */
    lv_obj_t *name = label(row, dose_name(i), MAIN_FONT, ui_dose_name_color(i));
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_height(name, lv_font_get_line_height(MAIN_FONT));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    uint8_t days = dose_def(i)->days;
    for (int k = 0; k < 7; k++) {
        if (days == (1u << k)) {
            one_line(label(row, wdays[k], MAIN_FONT, ui_dose_slot_color(i)));
        }
    }
    return row;
}

/* Home list row: time, name, check when taken; 13px, no period (the time says it) */
#define LIST_FONT (&inter_13_semibold)

lv_obj_t *ui_dose_list_row(lv_obj_t *parent, int i)
{
    bool taken = dose_state(i) == DOSE_TAKEN;
    lv_obj_t *row = ui_dose_pill(parent, i, LV_PCT(100), UI_DOSE_LIST_ROW_H, true);
    lv_obj_set_flex_grow(row, 1); /* fill the list when it shows fewer rows */
    lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(row, 9, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    uint32_t text = ui_dose_name_color(i);

    char buf[8];
    dose_format_time(dose_def(i)->minute, buf, sizeof(buf));
    one_line(label(row, buf, LIST_FONT, text));
    lv_obj_t *name = label(row, dose_name(i), LIST_FONT, text);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_height(name, lv_font_get_line_height(LIST_FONT));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    if (taken) {
        one_line(label(row, "\u2713", LIST_FONT, text));
    }
    return row;
}

#endif
