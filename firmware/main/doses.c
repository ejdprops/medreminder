/* Dose schedule and state. Placeholder schedule until real data comes from Home/Matter.
 * Taken doses are saved in NVS with the date, so a reboot doesn't forget them (or invite a
 * double dose); everything resets at midnight. A dose not taken OVERDUE_MIN after its time turns
 * overdue: the alert chime plays then and every ALERT_REPEAT_MIN until no dose is overdue. */
#include "doses.h"

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

typedef struct {
    const char *name;
    const char *slot;
    int minute; /* time of day, minutes after midnight */
} dose_def_t;

static const dose_def_t s_defs[] = {
    {"Evan", "AM", 6 * 60},
    {"Karen", "AM", 9 * 60},
    {"Karen", "MID", 13 * 60 + 30},
    {"Karen", "PM", 19 * 60},
    {"Karen", "BED", 21 * 60},
};
#define COUNT ((int)(sizeof(s_defs) / sizeof(s_defs[0])))

static uint32_t s_taken;          /* bit i: dose i taken today */
static uint32_t s_date;           /* YYYYMMDD the taken bits belong to; 0 until the clock is set */
static dose_state_t s_states[COUNT];
static time_t s_next_alert;       /* 0 = no alert pending */
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

static void save(void)
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

/* States without a clock: taken or not, nothing overdue */
static void compute_states(time_t now, bool clock_ok)
{
    bool next_found = false;
    for (int i = 0; i < COUNT; i++) {
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

void doses_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, NVS_KEY_DATE, &s_date);
        nvs_get_u32(nvs, NVS_KEY_TAKEN, &s_taken);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "saved: date %u, taken 0x%02x", (unsigned)s_date, (unsigned)s_taken);
    compute_states(0, false);
}

int doses_count(void)
{
    return COUNT;
}

const char *dose_name(int i)
{
    return s_defs[i].name;
}

const char *dose_slot(int i)
{
    return s_defs[i].slot;
}

dose_state_t dose_state(int i)
{
    return s_states[i];
}

int doses_next(void)
{
    for (int i = 0; i < COUNT; i++) {
        if (!(s_taken & (1u << i))) {
            return i;
        }
    }
    return 0;
}

bool doses_take(int i)
{
    if (s_taken & (1u << i)) {
        return false;
    }
    s_taken |= 1u << i;
    save();
    time_t now = time(NULL);
    compute_states(now, time_is_valid());
    ESP_LOGI(TAG, "taken: %s %s", s_defs[i].name, s_defs[i].slot);
    return true;
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
    save();
    compute_states(time(NULL), time_is_valid());
    ESP_LOGI(TAG, "reset");
}

bool doses_tick(time_t now)
{
    if (!time_is_valid()) {
        return false;
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
        save();
    }

    dose_state_t before[COUNT];
    for (int i = 0; i < COUNT; i++) {
        before[i] = s_states[i];
    }
    compute_states(now, true);
    bool changed = false;
    bool overdue = false;
    for (int i = 0; i < COUNT; i++) {
        changed |= s_states[i] != before[i];
        if (s_states[i] == DOSE_OVERDUE) {
            overdue = true;
            if (before[i] != DOSE_OVERDUE) {
                ESP_LOGW(TAG, "overdue: %s %s", s_defs[i].name, s_defs[i].slot);
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
