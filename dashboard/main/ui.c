/* Dashboard screen, 800x480, dark: the C5's home layout made larger, plus a column of tiles.
 *   Top: date · 24 h time.
 *   Left: today's doses in their colors (taken rows get a check, overdue rows a red border); hold a
 *   taken row HOLD_UNDO_MS to undo it (after a confirm dialog), as on the C5.
 *   Left column: today's doses over the next dose as a card; tap the card to take it (TAKE over
 *   the link, the C5 decides).
 *   Middle column: the Cozi calendar, today and the next CAL_DAYS days (calendar.c).
 *   Right column: weather (Open-Meteo), a kitchen-style timer (5-60 min wheel, Start/Pause/Reset).
 *   Bottom: a status line, only when the link or the C5 is down.
 * Boot: the Averak Labs logo for SPLASH_MS, then a fade to the dose screen (as the C5).
 * Schedule (button top right): every dose with its days, view only (the C5 owns the schedule);
 * back to the dose screen with Back or after SCHED_IDLE_MS untouched.
 * Dose states are worked out here from the node's snapshot exactly as the C5 does (doses.c). */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <esp_log.h>

#include "calendar.h"
#include "doses.h"
#include "fonts/fonts.h"
#include "icons/icons.h"
#include "images/images.h"
#include "link.h"
#include "weather.h"
#include "wifi.h"

static const char *TAG = "ui";

#define W 800
#define H 480
#define PAD 16
#define BANNER_H 60
#define TOP (BANNER_H + 8)
#define BOTTOM (H - 36) /* status line below */
/* Three equal columns: doses (list over the next-dose card) | calendar | weather, garage, package */
#define COL_W ((W - 4 * PAD) / 3)
#define COL_X(c) (PAD + (c) * (COL_W + PAD))
#define LIST_X COL_X(0)
#define LIST_W COL_W
#define LIST_H ((BOTTOM - TOP - PAD) / 2)
#define LIST_ROWS 5 /* as on the C5 */
#define LIST_FONT inter_16_semibold
#define ROW_GAP 6
#define CARD_X COL_X(0)
#define CARD_Y (TOP + LIST_H + PAD)
#define CARD_W COL_W
#define CARD_H (BOTTOM - CARD_Y)
#define TILES_X COL_X(2)
#define TILES_W COL_W
#define WX_ICON_COLOR 0xfbbf24 /* sun */
#define WX_CLOUD_COLOR 0xd1d5db
#define WX_RAIN_COLOR 0xbae6fd /* light: the tile is blue */
/* Tile colors: background, title, and for the timer the wheel/buttons on it */
#define WEATHER_BG 0x075985     /* dark sky blue */
#define WEATHER_TITLE 0xbae6fd
#define CALENDAR_BG 0x134e4a    /* deep teal */
#define CALENDAR_TITLE 0x99f6e4
#define CAL_BAND 0x0f766e       /* day header */
#define CAL_BAND_TODAY 0x0d9488
#define CAL_CHIP 0x1d6560       /* timed event */
#define CAL_ALLDAY_EDGE 0x5eead4 /* all-day event: outlined */
#define CAL_WHO_K 0x1f45e6      /* Karen: royal blue (dose palette 0) */
#define CAL_WHO_E 0xdc2626      /* Evan: red */
#define CAL_WHO_A 0x9333ea      /* All: purple */
#define CAL_WHO_OTHER 0x6b7280
#define TIMER_BG 0x4c1d95       /* deep purple */
#define TIMER_TITLE 0xc4b5fd
#define TIMER_WELL 0x2e1065     /* wheel background */
#define TIMER_BUTTON 0x6d28d9
#define TIMER_EDGE 0x7c3aed
#define TIMER_STEP_MIN 5
#define TIMER_STEPS 12 /* 5 ... 60 min */
#define TIMER_DEFAULT 1 /* 10 min */

#define BG 0x0b0c0f
#define TEXT 0xe5e7eb
#define MUTED 0x9ca3af
#define TILE_BG 0x1f2937
#define TILE_BORDER 0x374151
#define DATE_COLOR 0x93c5fd
#define TIME_COLOR 0x86efac
#define OK_GREEN 0x22c55e
#define OVERDUE_RED 0xef4444
#define LATE_TEXT 0xb91c1c
#define TAKE_TIMEOUT_MS 10000
#define SNOOZE_H 48 /* the snooze strip under an overdue card */
#define SNOOZE_GAP 6
#define SNOOZE_BLUE 0x93c5fd /* as on the C5 */
#define SNOOZE_BLUE_TEXT 0x111827
#define HOLD_UNDO_MS 1000
#define SPLASH_MS 5000
#define SPLASH_BG 0x0b0d14 /* the logo's background */
#define SCHED_IDLE_MS 60000
#define SCHED_ROWS 8 /* per column; two columns = DOSE_MAX */
#define BUTTON_BG 0x1f2937
#define DIALOG_BG 0x111827
#define UNDO_AMBER 0xfbbf24

static lv_obj_t *s_date_label, *s_time_label, *s_dose_area, *s_status_label, *s_card_hint;
static lv_obj_t *s_home, *s_sched, *s_sched_list;
static lv_obj_t *s_wx_icon, *s_wx_temp, *s_wx_sky, *s_wx_day;
static lv_obj_t *s_cal_body;
static link_view_t s_view; /* static: off the LVGL task stack */
static dose_state_t s_states[DOSE_MAX];
static int s_next = -1;
static uint32_t s_shown_sig; /* what the dose area shows; rebuilt when it changes */
static uint8_t s_taking_uid;  /* tapped, waiting for the C5's new state */
static uint32_t s_taking_since;
static bool s_snoozing; /* tapped Snooze, waiting for the C5's new state */
static uint32_t s_snoozing_since;
static lv_obj_t *s_snooze_label;
static lv_obj_t *s_confirm; /* undo dialog, on the top layer */
static uint8_t s_confirm_uid;

