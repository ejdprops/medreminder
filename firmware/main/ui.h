#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start display + touch and show the UI. No-op on boards without a display BSP. */
void ui_start(void);

#ifdef __cplusplus
}
#endif
