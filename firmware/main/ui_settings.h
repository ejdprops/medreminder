#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the settings screen; Back returns to the screen that was active. Call from LVGL context. */
void ui_settings_open(void);

/* A setting changed from outside the screen (the Med Dot app): rebuild it if it is showing, else
 * drop it so the next open builds it with the new values. LVGL context. */
void ui_settings_changed(void);

#ifdef __cplusplus
}
#endif
