#pragma once

#include <stdbool.h>

#include <lvgl.h>

/* CrowPanel 7" (DIS08070H V3.0): RGB panel, GT911 touch, backlight; LVGL running when it returns.
 * Draw with LVGL only between board_lock() and board_unlock(). */
lv_display_t *board_start(void);
bool board_lock(void);
void board_unlock(void);

/* Backlight 0-100 % */
void board_backlight(int percent);
