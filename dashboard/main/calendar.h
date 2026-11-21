#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CAL_DAYS 14  /* window: today and the next 13 days */
#define CAL_MAX 64   /* occurrences kept */
#define CAL_TITLE 48 /* bytes, ASCII (the fonts have no other glyphs) */

/* One occurrence in local days (days since 1970-01-01) and minutes */
typedef struct {
    int32_t first_day, last_day; /* days it shows on (inclusive) */
    int16_t start_min;           /* -1 = all day */
    int32_t end_day;             /* timed: when it ends, to drop it once over */
    int16_t end_min;
    char who;  /* initial of the Cozi member prefix ("Karen: ..." -> 'K', "All: ..." -> 'A'); 0 = none */
    char title[CAL_TITLE]; /* without the prefix */
} cal_event_t;

typedef struct {
    bool valid;
    time_t fetched;
    int32_t today; /* the day the window starts */
    uint32_t version;
    int count;
    cal_event_t ev[CAL_MAX]; /* sorted: day, all-day first, start time */
} calendar_t;

/* Cozi .ics feed (DASH_COZI_URL in secrets.h) every CAL_EVERY_S once Wi-Fi and the clock are up */
void calendar_start(void);
bool calendar_configured(void);
void calendar_get(calendar_t *c);
/* Local day number of a struct tm / of now */
int32_t cal_day(int year, int month, int mday);
int32_t cal_today(void);
