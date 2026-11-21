#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct {
    bool valid;
    time_t fetched;       /* when (our clock) */
    int temp_f;           /* now */
    int humidity;         /* %, now */
    int code;             /* WMO weather code, now */
    int high_f, low_f;    /* today */
    int rain_pct;         /* today's highest precipitation probability */
} weather_t;

/* Open-Meteo every WEATHER_EVERY_S once Wi-Fi is up (location from secrets.h) */
void weather_start(void);
void weather_get(weather_t *w);
/* "Partly cloudy" etc. for a WMO code */
const char *weather_text(int code);
