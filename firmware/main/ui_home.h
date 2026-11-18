#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the landscape 320x240 home screen (design artboard: "C5 2.8in rectangular") on scr.
 * Call with the display lock held. Content is placeholder data for now. */
void ui_home_create(lv_obj_t *scr);

/* Theme changed: rebuild the home screen in place */
void ui_home_restyle(void);

#ifdef __cplusplus
}
#endif
