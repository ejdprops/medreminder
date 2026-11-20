/* Home screen, 320x240 landscape, per CONFIG_MEDREMINDER_UI_LAYOUT:
 *   RECT (C5 board; design artboards "B (menu)"): a centered date · time banner on top with the
 *     menu button (Doses, Settings, About) in the top-right corner. Below it on the left, today's
 *     doses as a compact list (display only; hold a taken row 1 s to undo it), with up/down arrows
 *     to page it when more than 5 are due; fewer rows grow to fill the height (otherwise the list takes their space); on the right,
 *     the next dose as a big card, the same height as the list: the one tap target, it marks the
 *     dose taken. For 10 s after a take an Undo strip sits under the card. Node sync status
 *     (node_sync.c) and the version centered along the bottom.
 *   ROUND: ui_round.c. */
#include <sdkconfig.h>
#if __has_include("lvgl.h") && CONFIG_MEDREMINDER_UI_LAYOUT_RECT /* boards without a display build no UI */
#include "ui_home.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>

#include "audio.h"
#include "doses.h"
#include "fonts/fonts.h"
#include "fw_version.h"
#include "icons/icons.h"
#include "node_sync.h"
#include "time_sync.h"
#include "ui_about.h"
#include "ui_dose_row.h"
#include "ui_doses.h"
#include "ui_tap.h"
#include "ui_theme.h"
#include "ui_settings.h"

static const char *TAG = "ui_home";

static lv_obj_t *s_date_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_dose_area; /* everything that shows doses; rebuilt when they change */
static int s_last_minute = -1; /* clock labels last drawn for this minute */

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

/* Rounded gray button with a centered icon, like the Settings header buttons */
static lv_obj_t *icon_button(lv_obj_t *parent, const lv_image_dsc_t *icon, int32_t x, int32_t y, int32_t w,
                             int32_t h)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_T->btn_bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_T->btn_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    flex_center(b, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_t *img = lv_image_create(b);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(UI_T->text2), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return b;
}

static void build_doses(lv_obj_t *area);

static void rebuild_doses(void *arg)
{
    (void)arg;
    lv_obj_clean(s_dose_area);
    build_doses(s_dose_area);
}

#define ARROW_X 6
#define ARROW_W 28
#define LIST_X 38
#define BANNER_H 40 /* date · time, centered on the menu button */
#define LIST_Y 42
#define LIST_W 145
#define LIST_ROWS 5 /* 5 x 28px rows + 4 x 4px gaps = 156px, to y 198; fewer rows grow to fill it */
#define LIST_GAP 4
#define LIST_H (LIST_ROWS * UI_DOSE_LIST_ROW_H + (LIST_ROWS - 1) * LIST_GAP)
#define COL_X 189 /* right column: the card */
#define COL_W 123
#define CARD_Y LIST_Y
#define CARD_H LIST_H /* level with the list, above the sync status */
#define UNDO_H 56
#define UNDO_GAP 4
#define UNDO_MS 10000
#define UNDO_AMBER 0xfbbf24
#define UNDO_AMBER_TEXT 0x111827
#define OVERDUE_BORDER 0xef4444
#define HOLD_UNDO_MS 1000

static int s_today[DOSE_MAX]; /* indices of the doses due today, in time order */
static int s_today_n;
static int s_top; /* first row shown in the list */
static lv_obj_t *s_list, *s_up, *s_down;
static uint8_t s_undo_uid; /* last dose taken on the card, while its Undo strip shows; 0 = none */
static uint32_t s_undo_at;
static lv_timer_t *s_undo_timer;
static lv_obj_t *s_late_label; /* "Late 12 min" on the card, updated every minute */
static int s_card_dose = -1;
static lv_timer_t *s_hold_timer;
static lv_obj_t *s_menu;
static lv_obj_t *s_confirm;

static void collect_today(void)
{
    s_today_n = 0;
    for (int i = 0; i < doses_count(); i++) {
        if (dose_due_today(i)) {
            s_today[s_today_n++] = i;
        }
    }
}

static int max_top(void)
{
    return s_today_n > LIST_ROWS ? s_today_n - LIST_ROWS : 0;
}

