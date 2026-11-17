#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum press before a tap counts: ignores brushes and touch bounce */
#define UI_TAP_MIN_MS 100

typedef void (*ui_tap_cb_t)(lv_obj_t *obj, void *user_data);

/* Make obj clickable and call cb(obj, user_data) on a tap held at least UI_TAP_MIN_MS. */
void ui_on_tap(lv_obj_t *obj, ui_tap_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
