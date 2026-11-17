#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the dose schedule list (Settings > Doses); Back returns to the screen that was active. Call from LVGL context. */
void ui_doses_open(void);

#ifdef __cplusplus
}
#endif