/* Page the list so the next dose shows, two rows down when there are rows above it; with none
 * left, the end of the day */
static void snap_to_next(void)
{
    int next = doses_next();
    int pos = s_today_n;
    for (int k = 0; k < s_today_n; k++) {
        if (s_today[k] == next) {
            pos = k;
        }
    }
    s_top = LV_CLAMP(0, pos - 2, max_top());
}

static void refresh(bool snap)
{
    collect_today();
    if (snap) {
        snap_to_next();
    }
    rebuild_doses(NULL);
}

static void refresh_snap_async(void *arg)
{
    (void)arg;
    refresh(true);
}

/* ---- Undo: the strip under the card, and holding a taken row ---- */

static void undo_expire_cb(lv_timer_t *t)
{
    (void)t;
    s_undo_timer = NULL; /* one-shot: LVGL deletes it */
    s_undo_uid = 0;
    refresh(false);
}

static void undo_cancel(void)
{
    s_undo_uid = 0;
    if (s_undo_timer) {
        lv_timer_delete(s_undo_timer);
        s_undo_timer = NULL;
    }
}

static void untake_uid(uint8_t uid)
{
    int i = doses_find_uid(uid);
    if (i >= 0 && doses_untake(i)) {
        audio_play(AUDIO_CLICK);
    }
}

static void undo_strip_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    uint8_t uid = s_undo_uid;
    undo_cancel();
    untake_uid(uid);
    lv_async_call(refresh_snap_async, NULL); /* not from inside the strip's own event */
}

static void bar_width_exec(void *obj, int32_t v)
{
    lv_obj_set_width(obj, v);
}

static void build_undo_strip(lv_obj_t *area, int i)
{
    lv_obj_t *strip = plain_obj(area);
    lv_obj_set_size(strip, COL_W, UNDO_H);
    lv_obj_set_pos(strip, COL_X, CARD_Y + CARD_H - UNDO_H);
    lv_obj_set_style_radius(strip, 14, 0);
    lv_obj_set_style_clip_corner(strip, true, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(UI_T->text), 0); /* inverted: dark on light */
    lv_obj_set_style_pad_top(strip, 6, 0);
    lv_obj_set_style_pad_hor(strip, 8, 0);
    lv_obj_set_style_pad_bottom(strip, 9, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(strip, 4, 0);
    ui_on_tap(strip, undo_strip_cb, NULL);

    char what[40], t[8];
    dose_format_time(dose_def(i)->minute, t, sizeof(t));
    snprintf(what, sizeof(what), "✓ %s %s", dose_name(i), t);
    lv_obj_t *l = label(strip, what, &inter_12_semibold, UI_T->bg);
    lv_obj_set_size(l, LV_PCT(100), lv_font_get_line_height(&inter_12_semibold));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

    lv_obj_t *btn = plain_obj(strip);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UNDO_AMBER), 0);
    lv_obj_center(label(btn, "Undo", &inter_14_semibold, UNDO_AMBER_TEXT));

    /* Countdown along the bottom edge, shrinking over what's left of the 10 s */
    uint32_t left = UNDO_MS - LV_MIN(lv_tick_elaps(s_undo_at), UNDO_MS);
    lv_obj_t *bar = plain_obj(strip);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(bar, COL_W * left / UNDO_MS, 3);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UNDO_AMBER), 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, -8, 9); /* past the strip's padding, to its edge */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, bar_width_exec);
    lv_anim_set_values(&a, COL_W * left / UNDO_MS, 0);
    lv_anim_set_duration(&a, left);
    lv_anim_start(&a);
}

static void confirm_close(void)
{
    if (s_confirm) {
        lv_obj_delete_async(s_confirm);
        s_confirm = NULL;
    }
}

static void confirm_keep_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    confirm_close();
}

static void confirm_undo_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    uint8_t uid = (uint8_t)(uintptr_t)user_data;
    confirm_close();
    if (uid == s_undo_uid) {
        undo_cancel();
    }
    untake_uid(uid);
    lv_async_call(refresh_snap_async, NULL);
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t border, uint32_t fg)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_size(b, 100, 40);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_darken(lv_color_hex(bg), LV_OPA_10), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_center(label(b, text, &inter_14_semibold, fg));
    return b;
}

