/* Dose schedule screens, opened from Settings > Doses.
 *   List: every dose in time order (time, name, period), scrollable; tap a row to edit it,
 *     "+ Add" for a new one.
 *   Edit: name (on-screen keyboard), period (AM/MID/PM/BED), time (hour, minute in 5 min steps,
 *     24 h), repeat (Daily or one weekday), color (12 swatches); Save, Delete (tap twice) for an existing dose, Back
 *     cancels.
 * Validation (one time per person) lives in doses_set(); its message shows above the Delete
 * button. The home screen redraws on its next tick (doses_tick reports schedule changes). */
#if __has_include("lvgl.h") /* boards without a display build no UI */
#include "ui_doses.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>

#include "audio.h"
#include "doses.h"
#include "fonts/fonts.h"
#include "icons/icons.h"
#include "ui_dose_row.h"
#include "ui_scroll.h"
#include "ui_tap.h"
#include "ui_theme.h"

static const char *TAG = "ui_doses";

#define BG_COLOR (UI_T->bg)
#define CARD_COLOR UI_T->btn_bg
#define CARD_PRESSED UI_T->btn_pressed
#define CARD_BORDER UI_T->btn_border
#define SELECTED_COLOR 0x1d4ed8

/* List screen */
static lv_obj_t *s_list_screen;
static lv_obj_t *s_list;
static lv_obj_t *s_count_label;
static lv_obj_t *s_return_to;

/* Edit screen (built per edit, deleted on leaving) */
static lv_obj_t *s_edit_screen;
static int s_edit_index; /* -1: adding */
static int s_period;
static lv_obj_t *s_name_ta;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_period_btns[DOSE_PERIOD_COUNT];
static int s_color;
static lv_obj_t *s_sample, *s_sample_label;
static lv_obj_t *s_swatches[DOSE_COLOR_COUNT];
static lv_obj_t *s_hour_roller, *s_minute_roller, *s_day_roller;
static lv_obj_t *s_error_label;
static lv_obj_t *s_delete_label;
static bool s_delete_armed;

static void build_rows(void);

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

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

/* Rounded card button like the Settings header buttons */
static lv_obj_t *button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t border)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_ext_click_area(b, 4);
    return b;
}

static void header(lv_obj_t *scr, const char *title, ui_tap_cb_t back_cb)
{
    lv_obj_t *back = button(scr, 8, 6, 36, 36, CARD_BORDER);
    ui_on_tap(back, back_cb, NULL);
    lv_obj_t *chev = lv_image_create(back);
    lv_image_set_src(chev, &icon_chevron_left);
    lv_obj_set_style_image_recolor(chev, lv_color_hex(UI_T->text2), 0);
    lv_obj_set_style_image_recolor_opa(chev, LV_OPA_COVER, 0);
    lv_obj_center(chev);
    lv_obj_align(label(scr, title, &inter_12_bold, UI_T->text), LV_ALIGN_TOP_LEFT, 54, 17);
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t x, int32_t y)
{
    lv_obj_t *c = label(parent, text, &inter_8, UI_T->caption);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_pos(c, x, y);
    return c;
}

/* ---- Edit screen ---- */

static void close_edit(void)
{
    build_rows();
    ui_scroll_gutter_update(s_list);
    lv_screen_load(s_list_screen);
    lv_obj_delete_async(s_edit_screen); /* we may be inside one of its events */
    s_edit_screen = NULL;
}

static void edit_back_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    close_edit();
    audio_play(AUDIO_CLICK);
}

static void show_error(const char *msg)
{
    lv_label_set_text(s_error_label, msg);
    lv_obj_remove_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);
}

