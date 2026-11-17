#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DOSE_TAKEN, DOSE_NEXT, DOSE_LATER, DOSE_OVERDUE } dose_state_t;

/* The day's doses, in daily order. Not thread-safe: call everything from one task (the UI's). */
void doses_init(void);
int doses_count(void);
const char *dose_name(int i);
const char *dose_slot(int i);
dose_state_t dose_state(int i);

/* First dose not yet taken (the "next" dose, possibly overdue); 0 if all are taken */
int doses_next(void);

/* Mark dose i taken; false if it already was. Saved in NVS for the day. */
bool doses_take(int i);

/* Dev: mark every dose not taken (test schedule: restart it from now) */
void doses_reset(void);

/* Call about once a second. Handles midnight, overdue and the repeating alert chime; returns true
 * if any dose state changed (redraw). Does nothing until the clock is valid. */
bool doses_tick(time_t now);

#ifdef __cplusplus
}
#endif