/* ---- dose states (doses.c compute_states, on the snapshot) ---- */

static bool clock_ok(void)
{
    return time(NULL) > 1700000000;
}

static uint32_t date_of(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (uint32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

static bool due_today(const dose_def_t *d, int wday)
{
    return wday < 0 || (d->days & (1u << wday));
}

static void compute_states(void)
{
    const doses_snapshot_t *s = &s_view.doses;
    time_t now = time(NULL);
    bool ok = clock_ok();
    struct tm tm;
    localtime_r(&now, &tm);
    int wday = ok ? tm.tm_wday : -1;
    /* Taken bits belong to the snapshot's date; a new day clears them until the C5 says so */
    uint32_t taken = !ok || s->date == date_of(now) ? s->taken : 0;
    int minute_now = tm.tm_hour * 60 + tm.tm_min;
    bool next_found = false;
    s_next = -1;
    for (int i = 0; i < s->count; i++) {
        const dose_def_t *d = &s->defs[i];
        if (!due_today(d, wday)) {
            s_states[i] = DOSE_LATER; /* not shown today */
            continue;
        }
        if (taken & (1u << i)) {
            s_states[i] = DOSE_TAKEN;
            continue;
        }
        if (ok && minute_now >= d->minute + s->overdue_min) {
            s_states[i] = DOSE_OVERDUE;
        } else {
            s_states[i] = next_found ? DOSE_LATER : DOSE_NEXT;
        }
        if (!next_found) {
            s_next = i;
        }
        next_found = true;
    }
}

static int today_list(int *out)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int wday = clock_ok() ? tm.tm_wday : -1;
    int n = 0;
    for (int i = 0; i < s_view.doses.count; i++) {
        if (due_today(&s_view.doses.defs[i], wday)) {
            out[n++] = i;
        }
    }
    return n;
}

/* Late minutes for the overdue card (0 if not overdue) */
static int late_min(int i)
{
    if (i < 0 || s_states[i] != DOSE_OVERDUE) {
        return 0;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int late = tm.tm_hour * 60 + tm.tm_min - s_view.doses.defs[i].minute;
    return late > 0 ? late : 0;
}

/* ---- drawing ---- */

static uint32_t dose_color(int i)
{
    uint8_t c = s_view.doses.defs[i].color;
    return dose_colors[c < DOSE_COLOR_COUNT ? c : 0];
}

/* Dark text on light dose colors (yellow, pale sky...), white on the rest: as ui_dose_row.c */
static uint32_t text_on(uint32_t c)
{
    int luma = (299 * ((c >> 16) & 0xff) + 587 * ((c >> 8) & 0xff) + 114 * (c & 0xff)) / 1000;
    return luma > 150 ? 0x111827 : 0xffffff;
}

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
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

static void fmt_time(int minute, char *buf, int len)
{
    snprintf(buf, len, "%02d%02d", minute / 60, minute % 60);
}

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa(obj, (lv_opa_t)v, 0);
}

/* Red border, fading in and out */
static void overdue_border(lv_obj_t *o, int width)
{
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(OVERDUE_RED), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, pulse_exec);
    lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void confirm_close(void)
{
    if (s_confirm) {
        lv_obj_delete(s_confirm);
        s_confirm = NULL;
    }
}

static void confirm_cb(lv_event_t *e)
{
    bool undo = (bool)(intptr_t)lv_event_get_user_data(e);
    if (undo && s_confirm_uid && s_view.up) {
        ESP_LOGI(TAG, "undo uid %u", s_confirm_uid);
        s_taking_uid = s_confirm_uid;
        s_taking_since = lv_tick_get();
        link_take(s_confirm_uid, false);
    }
    confirm_close();
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg, bool undo)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_size(b, 180, 64);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(lv_color_hex(bg), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, confirm_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)undo);
    lv_obj_t *l = label(b, text, &inter_23_semibold, fg);
    lv_obj_center(l);
    return b;
}

/* "Undo Karen 0930?": Keep / Undo. Tapping outside the box keeps it. */
static void confirm_open(int i)
{
    confirm_close();
    const dose_def_t *d = &s_view.doses.defs[i];
    s_confirm_uid = d->uid;
    s_confirm = plain_obj(lv_layer_top());
    lv_obj_set_size(s_confirm, W, H);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_confirm, confirm_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)false);

    lv_obj_t *box = plain_obj(s_confirm);
    lv_obj_set_size(box, 460, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE); /* swallow taps */
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(DIALOG_BG), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(TILE_BORDER), 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    char t[8], text[48];
    fmt_time(d->minute, t, sizeof(t));
    snprintf(text, sizeof(text), "Undo %s %s?", d->name, t);
    label(box, text, &inter_30_semibold, TEXT);
    label(box, "It will show as not taken.", &inter_16_semibold, MUTED);
    lv_obj_t *buttons = plain_obj(box);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dialog_button(buttons, "Keep", TILE_BG, TEXT, false);
    dialog_button(buttons, "Undo", UNDO_AMBER, 0x111827, true);
}

static void row_hold_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_view.doses.count && s_states[i] == DOSE_TAKEN && !s_taking_uid) {
        confirm_open(i);
    }
}

