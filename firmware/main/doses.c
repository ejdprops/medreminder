/* Dose schedule and state. The schedule (name, period, time; up to DOSE_MAX, sorted by time) is
 * edited on the device and saved in NVS; until one is saved, the defaults below are used.
 * Taken doses are saved in NVS with the date, so a reboot doesn't forget them (or invite a
 * double dose); everything resets at midnight. A dose not taken s_overdue_min after its time
 * turns overdue (Settings > Overdue after, OVERDUE_MIN until set): the alert chime plays then and every ALERT_REPEAT_MIN until no dose is overdue.
 * A snooze (doses_snooze: the C5's card, the dashboard) silences it for SNOOZE_S, at most
 * DOSE_SNOOZE_MAX times per overdue spell; when it ends the chime plays and Home notifies again. */
#include "doses.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>

#include "audio.h"
#include "matter_dev.h"
#include "node_sync.h"
#include "time_sync.h"
#include "uart_link.h"

static const char *TAG = "doses";

#if CONFIG_MEDREMINDER_DOSE_TEST
/* Test schedule: doses 2 min apart, the first due when the clock is set (or at boot), so
 * the first goes overdue after 1 min */
#define OVERDUE_MIN 1
#define ALERT_REPEAT_MIN 1
#define SNOOZE_S (2 * 60)
#else
#define OVERDUE_MIN 30
#define ALERT_REPEAT_MIN 5
#define SNOOZE_S (DOSE_SNOOZE_MIN * 60)
#endif

#if CONFIG_MEDREMINDER_DOSE_TEST
#define NVS_NAMESPACE "doses_test" /* test runs never touch the real taken flags */
#else
#define NVS_NAMESPACE "doses"
#endif
#define NVS_KEY_DATE "date"
#define NVS_KEY_TAKEN "taken"
#define NVS_KEY_SCHEDULE "sched2" /* blob: dose_def_t[count] */
#define NVS_KEY_SCHEDULE_V1 "sched" /* before colors: dose_def_v1_t[count], migrated on load */
#define NVS_KEY_OVERDUE "overdue"   /* u8: minutes after the time a dose turns overdue */
#define NVS_KEY_UID "uid"           /* u8: next dose uid to try (uids aren't reused soon) */

typedef struct {
    char name[DOSE_NAME_MAX + 1];
    char period[8];
    int minute;
} dose_def_v1_t;

const char *const dose_periods[DOSE_PERIOD_COUNT] = {"AM", "MID", "PM", "BED"};

/* Starting color for doses that have none yet (defaults, migrated schedules): by period */
static uint8_t period_color(const char *period)
{
    static const uint8_t by_period[DOSE_PERIOD_COUNT] = {5, 6, 0, 2}; /* amber, teal, royal blue, purple */
    for (int p = 0; p < DOSE_PERIOD_COUNT; p++) {
        if (strcmp(period, dose_periods[p]) == 0) {
            return by_period[p];
        }
    }
    return 7;
}

/* Defaults for a new setup (nothing saved yet), one per period; rename/edit on the device */
static const dose_def_t s_default_schedule[] = {
    {"Dose", "AM", 7 * 60, 5, DOSE_OPACITY_DEFAULT, DOSE_DAILY, 1},
    {"Dose", "MID", 12 * 60, 6, DOSE_OPACITY_DEFAULT, DOSE_DAILY, 2},
    {"Dose", "PM", 18 * 60, 0, DOSE_OPACITY_DEFAULT, DOSE_DAILY, 3},
    {"Dose", "BED", 21 * 60, 2, DOSE_OPACITY_DEFAULT, DOSE_DAILY, 4},
};

static dose_def_t s_defs[DOSE_MAX];
static int s_count;
static uint32_t s_taken;          /* bit i: dose i taken today */
static uint32_t s_date;           /* YYYYMMDD the taken bits belong to; 0 until the clock is set */
static dose_state_t s_states[DOSE_MAX];
static time_t s_next_alert;       /* 0 = no alert pending */
static time_t s_snooze_end;       /* main unit: 0 = not snoozed */
static int s_snoozes;             /* main unit: used in this overdue spell */
static uint16_t s_snooze_code;    /* node: as the main unit sent it */
static uint8_t s_today = DOSE_DAILY; /* today's weekday bit; all days until the clock is set */
static bool s_dirty;              /* changed outside doses_tick: report it on the next tick */
static uint8_t s_uid_next = 1;    /* next uid to try */
static int s_overdue_min = OVERDUE_MIN;
#if CONFIG_MEDREMINDER_DOSE_TEST
static time_t s_test_base;        /* test schedule start; 0 until the clock is set */
#endif

