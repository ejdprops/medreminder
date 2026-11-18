#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the About screen; Back returns to the screen that was active. Call from LVGL context. */
void ui_about_open(void);

/* Theme changed: drop the cached screen so the next open rebuilds it */
void ui_about_invalidate(void);

#ifdef __cplusplus
}
#endif
