#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the dose schedule list (Settings > Doses); Back returns to the screen that was active. Call from LVGL context. */
void ui_doses_open(void);

/* Theme changed: drop the cached list screen so the next open rebuilds it */
void ui_doses_invalidate(void);

#ifdef __cplusplus
}
#endif