static uint32_t date_of(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (uint32_t)((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}

static time_t due_time(int i, time_t now)
{
#if CONFIG_MEDREMINDER_DOSE_TEST
    (void)now;
    return s_test_base + i * 120;
#else
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = s_defs[i].minute / 60;
    tm.tm_min = s_defs[i].minute % 60;
    tm.tm_sec = 0;
    tm.tm_isdst = -1; /* let mktime work out DST for that time */
    return mktime(&tm);
#endif
}

static void save_taken(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    nvs_set_u32(nvs, NVS_KEY_DATE, s_date);
    nvs_set_u32(nvs, NVS_KEY_TAKEN, s_taken);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void save_schedule(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    /* NVS rejects a zero-length blob: store one zeroed byte for "every dose deleted" */
    static const uint8_t empty = 0;
    if (s_count > 0) {
        nvs_set_blob(nvs, NVS_KEY_SCHEDULE, s_defs, s_count * sizeof(dose_def_t));
    } else {
        nvs_set_blob(nvs, NVS_KEY_SCHEDULE, &empty, 1);
    }
    nvs_set_u32(nvs, NVS_KEY_TAKEN, s_taken);
    nvs_set_u8(nvs, NVS_KEY_UID, s_uid_next);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static bool uid_used(uint8_t uid, int except)
{
    for (int i = 0; i < s_count; i++) {
        if (i != except && s_defs[i].uid == uid) {
            return true;
        }
    }
    return false;
}

/* A uid no other dose has, counting up so a deleted dose's uid (and its Home accessory) isn't
 * handed to the next new dose. 1-255; DOSE_MAX doses can't use them all. */
static uint8_t new_uid(int except)
{
    while (true) {
        uint8_t uid = s_uid_next;
        s_uid_next = s_uid_next == 255 ? 1 : s_uid_next + 1;
        if (!uid_used(uid, except)) {
            return uid;
        }
    }
}

static bool due_today(int i)
{
    return s_defs[i].days & s_today;
}

static void compute_states(time_t now, bool clock_ok)
{
    if (clock_ok) {
        struct tm tm;
        localtime_r(&now, &tm);
        s_today = 1u << tm.tm_wday;
    }
    bool next_found = false;
    for (int i = 0; i < s_count; i++) {
        if (!due_today(i)) {
            s_states[i] = DOSE_LATER; /* not shown today */
            continue;
        }
        if (s_taken & (1u << i)) {
            s_states[i] = DOSE_TAKEN;
            continue;
        }
        if (clock_ok && now >= due_time(i, now) + s_overdue_min * 60) {
            s_states[i] = DOSE_OVERDUE;
        } else {
            s_states[i] = next_found ? DOSE_LATER : DOSE_NEXT;
        }
        next_found = true;
    }
}

static int cmp_def(const void *a, const void *b)
{
    const dose_def_t *x = a, *y = b;
    if (x->minute != y->minute) {
        return x->minute - y->minute;
    }
    return strcmp(x->name, y->name);
}

static bool same_dose(const dose_def_t *a, const dose_def_t *b)
{
    return a->minute == b->minute && a->days == b->days && strcmp(a->name, b->name) == 0 &&
           strcmp(a->period, b->period) == 0;
}

/* Sort s_defs by time, carrying today's taken flags along with their doses */
static void sort_keep_taken(void)
{
    dose_def_t old[DOSE_MAX];
    memcpy(old, s_defs, sizeof(old));
    uint32_t old_taken = s_taken;
    qsort(s_defs, s_count, sizeof(dose_def_t), cmp_def);
    s_taken = 0;
    for (int i = 0; i < s_count; i++) {
        for (int j = 0; j < s_count; j++) {
            if ((old_taken & (1u << j)) && same_dose(&s_defs[i], &old[j])) {
                s_taken |= 1u << i;
                break;
            }
        }
    }
}

void doses_init(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(s_defs);
    bool saved = false; /* an empty saved schedule (every dose deleted) stays empty */
    bool migrated = false;
    s_count = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_KEY_DATE, &s_date);
        nvs_get_u32(nvs, NVS_KEY_TAKEN, &s_taken);
        nvs_get_u8(nvs, NVS_KEY_UID, &s_uid_next);
#if !CONFIG_MEDREMINDER_DOSE_TEST
        uint8_t overdue;
        if (nvs_get_u8(nvs, NVS_KEY_OVERDUE, &overdue) == ESP_OK && overdue > 0) {
            s_overdue_min = overdue;
        }
#endif
        if (nvs_get_blob(nvs, NVS_KEY_SCHEDULE, s_defs, &len) == ESP_OK) {
            s_count = len % sizeof(dose_def_t) == 0 ? len / sizeof(dose_def_t) : 0; /* 1 byte: empty */
            saved = true;
        } else {
            static dose_def_v1_t v1[DOSE_MAX]; /* static: keep it off the UI task stack */
            len = sizeof(v1);
            if (nvs_get_blob(nvs, NVS_KEY_SCHEDULE_V1, v1, &len) == ESP_OK) {
                s_count = len % sizeof(dose_def_v1_t) == 0 ? len / sizeof(dose_def_v1_t) : 0;
                for (int i = 0; i < s_count; i++) {
                    memcpy(s_defs[i].name, v1[i].name, sizeof(s_defs[i].name));
                    memcpy(s_defs[i].period, v1[i].period, sizeof(s_defs[i].period));
                    s_defs[i].minute = v1[i].minute;
                    s_defs[i].color = period_color(v1[i].period);
                    s_defs[i].opacity = DOSE_OPACITY_DEFAULT;
                    s_defs[i].days = DOSE_DAILY;
                }
                saved = migrated = true;
            }
        }
        nvs_close(nvs);
    }
    if (!saved) {
        s_count = sizeof(s_default_schedule) / sizeof(s_default_schedule[0]);
        memcpy(s_defs, s_default_schedule, sizeof(s_default_schedule));
        ESP_LOGI(TAG, "no saved schedule: using the default %d doses", s_count);
    }
    /* uids: saved before they existed (0, or whatever the padding byte held) or duplicated */
    if (s_uid_next == 0) {
        s_uid_next = 1;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_defs[i].uid >= s_uid_next) {
            s_uid_next = s_defs[i].uid == 255 ? 1 : s_defs[i].uid + 1;
        }
    }
    for (int i = 0; i < s_count; i++) {
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) {
            dup = s_defs[j].uid == s_defs[i].uid;
        }
        if (s_defs[i].uid == 0 || dup) {
            s_defs[i].uid = new_uid(i);
            migrated |= saved;
        }
    }
    if (migrated) {
        ESP_LOGI(TAG, "updated the saved schedule (%d doses)", s_count);
        save_schedule();
    }
    for (int i = 0; i < s_count; i++) {
        if (s_defs[i].color >= DOSE_COLOR_COUNT) {
            s_defs[i].color = period_color(s_defs[i].period);
        }
        if (!(s_defs[i].days & DOSE_DAILY)) {
            s_defs[i].days = DOSE_DAILY; /* saved before weekly doses existed (padding byte) */
        }
        if (s_defs[i].opacity < DOSE_OPACITY_MIN || s_defs[i].opacity > DOSE_OPACITY_MAX) {
            s_defs[i].opacity = DOSE_OPACITY_DEFAULT; /* saved before opacity existed (padding byte) */
        }
    }
    s_taken &= (s_count < 32 ? (1u << s_count) : 0) - 1;
    ESP_LOGI(TAG, "%d doses; saved date %u, taken 0x%02x", s_count, (unsigned)s_date, (unsigned)s_taken);
    compute_states(0, false);
}