static void build_list(lv_obj_t *area)
{
    int idx[DOSE_MAX];
    int n = today_list(idx);
    lv_obj_t *list = plain_obj(area);
    lv_obj_set_pos(list, LIST_X, TOP);
    lv_obj_set_size(list, LIST_W, LIST_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, ROW_GAP, 0);
    if (n == 0) {
        label(list, s_view.have_doses ? "No doses today" : "Waiting for the doses...", &LIST_FONT, MUTED);
        return;
    }
    /* More rows than fit: start so the next dose shows (one row of context above it) */
    int first = 0;
    if (n > LIST_ROWS) {
        for (int k = 0; k < n; k++) {
            if (idx[k] == s_next) {
                first = k > 0 ? k - 1 : 0;
            }
        }
        first = first > n - LIST_ROWS ? n - LIST_ROWS : first;
    }
    int rows = n < LIST_ROWS ? n : LIST_ROWS;
    int row_h = (LIST_H - (LIST_ROWS - 1) * ROW_GAP) / LIST_ROWS;
    for (int k = first; k < first + rows; k++) {
        int i = idx[k];
        const dose_def_t *d = &s_view.doses.defs[i];
        uint32_t color = dose_color(i), fg = text_on(color);
        lv_obj_t *row = plain_obj(list);
        lv_obj_set_size(row, LV_PCT(100), row_h);
        lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(color), 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        if (s_states[i] == DOSE_OVERDUE) {
            overdue_border(row, 2);
        }
        char t[8];
        fmt_time(d->minute, t, sizeof(t));
        label(row, t, &LIST_FONT, fg);
        lv_obj_t *name = label(row, d->name, &LIST_FONT, fg);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_height(name, lv_font_get_line_height(&LIST_FONT));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (s_states[i] == DOSE_TAKEN) {
            label(row, "✓", &LIST_FONT, fg);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, lv_color_lighten(lv_color_hex(color), LV_OPA_20), LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, row_hold_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        }
    }
}

static void card_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_view.doses.count || s_taking_uid || !s_view.up) {
        return;
    }
    s_taking_uid = s_view.doses.defs[i].uid;
    s_taking_since = lv_tick_get();
    char t[8];
    fmt_time(s_view.doses.defs[i].minute, t, sizeof(t));
    ESP_LOGI(TAG, "take %s %s (uid %u)", s_view.doses.defs[i].name, t, s_taking_uid);
    link_take(s_taking_uid, true);
    if (s_card_hint) {
        lv_label_set_text(s_card_hint, "Taking...");
    }
}

static void build_done_card(lv_obj_t *card)
{
    lv_obj_set_style_bg_color(card, lv_color_hex(TILE_BG), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(TILE_BORDER), 0);
    bool any = today_list((int[DOSE_MAX]){0}) > 0;
    lv_obj_t *top = plain_obj(card);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, 0);
    label(top, "✓", &inter_30_semibold, OK_GREEN);
    label(top, any ? "All done today" : "No doses today", &inter_23_semibold, TEXT);
    if (!clock_ok()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int tomorrow = (tm.tm_wday + 1) % 7;
    for (int i = 0; i < s_view.doses.count; i++) { /* sorted by time: the first due tomorrow */
        if (due_today(&s_view.doses.defs[i], tomorrow)) {
            char text[48], t[8];
            fmt_time(s_view.doses.defs[i].minute, t, sizeof(t));
            snprintf(text, sizeof(text), "Tomorrow first:\n%s %s", t, s_view.doses.defs[i].name);
            lv_obj_t *l = label(card, text, &inter_16_semibold, MUTED);
            lv_obj_set_width(l, LV_PCT(100));
            break;
        }
    }
}

/* ---- snooze (the C5 decides; its state says until when and how many are left) ---- */

static bool snooze_wanted(void)
{
    uint16_t code = s_view.doses.snooze;
    return s_next >= 0 && s_states[s_next] == DOSE_OVERDUE &&
           (DOSE_SNOOZE_UNTIL(code) >= 0 || DOSE_SNOOZE_LEFT(code) > 0);
}

static void snooze_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_snoozing || !s_view.up) {
        return;
    }
    ESP_LOGI(TAG, "snooze");
    s_snoozing = true;
    s_snoozing_since = lv_tick_get();
    link_snooze();
    if (s_snooze_label) {
        lv_label_set_text(s_snooze_label, "Snoozing...");
    }
}

