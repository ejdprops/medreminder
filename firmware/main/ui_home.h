#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the landscape 320x240 home screen (design: "Living room — C5 2.8in") on scr.
 * Call with the display lock held. Content is placeholder data for now. */
void ui_home_create(lv_obj_t *scr);

#ifdef __cplusplus
}
#endif