int doses_count(void)
{
    return s_count;
}

const dose_def_t *dose_def(int i)
{
    return &s_defs[i];
}

const char *dose_name(int i)
{
    return s_defs[i].name;
}

const char *dose_slot(int i)
{
    return s_defs[i].period;
}

dose_state_t dose_state(int i)
{
    return s_states[i];
}

bool dose_due_today(int i)
{
    return due_today(i);
}

bool dose_taken(int i)
{
    return s_taken & (1u << i);
}

int doses_next(void)
{
    for (int i = 0; i < s_count; i++) {
        if (due_today(i) && !(s_taken & (1u << i))) {
            return i;
        }
    }
    return -1;
}

int doses_find_uid(uint8_t uid)
{
    for (int i = 0; i < s_count; i++) {
        if (s_defs[i].uid == uid) {
            return i;
        }
    }
    return -1;
}

int doses_first_on(int wday)
{
    for (int i = 0; i < s_count; i++) {
        if (s_defs[i].days & (1u << wday)) {
            return i;
        }
    }
    return -1;
}

bool doses_take(int i)
{
    if (i < 0 || i >= s_count || (s_taken & (1u << i))) {
        return false;
    }
    s_taken |= 1u << i;
    save_taken();
    compute_states(time(NULL), time_is_valid());
    ESP_LOGI(TAG, "taken: %s %s", s_defs[i].name, s_defs[i].period);
    return true;
}

