/* Calendar tile data: the family's Cozi calendar as an iCalendar feed (DASH_COZI_URL in secrets.h,
 * never logged). Fetched whole into PSRAM (~120 KB) every CAL_EVERY_S and expanded into the
 * occurrences of the next CAL_DAYS days.
 *
 * Only what Cozi's feed uses is supported (checked against the real feed, 2026-11):
 *   DTSTART/DTEND as a date (all day, DTEND exclusive) or a local date-time with
 *   TZID=America/Chicago (= the panel's TZ, so taken as local) or UTC "Z";
 *   RRULE FREQ=DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, UNTIL (UTC), BYDAY (WEEKLY: weekdays;
 *   MONTHLY/YEARLY: "2SU", "-1FR"), BYMONTHDAY, BYMONTH; EXDATE. Every title starts with the member
 *   it's for ("Evan:", "Karen:", "All:"): kept as an initial, shown as a colored badge. No COUNT or RECURRENCE-ID in the
 *   feed: an event with COUNT is shown without the limit (logged once). */
#include "calendar.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "wifi.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "calendar";

#define CAL_EVERY_S (15 * 60)
#define RETRY_S 60
#define WAIT_S 2
#define FEED_MAX (1024 * 1024)
#define EXDATE_MAX 24
#define USER_AGENT "MedReminder-Dashboard/1.0" /* Cozi answers 403 without a User-Agent */

static SemaphoreHandle_t s_lock;
static calendar_t s_cal, s_work; /* s_work: built by the task, then copied under the lock */

/* ---- days ---- */

