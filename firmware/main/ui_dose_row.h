#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A dose drawn as a pill-shaped row (time, name, period, weekday for weekly doses), solid in the
 * dose's color with dark or white text by the color's brightness. Shared by the home list and
 * Settings > Doses so both show the same thing.
 *   live: show today's state ("Taken ✓", "· next", "· late", overdue pulse + red border);
 *   next: this is the next dose (for "· next"/"· late"). */
#define UI_DOSE_ROW_H 30

lv_obj_t *ui_dose_row(lv_obj_t *parent, int i, bool live, bool next);

/* Building blocks, also used by the round layout */
lv_obj_t *ui_dose_pill(lv_obj_t *parent, int i, int32_t w, int32_t h, bool live);
const char *ui_dose_slot_text(int i, bool live, bool next, char *buf, size_t len);
uint32_t ui_dose_name_color(int i);
uint32_t ui_dose_slot_color(int i);

#ifdef __cplusplus
}
#endif