bool doses_untake(int i)
{
    if (i < 0 || i >= s_count || !(s_taken & (1u << i))) {
        return false;
    }
    s_taken &= ~(1u << i);
    save_taken();
    compute_states(time(NULL), time_is_valid());
    ESP_LOGI(TAG, "undo: %s %s", s_defs[i].name, s_defs[i].period);
    return true;
}

int doses_overdue_min(void)
{
    return s_overdue_min;
}

void doses_set_overdue_min(int minutes)
{
    if (minutes < 1 || minutes > 255 || minutes == s_overdue_min) {
        return;
    }
    s_overdue_min = minutes;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_OVERDUE, (uint8_t)minutes);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    compute_states(time(NULL), time_is_valid());
    s_dirty = true;
    ESP_LOGI(TAG, "overdue after %d min", minutes);
}

bool doses_any_due(time_t now)
{
    if (!time_is_valid()) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (due_today(i) && !(s_taken & (1u << i)) && now >= due_time(i, now)) {
            return true;
        }
    }
    return false;
}

void dose_format_time(int minute, char *buf, int len)
{
    snprintf(buf, len, "%02d%02d", minute / 60, minute % 60); /* 24 h, military style: "0600" */
}

const char *doses_set(int index, const dose_def_t *d)
{
    static char err[80];
    if (d->name[0] == '\0') {
        return "Enter a name";
    }
    if (index < 0 && s_count >= DOSE_MAX) {
        snprintf(err, sizeof(err), "Schedule is full (%d doses)", DOSE_MAX);
        return err;
    }
    for (int i = 0; i < s_count; i++) {
        if (i != index && s_defs[i].minute == d->minute && (s_defs[i].days & d->days) &&
            strcasecmp(s_defs[i].name, d->name) == 0) {
            char t[12];
            dose_format_time(d->minute, t, sizeof(t));
            snprintf(err, sizeof(err), "%.15s already has a dose at %.11s", s_defs[i].name, t);
            return err;
        }
    }
    uint8_t uid;
    if (index < 0) {
        index = s_count++;
        s_taken &= ~(1u << index);
        uid = new_uid(index);
    } else {
        uid = s_defs[index].uid;
        if (!same_dose(&s_defs[index], d)) {
            s_taken &= ~(1u << index); /* a changed dose starts not taken */
        }
    }
    s_defs[index] = *d;
    s_defs[index].uid = uid;
    sort_keep_taken();
    save_schedule();
    compute_states(time(NULL), time_is_valid());
    s_dirty = true;
    ESP_LOGI(TAG, "saved dose: %s %s %d:%02d (%d doses)", d->name, d->period, d->minute / 60, d->minute % 60,
             s_count);
    return NULL;
}

void doses_delete(int index)
{
    if (index < 0 || index >= s_count) {
        return;
    }
    ESP_LOGI(TAG, "deleted dose: %s %s", s_defs[index].name, s_defs[index].period);
    /* Shift the dose definitions and the taken bits above index down by one */
    uint32_t low = s_taken & ((1u << index) - 1);
    uint32_t high = (s_taken >> (index + 1)) << index;
    s_taken = low | high;
    memmove(&s_defs[index], &s_defs[index + 1], (s_count - index - 1) * sizeof(dose_def_t));
    s_count--;
    save_schedule();
    compute_states(time(NULL), time_is_valid());
    s_dirty = true;
}

void doses_get_snapshot(doses_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->date = s_date;
    snap->taken = s_taken;
    snap->count = (uint8_t)s_count;
    snap->overdue_min = (uint8_t)s_overdue_min;
    snap->snooze = s_snooze_code;
    memcpy(snap->defs, s_defs, s_count * sizeof(dose_def_t));
}