/* "Mark Karen 1330 as not taken?" [Keep] [Undo], over everything */
static void confirm_open(int i)
{
    confirm_close();
    s_confirm = plain_obj(lv_layer_top());
    lv_obj_set_size(s_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE); /* taps outside the panel go nowhere */

    lv_obj_t *panel = plain_obj(s_confirm);
    lv_obj_set_size(panel, 236, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_T->bg), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 12, 0);

    char text[64], t[8];
    dose_format_time(dose_def(i)->minute, t, sizeof(t));
    snprintf(text, sizeof(text), "Mark %s %s\nas not taken?", dose_name(i), t);
    lv_obj_t *l = label(panel, text, &inter_14_semibold, UI_T->text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    ui_on_tap(dialog_button(panel, "Keep", UI_T->btn_bg, UI_T->btn_border, UI_T->text2), confirm_keep_cb, NULL);
    ui_on_tap(dialog_button(panel, "Undo", UNDO_AMBER, UNDO_AMBER, UNDO_AMBER_TEXT), confirm_undo_cb,
              (void *)(uintptr_t)dose_def(i)->uid);
    ESP_LOGI(TAG, "undo? %s %s", dose_name(i), t);
}

static void hold_timer_cb(lv_timer_t *t)
{
    s_hold_timer = NULL; /* one-shot */
    confirm_open((int)(intptr_t)lv_timer_get_user_data(t));
}

static void hold_cancel(void)
{
    if (s_hold_timer) {
        lv_timer_delete(s_hold_timer);
        s_hold_timer = NULL;
    }
}

/* Taken rows: held HOLD_UNDO_MS, ask whether to undo */
static void hold_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        hold_cancel();
        s_hold_timer = lv_timer_create(hold_timer_cb, HOLD_UNDO_MS, lv_event_get_user_data(e));
        lv_timer_set_repeat_count(s_hold_timer, 1);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_DELETE:
        hold_cancel();
        break;
    default:
        break;
    }
}

/* ---- The list ---- */

/* Up to LIST_ROWS doses: no arrows, the list takes their space */
static bool need_arrows(void)
{
    return s_today_n > LIST_ROWS;
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    for (int k = s_top; k < s_today_n && k < s_top + LIST_ROWS; k++) {
        int i = s_today[k];
        lv_obj_t *row = ui_dose_list_row(s_list, i);
        if (dose_state(i) == DOSE_TAKEN) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, hold_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        }
    }
    lv_obj_set_state(s_up, LV_STATE_DISABLED, s_top == 0);
    lv_obj_set_state(s_down, LV_STATE_DISABLED, s_top >= max_top());
}

static void arrow_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    int step = (int)(intptr_t)user_data * (LIST_ROWS - 1); /* the last row seen stays on screen */
    int top = LV_CLAMP(0, s_top + step, max_top());
    if (top == s_top) {
        return;
    }
    s_top = top;
    hold_cancel();
    fill_list();
    audio_play(AUDIO_CLICK);
}

/* ---- The card ---- */

static void take_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    int i = (int)(intptr_t)user_data;
    if (!doses_take(i)) {
        return;
    }
    audio_play(AUDIO_CLICK);
    undo_cancel();
    s_undo_uid = dose_def(i)->uid;
    s_undo_at = lv_tick_get();
    s_undo_timer = lv_timer_create(undo_expire_cb, UNDO_MS, NULL);
    lv_timer_set_repeat_count(s_undo_timer, 1);
    lv_async_call(refresh_snap_async, NULL); /* not from inside the card's own event */
}

