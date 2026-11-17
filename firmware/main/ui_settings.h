#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the settings screen; Back returns to the screen that was active. Call from LVGL context. */
void ui_settings_open(void);

#ifdef __cplusplus
}
#endif