void doses_apply_snapshot(const doses_snapshot_t *snap)
{
    s_count = (snap->count < DOSE_MAX ? snap->count : DOSE_MAX);
    memcpy(s_defs, snap->defs, s_count * sizeof(dose_def_t));
    s_taken = snap->taken & ((s_count < 32 ? (1u << s_count) : 0) - 1);
    s_date = snap->date;
    if (snap->overdue_min > 0) {
        s_overdue_min = snap->overdue_min;
    }
    s_snooze_code = snap->snooze;
    save_schedule();
    save_taken();
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_OVERDUE, (uint8_t)s_overdue_min);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    compute_states(time(NULL), time_is_valid());
    s_dirty = true;
    ESP_LOGI(TAG, "synced from the main unit: %d doses, date %u, taken 0x%02x", s_count, (unsigned)s_date,
             (unsigned)s_taken);
}

/* Apple Home's view: one accessory per dose, "<name> <time>" (+ weekday for weekly doses), on
 * when taken today; plus whether any dose is overdue */
static void publish(void)
{
    static const char *const wday[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    matter_dose_t doses[DOSE_MAX];
    bool overdue = false;
    for (int i = 0; i < s_count; i++) {
        const dose_def_t *d = &s_defs[i];
        doses[i].uid = d->uid;
        doses[i].on = due_today(i) && (s_taken & (1u << i));
        int day = -1;
        for (int w = 0; w < 7 && d->days != DOSE_DAILY; w++) {
            if (d->days & (1u << w)) {
                day = w;
            }
        }
        char t[8];
        dose_format_time(d->minute, t, sizeof(t));
        memset(doses[i].label, 0, sizeof(doses[i].label)); /* compared with memcmp */
        snprintf(doses[i].label, sizeof(doses[i].label), "%.15s %.4s%s%.3s", d->name, t, day >= 0 ? " " : "",
                 day >= 0 ? wday[day] : "");
        /* Serial: first letter of the name, the time, the weekday: "K-0900-TUE" */
        memset(doses[i].serial, 0, sizeof(doses[i].serial));
        snprintf(doses[i].serial, sizeof(doses[i].serial), "%c-%.4s%s%.3s", toupper((unsigned char)d->name[0]), t,
                 day >= 0 ? "-" : "", day >= 0 ? wday[day] : "");
        overdue |= s_states[i] == DOSE_OVERDUE;
    }
    matter_dev_publish(doses, s_count, overdue);
}

/* Take/undo from Home (and takes from nodes). Only doses due today; anything else is refused and publish() sets Home's
 * switch back. */
static bool apply_home_requests(void)
{
    bool changed = false;
    uint8_t uid;
    bool on;
    while (matter_dev_next_request(&uid, &on)) {
        for (int i = 0; i < s_count; i++) {
            if (s_defs[i].uid == uid && due_today(i)) {
                changed |= on ? doses_take(i) : doses_untake(i);
            }
        }
    }
    /* Taken, undone or snoozed from a node (shake, dashboard); same rules */
    node_req_t req;
    while (node_sync_next_request(&uid, &req)) {
        if (req == NODE_REQ_SNOOZE) {
            changed |= doses_snooze(time(NULL));
            continue;
        }
        for (int i = 0; i < s_count; i++) {
            if (s_defs[i].uid == uid && due_today(i)) {
                changed |= req == NODE_REQ_TAKE ? doses_take(i) : doses_untake(i);
            }
        }
    }
    return changed;
}

/* Node: the main unit's newest state, if one arrived */
static void apply_sync(void)
{
#if CONFIG_MEDREMINDER_ROLE_NODE
    static doses_snapshot_t snap; /* static: keep it off the UI task stack */
    if (node_sync_take_state(&snap)) {
        doses_apply_snapshot(&snap);
    }
#endif
}

/* ---- snooze ---- */

#if !CONFIG_MEDREMINDER_ROLE_NODE
static bool any_overdue(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_states[i] == DOSE_OVERDUE) {
            return true;
        }
    }
    return false;
}
#endif

