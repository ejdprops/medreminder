#pragma once

#include <stdbool.h>

/* Screen sleep: backlight off after Settings > Screen off (minutes without a touch; 0 = never).
 * Never while a dose is due and not taken; a due dose wakes it. The tap that wakes the screen
 * does nothing else. LVGL task only. */
void ui_sleep_init(void);
int ui_sleep_minutes(void);
void ui_sleep_set_minutes(int minutes);
void ui_sleep_wake(const char *why);
