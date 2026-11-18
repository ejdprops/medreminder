/* Dose schedule and state. The schedule (name, period, time; up to DOSE_MAX, sorted by time) is
 * edited on the device and saved in NVS; until one is saved, the defaults below are used.
 * Taken doses are saved in NVS with the date, so a reboot doesn't forget them (or invite a
 * double dose); everything resets at midnight. A dose not taken OVERDUE_MIN after its time turns
 * overdue: the alert chime plays then and every ALERT_REPEAT_MIN until no dose is overdue. */
#include "doses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>

#include "audio.h"
#include "time_sync.h"

static const char *TAG = "doses";

#if CONFIG_MEDREMINDER_DOSE_TEST
/* Test schedule: doses 2 min apart, the first due when the clock is set (or at a dev reset), so
 * the first goes overdue after 1 min */
#define OVERDUE_MIN 1
#define ALERT_REPEAT_MIN 1
#else
#define OVERDUE_MIN 30
#define ALERT_REPEAT_MIN 5
#endif

#if CONFIG_MEDREMINDER_DOSE_TEST
#define NVS_NAMESPACE "doses_test" /* test runs never touch the real taken flags */
#else
#define NVS_NAMESPACE "doses"
#endif
#define NVS_KEY_DATE "date"
#define NVS_KEY_TAKEN "taken"
#define NVS_KEY_SCHEDULE "sched" /* blob: dose_def_t[count] */

const char *const dose_periods[DOSE_PERIOD_COUNT] = {"AM", "MID", "PM", "BED"};

/* Defaults for a new setup (nothing saved yet), one per period; rename/edit on the device */
static const dose_def_t s_default_schedule[] = {
    {"Dose", "AM", 7 * 60},
    {"Dose", "MID", 12 * 60},
    {"Dose", "PM", 18 * 60},
    {"Dose", "BED", 21 * 60},
};

static dose_def_t s_defs[DOSE_MAX];
static int s_count;
static uint32_t s_taken;          /* bit i: dose i taken today */
static uint32_t s_date;           /* YYYYMMDD the taken bits belong to; 0 until the clock is set */
static dose_state_t s_states[DOSE_MAX];
static time_t s_next_alert;       /* 0 = no alert pending */
static bool s_dirty;              /* changed outside doses_tick: report it on the next tick */
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
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void compute_states(time_t now, bool clock_ok)
{
    bool next_found = false;
    for (int i = 0; i < s_count; i++) {
        if (s_taken & (1u << i)) {
            s_states[i] = DOSE_TAKEN;
            continue;
        }
        if (clock_ok && now >= due_time(i, now) + OVERDUE_MIN * 60) {
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
    return a->minute == b->minute && strcmp(a->name, b->name) == 0 && strcmp(a->period, b->period) == 0;
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
    s_count = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_KEY_DATE, &s_date);
        nvs_get_u32(nvs, NVS_KEY_TAKEN, &s_taken);
        if (nvs_get_blob(nvs, NVS_KEY_SCHEDULE, s_defs, &len) == ESP_OK) {
            s_count = len % sizeof(dose_def_t) == 0 ? len / sizeof(dose_def_t) : 0; /* 1 byte: empty */
            saved = true;
        }
        nvs_close(nvs);
    }
    if (!saved) {
        s_count = sizeof(s_default_schedule) / sizeof(s_default_schedule[0]);
        memcpy(s_defs, s_default_schedule, sizeof(s_default_schedule));
        ESP_LOGI(TAG, "no saved schedule: using the default %d doses", s_count);
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

bool dose_taken(int i)
{
    return s_taken & (1u << i);
}

int doses_next(void)
{
    for (int i = 0; i < s_count; i++) {
        if (!(s_taken & (1u << i))) {
            return i;
        }
    }
    return 0;
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

void dose_format_time(int minute, char *buf, int len)
{
    int h = minute / 60, m = minute % 60;
    snprintf(buf, len, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
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
        if (i != index && s_defs[i].minute == d->minute && strcasecmp(s_defs[i].name, d->name) == 0) {
            char t[12];
            dose_format_time(d->minute, t, sizeof(t));
            snprintf(err, sizeof(err), "%.15s already has a dose at %.11s", s_defs[i].name, t);
            return err;
        }
    }
    if (index < 0) {
        index = s_count++;
        s_taken &= ~(1u << index);
    } else if (!same_dose(&s_defs[index], d)) {
        s_taken &= ~(1u << index); /* a changed dose starts not taken */
    }
    s_defs[index] = *d;
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

void doses_reset(void)
{
    s_taken = 0;
    s_next_alert = 0;
#if CONFIG_MEDREMINDER_DOSE_TEST
    if (s_test_base) {
        s_test_base = time(NULL);
    }
#endif
    save_taken();
    compute_states(time(NULL), time_is_valid());
    ESP_LOGI(TAG, "reset");
}

bool doses_tick(time_t now)
{
    bool changed = s_dirty;
    s_dirty = false;
    if (!time_is_valid()) {
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
        save_taken();
    }

    dose_state_t before[DOSE_MAX];
    memcpy(before, s_states, sizeof(before));
    compute_states(now, true);
    bool overdue = false;
    for (int i = 0; i < s_count; i++) {
        changed |= s_states[i] != before[i];
        if (s_states[i] == DOSE_OVERDUE) {
            overdue = true;
            if (before[i] != DOSE_OVERDUE) {
                ESP_LOGW(TAG, "overdue: %s %s", s_defs[i].name, s_defs[i].period);
            }
        }
    }

    if (!overdue) {
        s_next_alert = 0;
    } else if (!s_next_alert || now >= s_next_alert) {
        audio_play(AUDIO_ALERT);
        s_next_alert = now + ALERT_REPEAT_MIN * 60;
    }
    return changed;
}
