#pragma once

#include <stdbool.h>
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

typedef struct {
    char name[DOSE_NAME_MAX + 1];
    char period[8];
    int minute; /* time of day, minutes after midnight */
} dose_def_t;

/* The day's doses, sorted by time. Not thread-safe: call everything from one task (the UI's). */
void doses_init(void);
int doses_count(void);
const dose_def_t *dose_def(int i);
const char *dose_name(int i);
const char *dose_slot(int i);
dose_state_t dose_state(int i);
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

/* Format minutes after midnight as "6:00 AM" */
void dose_format_time(int minute, char *buf, int len);

#ifdef __cplusplus
}
#endif
