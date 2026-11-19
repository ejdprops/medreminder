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

/* Periods: no longer edited, set from the dose's time by the editor; the round layout groups by
 * them */
#define DOSE_PERIOD_COUNT 4
extern const char *const dose_periods[DOSE_PERIOD_COUNT];

/* Dose colors. The index is stored per dose, so only append to the palette. dose_color_order is
 * what the picker offers, in its order: not every palette color (dull ones were dropped, doses
 * that already have one keep it). */
#define DOSE_COLOR_COUNT 25
#define DOSE_PICKER_COUNT 18
extern const uint32_t dose_colors[DOSE_COLOR_COUNT];
extern const uint8_t dose_color_order[DOSE_PICKER_COUNT];

typedef struct {
    char name[DOSE_NAME_MAX + 1];
    char period[8];
    int minute;    /* time of day, minutes after midnight */
    uint8_t color;   /* index into dose_colors */
    uint8_t opacity; /* percent, for pending/taken rows; 0 (older saves) = DOSE_OPACITY_DEFAULT */
    uint8_t days;    /* weekdays it's due, bit 0 = Sunday; DOSE_DAILY (0 in older saves = daily) */
    uint8_t uid;     /* stable id 1-255 (Matter endpoint), kept through edits; set by doses.c */
} dose_def_t;
_Static_assert(sizeof(dose_def_t) == 32, "dose_def_t is saved as a blob");

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

/* First dose due today and not yet taken (the "next" dose, possibly overdue); -1 if none */
int doses_next(void);

/* Index of the dose with this uid; -1 if it was deleted */
int doses_find_uid(uint8_t uid);

/* Earliest dose due on weekday wday (0 = Sunday); -1 if none */
int doses_first_on(int wday);

/* Mark dose i taken / not taken; false if it already was. Saved in NVS for the day. */
bool doses_take(int i);
bool doses_untake(int i);

/* Schedule editing (saved in NVS). index -1 adds; d->uid is ignored (kept, or new for an add). Returns NULL on success or an error message
 * for the user (empty name, a clash with the same person's other dose time, schedule full). */
const char *doses_set(int index, const dose_def_t *d);
void doses_delete(int index);

/* Call about once a second. Handles midnight, overdue, the repeating alert chime and doses taken
 * or undone from Apple Home, and keeps the Matter accessories up to date; returns true if anything
 * shown changed (dose states or the schedule: redraw). States only update once the clock is
 * valid. */
bool doses_tick(time_t now);

/* Minutes after its time a dose turns overdue (saved in NVS; the test schedule ignores the
 * saved value) */
int doses_overdue_min(void);
void doses_set_overdue_min(int minutes);

/* A dose due today has reached its time and isn't taken (the screen stays awake for it) */
bool doses_any_due(time_t now);

/* Format minutes after midnight as 24 h military time, "0600" */
void dose_format_time(int minute, char *buf, int len);

#ifdef __cplusplus
}
#endif