/* Main unit: what the snapshot carries (nodes keep the main unit's) */
static void update_snooze_code(void)
{
#if !CONFIG_MEDREMINDER_ROLE_NODE
    int until = -1;
    if (s_snooze_end) {
        struct tm tm;
        localtime_r(&s_snooze_end, &tm);
        until = tm.tm_hour * 60 + tm.tm_min;
    }
    uint16_t code = DOSE_SNOOZE_CODE(until, DOSE_SNOOZE_MAX - s_snoozes);
    if (code != s_snooze_code) {
        s_snooze_code = code;
        s_dirty = true;
    }
#endif
}

bool doses_snooze(time_t now)
{
#if CONFIG_MEDREMINDER_ROLE_NODE
    (void)now;
    return false;
#else
    /* Not while one runs: a node repeats its request until the new state arrives */
    if (!any_overdue() || s_snooze_end || s_snoozes >= DOSE_SNOOZE_MAX) {
        return false;
    }
    s_snoozes++;
    s_snooze_end = now + SNOOZE_S;
    s_next_alert = s_snooze_end; /* the chime comes back when it ends */
    update_snooze_code();
    ESP_LOGI(TAG, "snoozed until %02d%02d (%d of %d)", DOSE_SNOOZE_UNTIL(s_snooze_code) / 60,
             DOSE_SNOOZE_UNTIL(s_snooze_code) % 60, s_snoozes, DOSE_SNOOZE_MAX);
    return true;
#endif
}

int doses_snooze_until(void)
{
    return DOSE_SNOOZE_UNTIL(s_snooze_code);
}

int doses_snoozes_left(void)
{
    return DOSE_SNOOZE_LEFT(s_snooze_code);
}

/* What this device holds, for node sync (the main unit sends it; a node reports it back) and a
 * dashboard panel on the UART link */
static void publish_sync(void)
{
    static doses_snapshot_t snap;
    doses_get_snapshot(&snap);
    node_sync_publish(&snap);
    uart_link_publish(&snap);
}

bool doses_tick(time_t now)
{
    apply_sync();
    bool changed = s_dirty | apply_home_requests();
    s_dirty = false;
    if (!time_is_valid()) {
        publish();
        publish_sync();
        return changed;
    }
#if CONFIG_MEDREMINDER_DOSE_TEST
    if (!s_test_base) {
        s_test_base = now;
        s_taken = 0;
        ESP_LOGI(TAG, "test schedule: first dose due now, then every 2 min; overdue after 1 min");
    }
#endif
    /* New day (or first valid clock with taken bits from another day): start fresh */
    uint32_t today = date_of(now);
    if (today != s_date) {
        ESP_LOGI(TAG, "new day %u (saved %u): all doses not taken", (unsigned)today, (unsigned)s_date);
        s_date = today;
        s_taken = 0;
        s_next_alert = 0;
        s_snooze_end = 0;
        s_snoozes = 0;
        save_taken();
    }

    dose_state_t before[DOSE_MAX];
    memcpy(before, s_states, sizeof(before));
    compute_states(now, true);
    bool overdue = false, newly_overdue = false;
    for (int i = 0; i < s_count; i++) {
        changed |= s_states[i] != before[i];
        if (s_states[i] == DOSE_OVERDUE) {
            overdue = true;
            if (before[i] != DOSE_OVERDUE) {
                newly_overdue = true;
                ESP_LOGW(TAG, "overdue: %s %s", s_defs[i].name, s_defs[i].period);
            }
        }
    }

    if (!overdue) {
        s_next_alert = 0;
        s_snooze_end = 0; /* the spell is over: a later one gets all its snoozes */
        s_snoozes = 0;
    } else {
        if (s_snooze_end && newly_overdue) {
            ESP_LOGI(TAG, "another dose is overdue: snooze over");
            s_snooze_end = 0;
            s_next_alert = 0;
        } else if (s_snooze_end && now >= s_snooze_end) {
            ESP_LOGI(TAG, "snooze over, %d left", DOSE_SNOOZE_MAX - s_snoozes);
            s_snooze_end = 0;
            matter_dev_renotify(); /* Home: another notification */
        }
        if (!s_snooze_end && (!s_next_alert || now >= s_next_alert)) {
            audio_play(AUDIO_ALERT);
            s_next_alert = now + ALERT_REPEAT_MIN * 60;
        }
    }
    update_snooze_code();
    changed |= s_dirty; /* the snooze changed */
    s_dirty = false;

    publish();
    publish_sync();
    return changed;
}
