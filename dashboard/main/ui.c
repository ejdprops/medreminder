/* Dashboard screen, 800x480, dark: the C5's home layout made larger, plus a column of tiles.
 *   Top: date · 24 h time.
 *   Left: today's doses in their colors (taken rows get a check, overdue rows a red border); hold a
 *   taken row HOLD_UNDO_MS to undo it (after a confirm dialog), as on the C5.
 *   Middle: the next dose as a big card; tap it to take it (TAKE over the link, the C5 decides).
 *   Right: tiles (weather, calendar, garage, package box), placeholders until those steps.
 *   Bottom: a status line, only when the link or the C5 is down.
 * Dose states are worked out here from the node's snapshot exactly as the C5 does (doses.c). */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <esp_log.h>

#include "doses.h"
#include "fonts/fonts.h"
#include "link.h"

static const char *TAG = "ui";

#define W 800
#define H 480
#define PAD 16
#define BANNER_H 60
#define TOP (BANNER_H + 8)
#define BOTTOM (H - 36) /* status line below */
#define LIST_X PAD
#define LIST_W 300
#define LIST_ROWS 7
#define ROW_GAP 8
#define CARD_X (LIST_X + LIST_W + PAD)
#define CARD_W 236
#define TILES_X (CARD_X + CARD_W + PAD)
#define TILES_W (W - PAD - TILES_X)
#define TILE_GAP 8

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
#define HOLD_UNDO_MS 1000
#define DIALOG_BG 0x111827
#define UNDO_AMBER 0xfbbf24

static lv_obj_t *s_date_label, *s_time_label, *s_dose_area, *s_status_label, *s_card_hint;
static link_view_t s_view; /* static: off the LVGL task stack */
static dose_state_t s_states[DOSE_MAX];
static int s_next = -1;
static uint32_t s_shown_sig; /* what the dose area shows; rebuilt when it changes */
static uint8_t s_taking_uid;  /* tapped, waiting for the C5's new state */
static uint32_t s_taking_since;
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
    lv_obj_set_size(list, LIST_W, BOTTOM - TOP);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, ROW_GAP, 0);
    if (n == 0) {
        label(list, s_view.have_doses ? "No doses today" : "Waiting for the doses...", &inter_23_semibold, MUTED);
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
    int row_h = (BOTTOM - TOP - (LIST_ROWS - 1) * ROW_GAP) / LIST_ROWS;
    for (int k = first; k < first + rows; k++) {
        int i = idx[k];
        const dose_def_t *d = &s_view.doses.defs[i];
        uint32_t color = dose_color(i), fg = text_on(color);
        lv_obj_t *row = plain_obj(list);
        lv_obj_set_size(row, LV_PCT(100), row_h);
        lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(color), 0);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        if (s_states[i] == DOSE_OVERDUE) {
            overdue_border(row, 3);
        }
        char t[8];
        fmt_time(d->minute, t, sizeof(t));
        label(row, t, &inter_23_semibold, fg);
        lv_obj_t *name = label(row, d->name, &inter_23_semibold, fg);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_height(name, lv_font_get_line_height(&inter_23_semibold));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        if (s_states[i] == DOSE_TAKEN) {
            label(row, "✓", &inter_23_semibold, fg);
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
    label(card, "✓", &inter_48_bold, OK_GREEN);
    bool any = today_list((int[DOSE_MAX]){0}) > 0;
    lv_obj_t *big = label(card, any ? "All done\ntoday" : "No doses\ntoday", &inter_30_semibold, TEXT);
    lv_obj_set_flex_grow(big, 1);
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

static void build_card(lv_obj_t *area)
{
    lv_obj_t *card = plain_obj(area);
    lv_obj_set_pos(card, CARD_X, TOP);
    lv_obj_set_size(card, CARD_W, BOTTOM - TOP);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
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
        overdue_border(card, 5);
        lv_obj_set_style_pad_all(card, 14, 0);
    }
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, card_tap_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

    char t[8];
    fmt_time(d->minute, t, sizeof(t));
    label(card, t, &inter_48_bold, fg);

    lv_obj_t *mid = plain_obj(card);
    lv_obj_set_size(mid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 8, 0);
    lv_obj_t *name = label(mid, d->name, &inter_30_semibold, fg);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_height(name, 2 * lv_font_get_line_height(&inter_30_semibold));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    if (overdue) {
        lv_obj_t *badge = plain_obj(mid);
        lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
        lv_obj_set_style_pad_hor(badge, 12, 0);
        lv_obj_set_style_pad_ver(badge, 4, 0);
        int late = late_min(i);
        char text[24];
        if (late < 60) {
            snprintf(text, sizeof(text), "Late %d min", late);
        } else {
            snprintf(text, sizeof(text), "Late %dh %02dm", late / 60, late % 60);
        }
        label(badge, text, &inter_16_bold, LATE_TEXT);
    }
    s_card_hint = label(card, s_taking_uid ? "Taking..." : "Tap when taken", &inter_23_semibold, fg);
    lv_obj_set_width(s_card_hint, LV_PCT(100));
}

static void tile(lv_obj_t *area, int x, int y, int w, int h, const char *title, const char *body)
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
    lv_obj_t *b = label(t, body, &inter_16_semibold, TEXT);
    lv_obj_set_width(b, LV_PCT(100));
}

/* Placeholders until the tiles get their data (weather, Cozi: Wi-Fi; garage, package: the XIAO) */
static void build_tiles(lv_obj_t *scr)
{
    int h = BOTTOM - TOP;
    int weather_h = 120, small_h = 96;
    int cal_h = h - weather_h - small_h - 2 * TILE_GAP;
    int half = (TILES_W - TILE_GAP) / 2;
    tile(scr, TILES_X, TOP, TILES_W, weather_h, "WEATHER", "-");
    tile(scr, TILES_X, TOP + weather_h + TILE_GAP, TILES_W, cal_h, "CALENDAR", "-");
    int y = BOTTOM - small_h;
    tile(scr, TILES_X, y, half, small_h, "GARAGE", "-");
    tile(scr, TILES_X + half + TILE_GAP, y, half, small_h, "PACKAGE", "-");
}

static void rebuild_doses(void)
{
    lv_obj_clean(s_dose_area);
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
        s_taking_uid = 0; /* a new state: the take went through (or something else changed) */
    } else if (s_taking_uid && lv_tick_elaps(s_taking_since) > TAKE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "take of uid %u: no new state in %d s", s_taking_uid, TAKE_TIMEOUT_MS / 1000);
        s_taking_uid = 0;
        changed = true; /* back to "Tap when taken" */
    }
    compute_states();
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
}

void ui_start(void)
{
    lv_obj_t *scr = lv_screen_active();
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
    lv_timer_create(tick_cb, 1000, NULL);
    tick_cb(NULL);
}