static void update_late_label(void)
{
    if (!s_late_label || s_card_dose < 0 || !time_is_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int late = LV_MAX(0, tm.tm_hour * 60 + tm.tm_min - dose_def(s_card_dose)->minute);
    if (late < 60) {
        lv_label_set_text_fmt(s_late_label, "Late %d min", late);
    } else {
        lv_label_set_text_fmt(s_late_label, "Late %dh %02dm", late / 60, late % 60);
    }
}

/* Name in 23px: up to max_lines lines, then "..." */
static void card_name(lv_obj_t *card, int i, uint32_t color, int max_lines)
{
    const lv_font_t *font = &inter_23_semibold;
    lv_obj_t *name = label(card, dose_name(i), font, color);
    int32_t w = COL_W - 2 * 10;
    lv_point_t size;
    lv_text_get_size(&size, dose_name(i), font, 0, 0, w, LV_TEXT_FLAG_NONE);
    lv_obj_set_size(name, w, LV_MIN(size.y, max_lines * lv_font_get_line_height(font)));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
}

/* Nothing left today: gray card, ignores taps; tomorrow's first dose */
static void build_done_card(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_T->btn_bg), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, &icon_check_circle);
    lv_obj_set_style_image_recolor(img, lv_color_hex(UI_T->ok), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_t *big = label(card, s_today_n ? "All done\ntoday" : "No doses\ntoday", &inter_16_bold, UI_T->text);
    lv_obj_set_flex_grow(big, 1);
    lv_obj_set_style_margin_top(big, 8, 0);

    if (!time_is_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int first = doses_first_on((tm.tm_wday + 1) % 7);
    if (first >= 0) {
        char text[48], t[8];
        dose_format_time(dose_def(first)->minute, t, sizeof(t));
        snprintf(text, sizeof(text), "Tomorrow first:\n%s %s", t, dose_name(first));
        lv_obj_t *l = label(card, text, &inter_12_semibold, UI_T->muted);
        lv_obj_set_width(l, LV_PCT(100));
    }
}

static void build_card(lv_obj_t *area, bool undo)
{
    lv_obj_t *card = plain_obj(area);
    lv_obj_set_size(card, COL_W, undo ? CARD_H - UNDO_H - UNDO_GAP : CARD_H);
    lv_obj_set_pos(card, COL_X, CARD_Y);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    int i = doses_next();
    s_card_dose = i;
    s_late_label = NULL;
    if (i < 0) {
        build_done_card(card);
        return;
    }
    uint32_t color = dose_colors[dose_def(i)->color];
    uint32_t text = ui_dose_name_color(i);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(card, lv_color_lighten(lv_color_hex(color), LV_OPA_20), LV_STATE_PRESSED);
    bool overdue = dose_state(i) == DOSE_OVERDUE;
    if (overdue) {
        lv_obj_set_style_border_width(card, 3, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(OVERDUE_BORDER), 0);
        lv_obj_set_style_pad_all(card, 7, 0);
        ui_dose_pulse(card);
    }
    ui_on_tap(card, take_cb, (void *)(intptr_t)i);

    char t[8];
    dose_format_time(dose_def(i)->minute, t, sizeof(t));
    label(card, t, &inter_16_bold, text);

    /* Name (and the late badge) between the time and the hint */
    lv_obj_t *mid = plain_obj(card);
    lv_obj_set_size(mid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 4, 0);
    card_name(mid, i, text, undo ? 1 : 2);
    if (overdue) {
        lv_obj_t *badge = plain_obj(mid);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
        lv_obj_set_style_pad_hor(badge, 6, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        s_late_label = label(badge, "Late", &inter_12_bold, 0xb91c1c);
        update_late_label();
    }

    if (undo && overdue) {
        return; /* no room for the hint above the Undo strip with the late badge */
    }
    const lv_font_t *hint_font = undo ? &inter_12_semibold : &inter_14_semibold;
    lv_obj_t *hint = label(card, "Tap when taken", hint_font, text);
    lv_obj_set_width(hint, LV_PCT(100));
}

/* ---- Menu (Doses, Settings, About) ---- */

static void menu_close(void)
{
    if (s_menu) {
        lv_obj_delete_async(s_menu); /* may be inside one of its children's events */
        s_menu = NULL;
    }
}

static void menu_close_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    menu_close();
    audio_play(AUDIO_CLICK);
}

static void menu_item_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    menu_close();
    switch ((int)(intptr_t)user_data) {
    case 0:
        ui_doses_open();
        break;
    case 1:
        ui_settings_open();
        break;
    default:
        ui_about_open();
        break;
    }
    audio_play(AUDIO_CLICK);
}

