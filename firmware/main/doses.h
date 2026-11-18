#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DOSE_TAKEN, DOSE_NEXT, DOSE_LATER, DOSE_OVERDUE } dose_state_t;

#define DOSE_MAX 16
#define DOSE_NAME_MAX 15 /* characters */

/* Periods offered by the schedule editor; pills of the same period stack when there are more
 * than five doses */
#define DOSE_PERIOD_COUNT 4
extern const char *const dose_periods[DOSE_PERIOD_COUNT];

/* Dose colors the user picks from. The index is stored per dose, so only append to the palette;
 * dose_color_order is the order the picker shows them in. */
#define DOSE_COLOR_COUNT 12
extern const uint32_t dose_colors[DOSE_COLOR_COUNT];
extern const uint8_t dose_color_order[DOSE_COLOR_COUNT];

typedef struct {
    char name[DOSE_NAME_MAX + 1];
    char period[8];
    int minute;    /* time of day, minutes after midnight */
    uint8_t color;   /* index into dose_colors */
    uint8_t opacity; /* percent, for pending/taken rows; 0 (older saves) = DOSE_OPACITY_DEFAULT */
    uint8_t days;    /* weekdays it's due, bit 0 = Sunday; DOSE_DAILY (0 in older saves = daily) */
} dose_def_t;

#define DOSE_DAILY 0x7f

#define DOSE_OPACITY_DEFAULT 45
#define DOSE_OPACITY_MIN 15
#define DOSE_OPACITY_MAX 85

/* The day's doses, sorted by time. Not thread-safe: call everything from one task (the UI's). */
void doses_init(void);
int doses_count(void);
const dose_def_t *dose_def(int i);
const char *dose_name(int i);
const char *dose_slot(int i);
dose_state_t dose_state(int i);
/* Due today (weekly doses only on their day; all are due until the clock is set). Doses not due
 * today are never next/overdue and the home screen leaves them out. */
bool dose_due_today(int i);
bool dose_taken(int i);

/* First dose not yet taken (the "next" dose, possibly overdue); 0 if all are taken */
int doses_next(void);

/* Mark dose i taken / not taken; false if it already was. Saved in NVS for the day. */
bool doses_take(int i);
bool doses_untake(int i);

/* Schedule editing (saved in NVS). index -1 adds. Returns NULL on success or an error message
 * for the user (empty name, a clash with the same person's other dose time, schedule full). */
const char *doses_set(int index, const dose_def_t *d);
void doses_delete(int index);

/* Dev: mark every dose not taken (test schedule: restart it from now) */
void doses_reset(void);

/* Call about once a second. Handles midnight, overdue and the repeating alert chime; returns true
 * if anything shown changed (dose states or the schedule: redraw). States only update once the
 * clock is valid. */
bool doses_tick(time_t now);

/* Format minutes after midnight as 24 h military time, "0600" */
void dose_format_time(int minute, char *buf, int len);

#ifdef __cplusplus
}
#endif