static void build_snooze_strip(lv_obj_t *area)
{
    uint16_t code = s_view.doses.snooze;
    int until = DOSE_SNOOZE_UNTIL(code), left = DOSE_SNOOZE_LEFT(code);
    lv_obj_t *strip = plain_obj(area);
    lv_obj_set_pos(strip, CARD_X, CARD_Y + CARD_H - SNOOZE_H);
    lv_obj_set_size(strip, CARD_W, SNOOZE_H);
    lv_obj_set_style_radius(strip, 12, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(TILE_BG), 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_style_border_color(strip, lv_color_hex(TILE_BORDER), 0);
    lv_obj_set_style_pad_all(strip, 6, 0);
    lv_obj_set_style_pad_left(strip, 12, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(strip, 10, 0);
    char text[40];
    s_snooze_label = NULL;
    if (until >= 0) {
        /* Snoozed: no button until it ends (one at a time) */
        snprintf(text, sizeof(text), "Quiet until %02d%02d  \u00b7  %d left", until / 60, until % 60, left);
        label(strip, text, &inter_16_semibold, TEXT);
        return;
    }
    snprintf(text, sizeof(text), "%d left", left);
    label(strip, text, &inter_16_semibold, MUTED);
    lv_obj_t *btn = plain_obj(strip);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SNOOZE_BLUE), 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(lv_color_hex(SNOOZE_BLUE), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, snooze_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    snprintf(text, sizeof(text), "Snooze %d min", DOSE_SNOOZE_MIN);
    s_snooze_label = label(btn, s_snoozing ? "Snoozing..." : text, &inter_16_semibold, SNOOZE_BLUE_TEXT);
    lv_obj_center(s_snooze_label);
}

static void build_card(lv_obj_t *area)
{
    bool strip = snooze_wanted();
    lv_obj_t *card = plain_obj(area);
    lv_obj_set_pos(card, CARD_X, CARD_Y);
    lv_obj_set_size(card, CARD_W, strip ? CARD_H - SNOOZE_H - SNOOZE_GAP : CARD_H);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_card_hint = NULL;
    int i = s_next;
    if (i < 0) {
        if (s_view.have_doses) {
            build_done_card(card);
        } else {
            lv_obj_set_style_bg_color(card, lv_color_hex(TILE_BG), 0);
        }
        return;
    }
    const dose_def_t *d = &s_view.doses.defs[i];
    uint32_t color = dose_color(i), fg = text_on(color);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(card, lv_color_lighten(lv_color_hex(color), LV_OPA_20), LV_STATE_PRESSED);
    bool overdue = s_states[i] == DOSE_OVERDUE;
    if (overdue) {
        overdue_border(card, 4);
        lv_obj_set_style_pad_all(card, 10, 0);
    }
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_tap_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

    char t[8];
    fmt_time(d->minute, t, sizeof(t));
    label(card, t, &inter_30_semibold, fg);

    lv_obj_t *mid = plain_obj(card);
    lv_obj_set_size(mid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 6, 0);
    lv_obj_t *name = label(mid, d->name, &inter_23_semibold, fg);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_height(name, lv_font_get_line_height(&inter_23_semibold));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    if (overdue) {
        lv_obj_t *badge = plain_obj(mid);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
        lv_obj_set_style_pad_hor(badge, 10, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        int late = late_min(i);
        char text[24];
        if (late < 60) {
            snprintf(text, sizeof(text), "Late %d min", late);
        } else {
            snprintf(text, sizeof(text), "Late %dh %02dm", late / 60, late % 60);
        }
        label(badge, text, &inter_16_bold, LATE_TEXT);
    }
    if (strip) {
        build_snooze_strip(area); /* no room for the hint: the card is still the tap target */
        return;
    }
    s_card_hint = label(card, s_taking_uid ? "Taking..." : "Tap when taken", &inter_16_semibold, fg);
    lv_obj_set_width(s_card_hint, LV_PCT(100));
}

static lv_obj_t *tile(lv_obj_t *area, int x, int y, int w, int h, const char *title, const char *body)
{
    lv_obj_t *t = plain_obj(area);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(TILE_BG), 0);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 6, 0);
    label(t, title, &inter_16_bold, MUTED);
    if (body) {
        lv_obj_t *b = label(t, body, &inter_16_semibold, TEXT);
        lv_obj_set_width(b, LV_PCT(100));
    }
    return t;
}

static void tile_colors(lv_obj_t *t, uint32_t bg, uint32_t title)
{
    lv_obj_set_style_bg_color(t, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(t, 0), lv_color_hex(title), 0);
}

/* Weather tile: "52 F" big with the sky next to it, then today's high/low and rain chance
 * (the fonts have no degree sign) */
static void build_weather(lv_obj_t *t)
{
    tile_colors(t, WEATHER_BG, WEATHER_TITLE);
    /* Under the title, centered in the rest of the tile: icon + temperature, sky, the day */
    lv_obj_t *body = plain_obj(t);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_t *now = plain_obj(body);
    lv_obj_set_size(now, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 12, 0);
    s_wx_icon = lv_image_create(now);
    lv_obj_set_style_image_recolor_opa(s_wx_icon, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_wx_icon, LV_OBJ_FLAG_HIDDEN);
    s_wx_temp = label(now, "", &inter_30_semibold, TEXT);
    s_wx_sky = label(body, "", &inter_16_semibold, TEXT);
    s_wx_day = label(body, "", &inter_16_semibold, WEATHER_TITLE);
    lv_obj_set_style_text_align(s_wx_day, LV_TEXT_ALIGN_CENTER, 0);
}

/* Icon and its color for a WMO code (NULL: none) */
static const lv_image_dsc_t *weather_icon(int code, uint32_t *color)
{
    *color = WX_CLOUD_COLOR;
    if (code <= 1) {
        *color = WX_ICON_COLOR;
        return &icon_wx_sun;
    }
    if (code == 2) {
        return &icon_wx_partly;
    }
    if (code == 3) {
        return &icon_wx_cloud;
    }
    if (code == 45 || code == 48) {
        return &icon_wx_fog;
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        *color = WX_RAIN_COLOR;
        return &icon_wx_rain;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return &icon_wx_snow;
    }
    if (code >= 95) {
        *color = WX_ICON_COLOR;
        return &icon_wx_storm;
    }
    return NULL;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void update_weather(void)
{
    weather_t w;
    weather_get(&w);
    char temp[16] = "", sky[40] = "", day[64] = "";
    if (!wifi_configured()) {
        snprintf(sky, sizeof(sky), "Wi-Fi not set up");
    } else if (!w.valid) {
        snprintf(sky, sizeof(sky), wifi_connected() ? "Loading..." : "Connecting to Wi-Fi...");
    } else {
        snprintf(temp, sizeof(temp), "%d F", w.temp_f);
        snprintf(sky, sizeof(sky), "%s", weather_text(w.code));
        int n = snprintf(day, sizeof(day), "H %d  \u00b7  L %d  \u00b7  Rain %d%%", w.high_f, w.low_f, w.rain_pct);
        /* More than an hour old (Wi-Fi or Open-Meteo down): say when it's from */
        if (clock_ok() && time(NULL) - w.fetched > 3600) {
            struct tm tm;
            localtime_r(&w.fetched, &tm);
            snprintf(day + n, sizeof(day) - n, "\nas of %02d%02d", tm.tm_hour, tm.tm_min);
        }
    }
    uint32_t color;
    const lv_image_dsc_t *icon = w.valid ? weather_icon(w.code, &color) : NULL;
    if (icon) {
        if (lv_image_get_src(s_wx_icon) != icon) {
            lv_image_set_src(s_wx_icon, icon);
            lv_obj_set_style_image_recolor(s_wx_icon, lv_color_hex(color), 0);
        }
        lv_obj_remove_flag(s_wx_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wx_icon, LV_OBJ_FLAG_HIDDEN);
    }
    set_text(s_wx_temp, temp);
    set_text(s_wx_sky, sky);
    set_text(s_wx_day, day);
}

/* ---- timer tile ---- */

typedef enum { TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_DONE } timer_state_t;

static timer_state_t s_timer;
static uint32_t s_timer_left_ms, s_timer_since; /* running: left_ms at since (lv_tick) */
static lv_obj_t *s_timer_roller, *s_timer_label, *s_timer_start, *s_timer_pause, *s_timer_done;

static uint32_t timer_set_ms(void)
{
    return (lv_roller_get_selected(s_timer_roller) + 1) * TIMER_STEP_MIN * 60000u;
}

static uint32_t timer_left_ms(void)
{
    if (s_timer != TIMER_RUNNING) {
        return s_timer_left_ms;
    }
    uint32_t gone = lv_tick_elaps(s_timer_since);
    return gone >= s_timer_left_ms ? 0 : s_timer_left_ms - gone;
}

static void enable(lv_obj_t *o, bool on)
{
    if (on) {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

static void timer_show(void)
{
    uint32_t s = (timer_left_ms() + 999) / 1000; /* 0:01 until it is really over */
    char text[16];
    snprintf(text, sizeof(text), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    set_text(s_timer_label, s_timer == TIMER_DONE ? "Done" : text);
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(s_timer == TIMER_PAUSED ? TIMER_TITLE : TEXT), 0);
    enable(s_timer_roller, s_timer == TIMER_IDLE);
    enable(s_timer_start, s_timer == TIMER_IDLE || s_timer == TIMER_PAUSED);
    enable(s_timer_pause, s_timer == TIMER_RUNNING);
}

static void timer_reset(void)
{
    if (s_timer_done) {
        lv_obj_delete(s_timer_done);
        s_timer_done = NULL;
    }
    s_timer = TIMER_IDLE;
    s_timer_left_ms = timer_set_ms();
    timer_show();
}

static void timer_done_cb(lv_event_t *e)
{
    (void)e;
    timer_reset();
}

/* Full screen, over whichever screen is showing; any tap dismisses it and resets the timer */
static void timer_done_open(void)
{
    s_timer_done = plain_obj(lv_layer_top());
    lv_obj_set_size(s_timer_done, W, H);
    lv_obj_set_style_bg_opa(s_timer_done, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_timer_done, lv_color_black(), 0);
    lv_obj_add_flag(s_timer_done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_timer_done, timer_done_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *box = plain_obj(s_timer_done);
    lv_obj_set_size(box, 460, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(DIALOG_BG), 0);
    overdue_border(box, 4);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE); /* taps go through to the backdrop */
    label(box, "Timer done", &inter_48_bold, TEXT);
    char text[24];
    snprintf(text, sizeof(text), "%u min", (unsigned)(timer_set_ms() / 60000));
    label(box, text, &inter_23_semibold, MUTED);
    label(box, "Tap anywhere to close", &inter_16_semibold, MUTED);
}

static void timer_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_timer == TIMER_RUNNING && timer_left_ms() == 0) {
        ESP_LOGI(TAG, "timer done");
        s_timer = TIMER_DONE;
        s_timer_left_ms = 0;
        timer_done_open();
    }
    timer_show();
}

static void timer_button_cb(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    if (b == s_timer_start && (s_timer == TIMER_IDLE || s_timer == TIMER_PAUSED)) {
        if (s_timer == TIMER_IDLE) {
            s_timer_left_ms = timer_set_ms();
        }
        s_timer_since = lv_tick_get();
        s_timer = TIMER_RUNNING;
    } else if (b == s_timer_pause && s_timer == TIMER_RUNNING) {
        s_timer_left_ms = timer_left_ms();
        s_timer = TIMER_PAUSED;
    } else if (b != s_timer_start && b != s_timer_pause) {
        timer_reset();
        return;
    }
    timer_show();
}

static void timer_roller_cb(lv_event_t *e)
{
    (void)e;
    if (s_timer == TIMER_IDLE) {
        s_timer_left_ms = timer_set_ms();
        timer_show();
    }
}

static lv_obj_t *timer_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_height(b, 36);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TIMER_BUTTON), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TIMER_EDGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(TIMER_EDGE), 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, timer_button_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_center(label(b, text, &inter_16_semibold, TEXT));
    return b;
}