static void save_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    dose_def_t d = {0};
    /* Name without leading/trailing spaces */
    const char *txt = lv_textarea_get_text(s_name_ta);
    while (*txt == ' ') {
        txt++;
    }
    strlcpy(d.name, txt, sizeof(d.name));
    for (int n = strlen(d.name); n > 0 && d.name[n - 1] == ' '; n--) {
        d.name[n - 1] = '\0';
    }
    strlcpy(d.period, dose_periods[s_period], sizeof(d.period));
    d.color = (uint8_t)s_color;
    d.opacity = 100; /* rows are solid; the field is kept for the saved layout */
    d.minute = lv_roller_get_selected(s_hour_roller) * 60 + lv_roller_get_selected(s_minute_roller) * 5;
    int day = lv_roller_get_selected(s_day_roller); /* 0 = Daily, 1..7 = Sun..Sat */
    d.days = day == 0 ? DOSE_DAILY : (uint8_t)(1u << (day - 1));

    const char *err = doses_set(s_edit_index, &d);
    if (err) {
        show_error(err);
        return;
    }
    close_edit();
    audio_play(AUDIO_CLICK);
}

static void delete_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    if (!s_delete_armed) {
        s_delete_armed = true; /* a second tap deletes */
        lv_label_set_text(s_delete_label, "Confirm");
        return;
    }
    doses_delete(s_edit_index);
    close_edit();
    audio_play(AUDIO_CLICK);
}

/* Header sample: how the dose's row looks on the home screen */
static void update_sample(void)
{
    if (!s_sample) {
        return;
    }
    uint32_t c = dose_colors[s_color];
    int luma = (299 * ((c >> 16) & 0xff) + 587 * ((c >> 8) & 0xff) + 114 * (c & 0xff)) / 1000;
    lv_obj_set_style_bg_color(s_sample, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(s_sample, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_sample_label, lv_color_hex(luma > 150 ? 0x111827 : 0xffffff), 0);
    const char *name = lv_textarea_get_text(s_name_ta);
    lv_label_set_text_fmt(s_sample_label, "%s \u00b7 %s", name[0] ? name : "Name", dose_periods[s_period]);
}

static void style_period_btns(void)
{
    for (int p = 0; p < DOSE_PERIOD_COUNT; p++) {
        bool sel = p == s_period;
        lv_obj_set_style_bg_color(s_period_btns[p], lv_color_hex(sel ? SELECTED_COLOR : CARD_COLOR), 0);
        lv_obj_set_style_border_color(s_period_btns[p], lv_color_hex(sel ? 0x3b82f6 : CARD_BORDER), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_period_btns[p], 0), lv_color_hex(sel ? 0xffffff : UI_T->text), 0);
    }
}

static void period_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    s_period = (int)(intptr_t)user_data;
    style_period_btns();
    update_sample();
}

static void style_swatches(void)
{
    for (int k = 0; k < DOSE_COLOR_COUNT; k++) {
        lv_obj_set_style_border_width(s_swatches[k], dose_color_order[k] == s_color ? 2 : 0, 0);
    }
}

static void color_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    s_color = (int)(intptr_t)user_data;
    style_swatches();
    update_sample();
}

static void keyboard_hide(void)
{
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_name_ta, LV_STATE_FOCUSED);
}

static void name_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        update_sample();
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_obj_add_state(s_name_ta, LV_STATE_FOCUSED);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        keyboard_hide();
    }
}

