#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Doses list row: a 30px pill (time, name, weekday for weekly doses), solid in the dose's color
 * with dark or white text by the color's brightness. No today state. */
#define UI_DOSE_ROW_H 30

lv_obj_t *ui_dose_row(lv_obj_t *parent, int i);

/* Home list row (RECT layout): time + name, check when taken. Always the solid dose color;
 * only overdue rows get a red border and pulse. UI_DOSE_LIST_ROW_H tall, or taller: it grows to
 * share its flex-column parent's height when fewer rows are shown. */
#define UI_DOSE_LIST_ROW_H 28
lv_obj_t *ui_dose_list_row(lv_obj_t *parent, int i);

/* Overdue pulse on obj's background: 1 -> 0.72 opacity, 1.4s cycle */
void ui_dose_pulse(lv_obj_t *obj);

/* Building blocks, also used by the round layout */
lv_obj_t *ui_dose_pill(lv_obj_t *parent, int i, int32_t w, int32_t h, bool live);
const char *ui_dose_slot_text(int i, bool live, bool next, char *buf, size_t len);
uint32_t ui_dose_name_color(int i);
uint32_t ui_dose_slot_color(int i);

#ifdef __cplusplus
}
#endif