static void build_timer(lv_obj_t *t)
{
    tile_colors(t, TIMER_BG, TIMER_TITLE);
    /* Wheel pinned to the left edge; the time centered in the rest (its width changes) */
    lv_obj_t *top = plain_obj(t);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char opts[TIMER_STEPS * 8] = "";
    for (int i = 1; i <= TIMER_STEPS; i++) {
        char o[10];
        snprintf(o, sizeof(o), "%s%d min", i > 1 ? "\n" : "", i * TIMER_STEP_MIN);
        strcat(opts, o);
    }
    s_timer_roller = lv_roller_create(top);
    lv_roller_set_options(s_timer_roller, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_timer_roller, 3);
    lv_roller_set_selected(s_timer_roller, TIMER_DEFAULT, LV_ANIM_OFF);
    lv_obj_set_width(s_timer_roller, 96);
    lv_obj_set_style_text_font(s_timer_roller, &inter_16_semibold, 0);
    lv_obj_set_style_text_font(s_timer_roller, &inter_16_semibold, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(s_timer_roller, 10, 0);
    lv_obj_set_style_bg_color(s_timer_roller, lv_color_hex(TIMER_WELL), 0);
    lv_obj_set_style_text_color(s_timer_roller, lv_color_hex(TIMER_TITLE), 0);
    lv_obj_set_style_border_color(s_timer_roller, lv_color_hex(TIMER_EDGE), 0);
    lv_obj_set_style_bg_color(s_timer_roller, lv_color_hex(TIMER_BUTTON), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_timer_roller, lv_color_hex(TEXT), LV_PART_SELECTED);
    lv_obj_set_style_opa(s_timer_roller, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_timer_roller, timer_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_timer_label = label(top, "", &inter_30_semibold, TEXT);
    lv_obj_set_flex_grow(s_timer_label, 1);
    lv_obj_set_style_text_align(s_timer_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *buttons = plain_obj(t);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    s_timer_start = timer_button(buttons, "Start");
    s_timer_pause = timer_button(buttons, "Pause");
    timer_button(buttons, "Reset");

    timer_reset();
    lv_timer_create(timer_tick_cb, 200, NULL);
}

/* ---- calendar tile ---- */

/* Each day: a band (brighter for today), then its events as chips: filled = timed, outlined =
 * all day (or a timed event continuing from an earlier day) */
#define CAL_HEAD_H 26
#define CAL_HEAD_GAP 6 /* extra space above a band, except the first */
#define CAL_ROW_H 26
#define CAL_GAP 4      /* between rows (the body's pad_row) */
#define CAL_TIME_W 44

static void cal_badge(lv_obj_t *row, char who)
{
    uint32_t color = who == 'K' ? CAL_WHO_K : who == 'E' ? CAL_WHO_E : who == 'A' ? CAL_WHO_A : CAL_WHO_OTHER;
    lv_obj_t *b = plain_obj(row);
    lv_obj_set_size(b, 20, 20);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_margin_right(b, 6, 0);
    char text[2] = {who, 0};
    lv_obj_center(label(b, text, &inter_12_bold, 0xffffff));
}

static void cal_line(lv_obj_t *parent, char who, const char *time_text, const char *title)
{
    lv_obj_t *row = plain_obj(parent);
    lv_obj_set_size(row, LV_PCT(100), CAL_ROW_H);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    if (time_text) {
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(CAL_CHIP), 0);
    } else {
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(CAL_ALLDAY_EDGE), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
    }
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, who ? 3 : 8, 0);
    if (who) {
        cal_badge(row, who);
    }
    if (time_text) {
        lv_obj_t *t = label(row, time_text, &inter_16_semibold, CALENDAR_TITLE);
        lv_obj_set_width(t, CAL_TIME_W);
    }
    lv_obj_t *l = label(row, title, &inter_16_semibold, TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_height(l, lv_font_get_line_height(&inter_16_semibold));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

static void cal_header(lv_obj_t *parent, int32_t day, int32_t today, bool first)
{
    static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    time_t t = (time_t)day * 86400 + 12 * 3600; /* noon UTC: the right date in any US zone */
    struct tm tm;
    gmtime_r(&t, &tm);
    char date[16];
    snprintf(date, sizeof(date), "%s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);

    lv_obj_t *band = plain_obj(parent);
    lv_obj_set_size(band, LV_PCT(100), CAL_HEAD_H);
    if (!first) {
        lv_obj_set_style_margin_top(band, CAL_HEAD_GAP, 0);
    }
    lv_obj_set_style_radius(band, 6, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(band, lv_color_hex(day == today ? CAL_BAND_TODAY : CAL_BAND), 0);
    lv_obj_set_style_pad_hor(band, 8, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(band, 8, 0);
    if (day <= today + 1) {
        label(band, day == today ? "TODAY" : "TOMORROW", &inter_16_bold, 0xffffff);
        label(band, date, &inter_16_semibold, CALENDAR_TITLE);
    } else {
        label(band, date, &inter_16_bold, 0xffffff);
    }
}

/* Rebuilt when the feed changes and every minute (events drop off once over, the day moves) */
static void update_calendar(void)
{
    static calendar_t c;
    static uint32_t shown_version;
    static int32_t shown_minute = -1;
    calendar_get(&c);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int32_t today = cal_day(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int now_min = tm.tm_hour * 60 + tm.tm_min;
    int32_t minute = clock_ok() ? today * 1440 + now_min : 0;
    if (c.version == shown_version && minute == shown_minute) {
        return;
    }
    shown_version = c.version;
    shown_minute = minute;
    lv_obj_clean(s_cal_body);

    if (!c.valid) {
        const char *msg = !calendar_configured() ? "Calendar not set up"
                          : !wifi_connected()    ? "Connecting to Wi-Fi..."
                                                 : "Loading...";
        label(s_cal_body, msg, &inter_16_semibold, CALENDAR_TITLE);
        return;
    }
    int room = lv_obj_get_content_height(s_cal_body), used = 0, shown = 0;
    for (int32_t day = today; day < today + CAL_DAYS; day++) {
        bool header = false;
        /* All-day and continuing events first, then by time (the list is sorted that way per start day) */
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < c.count; i++) {
                const cal_event_t *e = &c.ev[i];
                if (day < e->first_day || day > e->last_day) {
                    continue;
                }
                bool timed_here = e->start_min >= 0 && day == e->first_day;
                if (timed_here != (pass == 1)) {
                    continue;
                }
                if (day == today && e->start_min >= 0 &&
                    (e->end_day < today || (e->end_day == today && e->end_min <= now_min))) {
                    continue; /* over */
                }
                int need = CAL_ROW_H + CAL_GAP + (header ? 0 : CAL_HEAD_H + CAL_GAP + (used ? CAL_HEAD_GAP : 0));
                if (used + need > room + CAL_GAP) {
                    goto full;
                }
                if (!header) {
                    cal_header(s_cal_body, day, today, used == 0);
                    header = true;
                }
                char t[8] = "";
                if (timed_here) {
                    snprintf(t, sizeof(t), "%02d%02d", e->start_min / 60, e->start_min % 60);
                }
                cal_line(s_cal_body, e->who, timed_here ? t : NULL, e->title);
                used += need;
                shown++;
            }
        }
    }
full:
    if (!shown && !used) {
        label(s_cal_body, "Nothing in the next 2 weeks", &inter_16_semibold, CALENDAR_TITLE);
    }
}

static void build_calendar(lv_obj_t *t)
{
    tile_colors(t, CALENDAR_BG, CALENDAR_TITLE);
    s_cal_body = plain_obj(t);
    lv_obj_set_width(s_cal_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_cal_body, 1);
    lv_obj_set_flex_flow(s_cal_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cal_body, CAL_GAP, 0);
    lv_obj_update_layout(t); /* its height decides how many rows fit */
}

static void build_tiles(lv_obj_t *scr)
{
    /* Middle column: the calendar, full height. Right: weather level with the dose list, the
     * timer level with the card */
    build_calendar(tile(scr, COL_X(1), TOP, COL_W, BOTTOM - TOP, "CALENDAR", NULL));
    build_weather(tile(scr, TILES_X, TOP, TILES_W, LIST_H, "WEATHER", NULL));
    build_timer(tile(scr, TILES_X, CARD_Y, TILES_W, CARD_H, "TIMER", NULL));
}

static void rebuild_doses(void)
{
    lv_obj_clean(s_dose_area);
    s_snooze_label = NULL;
    build_list(s_dose_area);
    build_card(s_dose_area);
}

/* What the dose area depends on: rebuild only when it changes */
static uint32_t dose_sig(void)
{
    uint32_t sig = 2166136261u;
    for (int i = 0; i < s_view.doses.count; i++) {
        sig = (sig ^ s_states[i]) * 16777619u;
    }
    sig = (sig ^ (uint32_t)(s_next + 1)) * 16777619u;
    sig = (sig ^ (uint32_t)late_min(s_next)) * 16777619u;
    sig = (sig ^ (s_view.have_doses ? 1u : 0u)) * 16777619u;
    return sig;
}

/* ---- schedule (view only) ---- */

static void days_text(uint8_t days, char *buf, int len)
{
    static const char *const names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if ((days & DOSE_DAILY) == DOSE_DAILY) {
        snprintf(buf, len, "Daily");
        return;
    }
    int n = 0;
    buf[0] = 0;
    for (int d = 0; d < 7; d++) {
        if (days & (1u << d)) {
            n += snprintf(buf + n, len - n, "%s%s", n ? " " : "", names[d]);
        }
    }
}

static void fill_schedule(void)
{
    lv_obj_clean(s_sched_list);
    const doses_snapshot_t *s = &s_view.doses;
    if (!s_view.have_doses || s->count == 0) {
        label(s_sched_list, s_view.have_doses ? "No doses set up" : "Waiting for the doses...", &inter_23_semibold,
              MUTED);
        return;
    }
    int col_w = (W - 2 * PAD - PAD) / 2;
    int row_h = (H - TOP - 44 - (SCHED_ROWS - 1) * ROW_GAP) / SCHED_ROWS;
    for (int i = 0; i < s->count; i++) {
        const dose_def_t *d = &s->defs[i];
        uint32_t color = dose_color(i), fg = text_on(color);
        lv_obj_t *row = plain_obj(s_sched_list);
        lv_obj_set_size(row, col_w, row_h);
        lv_obj_set_pos(row, (i / SCHED_ROWS) * (col_w + PAD), (i % SCHED_ROWS) * (row_h + ROW_GAP));
        lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(color), 0);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        char t[8], days[32];
        fmt_time(d->minute, t, sizeof(t));
        days_text(d->days, days, sizeof(days));
        label(row, t, &inter_23_semibold, fg);
        lv_obj_t *name = label(row, d->name, &inter_23_semibold, fg);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_height(name, lv_font_get_line_height(&inter_23_semibold));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        label(row, days, &inter_16_semibold, fg);
    }
    char foot[64];
    snprintf(foot, sizeof(foot), "Overdue %u min after the time  \u00b7  edit on the main unit", s->overdue_min);
    lv_obj_t *f = label(s_sched_list, foot, &inter_16_semibold, MUTED);
    lv_obj_set_pos(f, 0, SCHED_ROWS * (row_h + ROW_GAP) + 6);
}

static lv_obj_t *nav_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = plain_obj(parent);
    lv_obj_set_size(b, 150, 44);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(BUTTON_BG), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(TILE_BORDER), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(TILE_BORDER), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_center(label(b, text, &inter_23_semibold, TEXT));
    return b;
}

static void sched_back_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_home);
}

static void sched_open_cb(lv_event_t *e)
{
    (void)e;
    fill_schedule();
    lv_screen_load(s_sched);
}

static void build_schedule_screen(void)
{
    s_sched = lv_obj_create(NULL);
    lv_obj_remove_flag(s_sched, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_sched, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s_sched, LV_OPA_COVER, 0);
    lv_obj_t *back = nav_button(s_sched, "Back", sched_back_cb);
    lv_obj_set_pos(back, PAD, 8);
    lv_obj_t *title = label(s_sched, "Dose schedule", &inter_30_semibold, TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    s_sched_list = plain_obj(s_sched);
    lv_obj_set_pos(s_sched_list, PAD, TOP);
    lv_obj_set_size(s_sched_list, W - 2 * PAD, H - TOP);
}

/* ---- boot splash ---- */

static void splash_done_cb(lv_timer_t *t)
{
    (void)t;
    lv_screen_load_anim(s_home, LV_SCREEN_LOAD_ANIM_FADE_IN, 300, 0, true);
}

static void show_splash(void)
{
    lv_obj_t *splash = lv_screen_active();
    lv_obj_remove_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(splash, lv_color_hex(SPLASH_BG), 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_t *logo = lv_image_create(splash);
    lv_image_set_src(logo, &img_averak_logo_large);
    lv_obj_center(logo);
    lv_timer_t *t = lv_timer_create(splash_done_cb, SPLASH_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bool changed = link_get(&s_view);
    /* The node's clock (Thread SNTP via the C5's network) sets ours */
    if (s_view.up && s_view.status.time) {
        time_t now = time(NULL);
        if (now - (time_t)s_view.status.time > 2 || (time_t)s_view.status.time - now > 2) {
            struct timeval tv = {.tv_sec = s_view.status.time};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "clock set from the node");
        }
    }
    if (changed) {
        /* A new state: the take or snooze went through (or something else changed) */
        s_taking_uid = 0;
        s_snoozing = false;
    } else {
        bool timed_out = false;
        if (s_taking_uid && lv_tick_elaps(s_taking_since) > TAKE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "take of uid %u: no new state in %d s", s_taking_uid, TAKE_TIMEOUT_MS / 1000);
            s_taking_uid = 0;
            timed_out = true;
        }
        if (s_snoozing && lv_tick_elaps(s_snoozing_since) > TAKE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "snooze: no new state in %d s", TAKE_TIMEOUT_MS / 1000);
            s_snoozing = false;
            timed_out = true;
        }
        changed = timed_out; /* back to "Tap when taken" / "Snooze 15 min" */
    }
    compute_states();
    if (lv_screen_active() == s_sched) {
        if (changed) {
            fill_schedule();
        }
        if (lv_display_get_inactive_time(NULL) > SCHED_IDLE_MS) {
            lv_screen_load(s_home);
        }
    }
    uint32_t sig = dose_sig();
    if (changed || sig != s_shown_sig) {
        s_shown_sig = sig;
        rebuild_doses();
    }

    if (clock_ok()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        lv_label_set_text_fmt(s_date_label, "%s %d %s  ·", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
        lv_label_set_text_fmt(s_time_label, "%02d%02d hrs", tm.tm_hour, tm.tm_min);
    }

    const char *problem = NULL;
    if (!s_view.up) {
        problem = "Dashboard node not answering";
    } else if (!(s_view.status.flags & LINK_FLAG_MAIN_CONNECTED)) {
        problem = "Main unit not connected: doses may be out of date";
    }
    lv_label_set_text(s_status_label, problem ? problem : "");
    update_weather();
    update_calendar();
}

void ui_start(void)
{
    lv_obj_t *scr = s_home = lv_obj_create(NULL); /* shown after the splash */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *banner = plain_obj(scr);
    lv_obj_set_size(banner, W, BANNER_H);
    lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(banner, 12, 0);
    s_date_label = label(banner, "", &inter_30_semibold, DATE_COLOR);
    s_time_label = label(banner, "", &inter_30_semibold, TIME_COLOR);
    lv_obj_t *sched = nav_button(scr, "Schedule", sched_open_cb);
    lv_obj_set_pos(sched, W - PAD - 150, 8);

    s_dose_area = plain_obj(scr);
    lv_obj_set_size(s_dose_area, W, H);
    lv_obj_remove_flag(s_dose_area, LV_OBJ_FLAG_CLICKABLE);
    build_tiles(scr);

    s_status_label = label(scr, "", &inter_16_semibold, OVERDUE_RED);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* Long press = hold to undo: 1 s, as on the C5 (only taken rows listen for it) */
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        lv_indev_set_long_press_time(in, HOLD_UNDO_MS);
    }
    build_schedule_screen();
    show_splash();
    lv_timer_create(tick_cb, 1000, NULL);
    tick_cb(NULL);
}