/* Days since 1970-01-01 (proleptic Gregorian; H. Hinnant's days_from_civil) */
int32_t cal_day(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int weekday(int32_t day) /* 0 = Sunday */
{
    return (int)(((day % 7) + 7 + 4) % 7);
}

static int days_in_month(int y, int m)
{
    return cal_day(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - cal_day(y, m, 1);
}

int32_t cal_today(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return cal_day(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* ---- parsing ---- */

typedef struct {
    int32_t day; /* local */
    int16_t min; /* -1 = date only */
} when_t;

/* "20261121", "20261121T093000" (local), "20261121T153000Z" (UTC) */
static bool parse_when(const char *v, when_t *w)
{
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) {
        return false;
    }
    if (v[8] != 'T') {
        *w = (when_t){cal_day(y, mo, d), -1};
        return true;
    }
    if (sscanf(v + 9, "%2d%2d%2d", &h, &mi, &s) < 2) {
        return false;
    }
    if (v[15] == 'Z') {
        time_t t = (time_t)cal_day(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
        struct tm tm;
        localtime_r(&t, &tm);
        *w = (when_t){cal_day(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday), (int16_t)(tm.tm_hour * 60 + tm.tm_min)};
    } else {
        *w = (when_t){cal_day(y, mo, d), (int16_t)(h * 60 + mi)};
    }
    return true;
}

static const char *const s_wd[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

typedef struct {
    char freq; /* 0 none, 'D' 'W' 'M' 'Y' */
    int interval;
    bool has_until;
    when_t until;
    uint8_t wdays;  /* BYDAY weekdays (bit 0 = Sunday) */
    int8_t nth;     /* BYDAY position (MONTHLY/YEARLY "2SU" = 2, "-1FR" = -1; 0 = any) */
    int8_t monthday;
    int8_t month;
    bool count;
} rrule_t;

static void parse_rrule(char *v, rrule_t *r)
{
    *r = (rrule_t){.interval = 1};
    for (char *save, *p = strtok_r(v, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq++ = 0;
        if (!strcmp(p, "FREQ")) {
            r->freq = !strcmp(eq, "DAILY") ? 'D' : !strcmp(eq, "WEEKLY") ? 'W' : !strcmp(eq, "MONTHLY") ? 'M'
                                                                         : !strcmp(eq, "YEARLY")  ? 'Y'
                                                                                                  : '?';
        } else if (!strcmp(p, "INTERVAL")) {
            r->interval = atoi(eq) > 0 ? atoi(eq) : 1;
        } else if (!strcmp(p, "UNTIL")) {
            r->has_until = parse_when(eq, &r->until);
        } else if (!strcmp(p, "BYMONTHDAY")) {
            r->monthday = (int8_t)atoi(eq);
        } else if (!strcmp(p, "BYMONTH")) {
            r->month = (int8_t)atoi(eq);
        } else if (!strcmp(p, "COUNT")) {
            r->count = true;
        } else if (!strcmp(p, "BYDAY")) {
            for (char *s2, *d = strtok_r(eq, ",", &s2); d; d = strtok_r(NULL, ",", &s2)) {
                int n = 0, len = (int)strlen(d);
                if (len > 2) {
                    n = atoi(d);
                }
                for (int k = 0; k < 7 && len >= 2; k++) {
                    if (!strcmp(d + len - 2, s_wd[k])) {
                        r->wdays |= 1u << k;
                    }
                }
                r->nth = (int8_t)n;
            }
        }
    }
}

/* Does a recurring event that started on `base` have an occurrence on `day`? */
static bool occurs(const rrule_t *r, int32_t base, int32_t day)
{
    if (day < base) {
        return false;
    }
    int by, bm, bd, y, m, d;
    civil(base, &by, &bm, &bd);
    civil(day, &y, &m, &d);
    int wd = weekday(day);
    switch (r->freq) {
    case 'D':
        return (day - base) % r->interval == 0;
    case 'W': {
        uint8_t days = r->wdays ? r->wdays : 1u << weekday(base);
        /* Weeks start on Monday (WKST default) */
        int32_t wk_base = base - (weekday(base) + 6) % 7, wk = day - (wd + 6) % 7;
        return (days & (1u << wd)) && ((wk - wk_base) / 7) % r->interval == 0;
    }
    case 'M':
    case 'Y': {
        int steps = r->freq == 'M' ? (y - by) * 12 + (m - bm) : y - by;
        if (steps % r->interval) {
            return false;
        }
        if (r->freq == 'Y' && m != (r->month ? r->month : bm)) {
            return false;
        }
        if (r->wdays) {
            if (!(r->wdays & (1u << wd))) {
                return false;
            }
            if (r->nth > 0) {
                return (d - 1) / 7 + 1 == r->nth;
            }
            if (r->nth < 0) {
                return (days_in_month(y, m) - d) / 7 + 1 == -r->nth;
            }
            return true;
        }
        return d == (r->monthday ? r->monthday : bd);
    }
    default:
        return false;
    }
}

/* Unescape iCalendar text (\, \; \n \\) and fold it to ASCII for the fonts */
static void clean_title(const char *in, char *out, int len)
{
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n < len - 4;) {
        unsigned c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            out[n++] = (c == 'n' || c == 'N') ? ' ' : (char)c;
            continue;
        }
        if (c < 0x80) {
            if (c >= 0x20) {
                out[n++] = (char)c;
            }
            continue;
        }
        /* UTF-8 */
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        unsigned cp = c & (0x3F >> extra);
        for (int k = 0; k < extra && (*p & 0xC0) == 0x80; k++) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp >= 0xC0 && cp <= 0xFF) {
            out[n++] = latin1[cp - 0xC0];
        } else if (cp == 0x2018 || cp == 0x2019) {
            out[n++] = '\'';
        } else if (cp == 0x201C || cp == 0x201D) {
            out[n++] = '"';
        } else if (cp == 0x2013 || cp == 0x2014) {
            out[n++] = '-';
        } else if (cp == 0x2026) {
            out[n++] = '.', out[n++] = '.', out[n++] = '.';
        } else if (cp == 0xA0) {
            out[n++] = ' ';
        } /* anything else (emoji, symbols): dropped */
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    int start = 0;
    while (start < n && out[start] == ' ') {
        start++;
    }
    memmove(out, out + start, n - start);
    out[n - start] = 0;
}

typedef struct {
    when_t start, end;
    bool has_start, has_end;
    rrule_t rule;
    int32_t exdate[EXDATE_MAX];
    int n_exdate;
    char title[CAL_TITLE];
} vevent_t;

static void add(calendar_t *c, int32_t first, int32_t last, const when_t *start, const when_t *end, const char *title)
{
    if (c->count >= CAL_MAX) {
        return;
    }
    cal_event_t *e = &c->ev[c->count++];
    e->first_day = first;
    e->last_day = last;
    e->start_min = start->min;
    e->end_day = end->day;
    e->end_min = end->min;
    /* Cozi starts every title with who it's for: "Karen: ...", "Evan: ...", "All: ..." */
    e->who = 0;
    const char *colon = strchr(title, ':');
    int n = colon ? (int)(colon - title) : 0;
    bool prefix = n > 0 && n <= 15;
    for (int k = 0; prefix && k < n; k++) {
        prefix = isalpha((unsigned char)title[k]);
    }
    if (prefix) {
        e->who = (char)toupper((unsigned char)title[0]);
        title = colon + 1;
        while (*title == ' ') {
            title++;
        }
    }
    snprintf(e->title, sizeof(e->title), "%s", title[0] ? title : "(no title)");
}

static void expand(calendar_t *c, vevent_t *v, int32_t win_first, int32_t win_last)
{
    if (!v->has_start) {
        return;
    }
    bool all_day = v->start.min < 0;
    when_t end = v->has_end ? v->end : v->start;
    /* Days an occurrence shows on: all day: up to DTEND (exclusive); timed: through the end's day
     * (not the next day when it ends at midnight) */
    int32_t span = all_day ? (end.day > v->start.day ? end.day - v->start.day - 1 : 0)
                           : end.day - v->start.day - (end.min == 0 && end.day > v->start.day);
    if (span < 0) {
        span = 0;
    }
    if (!v->rule.freq) {
        int32_t last = v->start.day + span;
        if (last >= win_first && v->start.day <= win_last) {
            add(c, v->start.day, last, &v->start, &end, v->title);
        }
        return;
    }
    if (v->rule.freq == '?') {
        return;
    }
    if (v->rule.count) {
        ESP_LOGW(TAG, "a repeating event has COUNT (not supported): shown without the limit");
    }
    for (int32_t day = win_first - span; day <= win_last; day++) {
        if (!occurs(&v->rule, v->start.day, day)) {
            continue;
        }
        if (v->rule.has_until && (day > v->rule.until.day ||
                                  (day == v->rule.until.day && !all_day && v->start.min > v->rule.until.min))) {
            continue;
        }
        bool skipped = false;
        for (int k = 0; k < v->n_exdate; k++) {
            skipped |= v->exdate[k] == day;
        }
        if (skipped) {
            continue;
        }
        when_t s = {day, v->start.min}, e = {day + (end.day - v->start.day), end.min};
        add(c, day, day + span, &s, &e, v->title);
    }
}

static int by_time(const void *a, const void *b)
{
    const cal_event_t *x = a, *y = b;
    if (x->first_day != y->first_day) {
        return x->first_day < y->first_day ? -1 : 1;
    }
    return x->start_min - y->start_min; /* all day (-1) first */
}

/* Parse the feed (unfolding continuation lines in place) into c's occurrences for the window */
static void parse_feed(char *buf, calendar_t *c, int32_t today)
{
    int32_t win_last = today + CAL_DAYS - 1;
    static vevent_t v;
    bool in_event = false;
    int depth = 0; /* nested components inside the event (VALARM) */
    int events = 0;
    char *line = buf, *w = buf;
    for (char *r = buf;; r++) {
        /* Unfold: CRLF/LF followed by a space or tab continues the line */
        if (*r == '\r') {
            continue;
        }
        if (*r == '\n' && (r[1] == ' ' || r[1] == '\t')) {
            r++;
            continue;
        }
        if (*r != '\n' && *r != 0) {
            *w++ = *r;
            continue;
        }
        bool end = *r == 0;
        *w = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            char *val = colon + 1;
            char *semi = strchr(line, ';'); /* parameters (TZID=..., VALUE=DATE): not needed */
            if (semi) {
                *semi = 0;
            }
            if (!strcmp(line, "BEGIN")) {
                if (!strcmp(val, "VEVENT")) {
                    memset(&v, 0, sizeof(v));
                    in_event = true;
                    depth = 0;
                } else if (in_event) {
                    depth++;
                }
            } else if (!strcmp(line, "END")) {
                if (in_event && depth) {
                    depth--;
                } else if (in_event && !strcmp(val, "VEVENT")) {
                    in_event = false;
                    events++;
                    expand(c, &v, today, win_last);
                }
            } else if (in_event && !depth) {
                if (!strcmp(line, "DTSTART")) {
                    v.has_start = parse_when(val, &v.start);
                } else if (!strcmp(line, "DTEND")) {
                    v.has_end = parse_when(val, &v.end);
                } else if (!strcmp(line, "RRULE")) {
                    parse_rrule(val, &v.rule);
                } else if (!strcmp(line, "EXDATE")) {
                    for (char *s, *x = strtok_r(val, ",", &s); x; x = strtok_r(NULL, ",", &s)) {
                        when_t ex;
                        if (v.n_exdate < EXDATE_MAX && parse_when(x, &ex)) {
                            v.exdate[v.n_exdate++] = ex.day;
                        }
                    }
                } else if (!strcmp(line, "SUMMARY")) {
                    clean_title(val, v.title, sizeof(v.title));
                }
            }
        }
        if (end) {
            break;
        }
        line = w = r + 1;
    }
    qsort(c->ev, c->count, sizeof(c->ev[0]), by_time);
    ESP_LOGI(TAG, "%d events in the feed, %d occurrences in the next %d days%s", events, c->count, CAL_DAYS,
             c->count >= CAL_MAX ? " (list full)" : "");
}

/* ---- fetching ---- */

static char *fetch(int *len_out)
{
#ifdef DASH_COZI_URL
    const esp_http_client_config_t cfg = {
        .url = DASH_COZI_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = USER_AGENT,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    char *buf = NULL;
    int len = 0, cap = 0;
    bool ok = false;
    if (esp_http_client_open(h, 0) != ESP_OK) {
        ESP_LOGW(TAG, "can't reach the calendar server");
        goto out;
    }
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto out;
    }
    while (true) {
        if (cap - len < 4096) {
            if (cap >= FEED_MAX) {
                ESP_LOGW(TAG, "feed over %d KB: cut", FEED_MAX / 1024);
                break;
            }
            cap = cap ? cap * 2 : 128 * 1024;
            char *nb = heap_caps_realloc(buf, cap + 1, MALLOC_CAP_SPIRAM);
            if (!nb) {
                ESP_LOGW(TAG, "out of PSRAM");
                goto out;
            }
            buf = nb;
        }
        int n = esp_http_client_read(h, buf + len, cap - len);
        if (n < 0) {
            ESP_LOGW(TAG, "read failed");
            goto out;
        }
        if (n == 0) {
            break;
        }
        len += n;
    }
    buf[len] = 0;
    ok = len > 0;
out:
    esp_http_client_cleanup(h);
    if (!ok) {
        free(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
#else
    (void)len_out;
    return NULL;
#endif
}

static bool clock_set(void)
{
    return time(NULL) > 1700000000; /* set from the node (or not yet) */
}

static void task(void *arg)
{
    (void)arg;
    int32_t parsed_day = 0;
    int64_t next = 0;
    while (true) {
        /* Refresh on schedule, and at midnight (the window moves) */
        int64_t now = time(NULL);
        if (wifi_connected() && clock_set() && (now >= next || cal_today() != parsed_day)) {
            int len;
            char *feed = fetch(&len);
            if (feed) {
                int32_t today = cal_today();
                memset(&s_work, 0, sizeof(s_work));
                parse_feed(feed, &s_work, today);
                free(feed);
                s_work.valid = true;
                s_work.fetched = time(NULL);
                s_work.today = today;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_work.version = s_cal.version + 1;
                s_cal = s_work;
                xSemaphoreGive(s_lock);
                parsed_day = today;
                next = now + CAL_EVERY_S;
            } else {
                next = now + RETRY_S;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(WAIT_S * 1000));
    }
}

void calendar_get(calendar_t *c)
{
    if (!s_lock) {
        memset(c, 0, sizeof(*c));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *c = s_cal;
    xSemaphoreGive(s_lock);
}

bool calendar_configured(void)
{
#ifdef DASH_COZI_URL
    return wifi_configured() && DASH_COZI_URL[0];
#else
    return false;
#endif
}

void calendar_start(void)
{
#ifdef DASH_COZI_URL
    if (!calendar_configured()) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(task, "calendar", 8192, NULL, 3, NULL);
#endif
}