static void menu_item(lv_obj_t *panel, const lv_image_dsc_t *icon, const char *text, int id, bool first)
{
    lv_obj_t *item = plain_obj(panel);
    lv_obj_set_size(item, LV_PCT(100), 44);
    lv_obj_set_style_pad_hor(item, 12, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 10, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(UI_T->btn_pressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_STATE_PRESSED);
    if (!first) {
        lv_obj_set_style_border_side(item, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(UI_T->btn_pressed), 0);
    }
    ui_on_tap(item, menu_item_cb, (void *)(intptr_t)id);
    lv_obj_t *img = lv_image_create(item);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, lv_color_hex(UI_T->text2), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    label(item, text, &inter_14_semibold, UI_T->text);
}

/* Over everything: dims the screen; tap outside the panel or the X to close (that tap does
 * nothing else) */
static void menu_open_cb(lv_obj_t *obj, void *user_data)
{
    (void)obj;
    (void)user_data;
    menu_close();
    s_menu = plain_obj(lv_layer_top());
    lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(s_menu, lv_color_black(), 0);
    ui_on_tap(s_menu, menu_close_cb, NULL);

    lv_obj_t *x = icon_button(s_menu, &icon_close, 272, 4, 40, 32);
    lv_obj_set_style_bg_color(x, lv_color_hex(UI_T->btn_pressed), 0);
    lv_obj_set_ext_click_area(x, 4);
    ui_on_tap(x, menu_close_cb, NULL);

    lv_obj_t *panel = plain_obj(s_menu);
    lv_obj_set_size(panel, 140, LV_SIZE_CONTENT);
    lv_obj_set_pos(panel, 172, 40);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE); /* a tap between items doesn't close it */
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_T->bg), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_T->btn_border), 0);
    lv_obj_set_style_pad_ver(panel, 4, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    menu_item(panel, &icon_pill, "Doses", 0, true);
    menu_item(panel, &icon_settings, "Settings", 1, false);
    menu_item(panel, &icon_info, "About", 2, false);
    audio_play(AUDIO_CLICK);
    ESP_LOGI(TAG, "menu");
}

/* ---- Layout ---- */

/* Banner centered across the top, "THU 19 NOV · 0511 hrs"; the menu button in the top-right corner */
static void build_clock(lv_obj_t *scr)
{
    lv_obj_t *row = plain_obj(scr);
    lv_obj_set_size(row, 320, BANNER_H);
    lv_obj_set_pos(row, 0, 0);
    flex_center(row, LV_FLEX_FLOW_ROW, 8);
    s_date_label = label(row, "--- -- ---", &inter_16_bold, UI_T->date);
    label(row, "\u00b7", &inter_16_bold, UI_T->caption);
    s_time_label = label(row, "---- hrs", &inter_16_bold, UI_T->time);

    lv_obj_t *menu = icon_button(scr, &icon_menu, 272, 4, 40, 32);
    lv_obj_set_ext_click_area(menu, 4);
    ui_on_tap(menu, menu_open_cb, NULL);
}

static void build_doses(lv_obj_t *area)
{
    s_up = icon_button(area, &icon_chevron_up, ARROW_X, LIST_Y, ARROW_W, LIST_H / 2 - 2);
    ui_on_tap(s_up, arrow_cb, (void *)(intptr_t)-1);
    s_down = icon_button(area, &icon_chevron_down, ARROW_X, LIST_Y + LIST_H / 2 + 2, ARROW_W, LIST_H / 2 - 2);
    ui_on_tap(s_down, arrow_cb, (void *)(intptr_t)1);
    int32_t x = LIST_X;
    if (!need_arrows()) {
        lv_obj_add_flag(s_up, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_down, LV_OBJ_FLAG_HIDDEN);
        x = ARROW_X;
    }

    /* 2px padding all round */
    s_list = plain_obj(area);
    lv_obj_set_size(s_list, LIST_X + LIST_W - x + 4, LIST_H + 4);
    lv_obj_set_pos(s_list, x - 2, LIST_Y - 2);
    lv_obj_set_style_pad_all(s_list, 2, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, LIST_GAP, 0);
    fill_list();

    int undo = s_undo_uid ? doses_find_uid(s_undo_uid) : -1;
    if (undo < 0 || !dose_taken(undo)) { /* deleted or undone elsewhere (Home) */
        undo_cancel();
        undo = -1;
    }
    build_card(area, undo >= 0);
    if (undo >= 0) {
        build_undo_strip(area, undo);
    }
}