static lv_obj_t *roller(lv_obj_t *parent, const char *options, int32_t x, int32_t w, uint32_t selected)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(r, &inter_16_semibold, 0);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_set_pos(r, x, 116);
    lv_obj_update_layout(r);
    lv_obj_set_height(r, lv_obj_get_height(r) * 9 / 10); /* 3 rows, 10% tighter */
    lv_obj_set_style_text_color(r, lv_color_hex(UI_T->caption), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_text_font(r, &inter_16_semibold, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(0xffffff), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, lv_color_hex(SELECTED_COLOR), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_roller_set_selected(r, selected, LV_ANIM_OFF);
    return r;
}

static void open_edit(int index)
{
    s_edit_index = index;
    s_delete_armed = false;
    dose_def_t d = {.name = "", .period = "AM", .minute = 8 * 60, .color = 0, .opacity = 100, .days = DOSE_DAILY};
    if (index >= 0) {
        d = *dose_def(index);
    }
    s_color = d.color < DOSE_COLOR_COUNT ? d.color : 0;
    s_sample = NULL;
    s_period = 0;
    for (int p = 0; p < DOSE_PERIOD_COUNT; p++) {
        if (strcmp(d.period, dose_periods[p]) == 0) {
            s_period = p;
        }
    }

    s_edit_screen = new_screen();
    lv_obj_t *scr = s_edit_screen;
    header(scr, index >= 0 ? "Edit dose" : "Add dose", edit_back_cb);

    lv_obj_t *save = button(scr, 247, 8, 65, 32, UI_T->ok);
    ui_on_tap(save, save_cb, NULL);
    lv_obj_center(label(save, "Save", &inter_11_semibold, UI_T->ok));

    /* Name */
    caption(scr, "NAME", 12, 50);
    s_name_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, DOSE_NAME_MAX);
    lv_textarea_set_placeholder_text(s_name_ta, "Name");
    lv_textarea_set_text(s_name_ta, d.name);
    lv_obj_set_size(s_name_ta, 140, 34);
    lv_obj_set_pos(s_name_ta, 12, 62);
    lv_obj_set_style_text_font(s_name_ta, &inter_12_semibold, 0);
    lv_obj_set_style_text_color(s_name_ta, lv_color_hex(UI_T->text), 0);
    lv_obj_set_style_bg_color(s_name_ta, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(CARD_BORDER), 0);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(0x3b82f6), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_name_ta, 10, 0);
    /* 34px box = 2 border + 2x8 pad + 15px line; any taller and the one-line text area scrolls by
     * a pixel and jitters as the cursor blinks */
    lv_obj_set_style_pad_ver(s_name_ta, 8, 0);
    lv_obj_set_style_border_width(s_name_ta, 1, 0);
    lv_obj_add_event_cb(s_name_ta, name_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_name_ta, name_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Period */
    caption(scr, "PERIOD", 164, 50);
    for (int p = 0; p < DOSE_PERIOD_COUNT; p++) {
        lv_obj_t *b = button(scr, 164 + p * 38, 62, 34, 34, CARD_BORDER);
        lv_obj_set_ext_click_area(b, 2);
        ui_on_tap(b, period_cb, (void *)(intptr_t)p);
        lv_obj_center(label(b, dose_periods[p], &inter_10_semibold, UI_T->text));
        s_period_btns[p] = b;
    }
    style_period_btns();

    /* Time */
    caption(scr, "TIME", 12, 104);
    caption(scr, "REPEAT", 116, 104);
    s_hour_roller = roller(scr, "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23", 12, 46, d.minute / 60);
    s_minute_roller = roller(scr, "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55", 64, 46, (d.minute % 60) / 5);
    int day = 0; /* Daily, unless exactly one weekday */
    for (int k = 0; k < 7; k++) {
        if (d.days == (1u << k)) {
            day = k + 1;
        }
    }
    s_day_roller = roller(scr, "Daily\nSun\nMon\nTue\nWed\nThu\nFri\nSat", 116, 64, day);

    /* Color: three rows of four swatches in spectrum order, the selected one ringed in white */
    caption(scr, "COLOR", 218, 104);
    for (int k = 0; k < DOSE_COLOR_COUNT; k++) {
        int c = dose_color_order[k];
        lv_obj_t *sw = plain_obj(scr);
        lv_obj_set_size(sw, 20, 20);
        lv_obj_set_pos(sw, 218 + (k % 4) * 24, 116 + (k / 4) * 24);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(dose_colors[c]), 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(UI_T->text), 0);
        lv_obj_set_ext_click_area(sw, 2);
        ui_on_tap(sw, color_cb, (void *)(intptr_t)c);
        s_swatches[k] = sw;
    }
    style_swatches();

    /* Errors from doses_set: over the title, between Back and Save */
    s_error_label = label(scr, "", &inter_11_semibold, UI_T->error);
    lv_obj_set_size(s_error_label, 182, 36);
    lv_label_set_long_mode(s_error_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_error_label, 52, 6);
    lv_obj_set_style_bg_color(s_error_label, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_error_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(s_error_label, 4, 0);
    lv_obj_add_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);

    if (index >= 0) {
        lv_obj_t *del = button(scr, 247, 200, 65, 32, UI_T->del_border);
        ui_on_tap(del, delete_cb, NULL);
        s_delete_label = label(del, "Delete", &inter_11_semibold, UI_T->del_text);
        lv_obj_center(s_delete_label);
    }

    /* Sample of the pending row, centered between the title and Save */
    s_sample = plain_obj(scr);
    lv_obj_set_size(s_sample, 112, 26);
    lv_obj_set_pos(s_sample, 128, 11);
    lv_obj_set_style_radius(s_sample, 13, 0);
    lv_obj_set_style_pad_hor(s_sample, 10, 0);
    s_sample_label = label(s_sample, "", &inter_11_semibold, UI_T->text);
    lv_obj_set_width(s_sample_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_sample_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sample_label, LV_LABEL_LONG_DOT);
    lv_obj_center(s_sample_label);
    update_sample();
    lv_obj_move_foreground(s_error_label); /* errors still cover the header */

    /* Keyboard over the lower half while the name is edited */
    s_keyboard = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(s_keyboard, s_name_ta);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(UI_T->card), 0);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(UI_T->btn_bg), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(UI_T->btn_pressed), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(UI_T->btn_border), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, lv_color_hex(UI_T->text), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(UI_T->btn_border), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_name_ta, lv_color_hex(UI_T->caption), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(UI_T->text), LV_PART_CURSOR);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (index < 0) { /* new dose: start with the name */
        lv_obj_add_state(s_name_ta, LV_STATE_FOCUSED);
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }

    lv_screen_load(scr);
    ESP_LOGI(TAG, "edit %d", index);
}

/* ---- List screen ---- */

static void list_back_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    lv_screen_load(s_return_to);
    audio_play(AUDIO_CLICK);
}

static void add_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    open_edit(-1);
    audio_play(AUDIO_CLICK);
}

static void row_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    open_edit((int)(intptr_t)user_data);
    audio_play(AUDIO_CLICK);
}

static void build_rows(void)
{
    lv_obj_clean(s_list);
    int count = doses_count();
    lv_label_set_text_fmt(s_count_label, "%d of %d", count, DOSE_MAX);
    if (count == 0) {
        label(s_list, "No doses. Tap + Add.", &inter_11_semibold, UI_T->caption);
        return;
    }
    for (int i = 0; i < count; i++) {
        /* The row exactly as on the home screen, without today's state */
        lv_obj_t *row = ui_dose_row(s_list, i, false, false);
        ui_on_tap(row, row_cb, (void *)(intptr_t)i);
    }
}

static void build_list(void)
{
    s_list_screen = new_screen();
    header(s_list_screen, "Doses", list_back_cb);
    s_count_label = label(s_list_screen, "", &inter_9, UI_T->caption);
    lv_obj_align(s_count_label, LV_ALIGN_TOP_LEFT, 100, 19);

    lv_obj_t *add = button(s_list_screen, 240, 6, 72, 36, CARD_BORDER);
    ui_on_tap(add, add_cb, NULL);
    lv_obj_center(label(add, "+ Add", &inter_11_semibold, UI_T->text2));

    /* Scrollable list below the header */
    s_list = plain_obj(s_list_screen);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_size(s_list, 320, 192);
    lv_obj_set_pos(s_list, 0, 48);
    lv_obj_set_style_pad_left(s_list, 8, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    ui_scroll_gutter(s_list); /* drag strip + scrollbar on the right */
}

void ui_doses_invalidate(void)
{
    if (s_list_screen && s_list_screen != lv_screen_active()) {
        lv_obj_delete(s_list_screen);
        s_list_screen = NULL;
    }
}

void ui_doses_open(void)
{
    if (!s_list_screen) {
        build_list();
    }
    s_return_to = lv_screen_active();
    build_rows();
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    ui_scroll_gutter_update(s_list);
    lv_screen_load(s_list_screen);
    ESP_LOGI(TAG, "open");
}

#endif