/* ---- Node sync status ---- */

#define SYNC_GREEN 0x22c55e
#define SYNC_RED 0xef4444
#define SYNC_GRAY 0x9ca3af

static lv_obj_t *s_sync_pill, *s_sync_label;
static int s_sync_shown = -1; /* synced * 100 + known, as drawn; -1 = not drawn */

/* "2 nodes synced" (green), "1 of 2 nodes synced" (red), "No nodes" (gray) */
static void update_sync_status(void)
{
    int synced, known;
    node_sync_status(&synced, &known);
    if (synced * 100 + known == s_sync_shown) {
        return;
    }
    s_sync_shown = synced * 100 + known;
    uint32_t tint = known == 0 ? SYNC_GRAY : synced == known ? SYNC_GREEN : SYNC_RED;
    if (known == 0) {
        lv_label_set_text(s_sync_label, "No nodes");
    } else if (synced == known) {
        lv_label_set_text_fmt(s_sync_label, "%d node%s synced", known, known == 1 ? "" : "s");
    } else {
        lv_label_set_text_fmt(s_sync_label, "%d of %d nodes synced", synced, known);
    }
    lv_obj_set_style_bg_color(s_sync_pill, lv_color_hex(tint), 0);
    lv_obj_set_style_border_color(s_sync_pill, lv_color_hex(tint), 0);
}

/* Node sync status (faint green/red outline instead of a dot) with the version below it,
 * centered on the screen */
static void build_bottom_row(lv_obj_t *scr)
{
    lv_obj_t *box = plain_obj(scr);
    lv_obj_set_size(box, 320, 42);
    lv_obj_set_pos(box, 0, 198);
    s_sync_pill = plain_obj(box);
    lv_obj_set_size(s_sync_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_sync_pill, 10, 0);
    lv_obj_set_style_pad_ver(s_sync_pill, 3, 0);
    lv_obj_set_style_radius(s_sync_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_sync_pill, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_sync_pill, 1, 0);
    lv_obj_set_style_border_opa(s_sync_pill, LV_OPA_40, 0);
    s_sync_label = label(s_sync_pill, "", &inter_11_semibold, UI_T->muted);
    lv_obj_align(s_sync_pill, LV_ALIGN_BOTTOM_MID, 0, -18);
    s_sync_shown = -1;
    update_sync_status();

    lv_obj_t *ver = label(box, fw_version_short(), &inter_8, UI_T->caption);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* Every second: dose states and alerts, then the date and time labels (dashes until the first
 * SNTP sync) */
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    if (doses_tick(now)) { /* also reports schedule edits before the clock is set */
        refresh(true);
    }
    update_sync_status();
    if (!time_is_valid()) {
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
    lv_label_set_text_fmt(s_time_label, "%02d%02d hrs", tm.tm_hour, tm.tm_min); /* 24 h, military style */
    lv_label_set_text_fmt(s_date_label, "%s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
    update_late_label();
}

static lv_obj_t *s_screen;

static void build(lv_obj_t *scr)
{
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_T->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_clock(scr);
    collect_today();
    snap_to_next();
    /* Transparent, non-clickable area in screen coordinates, so positions inside stay absolute */
    s_dose_area = plain_obj(scr);
    lv_obj_set_size(s_dose_area, 320, 240);
    build_doses(s_dose_area);
    build_bottom_row(scr);
}

void ui_home_create(lv_obj_t *scr)
{
    s_screen = scr;
    doses_init();
    build(scr);
    lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

void ui_home_restyle(void)
{
    lv_obj_clean(s_screen);
    build(s_screen);
    s_last_minute = -1; /* redraw the new clock labels */
    clock_timer_cb(NULL);
}

#endif
